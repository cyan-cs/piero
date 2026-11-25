#ifndef WF_DIRSIZE_H
#define WF_DIRSIZE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../fs/wf_size.h"

/*
 * What `D` has counted for the folders of one listing: the folder's path, the
 * names asked about (sorted, so a row finds its total by binary search while
 * the screen is drawn) and the newest totals the worker handed over.
 *
 * The totals are as of when they were counted; they belong to `dir` alone and
 * are never looked up for another folder.
 */
typedef struct
{
	uint32_t gen; /* the newest request; 0: none */
	bool running;
	size_t asked, done; /* folders asked about / counted to the end */
	wchar_t *dir;       /* owned */
	/* `name` is its own array because the worker takes it as one; `u` is
	 * index-aligned with it, and an entry whose `gen` is 0 has had no update. */
	wchar_t **name;  /* owned, sorted by wcscmp, n entries */
	WfSizeUpdate *u; /* owned */
	size_t n;
} WfDirSizes;

void wf_dirsizes_free(WfDirSizes *s);
void wf_dirsizes_clear(WfDirSizes *s); /* free, and forget the request */

/* Copies and sorts `names` in `dir`, then replaces the previous request state.
 * Failure leaves the previous state intact. Use s->name for the worker request
 * so update indexes match the stored order. */
bool wf_dirsizes_begin(WfDirSizes *s,
        uint32_t gen,
        const wchar_t *dir,
        const wchar_t *const *names,
        size_t n);

/* Takes one update of the newest request; false when it is not one. */
bool wf_dirsizes_take(WfDirSizes *s, const WfSizeUpdate *u);

/* The totals of `name` in `dir`, or NULL when nothing is known. */
const WfSizeUpdate *wf_dirsizes_find(
        const WfDirSizes *s, const wchar_t *dir, const wchar_t *name);

/* "1.2 GiB", "1.2 GiB+" (at least), "-" when the folder could not be
 * counted. ASCII only: the text sits at the end of a row. */
void wf_dirsizes_short(const WfSizeUpdate *u, char *out, size_t cap);

#endif
