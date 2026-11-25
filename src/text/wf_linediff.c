#include "wf_linediff.h"

#include <stdlib.h>
#include <string.h>

typedef struct
{
	const char *text;
	const WfDiffLine *line;
	size_t n;
	uint64_t *hash;
} Side;

static size_t effective_len(const char *s, size_t len)
{
	return len && '\r' == s[len - 1] ? len - 1 : len;
}

static uint64_t hash_line(const char *s, size_t len)
{
	uint64_t h = 14695981039346656037ull;

	for (size_t i = 0; i < len; i++) {
		h = (h ^ (unsigned char)s[i]) * 1099511628211ull;
	}
	return h;
}

/* How many lines the text has: a last line without a break counts. */
static size_t count_lines(const char *t, size_t n)
{
	size_t lines = 0;

	for (size_t i = 0; i < n; i++) {
		lines += '\n' == t[i];
	}
	return lines + (n && '\n' != t[n - 1]);
}

/* The lines of a text; *count is how many. NULL with *count 0 for no lines or no
 * memory (told apart by `ok`). */
static WfDiffLine *split(const char *t, size_t n, size_t *count, bool *ok)
{
	size_t lines = count_lines(t, n), start = 0, k = 0;
	WfDiffLine *l;

	*ok = true;
	*count = lines;
	if (!lines) {
		return NULL;
	}
	l = malloc(lines * sizeof *l);
	if (!l) {
		*ok = false;
		*count = 0;
		return NULL;
	}
	for (size_t i = 0; i < n; i++) {
		if ('\n' == t[i]) {
			l[k].off = start;
			l[k++].len = i - start;
			start = i + 1;
		}
	}
	if (start < n) {
		l[k].off = start;
		l[k++].len = n - start;
	}
	return l;
}

static bool same(const Side *a, size_t i, const Side *b, size_t j)
{
	const WfDiffLine *x = &a->line[i], *y = &b->line[j];
	size_t xl = effective_len(a->text + x->off, x->len);
	size_t yl = effective_len(b->text + y->off, y->len);

	return a->hash[i] == b->hash[j] && xl == yl &&
	        0 == memcmp(a->text + x->off, b->text + y->off, xl);
}

void wf_linediff_free(WfLineDiff *d)
{
	if (d) {
		free(d->a_text);
		free(d->b_text);
		free(d->a_line);
		free(d->b_line);
		free(d->h);
		memset(d, 0, sizeof *d);
	}
}

static char *copy_text(const char *t, size_t n)
{
	char *c = malloc(n + 1);

	if (c) {
		if (n) {
			memcpy(c, t, n);
		}
		c[n] = '\0';
	}
	return c;
}

/* The Myers search over lines [p, p+N) of a and [p, p+M) of b. Marks the lines
 * removed from a and added to b. WF_E_RANGE when the edits exceed the bound. */
static WfStatus myers(const Side *a,
        const Side *b,
        size_t p,
        size_t N,
        size_t M,
        uint8_t *del,
        uint8_t *ins,
        size_t *edits)
{
	size_t dmax = N + M < WF_LD_EDITS_MAX ? N + M : WF_LD_EDITS_MAX;
	int32_t *v = NULL, **trace = NULL;
	size_t found = 0;
	bool done = false;
	WfStatus st = WF_E_RANGE;
	ptrdiff_t off = (ptrdiff_t)dmax + 1;

	v = calloc(2 * dmax + 3, sizeof *v);
	trace = calloc(dmax + 1, sizeof *trace);
	if (!v || !trace) {
		st = WF_E_NOMEM;
		goto out;
	}
	for (size_t d = 0; d <= dmax && !done; d++) {
		ptrdiff_t di = (ptrdiff_t)d;

		for (ptrdiff_t k = -di; k <= di; k += 2) {
			ptrdiff_t x, y;

			if (k == -di ||
			        (k != di && v[off + k - 1] < v[off + k + 1])) {
				x = v[off + k + 1];
			} else {
				x = v[off + k - 1] + 1;
			}
			y = x - k;
			while ((size_t)x < N && (size_t)y < M &&
			        same(a, p + (size_t)x, b, p + (size_t)y)) {
				x++;
				y++;
			}
			v[off + k] = (int32_t)x;
			if ((size_t)x >= N && (size_t)y >= M) {
				found = d;
				done = true;
				break;
			}
		}
		if (!done) {
			trace[d] = malloc((2 * d + 1) * sizeof **trace);
			if (!trace[d]) {
				st = WF_E_NOMEM;
				goto out;
			}
			for (ptrdiff_t k = -di; k <= di; k += 2) {
				trace[d][k + di] = v[off + k];
			}
		}
	}
	if (!done) {
		goto out; /* WF_E_RANGE */
	}
	{
		ptrdiff_t x = (ptrdiff_t)N, y = (ptrdiff_t)M;

		for (size_t d = found; d > 0; d--) {
			ptrdiff_t di = (ptrdiff_t)d, k = x - y, prev_k, px, py;
			const int32_t *vp = trace[d - 1];
			ptrdiff_t base = di - 1;

			if (k == -di ||
			        (k != di &&
			                vp[k - 1 + base] < vp[k + 1 + base])) {
				prev_k = k + 1;
			} else {
				prev_k = k - 1;
			}
			px = vp[prev_k + base];
			py = px - prev_k;
			if (prev_k == k + 1) {
				ins[p + (size_t)py] = 1; /* b[py] was added */
			} else {
				del[p + (size_t)px] = 1; /* a[px] was removed */
			}
			x = px;
			y = py;
		}
		*edits = found;
		st = WF_OK;
	}
out:
	if (trace) {
		for (size_t d = 0; d <= dmax; d++) {
			free(trace[d]);
		}
	}
	free(trace);
	free(v);
	return st;
}

WfStatus wf_linediff(
        const char *a, size_t an, const char *b, size_t bn, WfLineDiff *out)
{
	WfLineDiff r;
	Side sa, sb;
	size_t pre = 0, suf = 0, N, M, edits = 0, nh = 0;
	uint8_t *del = NULL, *ins = NULL;
	WfStatus st = WF_E_NOMEM;
	bool ok_a, ok_b;

	if (!out || (!a && an) || (!b && bn)) {
		return WF_E_INVAL;
	}
	memset(out, 0, sizeof *out);
	/* Over the bounds is decided here, from the texts as they are: nothing is
	 * copied or allocated to find that out, and a length that cannot take the
	 * closing NUL of a copy is refused before it is read. */
	if (an > SIZE_MAX - 1 || bn > SIZE_MAX - 1 ||
	        count_lines(a, an) > WF_LD_LINES_MAX ||
	        count_lines(b, bn) > WF_LD_LINES_MAX) {
		return WF_E_RANGE;
	}
	memset(&r, 0, sizeof r);
	memset(&sa, 0, sizeof sa);
	memset(&sb, 0, sizeof sb);
	r.a_text = copy_text(a, an);
	r.b_text = copy_text(b, bn);
	if (!r.a_text || !r.b_text) {
		goto fail;
	}
	r.a_line = split(r.a_text, an, &r.a_n, &ok_a);
	r.b_line = split(r.b_text, bn, &r.b_n, &ok_b);
	if (!ok_a || !ok_b) {
		goto fail;
	}
	sa = (Side){r.a_text, r.a_line, r.a_n, NULL};
	sb = (Side){r.b_text, r.b_line, r.b_n, NULL};
	sa.hash = malloc((r.a_n + 1) * sizeof *sa.hash);
	sb.hash = malloc((r.b_n + 1) * sizeof *sb.hash);
	del = calloc(r.a_n + 1, 1);
	ins = calloc(r.b_n + 1, 1);
	if (!sa.hash || !sb.hash || !del || !ins) {
		goto fail;
	}
	for (size_t i = 0; i < r.a_n; i++) {
		sa.hash[i] = hash_line(r.a_text + r.a_line[i].off,
		        effective_len(
		                r.a_text + r.a_line[i].off, r.a_line[i].len));
	}
	for (size_t i = 0; i < r.b_n; i++) {
		sb.hash[i] = hash_line(r.b_text + r.b_line[i].off,
		        effective_len(
		                r.b_text + r.b_line[i].off, r.b_line[i].len));
	}
	while (pre < r.a_n && pre < r.b_n && same(&sa, pre, &sb, pre)) {
		pre++;
	}
	while (suf < r.a_n - pre && suf < r.b_n - pre &&
	        same(&sa, r.a_n - 1 - suf, &sb, r.b_n - 1 - suf)) {
		suf++;
	}
	N = r.a_n - pre - suf;
	M = r.b_n - pre - suf;
	if (0 == N) {
		memset(ins + pre, 1, M);
		edits = M;
	} else if (0 == M) {
		memset(del + pre, 1, N);
		edits = N;
	} else {
		st = myers(&sa, &sb, pre, N, M, del, ins, &edits);
		if (WF_OK != st) {
			goto fail;
		}
	}
	/* every way here (all new, all gone, or found by the search) is bounded the
	 * same: the result and what is drawn from it are never larger than this */
	if (edits > WF_LD_EDITS_MAX) {
		st = WF_E_RANGE;
		goto fail;
	}
	if (edits) {
		r.h = calloc(edits + 1, sizeof *r.h);
		if (!r.h) {
			st = WF_E_NOMEM;
			goto fail;
		}
		for (size_t i = 0, j = 0; i < r.a_n || j < r.b_n;) {
			WfDiffHunk h;

			if (i < r.a_n && j < r.b_n && !del[i] && !ins[j]) {
				i++;
				j++;
				continue;
			}
			h.a = i;
			h.b = j;
			while (i < r.a_n && del[i]) {
				i++;
			}
			while (j < r.b_n && ins[j]) {
				j++;
			}
			h.an = i - h.a;
			h.bn = j - h.b;
			if (0 == h.an && 0 == h.bn) {
				break; /* cannot happen: a mark that matches nothing */
			}
			if (nh < edits + 1) {
				r.h[nh++] = h;
			}
		}
		r.n = nh;
	}
	free(sa.hash);
	free(sb.hash);
	free(del);
	free(ins);
	*out = r;
	return WF_OK;
fail:
	free(sa.hash);
	free(sb.hash);
	free(del);
	free(ins);
	wf_linediff_free(&r);
	return st;
}
