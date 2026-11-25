#include "wf_gitstatus.h"

#include <stdlib.h>
#include <string.h>

static bool letter_ok(char c)
{
	return NULL != strchr(".MADRCTU", c) && '\0' != c;
}

/* The text after the `k`-th space of the record, or NULL if it has fewer. */
static char *after_spaces(char *rec, size_t k)
{
	for (; k && *rec; rec++) {
		if (' ' == *rec) {
			k--;
		}
	}
	return 0 == k && *rec ? rec : NULL;
}

static int by_path(const void *a, const void *b)
{
	const WfGitEntry *x = a, *y = b;
	size_t n = x->path_n < y->path_n ? x->path_n : y->path_n;
	int c = memcmp(x->path, y->path, n);

	if (c) {
		return c;
	}
	return x->path_n < y->path_n ? -1 : x->path_n > y->path_n;
}

void wf_gitstatus_free(WfGitStatus *s)
{
	if (s) {
		free(s->e);
		free(s->blob);
		memset(s, 0, sizeof *s);
	}
}

WfStatus wf_gitstatus_parse(const uint8_t *data, size_t n, WfGitStatus *out)
{
	WfGitStatus t = {0};
	char *p, *end;
	size_t records;

	if (!out || (!data && n)) {
		return WF_E_INVAL;
	}
	memset(out, 0, sizeof *out);
	if (0 == n) {
		return WF_OK;
	}
	/* a record is a run of bytes between NULs: an empty one is skipped below
	 * and has no entry, so it is not counted (a stream of NULs asks for
	 * nothing); and the table is bounded, whatever the input */
	records = 0;
	for (size_t i = 0; i < n; i++) {
		records += 0 != data[i] && (0 == i || 0 == data[i - 1]);
	}
	if (records > WF_GITSTATUS_MAX_ENTRIES) {
		return WF_E_RANGE;
	}
	t.blob = malloc(n + 1);
	t.e = calloc(records ? records : 1, sizeof *t.e);
	if (!t.blob || !t.e) {
		wf_gitstatus_free(&t);
		return WF_E_NOMEM;
	}
	memcpy(t.blob, data, n);
	t.blob[n] = '\0';
	p = t.blob;
	end = t.blob + n;
	while (p < end) {
		char *rec = p, *path;
		size_t len = strlen(rec), fields;
		WfGitEntry e = {0};

		p += len + 1;
		if (0 == len || '#' == rec[0] || '!' == rec[0]) {
			continue; /* nothing, a header, or an ignored file */
		}
		if ('?' == rec[0]) {
			if (' ' != rec[1] || !rec[2]) {
				goto bad;
			}
			e.path = rec + 2;
			e.index = '.';
			e.work = '?';
		} else if ('1' == rec[0] || '2' == rec[0] || 'u' == rec[0]) {
			fields = '1' == rec[0] ? 8 : '2' == rec[0] ? 9 : 10;
			if (len < 6 || ' ' != rec[1] || ' ' != rec[4] ||
			        !letter_ok(rec[2]) || !letter_ok(rec[3])) {
				goto bad;
			}
			path = after_spaces(rec, fields);
			if (!path) {
				goto bad;
			}
			e.path = path;
			e.index = rec[2];
			e.work = rec[3];
			e.conflict = 'u' == rec[0];
			if ('2' ==
			        rec[0]) { /* the old path is the next field */
				if (p >= end) {
					goto bad;
				}
				e.orig = p;
				p += strlen(p) + 1;
			}
		} else {
			goto bad;
		}
		e.path_n = strlen(e.path);
		if (0 == e.path_n) {
			goto bad;
		}
		t.e[t.n++] = e;
	}
	qsort(t.e, t.n, sizeof *t.e, by_path);
	*out = t;
	return WF_OK;
bad:
	wf_gitstatus_free(&t);
	return WF_E_CORRUPT;
}

const WfGitEntry *wf_gitstatus_find(
        const WfGitStatus *s, const char *path, size_t n)
{
	size_t lo = 0, hi;

	if (!s || !path) {
		return NULL;
	}
	hi = s->n;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		const WfGitEntry *e = &s->e[mid];
		size_t m = e->path_n < n ? e->path_n : n;
		int c = memcmp(e->path, path, m);

		if (0 == c) {
			c = e->path_n < n ? -1 : e->path_n > n;
		}
		if (0 == c) {
			return e;
		}
		if (c < 0) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return NULL;
}

const WfGitEntry *wf_gitstatus_find_covering(
        const WfGitStatus *s, const char *path, size_t n)
{
	const WfGitEntry *e = wf_gitstatus_find(s, path, n);

	for (size_t i = 0; !e && i + 1 < n; i++) {
		if ('/' == path[i]) {
			const WfGitEntry *dir =
			        wf_gitstatus_find(s, path, i + 1);

			e = dir && '?' == dir->work ? dir : NULL;
		}
	}
	return e;
}

bool wf_gitstatus_has_under(const WfGitStatus *s, const char *dir, size_t n)
{
	size_t lo = 0, hi;

	if (!s || !dir || 0 == n) {
		return false;
	}
	hi = s->n;
	while (lo < hi) { /* the first record that does not sort before `dir` */
		size_t mid = lo + (hi - lo) / 2;
		const WfGitEntry *e = &s->e[mid];
		size_t m = e->path_n < n ? e->path_n : n;
		int c = memcmp(e->path, dir, m);

		if (0 == c) {
			c = e->path_n < n ? -1 : 0;
		}
		if (c < 0) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return lo < s->n && s->e[lo].path_n >= n &&
	        0 == memcmp(s->e[lo].path, dir, n);
}

char wf_gitentry_letter(const WfGitEntry *e)
{
	if (!e) {
		return 0;
	}
	if (e->conflict) {
		return '!';
	}
	if ('?' == e->work) {
		return 'U';
	}
	if ('D' == e->work) {
		return 'D';
	}
	if ('A' == e->index) {
		return 'A';
	}
	if ('R' == e->index) {
		return 'R';
	}
	if ('M' == e->work || 'T' == e->work) {
		return 'M';
	}
	if ('C' == e->index) {
		return 'A';
	}
	if ('D' == e->index) {
		return 'D';
	}
	if ('M' == e->index || 'T' == e->index) {
		return 'M';
	}
	return 0;
}
