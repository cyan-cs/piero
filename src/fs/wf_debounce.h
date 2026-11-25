#ifndef WF_DEBOUNCE_H
#define WF_DEBOUNCE_H

#include <stdbool.h>
#include <stdint.h>

#define WF_WAIT_FOREVER 0xFFFFFFFFu

/*
 * Coalesces bursts of change notifications into one refresh. The refresh
 * fires at most once per interval; the interval backs off 100 -> 250 -> 500
 * -> 1000 ms while notifications keep arriving right after a refresh, and
 * resets once things go quiet. Pure bookkeeping: the caller supplies time.
 */
typedef struct
{
	bool pending;
	bool has_fired;
	unsigned step; /* index into the interval table */
	uint64_t due;  /* when the pending refresh is allowed to fire */
	uint64_t last_fire;
} WfDebounce;

void wf_debounce_init(WfDebounce *d);

/* A change notification arrived at `now` (ms). */
void wf_debounce_note(WfDebounce *d, uint64_t now);

/* True exactly once per due refresh, and consumes it. */
bool wf_debounce_take(WfDebounce *d, uint64_t now);

/* Milliseconds until a refresh is due, 0 if overdue, WF_WAIT_FOREVER if
 * nothing is pending. */
uint32_t wf_debounce_timeout(const WfDebounce *d, uint64_t now);

#endif
