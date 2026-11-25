#include <stdlib.h>
#include <string.h>

#include "../path/cwd_neutralize.h"
#include "wf_diag_internal.h"

void wf_diag_close(WfDiagnostics *log)
{
	if (!log) {
		return;
	}
	if (INVALID_HANDLE_VALUE != log->file) {
		CloseHandle(log->file);
	}
	if (INVALID_HANDLE_VALUE != log->marker) {
		CloseHandle(log->marker);
	}
	if (INVALID_HANDLE_VALUE != log->root) {
		CloseHandle(log->root);
	}
	wf_canonical_free(&log->canonical);
	free(log);
}

bool wf_diag_open(const wchar_t *root,
        WfDiagLevel level,
        bool paths,
        WfDiagnostics **out,
        DWORD *error)
{
	DWORD local_error;
	WfDiagnostics *log = NULL;
	wchar_t volume[50], fs[32];
	if (!error) {
		error = &local_error;
	}
	*error = ERROR_INVALID_PARAMETER;
	if (out) {
		*out = NULL;
	}
	if (!root || !out || (unsigned)level > WF_DIAG_DEBUG) {
		return 0;
	}
	log = calloc(1, sizeof *log);
	if (!log) {
		*error = ERROR_NOT_ENOUGH_MEMORY;
		return 0;
	}
	log->root = log->marker = log->file = INVALID_HANDLE_VALUE;
	log->level = level;
	log->paths = paths;
	InitializeSRWLock(&log->lock);
	if (!wf_private_directory_open(root, false, &log->root, error) ||
	        !wf_diag_plain(log->root, true, NULL, &log->root_id, error) ||
	        !wf_canonical_from_handle(log->root, &log->canonical, error)) {
		goto fail;
	}
	if (wcslen(log->canonical.path) > WF_DIAG_PATH_UNITS) {
		*error = ERROR_FILENAME_EXCED_RANGE;
		goto fail;
	}
	memcpy(volume, log->canonical.path, 49 * sizeof(wchar_t));
	volume[49] = 0;
	if (DRIVE_FIXED != GetDriveTypeW(volume)) {
		*error = ERROR_NOT_SUPPORTED;
		goto fail;
	}
	if (!GetVolumeInformationByHandleW(
	            log->root, NULL, 0, NULL, NULL, NULL, fs, 32)) {
		*error = GetLastError();
		goto fail;
	}
	if (wcscmp(fs, L"NTFS") && wcscmp(fs, L"ReFS")) {
		*error = ERROR_NOT_SUPPORTED;
		goto fail;
	}
	if (!wf_diag_marker_open(log, error) ||
	        !wf_diag_root_valid(log, error)) {
		goto fail;
	}
	if (!wf_diag_slots_open(log, error)) {
		goto fail;
	}
	*out = log;
	*error = 0;
	return 1;
fail:
	if (!*error) {
		*error = ERROR_INVALID_DATA;
	}
	wf_diag_close(log);
	return 0;
}

/* Proves the open log is still exactly the object this process created: same
 * root, a plain file with the recorded identity and the recorded length. */
static bool log_still_ours(WfDiagnostics *log, DWORD *error)
{
	uint64_t bytes;
	FILE_ID_INFO id;

	if (!wf_diag_root_valid(log, error) ||
	        !wf_diag_plain(log->file, false, &bytes, &id, error)) {
		return false;
	}
	if (!wf_diag_equal_id(&id, &log->slots[log->active].header.object) ||
	        bytes != log->slots[log->active].bytes) {
		*error = ERROR_INVALID_DATA;
		return false;
	}
	return true;
}

/* Appends one length-and-CRC prefixed record to the active slot, rotating
 * first when it would not fit, and flushes it. Any failure leaves the log in an
 * unknown state: the caller must stop logging. */
static bool append_record(
        WfDiagnostics *log, const WfBuf *pending, DWORD *error)
{
	WfDiagDiskRecord prefix;
	LARGE_INTEGER end = {0};
	const WfBuf text = *pending; /* a read-only view of the event text */
	uint64_t bytes = log->slots[log->active].bytes;

	if (!log_still_ours(log, error)) {
		return false;
	}
	if (bytes + sizeof prefix + text.len > WF_DIAG_FILE_BYTES &&
	        !wf_diag_rotate(log, error)) {
		return false;
	}
	prefix.bytes = (uint32_t)text.len;
	prefix.crc32 = wf_diag_crc32(text.data, text.len);
	if (!SetFilePointerEx(log->file, end, NULL, FILE_END)) {
		*error = GetLastError();
		return false;
	}
	if (!wf_diag_write_bytes(log->file, &prefix, sizeof prefix, error) ||
	        !wf_diag_write_bytes(
	                log->file, text.data, prefix.bytes, error)) {
		return false;
	}
	if (!FlushFileBuffers(log->file)) {
		*error = GetLastError();
		return false;
	}
	log->slots[log->active].bytes += sizeof prefix + text.len;
	return true;
}

bool wf_diag_write(WfDiagnostics *log, const WfDiagEvent *event, DWORD *error)
{
	DWORD local_error;
	WfBuf text;
	bool ok = false;
	size_t header;
	if (!error) {
		error = &local_error;
	}
	*error = ERROR_INVALID_PARAMETER;
	if (!log || !event || (unsigned)event->level > WF_DIAG_DEBUG ||
	        (unsigned)event->stage > WF_DIAG_RECOVERY ||
	        (unsigned)event->result > WF_DIAG_CANCELED) {
		return 0;
	}
	AcquireSRWLockExclusive(&log->lock);
	wf_buf_init(&text);
	if (log->poisoned) {
		*error = ERROR_INVALID_DATA;
		goto done;
	}
	if (event->level > log->level) {
		*error = 0;
		ok = true;
		goto done;
	}
	if (!wf_diag_format_event(log, event, &text, error)) {
		goto done;
	}
	header = sizeof(WfDiagDiskHeader) +
	        wcslen(log->canonical.path) * sizeof(wchar_t);
	if (text.len > WF_DIAG_FILE_BYTES - header - sizeof(WfDiagDiskRecord)) {
		*error = ERROR_BUFFER_OVERFLOW;
		goto done;
	}
	/* A refused event leaves the log usable; a failed append does not. */
	if (!append_record(log, &text, error)) {
		log->poisoned = true;
		goto done;
	}
	*error = 0;
	ok = true;
done:
	wf_buf_free(&text);
	ReleaseSRWLockExclusive(&log->lock);
	return ok;
}
