/* SPDX-License-Identifier: MIT */
#ifndef FYAI_DESKTOP_H
#define FYAI_DESKTOP_H

#include <stddef.h>
#include <stdbool.h>

#include "fyai.h"

void fyai_desktop_emit_text(struct fyai_ctx *ctx, const char *kind,
			    const char *text, size_t len);
void fyai_desktop_emit_tool(struct fyai_ctx *ctx, fy_generic call,
			    const char *state, bool ok);
fy_generic fyai_desktop_ask_user(struct fyai_ctx *ctx, fy_generic args);

#endif
