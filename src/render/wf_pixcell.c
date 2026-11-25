#include "wf_pixcell.h"

#include <string.h>

#include "../base/wf_macro.h"
#include "../text/wf_text.h"

#define UPPER_HALF_BLOCK "\xE2\x96\x80"

/* U+2580 is East_Asian_Width=A: with `[terminal] ambiguous_width = 2` the
 * terminal draws it two cells wide and the picture would tear. Then each cell
 * is a space on the mean colour of its two pixels: half the height, but never
 * torn. */
static bool half_block_is_one_cell(void)
{
	return 1 == wf_cp_width(0x2580);
}

void wf_pixcell_grid(int cols, int rows, uint32_t *w, uint32_t *h)
{
	*w = cols > 0 ? (uint32_t)cols : 1;
	*h = rows > 0 ? (uint32_t)rows * 2 : 2;
}

/* Brightness 0..255 of an opaque colour (Rec. 601 weights, integers). */
static unsigned luma(uint32_t rgb)
{
	return (((rgb >> 16) & 255) * 299 + ((rgb >> 8) & 255) * 587 +
	               (rgb & 255) * 114) /
	        1000;
}

static uint32_t mean_rgb(uint32_t a, uint32_t b)
{
	uint32_t out = 0;

	for (int shift = 16; shift >= 0; shift -= 8) {
		out |= ((((a >> shift) & 255) + ((b >> shift) & 255) + 1) / 2)
		        << shift;
	}
	return out;
}

void wf_pixcell_pan_limits(const WfPixmap *pm,
        int cols,
        int rows,
        uint32_t *max_x,
        uint32_t *max_y)
{
	*max_x = pm && cols > 0 && pm->w > (uint32_t)cols
	        ? pm->w - (uint32_t)cols
	        : 0;
	*max_y = pm && rows > 0 && pm->h > 2 * (uint32_t)rows
	        ? pm->h - 2 * (uint32_t)rows
	        : 0;
}

void wf_pixcell_draw(WfFrame *f,
        int x,
        int y,
        int cols,
        int rows,
        const WfPixmap *pm,
        uint32_t bg_rgb,
        WfPixStyle style,
        int *used_cols,
        int *used_rows)
{
	wf_pixcell_draw_view(f, x, y, cols, rows, pm, 0, 0, bg_rgb, style,
	        used_cols, used_rows);
}

void wf_pixcell_draw_view(WfFrame *f,
        int x,
        int y,
        int cols,
        int rows,
        const WfPixmap *pm,
        uint32_t off_x,
        uint32_t off_y,
        uint32_t bg_rgb,
        WfPixStyle style,
        int *used_cols,
        int *used_rows)
{
	static const char ramp[] = " .:-=+*#%@";
	WfStyle blank = {0, bg_rgb, 0};
	int cw, ch, ox, oy;
	uint32_t max_x, max_y;

	if (used_cols) {
		*used_cols = 0;
	}
	if (used_rows) {
		*used_rows = 0;
	}
	if (!f || cols <= 0 || rows <= 0) {
		return;
	}
	wf_frame_fill(f, x, y, cols, rows, ' ', &blank);
	if (!pm || 0 == pm->w || 0 == pm->h) {
		return;
	}
	/* cells the picture covers; an odd last pixel row is a half cell */
	cw = pm->w > (uint32_t)cols ? cols : (int)pm->w;
	ch = (int)((pm->h + 1) / 2);
	if (ch > rows) {
		ch = rows;
	}
	ox = x + (cols - cw) / 2;
	oy = y + (rows - ch) / 2;
	/* a picture larger than the area shows the part at the offset, never
	 * beyond its own edge */
	wf_pixcell_pan_limits(pm, cols, rows, &max_x, &max_y);
	if (off_x > max_x) {
		off_x = max_x;
	}
	if (off_y > max_y) {
		off_y = max_y;
	}
	const bool block = half_block_is_one_cell();

	for (int cy = 0; cy < ch; cy++) {
		uint32_t sy = off_y + 2 * (uint32_t)cy;

		for (int cx = 0; cx < cw; cx++) {
			size_t sx = (size_t)off_x + (size_t)cx;
			uint32_t top = wf_px_over(
			        pm->px[(size_t)sy * pm->w + sx], bg_rgb);
			uint32_t bottom = bg_rgb;
			WfStyle st;

			if (sy + 1 < pm->h) {
				bottom = wf_px_over(
				        pm->px[(size_t)(sy + 1) * pm->w + sx],
				        bg_rgb);
			}
			if (WF_PIXSTYLE_HALFBLOCK == style && !block) {
				st.fg_rgb = mean_rgb(top, bottom);
				st.bg_rgb = st.fg_rgb;
				st.attributes = 0;
				(void)wf_frame_put(
				        f, ox + cx, oy + cy, " ", 1, &st, 1);
			} else if (WF_PIXSTYLE_HALFBLOCK == style) {
				st.fg_rgb = top;
				st.bg_rgb = bottom;
				st.attributes = 0;
				(void)wf_frame_put(f, ox + cx, oy + cy,
				        UPPER_HALF_BLOCK, 3, &st, 1);
			} else {
				uint32_t mean = mean_rgb(top, bottom);
				unsigned level = luma(mean) *
				        (unsigned)(WF_ARRAY_LEN(ramp) - 2) /
				        255;
				char c = ramp[level];

				st.fg_rgb = mean;
				st.bg_rgb = bg_rgb;
				st.attributes = 0;
				(void)wf_frame_put(
				        f, ox + cx, oy + cy, &c, 1, &st, 1);
			}
		}
	}
	if (used_cols) {
		*used_cols = cw;
	}
	if (used_rows) {
		*used_rows = ch;
	}
}
