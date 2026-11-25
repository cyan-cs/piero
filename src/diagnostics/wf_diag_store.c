#include <windows.h>

#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../fs/owned_delete.h"
#include "wf_diag_internal.h"

wchar_t *wf_diag_path_of(WfDiagnostics *log, const wchar_t *leaf)
{
	size_t a = wcslen(log->canonical.path), b = wcslen(leaf);
	wchar_t *path;
	if (a > SIZE_MAX / sizeof *path - b - 2) {
		return NULL;
	}
	path = malloc((a + b + 2) * sizeof *path);
	if (path) {
		swprintf(path, a + b + 2, L"%ls\\%ls", log->canonical.path,
		        leaf);
	}
	return path;
}

wchar_t *wf_diag_slot_path(WfDiagnostics *log, unsigned slot)
{
	wchar_t leaf[16];
	swprintf(leaf, 16, L"log%u.wfl", slot);
	return wf_diag_path_of(log, leaf);
}

bool wf_diag_write_bytes(HANDLE h, const void *bytes, DWORD count, DWORD *error)
{
	DWORD wrote;
	if (!WriteFile(h, bytes, count, &wrote, NULL)) {
		*error = GetLastError();
		return 0;
	}
	if (wrote != count) {
		*error = ERROR_WRITE_FAULT;
		return 0;
	}
	return 1;
}

static bool marker_create(WfDiagnostics *log, const wchar_t *path, DWORD *error)
{
	WfDiagDiskMarker marker = {0};
	/* Same bound marker_open enforces: refuse before creating a file the
	 * next start would reject. A failure after CREATE_NEW leaves the
	 * partial marker in place; removal belongs to the ownership-proven
	 * delete path (CI-DEL-01). */
	if (wcslen(log->canonical.path) > WF_DIAG_PATH_UNITS) {
		*error = ERROR_FILENAME_EXCED_RANGE;
		return 0;
	}
	log->marker =
	        CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
	                NULL, CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	if (INVALID_HANDLE_VALUE == log->marker) {
		*error = GetLastError();
		return 0;
	}
	if (!wf_diag_plain(log->marker, false, NULL, &log->marker_id, error) ||
	        !wf_diag_exact_path(log->marker, path, error)) {
		return 0;
	}
	memcpy(marker.magic, "WFDROOT1", 8);
	marker.root = log->root_id;
	marker.marker = log->marker_id;
	marker.path_units = (uint32_t)wcslen(log->canonical.path);
	if (BCryptGenRandom(
	            NULL, marker.nonce, 32, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) {
		*error = ERROR_GEN_FAILURE;
		return 0;
	}
	marker.crc32 = wf_diag_marker_crc(&marker, log->canonical.path);
	if (!wf_diag_write_bytes(log->marker, &marker, sizeof marker, error) ||
	        !wf_diag_write_bytes(log->marker, log->canonical.path,
	                marker.path_units * (DWORD)sizeof(wchar_t), error)) {
		return 0;
	}
	if (!FlushFileBuffers(log->marker)) {
		*error = GetLastError();
		return 0;
	}
	CloseHandle(log->marker);
	log->marker = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	if (INVALID_HANDLE_VALUE == log->marker) {
		*error = GetLastError();
		return 0;
	}
	return 1;
}

bool wf_diag_marker_open(WfDiagnostics *log, DWORD *error)
{
	wchar_t *path = wf_diag_path_of(log, WF_DIAG_MARKER_NAME);
	WfDiagDiskMarker marker = {0};
	unsigned char *bytes = NULL;
	size_t n = 0;
	bool ok = false;
	DWORD saved;
	if (!path) {
		*error = ERROR_NOT_ENOUGH_MEMORY;
		return 0;
	}
	log->marker = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	if (INVALID_HANDLE_VALUE == log->marker) {
		saved = GetLastError();
		if (ERROR_FILE_NOT_FOUND != saved) {
			*error = saved;
			goto done;
		}
		if (!wf_diag_known_entries(log, true, error)) {
			goto done;
		}
		if (!marker_create(log, path, error)) {
			goto done;
		}
	}
	if (!wf_diag_read_file(log->marker, &bytes, &n, error)) {
		goto done;
	}
	if (n < sizeof marker) {
		*error = ERROR_INVALID_DATA;
		goto done;
	}
	memcpy(&marker, bytes, sizeof marker);
	if (!wf_diag_plain(log->marker, false, NULL, &log->marker_id, error) ||
	        !wf_diag_exact_path(log->marker, path, error)) {
		goto done;
	}
	if (memcmp(marker.magic, "WFDROOT1", 8) ||
	        !wf_diag_equal_id(&marker.root, &log->root_id) ||
	        !wf_diag_equal_id(&marker.marker, &log->marker_id) ||
	        marker.path_units != wcslen(log->canonical.path) ||
	        marker.path_units > WF_DIAG_PATH_UNITS ||
	        n !=
	                sizeof marker +
	                        (size_t)marker.path_units * sizeof(wchar_t) ||
	        memcmp(bytes + sizeof marker, log->canonical.path,
	                (size_t)marker.path_units * sizeof(wchar_t)) ||
	        !wf_diag_nonzero(marker.nonce, 32) ||
	        marker.crc32 !=
	                wf_diag_marker_crc(&marker, log->canonical.path)) {
		*error = ERROR_INVALID_DATA;
		goto done;
	}
	memcpy(log->nonce, marker.nonce, 32);
	ok = true;
done:
	free(bytes);
	free(path);
	return ok;
}

static bool create_slot(WfDiagnostics *log, uint64_t generation, DWORD *error)
{
	unsigned slot = (unsigned)((generation - 1) % WF_DIAG_SLOTS);
	WfDiagDiskHeader h = {0};
	FILE_ID_INFO id;
	wchar_t *path = wf_diag_slot_path(log, slot);
	bool ok = false;
	if (!path) {
		*error = ERROR_NOT_ENOUGH_MEMORY;
		return 0;
	}
	log->file =
	        CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
	                NULL, CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	if (INVALID_HANDLE_VALUE == log->file) {
		*error = GetLastError();
		goto done;
	}
	if (!wf_diag_plain(log->file, false, NULL, &id, error) ||
	        !wf_diag_exact_path(log->file, path, error) ||
	        !wf_diag_root_valid(log, error)) {
		goto done;
	}
	memcpy(h.magic, "WFDLOG01", 8);
	h.root = log->root_id;
	h.marker = log->marker_id;
	h.object = id;
	memcpy(h.root_nonce, log->nonce, 32);
	h.generation = generation;
	h.slot = slot;
	h.path_units = (uint32_t)wcslen(log->canonical.path);
	if (BCryptGenRandom(NULL, h.object_nonce, 32,
	            BCRYPT_USE_SYSTEM_PREFERRED_RNG)) {
		*error = ERROR_GEN_FAILURE;
		goto done;
	}
	h.crc32 = wf_diag_header_crc(&h, log->canonical.path);
	if (!wf_diag_write_bytes(log->file, &h, sizeof h, error) ||
	        !wf_diag_write_bytes(log->file, log->canonical.path,
	                h.path_units * (DWORD)sizeof(wchar_t), error)) {
		goto done;
	}
	if (!FlushFileBuffers(log->file)) {
		*error = GetLastError();
		goto done;
	}
	log->active = slot;
	log->slots[slot].present = true;
	log->slots[slot].header = h;
	log->slots[slot].bytes =
	        sizeof h + (size_t)h.path_units * sizeof(wchar_t);
	ok = true;
done:
	free(path);
	return ok;
}

bool wf_diag_slots_open(WfDiagnostics *log, DWORD *error)
{
	uint64_t newest = 0, oldest = UINT64_MAX;
	for (unsigned i = 0; i < WF_DIAG_SLOTS; i++) {
		wchar_t *path = wf_diag_slot_path(log, i);
		HANDLE h;
		DWORD saved;
		if (!path) {
			*error = ERROR_NOT_ENOUGH_MEMORY;
			goto fail;
		}
		h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
		        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
		saved = GetLastError();
		free(path);
		if (INVALID_HANDLE_VALUE == h) {
			if (ERROR_FILE_NOT_FOUND == saved) {
				continue;
			}
			*error = saved;
			goto fail;
		}
		bool ok = wf_diag_header_read(log, h, i, &log->slots[i], error);
		CloseHandle(h);
		if (!ok) {
			goto fail;
		}
		uint64_t generation = log->slots[i].header.generation;
		if (generation > newest) {
			newest = generation;
			log->active = i;
		}
		if (generation < oldest) {
			oldest = generation;
		}
	}
	if (newest && newest - oldest >= WF_DIAG_SLOTS) {
		*error = ERROR_INVALID_DATA;
		goto fail;
	}
	if (!newest) {
		if (!create_slot(log, 1, error)) {
			goto fail;
		}
	} else {
		wchar_t *path = wf_diag_slot_path(log, log->active);
		LogSlot checked;
		DWORD saved;
		if (!path) {
			*error = ERROR_NOT_ENOUGH_MEMORY;
			goto fail;
		}
		log->file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
		        FILE_SHARE_READ, NULL, OPEN_EXISTING,
		        FILE_FLAG_OPEN_REPARSE_POINT, NULL);
		saved = GetLastError();
		free(path);
		if (INVALID_HANDLE_VALUE == log->file) {
			*error = saved;
			goto fail;
		}
		if (!wf_diag_header_read(
		            log, log->file, log->active, &checked, error)) {
			goto fail;
		}
		if (!wf_diag_equal_id(&checked.header.object,
		            &log->slots[log->active].header.object) ||
		        checked.bytes != log->slots[log->active].bytes) {
			*error = ERROR_INVALID_DATA;
			goto fail;
		}
	}
	return 1;
fail:
	return 0;
}

bool wf_diag_rotate(WfDiagnostics *log, DWORD *error)
{
	uint64_t generation = log->slots[log->active].header.generation;
	unsigned slot;
	WfOwnedLogRecord record = {0};
	WfOwnedReport report;
	WfVerifiedOwned *owned = NULL;
	if (UINT64_MAX == generation) {
		*error = ERROR_ARITHMETIC_OVERFLOW;
		return 0;
	}
	slot = (unsigned)(generation % WF_DIAG_SLOTS);
	if (INVALID_HANDLE_VALUE != log->file) {
		CloseHandle(log->file);
		log->file = INVALID_HANDLE_VALUE;
	}
	if (log->slots[slot].present) {
		WfDiagDiskHeader *h = &log->slots[slot].header;
		record.root_path = log->canonical.path;
		record.root = log->root_id;
		record.marker = log->marker_id;
		record.object = h->object;
		memcpy(record.root_nonce, log->nonce, 32);
		memcpy(record.object_nonce, h->object_nonce, 32);
		record.generation = h->generation;
		record.slot = slot;
		record.object_size = log->slots[slot].bytes;
		if (WF_OWNED_VERIFIED !=
		        wf_owned_verify_log(&record, &owned, &report)) {
			*error = report.error ? report.error
			                      : ERROR_INVALID_DATA;
			return 0;
		}
		WfOwnedResult result = wf_owned_delete(owned, &report);
		wf_owned_release(owned);
		if (WF_OWNED_DELETED != result) {
			*error = report.error ? report.error
			                      : ERROR_INVALID_DATA;
			return 0;
		}
		log->slots[slot].present = false;
	}
	return create_slot(log, generation + 1, error);
}
