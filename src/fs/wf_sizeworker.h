#ifndef WF_SIZEWORKER_H
#define WF_SIZEWORKER_H

#include <windows.h>

#include "../async/wf_worker.h"
#include "wf_size.h"

/*
 * Counts what folders hold, on a thread of its own, one request at a time.
 * Starting another request (or cancelling) changes the wanted generation.
 * The walk checks it periodically during enumeration and suppresses obsolete
 * updates; a blocking enumeration call can delay cancellation.
 *
 * Each folder has one slot that holds its newest totals, so a slow consumer
 * may miss intermediate totals but can still read the final totals of the
 * current request. Replacing or cancelling a request discards its slots.
 * The walk is iterative (no recursion), follows no link, and reads names and
 * sizes from the listing only. Each search handle closes before the next
 * folder is listed.
 */
typedef struct WfSizeWorker WfSizeWorker;

WfStatus wf_sizeworker_start(WfSizeWorker **out);
WfWorkerStopReport wf_sizeworker_stop(WfSizeWorker *w);
/* True when nothing is in flight and nothing waits to be polled: the worker
 * can be stopped without losing an answer (DR-DRAFT-036). NULL: true. */
bool wf_sizeworker_idle(WfSizeWorker *w);

/* Manual-reset event: signals available updates; poll until false to reset it. */
HANDLE wf_sizeworker_event(const WfSizeWorker *w);
bool wf_sizeworker_poll(WfSizeWorker *w, WfSizeUpdate *out);

WfSizePort wf_sizeworker_port(WfSizeWorker *w);

/* For tests: how many directory search handles are open right now. */
int wf_sizeworker_open_handles(WfSizeWorker *w);

#endif
