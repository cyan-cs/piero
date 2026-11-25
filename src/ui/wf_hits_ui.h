#ifndef WF_HITS_UI_H
#define WF_HITS_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../search/wf_search.h"
#include "../xfer/wf_xfer.h"

/*
 * The list of results of a search below a folder (F4, DR-DRAFT-031), and what is
 * kept for a question that is being asked about some of them (wf_app_hits.c).
 */
#define WF_HITS_MAX_FOLDERS 64 /* folders one Delete takes at a time */

typedef struct
{
	WfSearchPort port;

	bool open;    /* the window is up */
	bool running; /* a request is out to the worker */
	uint32_t gen;
	bool done;     /* the last batch came */
	bool at_least; /* ... and the walk did not finish */
	bool stopped; /* Esc ended the walk: what there is, is what was found */
	WfStatus st;
	WfWalkStats walk;

	WfHit *hit; /* owned: the results so far, in the order found */
	size_t n, cap;
	bool *sel; /* owned: n flags */
	size_t nsel;
	size_t cursor, top;

	wchar_t *root; /* owned: where the search started */
	WfFileId root_id;
	char query[96]; /* what was asked, as shown */
	bool hidden;

	/* The duplicate search (F): the same list, grouped. */
	bool dupes;
	uint64_t read_done, read_total; /* files read of files to read */
	size_t groups;                  /* with `done` */
	size_t skipped; /* with `done`: files that could not be compared */

	/* The Recycle question being asked: one slot for each folder the chosen
	 * results are in. Freed with the answer, or by the next question. */
	WfTransferSlot *pending;
	size_t n_pending, items_pending;
} WfHits;

#endif
