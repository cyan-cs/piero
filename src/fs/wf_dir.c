#include "wf_dir.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../archive/wf_vpath.h"
#include "../base/wf_buf.h"
#include "../text/wf_text.h"
#include "../base/wf_mem.h"

WfDir *wf_dir_new(void)
{
	WfDir *d = calloc(1, sizeof *d);
	if (d) {
		d->owned_bytes = sizeof *d;
	}
	return d;
}

void wf_dir_free(WfDir *d)
{
	size_t i;

	if (!d) {
		return;
	}
	for (i = 0; i < d->n; i++) {
		free(d->e[i].name);
		free(d->e[i].display);
	}
	free(d->e);
	free(d);
}

static WfStatus grow(WfDir *d)
{
	size_t cap = d->cap ? d->cap * 2 : 64;
	WfEntry *p;

	if (cap > SIZE_MAX / sizeof *p) {
		return WF_E_RANGE;
	}
	size_t delta = (cap - d->cap) * sizeof *p;
	if (delta > SIZE_MAX - d->owned_bytes ||
	        (d->byte_limit &&
	                (d->owned_bytes > d->byte_limit ||
	                        delta > d->byte_limit - d->owned_bytes))) {
		return WF_E_RANGE;
	}
	p = realloc(d->e, cap * sizeof *p);
	if (!p) {
		return WF_E_NOMEM;
	}
	d->e = p;
	d->cap = cap;
	d->owned_bytes += delta;
	return WF_OK;
}

void wf_dir_set_meta(
        WfDir *d, size_t index, WfFileTime ctime, uint32_t attributes)
{
	if (!d || index >= d->n) {
		return;
	}
	d->e[index].ctime = ctime;
	d->e[index].attributes = attributes;
	d->e[index].meta_known = true;
}

WfStatus wf_dir_add(WfDir *d,
        const wchar_t *name,
        size_t name_len,
        uint32_t flags,
        uint64_t size,
        WfFileTime mtime)
{
	return wf_dir_add_ex(d, name, name_len, flags, size, mtime, 0);
}

WfStatus wf_dir_add_ex(WfDir *d,
        const wchar_t *name,
        size_t name_len,
        uint32_t flags,
        uint64_t size,
        WfFileTime mtime,
        uint32_t reparse_tag)
{
	WfEntry *ent;
	WfBuf disp;
	WfStatus st;

	if (!d || (name_len && !name)) {
		return WF_E_INVAL;
	}
	if (name_len > SIZE_MAX / sizeof *ent->name - 1) {
		return WF_E_RANGE;
	}
	if (d->n >= (d->limit ? d->limit : WF_DIR_MAX_ENTRIES)) {
		d->truncated = 1;
		return WF_OK;
	}
	if (d->n == d->cap && WF_OK != (st = grow(d))) {
		return st;
	}

	/* Sanitized control labels and buffer growth need more than raw UTF-16.
	 * Reserve a conservative per-name bound before making either allocation. */
	if (name_len > (SIZE_MAX - 66) / 18 ||
	        18 * name_len + 66 > SIZE_MAX - d->owned_bytes ||
	        (d->byte_limit &&
	                (d->owned_bytes > d->byte_limit ||
	                        18 * name_len + 66 >
	                                d->byte_limit - d->owned_bytes))) {
		return WF_E_RANGE;
	}
	ent = &d->e[d->n];
	ent->name = malloc((name_len + 1) * sizeof *ent->name);
	if (!ent->name) {
		return WF_E_NOMEM;
	}
	if (name_len) {
		memcpy(ent->name, name, name_len * sizeof *ent->name);
	}
	ent->name[name_len] = L'\0';

	wf_buf_init(&disp);
	st = d->no_display ? WF_OK : wf_sanitize_utf16(name, name_len, &disp);
	if (WF_OK == st && d->no_display) {
		ent->display = NULL;
	} else if (WF_OK == st) {
		/* take the sanitized buffer instead of copying it; an empty
		 * name leaves data NULL, so give it a real empty string */
		if (disp.data) {
			d->owned_bytes += disp.cap;
			ent->display = disp.data;
			wf_buf_init(&disp);
		} else if (NULL != (ent->display = malloc(1))) {
			ent->display[0] = '\0';
			d->owned_bytes++;
		} else {
			st = WF_E_NOMEM;
		}
	}
	wf_buf_free(&disp);
	if (WF_OK != st) {
		free(ent->name);
		ent->name = NULL;
		return st;
	}

	d->owned_bytes += (name_len + 1) * sizeof *ent->name;
	ent->flags = flags;
	/* a plain file whose name says "archive" can be entered (never a link, and
	 * never a cloud placeholder: entering would read it) */
	if (!(flags & (WF_ENT_DIR | WF_ENT_LINK | WF_ENT_CLOUD)) &&
	        wf_archive_name_recognized(ent->name)) {
		ent->flags |= WF_ENT_ARCHIVE;
	}
	ent->size = size;
	ent->mtime = mtime;
	ent->reparse_tag = reparse_tag;
	ent->ctime = 0;
	ent->attributes = 0;
	ent->meta_known = false;
	memset(&ent->identity, 0, sizeof ent->identity);
	d->n++;
	return WF_OK;
}

static wchar_t fold(wchar_t c)
{
	return c >= L'A' && c <= L'Z' ? (wchar_t)(c + 32) : c;
}

static bool is_digit(wchar_t c)
{
	return c >= L'0' && c <= L'9';
}

int wf_natcmp(const wchar_t *a, const wchar_t *b)
{
	while (*a && *b) {
		if (is_digit(*a) && is_digit(*b)) {
			const wchar_t *ea, *eb;
			size_t la, lb;

			while (L'0' == *a) {
				a++;
			}
			while (L'0' == *b) {
				b++;
			}
			for (ea = a; is_digit(*ea); ea++) {
			}
			for (eb = b; is_digit(*eb); eb++) {
			}
			la = (size_t)(ea - a);
			lb = (size_t)(eb - b);
			if (la != lb) {
				return la < lb ? -1 : 1;
			}
			/* Digit runs start at arbitrary 2-byte offsets; the
			 * UCRT's inline wmemcmp does wide loads that assume
			 * more alignment than that. */
			for (size_t k = 0; k < la; k++) {
				if (a[k] != b[k]) {
					return a[k] < b[k] ? -1 : 1;
				}
			}
			a = ea;
			b = eb;
			continue;
		}
		if (fold(*a) != fold(*b)) {
			return (uint16_t)fold(*a) < (uint16_t)fold(*b) ? -1 : 1;
		}
		a++;
		b++;
	}
	if (!*a && !*b) {
		return 0;
	}
	return *a ? 1 : -1;
}

static int ordinal(const wchar_t *a, const wchar_t *b)
{
	for (; *a && *a == *b; a++, b++) {
	}
	if (*a == *b) {
		return 0;
	}
	return (uint16_t)*a < (uint16_t)*b ? -1 : 1;
}

static const wchar_t *ext_of(const wchar_t *name)
{
	const wchar_t *dot = wcsrchr(name, L'.');

	return dot && dot != name ? dot + 1 : L"";
}

static int cmp_u64(uint64_t a, uint64_t b)
{
	return a < b ? -1 : a > b;
}

typedef struct
{
	WfSortKey key;
	bool desc;
	bool dirs_first;
} SortSpec;

static int entry_cmp(const WfEntry *a, const WfEntry *b, const SortSpec *s)
{
	int ad = 0 != (a->flags & WF_ENT_DIR),
	    bd = 0 != (b->flags & WF_ENT_DIR);
	int r = 0;

	if (s->dirs_first && ad != bd) {
		return ad ? -1
		          : 1; /* directories first, regardless of direction */
	}

	switch (s->key) {
	case WF_SORT_SIZE: r = cmp_u64(a->size, b->size); break;
	case WF_SORT_DATE: r = cmp_u64(a->mtime, b->mtime); break;
	case WF_SORT_EXT:
		r = wf_natcmp(ext_of(a->name), ext_of(b->name));
		break;
	case WF_SORT_NAME: break;
	}
	if (0 == r) {
		r = wf_natcmp(a->name, b->name);
	}
	if (0 == r) {
		r = ordinal(a->name, b->name);
	}
	return s->desc ? -r : r;
}

/* Stable top-down merge sort. qsort has no context argument and its
 * comparator-state workarounds are not thread-safe. */
static void msort(WfEntry *a, WfEntry *tmp, size_t n, const SortSpec *s)
{
	size_t mid = n / 2, i = 0, j = mid, k = 0;

	if (n < 2) {
		return;
	}
	msort(a, tmp, mid, s);
	msort(a + mid, tmp, n - mid, s);
	while (i < mid && j < n) {
		tmp[k++] = entry_cmp(&a[j], &a[i], s) < 0 ? a[j++] : a[i++];
	}
	while (i < mid) {
		tmp[k++] = a[i++];
	}
	while (j < n) {
		tmp[k++] = a[j++];
	}
	memcpy(a, tmp, n * sizeof *a);
}

WfStatus wf_dir_sort(WfDir *d, WfSortKey key, bool descending)
{
	return wf_dir_sort_with(d, key, descending, true);
}

WfStatus wf_dir_sort_with(
        WfDir *d, WfSortKey key, bool descending, bool dirs_first)
{
	SortSpec s = {key, descending, dirs_first};
	WfEntry *tmp;

	if (d->n < 2) {
		return WF_OK;
	}
	/* Revalidated/cached snapshots often already have the requested order.
	 * Use the full comparator, including directories and ordinal ties. */
	size_t ordered = 1;
	while (ordered < d->n &&
	        entry_cmp(&d->e[ordered - 1], &d->e[ordered], &s) <= 0) {
		ordered++;
	}
	if (ordered == d->n) {
		return WF_OK;
	}
	tmp = wf_malloc_array(d->n, sizeof *tmp);
	if (!tmp) {
		return WF_E_NOMEM;
	}
	msort(d->e, tmp, d->n, &s);
	free(tmp);
	return WF_OK;
}
