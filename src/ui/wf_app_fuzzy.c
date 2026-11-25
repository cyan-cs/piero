/*
 * Fuzzy search of the names in the folder on screen (s). DR-DRAFT-023.
 *
 * The prompt is the bottom line (where the filter's is); the results take the
 * place of the current pane's rows, best first, with the matched characters
 * marked. The folder's own cursor follows the chosen result, so the preview
 * shows it, and Esc puts the cursor and the scroll back by the name of the
 * entry that had them. Nothing is selected, opened or changed by a search.
 *
 * The names are read from the listing the view holds: nothing is copied. The
 * view's guard (wf_view.h) calls wf_app_fuzzy_guard() before the listing
 * changes; it stops the worker from reading and drops the answer, and the next
 * tick asks again for the same letters in the new listing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../base/wf_mem.h"
#include "../text/wf_text.h"
#include "wf_app_priv.h"

#define K_GROWTH 4u /* a request for more asks for this many times as many */

void wf_app_set_fuzzy(WfApp *app, WfFuzzyPort port)
{
	app->fuzzy_port = port;
}

static void release_worker(WfApp *app)
{
	if (app->fuzzy_port.release) {
		app->fuzzy_port.release(app->fuzzy_port.ctx);
	}
}

static void drop_hits(WfFuzzy *z)
{
	free(z->hit);
	z->hit = NULL;
	z->nhit = z->matched = 0;
	z->sel = z->top = 0;
}

static wchar_t *dup_name(const wchar_t *s)
{
	size_t n = wcslen(s) + 1;
	wchar_t *d = wf_malloc_array(n, sizeof *d);

	if (d) {
		memcpy(d, s, n * sizeof *d);
	}
	return d;
}

static uint32_t next_gen(WfFuzzy *z)
{
	z->gen++;
	if (0 == z->gen) {
		z->gen = 1;
	}
	return z->gen;
}

/* The row of `vis` that holds the entry called `name`, or SIZE_MAX. */
static size_t row_of(const WfView *v, const wchar_t *name)
{
	for (size_t i = 0; v->dir && name && i < v->nvis; i++) {
		if (0 == wcscmp(v->dir->e[v->vis[i]].name, name)) {
			return i;
		}
	}
	return SIZE_MAX;
}

static void restore_cursor(WfApp *app)
{
	WfFuzzy *z = &app->fz;
	size_t row = row_of(&app->cur, z->orig_name);

	if (SIZE_MAX != row) {
		app->cur.cursor = row;
		app->cur.top = z->orig_top;
		app->cur.top = wf_view_top(&app->cur, wf_app_body_height(app));
	}
}

/* Keeps the chosen row inside the rows the pane shows. */
static void scroll_to_sel(WfApp *app)
{
	WfFuzzy *z = &app->fz;
	size_t h = (size_t)wf_app_body_height(app);

	if (z->sel < z->top) {
		z->top = z->sel;
	} else if (z->sel >= z->top + h) {
		z->top = z->sel - h + 1;
	}
}

/* The folder's cursor goes where the chosen result is (so the preview
 * follows); with no result it goes back to where it was. */
static void mirror(WfApp *app)
{
	WfFuzzy *z = &app->fz;
	WfView *v = &app->cur;

	if (z->nhit && z->sel < z->nhit && z->hit[z->sel].index < v->nvis) {
		v->cursor = z->hit[z->sel].index;
		v->top = wf_view_top(v, wf_app_body_height(app));
	} else {
		restore_cursor(app);
	}
	wf_app_preview_sync(app, false);
}

/* The answer to the query on screen is not here yet: a search is out, or the
 * listing changed and the search has not been asked again. */
static bool answer_out(const WfFuzzy *z)
{
	return z->pending || z->dirty;
}

static void finish(WfApp *app, bool restore)
{
	WfFuzzy *z = &app->fz;

	release_worker(app);
	next_gen(z); /* nothing that is still out can be taken */
	if (restore) {
		restore_cursor(app);
	}
	drop_hits(z);
	free(z->orig_name);
	free(z->keep_name);
	z->orig_name = z->keep_name = NULL;
	wf_fz_scratch_free(z->scratch);
	z->scratch = NULL;
	z->active = z->pending = z->dirty = z->more = z->advance = false;
	z->query[0] = L'\0';
	app->mode = &WF_MODE_NORMAL;
	wf_app_preview_sync(app, false);
}

void wf_app_fuzzy_destroy(WfApp *app)
{
	WfFuzzy *z = &app->fz;

	release_worker(app);
	drop_hits(z);
	free(z->orig_name);
	free(z->keep_name);
	wf_fz_scratch_free(z->scratch);
	{
		uint32_t gen = z->gen; /* generations never go back */
		unsigned dropped = z->dropped;

		memset(z, 0, sizeof *z);
		z->gen = gen;
		z->dropped = dropped;
	}
}

/* Applies an answer of the newest request. Takes the hits. */
static void take(WfApp *app, WfFzAnswer *a)
{
	WfFuzzy *z = &app->fz;
	bool advance = z->advance;
	size_t sel = z->sel, top = z->top;

	drop_hits(z);
	z->sel =
	        sel; /* more of the same letters: the first ones are the same */
	z->top = top;
	z->hit = a->hit;
	z->nhit = a->nhit;
	z->matched = a->matched;
	a->hit = NULL;
	a->nhit = 0;
	z->pending = z->more = z->advance = false;
	if (z->keep_name) {
		/* the listing changed: stay on the same name if it is there */
		for (size_t i = 0; i < z->nhit; i++) {
			const WfView *v = &app->cur;

			if (z->hit[i].index < v->nvis &&
			        0 ==
			                wcscmp(v->dir
			                                ->e[v->vis[z->hit[i]
			                                                .index]]
			                                .name,
			                        z->keep_name)) {
				z->sel = i;
				break;
			}
		}
		free(z->keep_name);
		z->keep_name = NULL;
	}
	if (advance && z->sel + 1 < z->nhit) {
		z->sel++;
	}
	if (z->sel >= z->nhit) {
		z->sel = 0;
	}
	scroll_to_sel(app);
	mirror(app);
}

void wf_app_on_fuzzy(WfApp *app, WfFzAnswer *a)
{
	WfFuzzy *z = &app->fz;

	if (!a) {
		return;
	}
	if (!z->active || a->gen != z->gen) {
		z->dropped++;
		wf_fz_answer_free(a);
		return;
	}
	if (WF_OK != a->st) {
		z->pending = false;
		wf_fz_answer_free(a);
		wf_app_set_msg(app, wf_msg(MSG_XFER_NOMEM));
		return;
	}
	take(app, a);
}

static bool run_here(WfApp *app, const WfFzJob *job)
{
	WfFuzzy *z = &app->fz;
	WfFzAnswer a = {0};
	WfFzTopK top;
	WfFzStats st = {0};
	WfFzScratch *x = z->scratch ? z->scratch : wf_fz_scratch_new();

	a.gen = z->gen;
	if (!x || WF_OK != wf_fz_topk_init(&top, job->k)) {
		if (x != z->scratch) {
			wf_fz_scratch_free(x);
		}
		return false;
	}
	a.st = wf_fz_search(&job->query, job->e, job->vis, job->nvis, x, &top,
	        NULL, NULL, &st);
	a.hit = top.hit;
	a.nhit = top.n;
	a.matched = st.matched;
	a.scanned = st.scanned;
	if (x != z->scratch) {
		wf_fz_scratch_free(x);
	}
	if (WF_OK != a.st) {
		wf_fz_answer_free(&a);
		return false;
	}
	take(app, &a);
	return true;
}

/* Asks for the best `k` names for the letters typed so far. */
static void request(WfApp *app, size_t k)
{
	WfFuzzy *z = &app->fz;
	WfView *v = &app->cur;
	WfFzJob job;

	next_gen(z);
	z->pending = z->dirty = false;
	if (!wf_fz_query_set(&job.query, z->query, wcslen(z->query)) ||
	        !v->dir || 0 == v->nvis) {
		release_worker(app);
		drop_hits(z);
		z->more = z->advance = false;
		mirror(app);
		return;
	}
	if (k > v->nvis) {
		k = v->nvis;
	}
	job.e = v->dir->e;
	job.vis = v->vis;
	job.nvis = v->nvis;
	job.k = z->k = k;
	if (v->nvis <=
	        WF_FZ_SYNC_MAX) { /* small: quicker here than to hand over */
		if (!run_here(app, &job)) {
			drop_hits(z);
			wf_app_set_msg(app, wf_msg(MSG_XFER_NOMEM));
		}
		return;
	}
	/* Large: only the worker searches. If it cannot take the request, that is
	 * said; the whole list is never searched on this thread instead. */
	if (app->fuzzy_port.start &&
	        WF_OK ==
	                app->fuzzy_port.start(
	                        app->fuzzy_port.ctx, z->gen, &job)) {
		z->pending = true;
		return;
	}
	drop_hits(z);
	z->more = z->advance = false;
	mirror(app);
	wf_app_set_msg(app, wf_msg(MSG_FUZZY_NO_WORKER));
}

void wf_app_fuzzy_guard(void *ctx)
{
	WfApp *app = ctx;
	WfFuzzy *z = &app->fz;

	if (!z->active) {
		return;
	}
	release_worker(app);
	if (!z->dirty && !z->keep_name && z->nhit && z->sel < z->nhit &&
	        z->hit[z->sel].index < app->cur.nvis) {
		z->keep_name = dup_name(
		        app->cur.dir->e[app->cur.vis[z->hit[z->sel].index]]
		                .name);
	}
	next_gen(z);
	drop_hits(z);
	z->pending = false;
	z->dirty = true;
}

bool wf_app_fuzzy_redo_due(const WfApp *app)
{
	return app->fz.active && app->fz.dirty;
}

bool wf_app_fuzzy_tick(WfApp *app)
{
	WfFuzzy *z = &app->fz;

	if (z->active && &WF_MODE_FUZZY != app->mode) {
		/* something else took the screen (a question, say): the search is
		 * over; it must not go on reading the list behind it */
		wf_app_fuzzy_destroy(app);
		return false;
	}
	if (!z->active || !z->dirty) {
		return false;
	}
	if (z->cwd_version != app->cwd_version || !app->cur.dir) {
		/* another folder: what was typed is about names that are gone */
		finish(app, false);
		return true;
	}
	request(app, z->k ? z->k : WF_FZ_K_DEFAULT);
	return true;
}

void wf_app_cmd_fuzzy(WfApp *app)
{
	WfFuzzy *z = &app->fz;
	const WfEntry *cur = wf_view_current(&app->cur);

	if (!app->cur.dir || 0 == app->cur.nvis || !cur) {
		wf_app_set_msg(app, wf_msg(MSG_FUZZY_EMPTY_LIST));
		return;
	}
	if (z->active) {
		wf_app_fuzzy_destroy(app); /* one left open: nothing leaks */
	}
	z->orig_name = dup_name(cur->name);
	z->scratch = wf_fz_scratch_new();
	if (!z->orig_name || !z->scratch) {
		wf_app_fuzzy_destroy(app);
		wf_app_set_msg(app, wf_msg(MSG_XFER_NOMEM));
		return;
	}
	z->orig_top = app->cur.top;
	z->cwd_version = app->cwd_version;
	z->k = WF_FZ_K_DEFAULT;
	z->active = true;
	app->mode = &WF_MODE_FUZZY;
}

static bool scalar_valid(uint32_t cp)
{
	return cp >= 32 && 127 != cp && cp <= 0x10FFFF &&
	        !(cp >= 0xD800 && cp <= 0xDFFF);
}

static bool query_append(WfFuzzy *z, uint32_t cp)
{
	size_t n = wcslen(z->query), need = cp >= 0x10000 ? 2 : 1;

	if (!scalar_valid(cp) || n + need >= WF_FILTER_MAX) {
		return false;
	}
	if (2 == need) {
		cp -= 0x10000;
		z->query[n++] = (wchar_t)(0xD800 + (cp >> 10));
		z->query[n++] = (wchar_t)(0xDC00 + (cp & 0x3FF));
	} else {
		z->query[n++] = (wchar_t)cp;
	}
	z->query[n] = L'\0';
	return true;
}

static void query_backspace(WfFuzzy *z)
{
	size_t n = wcslen(z->query);

	if (0 == n) {
		return;
	}
	n--;
	if (n > 0 && (uint16_t)z->query[n] >= 0xDC00 &&
	        (uint16_t)z->query[n] <= 0xDFFF) {
		n--; /* the whole pair */
	}
	z->query[n] = L'\0';
}

/* The query changed: the answer for the new one starts at the best result. */
static void requery(WfApp *app)
{
	WfFuzzy *z = &app->fz;

	free(z->keep_name);
	z->keep_name = NULL;
	z->more = z->advance = false;
	z->sel = z->top = 0;
	request(app, WF_FZ_K_DEFAULT);
}

static void move_down(WfApp *app)
{
	WfFuzzy *z = &app->fz;

	if (z->sel + 1 < z->nhit) {
		z->sel++;
	} else if (z->nhit < z->matched && !z->pending) {
		/* the best K are all shown: ask for four times as many; the first
		 * ones come back the same, and the cursor goes on from there */
		size_t k = z->nhit > SIZE_MAX / K_GROWTH ? z->matched
		                                         : z->nhit * K_GROWTH;

		z->more = z->advance = true;
		request(app, k);
		return;
	}
	scroll_to_sel(app);
	mirror(app);
}

static WfAction fuzzy_event(WfApp *app, const WfEvent *ev)
{
	WfFuzzy *z = &app->fz;

	if (!z->active) {
		app->mode = &WF_MODE_NORMAL;
		return WF_ACT_REDRAW;
	}
	if (WF_EV_CHAR == ev->type) {
		if (query_append(z, ev->ch)) {
			requery(app);
		}
		return WF_ACT_REDRAW;
	}
	if (WF_EV_KEY != ev->type) {
		return WF_ACT_NONE;
	}
	switch (ev->key) {
	case WF_KEY_BACKSPACE:
		query_backspace(z);
		requery(app);
		return WF_ACT_REDRAW;
	case WF_KEY_UP:
		if (z->sel > 0) {
			z->sel--;
			scroll_to_sel(app);
			mirror(app);
		}
		return WF_ACT_REDRAW;
	case WF_KEY_DOWN: move_down(app); return WF_ACT_REDRAW;
	case WF_KEY_CTRL_UP:
		z->sel = 0;
		scroll_to_sel(app);
		mirror(app);
		return WF_ACT_REDRAW;
	case WF_KEY_CTRL_DOWN:
		z->sel = z->nhit ? z->nhit - 1 : 0;
		scroll_to_sel(app);
		mirror(app);
		return WF_ACT_REDRAW;
	case WF_KEY_ENTER:
		if (answer_out(z)) {
			/* the entries on screen are for an older query (or none is
			 * there yet): Enter waits for the answer to this one */
			return WF_ACT_NONE;
		}
		/* the cursor is on the chosen entry; with no result it goes back */
		finish(app, 0 == z->nhit);
		return WF_ACT_REDRAW;
	case WF_KEY_ESC: finish(app, true); return WF_ACT_REDRAW;
	default:         return WF_ACT_NONE;
	}
}

static WfStyle marked(WfStyle st, bool on_cursor)
{
	if (!on_cursor) {
		st = wf_theme_styles()->match;
	}
	st.attributes |= WF_ATTR_BOLD | WF_ATTR_UNDERLINE;
	return st;
}

/* One result: the name a character at a time (a character is never split),
 * the matched ones marked, clipped at `w` cells. Returns the cells used. */
static int draw_name(WfFrame *f,
        int x,
        int y,
        int w,
        const wchar_t *name,
        const WfFzQuery *q,
        WfFzScratch *scratch,
        const WfStyle *base,
        bool on_cursor)
{
	uint32_t units[WF_FZ_QUERY_MAX];
	size_t len = wcslen(name), i = 0, m = 0;
	bool have =
	        scratch && wf_fz_positions(q, name, len, scratch, units, NULL);
	int used = 0;
	WfBuf s;

	wf_buf_init(&s);
	while (i < len && used < w) {
		size_t u = wf_fz_cp_units(name, len, i);
		bool hit = have && m < q->n && units[m] == i;
		WfStyle st = hit ? marked(*base, on_cursor) : *base;

		wf_buf_clear(&s);
		if (WF_OK != wf_sanitize_utf16(name + i, u, &s)) {
			break;
		}
		if (hit) {
			m++;
		}
		used += wf_frame_put(
		        f, x + used, y, wf_buf_cstr(&s), s.len, &st, w - used);
		i += u;
	}
	wf_buf_free(&s);
	return used;
}

static void draw_results(const WfApp *app, WfFrame *f, const WfLayout *lay)
{
	const WfFuzzy *z = &app->fz;
	const WfView *v = &app->cur;
	int x = lay->pane[WF_PANE_CURRENT].x, w = lay->pane[WF_PANE_CURRENT].w;
	int y0 = lay->body_y, h = lay->body_h;
	const WfNameSet *marks = wf_sel_folder(&app->sel, app->cwd);
	WfFzQuery q;
	size_t top = z->top;

	wf_frame_fill(f, x, y0, w, h, ' ', &WF_STYLE_BASE);
	if (0 == z->nhit) {
		if (!z->pending && !z->dirty) {
			wf_app_put_fit(f, x + 2, y0, wf_msg(MSG_FUZZY_NONE),
			        &wf_theme_styles()->dim, w - 2);
		}
		return;
	}
	if (!wf_fz_query_set(&q, z->query, wcslen(z->query))) {
		return;
	}
	if (z->sel < top) {
		top = z->sel;
	} else if (z->sel >= top + (size_t)h) {
		top = z->sel - (size_t)h + 1;
	}
	for (int row = 0; row < h && top + (size_t)row < z->nhit; row++) {
		size_t i = top + (size_t)row;
		size_t index = z->hit[i].index;
		const WfEntry *e;
		bool is_dir, cur = i == z->sel;
		WfStyle st;
		int cx = x, room, used;

		if (index >= v->nvis) {
			break;
		}
		e = &v->dir->e[v->vis[index]];
		is_dir = 0 != (e->flags & WF_ENT_DIR);
		st = is_dir ? wf_theme_styles()->directory : WF_STYLE_BASE;
		if (cur) {
			st = wf_theme_styles()->selection;
			if (is_dir) {
				st.attributes |= WF_ATTR_BOLD;
			}
			wf_frame_fill(f, x, y0 + row, w, 1, ' ', &st);
			wf_frame_put(f, cx, y0 + row, ">", 1, &st, 1);
		}
		if (wf_nameset_has(marks, e->name)) {
			wf_frame_put(f, cx + 1, y0 + row, "*", 1, &st, 1);
		}
		cx += 2;
		room = w - 2 - (is_dir ? 1 : 0);
		if (room <= 0) {
			continue;
		}
		used = draw_name(f, cx, y0 + row, room, e->name, &q, z->scratch,
		        &st, cur);
		if (is_dir) {
			wf_frame_put(f, cx + used, y0 + row, "/", 1, &st, 1);
		}
	}
}

static void draw_prompt(const WfApp *app, WfFrame *f)
{
	const WfFuzzy *z = &app->fz;
	WfBuf s;
	char right[96];
	int rw = 0, lcells = 0;
	bool qok;
	const char *label = wf_msg(MSG_FUZZY_PROMPT);

	if (f->rows < 2) {
		return;
	}
	wf_frame_fill(f, 0, f->rows - 2, f->cols, 1, ' ', &wf_app_style_bar);
	wf_buf_init(&s);
	qok = WF_OK == wf_sanitize_utf16(z->query, wcslen(z->query), &s);
	right[0] = '\0';
	if (z->query[0]) {
		if (answer_out(z)) {
			snprintf(right, sizeof right, "%s",
			        wf_msg(MSG_FUZZY_BUSY));
		} else if (0 == z->matched) {
			snprintf(right, sizeof right, "%s",
			        wf_msg(MSG_FUZZY_NONE));
		} else if (z->nhit < z->matched) {
			(void)wf_msgf(right, sizeof right, MSG_FUZZY_SHOWN,
			        (WfMsgArg[]){WF_ARG_N(z->nhit),
			                WF_ARG_N(z->matched)},
			        2);
		} else {
			(void)wf_msgf(right, sizeof right,
			        wf_msg_count(MSG_FUZZY_MATCHES_ONE,
			                MSG_FUZZY_MATCHES_OTHER,
			                (long long)z->matched),
			        &WF_ARG_N(z->matched), 1);
		}
	}
	rw = wf_utf8_cells(right, strlen(right));
	lcells = wf_utf8_cells(label, strlen(label));
	wf_frame_put(f, 0, f->rows - 2, label, strlen(label), &wf_app_style_bar,
	        f->cols - (rw ? rw + 1 : 0));
	if (qok) {
		/* what was typed last stays on screen, with the cursor, whatever
		 * the counter on the right takes */
		int room = f->cols - lcells - (rw ? rw + 1 : 0);
		int cx = wf_app_put_input_tail(f, lcells, f->rows - 2,
		        wf_buf_cstr(&s), s.len, room, &wf_app_style_bar);

		wf_frame_set_cursor(f, cx, f->rows - 2);
		if (cx < f->cols - (rw ? rw + 1 : 0)) {
			wf_frame_put(f, cx, f->rows - 2, "_", 1,
			        &wf_app_style_bar, 1);
		}
	}
	if (rw && rw < f->cols) {
		wf_frame_put(f, f->cols - rw, f->rows - 2, right, strlen(right),
		        &wf_app_style_bar, rw);
	}
	wf_buf_free(&s);
}

static void fuzzy_draw(const WfApp *app, WfFrame *f)
{
	WfLayout lay;

	wf_layout_compute(f->cols, f->rows, &app->ratios, &lay);
	if (!lay.minimal && lay.pane[WF_PANE_CURRENT].w > 0 && app->cur.dir &&
	        app->fz.query[0]) {
		draw_results(app, f, &lay);
	}
	draw_prompt(app, f);
}

const WfMode WF_MODE_FUZZY = {"FUZZY", fuzzy_draw, fuzzy_event};
