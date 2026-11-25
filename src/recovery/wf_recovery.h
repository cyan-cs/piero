#ifndef WF_RECOVERY_H
#define WF_RECOVERY_H
#include "../fs/owned_delete.h"
typedef enum
{
	WF_RECOVERY_END,
	WF_RECOVERY_MANUAL,
	WF_RECOVERY_CLEANUP_CANDIDATE,
	WF_RECOVERY_ABORTED,
	WF_RECOVERY_COMMITTED
} WfRecoveryClass;
typedef struct
{
	WfRecoveryClass classification;
	WfOwnedResult payload_proof, residue_proof;
	DWORD error;
	const char *reason;
} WfRecoveryReport;
/* Read-only inspection. A candidate is never deletion authority. Directory
 * transactions before commit always remain manual after process restart. */
WfRecoveryClass wf_recovery_inspect(const WfOwnershipRecord *record,
        const wchar_t *final_leaf,
        int directory_transaction,
        WfRecoveryReport *report);
const char *wf_recovery_class_name(WfRecoveryClass classification);
#endif
