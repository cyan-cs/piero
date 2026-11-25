#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "owned_internal.h"

bool wfo_committed_matches(WfVerifiedOwned *o, WfOwnedReport *report)
{
	wchar_t *payload = wfo_join(o->parent_path, o->rel_payload),
	        *dir = NULL, *owner = NULL;
	FILE_STANDARD_INFO size;
	uint8_t nonce[32];
	DWORD got, attr, error;
	LARGE_INTEGER zero = {0};
	bool ok = false;
	if (WF_OWNED_TXN_DIRECTORY == o->kind &&
	        (!o->nonce_proven || !o->owner_deleted ||
	                memcmp(o->proof_nonce, o->record.nonce, 32))) {
		goto done;
	}
	if (!payload || !(dir = _wcsdup(payload))) {
		goto done;
	}
	*wcsrchr(dir, L'\\') = 0;
	owner = wfo_join(dir, L"owner.bin");
	if (!owner ||
	        !wfo_object_matches(o->parent, &o->record.parent, 1,
	                o->parent_path, report) ||
	        !wfo_object_matches(
	                o->directory, &o->record.directory, 1, dir, report) ||
	        !wfo_object_matches(o->destination, &o->record.payload,
	                o->directory_payload, o->final_path, report)) {
		goto done;
	}
	if (!GetFileInformationByHandleEx(
	            o->destination, FileStandardInfo, &size, sizeof size)) {
		report->error = GetLastError();
		goto done;
	}
	if (!o->directory_payload &&
	        (size.EndOfFile.QuadPart < 0 ||
	                (uint64_t)size.EndOfFile.QuadPart !=
	                        o->record.source_size)) {
		goto done;
	}
	attr = GetFileAttributesW(payload);
	if (INVALID_FILE_ATTRIBUTES != attr) {
		goto done;
	}
	error = GetLastError();
	if (ERROR_FILE_NOT_FOUND != error) {
		report->error = error;
		goto done;
	}
	if (WF_OWNED_TXN_OWNER == o->kind) {
		if (!wfo_object_matches(
		            o->owner, &o->record.owner, 0, owner, report)) {
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
		        got != sizeof nonce ||
		        memcmp(nonce, o->record.nonce, sizeof nonce)) {
			goto done;
		}
		memcpy(o->proof_nonce, nonce, 32);
		o->nonce_proven = 1;
	}
	ok = wfo_contents_match(
	        o->directory, WF_OWNED_TXN_OWNER == o->kind, 0, report);
done:
	free(owner);
	free(dir);
	free(payload);
	return ok;
}

/* Normalizes the record into `o`, then proves a local NTFS/ReFS volume that
 * supports open-by-ID and pins the parent chain. Every failure here is
 * "uncertain": the caller reports MANUAL. */
static bool committed_prepare(WfVerifiedOwned *o,
        const WfOwnershipRecord *record,
        const wchar_t *final_leaf,
        WfOwnedReport *report)
{
	wchar_t fs[32], root[GUID_ROOT_UNITS + 1];
	DWORD flags;

	if (!wf_canonical_normalize(record->parent_path, &o->parent_path)) {
		return false;
	}
	o->rel_payload = _wcsdup(record->rel_payload);
	o->final_path = wfo_join(o->parent_path, final_leaf);
	if (!o->rel_payload || !o->final_path) {
		return false;
	}
	o->record.parent_path = o->parent_path;
	o->record.rel_payload = o->rel_payload;
	memcpy(root, o->parent_path, GUID_ROOT_UNITS * sizeof *root);
	root[GUID_ROOT_UNITS] = 0;
	if (DRIVE_FIXED != GetDriveTypeW(root) &&
	        DRIVE_REMOVABLE != GetDriveTypeW(root)) {
		return false;
	}
	if (!wfo_pin_parents(o, report)) {
		return false;
	}
	if (!GetVolumeInformationByHandleW(
	            o->parent, NULL, 0, NULL, NULL, &flags, fs, 32)) {
		report->error = GetLastError();
		return false;
	}
	return !(wcscmp(fs, L"NTFS") && wcscmp(fs, L"ReFS")) &&
	        (flags & FILE_SUPPORTS_OPEN_BY_FILE_ID);
}

/* The transaction directory and its owner file, as recorded. */
static bool transaction_locations(
        const WfVerifiedOwned *o, wchar_t **dir, wchar_t **owner)
{
	*owner = NULL;
	*dir = wfo_join(o->parent_path, o->rel_payload);
	if (!*dir) {
		return false;
	}
	*wcsrchr(*dir, L'\\') = 0;
	*owner = wfo_join(*dir, L"owner.bin");
	return NULL != *owner;
}

/* The transaction directory is gone. That is a clean ABSENT only when the
 * parent and the committed final object still match the record exactly (same
 * identity, path, kind and, for files, size). */
static bool residue_absent(const WfVerifiedOwned *o,
        const WfOwnershipRecord *record,
        int directory_payload,
        WfOwnedReport *report)
{
	FILE_STANDARD_INFO final_size;

	return wfo_object_matches(
	               o->parent, &record->parent, 1, o->parent_path, report) &&
	        wfo_object_matches(o->destination, &record->payload,
	                directory_payload, o->final_path, report) &&
	        GetFileInformationByHandleEx(o->destination, FileStandardInfo,
	                &final_size, sizeof final_size) &&
	        (directory_payload ||
	                (final_size.EndOfFile.QuadPart >= 0 &&
	                        (uint64_t)final_size.EndOfFile.QuadPart ==
	                                record->source_size));
}

WfOwnedResult wf_owned_verify_committed(WfOwnedKind kind,
        const WfOwnershipRecord *record,
        const wchar_t *final_leaf,
        int directory_payload,
        WfVerifiedOwned **out,
        WfOwnedReport *report)
{
	WfOwnedReport local = {0};
	WfVerifiedOwned *o = NULL;
	wchar_t *dir = NULL, *owner = NULL;
	DWORD error;
	if (!report) {
		report = &local;
	}
	memset(report, 0, sizeof *report);
	if (out) {
		*out = NULL;
	}
	/* A journal nonce cannot prove an already absent owner. Only the
	 * retained receipt from its verified disposition can authorize
	 * empty-dir cleanup. */
	if (!out || WF_OWNED_TXN_OWNER != kind ||
	        !wf_owned_record_valid(record) || !wfo_leaf_valid(final_leaf) ||
	        (0 != directory_payload && 1 != directory_payload)) {
		return wfo_set_result(report, WF_OWNED_MANUAL,
		        "owner nonce proof required; empty residue needs "
		        "retained receipt",
		        0);
	}
	o = calloc(1, sizeof *o);
	if (!o) {
		return wfo_set_result(report, WF_OWNED_MANUAL, "allocation",
		        ERROR_NOT_ENOUGH_MEMORY);
	}
	o->parent = o->directory = o->owner = o->payload = o->destination =
	        INVALID_HANDLE_VALUE;
	o->kind = kind;
	o->record = *record;
	o->directory_payload = directory_payload;
	if (!committed_prepare(o, record, final_leaf, report) ||
	        !transaction_locations(o, &dir, &owner)) {
		goto fail;
	}
	o->destination = CreateFileW(o->final_path, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ, NULL, OPEN_EXISTING, OPEN_FLAGS, NULL);
	if (INVALID_HANDLE_VALUE == o->destination) {
		report->error = GetLastError();
		goto fail;
	}
	o->directory = CreateFileW(dir,
	        DELETE | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES |
	                FILE_WRITE_ATTRIBUTES,
	        FILE_SHARE_READ, NULL, OPEN_EXISTING, OPEN_FLAGS, NULL);
	if (INVALID_HANDLE_VALUE == o->directory) {
		error = GetLastError();
		if (ERROR_FILE_NOT_FOUND == error &&
		        residue_absent(o, record, directory_payload, report)) {
			free(owner);
			free(dir);
			wf_owned_release(o);
			return wfo_set_result(report, WF_OWNED_ABSENT,
			        "exact committed final identity; transaction "
			        "residue absent",
			        0);
		}
		report->error = error;
		goto fail;
	}
	o->owner = CreateFileW(owner,
	        GENERIC_READ | DELETE | FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ,
	        NULL, OPEN_EXISTING, OPEN_FLAGS, NULL);
	if (INVALID_HANDLE_VALUE == o->owner) {
		report->error = GetLastError();
		goto fail;
	}
	if (!wfo_committed_matches(o, report)) {
		goto fail;
	}
	free(owner);
	free(dir);
	*out = o;
	return wfo_set_result(report, WF_OWNED_VERIFIED,
	        "committed final identity and owner nonce proof held", 0);
fail:
	free(owner);
	free(dir);
	wf_owned_release(o);
	return wfo_set_result(report, WF_OWNED_MANUAL,
	        "committed final identity or owner nonce proof uncertain",
	        report->error);
}

WfOwnedResult wf_owned_finish_committed(
        WfVerifiedOwned *owned, WfOwnedReport *report)
{
	WfOwnedReport local = {0};
	if (!report) {
		report = &local;
	}
	memset(report, 0, sizeof *report);
	if (!owned ||
	        (WF_OWNED_TXN_OWNER != owned->kind &&
	                WF_OWNED_TXN_DIRECTORY != owned->kind) ||
	        !owned->nonce_proven || !owned->owner_deleted ||
	        INVALID_HANDLE_VALUE != owned->owner ||
	        INVALID_HANDLE_VALUE == owned->directory ||
	        memcmp(owned->proof_nonce, owned->record.nonce, 32)) {
		return wfo_set_result(report, WF_OWNED_MANUAL,
		        "no retained owner-disposition nonce receipt", 0);
	}
	owned->kind = WF_OWNED_TXN_DIRECTORY;
	return wf_owned_delete(owned, report);
}
