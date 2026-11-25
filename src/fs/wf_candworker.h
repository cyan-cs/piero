#ifndef WF_CANDWORKER_H
#define WF_CANDWORKER_H

#include <windows.h>

#include "../async/wf_worker.h"
#include "wf_cand.h"

/*
 * Reads Smart Go candidates on its own thread, up to WF_CAND_BATCH matches
 * per batch. One search exists at a time: starting another (or cancelling) ends
 * the old one at its next entry and closes its search handle; a result of an
 * ended search is never delivered. The search handle is created, used and
 * closed by the worker thread alone, so no other thread can close it under a
 * running call. Between batches the handle stays open; enumeration resumes
 * where the last batch stopped.
 *
 * Prefix matching uses CompareStringOrdinal with case ignored on each entry,
 * rather than a wildcard that could also match short 8.3 names.
 */
typedef struct WfCandWorker WfCandWorker;

WfStatus wf_candworker_start(WfCandWorker **out);
WfWorkerStopReport wf_candworker_stop(WfCandWorker *w);

/* Manual-reset event: signalled while batches are waiting. */
HANDLE wf_candworker_event(const WfCandWorker *w);
bool wf_candworker_poll(WfCandWorker *w, WfCandBatch *out);

WfCandPort wf_candworker_port(WfCandWorker *w);

/* For tests: how many search handles are open right now (0 or 1). */
int wf_candworker_open_handles(WfCandWorker *w);

#endif
