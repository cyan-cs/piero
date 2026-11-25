#include "wf_frame.h"

#include <stdlib.h>
#include <string.h>

#include "../text/wf_text.h"

static void set_cell(
        WfCell *c, const char *text, size_t n, int width, const WfStyle *st)
{
	memcpy(c->utf8, text, n);
	c->utf8[n] = '\0';
	c->fg_rgb = st->fg_rgb;
	c->bg_rgb = st->bg_rgb;
	c->attributes = st->attributes;
	c->width = (uint8_t)width;
	c->flags = 0;
}

static void set_blank(WfCell *c, const WfStyle *st)
{
	set_cell(c, " ", 1, 1, st);
}

WfFrame *wf_frame_new(int cols, int rows, const WfStyle *blank)
{
	WfFrame *f;
	size_t count, i;

	if (!blank || cols <= 0 || rows <= 0 || cols > WF_FRAME_MAX_DIM ||
	        rows > WF_FRAME_MAX_DIM ||
	        (size_t)cols * (size_t)rows > WF_FRAME_MAX_CELLS) {
		return NULL;
	}

	f = calloc(1, sizeof *f);
	if (!f) {
		return NULL;
	}
	count = (size_t)cols * (size_t)rows; /* bounded: WF_FRAME_MAX_CELLS */
	f->cells = calloc(count, sizeof *f->cells);
	if (!f->cells) {
		free(f);
		return NULL;
	}
	f->cols = cols;
	f->rows = rows;
	for (i = 0; i < count; i++) {
		set_blank(&f->cells[i], blank);
	}
	return f;
}

bool wf_frame_reuse(WfFrame **f, int cols, int rows, const WfStyle *blank)
{
	WfFrame *fresh;

	if (*f && (*f)->cols == cols && (*f)->rows == rows) {
		memset(&(*f)->graphic, 0, sizeof(*f)->graphic);
		memset(&(*f)->cursor, 0, sizeof(*f)->cursor);
		return true;
	}
	fresh = wf_frame_new(cols, rows, blank);
	if (!fresh) {
		return false;
	}
	wf_frame_free(*f);
	*f = fresh;
	return true;
}

void wf_frame_free(WfFrame *f)
{
	if (!f) {
		return;
	}
	free(f->cells);
	free(f);
}

bool wf_cell_equal(const WfCell *a, const WfCell *b)
{
	return a->width == b->width && a->fg_rgb == b->fg_rgb &&
	        a->bg_rgb == b->bg_rgb && a->attributes == b->attributes &&
	        a->flags == b->flags && 0 == strcmp(a->utf8, b->utf8);
}

/* About to overwrite row[pos]: if it is half of a wide pair, blank the
 * other half so no orphan continuation or head survives. */
static void break_wide(WfCell *row, int cols, int pos)
{
	WfStyle keep;

	if (0 == row[pos].width && pos > 0) {
		keep = (WfStyle){
		        row[pos].fg_rgb, row[pos].bg_rgb, row[pos].attributes};
		set_blank(&row[pos - 1], &keep);
	} else if (2 == row[pos].width && pos + 1 < cols) {
		keep = (WfStyle){
		        row[pos].fg_rgb, row[pos].bg_rgb, row[pos].attributes};
		set_blank(&row[pos + 1], &keep);
	}
}

static void place(WfCell *row,
        int cols,
        int pos,
        const char *text,
        size_t n,
        int width,
        const WfStyle *st)
{
	int k;

	/* a wide cell needs its continuation inside the row */
	if (pos < 0 || width < 1 || width > 2 || pos > cols - width ||
	        n >= WF_CELL_TEXT_MAX) {
		return;
	}
	for (k = 0; k < width; k++) {
		break_wide(row, cols, pos + k);
	}
	set_cell(&row[pos], text, n, width, st);
	if (2 == width) {
		set_cell(&row[pos + 1], "", 0, 0, st);
	}
}

int wf_frame_put(WfFrame *f,
        int x,
        int y,
        const char *s,
        size_t len,
        const WfStyle *st,
        int max_cells)
{
	WfCell *row, *head = NULL;
	int used = 0, poisoned = 0;

	if (!f || !st || (len && !s)) {
		return 0;
	}
	if (y < 0 || y >= f->rows || x < 0 || x >= f->cols) {
		return 0;
	}
	if (max_cells > f->cols - x) {
		max_cells = f->cols - x;
	}
	row = &f->cells[(size_t)y * (size_t)f->cols];

	/* The row being full ends the text only at the next character that needs a
	 * cell: what has no width (a mark that goes on the cell before it) is read
	 * after the last cell too, or "e" + an accent at the last column would be
	 * an "e". The cut is between characters, not within one. */
	while (len > 0) {
		uint32_t cp;
		size_t n;
		char enc[4];
		size_t en;
		int w;

		if ((unsigned char)*s < 0x80) {
			/* A cell is data, never a terminal command. Multiline views
			 * split their lines before reaching this single-row API. */
			enc[0] = (unsigned char)*s < 32 || *s == 127 ? '?' : *s;
			n = en = 1;
			w = 1;
		} else {
			n = wf_utf8_decode(s, len, &cp);
			if (cp >= 0x80 && cp <= 0x9f) {
				enc[0] = '?';
				en = 1;
				w = 1;
			} else {
				en = wf_utf8_encode(cp, enc);
				w = wf_cp_width(cp);
			}
		}

		s += n;
		len -= n;

		if (0 == w) {
			size_t cur;

			if (!head || poisoned) {
				continue;
			}
			cur = strlen(head->utf8);
			if (cur + en >= WF_CELL_TEXT_MAX) {
				/* grapheme too big for a cell: ASCII stand-in
				 */
				memcpy(head->utf8,
				        2 == head->width ? "??" : "?",
				        2 == head->width ? 3
				                         : 2); /* incl. NUL */
				poisoned = 1;
				continue;
			}
			memcpy(head->utf8 + cur, enc, en);
			head->utf8[cur + en] = '\0';
			continue;
		}

		if (w > max_cells - used) {
			if (used < max_cells) {
				place(row, f->cols, x + used++, "?", 1, 1, st);
			}
			break;
		}
		head = &row[x + used];
		place(row, f->cols, x + used, enc, en, w, st);
		used += w;
		poisoned = 0;
	}
	return used;
}

void wf_frame_set_cursor(WfFrame *f, int x, int y)
{
	f->cursor.visible = x >= 0 && x < f->cols && y >= 0 && y < f->rows;
	f->cursor.x = f->cursor.visible ? x : 0;
	f->cursor.y = f->cursor.visible ? y : 0;
}

void wf_frame_fill(
        WfFrame *f, int x, int y, int w, int h, char ch, const WfStyle *st)
{
	int xx, yy;

	if (!f || !st) {
		return;
	}
	for (yy = y < 0 ? 0 : y; yy < y + h && yy < f->rows; yy++) {
		WfCell *row = &f->cells[(size_t)yy * (size_t)f->cols];

		for (xx = x < 0 ? 0 : x; xx < x + w && xx < f->cols; xx++) {
			place(row, f->cols, xx, &ch, 1, 1, st);
		}
	}
}
