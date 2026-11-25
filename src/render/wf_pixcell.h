#ifndef WF_PIXCELL_H
#define WF_PIXCELL_H

#include <stdint.h>

#include "../preview/wf_pixmap.h"
#include "wf_frame.h"

/*
 * Pictures drawn with ordinary cells, so they go through the same diff and
 * colour-depth path as the rest of the screen and need nothing from the
 * terminal beyond colours (spec 4.2).
 *
 *   HALFBLOCK  each cell is two stacked square pixels: the upper half block
 *              (U+2580) in the upper pixel's colour over the lower pixel's
 *              colour as background. The pixel grid is cols x 2*rows, so the
 *              aspect ratio assumes cells twice as tall as they are wide.
 *              When U+2580 has display width 2, a space with the two pixels'
 *              mean background colour is used instead.
 *   ASCII      the same grid, but a cell shows a character whose density
 *              follows the brightness of its two pixels, in their mean
 *              colour. For terminals without half blocks or colour.
 *
 * Transparent pixels are composited over `bg_rgb`.
 */
typedef enum
{
	WF_PIXSTYLE_HALFBLOCK,
	WF_PIXSTYLE_ASCII
} WfPixStyle;

/* Pixel grid for a cols x rows area: cols x 2*rows, with minima 1 x 2. */
void wf_pixcell_grid(int cols, int rows, uint32_t *w, uint32_t *h);

/*
 * How far a picture can be moved inside an area: the number of pixel columns
 * and rows of it that do not fit (0 when it fits). The pixel grid of an area
 * is cols x 2*rows.
 */
void wf_pixcell_pan_limits(const WfPixmap *pm,
        int cols,
        int rows,
        uint32_t *max_x,
        uint32_t *max_y);

/*
 * Like wf_pixcell_draw, for a picture larger than the area: the part starting
 * at pixel (off_x, off_y) is shown (offsets are clamped to wf_pixcell_pan_limits).
 * A picture that fits one way is centred that way and its offset ignored.
 */
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
        int *used_rows);

/*
 * Draws `pm` centred in the rectangle (x, y, cols, rows) of `f`, after
 * filling the rectangle with `bg_rgb`. A picture larger than the grid is
 * clipped, not scaled: the caller reduces it with wf_pixcell_grid and
 * wf_fit_box first. Optional used_cols/used_rows receive the coverage.
 */
void wf_pixcell_draw(WfFrame *f,
        int x,
        int y,
        int cols,
        int rows,
        const WfPixmap *pm,
        uint32_t bg_rgb,
        WfPixStyle style,
        int *used_cols,
        int *used_rows);

#endif
