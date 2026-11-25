#include "wf_verify_win.h"

#include <string.h>

WfVerifyVerdict wf_verify_verdict(WfIntegrityResult result)
{
	if (WF_INTEGRITY_MATCH == result) {
		return WF_VERDICT_MATCH;
	}
	if (WF_INTEGRITY_MISMATCH == result) {
		return WF_VERDICT_MISMATCH;
	}
	if (WF_INTEGRITY_CHANGED == result) {
		return WF_VERDICT_CHANGED;
	}
	if (WF_INTEGRITY_CANCELLED == result) {
		return WF_VERDICT_CANCELLED;
	}
	return WF_VERDICT_ERROR;
}

bool wf_verify_report(WfStageMachine *m, WfIntegrityResult result)
{
	return wf_stage_verified(m, wf_verify_verdict(result));
}

bool wf_verify_handles(WfStageMachine *m,
        HANDLE source,
        const FILE_ID_INFO *source_id,
        HANDLE destination,
        const FILE_ID_INFO *destination_id,
        uint64_t size,
        WfIntegrityCancel cancelled,
        void *context,
        WfIntegrityReport *report)
{
	WfIntegrityJob *job = NULL;
	WfIntegrityResult result;
	WfIntegrityReport local;

	if (!report) {
		report = &local;
	}
	memset(report, 0, sizeof *report);
	if (!wf_integrity_begin(&job, report)) {
		/* a job left behind by a failed begin must still be released */
		wf_integrity_end(&job);
		return wf_verify_report(m, WF_INTEGRITY_ERROR);
	}
	result = wf_integrity_compare(job, source, source_id, destination,
	        destination_id, size, cancelled, context, report);
	if (!wf_integrity_end(&job) && WF_INTEGRITY_MATCH == result) {
		result = WF_INTEGRITY_ERROR; /* an unreleased provider is not a
		                                clean measurement */
	}
	return wf_verify_report(m, result);
}

bool wf_verify_owned(WfStageMachine *m,
        HANDLE source_guard,
        const FILE_ID_INFO *source_id,
        WfVerifiedOwned *destination,
        WfIntegrityCancel cancelled,
        void *context,
        WfIntegrityReport *integrity,
        WfOwnedReport *ownership)
{
	WfIntegrityJob *job = NULL;
	WfIntegrityResult result;
	WfIntegrityReport local_integrity;
	WfOwnedReport local_ownership;

	if (!integrity) {
		integrity = &local_integrity;
	}
	if (!ownership) {
		ownership = &local_ownership;
	}
	memset(integrity, 0, sizeof *integrity);
	memset(ownership, 0, sizeof *ownership);
	if (!wf_integrity_begin(&job, integrity)) {
		wf_integrity_end(&job);
		return wf_verify_report(m, WF_INTEGRITY_ERROR);
	}
	result = wf_owned_integrity_compare(job, source_guard, source_id,
	        destination, cancelled, context, integrity, ownership);
	if (!wf_integrity_end(&job) && WF_INTEGRITY_MATCH == result) {
		result = WF_INTEGRITY_ERROR;
	}
	return wf_verify_report(m, result);
}
