#ifndef WF_FUZZYWORKER_H
#define WF_FUZZYWORKER_H

#include <windows.h>

#include "../async/wf_worker.h"
#include "wf_fuzzy.h"

/*
 * Runs fuzzy searches on a thread of its own, one at a time (DR-DRAFT-023).
 * One request slot: a newer request replaces one that has not begun and ends
 * the running one at its next look (every 32 names). One answer slot: only the
 * answer of the newest request is ever kept.
 *
 * The job's pointers are borrowed from the view on screen. release() (the
 * port's) returns only when the thread is no longer reading them; the view's
 * guard calls it before the listing changes.
 */
typedef struct WfFuzzyWorker WfFuzzyWorker;

WfStatus wf_fuzzyworker_start(WfFuzzyWorker **out);
WfWorkerStopReport wf_fuzzyworker_stop(WfFuzzyWorker *w);

/* Manual-reset event: signalled while an answer waits. */
HANDLE wf_fuzzyworker_event(const WfFuzzyWorker *w);
/* Moves the answer out (the caller frees it with wf_fz_answer_free). */
bool wf_fuzzyworker_poll(WfFuzzyWorker *w, WfFzAnswer *out);

WfFuzzyPort wf_fuzzyworker_port(WfFuzzyWorker *w);

#endif
