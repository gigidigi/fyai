/*
 * fyai_cmd_page.c - handlers of the page commands
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_cmd.h"
#include "fyai_cmd_int.h"
#include "fyai_page.h"
#include "fyai_sink.h"
#include "fyai_ui.h"
#include "utils.h"

#define FYAI_MODULE FYAIEM_DISPLAY

int fyai_cmd_page(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	return fyai_ui_page_report(call->ctx);
}

int fyai_cmd_page_review(struct fyai_cmd_call *call, fy_generic *result)
{
	bool on;

	if (fyai_ui_page_review(call->ctx, fyai_cmd_arg_str(call, "how"), &on))
		return -1;
	*result = fy_mapping(call->gb, "review", on ? "on" : "off");
	return 0;
}

/* The state of the sample @name, fitted to @height rows. */
static void page_sample_state(const char *name, int height,
			      struct fyai_page_state *st)
{
	static const char *const options[] = {
		"Yes", "No", "Show me the diff first",
	};

	memset(st, 0, sizeof(*st));
	st->header = "~/project  main  gpt-5";
	st->elapsed = " 12s";
	st->activity = "*";
	st->gutter_cols = 2;
	st->status = "working: 2 tools";
	st->hint = "Ctrl-T moves the keys to a tile";
	st->prompt_rows = 1;
	st->prompt_card = true;
	st->input_mode = "prompt";
	st->fullscreen = strcmp(name, "inline") != 0;
	/* The pane and the tail ask for rows; the fit gives them theirs. */
	st->pane_rows = 6;
	st->cap = "3 tiles <fy-fill/> Ctrl-T";
	st->tail_rows = height;
	if (!strcmp(name, "question")) {
		st->input_mode = "ask";
		st->ask_question = "Apply the patch?";
		st->ask_from = "main/agent:review";
		st->ask_options = options;
		st->ask_noptions = sizeof(options) / sizeof(options[0]);
		st->ask_waiting = 1;
	} else if (!strcmp(name, "popup")) {
		st->popup_title = "/stats";
	}
	fyai_page_fit(st, height);
}

int fyai_cmd_page_review_sample(struct fyai_cmd_call *call,
				fy_generic *result)
{
	struct fyai_ctx *ctx = call->ctx;
	struct response_buffer picture = {0};
	const struct fyai_page_action *actions;
	struct fyai_page_state st;
	struct fyai_page *pg = NULL;
	size_t nactions;
	int cols, rows, rc;

	cols = (int)fy_get(call->args, "width", 80LL);
	rows = (int)fy_get(call->args, "height", 24LL);
	page_sample_state(fyai_cmd_arg_str(call, "sample"), rows, &st);
	fyai_ui_page_actions(&actions, &nactions);
	st.actions = actions;
	st.nactions = nactions;
	pg = fyai_page_create_from(ctx, fyai_cmd_arg_str(call, "page"), true,
				   actions, nactions);
	fyai_error_check(ctx, pg, err_out, "cannot make the page");
	rc = fyai_page_review(pg, fyai_page_state_generic(call->gb, &st), cols,
			      rows, call->gb, &picture, result);
	if (rc)
		goto err_out;
	/* The picture is presentation; a machine format takes the areas. */
	if (call->format == FYAI_CMD_OUT_MARKDOWN && picture.data) {
		rc = fyai_sink_write(ctx->sink, FYAI_SINK_NOTICE, picture.data,
				     picture.len);
		fyai_error_check(ctx, !rc, err_out,
				 "cannot write the picture of the page");
	}
	free(picture.data);
	fyai_page_destroy(pg);
	return 0;

err_out:
	free(picture.data);
	fyai_page_destroy(pg);
	return -1;
}
