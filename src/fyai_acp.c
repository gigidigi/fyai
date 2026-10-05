/*
 * fyai_acp.c - invocation-local ACP v1 session transport
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define FYAI_MODULE FYAIEM_UNKNOWN

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_auth.h"
#include "fyai_branch.h"
#include "fyai_catalog.h"
#include "fyai_cmd.h"
#include "fyai_cmd_int.h"
#include "fyai_config.h"
#include "fyai_display.h"
#include "fyai_event.h"
#include "fyai_jsonrpc.h"
#include "fyai_output.h"
#include "fyai_session.h"
#include "fyai_sink.h"
#include "fyai_storage.h"
#include "fyai_turn.h"

struct acp_session {
	struct fyai_ctx *ctx;
	struct jsonrpc_conn *conn;
	char *cwd;
	char *session_id;
	char *prompt;
	fy_generic prompt_id;
	fy_generic close_id;
	struct fyai_turn_run *run;
	bool initialized;
	bool pending;
	bool cancelled;
	bool close_pending;
	bool quit;
	bool form_capable;
	bool notice_capable;
	bool replaying;
	struct acp_edit_snapshot *edits;
	struct acp_replay_call *replay_calls;
};

struct acp_edit_snapshot {
	char *id;
	char *path;
	char *old_text;
	bool old_unavailable;
	struct acp_edit_snapshot *next;
};

struct acp_replay_call {
	char *id;
	fy_generic call;
	struct acp_replay_call *next;
};

static fy_generic acp_call_id(fy_generic call)
{
	fy_generic id;

	id = fy_get(call, "call_id", fy_invalid);
	return fy_is_string(id) ? id : fy_get(call, "id", fy_invalid);
}

static fy_generic acp_call_name(fy_generic call)
{
	fy_generic name;

	if (fy_equal(fy_get(call, "type", fy_invalid), "shell_call"))
		return fy_value("shell");
	name = fy_get(fy_get(call, "function", fy_invalid), "name",
		      fy_invalid);
	if (!fy_is_string(name))
		name = fy_get(call, "name", fy_invalid);
	return name;
}

static const char *acp_tool_kind(const char *name)
{
	if (!strcmp(name, "read_file"))
		return "read";
	if (!strcmp(name, "write_file") || !strcmp(name, "apply_patch"))
		return "edit";
	if (!strcmp(name, "shell") || !strcmp(name, "agent"))
		return "execute";
	return "other";
}

static fy_generic acp_call_args(struct acp_session *acp, fy_generic call)
{
	fy_generic value;
	const char *json;

	value = fy_get(fy_get(call, "function", fy_invalid), "arguments",
		       fy_invalid);
	if (!fy_is_string(value))
		value = fy_get(call, "arguments", fy_invalid);
	json = fy_castp(&value, "{}");
	return parse_json_string(fyai_ctx_transient_gb(acp->ctx), json);
}

/* Capture the pre-image before an edit runs. Keep only bounded display data. */
static void acp_edit_capture(struct acp_session *acp, const char *id,
			     const char *path)
{
	struct acp_edit_snapshot *edit;
	struct acp_edit_snapshot *existing;
	struct stat st;
	char *absolute;
	int fd;
	int rc;
	ssize_t n;

	if (!path || !*path || !id || !*id)
		return;
	absolute = *path == '/' ? strdup(path) : NULL;
	if (!absolute && asprintf(&absolute, "%s/%s", acp->cwd, path) < 0)
		return;
	if (!absolute)
		return;
	for (existing = acp->edits; existing; existing = existing->next)
		if (!strcmp(existing->id, id) &&
		    !strcmp(existing->path, absolute)) {
			free(absolute);
			return;
		}
	edit = calloc(1, sizeof(*edit));
	if (!edit) {
		free(absolute);
		return;
	}
	edit->id = strdup(id);
	edit->path = absolute;
	rc = stat(absolute, &st);
	if (!rc && S_ISREG(st.st_mode) &&
	    st.st_size >= 0 && st.st_size <= (1 << 20)) {
		fd = open(absolute, O_RDONLY | O_CLOEXEC);
		if (fd >= 0) {
			edit->old_text = malloc((size_t)st.st_size + 1);
			if (edit->old_text) {
				n = read(fd, edit->old_text, (size_t)st.st_size);
				if (n == st.st_size &&
				    !memchr(edit->old_text, '\0', (size_t)n))
					edit->old_text[n] = '\0';
				else {
					free(edit->old_text);
					edit->old_text = NULL;
					edit->old_unavailable = true;
				}
			} else
				edit->old_unavailable = true;
			close(fd);
		} else
			edit->old_unavailable = true;
	} else if (!rc || errno != ENOENT)
		edit->old_unavailable = true;
	if (!edit->id || !edit->path) {
		free(edit->old_text);
		free(edit->id);
		free(edit->path);
		free(edit);
		return;
	}
	edit->next = acp->edits;
	acp->edits = edit;
}

static void acp_patch_capture(struct acp_session *acp, const char *id,
			      const char *patch)
{
	const char *line, *end, *path;
	size_t len;
	char *copy;

	for (line = patch; *line; line = *end ? end + 1 : end) {
		end = strchr(line, '\n');
		if (!end)
			end = line + strlen(line);
		path = NULL;
		if (!strncmp(line, "*** Update File: ", 17) ||
		    !strncmp(line, "*** Delete File: ", 17))
			path = line + 17;
		else if (!strncmp(line, "*** Add File: ", 14) ||
			 !strncmp(line, "*** Move to: ", 13))
			path = line + (line[4] == 'A' ? 14 : 13);
		else if (!strncmp(line, "--- ", 4) ||
			 !strncmp(line, "+++ ", 4)) {
			path = line + 4;
			if (!strncmp(path, "a/", 2) ||
			    !strncmp(path, "b/", 2))
				path += 2;
			if ((size_t)(end - path) >= 9 &&
			    !strncmp(path, "/dev/null", 9))
				path = NULL;
		}
		if (!path)
			continue;
		len = (size_t)(end - path);
		if (line[0] == '-' || line[0] == '+')
			len = strcspn(path, "\t \r\n");
		if (!len || len > 4096)
			continue;
		copy = strndup(path, len);
		if (copy) {
			acp_edit_capture(acp, id, copy);
			free(copy);
		}
	}
}

/* NULL means the file cannot be represented as a bounded text diff. */
static char *acp_edit_after(const char *path)
{
	struct stat st;
	char *text;
	ssize_t n;
	int fd;

	if (stat(path, &st))
		return errno == ENOENT ? strdup("") : NULL;
	if (!S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > (1 << 20))
		return NULL;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return NULL;
	text = malloc((size_t)st.st_size + 1);
	if (text) {
		n = read(fd, text, (size_t)st.st_size);
		if (n == st.st_size && !memchr(text, '\0', (size_t)n))
			text[n] = '\0';
		else {
			free(text);
			text = NULL;
		}
	}
	close(fd);
	return text;
}

static void acp_tool(void *userdata, fy_generic call, fy_generic result,
		     const char *status)
{
	struct acp_session *acp = userdata;
	struct fy_generic_builder *gb;
	struct acp_edit_snapshot *edit;
	struct fyai_branch branch;
	fy_generic id_value, name_value, args, path_value, content_value, update;
	fy_generic locations, blocks, block;
	fy_generic stored, base_store, acp_store, diffs, diff, new_diffs;
	fy_generic saved;
	const char *id, *name, *path, *output, *new_text;
	char *bounded, *after;
	size_t keep;

	if (!acp->session_id || (!acp->prompt && !acp->replaying) ||
	    jsonrpc_conn_closed(acp->conn))
		return;
	gb = fyai_ctx_transient_gb(acp->ctx);
	id_value = acp_call_id(call);
	id = fy_castp(&id_value, "");
	if (!*id)
		return;
	name_value = acp_call_name(call);
	name = fy_castp(&name_value, "tool");
	args = acp_call_args(acp, call);
	path_value = fy_get(args, "path", fy_invalid);
	path = fy_castp(&path_value, "");
	if (!acp->replaying && !strcmp(status, "pending") &&
	    !strcmp(name, "write_file"))
		acp_edit_capture(acp, id, path);
	if (!acp->replaying && !strcmp(status, "pending") &&
	    !strcmp(name, "apply_patch")) {
		content_value = fy_get(args, "patch", fy_invalid);
		acp_patch_capture(acp, id, fy_castp(&content_value, ""));
	}
	base_store = acp->ctx->branch_store;
	if (!fy_is_mapping(base_store) &&
	    fyai_branch_decode(acp->ctx->branch_prev, &branch))
		base_store = branch.store;
	if (!fy_is_mapping(base_store))
		base_store = fy_map_empty;
	acp_store = fy_get(base_store, "_fyai/acp", fy_invalid);
	diffs = fy_get(acp_store, "diffs", fy_invalid);
	stored = fy_get(diffs, id, fy_invalid);
	locations = fy_seq_empty;
	for (edit = acp->edits; edit; edit = edit->next)
		if (!strcmp(edit->id, id))
			locations = fy_append(gb, locations,
				fy_mapping(gb, "path", edit->path));
	if (fy_empty(locations) && fy_is_sequence(stored))
		fy_foreach(saved, stored) {
			path_value = fy_get(saved, "path", fy_invalid);
			if (fy_is_string(path_value))
				locations = fy_append(gb, locations,
					fy_mapping(gb, "path", path_value));
		}
	blocks = fy_seq_empty;
	if (!strcmp(status, "completed") || !strcmp(status, "failed")) {
		output = fy_is_string(result) ? fy_castp(&result, "") :
			  emit_json_string(gb, result);
		if (!output)
			output = "";
		keep = strlen(output);
		if (keep > 4096) {
			keep = 4096;
			while (keep &&
			       ((unsigned char)output[keep] & 0xc0) == 0x80)
				keep--;
		}
		bounded = strndup(output, keep);
		if (bounded) {
			block = fy_mapping(gb, "type", "content",
				"content", fy_mapping(gb, "type", "text",
						       "text", bounded));
			blocks = fy_append(gb, blocks, block);
			free(bounded);
		}
		if (!strcmp(status, "completed") && !acp->replaying &&
		    (!strcmp(name, "write_file") ||
		     !strcmp(name, "apply_patch"))) {
			new_diffs = fy_seq_empty;
			for (edit = acp->edits; edit; edit = edit->next) {
				if (strcmp(edit->id, id) || edit->old_unavailable)
					continue;
				after = NULL;
				content_value = fy_get(args, "content", fy_invalid);
				if (!strcmp(name, "write_file") &&
				    fy_is_string(content_value))
					new_text = fy_castp(&content_value, "");
				else {
					after = acp_edit_after(edit->path);
					new_text = after;
				}
				if (!new_text || strlen(new_text) > (1 << 20)) {
					free(after);
					continue;
				}
				diff = fy_mapping(acp->ctx->gb,
					"path", edit->path, "newText", new_text);
				if (edit->old_text)
					diff = fy_assoc(acp->ctx->gb, diff,
							"oldText", edit->old_text);
				block = fy_assoc(gb, diff, "type", "diff");
				blocks = fy_append(gb, blocks, block);
				new_diffs = fy_append(acp->ctx->gb,
						      new_diffs, diff);
				free(after);
			}
			if (fy_len(new_diffs)) {
				if (!fy_is_mapping(diffs))
					diffs = fy_map_empty;
				diffs = fy_assoc(acp->ctx->gb, diffs,
						 id, new_diffs);
				if (!fy_is_mapping(acp_store))
					acp_store = fy_map_empty;
				acp_store = fy_assoc(acp->ctx->gb,
					acp_store, "diffs", diffs);
				acp->ctx->branch_store = fy_assoc(
					acp->ctx->gb, base_store,
					"_fyai/acp", acp_store);
			}
		}
		if (!strcmp(status, "completed") && acp->replaying &&
		    fy_is_sequence(stored))
			fy_foreach(saved, stored) {
				block = fy_assoc(gb, saved, "type", "diff");
				blocks = fy_append(gb, blocks, block);
			}
	}
	update = fy_mapping(gb,
		"sessionUpdate", !strcmp(status, "pending") ? "tool_call" :
							"tool_call_update",
		"toolCallId", id, "status", status,
		"name", name, "title", name, "kind", acp_tool_kind(name),
		"content", blocks, "locations", locations);
	(void)jsonrpc_notify(acp->conn, "session/update",
		fy_mapping(gb, "sessionId", acp->session_id,
				"update", update));
}

static bool acp_permission(void *userdata, fy_generic call)
{
	struct acp_session *acp = userdata;
	struct fy_generic_builder *gb;
	struct jsonrpc_request *req;
	fy_generic id_value, name_value, detail, content, tool, options;
	fy_generic params, reply, outcome, option;
	const char *id, *name, *text;
	char *preview;
	bool allowed = false;
	int rc;

	if (acp->cancelled || !acp->session_id ||
	    jsonrpc_conn_closed(acp->conn))
		return false;
	gb = fyai_ctx_transient_gb(acp->ctx);
	id_value = acp_call_id(call);
	id = fy_castp(&id_value, "");
	if (!*id)
		return false;
	name_value = acp_call_name(call);
	name = fy_castp(&name_value, "tool");
	detail = fy_equal(fy_get(call, "type", fy_invalid), "shell_call") ?
		fy_get(call, "action", fy_invalid) : acp_call_args(acp, call);
	text = emit_json_string(gb, detail);
	preview = text ? strndup(text, 4096) : NULL;
	if (!preview)
		return false;
	content = fy_sequence(gb, fy_mapping(gb,
		"type", "content", "content", fy_mapping(gb,
			"type", "text", "text", preview)));
	tool = fy_mapping(gb, "toolCallId", id, "title", name,
			  "name", name, "kind", acp_tool_kind(name),
			  "status", "pending", "content", content);
	options = fy_sequence(gb,
		fy_mapping(gb, "optionId", "allow-once",
			   "name", "Allow once", "kind", "allow_once"),
		fy_mapping(gb, "optionId", "deny-once",
			   "name", "Deny", "kind", "reject_once"));
	params = fy_mapping(gb, "sessionId", acp->session_id,
			    "toolCall", tool, "options", options);
	free(preview);
	req = jsonrpc_request_submit(acp->conn, "session/request_permission",
		params, jsonrpc_conn_next_id(acp->conn), false, NULL, NULL);
	if (!req)
		return false;
	while (!jsonrpc_request_done(req) && !acp->cancelled &&
	       !jsonrpc_conn_closed(acp->conn)) {
		rc = fyai_event_loop_step(fyai_ctx_loop(acp->ctx), -1);
		if (rc < 0)
			break;
	}
	if (!acp->cancelled && jsonrpc_request_done(req) &&
	    jsonrpc_request_ok(req)) {
		reply = jsonrpc_request_result(req);
		outcome = fy_get(reply, "outcome", fy_invalid);
		option = fy_get(outcome, "optionId", fy_invalid);
		allowed = fy_equal(fy_get(outcome, "outcome", fy_invalid),
				   "selected") && fy_equal(option, "allow-once");
	}
	if (!jsonrpc_request_done(req))
		jsonrpc_request_cancel(req);
	jsonrpc_request_destroy(req);
	return allowed;
}

static fy_generic acp_question(void *userdata, fy_generic args,
			       const char *tool_call_id)
{
	struct acp_session *acp = userdata;
	struct fy_generic_builder *gb;
	struct jsonrpc_request *req;
	fy_generic question, from, message, choices, choice, property, schema;
	fy_generic params, reply, action, content, answer;
	const char *text;
	fy_generic result;
	char suggestions[2048];
	size_t used;
	int written;
	int rc;

	gb = fyai_ctx_transient_gb(acp->ctx);
	if (!acp->form_capable || !acp->session_id ||
	    jsonrpc_conn_closed(acp->conn))
		goto no_answer;
	question = fy_get(args, "question", fy_invalid);
	text = fy_castp(&question, "");
	if (!*text)
		goto no_answer;
	from = fy_get(args, "from", fy_invalid);
	if (fy_is_string(from) && *fy_castp(&from, "")) {
		message = fy_stringf(gb, "the sub-agent '%s' asks: %s",
				fy_castp(&from, ""), text);
		text = fy_castp(&message, text);
	}
	/* ACP form mode cannot carry secrets. Refuse common secret requests. */
	if (strcasestr(text, "password") || strcasestr(text, "api key") ||
	    strcasestr(text, "access token") ||
	    strcasestr(text, "private key") ||
	    strcasestr(text, "recovery code") ||
	    strcasestr(text, "payment card"))
		goto no_answer;
	property = fy_mapping(gb, "type", "string", "title", "Answer",
			      "maxLength", 4096LL);
	choices = fy_get(args, "options", fy_invalid);
	if (fy_is_sequence(choices) && fy_len(choices)) {
		used = (size_t)snprintf(suggestions, sizeof(suggestions),
					"Suggested answers: ");
		fy_foreach(choice, choices) {
			if (!fy_is_string(choice) || used >= sizeof(suggestions))
				continue;
			written = snprintf(suggestions + used,
					   sizeof(suggestions) - used,
					   "%s%s", used > 19 ? ", " : "",
					   fy_castp(&choice, ""));
			if (written < 0)
				break;
			used += (size_t)written;
		}
		property = fy_assoc(gb, property, "description", suggestions);
	}
	schema = fy_mapping(gb, "type", "object",
		"properties", fy_mapping(gb, "answer", property),
		"required", fy_sequence(gb, "answer"));
	params = fy_mapping(gb, "sessionId", acp->session_id,
		"mode", "form", "message", text, "requestedSchema", schema);
	if (tool_call_id && *tool_call_id)
		params = fy_assoc(gb, params, "toolCallId", tool_call_id);
	req = jsonrpc_request_submit(acp->conn, "elicitation/create",
		params, jsonrpc_conn_next_id(acp->conn), false, NULL, NULL);
	if (!req)
		goto no_answer;
	while (!jsonrpc_request_done(req) && !acp->cancelled &&
	       !jsonrpc_conn_closed(acp->conn)) {
		rc = fyai_event_loop_step(fyai_ctx_loop(acp->ctx), -1);
		if (rc < 0)
			break;
	}
	result = fy_invalid;
	if (jsonrpc_request_done(req) && jsonrpc_request_ok(req)) {
		reply = jsonrpc_request_result(req);
		action = fy_get(reply, "action", fy_invalid);
		if (fy_equal(action, "accept")) {
			content = fy_get(reply, "content", fy_invalid);
			answer = fy_get(content, "answer", fy_invalid);
			if (fy_is_string(answer) &&
			    strlen(fy_castp(&answer, "")) <= 4096)
				result = fy_value(gb, fy_castp(&answer, ""));
		}
	}
	if (!jsonrpc_request_done(req))
		jsonrpc_request_cancel(req);
	jsonrpc_request_destroy(req);
	if (fy_is_valid(result))
		return result;
no_answer:
	acp->ctx->ask_abort = true;
	return fy_value(gb, "tool note: the user did not provide an answer");
}

static fy_generic acp_error(struct fyai_ctx *ctx, long long code,
			    const char *message)
{
	return fy_mapping(fyai_ctx_transient_gb(ctx),
			  "code", code, "message", message);
}

static bool acp_same_cwd(struct acp_session *acp, fy_generic params)
{
	fy_generic value;
	char *resolved;
	const char *path;
	bool same;

	value = fy_get(params, "cwd", fy_invalid);
	path = fy_castp(&value, "");
	if (*path != '/')
		return false;
	resolved = realpath(path, NULL);
	if (!resolved)
		return false;
	same = !strcmp(acp->cwd, resolved);
	free(resolved);
	return same;
}

static bool acp_empty_mcp(fy_generic params)
{
	fy_generic servers;

	servers = fy_get(params, "mcpServers", fy_invalid);
	return fy_is_sequence(servers) && !fy_len(servers);
}

static bool acp_match_session(struct acp_session *acp, fy_generic params)
{
	fy_generic value;
	const char *session;

	value = fy_get(params, "sessionId", fy_invalid);
	session = fy_castp(&value, "");
	return acp->session_id && !strcmp(acp->session_id, session);
}

static int acp_new_id(char id[38])
{
	static const char hex[] = "0123456789abcdef";
	unsigned char random[16];
	size_t done, i;
	ssize_t got;

	done = 0;
	while (done < sizeof(random)) {
		got = getrandom(random + done, sizeof(random) - done, 0);
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			return -1;
		done += (size_t)got;
	}
	memcpy(id, "sess_", 5);
	for (i = 0; i < sizeof(random); i++) {
		id[5 + 2 * i] = hex[random[i] >> 4];
		id[6 + 2 * i] = hex[random[i] & 15];
	}
	id[37] = '\0';
	return 0;
}

static fy_generic acp_stored_id(const struct fyai_branch *branch)
{
	return fy_get(fy_get(branch->store, "_fyai/acp", fy_invalid),
		      "id", fy_invalid);
}

static char *acp_find_branch(struct fyai_ctx *ctx, const char *session_id,
			    const char *cwd)
{
	struct fyai_branch branch;
	fy_generic name, entry, stored;
	const char *branch_name, *id;

	fy_foreach_key_value(name, entry, ctx->arena_branches) {
		branch_name = fy_castp(&name, "");
		if (!fyai_branch_decode(entry, &branch) ||
		    fy_is_valid(branch.agent))
			continue;
		if (!fyai_branch_cwd(&branch) ||
		    strcmp(fyai_branch_cwd(&branch), cwd))
			continue;
		stored = acp_stored_id(&branch);
		id = fy_is_string(stored) ? fy_castp(&stored, "") :
			branch_name;
		if (!strcmp(id, session_id))
			return strdup(branch_name);
	}
	return NULL;
}

static int acp_update(struct acp_session *acp, const char *kind,
		      const char *text, const char *message_id)
{
	struct fy_generic_builder *gb;
	fy_generic update;

	gb = fyai_ctx_transient_gb(acp->ctx);
	update = fy_mapping(gb,
			    "sessionUpdate", kind,
			    "content", fy_mapping(gb,
						"type", "text", "text", text));
	if (message_id)
		update = fy_assoc(gb, update, "messageId", message_id);
	return jsonrpc_notify(acp->conn, "session/update",
		fy_mapping(gb, "sessionId", acp->session_id,
				    "update", update));
}

static void acp_output(void *userdata, int tag, const char *text, size_t len)
{
	struct acp_session *acp = userdata;
	char *copy;

	if (!acp->run || tag != FYAI_OUTPUT_ASSISTANT || !len)
		return;
	copy = strndup(text, len);
	if (!copy)
		return;
	(void)acp_update(acp, "agent_message_chunk", copy, NULL);
	free(copy);
}

static int acp_replay_turn(void *userdata, fy_generic turn)
{
	struct acp_session *acp = userdata;
	struct acp_replay_call *saved;
	fy_generic message, role, content, calls, call, id_value, type;
	const char *r, *text, *kind, *id;
	char msg_id[64];
	static unsigned long long sequence;

	fy_foreach(message, fy_get(turn, "messages", fy_seq_empty)) {
		role = fy_get(message, "role", fy_invalid);
		content = fy_get(message, "content", fy_invalid);
		r = fy_castp(&role, "");
		text = fy_castp(&content, "");
		kind = !strcmp(r, "user") ? "user_message_chunk" :
		       !strcmp(r, "assistant") ? "agent_message_chunk" : NULL;
		if (kind && fy_is_string(content)) {
			snprintf(msg_id, sizeof(msg_id), "fyai-msg-%llu", ++sequence);
			if (acp_update(acp, kind, text, msg_id))
				return -1;
		}
		calls = fy_get(message, "tool_calls", fy_invalid);
		if (!fy_is_sequence(calls)) {
			type = fy_get(message, "type", fy_invalid);
			if (fy_equal(type, "function_call") ||
			    fy_equal(type, "shell_call"))
				calls = fy_sequence(fyai_ctx_transient_gb(acp->ctx),
						    message);
		}
		fy_foreach(call, calls) {
			id_value = acp_call_id(call);
			id = fy_castp(&id_value, "");
			if (!*id)
				continue;
			saved = calloc(1, sizeof(*saved));
			if (!saved)
				return -1;
			saved->id = strdup(id);
			if (!saved->id) {
				free(saved);
				return -1;
			}
			saved->call = call;
			saved->next = acp->replay_calls;
			acp->replay_calls = saved;
			acp_tool(acp, call, fy_invalid, "pending");
			acp_tool(acp, call, fy_invalid, "in_progress");
		}
		id_value = fy_get(message, "tool_call_id", fy_invalid);
		if (!fy_is_string(id_value))
			id_value = fy_get(message, "call_id", fy_invalid);
		id = fy_castp(&id_value, "");
		if (!*id)
			continue;
		for (saved = acp->replay_calls; saved; saved = saved->next)
			if (!strcmp(saved->id, id))
				break;
		if (!saved)
			continue;
		if (fy_is_invalid(content))
			content = fy_get(message, "output", fy_invalid);
		acp_tool(acp, saved->call, content,
			 fy_is_string(content) &&
			 !strncmp(fy_castp(&content, ""), "tool error:", 11) ?
			 "failed" : "completed");
	}
	return 0;
}

static fy_generic acp_config_options(struct acp_session *acp)
{
	struct fy_generic_builder *gb;
	fy_generic rows, row, name, model_options, effort_options;
	fy_generic options, model, configured_model;
	const char *selected;

	gb = fyai_ctx_transient_gb(acp->ctx);
	rows = fyai_catalog_list_data(acp->ctx, gb, "models");
	model_options = fy_seq_empty;
	fy_foreach(row, rows) {
		name = fy_get(row, "name", fy_invalid);
		if (fy_is_string(name))
			model_options = fy_append(gb, model_options,
				fy_mapping(gb, "value", name,
						 "name", name));
	}
	if (fy_len(fy_get(fy_get(acp->ctx->cfg->config_doc, "routing"),
			      "candidates", fy_seq_empty)))
		model_options = fy_append(gb, model_options,
			fy_mapping(gb, "value", "auto", "name", "Automatic"));
	configured_model = fy_get(acp->ctx->cfg->config_doc, "model", fy_invalid);
	selected = fy_equal(fy_get(fy_get(acp->ctx->cfg->config_doc,
						"routing"), "mode"), "auto") ?
		"auto" : fy_is_string(configured_model) ?
		fy_castp(&configured_model, "") :
		(acp->ctx->cfg->model ? acp->ctx->cfg->model : "");
	options = fy_sequence(gb, fy_mapping(gb,
		"id", "model", "name", "Model", "type", "select",
		"category", "model", "currentValue", selected,
		"options", model_options));
	model = fyai_catalog_resolved_model(
		fyai_catalog_effective(acp->ctx->arena_catalog,
					acp->ctx->cfg->gb), selected);
	if (fyai_catalog_model_has_cap(model, "reasoning_effort")) {
		effort_options = fy_sequence(gb,
			fy_mapping(gb, "value", "default", "name", "Default"),
			fy_mapping(gb, "value", "minimal", "name", "Minimal"),
			fy_mapping(gb, "value", "low", "name", "Low"),
			fy_mapping(gb, "value", "medium", "name", "Medium"),
			fy_mapping(gb, "value", "high", "name", "High"));
		options = fy_append(gb, options, fy_mapping(gb,
			"id", "effort", "name", "Reasoning effort",
			"type", "select", "category", "thought_level",
			"currentValue", acp->ctx->cfg->reasoning_effort ?
				acp->ctx->cfg->reasoning_effort : "default",
			"options", effort_options));
	}
	return options;
}

static bool acp_config_option_allowed(struct acp_session *acp,
				      const char *config_id, const char *setting)
{
	fy_generic options, option, choices, choice, value;
	const char *id, *candidate;
	char *wanted_id, *wanted_setting;
	bool allowed = false;

	/* The incoming strings live in the transient builder used below. */
	wanted_id = strdup(config_id);
	wanted_setting = strdup(setting);
	if (!wanted_id || !wanted_setting)
		goto out;
	options = acp_config_options(acp);
	fy_foreach(option, options) {
		value = fy_get(option, "id", fy_invalid);
		id = fy_castp(&value, "");
		if (strcmp(id, wanted_id))
			continue;
		choices = fy_get(option, "options", fy_seq_empty);
		fy_foreach(choice, choices) {
			value = fy_get(choice, "value", fy_invalid);
			candidate = fy_castp(&value, "");
			if (!strcmp(candidate, wanted_setting)) {
				allowed = true;
				goto out;
			}
		}
		break;
	}
out:
	free(wanted_id);
	free(wanted_setting);
	return allowed;
}

static fy_generic acp_session_result(struct acp_session *acp, bool with_id)
{
	struct fy_generic_builder *gb;
	fy_generic result;

	gb = fyai_ctx_transient_gb(acp->ctx);
	result = fy_mapping(gb, "configOptions", acp_config_options(acp));
	if (with_id)
		result = fy_assoc(gb, result, "sessionId", acp->session_id);
	return result;
}

static fy_generic acp_serve(struct jsonrpc_conn *conn, const char *method,
			    fy_generic params, fy_generic id,
			    void *userdata, fy_generic *errorp)
{
	struct acp_session *acp = userdata;
	struct fyai_ctx *ctx = acp->ctx;
	struct fy_generic_builder *gb;
	struct fyai_turn_selector_args all = { .type = FYAITST_ALL };
	fy_generic version, value, config_key, blocks, block, sessions, entry, name, stored;
	struct fyai_branch branch;
	const char *session, *config_id, *setting, *text, *branch_name;
	const char *branch_cwd, *title;
	char new_id[38];
	char *prompt, *resolved_branch, *requested;
	size_t total, len;
	int rc = -1;

	gb = fyai_ctx_transient_gb(ctx);
	if (!strcmp(method, "initialize")) {
		if (acp->initialized) {
			*errorp = acp_error(ctx, -32600, "already initialized");
			return fy_invalid;
		}
		version = fy_get(params, "protocolVersion", fy_invalid);
		if (!fy_is_valid(version) || !fy_equal(version, 1LL)) {
			*errorp = acp_error(ctx, -32602, "ACP v1 is required");
			return fy_invalid;
		}
		acp->initialized = true;
		value = fy_get(fy_get(fy_get(params, "clientCapabilities",
			fy_invalid), "elicitation", fy_invalid),
			"form", fy_invalid);
		acp->form_capable = fy_is_mapping(value);
		value = fy_get(fy_get(fy_get(params, "clientCapabilities",
				fy_invalid), "session", fy_invalid),
				"notices", fy_invalid);
		acp->notice_capable = fy_is_mapping(value);
		return fy_mapping(gb,
			"protocolVersion", 1LL,
			"agentCapabilities", fy_mapping(gb,
				"loadSession", fy_true,
				"sessionCapabilities", fy_mapping(gb,
					"list", fy_mapping(gb),
					"resume", fy_mapping(gb),
					"close", fy_mapping(gb))),
			"agentInfo", fy_mapping(gb,
				"name", "fyai", "title", "FYAI",
				"version", VERSION "+g" FYAI_SOURCE_REVISION),
			"authMethods", fy_sequence(gb),
			"_meta", fy_mapping(gb,
				"_fyai/pid", (long long)getpid(),
				"_fyai/pgid", (long long)getpgrp()));
	}
	if (!acp->initialized) {
		*errorp = acp_error(ctx, -32000, "initialize first");
		return fy_invalid;
	}
	if (!strcmp(method, "session/list")) {
		if (fy_is_valid(fy_get(params, "cursor", fy_invalid))) {
			*errorp = acp_error(ctx, -32602, "unknown cursor");
			return fy_invalid;
		}
		value = fy_get(params, "cwd", fy_invalid);
		if (fy_is_valid(value) && !acp_same_cwd(acp, params)) {
			*errorp = acp_error(ctx, -32602, "cwd does not match process");
			return fy_invalid;
		}
		sessions = fy_seq_empty;
		fy_foreach_key_value(name, entry, ctx->arena_branches) {
			branch_name = fy_castp(&name, "");
			if (!fyai_branch_decode(entry, &branch) ||
			    fy_is_valid(branch.agent))
				continue;
			branch_cwd = fyai_branch_cwd(&branch);
			if (!branch_cwd || (fy_is_valid(value) &&
					   strcmp(branch_cwd, acp->cwd)))
				continue;
			stored = acp_stored_id(&branch);
			title = fy_castp(&branch.description, branch_name);
			sessions = fy_append(gb, sessions,
				fy_mapping(gb, "sessionId",
					fy_is_string(stored) ? stored : name,
					"cwd", branch_cwd,
					"title", *title ? title : branch_name));
		}
		return fy_mapping(gb, "sessions", sessions);
	}
	if (!strcmp(method, "session/new") ||
	    !strcmp(method, "session/load") ||
	    !strcmp(method, "session/resume")) {
		if (acp->session_id || acp->run || acp->pending) {
			*errorp = acp_error(ctx, -32003, "process already owns a session");
			return fy_invalid;
		}
		if (!acp_same_cwd(acp, params) || !acp_empty_mcp(params)) {
			*errorp = acp_error(ctx, -32602,
				"cwd must match and mcpServers must be empty");
			return fy_invalid;
		}
		if (!strcmp(method, "session/new")) {
			if (!ctx->session_unstored || acp_new_id(new_id)) {
				*errorp = acp_error(ctx, -32000,
					"could not create a fresh session");
				return fy_invalid;
			}
			ctx->branch_store = fy_mapping(ctx->gb,
				"_fyai/acp", fy_mapping(ctx->gb,
						"version", 1LL, "id", new_id));
			ctx->session_persist_empty = true;
			rc = fyai_publish_state(ctx);
			ctx->session_persist_empty = false;
			if (rc) {
				*errorp = acp_error(ctx, -32000,
					"could not persist new session");
				return fy_invalid;
			}
			acp->session_id = strdup(new_id);
		} else {
			value = fy_get(params, "sessionId", fy_invalid);
			session = fy_castp(&value, "");
			requested = strdup(session);
			resolved_branch = acp_find_branch(ctx, session, acp->cwd);
			if (!requested || !resolved_branch) {
				free(requested);
				free(resolved_branch);
				*errorp = acp_error(ctx, -32602, "unknown sessionId");
				return fy_invalid;
			}
			rc = fyai_session_branch_switch(ctx, resolved_branch,
							false, true);
			free(resolved_branch);
			if (rc) {
				free(requested);
				*errorp = acp_error(ctx, -32000,
						"could not load the session");
				return fy_invalid;
			}
			acp->session_id = requested;
		}
		if (!acp->session_id) {
			*errorp = acp_error(ctx, -32000, "out of memory");
			return fy_invalid;
		}
		if (!strcmp(method, "session/load")) {
			acp->replaying = true;
			rc = fyai_display_foreach_turn(ctx, &all,
						   acp_replay_turn, acp);
			acp->replaying = false;
			if (rc) {
				*errorp = acp_error(ctx, -32000,
						"could not replay history");
				return fy_invalid;
			}
		}
		return acp_session_result(acp, !strcmp(method, "session/new"));
	}
	if (!acp_match_session(acp, params)) {
		*errorp = acp_error(ctx, -32602, "wrong sessionId");
		return fy_invalid;
	}
	if (!strcmp(method, "session/cancel")) {
		acp->cancelled = acp->run || acp->pending;
		if (acp->run)
			fyai_turn_run_cancel(acp->run);
		return fy_invalid;
	}
	if (!strcmp(method, "session/close")) {
		if (acp->run || acp->pending) {
			acp->cancelled = true;
			if (acp->run)
				fyai_turn_run_cancel(acp->run);
			acp->close_id = id;
			acp->close_pending = true;
			jsonrpc_conn_defer(conn);
			return fy_invalid;
		}
		acp->quit = true;
		return fy_mapping(gb);
	}
	if (!strcmp(method, "session/set_config_option")) {
		if (acp->run || acp->pending) {
			*errorp = acp_error(ctx, -32003, "session is busy");
			return fy_invalid;
		}
		config_key = fy_get(params, "configId", fy_invalid);
		config_id = fy_equal(config_key, "model") ? "model" :
			fy_equal(config_key, "effort") ? "effort" : "";
		value = fy_get(params, "value", fy_invalid);
		setting = fy_castp(&value, "");
		if (!fy_is_string(value)) {
			*errorp = acp_error(ctx, -32602, "value must be a string");
			return fy_invalid;
		}
		if (!acp_config_option_allowed(acp, config_id, setting)) {
			*errorp = acp_error(ctx, -32602,
					"config option value is not available");
			return fy_invalid;
		}
		if (!strcmp(config_id, "model")) {
			if (strcmp(setting, "auto"))
				rc = fyai_config_set_generic(ctx, "model", value);
			else
				rc = 0;
			if (!rc)
				rc = fyai_config_set_generic(ctx, "routing/mode",
					fy_value(!strcmp(setting, "auto") ?
						 "auto" : "manual"));
		}
		else if (!strcmp(config_id, "effort"))
			rc = !strcmp(setting, "default") ?
				fyai_config_delete(ctx, "reasoning/effort") :
				fyai_config_set_generic(ctx,
						"reasoning/effort", value);
		else {
			*errorp = acp_error(ctx, -32602, "unknown configId");
			return fy_invalid;
		}
		if (rc || fyai_config_rederive(ctx) || fyai_auth_resolve(ctx) ||
		    fyai_request_state_apply(ctx)) {
			*errorp = acp_error(ctx, -32000,
					"could not apply config option");
			return fy_invalid;
		}
		return acp_session_result(acp, false);
	}
	if (!strcmp(method, "session/prompt")) {
		if (!fy_is_valid(id) || acp->run || acp->pending) {
			*errorp = acp_error(ctx, -32003, "session is busy");
			return fy_invalid;
		}
		blocks = fy_get(params, "prompt", fy_invalid);
		if (!fy_is_sequence(blocks)) {
			*errorp = acp_error(ctx, -32602, "prompt must be an array");
			return fy_invalid;
		}
		total = 0;
		fy_foreach(block, blocks) {
			value = fy_get(block, "type", fy_invalid);
			if (!fy_equal(value, "text")) {
				*errorp = acp_error(ctx, -32602,
					"only text prompts are supported");
				return fy_invalid;
			}
			value = fy_get(block, "text", fy_invalid);
			if (!fy_is_string(value)) {
				*errorp = acp_error(ctx, -32602,
					"text prompt is malformed");
				return fy_invalid;
			}
			text = fy_castp(&value, "");
			total += strlen(text) + 1;
		}
		if (!total || total > 1000000) {
			*errorp = acp_error(ctx, -32602, "prompt is empty or too long");
			return fy_invalid;
		}
		prompt = malloc(total + 1);
		if (!prompt) {
			*errorp = acp_error(ctx, -32000, "out of memory");
			return fy_invalid;
		}
		len = 0;
		fy_foreach(block, blocks) {
			value = fy_get(block, "text", fy_invalid);
			text = fy_castp(&value, "");
			if (len)
				prompt[len++] = '\n';
			memcpy(prompt + len, text, strlen(text));
			len += strlen(text);
		}
		prompt[len] = '\0';
		acp->prompt = prompt;
		acp->prompt_id = id;
		acp->pending = true;
		jsonrpc_conn_defer(conn);
		return fy_invalid;
	}
	*errorp = acp_error(ctx, -32601, "method not found");
	return fy_invalid;
}

static void acp_finish_prompt(struct acp_session *acp)
{
	struct fyai_ctx *ctx = acp->ctx;
	struct fy_generic_builder *gb;
	fy_generic result;
	bool published, attempted;

	gb = fyai_ctx_transient_gb(ctx);
	result = fyai_turn_run_collect(acp->run);
	fyai_turn_run_destroy(acp->run);
	acp->run = NULL;
	result = fyai_report_diag(ctx, result);
	published = false;
	attempted = fy_is_valid(result);
	if (fy_is_valid(result)) {
		ctx->last_message = result;
		published = !fyai_publish_state(ctx);
	}
	if (acp->notice_capable && ctx->route_explanation)
		(void)jsonrpc_notify(acp->conn, "session/update",
			fy_mapping(gb, "sessionId", acp->session_id,
			"update", fy_mapping(gb,
				"sessionUpdate", "notice",
				"severity", "info",
				"title", "Task cost",
				"description", ctx->route_usage_unknown ?
					fy_value("Cost unavailable: provider did not report token usage") :
					fy_stringf(gb,
					"$%.6f (%s); provider usage may exceed the preflight estimate",
					ctx->usage_cost - ctx->route_turn_start_cost,
					ctx->route_turn_cost_source ?
					ctx->route_turn_cost_source : "unknown"))));
	if (attempted && !published) {
		(void)jsonrpc_conn_respond(acp->conn, acp->prompt_id,
			fy_invalid,
			acp_error(ctx, -32000, "turn was not published"));
	} else if (acp->cancelled) {
		(void)jsonrpc_conn_respond(acp->conn, acp->prompt_id,
			fy_mapping(gb, "stopReason", "cancelled"), fy_invalid);
	} else if (published) {
		(void)jsonrpc_conn_respond(acp->conn, acp->prompt_id,
			fy_mapping(gb, "stopReason", "end_turn"), fy_invalid);
	} else {
		(void)jsonrpc_conn_respond(acp->conn, acp->prompt_id,
			fy_invalid,
			acp_error(ctx, -32000, "turn failed or was not published"));
	}
	free(acp->prompt);
	acp->prompt = NULL;
	acp->cancelled = false;
	if (acp->close_pending) {
		(void)jsonrpc_conn_respond(acp->conn, acp->close_id,
			fy_mapping(gb), fy_invalid);
		acp->close_pending = false;
		acp->quit = true;
	}
	fyai_cleanup_transient_builder(ctx);
}

static int acp_start_prompt(struct acp_session *acp)
{
	struct fyai_ctx *ctx = acp->ctx;
	struct fy_generic_builder *gb;
	fy_generic turn;

	acp->pending = false;
	gb = fyai_ctx_transient_gb(ctx);
	turn = fyai_turn_append(ctx, ctx->last_message,
		fy_sequence(gb, fyai_make_user_message(ctx, acp->prompt)));
	if (fy_is_valid(turn))
		turn = fyai_output_record(ctx, turn, FYAI_OUTPUT_USER,
					  acp->prompt);
	if (!fy_is_valid(turn))
		return -1;
	acp->run = fyai_turn_run_submit(ctx, turn);
	if (acp->run && acp->notice_capable && ctx->route_explanation)
		(void)jsonrpc_notify(acp->conn, "session/update",
			fy_mapping(gb, "sessionId", acp->session_id,
			"update", fy_mapping(gb,
				"sessionUpdate", "notice",
				"severity", "info",
				"title", "Automatic route",
				"description", ctx->route_explanation)));
	return acp->run ? 0 : -1;
}

static void acp_cancel_pending(struct acp_session *acp)
{
	struct fy_generic_builder *gb;

	gb = fyai_ctx_transient_gb(acp->ctx);
	(void)jsonrpc_conn_respond(acp->conn, acp->prompt_id,
		fy_mapping(gb, "stopReason", "cancelled"), fy_invalid);
	free(acp->prompt);
	acp->prompt = NULL;
	acp->pending = false;
	acp->cancelled = false;
	if (acp->close_pending) {
		(void)jsonrpc_conn_respond(acp->conn, acp->close_id,
			fy_mapping(gb), fy_invalid);
		acp->close_pending = false;
		acp->quit = true;
	}
	fyai_cleanup_transient_builder(acp->ctx);
}

int fyai_cmd_acp_prepare(struct fyai_cfg *cfg, fy_generic args)
{
	(void)args;
	cfg->acp_mode = true;
	cfg->interactive = false;
	cfg->fresh_session = !cfg->branch_explicit;
	cfg->mcp_enabled = false;
	return 0;
}

int fyai_cmd_acp(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = call->ctx;
	struct acp_session acp = { .ctx = ctx };
	struct fyai_event_loop *el;
	struct fyai_sink *sink;
	int rc = -1;

	(void)result;
	/* The desktop's fallback kill targets this group, not its WSL parent. */
	if (getpgrp() != getpid() && setpgid(0, 0)) {
		fyai_error(ctx, "could not isolate ACP process group: %s",
			   strerror(errno));
		return -1;
	}
	acp.cwd = getcwd(NULL, 0);
	if (!acp.cwd)
		return -1;
	sink = fyai_sink_create_discard(ctx);
	if (!sink)
		goto out;
	fyai_sink_destroy(ctx->sink);
	ctx->sink = sink;
	el = fyai_ctx_loop(ctx);
	if (!el)
		goto out;
	acp.conn = jsonrpc_conn_stdio(ctx, STDOUT_FILENO, STDIN_FILENO,
				      0, "acp", NULL);
	if (!acp.conn)
		goto out;
	jsonrpc_conn_set_output_limit(acp.conn, 8UL << 20);
	if (jsonrpc_conn_serve(acp.conn, acp_serve, &acp))
		goto out;
	ctx->output_observer = acp_output;
	ctx->output_observer_userdata = &acp;
	ctx->tool_observer = acp_tool;
	ctx->tool_observer_userdata = &acp;
	ctx->tool_permission = acp_permission;
	ctx->tool_permission_userdata = &acp;
	ctx->question_handler = acp_question;
	ctx->question_handler_userdata = &acp;
	while (!acp.quit && !jsonrpc_conn_closed(acp.conn)) {
		if (acp.pending && !acp.run) {
			if (acp.cancelled) {
				acp_cancel_pending(&acp);
				continue;
			}
			if (acp_start_prompt(&acp)) {
				(void)jsonrpc_conn_respond(acp.conn, acp.prompt_id,
					fy_invalid,
					acp_error(ctx, -32000,
						"could not start turn"));
				free(acp.prompt);
				acp.prompt = NULL;
				fyai_cleanup_transient_builder(ctx);
			}
			continue;
		}
		if (acp.run && fyai_turn_run_done(acp.run)) {
			acp_finish_prompt(&acp);
			continue;
		}
		if (fyai_event_loop_step(el, -1) < 0)
			break;
	}
	if (acp.run) {
		fyai_turn_run_cancel(acp.run);
		acp.cancelled = true;
		while (!fyai_turn_run_done(acp.run))
			if (fyai_event_loop_step(el, 100) < 0)
				break;
		if (fyai_turn_run_done(acp.run))
			acp_finish_prompt(&acp);
		else
			fyai_turn_run_destroy(acp.run);
	}
	while (jsonrpc_conn_has_output(acp.conn))
		if (fyai_event_loop_step(el, 1000) <= 0)
			break;
	rc = 0;
	ctx->output_observer = NULL;
	ctx->output_observer_userdata = NULL;
	ctx->tool_observer = NULL;
	ctx->tool_observer_userdata = NULL;
	ctx->tool_permission = NULL;
	ctx->tool_permission_userdata = NULL;
	ctx->question_handler = NULL;
	ctx->question_handler_userdata = NULL;
out:
	while (acp.replay_calls) {
		struct acp_replay_call *saved = acp.replay_calls;

		acp.replay_calls = saved->next;
		free(saved->id);
		free(saved);
	}
	while (acp.edits) {
		struct acp_edit_snapshot *edit = acp.edits;

		acp.edits = edit->next;
		free(edit->id);
		free(edit->path);
		free(edit->old_text);
		free(edit);
	}
	free(acp.prompt);
	free(acp.session_id);
	free(acp.cwd);
	if (acp.conn)
		jsonrpc_conn_destroy(acp.conn);
	return rc;
}
