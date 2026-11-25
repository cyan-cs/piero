#ifndef WF_SYNWORKER_H
#define WF_SYNWORKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../async/wf_worker.h"
#include "../base/wf_status.h"
#include "wf_synport.h"
#include "wf_syntax_abi.h"

/*
 * The thread that colours text (DR-DRAFT-033). It loads piero-syntax.dll
 * and runs its parsers off the UI thread, keeps one session per slot
 * (the preview pane and the editor), and answers with runs of classes.
 * A request replaces the older one of its slot;
 * an answer is dropped if its slot has moved on, so a result for a file that
 * is no longer shown never reaches the screen.
 */
typedef struct WfSynWorker WfSynWorker;

WfStatus wf_synworker_start(WfSynWorker **out);
WfWorkerStopReport wf_synworker_stop(WfSynWorker *w);
HANDLE wf_synworker_event(const WfSynWorker *w);
/* The newest answer of a slot, if any; the caller frees it. */
bool wf_synworker_poll(WfSynWorker *w, WfSynAnswer *out);
WfSynPort wf_synworker_port(WfSynWorker *w);

#endif
