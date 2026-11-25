#include "wf_graphics.h"
#include "wf_present.h"
#include <stdio.h>

static bool unchanged(const WfFrame *p, const WfFrame *n)
{
	if (!p || p->cols != n->cols || p->rows != n->rows ||
	        p->graphic.gen != n->graphic.gen ||
	        p->graphic.len != n->graphic.len ||
	        p->graphic.x != n->graphic.x || p->graphic.y != n->graphic.y ||
	        p->graphic.w != n->graphic.w || p->graphic.h != n->graphic.h) {
		return false;
	}
	for (int y = p->graphic.y; y < p->graphic.y + p->graphic.h; y++) {
		for (int x = p->graphic.x; x < p->graphic.x + p->graphic.w;
		        x++) {
			size_t i = (size_t)y * (size_t)p->cols + (size_t)x;
			if (!wf_cell_equal(&p->cells[i], &n->cells[i])) {
				return false;
			}
		}
	}
	return true;
}

WfStatus wf_graphics_present(WfFrame *prev, const WfFrame *next, WfBuf *out)
{
	char seq[128];
	bool keep;
	WfStatus st;
	if (!next || !out) {
		return WF_E_INVAL;
	}
	if (next->graphic.len &&
	        (!next->graphic.data || next->graphic.x < 0 ||
	                next->graphic.y < 0 || next->graphic.w <= 0 ||
	                next->graphic.h <= 0 ||
	                next->graphic.w > next->cols - next->graphic.x ||
	                next->graphic.h > next->rows - next->graphic.y)) {
		return WF_E_INVAL;
	}
	keep = unchanged(prev, next);
	if (prev && prev->graphic.len && !keep) {
		/* DECERA explicitly erases ImageSlice cells in Windows Terminal.
		 * Redraw those text cells even if their character values match. */
		snprintf(seq, sizeof seq, "\x1b[0m\x1b[%d;%d;%d;%d$z",
		        prev->graphic.y + 1, prev->graphic.x + 1,
		        prev->graphic.y + prev->graphic.h,
		        prev->graphic.x + prev->graphic.w);
		st = wf_buf_puts(out, seq);
		if (WF_OK != st) {
			return st;
		}
		for (int y = prev->graphic.y;
		        y < prev->graphic.y + prev->graphic.h; y++) {
			for (int x = prev->graphic.x;
			        x < prev->graphic.x + prev->graphic.w; x++) {
				prev->cells[(size_t)y * (size_t)prev->cols +
				            (size_t)x]
				        .flags ^= 128;
			}
		}
	}
	st = wf_present_diff(prev, next, out);
	if (WF_OK != st || keep || !next->graphic.len) {
		return st;
	}
	/* Reset DECSDM: draw at CUP, preserve the text cursor. Do not enable
	 * display mode: that would anchor the image at the screen origin. */
	snprintf(seq, sizeof seq,
	        "\x1b"
	        "7\x1b[?80l\x1b[%d;%dH",
	        next->graphic.y + 1, next->graphic.x + 1);
	st = wf_buf_puts(out, seq);
	if (WF_OK == st) {
		st = wf_buf_append(out, next->graphic.data, next->graphic.len);
	}
	if (WF_OK == st) {
		st = wf_buf_puts(out,
		        "\x1b"
		        "8");
	}
	return st;
}
