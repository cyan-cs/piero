#include "wf_edit_win.h"

#include <stdlib.h>
#include <string.h>

#include "../path/wf_canonical_win.h"
#include "../path/wf_path.h"
#include "wf_weakid_win.h"

typedef struct
{
	HANDLE handle;
	WfCanonicalPath canonical;
	DWORD component;
	WfFileId id;
} Parent;

static WfStatus error_status(DWORD error)
{
	return ERROR_ACCESS_DENIED == error || ERROR_SHARING_VIOLATION == error
	        ? WF_E_ACCESS
	        : ERROR_FILE_NOT_FOUND == error || ERROR_PATH_NOT_FOUND == error
	        ? WF_E_NOENT
	        : WF_E_OS;
}

/* `weak_ok`: a weak identity (FAT) is accepted for an object that is only
 * being named into (a new file or folder); everything else asks for the proven
 * one, unless the listing's own expectation is weak. */
static WfStatus handle_identity(HANDLE handle,
        const WfFileId *expected,
        WfFileId *out,
        WfEditReport *report,
        bool weak_ok)
{
	WfFileId actual;
	DWORD attributes;

	if (!wf_handle_identity_any(handle, &actual, &report->error) ||
	        !wf_handle_attributes(handle, &attributes, &report->error)) {
		return error_status(report->error);
	}
	if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
		return WF_E_INVAL;
	}
	/* a weak identity (FAT) only confirms a weak one that was listed: it is
	 * never the answer to a strong expectation, or to none */
	if (wf_fileid_is_weak(&actual) && !weak_ok &&
	        !(expected && wf_fileid_is_weak(expected))) {
		return WF_E_INVAL;
	}
	*out = actual;
	if (expected && !wf_fileid_equal_or_weak(expected, out)) {
		return WF_E_INVAL;
	}
	return WF_OK;
}

static void parent_close(Parent *parent)
{
	wf_canonical_free(&parent->canonical);
	if (INVALID_HANDLE_VALUE != parent->handle) {
		CloseHandle(parent->handle);
	}
}

/* Like wfo_pin_parents, pin the descendant before checking ancestry. On the
 * supported local filesystems a non-share-delete descendant pins ancestors.
 * Attribute-only queries avoid requiring list access to a profile root. */
static WfStatus parent_check_chain(wchar_t *path, WfEditReport *report)
{
	size_t n = wcslen(path), root = n, i;
	if (n >= 49 && !wcsncmp(path, L"\\\\?\\Volume{", 11)) {
		root = 49;
	} else {
		for (;;) {
			size_t next;
			wchar_t saved = path[root];
			path[root] = 0;
			next = wf_path_parent_len(path);
			path[root] = saved;
			if (!next) {
				break;
			}
			root = next;
		}
	}
	for (i = root; i <= n; i++) {
		wchar_t saved;
		DWORD attributes;
		if (i != root && i != n && L'\\' != path[i]) {
			continue;
		}
		if (i > root && L'\\' == path[i - 1]) {
			continue;
		}
		saved = path[i];
		path[i] = 0;
		attributes = GetFileAttributesW(path);
		if (INVALID_FILE_ATTRIBUTES == attributes) {
			report->error = GetLastError();
			path[i] = saved;
			return error_status(report->error);
		}
		path[i] = saved;
		if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
		        (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
			return WF_E_INVAL;
		}
	}
	return WF_OK;
}

/* The same check for a caller that pins its own folders (the batch): every
 * component of `path` exists, is a folder and is not a reparse point. */
bool wf_edit_path_chain_ok(const wchar_t *path, unsigned long *error)
{
	WfEditReport report = {0};
	wchar_t *copy = wf_path_dup(path);
	WfStatus status;

	if (!copy) {
		*error = ERROR_NOT_ENOUGH_MEMORY;
		return false;
	}
	status = parent_check_chain(copy, &report);
	free(copy);
	*error = report.error;
	return WF_OK == status;
}

static WfStatus parent_open(
        const wchar_t *path, Parent *parent, WfEditReport *report, bool weak_ok)
{
	wchar_t *extended = NULL, *canonical = NULL;
	DWORD flags;
	WfStatus status;
	DWORD attributes;
	memset(parent, 0, sizeof *parent);
	parent->handle = INVALID_HANDLE_VALUE;
	status = wf_path_to_extended(path, &extended);
	if (WF_OK != status) {
		return status;
	}
	if (wcslen(extended) > 32766) {
		status = WF_E_RANGE;
		goto done;
	}
	/* No write sharing: the pinned leaf cannot acquire a reparse tag. */
	parent->handle = CreateFileW(extended, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ, NULL, OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	if (INVALID_HANDLE_VALUE == parent->handle) {
		report->error = GetLastError();
		status = error_status(report->error);
		goto done;
	}
	status = handle_identity(
	        parent->handle, NULL, &parent->id, report, weak_ok);
	if (WF_OK != status) {
		goto done;
	}
	if (!wf_handle_attributes(
	            parent->handle, &attributes, &report->error)) {
		status = error_status(report->error);
		goto done;
	}
	if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
		status = WF_E_INVAL;
		goto done;
	}
	if (!wf_canonical_from_handle_weak_ok(
	            parent->handle, &parent->canonical, &report->error)) {
		status = error_status(report->error);
		goto done;
	}
	canonical = wf_path_dup(parent->canonical.path);
	if (!canonical) {
		status = WF_E_NOMEM;
		goto done;
	}
	status = parent_check_chain(extended, report);
	if (WF_OK != status) {
		goto done;
	}
	status = parent_check_chain(canonical, report);
	if (WF_OK != status) {
		goto done;
	}
	if (!GetVolumeInformationByHandleW(parent->handle, NULL, 0, NULL,
	            &parent->component, &flags, NULL, 0)) {
		report->error = GetLastError();
		status = error_status(report->error);
		goto done;
	}
	if (flags & FILE_READ_ONLY_VOLUME) {
		report->error = ERROR_WRITE_PROTECT;
		status = WF_E_ACCESS;
	}
done:
	free(canonical);
	free(extended);
	return status;
}

static WfStatus child_path(Parent *parent,
        const wchar_t *name,
        wchar_t **out,
        WfEditReport *report)
{
	WfStatus status;
	report->name = wf_name_validate(
	        name, name ? wcslen(name) : 0, parent->component);
	if (WF_NAME_OK != report->name) {
		return WF_E_INVAL;
	}
	status = wf_path_join(parent->canonical.path, name, out);
	if (WF_OK == status && wcslen(*out) > 32766) {
		free(*out);
		*out = NULL;
		status = WF_E_RANGE;
	}
	return status;
}

static WfStatus edit_create(const wchar_t *parent,
        const WfFileId *expected_parent,
        const wchar_t *name,
        int directory,
        WfEditReport *report)
{
	Parent pinned;
	wchar_t *target = NULL;
	HANDLE child;
	WfStatus status;
	if (!report) {
		return WF_E_INVAL;
	}
	memset(report, 0, sizeof *report);
	if (!parent || !name || (0 != directory && 1 != directory)) {
		return WF_E_INVAL;
	}
	status = parent_open(parent, &pinned, report, true);
	if (WF_OK != status) {
		goto done;
	}
	if (expected_parent &&
	        !wf_fileid_equal_or_weak(expected_parent, &pinned.id)) {
		status = WF_E_INVAL;
		goto done;
	}
	status = child_path(&pinned, name, &target, report);
	if (WF_OK != status) {
		goto done;
	}
	if (directory) {
		if (!CreateDirectoryW(target, NULL)) {
			report->error = GetLastError();
			status = error_status(report->error);
		}
	} else {
		child = CreateFileW(target, GENERIC_WRITE, FILE_SHARE_READ,
		        NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
		if (INVALID_HANDLE_VALUE == child) {
			report->error = GetLastError();
			status = error_status(report->error);
		} else {
			CloseHandle(child);
		}
	}
done:
	free(target);
	parent_close(&pinned);
	return status;
}

WfStatus wf_edit_create(const wchar_t *parent,
        const wchar_t *name,
        int directory,
        WfEditReport *report)
{
	return edit_create(parent, NULL, name, directory, report);
}

WfStatus wf_edit_create_checked(const wchar_t *parent,
        const WfFileId *expected_parent,
        const wchar_t *name,
        int directory,
        WfEditReport *report)
{
	if (!wf_fileid_usable_to_read(expected_parent)) {
		if (report) {
			memset(report, 0, sizeof *report);
		}
		return WF_E_INVAL;
	}
	return edit_create(parent, expected_parent, name, directory, report);
}

/* "a/b/c": a folder on the way is made when it is missing and entered when it
 * already is a folder (parent_open refuses a file and a reparse point). Each
 * step is pinned by the identity seen in the step before, so a folder swapped
 * in between fails the check. Folders made before a failure stay. */
WfStatus wf_edit_create_path_checked(const wchar_t *parent,
        const WfFileId *expected_parent,
        const wchar_t *path,
        int directory,
        WfEditReport *report)
{
	enum
	{
		COMPONENT_CAP = 256
	};
	wchar_t name[COMPONENT_CAP];
	wchar_t *dir, *next = NULL;
	WfFileId id;
	WfStatus status;
	size_t depth = 0;

	if (!report) {
		return WF_E_INVAL;
	}
	memset(report, 0, sizeof *report);
	if (!parent || !path || !wf_fileid_usable_to_read(expected_parent)) {
		return WF_E_INVAL;
	}
	dir = wf_path_dup(parent);
	if (!dir) {
		return WF_E_NOMEM;
	}
	id = *expected_parent;
	for (;;) {
		const wchar_t *after;
		size_t n = wf_name_split_first(path, &after);
		Parent pinned;

		if (n >= COMPONENT_CAP || ++depth > WF_NAME_PATH_DEPTH) {
			report->name = WF_NAME_TOO_LONG;
			status = WF_E_INVAL;
			break;
		}
		memcpy(name, path, n * sizeof *name);
		name[n] = L'\0';
		if (!after) {
			status = edit_create(dir, &id, name, directory, report);
			break;
		}
		status = edit_create(dir, &id, name, 1, report);
		if (WF_OK != status && ERROR_ALREADY_EXISTS != report->error) {
			break;
		}
		report->error = 0;
		status = wf_path_join(dir, name, &next);
		if (WF_OK != status) {
			break;
		}
		free(dir);
		dir = next;
		next = NULL;
		status = parent_open(dir, &pinned, report, true);
		if (WF_OK != status) {
			if (!report->error) {
				report->error = ERROR_ALREADY_EXISTS;
			}
			break;
		}
		id = pinned.id;
		parent_close(&pinned);
		path = after;
	}
	free(dir);
	return status;
}

static WfStatus move_child(Parent *from,
        const wchar_t *old_name,
        const WfFileId *expected,
        Parent *to,
        const wchar_t *new_name,
        WfEditReport *report)
{
	wchar_t *source = NULL, *target = NULL;
	HANDLE child = INVALID_HANDLE_VALUE;
	FILE_RENAME_INFO *rename = NULL;
	WfFileId observed;
	WfStatus status;
	size_t bytes;
	report->name =
	        wf_name_validate(old_name, wcslen(old_name), from->component);
	if (WF_NAME_OK != report->name) {
		return WF_E_INVAL;
	}
	status = child_path(to, new_name, &target, report);
	if (WF_OK != status) {
		goto done;
	}
	status = wf_path_join(from->canonical.path, old_name, &source);
	if (WF_OK != status) {
		goto done;
	}
	child = CreateFileW(source, DELETE | FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ, NULL, OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	if (INVALID_HANDLE_VALUE == child) {
		report->error = GetLastError();
		status = error_status(report->error);
		goto done;
	}
	status = handle_identity(child, expected, &observed, report, false);
	if (WF_OK != status) {
		goto done;
	}
	bytes = wcslen(target) * sizeof *target;
	if (bytes > MAXDWORD - sizeof *rename) {
		status = WF_E_RANGE;
		goto done;
	}
	rename = calloc(1, sizeof *rename + bytes);
	if (!rename) {
		status = WF_E_NOMEM;
		goto done;
	}
	rename->ReplaceIfExists = FALSE;
	rename->RootDirectory = NULL;
	rename->FileNameLength = (DWORD)bytes;
	memcpy(rename->FileName, target, bytes);
	if (!SetFileInformationByHandle(child, FileRenameInfo, rename,
	            (DWORD)(sizeof *rename + bytes))) {
		report->error = GetLastError();
		status = error_status(report->error);
		goto done;
	}
	report->renamed = true;
	/* A postcheck failure is reported without destructive rollback. */
	status = handle_identity(child, expected, &observed, report, false);
done:
	free(rename);
	if (INVALID_HANDLE_VALUE != child) {
		CloseHandle(child);
	}
	free(target);
	free(source);
	return status;
}

static WfStatus edit_rename(const wchar_t *parent,
        const WfFileId *expected_parent,
        const wchar_t *old_name,
        const WfFileId *expected,
        const wchar_t *new_name,
        WfEditReport *report)
{
	Parent pinned;
	WfStatus status;
	if (!report) {
		return WF_E_INVAL;
	}
	memset(report, 0, sizeof *report);
	if (!parent || !old_name || !new_name || !expected ||
	        !(expected->kind == WF_ID_STRONG)) {
		return WF_E_INVAL;
	}
	status = parent_open(parent, &pinned, report, false);
	if (WF_OK == status && expected_parent &&
	        !wf_fileid_equal(expected_parent, &pinned.id)) {
		status = WF_E_INVAL;
	}
	if (WF_OK == status) {
		status = move_child(
		        &pinned, old_name, expected, &pinned, new_name, report);
	}
	parent_close(&pinned);
	return status;
}

WfStatus wf_edit_rename(const wchar_t *parent,
        const wchar_t *old_name,
        const WfFileId *expected,
        const wchar_t *new_name,
        WfEditReport *report)
{
	return edit_rename(parent, NULL, old_name, expected, new_name, report);
}

WfStatus wf_edit_rename_checked(const wchar_t *parent,
        const WfFileId *expected_parent,
        const wchar_t *old_name,
        const WfFileId *expected,
        const wchar_t *new_name,
        WfEditReport *report)
{
	if (!expected_parent || !(expected_parent->kind == WF_ID_STRONG)) {
		if (report) {
			memset(report, 0, sizeof *report);
		}
		return WF_E_INVAL;
	}
	return edit_rename(
	        parent, expected_parent, old_name, expected, new_name, report);
}

WfStatus wf_edit_move(const wchar_t *source_parent,
        const WfFileId *expected_source_parent,
        const wchar_t *old_name,
        const WfFileId *expected_source,
        const wchar_t *destination_parent,
        const WfFileId *expected_destination_parent,
        const wchar_t *new_name,
        WfEditReport *report)
{
	Parent from, to;
	WfStatus status;
	if (!report) {
		return WF_E_INVAL;
	}
	memset(report, 0, sizeof *report);
	if (!source_parent || !destination_parent || !old_name || !new_name ||
	        !expected_source_parent ||
	        !(expected_source_parent->kind == WF_ID_STRONG) ||
	        !expected_destination_parent ||
	        !(expected_destination_parent->kind == WF_ID_STRONG) ||
	        !expected_source || !(expected_source->kind == WF_ID_STRONG)) {
		return WF_E_INVAL;
	}
	status = parent_open(source_parent, &from, report, false);
	if (WF_OK != status) {
		goto done;
	}
	if (!wf_fileid_equal(&from.id, expected_source_parent)) {
		status = WF_E_INVAL;
		goto done;
	}
	status = parent_open(destination_parent, &to, report, false);
	if (WF_OK == status &&
	        !wf_fileid_equal(&to.id, expected_destination_parent)) {
		status = WF_E_INVAL;
	}
	if (WF_OK == status && from.id.volume != to.id.volume) {
		report->error = ERROR_NOT_SAME_DEVICE;
		status = WF_E_INVAL;
	}
	if (WF_OK == status) {
		status = move_child(&from, old_name, expected_source, &to,
		        new_name, report);
	}
	parent_close(&to);
done:
	parent_close(&from);
	return status;
}
