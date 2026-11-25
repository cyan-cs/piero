/*
 * Bulk rename (M; DR-DRAFT-022, UI).
 *
 * Four screens in one mode, one after the other:
 *   pattern  the fields of a WfBulkPattern, checked as you type
 *   preview  every row as before -> after with the planner's verdict; the plan
 *            and the preflight are made here (reads only)
 *   running  on the batch worker; Esc asks it to stop and it rolls back
 *   result   what happened, in words that are true for the result
 * A run is possible only when the preview has no refused row, something to do,
 * and one question has been answered with y. The engine is behind a port
 * (wf_batchport.h): a build without it, or without the edit port that the
 * operation forms stop at, says so and changes nothing.
 *
 * A completed batch is recorded for Undo (DR-DRAFT-029). The journal
 * also records interrupted runs for recovery. Existing Undo entries are
 * left to their own identity checks.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../base/wf_mem.h"
#include "../fs/wf_loc.h"
#include "../path/wf_path.h"
#include "../text/wf_text.h"
#include "wf_app_priv.h"

#define SAMPLE_ROWS 6
#define VERDICT_MAX 62 /* cells of the right-hand column of the preview */

void wf_app_set_batch(WfApp *app, WfBatchPort port)
{
	app->batch.port = port;
}

/* The operation forms stop at the release gate when there is no edit port
 * (name_submit in wf_app_input.c). A batch changes names, so it stops at the
 * same place, at the start and again before anything is handed to the worker. */
static bool gate_open(const WfApp *app)
{
	return NULL != app->edit.submit;
}

static void drop_job(WfApp *app)
{
	WfBatch *b = &app->batch;

	if (b->job && b->port.job_free) {
		b->port.job_free(b->port.ctx, b->job);
	}
	b->job = NULL;
}

static void free_items(WfBatch *b)
{
	for (size_t i = 0; b->item && i < b->n; i++) {
		free(b->item[i].name);
		free(b->item[i].path);
		free(b->item[i].to);
	}
	free(b->item);
	free(b->dir);
	b->item = NULL;
	b->dir = NULL;
	b->n = 0;
}

/* Ends the screens. A run in flight is not ended here: its answer is. */
static void reset(WfApp *app)
{
	WfBatch *b = &app->batch;

	drop_job(app);
	free_items(b);
	memset(&b->pre, 0, sizeof b->pre);
	b->stage = WF_BS_IDLE;
	b->top = b->n_renames = b->n_bad = 0;
	b->result_n = 0;
	b->undo_uid = 0;
}

void wf_app_batch_destroy(WfApp *app)
{
	reset(app);
}

static void close_screen(WfApp *app)
{
	reset(app);
	app->mode = &WF_MODE_NORMAL;
}

bool wf_app_batch_busy(const WfApp *app)
{
	return app->batch.busy;
}

static void fields_default(WfBatch *b)
{
	memset(b->f_text, 0, sizeof b->f_text);
	wcscpy_s(b->f_text[WF_BF_TEMPLATE], WF_BF_TEXT_MAX, L"{name}");
	wcscpy_s(b->f_text[WF_BF_START], WF_BF_TEXT_MAX, L"1");
	wcscpy_s(b->f_text[WF_BF_STEP], WF_BF_TEXT_MAX, L"1");
	b->stem_case = b->ext_case = WF_BULK_CASE_KEEP;
	b->field = WF_BF_TEMPLATE;
}

static bool read_number(const wchar_t *s, int64_t *out)
{
	int64_t v = 0;
	bool neg = false;
	size_t i = 0, digits = 0;

	if (L'-' == s[0]) {
		neg = true;
		i = 1;
	}
	for (; s[i]; i++) {
		if (s[i] < L'0' || s[i] > L'9' || ++digits > 18) {
			return false;
		}
		v = v * 10 + (s[i] - L'0');
	}
	if (!digits) {
		return false;
	}
	*out = neg ? -v : v;
	return true;
}

/* The pattern the fields say. 0, or the field that is wrong (1 template,
 * 2 start, 3 step) with the position in the template. */
static int make_pattern(const WfBatch *b, WfBulkPattern *p, size_t *pos)
{
	memset(p, 0, sizeof *p);
	*pos = 0;
	p->tmpl = b->f_text[WF_BF_TEMPLATE];
	p->find = b->f_text[WF_BF_FIND];
	p->replace = b->f_text[WF_BF_REPLACE];
	p->stem_case = (WfBulkCase)b->stem_case;
	p->ext_case = (WfBulkCase)b->ext_case;
	if (!read_number(b->f_text[WF_BF_START], &p->start)) {
		return 2;
	}
	if (!read_number(b->f_text[WF_BF_STEP], &p->step)) {
		return 3;
	}
	if (WF_OK != wf_bulk_check(p, pos)) {
		return 1;
	}
	return 0;
}

static void pattern_refresh(WfBatch *b)
{
	WfBulkPattern p;

	b->pat_err = make_pattern(b, &p, &b->pat_pos);
}

static bool scalar_valid(uint32_t cp)
{
	return cp >= 32 && 127 != cp && cp <= 0x10FFFF &&
	        !(cp >= 0xD800 && cp <= 0xDFFF);
}

static bool field_is_text(int field)
{
	return WF_BF_TEMPLATE == field || WF_BF_FIND == field ||
	        WF_BF_REPLACE == field;
}

static bool field_is_number(int field)
{
	return WF_BF_START == field || WF_BF_STEP == field;
}

static void field_append(WfBatch *b, uint32_t cp)
{
	wchar_t *t = b->f_text[b->field];
	size_t n = wcslen(t), need = cp >= 0x10000 ? 2 : 1;
	size_t cap = field_is_number(b->field) ? WF_BF_NUM_MAX : WF_BF_TEXT_MAX;

	if (!scalar_valid(cp) || n + need >= cap) {
		return;
	}
	if (field_is_number(b->field) &&
	        !((cp >= '0' && cp <= '9') || ('-' == cp && 0 == n))) {
		return;
	}
	if (2 == need) {
		cp -= 0x10000;
		t[n++] = (wchar_t)(0xD800 + (cp >> 10));
		t[n++] = (wchar_t)(0xDC00 + (cp & 0x3FF));
	} else {
		t[n++] = (wchar_t)cp;
	}
	t[n] = L'\0';
}

static void field_backspace(WfBatch *b)
{
	wchar_t *t = b->f_text[b->field];
	size_t n = wcslen(t);

	if (0 == n) {
		return;
	}
	n--;
	if (n > 0 && (uint16_t)t[n] >= 0xDC00 && (uint16_t)t[n] <= 0xDFFF) {
		n--; /* the whole pair */
	}
	t[n] = L'\0';
}

static int *case_of(WfBatch *b)
{
	return WF_BF_STEM_CASE == b->field ? &b->stem_case : &b->ext_case;
}

static void case_step(WfBatch *b, int by)
{
	int *c = case_of(b);

	*c = (*c + by + 3) % 3;
}

static bool listing_stale(const WfApp *app)
{
	const WfBatch *b = &app->batch;

	return app->want_path || b->nav_gen != app->nav_gen ||
	        b->cwd_version != app->cwd_version || !app->cur.dir ||
	        !app->cwd || !b->dir || wcscmp(b->dir, app->cwd) ||
	        !wf_fileid_equal(&b->dir_id, &app->cur.dir->identity);
}

static const WfBatchPlan *plan_of(const WfApp *app)
{
	const WfBatch *b = &app->batch;

	if (!b->job || !b->port.plan ||
	        (WF_BX_PRE_READY != b->pre.pre &&
	                WF_BX_PRE_PLAN != b->pre.pre)) {
		return NULL;
	}
	return b->port.plan(b->port.ctx, b->job);
}

/* Whether the preview on screen may be run: a job that is ready, no row
 * refused, something to do. Anything else changes nothing. */
static bool executable(const WfApp *app)
{
	const WfBatch *b = &app->batch;
	const WfBatchPlan *pl;

	if (!b->job || !b->port.ready || !b->port.ready(b->port.ctx, b->job) ||
	        WF_BX_PRE_READY != b->pre.pre || 0 != b->n_bad) {
		return false;
	}
	pl = plan_of(app);
	return pl && 0 == pl->n_refused && pl->n_steps > 0;
}

/* Makes the new names and the plan for the items held. False (and the pattern
 * stays on screen with a word) when a name cannot be made at all. */
static bool build_preview(WfApp *app)
{
	WfBatch *b = &app->batch;
	WfBulkPattern p;
	size_t pos;
	WfBxSource *src;

	drop_job(app);
	memset(&b->pre, 0, sizeof b->pre);
	b->n_renames = b->n_bad = 0;
	if (make_pattern(b, &p, &pos)) {
		return false;
	}
	for (size_t i = 0; i < b->n; i++) {
		free(b->item[i].to);
		b->item[i].to = NULL;
		if (WF_OK !=
		        wf_bulk_apply(&p, b->item[i].name, b->item[i].is_dir, i,
		                &b->item[i].to)) {
			b->item[i].to = NULL;
			if (0 == b->n_bad) {
				char text[96];
				WfBuf shown;

				wf_buf_init(&shown);
				if (WF_OK !=
				        wf_sanitize_utf16(b->item[i].name,
				                wcslen(b->item[i].name),
				                &shown)) {
					wf_buf_puts(&shown, "?");
				}
				(void)wf_msgf(text, sizeof text,
				        MSG_BATCH_RANGE_ROW,
				        &WF_ARG_S(wf_buf_cstr(&shown)), 1);
				wf_buf_free(&shown);
				wf_app_set_msg(app, text);
			}
			b->n_bad++;
		}
	}
	if (b->n_bad) {
		return false;
	}
	src = wf_malloc_array(b->n, sizeof *src);
	if (!src) {
		wf_app_set_error(app, WF_E_NOMEM);
		return false;
	}
	for (size_t i = 0; i < b->n; i++) {
		src[i].path = b->item[i].path;
		src[i].expected = b->item[i].id;
		src[i].to_name = b->item[i].to;
	}
	b->job = b->port.prepare(b->port.ctx, src, b->n, &b->pre);
	free(src);
	b->prepares++;
	/* what the plan was made from: the listing as it is now */
	b->nav_gen = app->nav_gen;
	b->cwd_version = app->cwd_version;
	if (app->cur.dir) {
		b->dir_id = app->cur.dir->identity;
	}
	{
		const WfBatchPlan *pl = plan_of(app);

		if (pl) {
			b->n_renames = b->n - pl->n_skipped - pl->n_refused;
		}
	}
	b->top = 0;
	b->stage = WF_BS_PREVIEW;
	app->msg[0] = '\0';
	return true;
}

static void start_error(WfApp *app, WfMsg msg)
{
	wf_app_set_msg(app, wf_msg(msg));
	app->mode = &WF_MODE_BATCH;
}

static void batch_yes(WfApp *app, void *ctx)
{
	WfBatch *b = &app->batch;
	WfStatus st;

	(void)ctx;
	if (WF_BS_PREVIEW != b->stage) {
		return;
	}
	if (!gate_open(app)) {
		close_screen(app);
		wf_app_set_msg(app, wf_msg(MSG_BATCH_GATED));
		return;
	}
	if (listing_stale(app)) {
		/* the folder changed between the preview and the answer: the
		 * plan is made again and shown again; nothing runs on the old */
		if (!app->cwd || !b->dir || wcscmp(b->dir, app->cwd)) {
			close_screen(app);
			wf_app_set_msg(app, wf_msg(MSG_BATCH_STALE_CANCEL));
			return;
		}
		app->mode = &WF_MODE_BATCH;
		if (build_preview(app)) {
			wf_app_set_msg(app, wf_msg(MSG_BATCH_STALE));
		}
		return;
	}
	if (!executable(app)) {
		start_error(app, MSG_BATCH_PV_REFUSED_SHORT);
		return;
	}
	if (!b->port.run) {
		start_error(app, MSG_BATCH_OFF);
		return;
	}
	if (0 == ++b->gen) {
		b->gen = 1;
	}
	st = b->port.run(b->port.ctx, b->gen, b->job);
	if (WF_OK != st) {
		start_error(app, MSG_BATCH_BUSY);
		return;
	}
	b->job = NULL; /* the worker has it */
	b->busy = true;
	b->recover = false;
	b->cancelling = false;
	b->progress = b->shown_progress = 0;
	b->stage = WF_BS_RUNNING;
	app->msg[0] = '\0';
	app->mode = &WF_MODE_BATCH;
}

static void ask_to_run(WfApp *app, const WfEvent *ev)
{
	WfBatch *b = &app->batch;
	const WfBatchPlan *pl = plan_of(app);
	const char *lines[6];
	char l1[WF_CONFIRM_LINE_BYTES], l2[WF_CONFIRM_LINE_BYTES];
	WfBuf dir;
	size_t skipped = pl ? pl->n_skipped : 0;
	WfStatus st;

	wf_buf_init(&dir);
	if (WF_OK != wf_sanitize_utf16(b->dir, wcslen(b->dir), &dir)) {
		wf_buf_puts(&dir, "?");
	}
	(void)wf_msgf(l1, sizeof l1, MSG_BATCH_Q_1,
	        (WfMsgArg[]){WF_ARG_N(b->n_renames), WF_ARG_N(skipped)}, 2);
	(void)wf_msgf(l2, sizeof l2, MSG_BATCH_Q_DIR,
	        &WF_ARG_S(wf_buf_cstr(&dir)), 1);
	lines[0] = wf_msg(MSG_BATCH_Q_TITLE);
	lines[1] = l1;
	lines[2] = l2;
	lines[3] = wf_msg(MSG_BATCH_Q_2);
	lines[4] = wf_msg(MSG_BATCH_Q_3);
	lines[5] = wf_msg(MSG_BATCH_Q_4);
	st = wf_app_confirm_begin(app, lines, 6, batch_yes, NULL, ev);
	wf_buf_free(&dir);
	if (WF_OK != st) {
		wf_app_set_error(app, st);
	}
}

static void line(WfBatch *b, WfMsg id, const WfMsgArg *args, size_t n)
{
	if (b->result_n < (int)(sizeof b->result / sizeof b->result[0])) {
		(void)wf_msgf(b->result[b->result_n++], sizeof b->result[0], id,
		        args, n);
	}
}

static void compose_run(WfBatch *b, const WfBxDone *d)
{
	const WfBxReport *r = &d->rep;

	b->result_n = 0;
	switch (r->result) {
	case WF_BX_SUCCESS:
		line(b, b->undo_uid ? MSG_BATCH_UNDO_DONE : MSG_BATCH_R_DONE,
		        &WF_ARG_N(b->n_renames), 1);
		break;
	case WF_BX_ROLLED_BACK:
		line(b,
		        b->cancelling ? MSG_BATCH_R_CANCELLED
		                      : MSG_BATCH_R_ROLLED,
		        (WfMsgArg[]){WF_ARG_N(r->steps_done),
		                WF_ARG_N(r->steps_total)},
		        2);
		if (!b->cancelling) {
			line(b, MSG_BATCH_R_ERROR,
			        (WfMsgArg[]){WF_ARG_N(r->failed_step + 1),
			                WF_ARG_N(r->error)},
			        2);
		}
		break;
	case WF_BX_RECOVERY_REQUIRED:
		line(b, MSG_BATCH_R_INCOMPLETE,
		        (WfMsgArg[]){WF_ARG_N(r->steps_done),
		                WF_ARG_N(r->rolled_back),
		                WF_ARG_N(r->not_restored)},
		        3);
		line(b, MSG_BATCH_R_ERROR,
		        (WfMsgArg[]){WF_ARG_N(r->failed_step + 1),
		                WF_ARG_N(r->error)},
		        2);
		line(b, MSG_BATCH_R_KEPT, NULL, 0);
		break;
	default:
		line(b, MSG_BATCH_R_FAILED_BEFORE, &WF_ARG_N(r->error), 1);
		break;
	}
}

static void compose_recover(WfBatch *b, const WfBxDone *d)
{
	b->result_n = 0;
	if (0 == d->rec_total) {
		line(b, MSG_BATCH_RR_NONE, NULL, 0);
	} else if (d->rec_open) {
		line(b, MSG_BATCH_RR_OPEN,
		        (WfMsgArg[]){WF_ARG_N(d->rec_open),
		                WF_ARG_N(d->rep.not_restored)},
		        2);
		line(b, MSG_BATCH_R_KEPT, NULL, 0);
	} else {
		line(b, MSG_BATCH_RR_OK,
		        (WfMsgArg[]){WF_ARG_N(d->rec_total),
		                WF_ARG_N(d->rec_done),
		                WF_ARG_N(d->rec_rolled_back)},
		        3);
	}
}

/* The first item whose name changed, as the new name; NULL when none did. */
static const wchar_t *first_renamed(const WfBatch *b)
{
	for (size_t i = 0; i < b->n; i++) {
		if (b->item[i].to && wcscmp(b->item[i].to, b->item[i].name)) {
			return b->item[i].to;
		}
	}
	return NULL;
}

/* What a finished batch needs to be taken back: the items whose name changed,
 * by identity, with the name they have now and the one they had. Returns the
 * uid of the Undo entry; 0 when memory did not allow one (then the row of the
 * history says it cannot be undone). */
static uint32_t keep_for_undo(WfApp *app)
{
	const WfBatch *b = &app->batch;
	WfUndoEntry u;
	size_t n = 0;

	for (size_t i = 0; i < b->n; i++) {
		n += b->item[i].to && wcscmp(b->item[i].to, b->item[i].name)
		        ? 1u
		        : 0u;
	}
	if (!n || !b->dir) {
		return 0;
	}
	memset(&u, 0, sizeof u);
	u.kind = WF_UNDO_BATCH;
	u.folder = wf_path_dup(b->dir);
	u.folder_id = b->dir_id;
	u.item = calloc(n, sizeof *u.item);
	if (!u.folder || !u.item) {
		wf_app_undo_free(&u);
		return 0;
	}
	for (size_t i = 0; i < b->n; i++) {
		if (!b->item[i].to || !wcscmp(b->item[i].to, b->item[i].name)) {
			continue;
		}
		u.item[u.n].name = wf_path_dup(b->item[i].to);
		u.item[u.n].old_name = wf_path_dup(b->item[i].name);
		u.item[u.n].id = b->item[i].id;
		u.item[u.n].is_dir = b->item[i].is_dir;
		u.n++; /* counted first: a failure below frees these two too */
		if (!u.item[u.n - 1].name || !u.item[u.n - 1].old_name) {
			wf_app_undo_free(&u);
			return 0;
		}
	}
	return wf_app_undo_push(app, &u);
}

/* The row of the history for a run that ended (not for an Undo, which marks the
 * row it took back, and not for a run that never began). */
static void history_row(WfApp *app, const WfBxDone *d)
{
	const WfBatch *b = &app->batch;
	char head[128], where[80];
	WfBuf dir;

	if (WF_BX_FAILED_BEFORE == d->rep.result || !b->dir) {
		return;
	}
	wf_buf_init(&dir);
	(void)wf_sanitize_utf16(b->dir, wcslen(b->dir), &dir);
	wf_utf8_elide(where, sizeof where, wf_buf_cstr(&dir), 40);
	wf_buf_free(&dir);
	switch (d->rep.result) {
	case WF_BX_SUCCESS:
		(void)wf_msgf(head, sizeof head, MSG_HIST_BATCH,
		        (WfMsgArg[]){WF_ARG_N(b->n_renames), WF_ARG_S(where)},
		        2);
		wf_app_hist_add(
		        app, head, keep_for_undo(app), MSG_HIST_WHY_NOT_KEPT);
		break;
	case WF_BX_ROLLED_BACK:
		(void)wf_msgf(head, sizeof head, MSG_HIST_BATCH_BACK,
		        &WF_ARG_S(where), 1);
		wf_app_hist_add(app, head, 0, MSG_HIST_WHY_NOTHING);
		break;
	default:
		(void)wf_msgf(head, sizeof head, MSG_HIST_BATCH_OPEN,
		        &WF_ARG_S(where), 1);
		wf_app_hist_add(app, head, 0, MSG_HIST_WHY_INCOMPLETE);
		break;
	}
}

bool wf_app_batch_undo_begin(WfApp *app, const WfUndoEntry *u)
{
	WfBatch *b = &app->batch;
	WfBxSource *src;
	const WfBatchPlan *pl;
	bool ok = true;

	if (!gate_open(app) || !b->port.prepare || !b->port.run) {
		wf_app_set_msg(app, wf_msg(MSG_BATCH_GATED));
		return false;
	}
	if (b->busy || WF_BS_IDLE != b->stage || WF_UNDO_BATCH != u->kind ||
	        !u->n || !u->item[0].old_name) {
		wf_app_set_msg(app, wf_msg(MSG_OPS_BUSY));
		return false;
	}
	/* the items as they are now: the names the batch gave, to go back to the
	 * names they had */
	reset(app);
	b->dir = wf_path_dup(u->folder);
	b->dir_id = u->folder_id;
	b->item = calloc(u->n, sizeof *b->item);
	src = calloc(u->n, sizeof *src);
	b->n = u->n; /* so that reset() frees every name made below */
	ok = b->dir && b->item && src;
	for (size_t i = 0; ok && i < u->n; i++) {
		b->item[i].name = wf_path_dup(u->item[i].name);
		b->item[i].to = wf_path_dup(u->item[i].old_name);
		b->item[i].id = u->item[i].id;
		b->item[i].is_dir = u->item[i].is_dir;
		ok = b->item[i].name && b->item[i].to &&
		        WF_OK ==
		                wf_path_join(u->folder, u->item[i].name,
		                        &b->item[i].path);
		if (ok) {
			src[i].path = b->item[i].path;
			src[i].expected = b->item[i].id;
			src[i].to_name = b->item[i].to;
		}
	}
	if (!ok) {
		free(src);
		if (!b->item) {
			b->n = 0; /* the array itself was not made */
		}
		reset(app);
		wf_app_set_msg(app, wf_msg(MSG_JOB_NOMEM));
		return false;
	}
	b->job = b->port.prepare(b->port.ctx, src, b->n, &b->pre);
	free(src);
	b->n_renames = b->n;
	pl = plan_of(app);
	/* all or nothing, and only when every item is still the file that was
	 * renamed and every old name is free: anything else changes nothing and
	 * the entry stays */
	if (!executable(app) || !pl || pl->n_skipped) {
		reset(app);
		wf_app_set_msg(app, wf_msg(MSG_BATCH_UNDO_REFUSED));
		return false;
	}
	if (0 == ++b->gen) {
		b->gen = 1;
	}
	if (WF_OK != b->port.run(b->port.ctx, b->gen, b->job)) {
		reset(app);
		wf_app_set_msg(app, wf_msg(MSG_BATCH_BUSY));
		return false;
	}
	b->job = NULL; /* the worker has it */
	b->undo_uid = u->uid;
	b->busy = true;
	b->recover = false;
	b->cancelling = false;
	b->progress = b->shown_progress = 0;
	b->stage = WF_BS_RUNNING;
	app->msg[0] = '\0';
	app->mode = &WF_MODE_BATCH;
	return true;
}

void wf_app_on_batch(WfApp *app, const WfBxDone *d)
{
	WfBatch *b = &app->batch;
	bool ours;
	const wchar_t *select = NULL;

	if (!app || !d || !b->busy || d->gen != b->gen) {
		return; /* not the request that is out: it carries no authority */
	}
	b->busy = false;
	b->done = *d;
	if (d->recover) {
		compose_recover(b, d);
	} else {
		compose_run(b, d);
	}
	if (!d->recover && b->undo_uid) {
		/* an Undo: only a batch made whole retires the entry; taken back
		 * or incomplete, it stays (a retry is refused by the identities
		 * and the names, never guessed) */
		if (WF_BX_SUCCESS == d->rep.result) {
			wf_app_undo_retire(app, b->undo_uid);
		}
	} else if (!d->recover) {
		history_row(app, d);
	}
	ours = !d->recover && b->dir && app->cwd && !wcscmp(b->dir, app->cwd);
	if (ours && WF_BX_SUCCESS == d->rep.result) {
		select = first_renamed(b);
		wf_sel_clear(&app->sel, app->cwd);
	}
	/* whatever happened, the listing is read again; after a success the
	 * cursor is on the first renamed item, by its new name */
	wf_app_dircache_invalidate(app);
	if (ours && select) {
		(void)wf_app_navigate(app, app->cwd, select, SIZE_MAX);
	} else {
		wf_app_reload(app);
	}
	if (&WF_MODE_BATCH == app->mode) {
		b->stage = WF_BS_RESULT;
		wf_app_set_msg(app, b->result[0]);
	} else {
		wf_app_set_msg(app, b->result[0]);
		reset(app);
	}
	app->msg_keep_until = app->now_ms + WF_RECEIPT_KEEP_MS;
}

static void recover_yes(WfApp *app, void *ctx)
{
	WfBatch *b = &app->batch;

	(void)ctx;
	if (!gate_open(app) || !b->port.recover) {
		wf_app_set_msg(app, wf_msg(MSG_BATCH_GATED));
		return;
	}
	if (0 == ++b->gen) {
		b->gen = 1;
	}
	if (WF_OK != b->port.recover(b->port.ctx, b->gen)) {
		wf_app_set_msg(app, wf_msg(MSG_BATCH_BUSY));
		return;
	}
	reset(app);
	b->busy = true;
	b->recover = true;
	b->cancelling = false;
	b->progress = b->shown_progress = 0;
	b->stage = WF_BS_RUNNING;
	app->mode = &WF_MODE_BATCH;
}

/* At the start: are there journals that need a look? Only counts; asks later
 * (wf_app_batch_tick), when the screen is free. Never recovers by itself. */
void wf_app_batch_startup(WfApp *app)
{
	WfBatch *b = &app->batch;

	b->unfinished = 0;
	b->ask_pending = false;
	if (!b->port.unfinished || !b->port.recover) {
		return;
	}
	b->unfinished = b->port.unfinished(b->port.ctx);
	b->ask_pending = b->unfinished > 0;
}

static void ask_to_recover(WfApp *app)
{
	WfBatch *b = &app->batch;
	const char *lines[5];
	char l1[WF_CONFIRM_LINE_BYTES];

	b->ask_pending = false;
	if (!gate_open(app)) {
		char text[96];

		(void)wf_msgf(text, sizeof text, MSG_BATCH_REC_GATED,
		        &WF_ARG_N(b->unfinished), 1);
		wf_app_notice(app, text);
		return;
	}
	(void)wf_msgf(
	        l1, sizeof l1, MSG_BATCH_REC_1, &WF_ARG_N(b->unfinished), 1);
	lines[0] = wf_msg(MSG_BATCH_REC_TITLE);
	lines[1] = l1;
	lines[2] = wf_msg(MSG_BATCH_REC_2);
	lines[3] = wf_msg(MSG_BATCH_REC_3);
	lines[4] = wf_msg(MSG_BATCH_REC_4);
	if (WF_OK !=
	        wf_app_confirm_begin(app, lines, 5, recover_yes, NULL, NULL)) {
		wf_app_notice(app, wf_msg(MSG_BATCH_REC_4));
	}
}

bool wf_app_batch_tick(WfApp *app)
{
	WfBatch *b = &app->batch;
	bool redraw = false;

	if (b->ask_pending && !b->busy && &WF_MODE_NORMAL == app->mode) {
		ask_to_recover(app);
		redraw = true;
	}
	if (WF_BS_IDLE != b->stage && !b->busy && &WF_MODE_BATCH != app->mode &&
	        &WF_MODE_CONFIRM != app->mode) {
		/* something else took the screen: the plan is not kept (it holds
		 * folders open) */
		reset(app);
		return true;
	}
	if (b->busy && b->port.progress) {
		size_t p = b->port.progress(b->port.ctx);

		if (p != b->shown_progress) {
			b->shown_progress = b->progress = p;
			redraw = true;
		}
	}
	return redraw;
}

uint32_t wf_app_batch_timeout(const WfApp *app)
{
	const WfBatch *b = &app->batch;

	if (b->ask_pending && &WF_MODE_NORMAL == app->mode) {
		return 0;
	}
	return b->busy ? 100u : WF_WAIT_FOREVER;
}

static void refuse_entry(WfApp *app, const wchar_t *name)
{
	char text[96];
	WfBuf shown;

	wf_buf_init(&shown);
	if (WF_OK != wf_sanitize_utf16(name, wcslen(name), &shown)) {
		wf_buf_puts(&shown, "?");
	}
	(void)wf_msgf(text, sizeof text, MSG_BATCH_BAD_ENTRY,
	        &WF_ARG_S(wf_buf_cstr(&shown)), 1);
	wf_buf_free(&shown);
	wf_app_set_msg(app, text);
}

/* Takes the items: the marked entries of this folder in the order the list
 * has them, or the entry under the cursor when none is marked. */
static bool capture(WfApp *app)
{
	WfBatch *b = &app->batch;
	const WfNameSet *marks = wf_sel_folder(&app->sel, app->cwd);
	const WfDir *d = app->cur.dir;
	const WfEntry *cur = wf_view_current(&app->cur);
	size_t n = 0, at = 0;

	if (marks && marks->n) {
		for (size_t i = 0; i < d->n; i++) {
			n += wf_nameset_has(marks, d->e[i].name);
		}
	} else if (cur) {
		n = 1;
	}
	if (0 == n) {
		wf_app_set_msg(app, wf_msg(MSG_BATCH_NONE));
		return false;
	}
	if (n > WF_BATCH_MAX_ITEMS) {
		char text[96];

		(void)wf_msgf(text, sizeof text, MSG_BATCH_TOO_MANY,
		        (WfMsgArg[]){WF_ARG_N(n), WF_ARG_N(WF_BATCH_MAX_ITEMS)},
		        2);
		wf_app_set_msg(app, text);
		return false;
	}
	for (size_t i = 0; i < d->n; i++) {
		const WfEntry *e = &d->e[i];

		if (marks && marks->n ? !wf_nameset_has(marks, e->name)
		                      : e != cur) {
			continue;
		}
		if (!(e->identity.kind == WF_ID_STRONG) ||
		        e->identity.volume != d->identity.volume ||
		        (e->flags & WF_ENT_LINK)) {
			refuse_entry(app, e->name);
			return false;
		}
	}
	reset(app);
	b->item = calloc(n, sizeof *b->item);
	b->dir = wf_path_dup(app->cwd);
	b->n = n; /* so that free_items frees what was made */
	if (!b->item || !b->dir) {
		free_items(b);
		wf_app_set_error(app, WF_E_NOMEM);
		return false;
	}
	for (size_t i = 0; i < d->n && at < n; i++) {
		const WfEntry *e = &d->e[i];

		if (marks && marks->n ? !wf_nameset_has(marks, e->name)
		                      : e != cur) {
			continue;
		}
		b->item[at].name = wf_path_dup(e->name);
		if (!b->item[at].name ||
		        WF_OK !=
		                wf_path_join(
		                        app->cwd, e->name, &b->item[at].path)) {
			free_items(b);
			wf_app_set_error(app, WF_E_NOMEM);
			return false;
		}
		b->item[at].id = e->identity;
		b->item[at].is_dir = 0 != (e->flags & WF_ENT_DIR);
		at++;
	}
	b->dir_id = d->identity;
	b->nav_gen = app->nav_gen;
	b->cwd_version = app->cwd_version;
	return true;
}

void wf_app_cmd_bulk_rename(WfApp *app)
{
	WfBatch *b = &app->batch;

	if (!b->port.prepare || !b->port.run) {
		wf_app_set_msg(app, wf_msg(MSG_BATCH_OFF));
		return;
	}
	if (!gate_open(app)) {
		wf_app_set_msg(app, wf_msg(MSG_BATCH_GATED));
		return;
	}
	if (wf_app_archive_guard(app)) {
		return;
	}
	if (!app->cwd || wf_wsl_unc_is(app->cwd)) {
		wf_app_set_msg(app, wf_msg(MSG_BATCH_NOT_HERE));
		return;
	}
	if (app->want_path || !app->cur.dir) {
		wf_app_set_msg(app, wf_msg(MSG_BATCH_LOADING));
		return;
	}
	if (WF_BS_IDLE != b->stage && !b->busy &&
	        &WF_MODE_NORMAL == app->mode) {
		reset(app); /* what a declined question left behind */
	}
	if (b->busy || app->edit_busy || app->move.active ||
	        WF_BS_IDLE != b->stage) {
		wf_app_set_msg(app, wf_msg(MSG_BATCH_BUSY));
		return;
	}
	if (!(app->cur.dir->identity.kind == WF_ID_STRONG)) {
		wf_app_set_msg(app, wf_msg(MSG_OPS_BLOCKED_IDENTITY));
		return;
	}
	if (!capture(app)) {
		return;
	}
	fields_default(b);
	pattern_refresh(b);
	b->stage = WF_BS_PATTERN;
	app->msg[0] = '\0';
	app->mode = &WF_MODE_BATCH;
}

static WfAction pattern_event(WfApp *app, const WfEvent *ev)
{
	WfBatch *b = &app->batch;

	if (WF_EV_CHAR == ev->type) {
		if (field_is_text(b->field) || field_is_number(b->field)) {
			field_append(b, ev->ch);
			pattern_refresh(b);
		} else if (' ' == ev->ch) {
			case_step(b, 1);
		} else if ('k' == ev->ch || 'K' == ev->ch) {
			*case_of(b) = WF_BULK_CASE_KEEP;
		} else if ('l' == ev->ch || 'L' == ev->ch) {
			*case_of(b) = WF_BULK_CASE_LOWER;
		} else if ('u' == ev->ch || 'U' == ev->ch) {
			*case_of(b) = WF_BULK_CASE_UPPER;
		}
		return WF_ACT_REDRAW;
	}
	if (WF_EV_KEY != ev->type) {
		return WF_ACT_NONE;
	}
	switch (ev->key) {
	case WF_KEY_TAB:
	case WF_KEY_DOWN:      b->field = (b->field + 1) % WF_BF_COUNT; break;
	case WF_KEY_SHIFT_TAB:
	case WF_KEY_UP:
		b->field = (b->field + WF_BF_COUNT - 1) % WF_BF_COUNT;
		break;
	case WF_KEY_LEFT:
		if (!field_is_text(b->field) && !field_is_number(b->field)) {
			case_step(b, -1);
		}
		break;
	case WF_KEY_RIGHT:
		if (!field_is_text(b->field) && !field_is_number(b->field)) {
			case_step(b, 1);
		}
		break;
	case WF_KEY_BACKSPACE:
		if (field_is_text(b->field) || field_is_number(b->field)) {
			field_backspace(b);
			pattern_refresh(b);
		}
		break;
	case WF_KEY_ENTER:
		if (b->pat_err) {
			return WF_ACT_REDRAW; /* the line says what is wrong */
		}
		if (!(ev->vk & WF_VK_PASTED)) {
			(void)build_preview(app);
		}
		break;
	case WF_KEY_ESC: close_screen(app); break;
	default:         return WF_ACT_NONE;
	}
	return WF_ACT_REDRAW;
}

static size_t page_rows(const WfApp *app)
{
	return app->rows > 6 ? (size_t)app->rows - 6 : 1;
}

static WfAction preview_event(WfApp *app, const WfEvent *ev)
{
	WfBatch *b = &app->batch;
	size_t page = page_rows(app), limit;

	if (WF_EV_KEY != ev->type) {
		return WF_ACT_NONE;
	}
	limit = b->n > page ? b->n - page : 0;
	switch (ev->key) {
	case WF_KEY_UP:   b->top = b->top ? b->top - 1 : 0; break;
	case WF_KEY_DOWN: b->top = b->top < limit ? b->top + 1 : limit; break;
	case WF_KEY_PGUP: b->top = b->top > page ? b->top - page : 0; break;
	case WF_KEY_PGDN:
		b->top = b->top + page < limit ? b->top + page : limit;
		break;
	case WF_KEY_HOME: b->top = 0; break;
	case WF_KEY_END:  b->top = limit; break;
	case WF_KEY_ESC:
		/* back to the fields; the plan (and the folders it holds open)
		 * is let go */
		drop_job(app);
		memset(&b->pre, 0, sizeof b->pre);
		b->stage = WF_BS_PATTERN;
		break;
	case WF_KEY_ENTER:
		if (ev->vk & WF_VK_PASTED) {
			return WF_ACT_NONE;
		}
		if (!executable(app)) {
			return WF_ACT_REDRAW; /* the screen says why */
		}
		ask_to_run(app, ev);
		break;
	default: return WF_ACT_NONE;
	}
	return WF_ACT_REDRAW;
}

static WfAction running_event(WfApp *app, const WfEvent *ev)
{
	WfBatch *b = &app->batch;

	if (WF_EV_KEY == ev->type && WF_KEY_ESC == ev->key && b->busy &&
	        !b->recover && !b->cancelling) {
		b->cancelling = true;
		if (b->port.cancel) {
			b->port.cancel(b->port.ctx);
		}
		return WF_ACT_REDRAW;
	}
	return WF_EV_RESIZE == ev->type ? WF_ACT_REDRAW : WF_ACT_NONE;
}

static WfAction batch_event(WfApp *app, const WfEvent *ev)
{
	WfBatch *b = &app->batch;

	if (WF_EV_RELEASE == ev->type) {
		return WF_ACT_NONE;
	}
	if (WF_EV_RESIZE == ev->type) {
		return WF_ACT_REDRAW;
	}
	switch (b->stage) {
	case WF_BS_PATTERN: return pattern_event(app, ev);
	case WF_BS_PREVIEW: return preview_event(app, ev);
	case WF_BS_RUNNING: return running_event(app, ev);
	case WF_BS_RESULT:
		if (WF_EV_KEY == ev->type &&
		        (WF_KEY_ESC == ev->key || WF_KEY_ENTER == ev->key)) {
			close_screen(app);
			return WF_ACT_REDRAW;
		}
		if (WF_EV_CHAR == ev->type && ' ' == ev->ch) {
			close_screen(app);
			return WF_ACT_REDRAW;
		}
		return WF_ACT_NONE;
	default: break;
	}
	app->mode = &WF_MODE_NORMAL;
	return WF_ACT_REDRAW;
}

/* A wide string, made safe to show, clipped to `w` cells. Returns the cells used. */
static int put_w(
        WfFrame *f, int x, int y, int w, const wchar_t *s, const WfStyle *st)
{
	WfBuf buf;
	int used = 0;

	if (w <= 0) {
		return 0;
	}
	wf_buf_init(&buf);
	if (WF_OK == wf_sanitize_utf16(s, wcslen(s), &buf)) {
		used = wf_app_put_fit(f, x, y, wf_buf_cstr(&buf), st, w);
	}
	wf_buf_free(&buf);
	return used;
}

static void draw_footer(const WfApp *app, WfFrame *f, WfMsg hint)
{
	const WfThemeStyles *th = wf_theme_styles();

	if (app->msg[0] && f->rows > 3) {
		wf_app_put_fit(
		        f, 1, f->rows - 2, app->msg, &th->warning, f->cols - 2);
	}
	wf_app_put_fit(f, 1, f->rows - 1, wf_msg(hint), &th->dim, f->cols - 2);
}

static void draw_title(const WfApp *app, WfFrame *f, const char *text)
{
	(void)app;
	wf_frame_fill(f, 0, 0, f->cols, 1, ' ', &wf_app_style_bar);
	wf_app_put_fit(f, 1, 0, text, &wf_app_style_bar, f->cols - 2);
}

static const WfMsg field_label[WF_BF_COUNT] = {MSG_BATCH_F_TEMPLATE,
        MSG_BATCH_F_FIND, MSG_BATCH_F_REPLACE, MSG_BATCH_F_START,
        MSG_BATCH_F_STEP, MSG_BATCH_F_STEM_CASE, MSG_BATCH_F_EXT_CASE};

static const char *case_text(int c)
{
	return wf_msg(WF_BULK_CASE_LOWER == c     ? MSG_BATCH_CASE_LOWER
	                : WF_BULK_CASE_UPPER == c ? MSG_BATCH_CASE_UPPER
	                                          : MSG_BATCH_CASE_KEEP);
}

static void draw_pattern(const WfApp *app, WfFrame *f)
{
	const WfBatch *b = &app->batch;
	const WfThemeStyles *th = wf_theme_styles();
	char text[200];
	WfBuf dir;
	int lw = 0, y = 2, vx;
	WfBulkPattern p;
	size_t pos;
	int err = make_pattern(b, &p, &pos);

	wf_buf_init(&dir);
	if (WF_OK != wf_sanitize_utf16(b->dir, wcslen(b->dir), &dir)) {
		wf_buf_puts(&dir, "?");
	}
	(void)wf_msgf(text, sizeof text, MSG_BATCH_TITLE,
	        (WfMsgArg[]){WF_ARG_N(b->n), WF_ARG_S(wf_buf_cstr(&dir))}, 2);
	wf_buf_free(&dir);
	draw_title(app, f, text);
	for (int i = 0; i < WF_BF_COUNT; i++) {
		const char *l = wf_msg(field_label[i]);
		int c = wf_utf8_cells(l, strlen(l));

		lw = c > lw ? c : lw;
	}
	if (lw > f->cols / 2) {
		lw = f->cols / 2;
	}
	vx = 3 + lw + 1;
	for (int i = 0; i < WF_BF_COUNT && y < f->rows - 3; i++, y++) {
		bool on = i == b->field;
		const WfStyle *ls = on ? &th->accent : &th->dim;

		wf_app_put_fit(f, 1, y, on ? ">" : " ", ls, 1);
		wf_app_put_fit(f, 3, y, wf_msg(field_label[i]), ls, lw);
		if (WF_BF_STEM_CASE == i || WF_BF_EXT_CASE == i) {
			snprintf(text, sizeof text, "< %s >",
			        case_text(WF_BF_STEM_CASE == i ? b->stem_case
			                                       : b->ext_case));
			wf_app_put_fit(f, vx, y, text, &th->base, f->cols - vx);
		} else {
			int used = put_w(f, vx, y, f->cols - vx - 1,
			        b->f_text[i], &th->base);

			if (on) {
				wf_app_put_fit(
				        f, vx + used, y, "_", &th->base, 1);
				wf_frame_set_cursor(f, vx + used, y);
			}
		}
	}
	y++;
	if (y < f->rows - 3) {
		if (err) {
			if (1 == err) {
				(void)wf_msgf(text, sizeof text,
				        MSG_BATCH_ERR_TEMPLATE,
				        &WF_ARG_N(pos + 1), 1);
			} else {
				snprintf(text, sizeof text, "%s",
				        wf_msg(2 == err ? MSG_BATCH_ERR_START
				                        : MSG_BATCH_ERR_STEP));
			}
			wf_app_put_fit(f, 3, y, text, &th->error, f->cols - 4);
		} else {
			snprintf(text, sizeof text,
			        "{name} %s   {n} %s   {n:4} %s",
			        wf_msg(MSG_BATCH_HELP_NAME),
			        wf_msg(MSG_BATCH_HELP_N),
			        wf_msg(MSG_BATCH_HELP_N4));
			wf_app_put_fit(f, 3, y, text, &th->dim, f->cols - 4);
		}
		y += 2;
	}
	if (y < f->rows - 3) {
		wf_app_put_fit(f, 3, y++, wf_msg(MSG_BATCH_SAMPLE), &th->dim,
		        f->cols - 4);
	}
	for (size_t i = 0; i < b->n && i < SAMPLE_ROWS && y < f->rows - 3;
	        i++, y++) {
		int half = (f->cols - 8) / 2, used;
		wchar_t *to = NULL;

		used = put_w(f, 3, y, half, b->item[i].name, &th->base);
		wf_app_put_fit(f, 3 + used, y, " -> ", &th->dim, 4);
		if (!err &&
		        WF_OK ==
		                wf_bulk_apply(&p, b->item[i].name,
		                        b->item[i].is_dir, i, &to)) {
			put_w(f, 3 + used + 4, y, f->cols - 7 - used - 4, to,
			        &th->accent);
		} else {
			wf_app_put_fit(f, 3 + used + 4, y, "?", &th->error, 1);
		}
		free(to);
	}
	draw_footer(app, f, MSG_STATUS_HINT_BATCH_PATTERN);
}

/* What the planner said about row i, in the language of the screen. */
const char *wf_app_batch_row_text(
        const WfBatchPlan *pl, size_t i, char *buf, size_t cap)
{
	const WfBatchRow *r;

	if (!pl || i >= pl->n || !pl->row) {
		return "";
	}
	r = &pl->row[i];
	if (WF_BP_OK == r->why) {
		return r->temp ? wf_msg(MSG_BATCH_ROW_TEMP) : "";
	}
	if (WF_BP_NAME == r->why) {
		(void)wf_msgf(buf, cap, MSG_BATCH_WHY_NAME,
		        &WF_ARG_S(wf_app_name_reason_text((int)r->name)), 1);
		return buf;
	}
	return wf_msg_reason(wf_batch_why_text(r->why));
}

static const WfMsg pre_msg[] = {MSG_BATCH_PRE_NOMEM, MSG_BATCH_PRE_MISSING,
        MSG_BATCH_PRE_CHANGED, MSG_BATCH_PRE_ACCESS, MSG_BATCH_PRE_NOMEM,
        MSG_BATCH_PRE_LIMITS, MSG_BATCH_PRE_UNSUPPORTED, MSG_BATCH_PRE_NOMEM};

/* The one line under the counts: what can be done, or why it cannot. */
static void preview_state(const WfApp *app, char *out, size_t cap, bool *bad)
{
	const WfBatch *b = &app->batch;
	const WfBatchPlan *pl = plan_of(app);

	*bad = true;
	if (WF_BX_PRE_PLAN == b->pre.pre && pl) {
		char first[160];
		const char *why = wf_app_batch_row_text(
		        pl, b->pre.pre_item, first, sizeof first);

		(void)wf_msgf(out, cap, MSG_BATCH_PV_REFUSED,
		        (WfMsgArg[]){WF_ARG_N(pl->n_refused), WF_ARG_S(why)},
		        2);
	} else if (WF_BX_PRE_READY != b->pre.pre || !b->job) {
		WfBuf shown;
		size_t it = b->pre.pre_item;

		wf_buf_init(&shown);
		if (it < b->n) {
			if (WF_OK !=
			        wf_sanitize_utf16(b->item[it].name,
			                wcslen(b->item[it].name), &shown)) {
				wf_buf_puts(&shown, "?");
			}
		}
		(void)wf_msgf(out, cap, MSG_BATCH_PV_NOTREADY,
		        (WfMsgArg[]){WF_ARG_S(wf_buf_cstr(&shown)),
		                WF_ARG_N(b->pre.error),
		                WF_ARG_S(wf_msg(
		                        pre_msg[b->pre.pre < 8 ? b->pre.pre
		                                               : 0]))},
		        3);
		wf_buf_free(&shown);
	} else if (pl && 0 == pl->n_steps) {
		snprintf(out, cap, "%s", wf_msg(MSG_BATCH_PV_NOTHING));
	} else if (!executable(app)) {
		snprintf(out, cap, "%s", wf_msg(MSG_BATCH_PV_REFUSED_SHORT));
	} else {
		snprintf(out, cap, "%s", wf_msg(MSG_BATCH_PV_READY));
		*bad = false;
	}
}

static void draw_preview(const WfApp *app, WfFrame *f)
{
	const WfBatch *b = &app->batch;
	const WfThemeStyles *th = wf_theme_styles();
	const WfBatchPlan *pl = plan_of(app);
	char counts[200], state[240], verdict[160];
	bool bad;
	size_t page = page_rows(app), top = b->top;
	int vw = f->cols / 2 < VERDICT_MAX ? f->cols / 2 : VERDICT_MAX, y;

	(void)wf_msgf(counts, sizeof counts, MSG_BATCH_PV_COUNTS,
	        (WfMsgArg[]){WF_ARG_N(b->n), WF_ARG_N(b->n_renames),
	                WF_ARG_N(pl ? pl->n_skipped : 0),
	                WF_ARG_N(pl ? pl->n_refused : 0)},
	        4);
	if (pl && pl->n_cycles) {
		size_t n = strlen(counts);

		counts[n++] = ' ';
		counts[n] = '\0';
		(void)wf_msgf(counts + n, sizeof counts - n,
		        MSG_BATCH_PV_CYCLES, &WF_ARG_N(pl->n_cycles), 1);
	}
	draw_title(app, f, counts);
	preview_state(app, state, sizeof state, &bad);
	wf_app_put_fit(
	        f, 1, 1, state, bad ? &th->error : &th->accent, f->cols - 2);
	if (top + page > b->n) {
		top = b->n > page ? b->n - page : 0;
	}
	for (size_t i = top, row = 0; i < b->n && row < page; i++, row++) {
		int half, used;
		const char *why =
		        wf_app_batch_row_text(pl, i, verdict, sizeof verdict);
		bool refused = pl && pl->row && i < pl->n &&
		        WF_BP_OK != pl->row[i].why &&
		        WF_BP_UNCHANGED != pl->row[i].why;
		const WfStyle *st = refused ? &th->error
		        : pl && pl->row && i < pl->n &&
		                WF_BP_UNCHANGED == pl->row[i].why
		        ? &th->dim
		        : &th->base;

		y = 3 + (int)row;
		half = (f->cols - 2 - vw - 1 - 4) / 2;
		if (half < 1) {
			half = 1;
		}
		used = put_w(f, 1, y, half, b->item[i].name, st);
		wf_app_put_fit(f, 1 + used, y, " -> ", &th->dim, 4);
		if (b->item[i].to) {
			put_w(f, 1 + used + 4, y, f->cols - 2 - vw - used - 5,
			        b->item[i].to, st);
		}
		if (why[0]) {
			wf_app_put_fit(f, f->cols - 1 - vw, y, why, st, vw);
		}
	}
	draw_footer(app, f,
	        bad ? MSG_STATUS_HINT_BATCH_PREVIEW_NO
	            : MSG_STATUS_HINT_BATCH_PREVIEW);
}

static void draw_running(const WfApp *app, WfFrame *f)
{
	const WfBatch *b = &app->batch;
	const WfThemeStyles *th = wf_theme_styles();
	char text[200];

	draw_title(app, f,
	        wf_msg(b->recover ? MSG_BATCH_RECOVERING
	                          : MSG_BATCH_TITLE_RUN));
	if (!b->recover) {
		size_t total = b->pre.steps_total;

		(void)wf_msgf(text, sizeof text, MSG_BATCH_RUNNING,
		        (WfMsgArg[]){WF_ARG_N(b->progress), WF_ARG_N(total)},
		        2);
		wf_app_put_fit(f, 1, 2, text, &th->base, f->cols - 2);
	}
	if (b->cancelling) {
		wf_app_put_fit(f, 1, 4, wf_msg(MSG_BATCH_RUN_CANCELLING),
		        &th->warning, f->cols - 2);
	}
	draw_footer(app, f,
	        b->recover ? MSG_STATUS_HINT_BATCH_RECOVER
	                   : MSG_STATUS_HINT_BATCH_RUN);
}

static void draw_result(const WfApp *app, WfFrame *f)
{
	const WfBatch *b = &app->batch;
	const WfThemeStyles *th = wf_theme_styles();
	bool good = b->recover ? b->done.rec_open == 0
	                       : WF_BX_SUCCESS == b->done.rep.result;

	draw_title(app, f, wf_msg(MSG_BATCH_TITLE_RESULT));
	for (int i = 0; i < b->result_n && 2 + i < f->rows - 2; i++) {
		wf_app_put_fit(f, 1, 2 + i, b->result[i],
		        0 == i && !good ? &th->warning : &th->base,
		        f->cols - 2);
	}
	wf_app_put_fit(f, 1, f->rows - 1, wf_msg(MSG_STATUS_HINT_BATCH_RESULT),
	        &th->dim, f->cols - 2);
}

static void batch_draw(const WfApp *app, WfFrame *f)
{
	const WfBatch *b = &app->batch;

	wf_frame_fill(f, 0, 0, f->cols, f->rows, ' ', &WF_STYLE_BASE);
	if (f->cols < 24 || f->rows < 8) {
		wf_app_put_fit(f, 0, 0, wf_msg(MSG_SCREEN_TOO_SMALL),
		        &WF_STYLE_BASE, f->cols);
		return;
	}
	switch (b->stage) {
	case WF_BS_PATTERN: draw_pattern(app, f); break;
	case WF_BS_PREVIEW: draw_preview(app, f); break;
	case WF_BS_RUNNING: draw_running(app, f); break;
	case WF_BS_RESULT:  draw_result(app, f); break;
	default:            break;
	}
}

const WfMode WF_MODE_BATCH = {"BATCH", batch_draw, batch_event};
