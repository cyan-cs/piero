#include "wf_synruns.h"
#include "wf_synport.h"

#include <stdlib.h>
#include <string.h>

void wf_synanswer_free(WfSynAnswer *a)
{
	if (a) {
		free(a->runs);
		a->runs = NULL;
		a->nruns = 0;
	}
}

uint32_t wf_syn_runs_build(const uint8_t *cls,
        uint32_t len,
        uint32_t max_runs,
        WfSynRun **runs,
        uint32_t *nruns)
{
	uint32_t n = 0, i;
	WfSynRun *r;

	*runs = NULL;
	*nruns = 0;
	for (i = 0; i < len;) {
		uint32_t j = i + 1;

		while (j < len && cls[j] == cls[i]) {
			j++;
		}
		if (cls[i] && ++n > max_runs) {
			return WF_SYN_E_LIMIT;
		}
		i = j;
	}
	if (!n) {
		return WF_SYN_OK;
	}
	r = malloc((size_t)n * sizeof *r);
	if (!r) {
		return WF_SYN_E_NOMEM;
	}
	n = 0;
	for (i = 0; i < len;) {
		uint32_t j = i + 1;

		while (j < len && cls[j] == cls[i]) {
			j++;
		}
		if (cls[i]) {
			r[n].start = i;
			r[n].length = j - i;
			r[n].cls = cls[i];
			n++;
		}
		i = j;
	}
	*runs = r;
	*nruns = n;
	return WF_SYN_OK;
}

/* The first run whose end is after `at`. */
static uint32_t first_after(const WfSynRun *runs, uint32_t n, uint32_t at)
{
	uint32_t lo = 0, hi = n;

	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;

		if (runs[mid].start + runs[mid].length <= at) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return lo;
}

uint32_t wf_syn_runs_class_at(const WfSynRun *runs, uint32_t n, uint32_t at)
{
	uint32_t i = first_after(runs, n, at);

	return i < n && runs[i].start <= at ? runs[i].cls : 0;
}

void wf_syn_runs_fill(const WfSynRun *runs,
        uint32_t n,
        uint32_t from,
        uint32_t len,
        uint8_t *out)
{
	uint32_t i;

	memset(out, 0, len);
	if (!runs || !n) {
		return;
	}
	for (i = first_after(runs, n, from); i < n; i++) {
		uint32_t a = runs[i].start, b = runs[i].start + runs[i].length;

		if (a >= from + len) {
			break;
		}
		if (a < from) {
			a = from;
		}
		if (b > from + len) {
			b = from + len;
		}
		if (a < b) {
			memset(out + (a - from), (int)runs[i].cls, b - a);
		}
	}
}
