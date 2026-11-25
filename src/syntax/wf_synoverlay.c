#include "wf_synoverlay.h"

#include <stdlib.h>
#include <string.h>

#include "wf_synruns.h"

void wf_synov_clear(WfSynOverlay *ov)
{
	if (!ov) {
		return;
	}
	free(ov->runs);
	free(ov->base);
	memset(ov, 0, sizeof *ov);
}

static bool runs_are_sane(const WfSynRun *runs, uint32_t n, size_t text_len)
{
	uint64_t end = 0;

	for (uint32_t i = 0; i < n; i++) {
		uint64_t s = runs[i].start, e = s + runs[i].length;

		if (!runs[i].length || s < end || e > text_len ||
		        runs[i].cls >= WF_SC_COUNT) {
			return false;
		}
		end = e;
	}
	return true;
}

bool wf_synov_set(WfSynOverlay *ov,
        WfSynRun *runs,
        uint32_t n,
        char *base,
        size_t base_len,
        size_t text_len)
{
	wf_synov_clear(ov);
	if (!runs_are_sane(runs, n, text_len)) {
		free(runs);
		free(base);
		return false;
	}
	ov->runs = runs;
	ov->n = n;
	ov->base = base;
	ov->base_len = base ? base_len : 0;
	ov->cur_len = ov->base_len;
	ov->pre = ov->base_len;
	ov->suf = 0;
	ov->active = true;
	return true;
}

void wf_synov_retarget(WfSynOverlay *ov, const char *cur, size_t cur_len)
{
	size_t pre = 0, suf = 0, max;

	if (!ov || !ov->active || !ov->base) {
		return;
	}
	max = ov->base_len < cur_len ? ov->base_len : cur_len;
	while (pre < max && ov->base[pre] == cur[pre]) {
		pre++;
	}
	if (pre == cur_len && pre == ov->base_len) {
		ov->pre = cur_len; /* the same text */
		ov->suf = 0;
		ov->cur_len = cur_len;
		return;
	}
	while (suf < max - pre &&
	        ov->base[ov->base_len - 1 - suf] == cur[cur_len - 1 - suf]) {
		suf++;
	}
	ov->pre = pre;
	ov->suf = suf;
	ov->cur_len = cur_len;
}

void wf_synov_fill(
        const WfSynOverlay *ov, size_t from, size_t len, uint8_t *out)
{
	size_t to = from + len;

	if (!len) {
		return;
	}
	if (!ov || !ov->active || !ov->runs) {
		memset(out, 0, len);
		return;
	}
	if (!ov->base) { /* the text never changes: the runs are of it */
		wf_syn_runs_fill(
		        ov->runs, ov->n, (uint32_t)from, (uint32_t)len, out);
		return;
	}
	memset(out, 0, len);
	if (from < ov->pre) { /* before the edit: the same offsets */
		size_t e = to < ov->pre ? to : ov->pre;

		wf_syn_runs_fill(ov->runs, ov->n, (uint32_t)from,
		        (uint32_t)(e - from), out);
	}
	{ /* after the edit: shifted by how much the text grew */
		size_t tail = ov->cur_len - ov->suf;
		size_t a = from > tail ? from : tail;

		if (a < to && ov->suf) {
			size_t old = a + ov->base_len - ov->cur_len;

			wf_syn_runs_fill(ov->runs, ov->n, (uint32_t)old,
			        (uint32_t)(to - a), out + (a - from));
		}
	}
}
