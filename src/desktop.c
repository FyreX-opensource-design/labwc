// SPDX-License-Identifier: GPL-2.0-only
#include "config.h"
#include <assert.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include "common/scene-helpers.h"
#include "config/rcxml.h"
#include "dnd.h"
#include "labwc.h"
#include "layers.h"
#include "node.h"
#include "output.h"
#include "ssd.h"
#include "view.h"
#include "window-rules.h"
#include "workspaces.h"

#if HAVE_XWAYLAND
#include <wlr/xwayland.h>
#endif

void
desktop_arrange_all_views(struct server *server)
{
	/*
	 * Adjust window positions/sizes. Skip views with no size since
	 * we can't do anything useful with them; they will presumably
	 * be initialized with valid positions/sizes later.
	 *
	 * We do not simply check view->mapped/been_mapped here because
	 * views can have maximized/fullscreen geometry applied while
	 * still unmapped. We do want to adjust the geometry of those
	 * views.
	 */
	struct view *view;
	wl_list_for_each(view, &server->views, link) {
		if (!wlr_box_empty(&view->pending)) {
			view_adjust_for_layout_change(view);
		}
	}
}

static void
set_or_offer_focus(struct view *view)
{
	struct seat *seat = &view->server->seat;
	switch (view_wants_focus(view)) {
	case VIEW_WANTS_FOCUS_ALWAYS:
		if (view->surface != seat->seat->keyboard_state.focused_surface) {
			seat_focus_surface(seat, view->surface);
		}
		break;
	case VIEW_WANTS_FOCUS_LIKELY:
	case VIEW_WANTS_FOCUS_UNLIKELY:
		view_offer_focus(view);
		break;
	case VIEW_WANTS_FOCUS_NEVER:
		break;
	}
}

void
desktop_focus_view(struct view *view, bool raise)
{
	assert(view);
	/*
	 * Guard against views with no mapped surfaces when handling
	 * 'request_activate' and 'request_minimize'.
	 */
	if (!view->surface) {
		return;
	}

	if (view->server->input_mode == LAB_INPUT_STATE_CYCLE) {
		wlr_log(WLR_DEBUG, "not focusing window while window switching");
		return;
	}

	if (view->minimized) {
		/*
		 * Unminimizing will map the view which triggers a call to this
		 * function again (with raise=true).
		 */
		view_minimize(view, false);
		return;
	}

	if (!view->mapped) {
		return;
	}

	/*
	 * Switch workspace if necessary to make the view visible
	 * (unnecessary for "always on {top,bottom}" views).
	 */
	if (!view_is_always_on_top(view) && !view_is_always_on_bottom(view)) {
		workspaces_switch_to(view->workspace, /*update_focus*/ false);
	}

	if (raise) {
		view_move_to_front(view);
	}

	/*
	 * If any child/sibling of the view is a modal dialog, focus
	 * the dialog instead. It does not need to be raised separately
	 * since view_move_to_front() raises all sibling views together.
	 */
	struct view *dialog = view_get_modal_dialog(view);
	set_or_offer_focus(dialog ? dialog : view);
}

/* TODO: focus layer-shell surfaces also? */
void
desktop_focus_view_or_surface(struct seat *seat, struct view *view,
		struct wlr_surface *surface, bool raise)
{
	assert(view || surface);
	if (view) {
		desktop_focus_view(view, raise);
#if HAVE_XWAYLAND
	} else {
		struct wlr_xwayland_surface *xsurface =
			wlr_xwayland_surface_try_from_wlr_surface(surface);
		if (xsurface && wlr_xwayland_surface_override_redirect_wants_focus(xsurface)) {
			seat_focus_surface(seat, surface);
		}
#endif
	}
}

static struct view *
desktop_topmost_focusable_view(struct server *server)
{
	struct view *view;
	struct wl_list *node_list;
	struct wlr_scene_node *node;
	node_list = &server->workspaces.current->tree->children;
	wl_list_for_each_reverse(node, node_list, link) {
		if (!node->data) {
			/* We found some non-view, most likely the region overlay */
			continue;
		}
		view = node_view_from_node(node);
		if (view_is_focusable(view) && !view->minimized) {
			return view;
		}
	}
	return NULL;
}

void
desktop_focus_topmost_view(struct server *server)
{
	struct view *view = desktop_topmost_focusable_view(server);
	if (view) {
		desktop_focus_view(view, /*raise*/ true);
	} else {
		/*
		 * Defocus previous focused surface/view if no longer
		 * focusable (e.g. unmapped or on a different workspace).
		 */
		seat_focus_surface(&server->seat, NULL);
	}
}

void
desktop_focus_output(struct output *output)
{
	if (!output_is_usable(output) || output->server->input_mode
			!= LAB_INPUT_STATE_PASSTHROUGH) {
		return;
	}
	struct view *view;
	struct wlr_scene_node *node;
	struct wlr_output_layout *layout = output->server->output_layout;
	struct wl_list *list_head =
		&output->server->workspaces.current->tree->children;
	wl_list_for_each_reverse(node, list_head, link) {
		if (!node->data) {
			continue;
		}
		view = node_view_from_node(node);
		if (!view_is_focusable(view)) {
			continue;
		}
		if (wlr_output_layout_intersects(layout,
				output->wlr_output, &view->current)) {
			desktop_focus_view(view, /*raise*/ false);
			wlr_cursor_warp(view->server->seat.cursor, NULL,
				view->current.x + view->current.width / 2,
				view->current.y + view->current.height / 2);
			cursor_update_focus(view->server);
			return;
		}
	}
	/* No view found on desired output */
	struct wlr_box layout_box;
	wlr_output_layout_get_box(output->server->output_layout,
		output->wlr_output, &layout_box);
	wlr_cursor_warp(output->server->seat.cursor, NULL,
		layout_box.x + output->usable_area.x + output->usable_area.width / 2,
		layout_box.y + output->usable_area.y + output->usable_area.height / 2);
	cursor_update_focus(output->server);
}

void
desktop_update_top_layer_visibility(struct server *server)
{
	struct view *view;
	struct output *output;
	uint32_t top = ZWLR_LAYER_SHELL_V1_LAYER_TOP;

	/* Enable all top layers */
	wl_list_for_each(output, &server->outputs, link) {
		if (!output_is_usable(output)) {
			continue;
		}
		wlr_scene_node_set_enabled(&output->layer_tree[top]->node, true);
	}

	/*
	 * And disable them again when there is a fullscreen view without
	 * any views above it
	 */
	uint64_t outputs_covered = 0;
	for_each_view(view, &server->views, LAB_VIEW_CRITERIA_CURRENT_WORKSPACE) {
		if (view->minimized) {
			continue;
		}
		if (!output_is_usable(view->output)) {
			continue;
		}
		if (view->fullscreen && !(view->outputs & outputs_covered)) {
			wlr_scene_node_set_enabled(
				&view->output->layer_tree[top]->node, false);
		}
		outputs_covered |= view->outputs;
	}
}

/*
 * Work around rounding issues in some clients (notably Qt apps) where
 * cursor coordinates in the rightmost or bottom pixel are incorrectly
 * rounded up, putting them outside the surface bounds. The effect is
 * especially noticeable in right/bottom desktop panels, since driving
 * the cursor to the edge of the screen no longer works.
 *
 * Under X11, such rounding issues went unnoticed since cursor positions
 * were always integers (i.e. whole pixel boundaries) anyway. Until more
 * clients/toolkits are fractional-pixel clean, limit surface cursor
 * coordinates to (w - 1, h - 1) as a workaround.
 */
static void
avoid_edge_rounding_issues(struct cursor_context *ctx)
{
	if (!ctx->surface) {
		return;
	}

	int w = ctx->surface->current.width;
	int h = ctx->surface->current.height;
	/*
	 * The cursor isn't expected to be outside the surface bounds
	 * here, but check (sx < w, sy < h) just in case.
	 */
	if (ctx->sx > w - 1 && ctx->sx < w) {
		ctx->sx = w - 1;
	}
	if (ctx->sy > h - 1 && ctx->sy < h) {
		ctx->sy = h - 1;
	}
}

/* TODO: make this less big and scary */
struct cursor_context
get_cursor_context(struct server *server)
{
	struct cursor_context ret = {.type = LAB_NODE_NONE};
	struct wlr_cursor *cursor = server->seat.cursor;

	/* Prevent drag icons to be on top of the hitbox detection */
	if (server->seat.drag.active) {
		dnd_icons_show(&server->seat, false);
	}

	struct wlr_scene_node *node =
		wlr_scene_node_at(&server->scene->tree.node,
			cursor->x, cursor->y, &ret.sx, &ret.sy);

	if (server->seat.drag.active) {
		dnd_icons_show(&server->seat, true);
	}

	if (!node) {
		ret.type = LAB_NODE_ROOT;
		return ret;
	}
	ret.node = node;
	ret.surface = lab_wlr_surface_from_node(node);

	avoid_edge_rounding_issues(&ret);

#if HAVE_XWAYLAND
	/* TODO: attach LAB_NODE_UNMANAGED node-descriptor to unmanaged surfaces */
	if (node->type == WLR_SCENE_NODE_BUFFER) {
		if (node->parent == server->unmanaged_tree) {
			ret.type = LAB_NODE_UNMANAGED;
			return ret;
		}
	}
#endif
	while (node) {
		struct node_descriptor *desc = node->data;
		if (desc) {
			switch (desc->type) {
			case LAB_NODE_VIEW:
			case LAB_NODE_XDG_POPUP:
				ret.view = desc->view;
				if (ret.surface) {
					ret.type = LAB_NODE_CLIENT;
				} else {
					/* e.g. when cursor is on resize-indicator */
					ret.type = LAB_NODE_NONE;
				}
				return ret;
			case LAB_NODE_LAYER_SURFACE:
				ret.type = LAB_NODE_LAYER_SURFACE;
				return ret;
			case LAB_NODE_LAYER_POPUP:
			case LAB_NODE_SESSION_LOCK_SURFACE:
			case LAB_NODE_IME_POPUP:
				ret.type = LAB_NODE_CLIENT;
				return ret;
			case LAB_NODE_MENUITEM:
				/* Always return the top scene node for menu items */
				ret.node = node;
				ret.type = LAB_NODE_MENUITEM;
				return ret;
			case LAB_NODE_CYCLE_OSD_ITEM:
				/* Always return the top scene node for osd items */
				ret.node = node;
				ret.type = LAB_NODE_CYCLE_OSD_ITEM;
				return ret;
			case LAB_NODE_BUTTON_FIRST...LAB_NODE_BUTTON_LAST:
			case LAB_NODE_SSD_ROOT:
			case LAB_NODE_TITLE:
			case LAB_NODE_TITLEBAR:
				/* Always return the top scene node for ssd parts */
				ret.node = node;
				ret.view = desc->view;
				/*
				 * A node_descriptor attached to a ssd part
				 * must have an associated view.
				 */
				assert(ret.view);

				/*
				 * When cursor is on the ssd border or extents,
				 * desc->type is usually LAB_NODE_SSD_ROOT.
				 * But desc->type can also be LAB_NODE_TITLEBAR
				 * when cursor is on the curved border at the
				 * titlebar.
				 *
				 * ssd_get_resizing_type() overwrites both of
				 * them with LAB_NODE_{BORDER,CORNER}_* node
				 * types, which are mapped to mouse contexts
				 * like Left and TLCorner.
				 */
				ret.type = ssd_get_resizing_type(ret.view->ssd, cursor);
				if (ret.type == LAB_NODE_NONE) {
					/*
					 * If cursor is not on border/extents,
					 * just use desc->type which should be
					 * mapped to mouse contexts like Title,
					 * Titlebar and Iconify.
					 */
					ret.type = desc->type;
				}

				return ret;
			default:
				/* Other node types are not attached a scene node */
				wlr_log(WLR_ERROR, "unexpected node type: %d", desc->type);
				break;
			}
		}

		/* node->parent is always a *wlr_scene_tree */
		node = node->parent ? &node->parent->node : NULL;
	}

	/*
	 * TODO: add node descriptors for the OSDs and reinstate
	 *       wlr_log(WLR_DEBUG, "Unknown node detected");
	 */
	return ret;
}

/**
 * desktop_arrange_tiled() - Arrange all windows on the current workspace
 * in a tiled layout, similar to Sway's automatic tiling.
 *
 * Windows are arranged in a grid-like layout, with each window getting
 * an equal share of the screen space.
 */
void
desktop_arrange_tiled(struct server *server)
{
	if (!server->tiling_mode) {
		return;
	}

	/* Count tiled views on current workspace */
	int count = 0;
	struct view *view;
	for_each_view(view, &server->views, LAB_VIEW_CRITERIA_CURRENT_WORKSPACE) {
		/* Skip views that shouldn't be tiled */
		if (view->minimized || view->fullscreen ||
		    view_is_always_on_top(view) || view_is_always_on_bottom(view)) {
			continue;
		}
		/* Skip views with fixed position */
		if (window_rules_get_property(view, "fixedPosition") == LAB_PROP_TRUE) {
			continue;
		}
		/* Skip views that explicitly opt out of tiling */
		enum property tile_prop = window_rules_get_property(view, "tile");
		if (tile_prop == LAB_PROP_FALSE) {
			continue;
		}
		count++;
	}

	if (count == 0) {
		return;
	}

	/* Group views by output */
	struct output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (!output_is_usable(output)) {
			continue;
		}

		/* Count views on this output */
		int output_count = 0;
		for_each_view(view, &server->views, LAB_VIEW_CRITERIA_CURRENT_WORKSPACE) {
			if (view->minimized || view->fullscreen ||
			    view_is_always_on_top(view) || view_is_always_on_bottom(view)) {
				continue;
			}
			if (window_rules_get_property(view, "fixedPosition") == LAB_PROP_TRUE) {
				continue;
			}
			/* Skip views that explicitly opt out of tiling */
			enum property tile_prop = window_rules_get_property(view, "tile");
			if (tile_prop == LAB_PROP_FALSE) {
				continue;
			}
			if (view->output == output) {
				output_count++;
			}
		}

		if (output_count == 0) {
			continue;
		}

		struct wlr_box usable = output_usable_area_in_layout_coords(output);

		/* Check if any window has a preferred tile direction */
		bool prefer_vertical = false;
		bool prefer_horizontal = false;
		for_each_view(view, &server->views, LAB_VIEW_CRITERIA_CURRENT_WORKSPACE) {
			if (view->minimized || view->fullscreen ||
			    view_is_always_on_top(view) || view_is_always_on_bottom(view)) {
				continue;
			}
			if (window_rules_get_property(view, "fixedPosition") == LAB_PROP_TRUE) {
				continue;
			}
			enum property tile_prop = window_rules_get_property(view, "tile");
			if (tile_prop == LAB_PROP_FALSE) {
				continue;
			}
			if (view->output != output) {
				continue;
			}
			enum property tile_dir = window_rules_get_property(view, "tileDirection");
			if (tile_dir == LAB_PROP_TRUE) {
				prefer_vertical = true;
			} else if (tile_dir == LAB_PROP_FALSE) {
				prefer_horizontal = true;
			}
		}

		/* Calculate optimal layout - choose between horizontal and vertical splitting */
		bool use_vertical_split = false;
		int cols, rows;

		if (output_count == 1) {
			cols = 1;
			rows = 1;
		} else if (output_count == 2) {
			/* 2 windows: side by side */
			cols = 2;
			rows = 1;
		} else if (output_count == 3) {
			/* 3 windows: choose layout based on window rules or aspect ratio */
			if (prefer_vertical && !prefer_horizontal) {
				use_vertical_split = true;
				cols = 2;
				rows = 2;
			} else if (prefer_horizontal && !prefer_vertical) {
				cols = 2;
				rows = 2;
			} else {
				/* Auto: choose based on aspect ratio */
				double aspect = (double)usable.width / usable.height;
				if (aspect > 1.5) {
					/* Wide screen: 2 on top, 1 on bottom */
					cols = 2;
					rows = 2;
				} else {
					/* Tall screen: 1 on left, 2 on right */
					use_vertical_split = true;
					cols = 2;
					rows = 2;
				}
			}
		} else if (output_count == 4) {
			cols = 2;
			rows = 2;
		} else if (output_count == 5) {
			/* 5 windows: choose better layout */
			if (prefer_vertical && !prefer_horizontal) {
				cols = 2;
				rows = 3;
			} else if (prefer_horizontal && !prefer_vertical) {
				cols = 3;
				rows = 2;
			} else {
				/* Auto: choose based on aspect ratio */
				double aspect = (double)usable.width / usable.height;
				if (aspect > 1.3) {
					/* Wide: 3 cols, last row incomplete */
					cols = 3;
					rows = 2;
				} else {
					/* Tall: 2 cols, last row incomplete */
					cols = 2;
					rows = 3;
				}
			}
		} else if (output_count == 6) {
			cols = 3;
			rows = 2;
		} else {
			/* For more windows, use a 3-column layout */
			cols = 3;
			rows = (output_count + 2) / 3; /* Ceiling division */
		}

		/* Check if there's a manually resized window we should preserve */
		/* Skip resize preservation if grid mode is enabled (simple grid snapping) */
		struct view *resized_view = NULL;
		if (!server->tiling_grid_mode && server->resized_view && server->resized_view->output == output) {
			resized_view = server->resized_view;
			/* Adjust output_count to exclude the resized window */
			output_count--;
		}

		/* Recalculate layout if we excluded a resized window */
		if (output_count == 0) {
			/* Only the resized window, just preserve its geometry */
			if (resized_view) {
				struct wlr_box geo = server->resized_view_geometry;
				view_move_resize(resized_view, geo);
			}
			continue;
		}

		/* Recalculate optimal layout based on remaining window count */
		if (output_count == 1) {
			cols = 1;
			rows = 1;
		} else if (output_count == 2) {
			cols = 2;
			rows = 1;
		} else if (output_count == 3) {
			if (prefer_vertical && !prefer_horizontal) {
				use_vertical_split = true;
				cols = 2;
				rows = 2;
			} else if (prefer_horizontal && !prefer_vertical) {
				cols = 2;
				rows = 2;
			} else {
				double aspect = (double)usable.width / usable.height;
				if (aspect > 1.5) {
					cols = 2;
					rows = 2;
				} else {
					use_vertical_split = true;
					cols = 2;
					rows = 2;
				}
			}
		} else if (output_count == 4) {
			cols = 2;
			rows = 2;
		} else if (output_count == 5) {
			if (prefer_vertical && !prefer_horizontal) {
				cols = 2;
				rows = 3;
			} else if (prefer_horizontal && !prefer_vertical) {
				cols = 3;
				rows = 2;
			} else {
				double aspect = (double)usable.width / usable.height;
				if (aspect > 1.3) {
					cols = 3;
					rows = 2;
				} else {
					cols = 2;
					rows = 3;
				}
			}
		} else if (output_count == 6) {
			cols = 3;
			rows = 2;
		} else {
			cols = 3;
			rows = (output_count + 2) / 3;
		}

		int last_row_count = output_count % cols;
		if (last_row_count == 0) {
			last_row_count = cols;
		}

		int cell_width, cell_height;
		if (!resized_view) {
			cell_width = (usable.width - (cols + 1) * rc.gap) / cols;
			cell_height = (usable.height - (rows + 1) * rc.gap) / rows;
		} else {
			/* Will be recalculated for remaining space below */
			cell_width = 0;
			cell_height = 0;
		}

		/* If there's a resized window, calculate remaining space for other windows */
		struct wlr_box remaining_space = usable;
		struct wlr_box *layout_area = &usable;
		if (resized_view) {
			struct border resized_margin = ssd_thickness(resized_view);
			/* Get the actual geometry with margins, relative to usable area */
			struct wlr_box resized_full = (struct wlr_box){
				.x = server->resized_view_geometry.x - resized_margin.left,
				.y = server->resized_view_geometry.y - resized_margin.top,
				.width = server->resized_view_geometry.width + resized_margin.left + resized_margin.right,
				.height = server->resized_view_geometry.height + resized_margin.top + resized_margin.bottom,
			};

			/* Calculate the boundaries of the resized window's occupied space including gaps */
			int resized_left = resized_full.x - rc.gap;
			int resized_right = resized_full.x + resized_full.width + rc.gap;
			int resized_top = resized_full.y - rc.gap;
			int resized_bottom = resized_full.y + resized_full.height + rc.gap;

			/* For 2 remaining windows, check where they are relative to resized window */
			if (output_count == 2) {
				/* Check positions of other windows to determine layout */
				struct view *other1 = NULL, *other2 = NULL;
				struct view *view_iter;
				for_each_view(view_iter, &server->views, LAB_VIEW_CRITERIA_CURRENT_WORKSPACE) {
					if (view_iter->minimized || view_iter->fullscreen ||
					    view_is_always_on_top(view_iter) || view_is_always_on_bottom(view_iter)) {
						continue;
					}
					if (window_rules_get_property(view_iter, "fixedPosition") == LAB_PROP_TRUE) {
						continue;
					}
					enum property tile_prop = window_rules_get_property(view_iter, "tile");
					if (tile_prop == LAB_PROP_FALSE) {
						continue;
					}
					if (view_iter->output != output || view_iter == resized_view) {
						continue;
					}
					if (!other1) {
						other1 = view_iter;
					} else if (!other2) {
						other2 = view_iter;
						break;
					}
				}

				/* Determine layout based on other windows' positions */
				bool windows_below = false, windows_above = false;
				bool windows_left = false, windows_right = false;
				bool windows_overlap_horizontally = false;

				if (other1) {
					struct border m1 = ssd_thickness(other1);
					struct wlr_box o1 = (struct wlr_box){
						.x = other1->current.x - m1.left,
						.y = other1->current.y - m1.top,
						.width = other1->current.width + m1.left + m1.right,
						.height = other1->current.height + m1.top + m1.bottom,
					};
					/* Check if windows overlap horizontally (within 50 pixels to account for gaps) */
					if ((o1.x < resized_right + 50) && (o1.x + o1.width > resized_left - 50)) {
						windows_overlap_horizontally = true;
						if (o1.y > resized_bottom - 50) {
							windows_below = true;
						} else if (o1.y + o1.height < resized_top + 50) {
							windows_above = true;
						}
					}
					if (o1.x + o1.width < resized_left) {
						windows_left = true;
					} else if (o1.x > resized_right) {
						windows_right = true;
					}
				}
				if (other2) {
					struct border m2 = ssd_thickness(other2);
					struct wlr_box o2 = (struct wlr_box){
						.x = other2->current.x - m2.left,
						.y = other2->current.y - m2.top,
						.width = other2->current.width + m2.left + m2.right,
						.height = other2->current.height + m2.top + m2.bottom,
					};
					if ((o2.x < resized_right + 50) && (o2.x + o2.width > resized_left - 50)) {
						windows_overlap_horizontally = true;
						if (o2.y > resized_bottom - 50) {
							windows_below = true;
						} else if (o2.y + o2.height < resized_top + 50) {
							windows_above = true;
						}
					}
					if (o2.x + o2.width < resized_left) {
						windows_left = true;
					} else if (o2.x > resized_right) {
						windows_right = true;
					}
				}

				/* Use window positions to determine layout */
				if (windows_overlap_horizontally && windows_below) {
					/* Windows are stacked vertically - resized window grew downward */
					/* Use space below resized window, stack remaining windows vertically */
					remaining_space.x = resized_full.x;
					remaining_space.width = resized_full.width;
					remaining_space.y = resized_bottom;
					remaining_space.height = usable.y + usable.height - resized_bottom;
					cols = 1;
					rows = 2;
				} else if (windows_overlap_horizontally && windows_above) {
					/* Windows are stacked vertically - resized window grew upward */
					/* Use space above resized window, stack remaining windows vertically */
					remaining_space.x = resized_full.x;
					remaining_space.width = resized_full.width;
					remaining_space.y = usable.y;
					remaining_space.height = resized_top - usable.y;
					cols = 1;
					rows = 2;
				} else if (windows_left && !windows_right) {
					/* Resized window on right - use left side, stack vertically */
					remaining_space.x = usable.x;
					remaining_space.width = resized_left - usable.x;
					remaining_space.y = usable.y;
					remaining_space.height = usable.height;
					cols = 1;
					rows = 2;
				} else if (windows_right && !windows_left) {
					/* Resized window on left - use right side, stack vertically */
					remaining_space.x = resized_right;
					remaining_space.width = usable.x + usable.width - resized_right;
					remaining_space.y = usable.y;
					remaining_space.height = usable.height;
					cols = 1;
					rows = 2;
				} else {
					/* Fallback: use screen position and largest area */
					int screen_center_x = usable.x + usable.width / 2;
					bool clearly_on_left = resized_right < screen_center_x;
					bool clearly_on_right = resized_left > screen_center_x;
					
					int left_area = (resized_left - usable.x) * usable.height;
					int right_area = (usable.x + usable.width - resized_right) * usable.height;
					int top_area = (resized_top - usable.y) * usable.width;
					int bottom_area = (usable.y + usable.height - resized_bottom) * usable.width;
					
					if (clearly_on_left && left_area < right_area) {
						/* Use right area - stack vertically */
						remaining_space.x = resized_right;
						remaining_space.width = usable.x + usable.width - resized_right;
						remaining_space.y = usable.y;
						remaining_space.height = usable.height;
						cols = 1;
						rows = 2;
					} else if (clearly_on_right && right_area < left_area) {
						/* Use left area - stack vertically */
						remaining_space.x = usable.x;
						remaining_space.width = resized_left - usable.x;
						remaining_space.y = usable.y;
						remaining_space.height = usable.height;
						cols = 1;
						rows = 2;
					} else if (top_area >= bottom_area && top_area > left_area && top_area > right_area) {
						/* Use top area - side by side */
						remaining_space.x = usable.x;
						remaining_space.width = usable.width;
						remaining_space.y = usable.y;
						remaining_space.height = resized_top - usable.y;
						cols = 2;
						rows = 1;
					} else if (bottom_area > left_area && bottom_area > right_area) {
						/* Use bottom area - side by side */
						remaining_space.x = usable.x;
						remaining_space.width = usable.width;
						remaining_space.y = resized_bottom;
						remaining_space.height = usable.y + usable.height - resized_bottom;
						cols = 2;
						rows = 1;
					} else if (left_area >= right_area) {
						/* Use left area - stack vertically */
						remaining_space.x = usable.x;
						remaining_space.width = resized_left - usable.x;
						remaining_space.y = usable.y;
						remaining_space.height = usable.height;
						cols = 1;
						rows = 2;
					} else {
						/* Use right area - stack vertically */
						remaining_space.x = resized_right;
						remaining_space.width = usable.x + usable.width - resized_right;
						remaining_space.y = usable.y;
						remaining_space.height = usable.height;
						cols = 1;
						rows = 2;
					}
				}
				/* Recalculate last_row_count for new layout */
				last_row_count = output_count % cols;
				if (last_row_count == 0) {
					last_row_count = cols;
				}
			} else {
				/* More than 2 remaining windows - use simple left/right or top/bottom split */
				if (resized_left <= usable.x + usable.width / 2) {
					/* Resized window on left - use right side for others */
					remaining_space.x = resized_right;
					remaining_space.width = usable.x + usable.width - resized_right;
					remaining_space.y = usable.y;
					remaining_space.height = usable.height;
				} else {
					/* Resized window on right - use left side for others */
					remaining_space.x = usable.x;
					remaining_space.width = resized_left - usable.x;
					remaining_space.y = usable.y;
					remaining_space.height = usable.height;
				}
			}

			/* Recalculate cell sizes for remaining space, ensuring no gaps */
			if (remaining_space.width > 0 && remaining_space.height > 0) {
				/* Ensure we account for gaps properly - windows should fill the space */
				int total_gap_width = (cols + 1) * rc.gap;
				int total_gap_height = (rows + 1) * rc.gap;
				
				if (remaining_space.width > total_gap_width && remaining_space.height > total_gap_height) {
					cell_width = (remaining_space.width - total_gap_width) / cols;
					cell_height = (remaining_space.height - total_gap_height) / rows;
					layout_area = &remaining_space;
				}
			}
		}

		/* Tile views */
		int idx = 0;
		for_each_view(view, &server->views, LAB_VIEW_CRITERIA_CURRENT_WORKSPACE) {
			if (view->minimized || view->fullscreen ||
			    view_is_always_on_top(view) || view_is_always_on_bottom(view)) {
				continue;
			}
			if (window_rules_get_property(view, "fixedPosition") == LAB_PROP_TRUE) {
				continue;
			}
			/* Skip views that explicitly opt out of tiling */
			enum property tile_prop = window_rules_get_property(view, "tile");
			if (tile_prop == LAB_PROP_FALSE) {
				continue;
			}
			if (view->output != output) {
				continue;
			}

			/* Handle manually resized window separately */
			if (view == resized_view) {
				/* Preserve the resized window's geometry */
				struct wlr_box geo = server->resized_view_geometry;
				view_move_resize(view, geo);
				continue;
			}


			/* Unmaximize and untile if needed */
			if (view->maximized != VIEW_AXIS_NONE) {
				view_maximize(view, VIEW_AXIS_NONE,
					/*store_natural_geometry*/ false);
			}
			if (view_is_tiled(view)) {
				view_set_untiled(view);
			}

			struct border margin = ssd_thickness(view);
			struct wlr_box geo;

			if (use_vertical_split && output_count == 3 && !resized_view) {
				/* Special case: 3 windows with vertical split (only when no resized window) */
				/* 1 window on left (full height), 2 windows on right (each 50% height) */
				if (idx == 0) {
					/* Left window: full height */
					geo = (struct wlr_box){
						.x = layout_area->x + rc.gap + margin.left,
						.y = layout_area->y + rc.gap + margin.top,
						.width = (layout_area->width - 3 * rc.gap) / 2 - margin.left - margin.right,
						.height = layout_area->height - 2 * rc.gap - margin.top - margin.bottom,
					};
				} else {
					/* Right windows: each 50% height */
					int right_row = idx - 1;
					int right_width = (layout_area->width - 3 * rc.gap) / 2;
					int right_height = (layout_area->height - 3 * rc.gap) / 2;
					geo = (struct wlr_box){
						.x = layout_area->x + 2 * rc.gap + right_width + margin.left,
						.y = layout_area->y + (right_row + 1) * rc.gap + right_row * right_height + margin.top,
						.width = right_width - margin.left - margin.right,
						.height = right_height - margin.top - margin.bottom,
					};
				}
				view_move_resize(view, geo);
			} else {
				/* Standard grid layout */
				int col = idx % cols;
				int row = idx / cols;
				int width, height;
				int x_pos;

				/* Check if this is the last row and it's incomplete */
				bool is_last_row = (row == rows - 1);
				bool last_row_incomplete = (last_row_count < cols);

				if (is_last_row && last_row_incomplete) {
					/* Last row is incomplete - make windows span to fill width */
					width = (layout_area->width - (last_row_count + 1) * rc.gap) / last_row_count;
					height = cell_height;
					x_pos = layout_area->x + (col + 1) * rc.gap + col * width;
				} else {
					/* Normal grid cell */
					width = cell_width;
					height = cell_height;
					x_pos = layout_area->x + (col + 1) * rc.gap + col * cell_width;
				}

				/* Check if this is the last column and it should fill remaining width */
				bool is_last_col = (col == cols - 1);
				if (is_last_col && !is_last_row) {
					/* Last column - ensure it fills to the edge of layout area */
					int expected_right = layout_area->x + layout_area->width - rc.gap;
					int current_right = x_pos + width;
					if (current_right < expected_right) {
						width += expected_right - current_right;
					}
				}

				/* Check if this is the last row and it should fill remaining height */
				if (is_last_row) {
					int expected_bottom = layout_area->y + layout_area->height - rc.gap;
					int current_bottom = layout_area->y + (row + 1) * rc.gap + row * cell_height + height;
					if (current_bottom < expected_bottom) {
						height += expected_bottom - current_bottom;
					}
				}

				geo = (struct wlr_box){
					.x = x_pos + margin.left,
					.y = layout_area->y + (row + 1) * rc.gap + row * cell_height + margin.top,
					.width = width - margin.left - margin.right,
					.height = height - margin.top - margin.bottom,
				};
				view_move_resize(view, geo);
			}

			idx++;
		}
	}
}

