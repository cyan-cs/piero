#ifndef WF_EDRAFTW_WIN_H
#define WF_EDRAFTW_WIN_H

#include <windows.h>

#include <stdbool.h>
#include <stddef.h>

#include "../async/wf_worker.h"
#include "../base/wf_status.h"
#include "wf_eenc.h"

/* Writes recovery snapshots on a worker thread. New text replaces a queued
 * snapshot with the same exact path spelling; an active write finishes first.
 * Submission copies the input without waiting for a disk flush.
 *
 * Before deleting a draft, settle with drop=true to discard queued text and
 * wait for the active write. Failed writes retain one notification per path
 * until it is consumed, explicitly dropped, or followed by a successful write.
 */
typedef struct WfDraftWriter WfDraftWriter;

WfStatus wf_draftw_start(WfDraftWriter **out);

/* Takes a copy of the text for `path` and returns at once. WF_E_NOMEM when it
 * cannot be copied or the 16 slots for queued writes, active writes and pending
 * failure notices are occupied; nothing is queued then. Replacing queued text
 * uses its existing slot. */
WfStatus wf_draftw_submit(WfDraftWriter *w,
        const wchar_t *path,
        WfEncoding enc,
        const char *utf8,
        size_t n);

/* Whether the last write for `path` failed and has not been told yet. */
bool wf_draftw_take_failure(WfDraftWriter *w, const wchar_t *path);

/* Waits until nothing of `path` is being written; with `drop`, first drops the
 * text of `path` that has not been started (and forgets a failure of it). Without
 * it, what waits is written first (before the draft is read). */
void wf_draftw_settle(WfDraftWriter *w, const wchar_t *path, bool drop);

/* Writes what waits, then ends the thread; waits `wait_ms` at most. */
WfWorkerStopReport wf_draftw_stop(WfDraftWriter *w, DWORD wait_ms);

#endif
