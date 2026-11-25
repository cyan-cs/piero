#include "wf_recovery.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../base/wf_mem.h"

const char *wf_recovery_class_name(WfRecoveryClass c)
{
	switch (c) {
	case WF_RECOVERY_END:               return "END";
	case WF_RECOVERY_MANUAL:            return "MANUAL";
	case WF_RECOVERY_CLEANUP_CANDIDATE: return "AUTO_CANDIDATE";
	case WF_RECOVERY_ABORTED:           return "ABORTED";
	case WF_RECOVERY_COMMITTED:         return "COMMITTED";
	}
	return "MANUAL";
}

WfRecoveryClass wf_recovery_inspect(const WfOwnershipRecord *record,
        const wchar_t *final_leaf,
        int directory_transaction,
        WfRecoveryReport *report)
{
	WfRecoveryReport local = {0};
	WfVerifiedOwned *token = NULL;
	WfOwnedReport proof = {0};
	size_t n;
	wchar_t *path = NULL;
	if (!report) {
		report = &local;
	}
	memset(report, 0, sizeof *report);
	report->classification = WF_RECOVERY_MANUAL;
	report->payload_proof = report->residue_proof = WF_OWNED_MANUAL;
	report->reason = "missing or malformed durable recovery identity";
	if (!wf_owned_record_valid(record) || !final_leaf || !*final_leaf ||
	        wcspbrk(final_leaf, L"\\/:*?\"<>|") ||
	        !wcscmp(final_leaf, L".") || !wcscmp(final_leaf, L"..") ||
	        wcslen(final_leaf) > 255 ||
	        (0 != directory_transaction && 1 != directory_transaction)) {
		return report->classification;
	}
	if (!directory_transaction) {
		report->payload_proof = wf_owned_verify(
		        WF_OWNED_COPY_PAYLOAD, record, &token, &proof);
		wf_owned_release(token);
		token = NULL;
		if (WF_OWNED_VERIFIED == report->payload_proof) {
			report->reason = "durable file identity and complete "
			                 "retained-handle proof";
			return report->classification =
			               WF_RECOVERY_CLEANUP_CANDIDATE;
		}
	}
	report->residue_proof = wf_owned_verify_committed(WF_OWNED_TXN_OWNER,
	        record, final_leaf, directory_transaction, &token, &proof);
	wf_owned_release(token);
	token = NULL;
	if (WF_OWNED_VERIFIED == report->residue_proof ||
	        WF_OWNED_ABSENT == report->residue_proof) {
		report->reason = "exact final identity and ownership-checked "
		                 "committed residue";
		return report->classification = WF_RECOVERY_COMMITTED;
	}
	report->error = proof.error;
	report->reason = directory_transaction
	        ? "directory crash requires manual recovery"
	        : proof.reason;
	if (WF_OWNED_ABSENT != report->payload_proof) {
		return report->classification;
	}
	n = wcslen(record->parent_path) + wcslen(final_leaf) + 2;
	if (n > 32768 || !(path = wf_malloc_array(n, sizeof *path))) {
		return report->classification;
	}
	swprintf(path, n, L"%ls\\%ls", record->parent_path, final_leaf);
	if (INVALID_FILE_ATTRIBUTES == GetFileAttributesW(path) &&
	        ERROR_FILE_NOT_FOUND == GetLastError()) {
		report->classification = WF_RECOVERY_ABORTED;
		report->reason = "recorded ID, temporary location and final "
		                 "location absent";
	}
	free(path);
	return report->classification;
}
