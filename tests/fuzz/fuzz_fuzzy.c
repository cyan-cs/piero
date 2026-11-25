/* libFuzzer harness: the fuzzy search engine (DR-DRAFT-023). Any UTF-16 units
 * (lone surrogates, names far past the limit, queries past the limit) must never
 * read or write outside a buffer, the rolling score must equal the full-matrix
 * score and the plain score of the path read back, and every position must be
 * the start of a character inside the name. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "search/wf_fuzzy.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	size_t units = size / 2, qn, nn;
	wchar_t *w = malloc((units + 1) * sizeof *w);
	WfFzScratch *x = wf_fz_scratch_new();
	WfFzQuery q;

	if (!w || !x) {
		abort();
	}
	memcpy(w, data, units * sizeof *w);
	w[units] = 0;
	if (0 == units) {
		goto done;
	}
	/* the first unit says how many of the rest are the query */
	qn = (uint16_t)w[0] % units;
	nn = units - 1 - qn;
	if (units < 1 + qn || !wf_fz_query_set(&q, w + 1, qn)) {
		goto done;
	}
	{
		const wchar_t *name = w + 1 + qn;
		uint32_t pos[WF_FZ_QUERY_MAX];
		int32_t a = wf_fz_score(&q, name, nn, x), b = 0;
		bool ok = wf_fz_positions(&q, name, nn, x, pos, &b);

		if ((WF_FZ_NO_MATCH != a) != ok) {
			abort();
		}
		if (ok) {
			if (a != b ||
			        wf_fz_path_score(&q, name, nn, x, pos) != a) {
				abort();
			}
			for (size_t i = 0; i < q.n; i++) {
				if (pos[i] >= nn ||
				        0 == wf_fz_cp_units(name, nn, pos[i]) ||
				        (i && pos[i] <= pos[i - 1])) {
					abort();
				}
			}
		}
		(void)wf_fz_prefilter(&q, name, nn);
	}
done:
	wf_fz_scratch_free(x);
	free(w);
	return 0;
}
