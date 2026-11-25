#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../diagnostics/wf_diag_format.h"
#include "owned_internal.h"

static bool log_read(HANDLE h,
        uint64_t expected_size,
        uint8_t **bytes,
        WfOwnedReport *report)
{
	FILE_STANDARD_INFO standard;
	LARGE_INTEGER zero = {0};
	DWORD got;
	*bytes = NULL;
	if (!expected_size || expected_size > WF_DIAG_FILE_BYTES) {
		return 0;
	}
	if (!GetFileInformationByHandleEx(
	            h, FileStandardInfo, &standard, sizeof standard)) {
		report->error = GetLastError();
		return 0;
	}
	if (standard.EndOfFile.QuadPart < 0 ||
	        (uint64_t)standard.EndOfFile.QuadPart != expected_size) {
		return 0;
	}
	*bytes = malloc((size_t)expected_size);
	if (!*bytes) {
		report->error = ERROR_NOT_ENOUGH_MEMORY;
		return 0;
	}
	if (!SetFilePointerEx(h, zero, NULL, FILE_BEGIN)) {
		report->error = GetLastError();
		goto fail;
	}
	if (!ReadFile(h, *bytes, (DWORD)expected_size, &got, NULL)) {
		report->error = GetLastError();
		goto fail;
	}
	if (got != expected_size) {
		report->error = ERROR_HANDLE_EOF;
		goto fail;
	}
	return 1;
fail:
	free(*bytes);
	*bytes = NULL;
	return 0;
}

static bool log_text_valid(
        const uint8_t *bytes, size_t size, WfOwnedReport *report)
{
	if (!wf_diag_records_valid(bytes, size)) {
		return 0;
	}
	while (size) {
		WfDiagDiskRecord record;
		memcpy(&record, bytes, sizeof record);
		bytes += sizeof record;
		size -= sizeof record;
		if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		            (const char *)bytes, (int)record.bytes, NULL, 0)) {
			report->error = GetLastError();
			return 0;
		}
		bytes += record.bytes;
		size -= record.bytes;
	}
	return 1;
}

bool wfo_log_matches(WfVerifiedOwned *o, WfOwnedReport *report)
{
	WfOwnedLogRecord *r = &o->log_record;
	WfDiagDiskMarker marker;
	WfDiagDiskHeader header;
	uint8_t *marker_bytes = NULL, *log_bytes = NULL;
	wchar_t leaf[16], *marker_path = NULL, *object_path = NULL;
	size_t units = wcslen(o->parent_path),
	       path_bytes = units * sizeof(wchar_t), header_bytes;
	bool ok = false;
	marker_path = wfo_join(o->parent_path, WF_DIAG_MARKER_NAME);
	swprintf(leaf, 16, L"log%u.wfl", (unsigned)r->slot);
	object_path = wfo_join(o->parent_path, leaf);
	if (!marker_path || !object_path) {
		report->error = ERROR_NOT_ENOUGH_MEMORY;
		goto done;
	}
	if (!wfo_object_matches(
	            o->parent, &r->root, 1, o->parent_path, report) ||
	        !wfo_object_matches(
	                o->owner, &r->marker, 0, marker_path, report) ||
	        !wfo_object_matches(
	                o->payload, &r->object, 0, object_path, report)) {
		goto done;
	}
	if (!log_read(o->owner, sizeof marker + path_bytes, &marker_bytes,
	            report)) {
		goto done;
	}
	memcpy(&marker, marker_bytes, sizeof marker);
	if (memcmp(marker.magic, "WFDROOT1", 8) ||
	        !wfo_id_equal(&marker.root, &r->root) ||
	        !wfo_id_equal(&marker.marker, &r->marker) ||
	        memcmp(marker.nonce, r->root_nonce, 32) ||
	        marker.path_units != units ||
	        memcmp(marker_bytes + sizeof marker, o->parent_path,
	                path_bytes) ||
	        marker.crc32 != wf_diag_marker_crc(&marker, o->parent_path)) {
		goto done;
	}
	if (!log_read(o->payload, r->object_size, &log_bytes, report)) {
		goto done;
	}
	memcpy(&header, log_bytes, sizeof header);
	header_bytes = sizeof header + path_bytes;
	if (memcmp(header.magic, "WFDLOG01", 8) || header.reserved ||
	        !wfo_id_equal(&header.root, &r->root) ||
	        !wfo_id_equal(&header.marker, &r->marker) ||
	        !wfo_id_equal(&header.object, &r->object) ||
	        memcmp(header.root_nonce, r->root_nonce, 32) ||
	        memcmp(header.object_nonce, r->object_nonce, 32) ||
	        header.generation != r->generation || header.slot != r->slot ||
	        header.path_units != units ||
	        memcmp(log_bytes + sizeof header, o->parent_path, path_bytes) ||
	        header.crc32 != wf_diag_header_crc(&header, o->parent_path) ||
	        !log_text_valid(log_bytes + header_bytes,
	                (size_t)r->object_size - header_bytes, report)) {
		goto done;
	}
	ok = true;
done:
	free(marker_path);
	free(object_path);
	free(marker_bytes);
	free(log_bytes);
	return ok;
}

WfOwnedResult wf_owned_verify_log(const WfOwnedLogRecord *record,
        WfVerifiedOwned **out,
        WfOwnedReport *report)
{
	WfOwnedReport local = {0};
	WfVerifiedOwned *o = NULL;
	wchar_t root[GUID_ROOT_UNITS + 1], fs[32], leaf[16], *marker = NULL,
	                                                     *object = NULL;
	DWORD flags;
	if (!report) {
		report = &local;
	}
	memset(report, 0, sizeof *report);
	if (out) {
		*out = NULL;
	}
	if (!out || !record || !record->generation ||
	        record->slot >= WF_DIAG_SLOTS ||
	        record->slot != (record->generation - 1) % WF_DIAG_SLOTS ||
	        !wfo_nonzero(record->root_nonce, 32) ||
	        !wfo_nonzero(record->object_nonce, 32) ||
	        !wfo_nonzero(record->root.FileId.Identifier, 16) ||
	        !wfo_nonzero(record->marker.FileId.Identifier, 16) ||
	        !wfo_nonzero(record->object.FileId.Identifier, 16) ||
	        record->marker.VolumeSerialNumber !=
	                record->root.VolumeSerialNumber ||
	        record->object.VolumeSerialNumber !=
	                record->root.VolumeSerialNumber ||
	        wfo_id_equal(&record->root, &record->marker) ||
	        wfo_id_equal(&record->root, &record->object) ||
	        wfo_id_equal(&record->marker, &record->object) ||
	        record->object_size < sizeof(WfDiagDiskHeader) ||
	        record->object_size > WF_DIAG_FILE_BYTES) {
		return wfo_set_result(report, WF_OWNED_MANUAL,
		        "invalid diagnostic log record", 0);
	}
	o = calloc(1, sizeof *o);
	if (!o) {
		return wfo_set_result(report, WF_OWNED_MANUAL, "log allocation",
		        ERROR_NOT_ENOUGH_MEMORY);
	}
	o->parent = o->directory = o->owner = o->payload = o->destination =
	        INVALID_HANDLE_VALUE;
	o->kind = WF_OWNED_LOG;
	o->log_record = *record;
	if (!wf_canonical_normalize(record->root_path, &o->parent_path)) {
		goto fail;
	}
	o->log_record.root_path = o->parent_path;
	if (wcslen(o->parent_path) > WF_DIAG_PATH_UNITS ||
	        sizeof(WfDiagDiskHeader) +
	                        wcslen(o->parent_path) * sizeof(wchar_t) >
	                record->object_size) {
		goto fail;
	}
	memcpy(root, o->parent_path, GUID_ROOT_UNITS * sizeof *root);
	root[GUID_ROOT_UNITS] = 0;
	if (DRIVE_FIXED != GetDriveTypeW(root) || !wfo_pin_parents(o, report)) {
		goto fail;
	}
	if (!GetVolumeInformationByHandleW(
	            o->parent, NULL, 0, NULL, NULL, &flags, fs, 32)) {
		report->error = GetLastError();
		goto fail;
	}
	if ((wcscmp(fs, L"NTFS") && wcscmp(fs, L"ReFS")) ||
	        !(flags & FILE_SUPPORTS_OPEN_BY_FILE_ID)) {
		goto fail;
	}
	marker = wfo_join(o->parent_path, WF_DIAG_MARKER_NAME);
	swprintf(leaf, 16, L"log%u.wfl", (unsigned)record->slot);
	object = wfo_join(o->parent_path, leaf);
	if (!marker || !object) {
		report->error = ERROR_NOT_ENOUGH_MEMORY;
		goto fail;
	}
	o->owner = CreateFileW(marker, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, OPEN_FLAGS, NULL);
	if (INVALID_HANDLE_VALUE == o->owner) {
		report->error = GetLastError();
		goto fail;
	}
	o->payload = CreateFileW(object,
	        GENERIC_READ | DELETE | FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ,
	        NULL, OPEN_EXISTING, OPEN_FLAGS, NULL);
	if (INVALID_HANDLE_VALUE == o->payload) {
		report->error = GetLastError();
		goto fail;
	}
	if (!wfo_log_matches(o, report)) {
		goto fail;
	}
	free(marker);
	free(object);
	*out = o;
	return wfo_set_result(report, WF_OWNED_VERIFIED,
	        "diagnostic log IDs, nonces, exact path and every CRC held", 0);
fail:
	free(marker);
	free(object);
	wf_owned_release(o);
	return wfo_set_result(report, WF_OWNED_MANUAL,
	        "diagnostic log ownership or CRC uncertain", report->error);
}
