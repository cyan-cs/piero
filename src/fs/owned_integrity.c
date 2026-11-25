#include "owned_integrity.h"

#include <string.h>

#include "owned_internal.h"

WfIntegrityResult wf_owned_integrity_compare(WfIntegrityJob *job,
        HANDLE source_guard,
        const FILE_ID_INFO *source_id,
        WfVerifiedOwned *destination,
        WfIntegrityCancel cancelled,
        void *context,
        WfIntegrityReport *integrity,
        WfOwnedReport *ownership)
{
	HANDLE reader;
	WfIntegrityResult result;
	if (!integrity || !ownership) {
		return WF_INTEGRITY_ERROR;
	}
	memset(integrity, 0, sizeof *integrity);
	memset(ownership, 0, sizeof *ownership);
	if (!destination || WF_OWNED_COPY_PAYLOAD != destination->kind ||
	        !wfo_transaction_matches(destination, ownership)) {
		return WF_INTEGRITY_CHANGED;
	}
	reader = ReOpenFile(destination->payload, GENERIC_READ,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	        FILE_FLAG_SEQUENTIAL_SCAN);
	if (INVALID_HANDLE_VALUE == reader) {
		integrity->error = GetLastError();
		return WF_INTEGRITY_ERROR;
	}
	result = wf_integrity_compare(job, source_guard, source_id, reader,
	        &destination->record.payload, destination->record.source_size,
	        cancelled, context, integrity);
	if (!CloseHandle(reader)) {
		integrity->error = GetLastError();
		result = WF_INTEGRITY_ERROR;
	}
	if (!wfo_transaction_matches(destination, ownership)) {
		result = WF_INTEGRITY_CHANGED;
	}
	return result;
}
