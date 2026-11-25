#include "wf_fuzzy.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

enum
{
	S_MATCH = 16,
	S_GAP_START = -3,
	S_GAP_EXT = -1,
	B_WHITE = 10,  /* after a space, and the start of a name */
	B_PATH = 9,    /* after \ or / */
	B_NONWORD = 8, /* after _ - . or another symbol; a symbol itself */
	B_CAMEL = 7,   /* lower to upper, letter to digit */
	B_CONSEC = 4,  /* inside a run */
	B_FIRST_MULT = 2
};

#define LEAD_CAP 8u /* positions before the first match that cost a point */
#define LEN_CAP 64u /* characters beyond the query that cost a point */

/* The most a score can reach or fall: per query character at most
 * match + 2 * boundary (first) or match + boundary, and gaps of at most
 * the name's length. Far inside int32. */
_Static_assert((int64_t)WF_FZ_QUERY_MAX *(S_MATCH + 2 * B_WHITE) +
                        (int64_t)WF_FZ_NAME_MAX * 3 <
                INT32_MAX / 4,
        "scores must not overflow");

/* "No score here": far from INT32_MIN so that adding a gap cannot wrap. */
#define NEG (-(1 << 28))
#define NEG_LIM (NEG / 2)

typedef enum
{
	K_NONWORD,
	K_WHITE,
	K_PATH,
	K_LOWER,
	K_UPPER,
	K_DIGIT
} Kind;

/* One character at *i, advancing; a lone surrogate is its own value (it matches
 * only itself and never joins a pair). */
static inline uint32_t next_cp(const wchar_t *s, size_t n, size_t *i)
{
	uint32_t c = (uint16_t)s[(*i)++];

	if (c >= 0xD800 && c <= 0xDBFF && *i < n) {
		uint32_t d = (uint16_t)s[*i];

		if (d >= 0xDC00 && d <= 0xDFFF) {
			(*i)++;
			return 0x10000u + ((c - 0xD800u) << 10) + (d - 0xDC00u);
		}
	}
	return c;
}

size_t wf_fz_cp_units(const wchar_t *s, size_t len, size_t at)
{
	size_t i = at;

	if (at >= len) {
		return 0;
	}
	(void)next_cp(s, len, &i);
	return i - at;
}

uint32_t wf_fz_fold(uint32_t c)
{
	if (c < 0x80) {
		return c >= 'A' && c <= 'Z' ? c + 32 : c;
	}
	if (c < 0x100) {
		return c >= 0xC0 && c <= 0xDE && c != 0xD7 ? c + 32 : c;
	}
	if (c < 0x180) { /* Latin Extended-A: upper and lower alternate */
		if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) {
			return c | 1u;
		}
		if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) {
			return (c & 1u) ? c + 1 : c;
		}
		return c == 0x178 ? 0xFFu : c;
	}
	if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) {
		return c + 32;
	}
	if (c >= 0x410 && c <= 0x42F) {
		return c + 32;
	}
	if (c >= 0x400 && c <= 0x40F) {
		return c + 80;
	}
	if (c >= 0x30A1 && c <= 0x30F6) {
		return c - 0x60; /* Katakana to Hiragana */
	}
	if (c >= 0xFF21 && c <= 0xFF3A) {
		return c - 0xFF21 + 'a';
	}
	if (c >= 0xFF41 && c <= 0xFF5A) {
		return c - 0xFF41 + 'a';
	}
	if (c >= 0xFF10 && c <= 0xFF19) {
		return c - 0xFF10 + '0';
	}
	return c;
}

static bool upper_beyond_ascii(uint32_t c)
{
	if (c >= 0xC0 && c <= 0xDE) {
		return c != 0xD7;
	}
	if (c >= 0x100 && c <= 0x17E) {
		return (c <= 0x137 && !(c & 1u)) ||
		        (c >= 0x139 && c <= 0x148 && (c & 1u)) ||
		        (c >= 0x14A && c <= 0x177 && !(c & 1u)) || c == 0x178 ||
		        (c >= 0x179 && (c & 1u));
	}
	return (c >= 0x391 && c <= 0x3A9) || (c >= 0x400 && c <= 0x42F) ||
	        (c >= 0xFF21 && c <= 0xFF3A);
}

static Kind kind_of(uint32_t c)
{
	if (c < 0x80) {
		if (c >= 'a' && c <= 'z') {
			return K_LOWER;
		}
		if (c >= 'A' && c <= 'Z') {
			return K_UPPER;
		}
		if (c >= '0' && c <= '9') {
			return K_DIGIT;
		}
		if (' ' == c) {
			return K_WHITE;
		}
		return '\\' == c || '/' == c ? K_PATH : K_NONWORD;
	}
	if (0xA0 == c || 0x3000 == c) {
		return K_WHITE;
	}
	if (upper_beyond_ascii(c)) {
		return K_UPPER;
	}
	if (c >= 0xFF10 && c <= 0xFF19) {
		return K_DIGIT;
	}
	if ((c >= 0xA1 && c <= 0xBF && 0xAA != c && 0xB5 != c && 0xBA != c) ||
	        0xD7 == c || 0xF7 == c || (c >= 0x2000 && c <= 0x206F) ||
	        (c >= 0x3001 && c <= 0x303F) || (c >= 0xFF01 && c <= 0xFF0F) ||
	        (c >= 0xFF1A && c <= 0xFF20) || (c >= 0xFF3B && c <= 0xFF40) ||
	        (c >= 0xFF5B && c <= 0xFF65)) {
		return K_NONWORD;
	}
	return K_LOWER; /* letters of scripts without case, and anything else */
}

static uint8_t bonus_for(Kind prev, Kind cur)
{
	if (cur < K_LOWER) {
		return B_NONWORD; /* a symbol or a space matched itself */
	}
	switch (prev) {
	case K_WHITE:   return B_WHITE;
	case K_PATH:    return B_PATH;
	case K_NONWORD: return B_NONWORD;
	default:        break;
	}
	if (K_LOWER == prev && K_UPPER == cur) {
		return B_CAMEL;
	}
	if (K_DIGIT != prev && K_DIGIT == cur) {
		return B_CAMEL;
	}
	return 0;
}

bool wf_fz_query_set(WfFzQuery *q, const wchar_t *s, size_t len)
{
	size_t i = 0;

	q->n = 0;
	while (i < len) {
		if (q->n >= WF_FZ_QUERY_MAX) {
			q->n = 0;
			return false;
		}
		q->cp[q->n++] = wf_fz_fold(next_cp(s, len, &i));
	}
	return q->n > 0;
}

struct WfFzScratch
{
	uint32_t t[WF_FZ_NAME_MAX];    /* the name's characters, folded */
	uint32_t unit[WF_FZ_NAME_MAX]; /* where each starts, in units */
	uint8_t bonus[WF_FZ_NAME_MAX];
	size_t n; /* characters decoded */
	int32_t row[2][WF_FZ_NAME_MAX];
	uint8_t run[2][WF_FZ_NAME_MAX];
	int32_t *hm; /* QUERY_MAX x NAME_MAX, once positions are asked for */
	uint8_t *rm;
};

WfFzScratch *wf_fz_scratch_new(void)
{
	return calloc(1, sizeof(WfFzScratch));
}

void wf_fz_scratch_free(WfFzScratch *x)
{
	if (x) {
		free(x->hm);
		free(x->rm);
		free(x);
	}
}

bool wf_fz_prefilter(const WfFzQuery *q, const wchar_t *s, size_t len)
{
	size_t i = 0, k = 0;

	if (0 == q->n) {
		return true;
	}
	if (len < q->n) {
		return false; /* units are never fewer than characters */
	}
	while (i < len && k < q->n) {
		uint32_t c = (uint16_t)s[i];

		if (c < 0x80) { /* the common case, without the call */
			i++;
			k += (c - 'A' < 26u ? c + 32u : c) == q->cp[k];
			continue;
		}
		if (wf_fz_fold(next_cp(s, len, &i)) == q->cp[k]) {
			k++;
		}
	}
	return k == q->n;
}

/* The first CHARACTERS of the name, folded, with where each begins and the
 * bonus its place gives. */
static void decode(const wchar_t *s, size_t len, WfFzScratch *x)
{
	size_t i = 0, n = 0;
	Kind prev = K_WHITE; /* the start of a name counts as after a space */

	while (i < len && n < WF_FZ_NAME_MAX) {
		size_t at = i;
		uint32_t c = next_cp(s, len, &i);
		Kind k = kind_of(c);

		x->t[n] = wf_fz_fold(c);
		x->unit[n] = (uint32_t)at;
		x->bonus[n] = bonus_for(prev, k);
		prev = k;
		n++;
	}
	x->n = n;
}

static int32_t max32(int32_t a, int32_t b)
{
	return a > b ? a : b;
}

/* How the next character's bonus and run length follow from the run that ends
 * before it. `run` is the length of that run (0: no run, the previous cell was
 * a gap), `j` the index of the character. */
static int32_t run_step(
        const WfFzScratch *x, size_t j, unsigned run, unsigned *out_run)
{
	unsigned consec = run + 1;
	int32_t b = x->bonus[j];

	if (consec > 1) {
		int32_t first = x->bonus[j - consec + 1];

		if (b >= B_NONWORD && b > first) {
			consec = 1; /* a word start begins a new run */
		} else {
			b = max32(b, max32(B_CONSEC, first));
		}
	}
	*out_run = consec;
	return b;
}

/* The programme over the decoded name. Rolling (two rows) when `full` is
 * false; otherwise every row is kept in the matrices so that the path can be
 * read back. Returns the best score before the length term, and with `full`
 * the column (from `*lo`) it ends in. */
static int32_t run_dp(const WfFzQuery *q,
        WfFzScratch *x,
        bool full,
        size_t *lo_out,
        size_t *best_at)
{
	size_t m = q->n, lo = 0, hi = 0, w, i, jr;
	int32_t best = NEG;
	bool found = false;

	if (0 == m || x->n < m) {
		return NEG;
	}
	for (lo = 0; lo < x->n && x->t[lo] != q->cp[0]; lo++) {
	}
	for (hi = x->n; hi > 0 && x->t[hi - 1] != q->cp[m - 1]; hi--) {
	}
	if (lo >= x->n || hi <= lo || hi - lo < m) {
		return NEG;
	}
	w = hi - lo; /* the window is [lo, hi) */
	for (i = 0; i < m; i++) {
		size_t jstart = i, jend = w - (m - i); /* inclusive */
		int32_t *H = full ? x->hm + i * WF_FZ_NAME_MAX : x->row[i & 1];
		uint8_t *R = full ? x->rm + i * WF_FZ_NAME_MAX : x->run[i & 1];
		const int32_t *PH = NULL;
		const uint8_t *PR = NULL;

		if (i > 0) {
			PH = full ? x->hm + (i - 1) * WF_FZ_NAME_MAX
			          : x->row[(i - 1) & 1];
			PR = full ? x->rm + (i - 1) * WF_FZ_NAME_MAX
			          : x->run[(i - 1) & 1];
		}
		for (jr = jstart; jr <= jend; jr++) {
			size_t j = lo + jr;
			int32_t s1 = NEG, s2 = NEG;
			unsigned run = 0;

			if (x->t[j] == q->cp[i]) {
				if (0 == i) {
					s1 = S_MATCH +
					        (int32_t)x->bonus[j] *
					                B_FIRST_MULT -
					        (int32_t)(j < LEAD_CAP
					                        ? j
					                        : LEAD_CAP);
					run = 1;
				} else if (PH[jr - 1] > NEG_LIM) {
					int32_t b = run_step(
					        x, j, PR[jr - 1], &run);

					s1 = PH[jr - 1] + S_MATCH + b;
				}
			}
			if (jr > jstart && H[jr - 1] > NEG_LIM) {
				s2 = H[jr - 1] +
				        (R[jr - 1] > 0 ? S_GAP_START
				                       : S_GAP_EXT);
			}
			if (s1 > NEG_LIM && s1 >= s2) {
				H[jr] = s1;
				R[jr] = (uint8_t)run;
			} else {
				H[jr] = s2;
				R[jr] = 0;
			}
		}
		if (m - 1 == i) {
			for (jr = jstart; jr <= jend; jr++) {
				if (R[jr] > 0 && (!found || H[jr] > best)) {
					best = H[jr];
					*best_at = jr;
					found = true;
				}
			}
		}
	}
	*lo_out = lo;
	return found ? best : NEG;
}

/* What the length of the name costs: one point for each character beyond the
 * query's, to a limit. */
static int32_t length_cost(const WfFzQuery *q, const WfFzScratch *x)
{
	size_t extra = x->n > q->n ? x->n - q->n : 0;

	return (int32_t)(extra < LEN_CAP ? extra : LEN_CAP);
}

/* The caller has already checked the subsequence prefilter. */
static int32_t score_passed(
        const WfFzQuery *q, const wchar_t *s, size_t len, WfFzScratch *x)
{
	size_t lo, at;
	int32_t best;

	decode(s, len, x);
	best = run_dp(q, x, false, &lo, &at);
	return best <= NEG_LIM ? WF_FZ_NO_MATCH : best - length_cost(q, x);
}

int32_t wf_fz_score(
        const WfFzQuery *q, const wchar_t *s, size_t len, WfFzScratch *x)
{
	if (0 == q->n || !wf_fz_prefilter(q, s, len)) {
		return WF_FZ_NO_MATCH;
	}
	return score_passed(q, s, len, x);
}

bool wf_fz_positions(const WfFzQuery *q,
        const wchar_t *s,
        size_t len,
        WfFzScratch *x,
        uint32_t *units,
        int32_t *score)
{
	size_t lo = 0, at = 0, i, jr;
	int32_t best;

	if (!x->hm) {
		x->hm = malloc((size_t)WF_FZ_QUERY_MAX * WF_FZ_NAME_MAX *
		        sizeof *x->hm);
		x->rm = malloc((size_t)WF_FZ_QUERY_MAX * WF_FZ_NAME_MAX *
		        sizeof *x->rm);
		if (!x->hm || !x->rm) {
			free(x->hm);
			free(x->rm);
			x->hm = NULL;
			x->rm = NULL;
			return false;
		}
	}
	if (!wf_fz_prefilter(q, s, len) || 0 == q->n) {
		return false;
	}
	decode(s, len, x);
	best = run_dp(q, x, true, &lo, &at);
	if (best <= NEG_LIM) {
		return false;
	}
	/* read the path back from the cells: a cell with a run is a match; one
	 * without is a gap carried from its left neighbour */
	i = q->n - 1;
	jr = at;
	for (;;) {
		units[i] = x->unit[lo + jr];
		if (0 == i) {
			break;
		}
		i--;
		jr--;
		while (jr > i && 0 == x->rm[i * WF_FZ_NAME_MAX + jr]) {
			jr--;
		}
	}
	if (score) {
		*score = best - length_cost(q, x);
	}
	return true;
}

int32_t wf_fz_path_score(const WfFzQuery *q,
        const wchar_t *s,
        size_t len,
        WfFzScratch *x,
        const uint32_t *units)
{
	size_t idx[WF_FZ_QUERY_MAX], i, j = 0;
	int32_t score = 0;
	unsigned run = 1;

	if (0 == q->n) {
		return WF_FZ_NO_MATCH;
	}
	decode(s, len, x);
	for (i = 0; i < q->n; i++) {
		while (j < x->n && x->unit[j] != units[i]) {
			j++;
		}
		if (j >= x->n || x->t[j] != q->cp[i] ||
		        (i && j <= idx[i - 1])) {
			return WF_FZ_NO_MATCH;
		}
		idx[i] = j;
	}
	for (i = 0; i < q->n; i++) {
		j = idx[i];
		if (0 == i) {
			score = S_MATCH + (int32_t)x->bonus[j] * B_FIRST_MULT -
			        (int32_t)(j < LEAD_CAP ? j : LEAD_CAP);
			run = 1;
		} else if (j == idx[i - 1] + 1) {
			score += S_MATCH + run_step(x, j, run, &run);
		} else {
			size_t gap = j - idx[i - 1] - 1;

			score += S_GAP_START + (int32_t)(gap - 1) * S_GAP_EXT;
			score += S_MATCH + run_step(x, j, 0, &run);
		}
	}
	return score - length_cost(q, x);
}

bool wf_fz_better(WfFzHit a, WfFzHit b)
{
	return a.score > b.score || (a.score == b.score && a.index < b.index);
}

WfStatus wf_fz_topk_init(WfFzTopK *t, size_t k)
{
	t->n = 0;
	t->k = k;
	t->hit = NULL;
	if (0 == k) {
		return WF_OK;
	}
	if (k > SIZE_MAX / sizeof *t->hit) {
		t->k = 0;
		return WF_E_RANGE;
	}
	t->hit = malloc(k * sizeof *t->hit);
	if (!t->hit) {
		t->k = 0;
		return WF_E_NOMEM;
	}
	return WF_OK;
}

void wf_fz_topk_free(WfFzTopK *t)
{
	free(t->hit);
	t->hit = NULL;
	t->n = t->k = 0;
}

static void sift_up(WfFzHit *h, size_t i)
{
	while (i > 0) {
		size_t p = (i - 1) / 2;
		WfFzHit tmp;

		if (!wf_fz_better(h[p], h[i])) {
			break; /* the parent is the worse one: the heap is right */
		}
		tmp = h[p];
		h[p] = h[i];
		h[i] = tmp;
		i = p;
	}
}

static void sift_down(WfFzHit *h, size_t n, size_t i)
{
	for (;;) {
		size_t l = 2 * i + 1, r = l + 1, w = i;
		WfFzHit tmp;

		if (l < n && wf_fz_better(h[w], h[l])) {
			w = l;
		}
		if (r < n && wf_fz_better(h[w], h[r])) {
			w = r;
		}
		if (w == i) {
			return;
		}
		tmp = h[w];
		h[w] = h[i];
		h[i] = tmp;
		i = w;
	}
}

void wf_fz_topk_offer(WfFzTopK *t, WfFzHit h)
{
	if (0 == t->k) {
		return;
	}
	if (t->n < t->k) {
		t->hit[t->n] = h;
		sift_up(t->hit, t->n);
		t->n++;
	} else if (wf_fz_better(h, t->hit[0])) {
		t->hit[0] = h;
		sift_down(t->hit, t->n, 0);
	}
}

static int by_rank(const void *a, const void *b)
{
	WfFzHit x = *(const WfFzHit *)a, y = *(const WfFzHit *)b;

	return wf_fz_better(x, y) ? -1 : wf_fz_better(y, x) ? 1 : 0;
}

void wf_fz_topk_finish(WfFzTopK *t)
{
	if (t->n > 1) {
		qsort(t->hit, t->n, sizeof *t->hit, by_rank);
	}
}

#define CANCEL_EVERY 32u

WfStatus wf_fz_search(const WfFzQuery *q,
        const WfEntry *e,
        const size_t *vis,
        size_t nvis,
        WfFzScratch *x,
        WfFzTopK *top,
        WfFzCancel cancel,
        void *cancel_ctx,
        WfFzStats *stats)
{
	WfFzStats st = {0};
	size_t i;

	if (!q || !e || !x || !top || (nvis && !vis) || 0 == q->n) {
		return WF_E_INVAL;
	}
	for (i = 0; i < nvis; i++) {
		const wchar_t *name;
		size_t len;
		int32_t score;

		if (cancel && 0 == i % CANCEL_EVERY && cancel(cancel_ctx)) {
			st.cancelled = true;
			break;
		}
		name = e[vis[i]].name;
		len = wcslen(name);
		st.scanned++;
		if (!wf_fz_prefilter(q, name, len)) {
			continue;
		}
		st.passed++;
		score = score_passed(q, name, len, x);
		if (WF_FZ_NO_MATCH == score) {
			continue;
		}
		st.matched++;
		wf_fz_topk_offer(top, (WfFzHit){score, (uint32_t)i});
	}
	wf_fz_topk_finish(top);
	if (stats) {
		*stats = st;
	}
	return WF_OK;
}

void wf_fz_answer_free(WfFzAnswer *a)
{
	if (a) {
		free(a->hit);
		a->hit = NULL;
		a->nhit = 0;
	}
}
