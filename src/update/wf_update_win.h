#ifndef WF_UPDATE_WIN_H
#define WF_UPDATE_WIN_H
#include <windows.h>
#include "wf_update.h"
typedef struct WfUpdateWorker WfUpdateWorker;
/* Caller must check consent/configuration with wf_update_allowed before
 * starting; this function does not enforce them. Copies current and retains
 * no borrowed app/config pointers. NULL on invalid input or startup failure. */
WfUpdateWorker *wf_update_start(const char *current, bool manual);
HANDLE wf_update_event(const WfUpdateWorker *worker);
/* Copies a completed result. Repeated polls return the same result; neither
 * the result nor the completion event is consumed. */
bool wf_update_poll(
        WfUpdateWorker *worker, WfUpdateResult *result, bool *manual);
/* Cooperative cancellation, checked between network operations; does not wait. */
void wf_update_cancel(WfUpdateWorker *worker);
void wf_update_disable_automatic(WfUpdateWorker *worker);
/* Nonblocking shutdown. An in-flight timeout-bounded worker owns its remaining
 * reference and frees itself; it never calls back into the application. */
void wf_update_release(WfUpdateWorker *worker);
#endif
