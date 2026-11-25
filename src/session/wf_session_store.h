#ifndef WF_SESSION_STORE_H
#define WF_SESSION_STORE_H

#include <windows.h>

#include <stdbool.h>
#include <stddef.h>

#include "../base/wf_status.h"
#include "wf_session.h"

/*
 * Where the saved session lives: `session.a` and `session.b` in the state
 * folder (two slots, see wf_slotfile.h) and `session.lock`. The folder is the
 * one the program already chose and checked as private (DR-DRAFT-017); this
 * module never creates, moves or deletes a folder.
 *
 * Who may write: the process that holds `session.lock`. The lock is the open
 * handle itself, opened so that no second writer can open it; the system
 * releases it when the process ends, however it ends. No process id is stored
 * or trusted. Reading needs no lock: a reader takes the newest whole slot.
 */
typedef struct WfSessionStore WfSessionStore;

/* `state_dir` is the absolute path of the state folder in use. */
WfStatus wf_session_store_open(const wchar_t *state_dir, WfSessionStore **out);
void wf_session_store_close(WfSessionStore *s); /* NULL-safe; frees the lock */

typedef enum
{
	WF_SESSION_NONE,       /* both slots are missing or empty */
	WF_SESSION_FOUND,      /* `*out` holds it */
	WF_SESSION_DAMAGED,    /* something is there that cannot be read */
	WF_SESSION_UNSUPPORTED /* header names a different version: left as it is */
} WfSessionFound;

/* `*version` (NULL-able) is the unsupported version number when UNSUPPORTED.
 * A failed header read is DAMAGED. Damaged/unsupported data blocks writes;
 * a whole supported slot can still recover a missing or torn companion. */
WfSessionFound wf_session_store_read(
        WfSessionStore *s, WfSession *out, int *version);

/* Takes the right to write and validates existing data before checkpoints.
 * False: another process holds it, or the lock file cannot be used safely.
 * Idempotent; a successful lock does not imply the data is writable. */
bool wf_session_store_lock(WfSessionStore *s);
bool wf_session_store_locked(const WfSessionStore *s);

/* Writes one record (the body of wf_session_encode). Needs the lock; refused
 * for unsupported data (WF_E_FORMAT) or damaged data (WF_E_CORRUPT). A failed
 * decode allocation also blocks writes with WF_E_NOMEM. Flushed to the
 * disk before it returns, so call it from a worker, not the UI thread. */
WfStatus wf_session_store_write(WfSessionStore *s, const char *body, size_t n);

/* Empties both slot files: their content is gone, the files stay (nothing
 * here deletes). Needs the lock (WF_E_ACCESS without it). Only successful
 * emptying of both slots clears a write block. */
WfStatus wf_session_store_forget(WfSessionStore *s);

#endif
