#ifndef WF_SEARCHWORKER_H
#define WF_SEARCHWORKER_H

#include <windows.h>

#include "../async/wf_worker.h"
#include "wf_search.h"

/*
 * Walks below a folder on a thread of its own and hands the hits over as they are
 * found (DR-DRAFT-031). One request at a time: a newer one, or a cancel, ends the
 * running one at its next folder or entry and nothing of an ended request is
 * delivered. Hits come in batches of at most WF_SEARCH_BATCH. During the search,
 * every 64 visited entries the worker also checks whether a pending batch is
 * 100 ms old;
 * a blocking folder read can delay delivery. For duplicate searches, hashing
 * progress callbacks also check the 100 ms interval. With 64 batches waiting
 * the walk waits for the screen, bounding queued batch memory however slow
 * the screen is. The last batch of a request has `done` set.
 */
typedef struct WfSearchWorker WfSearchWorker;

/* `list` reads one folder; the program passes its enumerator, a test its own. */
WfStatus wf_searchworker_start(WfSearchWorker **out, WfSearchListFn list);
WfWorkerStopReport wf_searchworker_stop(WfSearchWorker *w);
/* True when nothing is in flight and nothing waits to be polled: the worker
 * can be stopped without losing an answer (DR-DRAFT-036). NULL: true. */
bool wf_searchworker_idle(WfSearchWorker *w);

/* How a file is hashed for the duplicate search (DR-DRAFT-032); without it the port's
 * start_dupes refuses. Set it before the first request. */
void wf_searchworker_set_hash(WfSearchWorker *w, WfSearchHashFn hash);

/* Manual-reset event: signalled while a batch waits. */
HANDLE wf_searchworker_event(const WfSearchWorker *w);
/* Moves the oldest batch out (the caller frees it with wf_search_batch_free). */
bool wf_searchworker_poll(WfSearchWorker *w, WfSearchBatch *out);

WfSearchPort wf_searchworker_port(WfSearchWorker *w);

#endif
