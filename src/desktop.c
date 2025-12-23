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
#include "common/list.h"
#include "common/mem.h"
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
		if (!server->tiling_grid_mode && server->resized_view &&
		    server->resized_view->output == output &&
		    server->resized_view->workspace == server->workspaces.current &&
		    !server->resized_view->minimized) {
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

		/* If there's a resized window, identify adjacent windows and only adjust those */
		struct wlr_box remaining_space = usable;
		struct wlr_box *layout_area = &usable;
		struct wl_list adjacent_views;
		wl_list_init(&adjacent_views);
		
		/* Helper structure for adjacent views list */
		struct adjacent_view_entry {
			struct view *view;
			struct wl_list link;
		};
		
		if (resized_view) {
			struct border resized_margin = ssd_thickness(resized_view);
			/* Get the actual geometry with margins, relative to usable area */
			struct wlr_box resized_full = (struct wlr_box){
				.x = server->resized_view_geometry.x - resized_margin.left,
				.y = server->resized_view_geometry.y - resized_margin.top,
				.width = server->resized_view_geometry.width + resized_margin.left + resized_margin.right,
				.height = server->resized_view_geometry.height + resized_margin.top + resized_margin.bottom,
			};

			/* Ensure the resized window's geometry is within usable area bounds */
			if (resized_full.x < usable.x) {
				resized_full.width -= (usable.x - resized_full.x);
				resized_full.x = usable.x;
			}
			if (resized_full.y < usable.y) {
				resized_full.height -= (usable.y - resized_full.y);
				resized_full.y = usable.y;
			}
			if (resized_full.x + resized_full.width > usable.x + usable.width) {
				resized_full.width = usable.x + usable.width - resized_full.x;
			}
			if (resized_full.y + resized_full.height > usable.y + usable.height) {
				resized_full.height = usable.y + usable.height - resized_full.y;
			}

			/* Calculate the boundaries of the resized window's occupied space */
			int resized_left = resized_full.x;
			int resized_right = resized_full.x + resized_full.width;
			int resized_top = resized_full.y;
			int resized_bottom = resized_full.y + resized_full.height;

			/* Find windows that are adjacent to the resized window */
			/* A window is adjacent if it shares an edge or overlaps with the resized window's area */
			for_each_view(view, &server->views, LAB_VIEW_CRITERIA_CURRENT_WORKSPACE) {
				if (view == resized_view || view->minimized || view->fullscreen ||
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

				struct border view_margin = ssd_thickness(view);
				struct wlr_box view_full = (struct wlr_box){
					.x = view->current.x - view_margin.left,
					.y = view->current.y - view_margin.top,
					.width = view->current.width + view_margin.left + view_margin.right,
					.height = view->current.height + view_margin.top + view_margin.bottom,
				};

				/* Check if windows share an edge or overlap */
				/* Adjacent if they share a horizontal or vertical edge (within gap tolerance) */
				bool shares_horizontal_edge = false;
				bool shares_vertical_edge = false;
				
				/* Check for horizontal edge sharing (top/bottom alignment) */
				if ((abs(view_full.y - resized_bottom) <= rc.gap + 5) ||
				    (abs((view_full.y + view_full.height) - resized_top) <= rc.gap + 5) ||
				    (view_full.y < resized_bottom && (view_full.y + view_full.height) > resized_top)) {
					shares_horizontal_edge = true;
				}
				
				/* Check for vertical edge sharing (left/right alignment) */
				if ((abs(view_full.x - resized_right) <= rc.gap + 5) ||
				    (abs((view_full.x + view_full.width) - resized_left) <= rc.gap + 5) ||
				    (view_full.x < resized_right && (view_full.x + view_full.width) > resized_left)) {
					shares_vertical_edge = true;
				}

				/* Window is adjacent if it shares at least one edge */
				if (shares_horizontal_edge || shares_vertical_edge) {
					/* Add to adjacent list for later processing */
					struct adjacent_view_entry *entry = znew(*entry);
					entry->view = view;
					wl_list_append(&adjacent_views, &entry->link);
				}
			}

			/* Count adjacent windows */
			int adjacent_count = 0;
			struct adjacent_view_entry *entry;
			wl_list_for_each(entry, &adjacent_views, link) {
				adjacent_count++;
			}

			/* If we found adjacent windows, only adjust those */
			if (adjacent_count > 0) {
				/* Recalculate layout for adjacent windows only */
				output_count = adjacent_count;
				if (output_count == 1) {
					cols = 1;
					rows = 1;
				} else if (output_count == 2) {
					cols = 2;
					rows = 1;
				} else if (output_count == 3) {
					cols = 2;
					rows = 2;
				} else if (output_count == 4) {
					cols = 2;
					rows = 2;
				} else {
					cols = 3;
					rows = (output_count + 2) / 3;
				}

				last_row_count = output_count % cols;
				if (last_row_count == 0) {
					last_row_count = cols;
				}

				/* Calculate remaining space after resized window */
				int left_space = resized_left - usable.x;
				int right_space = (usable.x + usable.width) - resized_right;
				int top_space = resized_top - usable.y;
				int bottom_space = (usable.y + usable.height) - resized_bottom;
				
				/* Determine which side the adjacent windows are on */
				/* Check where adjacent windows are positioned relative to resized window */
				bool adjacent_on_right = false, adjacent_on_left = false;
				bool adjacent_on_bottom = false, adjacent_on_top = false;
				
				struct adjacent_view_entry *adj_entry;
				wl_list_for_each(adj_entry, &adjacent_views, link) {
					struct view *adj_view = adj_entry->view;
					struct border adj_margin = ssd_thickness(adj_view);
					struct wlr_box adj_full = (struct wlr_box){
						.x = adj_view->current.x - adj_margin.left,
						.y = adj_view->current.y - adj_margin.top,
						.width = adj_view->current.width + adj_margin.left + adj_margin.right,
						.height = adj_view->current.height + adj_margin.top + adj_margin.bottom,
					};
					
					if (adj_full.x >= resized_right - rc.gap - 5) {
						adjacent_on_right = true;
					}
					if (adj_full.x + adj_full.width <= resized_left + rc.gap + 5) {
						adjacent_on_left = true;
					}
					if (adj_full.y >= resized_bottom - rc.gap - 5) {
						adjacent_on_bottom = true;
					}
					if (adj_full.y + adj_full.height <= resized_top + rc.gap + 5) {
						adjacent_on_top = true;
					}
				}

				/* Determine layout area for adjacent windows */
				if (adjacent_on_right && !adjacent_on_left) {
					/* Adjacent windows on right - use right space */
					remaining_space.x = resized_right + rc.gap;
					remaining_space.y = usable.y;
					remaining_space.width = right_space - rc.gap;
					remaining_space.height = usable.height;
				} else if (adjacent_on_left && !adjacent_on_right) {
					/* Adjacent windows on left - use left space */
					remaining_space.x = usable.x;
					remaining_space.y = usable.y;
					remaining_space.width = left_space - rc.gap;
					remaining_space.height = usable.height;
				} else if (adjacent_on_bottom && !adjacent_on_top) {
					/* Adjacent windows on bottom - use bottom space */
					remaining_space.x = usable.x;
					remaining_space.y = resized_bottom + rc.gap;
					remaining_space.width = usable.width;
					remaining_space.height = bottom_space - rc.gap;
				} else if (adjacent_on_top && !adjacent_on_bottom) {
					/* Adjacent windows on top - use top space */
					remaining_space.x = usable.x;
					remaining_space.y = usable.y;
					remaining_space.width = usable.width;
					remaining_space.height = top_space - rc.gap;
				} else {
					/* Mixed or unclear - use largest available space */
					int left_area = left_space * usable.height;
					int right_area = right_space * usable.height;
					int top_area = top_space * usable.width;
					int bottom_area = bottom_space * usable.width;
					
					if (right_area >= left_area && right_area >= top_area && right_area >= bottom_area && right_space > rc.gap) {
						remaining_space.x = resized_right + rc.gap;
						remaining_space.y = usable.y;
						remaining_space.width = right_space - rc.gap;
						remaining_space.height = usable.height;
					} else if (left_area >= top_area && left_area >= bottom_area && left_space > rc.gap) {
						remaining_space.x = usable.x;
						remaining_space.y = usable.y;
						remaining_space.width = left_space - rc.gap;
						remaining_space.height = usable.height;
					} else if (bottom_area >= top_area && bottom_space > rc.gap) {
						remaining_space.x = usable.x;
						remaining_space.y = resized_bottom + rc.gap;
						remaining_space.width = usable.width;
						remaining_space.height = bottom_space - rc.gap;
					} else if (top_space > rc.gap) {
						remaining_space.x = usable.x;
						remaining_space.y = usable.y;
						remaining_space.width = usable.width;
						remaining_space.height = top_space - rc.gap;
					} else {
						remaining_space = usable;
					}
				}

				/* Recalculate cell sizes for remaining space */
				if (remaining_space.width > 0 && remaining_space.height > 0) {
					int total_gap_width = (cols + 1) * rc.gap;
					int total_gap_height = (rows + 1) * rc.gap;
					
					if (remaining_space.width > total_gap_width && remaining_space.height > total_gap_height) {
						cell_width = (remaining_space.width - total_gap_width) / cols;
						cell_height = (remaining_space.height - total_gap_height) / rows;
						layout_area = &remaining_space;
					} else {
						cell_width = remaining_space.width / cols;
						cell_height = remaining_space.height / rows;
						layout_area = &remaining_space;
					}
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

			/* If we have a resized view with adjacent windows, only process adjacent ones */
			if (resized_view && !wl_list_empty(&adjacent_views)) {
				/* Check if this view is in the adjacent list */
				bool is_adjacent = false;
				struct adjacent_view_entry *entry;
				wl_list_for_each(entry, &adjacent_views, link) {
					if (entry->view == view) {
						is_adjacent = true;
						break;
					}
				}
				
				/* Skip non-adjacent windows - they stay in their current position */
				if (!is_adjacent && view != resized_view) {
					continue;
				}
			}

			/* Handle manually resized window - adjust if necessary to prevent overlaps or fill empty space */
			if (view == resized_view) {
				struct wlr_box geo = server->resized_view_geometry;
				struct border resized_margin = ssd_thickness(resized_view);
				struct wlr_box resized_full = (struct wlr_box){
					.x = geo.x - resized_margin.left,
					.y = geo.y - resized_margin.top,
					.width = geo.width + resized_margin.left + resized_margin.right,
					.height = geo.height + resized_margin.top + resized_margin.bottom,
				};

				/* Check for empty space around the resized window */
				int empty_left = resized_full.x - usable.x;
				int empty_right = (usable.x + usable.width) - (resized_full.x + resized_full.width);
				int empty_top = resized_full.y - usable.y;
				int empty_bottom = (usable.y + usable.height) - (resized_full.y + resized_full.height);

				/* Check if there's significant empty space (more than gap) */
				bool has_empty_space = (empty_left > rc.gap) || (empty_right > rc.gap) ||
				                       (empty_top > rc.gap) || (empty_bottom > rc.gap);

				/* Check for overlaps with other windows and adjust if necessary */
				bool needs_adjustment = false;
				struct view *other_view;
				for_each_view(other_view, &server->views, LAB_VIEW_CRITERIA_CURRENT_WORKSPACE) {
					if (other_view == resized_view || other_view->minimized ||
					    other_view->fullscreen || view_is_always_on_top(other_view) ||
					    view_is_always_on_bottom(other_view)) {
						continue;
					}
					if (window_rules_get_property(other_view, "fixedPosition") == LAB_PROP_TRUE) {
						continue;
					}
					enum property other_tile_prop = window_rules_get_property(other_view, "tile");
					if (other_tile_prop == LAB_PROP_FALSE) {
						continue;
					}
					if (other_view->output != output) {
						continue;
					}

					/* Skip if this other view is adjacent (it will be repositioned) */
					if (resized_view && !wl_list_empty(&adjacent_views)) {
						bool other_is_adjacent = false;
						struct adjacent_view_entry *entry;
						wl_list_for_each(entry, &adjacent_views, link) {
							if (entry->view == other_view) {
								other_is_adjacent = true;
								break;
							}
						}
						if (other_is_adjacent) {
							continue;
						}
					}

					struct border other_margin = ssd_thickness(other_view);
					struct wlr_box other_full = (struct wlr_box){
						.x = other_view->current.x - other_margin.left,
						.y = other_view->current.y - other_margin.top,
						.width = other_view->current.width + other_margin.left + other_margin.right,
						.height = other_view->current.height + other_margin.top + other_margin.bottom,
					};

					/* Check for overlap */
					if (!(resized_full.x + resized_full.width <= other_full.x ||
					      resized_full.x >= other_full.x + other_full.width ||
					      resized_full.y + resized_full.height <= other_full.y ||
					      resized_full.y >= other_full.y + other_full.height)) {
						/* Overlap detected - adjust resized window to avoid it */
						if (resized_full.x < other_full.x + other_full.width &&
						    resized_full.x + resized_full.width > other_full.x) {
							/* Horizontal overlap - adjust width */
							if (resized_full.x < other_full.x) {
								geo.width = other_full.x - resized_full.x - resized_margin.left - resized_margin.right;
							} else {
								int new_x = other_full.x + other_full.width + rc.gap;
								geo.x = new_x + resized_margin.left;
								geo.width = resized_full.x + resized_full.width - new_x - resized_margin.left - resized_margin.right;
							}
							needs_adjustment = true;
						}
						if (resized_full.y < other_full.y + other_full.height &&
						    resized_full.y + resized_full.height > other_full.y) {
							/* Vertical overlap - adjust height */
							if (resized_full.y < other_full.y) {
								geo.height = other_full.y - resized_full.y - resized_margin.top - resized_margin.bottom;
							} else {
								int new_y = other_full.y + other_full.height + rc.gap;
								geo.y = new_y + resized_margin.top;
								geo.height = resized_full.y + resized_full.height - new_y - resized_margin.top - resized_margin.bottom;
							}
							needs_adjustment = true;
						}
					}
				}

				/* If there's empty space and no overlaps, expand the resized window to fill it */
				/* Only expand if there are no adjacent windows that would be affected */
				if (has_empty_space && !needs_adjustment) {
					/* Check if adjacent windows would prevent expansion in each direction */
					bool can_expand_left = true, can_expand_right = true;
					bool can_expand_top = true, can_expand_bottom = true;
					
					if (!wl_list_empty(&adjacent_views)) {
						struct adjacent_view_entry *adj_entry;
						wl_list_for_each(adj_entry, &adjacent_views, link) {
							struct view *adj_view = adj_entry->view;
							struct border adj_margin = ssd_thickness(adj_view);
							struct wlr_box adj_full = (struct wlr_box){
								.x = adj_view->current.x - adj_margin.left,
								.y = adj_view->current.y - adj_margin.top,
								.width = adj_view->current.width + adj_margin.left + adj_margin.right,
								.height = adj_view->current.height + adj_margin.top + adj_margin.bottom,
							};
							
							/* Check which side the adjacent window is on */
							if (adj_full.x + adj_full.width <= resized_full.x + rc.gap) {
								can_expand_left = false; /* Adjacent window on left */
							}
							if (adj_full.x >= resized_full.x + resized_full.width - rc.gap) {
								can_expand_right = false; /* Adjacent window on right */
							}
							if (adj_full.y + adj_full.height <= resized_full.y + rc.gap) {
								can_expand_top = false; /* Adjacent window on top */
							}
							if (adj_full.y >= resized_full.y + resized_full.height - rc.gap) {
								can_expand_bottom = false; /* Adjacent window on bottom */
							}
						}
					}
					
					/* Check which direction has the most empty space and can be expanded */
					/* Prefer expanding horizontally (left/right) over vertically */
					if (can_expand_left && empty_left >= empty_right && empty_left >= empty_top && 
					    empty_left >= empty_bottom && empty_left > rc.gap) {
						/* Expand left */
						geo.x = usable.x + resized_margin.left;
						geo.width += empty_left - resized_margin.left - resized_margin.right;
						needs_adjustment = true;
					} else if (can_expand_right && empty_right >= empty_top && 
					           empty_right >= empty_bottom && empty_right > rc.gap) {
						/* Expand right */
						geo.width += empty_right - resized_margin.left - resized_margin.right;
						needs_adjustment = true;
					} else if (can_expand_top && empty_top >= empty_bottom && empty_top > rc.gap) {
						/* Expand top */
						geo.y = usable.y + resized_margin.top;
						geo.height += empty_top - resized_margin.top - resized_margin.bottom;
						needs_adjustment = true;
					} else if (can_expand_bottom && empty_bottom > rc.gap) {
						/* Expand bottom */
						geo.height += empty_bottom - resized_margin.top - resized_margin.bottom;
						needs_adjustment = true;
					}
				}

				/* Ensure geometry is within bounds */
				if (geo.x < usable.x) {
					geo.width -= (usable.x - geo.x);
					geo.x = usable.x;
				}
				if (geo.y < usable.y) {
					geo.height -= (usable.y - geo.y);
					geo.y = usable.y;
				}
				if (geo.x + geo.width > usable.x + usable.width) {
					geo.width = usable.x + usable.width - geo.x;
				}
				if (geo.y + geo.height > usable.y + usable.height) {
					geo.height = usable.y + usable.height - geo.y;
				}

				/* Update stored geometry if adjusted */
				if (needs_adjustment) {
					server->resized_view_geometry = geo;
				}

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

		/* Clean up adjacent views list */
		if (resized_view && !wl_list_empty(&adjacent_views)) {
			struct adjacent_view_entry *entry, *tmp;
			wl_list_for_each_safe(entry, tmp, &adjacent_views, link) {
				wl_list_remove(&entry->link);
				free(entry);
			}
		}
	}

	/* Proactively fill empty space - iterate until usable area is filled */
	if (!server->tiling_grid_mode) {
		const int max_iterations = 10; /* Prevent infinite loops */
		for (int iteration = 0; iteration < max_iterations; iteration++) {
			bool space_filled = true;
			
			wl_list_for_each(output, &server->outputs, link) {
				if (!output_is_usable(output)) {
					continue;
				}

				struct wlr_box usable = output_usable_area_in_layout_coords(output);
				
				/* Check for empty space by examining all tiled windows */
				/* Find the bounding box of all tiled windows */
				struct wlr_box occupied = {0};
				bool has_occupied = false;
				
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

					struct border margin = ssd_thickness(view);
					struct wlr_box view_full = (struct wlr_box){
						.x = view->current.x - margin.left,
						.y = view->current.y - margin.top,
						.width = view->current.width + margin.left + margin.right,
						.height = view->current.height + margin.top + margin.bottom,
					};

					if (!has_occupied) {
						occupied = view_full;
						has_occupied = true;
					} else {
						/* Expand bounding box to include this window */
						int occupied_right = occupied.x + occupied.width;
						int occupied_bottom = occupied.y + occupied.height;
						int view_right = view_full.x + view_full.width;
						int view_bottom = view_full.y + view_full.height;
						
						if (view_full.x < occupied.x) {
							occupied.width += occupied.x - view_full.x;
							occupied.x = view_full.x;
						}
						if (view_full.y < occupied.y) {
							occupied.height += occupied.y - view_full.y;
							occupied.y = view_full.y;
						}
						if (view_right > occupied_right) {
							occupied.width = view_right - occupied.x;
						}
						if (view_bottom > occupied_bottom) {
							occupied.height = view_bottom - occupied.y;
						}
					}
				}

				if (!has_occupied) {
					continue; /* No windows on this output */
				}

				/* Check for empty space around occupied area */
				/* Account for gaps in the occupied area calculation */
				int empty_left = occupied.x - usable.x;
				int empty_right = (usable.x + usable.width) - (occupied.x + occupied.width);
				int empty_top = occupied.y - usable.y;
				int empty_bottom = (usable.y + usable.height) - (occupied.y + occupied.height);

				/* Check if there's significant empty space (more than gap) */
				if (empty_left > rc.gap || empty_right > rc.gap ||
				    empty_top > rc.gap || empty_bottom > rc.gap) {
					space_filled = false;

					/* Find all windows that can expand to fill empty space */
					/* Expand multiple windows in one iteration for efficiency */
					int windows_expanded = 0;
					
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

						/* Skip resized view if it exists - it should maintain its size */
						if (server->resized_view == view) {
							continue;
						}

						struct border margin = ssd_thickness(view);
						struct wlr_box view_full = (struct wlr_box){
							.x = view->current.x - margin.left,
							.y = view->current.y - margin.top,
							.width = view->current.width + margin.left + margin.right,
							.height = view->current.height + margin.top + margin.bottom,
						};

						struct wlr_box new_geo = view->current;
						bool expanded = false;

						/* Check if window is on left edge and can expand left */
						if (empty_left > rc.gap && abs(view_full.x - occupied.x) <= rc.gap + 5) {
							int expand_amount = empty_left - rc.gap;
							new_geo.x = usable.x + margin.left;
							new_geo.width += expand_amount;
							expanded = true;
						}
						/* Check if window is on right edge and can expand right */
						if (empty_right > rc.gap && 
						    abs((view_full.x + view_full.width) - (occupied.x + occupied.width)) <= rc.gap + 5) {
							int expand_amount = empty_right - rc.gap;
							new_geo.width += expand_amount;
							expanded = true;
						}
						/* Check if window is on top edge and can expand top */
						if (empty_top > rc.gap && abs(view_full.y - occupied.y) <= rc.gap + 5) {
							int expand_amount = empty_top - rc.gap;
							new_geo.y = usable.y + margin.top;
							new_geo.height += expand_amount;
							expanded = true;
						}
						/* Check if window is on bottom edge and can expand bottom */
						if (empty_bottom > rc.gap &&
						    abs((view_full.y + view_full.height) - (occupied.y + occupied.height)) <= rc.gap + 5) {
							int expand_amount = empty_bottom - rc.gap;
							new_geo.height += expand_amount;
							expanded = true;
						}

						if (expanded) {
							/* Ensure geometry is within bounds */
							if (new_geo.x < usable.x) {
								new_geo.width -= (usable.x - new_geo.x);
								new_geo.x = usable.x;
							}
							if (new_geo.y < usable.y) {
								new_geo.height -= (usable.y - new_geo.y);
								new_geo.y = usable.y;
							}
							if (new_geo.x + new_geo.width > usable.x + usable.width) {
								new_geo.width = usable.x + usable.width - new_geo.x;
							}
							if (new_geo.y + new_geo.height > usable.y + usable.height) {
								new_geo.height = usable.y + usable.height - new_geo.y;
							}

							view_move_resize(view, new_geo);
							windows_expanded++;
							
							/* Update occupied area for next window check */
							view_full = (struct wlr_box){
								.x = new_geo.x - margin.left,
								.y = new_geo.y - margin.top,
								.width = new_geo.width + margin.left + margin.right,
								.height = new_geo.height + margin.top + margin.bottom,
							};
							
							/* Recalculate occupied area */
							if (view_full.x < occupied.x) {
								occupied.width += occupied.x - view_full.x;
								occupied.x = view_full.x;
							}
							if (view_full.y < occupied.y) {
								occupied.height += occupied.y - view_full.y;
								occupied.y = view_full.y;
							}
							if (view_full.x + view_full.width > occupied.x + occupied.width) {
								occupied.width = (view_full.x + view_full.width) - occupied.x;
							}
							if (view_full.y + view_full.height > occupied.y + occupied.height) {
								occupied.height = (view_full.y + view_full.height) - occupied.y;
							}
							
							/* Recalculate empty space */
							empty_left = occupied.x - usable.x;
							empty_right = (usable.x + usable.width) - (occupied.x + occupied.width);
							empty_top = occupied.y - usable.y;
							empty_bottom = (usable.y + usable.height) - (occupied.y + occupied.height);
						}
					}
					
					/* If we expanded windows, continue to next iteration to check for more space */
					if (windows_expanded == 0) {
						/* No windows could expand, but there's still empty space */
						/* This might happen if all windows are resized views or fixed position */
						space_filled = true; /* Give up on this output */
					}
				}
			}

			/* If no space was filled in this iteration, we're done */
			if (space_filled) {
				break;
			}
		}
	}
}

