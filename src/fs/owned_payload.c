#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "owned_internal.h"

bool wfo_transaction_matches(WfVerifiedOwned *o, WfOwnedReport *report)
{
	wchar_t *payload_path = wfo_join(o->parent_path, o->rel_payload),
	        *dir_path = NULL, *owner_path = NULL;
	FILE_STANDARD_INFO size;
	uint8_t nonce[32];
	DWORD got;
	LARGE_INTEGER zero = {0};
	bool ok = false;
	if (!payload_path) {
		goto done;
	}
	dir_path = _wcsdup(payload_path);
	if (!dir_path) {
		goto done;
	}
	*wcsrchr(dir_path, L'\\') = L'\0';
	owner_path = wfo_join(dir_path, L"owner.bin");
	if (!owner_path) {
		goto done;
	}
	if (!wfo_object_matches(
	            o->parent, &o->record.parent, 1, o->parent_path, report) ||
	        !wfo_object_matches(o->directory, &o->record.directory, 1,
	                dir_path, report) ||
	        !wfo_object_matches(
	                o->owner, &o->record.owner, 0, owner_path, report)) {
		goto done;
	}
	if (!GetFileInformationByHandleEx(
	            o->owner, FileStandardInfo, &size, sizeof size) ||
	        !SetFilePointerEx(o->owner, zero, NULL, FILE_BEGIN) ||
	        !ReadFile(o->owner, nonce, sizeof nonce, &got, NULL)) {
		report->error = GetLastError();
		goto done;
	}
	if (size.EndOfFile.QuadPart != (LONGLONG)sizeof nonce ||
	        got != sizeof nonce || memcmp(nonce, o->record.nonce, 32)) {
		goto done;
	}
	if (INVALID_HANDLE_VALUE != o->payload) {
		if (!wfo_object_matches(o->payload, &o->record.payload,
		            o->directory_payload, payload_path, report)) {
			goto done;
		}
		if (!GetFileInformationByHandleEx(
		            o->payload, FileStandardInfo, &size, sizeof size)) {
			report->error = GetLastError();
			goto done;
		}
		if (!o->directory_payload &&
		        (size.EndOfFile.QuadPart < 0 ||
		                (uint64_t)size.EndOfFile.QuadPart >
		                        o->record.source_size)) {
			goto done;
		}
	}
	ok = wfo_contents_match(
	        o->directory, 1, INVALID_HANDLE_VALUE != o->payload, report);
done:
	free(owner_path);
	free(dir_path);
	free(payload_path);
	return ok;
}

/* The three recorded locations, derived once from the normalized parent and
 * the record's relative payload path. */
typedef struct
{
	wchar_t *payload, *dir, *owner;
} Locations;

static void locations_free(Locations *loc)
{
	free(loc->owner);
	free(loc->dir);
	free(loc->payload);
	memset(loc, 0, sizeof *loc);
}

static bool locations_build(const WfVerifiedOwned *o, Locations *loc)
{
	memset(loc, 0, sizeof *loc);
	loc->payload = wfo_join(o->parent_path, o->rel_payload);
	loc->dir = loc->payload ? _wcsdup(loc->payload) : NULL;
	if (!loc->dir) {
		locations_free(loc);
		return false;
	}
	*wcsrchr(loc->dir, L'\\') = 0;
	loc->owner = wfo_join(loc->dir, L"owner.bin");
	if (!loc->owner) {
		locations_free(loc);
		return false;
	}
	return true;
}

/* Normalizes the record into `o`, then proves the volume is a local NTFS/ReFS
 * one, pins the parent chain and matches the recorded parent. Sets
 * report->reason on every refusal. */
static bool prepare_context(WfVerifiedOwned *o,
        const WfOwnershipRecord *record,
        WfOwnedReport *report)
{
	wchar_t fs[32], root[GUID_ROOT_UNITS + 1];
	DWORD flags;

	wf_canonical_normalize(record->parent_path, &o->parent_path);
	o->rel_payload = _wcsdup(record->rel_payload);
	if (!o->parent_path || !o->rel_payload) {
		return false;
	}
	o->record.parent_path = o->parent_path;
	o->record.rel_payload = o->rel_payload;
	memcpy(root, o->parent_path, GUID_ROOT_UNITS * sizeof *root);
	root[GUID_ROOT_UNITS] = 0;
	if (DRIVE_FIXED != GetDriveTypeW(root) &&
	        DRIVE_REMOVABLE != GetDriveTypeW(root)) {
		report->reason = "not a known local volume";
		return false;
	}
	if (!wfo_pin_parents(o, report)) {
		report->reason = "parent chain unavailable or reparse";
		return false;
	}
	if (!GetVolumeInformationByHandleW(
	            o->parent, NULL, 0, NULL, NULL, &flags, fs, 32)) {
		report->error = GetLastError();
		report->reason = "volume query failed";
		return false;
	}
	if ((wcscmp(fs, L"NTFS") && wcscmp(fs, L"ReFS")) ||
	        !(flags & FILE_SUPPORTS_OPEN_BY_FILE_ID)) {
		report->reason = "filesystem not supported for ownership proof";
		return false;
	}
	if (!wfo_object_matches(
	            o->parent, &record->parent, 1, o->parent_path, report)) {
		report->reason = "parent identity/path mismatch";
		return false;
	}
	return true;
}

/* Looks for the recorded payload ID even if its recorded name is gone. By-ID
 * NTFS handles cannot carry a namespace disposition (error 87), so this handle
 * is only used to compare identity; it is closed again and a NAMED handle is
 * acquired later. *found tells whether the ID resolved. Only an unambiguous
 * NOT_FOUND establishes absence: NTFS can return INVALID_PARAMETER for a stale
 * ID, which stays uncertain (false). */
static bool discover_payload(HANDLE parent,
        const WfOwnershipRecord *record,
        const wchar_t *payload_path,
        bool *found,
        WfOwnedReport *report)
{
	HANDLE discovery =
	        wfo_open_id(parent, &record->payload, FILE_READ_ATTRIBUTES);
	DWORD error;

	*found = INVALID_HANDLE_VALUE != discovery;
	if (*found) {
		bool matches = wfo_object_matches(
		        discovery, &record->payload, 0, payload_path, report);

		CloseHandle(discovery);
		if (!matches) {
			report->reason = "discovered payload "
			                 "identity/path/type/link mismatch";
		}
		return matches;
	}
	error = GetLastError();
	if (ERROR_FILE_NOT_FOUND != error) {
		report->error = error;
		report->reason = "by-ID discovery uncertain";
		return false;
	}
	return true;
}

/* Handles on the transaction directory and its owner file. */
static bool acquire_transaction(
        WfVerifiedOwned *o, const Locations *loc, WfOwnedReport *report)
{
	o->directory = CreateFileW(loc->dir,
	        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
	        NULL, OPEN_EXISTING, OPEN_FLAGS, NULL);
	if (INVALID_HANDLE_VALUE != o->directory) {
		o->owner = CreateFileW(loc->owner, GENERIC_READ,
		        FILE_SHARE_READ, NULL, OPEN_EXISTING, OPEN_FLAGS, NULL);
	}
	if (INVALID_HANDLE_VALUE == o->directory ||
	        INVALID_HANDLE_VALUE == o->owner) {
		report->error = GetLastError();
		report->reason = "transaction/owner acquisition failed";
		return false;
	}
	return true;
}

/* Opens the payload by name for deletion authority, retrying only sharing and
 * lock violations on the fixed schedule. Returns 0 on success, otherwise the
 * Win32 error of the last attempt. */
static DWORD acquire_payload(WfVerifiedOwned *o, const wchar_t *payload_path)
{
	DWORD error = ERROR_GEN_FAILURE;

	for (o->open_attempts = 0; o->open_attempts < 6;) {
		if (wfo_retry_ms[o->open_attempts]) {
			Sleep(wfo_retry_ms[o->open_attempts]);
		}
		o->open_attempts++;
		o->payload = CreateFileW(payload_path,
		        DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
		        FILE_SHARE_READ, NULL, OPEN_EXISTING, OPEN_FLAGS, NULL);
		if (INVALID_HANDLE_VALUE != o->payload) {
			return 0;
		}
		error = GetLastError();
		if (ERROR_SHARING_VIOLATION != error &&
		        ERROR_LOCK_VIOLATION != error) {
			break;
		}
	}
	return error;
}

/* Classifies a failed payload acquisition. The only way to ABSENT is: the ID
 * did not resolve, the name is gone, and the rest of the transaction still
 * matches. */
static WfOwnedResult payload_refusal(
        WfVerifiedOwned *o, DWORD error, bool id_found, WfOwnedReport *report)
{
	if (ERROR_FILE_NOT_FOUND == error && !id_found &&
	        wfo_transaction_matches(o, report)) {
		report->reason = "recorded ID and transaction location absent; "
		                 "no delete";
		return WF_OWNED_ABSENT;
	}
	report->error = error;
	report->reason = "payload acquisition failed; no deletion authority";
	return ERROR_SHARING_VIOLATION == error || ERROR_LOCK_VIOLATION == error
	        ? WF_OWNED_RECOVERY_REQUIRED
	        : WF_OWNED_MANUAL;
}

/* Acquires the named handles after the by-ID identity check, then reverifies
 * all identities and paths on them and retains them. No pathname is ever
 * supplied to wf_owned_delete or disposition. */
static WfOwnedResult acquire_objects(WfVerifiedOwned *o,
        const WfOwnershipRecord *record,
        WfOwnedReport *report)
{
	Locations loc;
	bool id_found;
	DWORD error;
	WfOwnedResult result = WF_OWNED_MANUAL;

	if (!locations_build(o, &loc)) {
		return WF_OWNED_MANUAL;
	}
	if (!discover_payload(
	            o->parent, record, loc.payload, &id_found, report) ||
	        !acquire_transaction(o, &loc, report)) {
		goto done;
	}
	error = acquire_payload(o, loc.payload);
	report->open_attempts = o->open_attempts;
	if (error) {
		result = payload_refusal(o, error, id_found, report);
		goto done;
	}
	result = WF_OWNED_VERIFIED;
done:
	locations_free(&loc);
	return result;
}

WfOwnedResult wf_owned_verify(WfOwnedKind kind,
        const WfOwnershipRecord *record,
        WfVerifiedOwned **out,
        WfOwnedReport *report)
{
	WfOwnedReport local = {0};
	WfVerifiedOwned *o;
	WfOwnedResult result = WF_OWNED_MANUAL;

	if (!report) {
		report = &local;
	}
	memset(report, 0, sizeof *report);
	if (out) {
		*out = NULL;
	}
	if (!out || WF_OWNED_COPY_PAYLOAD != kind ||
	        !wf_owned_record_valid(record)) {
		return wfo_set_result(report, result,
		        "invalid record or unsupported owned kind", 0);
	}
	o = calloc(1, sizeof *o);
	if (!o) {
		return wfo_set_result(
		        report, result, "allocation", ERROR_NOT_ENOUGH_MEMORY);
	}
	o->kind = kind;
	o->record = *record;
	o->parent = o->directory = o->owner = o->payload = o->destination =
	        INVALID_HANDLE_VALUE;
	if (!prepare_context(o, record, report)) {
		goto done;
	}
	result = acquire_objects(o, record, report);
	if (WF_OWNED_VERIFIED != result) {
		goto done;
	}
	result = WF_OWNED_MANUAL;
	if (!wfo_transaction_matches(o, report)) {
		report->reason = "transaction ownership predicate failed";
		goto done;
	}
	*out = o;
	report->reason = "all ownership checks held; retained handles";
	return WF_OWNED_VERIFIED;
done:
	if (!report->reason) {
		report->reason = "allocation or ownership uncertainty";
	}
	wf_owned_release(o);
	return result;
}
