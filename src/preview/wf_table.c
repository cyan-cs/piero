#include "wf_table.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../text/wf_text.h"

char wf_preview_table_delim(const wchar_t *name)
{
	const wchar_t *dot = name ? wcsrchr(name, L'.') : NULL;

	if (!dot) {
		return 0;
	}
	if (0 == _wcsicmp(dot, L".csv")) {
		return ',';
	}
	if (0 == _wcsicmp(dot, L".tsv") || 0 == _wcsicmp(dot, L".tab")) {
		return '\t';
	}
	return 0;
}

typedef struct
{
	char **cell; /* rows * WF_TABLE_COLS owned strings, NULL where absent */
	size_t rows;
	size_t cols; /* widest record drawn */
	bool cut;
} Grid;

static void grid_free(Grid *g)
{
	if (g->cell) {
		for (size_t i = 0; i < WF_TABLE_ROWS * WF_TABLE_COLS; i++) {
			free(g->cell[i]);
		}
		free(g->cell);
	}
	g->cell = NULL;
}

/* One finished field becomes a cell (cut to its cells), or is dropped when the
 * record is already as wide as is drawn. */
static WfStatus put_cell(Grid *g, size_t row, size_t col, const WfBuf *field)
{
	char text[WF_TABLE_CELL_CELLS * 4u + 8u];
	char *copy;
	size_t n;

	if (col >= WF_TABLE_COLS) {
		g->cut = true;
		return WF_OK;
	}
	wf_utf8_elide(
	        text, sizeof text, wf_buf_cstr(field), WF_TABLE_CELL_CELLS);
	n = strlen(text) + 1;
	copy = malloc(n);
	if (!copy) {
		return WF_E_NOMEM;
	}
	memcpy(copy, text, n);
	g->cell[row * WF_TABLE_COLS + col] = copy;
	if (col + 1 > g->cols) {
		g->cols = col + 1;
	}
	return WF_OK;
}

static WfStatus parse(const char *s, size_t n, char delim, Grid *g)
{
	WfBuf field;
	size_t i = 0, row = 0, col = 0;
	bool quoted = false, was_quoted = false, any = false;
	WfStatus st = WF_OK;

	wf_buf_init(&field);
	g->cell = calloc(WF_TABLE_ROWS * WF_TABLE_COLS, sizeof *g->cell);
	if (!g->cell) {
		return WF_E_NOMEM;
	}
	while (i <= n && WF_OK == st) {
		char c = i < n ? s[i]
		               : '\n'; /* the end of the text ends a record */
		bool end = i >= n;

		if (quoted) {
			if (end) {
				quoted =
				        false; /* an unclosed quote ends at the end */
				continue;
			}
			if ('"' == c) {
				if (i + 1 < n && '"' == s[i + 1]) {
					st = wf_buf_putc(&field, '"');
					i += 2;
					continue;
				}
				quoted = false;
				i++;
				continue;
			}
			st = wf_buf_putc(&field, '\n' == c ? ' ' : c);
			i++;
			continue;
		}
		if ('"' == c && 0 == field.len && !was_quoted) {
			quoted = true;
			was_quoted = true;
			any = true;
			i++;
			continue;
		}
		if (c == delim && !end) {
			st = put_cell(g, row, col++, &field);
			wf_buf_clear(&field);
			was_quoted = false;
			any = true;
			i++;
			continue;
		}
		if ('\n' == c) {
			if (any || field.len) {
				st = put_cell(g, row, col, &field);
				if (WF_OK == st) {
					row++;
				}
			}
			wf_buf_clear(&field);
			col = 0;
			was_quoted = false;
			any = false;
			if (WF_OK == st && row == WF_TABLE_ROWS) {
				/* more records follow? skip empty ones */
				size_t k = i + 1;

				while (k < n &&
				        ('\n' == s[k] || '\r' == s[k])) {
					k++;
				}
				if (k < n) {
					g->cut = true;
				}
				break;
			}
			i++;
			continue;
		}
		if ('\r' == c && i + 1 < n && '\n' == s[i + 1]) {
			i++; /* CRLF: the break is the LF */
			continue;
		}
		st = wf_buf_putc(&field, c);
		any = true;
		i++;
	}
	g->rows = row;
	wf_buf_free(&field);
	return st;
}

static WfStatus pad_to(WfBuf *out, int have, int want)
{
	WfStatus st = WF_OK;

	for (int k = have; k < want && WF_OK == st; k++) {
		st = wf_buf_putc(out, ' ');
	}
	return st;
}

WfStatus wf_table_render(const char *utf8,
        size_t n,
        char delim,
        WfBuf *out,
        size_t *rows,
        bool *cut)
{
	Grid g;
	int width[WF_TABLE_COLS];
	WfStatus st;

	if (!out || !rows || !cut || (n && !utf8) || !delim) {
		return WF_E_INVAL;
	}
	*rows = 0;
	*cut = false;
	memset(&g, 0, sizeof g);
	st = parse(utf8, n, delim, &g);
	if (WF_OK != st || 0 == g.rows) {
		grid_free(&g);
		return st;
	}
	for (size_t c = 0; c < g.cols; c++) {
		width[c] = 0;
		for (size_t r = 0; r < g.rows; r++) {
			const char *cell = g.cell[r * WF_TABLE_COLS + c];
			int w = cell ? wf_utf8_cells(cell, strlen(cell)) : 0;

			if (w > width[c]) {
				width[c] = w;
			}
		}
	}
	for (size_t r = 0; r < g.rows && WF_OK == st; r++) {
		size_t last = 0;

		/* the last cell with text ends the line: no trailing blanks */
		for (size_t c = 0; c < g.cols; c++) {
			const char *cell = g.cell[r * WF_TABLE_COLS + c];

			if (cell && *cell) {
				last = c;
			}
		}
		if (r) {
			st = wf_buf_putc(out, '\n');
		}
		for (size_t c = 0; c <= last && WF_OK == st; c++) {
			const char *cell = g.cell[r * WF_TABLE_COLS + c];
			int w = cell ? wf_utf8_cells(cell, strlen(cell)) : 0;

			if (c) {
				st = wf_buf_puts(out, " | ");
			}
			if (WF_OK == st && cell) {
				st = wf_buf_puts(out, cell);
			}
			if (WF_OK == st && c < last) {
				st = pad_to(out, w, width[c]);
			}
		}
		if (0 == r && WF_OK == st) {
			st = wf_buf_putc(out, '\n');
			for (size_t c = 0; c < g.cols && WF_OK == st; c++) {
				if (c) {
					st = wf_buf_puts(out, "-+-");
				}
				for (int k = 0; k < width[c] && WF_OK == st;
				        k++) {
					st = wf_buf_putc(out, '-');
				}
			}
		}
	}
	*rows = g.rows;
	*cut = g.cut;
	grid_free(&g);
	return st;
}

/* The lines of the preview as one text, '\n' between them. */
static WfStatus join_lines(const WfPreview *p, WfBuf *joined)
{
	WfStatus st = WF_OK;

	for (size_t i = 0; i < p->nlines && WF_OK == st; i++) {
		const char *line = p->text.data + p->lines[i];

		if (i) {
			st = wf_buf_putc(joined, '\n');
		}
		if (WF_OK == st) {
			st = wf_buf_puts(joined, line);
		}
	}
	return st;
}

/* A ';' file (the CSV of many locales): the comma is not the delimiter when the
 * first record has no comma but has a semicolon. Only what is outside quotes is
 * counted, and a record goes on past a line break inside quotes (a doubled quote
 * closes and opens again: it changes nothing). */
static char pick_delim(const char *text, char delim)
{
	size_t commas = 0, semis = 0;
	bool quoted = false;

	if (',' != delim) {
		return delim;
	}
	for (const char *c = text; *c && (quoted || '\n' != *c); c++) {
		if ('"' == *c) {
			quoted = !quoted;
		} else if (!quoted) {
			commas += ',' == *c;
			semis += ';' == *c;
		}
	}
	return 0 == commas && semis ? ';' : delim;
}

WfStatus wf_preview_tableize(WfPreview *p, char delim)
{
	WfBuf joined, table, text;
	size_t rows = 0, nlines = 0, widest = 0;
	size_t lines[WF_PREVIEW_LINES];
	bool cut = false;
	WfStatus st;

	if (!p || !delim) {
		return WF_E_INVAL;
	}
	if (WF_PREVIEW_TEXT != p->kind || !p->text.data || 0 == p->nlines) {
		return WF_OK;
	}
	wf_buf_init(&joined);
	wf_buf_init(&table);
	wf_buf_init(&text);
	st = join_lines(p, &joined);
	if (WF_OK == st) {
		st = wf_table_render(wf_buf_cstr(&joined), joined.len,
		        pick_delim(wf_buf_cstr(&joined), delim), &table, &rows,
		        &cut);
	}
	if (WF_OK != st || 0 == rows || !table.data) {
		goto done;
	}
	/* NUL-separated lines, as the pane reads them */
	{
		const char *s = table.data;
		size_t at = 0;

		while (WF_OK == st) {
			const char *nl = strchr(s + at, '\n');
			size_t len =
			        nl ? (size_t)(nl - (s + at)) : strlen(s + at);
			int cells;

			if (nlines == WF_PREVIEW_LINES) {
				cut = true;
				break;
			}
			lines[nlines++] = text.len;
			st = wf_buf_append(&text, s + at, len);
			if (WF_OK == st) {
				st = wf_buf_putc(&text, '\0');
			}
			cells = wf_utf8_cells(s + at, len);
			if (cells > 0 && (size_t)cells > widest) {
				widest = (size_t)cells;
			}
			if (!nl) {
				break;
			}
			at += len + 1;
		}
	}
	if (WF_OK == st) {
		wf_buf_free(&p->text);
		p->text = text;
		wf_buf_init(&text);
		memcpy(p->lines, lines, nlines * sizeof lines[0]);
		p->nlines = nlines;
		p->max_cells = widest;
		p->truncated = p->truncated || cut;
	}
done:
	wf_buf_free(&joined);
	wf_buf_free(&table);
	wf_buf_free(&text);
	return st;
}
