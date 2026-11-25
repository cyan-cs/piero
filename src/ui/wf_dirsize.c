#include "wf_dirsize.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../path/wf_path.h"

void wf_dirsizes_free(WfDirSizes *s)
{
	for (size_t i = 0; s->name && i < s->n; i++) {
		free(s->name[i]);
	}
	free(s->name);
	free(s->u);
	free(s->dir);
	s->name = NULL;
	s->u = NULL;
	s->dir = NULL;
	s->n = 0;
}

void wf_dirsizes_clear(WfDirSizes *s)
{
	wf_dirsizes_free(s);
	memset(s, 0, sizeof *s);
}

static int by_name(const void *a, const void *b)
{
	return wcscmp(*(wchar_t *const *)a, *(wchar_t *const *)b);
}

bool wf_dirsizes_begin(WfDirSizes *s,
        uint32_t gen,
        const wchar_t *dir,
        const wchar_t *const *names,
        size_t n)
{
	WfDirSizes next;

	memset(&next, 0, sizeof next);
	next.dir = wf_path_dup(dir);
	next.name = calloc(n, sizeof *next.name);
	next.u = calloc(n, sizeof *next.u);
	next.n = n; /* what wf_dirsizes_free walks; unset names are NULL */
	for (size_t i = 0; next.name && i < n; i++) {
		next.name[i] = wf_path_dup(names[i]);
		if (!next.name[i]) {
			wf_dirsizes_free(&next);
			return false;
		}
	}
	if (!next.dir || !next.name || !next.u) {
		wf_dirsizes_free(&next);
		return false;
	}
	qsort(next.name, n, sizeof *next.name, by_name);
	next.gen = gen;
	next.running = true;
	next.asked = n;
	wf_dirsizes_free(s);
	*s = next;
	return true;
}

bool wf_dirsizes_take(WfDirSizes *s, const WfSizeUpdate *u)
{
	if (!s->gen || u->gen != s->gen || u->index >= s->n) {
		return false;
	}
	if (u->finished && !s->u[u->index].finished) {
		s->done++;
	}
	s->u[u->index] = *u; /* its gen is the request's, so never 0 */
	if (s->done >= s->asked) {
		s->running = false;
	}
	return true;
}

const WfSizeUpdate *wf_dirsizes_find(
        const WfDirSizes *s, const wchar_t *dir, const wchar_t *name)
{
	size_t lo = 0, hi = s->n;

	if (!s->dir || !dir || !name || wcscmp(s->dir, dir)) {
		return NULL;
	}
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		int c = wcscmp(name, s->name[mid]);

		if (!c) {
			return s->u[mid].gen ? &s->u[mid] : NULL;
		}
		if (c < 0) {
			hi = mid;
		} else {
			lo = mid + 1;
		}
	}
	return NULL;
}

void wf_dirsizes_short(const WfSizeUpdate *u, char *out, size_t cap)
{
	static const char *const unit[] = {
	        "B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
	uint64_t whole = u->bytes, rest = 0;
	size_t k = 0;
	bool at_least = !u->finished || u->partial;

	if (u->finished && u->partial && WF_OK != u->st && !u->bytes &&
	        !u->files && !u->folders) {
		snprintf(out, cap, "-"); /* nothing could be read at all */
		return;
	}
	while (whole >= 1024 && k + 1 < sizeof unit / sizeof *unit) {
		rest = whole % 1024;
		whole /= 1024;
		k++;
	}
	if (0 == k) {
		snprintf(out, cap, "%llu B%s", (unsigned long long)whole,
		        at_least ? "+" : "");
	} else {
		snprintf(out, cap, "%llu.%llu %s%s", (unsigned long long)whole,
		        (unsigned long long)(rest * 10 / 1024), unit[k],
		        at_least ? "+" : "");
	}
}
