/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_OUTPUT_VIRTUAL_H
#define LABWC_OUTPUT_VIRTUAL_H

#include <stdint.h>

struct server;
struct wlr_output;

void output_virtual_add(struct server *server, const char *output_name,
		struct wlr_output **store_wlr_output, int width, int height,
		int32_t refresh);
void output_virtual_remove(struct server *server, const char *output_name);
void output_virtual_update_fallback(struct server *server);

#endif
