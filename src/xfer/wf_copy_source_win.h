#ifndef WF_COPY_SOURCE_WIN_H
#define WF_COPY_SOURCE_WIN_H
#include <windows.h>

#include "wf_xfer.h"

typedef struct WfCopySourceObserver WfCopySourceObserver;
typedef enum
{
	WF_COPY_SOURCE_GUARDED,
	WF_COPY_SOURCE_ACCEPTED,
	WF_COPY_SOURCE_REJECTED,
	WF_COPY_SOURCE_UNOBSERVED
} WfCopySourceState;
typedef struct
{
	WfCopySourceState state;
	DWORD error;
	unsigned callback_checks;
	FILE_ID_INFO expected, observed;
} WfCopySourceReport;

/* Borrow an existing GENERIC_READ/FILE_SHARE_READ guard until release. The
 * caller must acquire that guard against the Transfer Slot snapshot first.
 * This observer closes the acceptance gap, not CopyFileExW's pathname lookup.
 * It grants no destination, journal, commit or cleanup authority. */
bool wf_copy_source_begin(HANDLE guard,
        const WfNodeInfo *snapshot,
        WfCopySourceObserver **out,
        WfCopySourceReport *report);
/* Pass the actual hSourceFile and stream number from EVERY progress callback.
 * Failure is sticky; caller must return PROGRESS_STOP. Unknown/ADS streams
 * refuse until separately qualified. A duplicate retains the observed source
 * object after CopyFileExW closes its callback handles. */
bool wf_copy_source_observe(WfCopySourceObserver *observer,
        HANDLE actual_source,
        DWORD stream_number,
        WfCopySourceReport *report);
/* No callback, stopped/failed API, changed guard or changed observed handle
 * cannot yield an accepted copy. Call before any verification/commit stage. */
bool wf_copy_source_finish(WfCopySourceObserver *observer,
        bool copied,
        DWORD copy_error,
        WfCopySourceReport *report);
void wf_copy_source_release(WfCopySourceObserver *observer);
#endif
