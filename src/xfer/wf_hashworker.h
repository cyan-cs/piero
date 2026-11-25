#ifndef WF_HASHWORKER_H
#define WF_HASHWORKER_H

#include <windows.h>

#include "../async/wf_worker.h"
#include "wf_hash.h"

/*
 * Hashes one file at a time on a thread of its own (DR-DRAFT-027). Starting
 * another request, or cancelling, stops the running one at a cancellation
 * checkpoint; a blocking read can delay this. Superseded or cancelled results
 * are discarded. Progress is published no more often than every 100 ms; a
 * slow consumer sees the latest update, including completion of the current request. The memory is one 256 KiB buffer and the hash state, whatever the size
 * of the file.
 */
typedef struct WfHashWorker WfHashWorker;

WfStatus wf_hashworker_start(WfHashWorker **out);
WfWorkerStopReport wf_hashworker_stop(WfHashWorker *w);
/* True when nothing is in flight and nothing waits to be polled: the worker
 * can be stopped without losing an answer (DR-DRAFT-036). NULL: true. */
bool wf_hashworker_idle(WfHashWorker *w);

/* Manual-reset event: signalled while an update waits. */
HANDLE wf_hashworker_event(const WfHashWorker *w);
bool wf_hashworker_poll(WfHashWorker *w, WfHashUpdate *out);

WfHashPort wf_hashworker_port(WfHashWorker *w);

#endif
