/* SPDX-License-Identifier: MIT */
#ifndef FYAI_ROUTE_H
#define FYAI_ROUTE_H

#include "fyai.h"

/* Automatic routing is opt-in. The turn pointer is replaced with provenance. */
int fyai_route_select(struct fyai_ctx *ctx, fy_generic *turnp);
int fyai_route_budget_check(struct fyai_ctx *ctx, fy_generic turn);
void fyai_route_restore(struct fyai_ctx *ctx);
fy_generic fyai_route_price_usage(struct fyai_ctx *ctx, fy_generic usage);

#endif
