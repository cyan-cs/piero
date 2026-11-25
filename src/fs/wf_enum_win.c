#include <windows.h>

#include "wf_enum_win.h"

#include <stdlib.h>
#include <string.h>

#include "../path/wf_path.h"
#include "../base/wf_mem.h"
#include "../base/wf_win32status.h"
#include "wf_weakid_win.h"

static WfStatus map_error(DWORD err)
{
	return wf_status_from_win32(err);
}

static uint32_t entry_flags(DWORD attrs)
{
	uint32_t f = 0;

	if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
		f |= WF_ENT_DIR;
	}
	if (attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) {
		f |= WF_ENT_HIDDEN;
	}
	if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
		f |= WF_ENT_LINK;
	}
	if (attrs &
	        (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS |
	                FILE_ATTRIBUTE_RECALL_ON_OPEN |
	                FILE_ATTRIBUTE_OFFLINE)) {
		f |= WF_ENT_CLOUD;
	}
	return f;
}

static bool is_dot_entry(const wchar_t *n)
{
	return L'.' == n[0] &&
	        (L'\0' == n[1] || (L'.' == n[1] && L'\0' == n[2]));
}

/* "<extended path>\*" */
static wchar_t *search_pattern(const wchar_t *path)
{
	wchar_t *ext = NULL, *pat;
	size_t n;
	int sep;

	if (WF_OK != wf_path_to_extended(path, &ext)) {
		return NULL;
	}
	n = wcslen(ext);
	sep = n > 0 && L'\\' != ext[n - 1];
	pat = wf_realloc_array(ext, (n + 3), sizeof *pat);
	if (!pat) {
		free(ext);
		return NULL;
	}
	if (sep) {
		pat[n++] = L'\\';
	}
	pat[n++] = L'*';
	pat[n] = L'\0';
	return pat;
}

typedef struct
{
	WfSortKey key;
	bool descending, dirs_first;
} Order;

static const Order BY_NAME = {WF_SORT_NAME, false, true};

/* Compatibility browsing only: these path-based results carry no identity. */
static WfStatus enum_names(const wchar_t *path,
        size_t limit,
        size_t bytes,
        bool names_only,
        Order order,
        bool (*cancel)(void *),
        void *ctx,
        WfDir **out)
{
	WIN32_FIND_DATAW fd;
	wchar_t *pat = search_pattern(path);
	HANDLE h;
	WfDir *d = NULL;
	WfStatus st;
	DWORD err;
	size_t before;

	if (!pat) {
		return WF_E_INVAL;
	}

	h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch,
	        NULL, FIND_FIRST_EX_LARGE_FETCH);
	err = GetLastError(); /* read before free() can disturb it */
	free(pat);
	if (INVALID_HANDLE_VALUE == h) {
		return map_error(err);
	}

	d = wf_dir_new();
	if (!d) {
		FindClose(h);
		return WF_E_NOMEM;
	}
	d->limit = limit;
	d->byte_limit = bytes;
	d->no_display = names_only;

	do {
		if (cancel && !(d->n % 64) && cancel(ctx)) {
			st = WF_E_CANCELLED;
			goto fail;
		}
		if (is_dot_entry(fd.cFileName)) {
			continue;
		}
		/* dwReserved0 holds the reparse tag when the attribute is set
		 */
		before = d->n;
		st = wf_dir_add_ex(d, fd.cFileName, wcslen(fd.cFileName),
		        entry_flags(fd.dwFileAttributes),
		        ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow,
		        ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) |
		                fd.ftLastWriteTime.dwLowDateTime,
		        (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
		                ? fd.dwReserved0
		                : 0);
		if (WF_OK != st) {
			goto fail;
		}
		if (d->n > before) {
			wf_dir_set_meta(d, before,
			        ((uint64_t)fd.ftCreationTime.dwHighDateTime
			                << 32) |
			                fd.ftCreationTime.dwLowDateTime,
			        fd.dwFileAttributes);
		}
	} while (!d->truncated && FindNextFileW(h, &fd));

	err = GetLastError();
	if (!d->truncated && ERROR_NO_MORE_FILES != err) {
		st = map_error(err);
		goto fail;
	}
	FindClose(h);

	st = wf_dir_sort_with(d, order.key, order.descending, order.dirs_first);
	if (WF_OK != st) {
		wf_dir_free(d);
		return st;
	}
	*out = d;
	return WF_OK;
fail:
	FindClose(h);
	wf_dir_free(d);
	return st;
}

static bool unsupported_info(DWORD err)
{
	return ERROR_INVALID_PARAMETER == err || ERROR_NOT_SUPPORTED == err ||
	        ERROR_INVALID_LEVEL == err || ERROR_INVALID_FUNCTION == err ||
	        ERROR_CALL_NOT_IMPLEMENTED == err;
}

static bool nonzero_id(const BYTE *id)
{
	size_t i;
	for (i = 0; i < 16; i++) {
		if (id[i]) {
			return 1;
		}
	}
	return 0;
}

/* A GUID volume name excludes redirectors whose volume/128-bit ID contract
 * is not established here. Both this proof and FileIdInfo use the retained
 * enumeration handle; reopening a path would sever that relationship. */
static void directory_identity(HANDLE h, WfFileId *out)
{
	FILE_ID_INFO id;
	wchar_t *final;
	DWORD need, got;

	if (!GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof id) ||
	        !nonzero_id(id.FileId.Identifier)) {
		return;
	}
	need = GetFinalPathNameByHandleW(h, NULL, 0, VOLUME_NAME_GUID);
	if (!need || need > 32768) {
		return;
	}
	final = wf_malloc_array((size_t)need, sizeof *final);
	if (!final) {
		return;
	}
	got = GetFinalPathNameByHandleW(h, final, need, VOLUME_NAME_GUID);
	if (got && got < need && 0 == wcsncmp(final, L"\\\\?\\Volume{", 11)) {
		out->volume = id.VolumeSerialNumber;
		memcpy(out->id, id.FileId.Identifier, sizeof out->id);
		out->kind = WF_ID_STRONG;
	}
	free(final);
}

/* FileIdExtdDirectoryInfo supplies the name, attributes and full 128-bit ID
 * together. No per-entry path lookup may manufacture this association. */
enum
{
	ENUM_BUFFER_BYTES = 65536
};

/* The record at `offset` in a buffer the OS filled, after proving it lies
 * wholly inside it: header, name, and (when another record follows) an
 * aligned next offset that stays in range. */
static bool record_at(BYTE *buffer, size_t offset, FILE_ID_EXTD_DIR_INFO **out)
{
	FILE_ID_EXTD_DIR_INFO *info;
	size_t bytes;

	if (ENUM_BUFFER_BYTES - offset <
	        offsetof(FILE_ID_EXTD_DIR_INFO, FileName)) {
		return false;
	}
	info = (FILE_ID_EXTD_DIR_INFO *)(void *)(buffer +
	        offset); /* offset: sum of NextEntryOffset, each checked % 8;
		            malloc is 16-aligned */
	bytes = offsetof(FILE_ID_EXTD_DIR_INFO, FileName) +
	        (size_t)info->FileNameLength;
	if (!info->FileNameLength || info->FileNameLength % sizeof(wchar_t) ||
	        bytes > ENUM_BUFFER_BYTES - offset ||
	        (info->NextEntryOffset &&
	                (info->NextEntryOffset < bytes ||
	                        info->NextEntryOffset % 8 ||
	                        info->NextEntryOffset >=
	                                ENUM_BUFFER_BYTES - offset))) {
		return false;
	}
	*out = info;
	return true;
}

/* Adds one validated record to the listing, with its metadata and (when the
 * directory handle's identity is proven) its own identity. Dot entries are
 * skipped. */
static WfStatus add_record(WfDir *d, const FILE_ID_EXTD_DIR_INFO *info)
{
	size_t count = info->FileNameLength / sizeof(wchar_t), before;
	WfStatus st;

	if (L'.' == info->FileName[0] &&
	        (1 == count || (2 == count && L'.' == info->FileName[1]))) {
		return WF_OK;
	}
	before = d->n;
	st = wf_dir_add_ex(d, info->FileName, count,
	        entry_flags(info->FileAttributes),
	        (uint64_t)info->EndOfFile.QuadPart,
	        (uint64_t)info->LastWriteTime.QuadPart,
	        (info->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
	                ? info->ReparsePointTag
	                : 0);
	if (WF_OK != st || d->truncated || d->n == before) {
		return st;
	}
	wf_dir_set_meta(d, before, (uint64_t)info->CreationTime.QuadPart,
	        info->FileAttributes);
	if ((d->identity.kind == WF_ID_STRONG) &&
	        nonzero_id(info->FileId.Identifier)) {
		WfFileId *id = &d->e[before].identity;

		id->volume = d->identity.volume;
		memcpy(id->id, info->FileId.Identifier, sizeof id->id);
		id->kind = WF_ID_STRONG;
	}
	return WF_OK;
}

/* The same listing for a FAT volume: FileIdBothDirectoryInfo has the name, the
 * attributes and the 64-bit entry index together, and the volume serial comes
 * from the directory's own handle. The identities are weak (wf_weakid_win.h);
 * `valid` stays false everywhere. */
static bool weak_record_at(
        BYTE *buffer, size_t offset, FILE_ID_BOTH_DIR_INFO **out)
{
	FILE_ID_BOTH_DIR_INFO *info;
	size_t bytes;

	if (ENUM_BUFFER_BYTES - offset <
	        offsetof(FILE_ID_BOTH_DIR_INFO, FileName)) {
		return false;
	}
	info = (FILE_ID_BOTH_DIR_INFO *)(void *)(buffer + offset);
	bytes = offsetof(FILE_ID_BOTH_DIR_INFO, FileName) +
	        (size_t)info->FileNameLength;
	if (!info->FileNameLength || info->FileNameLength % sizeof(wchar_t) ||
	        bytes > ENUM_BUFFER_BYTES - offset ||
	        (info->NextEntryOffset &&
	                (info->NextEntryOffset < bytes ||
	                        info->NextEntryOffset % 8 ||
	                        info->NextEntryOffset >=
	                                ENUM_BUFFER_BYTES - offset))) {
		return false;
	}
	*out = info;
	return true;
}

static WfStatus add_weak_record(WfDir *d, const FILE_ID_BOTH_DIR_INFO *info)
{
	size_t count = info->FileNameLength / sizeof(wchar_t), before;
	WfStatus st;

	if (L'.' == info->FileName[0] &&
	        (1 == count || (2 == count && L'.' == info->FileName[1]))) {
		return WF_OK;
	}
	before = d->n;
	st = wf_dir_add_ex(d, info->FileName, count,
	        entry_flags(info->FileAttributes),
	        (uint64_t)info->EndOfFile.QuadPart,
	        (uint64_t)info->LastWriteTime.QuadPart,
	        (info->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
	                ? info->EaSize /* the reparse tag, for a reparse point */
	                : 0);
	if (WF_OK != st || d->truncated || d->n == before) {
		return st;
	}
	wf_dir_set_meta(d, before, (uint64_t)info->CreationTime.QuadPart,
	        info->FileAttributes);
	if (info->FileId.QuadPart) {
		wf_weak_fill(&d->e[before].identity, d->identity.volume,
		        (uint64_t)info->FileId.QuadPart);
	}
	return WF_OK;
}

static WfStatus enum_weak(
        HANDLE h, WfDir *d, int *unsupported, bool (*cancel)(void *), void *ctx)
{
	BYTE *buffer = malloc(ENUM_BUFFER_BYTES);
	WfStatus st = WF_OK;
	bool first = true;

	if (!buffer) {
		return WF_E_NOMEM;
	}
	for (;;) {
		size_t offset = 0;

		if (cancel && cancel(ctx)) {
			st = WF_E_CANCELLED;
			break;
		}
		if (!GetFileInformationByHandleEx(h,
		            first ? FileIdBothDirectoryRestartInfo
		                  : FileIdBothDirectoryInfo,
		            buffer, ENUM_BUFFER_BYTES)) {
			DWORD err = GetLastError();

			if (ERROR_NO_MORE_FILES != err) {
				*unsupported = unsupported_info(err);
				st = map_error(err);
			}
			break;
		}
		first = false;
		for (;;) {
			FILE_ID_BOTH_DIR_INFO *info;

			if (!weak_record_at(buffer, offset, &info)) {
				st = WF_E_OS;
				goto done;
			}
			st = add_weak_record(d, info);
			if (WF_OK != st || d->truncated) {
				goto done;
			}
			if (!info->NextEntryOffset) {
				break;
			}
			offset += info->NextEntryOffset;
		}
	}
done:
	free(buffer);
	return st;
}

static WfStatus enum_handle(
        HANDLE h, WfDir *d, int *unsupported, bool (*cancel)(void *), void *ctx)
{
	BYTE *buffer = malloc(ENUM_BUFFER_BYTES);
	WfStatus st = WF_OK;

	if (!buffer) {
		return WF_E_NOMEM;
	}
	for (;;) {
		size_t offset = 0;
		if (cancel && cancel(ctx)) {
			st = WF_E_CANCELLED;
			break;
		}

		if (!GetFileInformationByHandleEx(h, FileIdExtdDirectoryInfo,
		            buffer, ENUM_BUFFER_BYTES)) {
			DWORD err = GetLastError();

			if (ERROR_NO_MORE_FILES != err) {
				*unsupported = unsupported_info(err);
				st = map_error(err);
			}
			break;
		}
		for (;;) {
			FILE_ID_EXTD_DIR_INFO *info;

			if (!record_at(buffer, offset, &info)) {
				st = WF_E_OS;
				goto done;
			}
			st = add_record(d, info);
			if (WF_OK != st || d->truncated) {
				goto done;
			}
			if (!info->NextEntryOffset) {
				break;
			}
			offset += info->NextEntryOffset;
		}
	}
done:
	free(buffer);
	return st;
}

WfStatus wf_enum_dir_win(const wchar_t *path, WfDir **out)
{
	return wf_enum_dir_win_limited(path, 0, out);
}

WfStatus wf_enum_dir_win_limited(const wchar_t *path, size_t limit, WfDir **out)
{
	return wf_enum_dir_win_budget(path, limit, 0, out);
}

WfStatus wf_enum_dir_win_budget(
        const wchar_t *path, size_t limit, size_t bytes, WfDir **out)
{
	return wf_enum_dir_win_controlled(path, limit, bytes, NULL, NULL, out);
}

/* FAT and exFAT volumes reject FileAttributeTagInfo with an invalid-parameter
 * error, so the handle's attributes are read the older way there. Any other
 * failure stays a failure: the caller must not treat the directory as plain. */
static bool handle_attributes(HANDLE h, FILE_ATTRIBUTE_TAG_INFO *attr)
{
	BY_HANDLE_FILE_INFORMATION info;

	if (GetFileInformationByHandleEx(
	            h, FileAttributeTagInfo, attr, sizeof *attr)) {
		return true;
	}
	if (!unsupported_info(GetLastError()) ||
	        !GetFileInformationByHandle(h, &info)) {
		return false;
	}
	attr->FileAttributes = info.dwFileAttributes;
	attr->ReparseTag = 0;
	return true;
}

static WfStatus enum_dir(const wchar_t *path,
        size_t limit,
        size_t bytes,
        bool names_only,
        Order order,
        bool (*cancel)(void *),
        void *ctx,
        WfDir **out)
{
	wchar_t *ext = NULL;
	HANDLE h;
	FILE_ATTRIBUTE_TAG_INFO attr;
	WfDir *d;
	WfStatus st;
	int unsupported = 0;
	DWORD err;

	if (!path || !out) {
		return WF_E_INVAL;
	}
	*out = NULL;
	st = wf_path_to_extended(path, &ext);
	if (WF_OK != st) {
		return st;
	}
	/* Keep the listed directory from being renamed/replaced while we read
	 * its identity and records. Opening the leaf itself detects a reparse.
	 */
	h = CreateFileW(ext, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	err = GetLastError();
	free(ext);
	if (INVALID_HANDLE_VALUE == h) {
		return map_error(err);
	}
	if (!handle_attributes(h, &attr) ||
	        (attr.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
		st = bytes ? WF_E_ACCESS
		           : enum_names(path, limit, bytes, names_only, order,
		                     cancel, ctx, out);
		CloseHandle(h);
		return st;
	}
	d = wf_dir_new();
	if (!d) {
		CloseHandle(h);
		return WF_E_NOMEM;
	}
	d->limit = limit;
	d->byte_limit = bytes;
	d->no_display = names_only;
	directory_identity(h, &d->identity);
	st = enum_handle(h, d, &unsupported, cancel, ctx);
	if (unsupported && !(d->identity.kind == WF_ID_STRONG) && 0 == d->n &&
	        wf_weak_of_handle(h, &d->identity)) {
		/* FAT: the entries carry a weak identity */
		unsupported = 0;
		st = enum_weak(h, d, &unsupported, cancel, ctx);
		if (unsupported) {
			memset(&d->identity, 0, sizeof d->identity);
		}
	}
	if (unsupported) {
		wf_dir_free(d);
		st = enum_names(path, limit, bytes, names_only, order, cancel,
		        ctx, out);
		CloseHandle(h);
		return st;
	}
	CloseHandle(h);
	if (WF_OK == st) {
		st = wf_dir_sort_with(
		        d, order.key, order.descending, order.dirs_first);
	}
	if (WF_OK != st) {
		wf_dir_free(d);
		return st;
	}
	*out = d;
	return WF_OK;
}

WfStatus wf_enum_dir_win_controlled(const wchar_t *path,
        size_t limit,
        size_t bytes,
        bool (*cancel)(void *),
        void *ctx,
        WfDir **out)
{
	return enum_dir(path, limit, bytes, false, BY_NAME, cancel, ctx, out);
}

WfStatus wf_enum_dir_win_ordered(const wchar_t *path,
        size_t limit,
        size_t bytes,
        WfSortKey key,
        bool descending,
        bool dirs_first,
        bool (*cancel)(void *),
        void *ctx,
        WfDir **out)
{
	Order order = {key, descending, dirs_first};

	return enum_dir(path, limit, bytes, false, order, cancel, ctx, out);
}

WfStatus wf_enum_names_win(
        const wchar_t *path, bool (*cancel)(void *), void *ctx, WfDir **out)
{
	return enum_dir(path, 0, 0, true, BY_NAME, cancel, ctx, out);
}
