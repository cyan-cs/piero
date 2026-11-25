#include "wf_emdpane.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../i18n/wf_i18n.h"
#include "../render/wf_mddraw.h"
#include "../render/wf_theme.h"
#include "../text/wf_text.h"
#include "wf_esession_internal.h"

void wf_mdp_free(WfMdPane *p)
{
	wf_md_free(p->doc);
	memset(p, 0, sizeof *p);
}

/* The text as one string, lines joined by LF, up to the Markdown limit. */
static char *copy_text(const WfEText *t, size_t *n, bool *cut)
{
	size_t total = 0, i, k = 0;
	char *s;

	for (i = 0; i < t->n; i++) {
		size_t room = WF_MD_SRC_MAX - total;

		if (t->lines[i].len >= room) {
			total += wf_utf8_prefix(
			        t->lines[i].s, t->lines[i].len, room);
			break;
		}
		total += t->lines[i].len + 1;
	}
	s = malloc(total + 1);
	if (!s) {
		return NULL;
	}
	for (size_t j = 0; j < i; j++) {
		memcpy(s + k, t->lines[j].s, t->lines[j].len);
		k += t->lines[j].len;
		s[k++] = '\n';
	}
	if (k < total) {
		memcpy(s + k, t->lines[i].s, total - k);
		k = total;
	}
	s[k] = '\0';
	*n = k;
	*cut = i < t->n;
	return s;
}

WfStatus wf_mdp_refresh(WfMdPane *p, const WfEText *t, int width)
{
	size_t n = 0;
	bool cut = false;
	char *src = copy_text(t, &n, &cut);
	WfMdDoc *d = NULL;
	WfStatus st;

	if (!src) {
		return WF_E_NOMEM;
	}
	st = wf_md_render(src, n, width, &d);
	free(src);
	if (WF_OK != st) {
		return st;
	}
	if (cut && d) { /* source bytes were left out */
		d->truncated = true;
	}
	wf_md_free(p->doc);
	p->doc = d;
	p->width = width;
	p->gen = t->gen;
	p->seen = t->gen;
	p->seen_valid = true;
	p->due_ms = 0;
	return WF_OK;
}

void wf_mdp_note(WfMdPane *p, const WfEText *t, uint64_t now_ms)
{
	if (p->seen_valid && p->seen == t->gen) {
		return;
	}
	p->seen = t->gen;
	p->seen_valid = true;
	if (p->doc && p->gen != t->gen) {
		p->due_ms = now_ms + WF_MDP_DEBOUNCE_MS;
	}
}

#define WF_MD_NARROW_MIN 60
#define WF_MD_WIDE_MIN 100

int wf_es_md_cols(const WfEditSession *s, const WfBuffer *b)
{
	bool on;

	if (!b || WF_LANG_MARKDOWN != b->lang || PR_HELP == s->pk) {
		return 0;
	}
	if (s->md_force) {
		on = s->md_force > 0 && s->cols >= WF_MD_NARROW_MIN;
	} else if (2 == s->cfg.md_split) {
		on = false;
	} else {
		on = s->cols >= (1 == s->cfg.md_split ? WF_MD_NARROW_MIN
		                                      : WF_MD_WIDE_MIN);
	}
	return on ? 1 + (s->cols - 1) / 2 : 0;
}

uint32_t wf_es_md_wait(const WfEditSession *s, uint64_t now_ms)
{
	const WfBuffer *b = s->n ? s->buf[s->cur] : NULL;

	if (!b || !b->md.due_ms || !wf_es_md_cols(s, b)) {
		return UINT32_MAX;
	}
	return now_ms >= b->md.due_ms ? 0 : (uint32_t)(b->md.due_ms - now_ms);
}

bool wf_es_md_tick(WfEditSession *s, uint64_t now_ms)
{
	WfBuffer *b = s->n ? s->buf[s->cur] : NULL;
	int cols = wf_es_md_cols(s, b);

	if (!cols || !b->md.due_ms || now_ms < b->md.due_ms) {
		return false;
	}
	/* the text is laid out here, at idle, never in a key handler */
	if (WF_OK != wf_mdp_refresh(&b->md, &b->ed.text, cols - 1)) {
		b->md.due_ms = 0; /* keep the last layout */
	}
	return true;
}

WfEsResult wf_es_md_toggle(WfEditSession *s)
{
	WfBuffer *b = wf_es_buffer(s);

	if (!b) {
		return WF_ES_NONE;
	}
	if (WF_LANG_MARKDOWN != b->lang) {
		snprintf(s->msg, sizeof s->msg, "%s", wf_msg(MSG_ED_MD_NONE));
		s->msg_error = true;
	} else if (wf_es_md_cols(s, b)) {
		s->md_force = -1;
	} else if (s->cols < WF_MD_NARROW_MIN) {
		snprintf(s->msg, sizeof s->msg, "%s", wf_msg(MSG_ED_MD_NARROW));
		s->msg_error = true;
	} else {
		s->md_force = 1;
	}
	return WF_ES_REDRAW;
}

void wf_es_md_draw(WfEditSession *s, WfBuffer *b, WfFrame *f, int th)
{
	int total = wf_es_md_cols(s, b), right, x, rows;
	WfMdPane *p;
	WfStyle dim = wf_theme_styles()->dim;
	size_t first, row_of_cursor, top, limit;
	const WfMdDoc *d;

	if (!total || th < 1) {
		return;
	}
	p = &b->md;
	right = total - 1;
	x = f->cols - total;
	wf_mdp_note(p, &b->ed.text, s->now_ms);
	if (!p->doc || p->width != right) { /* first sight, or a resize */
		if (WF_OK != wf_mdp_refresh(p, &b->ed.text, right) && !p->doc) {
			wf_frame_put(f, x + 1, 1, wf_msg(MSG_ED_MD_FAILED),
			        strlen(wf_msg(MSG_ED_MD_FAILED)), &dim, right);
		}
	}
	for (int y = 1; y <= th; y++) {
		wf_frame_put(f, x, y, "|", 1, &dim, 1);
	}
	d = p->doc;
	if (!d) {
		return;
	}
	rows = th - (d->truncated && th > 1 ? 1 : 0);
	/* the first row shown is the one the cursor's line is drawn as, at the
	 * row the cursor has on the left */
	first = wf_md_line_for_source(d, b->ed.cur.line);
	row_of_cursor = b->ed.cur.line > b->top ? b->ed.cur.line - b->top : 0;
	if (row_of_cursor >= (size_t)rows) {
		row_of_cursor = (size_t)rows - 1;
	}
	top = first > row_of_cursor ? first - row_of_cursor : 0;
	limit = d->nlines > (size_t)rows ? d->nlines - (size_t)rows : 0;
	if (top > limit) {
		top = limit;
	}
	wf_md_draw(f, x + 1, 1, right, rows, d, top);
	if (d->truncated && th > 1) {
		wf_frame_put(f, x + 1, th, wf_msg(MSG_ED_MD_CUT),
		        strlen(wf_msg(MSG_ED_MD_CUT)), &dim, right);
	}
}
