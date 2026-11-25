/*
 * Who has a file open (L; DR-DRAFT-024).
 *
 * A failed copy, move, delete, rename or editor save leaves a note of what
 * failed, with the identities the operation had. L opens a window that asks
 * the lock worker (Restart Manager) which programs use the file, and lets the
 * person ask one to close, end it (two questions), and put the operation back.
 *
 * Nothing here opens, copies, moves or removes a file, and nothing here ends a
 * process: the worker does, after checking the process again. "Try again"
 * never performs anything: it puts the operation back at its ordinary entry
 * (the paste, the Delete question, the rename prompt, the editor's save), so
 * every check runs from the start. Nothing is retried by itself.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../path/wf_path.h"
#include "../text/wf_text.h"
#include "wf_app_priv.h"
#include "wf_info.h"

/* The smallest screen on which the warning of "force terminate" is shown whole. */
#define END_MIN_COLS 44
#define END_MIN_ROWS 14
#define JOBS_MAX 8

static void ctx_clear(WfApp *app)
{
	free(app->lock.path);
	free(app->lock.dir);
	free(app->lock.dest);
	free(app->lock.old_name);
	free(app->lock.new_name);
	wf_slot_free(&app->lock.slot);
	app->lock.path = app->lock.dir = app->lock.dest = NULL;
	app->lock.old_name = app->lock.new_name = NULL;
	memset(&app->lock.source_id, 0, sizeof app->lock.source_id);
	app->lock.op = WF_LOCKOP_NONE;
}

static void job_free(WfApp *app, size_t i)
{
	wf_slot_free(&app->lock.jobs[i].slot);
	free(app->lock.jobs[i].dest);
	app->lock.jobs_n--;
	if (i != app->lock.jobs_n) {
		app->lock.jobs[i] = app->lock.jobs[app->lock.jobs_n];
	}
	memset(&app->lock.jobs[app->lock.jobs_n], 0, sizeof app->lock.jobs[0]);
}

void wf_app_lock_destroy(WfApp *app)
{
	ctx_clear(app);
	while (app->lock.jobs_n) {
		job_free(app, app->lock.jobs_n - 1);
	}
	wf_locklist_free(&app->lock.list);
}

void wf_app_set_lock(WfApp *app, WfLockPort port)
{
	app->lock.port = port;
}

void wf_app_set_editor_lock(
        WfApp *app, wchar_t *(*path)(void *ctx), int (*save)(void *ctx))
{
	app->lock.editor_path = path;
	app->lock.editor_save = save;
}

void wf_app_lock_note_job(WfApp *app, uint32_t job, const WfCopySpec *spec)
{
	size_t at;

	if (WF_CJ_COPY != spec->kind && WF_CJ_MOVE_ACROSS != spec->kind &&
	        WF_CJ_RECYCLE != spec->kind) {
		return;
	}
	if (JOBS_MAX == app->lock.jobs_n) {
		job_free(app, 0);
	}
	at = app->lock.jobs_n;
	memset(&app->lock.jobs[at], 0, sizeof app->lock.jobs[0]);
	wf_slot_init(&app->lock.jobs[at].slot);
	if (WF_OK != wf_slot_clone(&app->lock.jobs[at].slot, &spec->slot)) {
		wf_slot_free(&app->lock.jobs[at].slot);
		return;
	}
	if (spec->dest) {
		app->lock.jobs[at].dest = wf_path_dup(spec->dest);
		if (!app->lock.jobs[at].dest) {
			wf_slot_free(&app->lock.jobs[at].slot);
			return;
		}
	}
	app->lock.jobs[at].job = job;
	app->lock.jobs[at].kind = spec->kind;
	app->lock.jobs_n++;
}

/* Keeps only the sources of `s` for which keep[i] is set. */
static void slot_keep(WfTransferSlot *s, const bool *keep)
{
	size_t out = 0;

	for (size_t i = 0; i < s->count; i++) {
		if (keep[i]) {
			s->src[out++] = s->src[i];
		} else {
			free(s->src[i].name);
		}
	}
	s->count = out;
}

bool wf_app_lock_job_result(WfApp *app, const WfCopyResult *r)
{
	size_t j = app->lock.jobs_n, first = r->fail_all_n;
	bool *keep = NULL;
	WfTransferSlot *s;

	for (size_t i = 0; i < app->lock.jobs_n; i++) {
		if (app->lock.jobs[i].job == r->job) {
			j = i;
			break;
		}
	}
	for (size_t f = 0; f < r->fail_all_n; f++) {
		if (WF_LOCK_IN_USE == wf_lock_classify(r->fail_all[f].error)) {
			first = f;
			break;
		}
	}
	if (first == r->fail_all_n) { /* no failure that says "in use" */
		if (j < app->lock.jobs_n) {
			job_free(app, j);
		}
		return false;
	}
	ctx_clear(app);
	app->lock.path = wf_path_dup(r->fail_all[first].path);
	app->lock.op = WF_CJ_RECYCLE == r->kind ? WF_LOCKOP_RECYCLE
	        : WF_CJ_MOVE_ACROSS == r->kind  ? WF_LOCKOP_MOVE
	                                        : WF_LOCKOP_COPY;
	if (!app->lock.path) {
		app->lock.op = WF_LOCKOP_NONE;
	}
	if (j == app->lock.jobs_n) {
		return NULL != app->lock.path; /* asked, but no retry */
	}
	s = &app->lock.jobs[j].slot;
	keep = calloc(s->count ? s->count : 1, sizeof *keep);
	if (keep) {
		size_t n = 0;

		for (size_t f = 0; f < r->fail_all_n; f++) {
			if (WF_LOCK_IN_USE !=
			        wf_lock_classify(r->fail_all[f].error)) {
				continue;
			}
			for (size_t i = 0; i < s->count; i++) {
				if (wf_app_path_in_source(r->fail_all[f].path,
				            s->dir, s->src[i].name)) {
					keep[i] = true;
				}
			}
		}
		for (size_t i = 0; i < s->count; i++) {
			n += keep[i];
		}
		if (0 ==
		        n) { /* a failure on the destination side: all of them */
			for (size_t i = 0; i < s->count; i++) {
				keep[i] = true;
			}
		}
		slot_keep(s, keep);
		free(keep);
		app->lock.slot = *s;
		memset(s, 0, sizeof *s);
		app->lock.dest = app->lock.jobs[j].dest;
		app->lock.jobs[j].dest = NULL;
	}
	job_free(app, j);
	return NULL != app->lock.path;
}

void wf_app_lock_rename_failed(WfApp *app, const WfEditOutcome *outcome)
{
	const WfUndoEntry *u = &app->undo_pending;

	if (outcome->renamed ||
	        WF_LOCK_IN_USE != wf_lock_classify(outcome->error) ||
	        !app->undo_pending_set || WF_UNDO_RENAME != u->kind ||
	        1 != u->n || !u->folder || !u->item[0].name ||
	        !u->item[0].old_name) {
		return;
	}
	ctx_clear(app);
	app->lock.dir = wf_path_dup(u->folder);
	app->lock.old_name = wf_path_dup(u->item[0].old_name);
	app->lock.new_name = wf_path_dup(u->item[0].name);
	app->lock.source_id = u->item[0].id;
	if (!app->lock.dir || !app->lock.old_name || !app->lock.new_name ||
	        WF_OK !=
	                wf_path_join(app->lock.dir, app->lock.old_name,
	                        &app->lock.path)) {
		ctx_clear(app);
		return;
	}
	app->lock.op = WF_LOCKOP_RENAME;
}

void wf_app_lock_move_failed(
        WfApp *app, const WfEditOutcome *outcome, size_t next)
{
	const WfTransferSlot *s = &app->move.slot;
	bool *keep;

	if (WF_LOCK_IN_USE != wf_lock_classify(outcome->error) ||
	        app->move.is_undo || next >= s->count || !app->move.dest) {
		return;
	}
	ctx_clear(app);
	if (WF_OK != wf_path_join(s->dir, s->src[next].name, &app->lock.path)) {
		return;
	}
	wf_slot_init(&app->lock.slot);
	if (WF_OK != wf_slot_clone(&app->lock.slot, s)) {
		ctx_clear(app);
		return;
	}
	keep = calloc(s->count, sizeof *keep);
	if (!keep) {
		ctx_clear(app);
		return;
	}
	for (size_t i = next; i < s->count; i++) {
		keep[i] = true;
	}
	slot_keep(&app->lock.slot, keep);
	free(keep);
	app->lock.dest = wf_path_dup(app->move.dest);
	app->lock.op = app->lock.dest ? WF_LOCKOP_MOVE_SAME : WF_LOCKOP_NONE;
	if (!app->lock.dest) {
		ctx_clear(app);
	}
}

static void forget_pick(WfApp *app)
{
	app->lock.pick = 0;
}

static bool query_start(WfApp *app)
{
	if (!app->lock.port.query || !app->lock.path) {
		wf_app_set_msg(app, wf_msg(MSG_LOCK_UNAVAILABLE));
		return false;
	}
	if (app->lock.busy) {
		snprintf(app->lock.note, sizeof app->lock.note, "%s",
		        wf_msg(MSG_LOCK_N_BUSY));
		return false;
	}
	app->lock.searching = true;
	if (WF_OK !=
	        app->lock.port.query(
	                app->lock.port.ctx, ++app->lock.gen, app->lock.path)) {
		app->lock.searching = false;
		snprintf(app->lock.note, sizeof app->lock.note, "%s",
		        wf_msg(MSG_LOCK_N_BUSY));
		return false;
	}
	app->lock.busy = WF_LOCKREQ_QUERY + 1;
	return true;
}

static void say_note(WfApp *app, WfMsg id, const char *a, const char *b)
{
	(void)wf_msgf(app->lock.note, sizeof app->lock.note, id,
	        (WfMsgArg[]){WF_ARG_S(a ? a : ""), WF_ARG_S(b ? b : "")}, 2);
	if (&WF_MODE_LOCK != app->mode) {
		wf_app_set_msg(app, app->lock.note);
	}
}

static const char *deny_text(WfLockDeny d)
{
	switch (d) {
	case WF_DENY_SELF:         return wf_msg(MSG_LOCK_DENY_SELF);
	case WF_DENY_ANCESTOR:     return wf_msg(MSG_LOCK_DENY_ANCESTOR);
	case WF_DENY_IDENTITY:     return wf_msg(MSG_LOCK_DENY_IDENTITY);
	case WF_DENY_UNKNOWN:      return wf_msg(MSG_LOCK_DENY_UNKNOWN);
	case WF_DENY_SYSTEM:       return wf_msg(MSG_LOCK_DENY_SYSTEM);
	case WF_DENY_CRITICAL:     return wf_msg(MSG_LOCK_DENY_CRITICAL);
	case WF_DENY_PROTECTED:    return wf_msg(MSG_LOCK_DENY_PROTECTED);
	case WF_DENY_KIND:         return wf_msg(MSG_LOCK_DENY_KIND);
	case WF_DENY_OTHER_USER:   return wf_msg(MSG_LOCK_DENY_OTHER_USER);
	case WF_DENY_SYSTEM_IMAGE: return wf_msg(MSG_LOCK_DENY_SYSTEM_IMAGE);
	case WF_DENY_NO_ACCESS:    return wf_msg(MSG_LOCK_DENY_NO_ACCESS);
	case WF_DENY_NO_WINDOW:    return wf_msg(MSG_LOCK_DENY_NO_WINDOW);
	case WF_DENY_NONE:         break;
	}
	return "";
}

void wf_app_on_lock(WfApp *app, WfLockAnswer *a)
{
	char num[24];

	if (!a) {
		return;
	}
	if (a->gen == app->lock.gen) {
		app->lock.busy = 0;
	}
	if (WF_LOCKREQ_QUERY == a->req) {
		if (a->gen == app->lock.gen) {
			uint32_t pid = app->lock.list.n &&
			                app->lock.pick < app->lock.list.n
			        ? app->lock.list.h[app->lock.pick].pid
			        : 0;
			uint64_t start = pid
			        ? app->lock.list.h[app->lock.pick].start
			        : 0;

			wf_locklist_free(&app->lock.list);
			app->lock.list = a->list;
			memset(&a->list, 0, sizeof a->list);
			app->lock.qst = a->st;
			app->lock.qerr = a->error;
			app->lock.searching = false;
			forget_pick(app);
			for (size_t i = 0; pid && i < app->lock.list.n; i++) {
				if (app->lock.list.h[i].pid == pid &&
				        app->lock.list.h[i].start == start) {
					app->lock.pick = i;
				}
			}
		}
		wf_lockanswer_free(a);
		return;
	}
	snprintf(num, sizeof num, "%lu", a->error);
	{
		const char *name = app->lock.act.name;

		switch (a->result) {
		case WF_LOCKRES_EXITED:
			say_note(app,
			        WF_LOCKREQ_CLOSE == a->req ? MSG_LOCK_N_CLOSED
			                                   : MSG_LOCK_N_ENDED,
			        name, NULL);
			break;
		case WF_LOCKRES_STILL:
			say_note(app,
			        WF_LOCKREQ_CLOSE == a->req
			                ? MSG_LOCK_N_STILL_CLOSE
			                : MSG_LOCK_N_STILL_END,
			        name, NULL);
			break;
		case WF_LOCKRES_DENIED:
			say_note(app, MSG_LOCK_N_DENIED, name,
			        deny_text(a->deny));
			break;
		case WF_LOCKRES_GONE:
			say_note(app, MSG_LOCK_N_GONE, name, NULL);
			break;
		case WF_LOCKRES_FAILED:
			say_note(app, MSG_LOCK_N_FAILED, name, num);
			break;
		}
	}
	/* look again, once, so the window shows what is true now */
	if (&WF_MODE_LOCK == app->mode) {
		(void)query_start(app);
	}
	wf_lockanswer_free(a);
}

static void shown(const wchar_t *w, size_t cells, char *out, size_t cap)
{
	WfBuf b;

	wf_buf_init(&b);
	if (w && WF_OK == wf_sanitize_utf16(w, wcslen(w), &b)) {
		wf_utf8_elide(out, cap, wf_buf_cstr(&b), cells);
	} else {
		snprintf(out, cap, "?");
	}
	wf_buf_free(&b);
}

static const wchar_t *leaf_of(const wchar_t *p)
{
	const wchar_t *s = wcsrchr(p, L'\\');

	return s ? s + 1 : p;
}

static void holder_name(const WfLockHolder *h, char *out, size_t cap)
{
	if (h->app[0]) {
		shown(h->app, 40, out, cap);
	} else if (h->exe[0]) {
		shown(leaf_of(h->exe), 40, out, cap);
	} else {
		snprintf(out, cap, "?");
	}
}

static const char *kind_text(WfLockAppType t)
{
	switch (t) {
	case WF_LOCKAPP_MAIN_WINDOW:
	case WF_LOCKAPP_OTHER_WINDOW: return wf_msg(MSG_LOCK_KIND_WINDOW);
	case WF_LOCKAPP_SERVICE:      return wf_msg(MSG_LOCK_KIND_SERVICE);
	case WF_LOCKAPP_EXPLORER:     return wf_msg(MSG_LOCK_KIND_EXPLORER);
	case WF_LOCKAPP_CONSOLE:      return wf_msg(MSG_LOCK_KIND_CONSOLE);
	case WF_LOCKAPP_CRITICAL:     return wf_msg(MSG_LOCK_KIND_CRITICAL);
	case WF_LOCKAPP_UNKNOWN:      break;
	}
	return wf_msg(MSG_LOCK_KIND_UNKNOWN);
}

static const char *op_text(int op)
{
	switch (op) {
	case WF_LOCKOP_COPY:      return wf_msg(MSG_LOCK_OP_COPY);
	case WF_LOCKOP_MOVE:
	case WF_LOCKOP_MOVE_SAME: return wf_msg(MSG_LOCK_OP_MOVE);
	case WF_LOCKOP_RECYCLE:   return wf_msg(MSG_LOCK_OP_RECYCLE);
	case WF_LOCKOP_RENAME:    return wf_msg(MSG_LOCK_OP_RENAME);
	case WF_LOCKOP_EDITOR:    return wf_msg(MSG_LOCK_OP_SAVE);
	default:                  break;
	}
	return "";
}

static void line_at(
        WfFrame *f, int *y, const char *text, int indent, const WfStyle *st)
{
	if (*y < f->rows - 3) {
		wf_app_put_fit(f, indent, *y, text, st, f->cols - indent);
	}
	(*y)++;
}

static void details(const WfApp *app, WfFrame *f, int *y, const WfLockHolder *h)
{
	char t[200], a[WF_LOCK_EXE_CHARS], when[48];
	WfFileTime ft = h->start;

	if (app->local_time) {
		ft = app->local_time(ft);
	}
	wf_info_format_time(ft, when, sizeof when);
	(void)wf_msgf(t, sizeof t, MSG_LOCK_D_START, &WF_ARG_S(when), 1);
	line_at(f, y, t, 4, &WF_STYLE_BASE);
	if (h->exe[0]) {
		shown(h->exe, 150, a, sizeof a);
		(void)wf_msgf(t, sizeof t, MSG_LOCK_D_IMAGE, &WF_ARG_S(a), 1);
	} else {
		snprintf(t, sizeof t, "%s", wf_msg(MSG_LOCK_D_IMAGE_NO));
	}
	line_at(f, y, t, 4, &WF_STYLE_BASE);
	(void)wf_msgf(
	        t, sizeof t, MSG_LOCK_D_KIND, &WF_ARG_S(kind_text(h->type)), 1);
	line_at(f, y, t, 4, &WF_STYLE_BASE);
	if (WF_DENY_NONE == h->close_deny) {
		snprintf(t, sizeof t, "%s", wf_msg(MSG_LOCK_D_CLOSE_OK));
	} else {
		(void)wf_msgf(t, sizeof t, MSG_LOCK_D_CLOSE_NO,
		        &WF_ARG_S(deny_text(h->close_deny)), 1);
	}
	line_at(f, y, t, 4, &WF_STYLE_BASE);
	if (WF_DENY_NONE == h->end_deny) {
		snprintf(t, sizeof t, "%s", wf_msg(MSG_LOCK_D_END_OK));
	} else {
		(void)wf_msgf(t, sizeof t, MSG_LOCK_D_END_NO,
		        &WF_ARG_S(deny_text(h->end_deny)), 1);
	}
	line_at(f, y, t, 4, &WF_STYLE_BASE);
}

static void lock_draw(const WfApp *app, WfFrame *f)
{
	const WfLockList *l = &app->lock.list;
	char t[300], a[WF_LOCK_EXE_CHARS], b[96];
	int y = 0;

	if (f->rows < 4) {
		return;
	}
	wf_frame_fill(f, 0, 0, f->cols, f->rows - 1, ' ', &WF_STYLE_BASE);
	wf_app_put_fit(f, 0, 0,
	        wf_msg(app->lock.op ? MSG_LOCK_TITLE_FAILED
	                            : MSG_LOCK_TITLE_HAND),
	        &wf_app_style_bar, f->cols);
	y = 2;
	shown(app->lock.path, f->cols > 4 ? (size_t)f->cols - 2 : 2, a,
	        sizeof a);
	line_at(f, &y, a, 1, &WF_STYLE_BASE);
	if (app->lock.op) {
		(void)wf_msgf(t, sizeof t, MSG_LOCK_OPLINE,
		        &WF_ARG_S(op_text(app->lock.op)), 1);
		line_at(f, &y, t, 1, &WF_STYLE_BASE);
	}
	y++;
	if (app->lock.searching) {
		line_at(f, &y, wf_msg(MSG_LOCK_SEARCHING), 1, &WF_STYLE_BASE);
	} else if (WF_OK != app->lock.qst) {
		WfMsg m = WF_E_INVAL == app->lock.qst ? MSG_LOCK_ERR_NOT_FILE
		        : WF_E_NOENT == app->lock.qst ? MSG_LOCK_ERR_GONE
		        : WF_E_RANGE == app->lock.qst ? MSG_LOCK_ERR_LONG
		                                      : MSG_LOCK_ERR_FAILED;

		snprintf(b, sizeof b, "%lu", app->lock.qerr);
		(void)wf_msgf(t, sizeof t, m, &WF_ARG_S(b), 1);
		line_at(f, &y, t, 1, &WF_STYLE_BASE);
	} else if (0 == l->n) {
		line_at(f, &y, wf_msg(MSG_LOCK_NONE_1), 1, &WF_STYLE_BASE);
		line_at(f, &y, wf_msg(MSG_LOCK_NONE_2), 1, &WF_STYLE_BASE);
		line_at(f, &y, wf_msg(MSG_LOCK_NONE_3), 1, &WF_STYLE_BASE);
	} else {
		line_at(f, &y, wf_msg(MSG_LOCK_USING), 1, &WF_STYLE_BASE);
		for (size_t i = 0; i < l->n; i++) {
			char pid[16];

			holder_name(&l->h[i], b, sizeof b);
			snprintf(pid, sizeof pid, "%u", (unsigned)l->h[i].pid);
			(void)wf_msgf(a, sizeof a, MSG_LOCK_ROW,
			        (WfMsgArg[]){WF_ARG_S(b), WF_ARG_S(pid)}, 2);
			snprintf(t, sizeof t, "%c %s",
			        i == app->lock.pick ? '>' : ' ', a);
			line_at(f, &y, t, 1,
			        i == app->lock.pick
			                ? &wf_theme_styles()->selection
			                : &WF_STYLE_BASE);
		}
		if (l->more) {
			(void)wf_msgf(t, sizeof t, MSG_LOCK_MORE,
			        &WF_ARG_N(l->more), 1);
			line_at(f, &y, t, 1, &WF_STYLE_BASE);
		}
		if (app->lock.pick < l->n) {
			details(app, f, &y, &l->h[app->lock.pick]);
		}
	}
	if (app->lock.note[0] && f->rows >= 6) {
		/* the outcome of the last action has a row of its own, always seen */
		wf_frame_fill(
		        f, 0, f->rows - 3, f->cols, 1, ' ', &WF_STYLE_BASE);
		wf_app_put_fit(f, 1, f->rows - 3, app->lock.note,
		        &WF_STYLE_BASE, f->cols - 1);
	}
	wf_frame_fill(f, 0, f->rows - 2, f->cols, 1, ' ', &wf_app_style_bar);
	wf_app_put_fit(f, 0, f->rows - 2,
	        wf_msg(app->lock.op ? MSG_LOCK_KEYS : MSG_LOCK_KEYS_HAND),
	        &wf_app_style_bar, f->cols);
}

static void close_window(WfApp *app)
{
	app->mode = app->lock.back ? app->lock.back : &WF_MODE_NORMAL;
}

static void open_window(WfApp *app, const WfMode *back)
{
	app->lock.back = back;
	app->lock.note[0] = '\0';
	app->lock.shown = true;
	forget_pick(app);
	wf_locklist_free(&app->lock.list);
	app->lock.qst = WF_OK;
	app->lock.qerr = 0;
	app->mode = &WF_MODE_LOCK;
	if (!app->lock.busy) {
		(void)query_start(app);
	} else {
		app->lock.searching =
		        true; /* the request that is out answers */
	}
}

/* The row the person chose, or NULL. */
static const WfLockHolder *picked(WfApp *app)
{
	if (app->lock.searching || app->lock.pick >= app->lock.list.n) {
		snprintf(app->lock.note, sizeof app->lock.note, "%s",
		        wf_msg(app->lock.searching ? MSG_LOCK_N_BUSY
		                                   : MSG_LOCK_N_NOPICK));
		return NULL;
	}
	return &app->lock.list.h[app->lock.pick];
}

static void remember_act(WfApp *app, const WfLockHolder *h)
{
	char name[64];

	holder_name(h, name, sizeof name);
	app->lock.act.pid = h->pid;
	app->lock.act.start = h->start;
	app->lock.act.type = h->type;
	snprintf(app->lock.act.name, sizeof app->lock.act.name, "%s", name);
}

static bool fits_for_end(const WfApp *app)
{
	return app->cols >= END_MIN_COLS && app->rows >= END_MIN_ROWS;
}

/* The program's name and PID as one confirmation line. */
static void who_line(const WfApp *app, char *out, size_t cap)
{
	char pid[16];

	snprintf(pid, sizeof pid, "%u", (unsigned)app->lock.act.pid);
	(void)wf_msgf(out, cap, MSG_LOCK_ROW,
	        (WfMsgArg[]){WF_ARG_S(app->lock.act.name), WF_ARG_S(pid)}, 2);
}

static void close_yes(WfApp *app, void *ctx)
{
	(void)ctx;
	app->mode = &WF_MODE_LOCK;
	if (!app->lock.port.close ||
	        WF_OK !=
	                app->lock.port.close(app->lock.port.ctx,
	                        ++app->lock.gen, app->lock.act.pid,
	                        app->lock.act.start, app->lock.act.type)) {
		snprintf(app->lock.note, sizeof app->lock.note, "%s",
		        wf_msg(MSG_LOCK_N_BUSY));
		return;
	}
	app->lock.busy = WF_LOCKREQ_CLOSE + 1;
	app->lock.note[0] = '\0';
}

static void end_yes2(WfApp *app, void *ctx)
{
	(void)ctx;
	app->mode = &WF_MODE_LOCK;
	if (!fits_for_end(app)) {
		snprintf(app->lock.note, sizeof app->lock.note, "%s",
		        wf_msg(MSG_LOCK_TOO_SMALL));
		return;
	}
	if (!app->lock.port.end ||
	        WF_OK !=
	                app->lock.port.end(app->lock.port.ctx, ++app->lock.gen,
	                        app->lock.act.pid, app->lock.act.start,
	                        app->lock.act.type)) {
		snprintf(app->lock.note, sizeof app->lock.note, "%s",
		        wf_msg(MSG_LOCK_N_BUSY));
		return;
	}
	app->lock.busy = WF_LOCKREQ_END + 1;
	app->lock.note[0] = '\0';
}

static void end_yes1(WfApp *app, void *ctx)
{
	char who[96];
	const char *lines[4];
	WfStatus st;

	(void)ctx;
	app->mode = &WF_MODE_LOCK;
	if (!fits_for_end(app)) {
		snprintf(app->lock.note, sizeof app->lock.note, "%s",
		        wf_msg(MSG_LOCK_TOO_SMALL));
		return;
	}
	who_line(app, who, sizeof who);
	lines[0] = wf_msg(MSG_LOCK_EQ2_TITLE);
	lines[1] = who;
	lines[2] = wf_msg(MSG_LOCK_EQ_LOSS);
	st = wf_app_confirm_begin_ex(app, lines, 3, 'k',
	        wf_msg(MSG_LOCK_EQ2_ARMED), end_yes2, NULL, NULL);
	if (WF_OK != st) {
		app->mode = &WF_MODE_LOCK;
	}
}

static void ask_close(WfApp *app, const WfEvent *ev)
{
	const WfLockHolder *h = picked(app);
	char who[96];
	const char *lines[3];

	if (!h) {
		return;
	}
	if (WF_DENY_NONE != h->close_deny) {
		(void)wf_msgf(app->lock.note, sizeof app->lock.note,
		        MSG_LOCK_D_CLOSE_NO,
		        &WF_ARG_S(deny_text(h->close_deny)), 1);
		return;
	}
	if (app->lock.busy) {
		snprintf(app->lock.note, sizeof app->lock.note, "%s",
		        wf_msg(MSG_LOCK_N_BUSY));
		return;
	}
	remember_act(app, h);
	who_line(app, who, sizeof who);
	lines[0] = wf_msg(MSG_LOCK_CQ_TITLE);
	lines[1] = who;
	lines[2] = wf_msg(MSG_LOCK_CQ_NOTE);
	if (WF_OK !=
	        wf_app_confirm_begin_ex(app, lines, 3, 'y',
	                wf_msg(MSG_LOCK_CQ_ARMED), close_yes, NULL, ev)) {
		app->mode = &WF_MODE_LOCK;
	}
}

static void ask_end(WfApp *app, const WfEvent *ev)
{
	const WfLockHolder *h = picked(app);
	char who[96];
	const char *lines[4];

	if (!h) {
		return;
	}
	if (WF_DENY_NONE != h->end_deny) {
		(void)wf_msgf(app->lock.note, sizeof app->lock.note,
		        MSG_LOCK_D_END_NO, &WF_ARG_S(deny_text(h->end_deny)),
		        1);
		return;
	}
	if (app->lock.busy) {
		snprintf(app->lock.note, sizeof app->lock.note, "%s",
		        wf_msg(MSG_LOCK_N_BUSY));
		return;
	}
	if (!fits_for_end(app)) {
		snprintf(app->lock.note, sizeof app->lock.note, "%s",
		        wf_msg(MSG_LOCK_TOO_SMALL));
		return;
	}
	remember_act(app, h);
	who_line(app, who, sizeof who);
	lines[0] = wf_msg(MSG_LOCK_EQ1_TITLE);
	lines[1] = who;
	lines[2] = wf_msg(MSG_LOCK_EQ_LOSS);
	lines[3] = wf_msg(MSG_LOCK_EQ_NOTE);
	if (WF_OK !=
	        wf_app_confirm_begin_ex(app, lines, 4, 'y',
	                wf_msg(MSG_LOCK_EQ1_ARMED), end_yes1, NULL, ev)) {
		app->mode = &WF_MODE_LOCK;
	}
}

static void retry(WfApp *app)
{
	int op = app->lock.op;
	bool ok = false;

	if (WF_LOCKOP_NONE == op) {
		snprintf(app->lock.note, sizeof app->lock.note, "%s",
		        wf_msg(MSG_LOCK_RETRY_NONE));
		return;
	}
	if (WF_LOCKOP_EDITOR == op) {
		close_window(app);
		ctx_clear(app);
		wf_app_editor_save_again(app);
		return;
	}
	if (WF_LOCKOP_RENAME == op) {
		wchar_t *dir = app->lock.dir, *old = app->lock.old_name,
		        *nw = app->lock.new_name;
		WfFileId id = app->lock.source_id;

		app->lock.dir = app->lock.old_name = app->lock.new_name = NULL;
		ctx_clear(app);
		app->mode = &WF_MODE_NORMAL;
		ok = wf_app_rename_again(app, dir, old, nw, &id);
		free(dir);
		free(old);
		free(nw);
		if (!ok) {
			return;
		}
		return;
	}
	if (0 == app->lock.slot.count) {
		snprintf(app->lock.note, sizeof app->lock.note, "%s",
		        wf_msg(MSG_LOCK_RETRY_REFUSED));
		return;
	}
	{
		WfTransferSlot slot = app->lock.slot;
		wchar_t *dest = app->lock.dest;

		memset(&app->lock.slot, 0, sizeof app->lock.slot);
		app->lock.dest = NULL;
		ctx_clear(app);
		app->mode = &WF_MODE_NORMAL;
		if (WF_LOCKOP_RECYCLE == op) {
			ok = wf_app_recycle_retry(app, &slot);
		} else {
			ok = dest &&
			        wf_app_retry_stage_slot(app, &slot, dest,
			                WF_LOCKOP_COPY == op
			                        ? WF_CJ_COPY
			                        : WF_CJ_MOVE_ACROSS,
			                MSG_LOCK_RETRY_STAGED);
		}
		if (!ok && &WF_MODE_NORMAL == app->mode && !app->msg[0]) {
			wf_app_set_msg(app, wf_msg(MSG_LOCK_RETRY_REFUSED));
		}
		wf_slot_free(&slot);
		free(dest);
	}
}

static WfAction lock_event(WfApp *app, const WfEvent *ev)
{
	size_t n = app->lock.list.n;

	if (WF_EV_RESIZE == ev->type) {
		return WF_ACT_REDRAW;
	}
	if (WF_EV_RELEASE == ev->type) {
		return WF_ACT_NONE;
	}
	if (WF_EV_KEY == ev->type) {
		switch (ev->key) {
		case WF_KEY_UP:
			if (n) {
				app->lock.pick = (app->lock.pick + n - 1) % n;
			}
			return WF_ACT_REDRAW;
		case WF_KEY_DOWN:
			if (n) {
				app->lock.pick = (app->lock.pick + 1) % n;
			}
			return WF_ACT_REDRAW;
		case WF_KEY_ESC: close_window(app); return WF_ACT_REDRAW;
		default:         return WF_ACT_NONE;
		}
	}
	if (WF_EV_CHAR != ev->type) {
		return WF_ACT_NONE;
	}
	app->lock.note[0] = '\0';
	switch (ev->ch) {
	case 'r':
	case 'R': (void)query_start(app); break;
	case 'c':
	case 'C': ask_close(app, ev); break;
	case 'k':
	case 'K': ask_end(app, ev); break;
	case 't':
	case 'T': retry(app); break;
	default:  return WF_ACT_NONE;
	}
	return WF_ACT_REDRAW;
}

const WfMode WF_MODE_LOCK = {"LOCK", lock_draw, lock_event};

void wf_app_cmd_locks(WfApp *app)
{
	if (!app->lock.port.query) {
		wf_app_set_msg(app, wf_msg(MSG_LOCK_UNAVAILABLE));
		return;
	}
	if (app->lock.op && app->lock.path) {
		open_window(app, &WF_MODE_NORMAL);
		return;
	}
	if (wf_app_archive_guard(app)) {
		return;
	}
	{
		const WfEntry *e = wf_view_current(&app->cur);
		wchar_t *path = NULL;

		if (!e || (e->flags & (WF_ENT_DIR | WF_ENT_LINK)) ||
		        app->want_path || !app->cwd) {
			wf_app_set_msg(app, wf_msg(MSG_LOCK_NO_FILE));
			return;
		}
		if (WF_OK != wf_path_join(app->cwd, e->name, &path)) {
			wf_app_set_error(app, WF_E_NOMEM);
			return;
		}
		ctx_clear(app);
		app->lock.path = path;
		open_window(app, &WF_MODE_NORMAL);
	}
}

/* From the editor: its save failed in use and the person chose to look. */
void wf_app_editor_lock_look(WfApp *app)
{
	wchar_t *path;

	if (!app->lock.editor_path || !app->lock.port.query) {
		return;
	}
	path = app->lock.editor_path(app->editor.ctx);
	if (!path) {
		return;
	}
	ctx_clear(app);
	app->lock.path = path;
	app->lock.op = WF_LOCKOP_EDITOR;
	open_window(app, &WF_MODE_EDITOR);
}
