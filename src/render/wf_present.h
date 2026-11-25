#ifndef WF_PRESENT_H
#define WF_PRESENT_H

#include "../base/wf_buf.h"
#include "wf_frame.h"

/*
 * Appends the VT sequence that turns the terminal from `prev` into `next`.
 * prev == NULL, or a different size, means a full redraw. This is the only
 * place that generates escape sequences from frame content (spec 4.2).
 *
 * Each dirty run starts with an absolute CUP so width-estimation errors
 * cannot drift. The bottom-right cell is never written (spec 4.4).
 */
WfStatus wf_present_diff(const WfFrame *prev, const WfFrame *next, WfBuf *out);

/*
 * How many colours the terminal is given. The frame always holds RGB; this
 * only changes how it is written out. TRUECOLOR writes it as is, 256 and 16
 * use the nearest colour of the terminal's palette, NONE writes no colour at
 * all (the terminal's own): a cell whose background is the active theme's
 * selection background (the selected row) is then drawn reversed so that the selection still shows.
 * The default is TRUECOLOR. UI thread only, like the presenter itself.
 */
typedef enum
{
	WF_DEPTH_TRUECOLOR,
	WF_DEPTH_256,
	WF_DEPTH_16,
	WF_DEPTH_NONE
} WfColorDepth;

void wf_present_set_color(WfColorDepth depth);
WfColorDepth wf_present_color_depth(void);

/* Nearest xterm-256 index (cube or grey ramp) and nearest of the 16 ANSI
 * colours (0-7 normal, 8-15 bright) for an RGB value. */
unsigned wf_color_to_256(uint32_t rgb);
unsigned wf_color_to_16(uint32_t rgb);

/*
 * What the environment says the terminal can show. `policy` is the
 * configuration (a WfColorPolicy: 0 auto, 1 truecolor, 2 256, 3 16, 4 none);
 * anything but auto is taken as given. Auto: a non-empty NO_COLOR means none
 * (https://no-color.org), TERM=dumb means none, COLORTERM=truecolor|24bit or
 * a Windows Terminal session (WT_SESSION) means truecolor, a TERM naming
 * "256color" without either means 256, and otherwise truecolor (Windows
 * consoles that run this program draw it). Arguments are the variables' values
 * or NULL.
 */
WfColorDepth wf_color_depth_resolve(int policy,
        const char *no_color,
        const char *colorterm,
        const char *wt_session,
        const char *term);

#endif
