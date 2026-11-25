#ifndef WF_WORKCORE_H
#define WF_WORKCORE_H

#include "wf_worker.h"

#include <stdbool.h>

/*
 * The life of a worker thread, shared: a thread that waits on a condition
 * variable under one lock, an event that tells the loop a result is ready,
 * two references (the caller's and the thread's), and a stop that waits a
 * bounded time and, when the thread is stuck in a system call, hands the
 * thread to the caller as a deferred completion instead of blocking.
 *
 * A worker embeds this as its first field and keeps only what is its own:
 * the request, the results and the loop that serves them. `lock` guards the
 * core and whatever the worker keeps under it.
 */
typedef struct
{
	HANDLE thread;
	HANDLE ready; /* manual-reset: set when a result can be polled */
	SRWLOCK lock;
	CONDITION_VARIABLE wake;
	bool stop;
	bool busy; /* the thread is serving a request */
	LONG references;
	WfWorkerStats stats;
} WfWorkCore;

/* Tests only: asked before each step of a start (0: the event, 1: the thread);
 * true makes that step fail as the system would. */
extern bool (*wf_wc_test_fail)(int step);

/* Initialises the core and starts `fn(arg)` on a thread. On failure nothing
 * is left open; the caller frees its own structure. */
WfStatus wf_wc_start(WfWorkCore *c, LPTHREAD_START_ROUTINE fn, void *arg);

/* Drops one reference; the last one closes the handles and calls
 * `free_rest(owner)`, which frees what the worker holds and the worker. */
void wf_wc_release(WfWorkCore *c, void (*free_rest)(void *owner), void *owner);

/* Asks the thread to stop and waits `wait_ms`. `cancel_io`: a request that
 * is running is asked to give up its synchronous I/O. Consumes the caller's
 * reference in every outcome (see wf_worker.h for the report). */
WfWorkerStopReport wf_wc_stop(WfWorkCore *c,
        DWORD wait_ms,
        bool cancel_io,
        void (*free_rest)(void *owner),
        void *owner);

#endif
