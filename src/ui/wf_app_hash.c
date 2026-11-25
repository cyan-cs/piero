/*
 * SHA-256 of the file under the cursor, on request (#; DR-DRAFT-027).
 *
 * The key starts a worker that reads the file and nothing else happens until it
 * answers; Esc stops it. The result is a claim about the bytes that were read, so
 * it is shown only for the file it was made from: the same path, and the size and
 * last write time the listing shows now are the ones the file had when it was read.
 * It is written to the status line and, for as long as it holds, to the details
 * window (i). Nothing is written to disk, and nothing is read until the key is
 * pressed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../archive/wf_vpath.h"
#include "../fs/wf_loc.h"
#include "../fs/wf_ntfsfacts.h"
#include "../fs/wf_sftpplace_win.h"
#include "../path/wf_path.h"
#include "wf_app_priv.h"
#include "wf_info.h"

void wf_app_set_hash(WfApp *app, WfHashPort port)
{
	app->hash_port = port;
}

static void forget_result(WfHash *h)
{
	h->has = false;
	memset(h->digest, 0, sizeof h->digest);
}

void wf_app_hash_destroy(WfApp *app)
{
	if (app->hash_port.cancel) {
		app->hash_port.cancel(app->hash_port.ctx);
	}
	free(app->hash.path);
	app->hash.path = NULL;
}

/* The message `id` with one or two words filled in. */
static void say(WfApp *app, WfMsg id, const char *a, const char *b)
{
	char text[sizeof app->msg];

	(void)wf_msgf(text, sizeof text, id,
	        (WfMsgArg[]){WF_ARG_S(a), WF_ARG_S(b ? b : "")}, b ? 2 : 1);
	wf_app_set_msg(app, text);
}

static const char *name_of(const WfApp *app)
{
	return app->hash.name[0] ? app->hash.name : "";
}

static void say_progress(WfApp *app)
{
	const WfHash *h = &app->hash;
	char what[96], size[40], pct[16];

	wf_info_format_size(h->total, size, sizeof size);
	snprintf(pct, sizeof pct, "%u%%",
	        h->total ? (unsigned)(h->done * 100u / h->total) : 0u);
	(void)wf_msgf(what, sizeof what, MSG_HASH_PROGRESS,
	        (WfMsgArg[]){WF_ARG_S(pct), WF_ARG_S(size)}, 2);
	say(app, MSG_HASH_RUNNING, name_of(app), what);
}

/* The full path of the entry under the cursor, or NULL. */
static wchar_t *entry_path(const WfApp *app, const WfEntry *e)
{
	wchar_t *full = NULL;

	if (!e || !app->cwd ||
	        WF_OK != wf_path_join(app->cwd, e->name, &full)) {
		return NULL;
	}
	return full;
}

/* True when the result in hand is of `e` as it is listed now. */
static bool holds_for(const WfApp *app, const WfEntry *e)
{
	wchar_t *full = entry_path(app, e);
	bool same = false;

	if (full && app->hash.has && app->hash.path) {
		same = !wcscmp(full, app->hash.path) &&
		        e->size == app->hash.size &&
		        e->mtime == app->hash.last_write;
	}
	free(full);
	return same;
}

bool wf_app_hash_line(const WfApp *app, const WfEntry *e, char *out, size_t cap)
{
	wchar_t *full;

	if (!e || (e->flags & WF_ENT_DIR) || cap < 65) {
		return false;
	}
	if (holds_for(app, e)) {
		wf_sha256_hex(app->hash.digest, out);
		return true;
	}
	full = entry_path(app, e);
	if (full && app->hash.running && app->hash.path &&
	        !wcscmp(full, app->hash.path)) {
		snprintf(out, cap, "%s", wf_msg(MSG_INFO_SHA_RUNNING));
		free(full);
		return true;
	}
	free(full);
	return false;
}

void wf_app_cmd_hash(WfApp *app)
{
	const WfEntry *e = wf_view_current(&app->cur);
	wchar_t *full;
	WfStatus st;

	if (!app->hash_port.start) {
		wf_app_set_msg(app, wf_msg(MSG_HASH_UNAVAILABLE));
		return;
	}
	if (app->want_path || !e) {
		wf_app_set_msg(app, wf_msg(MSG_HASH_NO_FILE));
		return;
	}
	if (e->flags & WF_ENT_DIR) {
		wf_app_set_msg(app, wf_msg(MSG_HASH_FOLDER));
		return;
	}
	if (e->flags & WF_ENT_LINK) {
		wf_app_set_msg(app, wf_msg(MSG_HASH_LINK));
		return;
	}
	/* a result already in hand for this file as it is now is shown again */
	if (holds_for(app, e)) {
		char hex[65];

		wf_sha256_hex(app->hash.digest, hex);
		say(app, MSG_HASH_DONE, hex, e->display);
		return;
	}
	full = entry_path(app, e);
	if (!full) {
		wf_app_set_error(app, WF_E_NOMEM);
		return;
	}
	if (wf_vpath_is(app->cwd) || wf_sftp_place_is(app->cwd) ||
	        wf_pcroot_is(app->cwd) || wf_wsl_unc_is(app->cwd) ||
	        wf_ntfsfacts_remote(full)) {
		free(full);
		wf_app_set_msg(app, wf_msg(MSG_HASH_REMOTE));
		return;
	}
	if (app->hash.running && app->hash_port.cancel) {
		app->hash_port.cancel(app->hash_port.ctx);
	}
	free(app->hash.path);
	app->hash.path = full;
	forget_result(&app->hash);
	snprintf(app->hash.name, sizeof app->hash.name, "%s", e->display);
	app->hash.size = e->size;
	app->hash.last_write = e->mtime;
	app->hash.done = 0;
	app->hash.total = e->size;
	st = app->hash_port.start(
	        app->hash_port.ctx, ++app->hash.gen, app->hash.path);
	if (WF_OK != st) {
		app->hash.running = false;
		wf_app_set_msg(app, wf_msg(MSG_HASH_UNAVAILABLE));
		return;
	}
	app->hash.running = true;
	say_progress(app);
}

bool wf_app_hash_stop(WfApp *app)
{
	if (!app->hash.running) {
		return false;
	}
	if (app->hash_port.cancel) {
		app->hash_port.cancel(app->hash_port.ctx);
	}
	app->hash.running =
	        false; /* what the worker still delivers is dropped */
	wf_app_set_msg(app, wf_msg(MSG_HASH_STOPPED));
	return true;
}

void wf_app_on_hash(WfApp *app, const WfHashUpdate *u)
{
	char text[40];

	if (!app->hash.running || u->gen != app->hash.gen) {
		return; /* not the newest request, or stopped */
	}
	if (!u->finished) {
		app->hash.done = u->done;
		app->hash.total = u->total;
		say_progress(app);
		return;
	}
	app->hash.running = false;
	switch (u->res.st) {
	case WF_OK: {
		char hex[65];

		memcpy(app->hash.digest, u->res.digest,
		        sizeof app->hash.digest);
		/* the file as it was when it was opened: the listing may be older,
		 * and then the details window will not show this as the file's */
		app->hash.size = u->res.size;
		app->hash.last_write = u->res.last_write;
		app->hash.has = true;
		wf_sha256_hex(u->res.digest, hex);
		say(app, MSG_HASH_DONE, hex, name_of(app));
		break;
	}
	case WF_E_CHANGED:
		say(app, MSG_HASH_CHANGED, name_of(app), NULL);
		break;
	case WF_E_ACCESS: say(app, MSG_HASH_ACCESS, name_of(app), NULL); break;
	case WF_E_NOENT:  say(app, MSG_HASH_NOENT, name_of(app), NULL); break;
	case WF_E_INVAL:  wf_app_set_msg(app, wf_msg(MSG_HASH_FOLDER)); break;
	default:
		snprintf(text, sizeof text, "%lu", (unsigned long)u->res.error);
		say(app, MSG_HASH_ERROR, name_of(app), text);
		break;
	}
}
