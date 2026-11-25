#include "wf_egitpane.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../i18n/wf_i18n.h"
#include "../render/wf_theme.h"
#include "wf_esession_internal.h"

#define CONTEXT 3u

void wf_gp_free(WfGitPane *p)
{
	wf_gitblob_release(p->base);
	wf_linediff_free(&p->diff);
	free(p->rows);
	memset(p, 0, sizeof *p);
}

typedef struct
{
	WfGpRow *r;
	size_t n, cap;
	bool failed;
} Rows;

static void push(Rows *rs, uint8_t kind, uint32_t hunk, size_t a, size_t b)
{
	if (rs->failed) {
		return;
	}
	if (rs->n == rs->cap) {
		size_t cap = rs->cap ? rs->cap * 2 : 64;
		WfGpRow *g = realloc(rs->r, cap * sizeof *g);

		if (!g) {
			rs->failed = true;
			return;
		}
		rs->r = g;
		rs->cap = cap;
	}
	rs->r[rs->n].kind = kind;
	rs->r[rs->n].hunk = hunk;
	rs->r[rs->n].a = a;
	rs->r[rs->n].b = b;
	rs->n++;
}

WfStatus wf_gp_build_rows(WfGitPane *p, size_t context)
{
	const WfLineDiff *d = &p->diff;
	Rows rs = {0};
	size_t i = 0;

	free(p->rows);
	p->rows = NULL;
	p->nrows = 0;
	while (i < d->n) {
		size_t j = i, start_b, end_b;
		const WfDiffHunk *first = &d->h[i];

		/* hunks that are close are shown as one */
		while (j + 1 < d->n &&
		        d->h[j + 1].b - (d->h[j].b + d->h[j].bn) <=
		                2 * context) {
			j++;
		}
		start_b = first->b > context ? first->b - context : 0;
		push(&rs, 'H', (uint32_t)i, first->a - (first->b - start_b),
		        start_b);
		for (size_t b = start_b; b < first->b; b++) {
			push(&rs, 'C', (uint32_t)i, first->a - (first->b - b),
			        b);
		}
		for (size_t k = i; k <= j; k++) {
			const WfDiffHunk *h = &d->h[k];

			for (size_t a = h->a; a < h->a + h->an; a++) {
				push(&rs, '-', (uint32_t)k, a, h->b);
			}
			for (size_t b = h->b; b < h->b + h->bn; b++) {
				push(&rs, '+', (uint32_t)k, h->a + h->an, b);
			}
			end_b = h->b + h->bn;
			if (k < j) {
				for (size_t b = end_b; b < d->h[k + 1].b; b++) {
					push(&rs, 'C', (uint32_t)k,
					        h->a + h->an + (b - end_b), b);
				}
			} else {
				for (size_t b = end_b;
				        b < end_b + context && b < d->b_n;
				        b++) {
					push(&rs, 'C', (uint32_t)k,
					        h->a + h->an + (b - end_b), b);
				}
			}
		}
		i = j + 1;
	}
	if (rs.failed) {
		free(rs.r);
		return WF_E_NOMEM;
	}
	p->rows = rs.r;
	p->nrows = rs.n;
	return WF_OK;
}

size_t wf_gp_top_for(const WfGitPane *p, size_t cursor, size_t height)
{
	size_t at = 0, top, limit;

	if (!p->nrows) {
		return 0;
	}
	while (at < p->nrows && p->rows[at].b < cursor) {
		at++;
	}
	if (at == p->nrows) {
		at = p->nrows - 1;
	}
	top = at > height / 3 ? at - height / 3 : 0;
	limit = p->nrows > height ? p->nrows - height : 0;
	return top < limit ? top : limit;
}

void wf_es_set_git(WfEditSession *s, WfGitPort port)
{
	s->git_port = port;
}

static uint32_t next_gen(WfEditSession *s)
{
	if (++s->git_gen == 0) {
		s->git_gen = 1;
	}
	return s->git_gen;
}

/* Asks for the base of one buffer. The worker keeps one request of this kind: a
 * newer one for another buffer replaces it, so the buffer that was replaced has
 * to ask again when it is the one on screen (see wf_es_git_tick). */
static void ask_base(WfEditSession *s, WfBuffer *b, const wchar_t *path)
{
	WfGitPane *p = &b->git;

	p->base_gen = next_gen(s);
	p->state = WF_GP_ASKED;
	s->git_base_owner = p->id;
	if (WF_OK !=
	        s->git_port.base(s->git_port.ctx, p->base_gen, p->id, path)) {
		p->state = WF_GP_OFF;
	}
}

void wf_es_git_assign(WfEditSession *s, WfBuffer *b, const wchar_t *path)
{
	WfGitPane *p = &b->git;
	uint64_t id = p->id;

	wf_gp_free(p);
	p->id = id;
	if (!s->git_port.base || !path) {
		return;
	}
	ask_base(s, b, path);
}

int wf_es_git_cols(const WfEditSession *s, const WfBuffer *b)
{
	if (!b || WF_GP_READY != b->git.state || PR_HELP == s->pk ||
	        s->cols < WF_GP_MIN_COLS || wf_es_md_cols(s, b)) {
		return 0;
	}
	return 1 + (s->cols - 1) / 2;
}

/* A change of the text moves the deadline; nothing else is done here. */
static void note(WfEditSession *s, WfBuffer *b)
{
	WfGitPane *p = &b->git;

	if (WF_GP_READY != p->state) {
		return;
	}
	if (!p->seen_valid || p->seen != b->ed.text.gen) {
		p->seen = b->ed.text.gen;
		p->seen_valid = true;
		p->due_ms = s->now_ms +
		        (p->has_diff || p->too_many ? WF_GP_DEBOUNCE_MS : 0);
		if (!p->due_ms) {
			p->due_ms = 1;
		}
	}
}

uint32_t wf_es_git_wait(const WfEditSession *s, uint64_t now_ms)
{
	const WfBuffer *b = s->n ? s->buf[s->cur] : NULL;

	if (!b || !b->git.due_ms || !wf_es_git_cols(s, b)) {
		return UINT32_MAX;
	}
	return now_ms >= b->git.due_ms ? 0 : (uint32_t)(b->git.due_ms - now_ms);
}

/* The text as one string, lines joined by LF; NULL when it is over the limit
 * or there is no memory. */
static char *flatten(const WfEText *t, size_t *n)
{
	size_t total = 0, k = 0;
	char *out;

	if (t->bytes + t->n > WF_GP_TEXT_MAX) {
		return NULL;
	}
	for (size_t i = 0; i < t->n; i++) {
		total += t->lines[i].len + 1;
	}
	out = malloc(total + 1);
	if (!out) {
		return NULL;
	}
	for (size_t i = 0; i < t->n; i++) {
		memcpy(out + k, t->lines[i].s, t->lines[i].len);
		k += t->lines[i].len;
		if (i + 1 < t->n) { /* the empty last line is the final break */
			out[k++] = '\n';
		}
	}
	out[k] = '\0';
	*n = k;
	return out;
}

/* No diff is shown: what was is gone, and no row of it is kept. */
static void clear_diff(WfGitPane *p)
{
	wf_linediff_free(&p->diff);
	free(p->rows);
	p->rows = NULL;
	p->nrows = 0;
	p->has_diff = false;
}

bool wf_es_git_tick(WfEditSession *s, uint64_t now_ms)
{
	WfBuffer *b = s->n ? s->buf[s->cur] : NULL;
	WfGitPane *p;
	char *text;
	size_t n = 0;
	WfStatus st;

	/* the base of this buffer was asked for and then the worker was asked for
	 * another buffer's: nothing will come for this one, so it asks again */
	if (b && WF_GP_ASKED == b->git.state && b->path && s->git_port.base &&
	        s->git_base_owner != b->git.id) {
		ask_base(s, b, b->path);
	}
	if (!b || !b->git.due_ms || now_ms < b->git.due_ms ||
	        !wf_es_git_cols(s, b)) {
		return false;
	}
	p = &b->git;
	p->due_ms = 0;
	text = flatten(&b->ed.text, &n);
	if (!text) { /* too large to compare, or no memory: say so, once */
		bool changed = p->has_diff || !p->too_many;

		clear_diff(p);
		p->too_many = true;
		p->diff_text_gen = b->ed.text.gen;
		p->diff_gen =
		        next_gen(s); /* an older answer no longer counts */
		return changed;
	}
	p->diff_gen = next_gen(s);
	p->diff_text_gen = b->ed.text.gen;
	st = s->git_port.diff(
	        s->git_port.ctx, p->diff_gen, p->id, p->base, text, n);
	free(text);
	if (WF_OK != st) {
		/* No answer will come. The diff on screen is of older text, so it is
		 * not left up; the request is made again after a pause. */
		bool changed = p->has_diff;

		clear_diff(p);
		p->too_many = false;
		p->due_ms = now_ms + WF_GP_RETRY_MS;
		return changed;
	}
	return false;
}

bool wf_es_on_git(WfEditSession *s, WfGitAnswer *a)
{
	WfBuffer *b = NULL;
	WfGitPane *p;

	if (!a || (WF_GIT_A_BASE != a->kind && WF_GIT_A_DIFF != a->kind)) {
		return false;
	}
	for (size_t i = 0; i < s->n; i++) {
		if (s->buf[i]->git.id == a->id) {
			b = s->buf[i];
		}
	}
	if (!b) {
		return false; /* the buffer was closed meanwhile */
	}
	p = &b->git;
	if (WF_GIT_A_BASE == a->kind) {
		if (a->gen != p->base_gen || WF_GP_ASKED != p->state) {
			return false;
		}
		if (WF_GITR_OK != a->state || !a->base) {
			p->state = WF_GP_OFF;
			return false;
		}
		wf_gitblob_release(p->base);
		p->base = a->base;
		a->base = NULL; /* ours now */
		p->state = WF_GP_READY;
		p->seen_valid =
		        false; /* the first diff is made as soon as the idle turn comes */
		p->due_ms = 1;
		return true;
	}
	if (a->gen != p->diff_gen || WF_GP_READY != p->state) {
		return false; /* not the newest request: never shown */
	}
	clear_diff(p);
	p->too_many = false;
	if (WF_GITR_OK == a->state) {
		p->diff = a->diff;
		memset(&a->diff, 0, sizeof a->diff); /* ours now */
		p->has_diff = true;
		if (WF_OK != wf_gp_build_rows(p, CONTEXT)) {
			wf_linediff_free(&p->diff);
			p->has_diff = false;
			p->too_many = true;
		}
	} else if (WF_GITR_TOO_LARGE == a->state) {
		p->too_many = true;
	}
	return true;
}

/* A line of either text, for the row. */
static void put_row(
        WfFrame *f, int x, int y, int w, const WfGitPane *p, const WfGpRow *r)
{
	const WfLineDiff *d = &p->diff;
	const WfThemeStyles *t = wf_theme_styles();
	const WfDiffLine *l = NULL;
	const char *text = NULL;
	char head[64];
	const WfStyle *st = &t->base;
	char mark = ' ';

	switch (r->kind) {
	case 'H':
		snprintf(head, sizeof head, "@@ -%zu +%zu @@", r->a + 1,
		        r->b + 1);
		wf_frame_put(f, x, y, head, strlen(head), &t->accent, w);
		return;
	case '-':
		l = &d->a_line[r->a];
		text = d->a_text;
		st = &t->error;
		mark = '-';
		break;
	case '+':
		l = &d->b_line[r->b];
		text = d->b_text;
		st = &t->note;
		mark = '+';
		break;
	default:
		l = &d->b_line[r->b];
		text = d->b_text;
		st = &t->dim;
		break;
	}
	wf_frame_put(f, x, y, &mark, 1, st, 1);
	if (w > 1) {
		size_t len = l->len;

		if (len && '\r' == text[l->off + len - 1]) {
			len--; /* the line end of a CRLF file is not a character */
		}
		wf_frame_put(f, x + 1, y, text + l->off, len, st, w - 1);
	}
}

void wf_es_git_draw(WfEditSession *s, WfBuffer *b, WfFrame *f, int th)
{
	int total = wf_es_git_cols(s, b), right, x;
	WfGitPane *p = &b->git;
	const WfThemeStyles *t = wf_theme_styles();
	char counts[48] = "";
	size_t add = 0, del = 0, top;
	int rows;

	if (!total || th < 1) {
		return;
	}
	right = total - 1;
	x = f->cols - total;
	if (s->git_last !=
	        b) { /* another buffer: its diff may have been dropped meanwhile */
		s->git_last = b;
		p->seen_valid = false;
	}
	note(s, b);
	for (int y = 1; y <= th; y++) {
		wf_frame_put(f, x, y, "|", 1, &t->dim, 1);
	}
	if (p->has_diff) {
		for (size_t k = 0; k < p->diff.n; k++) {
			add += p->diff.h[k].bn;
			del += p->diff.h[k].an;
		}
		snprintf(counts, sizeof counts, "  +%zu -%zu", add, del);
	}
	{
		const char *title = wf_msg(MSG_ED_GIT_TITLE);
		int used = wf_frame_put(
		        f, x + 1, 1, title, strlen(title), &t->bar_bold, right);

		if (used < right) {
			wf_frame_put(f, x + 1 + used, 1, counts, strlen(counts),
			        &t->dim, right - used);
		}
	}
	if (th < 2) {
		return;
	}
	rows = th - 1;
	if (p->too_many) {
		wf_frame_put(f, x + 1, 2, wf_msg(MSG_ED_GIT_TOO_MANY),
		        strlen(wf_msg(MSG_ED_GIT_TOO_MANY)), &t->dim, right);
		return;
	}
	if (!p->has_diff) {
		return; /* the first answer is on its way */
	}
	if (!p->nrows) {
		wf_frame_put(f, x + 1, 2, wf_msg(MSG_ED_GIT_NONE),
		        strlen(wf_msg(MSG_ED_GIT_NONE)), &t->dim, right);
		return;
	}
	/* the pane follows the line the cursor is on */
	top = wf_gp_top_for(p, b->ed.cur.line, (size_t)rows);
	for (int i = 0; i < rows && top + (size_t)i < p->nrows; i++) {
		put_row(f, x + 1, 2 + i, right, p, &p->rows[top + (size_t)i]);
	}
}
