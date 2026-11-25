#ifndef WF_BLOCKING_H
#define WF_BLOCKING_H
#include <windows.h>

#include <stdbool.h>

#include "../base/wf_status.h"

/*
 * Blocking work that must not freeze the screen.
 *
 * With a non-NULL wait->tick, wf_blocking_run() runs one function on a
 * thread of its own and waits on the caller's thread, calling a tick function
 * now and then so the caller
 * can redraw, follow a resize and read an Esc. The function and its arguments
 * stay where they are (the caller's stack); the call returns only after the
 * thread has finished, so nothing outlives it and there is no result to
 * poll, drop or lose.
 *
 * A job can be asked to stop. The request is a token (WfCancel) owned by the
 * run; the job's thread has it bound (wf_cancel_bind), and code deep inside
 * the job asks wf_cancel_requested() at the places where stopping is safe, so
 * the token never has to be threaded through every signature. A thread with
 * no token bound is never asked to stop. Tokens are per run: two runs at once
 * do not see each other's requests.
 */

typedef struct
{
	volatile LONG asked;
} WfCancel;

void wf_cancel_ask(WfCancel *c);
bool wf_cancel_asked(const WfCancel *c);

/* The token of the job running on this thread (NULL: none). */
void wf_cancel_bind(const WfCancel *c);
/* True when the token bound to this thread has been asked. */
bool wf_cancel_requested(void);

typedef WfStatus (*WfBlockingFn)(void *arg);
/* Called on the waiting thread; false asks the job to stop. */
typedef bool (*WfBlockingTick)(void *ctx, unsigned elapsed_ms);

typedef struct
{
	unsigned
	        first_ms; /* ticks begin once this elapsed threshold is reached */
	unsigned every_ms;   /* polling interval; 0 selects 50 ms */
	WfBlockingTick tick; /* NULL: just run it, on this thread, no tick */
	void *ctx;
} WfBlockingWait;

/* Returns what `fn` returns. `wait` may be NULL (run inline: the caller asked
 * for no waiting and no screen). With a non-NULL `wait->tick`, if the thread cannot be made
 * nothing is run and WF_E_NOMEM is returned: the job would hold the screen of
 * the caller, which is what the thread is for. */
WfStatus wf_blocking_run(
        WfBlockingFn fn, void *arg, const WfBlockingWait *wait);

/* Makes the next wf_blocking_run that needs a thread find none (for tests). */
void wf_blocking_fail_next_thread(void);

#endif
