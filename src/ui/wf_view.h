#ifndef WF_VIEW_H
#define WF_VIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../fs/wf_dir.h"

#define WF_FILTER_MAX 64 /* UTF-16 units including the NUL */

/*
 * A pane's view of one directory snapshot: which entries are visible and
 * where the cursor is. This view owns the snapshot and its visible indexes;
 * wf_view_resort may reorder the snapshot in place.
 */
typedef struct
{
	WfDir *dir;  /* owned; NULL = nothing loaded */
	size_t *vis; /* owned; indexes into dir->e of shown entries */
	size_t nvis;
	size_t cursor; /* index into vis */
	size_t top;    /* first visible row */
	bool show_hidden;
	wchar_t filter[WF_FILTER_MAX]; /* case-insensitive substring; "" = off
	                                */
	/* Called first by everything that changes or frees `dir` or `vis`, so a
	 * reader on another thread (the fuzzy search) can stop reading them. Set
	 * by the app on the current pane only; wf_view_init clears it. */
	void (*guard)(void *ctx);
	void *guard_ctx;
	/* Process-wide counter advanced on init and before listing/index changes,
	 * including attempted changes that fail. Used to invalidate cached counts;
	 * wraps as uint32_t. Set only on the UI thread. */
	uint32_t gen;
} WfView;

void wf_view_init(WfView *v);
void wf_view_free(WfView *v);

/* Takes ownership of `dir` even on failure. The cursor lands on the entry
 * named `select` if present, else on the first entry. `dir` may be NULL
 * (clears the view). Hidden and filter settings are kept. */
WfStatus wf_view_set_dir(WfView *v, WfDir *dir, const wchar_t *select);

/* The same, for a refresh of the folder already shown: the index for the new
 * listing is made first, and if there is no memory for it the list on screen
 * stays exactly as it was (the new listing is dropped). A failure here is
 * "could not refresh", never an empty folder. */
WfStatus wf_view_replace_keep(WfView *v, WfDir *dir, const wchar_t *select);

/* Orders the snapshot again (in place) and keeps the cursor on the entry it
 * was on. `dirs_first`: see wf_dir_sort_with. */
WfStatus wf_view_resort(
        WfView *v, WfSortKey key, bool descending, bool dirs_first);

/* Both keep the cursor on the same entry when it stays visible. */
WfStatus wf_view_set_hidden(WfView *v, bool show);
/* Over-long filters are truncated. */
WfStatus wf_view_set_filter(WfView *v, const wchar_t *filter);

/* Moves the cursor by delta, clamped; keeps it inside a `height`-row window. */
void wf_view_move(WfView *v, long delta, int height);
/* First/last visible item, independent of column layout; safe on empty views.
 */
void wf_view_move_edge(WfView *v, bool last, int height);

/* NULL when nothing is visible. */
const WfEntry *wf_view_current(const WfView *v);

/* First row to draw so the cursor is inside a `height`-row window. */
size_t wf_view_top(const WfView *v, int height);

#endif
