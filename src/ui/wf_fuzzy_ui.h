#ifndef WF_FUZZY_UI_H
#define WF_FUZZY_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#include "../search/wf_fuzzy.h"
#include "wf_view.h"

/* Names at or below this many are searched on the screen's thread, in the same
 * function the worker runs: a thread hop costs more than the search (measured,
 * DR-DRAFT-023). */
#define WF_FZ_SYNC_MAX 4096u

/* The fuzzy search of the names in the folder on screen (wf_app_fuzzy.c). */
typedef struct
{
	bool active;
	wchar_t query[WF_FILTER_MAX];
	uint32_t gen; /* the newest request; an answer must carry it */
	bool pending; /* a request is out to the worker */
	bool dirty;   /* the listing changed under the search: ask again */
	bool more;    /* the request out asks for more of the same query */
	bool advance; /* ... and the cursor moves down when they arrive */
	size_t k;     /* hits asked for by the newest request */
	WfFzHit *hit; /* owned, best first; rows of the view's `vis` */
	size_t nhit, matched;
	size_t sel, top; /* chosen hit and first hit shown */
	wchar_t *
	        orig_name; /* owned: the entry that had the cursor at the start */
	size_t orig_top;
	wchar_t *keep_name; /* owned: the chosen entry, kept across a refresh */
	uint32_t cwd_version; /* the folder the search belongs to */
	WfFzScratch *scratch;
	unsigned dropped; /* answers of other generations that were refused */
} WfFuzzy;

#endif
