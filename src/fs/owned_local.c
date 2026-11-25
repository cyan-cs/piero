#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "owned_internal.h"

static bool recovery_id_valid(const wchar_t id[37])
{
	size_t i;
	if (!id) {
		return 0;
	}
	for (i = 0; i < 36; i++) {
		if (8 == i || 13 == i || 18 == i || 23 == i) {
			if (L'-' != id[i]) {
				return 0;
			}
		} else if (!((id[i] >= L'0' && id[i] <= L'9') ||
		                   (id[i] >= L'a' && id[i] <= L'f'))) {
			return 0;
		}
	}
	return 0 == id[36];
}

bool wfo_local_matches(WfVerifiedOwned *o, WfOwnedReport *report)
{
	WfOwnedRootMarker marker;
	WfOwnedLocalHeader header;
	WfOwnedLocalRecord *r = &o->local_record;
	FILE_STANDARD_INFO standard;
	DWORD got;
	LARGE_INTEGER zero = {0};
	wchar_t leaf[48],
	        *marker_path =
	                wfo_join(o->parent_path, L".wfile-recovery-root"),
	        *object_path = NULL;
	bool ok = false;
	swprintf(leaf, 48,
	        WF_OWNED_JOURNAL == o->kind ? L"%ls.wjr" : L"%ls.meta",
	        r->recovery_id);
	object_path = wfo_join(o->parent_path, leaf);
	if (!marker_path || !object_path ||
	        !wfo_object_matches(
	                o->parent, &r->root, 1, o->parent_path, report) ||
	        !wfo_object_matches(
	                o->owner, &r->marker, 0, marker_path, report) ||
	        !wfo_object_matches(
	                o->payload, &r->object, 0, object_path, report)) {
		goto done;
	}
	if (!GetFileInformationByHandleEx(
	            o->owner, FileStandardInfo, &standard, sizeof standard) ||
	        !SetFilePointerEx(o->owner, zero, NULL, FILE_BEGIN) ||
	        !ReadFile(o->owner, &marker, sizeof marker, &got, NULL)) {
		report->error = GetLastError();
		goto done;
	}
	if (standard.EndOfFile.QuadPart != (LONGLONG)sizeof marker ||
	        got != sizeof marker || memcmp(marker.magic, "WFROOT01", 8) ||
	        !wfo_id_equal(&marker.root, &r->root) ||
	        memcmp(marker.nonce, r->root_nonce, 32)) {
		goto done;
	}
	if (!GetFileInformationByHandleEx(
	            o->payload, FileStandardInfo, &standard, sizeof standard) ||
	        !SetFilePointerEx(o->payload, zero, NULL, FILE_BEGIN) ||
	        !ReadFile(o->payload, &header, sizeof header, &got, NULL)) {
		report->error = GetLastError();
		goto done;
	}
	if (standard.EndOfFile.QuadPart < 0 ||
	        (uint64_t)standard.EndOfFile.QuadPart != r->object_size ||
	        got != sizeof header ||
	        memcmp(header.magic,
	                WF_OWNED_JOURNAL == o->kind ? "WFJRNL01" : "WFMETA01",
	                8) ||
	        memcmp(header.root_nonce, r->root_nonce, 32) ||
	        memcmp(header.object_nonce, r->object_nonce, 32) ||
	        memcmp(header.recovery_id, r->recovery_id,
	                sizeof header.recovery_id)) {
		goto done;
	}
	ok = true;
done:
	free(marker_path);
	free(object_path);
	return ok;
}

WfOwnedResult wf_owned_verify_local(WfOwnedKind kind,
        const WfOwnedLocalRecord *record,
        WfVerifiedOwned **out,
        WfOwnedReport *report)
{
	WfOwnedReport local = {0};
	WfVerifiedOwned *o = NULL;
	wchar_t leaf[48], *path = NULL, *marker_path = NULL,
	                  root[GUID_ROOT_UNITS + 1], fs[32];
	DWORD flags;
	if (!report) {
		report = &local;
	}
	memset(report, 0, sizeof *report);
	if (out) {
		*out = NULL;
	}
	if (!out || !record ||
	        (WF_OWNED_JOURNAL != kind && WF_OWNED_JOURNAL_META != kind) ||
	        !recovery_id_valid(record->recovery_id) ||
	        !wfo_nonzero(record->root_nonce, 32) ||
	        !wfo_nonzero(record->object_nonce, 32) ||
	        record->object_size < sizeof(WfOwnedLocalHeader) ||
	        record->object_size > 16u * 1024u * 1024u ||
	        (WF_OWNED_JOURNAL_META == kind &&
	                record->object_size != sizeof(WfOwnedLocalMeta)) ||
	        record->object.VolumeSerialNumber !=
	                record->root.VolumeSerialNumber ||
	        record->marker.VolumeSerialNumber !=
	                record->root.VolumeSerialNumber) {
		return wfo_set_result(report, WF_OWNED_MANUAL,
		        "invalid local metadata proof", 0);
	}
	o = calloc(1, sizeof *o);
	if (!o) {
		return wfo_set_result(report, WF_OWNED_MANUAL, "allocation",
		        ERROR_NOT_ENOUGH_MEMORY);
	}
	o->parent = o->directory = o->owner = o->payload = o->destination =
	        INVALID_HANDLE_VALUE;
	o->kind = kind;
	o->local_record = *record;
	if (!wf_canonical_normalize(record->root_path, &o->parent_path)) {
		goto fail;
	}
	o->local_record.root_path = o->parent_path;
	memcpy(root, o->parent_path, GUID_ROOT_UNITS * sizeof *root);
	root[GUID_ROOT_UNITS] = 0;
	if (DRIVE_FIXED != GetDriveTypeW(root) || !wfo_pin_parents(o, report)) {
		goto fail;
	}
	if (!GetVolumeInformationByHandleW(
	            o->parent, NULL, 0, NULL, NULL, &flags, fs, 32) ||
	        (wcscmp(fs, L"NTFS") && wcscmp(fs, L"ReFS"))) {
		goto fail;
	}
	swprintf(leaf, 48, WF_OWNED_JOURNAL == kind ? L"%ls.wjr" : L"%ls.meta",
	        record->recovery_id);
	path = wfo_join(o->parent_path, leaf);
	marker_path = wfo_join(o->parent_path, L".wfile-recovery-root");
	if (!path || !marker_path) {
		goto fail;
	}
	o->owner = CreateFileW(marker_path, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, OPEN_FLAGS, NULL);
	o->payload =
	        CreateFileW(path, GENERIC_READ | DELETE | FILE_WRITE_ATTRIBUTES,
	                FILE_SHARE_READ, NULL, OPEN_EXISTING, OPEN_FLAGS, NULL);
	if (INVALID_HANDLE_VALUE == o->owner ||
	        INVALID_HANDLE_VALUE == o->payload) {
		report->error = GetLastError();
		goto fail;
	}
	if (!wfo_local_matches(o, report)) {
		goto fail;
	}
	free(path);
	free(marker_path);
	*out = o;
	return wfo_set_result(report, WF_OWNED_VERIFIED,
	        "local kind-specific metadata proof held", 0);
fail:
	free(path);
	free(marker_path);
	wf_owned_release(o);
	return wfo_set_result(report, WF_OWNED_MANUAL,
	        "local metadata identity, path or content uncertain",
	        report->error);
}
