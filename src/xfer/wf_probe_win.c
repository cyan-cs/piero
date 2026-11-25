#include <windows.h>

#include "wf_probe_win.h"

#include <stdlib.h>
#include <string.h>

#include "../fs/wf_enum_win.h"
#include "../fs/wf_weakid_win.h"
#include "../path/wf_path.h"
#include "../base/wf_macro.h"
#include "../base/wf_mem.h"
#include "../base/wf_win32status.h"

static WfStatus map_error(DWORD err)
{
	return wf_status_from_win32(err);
}

static uint64_t ft_u64(FILETIME ft)
{
	return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

/* Opens `path` (absolute, any form) in extended form. Caller closes. */
static HANDLE open_ext(
        const wchar_t *path, DWORD access, DWORD flags, DWORD *err)
{
	wchar_t *ext = NULL;
	HANDLE h;

	if (WF_OK != wf_path_to_extended(path, &ext)) {
		*err = ERROR_INVALID_NAME;
		return INVALID_HANDLE_VALUE;
	}
	h = CreateFileW(ext, access,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING, flags | FILE_FLAG_BACKUP_SEMANTICS, NULL);
	*err = GetLastError(); /* before free() can change it */
	free(ext);
	return h;
}

static WfStatus node(void *ctx, const wchar_t *path, WfNodeInfo *out)
{
	BY_HANDLE_FILE_INFORMATION bi;
	FILE_ID_INFO fid;
	FILE_ATTRIBUTE_TAG_INFO tag;
	DWORD err;
	HANDLE h;

	(void)ctx;
	memset(out, 0, sizeof *out);
	h = open_ext(
	        path, FILE_READ_ATTRIBUTES, FILE_FLAG_OPEN_REPARSE_POINT, &err);
	if (INVALID_HANDLE_VALUE == h) {
		return ERROR_INVALID_NAME == err ? WF_E_INVAL : map_error(err);
	}

	if (!GetFileInformationByHandle(h, &bi)) {
		err = GetLastError();
		CloseHandle(h);
		return map_error(err);
	}
	if (bi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
		out->flags |= WF_ENT_DIR;
	}
	if (bi.dwFileAttributes &
	        (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) {
		out->flags |= WF_ENT_HIDDEN;
	}
	if (bi.dwFileAttributes &
	        (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS |
	                FILE_ATTRIBUTE_RECALL_ON_OPEN |
	                FILE_ATTRIBUTE_OFFLINE)) {
		out->flags |= WF_ENT_CLOUD;
	}
	if (bi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
		out->flags |= WF_ENT_LINK;
		if (GetFileInformationByHandleEx(
		            h, FileAttributeTagInfo, &tag, sizeof tag)) {
			out->reparse_tag = tag.ReparseTag;
		} else {
			out->reparse_tag = 0; /* unknown tag: classified
			                         UNKNOWN, refused */
		}
	}
	out->size = ((uint64_t)bi.nFileSizeHigh << 32) | bi.nFileSizeLow;
	out->mtime = ft_u64(bi.ftLastWriteTime);
	out->nlinks = bi.nNumberOfLinks;

	/* A FAT volume has no FileId: id.valid stays 0 and the weak identity of
	 * the volume serial and entry index stands in (wf_weakid_win.h). Anything
	 * else without a FileId keeps neither, and identity falls back to
	 * attributes (see wf_preflight.c). */
	if (GetFileInformationByHandleEx(h, FileIdInfo, &fid, sizeof fid)) {
		out->id.kind = WF_ID_STRONG;
		out->id.volume = fid.VolumeSerialNumber;
		memcpy(out->id.id, fid.FileId.Identifier, sizeof out->id.id);
	} else if (wf_weak_unsupported(GetLastError())) {
		(void)wf_weak_of_handle(h, &out->id);
	}
	CloseHandle(h);
	return WF_OK;
}

static void copy_w(wchar_t *dst, size_t cap, const wchar_t *src)
{
	size_t n = wcslen(src);

	if (n >= cap) {
		n = cap - 1;
	}
	memcpy(dst, src, n * sizeof *dst);
	dst[n] = L'\0';
}

static WfStatus volume(void *ctx, const wchar_t *path, WfVolInfo *out)
{
	wchar_t root[1024], *final = NULL;
	DWORD serial = 0, maxc = 0, flags = 0, err = 0, need, got;
	ULARGE_INTEGER avail;
	HANDLE h;
	WfStatus status = WF_E_OS;
	(void)ctx;
	memset(out, 0, sizeof *out);
	h = open_ext(path, FILE_READ_ATTRIBUTES, 0, &err);
	if (INVALID_HANDLE_VALUE == h) {
		return map_error(err);
	}
	/* DOS aliases do not necessarily have a mount-manager volume root.
	 * Resolve the opened object's real DOS path before the root/free-space
	 * queries, and obtain filesystem capabilities from that same handle. */
	need = GetFinalPathNameByHandleW(h, NULL, 0, VOLUME_NAME_DOS);
	if (!need) {
		err = GetLastError();
		goto done;
	}
	if (need > 32768) {
		err = ERROR_FILENAME_EXCED_RANGE;
		goto done;
	}
	final = wf_malloc_array((size_t)need, sizeof *final);
	if (!final) {
		status = WF_E_NOMEM;
		goto done;
	}
	got = GetFinalPathNameByHandleW(h, final, need, VOLUME_NAME_DOS);
	if (!got) {
		err = GetLastError();
		goto done;
	}
	if (got >= need) {
		err = ERROR_INSUFFICIENT_BUFFER;
		goto done;
	}
	if (!GetVolumePathNameW(final, root, 1024)) {
		err = GetLastError();
		goto done;
	}
	if (!GetVolumeInformationByHandleW(
	            h, NULL, 0, &serial, &maxc, &flags, out->fs_name, 16)) {
		err = GetLastError();
		/* SMB has no handle-based volume-management support. This
		 * fallback only reports capabilities; it never grants recovery
		 * authority. */
		if (ERROR_NOT_SUPPORTED != err ||
		        !GetVolumeInformationW(root, NULL, 0, &serial, &maxc,
		                &flags, out->fs_name, 16)) {
			err = GetLastError();
			goto done;
		}
	}
	if (!GetDiskFreeSpaceExW(final, &avail, NULL, NULL)) {
		err = GetLastError();
		goto done;
	}
	copy_w(out->root, WF_ARRAY_LEN(out->root), root);
	out->serial = serial;
	out->max_component = maxc;
	out->read_only = 0 != (flags & FILE_READ_ONLY_VOLUME);
	out->supports_open_by_id = 0 != (flags & FILE_SUPPORTS_OPEN_BY_FILE_ID);
	out->is_local = DRIVE_FIXED == GetDriveTypeW(root) ||
	        DRIVE_REMOVABLE == GetDriveTypeW(root);
	out->free_bytes = avail.QuadPart;
	status = WF_OK;
done:
	free(final);
	CloseHandle(h);
	return WF_OK == status || WF_E_NOMEM == status ? status
	                                               : map_error(err);
}

static WfStatus resolve_id(void *ctx, const wchar_t *path, WfFileId *out)
{
	FILE_ID_INFO fid;
	DWORD err;
	HANDLE h;

	(void)ctx;
	memset(out, 0, sizeof *out);
	/* no FILE_FLAG_OPEN_REPARSE_POINT: the open follows links to the target
	 */
	h = open_ext(path, FILE_READ_ATTRIBUTES, 0, &err);
	if (INVALID_HANDLE_VALUE == h) {
		return ERROR_INVALID_NAME == err ? WF_E_INVAL : map_error(err);
	}
	if (GetFileInformationByHandleEx(h, FileIdInfo, &fid, sizeof fid)) {
		out->kind = WF_ID_STRONG;
		out->volume = fid.VolumeSerialNumber;
		memcpy(out->id, fid.FileId.Identifier, sizeof out->id);
	}
	CloseHandle(h);
	return WF_OK;
}

static WfStatus list(void *ctx, const wchar_t *dir, WfDir **out)
{
	(void)ctx;
	return wf_enum_dir_win(dir, out);
}

static WfStatus dest_access(void *ctx, const wchar_t *dir)
{
	DWORD err;
	HANDLE h;

	(void)ctx;
	h = open_ext(dir,
	        FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_READ_ATTRIBUTES, 0,
	        &err);
	if (INVALID_HANDLE_VALUE == h) {
		return ERROR_INVALID_NAME == err ? WF_E_INVAL : map_error(err);
	}
	CloseHandle(h);
	return WF_OK;
}

WfProbeOps wf_probe_win(void)
{
	WfProbeOps ops = {NULL, node, volume, resolve_id, list, dest_access};

	return ops;
}
