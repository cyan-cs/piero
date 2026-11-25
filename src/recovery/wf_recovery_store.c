#include <windows.h>

#include "wf_recovery_store.h"

#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../fs/owned_delete.h"
#include "../path/wf_canonical_win.h"
#include "../base/wf_mem.h"
#define FLAGS (FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT)
#define ROOT_UNITS 49
#define LOCAL_BYTES (16u * 1024u * 1024u)
#define AUDIT_BYTES (1024u * 1024u)
#define LOCAL_RECORDS 4096u

typedef struct
{
	WfRecoveryLocalInfo info;
	bool journal_name, metadata_name, intent, complete, inconsistent;
} LocalEntry;
struct WfRecoveryStore
{
	HANDLE directory, marker;
	WfCanonicalPath path;
	FILE_ID_INFO root_id, marker_id;
	uint8_t nonce[32];
	LocalEntry *entries;
	size_t count, capacity;
	bool snapshot_ready;
	DWORD snapshot_error;
};
/* Operands may be members of packed on-disk structs (unaligned): copy out. */
static bool id_equal(const void *a, const void *b)
{
	FILE_ID_INFO x, y;
	memcpy(&x, a, sizeof x);
	memcpy(&y, b, sizeof y);
	return x.VolumeSerialNumber == y.VolumeSerialNumber &&
	        !memcmp(x.FileId.Identifier, y.FileId.Identifier, 16);
}
static bool uuid_valid(const wchar_t *id)
{
	size_t i;
	if (!id || 36 != wcslen(id)) {
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
	return 1;
}
static wchar_t *path_of(WfRecoveryStore *s, const wchar_t *leaf)
{
	size_t n = wcslen(s->path.path) + wcslen(leaf) + 2;
	wchar_t *p;
	if (n > 32768) {
		return NULL;
	}
	p = wf_malloc_array(n, sizeof *p);
	if (p) {
		swprintf(p, n, L"%ls\\%ls", s->path.path, leaf);
	}
	return p;
}
static bool file_identity(HANDLE h,
        bool directory,
        FILE_ID_INFO *id,
        uint64_t *size,
        DWORD *error)
{
	FILE_ATTRIBUTE_TAG_INFO tag;
	FILE_STANDARD_INFO standard;
	if (!GetFileInformationByHandleEx(h, FileIdInfo, id, sizeof *id)) {
		*error = GetLastError();
		return 0;
	}
	if (!GetFileInformationByHandleEx(
	            h, FileAttributeTagInfo, &tag, sizeof tag)) {
		*error = GetLastError();
		return 0;
	}
	if (!GetFileInformationByHandleEx(
	            h, FileStandardInfo, &standard, sizeof standard)) {
		*error = GetLastError();
		return 0;
	}
	if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
	        standard.DeletePending || !!standard.Directory != directory ||
	        (!directory &&
	                (1 != standard.NumberOfLinks ||
	                        standard.EndOfFile.QuadPart < 0))) {
		*error = ERROR_INVALID_DATA;
		return 0;
	}
	*size = directory ? 0 : (uint64_t)standard.EndOfFile.QuadPart;
	return 1;
}
static bool read_exact(HANDLE h, void *bytes, DWORD count, DWORD *error)
{
	LARGE_INTEGER zero = {0};
	DWORD got;
	if (!SetFilePointerEx(h, zero, NULL, FILE_BEGIN)) {
		*error = GetLastError();
		return 0;
	}
	if (!ReadFile(h, bytes, count, &got, NULL)) {
		*error = GetLastError();
		return 0;
	}
	if (got != count) {
		*error = ERROR_HANDLE_EOF;
		return 0;
	}
	return 1;
}
static bool write_exact(HANDLE h, const void *bytes, DWORD count, DWORD *error)
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
static void invalidate(WfRecoveryStore *s)
{
	free(s->entries);
	s->entries = NULL;
	s->count = s->capacity = 0;
	s->snapshot_ready = false;
	s->snapshot_error = 0;
}
void wf_recovery_store_close(WfRecoveryStore *s)
{
	if (!s) {
		return;
	}
	invalidate(s);
	if (INVALID_HANDLE_VALUE != s->marker) {
		CloseHandle(s->marker);
	}
	if (INVALID_HANDLE_VALUE != s->directory) {
		CloseHandle(s->directory);
	}
	wf_canonical_free(&s->path);
	free(s);
}
static bool local_root(WfRecoveryStore *s, DWORD *error)
{
	wchar_t volume[ROOT_UNITS + 1], fs[32], *ancestor;
	DWORD attr;
	size_t i, n;
	uint64_t ignored;
	if (!file_identity(s->directory, true, &s->root_id, &ignored, error) ||
	        !wf_canonical_from_handle(s->directory, &s->path, error)) {
		return 0;
	}
	memcpy(volume, s->path.path, ROOT_UNITS * sizeof *volume);
	volume[ROOT_UNITS] = 0;
	if (DRIVE_FIXED != GetDriveTypeW(volume)) {
		*error = ERROR_NOT_SUPPORTED;
		return 0;
	}
	if (!GetVolumeInformationByHandleW(
	            s->directory, NULL, 0, NULL, NULL, NULL, fs, 32)) {
		*error = GetLastError();
		return 0;
	}
	if (wcscmp(fs, L"NTFS") && wcscmp(fs, L"ReFS")) {
		*error = ERROR_NOT_SUPPORTED;
		return 0;
	}
	ancestor = _wcsdup(s->path.path);
	if (!ancestor) {
		*error = ERROR_NOT_ENOUGH_MEMORY;
		return 0;
	}
	n = wcslen(ancestor);
	for (i = ROOT_UNITS; i <= n; i++) {
		wchar_t saved;
		if (ROOT_UNITS != i && i != n && L'\\' != ancestor[i]) {
			continue;
		}
		saved = ancestor[i];
		ancestor[i] = 0;
		attr = GetFileAttributesW(ancestor);
		if (INVALID_FILE_ATTRIBUTES == attr) {
			*error = GetLastError();
			free(ancestor);
			return 0;
		}
		ancestor[i] = saved;
		if (!(attr & FILE_ATTRIBUTE_DIRECTORY) ||
		        (attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
			*error = ERROR_INVALID_DATA;
			free(ancestor);
			return 0;
		}
	}
	free(ancestor);
	return 1;
}
static bool marker_open(WfRecoveryStore *s, bool create, DWORD *error)
{
	WfOwnedRootMarker marker = {0};
	wchar_t *path = path_of(s, L".wfile-recovery-root");
	HANDLE h;
	uint64_t size;
	DWORD saved;
	bool ok = false;
	if (!path) {
		*error = ERROR_NOT_ENOUGH_MEMORY;
		return 0;
	}
	if (create) {
		h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
		        FILE_SHARE_READ, NULL, CREATE_NEW,
		        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
		        NULL);
		saved = INVALID_HANDLE_VALUE == h ? GetLastError() : 0;
		if (INVALID_HANDLE_VALUE != h) {
			memcpy(marker.magic, "WFROOT01", 8);
			marker.root = s->root_id;
			if (BCryptGenRandom(NULL, marker.nonce, 32,
			            BCRYPT_USE_SYSTEM_PREFERRED_RNG)) {
				*error = ERROR_GEN_FAILURE;
			} else if (write_exact(
			                   h, &marker, sizeof marker, error)) {
				if (!FlushFileBuffers(h)) {
					*error = GetLastError();
				} else {
					ok = true;
				}
			}
			CloseHandle(h);
			if (!ok) {
				free(path);
				return 0;
			}
		} else if (ERROR_FILE_EXISTS != saved) {
			*error = saved;
			free(path);
			return 0;
		}
	}
	s->marker = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, FLAGS, NULL);
	saved = INVALID_HANDLE_VALUE == s->marker ? GetLastError() : 0;
	free(path);
	if (INVALID_HANDLE_VALUE == s->marker) {
		*error = saved;
		return 0;
	}
	if (!file_identity(s->marker, false, &s->marker_id, &size, error)) {
		return 0;
	}
	if (size != sizeof marker) {
		*error = ERROR_INVALID_DATA;
		return 0;
	}
	if (!read_exact(s->marker, &marker, sizeof marker, error)) {
		return 0;
	}
	if (memcmp(marker.magic, "WFROOT01", 8) ||
	        !id_equal(&marker.root, &s->root_id)) {
		*error = ERROR_INVALID_DATA;
		return 0;
	}
	memcpy(s->nonce, marker.nonce, 32);
	return 1;
}
bool wf_recovery_store_open(
        const wchar_t *root, bool create, WfRecoveryStore **out, DWORD *error)
{
	WfRecoveryStore *s;
	DWORD local_error, saved;
	if (!error) {
		error = &local_error;
	}
	*error = ERROR_INVALID_PARAMETER;
	if (out) {
		*out = NULL;
	}
	if (!root || !out) {
		return false;
	}
	s = calloc(1, sizeof *s);
	if (!s) {
		*error = ERROR_NOT_ENOUGH_MEMORY;
		return 0;
	}
	s->directory = s->marker = INVALID_HANDLE_VALUE;
	if (create && !CreateDirectoryW(root, NULL)) {
		saved = GetLastError();
		if (ERROR_ALREADY_EXISTS != saved) {
			*error = saved;
			goto fail;
		}
	}
	s->directory =
	        CreateFileW(root, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
	                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	                FLAGS, NULL);
	if (INVALID_HANDLE_VALUE == s->directory) {
		*error = GetLastError();
		goto fail;
	}
	if (!local_root(s, error) || !marker_open(s, create, error)) {
		goto fail;
	}
	*out = s;
	*error = 0;
	return 1;
fail:
	wf_recovery_store_close(s);
	return 0;
}
static bool write_object(WfRecoveryStore *s,
        const wchar_t *leaf,
        const void *header,
        DWORD header_bytes,
        const void *bytes,
        DWORD count,
        FILE_ID_INFO *id,
        DWORD *error)
{
	wchar_t *path = path_of(s, leaf);
	HANDLE h;
	DWORD saved;
	bool ok = false;
	if (!path) {
		*error = ERROR_NOT_ENOUGH_MEMORY;
		return 0;
	}
	h = CreateFileW(path, GENERIC_WRITE | FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	saved = INVALID_HANDLE_VALUE == h ? GetLastError() : 0;
	free(path);
	if (INVALID_HANDLE_VALUE == h) {
		*error = saved;
		return 0;
	}
	if (!write_exact(h, header, header_bytes, error) ||
	        (count && !write_exact(h, bytes, count, error))) {
		goto done;
	}
	if (!FlushFileBuffers(h)) {
		*error = GetLastError();
		goto done;
	}
	if (!GetFileInformationByHandleEx(h, FileIdInfo, id, sizeof *id)) {
		*error = GetLastError();
		goto done;
	}
	ok = true;
done:
	CloseHandle(h);
	return ok;
}
bool wf_recovery_store_put(WfRecoveryStore *s,
        const wchar_t *id,
        const wchar_t *volume,
        const void *journal,
        size_t count,
        DWORD *error)
{
	WfOwnedLocalMeta meta = {0};
	WfOwnedLocalHeader header = {0};
	FILE_ID_INFO ignored, journal_id;
	wchar_t leaf[48];
	DWORD local_error;
	if (!error) {
		error = &local_error;
	}
	*error = ERROR_INVALID_PARAMETER;
	if (!s || !uuid_valid(id) || !volume || !*volume ||
	        wcslen(volume) >= 128 || (count && !journal) ||
	        count > LOCAL_BYTES - sizeof header) {
		return 0;
	}
	invalidate(s);
	memcpy(header.magic, "WFJRNL01", 8);
	memcpy(header.root_nonce, s->nonce, 32);
	if (BCryptGenRandom(NULL, header.object_nonce, 32,
	            BCRYPT_USE_SYSTEM_PREFERRED_RNG) ||
	        BCryptGenRandom(NULL, meta.header.object_nonce, 32,
	                BCRYPT_USE_SYSTEM_PREFERRED_RNG)) {
		*error = ERROR_GEN_FAILURE;
		return 0;
	}
	wcscpy_s(header.recovery_id, 37, id);
	swprintf(leaf, 48, L"%ls.wjr", id);
	if (!write_object(s, leaf, &header, sizeof header, journal,
	            (DWORD)count, &journal_id, error)) {
		return 0;
	}
	memcpy(&meta.journal, &journal_id, sizeof journal_id);
	memcpy(meta.header.magic, "WFMETA01", 8);
	memcpy(meta.header.root_nonce, s->nonce, 32);
	wcscpy_s(meta.header.recovery_id, 37, id);
	wcscpy_s(meta.volume_identity, 128, volume);
	memcpy(meta.journal_nonce, header.object_nonce, 32);
	meta.journal_size = sizeof header + count;
	swprintf(leaf, 48, L"%ls.meta", id);
	if (!write_object(
	            s, leaf, &meta, sizeof meta, NULL, 0, &ignored, error)) {
		return 0;
	}
	*error = 0;
	return 1;
}
static HANDLE open_local(WfRecoveryStore *s,
        const wchar_t *id,
        const wchar_t *extension,
        DWORD *error)
{
	wchar_t leaf[48], *path;
	HANDLE h;
	DWORD saved;
	swprintf(leaf, 48, L"%ls%ls", id, extension);
	path = path_of(s, leaf);
	if (!path) {
		*error = ERROR_NOT_ENOUGH_MEMORY;
		return INVALID_HANDLE_VALUE;
	}
	h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, FLAGS, NULL);
	saved = INVALID_HANDLE_VALUE == h ? GetLastError() : 0;
	free(path);
	*error = saved;
	return h;
}
static bool header_valid(WfRecoveryStore *s,
        const WfOwnedLocalHeader *h,
        const wchar_t *id,
        const char *magic)
{
	return !memcmp(h->magic, magic, 8) &&
	        !memcmp(h->root_nonce, s->nonce, 32) &&
	        !memcmp(h->recovery_id, id, 37 * sizeof(wchar_t));
}
static bool read_meta(WfRecoveryStore *s,
        const wchar_t *id,
        WfOwnedLocalMeta *meta,
        FILE_ID_INFO *object,
        DWORD *error)
{
	HANDLE h = open_local(s, id, L".meta", error);
	uint64_t size;
	bool ok = false;
	if (INVALID_HANDLE_VALUE == h) {
		return 0;
	}
	if (!file_identity(h, false, object, &size, error)) {
		goto done;
	}
	if (size != sizeof *meta) {
		*error = ERROR_INVALID_DATA;
		goto done;
	}
	if (!read_exact(h, meta, sizeof *meta, error)) {
		goto done;
	}
	if (!header_valid(s, &meta->header, id, "WFMETA01") ||
	        !wmemchr(meta->volume_identity, 0, 128) ||
	        meta->journal_size < sizeof(WfOwnedLocalHeader) ||
	        meta->journal_size > LOCAL_BYTES) {
		*error = ERROR_INVALID_DATA;
		goto done;
	}
	ok = true;
	*error = 0;
done:
	CloseHandle(h);
	return ok;
}
static bool read_journal(WfRecoveryStore *s,
        const wchar_t *id,
        WfOwnedLocalHeader *header,
        FILE_ID_INFO *object,
        uint64_t *size,
        DWORD *error)
{
	HANDLE h = open_local(s, id, L".wjr", error);
	bool ok = false;
	if (INVALID_HANDLE_VALUE == h) {
		return 0;
	}
	if (!file_identity(h, false, object, size, error)) {
		goto done;
	}
	if (*size < sizeof *header || *size > LOCAL_BYTES) {
		*error = ERROR_INVALID_DATA;
		goto done;
	}
	if (!read_exact(h, header, sizeof *header, error)) {
		goto done;
	}
	if (!header_valid(s, header, id, "WFJRNL01")) {
		*error = ERROR_INVALID_DATA;
		goto done;
	}
	ok = true;
	*error = 0;
done:
	CloseHandle(h);
	return ok;
}
static LocalEntry *entry_for(
        WfRecoveryStore *s, const wchar_t *id, DWORD *error)
{
	LocalEntry *entries;
	size_t i, capacity;
	for (i = 0; i < s->count; i++) {
		if (!wcscmp(s->entries[i].info.recovery_id, id)) {
			return &s->entries[i];
		}
	}
	if (LOCAL_RECORDS == s->count) {
		*error = ERROR_TOO_MANY_NAMES;
		return NULL;
	}
	if (s->count == s->capacity) {
		capacity = s->capacity ? s->capacity * 2 : 16;
		entries =
		        wf_realloc_array(s->entries, capacity, sizeof *entries);
		if (!entries) {
			*error = ERROR_NOT_ENOUGH_MEMORY;
			return NULL;
		}
		s->entries = entries;
		s->capacity = capacity;
	}
	entries = &s->entries[s->count++];
	memset(entries, 0, sizeof *entries);
	wcscpy_s(entries->info.recovery_id, 37, id);
	return entries;
}
static bool collect_names(WfRecoveryStore *s, DWORD *error)
{
	WIN32_FIND_DATAW data;
	HANDLE find;
	wchar_t *path = path_of(s, L"*");
	bool ok = true;
	if (!path) {
		*error = ERROR_NOT_ENOUGH_MEMORY;
		return 0;
	}
	find = FindFirstFileW(path, &data);
	*error = INVALID_HANDLE_VALUE == find ? GetLastError() : 0;
	free(path);
	if (INVALID_HANDLE_VALUE == find) {
		return ERROR_FILE_NOT_FOUND == *error;
	}
	do {
		size_t n = wcslen(data.cFileName);
		wchar_t id[37];
		LocalEntry *entry;
		if (!wcscmp(data.cFileName, L".") ||
		        !wcscmp(data.cFileName, L"..") ||
		        !wcscmp(data.cFileName, L".wfile-recovery-root") ||
		        !wcscmp(data.cFileName, L"recovery.log")) {
			continue;
		}
		if (40 != n && 41 != n) {
			*error = ERROR_INVALID_DATA;
			ok = false;
			break;
		}
		memcpy(id, data.cFileName, 36 * sizeof *id);
		id[36] = 0;
		if (!uuid_valid(id) ||
		        (wcscmp(data.cFileName + 36, L".meta") &&
		                wcscmp(data.cFileName + 36, L".wjr"))) {
			*error = ERROR_INVALID_DATA;
			ok = false;
			break;
		}
		entry = entry_for(s, id, error);
		if (!entry) {
			ok = false;
			break;
		}
		if (!wcscmp(data.cFileName + 36, L".wjr")) {
			entry->journal_name = true;
		} else {
			entry->metadata_name = true;
		}
	} while (FindNextFileW(find, &data));
	if (ok) {
		*error = GetLastError();
		if (ERROR_NO_MORE_FILES != *error) {
			ok = false;
		} else {
			*error = 0;
		}
	}
	FindClose(find);
	return ok;
}
static bool audit_bytes(HANDLE h, uint8_t **bytes, size_t *length, DWORD *error)
{
	FILE_ID_INFO ignored;
	uint64_t size;
	*bytes = NULL;
	*length = 0;
	if (!file_identity(h, false, &ignored, &size, error)) {
		return 0;
	}
	if (size > AUDIT_BYTES) {
		*error = ERROR_FILE_TOO_LARGE;
		return 0;
	}
	if (!size) {
		return 1;
	}
	*bytes = malloc((size_t)size);
	if (!*bytes) {
		*error = ERROR_NOT_ENOUGH_MEMORY;
		return 0;
	}
	if (!read_exact(h, *bytes, (DWORD)size, error)) {
		free(*bytes);
		*bytes = NULL;
		return 0;
	}
	*length = (size_t)size;
	return 1;
}
static bool parse_audit(
        WfRecoveryStore *s, const uint8_t *bytes, size_t length, DWORD *error)
{
	static const char intent[] = "RECOVERY_ABANDON_INTENT ";
	static const char complete[] = "RECOVERY_ABANDONED ";
	size_t position = 0;
	while (position < length) {
		const uint8_t *line = bytes + position,
		              *end = memchr(line, '\n', length - position);
		size_t line_bytes, prefix, i;
		bool completion;
		wchar_t id[37];
		LocalEntry *entry;
		if (!end) {
			*error = ERROR_INVALID_DATA;
			return 0;
		}
		line_bytes = (size_t)(end - line) + 1;
		if (line_bytes == sizeof intent - 1 + 37 &&
		        !memcmp(line, intent, sizeof intent - 1)) {
			prefix = sizeof intent - 1;
			completion = false;
		} else if (line_bytes == sizeof complete - 1 + 37 &&
		        !memcmp(line, complete, sizeof complete - 1)) {
			prefix = sizeof complete - 1;
			completion = true;
		} else {
			*error = ERROR_INVALID_DATA;
			return 0;
		}
		for (i = 0; i < 36; i++) {
			id[i] = (wchar_t)line[prefix + i];
		}
		id[36] = 0;
		if (!uuid_valid(id)) {
			*error = ERROR_INVALID_DATA;
			return 0;
		}
		entry = entry_for(s, id, error);
		if (!entry) {
			return 0;
		}
		if (completion) {
			if (!entry->intent) {
				entry->inconsistent = true;
			}
			entry->complete = true;
		} else {
			if (entry->complete) {
				entry->inconsistent = true;
			}
			entry->intent = true;
		}
		position += line_bytes;
	}
	return 1;
}
static bool collect_audit(WfRecoveryStore *s, DWORD *error)
{
	wchar_t *path = path_of(s, L"recovery.log");
	HANDLE h;
	DWORD saved;
	uint8_t *bytes = NULL;
	size_t length;
	bool ok;
	if (!path) {
		*error = ERROR_NOT_ENOUGH_MEMORY;
		return 0;
	}
	h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, FLAGS, NULL);
	saved = INVALID_HANDLE_VALUE == h ? GetLastError() : 0;
	free(path);
	if (INVALID_HANDLE_VALUE == h) {
		*error = ERROR_FILE_NOT_FOUND == saved ? 0 : saved;
		return ERROR_FILE_NOT_FOUND == saved;
	}
	ok = audit_bytes(h, &bytes, &length, error) &&
	        parse_audit(s, bytes, length, error);
	free(bytes);
	CloseHandle(h);
	return ok;
}
/* What reading the two local objects of one recovery ID showed. Pure data:
 * the classification below never touches the disk. */
typedef struct
{
	WfOwnedLocalMeta meta;
	WfOwnedLocalHeader journal;
	FILE_ID_INFO metadata_id, journal_id;
	uint64_t journal_size;
	DWORD meta_error, journal_error;
	bool has_meta, has_journal;
} LocalObservation;

static void observe_local(
        WfRecoveryStore *s, const wchar_t *id, LocalObservation *obs)
{
	memset(obs, 0, sizeof *obs);
	obs->has_meta = read_meta(
	        s, id, &obs->meta, &obs->metadata_id, &obs->meta_error);
	obs->has_journal = read_journal(s, id, &obs->journal, &obs->journal_id,
	        &obs->journal_size, &obs->journal_error);
}

/* Decides the state and the permitted action from what was observed. Anything
 * unreadable, inconsistent or unexpected is HOLD: preserved, never repaired. */
static void classify_observation(LocalEntry *entry, const LocalObservation *obs)
{
	const WfOwnedLocalMeta meta = obs->meta;
	const WfOwnedLocalHeader journal = obs->journal;
	const FILE_ID_INFO journal_id = obs->journal_id;
	FILE_ID_INFO expected_journal;
	const uint64_t journal_size = obs->journal_size;
	const DWORD meta_error = obs->meta_error;
	const DWORD journal_error = obs->journal_error;
	const bool has_meta = obs->has_meta;
	const bool has_journal = obs->has_journal;
	WfRecoveryLocalInfo *info = &entry->info;
	wcscpy_s(info->volume_identity, 128,
	        L"(recorded volume unavailable: metadata incomplete)");
	info->state = WF_RECOVERY_LOCAL_MANUAL;
	info->action = WF_RECOVERY_ABANDON_HOLD;
	info->reason = "unknown, changed or incomplete local metadata; "
	               "preserve objects";
	if ((meta_error && ERROR_FILE_NOT_FOUND != meta_error) ||
	        (journal_error && ERROR_FILE_NOT_FOUND != journal_error)) {
		info->error = meta_error && ERROR_FILE_NOT_FOUND != meta_error
		        ? meta_error
		        : journal_error;
		return;
	}
	if (has_meta) {
		wcscpy_s(info->volume_identity, 128, meta.volume_identity);
	}
	if (entry->inconsistent ||
	        (entry->complete && (has_meta || has_journal))) {
		info->error = ERROR_INVALID_DATA;
		return;
	}
	if (has_meta && has_journal) {
		memcpy(&expected_journal, &meta.journal,
		        sizeof expected_journal);
		if (!id_equal(&expected_journal, &journal_id) ||
		        meta.journal_size != journal_size ||
		        memcmp(meta.journal_nonce, journal.object_nonce, 32)) {
			info->error = ERROR_INVALID_DATA;
			return;
		}
		info->state = entry->intent ? WF_RECOVERY_LOCAL_ABANDON_INTENT
		                            : WF_RECOVERY_LOCAL_READY;
		info->action = WF_RECOVERY_ABANDON_PAIR;
		info->reason = entry->intent
		        ? "abandon intent recorded; complete pair may be "
		          "reverified after exact ID"
		        : "complete local pair; exact ID required for "
		          "abandonment";
	} else if (has_journal) {
		info->state = entry->intent
		        ? WF_RECOVERY_LOCAL_ABANDON_METADATA_DELETED
		        : WF_RECOVERY_LOCAL_JOURNAL_ONLY;
		info->reason = "journal has no independent metadata FileId "
		               "proof; preserve after exact ID too";
	} else if (has_meta) {
		info->state = entry->intent
		        ? WF_RECOVERY_LOCAL_ABANDON_JOURNAL_DELETED
		        : WF_RECOVERY_LOCAL_METADATA_ONLY;
		info->reason = "journal missing; incomplete ownership chain is "
		               "retained for manual inspection";
	} else if (entry->complete) {
		info->state = WF_RECOVERY_LOCAL_ABANDONED;
		info->action = WF_RECOVERY_ABANDON_DONE;
		info->acknowledged = true;
		info->reason =
		        "local objects absent and completion audit recorded";
	} else if (entry->intent) {
		info->state = WF_RECOVERY_LOCAL_ABANDON_AUDIT_PENDING;
		info->action = WF_RECOVERY_ABANDON_AUDIT_ONLY;
		info->reason = "both local objects absent; exact ID may append "
		               "completion audit only";
	} else {
		info->state = WF_RECOVERY_LOCAL_ABSENT;
		info->reason = "local objects disappeared during inspection; "
		               "refresh required";
	}
}

static void classify(WfRecoveryStore *s, LocalEntry *entry)
{
	LocalObservation obs;

	observe_local(s, entry->info.recovery_id, &obs);
	classify_observation(entry, &obs);
}
const char *wf_recovery_local_state_name(WfRecoveryLocalState state)
{
	switch (state) {
	case WF_RECOVERY_LOCAL_ABSENT:         return "ABSENT";
	case WF_RECOVERY_LOCAL_READY:          return "READY";
	case WF_RECOVERY_LOCAL_JOURNAL_ONLY:   return "JOURNAL_ONLY";
	case WF_RECOVERY_LOCAL_METADATA_ONLY:  return "METADATA_ONLY";
	case WF_RECOVERY_LOCAL_ABANDON_INTENT: return "ABANDON_INTENT";
	case WF_RECOVERY_LOCAL_ABANDON_JOURNAL_DELETED:
		return "ABANDON_JOURNAL_DELETED";
	case WF_RECOVERY_LOCAL_ABANDON_METADATA_DELETED:
		return "ABANDON_METADATA_DELETED";
	case WF_RECOVERY_LOCAL_ABANDON_AUDIT_PENDING:
		return "ABANDON_AUDIT_PENDING";
	case WF_RECOVERY_LOCAL_ABANDONED: return "ABANDONED";
	case WF_RECOVERY_LOCAL_MANUAL:    return "MANUAL";
	}
	return "MANUAL";
}
const char *wf_recovery_abandon_action_name(WfRecoveryAbandonAction action)
{
	switch (action) {
	case WF_RECOVERY_ABANDON_HOLD:       return "HOLD";
	case WF_RECOVERY_ABANDON_PAIR:       return "REVERIFY_PAIR";
	case WF_RECOVERY_ABANDON_AUDIT_ONLY: return "AUDIT_ONLY";
	case WF_RECOVERY_ABANDON_DONE:       return "DONE";
	}
	return "HOLD";
}
static bool snapshot(WfRecoveryStore *s, DWORD *error)
{
	size_t i;
	if (!s->snapshot_ready) {
		if (!collect_names(s, error) || !collect_audit(s, error)) {
			DWORD saved = *error;
			invalidate(s);
			s->snapshot_error = saved;
		} else {
			for (i = 0; i < s->count; i++) {
				classify(s, &s->entries[i]);
			}
		}
		s->snapshot_ready = true;
	}
	*error = s->snapshot_error;
	return !*error;
}
bool wf_recovery_store_refresh(WfRecoveryStore *s, DWORD *error)
{
	DWORD local_error;
	if (!error) {
		error = &local_error;
	}
	if (!s) {
		*error = ERROR_INVALID_PARAMETER;
		return 0;
	}
	invalidate(s);
	return snapshot(s, error);
}
WfRecoveryLookup wf_recovery_store_inspect(WfRecoveryStore *s,
        const wchar_t *id,
        WfRecoveryLocalInfo *info,
        DWORD *error)
{
	DWORD local_error;
	size_t i;
	if (!error) {
		error = &local_error;
	}
	*error = ERROR_INVALID_PARAMETER;
	if (!s || !uuid_valid(id) || !info) {
		return WF_RECOVERY_LOOKUP_ERROR;
	}
	memset(info, 0, sizeof *info);
	if (!snapshot(s, error)) {
		info->state = WF_RECOVERY_LOCAL_MANUAL;
		info->error = *error;
		info->reason = "unknown or invalid store/audit content";
		return WF_RECOVERY_LOOKUP_ERROR;
	}
	for (i = 0; i < s->count; i++) {
		if (!wcscmp(s->entries[i].info.recovery_id, id)) {
			*info = s->entries[i].info;
			return WF_RECOVERY_LOOKUP_FOUND;
		}
	}
	*error = 0;
	info->reason = "recovery ID not present in local state";
	return WF_RECOVERY_LOOKUP_NONE;
}
WfRecoveryLookup wf_recovery_store_get(WfRecoveryStore *s,
        size_t index,
        WfRecoveryLocalInfo *info,
        DWORD *error)
{
	DWORD local_error;
	size_t i, found = 0;
	if (!error) {
		error = &local_error;
	}
	*error = ERROR_INVALID_PARAMETER;
	if (!s || !info) {
		return WF_RECOVERY_LOOKUP_ERROR;
	}
	memset(info, 0, sizeof *info);
	if (!snapshot(s, error)) {
		info->state = WF_RECOVERY_LOCAL_MANUAL;
		info->error = *error;
		info->reason = "unknown or invalid store/audit content";
		return WF_RECOVERY_LOOKUP_ERROR;
	}
	for (i = 0; i < s->count; i++) {
		if (WF_RECOVERY_LOCAL_ABANDONED == s->entries[i].info.state) {
			continue;
		}
		if (found++ == index) {
			*info = s->entries[i].info;
			return WF_RECOVERY_LOOKUP_FOUND;
		}
	}
	*error = 0;
	return WF_RECOVERY_LOOKUP_NONE;
}
static bool log_event(
        WfRecoveryStore *s, const wchar_t *id, const char *event, DWORD *error)
{
	wchar_t *path = path_of(s, L"recovery.log");
	HANDLE h;
	DWORD saved, count;
	uint8_t *existing = NULL;
	size_t length = 0, prefix = strlen(event), i;
	WfRecoveryStore check = {0};
	char line[128];
	bool ok = false;
	if (!path) {
		*error = ERROR_NOT_ENOUGH_MEMORY;
		return 0;
	}
	if (prefix > sizeof line - 38) {
		*error = ERROR_INVALID_PARAMETER;
		free(path);
		return 0;
	}
	h = CreateFileW(path,
	        GENERIC_READ | FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ, NULL, OPEN_ALWAYS,
	        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	saved = INVALID_HANDLE_VALUE == h ? GetLastError() : 0;
	free(path);
	if (INVALID_HANDLE_VALUE == h) {
		*error = saved;
		return 0;
	}
	if (!audit_bytes(h, &existing, &length, error) ||
	        !parse_audit(&check, existing, length, error)) {
		goto done;
	}
	memcpy(line, event, prefix);
	line[prefix++] = ' ';
	for (i = 0; i < 36; i++) {
		line[prefix + i] = (char)id[i];
	}
	line[prefix + 36] = '\n';
	count = (DWORD)(prefix + 37);
	if (count > AUDIT_BYTES - length) {
		*error = ERROR_FILE_TOO_LARGE;
		goto done;
	}
	if (!write_exact(h, line, count, error)) {
		goto done;
	}
	if (!FlushFileBuffers(h)) {
		*error = GetLastError();
		goto done;
	}
	ok = true;
	*error = 0;
done:
	free(check.entries);
	free(existing);
	CloseHandle(h);
	return ok;
}
static bool abandon_pair(
        WfRecoveryStore *s, const wchar_t *id, WfRecoveryAbandonReport *report)
{
	WfOwnedLocalMeta meta;
	FILE_ID_INFO metadata;
	WfOwnedLocalRecord record = {0};
	WfOwnedReport proof;
	WfVerifiedOwned *journal = NULL, *ack = NULL;
	bool ok = false;
	if (!read_meta(s, id, &meta, &metadata, &report->error)) {
		return 0;
	}
	record.root_path = s->path.path;
	record.root = s->root_id;
	record.marker = s->marker_id;
	memcpy(record.root_nonce, s->nonce, 32);
	wcscpy_s(record.recovery_id, 37, id);
	record.object = meta.journal;
	record.object_size = meta.journal_size;
	memcpy(record.object_nonce, meta.journal_nonce, 32);
	if (wf_owned_verify_local(WF_OWNED_JOURNAL, &record, &journal,
	            &proof) != WF_OWNED_VERIFIED) {
		report->error = proof.error;
		goto done;
	}
	record.object = metadata;
	record.object_size = sizeof meta;
	memcpy(record.object_nonce, meta.header.object_nonce, 32);
	if (wf_owned_verify_local(WF_OWNED_JOURNAL_META, &record, &ack,
	            &proof) != WF_OWNED_VERIFIED) {
		report->error = proof.error;
		goto done;
	}
	if (!log_event(s, id, "RECOVERY_ABANDON_INTENT", &report->error)) {
		goto done;
	}
	report->intent_logged = 1;
	if (WF_OWNED_DELETED != wf_owned_delete(journal, &proof)) {
		report->error = proof.error;
		goto done;
	}
	report->journal_deleted = 1;
	if (WF_OWNED_DELETED != wf_owned_delete(ack, &proof)) {
		report->error = proof.error;
		goto done;
	}
	report->metadata_deleted = 1;
	if (!log_event(s, id, "RECOVERY_ABANDONED", &report->error)) {
		goto done;
	}
	report->logged = 1;
	report->error = 0;
	ok = true;
done:
	wf_owned_release(ack);
	wf_owned_release(journal);
	return ok;
}
bool wf_recovery_store_abandon(WfRecoveryStore *s,
        const wchar_t *id,
        const wchar_t *typed,
        WfRecoveryAbandonReport *report)
{
	WfRecoveryAbandonReport local = {0};
	WfRecoveryLocalInfo info, after;
	DWORD inspection_error;
	bool ok = false;
	WfRecoveryLookup found;
	if (!report) {
		report = &local;
	}
	memset(report, 0, sizeof *report);
	report->error = ERROR_INVALID_PARAMETER;
	if (!s || !uuid_valid(id) || !typed || wcscmp(id, typed)) {
		if (s && uuid_valid(id) && typed) {
			report->error = ERROR_CANCELLED;
		}
		return 0;
	}
	if (!wf_recovery_store_refresh(s, &report->error)) {
		return 0;
	}
	found = wf_recovery_store_inspect(s, id, &info, &report->error);
	if (WF_RECOVERY_LOOKUP_FOUND != found) {
		if (WF_RECOVERY_LOOKUP_NONE == found) {
			report->error = ERROR_NOT_FOUND;
		}
		return false;
	}
	report->state_before = info.state;
	report->state_after = info.state;
	report->action = info.action;
	if (WF_RECOVERY_ABANDON_PAIR == info.action) {
		ok = abandon_pair(s, id, report);
	} else if (WF_RECOVERY_ABANDON_AUDIT_ONLY == info.action) {
		ok = log_event(s, id, "RECOVERY_ABANDONED", &report->error);
		if (ok) {
			report->intent_logged = 1;
			report->logged = 1;
		}
	} else if (WF_RECOVERY_ABANDON_DONE == info.action) {
		report->logged = 1;
		report->error = 0;
		ok = true;
	} else {
		report->error = info.error ? info.error : ERROR_INVALID_DATA;
	}
	invalidate(s);
	if (WF_RECOVERY_LOOKUP_FOUND ==
	        wf_recovery_store_inspect(s, id, &after, &inspection_error)) {
		report->state_after = after.state;
	}
	return ok;
}
