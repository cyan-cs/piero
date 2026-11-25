#ifndef WF_SIXEL_H
#define WF_SIXEL_H

#include "../base/wf_buf.h"
#include "../preview/wf_pixmap.h"

/*
 * Sixel (DEC graphics) output for a picture: a device control string the
 * terminal paints as pixels at the cursor. Self-contained: quantises to an
 * adaptive palette, encodes six pixel rows at a time with run-length
 * compression, no dependency.
 *
 * What this does NOT know is whether the terminal understands Sixel or how
 * big its cells are; the caller must have established both (wf_term_caps)
 * before using it, because a terminal that does not know the sequence prints
 * the payload as text.
 *
 * Pixels are composited over `bg_rgb` first. Pixels exactly equal to it are
 * not painted at all (the sequence selects "leave untouched"), which keeps
 * the output small for letterboxed pictures and lets the cell background show.
 *
 * `max_colors` is clamped to 2..256. Appends to `out`; on failure `out` is
 * left as it was found (the partial sequence is removed).
 */
#define WF_SIXEL_MAX_COLORS 256u
#define WF_SIXEL_MAX_PIXELS (2u * 1024u * 1024u)
#define WF_SIXEL_MAX_BYTES (2u * 1024u * 1024u)

typedef bool (*WfSixelCancelled)(void *ctx);
WfStatus wf_sixel_encode_ex(const WfPixmap *pm,
        uint32_t bg_rgb,
        unsigned max_colors,
        bool dither,
        WfSixelCancelled cancelled,
        void *ctx,
        WfBuf *out);

WfStatus wf_sixel_encode(
        const WfPixmap *pm, uint32_t bg_rgb, unsigned max_colors, WfBuf *out);

#endif
