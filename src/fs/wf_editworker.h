#ifndef WF_EDITWORKER_H
#define WF_EDITWORKER_H

#include <windows.h>

#include "../async/wf_worker.h"
#include "wf_edit.h"

/*
 * Runs create/rename off the UI thread. Unlike the listing, transfer-check and
 * preview workers this one carries EXECUTION RECEIPTS, so its contract is the
 * opposite of "latest wins":
 *
 *  - one request at a time: submit() refuses (WF_E_INVAL) while a request is
 *    queued, running, or its result has not been polled yet. Nothing is ever
 *    superseded or dropped;
 *  - a started mutation is never interrupted (no CancelSynchronousIo): a rename
 *    cannot be half-cancelled, so stop() waits for it instead;
 *  - every accepted request yields exactly one result, unless stop() discarded
 *    it before it started (then no filesystem call was made for it).
 *
 * The backend (wf_edit_win.c) re-checks the parent directory identity and the
 * source identity at the moment of the call and never replaces anything.
 */
typedef struct WfEditWorker WfEditWorker;

typedef struct
{
	uint32_t gen;
	WfEditKind kind;
	WfEditOutcome outcome;
	wchar_t new_name[256]; /* the requested name, for the status message */
} WfEditResult;

WfStatus wf_editworker_start(WfEditWorker **out);
/* Consumes the caller's reference. Waits for a running request to finish. */
WfWorkerStopReport wf_editworker_stop(WfEditWorker *w);
WfWorkerStats wf_editworker_stats(WfEditWorker *w);

HANDLE wf_editworker_event(const WfEditWorker *w);
bool wf_editworker_poll(WfEditWorker *w, WfEditResult *out);

WfEditPort wf_editworker_port(WfEditWorker *w);

#endif
