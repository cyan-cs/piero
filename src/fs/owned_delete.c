#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "owned_internal.h"
#include "base/wf_macro.h"

void wf_owned_release(WfVerifiedOwned *o)
{
	if (!o) {
		return;
	}
	if (INVALID_HANDLE_VALUE != o->payload) {
		CloseHandle(o->payload);
	}
	if (INVALID_HANDLE_VALUE != o->destination) {
		CloseHandle(o->destination);
	}
	if (INVALID_HANDLE_VALUE != o->owner) {
		CloseHandle(o->owner);
	}
	if (INVALID_HANDLE_VALUE != o->directory) {
		CloseHandle(o->directory);
	}
	if (INVALID_HANDLE_VALUE != o->parent) {
		CloseHandle(o->parent);
	}
	free(o->live_path);
	free(o->final_path);
	free(o->rel_payload);
	free(o->parent_path);
	free(o);
}

static DWORD disposition(HANDLE h, WfOwnedReport *report)
{
	FILE_DISPOSITION_INFO_EX ex = {FILE_DISPOSITION_FLAG_DELETE |
	        FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
	        FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE};
	FILE_DISPOSITION_INFO legacy = {TRUE};
	FILE_BASIC_INFO original, writable;
	DWORD error;
	bool changed = false;
	if (SetFileInformationByHandle(
	            h, FileDispositionInfoEx, &ex, sizeof ex)) {
		return 0;
	}
	error = GetLastError();
	report->ex_error = error;
	if (ERROR_INVALID_PARAMETER != error && ERROR_NOT_SUPPORTED != error &&
	        ERROR_INVALID_FUNCTION != error) {
		return error;
	}
	report->used_fallback = true;
	if (!GetFileInformationByHandleEx(
	            h, FileBasicInfo, &original, sizeof original)) {
		return GetLastError();
	}
	if (original.FileAttributes & FILE_ATTRIBUTE_READONLY) {
		writable = original;
		writable.FileAttributes &= ~(DWORD)FILE_ATTRIBUTE_READONLY;
		if (!writable.FileAttributes) {
			writable.FileAttributes = FILE_ATTRIBUTE_NORMAL;
		}
		if (!SetFileInformationByHandle(
		            h, FileBasicInfo, &writable, sizeof writable)) {
			return GetLastError();
		}
		changed = true;
	}
	if (SetFileInformationByHandle(
	            h, FileDispositionInfo, &legacy, sizeof legacy)) {
		return 0;
	}
	error = GetLastError();
	/* A failed fallback must not silently leave a readonly object writable.
	 */
	if (changed &&
	        !SetFileInformationByHandle(
	                h, FileBasicInfo, &original, sizeof original)) {
		report->restore_error = GetLastError();
	}
	return error;
}

WfOwnedResult wf_owned_delete(WfVerifiedOwned *owned, WfOwnedReport *report)
{
	WfOwnedReport local = {0};
	unsigned i;
	DWORD error = 0;
	if (!report) {
		report = &local;
	}
	memset(report, 0, sizeof *report);
	if (!owned ||
	        (WF_OWNED_COPY_PAYLOAD != owned->kind &&
	                WF_OWNED_TXN_OWNER != owned->kind &&
	                WF_OWNED_TXN_DIRECTORY != owned->kind &&
	                WF_OWNED_JOURNAL != owned->kind &&
	                WF_OWNED_JOURNAL_META != owned->kind &&
	                WF_OWNED_LIVE_TEMP_CHILD != owned->kind &&
	                WF_OWNED_LOG != owned->kind) ||
	        ((WF_OWNED_COPY_PAYLOAD == owned->kind ||
	                 WF_OWNED_JOURNAL == owned->kind ||
	                 WF_OWNED_JOURNAL_META == owned->kind ||
	                 WF_OWNED_LIVE_TEMP_CHILD == owned->kind ||
	                 WF_OWNED_LOG == owned->kind) &&
	                INVALID_HANDLE_VALUE == owned->payload) ||
	        (WF_OWNED_TXN_OWNER == owned->kind &&
	                INVALID_HANDLE_VALUE == owned->owner) ||
	        (WF_OWNED_TXN_DIRECTORY == owned->kind &&
	                INVALID_HANDLE_VALUE == owned->directory)) {
		return wfo_set_result(report, WF_OWNED_MANUAL,
		        "no verified live object handle", 0);
	}
	report->open_attempts = owned->open_attempts;
	for (i = 0; i < WF_ARRAY_LEN(wfo_retry_ms); i++) {
		if (wfo_retry_ms[i]) {
			Sleep(wfo_retry_ms[i]);
		}
		if (WF_OWNED_LIVE_TEMP_CHILD == owned->kind) {
			if (!wfo_live_matches(owned, report)) {
				return wfo_set_result(report, WF_OWNED_MANUAL,
				        "live manifested ownership changed",
				        report->error);
			}
		} else if (WF_OWNED_LOG == owned->kind) {
			if (!wfo_log_matches(owned, report)) {
				return wfo_set_result(report, WF_OWNED_MANUAL,
				        "diagnostic log proof changed before "
				        "deletion",
				        report->error);
			}
		} else if (WF_OWNED_JOURNAL == owned->kind ||
		        WF_OWNED_JOURNAL_META == owned->kind) {
			if (!wfo_local_matches(owned, report)) {
				return wfo_set_result(report, WF_OWNED_MANUAL,
				        "local metadata changed before "
				        "deletion",
				        report->error);
			}
		} else if (!wf_owned_record_valid(&owned->record) ||
		        !(WF_OWNED_COPY_PAYLOAD == owned->kind
		                        ? wfo_transaction_matches(owned, report)
		                        : wfo_committed_matches(
		                                  owned, report))) {
			return wfo_set_result(report, WF_OWNED_MANUAL,
			        "ownership changed before deletion",
			        report->error);
		}
		report->attempts++;
		HANDLE *target =
		        (WF_OWNED_COPY_PAYLOAD == owned->kind ||
		                WF_OWNED_JOURNAL == owned->kind ||
		                WF_OWNED_JOURNAL_META == owned->kind ||
		                WF_OWNED_LIVE_TEMP_CHILD == owned->kind ||
		                WF_OWNED_LOG == owned->kind)
		        ? &owned->payload
		        : WF_OWNED_TXN_OWNER == owned->kind ? &owned->owner
		                                            : &owned->directory;
		error = disposition(*target, report);
		if (!error) {
			if (WF_OWNED_TXN_OWNER == owned->kind) {
				owned->owner_deleted = 1;
			}
			CloseHandle(*target);
			*target = INVALID_HANDLE_VALUE;
			return wfo_set_result(report, WF_OWNED_DELETED,
			        "disposition accepted on verified handle; "
			        "handle closed",
			        0);
		}
		if (report->restore_error ||
		        (ERROR_SHARING_VIOLATION != error &&
		                ERROR_LOCK_VIOLATION != error)) {
			break;
		}
	}
	return wfo_set_result(report, WF_OWNED_RECOVERY_REQUIRED,
	        "handle disposition failed; object retained", error);
}

WfOwnedResult wf_owned_delete_save_temp(
        const wchar_t *path, const FILE_ID_INFO *created, WfOwnedReport *report)
{
	WfOwnedReport local = {0};
	FILE_ID_INFO id;
	FILE_ATTRIBUTE_TAG_INFO tag;
	FILE_STANDARD_INFO standard;
	HANDLE h;
	unsigned i;
	DWORD error = 0;

	if (!report) {
		report = &local;
	}
	memset(report, 0, sizeof *report);
	if (!path || !path[0] || !created) {
		return wfo_set_result(
		        report, WF_OWNED_MANUAL, "no temporary named", 0);
	}
	h = CreateFileW(path, DELETE | FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		error = GetLastError();
		if (ERROR_FILE_NOT_FOUND == error ||
		        ERROR_PATH_NOT_FOUND == error) {
			return wfo_set_result(report, WF_OWNED_ABSENT,
			        "the temporary is already gone", error);
		}
		return wfo_set_result(report, WF_OWNED_RECOVERY_REQUIRED,
		        "the temporary could not be opened", error);
	}
	if (!GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof id) ||
	        !GetFileInformationByHandleEx(
	                h, FileAttributeTagInfo, &tag, sizeof tag) ||
	        !GetFileInformationByHandleEx(
	                h, FileStandardInfo, &standard, sizeof standard)) {
		error = GetLastError();
		CloseHandle(h);
		return wfo_set_result(report, WF_OWNED_RECOVERY_REQUIRED,
		        "the temporary could not be inspected", error);
	}
	if (0 != memcmp(&id, created, sizeof id) ||
	        (tag.FileAttributes &
	                (FILE_ATTRIBUTE_DIRECTORY |
	                        FILE_ATTRIBUTE_REPARSE_POINT)) ||
	        standard.Directory || 1 != standard.NumberOfLinks) {
		CloseHandle(h);
		return wfo_set_result(report, WF_OWNED_MANUAL,
		        "the name no longer holds the file that was created",
		        0);
	}
	for (i = 0; i < WF_ARRAY_LEN(wfo_retry_ms); i++) {
		if (wfo_retry_ms[i]) {
			Sleep(wfo_retry_ms[i]);
		}
		report->attempts++;
		error = disposition(h, report);
		if (!error) {
			CloseHandle(h);
			return wfo_set_result(report, WF_OWNED_DELETED,
			        "temporary removed on its verified handle", 0);
		}
		if (report->restore_error ||
		        (ERROR_SHARING_VIOLATION != error &&
		                ERROR_LOCK_VIOLATION != error)) {
			break;
		}
	}
	CloseHandle(h);
	return wfo_set_result(report, WF_OWNED_RECOVERY_REQUIRED,
	        "the temporary could not be removed", error);
}

WfOwnedResult wf_owned_delete_empty_dir(
        const wchar_t *path, const FILE_ID_INFO *created, WfOwnedReport *report)
{
	WfOwnedReport local = {0};
	FILE_ID_INFO id;
	FILE_ATTRIBUTE_TAG_INFO tag;
	FILE_STANDARD_INFO standard;
	HANDLE h;
	unsigned step;
	DWORD error = 0, attrs;

	if (!report) {
		report = &local;
	}
	memset(report, 0, sizeof *report);
	if (!path || !path[0] || !created) {
		return wfo_set_result(
		        report, WF_OWNED_MANUAL, "no folder named", 0);
	}
	h = CreateFileW(path, DELETE | FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING,
	        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
	        NULL);
	if (INVALID_HANDLE_VALUE == h) {
		error = GetLastError();
		if (ERROR_FILE_NOT_FOUND == error ||
		        ERROR_PATH_NOT_FOUND == error) {
			return wfo_set_result(report, WF_OWNED_ABSENT,
			        "the folder is already gone", error);
		}
		return wfo_set_result(report, WF_OWNED_RECOVERY_REQUIRED,
		        "the folder could not be opened", error);
	}
	if (!GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof id) ||
	        !GetFileInformationByHandleEx(
	                h, FileAttributeTagInfo, &tag, sizeof tag) ||
	        !GetFileInformationByHandleEx(
	                h, FileStandardInfo, &standard, sizeof standard)) {
		error = GetLastError();
		CloseHandle(h);
		return wfo_set_result(report, WF_OWNED_RECOVERY_REQUIRED,
		        "the folder could not be inspected", error);
	}
	attrs = tag.FileAttributes;
	if (0 != memcmp(&id, created, sizeof id) || !standard.Directory ||
	        attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
		CloseHandle(h);
		return wfo_set_result(report, WF_OWNED_MANUAL,
		        "the name no longer holds the folder that was created",
		        0);
	}
	/* The disposition on a folder only succeeds while it is empty: nothing
	 * below it is ever removed here. */
	for (step = 0; step < WF_ARRAY_LEN(wfo_retry_ms); step++) {
		if (wfo_retry_ms[step]) {
			Sleep(wfo_retry_ms[step]);
		}
		report->attempts++;
		error = disposition(h, report);
		if (!error) {
			CloseHandle(h);
			return wfo_set_result(report, WF_OWNED_DELETED,
			        "empty folder removed on its verified handle",
			        0);
		}
		if (report->restore_error ||
		        (ERROR_SHARING_VIOLATION != error &&
		                ERROR_LOCK_VIOLATION != error)) {
			break;
		}
	}
	CloseHandle(h);
	return wfo_set_result(report, WF_OWNED_RECOVERY_REQUIRED,
	        "the folder could not be removed (not empty?)", error);
}
