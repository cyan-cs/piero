#include "wf_sel.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "../base/wf_mem.h"

static uint32_t last_gen;

void wf_sel_init(WfSelStore *s)
{
	if (!s) {
		return;
	}
	memset(s, 0, sizeof *s);
	s->gen = ++last_gen;
}

static void set_free(WfNameSet *set)
{
	size_t i;

	for (i = 0; i < set->n; i++) {
		free(set->names[i]);
	}
	free(set->names);
	memset(set, 0, sizeof *set);
}

void wf_sel_free(WfSelStore *s)
{
	size_t i;

	if (!s) {
		return;
	}
	for (i = 0; i < s->n; i++) {
		free(s->f[i].path);
		set_free(&s->f[i].set);
	}
	free(s->f);
	wf_sel_init(s);
}

/* Binary search; *pos is the match, or the insertion point. */
static bool set_find(const WfNameSet *set, const wchar_t *name, size_t *pos)
{
	size_t lo = 0, hi = set->n;

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		int c = wcscmp(set->names[mid], name);

		if (0 == c) {
			*pos = mid;
			return 1;
		}
		if (c < 0) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	*pos = lo;
	return 0;
}

bool wf_nameset_has(const WfNameSet *set, const wchar_t *name)
{
	size_t pos;

	return set && name && set_find(set, name, &pos);
}

static size_t folder_index(const WfSelStore *s, const wchar_t *dir)
{
	size_t i;

	if (!s || !dir) {
		return SIZE_MAX;
	}
	for (i = 0; i < s->n; i++) {
		if (0 == wcscmp(s->f[i].path, dir)) {
			return i;
		}
	}
	return SIZE_MAX;
}

const WfNameSet *wf_sel_folder(const WfSelStore *s, const wchar_t *dir)
{
	size_t i = folder_index(s, dir);

	return SIZE_MAX == i ? NULL : &s->f[i].set;
}

size_t wf_sel_count(const WfSelStore *s, const wchar_t *dir)
{
	const WfNameSet *set = wf_sel_folder(s, dir);

	return set ? set->n : 0;
}

static void folder_remove(WfSelStore *s, size_t i)
{
	free(s->f[i].path);
	set_free(&s->f[i].set);
	memmove(&s->f[i], &s->f[i + 1], (s->n - i - 1) * sizeof *s->f);
	s->n--;
}

static WfStatus folder_add(WfSelStore *s, const wchar_t *dir, size_t *idx)
{
	WfSelFolder *nf;
	wchar_t *path;

	if (s->n == s->cap) {
		size_t cap = s->cap ? s->cap * 2 : 4;

		nf = wf_realloc_array(s->f, cap, sizeof *nf);
		if (!nf) {
			return WF_E_NOMEM;
		}
		s->f = nf;
		s->cap = cap;
	}
	path = wf_malloc_array((wcslen(dir) + 1), sizeof *path);
	if (!path) {
		return WF_E_NOMEM;
	}
	memcpy(path, dir, (wcslen(dir) + 1) * sizeof *path);
	s->f[s->n].path = path;
	memset(&s->f[s->n].set, 0, sizeof s->f[s->n].set);
	*idx = s->n++;
	return WF_OK;
}

WfStatus wf_sel_toggle(WfSelStore *s, const wchar_t *dir, const wchar_t *name)
{
	size_t fi, pos;
	WfNameSet *set;
	wchar_t *copy;
	WfStatus st;

	if (!s || !dir || !name) {
		return WF_E_INVAL;
	}
	s->gen = ++last_gen;
	fi = folder_index(s, dir);
	if (SIZE_MAX == fi && WF_OK != (st = folder_add(s, dir, &fi))) {
		return st;
	}
	set = &s->f[fi].set;

	if (set_find(set, name, &pos)) {
		free(set->names[pos]);
		memmove(&set->names[pos], &set->names[pos + 1],
		        (set->n - pos - 1) * sizeof *set->names);
		if (--set->n == 0) {
			folder_remove(s, fi);
		}
		return WF_OK;
	}

	if (set->n == set->cap) {
		size_t cap = set->cap ? set->cap * 2 : 8;
		wchar_t **nn = realloc(set->names, cap * sizeof *nn);

		if (!nn) {
			if (0 == set->n) {
				folder_remove(s,
				        fi); /* do not leave an empty folder */
			}
			return WF_E_NOMEM;
		}
		set->names = nn;
		set->cap = cap;
	}
	copy = malloc((wcslen(name) + 1) * sizeof *copy);
	if (!copy) {
		if (0 == set->n) {
			folder_remove(s, fi);
		}
		return WF_E_NOMEM;
	}
	memcpy(copy, name, (wcslen(name) + 1) * sizeof *copy);
	memmove(&set->names[pos + 1], &set->names[pos],
	        (set->n - pos) * sizeof *set->names);
	set->names[pos] = copy;
	set->n++;
	return WF_OK;
}

void wf_sel_clear(WfSelStore *s, const wchar_t *dir)
{
	size_t i = folder_index(s, dir);

	s->gen = ++last_gen;
	if (SIZE_MAX != i) {
		folder_remove(s, i);
	}
}

/* Keeps the selected names the fresh listing still has. The set is already
 * sorted, so the listing is walked once and each entry is looked up in the set
 * (binary search): nothing of the listing is copied or sorted, and the walk
 * stops when every selected name has been seen. */
void wf_sel_prune(WfSelStore *s, const wchar_t *dir, const WfDir *d)
{
	size_t fi, i, seen = 0, kept = 0;
	bool *present;
	WfNameSet *set;

	if (!d) {
		return;
	}
	s->gen = ++last_gen;
	fi = folder_index(s, dir);
	if (SIZE_MAX == fi) {
		return;
	}
	set = &s->f[fi].set;
	present = calloc(set->n ? set->n : 1u, sizeof *present);
	if (!present) {
		/* cannot prove which names survive: a stale name could later be
		 * re-created as an unrelated file and appear selected */
		folder_remove(s, fi);
		return;
	}
	for (i = 0; i < d->n && seen < set->n; i++) {
		size_t pos;

		if (set_find(set, d->e[i].name, &pos) && !present[pos]) {
			present[pos] = true;
			seen++;
		}
	}
	for (i = 0; i < set->n; i++) {
		if (present[i]) {
			set->names[kept++] = set->names[i];
		} else {
			free(set->names[i]);
		}
	}
	set->n = kept;
	free(present);
	if (0 == set->n) {
		folder_remove(s, fi);
	}
}
