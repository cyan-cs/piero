#include "wf_layout.h"

#include <string.h>

const WfRatios WF_RATIOS_DEFAULT = {25, 45, 30};

static bool ratios_valid(const WfRatios *r)
{
	return r->parent >= 0 && r->current >= 0 && r->preview >= 0 &&
	        r->parent <= 100 && r->current <= 100 && r->preview <= 100 &&
	        r->parent + r->current + r->preview > 0;
}

void wf_layout_compute(
        int cols, int rows, const WfRatios *ratios, WfLayout *out)
{
	int weight[WF_PANE_COUNT];
	int order[WF_PANE_COUNT] = {
	        WF_PANE_CURRENT, WF_PANE_PREVIEW, WF_PANE_PARENT};
	int active = 0, sum = 0, content, given = 0, x, i;

	if (!out) {
		return;
	}
	memset(out, 0, sizeof *out);
	out->body_y = 3;
	out->body_h = rows - 5;
	if (cols < 40 || rows < 8) {
		out->minimal = true;
		out->body_h = 0;
		return;
	}

	if (!ratios || !ratios_valid(ratios)) {
		ratios = &WF_RATIOS_DEFAULT;
	}
	weight[WF_PANE_PARENT] = ratios->parent;
	weight[WF_PANE_CURRENT] = ratios->current;
	weight[WF_PANE_PREVIEW] = ratios->preview;

	/* Width tiers (spec 2.1). Narrow terminals shed panes, Parent first. */
	if (cols < 60) {
		weight[WF_PANE_PARENT] = 0;
		weight[WF_PANE_PREVIEW] = 0;
	} else if (cols < 80) {
		weight[WF_PANE_PARENT] = 0;
	} else if (cols < 120) {
		weight[WF_PANE_PARENT] /= 2;
	}
	if (0 == weight[WF_PANE_CURRENT]) {
		weight[WF_PANE_CURRENT] = 1; /* Current is never hidden */
	}

	for (i = 0; i < WF_PANE_COUNT; i++) {
		if (weight[i] > 0) {
			active++;
			sum += weight[i];
		}
	}

	content = cols - (active - 1); /* separator columns come off first */
	for (i = 0; i < WF_PANE_COUNT; i++) {
		if (weight[i] > 0) {
			out->pane[i].w =
			        (int)((long long)content * weight[i] / sum);
			given += out->pane[i].w;
		}
	}
	/* remainder goes Current -> Preview -> Parent */
	for (i = 0; given < content; i = (i + 1) % WF_PANE_COUNT) {
		if (weight[order[i]] > 0) {
			out->pane[order[i]].w++;
			given++;
		}
	}

	x = 0;
	for (i = 0; i < WF_PANE_COUNT; i++) {
		if (0 == out->pane[i].w) {
			continue;
		}
		out->pane[i].x = x;
		x += out->pane[i].w + 1;
	}
}
