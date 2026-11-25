#ifndef WF_WORKER_H
#define WF_WORKER_H
#include <windows.h>

#include <stdint.h>

#include "../base/wf_status.h"

/* Statistics for replaceable UI observations. Execution workers retain their
 * own receipt contracts; sharing a stop report does not permit dropping them.
 * Generation numbers identify requests; consumers reject stale generations.
 * Counters are cumulative and saturate instead of wrapping. */
typedef struct
{
	uint64_t completed_requests, superseded_requests, discarded_results;
} WfWorkerStats;

typedef struct
{
	WfStatus status;
	DWORD error, cancel_error;
	int deferred; /* running thread owns cleanup after the bounded wait */
	HANDLE completion; /* deferred: transferred thread handle; caller closes
	                      it */
	WfWorkerStats stats;
} WfWorkerStopReport;

/* stop consumes the caller's reference in every outcome. Stop producers and
 * pollers first; no API/port use is allowed afterward. A timed-out thread keeps
 * its own reference and releases all resources when its I/O eventually returns.
 * A non-NULL completion handle lets the caller observe that release without
 * retaining the worker pointer. Borrowed probe contexts must live until this
 * handle signals. Close it even if the caller chooses not to wait again.
 * Permanently unresponsive OS I/O remains outstanding until process exit. */
#endif
