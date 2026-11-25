#ifndef WF_PREVIEWWORKER_H
#define WF_PREVIEWWORKER_H
#include <windows.h>

#include "../async/wf_worker.h"
#include "wf_preview.h"

/* Keeps only the newest request/result. Replacement is counted and obsolete
 * reads do not publish. Stop follows the lifetime contract in wf_worker.h. */
typedef struct WfPreviewWorker WfPreviewWorker;
typedef struct
{
	uint32_t gen;
	WfPreview *preview;
} WfPreviewResult;
WfStatus wf_previewworker_start(
        WfPreviewWorker **out, size_t max_bytes, UINT fallback_codepage);
WfWorkerStopReport wf_previewworker_stop(WfPreviewWorker *w);
WfWorkerStats wf_previewworker_stats(WfPreviewWorker *w);
HANDLE wf_previewworker_event(const WfPreviewWorker *w);
bool wf_previewworker_poll(WfPreviewWorker *w, WfPreviewResult *out);
WfPreviewPort wf_previewworker_port(WfPreviewWorker *w);

#endif
