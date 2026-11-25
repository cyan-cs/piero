#ifndef WF_OWNED_INTEGRITY_H
#define WF_OWNED_INTEGRITY_H
#include "../xfer/wf_integrity_win.h"
#include "owned_delete.h"

/* Read-only candidate seam. No new owned kind, commit or cleanup authority.
 * Source must be a retained FILE_SHARE_READ guard, checked against its slot ID.
 * Destination is reopened from the proven HANDLE, never from a pathname.
 * Full ownership is rechecked around measurement. MATCH covers the unnamed
 * stream only; production execution remains blocked by the existing gates. */
WfIntegrityResult wf_owned_integrity_compare(WfIntegrityJob *job,
        HANDLE source_guard,
        const FILE_ID_INFO *source_id,
        WfVerifiedOwned *destination,
        WfIntegrityCancel cancelled,
        void *context,
        WfIntegrityReport *integrity,
        WfOwnedReport *ownership);
#endif
