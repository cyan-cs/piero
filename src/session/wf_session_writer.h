#ifndef WF_SESSION_WRITER_H
#define WF_SESSION_WRITER_H

#include <windows.h>

#include "../async/wf_worker.h"
#include "wf_session_store.h"

/*
 * Writes checkpoints on a worker thread. One record is pending at a time:
 * a newer one replaces an older queued record; an in-flight write completes.
 *
 * Startup synchronously takes the store's lock; a process that does not
 * get it has no writer (wf_session_writer_start fails with WF_E_ACCESS) and
 * writes nothing. The store must remain alive until the worker thread exits,
 * including after a timed-out stop (wait for the completion handle).
 */
typedef struct WfSessionWriter WfSessionWriter;

WfStatus wf_session_writer_start(WfSessionStore *store, WfSessionWriter **out);

/* Consumes `text` (malloc'd, `n` bytes), including when submission fails. */
WfStatus wf_session_writer_submit(WfSessionWriter *w, char *text, size_t n);

/* True if any write failed since the last call; consumes all accumulated
 * failures so that one notification covers them. */
bool wf_session_writer_take_failure(WfSessionWriter *w);

/* The records written so far (a count, for tests and the log). */
unsigned long wf_session_writer_written(const WfSessionWriter *w);

/* Writes what is pending, then stops, waiting at most `wait_ms`. Consumes the
 * writer in every outcome (see wf_worker.h). */
WfWorkerStopReport wf_session_writer_stop(WfSessionWriter *w, DWORD wait_ms);

#endif
