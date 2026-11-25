#include "wf_view.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include "../base/wf_mem.h"

static uint32_t last_gen;

void wf_view_init(WfView *v)
{
	memset(v, 0, sizeof *v);
	v->gen = ++last_gen;
}

/* Tells whoever reads the listing from elsewhere that it is about to change, and
 * whoever has counted it that the counts are no longer of this listing. */
static void guarded(WfView *v)
{
	v->gen = ++last_gen;
	if (v->guard) {
		v->guard(v->guard_ctx);
	}
}

void wf_view_free(WfView *v)
{
	guarded(v);
	wf_dir_free(v->dir);
	free(v->vis);
	wf_view_init(v);
}

/* Lower case for any letter the system knows (accented Latin, Greek,
 * Cyrillic, full-width...), not only ASCII. One UTF-16 unit at a time: a
 * character outside the BMP is compared as it is. */
static wchar_t fold(wchar_t c)
{
	if (c < 0x80) {
		return c >= L'A' && c <= L'Z' ? (wchar_t)(c + 32) : c;
	}
	{
		/* kernel32 only (no user32): the invariant locale's simple lower
		 * case, one UTF-16 unit */
		WCHAR in = (WCHAR)c, out = in;

		if (1 ==
		        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
		                &in, 1, &out, 1, NULL, NULL, 0)) {
			return (wchar_t)out;
		}
		return c;
	}
}

/* Scripts that have no case: the kana and CJK blocks and Hangul. Text made of
 * these (and ASCII) is folded without asking the system. */
static bool caseless(wchar_t c)
{
	return (c >= 0x3000 && c <= 0x30FF) || (c >= 0x3400 && c <= 0x4DBF) ||
	        (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7A3);
}

/* `n` units of `in` folded into `out` (the same count: the simple lower case
 * does not change the length). The system is asked once for the whole text,
 * and not at all when the text has no letter that has a case, so that a long
 * list of Japanese names costs no call per name. A call that does not give the
 * same count falls back to one unit at a time. */
static void fold_text(wchar_t *out, const wchar_t *in, size_t n)
{
	bool ask = false;
	size_t i;

	for (i = 0; i < n; i++) {
		wchar_t c = in[i];

		if (c < 0x80) {
			out[i] = c >= L'A' && c <= L'Z' ? (wchar_t)(c + 32) : c;
		} else {
			out[i] = c;
			ask |= !caseless(c);
		}
	}
	if (ask && n <= (size_t)INT_MAX) {
		if ((int)n ==
		        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
		                in, (int)n, out, (int)n, NULL, NULL, 0)) {
			return;
		}
		for (i = 0; i < n; i++) {
			out[i] = fold(in[i]);
		}
	}
}

/* The filter, folded once for the whole pass over the entries. */
typedef struct
{
	wchar_t f[WF_FILTER_MAX];
	size_t n;
} Needle;

static void needle_make(Needle *nd, const wchar_t *filter)
{
	size_t i;

	for (i = 0; filter[i] && i + 1 < WF_FILTER_MAX; i++) {
	}
	fold_text(nd->f, filter, i);
	nd->n = i;
}

/* Case-insensitive substring test against the folded filter. Letters match
 * their other case in every script that has one; text in different
 * normalization forms (a composed letter and a letter plus accent) is not
 * unified. The name is folded once, not at every place it is compared; a name
 * too long for the buffer is folded where it is compared, as before. */
static bool matches(const wchar_t *name, const Needle *nd)
{
	wchar_t folded[256];
	size_t hl = wcslen(name), i, j;
	bool whole = hl < sizeof folded / sizeof folded[0];

	if (0 == nd->n) { /* no filter: every name passes, none is read */
		return true;
	}
	if (hl < nd->n) {
		return false;
	}
	if (whole) {
		fold_text(folded, name, hl);
	}
	for (i = 0; i + nd->n <= hl; i++) {
		for (j = 0; j < nd->n &&
		        (whole ? folded[i + j] : fold(name[i + j])) == nd->f[j];
		        j++) {
		}
		if (j == nd->n) {
			return true;
		}
	}
	return false;
}

/* Fills `vis` with the visible entries, for the hidden and filter settings,
 * and makes it the index. The entries looked at are all of the dir, or, with
 * `from`, only the `nfrom` whose indexes it holds (`vis` has room for as many
 * as are looked at). Cannot fail. */
static void fill(WfView *v, size_t *vis, const size_t *from, size_t nfrom)
{
	size_t k, n = 0, count = from ? nfrom : v->dir->n;
	Needle nd;

	needle_make(&nd, v->filter);
	for (k = 0; k < count; k++) {
		size_t i = from ? from[k] : k;
		const WfEntry *e = &v->dir->e[i];

		if ((v->show_hidden || !(e->flags & WF_ENT_HIDDEN)) &&
		        matches(e->name, &nd)) {
			vis[n++] = i;
		}
	}
	free(v->vis);
	v->vis = vis;
	v->nvis = n;
}

/* Rebuilds vis for the current dir, hidden and filter settings. The new
 * index is made first: if memory runs out the index that was there is kept,
 * which is right only while the entries it points at have not changed. With
 * `narrow` the filter has only been made longer, so what matches now is among
 * what matched before: only those entries are looked at. */
static WfStatus rebuild(WfView *v, bool narrow)
{
	size_t *vis;

	if (!v->dir || 0 == v->dir->n) {
		free(v->vis);
		v->vis = NULL;
		v->nvis = 0;
		return WF_OK;
	}
	narrow = narrow && v->vis;
	vis = wf_malloc_array(narrow ? v->nvis + 1 : v->dir->n, sizeof *vis);
	if (!vis) {
		return WF_E_NOMEM;
	}
	fill(v, vis, narrow ? v->vis : NULL, v->nvis);
	return WF_OK;
}

/* Rebuilds vis, then puts the cursor back on the entry it was on (by dir
 * index) if that entry is still visible, else on the first row. */
static WfStatus refresh_keep_cursor(WfView *v, bool narrow)
{
	size_t keep = v->nvis ? v->vis[v->cursor] : SIZE_MAX, i;
	WfStatus st = rebuild(v, narrow);

	if (WF_OK != st) {
		return st; /* the old list, its cursor and its top are kept */
	}
	v->cursor = 0;
	v->top = 0;
	if (SIZE_MAX == keep) {
		return st;
	}
	for (i = 0; i < v->nvis; i++) {
		if (v->vis[i] == keep) {
			v->cursor = i;
			break;
		}
	}
	return WF_OK;
}

WfStatus wf_view_set_dir(WfView *v, WfDir *dir, const wchar_t *select)
{
	WfStatus st;
	size_t i;

	guarded(v);
	wf_dir_free(v->dir);
	v->dir = dir;
	v->cursor = 0;
	v->top = 0;
	st = rebuild(v, false);
	if (WF_OK != st) {
		/* the old index points into the dir that was just freed: the
		 * view shows nothing rather than entries that are not there */
		free(v->vis);
		v->vis = NULL;
		v->nvis = 0;
		return st;
	}
	if (select) {
		for (i = 0; i < v->nvis; i++) {
			if (0 == wcscmp(v->dir->e[v->vis[i]].name, select)) {
				v->cursor = i;
				break;
			}
		}
	}
	return WF_OK;
}

WfStatus wf_view_replace_keep(WfView *v, WfDir *dir, const wchar_t *select)
{
	size_t *vis = NULL;

	guarded(v);
	if (dir && dir->n) {
		vis = wf_malloc_array(dir->n, sizeof *vis);
		if (!vis) {
			wf_dir_free(
			        dir); /* the list on screen is left as it is */
			return WF_E_NOMEM;
		}
	}
	wf_dir_free(v->dir);
	v->dir = dir;
	v->cursor = 0;
	v->top = 0;
	if (vis) {
		fill(v, vis, NULL, 0);
	} else {
		free(v->vis);
		v->vis = NULL;
		v->nvis = 0;
	}
	for (size_t i = 0; select && i < v->nvis; i++) {
		if (0 == wcscmp(v->dir->e[v->vis[i]].name, select)) {
			v->cursor = i;
			break;
		}
	}
	return WF_OK;
}

WfStatus wf_view_resort(
        WfView *v, WfSortKey key, bool descending, bool dirs_first)
{
	wchar_t *name = NULL;
	size_t *vis = NULL;
	WfStatus st;
	size_t i;

	if (!v->dir) {
		return WF_OK;
	}
	guarded(v);
	/* the index is allocated before anything is reordered, so the sort and
	 * its index succeed or fail together */
	if (v->dir->n) {
		vis = wf_malloc_array(v->dir->n, sizeof *vis);
		if (!vis) {
			return WF_E_NOMEM;
		}
	}
	if (v->nvis) {
		const wchar_t *cur = v->dir->e[v->vis[v->cursor]].name;

		name = wf_malloc_array((wcslen(cur) + 1), sizeof *name);
		if (!name) {
			free(vis);
			return WF_E_NOMEM;
		}
		memcpy(name, cur, (wcslen(cur) + 1) * sizeof *name);
	}
	st = wf_dir_sort_with(v->dir, key, descending, dirs_first);
	if (vis) {
		fill(v, vis, NULL,
		        0); /* whatever order the sort left, the index matches */
	}
	v->cursor = 0;
	v->top = 0;
	for (i = 0; WF_OK == st && name && i < v->nvis; i++) {
		if (!wcscmp(v->dir->e[v->vis[i]].name, name)) {
			v->cursor = i;
			break;
		}
	}
	free(name);
	return st;
}

WfStatus wf_view_set_hidden(WfView *v, bool show)
{
	bool before = v->show_hidden;
	WfStatus st;

	guarded(v);
	v->show_hidden = show;
	st = refresh_keep_cursor(v, false);
	if (WF_OK != st) {
		v->show_hidden =
		        before; /* the list shown and the setting agree */
	}
	return st;
}

WfStatus wf_view_set_filter(WfView *v, const wchar_t *filter)
{
	size_t n;

	if (!v || !filter) {
		return WF_E_INVAL;
	}
	guarded(v);
	n = wcslen(filter);
	if (n >= WF_FILTER_MAX) {
		n = WF_FILTER_MAX - 1;
	}
	{
		wchar_t before[WF_FILTER_MAX];
		WfStatus st;
		/* a filter that begins with the old one matches a subset of it */
		bool longer;

		memcpy(before, v->filter, sizeof before);
		longer = n >= wcslen(before) &&
		        0 == wcsncmp(filter, before, wcslen(before));
		memcpy(v->filter, filter, n * sizeof *filter);
		v->filter[n] = L'\0';
		st = refresh_keep_cursor(v, longer);
		if (WF_OK != st) {
			memcpy(v->filter, before, sizeof before);
		}
		return st;
	}
}

size_t wf_view_top(const WfView *v, int height)
{
	size_t top = v->top, h = height > 0 ? (size_t)height : 1;

	if (v->cursor < top) {
		top = v->cursor;
	} else if (v->cursor >= top + h) {
		top = v->cursor - h + 1;
	}
	return top;
}

void wf_view_move(WfView *v, long delta, int height)
{
	/* magnitude without negating LONG_MIN */
	size_t back = delta < 0 ? (size_t)(-(delta + 1)) + 1 : 0;

	if (0 == v->nvis) {
		return;
	}
	if (delta < 0 && back > v->cursor) {
		v->cursor = 0;
	} else if (delta < 0) {
		v->cursor -= back;
	} else if ((size_t)delta >= v->nvis - v->cursor) {
		v->cursor = v->nvis - 1;
	} else {
		v->cursor += (size_t)delta;
	}
	v->top = wf_view_top(v, height);
}

const WfEntry *wf_view_current(const WfView *v)
{
	return v->nvis ? &v->dir->e[v->vis[v->cursor]] : NULL;
}

void wf_view_move_edge(WfView *v, bool last, int height)
{
	size_t h = height > 0 ? (size_t)height : 1;

	v->cursor = last && v->nvis ? v->nvis - 1 : 0;
	v->top = last && v->nvis > h ? v->nvis - h : 0;
}
