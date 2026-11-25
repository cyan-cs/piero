#ifndef WF_XFERWORKER_H
#define WF_XFERWORKER_H

#include <windows.h>

#include "../async/wf_worker.h"
#include "wf_xfer.h"

/*
 * Runs slot capture and batch preflight off the UI thread: both open many
 * files and may wait on slow network volumes. One job at a time; a newer
 * submission replaces a queued job and raises the cancel flag of a running
 * one (the preflight checks it between steps; blocking I/O is interrupted
 * with CancelSynchronousIo, best effort). The app filters stale results by
 * generation.
 */
typedef struct WfXferWorker WfXferWorker;

typedef enum
{
	WF_XR_CAPTURE,
	WF_XR_PREFLIGHT
} WfXferResultKind;

typedef struct
{
	WfXferResultKind kind;
	uint32_t gen;
	WfStatus st;
	WfTransferSlot slot;      /* CAPTURE: owned by the consumer */
	WfPreflightReport report; /* PREFLIGHT */
} WfXferResult;

WfStatus wf_xferworker_start(
        WfXferWorker **out, WfProbeOps ops, WfPreflightPolicy policy);
/* ops.ctx is borrowed until stop succeeds or report.completion signals. */
WfWorkerStopReport wf_xferworker_stop(WfXferWorker *w);
WfWorkerStats wf_xferworker_stats(WfXferWorker *w);

HANDLE wf_xferworker_event(const WfXferWorker *w);
bool wf_xferworker_poll(WfXferWorker *w, WfXferResult *out);
/* Bounded queue, newest observations take precedence. An overflow frees the
 * oldest result; stats records every discard. This is not all-result delivery.
 */

WfXferPort wf_xferworker_port(WfXferWorker *w);

#endif
