/*
 * fyai_desktop.c - invocation-local control for an arena-backed session
 *
 * SPDX-License-Identifier: MIT
 */
#define FYAI_MODULE FYAIEM_UNKNOWN

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "commands.h"
#include "fyai.h"
#include "fyai_cmd.h"
#include "fyai_desktop.h"
#include "fyai_display.h"
#include "fyai_event.h"
#include "fyai_jsonrpc.h"
#include "fyai_output.h"
#include "fyai_storage.h"
#include "fyai_turn.h"

struct desktop_rpc {
	struct fyai_ctx *ctx;
	struct jsonrpc_conn *conn;
	fy_generic run_id;
	char *prompt;
	bool pending;
	bool running;
	bool quit;
};

static fy_generic desktop_error(struct fyai_ctx *ctx, long long code,
				const char *message)
{
	return fy_mapping(fyai_ctx_transient_gb(ctx),
			  "code", code, "message", message);
}

void fyai_desktop_emit_text(struct fyai_ctx *ctx, const char *kind,
			    const char *text, size_t len)
{
	struct fy_generic_builder *gb;
	char *copy;

	if (!ctx || !ctx->desktop_rpc || !text || !len)
		return;
	gb = fyai_ctx_transient_gb(ctx);
	if (!gb)
		return;
	copy = strndup(text, len);
	if (!copy)
		return;
	(void)jsonrpc_notify(ctx->desktop_rpc, "session/update",
		fy_mapping(gb, "kind", kind, "text", fy_value(gb, copy)));
	free(copy);
}

void fyai_desktop_emit_tool(struct fyai_ctx *ctx, fy_generic call,
			    const char *state, bool ok)
{
	struct fy_generic_builder *gb;
	fy_generic name, id;

	if (!ctx || !ctx->desktop_rpc)
		return;
	gb = fyai_ctx_transient_gb(ctx);
	if (!gb)
		return;
	name = fy_get(call, "name", fy_invalid);
	if (!fy_is_string(name))
		name = fy_get(fy_get(call, "function", fy_invalid),
			      "name", fy_invalid);
	if (!fy_is_string(name))
		name = fy_value("");
	id = fy_get(call, "id", fy_invalid);
	if (!fy_is_string(id))
		id = fy_get(call, "call_id", fy_invalid);
	if (!fy_is_string(id))
		id = fy_value("");
	(void)jsonrpc_notify(ctx->desktop_rpc, "session/update",
		fy_mapping(gb, "kind", "tool", "name", name,
			   "callId", id, "state", state,
			   "ok", ok ? fy_true : fy_false));
}

fy_generic fyai_desktop_ask_user(struct fyai_ctx *ctx, fy_generic args)
{
	struct jsonrpc_request *req;
	struct fy_generic_builder *gb;
	fy_generic answer = fy_invalid;
	fy_generic result;

	gb = fyai_ctx_transient_gb(ctx);
	if (!gb)
		return fy_invalid;
	if (!ctx->desktop_rpc)
		return fy_value(gb, "tool error: desktop question channel unavailable");
	req = jsonrpc_request_submit(ctx->desktop_rpc, "user/ask", args,
				     jsonrpc_conn_next_id(ctx->desktop_rpc),
				     false, NULL, NULL);
	if (!req)
		return fy_value(gb, "tool error: the question could not be asked");
	while (!jsonrpc_request_done(req) && !ctx->desktop_cancel_requested &&
	       !jsonrpc_conn_closed(ctx->desktop_rpc))
		if (fyai_event_loop_step(fyai_ctx_loop(ctx), -1) < 0)
			break;
	if (!jsonrpc_request_done(req))
		jsonrpc_request_cancel(req);
	if (jsonrpc_request_ok(req)) {
		result = jsonrpc_request_result(req);
		answer = fy_get(result, "answer", fy_invalid);
		if (fy_is_string(answer))
			answer = fy_value(gb, fy_castp(&answer, ""));
	}
	jsonrpc_request_destroy(req);
	if (!fy_is_string(answer) || fy_empty(answer))
		return fy_value(gb, "tool note: the user did not provide an answer");
	return answer;
}

static int desktop_history_turn(void *userdata, fy_generic turn)
{
	struct {
		struct fy_generic_builder *gb;
		fy_generic messages;
		fy_generic documents;
	} *history = userdata;
	fy_generic message, document;

	fy_foreach(message, fy_get(turn, "messages", fy_seq_empty)) {
		history->messages = fy_append(history->gb, history->messages,
					      message);
		if (fy_is_invalid(history->messages))
			return -1;
	}
	fy_foreach(document, fy_get(turn, "display_outputs", fy_seq_empty)) {
		history->documents = fy_append(history->gb,
					       history->documents, document);
		if (fy_is_invalid(history->documents))
			return -1;
	}
	return 0;
}

static fy_generic desktop_serve(struct jsonrpc_conn *conn, const char *method,
				fy_generic params, fy_generic id,
				void *userdata, fy_generic *errorp)
{
	struct desktop_rpc *rpc = userdata;
	struct fyai_ctx *ctx = rpc->ctx;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	struct fyai_turn_selector_args sel = { .type = FYAITST_ALL };
	struct {
		struct fy_generic_builder *gb;
		fy_generic messages;
		fy_generic documents;
	} history;
	const char *prompt;

	if (!strcmp(method, "initialize"))
		return fy_mapping(gb, "protocol", 1LL,
				  "agentId", "fyai-desktop",
				  "history", fy_true, "questions", fy_true,
				  "cancel", fy_true);
	if (!strcmp(method, "shutdown")) {
		rpc->quit = true;
		ctx->desktop_cancel_requested = rpc->running;
		return fy_null;
	}
	if (!strcmp(method, "session/history")) {
		history.gb = gb;
		history.messages = fy_sequence(gb);
		history.documents = fy_sequence(gb);
		if (fyai_display_foreach_turn(ctx, &sel, desktop_history_turn,
					      &history)) {
			*errorp = desktop_error(ctx, -32000,
						"could not read session history");
			return fy_invalid;
		}
		return fy_mapping(gb, "messages", history.messages,
				  "documents", history.documents,
				  "documentsComplete",
				  fyai_display_stored_complete(ctx) ?
					fy_true : fy_false);
	}
	if (!strcmp(method, "session/cancel")) {
		ctx->desktop_cancel_requested = rpc->running;
		return fy_mapping(gb, "accepted",
				  rpc->running ? fy_true : fy_false);
	}
	if (!strcmp(method, "session/prompt")) {
		if (!fy_is_valid(id))
			return fy_invalid;
		if (rpc->running || rpc->pending) {
			*errorp = desktop_error(ctx, -32003, "session is busy");
			return fy_invalid;
		}
		prompt = fy_get(params, "prompt", "");
		if (!prompt || !*prompt || strlen(prompt) > 100000) {
			*errorp = desktop_error(ctx, -32602, "prompt is required");
			return fy_invalid;
		}
		rpc->prompt = strdup(prompt);
		if (!rpc->prompt) {
			*errorp = desktop_error(ctx, -32000, "out of memory");
			return fy_invalid;
		}
		rpc->run_id = id;
		rpc->pending = true;
		jsonrpc_conn_defer(conn);
		return fy_invalid;
	}
	*errorp = desktop_error(ctx, -32601, "method not found");
	return fy_invalid;
}

static void desktop_run_prompt(struct desktop_rpc *rpc)
{
	struct fyai_ctx *ctx = rpc->ctx;
	struct fy_generic_builder *gb;
	fy_generic turn, result;
	const char *state = "failed";
	int rc = -1;

	rpc->pending = false;
	rpc->running = true;
	gb = fyai_ctx_transient_gb(ctx);
	turn = fyai_turn_append(ctx, ctx->last_message,
			fy_sequence(gb, fyai_make_user_message(ctx, rpc->prompt)));
	if (fy_is_valid(turn))
		turn = fyai_output_record(ctx, turn, FYAI_OUTPUT_USER,
					  rpc->prompt);
	if (fy_is_valid(turn)) {
		result = fyai_run_turn(ctx, turn);
		result = fyai_report_diag(ctx, result);
		if (fy_is_valid(result)) {
			ctx->last_message = result;
			rc = fyai_publish_state(ctx);
			if (!rc)
				state = ctx->desktop_cancel_requested ?
					"cancelled" : "published";
		}
	}
	if (ctx->desktop_cancel_requested)
		state = "cancelled";
	(void)jsonrpc_conn_respond(rpc->conn, rpc->run_id,
		fy_mapping(gb, "status", state, "published",
			   rc == 0 ? fy_true : fy_false),
		fy_invalid);
	free(rpc->prompt);
	rpc->prompt = NULL;
	rpc->running = false;
	ctx->desktop_cancel_requested = false;
	fyai_cleanup_transient_builder(ctx);
}

int fyai_cmd_desktop_prepare(struct fyai_cfg *cfg, fy_generic args)
{
	(void)args;
	cfg->interactive = false;
	cfg->desktop_rpc = true;
	return 0;
}

int fyai_cmd_desktop(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = call->ctx;
	struct desktop_rpc rpc = { .ctx = ctx };
	struct fyai_event_loop *el = fyai_ctx_loop(ctx);
	struct jsonrpc_conn *conn;
	int rc = -1;

	(void)result;
	if (!el)
		return -1;
	conn = jsonrpc_conn_stdio(ctx, STDOUT_FILENO, STDIN_FILENO,
				  0, "desktop", NULL);
	if (!conn)
		return -1;
	rpc.conn = conn;
	ctx->desktop_rpc = conn;
	if (jsonrpc_conn_serve(conn, desktop_serve, &rpc))
		goto out;
	while (!rpc.quit && !jsonrpc_conn_closed(conn)) {
		if (rpc.pending) {
			desktop_run_prompt(&rpc);
			continue;
		}
		if (fyai_event_loop_step(el, -1) < 0)
			goto out;
	}
	while (jsonrpc_conn_has_output(conn))
		if (fyai_event_loop_step(el, 1000) <= 0)
			break;
	rc = 0;
out:
	free(rpc.prompt);
	ctx->desktop_rpc = NULL;
	jsonrpc_conn_destroy(conn);
	return rc;
}
