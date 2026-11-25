/*
 * Transfer Slot UI and the preflight hand-off (spec 2.3, 2.4, 7.4, 8.4, 12).
 *
 * y/x snapshot the *visible* selection (or the cursor entry) into the slot.
 * p runs the full preflight against the current folder and then either
 * refuses with a reason, asks the one confirmation the spec calls for, or
 * hands over to the executor. There is no executor in this build, so a
 * successful preflight ends with an explicit "engine disabled" message.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../archive/wf_vpath.h"
#include "../fs/wf_loc.h"
#include "../path/wf_path.h"
#include "../text/wf_text.h"
#include "wf_app_priv.h"
#include "../base/wf_macro.h"

static const char *verb_of(WfTransferMode m)
{
	return wf_msg(
	        WF_XFER_MOVE == m ? MSG_JOB_VERB_MOVE : MSG_JOB_VERB_COPY);
}

void wf_app_fmt_bytes(uint64_t b, char *out, size_t cap)
{
	static const char *const unit[] = {
	        "B", "KiB", "MiB", "GiB", "TiB", "PiB"};
	double v = (double)b;
	size_t u = 0;

	while (v >= 1024.0 && u + 1 < WF_ARRAY_LEN(unit)) {
		v /= 1024.0;
		u++;
	}
	if (0 == u) {
		snprintf(out, cap, "%llu B", (unsigned long long)b);
	} else {
		snprintf(out, cap, "%.1f %s", v, unit[u]);
	}
}

/* Middle ellipsis keeps the start and the end (the part that distinguishes
 * paths and file names). UTF-8 sequences are never cut. */
static void ellipsize_middle(const char *s, size_t len, size_t max, WfBuf *out)
{
	size_t head, tail_start;

	if (len <= max) {
		wf_buf_append(out, s, len);
		return;
	}
	head = max / 2 - 1;
	tail_start = len - (max - head - 3);
	while (head > 0 && 0x80 == ((unsigned char)s[head] & 0xC0)) {
		head--;
	}
	while (tail_start < len &&
	        0x80 == ((unsigned char)s[tail_start] & 0xC0)) {
		tail_start++;
	}
	wf_buf_append(out, s, head);
	wf_buf_puts(out, "...");
	wf_buf_append(out, s + tail_start, len - tail_start);
}

/* "<verb> FAILED: <reason> [name]" */
static void report_refusal(WfApp *app, const WfPreflightReport *rep)
{
	WfBuf b;

	wf_buf_init(&b);
	wf_buf_puts(&b, verb_of(app->slot.mode));
	wf_buf_puts(&b, " FAILED: ");
	wf_buf_puts(&b, wf_pf_reason_text(rep->reason));
	if (rep->bad_name[0]) {
		wf_buf_puts(&b, " [");
		wf_sanitize_utf16(rep->bad_name, wcslen(rep->bad_name), &b);
		wf_buf_putc(&b, ']');
	}
	wf_app_set_msg(app, wf_buf_cstr(&b));
	wf_buf_free(&b);
}

static bool xfer_observation_ready(const WfApp *app)
{
	/* A committed empty directory is a valid destination. Source capture
	 * separately requires visible selected entries or a current entry. */
	return NULL == app->want_path && NULL != app->cur.dir;
}

/* Collects the names to act on: the selected entries that are on screen, or
 * the entry under the cursor when nothing is selected. Selected entries the
 * view hides are counted, never silently included (spec 2.3). False, with the
 * reason shown when there is one, when there is nothing to do. */
bool wf_app_collect_targets(
        WfApp *app, const wchar_t **names, size_t *n, size_t *hidden)
{
	const WfNameSet *sel = wf_sel_folder(&app->sel, app->cwd);
	size_t total = sel ? sel->n : 0, i;

	*n = *hidden = 0;
	if (!total) {
		const WfEntry *e = wf_view_current(&app->cur);

		if (!e) {
			return false;
		}
		names[(*n)++] = e->name;
		return true;
	}
	for (i = 0; i < app->cur.nvis; i++) {
		const WfEntry *e = &app->cur.dir->e[app->cur.vis[i]];

		if (wf_nameset_has(sel, e->name)) {
			names[(*n)++] = e->name;
		}
	}
	if (0 == *n) {
		char m[160];

		(void)wf_msgf(m, sizeof m, MSG_XFER_HIDDEN_BY_VIEW,
		        &WF_ARG_N(total), 1);
		wf_app_set_msg(app, m);
		return false;
	}
	*hidden = total - *n;
	return true;
}

static void start_capture(WfApp *app, WfTransferMode mode)
{
	size_t n, hidden;
	const wchar_t **names;
	WfStatus st;

	if (!app->xfer.capture) {
		wf_app_set_msg(app, wf_msg(MSG_XFER_NOT_AVAILABLE));
		return;
	}
	if (WF_XB_IDLE != app->xfer_busy) {
		wf_app_set_msg(app, wf_msg(MSG_XFER_BUSY_CHECK));
		return;
	}
	if (!xfer_observation_ready(app)) {
		wf_app_set_msg(app, wf_msg(MSG_XFER_STALE_LISTING));
		return;
	}
	names = calloc(app->cur.nvis ? app->cur.nvis : 1, sizeof *names);
	if (!names) {
		wf_app_set_msg(app, wf_msg(MSG_XFER_NOMEM));
		return;
	}
	if (!wf_app_collect_targets(app, names, &n, &hidden)) {
		free(names);
		return;
	}
	app->capture_mode = mode;
	app->xfer_busy = WF_XB_CAPTURE;
	st = app->xfer.capture(app->xfer.ctx, ++app->xfer_gen, mode, app->cwd,
	        names, n, hidden);
	free(names);
	if (WF_OK != st) {
		app->xfer_busy = WF_XB_IDLE;
		wf_app_set_msg(app, wf_msg(MSG_XFER_CAPTURE_START_FAILED));
	}
}

bool wf_app_stage_names(WfApp *app,
        WfTransferMode mode,
        const wchar_t *dir,
        const wchar_t **names,
        size_t n)
{
	WfStatus st;

	if (!app->xfer.capture) {
		wf_app_set_msg(app, wf_msg(MSG_XFER_NOT_AVAILABLE));
		return false;
	}
	if (WF_XB_IDLE != app->xfer_busy) {
		wf_app_set_msg(app, wf_msg(MSG_XFER_BUSY_CHECK));
		return false;
	}
	app->capture_mode = mode;
	app->xfer_busy = WF_XB_CAPTURE;
	st = app->xfer.capture(
	        app->xfer.ctx, ++app->xfer_gen, mode, dir, names, n, 0);
	if (WF_OK != st) {
		app->xfer_busy = WF_XB_IDLE;
		wf_app_set_msg(app, wf_msg(MSG_XFER_CAPTURE_START_FAILED));
		return false;
	}
	return true;
}

void wf_app_on_capture(
        WfApp *app, uint32_t gen, WfStatus st, WfTransferSlot *slot)
{
	if (!slot) {
		return;
	}
	if (gen != app->xfer_gen || WF_XB_CAPTURE != app->xfer_busy) {
		wf_slot_free(slot);
		return;
	}
	app->xfer_busy = WF_XB_IDLE;
	if (WF_OK != st) {
		app->paste_after_capture = false;
		char m[160];

		(void)wf_msgf(m, sizeof m, MSG_XFER_CAPTURE_FAILED,
		        &WF_ARG_S(wf_status_name(st)), 1);
		wf_app_set_msg(app, m);
		wf_slot_free(slot);
		return;
	}
	wf_slot_free(&app->slot);
	app->slot = *slot;
	wf_slot_init(slot);
	wf_app_extract_drop(app); /* the newest pick is the only one */
	app->pending_cross = false;
	app->pending_collide = WF_COLLIDE_STOP;
	app->msg[0] = '\0';
	if (app->paste_after_capture) {
		/* V: the files of the Windows clipboard are the slot now, and the paste
		 * the person asked for goes on as `p` does (preflight, question, copy) */
		app->paste_after_capture = false;
		wf_app_cmd_paste(app);
	}
}

static void start_preflight(WfApp *app)
{
	WfStatus st;

	if (WF_XFER_NONE == app->slot.mode) {
		wf_app_set_msg(app, wf_msg(MSG_XFER_SLOT_EMPTY));
		return;
	}
	if (WF_XB_IDLE != app->xfer_busy) {
		wf_app_set_msg(app, wf_msg(MSG_XFER_BUSY_CHECK));
		return;
	}
	if (!xfer_observation_ready(app)) {
		wf_app_set_msg(app, wf_msg(MSG_XFER_STALE_LISTING));
		return;
	}
	if (!app->xfer.preflight) {
		wf_app_set_msg(app, wf_msg(MSG_XFER_NOT_AVAILABLE));
		return;
	}
	app->xfer_busy = WF_XB_PREFLIGHT;
	st = app->xfer.preflight(
	        app->xfer.ctx, ++app->xfer_gen, &app->slot, app->cwd);
	if (WF_OK != st) {
		app->xfer_busy = WF_XB_IDLE;
		wf_app_set_msg(app, wf_msg(MSG_XFER_PREFLIGHT_START_FAILED));
	}
}

/* Same-volume moves send the slot's items through the edit port one at a time.
 * Each carries the identities captured at `x` (source folder and item), plus
 * the destination folder's identity from its listing. The backend checks all three at
 * the moment of the call and never replaces anything. There is no rollback: a
 * failure stops the sequence and says how far it got. */

static void move_clear(WfApp *app)
{
	wf_slot_free(&app->move.slot);
	free(app->move.dest);
	memset(&app->move, 0, sizeof app->move);
}

/* A completed (or part-completed) forward move can be taken back: record the
 * items that moved, where they are now and where they came from. */
static uint32_t record_move_undo(WfApp *app)
{
	WfUndoEntry u;
	size_t i, n = app->move.moved;

	if (!n || app->move.is_undo) {
		return 0;
	}
	memset(&u, 0, sizeof u);
	u.kind = WF_UNDO_MOVE;
	u.folder = wf_path_dup(app->move.dest);
	u.folder_id = app->move.dest_id;
	u.origin = wf_path_dup(app->move.slot.dir);
	u.origin_id = app->move.slot.dir_id;
	u.item = calloc(n, sizeof *u.item);
	if (!u.folder || !u.origin || !u.item) {
		wf_app_undo_free(&u);
		return 0;
	}
	for (i = 0; i < n; i++) {
		u.item[i].name = wf_path_dup(app->move.slot.src[i].name);
		u.item[i].id = app->move.slot.src[i].snap.id;
		u.item[i].is_dir =
		        0 != (app->move.slot.src[i].snap.flags & WF_ENT_DIR);
		if (!u.item[i].name) {
			u.n = i;
			wf_app_undo_free(&u);
			return 0;
		}
		u.n = i + 1;
	}
	return wf_app_undo_push(app, &u);
}

/* The row of the history for a move that moved something. */
static void move_history_row(WfApp *app, size_t moved, uint32_t undo_uid)
{
	char head[128], where[80];
	WfBuf dest;

	wf_buf_init(&dest);
	(void)wf_sanitize_utf16(app->move.dest, wcslen(app->move.dest), &dest);
	wf_utf8_elide(where, sizeof where, wf_buf_cstr(&dest), 44);
	wf_buf_free(&dest);
	(void)wf_msgf(head, sizeof head, MSG_HIST_MOVE,
	        (WfMsgArg[]){WF_ARG_N(moved), WF_ARG_S(where)}, 2);
	wf_app_hist_add(app, head, undo_uid, MSG_HIST_WHY_NOT_KEPT);
}

/* Ends the sequence: reports, drops what was moved from the slot, reloads. */
static void move_finish(WfApp *app, const char *text)
{
	size_t moved = app->move.moved;
	bool was_undo = app->move.is_undo;

	if (was_undo) {
		wf_app_undo_done(app, moved); /* the entry shrinks or goes */
	}
	if (moved && !was_undo) {
		move_history_row(app, moved, record_move_undo(app));
	}
	move_clear(app);
	if (moved && !was_undo) {
		wf_slot_free(
		        &app->slot); /* the slot no longer describes the folder */
	}
	app->msg_keep_until = app->now_ms + WF_RECEIPT_KEEP_MS;
	wf_app_reload(app);
	wf_app_set_msg(app, text);
}

static bool move_submit_next(WfApp *app)
{
	const WfSourceSnap *item = &app->move.slot.src[app->move.next];
	WfEditRequest req;

	memset(&req, 0, sizeof req);
	req.kind = WF_EDIT_MOVE;
	req.parent = app->move.slot.dir;
	req.parent_id = app->move.slot.dir_id;
	req.old_name = item->name;
	req.source_id = item->snap.id;
	req.new_name = item->name;
	req.dest_parent = app->move.dest;
	req.dest_parent_id = app->move.dest_id;
	if (WF_OK != app->edit.submit(app->edit.ctx, ++app->edit_gen, &req)) {
		return false;
	}
	app->edit_busy = true;
	return true;
}

static void move_report(WfApp *app, bool stopped, const char *why)
{
	char text[192];
	const char *verb = wf_msg(
	        app->move.is_undo ? MSG_XFER_UNDO_VERB : MSG_JOB_VERB_MOVE);
	long long moved = (long long)app->move.moved;

	if (stopped) {
		(void)wf_msgf(text, sizeof text, MSG_XFER_MOVE_STOPPED,
		        (WfMsgArg[]){WF_ARG_S(verb), WF_ARG_N(app->move.moved),
		                WF_ARG_N(app->move.slot.count), WF_ARG_S(why)},
		        4);
	} else if (app->move.is_undo) {
		(void)wf_msgf(text, sizeof text,
		        wf_msg_count(MSG_XFER_UNDO_DONE_ONE,
		                MSG_XFER_UNDO_DONE_OTHER, moved),
		        &WF_ARG_N(app->move.moved), 1);
	} else {
		(void)wf_msgf(text, sizeof text,
		        wf_msg_count(MSG_XFER_MOVE_DONE_ONE,
		                MSG_XFER_MOVE_DONE_OTHER, moved),
		        &WF_ARG_N(app->move.moved), 1);
	}
	move_finish(app, text);
}

bool wf_app_move_receipt(
        WfApp *app, const WfEditOutcome *outcome, const wchar_t *name)
{
	char why[96] = "", shown[64] = "?";
	WfBuf clean;

	if (!app->move.active) {
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
		wf_app_edit_failure_text(outcome, shown, why, sizeof why);
		wf_app_lock_move_failed(app, outcome, app->move.next);
		move_report(app, true, why);
		return true;
	}
	app->move.moved++;
	app->move.next++;
	if (app->move.next == app->move.slot.count) {
		move_report(app, false, "");
		return true;
	}
	if (!move_submit_next(app)) {
		move_report(app, true, wf_msg(MSG_XFER_NEXT_FAILED));
	}
	return true;
}

/* Starts the sequence. Refuses, changing nothing, unless every identity it
 * needs is proven. */
bool wf_app_move_begin(WfApp *app,
        const WfTransferSlot *slot,
        const wchar_t *dest,
        const WfFileId *dest_id,
        bool is_undo)
{
	if (app->edit_busy || app->move.active) {
		wf_app_set_msg(app, wf_msg(MSG_OPS_BUSY));
		return false;
	}
	if (!dest_id || !(dest_id->kind == WF_ID_STRONG) ||
	        !(slot->dir_id.kind == WF_ID_STRONG)) {
		wf_app_set_msg(app, wf_msg(MSG_XFER_MOVE_BLOCKED_ID));
		return false;
	}
	if (WF_OK != wf_slot_clone(&app->move.slot, slot) ||
	        !(app->move.dest = wf_path_dup(dest))) {
		move_clear(app);
		wf_app_set_msg(app, wf_msg(MSG_XFER_MOVE_NOMEM));
		return false;
	}
	app->move.dest_id = *dest_id;
	app->move.is_undo = is_undo;
	app->move.active = true;
	if (!move_submit_next(app)) {
		move_clear(app);
		wf_app_set_msg(app, wf_msg(MSG_XFER_MOVE_START_FAILED));
		return false;
	}
	wf_app_set_msg(
	        app, wf_msg(is_undo ? MSG_XFER_UNDOING_MOVE : MSG_XFER_MOVING));
	return true;
}

static void start_move(WfApp *app)
{
	if (!app->cur.dir || !(app->cur.dir->identity.kind == WF_ID_STRONG)) {
		wf_app_set_msg(app, wf_msg(MSG_XFER_MOVE_BLOCKED_ID));
		return;
	}
	(void)wf_app_move_begin(
	        app, &app->slot, app->cwd, &app->cur.dir->identity, false);
}

static void proceed(WfApp *app, bool hydrate)
{
	const WfPreflightReport *rep = &app->last_report;
	char m[192];

	if (!app->exec.start && WF_XFER_MOVE == app->slot.mode &&
	        app->edit.submit) {
		start_move(app);
		return;
	}
	if (WF_XFER_COPY == app->slot.mode && wf_app_copy_start(app, hydrate)) {
		return;
	}
	if (app->exec.start) {
		WfStatus st = app->exec.start(
		        app->exec.ctx, &app->slot, app->cwd, rep, hydrate);

		if (WF_OK != st) {
			(void)wf_msgf(m, sizeof m, MSG_XFER_START_FAILED,
			        (WfMsgArg[]){WF_ARG_S(verb_of(app->slot.mode)),
			                WF_ARG_S(wf_status_name(st))},
			        2);
			wf_app_set_msg(app, m);
		}
		return;
	}
	/* Without an execution port, report only the preflight result. */
	if (rep->auto_recovery) {
		(void)wf_msgf(m, sizeof m, MSG_XFER_PREFLIGHT_OK,
		        &WF_ARG_S(verb_of(app->slot.mode)), 1);
	} else {
		WfBuf fs;

		wf_buf_init(&fs);
		wf_sanitize_utf16(rep->fs_name, wcslen(rep->fs_name), &fs);
		(void)wf_msgf(m, sizeof m, MSG_XFER_PREFLIGHT_OK_NO_RECOVERY,
		        &WF_ARG_S(wf_buf_cstr(&fs)), 1);
		wf_buf_free(&fs);
	}
	wf_app_set_msg(app, m);
}

static void yes_proceed(WfApp *app, void *ctx)
{
	(void)ctx;
	proceed(app, false);
}

static void yes_hydrate(WfApp *app, void *ctx)
{
	(void)ctx;
	proceed(app, true);
}

/* The destination line of a modal, shortened in the middle if needed. */
static void dest_line(const WfApp *app, WfBuf *out)
{
	WfBuf full;

	wf_buf_init(&full);
	if (WF_OK == wf_sanitize_utf16(app->cwd, wcslen(app->cwd), &full)) {
		ellipsize_middle(wf_buf_cstr(&full), full.len, 58, out);
	}
	wf_buf_free(&full);
}

static void offer_hydrate(WfApp *app)
{
	const WfPreflightReport *r = &app->last_report;
	char l1[WF_CONFIRM_LINE_BYTES], l2[WF_CONFIRM_LINE_BYTES], sz[32];
	WfBuf dest;
	const char *lines[5];
	WfStatus st;

	wf_app_fmt_bytes(r->cloud_bytes, sz, sizeof sz);
	(void)wf_msgf(l1, sizeof l1, MSG_XFER_HYDRATE_LINE,
	        &WF_ARG_N(r->cloud_files), 1);
	(void)wf_msgf(l2, sizeof l2, MSG_XFER_HYDRATE_SIZE, &WF_ARG_S(sz), 1);
	wf_buf_init(&dest);
	dest_line(app, &dest);
	lines[0] = wf_msg(MSG_JOB_VERB_COPY);
	lines[1] = l1;
	lines[2] = l2;
	lines[3] = wf_msg(MSG_XFER_DESTINATION);
	lines[4] = wf_buf_cstr(&dest);
	/* H, not Y: a stray "yes" must never start a download (spec 8.4) */
	st = wf_app_confirm_begin_ex(app, lines, 5, 'h',
	        wf_msg(MSG_XFER_HYDRATE_KEYS), yes_hydrate, NULL, NULL);
	wf_buf_free(&dest);
	if (WF_OK != st) {
		wf_app_set_error(app, st);
	}
}

static void offer_big_move(WfApp *app)
{
	const WfTransferSlot *s = &app->slot;
	size_t files = 0, dirs = 0, links = 0, i;
	char l1[WF_CONFIRM_LINE_BYTES], l2[WF_CONFIRM_LINE_BYTES],
	        l3[WF_CONFIRM_LINE_BYTES];
	WfBuf dest;
	const char *lines[8];
	WfStatus st;
	int n = 0;

	/* links are their own category, as in the spec's example (270 + 12 + 2)
	 */
	for (i = 0; i < s->count; i++) {
		uint32_t f = s->src[i].snap.flags;

		if (f & WF_ENT_LINK) {
			links++;
		} else if (f & WF_ENT_DIR) {
			dirs++;
		} else {
			files++;
		}
	}
	(void)wf_msgf(
	        l1, sizeof l1, MSG_XFER_BIG_MOVE_LINE, &WF_ARG_N(s->count), 1);
	(void)wf_msgf(l2, sizeof l2, MSG_XFER_BIG_MOVE_COUNTS,
	        (WfMsgArg[]){WF_ARG_N(files), WF_ARG_N(dirs), WF_ARG_N(links)},
	        3);
	wf_buf_init(&dest);
	dest_line(app, &dest);

	lines[n++] = wf_msg(MSG_JOB_VERB_MOVE);
	lines[n++] = l1;
	lines[n++] = l2;
	if (s->hidden_excluded) {
		(void)wf_msgf(l3, sizeof l3, MSG_XFER_HIDDEN_EXCLUDED,
		        &WF_ARG_N(s->hidden_excluded), 1);
		lines[n++] = l3;
	}
	lines[n++] = wf_msg(MSG_XFER_DESTINATION);
	lines[n++] = wf_buf_cstr(&dest);
	st = wf_app_confirm_begin(app, lines, n, yes_proceed, NULL, NULL);
	wf_buf_free(&dest);
	if (WF_OK != st) {
		wf_app_set_error(app, st);
	}
}

static void copy_instead(WfApp *app, void *ctx)
{
	(void)ctx;
	app->slot.mode = WF_XFER_COPY;
	start_preflight(app); /* a copy has stricter checks: run them all */
}

/* Consent given for one preflight must not outlive it: a later `p` starts
 * from "stop at an existing name, plain copy". */
static void reset_pending(WfApp *app)
{
	if (app->pending_cross) {
		app->slot.mode = WF_XFER_MOVE;
	}
	app->pending_cross = false;
	app->pending_collide = WF_COLLIDE_STOP;
	app->slot.collisions_ok = false;
}

static void collide_choice(WfApp *app, WfCollide how)
{
	app->pending_collide = how;
	app->slot.collisions_ok = true;
	start_preflight(app); /* every other check runs again */
}

static void collide_overwrite(WfApp *app, void *ctx)
{
	(void)ctx;
	collide_choice(app, WF_COLLIDE_OVERWRITE);
}

static void collide_keep_both(WfApp *app, void *ctx)
{
	(void)ctx;
	collide_choice(app, WF_COLLIDE_RENAME);
}

static void collide_skip(WfApp *app, void *ctx)
{
	(void)ctx;
	collide_choice(app, WF_COLLIDE_SKIP);
}

static void offer_collision(WfApp *app)
{
	const WfPreflightReport *r = &app->last_report;
	char l1[WF_CONFIRM_LINE_BYTES];
	WfBuf name;
	const char *lines[7];
	WfStatus st;

	wf_buf_init(&name);
	wf_sanitize_utf16(r->bad_name, wcslen(r->bad_name), &name);
	{
		char shown[64];

		wf_utf8_elide(shown, sizeof shown, wf_buf_cstr(&name), 46);
		(void)wf_msgf(l1, sizeof l1, MSG_XFER_ALREADY_THERE,
		        &WF_ARG_S(shown), 1);
	}
	lines[0] = verb_of(app->slot.mode);
	lines[1] = l1;
	lines[2] = wf_msg(MSG_XFER_COLLIDE_1);
	lines[3] = wf_msg(MSG_XFER_COLLIDE_2);
	lines[4] = wf_msg(MSG_XFER_COLLIDE_3);
	lines[5] = wf_msg(MSG_XFER_COLLIDE_4);
	lines[6] = wf_msg(MSG_XFER_COLLIDE_5);
	/* one choice for every name that is already there in this job */
	st = wf_app_confirm_begin_alt2(app, lines, 7, 's', 'k', 'o',
	        wf_msg(MSG_XFER_COLLIDE_KEYS), collide_skip, collide_keep_both,
	        collide_overwrite, NULL);
	wf_buf_free(&name);
	if (WF_OK != st) {
		wf_app_set_error(app, st);
	}
}

/* Move across volumes: copied and verified first (so it is checked as a
 * copy), then the originals go to the Recycle Bin. */
static void cross_move(WfApp *app, void *ctx)
{
	(void)ctx;
	app->pending_cross = true;
	app->slot.mode = WF_XFER_COPY;
	start_preflight(app);
}

static void offer_cross_move(WfApp *app)
{
	const char *const lines[] = {wf_msg(MSG_JOB_VERB_MOVE),
	        wf_msg(MSG_XFER_CROSS_1), wf_msg(MSG_XFER_CROSS_2),
	        wf_msg(MSG_XFER_CROSS_3)};
	WfStatus st = wf_app_confirm_begin_alt(app, lines, 4, 'm', 'c',
	        wf_msg(MSG_XFER_CROSS_KEYS), cross_move, copy_instead, NULL);

	if (WF_OK != st) {
		wf_app_set_error(app, st);
	}
}

static void offer_copy_instead(WfApp *app)
{
	const char *const lines[] = {wf_msg(MSG_JOB_VERB_MOVE),
	        wf_msg(MSG_XFER_INSTEAD_1), wf_msg(MSG_XFER_INSTEAD_2),
	        wf_msg(MSG_XFER_INSTEAD_3)};

	WfStatus st = wf_app_confirm_begin_ex(app, lines, 4, 'c',
	        wf_msg(MSG_XFER_INSTEAD_KEYS), copy_instead, NULL, NULL);

	if (WF_OK != st) {
		wf_app_set_error(app, st);
	}
}

void wf_app_on_preflight(
        WfApp *app, uint32_t gen, WfStatus st, const WfPreflightReport *rep)
{
	if (!rep || gen != app->xfer_gen || WF_XB_PREFLIGHT != app->xfer_busy) {
		return;
	}
	app->xfer_busy = WF_XB_IDLE;
	if (WF_OK != st) {
		char m[160];

		(void)wf_msgf(m, sizeof m, MSG_XFER_PREFLIGHT_FAILED,
		        &WF_ARG_S(wf_status_name(st)), 1);
		wf_app_set_msg(app, m);
		reset_pending(app);
		return;
	}
	app->last_report = *rep;

	switch (rep->reason) {
	case WF_PF_OK:
		if (WF_XFER_COPY == app->slot.mode && rep->cloud_files > 0) {
			offer_hydrate(app);
		} else if (WF_XFER_MOVE == app->slot.mode &&
		        app->slot.count >= WF_LARGE_MOVE_ITEMS) {
			offer_big_move(app);
		} else {
			proceed(app, false);
		}
		break;
	case WF_PF_CROSS_VOLUME:
		if (app->copy.submit) {
			offer_cross_move(app);
		} else {
			offer_copy_instead(app);
		}
		break;
	case WF_PF_COLLISION:
		if (WF_XFER_COPY == app->slot.mode && app->copy.submit &&
		        !app->slot.collisions_ok) {
			offer_collision(app);
			break;
		}
		reset_pending(app);
		report_refusal(app, rep);
		break;
	default:
		reset_pending(app);
		report_refusal(app, rep);
		break;
	}
}

/* The local slot only; a WSL mark is not touched. */
static bool drop_local_slot(WfApp *app)
{
	if (WF_XFER_NONE == app->slot.mode) {
		return false;
	}
	app->pending_cross = false;
	app->pending_collide = WF_COLLIDE_STOP;
	wf_slot_free(&app->slot);
	return true;
}

void wf_app_cmd_stage(WfApp *app, bool move)
{
	if (!move && wf_vpath_is(app->cwd)) {
		wf_app_extract_pick(app); /* a copy out of the archive */
		return;
	}
	if (wf_app_archive_guard(app)) {
		return;
	}
	if (wf_wsl_unc_is(app->cwd)) {
		/* no file identity there: the pick is kept as plain names. The new
		 * mark replaces the old one only once it exists. */
		if (wf_app_wsl_note_clip(app, move)) {
			char text[96];

			drop_local_slot(app);
			(void)wf_msgf(text, sizeof text, MSG_WSL_PICKED,
			        (WfMsgArg[]){WF_ARG_S(wf_msg(
			                             move ? MSG_WSL_VERB_MOVE
			                                  : MSG_WSL_VERB_COPY)),
			                WF_ARG_N(app->wclip.n)},
			        2);
			wf_app_set_msg(app, text);
		}
		return;
	}
	start_capture(app, move ? WF_XFER_MOVE : WF_XFER_COPY);
	(void)wf_app_wsl_note_clip(app, move);
}

void wf_app_cmd_paste(WfApp *app)
{
	if (wf_app_extract_paste(app)) {
		return;
	}
	if (wf_app_archive_guard(app) || wf_app_wsl_paste(app)) {
		return;
	}
	if (WF_XFER_COPY == app->slot.mode && !app->copy.submit &&
	        (app->copy_off_set || app->recovery.unknown)) {
		wf_app_set_msg(app,
		        wf_msg(app->copy_off_set ? app->copy_off
		                                 : MSG_COPY_SAFETY_BLOCKED));
		return;
	}
	reset_pending(app);
	start_preflight(app);
}

bool wf_app_xfer_drop_slot(WfApp *app)
{
	wf_app_wsl_clear_clip(app);
	return drop_local_slot(app);
}

void wf_app_xfer_status(const WfApp *app, WfBuf *s)
{
	wf_app_wsl_status(app, s);
	if (WF_XB_CAPTURE == app->xfer_busy) {
		wf_buf_puts(s, wf_msg(MSG_XFER_STATUS_READING));
	} else if (WF_XB_PREFLIGHT == app->xfer_busy) {
		wf_buf_puts(s, wf_msg(MSG_XFER_STATUS_CHECKING));
	}
	if (WF_XFER_NONE != app->slot.mode) {
		char text[96];

		wf_buf_puts(s, " | ");
		(void)wf_msgf(text, sizeof text,
		        wf_msg_count(MSG_XFER_STATUS_SLOT_ONE,
		                MSG_XFER_STATUS_SLOT_OTHER,
		                (long long)app->slot.count),
		        (WfMsgArg[]){WF_ARG_S(verb_of(app->slot.mode)),
		                WF_ARG_N(app->slot.count)},
		        2);
		wf_buf_puts(s, text);
	}
}

void wf_app_xfer_destroy(WfApp *app)
{
	wf_app_extract_drop(app);
	wf_app_wsl_destroy(app);
	move_clear(app);
	wf_slot_free(&app->slot);
}
