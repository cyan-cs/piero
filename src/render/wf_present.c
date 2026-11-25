#include "wf_present.h"

#include <stdlib.h>
#include <string.h>

#include "wf_theme.h"
#include "../base/wf_macro.h"

typedef struct
{
	WfBuf *out;
	WfStyle cur;
	bool cur_valid;
	int cx, cy; /* terminal cursor, -1 when unknown */
} Emitter;

static WfStatus put_rgb(WfBuf *b, const char *intro, uint32_t rgb)
{
	WfStatus st;

	st = wf_buf_puts(b, intro);
	if (WF_OK != st) {
		return st;
	}
	st = wf_buf_put_uint(b, (rgb >> 16) & 0xFF);
	if (WF_OK != st) {
		return st;
	}
	st = wf_buf_putc(b, ';');
	if (WF_OK != st) {
		return st;
	}
	st = wf_buf_put_uint(b, (rgb >> 8) & 0xFF);
	if (WF_OK != st) {
		return st;
	}
	st = wf_buf_putc(b, ';');
	if (WF_OK != st) {
		return st;
	}
	return wf_buf_put_uint(b, rgb & 0xFF);
}

static WfColorDepth depth = WF_DEPTH_TRUECOLOR;

void wf_present_set_color(WfColorDepth d)
{
	depth = d;
}

WfColorDepth wf_present_color_depth(void)
{
	return depth;
}

static unsigned channel(uint32_t rgb, int shift)
{
	return (rgb >> shift) & 0xFFu;
}

static unsigned long distance(uint32_t a, uint32_t b)
{
	long dr = (long)channel(a, 16) - (long)channel(b, 16);
	long dg = (long)channel(a, 8) - (long)channel(b, 8);
	long db = (long)channel(a, 0) - (long)channel(b, 0);

	return (unsigned long)(dr * dr + dg * dg + db * db);
}

unsigned wf_color_to_256(uint32_t rgb)
{
	static const unsigned levels[6] = {0, 95, 135, 175, 215, 255};
	unsigned best_index = 16, cube[3];
	unsigned long best;
	int shift, i;
	uint32_t cube_rgb;
	unsigned grey_step = 0;
	uint32_t grey_rgb;

	for (shift = 16, i = 0; shift >= 0; shift -= 8, i++) {
		unsigned c = channel(rgb, shift), k, pick = 0;
		unsigned long gap = ~0ul;

		for (k = 0; k < WF_ARRAY_LEN(levels); k++) {
			unsigned long g =
			        c > levels[k] ? c - levels[k] : levels[k] - c;

			if (g < gap) {
				gap = g;
				pick = k;
			}
		}
		cube[i] = pick;
	}
	cube_rgb =
	        levels[cube[0]] << 16 | levels[cube[1]] << 8 | levels[cube[2]];
	best_index = 16 + 36 * cube[0] + 6 * cube[1] + cube[2];
	best = distance(rgb, cube_rgb);
	/* grey ramp: 24 steps from 8 to 238 */
	{
		unsigned avg =
		        (channel(rgb, 16) + channel(rgb, 8) + channel(rgb, 0)) /
		        3;
		unsigned long dist;

		grey_step = avg < 8 ? 0 : (avg - 8 + 5) / 10;
		if (grey_step > 23) {
			grey_step = 23;
		}
		grey_rgb = (8 + 10 * grey_step) * 0x010101u;
		dist = distance(rgb, grey_rgb);
		if (dist < best) {
			best = dist;
			best_index = 232 + grey_step;
		}
	}
	return best_index;
}

unsigned wf_color_to_16(uint32_t rgb)
{
	/* the Windows console palette, which is what a console shows */
	static const uint32_t ansi[16] = {0x0C0C0C, 0xC50F1F, 0x13A10E,
	        0xC19C00, 0x0037DA, 0x881798, 0x3A96DD, 0xCCCCCC, 0x767676,
	        0xE74856, 0x16C60C, 0xF9F1A5, 0x3B78FF, 0xB4009E, 0x61D6D6,
	        0xF2F2F2};
	unsigned best = 0, i;
	unsigned long best_d = ~0ul;

	for (i = 0; i < WF_ARRAY_LEN(ansi); i++) {
		unsigned long d = distance(rgb, ansi[i]);

		if (d < best_d) {
			best_d = d;
			best = i;
		}
	}
	return best;
}

WfColorDepth wf_color_depth_resolve(int policy,
        const char *no_color,
        const char *colorterm,
        const char *wt_session,
        const char *term)
{
	if (1 == policy) {
		return WF_DEPTH_TRUECOLOR;
	}
	if (2 == policy) {
		return WF_DEPTH_256;
	}
	if (3 == policy) {
		return WF_DEPTH_16;
	}
	if (4 == policy) {
		return WF_DEPTH_NONE;
	}
	if (no_color && *no_color) {
		return WF_DEPTH_NONE;
	}
	if (term && !strcmp(term, "dumb")) {
		return WF_DEPTH_NONE;
	}
	if ((colorterm &&
	            (!strcmp(colorterm, "truecolor") ||
	                    !strcmp(colorterm, "24bit"))) ||
	        (wt_session && *wt_session)) {
		return WF_DEPTH_TRUECOLOR;
	}
	if (term && strstr(term, "256color")) {
		return WF_DEPTH_256;
	}
	return WF_DEPTH_TRUECOLOR;
}

static WfStatus put_color(WfBuf *b, bool foreground, uint32_t rgb)
{
	WfStatus st;

	switch (depth) {
	case WF_DEPTH_TRUECOLOR:
		return put_rgb(b, foreground ? ";38;2;" : ";48;2;", rgb);
	case WF_DEPTH_256:
		st = wf_buf_puts(b, foreground ? ";38;5;" : ";48;5;");
		if (WF_OK != st) {
			return st;
		}
		return wf_buf_put_uint(b, wf_color_to_256(rgb));
	case WF_DEPTH_16: {
		unsigned n = wf_color_to_16(rgb);
		unsigned code = (n < 8 ? (foreground ? 30u : 40u)
		                       : (foreground ? 90u : 100u)) +
		        (n & 7u);

		st = wf_buf_putc(b, ';');
		if (WF_OK != st) {
			return st;
		}
		return wf_buf_put_uint(b, code);
	}
	case WF_DEPTH_NONE: break;
	}
	return WF_OK;
}

static WfStatus set_style(Emitter *e, const WfCell *c)
{
	WfBuf *b = e->out;
	uint16_t attributes = c->attributes;
	WfStatus st;

	if (e->cur_valid && e->cur.fg_rgb == c->fg_rgb &&
	        e->cur.bg_rgb == c->bg_rgb &&
	        e->cur.attributes == c->attributes) {
		return WF_OK;
	}

	/* always reset first: attributes can only be turned off that way */
	if (WF_DEPTH_NONE == depth &&
	        c->bg_rgb == wf_theme_active()->selection_background) {
		attributes |= WF_ATTR_REVERSE; /* no colours: still show it */
	}
	st = wf_buf_puts(b, "\x1b[0");
	if (WF_OK != st) {
		return st;
	}
	if (attributes & WF_ATTR_BOLD) {
		st = wf_buf_puts(b, ";1");
		if (WF_OK != st) {
			return st;
		}
	}
	if (attributes & WF_ATTR_DIM) {
		st = wf_buf_puts(b, ";2");
		if (WF_OK != st) {
			return st;
		}
	}
	if (attributes & WF_ATTR_UNDERLINE) {
		st = wf_buf_puts(b, ";4");
		if (WF_OK != st) {
			return st;
		}
	}
	if (attributes & WF_ATTR_REVERSE) {
		st = wf_buf_puts(b, ";7");
		if (WF_OK != st) {
			return st;
		}
	}
	if (attributes & WF_ATTR_ITALIC) {
		st = wf_buf_puts(b, ";3");
		if (WF_OK != st) {
			return st;
		}
	}
	st = put_color(b, true, c->fg_rgb);
	if (WF_OK != st) {
		return st;
	}
	st = put_color(b, false, c->bg_rgb);
	if (WF_OK != st) {
		return st;
	}
	st = wf_buf_putc(b, 'm');
	if (WF_OK != st) {
		return st;
	}

	e->cur = (WfStyle){c->fg_rgb, c->bg_rgb, c->attributes};
	e->cur_valid = true;
	return WF_OK;
}

static WfStatus move_to(Emitter *e, int x, int y)
{
	WfStatus st;

	if (e->cx == x && e->cy == y) {
		return WF_OK;
	}
	st = wf_buf_puts(e->out, "\x1b[");
	if (WF_OK != st) {
		return st;
	}
	st = wf_buf_put_uint(e->out, (unsigned)y + 1);
	if (WF_OK != st) {
		return st;
	}
	st = wf_buf_putc(e->out, ';');
	if (WF_OK != st) {
		return st;
	}
	st = wf_buf_put_uint(e->out, (unsigned)x + 1);
	if (WF_OK != st) {
		return st;
	}
	st = wf_buf_putc(e->out, 'H');
	if (WF_OK != st) {
		return st;
	}
	e->cx = x;
	e->cy = y;
	return WF_OK;
}

/*
 * Marks the cells of one row that need output. If either half of a wide
 * pair changed, both halves are redrawn from the head cell.
 */
static void mark_dirty(
        unsigned char *dirty, const WfCell *p, const WfCell *n, int cols)
{
	int x;

	for (x = 0; x < cols; x++) {
		dirty[x] = p ? !wf_cell_equal(&p[x], &n[x]) : 1;
	}
	if (!p) {
		return;
	}

	for (x = 0; x + 1 < cols; x++) {
		if (dirty[x] && (2 == p[x].width || 2 == n[x].width)) {
			dirty[x + 1] = 1;
		}
	}
	for (x = cols - 1; x > 0; x--) {
		if (dirty[x] && (0 == p[x].width || 0 == n[x].width)) {
			dirty[x - 1] = 1;
		}
	}
}

/*
 * The terminal's own cursor. A terminal puts an IME composition at its
 * cursor, and drawing leaves the cursor wherever the last cell was written
 * (the status line), so a frame that has a text field in focus ends with the
 * cursor moved onto the insertion cell and shown. Any other frame hides it.
 *
 * Output only on change: when nothing was written and the cursor state is
 * the one the terminal already has, nothing is emitted (an idle frame stays
 * empty). A first frame, or one after a resize or a dropped frame, states the
 * visibility explicitly because the terminal's state is not known then.
 */
static WfStatus place_cursor(
        Emitter *e, const WfFrame *prev, const WfFrame *next, bool same_size)
{
	bool known = prev && same_size;
	bool was_visible = known && prev->cursor.visible;

	if (!next->cursor.visible) {
		return !known || was_visible ? wf_buf_puts(e->out, "\x1b[?25l")
		                             : WF_OK;
	}
	/* writing cells moved the terminal cursor; otherwise it is where the
	 * previous frame left it */
	if (!(known && was_visible && -1 == e->cx && -1 == e->cy &&
	            prev->cursor.x == next->cursor.x &&
	            prev->cursor.y == next->cursor.y)) {
		WfStatus st;

		e->cx = e->cy = -1; /* force the absolute move */
		st = move_to(e, next->cursor.x, next->cursor.y);
		if (WF_OK != st) {
			return st;
		}
	}
	return was_visible ? WF_OK : wf_buf_puts(e->out, "\x1b[?25h");
}

WfStatus wf_present_diff(const WfFrame *prev, const WfFrame *next, WfBuf *out)
{
	Emitter e = {out, {0, 0, 0}, false, -1, -1};
	unsigned char
	        dirty[WF_FRAME_MAX_DIM]; /* one row: no allocation per frame */
	int x, y;
	WfStatus st = WF_OK;

	if (!next || !out || next->cols > WF_FRAME_MAX_DIM) {
		return WF_E_INVAL;
	}
	bool same_size =
	        prev && prev->cols == next->cols && prev->rows == next->rows;

	if (prev && !same_size) {
		st = wf_buf_puts(out, "\x1b[0m\x1b[2J");
		if (WF_OK != st) {
			goto done;
		}
	}

	for (y = 0; y < next->rows; y++) {
		const WfCell *nrow =
		        &next->cells[(size_t)y * (size_t)next->cols];
		const WfCell *prow = same_size
		        ? &prev->cells[(size_t)y * (size_t)prev->cols]
		        : NULL;

		mark_dirty(dirty, prow, nrow, next->cols);
		for (x = 0; x < next->cols; x++) {
			const WfCell *c = &nrow[x];

			if (!dirty[x] || 0 == c->width) {
				continue;
			}
			if (y == next->rows - 1 && x == next->cols - 1) {
				continue;
			}
			st = move_to(&e, x, y);
			if (WF_OK == st) {
				st = set_style(&e, c);
			}
			if (WF_OK == st) {
				st = wf_buf_puts(out, c->utf8);
			}
			if (WF_OK != st) {
				goto done;
			}
			e.cx += c->width;
		}
	}
	if (e.cur_valid) {
		st = wf_buf_puts(out, "\x1b[0m");
	}
	if (WF_OK == st) {
		st = place_cursor(&e, prev, next, same_size);
	}
done:
	return st;
}
