// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L
#include "config/keybind.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wlr/types/wlr_keyboard_group.h>
#include <wlr/util/log.h>
#include "common/list.h"
#include "common/mem.h"
#include "common/spawn.h"
#include "config/rcxml.h"
#include "labwc.h"

uint32_t
parse_modifier(const char *symname)
{
	/* Mod2 == NumLock */
	if (!strcmp(symname, "S")) {
		return WLR_MODIFIER_SHIFT;
	} else if (!strcmp(symname, "C")) {
		return WLR_MODIFIER_CTRL;
	} else if (!strcmp(symname, "A") || !strcmp(symname, "Mod1")) {
		return WLR_MODIFIER_ALT;
	} else if (!strcmp(symname, "W") || !strcmp(symname, "Mod4")) {
		return WLR_MODIFIER_LOGO;
	} else if (!strcmp(symname, "M") || !strcmp(symname, "Mod5")) {
		return WLR_MODIFIER_MOD5;
	} else if (!strcmp(symname, "H") || !strcmp(symname, "Mod3")) {
		return WLR_MODIFIER_MOD3;
	} else {
		return 0;
	}
}

bool
keybind_the_same(struct keybind *a, struct keybind *b)
{
	assert(a && b);
	if (a->modifiers != b->modifiers || a->keysyms_len != b->keysyms_len) {
		return false;
	}
	for (size_t i = 0; i < a->keysyms_len; i++) {
		if (a->keysyms[i] != b->keysyms[i]) {
			return false;
		}
	}
	return true;
}

bool
keybind_contains_keycode(struct keybind *keybind, xkb_keycode_t keycode)
{
	assert(keybind);
	for (size_t i = 0; i < keybind->keycodes_len; i++) {
		if (keybind->keycodes[i] == keycode) {
			return true;
		}
	}
	return false;
}

bool
keybind_contains_keysym(struct keybind *keybind, xkb_keysym_t keysym)
{
	assert(keybind);
	for (size_t i = 0; i < keybind->keysyms_len; i++) {
		if (keybind->keysyms[i] == keysym) {
			return true;
		}
	}
	return false;
}

static bool
keybind_contains_any_keysym(struct keybind *keybind,
		const xkb_keysym_t *syms, int nr_syms)
{
	for (int i = 0; i < nr_syms; i++) {
		if (keybind_contains_keysym(keybind, syms[i])) {
			return true;
		}
	}
	return false;
}

static void
update_keycodes_iter(struct xkb_keymap *keymap, xkb_keycode_t key, void *data)
{
	struct keybind *keybind;
	const xkb_keysym_t *syms;
	xkb_layout_index_t layout = *(xkb_layout_index_t *)data;
	int nr_syms = xkb_keymap_key_get_syms_by_level(keymap, key, layout, 0, &syms);
	if (!nr_syms) {
		return;
	}
	wl_list_for_each(keybind, &rc.keybinds, link) {
		if (keybind->keycodes_layout >= 0
				&& (xkb_layout_index_t)keybind->keycodes_layout != layout) {
			/* Prevent storing keycodes from multiple layouts */
			continue;
		}
		if (keybind->use_syms_only) {
			continue;
		}
		if (keybind_contains_any_keysym(keybind, syms, nr_syms)) {
			if (keybind_contains_keycode(keybind, key)) {
				/* Prevent storing the same keycode twice */
				continue;
			}
			if (keybind->keycodes_len == MAX_KEYCODES) {
				wlr_log(WLR_ERROR,
					"Already stored %lu keycodes for keybind",
					keybind->keycodes_len);
				continue;
			}
			keybind->keycodes[keybind->keycodes_len++] = key;
			keybind->keycodes_layout = layout;
		}
	}
}

void
keybind_update_keycodes(struct server *server)
{
	struct xkb_state *state = server->seat.keyboard_group->keyboard.xkb_state;
	struct xkb_keymap *keymap = xkb_state_get_keymap(state);

	struct keybind *keybind;
	wl_list_for_each(keybind, &rc.keybinds, link) {
		keybind->keycodes_len = 0;
		keybind->keycodes_layout = -1;
	}
	xkb_layout_index_t layouts = xkb_keymap_num_layouts(keymap);
	for (xkb_layout_index_t i = 0; i < layouts; i++) {
		wlr_log(WLR_DEBUG, "Found layout %s", xkb_keymap_layout_get_name(keymap, i));
		xkb_keymap_key_for_each(keymap, update_keycodes_iter, &i);
	}
}

struct keybind *
keybind_find_by_id(const char *id)
{
	if (!id) {
		return NULL;
	}
	struct keybind *keybind;
	wl_list_for_each(keybind, &rc.keybinds, link) {
		if (keybind->id && !strcmp(keybind->id, id)) {
			return keybind;
		}
	}
	return NULL;
}

struct keybind *
keybind_create(const char *keybind)
{
	xkb_keysym_t sym;
	struct keybind *k = znew(*k);
	xkb_keysym_t keysyms[MAX_KEYSYMS];
	gchar **symnames = g_strsplit(keybind, "-", -1);
	for (size_t i = 0; symnames[i]; i++) {
		const char *symname = symnames[i];
		/*
		 * Since "-" is used as a separator, a keybind string like "W--"
		 * becomes "W", "", "". This means that it is impossible to bind
		 * an action to the "-" key in this way.
		 * We detect empty ""s outputted by g_strsplit and treat them as
		 * literal "-"s.
		 */
		if (!symname[0]) {
			/*
			 * You might have noticed that in the "W--" example, the
			 * output is "W", "", ""; which turns into "W", "-",
			 * "-". In order to avoid such duplications, we perform
			 * a lookahead on the tokens to treat that edge-case.
			 */
			if (symnames[i+1] && !symnames[i+1][0]) {
				continue;
			}
			symname = "-";
		}
		uint32_t modifier = parse_modifier(symname);
		if (modifier != 0) {
			k->modifiers |= modifier;
		} else {
			sym = xkb_keysym_from_name(symname, XKB_KEYSYM_CASE_INSENSITIVE);
			if (sym == XKB_KEY_NoSymbol && g_utf8_strlen(symname, -1) == 1) {
				/*
				 * xkb_keysym_from_name() only handles a legacy set of single
				 * characters. Thus we try to get the unicode codepoint here
				 * and try a direct translation instead.
				 *
				 * This allows using keybinds like 'W-ö' and similar.
				 */
				gunichar codepoint = g_utf8_get_char_validated(symname, -1);
				if (codepoint != (gunichar)-1) {
					sym = xkb_utf32_to_keysym(codepoint);
				}
			}
			sym = xkb_keysym_to_lower(sym);
			if (sym == XKB_KEY_NoSymbol) {
				wlr_log(WLR_ERROR, "unknown keybind (%s)", symname);
				free(k);
				k = NULL;
				break;
			}
			keysyms[k->keysyms_len] = sym;
			wlr_log(WLR_INFO, "keybind_create: added keysym %u (0x%x) for keybind '%s'", 
				sym, sym, keybind);
			k->keysyms_len++;
			if (k->keysyms_len == MAX_KEYSYMS) {
				wlr_log(WLR_ERROR, "There are a lot of fingers involved. "
					"We stopped counting at %u.", MAX_KEYSYMS);
				wlr_log(WLR_ERROR, "Offending keybind was %s", keybind);
				break;
			}
		}
	}
	g_strfreev(symnames);
	if (!k) {
		return NULL;
	}
	wl_list_append(&rc.keybinds, &k->link);
	k->keysyms = xmalloc(k->keysyms_len * sizeof(xkb_keysym_t));
	memcpy(k->keysyms, keysyms, k->keysyms_len * sizeof(xkb_keysym_t));
	wl_list_init(&k->actions);
	wl_list_init(&k->device_blacklist);
	wl_list_init(&k->device_whitelist);
	k->toggleable = false;
	k->enabled = true;
	k->id = NULL;
	k->condition_command = NULL;
	k->condition_values = NULL;
	k->condition_values_len = 0;
	return k;
}

void
keybind_destroy(struct keybind *keybind)
{
	assert(wl_list_empty(&keybind->actions));

	struct keybind_device_blacklist *entry, *entry_tmp;
	wl_list_for_each_safe(entry, entry_tmp, &keybind->device_blacklist, link) {
		wl_list_remove(&entry->link);
		zfree(entry->device_name);
		zfree(entry);
	}

	struct keybind_device_whitelist *whitelist_entry, *whitelist_entry_tmp;
	wl_list_for_each_safe(whitelist_entry, whitelist_entry_tmp, &keybind->device_whitelist, link) {
		wl_list_remove(&whitelist_entry->link);
		zfree(whitelist_entry->device_name);
		zfree(whitelist_entry);
	}

	zfree(keybind->keysyms);
	zfree(keybind->id);
	if (keybind->condition_values) {
		for (size_t i = 0; i < keybind->condition_values_len; i++) {
			zfree(keybind->condition_values[i]);
		}
		zfree(keybind->condition_values);
	}
	zfree(keybind->condition_command);
	zfree(keybind);
}

bool
keybind_check_condition_sync(struct keybind *keybind)
{
	if (!keybind->condition_command) {
		/* No condition, always true */
		return true;
	}

	int pipe_fd = 0;
	pid_t pid = spawn_piped(keybind->condition_command, &pipe_fd);
	if (pid <= 0) {
		wlr_log(WLR_ERROR, "Failed to spawn condition command: %s",
			keybind->condition_command);
		return false;
	}

	/* Read output synchronously */
	char buffer[4096];
	ssize_t total_read = 0;
	ssize_t n;
	while ((n = read(pipe_fd, buffer + total_read,
			sizeof(buffer) - total_read - 1)) > 0) {
		total_read += n;
		if (total_read >= (ssize_t)sizeof(buffer) - 1) {
			break;
		}
	}
	buffer[total_read] = '\0';

	/* Wait for process to finish */
	spawn_piped_close(pid, pipe_fd);

	/* Trim trailing newlines and whitespace */
	size_t len = strlen(buffer);
	while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r' ||
			buffer[len - 1] == ' ' || buffer[len - 1] == '\t')) {
		len--;
	}
	buffer[len] = '\0';

	/* Check if output matches any expected value */
	if (keybind->condition_values_len > 0) {
		for (size_t i = 0; i < keybind->condition_values_len; i++) {
			if (strcmp(buffer, keybind->condition_values[i]) == 0) {
				return true;
			}
		}
		return false;
	} else {
		/* If no values specified, any non-empty output is considered a match */
		return (len > 0);
	}
}
