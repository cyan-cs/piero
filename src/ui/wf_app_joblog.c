/*
 * The history (J; DR-DRAFT-029). A job's report window shows the first few
 * failures in a short form and is gone with the next key; this keeps the last
 * WF_JOBLOG_MAX changes of the session in a window that scrolls: the jobs of
 * the copy engine with every failure and its full path, and the changes made
 * another way (rename, move on one volume, new file or folder, bulk rename).
 * Under each row is whether Undo can still take it back, and when not, why.
 * Rows are text made when the change ends, so nothing here depends on the
 * listing, the slot or the engine afterwards; nothing is written to disk.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../text/wf_text.h"
#include "wf_app_priv.h"

#define PATH_INDENT 4

static char *dup_text(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = malloc(n);

	if (p) {
		memcpy(p, s, n);
	}
	return p;
}

static void entry_free(WfJobLogEntry *e)
{
	for (size_t i = 0; i < e->fail_n; i++) {
		free(e->fail[i].label);
		free(e->fail[i].path);
	}
	free(e->fail);
	wf_slot_free(&e->retry);
	free(e->retry_dest);
	memset(e, 0, sizeof *e);
}

void wf_app_joblog_destroy(WfApp *app)
{
	for (size_t i = 0; i < app->jobs.n; i++) {
		entry_free(&app->jobs.log[i]);
	}
	app->jobs.n = 0;
	app->jobs.lines_ok = false;
	wf_app_retry_free(app);
}

/* One failure as text. false: no memory for it (the entry keeps the ones
 * before it and says how many were not kept). */
static bool fail_text(WfJobLogFail *out,
        WfCopyFailKind kind,
        unsigned long error,
        const wchar_t *path)
{
	char code[16], label[120];
	WfBuf b;

	snprintf(code, sizeof code, "%lu", error);
	(void)wf_msgf(label, sizeof label, MSG_JOBLOG_FAIL,
	        (WfMsgArg[]){
	                WF_ARG_S(wf_app_copyfail_text(kind)), WF_ARG_S(code)},
	        2);
	wf_buf_init(&b);
	/* A path that could not be made into text whole is not shown as the path:
	 * a half of it would read as the file that failed. The failure is not kept,
	 * and the entry counts it with the others it could not keep. */
	out->label = dup_text(label);
	out->path = WF_OK == wf_sanitize_utf16(path, wcslen(path), &b)
	        ? dup_text(wf_buf_cstr(&b))
	        : NULL;
	wf_buf_free(&b);
	if (!out->label || !out->path) {
		free(out->label);
		free(out->path);
		memset(out, 0, sizeof *out);
		return false;
	}
	return true;
}

/* Puts the row at the end; the oldest goes when the history is full. */
static void hist_push(WfApp *app, const WfJobLogEntry *e)
{
	if (WF_JOBLOG_MAX == app->jobs.n) {
		entry_free(&app->jobs.log[0]);
		memmove(&app->jobs.log[0], &app->jobs.log[1],
		        (WF_JOBLOG_MAX - 1) * sizeof app->jobs.log[0]);
		app->jobs.n--;
	}
	app->jobs.log[app->jobs.n++] = *e;
	app->jobs.lines_ok = false;
}

static void undo_fields(WfJobLogEntry *e, uint32_t undo_uid, int why)
{
	e->undo_uid = undo_uid;
	e->undo = undo_uid ? WF_HU_KEPT : WF_HU_NONE;
	e->why = undo_uid ? 0 : why;
}

void wf_app_hist_add(WfApp *app, const char *head, uint32_t undo_uid, int why)
{
	WfJobLogEntry e;

	memset(&e, 0, sizeof e);
	snprintf(e.head, sizeof e.head, "%s", head);
	undo_fields(&e, undo_uid, why);
	hist_push(app, &e);
}

void wf_app_hist_undo_gone(WfApp *app, uint32_t uid, bool undone)
{
	for (size_t i = 0; uid && i < app->jobs.n; i++) {
		WfJobLogEntry *e = &app->jobs.log[i];

		if (WF_HU_KEPT == e->undo && e->undo_uid == uid) {
			e->undo = undone ? WF_HU_DONE : WF_HU_EXPIRED;
			e->undo_uid = 0;
		}
	}
}

void wf_app_joblog_add(WfApp *app,
        const char *head,
        WfCopyResult *r,
        uint32_t undo_uid,
        int why)
{
	WfJobLogEntry e;
	size_t n = r->fail_all_n
	        ? r->fail_all_n
	        : (r->failure_total < WF_CJ_FAILURES ? r->failure_total
	                                             : WF_CJ_FAILURES);

	memset(&e, 0, sizeof e);
	snprintf(e.head, sizeof e.head, "%s", head);
	e.job = r->job;
	undo_fields(&e, undo_uid, why);
	e.total = r->failure_total;
	if (n) {
		e.fail = calloc(n, sizeof *e.fail);
	}
	for (size_t i = 0; e.fail && i < n; i++) {
		const wchar_t *path = r->fail_all_n ? r->fail_all[i].path
		                                    : r->failures[i].name;
		WfCopyFailKind kind = r->fail_all_n ? r->fail_all[i].kind
		                                    : r->failures[i].kind;
		unsigned long error = r->fail_all_n ? r->fail_all[i].error
		                                    : r->failures[i].error;

		if (!fail_text(&e.fail[i], kind, error, path)) {
			break;
		}
		e.fail_n++;
	}
	wf_app_retry_take(app, r, &e);
	hist_push(app, &e);
}

/* What Undo can still do for the row, in words. Undo takes the newest change
 * first, so only the row of the newest Undo entry is the one `u` takes now. */
static void undo_line(
        const WfApp *app, const WfJobLogEntry *e, char *out, size_t cap)
{
	switch (e->undo) {
	case WF_HU_KEPT: {
		const WfUndoEntry *top =
		        app->undo_n ? &app->undo[app->undo_n - 1] : NULL;

		/* a row names its operation by the uid of the first entry */
		snprintf(out, cap, "%s",
		        wf_msg(top &&
		                                (top->group ? top->group
		                                            : top->uid) ==
		                                        e->undo_uid
		                        ? MSG_HIST_UNDO_NOW
		                        : MSG_HIST_UNDO_LATER));
		break;
	}
	case WF_HU_DONE:
		snprintf(out, cap, "%s", wf_msg(MSG_HIST_UNDONE));
		break;
	case WF_HU_EXPIRED:
		snprintf(out, cap, "%s", wf_msg(MSG_HIST_UNDO_EXPIRED));
		break;
	case WF_HU_NONE:
		(void)wf_msgf(out, cap, MSG_HIST_UNDO_NOT,
		        &WF_ARG_S(wf_msg(e->why ? (WfMsg)e->why
		                                : MSG_HIST_WHY_NOT_KEPT)),
		        1);
		break;
	}
}

typedef struct
{
	WfFrame *f; /* NULL: only count */
	int first, rows, line;
	size_t *rec; /* when counting: the lines of each entry go here */
	const size_t
	        *known; /* when drawing: the lines of each entry, if known */
} Walk;

static void put(Walk *w, const char *s, int indent)
{
	if (w->f && w->line >= w->first && w->line < w->first + w->rows) {
		wf_app_put_fit(w->f, indent, 1 + (w->line - w->first), s,
		        &WF_STYLE_BASE, w->f->cols - indent);
	}
	w->line++;
}

/* A path over as many lines as it takes: nothing is cut off. */
static void put_wrapped(Walk *w, const char *s, int cols)
{
	size_t len = strlen(s), at = 0;
	int width = cols - PATH_INDENT > 4 ? cols - PATH_INDENT : 4;

	if (0 == len) {
		put(w, "", PATH_INDENT);
		return;
	}
	while (at < len) {
		char piece[512];
		size_t take = wf_utf8_prefix(s + at, len - at, (size_t)width);
		size_t room = sizeof piece - 1;

		if (0 == take) { /* one wide character wider than the window */
			uint32_t cp;

			take = wf_utf8_decode(s + at, len - at, &cp);
			if (0 == take) {
				take = 1;
			}
		}
		if (take >
		        room) { /* the buffer ends at a character boundary too */
			take = room;
			while (take > 0 &&
			        0x80 == ((unsigned char)s[at + take] & 0xC0)) {
				take--;
			}
			if (0 == take) {
				take = room;
			}
		}
		memcpy(piece, s + at, take);
		piece[take] = '\0';
		put(w, piece, PATH_INDENT);
		at += take;
	}
}

static void walk(const WfApp *app, Walk *w, int cols)
{
	if (0 == app->jobs.n) {
		put(w, wf_msg(MSG_JOBLOG_EMPTY), 1);
		return;
	}
	for (size_t k = app->jobs.n; k-- > 0;) {
		const WfJobLogEntry *e = &app->jobs.log[k];
		char line[160];
		int start;

		if (w->f && w->known &&
		        w->line + (int)w->known[k] <= w->first) {
			w->line +=
			        (int)w->known
			                [k]; /* above the window: not walked */
			continue;
		}
		if (w->f && w->line >= w->first + w->rows) {
			return; /* below it: nothing more to draw */
		}
		start = w->line;
		if (e->job) {
			(void)wf_msgf(line, sizeof line, MSG_JOBLOG_JOB,
			        (WfMsgArg[]){
			                WF_ARG_N(e->job), WF_ARG_S(e->head)},
			        2);
		} else {
			snprintf(line, sizeof line, "%s", e->head);
		}
		put(w, line, 1);
		undo_line(app, e, line, sizeof line);
		put(w, line, 3);
		if (e->retry.count) {
			(void)wf_msgf(line, sizeof line, MSG_JOBLOG_RETRY_LINE,
			        &WF_ARG_N(e->retry.count), 1);
			put(w, line, 1);
		}
		for (size_t i = 0; i < e->fail_n; i++) {
			put(w, e->fail[i].label, 2);
			put_wrapped(w, e->fail[i].path, cols);
		}
		if (e->total > e->fail_n) {
			(void)wf_msgf(line, sizeof line, MSG_JOBLOG_UNRECORDED,
			        &WF_ARG_N(e->total - e->fail_n), 1);
			put(w, line, 1);
		}
		put(w, "", 0);
		if (w->rec) {
			w->rec[k] = (size_t)(w->line - start);
		}
	}
}

/* Counts the log's lines at `cols` columns, for each entry and in all, and keeps
 * them (until the log or the width changes). */
static size_t total_lines(WfApp *app, int cols)
{
	Walk w;

	if (!app->jobs.lines_ok || app->jobs.lines_cols != cols) {
		memset(&w, 0, sizeof w);
		w.rec = app->jobs.lines_of;
		walk(app, &w, cols);
		app->jobs.lines_total = (size_t)w.line;
		app->jobs.lines_cols = cols;
		app->jobs.lines_ok = true;
	}
	return app->jobs.lines_total;
}

static void joblog_draw(const WfApp *app, WfFrame *f)
{
	Walk w;

	if (f->rows < 4) {
		return;
	}
	wf_frame_fill(f, 0, 0, f->cols, f->rows - 1, ' ', &WF_STYLE_BASE);
	wf_app_put_fit(
	        f, 0, 0, wf_msg(MSG_JOBLOG_TITLE), &wf_app_style_bar, f->cols);
	memset(&w, 0, sizeof w);
	w.f = f;
	w.first = (int)app->jobs.top;
	w.rows = f->rows - 3;
	if (app->jobs.lines_ok && app->jobs.lines_cols == f->cols) {
		w.known = app->jobs.lines_of;
	}
	walk(app, &w, f->cols);
	wf_frame_fill(f, 0, f->rows - 2, f->cols, 1, ' ', &wf_app_style_bar);
	wf_app_put_fit(f, 0, f->rows - 2, wf_msg(MSG_JOBLOG_SCROLL),
	        &wf_app_style_bar, f->cols);
}

void wf_app_joblog_begin(WfApp *app)
{
	app->jobs.top = 0;
	app->mode = &WF_MODE_JOBLOG;
}

static WfAction joblog_event(WfApp *app, const WfEvent *ev)
{
	size_t total = total_lines(app, app->cols);
	size_t page = app->rows > 4 ? (size_t)app->rows - 4 : 1;
	size_t limit = total > page ? total - page : 0;

	if (WF_EV_RESIZE == ev->type) {
		if (app->jobs.top > limit) {
			app->jobs.top = limit;
		}
		return WF_ACT_REDRAW;
	}
	if (WF_EV_RELEASE == ev->type) {
		return WF_ACT_NONE;
	}
	if (WF_EV_CHAR == ev->type && 'r' == ev->ch) {
		wf_app_retry_stage(app);
		return WF_ACT_REDRAW;
	}
	if (WF_EV_CHAR == ev->type && 'u' == ev->ch) {
		/* the same Undo as in the list: the newest change, after its
		 * question */
		app->mode = &WF_MODE_NORMAL;
		wf_app_cmd_undo(app);
		return WF_ACT_REDRAW;
	}
	if (WF_EV_KEY == ev->type) {
		switch (ev->key) {
		case WF_KEY_DOWN:
			if (app->jobs.top < limit) {
				app->jobs.top++;
			}
			return WF_ACT_REDRAW;
		case WF_KEY_UP:
			if (app->jobs.top) {
				app->jobs.top--;
			}
			return WF_ACT_REDRAW;
		case WF_KEY_PGDN:
			app->jobs.top = app->jobs.top + page < limit
			        ? app->jobs.top + page
			        : limit;
			return WF_ACT_REDRAW;
		case WF_KEY_PGUP:
			app->jobs.top =
			        app->jobs.top > page ? app->jobs.top - page : 0;
			return WF_ACT_REDRAW;
		case WF_KEY_HOME: app->jobs.top = 0; return WF_ACT_REDRAW;
		case WF_KEY_END:  app->jobs.top = limit; return WF_ACT_REDRAW;
		default:          break;
		}
	}
	app->mode = &WF_MODE_NORMAL;
	return WF_ACT_REDRAW;
}

const WfMode WF_MODE_JOBLOG = {"JOBLOG", joblog_draw, joblog_event};
