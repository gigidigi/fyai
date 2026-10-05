/*
 * fyai_route.c - opt-in catalogue routing and request budget checks
 *
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_UNKNOWN

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "fyai_route.h"
#include "fyai_auth.h"
#include "fyai_catalog.h"
#include "fyai_config.h"
#include "fyai_session.h"
#include "fyai_sink.h"
#include "fyai_turn.h"

static fy_generic route_policy(struct fyai_ctx *ctx)
{
	return fy_get(ctx->cfg->config_doc, "routing", fy_invalid);
}

static bool route_auto(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg;
	size_t i;

	cfg = ctx->cfg;
	if (!fy_equal(fy_get(route_policy(ctx), "mode"), "auto"))
		return false;
	for (i = 0; i < cfg->config_op_count; i++) {
		if (cfg->config_ops[i].op == 's' &&
		    cfg->config_ops[i].key &&
		    (!strcmp(cfg->config_ops[i].key, "model") ||
		     !strcmp(cfg->config_ops[i].key, "reasoning/effort")))
			return false;
	}
	return true;
}

/* Price is per million tokens. A missing or non-USD price is not a route. */
static bool route_prices(struct fyai_ctx *ctx, const char *provider,
			 const char *model, enum fyai_api_mode api,
			 const char *url, double *input, double *output)
{
	fy_generic cat, prov, offer, price, endpoint;
	const char *root_url, *path;
	size_t root_len, path_len;

	cat = fyai_catalog_effective(ctx->arena_catalog, ctx->cfg->gb);
	prov = fyai_catalog_provider(cat, provider);
	endpoint = fyai_catalog_endpoint(prov, api);
	root_url = fy_get(prov, "root_url", "");
	path = fy_get(endpoint, "endpoint", "");
	root_len = strlen(root_url);
	path_len = strlen(path);
	if (!url || !root_len || !path_len ||
	    strlen(url) != root_len + path_len ||
	    strncmp(url, root_url, root_len) ||
	    strcmp(url + root_len, path))
		return false;
	fyai_catalog_offering(prov, model, &offer);
	price = fy_get(offer, "pricing", fy_invalid);
	if (!fy_equal(fy_get(price, "currency"), "USD") ||
	    !fy_equal(fy_get(price, "unit"), "per_million_tokens"))
		return false;
	*input = fy_number(fy_get(price, "input", fy_invalid), -1.0);
	*output = fy_number(fy_get(price, "output", fy_invalid), -1.0);
	return isfinite(*input) && isfinite(*output) &&
		*input >= 0.0 && *output >= 0.0;
}

static int route_target(fy_generic turn)
{
	fy_generic messages, message, content;
	const char *text;
	size_t length;

	messages = fy_get(turn, "messages", fy_seq_empty);
	text = "";
	fy_foreach(message, messages) {
		if (!fy_equal(fy_get(message, "role"), "user"))
			continue;
		content = fy_get(message, "content", fy_invalid);
		if (fy_is_string(content))
			text = fy_castp(&content, "");
	}
	length = strlen(text);
	if (length > 500 || strcasestr(text, "debug") ||
	    strcasestr(text, "refactor") || strcasestr(text, "architecture") ||
	    strcasestr(text, "complex") || strcasestr(text, "failing"))
		return 3;
	return length > 100 ? 2 : 1;
}

static bool route_candidate(struct fyai_ctx *ctx, fy_generic candidate,
			    struct fyai_cfg *out, double *estimate,
			    long long input_tokens, long long output_limit)
{
	struct fyai_cfg *cfg;
	fy_generic model_value, effort_value, catalog, model;
	fy_generic provider;
	const char *name, *effort, *slash;
	char prefix[128];
	double input_price, output_price;

	cfg = ctx->cfg;
	model_value = fy_get(candidate, "model", fy_invalid);
	effort_value = fy_get(candidate, "effort", fy_invalid);
	if (!fy_is_string(model_value))
		return false;
	name = fy_castp(&model_value, "");
	if (!*name)
		return false;
	effort = fy_is_string(effort_value) ?
		fy_castp(&effort_value, "") : "";
	if (*effort && strcmp(effort, "minimal") && strcmp(effort, "low") &&
	    strcmp(effort, "medium") && strcmp(effort, "high"))
		return false;
	catalog = fyai_catalog_effective(ctx->arena_catalog, cfg->gb);
	slash = strchr(name, '/');
	model = fy_invalid;
	if (slash && (size_t)(slash - name) < sizeof(prefix)) {
		memcpy(prefix, name, (size_t)(slash - name));
		prefix[slash - name] = '\0';
		provider = fyai_catalog_provider(catalog, prefix);
		if (fy_is_valid(provider))
			model = fyai_catalog_resolved_model(catalog, slash + 1);
	}
	if (fy_is_invalid(model))
		model = fyai_catalog_resolved_model(catalog, name);
	if (fy_is_invalid(model) ||
	    !fyai_catalog_model_has_cap(model, "tool_calling") ||
	    (*effort && !fyai_catalog_model_has_cap(model,
						    "reasoning_effort")))
		return false;
	*out = *cfg;
	out->model = fy_gb_intern_string(cfg->gb, name);
	out->provider = NULL;
	out->api_url = fy_is_valid(fy_get(cfg->config_doc, "api_url")) ?
		cfg->api_url : NULL;
	out->api_key = cfg->api_key_explicit ? cfg->api_key : NULL;
	out->reasoning_effort = *effort ?
		fy_gb_intern_string(cfg->gb, effort) : NULL;
	out->reasoning_summary = NULL;
	out->chatgpt_auth = false;
	if (out->max_tokens > output_limit)
		out->max_tokens = (int)output_limit;
	if (fyai_config_resolve_model(out))
		return false;
	if (*effort && out->api_mode == FYAI_API_MESSAGES)
		return false;
	/* An explicit key or URL is bound to the original provider. */
	if ((cfg->api_key_explicit ||
	     fy_is_valid(fy_get(cfg->config_doc, "api_url"))) &&
	    cfg->provider && strcmp(out->provider, cfg->provider))
		return false;
	if (!out->no_auth && (!out->api_key || !*out->api_key) &&
	    !(cfg->chatgpt_auth && !strcmp(out->provider, "openai") &&
	      out->api_mode == FYAI_API_RESPONSES))
		return false;
	if (!route_prices(ctx, out->provider, out->model,
			  out->api_mode, out->api_url,
			  &input_price, &output_price))
		return false;
	*estimate = ((double)input_tokens * input_price +
		     (double)out->max_tokens * output_price) / 1000000.0;
	return isfinite(*estimate);
}

static void route_save(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg;

	cfg = ctx->cfg;
	ctx->route_saved.active = true;
	ctx->route_saved.model = cfg->model;
	ctx->route_saved.provider = cfg->provider;
	ctx->route_saved.api_url = cfg->api_url;
	ctx->route_saved.api_key = cfg->api_key;
	ctx->route_saved.reasoning_effort = cfg->reasoning_effort;
	ctx->route_saved.reasoning_summary = cfg->reasoning_summary;
	ctx->route_saved.api_mode = cfg->api_mode;
	ctx->route_saved.max_tokens = cfg->max_tokens;
	ctx->route_saved.shell_tool_supported = cfg->shell_tool_supported;
	ctx->route_saved.web_search_supported = cfg->web_search_supported;
	ctx->route_saved.response_compaction_supported =
		cfg->response_compaction_supported;
	ctx->route_saved.chatgpt_auth = cfg->chatgpt_auth;
	ctx->route_saved.enable_builtin_shell = cfg->enable_builtin_shell;
	ctx->route_saved.enable_tools = cfg->enable_tools;
	ctx->route_saved.web_search = cfg->web_search;
}

void fyai_route_restore(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg;

	if (!ctx || !ctx->route_saved.active)
		return;
	cfg = ctx->cfg;
	cfg->model = ctx->route_saved.model;
	cfg->provider = ctx->route_saved.provider;
	cfg->api_url = ctx->route_saved.api_url;
	cfg->api_key = ctx->route_saved.api_key;
	cfg->reasoning_effort = ctx->route_saved.reasoning_effort;
	cfg->reasoning_summary = ctx->route_saved.reasoning_summary;
	cfg->api_mode = ctx->route_saved.api_mode;
	cfg->max_tokens = ctx->route_saved.max_tokens;
	cfg->shell_tool_supported = ctx->route_saved.shell_tool_supported;
	cfg->web_search_supported = ctx->route_saved.web_search_supported;
	cfg->response_compaction_supported =
		ctx->route_saved.response_compaction_supported;
	cfg->chatgpt_auth = ctx->route_saved.chatgpt_auth;
	cfg->enable_builtin_shell = ctx->route_saved.enable_builtin_shell;
	cfg->enable_tools = ctx->route_saved.enable_tools;
	cfg->web_search = ctx->route_saved.web_search;
	ctx->route_saved.active = false;
	if (fyai_request_state_apply(ctx))
		fyai_warning(ctx, "could not restore the manual request settings after routing");
}

int fyai_route_select(struct fyai_ctx *ctx, fy_generic *turnp)
{
	struct fyai_cfg *cfg, candidate_cfg, selected_cfg;
	struct fyai_context_prompt prompt;
	struct fy_generic_builder *gb;
	fy_generic policy, candidates, candidate, meta, turn;
	const char *effort;
	long long output_limit;
	double budget, estimate, selected_estimate;
	int target, quality, selected_quality, rejected, rc;
	bool selected, sufficient, best_sufficient;

	ctx->route_explanation = NULL;
	ctx->route_usage_unknown = false;
	ctx->route_turn_cost_source = NULL;
	if (!route_auto(ctx))
		return 0;
	ctx->route_turn_start_cost = ctx->usage_cost;
	cfg = ctx->cfg;
	policy = route_policy(ctx);
	candidates = fy_get(policy, "candidates", fy_invalid);
	budget = fy_number(fy_get(policy, "max_turn_usd", fy_invalid), 0.0);
	output_limit = fy_get(policy, "output_tokens", 2048LL);
	if (!fy_is_sequence(candidates) || !fy_len(candidates) ||
	    !isfinite(budget) || budget <= 0.0 ||
	    output_limit <= 0 || output_limit > 65536) {
		fyai_error(ctx, "auto routing needs candidates, max_turn_usd, and a valid output_tokens cap");
		return -1;
	}
	fyai_context_prompt_at(ctx, *turnp, &prompt);
	target = route_target(*turnp);
	selected = false;
	selected_estimate = 0.0;
	selected_quality = 0;
	rejected = 0;
	fy_foreach(candidate, candidates) {
		quality = fy_get(candidate, "quality", 2LL);
		if (quality < 1 || quality > 3 ||
		    !route_candidate(ctx, candidate, &candidate_cfg, &estimate,
				     prompt.prompt, output_limit) ||
		    estimate > budget) {
			rejected++;
			continue;
		}
		sufficient = quality >= target;
		best_sufficient = selected && selected_quality >= target;
		if (!selected || (sufficient && !best_sufficient) ||
		    (sufficient == best_sufficient &&
		     (sufficient ? estimate < selected_estimate :
		      quality > selected_quality ||
		      (quality == selected_quality &&
		       estimate < selected_estimate)))) {
			selected = true;
			selected_cfg = candidate_cfg;
			selected_estimate = estimate;
			selected_quality = quality;
		}
	}
	if (!selected) {
		fyai_error(ctx, "auto routing found no authenticated, tool-capable, priced route within the $%.6f turn budget", budget);
		return -1;
	}
	route_save(ctx);
	cfg->model = selected_cfg.model;
	cfg->provider = selected_cfg.provider;
	cfg->api_mode = selected_cfg.api_mode;
	cfg->api_url = selected_cfg.api_url;
	cfg->api_key = selected_cfg.api_key;
	cfg->reasoning_effort = selected_cfg.reasoning_effort;
	cfg->reasoning_summary = selected_cfg.reasoning_summary;
	cfg->max_tokens = selected_cfg.max_tokens;
	cfg->shell_tool_supported = selected_cfg.shell_tool_supported;
	cfg->web_search_supported = selected_cfg.web_search_supported;
	cfg->response_compaction_supported =
		selected_cfg.response_compaction_supported;
	cfg->chatgpt_auth = false;
	rc = fyai_auth_resolve(ctx);
	if (!rc)
		rc = fyai_config_messages_gate(cfg);
	if (!rc)
		rc = fyai_request_state_apply(ctx);
	if (rc) {
		fyai_route_restore(ctx);
		return -1;
	}
	effort = cfg->reasoning_effort ? cfg->reasoning_effort : "default";
	ctx->route_explanation = fy_gb_intern_string(cfg->gb,
		fy_sprintfa("%s via %s, effort %s; task tier %d, route tier %d; "
			    "catalogue estimate $%.6f for %lld input and %d output "
			    "tokens; $%.6f turn budget; %d routes "
			    "excluded",
			    cfg->model, cfg->provider, effort, target,
			    selected_quality, selected_estimate, prompt.prompt,
			    cfg->max_tokens, budget, rejected));
	if (!cfg->acp_mode)
		(void)fyai_report(ctx, "routing: %s\n", ctx->route_explanation);
	gb = fyai_ctx_transient_gb(ctx);
	turn = *turnp;
	meta = fyai_turn_meta(turn);
	meta = fy_assoc(gb, meta, "model", cfg->model);
	meta = fy_assoc(gb, meta, "provider", cfg->provider);
	meta = fy_assoc(gb, meta, "api", fyai_api_to_string(cfg->api_mode));
	meta = fy_assoc(gb, meta, "reasoning_effort",
			cfg->reasoning_effort ?
			fy_value(cfg->reasoning_effort) : fy_null);
	meta = fy_assoc(gb, meta, "routing", ctx->route_explanation);
	turn = fy_assoc(gb, turn, "metadata", meta);
	if (fy_is_invalid(turn)) {
		fyai_error(ctx, "could not record automatic route");
		fyai_route_restore(ctx);
		return -1;
	}
	*turnp = turn;
	return 0;
}

int fyai_route_budget_check(struct fyai_ctx *ctx, fy_generic turn)
{
	struct fyai_context_prompt prompt;
	fy_generic policy;
	double input_price, output_price, estimate, budget;

	if (!route_auto(ctx))
		return 0;
	if (ctx->route_usage_unknown) {
		fyai_error(ctx, "provider omitted usage; cannot price another request in this turn");
		return -1;
	}
	policy = route_policy(ctx);
	budget = fy_number(fy_get(policy, "max_turn_usd", fy_invalid), 0.0);
	if (!route_prices(ctx, ctx->cfg->provider, ctx->cfg->model,
			  ctx->cfg->api_mode, ctx->cfg->api_url,
			  &input_price, &output_price)) {
		fyai_error(ctx, "the selected route has no catalogue price");
		return -1;
	}
	fyai_context_prompt_at(ctx, turn, &prompt);
	estimate = ((double)prompt.prompt * input_price +
		    (double)ctx->cfg->max_tokens * output_price) / 1000000.0;
	if (ctx->usage_cost - ctx->route_turn_start_cost + estimate > budget) {
		fyai_error(ctx, "turn budget $%.6f cannot cover the next estimated $%.6f request after $%.6f spent",
			   budget, estimate,
			   ctx->usage_cost - ctx->route_turn_start_cost);
		return -1;
	}
	return 0;
}

fy_generic fyai_route_price_usage(struct fyai_ctx *ctx, fy_generic usage)
{
	fy_generic reported;
	double input_price, output_price, cost;
	bool priced;

	if (fy_is_invalid(usage))
		return usage;
	reported = fy_get(usage, "cost_source", fy_invalid);
	if (fy_equal(reported, "provider"))
		return usage;
	priced = route_prices(ctx, ctx->cfg->provider, ctx->cfg->model,
			      ctx->cfg->api_mode, ctx->cfg->api_url,
			      &input_price, &output_price);
	if (!priced)
		return fy_assoc(ctx->transient_gb, usage, "cost_source", "unknown");
	cost = ((double)fy_get(usage, "input", 0LL) * input_price +
		(double)fy_get(usage, "output", 0LL) * output_price) /
		1000000.0;
	usage = fy_assoc(ctx->transient_gb, usage, "cost", cost);
	usage = fy_assoc(ctx->transient_gb, usage, "price_input_usd_per_million",
			 input_price);
	usage = fy_assoc(ctx->transient_gb, usage, "price_output_usd_per_million",
			 output_price);
	return fy_assoc(ctx->transient_gb, usage, "cost_source",
			"catalogue_estimate");
}
