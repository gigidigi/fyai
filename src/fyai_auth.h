/* SPDX-License-Identifier: MIT */
#ifndef FYAI_AUTH_H
#define FYAI_AUTH_H

#include <stdbool.h>
#include <time.h>
#include <libfyaml/libfyaml-generic.h>

struct fyai_ctx;
struct fyai_auth_refresh_request;
struct fyai_auth_login_request;

typedef void (*fyai_auth_refresh_complete_fn)(
		struct fyai_auth_refresh_request *request, void *userdata);
typedef void (*fyai_auth_login_complete_fn)(
		struct fyai_auth_login_request *request, void *userdata);

enum fyai_auth_mode {
	FYAI_AUTH_AUTO,
	FYAI_AUTH_API_KEY,
	FYAI_AUTH_CHATGPT,
};

/* all pointer are stable in the cfg builder */
struct fyai_credentials {
	const char *access_token;
	const char *refresh_token;
	const char *id_token;
	const char *account_id;
	const char *email;
	const char *plan;
	bool fedramp;
	time_t expires_at;
	const char *storage;
};

const char *fyai_auth_mode_string(enum fyai_auth_mode mode);
fy_generic fyai_auth_status_data(struct fyai_ctx *ctx,
				 struct fy_generic_builder *gb, bool info);
int fyai_auth_login(struct fyai_ctx *ctx, bool device_code,
		    bool no_browser, bool manual);
int fyai_auth_logout(struct fyai_ctx *ctx);
/*
 * Fetch the live limits of the active subscription into @out_gb: the
 * response as the provider sends it with @raw, else a summary of it.
 */
int fyai_auth_usage(struct fyai_ctx *ctx, struct fy_generic_builder *out_gb,
		    bool raw, fy_generic *datap);
int fyai_auth_resolve(struct fyai_ctx *ctx);
int fyai_auth_refresh(struct fyai_ctx *ctx, bool force);
struct fyai_auth_refresh_request *
fyai_auth_refresh_submit(struct fyai_ctx *ctx, bool force,
			 fyai_auth_refresh_complete_fn complete,
			 void *userdata);
void fyai_auth_refresh_cancel(struct fyai_auth_refresh_request *request);
bool fyai_auth_refresh_done(
		const struct fyai_auth_refresh_request *request);
int fyai_auth_refresh_collect(
		const struct fyai_auth_refresh_request *request);
void fyai_auth_refresh_destroy(struct fyai_auth_refresh_request *request);
struct fyai_auth_login_request *
fyai_auth_login_submit(struct fyai_ctx *ctx, bool device_code,
		       bool no_browser, fyai_auth_login_complete_fn complete,
		       void *userdata);
void fyai_auth_login_cancel(struct fyai_auth_login_request *request);
bool fyai_auth_login_done(const struct fyai_auth_login_request *request);
int fyai_auth_login_collect(const struct fyai_auth_login_request *request);
void fyai_auth_login_destroy(struct fyai_auth_login_request *request);
int fyai_auth_apply_headers(struct fyai_ctx *ctx,
			    struct curl_slist **headers);
bool fyai_auth_should_retry(struct fyai_ctx *ctx, long status);
fy_generic fyai_auth_models(struct fyai_ctx *ctx,
			    struct fy_generic_builder *gb, bool full);
void fyai_auth_cleanup(struct fyai_ctx *ctx);

#endif
