/*
 * Copy, move and delete in and out of a WSL distribution.
 *
 * A distribution has no file identity, so none of the identity-bound
 * transfer machinery can run there. What c / x picked is kept as plain text
 * (folder + names); a paste or a delete then sends the items one at a time
 * through the edit port to wf_wslops_win, which never replaces a name and
 * never follows a link. Deleting is permanent: there is no Recycle Bin.
 *
 *   copy   WSL -> WSL, WSL -> local, local -> WSL
 *   move   inside one distribution only (a rename); across places it is off
 *   delete WSL only; the confirmation names how much will go
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../base/wf_macro.h"
#include "../fs/wf_loc.h"
#include "../fs/wf_wslops_win.h"
#include "../path/wf_path.h"
#include "../text/wf_text.h"
#include "wf_app_priv.h"

/* Frees `n` names and the array; NULL (a copy that failed) is nothing to free. */
static void names_free(wchar_t **names, size_t n)
{
	if (!names) {
		return;
	}
	for (size_t i = 0; i < n; i++) {
		free(names[i]);
	}
	free(names);
}

void wf_app_wsl_clear_clip(WfApp *app)
{
	names_free(app->wclip.names, app->wclip.n);
	free(app->wclip.dir);
	memset(&app->wclip, 0, sizeof app->wclip);
}

static void seq_clear(WfApp *app)
{
	names_free(app->wseq.names, app->wseq.n);
	names_free(app->wseq.names2, app->wseq.n);
	free(app->wseq.src_dir);
	free(app->wseq.dst_dir);
	memset(&app->wseq, 0, sizeof app->wseq);
}

static void undo_free(WfWslUndo *u)
{
	names_free(u->from, u->n);
	names_free(u->to, u->n);
	free(u->dir);
	free(u->dst);
	memset(u, 0, sizeof *u);
}

void wf_app_wsl_destroy(WfApp *app)
{
	wf_app_wsl_clear_clip(app);
	seq_clear(app);
	undo_free(&app->wundo);
	undo_free(&app->wundo_pending);
}

/* The names a command acts on: the visible selection, else the cursor. A
 * copy is made so nothing borrows the listing. */
static WfStatus pick(WfApp *app, wchar_t ***out, size_t *n_out)
{
	const WfNameSet *sel = wf_sel_folder(&app->sel, app->cwd);
	size_t total = sel ? sel->n : 0, n = 0, cap;
	wchar_t **names;

	*out = NULL;
	*n_out = 0;
	cap = total ? total : 1;
	names = calloc(cap, sizeof *names);
	if (!names) {
		return WF_E_NOMEM;
	}
	if (!total) {
		const WfEntry *e = wf_view_current(&app->cur);

		if (e) {
			names[n] = wf_path_dup(e->name);
			if (!names[n]) {
				names_free(names, cap);
				return WF_E_NOMEM;
			}
			n++;
		}
	} else {
		for (size_t i = 0; i < app->cur.nvis && n < cap; i++) {
			const WfEntry *e = &app->cur.dir->e[app->cur.vis[i]];

			if (wf_nameset_has(sel, e->name)) {
				names[n] = wf_path_dup(e->name);
				if (!names[n]) {
					/* all of the selection or none of it */
					names_free(names, cap);
					return WF_E_NOMEM;
				}
				n++;
			}
		}
	}
	if (0 == n) {
		names_free(names, cap);
		return WF_E_INVAL;
	}
	*out = names;
	*n_out = n;
	return WF_OK;
}

bool wf_app_wsl_note_clip(WfApp *app, bool move)
{
	wchar_t **names;
	size_t n;
	wchar_t *dir;

	WfStatus st = pick(app, &names, &n);

	if (WF_OK != st) {
		/* the earlier mark is kept: a failure does not lose it */
		if (WF_E_NOMEM == st) {
			wf_app_set_msg(app, wf_msg(MSG_WSL_CLIP_NOMEM));
		}
		return false;
	}
	dir = wf_path_dup(app->cwd);
	if (!dir) {
		names_free(names, n);
		wf_app_set_msg(app, wf_msg(MSG_WSL_CLIP_NOMEM));
		return false;
	}
	wf_app_wsl_clear_clip(app);
	app->wclip.dir = dir;
	app->wclip.names = names;
	app->wclip.n = n;
	app->wclip.move = move;
	return true;
}

static const char *verb_of(int kind)
{
	return wf_msg(WF_EDIT_WSL_COPY == kind ? MSG_WSL_VERB_COPY
	                : WF_EDIT_MOVE == kind ? MSG_WSL_VERB_MOVE
	                                       : MSG_WSL_VERB_DELETE);
}

static const char *verb_now(const WfApp *app)
{
	return app->wseq.is_undo ? wf_msg(MSG_WSL_VERB_UNDO)
	                         : verb_of(app->wseq.kind);
}

static bool submit_next(WfApp *app)
{
	WfEditRequest req;

	memset(&req, 0, sizeof req);
	req.kind = (WfEditKind)app->wseq.kind;
	req.parent = app->wseq.src_dir;
	req.old_name = app->wseq.names[app->wseq.next];
	req.new_name = app->wseq.names2 ? app->wseq.names2[app->wseq.next]
	                                : app->wseq.names[app->wseq.next];
	req.dest_parent = app->wseq.dst_dir;
	if (WF_OK != app->edit.submit(app->edit.ctx, ++app->edit_gen, &req)) {
		return false;
	}
	app->edit_busy = true;
	return true;
}

/* Takes ownership of `names`, `src` and `dst`. */
static bool seq_begin(WfApp *app,
        int kind,
        wchar_t *src,
        wchar_t *dst,
        wchar_t **names,
        wchar_t **names2,
        bool is_undo,
        size_t n)
{
	seq_clear(app);
	wf_wslops_progress_reset(); /* before the worker can see the request */
	app->wseq.kind = kind;
	app->wseq.names2 = names2;
	app->wseq.is_undo = is_undo;
	app->wseq.src_dir = src;
	app->wseq.dst_dir = dst;
	app->wseq.names = names;
	app->wseq.n = n;
	if (!app->edit.submit || !src ||
	        (WF_EDIT_WSL_DELETE != kind &&
	                WF_EDIT_WSL_REMOVE_EMPTY != kind && !dst) ||
	        !submit_next(app)) {
		seq_clear(app);
		wf_app_set_msg(app, wf_msg(MSG_XFER_MOVE_START_FAILED));
		return false;
	}
	app->wseq.active = true;
	{
		char text[96];

		(void)wf_msgf(text, sizeof text, MSG_WSL_RUNNING,
		        &WF_ARG_S(verb_now(app)), 1);
		wf_app_set_msg(app, text);
	}
	return true;
}

static wchar_t **names_dup(wchar_t *const *names, size_t n)
{
	wchar_t **out = calloc(n ? n : 1, sizeof *out);

	for (size_t i = 0; out && i < n; i++) {
		out[i] = wf_path_dup(names[i]);
		if (!out[i]) {
			names_free(out, n);
			return NULL;
		}
	}
	return out;
}

bool wf_app_wsl_paste(WfApp *app)
{
	bool dest_wsl = wf_wsl_unc_is(app->cwd);
	bool src_wsl = app->wclip.dir && wf_wsl_unc_is(app->wclip.dir);
	wchar_t **names;
	wchar_t *src, *dst;
	int kind;

	if (!dest_wsl && !src_wsl) {
		return false; /* the ordinary paste */
	}
	if (!app->wclip.n) {
		wf_app_set_msg(app, wf_msg(MSG_XFER_SLOT_EMPTY));
		return true;
	}
	if (app->edit_busy || app->wseq.active || app->move.active) {
		wf_app_set_msg(app, wf_msg(MSG_OPS_BUSY));
		return true;
	}
	if (app->want_path) {
		wf_app_set_msg(app, wf_msg(MSG_OPS_BLOCKED_LOADING));
		return true;
	}
	if (app->wclip.move && !wf_wsl_same_distro(app->wclip.dir, app->cwd)) {
		wf_app_set_msg(app, wf_msg(MSG_WSL_MOVE_ACROSS));
		return true;
	}
	kind = app->wclip.move ? WF_EDIT_MOVE : WF_EDIT_WSL_COPY;
	names = names_dup(app->wclip.names, app->wclip.n);
	src = wf_path_dup(app->wclip.dir);
	dst = wf_path_dup(app->cwd);
	if (!names || !src || !dst) {
		names_free(names, app->wclip.n);
		free(src);
		free(dst);
		wf_app_set_msg(app, wf_msg(MSG_XFER_MOVE_NOMEM));
		return true;
	}
	(void)seq_begin(app, kind, src, dst, names, NULL, false, app->wclip.n);
	return true;
}

bool wf_app_wsl_tick(WfApp *app)
{
	unsigned long long now;
	bool changed;

	if (!app->wseq.active) {
		return false;
	}
	now = wf_wslops_progress();
	changed = now != app->wseq.shown;
	app->wseq.shown = now;
	return changed;
}

uint32_t wf_app_wsl_next_ms(const WfApp *app)
{
	return app->wseq.active ? 250u : UINT32_MAX;
}

void wf_app_wsl_status(const WfApp *app, WfBuf *s)
{
	char bytes[32], text[128];

	if (!app->wseq.active) {
		return;
	}
	wf_app_fmt_bytes(wf_wslops_progress(), bytes, sizeof bytes);
	wf_buf_puts(s, " | ");
	(void)wf_msgf(text, sizeof text, MSG_WSL_PROGRESS,
	        (WfMsgArg[]){WF_ARG_S(verb_now(app)),
	                WF_ARG_N(app->wseq.next + 1), WF_ARG_N(app->wseq.n),
	                WF_ARG_S(bytes)},
	        4);
	wf_buf_puts(s, text);
}

/* An Undo that stopped after `done` items has put those back: they leave the
 * record, so that the next Undo goes on with the rest. */
static void undo_drop_done(WfWslUndo *u, size_t done)
{
	if (done >= u->n) {
		return;
	}
	for (size_t i = 0; i < done; i++) {
		free(u->from ? u->from[i] : NULL);
		free(u->to ? u->to[i] : NULL);
	}
	if (u->from) {
		memmove(u->from, u->from + done,
		        (u->n - done) * sizeof *u->from);
	}
	if (u->to) {
		memmove(u->to, u->to + done, (u->n - done) * sizeof *u->to);
	}
	u->n -= done;
}

/* What a finished move needs to be put back: the items that did move. */
static void record_move(WfApp *app)
{
	WfWslUndo u;
	size_t n = app->wseq.done;

	memset(&u, 0, sizeof u);
	u.kind = WF_EDIT_MOVE;
	u.dir = wf_path_dup(app->wseq.src_dir);
	u.dst = wf_path_dup(app->wseq.dst_dir);
	u.from = names_dup(app->wseq.names, n);
	u.to = names_dup(app->wseq.names, n);
	u.n = n;
	u.valid = u.dir && u.dst && u.from && u.to;
	undo_free(&app->wundo);
	if (u.valid) {
		app->wundo = u;
	} else {
		undo_free(&u);
	}
}

static void finish(WfApp *app, bool stopped, const char *why)
{
	char text[224];
	const char *verb = verb_now(app);
	const bool was_undo = app->wseq.is_undo;

	if (stopped) {
		(void)wf_msgf(text, sizeof text, MSG_WSL_STOPPED,
		        (WfMsgArg[]){WF_ARG_S(verb), WF_ARG_N(app->wseq.done),
		                WF_ARG_N(app->wseq.n), WF_ARG_S(why)},
		        4);
	} else {
		(void)wf_msgf(text, sizeof text, MSG_WSL_DONE,
		        (WfMsgArg[]){WF_ARG_S(verb), WF_ARG_N(app->wseq.done)},
		        2);
	}
	if (!stopped && WF_EDIT_WSL_COPY != app->wseq.kind && !was_undo) {
		wf_app_wsl_clear_clip(
		        app); /* moved or deleted: gone from there */
	}
	if (was_undo && !stopped) {
		undo_free(&app->wundo); /* it has been put back */
	} else if (was_undo) {
		undo_drop_done(&app->wundo, app->wseq.done);
	} else if (!was_undo && WF_EDIT_MOVE == app->wseq.kind &&
	        app->wseq.done > 0) {
		record_move(app);
	}
	if (app->wseq.warned) {
		char note[96];
		size_t used = strlen(text);

		(void)wf_msgf(note, sizeof note, MSG_WSL_WARNED,
		        (WfMsgArg[]){WF_ARG_N(app->wseq.warned),
		                WF_ARG_S(app->wseq.warn ? app->wseq.warn : "")},
		        2);
		snprintf(text + used, sizeof text - used, " [%s]", note);
	}
	seq_clear(app);
	app->msg_keep_until = app->now_ms + WF_RECEIPT_KEEP_MS;
	wf_app_reload(app);
	wf_app_set_msg(app, text);
}

bool wf_app_wsl_cancel(WfApp *app)
{
	if (!app->wseq.active) {
		return false;
	}
	app->wseq.cancelled = true;
	wf_wslops_request_cancel();
	wf_app_set_msg(app, wf_msg(MSG_JOB_CANCELLING));
	return true;
}

bool wf_app_wsl_receipt(
        WfApp *app, const WfEditOutcome *outcome, const wchar_t *name)
{
	char why[120] = "", shown[64] = "?";
	WfBuf clean;

	if (!app->wseq.active) {
		return false;
	}
	if (WF_OK != outcome->st) {
		wf_buf_init(&clean);
		if (name &&
		        WF_OK ==
		                wf_sanitize_utf16(name, wcslen(name), &clean)) {
			snprintf(
			        shown, sizeof shown, "%s", wf_buf_cstr(&clean));
		}
		wf_buf_free(&clean);
		if (outcome->detail) {
			snprintf(why, sizeof why, "%s: %s", shown,
			        outcome->detail);
		} else {
			wf_app_edit_failure_text(
			        outcome, shown, why, sizeof why);
		}
		if (outcome->partial) {
			/* the item that stopped was changed in part: say so, the
			 * count of top-level items alone would read "0 done" */
			char part[96];
			size_t used = strlen(why);

			(void)wf_msgf(part, sizeof part, MSG_WSL_PARTIAL,
			        &WF_ARG_N(outcome->partial), 1);
			snprintf(why + used, sizeof why - used, " (%s)", part);
		}
		finish(app, true, why);
		return true;
	}
	if (outcome->detail) { /* done, with something that did not matter */
		app->wseq.warned++;
		app->wseq.warn = outcome->detail;
	}
	app->wseq.done++;
	app->wseq.next++;
	if (app->wseq.cancelled && app->wseq.next < app->wseq.n) {
		finish(app, true, wf_msg(MSG_WSL_CANCELLED));
		return true;
	}
	if (app->wseq.next == app->wseq.n) {
		finish(app, false, "");
		return true;
	}
	if (!submit_next(app)) {
		finish(app, true, wf_msg(MSG_XFER_NEXT_FAILED));
	}
	return true;
}

static void delete_yes(WfApp *app, void *ctx)
{
	wchar_t **names;
	size_t n = 0;
	wchar_t *dir;

	(void)ctx;
	names = app->wseq.names;
	n = app->wseq.n;
	dir = app->wseq.src_dir;
	/* hand the pending list over to the running sequence */
	app->wseq.names = NULL;
	app->wseq.src_dir = NULL;
	app->wseq.n = 0;
	if (app->edit_busy || !names || !dir) {
		names_free(names, n);
		free(dir);
		wf_app_set_msg(app, wf_msg(MSG_OPS_BUSY));
		return;
	}
	(void)seq_begin(
	        app, WF_EDIT_WSL_DELETE, dir, NULL, names, NULL, false, n);
}

bool wf_app_wsl_delete(WfApp *app, const WfEvent *ev)
{
	wchar_t **names;
	size_t n;
	char count[96], first[96];
	const char *lines[3];
	WfBuf clean;
	WfStatus st;

	if (!wf_wsl_unc_is(app->cwd)) {
		return false;
	}
	if (app->edit_busy || app->wseq.active || app->move.active) {
		wf_app_set_msg(app, wf_msg(MSG_OPS_BUSY));
		return true;
	}
	if (app->want_path) {
		wf_app_set_msg(app, wf_msg(MSG_OPS_BLOCKED_LOADING));
		return true;
	}
	st = pick(app, &names, &n);
	if (WF_E_NOMEM == st) {
		wf_app_set_error(
		        app, st); /* nothing is started on part of it */
		return true;
	}
	if (WF_OK != st) {
		wf_app_set_msg(app, wf_msg(MSG_OPS_BLOCKED_NO_ENTRY));
		return true;
	}
	seq_clear(app); /* a pending list that was never confirmed */
	app->wseq.names = names;
	app->wseq.n = n;
	app->wseq.src_dir = wf_path_dup(app->cwd);
	if (!app->wseq.src_dir) {
		seq_clear(app);
		wf_app_set_msg(app, wf_msg(MSG_XFER_MOVE_NOMEM));
		return true;
	}
	(void)wf_msgf(
	        count, sizeof count, MSG_WSL_DELETE_COUNT, &WF_ARG_N(n), 1);
	wf_buf_init(&clean);
	snprintf(first, sizeof first, "%s", "?");
	if (WF_OK == wf_sanitize_utf16(names[0], wcslen(names[0]), &clean)) {
		snprintf(first, sizeof first, "%s", wf_buf_cstr(&clean));
	}
	wf_buf_free(&clean);
	lines[0] = wf_msg(MSG_WSL_DELETE_TITLE);
	lines[1] = count;
	lines[2] = first;
	st = wf_app_confirm_begin(app, lines, 3, delete_yes, NULL, ev);
	if (WF_OK != st) {
		seq_clear(app);
		wf_app_set_error(app, st);
	}
	return true;
}

void wf_app_wsl_note_form(WfApp *app, bool rename, bool directory)
{
	WfWslUndo u;

	undo_free(&app->wundo_pending);
	if (!wf_wsl_unc_is(app->cwd)) {
		return;
	}
	/* The undo removes one empty entry of this folder; "a/b" made folders
	 * on the way, so it has no undo. */
	if (!rename && wcspbrk(app->path_input, L"/\\")) {
		return;
	}
	memset(&u, 0, sizeof u);
	u.kind = rename     ? WF_EDIT_RENAME
	        : directory ? WF_EDIT_CREATE_DIR
	                    : WF_EDIT_CREATE_FILE;
	u.dir = wf_path_dup(app->cwd);
	u.dst = wf_path_dup(app->cwd);
	u.from = calloc(1, sizeof *u.from);
	u.to = calloc(1, sizeof *u.to);
	if (u.from && rename) {
		u.from[0] = wf_path_dup(app->name_form.source_name);
	}
	if (u.to) {
		u.to[0] = wf_path_dup_n(app->path_input, app->path_input_len);
	}
	u.n = 1;
	u.valid = u.dir && u.dst && u.from && u.to && u.to[0] &&
	        (!rename || u.from[0]);
	if (u.valid) {
		app->wundo_pending = u;
	} else {
		undo_free(&u);
	}
}

void wf_app_wsl_edit_done(WfApp *app, const WfEditOutcome *outcome)
{
	if (!app->wundo_pending.valid) {
		return;
	}
	if (outcome && WF_OK == outcome->st) {
		undo_free(&app->wundo);
		app->wundo = app->wundo_pending;
		memset(&app->wundo_pending, 0, sizeof app->wundo_pending);
	} else {
		undo_free(&app->wundo_pending);
	}
}

bool wf_app_wsl_undo(WfApp *app)
{
	const WfWslUndo *u = &app->wundo;
	wchar_t **to, **from = NULL;
	wchar_t *src, *dst = NULL;
	int kind;

	if (!u->valid || !wf_wsl_unc_is(app->cwd) ||
	        !wf_wsl_same_distro(u->dst, app->cwd)) {
		return false; /* the ordinary undo */
	}
	if (app->edit_busy || app->wseq.active || app->move.active) {
		wf_app_set_msg(app, wf_msg(MSG_OPS_BUSY));
		return true;
	}
	kind = WF_EDIT_RENAME == u->kind  ? WF_EDIT_RENAME
	        : WF_EDIT_MOVE == u->kind ? WF_EDIT_MOVE
	                                  : WF_EDIT_WSL_REMOVE_EMPTY;
	to = names_dup(u->to, u->n);
	src = wf_path_dup(u->dst);
	if (WF_EDIT_RENAME == kind) {
		from = names_dup(u->from, u->n);
		dst = wf_path_dup(u->dst);
	} else if (WF_EDIT_MOVE == kind) {
		dst = wf_path_dup(u->dir);
	}
	if (!to || !src || (WF_EDIT_RENAME == kind && !from) ||
	        (WF_EDIT_WSL_REMOVE_EMPTY != kind && !dst)) {
		names_free(to, u->n);
		names_free(from, u->n);
		free(src);
		free(dst);
		wf_app_set_msg(app, wf_msg(MSG_XFER_MOVE_NOMEM));
		return true;
	}
	(void)seq_begin(app, kind, src, dst, to, from, true, u->n);
	return true;
}
