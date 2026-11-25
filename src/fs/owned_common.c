#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "owned_internal.h"
#include "base/wf_mem.h" /* via -I src: this file is also built from a copy */

const DWORD wfo_retry_ms[6] = {0, 50, 100, 200, 400, 800};

bool wfo_nonzero(const void *data, size_t n)
{
	const uint8_t *p = data;
	size_t i;
	for (i = 0; i < n; i++) {
		if (p[i]) {
			return 1;
		}
	}
	return 0;
}

/* Operands may be members of the packed on-disk structs, whose address is not
 * 8-byte aligned: they are taken as void * and copied out, never dereferenced
 * as FILE_ID_INFO *. */
bool wfo_id_equal(const void *a, const void *b)
{
	FILE_ID_INFO x, y;
	memcpy(&x, a, sizeof x);
	memcpy(&y, b, sizeof y);
	return x.VolumeSerialNumber == y.VolumeSerialNumber &&
	        0 == memcmp(x.FileId.Identifier, y.FileId.Identifier, 16);
}

static void txn_relative(const uint8_t u[16], wchar_t out[80])
{
	swprintf(out, 80,
	        L".wfile-txn-%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%"
	        L"02x%02x%02x%02x%02x\\payload.bin",
	        u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9],
	        u[10], u[11], u[12], u[13], u[14], u[15]);
}

bool wfo_record_shape(const WfOwnershipRecord *r, bool require_temp_id)
{
	wchar_t rel[80], *parent = NULL;
	const FILE_ID_INFO *ids[4];
	size_t i, j;
	if (!r || !r->intent_durable ||
	        (require_temp_id && !r->temp_id_durable) ||
	        memcmp(r->intent_txn, r->txn, 16) || !wfo_nonzero(r->txn, 16) ||
	        !wfo_nonzero(r->nonce, 32) ||
	        r->intent_source_size != r->source_size || !r->rel_payload ||
	        !wf_canonical_normalize(r->parent_path, &parent)) {
		return 0;
	}
	free(parent);
	txn_relative(r->txn, rel);
	if (wcscmp(rel, r->rel_payload)) {
		return 0;
	}
	ids[0] = &r->parent;
	ids[1] = &r->directory;
	ids[2] = &r->owner;
	ids[3] = &r->payload;
	for (i = 0; i < (require_temp_id ? 4u : 3u); i++) {
		if (!wfo_nonzero(ids[i]->FileId.Identifier, 16) ||
		        ids[i]->VolumeSerialNumber !=
		                r->parent.VolumeSerialNumber) {
			return 0;
		}
		for (j = 0; j < i; j++) {
			if (wfo_id_equal(ids[i], ids[j])) {
				return 0;
			}
		}
	}
	return 1;
}

bool wf_owned_record_valid(const WfOwnershipRecord *record)
{
	return wfo_record_shape(record, 1);
}

wchar_t *wfo_join(const wchar_t *parent, const wchar_t *child)
{
	size_t a = wcslen(parent), b = wcslen(child);
	int slash = a && L'\\' != parent[a - 1];
	wchar_t *p;
	if (a + b + (size_t)slash > 32767) {
		return NULL;
	}
	p = wf_malloc_array((a + b + (size_t)slash + 1), sizeof *p);
	if (p) {
		swprintf(p, a + b + (size_t)slash + 1,
		        slash ? L"%ls\\%ls" : L"%ls%ls", parent, child);
	}
	return p;
}

WfOwnedResult wfo_set_result(
        WfOwnedReport *r, WfOwnedResult result, const char *reason, DWORD error)
{
	r->reason = reason;
	r->error = error;
	return result;
}

bool wfo_object_matches(HANDLE h,
        const FILE_ID_INFO *expected,
        bool directory,
        const wchar_t *path,
        WfOwnedReport *report)
{
	FILE_ID_INFO id;
	FILE_STANDARD_INFO standard;
	FILE_ATTRIBUTE_TAG_INFO tag;
	/* `recorded` only borrows `path` for the comparison; it is never
	 * written or freed. */
	WfCanonicalPath actual = {0},
	                recorded = {expected->VolumeSerialNumber,
	                        (wchar_t *)(uintptr_t)path};
	DWORD error;
	bool ok;
	if (!GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof id) ||
	        !GetFileInformationByHandleEx(
	                h, FileStandardInfo, &standard, sizeof standard) ||
	        !GetFileInformationByHandleEx(
	                h, FileAttributeTagInfo, &tag, sizeof tag)) {
		report->error = GetLastError();
		return 0;
	}
	if (!wfo_id_equal(&id, expected) || standard.DeletePending ||
	        !!standard.Directory != directory ||
	        (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
	        (!directory && 1 != standard.NumberOfLinks)) {
		return 0;
	}
	if (!wf_canonical_from_handle(h, &actual, &error)) {
		report->error = error;
		return 0;
	}
	if (WF_OK != wf_canonical_compare(&recorded, &actual, &ok)) {
		report->error = ERROR_INVALID_NAME; /* not "a different path" */
		ok = false;
	}
	wf_canonical_free(&actual);
	return ok;
}

/* No path fallback for by-ID acquisition. NTFS uses a 64-bit descriptor on
 * systems rejecting ExtendedFileIdType; only an exactly representable ID is
 * permitted. ReFS 128-bit IDs are never truncated. */
HANDLE wfo_open_id(HANDLE hint, const FILE_ID_INFO *id, DWORD access)
{
	FILE_ID_DESCRIPTOR d = {0};
	HANDLE h;
	DWORD error;
	d.dwSize = sizeof d;
	d.Type = ExtendedFileIdType;
	d.ExtendedFileId = id->FileId;
	h = OpenFileById(hint, &d, access, FILE_SHARE_READ, NULL, OPEN_FLAGS);
	if (INVALID_HANDLE_VALUE != h) {
		return h;
	}
	error = GetLastError();
	if ((ERROR_INVALID_PARAMETER == error ||
	            ERROR_NOT_SUPPORTED == error) &&
	        !wfo_nonzero(id->FileId.Identifier + 8, 8)) {
		d.Type = FileIdType;
		memcpy(&d.FileId, id->FileId.Identifier, 8);
		return OpenFileById(
		        hint, &d, access, FILE_SHARE_READ, NULL, OPEN_FLAGS);
	}
	SetLastError(error);
	return INVALID_HANDLE_VALUE;
}

/* Pinned directory enumeration, never a pathname search or recursive walk. */
bool wfo_contents_match(HANDLE dir,
        bool owner_present,
        bool payload_present,
        WfOwnedReport *report)
{
	const DWORD cap = 65536;
	unsigned char *buf = malloc(cap);
	int owner_count = 0, payload_count = 0;
	bool first = true, ok = false;
	if (!buf) {
		report->error = ERROR_NOT_ENOUGH_MEMORY;
		return 0;
	}
	for (;;) {
		size_t offset = 0;
		if (!GetFileInformationByHandleEx(dir,
		            first ? FileIdBothDirectoryRestartInfo
		                  : FileIdBothDirectoryInfo,
		            buf, cap)) {
			DWORD error = GetLastError();
			if (ERROR_NO_MORE_FILES == error) {
				ok = owner_count == (int)owner_present &&
				        payload_count == (int)payload_present;
			} else {
				report->error = error;
			}
			break;
		}
		first = false;
		for (;;) {
			FILE_ID_BOTH_DIR_INFO *e;
			size_t base = offsetof(FILE_ID_BOTH_DIR_INFO, FileName),
			       units;
			if (offset > cap - base) {
				goto done;
			}
			e = (FILE_ID_BOTH_DIR_INFO *)(void *)(buf +
			        offset); /* offset is a sum of NextEntryOffset,
				            each checked % 8 below; malloc is
				            16-aligned */
			if (e->FileNameLength % sizeof(wchar_t) ||
			        e->FileNameLength > cap - offset - base) {
				goto done;
			}
			units = e->FileNameLength / sizeof(wchar_t);
			if ((1 == units && L'.' == e->FileName[0]) ||
			        (2 == units &&
			                !wcsncmp(e->FileName, L"..", 2))) {
				/* Dot entries are not children. */
			} else if (9 == units &&
			        !wcsncmp(e->FileName, L"owner.bin", 9)) {
				owner_count++;
			} else if (11 == units &&
			        !wcsncmp(e->FileName, L"payload.bin", 11)) {
				payload_count++;
			} else {
				goto done;
			}
			if (!e->NextEntryOffset) {
				break;
			}
			if (e->NextEntryOffset < base + e->FileNameLength ||
			        e->NextEntryOffset % 8 ||
			        e->NextEntryOffset > cap - offset) {
				goto done;
			}
			offset += e->NextEntryOffset;
		}
	}
done:
	free(buf);
	return ok;
}

/* The non-share-delete destination handle pins its namespace ancestors on
 * the supported local filesystems (also exercised by the S07 collector).
 * Inspect every ancestor for reparse attributes after pinning the descendant;
 * attribute queries need no directory-list permission on a profile root.
 * Inaccessible attributes always wfo_refuse; no reparse exception is made. */
bool wfo_pin_parents(WfVerifiedOwned *o, WfOwnedReport *report)
{
	wchar_t *path = _wcsdup(o->parent_path);
	size_t i, n = wcslen(o->parent_path);
	bool ok = false;
	if (n < GUID_ROOT_UNITS) { /* no ancestor chain to prove: refuse, never
	                              pass */
		goto out_free_path;
	}
	if (!path) {
		report->error = ERROR_NOT_ENOUGH_MEMORY;
		goto out_free_path;
	}
	o->parent = CreateFileW(o->parent_path, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        OPEN_FLAGS, NULL);
	if (INVALID_HANDLE_VALUE == o->parent) {
		report->error = GetLastError();
		goto out_free_path;
	}
	for (i = GUID_ROOT_UNITS; i <= n; i++) {
		DWORD attributes;
		wchar_t saved;
		if (GUID_ROOT_UNITS != i && i != n && L'\\' != path[i]) {
			continue;
		}
		saved = path[i];
		path[i] = 0;
		attributes = GetFileAttributesW(path);
		path[i] = saved;
		if (INVALID_FILE_ATTRIBUTES == attributes) {
			report->error = GetLastError();
			goto out_free_path;
		}
		if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
		        (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
			goto out_free_path;
		}
	}
	ok = true;
out_free_path:
	free(path);
	return ok;
}

bool wfo_leaf_valid(const wchar_t *leaf)
{
	size_t n;
	if (!leaf || !*leaf || !wcscmp(leaf, L".") || !wcscmp(leaf, L"..")) {
		return 0;
	}
	n = wcslen(leaf);
	return n <= 255 && L'.' != leaf[n - 1] && L' ' != leaf[n - 1] &&
	        !wcspbrk(leaf, L"\\/:*?\"<>|") && !wcschr(leaf, L'\n') &&
	        !wcschr(leaf, L'\r');
}
