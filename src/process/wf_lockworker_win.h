#ifndef WF_LOCKWORKER_WIN_H
#define WF_LOCKWORKER_WIN_H

#include <windows.h>

#include "../async/wf_worker.h"
#include "wf_lock.h"
#include "wf_process_control_win.h"

/*
 * Runs the lock queries and the two process actions on a thread of its own
 * (DR-DRAFT-024). One request is served at a time; a request made while one is
 * waiting, running or awaiting polling is refused (WF_E_INVAL). It neither
 * queues behind nor replaces the existing request, so nothing the person chose
 * is dropped and nothing piles up.
 * The thread sleeps on a condition variable: no timer, no polling.
 *
 * The answer carries the request's generation; what to do with a stale one is
 * the screen's decision. stop() ends a wait for a closing program at once (the
 * stop event) and otherwise follows the common bounded stop.
 */
typedef struct WfLockWorker WfLockWorker;

/* `env` is copied; NULL means this process (tests may name another "self"). */
WfStatus wf_lockworker_start(WfLockWorker **out, const WfProcEnv *env);
WfWorkerStopReport wf_lockworker_stop(WfLockWorker *w);
/* True when nothing is in flight and nothing waits to be polled: the worker
 * can be stopped without losing an answer (DR-DRAFT-036). NULL: true. */
bool wf_lockworker_idle(WfLockWorker *w);

/* Manual-reset event: signalled while an answer waits. */
HANDLE wf_lockworker_event(const WfLockWorker *w);
/* Takes the answer; the caller owns it (wf_lockanswer_free). */
bool wf_lockworker_poll(WfLockWorker *w, WfLockAnswer *out);

WfLockPort wf_lockworker_port(WfLockWorker *w);

/* How long a program is given to close, and to be ended, in milliseconds. */
#define WF_LOCK_CLOSE_WAIT_MS 8000u
#define WF_LOCK_END_WAIT_MS 5000u

#endif
