#ifndef WF_VERIFY_WIN_H
#define WF_VERIFY_WIN_H

#include <windows.h>

#include "../fs/owned_integrity.h"
#include "wf_integrity_win.h"
#include "wf_verify.h"

/*
 * Connects the existing read-back measurement (wf_integrity_compare: keyed
 * SHA-256, no digest leaves the module) to the stage machine. Nothing here
 * hashes or compares bytes itself.
 *
 * Both entry points REPORT the outcome to the machine, so the only way to reach
 * "the source may be removed" is a MATCH that the measurement itself produced
 * after re-checking the identities around the read. Run them off the UI thread.
 */
WfVerifyVerdict wf_verify_verdict(WfIntegrityResult result);

/* Maps a finished measurement onto the machine; true when the transfer may go
 * on. */
bool wf_verify_report(WfStageMachine *m, WfIntegrityResult result);

/* Read-back of a destination the caller holds by handle (identity and size
 * are mandatory and re-checked before and after reading). A failure to begin or
 * release the measurement is an ERROR, never a MATCH. */
bool wf_verify_handles(WfStageMachine *m,
        HANDLE source,
        const FILE_ID_INFO *source_id,
        HANDLE destination,
        const FILE_ID_INFO *destination_id,
        uint64_t size,
        WfIntegrityCancel cancelled,
        void *context,
        WfIntegrityReport *report);

/* Read-back of an ownership-proven destination: the destination is reopened
 * from the proven handle, never from a pathname, and the whole ownership proof
 * is rechecked around the measurement. */
bool wf_verify_owned(WfStageMachine *m,
        HANDLE source_guard,
        const FILE_ID_INFO *source_id,
        WfVerifiedOwned *destination,
        WfIntegrityCancel cancelled,
        void *context,
        WfIntegrityReport *integrity,
        WfOwnedReport *ownership);

#endif
