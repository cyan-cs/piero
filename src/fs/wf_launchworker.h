#ifndef WF_LAUNCHWORKER_H
#define WF_LAUNCHWORKER_H

#include <windows.h>

#include "../async/wf_worker.h"
#include "wf_launch.h"

/*
 * Runs ShellExecuteExW off the UI thread, on its own single-threaded COM
 * apartment, so a slow handler never freezes the terminal. Same contract as
 * the edit worker: one request at a time, never superseded; unread results
 * also block new submissions. During normal operation each accepted request
 * produces one result. Stop drops queued work and leaves a running call
 * uninterrupted, reporting deferred cleanup if its bounded wait expires.
 */
typedef struct WfLaunchWorker WfLaunchWorker;

typedef struct
{
	uint32_t gen;
	WfLaunchOutcome outcome;
	wchar_t name[256]; /* file leaf or terminal directory, truncated for display */
} WfLaunchResult;

WfStatus wf_launchworker_start(WfLaunchWorker **out);
/* Consumes the caller's reference. */
WfWorkerStopReport wf_launchworker_stop(WfLaunchWorker *w);
/* True when nothing is in flight and nothing waits to be polled: the worker
 * can be stopped without losing an answer (DR-DRAFT-036). NULL: true. */
bool wf_launchworker_idle(WfLaunchWorker *w);
HANDLE wf_launchworker_event(const WfLaunchWorker *w);
bool wf_launchworker_poll(WfLaunchWorker *w, WfLaunchResult *out);
WfLaunchPort wf_launchworker_port(WfLaunchWorker *w);

#endif
