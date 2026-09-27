/*
 * fyai_cmd_session.c - handlers of the commands that only a session has
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * These act on the live session: its tiles, its pickers, and its side
 * questions. The backends present what they do.
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
#include "fyai_browser.h"
#include "fyai_sink.h"
#include "fyai_session.h"
#include "fyai_tools.h"
#include "fyai_ui.h"

#define FYAI_MODULE FYAIEM_SESSION

/* The dispatcher ends the session before a handler runs. */
int fyai_cmd_exit(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	fyai_ui_quit_request(call->ctx);
	return 0;
}

int fyai_cmd_btw(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	return fyai_session_btw(call->ctx, fyai_cmd_arg_str(call, "question"));
}

int fyai_cmd_branches(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	return fyai_browser_open(call->ctx);
}

/*
 * A named session is selected for this invocation; HEAD does not move. With
 * no name, the picker selects: in a session, and before the verb starts its
 * session.
 */
int fyai_cmd_resume(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *name = fyai_cmd_arg_str(call, "session");

	(void)result;
	/* The verb selected its session before setup; the prompt runs it. */
	if (call->surface == FYAI_CMD_CLI)
		return fyai_prompt(call->ctx);
	if (!name)
		return fyai_browser_open_switch(call->ctx,
						fyai_cmd_arg_bool(call, "all"));
	return fyai_session_branch_switch(call->ctx, name, false, true);
}

int fyai_cmd_zoom(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *name = fyai_cmd_arg_str(call, "name");
	const char *what;

	if (name && !strcmp(name, "off")) {
		/* fyai_tools_unzoom() reports the focus change. */
		fyai_tools_unzoom(call->ctx);
		return 0;
	}
	what = fyai_tools_zoom(call->ctx, name);
	*result = what ? fy_mapping(call->gb, "zoomed", what) :
		  fy_mapping(call->gb, "unknown", (bool)name,
			     "idle", !name);
	return 0;
}

int fyai_cmd_page(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	return fyai_ui_page_report(call->ctx);
}

int fyai_cmd_sessions(struct fyai_cmd_call *call, fy_generic *result)
{
	*result = fyai_tools_sessions_data(call->ctx, call->gb);
	fyai_error_check(call->ctx, fy_is_valid(*result), err,
			 "sessions: cannot list the sessions");
	return 0;
err:
	return -1;
}

int fyai_cmd_kill(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *name = fyai_cmd_arg_str(call, "name");
	const char *action = NULL;

	if (fyai_tools_kill(call->ctx, name, &action))
		return -1;
	*result = fy_mapping(call->gb, "name", name, "action", action);
	return 0;
}

/* One table shows the sections of the status: `Auth / status`, ... */
static fy_generic status_flatten(struct fy_generic_builder *gb,
				 fy_generic data)
{
	fy_generic out, key, value, k2, v2;
	const char *prefix, *name;

	out = fy_map_empty;
	fy_foreach_key_value(key, value, data) {
		if (!fy_is_mapping(value)) {
			out = fy_assoc(gb, out, key, value);
			continue;
		}
		prefix = fy_equal(key, "auth") ? "Auth" : "Usage";
		fy_foreach_key_value(k2, v2, value) {
			if (fy_is_mapping(v2) || fy_is_sequence(v2))
				continue;
			name = fy_sprintfa("%s / %s", prefix,
					   fy_castp(&k2, ""));
			out = fy_assoc(gb, out, name, v2);
		}
	}
	return out;
}

int fyai_cmd_status(struct fyai_cmd_call *call, fy_generic *result)
{
	fy_generic data;

	data = fyai_session_status_data(call->ctx, call->gb);
	fyai_error_check(call->ctx, fy_is_valid(data), err,
			 "status: cannot build the report");
	*result = call->format == FYAI_CMD_OUT_MARKDOWN ?
		  status_flatten(call->gb, data) : data;
	return 0;
err:
	return -1;
}
