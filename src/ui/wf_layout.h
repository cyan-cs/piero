#ifndef WF_LAYOUT_H
#define WF_LAYOUT_H

#include <stdbool.h>

typedef enum
{
	WF_PANE_PARENT,
	WF_PANE_CURRENT,
	WF_PANE_PREVIEW,
	WF_PANE_COUNT
} WfPane;

typedef struct
{
	int parent, current, preview; /* weights, 0..100 */
} WfRatios;

typedef struct
{
	int x, w; /* w == 0: pane hidden */
} WfSpan;

/*
 * Screen rows:  0 title | 1 rule | 2 pane headers | body... | rule | status
 * so body_h = rows - 5. `minimal` means the terminal is too small for the
 * normal chrome (< 40 cols or < 8 rows); callers draw a stub instead.
 */
typedef struct
{
	WfSpan pane[WF_PANE_COUNT];
	int body_y, body_h;
	bool minimal;
} WfLayout;

extern const WfRatios WF_RATIOS_DEFAULT; /* 25 / 45 / 30 */

/* Invalid ratios (outside 0..100, or all zero) fall back to the defaults. */
void wf_layout_compute(
        int cols, int rows, const WfRatios *ratios, WfLayout *out);

#endif
