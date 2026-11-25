/*
 * The results of a search below the folder on screen (F4 of DR-DRAFT-019,
 * DR-DRAFT-031).
 *
 * In the filter prompt (/), Tab widens the filter to "this folder and below": the
 * text becomes a query (words, ext:, size>, size<, newer:, older:) and a worker
 * walks the folders, nearest first, handing the hits over as it finds them. They
 * are a list of their own: a hit is a folder, a name and the identity the listing
 * showed, so Copy, Move and the Recycle Bin act on the real path and are refused if
 * the object is no longer that one. Nothing is read, changed or opened by looking.
 *
 *   Up Down PgUp PgDn Home End   move          Space   mark / unmark and go down
 *   Ctrl+A                       mark all      Enter   show it in its folder
 *   y x                          copy / move   (results of one folder at a time)
 *   Delete                       Recycle Bin   (any folders, one question)
 *   Esc                          stop the walk, and then close the list
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../archive/wf_vpath.h"
#include "../fs/wf_loc.h"
#include "../fs/wf_ntfsfacts.h"
#include "../fs/wf_sftpplace_win.h"
#include "../path/wf_path.h"
#include "../search/wf_dupes.h"
#include "../text/wf_text.h"
#include "wf_app_priv.h"
#include "wf_info.h"

void wf_app_set_search(WfApp *app, WfSearchPort port)
{
	app->hits.port = port;
}

static void pending_free(WfHits *h)
{
	for (size_t i = 0; i < h->n_pending; i++) {
		wf_slot_free(&h->pending[i]);
	}
	free(h->pending);
	h->pending = NULL;
	h->n_pending = h->items_pending = 0;
}

static void clear_hits(WfHits *h)
{
	for (size_t i = 0; i < h->n; i++) {
		wf_hit_clear(&h->hit[i]);
	}
	free(h->hit);
	free(h->sel);
	h->hit = NULL;
	h->sel = NULL;
	h->n = h->cap = h->nsel = h->cursor = h->top = 0;
}

static void stop_worker(WfApp *app)
{
	if (app->hits.port.cancel) {
		app->hits.port.cancel(app->hits.port.ctx);
	}
	app->hits.running = false;
}

/* Ends the window: the walk, the results, the root. */
static void close_hits(WfApp *app)
{
	WfHits *h = &app->hits;

	stop_worker(app);
	clear_hits(h);
	free(h->root);
	h->root = NULL;
	h->open = false;
	h->dupes = false;
	h->read_done = h->read_total = 0;
	h->groups = h->skipped = 0;
	h->done = h->at_least = h->stopped = false;
	h->st = WF_OK;
	memset(&h->walk, 0, sizeof h->walk);
	if (&WF_MODE_HITS == app->mode) {
		app->mode = &WF_MODE_NORMAL;
	}
}

void wf_app_hits_destroy(WfApp *app)
{
	close_hits(app);
	pending_free(&app->hits);
}

/* Whether the folder on screen can be searched below; says why not. */
static bool can_search(WfApp *app)
{
	if (app->want_path || !app->cwd || !app->cur.dir) {
		wf_app_set_msg(app, wf_msg(MSG_SEARCH_WAIT));
		return false;
	}
	if (wf_vpath_is(app->cwd) || wf_sftp_place_is(app->cwd) ||
	        wf_pcroot_is(app->cwd) || wf_wsl_unc_is(app->cwd) ||
	        wf_ntfsfacts_remote(app->cwd)) {
		wf_app_set_msg(app, wf_msg(MSG_SEARCH_LOCAL));
		return false;
	}
	return true;
}

void wf_app_search_begin(WfApp *app)
{
	WfHits *h = &app->hits;
	WfQuery q;
	size_t pos = 0;
	WfStatus st;
	WfFileId root_id;

	if (!h->port.start) {
		wf_app_set_msg(app, wf_msg(MSG_SEARCH_OFF));
		return;
	}
	if (!can_search(app)) {
		return;
	}
	st = wf_query_parse(app->cur.filter, &q, &pos);
	if (WF_OK != st) {
		char text[112], at[16];

		snprintf(at, sizeof at, "%zu", pos + 1);
		(void)wf_msgf(
		        text, sizeof text, MSG_SEARCH_BAD, &WF_ARG_S(at), 1);
		wf_app_set_msg(app, text);
		return;
	}
	if (wf_query_empty(&q)) {
		wf_app_set_msg(app, wf_msg(MSG_SEARCH_EMPTY));
		return;
	}
	root_id = app->cur.dir->identity;
	close_hits(app);
	pending_free(h);
	h->root = wf_path_dup(app->cwd);
	if (!h->root) {
		wf_app_set_error(app, WF_E_NOMEM);
		return;
	}
	h->root_id = root_id;
	h->hidden = app->cur.show_hidden;
	{
		WfBuf b;

		wf_buf_init(&b);
		(void)wf_sanitize_utf16(
		        app->cur.filter, wcslen(app->cur.filter), &b);
		snprintf(h->query, sizeof h->query, "%s", wf_buf_cstr(&b));
		wf_buf_free(&b);
	}
	if (0 == ++h->gen) {
		h->gen = 1;
	}
	st = h->port.start(
	        h->port.ctx, h->gen, h->root, root_id, &q, h->hidden);
	if (WF_OK != st) {
		free(h->root);
		h->root = NULL;
		wf_app_set_msg(app, wf_msg(MSG_SEARCH_OFF));
		return;
	}
	h->open = true;
	h->running = true;
	h->dupes = false;
	h->done = h->at_least = h->stopped = false;
	h->st = WF_OK;
	memset(&h->walk, 0, sizeof h->walk);
	app->msg[0] = '\0';
	app->mode = &WF_MODE_HITS;
}

/* F: the files below this folder that hold the same bytes (DR-DRAFT-032). */
void wf_app_dupes_begin(WfApp *app)
{
	WfHits *h = &app->hits;
	WfStatus st;
	WfFileId root_id;

	if (!h->port.start_dupes) {
		wf_app_set_msg(app, wf_msg(MSG_SEARCH_OFF));
		return;
	}
	if (!can_search(app)) {
		return;
	}
	root_id = app->cur.dir->identity;
	close_hits(app);
	pending_free(h);
	h->root = wf_path_dup(app->cwd);
	if (!h->root) {
		wf_app_set_error(app, WF_E_NOMEM);
		return;
	}
	h->root_id = root_id;
	h->hidden = app->cur.show_hidden;
	h->query[0] = '\0';
	if (0 == ++h->gen) {
		h->gen = 1;
	}
	st = h->port.start_dupes(
	        h->port.ctx, h->gen, h->root, root_id, h->hidden);
	if (WF_OK != st) {
		free(h->root);
		h->root = NULL;
		wf_app_set_msg(app, wf_msg(MSG_SEARCH_OFF));
		return;
	}
	h->open = true;
	h->running = true;
	h->dupes = true;
	h->done = h->at_least = h->stopped = false;
	h->st = WF_OK;
	h->read_done = h->read_total = 0;
	h->groups = h->skipped = 0;
	memset(&h->walk, 0, sizeof h->walk);
	app->msg[0] = '\0';
	app->mode = &WF_MODE_HITS;
}

void wf_app_on_search(WfApp *app, WfSearchBatch *b)
{
	WfHits *h = &app->hits;
	bool *grown_sel;
	WfHit *grown;

	if (!b) {
		return;
	}
	if (!h->open || !h->running || b->gen != h->gen ||
	        b->dupes != h->dupes) {
		wf_search_batch_free(b); /* not the request that is out */
		return;
	}
	if (b->dupes) {
		h->read_done = b->read_done;
		h->read_total = b->read_total;
	}
	if (b->n) {
		size_t need = h->n + b->n;

		if (need > h->cap) {
			size_t cap = h->cap ? h->cap * 2 : 512;

			while (cap < need) {
				cap *= 2;
			}
			grown = realloc(h->hit, cap * sizeof *grown);
			if (!grown) {
				wf_search_batch_free(b);
				wf_app_set_error(app, WF_E_NOMEM);
				stop_worker(app);
				h->done = true;
				h->at_least = true;
				return;
			}
			h->hit = grown;
			grown_sel = realloc(h->sel, cap * sizeof *grown_sel);
			if (!grown_sel) {
				wf_search_batch_free(b);
				wf_app_set_error(app, WF_E_NOMEM);
				stop_worker(app);
				h->done = true;
				h->at_least = true;
				return;
			}
			h->sel = grown_sel;
			h->cap = cap;
		}
		memcpy(h->hit + h->n, b->hit, b->n * sizeof *b->hit);
		memset(h->sel + h->n, 0, b->n * sizeof *h->sel);
		h->n += b->n;
		free(b->hit); /* the hits are the list's now */
		b->hit = NULL;
		b->n = 0;
	}
	if (b->done) {
		h->done = true;
		h->at_least = b->at_least;
		h->walk = b->walk;
		h->st = b->st;
		h->groups = b->groups;
		h->skipped = b->skipped;
		h->running = false;
	}
	free(b->hit);
	memset(b, 0, sizeof *b);
}

static size_t visible_rows(const WfApp *app)
{
	return app->rows > 4 ? (size_t)app->rows - 4
	                     : 1; /* title, status, hint, spare */
}

static void keep_cursor_on_screen(WfApp *app)
{
	WfHits *h = &app->hits;
	size_t rows = visible_rows(app);

	if (h->cursor >= h->n && h->n) {
		h->cursor = h->n - 1;
	}
	if (h->cursor < h->top) {
		h->top = h->cursor;
	} else if (h->cursor >= h->top + rows) {
		h->top = h->cursor - rows + 1;
	}
}

/* The folder of a hit below the root: "sub\dir", or "" for the root itself. */
static const wchar_t *relative_dir(const WfHits *h, const WfHit *hit)
{
	size_t rn = h->root ? wcslen(h->root) : 0;
	const wchar_t *p = hit->dir->path;

	if (rn && !_wcsnicmp(p, h->root, rn)) {
		p += rn;
		if (L'\\' == *p) {
			p++;
		}
		return p;
	}
	return p;
}

static void put_row(const WfApp *app, WfFrame *f, int y, size_t i)
{
	const WfHits *h = &app->hits;
	const WfHit *hit = &h->hit[i];
	const WfThemeStyles *th = wf_theme_styles();
	char size[24], when[32], left[600], mark[4];
	const WfStyle *style =
	        (hit->flags & WF_ENT_DIR) ? &th->directory : &th->base;
	WfBuf folder, name;
	int right_w = 10 + 1 + 16, name_w, group_w = 0;

	if (f->cols < 30) {
		right_w = 0;
	}
	if (i == h->cursor) {
		style = &th->cursor_line;
	}
	wf_frame_fill(f, 0, y, f->cols, 1, ' ', style);
	snprintf(mark, sizeof mark, "%c%c", i == h->cursor ? '>' : ' ',
	        h->sel[i] ? '*' : ' ');
	wf_buf_init(&folder);
	wf_buf_init(&name);
	if (h->dupes) { /* the group the file is in, so that equal files sit together */
		char tag[16];

		snprintf(tag, sizeof tag, "[%u] ", (unsigned)hit->group);
		wf_app_put_fit(f, 2, y, tag, &th->dim, 8);
		group_w = 8;
	}
	(void)wf_sanitize_utf16(
	        relative_dir(h, hit), wcslen(relative_dir(h, hit)), &folder);
	(void)wf_sanitize_utf16(hit->name, wcslen(hit->name), &name);
	if (folder.len) {
		snprintf(left, sizeof left, "%s\\%s%s", wf_buf_cstr(&folder),
		        wf_buf_cstr(&name),
		        (hit->flags & WF_ENT_DIR) ? "\\" : "");
	} else {
		snprintf(left, sizeof left, "%s%s", wf_buf_cstr(&name),
		        (hit->flags & WF_ENT_DIR) ? "\\" : "");
	}
	wf_buf_free(&folder);
	wf_buf_free(&name);
	name_w = f->cols - 2 - group_w - (right_w ? right_w + 1 : 0);
	wf_app_put_fit(f, 0, y, mark, style, 2);
	if (name_w > 0) {
		wf_app_put_fit(f, 2 + group_w, y, left, style, name_w);
	}
	if (right_w) {
		if (hit->flags & WF_ENT_DIR) {
			snprintf(size, sizeof size, "%s",
			        wf_msg(MSG_HITS_FOLDER_SIZE));
		} else {
			wf_app_fmt_bytes(hit->size, size, sizeof size);
		}
		wf_info_format_time(hit->mtime, when, sizeof when);
		when[16] = '\0'; /* the minute is enough */
		{
			char col[48];
			int pad = 10 - wf_utf8_cells(size, strlen(size));

			snprintf(col, sizeof col, "%*s%s %s", pad > 0 ? pad : 0,
			        "", size, when);
			wf_app_put_fit(
			        f, f->cols - right_w, y, col, style, right_w);
		}
	}
}

static void add(char *line, size_t cap, const char *part)
{
	size_t used = strlen(line);

	snprintf(line + used, cap - used, "%s%s", used ? " | " : "", part);
}

/* What the search did not do, on a line of its own so that it is not cut off by
 * the counts: the limit, folders left unread or not entered, why it stopped. */
static void put_account(const WfApp *app, WfFrame *f, int y)
{
	const WfHits *h = &app->hits;
	char line[400], part[120], m[24];

	line[0] = '\0';
	if (!h->done || h->stopped) {
		return;
	}
	if (WF_OK != h->st) {
		(void)wf_msgf(part, sizeof part, MSG_HITS_ERROR,
		        &WF_ARG_S(wf_status_name(h->st)), 1);
		add(line, sizeof line, part);
	}
	if (h->at_least && WF_OK == h->st && h->n >= WF_SEARCH_MAX_HITS) {
		snprintf(m, sizeof m, "%u", (unsigned)WF_SEARCH_MAX_HITS);
		(void)wf_msgf(
		        part, sizeof part, MSG_HITS_LIMIT, &WF_ARG_S(m), 1);
		add(line, sizeof line, part);
	}
	if (h->walk.pending_full) {
		add(line, sizeof line, wf_msg(MSG_HITS_PENDING));
	}
	if (h->dupes && h->walk.stopped) {
		snprintf(m, sizeof m, "%u", (unsigned)WF_DUPE_MAX_FILES);
		(void)wf_msgf(part, sizeof part, MSG_DUPES_FILE_LIMIT,
		        &WF_ARG_S(m), 1);
		add(line, sizeof line, part);
	}
	if (h->dupes && h->skipped) {
		snprintf(m, sizeof m, "%zu", h->skipped);
		(void)wf_msgf(
		        part, sizeof part, MSG_DUPES_SKIPPED, &WF_ARG_S(m), 1);
		add(line, sizeof line, part);
	}
	if (h->walk.unreadable) {
		snprintf(m, sizeof m, "%zu", h->walk.unreadable);
		(void)wf_msgf(part, sizeof part, MSG_HITS_UNREADABLE,
		        &WF_ARG_S(m), 1);
		add(line, sizeof line, part);
	}
	if (h->walk.not_entered) {
		snprintf(m, sizeof m, "%zu", h->walk.not_entered);
		(void)wf_msgf(part, sizeof part, MSG_HITS_NOT_ENTERED,
		        &WF_ARG_S(m), 1);
		add(line, sizeof line, part);
	}
	if (line[0]) {
		wf_app_put_fit(
		        f, 0, y, line, &wf_theme_styles()->warning, f->cols);
	}
}

static void put_status(const WfApp *app, WfFrame *f, int y)
{
	const WfHits *h = &app->hits;
	char line[256], part[96], n[24], m[24];

	line[0] = '\0';
	snprintf(n, sizeof n, "%zu", h->n);
	if (h->dupes) {
		snprintf(m, sizeof m, "%u",
		        h->n ? (unsigned)h->hit[h->n - 1].group : 0u);
		(void)wf_msgf(part, sizeof part, MSG_DUPES_STATUS,
		        (WfMsgArg[]){WF_ARG_S(n), WF_ARG_S(m)}, 2);
	} else {
		(void)wf_msgf(
		        part, sizeof part, MSG_HITS_FOUND, &WF_ARG_S(n), 1);
	}
	add(line, sizeof line, part);
	if (h->done && !h->dupes) {
		snprintf(m, sizeof m, "%zu", h->walk.folders);
		(void)wf_msgf(
		        part, sizeof part, MSG_HITS_FOLDERS, &WF_ARG_S(m), 1);
		add(line, sizeof line, part);
	}
	if (h->nsel) {
		snprintf(m, sizeof m, "%zu", h->nsel);
		(void)wf_msgf(
		        part, sizeof part, MSG_HITS_SELECTED, &WF_ARG_S(m), 1);
		add(line, sizeof line, part);
	}
	if (h->dupes && h->running) {
		if (h->read_total) {
			char a[24], b[24];

			snprintf(a, sizeof a, "%llu",
			        (unsigned long long)h->read_done);
			snprintf(b, sizeof b, "%llu",
			        (unsigned long long)h->read_total);
			(void)wf_msgf(part, sizeof part, MSG_DUPES_READING,
			        (WfMsgArg[]){WF_ARG_S(a), WF_ARG_S(b)}, 2);
			add(line, sizeof line, part);
		} else {
			add(line, sizeof line, wf_msg(MSG_DUPES_WALKING));
		}
	} else {
		add(line, sizeof line,
		        h->running           ? wf_msg(MSG_HITS_RUNNING)
		                : h->stopped ? wf_msg(MSG_HITS_STOPPED)
		                             : wf_msg(MSG_HITS_FINISHED));
	}
	wf_frame_fill(f, 0, y, f->cols, 1, ' ', &wf_app_style_bar);
	wf_app_put_fit(f, 0, y, line, &wf_app_style_bar, f->cols);
}

static void hits_draw(const WfApp *app, WfFrame *f)
{
	const WfHits *h = &app->hits;
	char title[400];
	WfBuf root;
	size_t rows = visible_rows(app);

	if (f->rows < 5) {
		return;
	}
	wf_frame_fill(f, 0, 0, f->cols, f->rows, ' ', &wf_theme_styles()->base);
	wf_buf_init(&root);
	if (h->root) {
		(void)wf_sanitize_utf16(h->root, wcslen(h->root), &root);
	}
	if (h->dupes) {
		(void)wf_msgf(title, sizeof title, MSG_DUPES_TITLE,
		        &WF_ARG_S(wf_buf_cstr(&root)), 1);
	} else {
		(void)wf_msgf(title, sizeof title, MSG_HITS_TITLE,
		        (WfMsgArg[]){WF_ARG_S(wf_buf_cstr(&root)),
		                WF_ARG_S(h->query)},
		        2);
	}
	wf_buf_free(&root);
	wf_frame_fill(f, 0, 0, f->cols, 1, ' ', &wf_app_style_bar);
	wf_app_put_fit(f, 0, 0, title, &wf_theme_styles()->bar_bold, f->cols);
	if (0 == h->n) {
		wf_app_put_fit(f, 1, 2,
		        wf_msg(h->running ? MSG_HITS_NONE_YET
		                          : (h->dupes ? MSG_DUPES_NONE
		                                      : MSG_HITS_NONE)),
		        &wf_theme_styles()->dim, f->cols - 2);
	}
	for (size_t k = 0; k < rows && h->top + k < h->n; k++) {
		put_row(app, f, 1 + (int)k, h->top + k);
	}
	put_account(app, f, f->rows - 3);
	put_status(app, f, f->rows - 2);
	wf_app_put_fit(f, 0, f->rows - 1,
	        app->msg[0] ? app->msg
	                    : wf_msg(h->dupes ? MSG_DUPES_HINT : MSG_HITS_HINT),
	        &wf_theme_styles()->dim, f->cols);
}

static void show_in_folder(WfApp *app, size_t i)
{
	WfHits *h = &app->hits;
	wchar_t *dir = wf_path_dup(h->hit[i].dir->path);
	wchar_t *name = wf_path_dup(h->hit[i].name);
	WfStatus st;

	if (!dir || !name) {
		free(dir);
		free(name);
		wf_app_set_error(app, WF_E_NOMEM);
		return;
	}
	close_hits(app);
	st = wf_app_navigate(app, dir, name, SIZE_MAX);
	if (WF_OK != st) {
		wf_app_set_error(app, st);
	}
	free(dir);
	free(name);
}

/* The indexes of the marked results, or of the one under the cursor when none is
 * marked. Caller frees. */
static size_t *chosen(const WfHits *h, size_t *count)
{
	size_t *idx = calloc(h->n ? h->n : 1, sizeof *idx);
	size_t n = 0;

	*count = 0;
	if (!idx) {
		return NULL;
	}
	if (h->nsel) {
		for (size_t i = 0; i < h->n; i++) {
			if (h->sel[i]) {
				idx[n++] = i;
			}
		}
	} else if (h->n) {
		idx[n++] = h->cursor;
	}
	*count = n;
	return idx;
}

/* The chosen results, folder by folder. One sort gives how many different folders
 * they are in (B01), and how many results are in each, so that a slot is made for
 * each folder with room for just its own (B03). Folders are the same when their
 * paths are (ordinal, as everywhere here). */
typedef struct
{
	const WfHitDir *dir;
	size_t k; /* which of the chosen results (an index into idx) */
} Pick;

typedef struct
{
	size_t start, count; /* its picks: pick[start .. start + count) */
	size_t first;        /* the smallest k among them */
} FolderRun;

typedef struct
{
	Pick *pick; /* folder by folder, and in order inside a folder */
	FolderRun
	        *run; /* one for each folder, in the order they first appear */
	size_t n, nrun;
} FolderPlan;

static void plan_free(FolderPlan *p)
{
	free(p->pick);
	free(p->run);
	memset(p, 0, sizeof *p);
}

static int pick_order(const void *a, const void *b)
{
	const Pick *x = a, *y = b;
	int c = x->dir == y->dir ? 0 : wcscmp(x->dir->path, y->dir->path);

	if (c) {
		return c;
	}
	return x->k < y->k ? -1 : x->k > y->k;
}

static bool same_folder(const Pick *a, const Pick *b)
{
	return a->dir == b->dir || !wcscmp(a->dir->path, b->dir->path);
}

static int run_order(const void *a, const void *b)
{
	const FolderRun *x = a, *y = b;

	return x->first < y->first ? -1 : x->first > y->first;
}

static bool folder_plan(
        const WfHits *h, const size_t *idx, size_t n, FolderPlan *p)
{
	bool one = true;
	size_t nrun = 0;

	memset(p, 0, sizeof *p);
	p->pick = malloc((n ? n : 1) * sizeof *p->pick);
	if (!p->pick) {
		return false;
	}
	for (size_t k = 0; k < n; k++) {
		p->pick[k].dir = h->hit[idx[k]].dir;
		p->pick[k].k = k;
		one = one && p->pick[k].dir == p->pick[0].dir;
	}
	p->n = n;
	if (!one) { /* in one folder, which is the usual case, there is nothing to sort */
		qsort(p->pick, n, sizeof *p->pick, pick_order);
	}
	for (size_t k = 0; k < n; k++) {
		nrun += 0 == k || !same_folder(&p->pick[k - 1], &p->pick[k]);
	}
	p->run = malloc((nrun ? nrun : 1) * sizeof *p->run);
	if (!p->run) {
		plan_free(p);
		return false;
	}
	for (size_t k = 0; k < n; k++) {
		if (0 == k || !same_folder(&p->pick[k - 1], &p->pick[k])) {
			p->run[p->nrun].start = k;
			p->run[p->nrun].count = 0;
			p->run[p->nrun].first = p->pick[k].k;
			p->nrun++;
		}
		p->run[p->nrun - 1].count++;
	}
	qsort(p->run, p->nrun, sizeof *p->run, run_order);
	return true;
}

/* The first and one past the last result of the group `i` is in (a group's files
 * come together). */
static void group_range(const WfHits *h, size_t i, size_t *a, size_t *b)
{
	*a = i;
	*b = i + 1;
	while (*a > 0 && h->hit[*a - 1].group == h->hit[i].group) {
		(*a)--;
	}
	while (*b < h->n && h->hit[*b].group == h->hit[i].group) {
		(*b)++;
	}
}

/* Whether, when the chosen results are removed, every group they touch still has a
 * file left: no group is ever removed whole (DR-DRAFT-032). `idx` is in
 * order and a group's files come together, so the results of one group are next
 * to each other: they are counted once for the group, against its size. */
static bool each_group_keeps_one(const WfHits *h, const size_t *idx, size_t n)
{
	size_t k = 0;

	while (k < n) {
		size_t a, b, chosen_here = 0;

		group_range(h, idx[k], &a, &b);
		while (k < n && idx[k] < b) {
			chosen_here++;
			k++;
		}
		if (chosen_here >= b - a) {
			return false; /* every file of it is among them */
		}
	}
	return true;
}

static bool all_proven(const WfHits *h, const size_t *idx, size_t n)
{
	for (size_t k = 0; k < n; k++) {
		const WfHit *x = &h->hit[idx[k]];

		if (!(x->id.kind == WF_ID_STRONG) ||
		        !(x->dir->id.kind == WF_ID_STRONG)) {
			return false;
		}
	}
	return true;
}

static void stage(WfApp *app, bool move)
{
	WfHits *h = &app->hits;
	size_t n, *idx = chosen(h, &n);
	const wchar_t **names;
	FolderPlan plan;

	if (!idx) {
		wf_app_set_error(app, WF_E_NOMEM);
		return;
	}
	if (!n) {
		free(idx);
		return;
	}
	if (!folder_plan(h, idx, n, &plan)) {
		free(idx);
		wf_app_set_error(app, WF_E_NOMEM);
		return;
	}
	if (1 != plan.nrun) {
		char text[128], c[16];

		snprintf(c, sizeof c, "%zu", plan.nrun);
		(void)wf_msgf(text, sizeof text, MSG_HITS_ONE_FOLDER,
		        &WF_ARG_S(c), 1);
		wf_app_set_msg(app, text);
		plan_free(&plan);
		free(idx);
		return;
	}
	plan_free(&plan);
	names = calloc(n, sizeof *names);
	if (!names) {
		free(idx);
		wf_app_set_error(app, WF_E_NOMEM);
		return;
	}
	for (size_t k = 0; k < n; k++) {
		names[k] = h->hit[idx[k]].name;
	}
	{
		wchar_t *dir = wf_path_dup(h->hit[idx[0]].dir->path);

		if (!dir) {
			wf_app_set_error(app, WF_E_NOMEM);
		} else if (wf_app_stage_names(app,
		                   move ? WF_XFER_MOVE : WF_XFER_COPY, dir,
		                   names, n)) {
			close_hits(
			        app); /* the slot is being made; go where it is to be put */
		}
		free(dir);
	}
	free(names);
	free(idx);
}

/* Takes the marked results as one slot for each folder, each with room for the
 * results that are in it and no more. */
static bool pending_make(WfApp *app, const size_t *idx, const FolderPlan *plan)
{
	WfHits *h = &app->hits;

	pending_free(h);
	h->pending = calloc(plan->nrun, sizeof *h->pending);
	if (!h->pending) {
		return false;
	}
	h->n_pending = plan->nrun;
	for (size_t i = 0; i < plan->nrun; i++) {
		wf_slot_init(&h->pending[i]);
	}
	for (size_t i = 0; i < plan->nrun; i++) {
		const FolderRun *fr = &plan->run[i];
		const WfHitDir *d = plan->pick[fr->start].dir;
		WfTransferSlot *s = &h->pending[i];

		s->dir = wf_path_dup(d->path);
		s->dir_id = d->id;
		s->src = calloc(fr->count, sizeof *s->src);
		if (!s->dir || !s->src) {
			pending_free(h);
			return false;
		}
		for (size_t j = 0; j < fr->count; j++) {
			const WfHit *x =
			        &h->hit[idx[plan->pick[fr->start + j].k]];

			s->src[j].name = wf_path_dup(x->name);
			if (!s->src[j].name) {
				pending_free(
				        h); /* s->count is the names made */
				return false;
			}
			s->src[j].snap.id = x->id;
			s->src[j].snap.flags = x->flags;
			s->src[j].snap.size = x->size;
			s->src[j].snap.mtime = x->mtime;
			s->count = j + 1;
		}
	}
	h->items_pending = plan->n;
	return true;
}

static void recycle_yes(WfApp *app, void *ctx)
{
	WfHits *h = &app->hits;
	size_t sent = 0, items = 0, total = h->n_pending;
	char text[128], a[16], b[16];

	(void)ctx;
	for (size_t i = 0; i < h->n_pending; i++) {
		size_t count = h->pending[i].count;

		if (!wf_app_recycle_send(app, &h->pending[i])) {
			break; /* the queue is full or the service is off: it said so */
		}
		sent++;
		items += count;
	}
	snprintf(a, sizeof a, "%zu", sent);
	snprintf(b, sizeof b, "%zu", total);
	if (sent == total) {
		snprintf(a, sizeof a, "%zu", items);
		(void)wf_msgf(text, sizeof text, MSG_HITS_SENT,
		        (WfMsgArg[]){WF_ARG_S(a), WF_ARG_S(b)}, 2);
		wf_app_set_msg(app, text);
	} else {
		(void)wf_msgf(text, sizeof text, MSG_HITS_SENT_PART,
		        (WfMsgArg[]){WF_ARG_S(a), WF_ARG_S(b)}, 2);
		wf_app_set_msg(app, text);
	}
	pending_free(h);
}

static void ask_recycle(WfApp *app, const WfEvent *trigger)
{
	WfHits *h = &app->hits;
	size_t n, *idx = chosen(h, &n), folders;
	FolderPlan plan;
	const char *lines[5];
	char l1[WF_CONFIRM_LINE_BYTES], l2[WF_CONFIRM_LINE_BYTES], a[16], b[16];
	WfBuf first;
	WfStatus st;

	if (!idx) {
		wf_app_set_error(app, WF_E_NOMEM);
		return;
	}
	if (!n) {
		free(idx);
		return;
	}
	if (!wf_copyjob_allowed(WF_CJ_RECYCLE, WF_COLLIDE_STOP)) {
		wf_app_set_msg(app, wf_msg(MSG_JOB_RECYCLE_UNAVAILABLE));
		free(idx);
		return;
	}
	if (!folder_plan(h, idx, n, &plan)) {
		wf_app_set_error(app, WF_E_NOMEM);
		free(idx);
		return;
	}
	folders = plan.nrun;
	if (folders > WF_HITS_MAX_FOLDERS) {
		char text[128];

		snprintf(a, sizeof a, "%u", (unsigned)WF_HITS_MAX_FOLDERS);
		snprintf(b, sizeof b, "%zu", folders);
		(void)wf_msgf(text, sizeof text, MSG_HITS_TOO_MANY_FOLDERS,
		        (WfMsgArg[]){WF_ARG_S(a), WF_ARG_S(b)}, 2);
		wf_app_set_msg(app, text);
		plan_free(&plan);
		free(idx);
		return;
	}
	if (!all_proven(h, idx, n)) {
		wf_app_set_msg(app, wf_msg(MSG_HITS_NO_ID));
		plan_free(&plan);
		free(idx);
		return;
	}
	if (h->dupes && !each_group_keeps_one(h, idx, n)) {
		wf_app_set_msg(app, wf_msg(MSG_DUPES_KEEP_ONE));
		plan_free(&plan);
		free(idx);
		return;
	}
	if (!pending_make(app, idx, &plan)) {
		wf_app_set_error(app, WF_E_NOMEM);
		plan_free(&plan);
		free(idx);
		return;
	}
	plan_free(&plan);
	snprintf(a, sizeof a, "%zu", n);
	snprintf(b, sizeof b, "%zu", folders);
	(void)wf_msgf(l1, sizeof l1, MSG_HITS_RECYCLE_LINE,
	        (WfMsgArg[]){WF_ARG_S(a), WF_ARG_S(b)}, 2);
	wf_buf_init(&first);
	(void)wf_sanitize_utf16(
	        h->hit[idx[0]].name, wcslen(h->hit[idx[0]].name), &first);
	wf_utf8_elide(l2, sizeof l2, wf_buf_cstr(&first), 50);
	wf_buf_free(&first);
	lines[0] = wf_msg(MSG_JOB_RECYCLE_TITLE);
	lines[1] = l1;
	lines[2] = l2;
	lines[3] = wf_msg(MSG_JOB_RECYCLE_NOTE);
	free(idx);
	close_hits(
	        app); /* the question takes the screen; what it asks about is kept */
	st = wf_app_confirm_begin(app, lines, 4, recycle_yes, NULL, trigger);
	if (WF_OK != st) {
		pending_free(h);
		wf_app_set_error(app, st);
	}
}

/* Whether the group of result `i` has another file that is not marked. The
 * group is read outwards from `i` and the reading stops at the first one, which
 * for a group being marked from one end is the next to it; only a group that is
 * all but marked is read to its end. */
static bool another_is_free(const WfHits *h, size_t i)
{
	bool up = true, down = true;

	for (size_t d = 1; up || down; d++) {
		up = up && i >= d && h->hit[i - d].group == h->hit[i].group;
		down = down && i + d < h->n &&
		        h->hit[i + d].group == h->hit[i].group;
		if ((up && !h->sel[i - d]) || (down && !h->sel[i + d])) {
			return true;
		}
	}
	return false;
}

static void toggle(WfApp *app, WfHits *h, size_t i)
{
	if (h->dupes &&
	        !h->sel[i]) { /* the last file of a group cannot be marked */
		if (!another_is_free(h, i)) {
			wf_app_set_msg(app, wf_msg(MSG_DUPES_LAST_COPY));
			return;
		}
	}
	h->sel[i] = !h->sel[i];
	h->nsel += h->sel[i] ? 1u : 0u;
	h->nsel -= h->sel[i] ? 0u : 1u;
}

static WfAction hits_event(WfApp *app, const WfEvent *ev)
{
	WfHits *h = &app->hits;
	size_t page = visible_rows(app);

	if (WF_EV_RESIZE == ev->type) {
		keep_cursor_on_screen(app);
		return WF_ACT_REDRAW;
	}
	if (WF_EV_RELEASE == ev->type) {
		return WF_ACT_NONE;
	}
	app->msg[0] = '\0'; /* what was said stays until the next key */
	if (h->dupes &&
	        ((WF_EV_CTRL == ev->type && 'a' == ev->ch) ||
	                (WF_EV_CHAR == ev->type &&
	                        ('y' == ev->ch || 'x' == ev->ch)))) {
		wf_app_set_msg(app, wf_msg(MSG_DUPES_ONLY_DELETE));
		return WF_ACT_REDRAW;
	}
	if (WF_EV_CTRL == ev->type && 'a' == ev->ch) {
		for (size_t i = 0; i < h->n; i++) {
			h->sel[i] = true;
		}
		h->nsel = h->n;
		return WF_ACT_REDRAW;
	}
	if (WF_EV_CHAR == ev->type) {
		switch (ev->ch) {
		case 'j':
			if (h->cursor + 1 < h->n) {
				h->cursor++;
			}
			break;
		case 'k':
			if (h->cursor) {
				h->cursor--;
			}
			break;
		case ' ':
			if (h->n) {
				toggle(app, h, h->cursor);
				if (h->cursor + 1 < h->n) {
					h->cursor++;
				}
			}
			break;
		case 'y': stage(app, false); return WF_ACT_REDRAW;
		case 'x': stage(app, true); return WF_ACT_REDRAW;
		default:  return WF_ACT_NONE;
		}
		keep_cursor_on_screen(app);
		return WF_ACT_REDRAW;
	}
	if (WF_EV_KEY != ev->type) {
		return WF_ACT_NONE;
	}
	switch (ev->key) {
	case WF_KEY_DOWN:
		if (h->cursor + 1 < h->n) {
			h->cursor++;
		}
		break;
	case WF_KEY_UP:
		if (h->cursor) {
			h->cursor--;
		}
		break;
	case WF_KEY_PGDN:
		h->cursor = h->cursor + page < h->n ? h->cursor + page
		                                    : (h->n ? h->n - 1 : 0);
		break;
	case WF_KEY_PGUP:
		h->cursor = h->cursor > page ? h->cursor - page : 0;
		break;
	case WF_KEY_HOME: h->cursor = 0; break;
	case WF_KEY_END:  h->cursor = h->n ? h->n - 1 : 0; break;
	case WF_KEY_ENTER:
		if (h->n) {
			show_in_folder(app, h->cursor);
		}
		return WF_ACT_REDRAW;
	case WF_KEY_DELETE: ask_recycle(app, ev); return WF_ACT_REDRAW;
	case WF_KEY_ESC:
		if (h->running) {
			stop_worker(app);
			h->stopped = true;
			h->done = true;
		} else {
			close_hits(app);
		}
		return WF_ACT_REDRAW;
	default: return WF_ACT_NONE;
	}
	keep_cursor_on_screen(app);
	return WF_ACT_REDRAW;
}

const WfMode WF_MODE_HITS = {"HITS", hits_draw, hits_event};
