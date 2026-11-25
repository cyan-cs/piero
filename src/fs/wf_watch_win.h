#ifndef WF_WATCH_WIN_H
#define WF_WATCH_WIN_H

#include <windows.h>

#include <stdbool.h>

#include "../base/wf_status.h"

/*
 * Change notification for one directory tree (ReadDirectoryChangesW, overlapped).
 * Names distinguish listing changes from selected-child changes. Overflow or
 * an ambiguous alias conservatively refreshes both; notifications never
 * authorize file operations.
 */
typedef struct WfWatch WfWatch;
typedef struct
{
	bool listing; /* an immediate child changed */
	bool preview; /* the selected child or its descendants changed */
} WfWatchChanges;

/* Pure bounded classification; empty/invalid/overflow data invalidates both.
 * The caller supplies DWORD-aligned data. Names are not NUL-terminated. */
WfWatchChanges wf_watch_classify(
        const void *data, size_t bytes, const wchar_t *selected);

/* The directory handle allows read/write/delete sharing, so watching never
 * blocks the user (or wfile) from renaming or removing the folder. */
WfStatus wf_watch_open(const wchar_t *dir, WfWatch **out);
/* NULL-safe. It waits a short while (WATCH_CLOSE_WAIT_MS) for the cancelled
 * request to finish; one that has not is released later, by a later open or
 * close, once the system has let go of its buffer. */
void wf_watch_close(WfWatch *w);

/* Auto-reset event signalled when a notification completes. */
HANDLE wf_watch_event(const WfWatch *w);

/* Call after the event fires: collects the completion and re-arms. Returns
 * true if the directory changed (or the watch overflowed), false if spurious
 * or the watch has died (e.g. the folder was deleted). */
bool wf_watch_consume_selected(
        WfWatch *w, const wchar_t *selected, WfWatchChanges *changes);
bool wf_watch_consume(WfWatch *w); /* conservative legacy consumer */

bool wf_watch_active(const WfWatch *w);

#endif
