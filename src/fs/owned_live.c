#include <windows.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winternl.h>

#include "owned_internal.h"

typedef NTSTATUS(NTAPI *WfNtCreateFile)(PHANDLE,
        ACCESS_MASK,
        POBJECT_ATTRIBUTES,
        PIO_STATUS_BLOCK,
        PLARGE_INTEGER,
        ULONG,
        ULONG,
        ULONG,
        ULONG,
        PVOID,
        ULONG);
typedef ULONG(WINAPI *WfNtError)(NTSTATUS);

/* NtCreateFile only reads ObjectName; UNICODE_STRING has no const form, so the
 * one qualifier cast lives here. `s` must outlive the call that uses `u`. */
static void unicode_view(UNICODE_STRING *u, const wchar_t *s, size_t units)
{
	u->Length = (USHORT)(units * sizeof(wchar_t));
	u->MaximumLength = u->Length;
	u->Buffer = (PWSTR)(uintptr_t)s;
}

/* GetProcAddress returns a generic FARPROC for both exports. */
static bool load_nt(WfNtCreateFile *create, WfNtError *translate)
{
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	FARPROC c, t;
	if (!ntdll) {
		return false;
	}
	c = GetProcAddress(ntdll, "NtCreateFile");
	t = GetProcAddress(ntdll, "RtlNtStatusToDosError");
	if (!c || !t) {
		return false;
	}
	/* Copying the pointer bytes is the one conversion that is well-defined
	 * on every Windows target and needs no function-type cast at all. */
	_Static_assert(
	        sizeof *create == sizeof c && sizeof *translate == sizeof t,
	        "function pointers share one representation on Windows");
	memcpy(create, &c, sizeof *create);
	memcpy(translate, &t, sizeof *translate);
	return true;
}

struct WfLiveManifest
{
	WfVerifiedOwned **entries;
	size_t count, capacity;
};

bool wfo_live_matches(WfVerifiedOwned *o, WfOwnedReport *report)
{
	WfLiveManifest *m = o->live_manifest;
	WfVerifiedOwned *root;
	FILE_STANDARD_INFO standard;
	if (!m || !m->count || !(root = m->entries[0]) ||
	        INVALID_HANDLE_VALUE == root->payload ||
	        !wf_owned_record_valid(&root->record) ||
	        !wfo_transaction_matches(root, report) ||
	        !wfo_object_matches(o->payload, &o->live_id, o->live_directory,
	                o->live_path, report)) {
		return 0;
	}
	if (!o->live_directory) {
		if (!GetFileInformationByHandleEx(o->payload, FileStandardInfo,
		            &standard, sizeof standard)) {
			report->error = GetLastError();
			return 0;
		}
		if (standard.EndOfFile.QuadPart < 0 ||
		        (uint64_t)standard.EndOfFile.QuadPart > o->live_limit) {
			return 0;
		}
	}
	return 1;
}

void wf_owned_live_release(WfLiveManifest *m)
{
	size_t i;
	if (!m) {
		return;
	}
	for (i = m->count; i > 0; i--) {
		wf_owned_release(m->entries[i - 1]);
	}
	free(m->entries);
	free(m);
}

/* One FILE_CREATE below `root` through the NT API (handle-relative, so no path
 * is resolved twice). On success *out is the new object's handle and *created
 * says whether the system reports it as newly created. On failure *out is
 * INVALID_HANDLE_VALUE and report->error holds the Win32 translation. */
static bool nt_create_child(HANDLE root,
        const wchar_t *leaf,
        size_t leaf_units,
        ACCESS_MASK access,
        ULONG options,
        HANDLE *out,
        bool *created,
        WfOwnedReport *report)
{
	WfNtCreateFile create;
	WfNtError translate;
	OBJECT_ATTRIBUTES attributes = {0};
	UNICODE_STRING name;
	IO_STATUS_BLOCK io;
	NTSTATUS status;

	*out = INVALID_HANDLE_VALUE;
	*created = false;
	if (!load_nt(&create, &translate)) {
		report->error = ERROR_NOT_SUPPORTED;
		return false;
	}
	unicode_view(&name, leaf, leaf_units);
	attributes.Length = sizeof attributes;
	attributes.RootDirectory = root;
	attributes.ObjectName = &name;
	status = create(out, access, &attributes, &io, NULL,
	        FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_CREATE,
	        FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT |
	                options,
	        NULL, 0);
	if (status < 0) {
		*out = INVALID_HANDLE_VALUE;
		report->error = translate(status);
		return false;
	}
	*created = FILE_CREATED == io.Information;
	return true;
}

/* Room for one more manifest entry; the manifest only ever grows. */
static WfOwnedResult manifest_reserve(WfLiveManifest *m, WfOwnedReport *report)
{
	WfVerifiedOwned **entries;
	size_t capacity;

	if (m->count < m->capacity) {
		return WF_OWNED_VERIFIED;
	}
	if (m->capacity > SIZE_MAX / (2 * sizeof *entries)) {
		return wfo_set_result(report, WF_OWNED_MANUAL,
		        "manifest capacity", ERROR_NOT_ENOUGH_MEMORY);
	}
	capacity = m->capacity * 2;
	entries = realloc(m->entries, capacity * sizeof *entries);
	if (!entries) {
		return wfo_set_result(report, WF_OWNED_MANUAL,
		        "manifest allocation", ERROR_NOT_ENOUGH_MEMORY);
	}
	m->entries = entries;
	m->capacity = capacity;
	return WF_OWNED_VERIFIED;
}

/* Normalizes the record into `o`, pins the parent chain and requires an
 * NTFS/ReFS volume that supports open-by-ID. */
static bool live_pin_parent(WfVerifiedOwned *o,
        const WfOwnershipRecord *record,
        WfOwnedReport *report)
{
	wchar_t fs[32];
	DWORD flags;

	o->record = *record;
	if (!wf_canonical_normalize(record->parent_path, &o->parent_path)) {
		return false;
	}
	o->rel_payload = _wcsdup(record->rel_payload);
	o->live_path = wfo_join(o->parent_path, record->rel_payload);
	if (!o->rel_payload || !o->live_path) {
		return false;
	}
	o->record.parent_path = o->parent_path;
	o->record.rel_payload = o->rel_payload;
	return wfo_pin_parents(o, report) &&
	        GetVolumeInformationByHandleW(
	                o->parent, NULL, 0, NULL, NULL, &flags, fs, 32) &&
	        !(wcscmp(fs, L"NTFS") && wcscmp(fs, L"ReFS")) &&
	        (flags & FILE_SUPPORTS_OPEN_BY_FILE_ID);
}

/* Opens the already-created transaction directory and its owner file and
 * proves them against the INTENT record. */
static bool live_open_transaction(WfVerifiedOwned *o, WfOwnedReport *report)
{
	wchar_t *directory = _wcsdup(o->live_path), *owner = NULL;
	bool ok = false;

	if (!directory) {
		return false;
	}
	*wcsrchr(directory, L'\\') = 0;
	owner = wfo_join(directory, L"owner.bin");
	if (!owner) {
		goto done;
	}
	o->directory = CreateFileW(directory,
	        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
	        NULL, OPEN_EXISTING, OPEN_FLAGS, NULL);
	o->owner = CreateFileW(owner, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, OPEN_FLAGS, NULL);
	if (INVALID_HANDLE_VALUE == o->directory ||
	        INVALID_HANDLE_VALUE == o->owner) {
		report->error = GetLastError();
		goto done;
	}
	ok = wfo_transaction_matches(o, report);
done:
	free(owner);
	free(directory);
	return ok;
}

/* Creates the empty payload directory. The new ID is recorded durably (the
 * TEMP_ID step) before anything else may rely on it. */
static bool live_create_root(WfVerifiedOwned *o,
        WfLiveDurableTempId durable_temp_id,
        void *context,
        WfOwnedReport *report)
{
	bool created;

	if (!nt_create_child(o->directory, L"payload.bin", 11,
	            DELETE | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES |
	                    FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
	            FILE_DIRECTORY_FILE, &o->payload, &created, report)) {
		return false;
	}
	if (!created ||
	        !GetFileInformationByHandleEx(o->payload, FileIdInfo,
	                &o->live_id, sizeof o->live_id)) {
		return false;
	}
	o->record.payload = o->live_id;
	if (!durable_temp_id(context, &o->record)) {
		report->error = ERROR_WRITE_FAULT;
		return false;
	}
	o->record.temp_id_durable = 1;
	return wf_owned_record_valid(&o->record) &&
	        wfo_transaction_matches(o, report) &&
	        wfo_contents_match(o->payload, 0, 0, report);
}

WfOwnedResult wf_owned_live_begin(const WfOwnershipRecord *record,
        WfLiveDurableTempId durable_temp_id,
        void *context,
        WfLiveManifest **out,
        WfOwnedReport *report)
{
	WfOwnedReport local = {0};
	WfVerifiedOwned *o = NULL;
	WfLiveManifest *m = NULL;

	if (!report) {
		report = &local;
	}
	memset(report, 0, sizeof *report);
	if (out) {
		*out = NULL;
	}
	if (!out || !durable_temp_id || !wfo_record_shape(record, 0) ||
	        record->temp_id_durable ||
	        wfo_nonzero(record->payload.FileId.Identifier, 16)) {
		return wfo_set_result(report, WF_OWNED_MANUAL,
		        "live constructor requires INTENT and a new root", 0);
	}
	m = calloc(1, sizeof *m);
	o = calloc(1, sizeof *o);
	if (!m || !o) {
		goto fail;
	}
	o->parent = o->directory = o->owner = o->payload = o->destination =
	        INVALID_HANDLE_VALUE;
	o->kind = WF_OWNED_LIVE_TEMP_CHILD;
	o->directory_payload = 1;
	/* order: prove the transaction, create the root, make its ID durable,
	 * re-prove, and only then publish the manifest */
	if (!live_pin_parent(o, record, report) ||
	        !live_open_transaction(o, report) ||
	        !live_create_root(o, durable_temp_id, context, report)) {
		goto fail;
	}
	m->entries = malloc(8 * sizeof *m->entries);
	if (!m->entries) {
		goto fail;
	}
	m->capacity = 8;
	m->count = 1;
	m->entries[0] = o;
	o->live_manifest = m;
	o->live_directory = 1;
	*out = m;
	return wfo_set_result(report, WF_OWNED_VERIFIED,
	        "live empty directory transaction pinned", 0);
fail:
	/* A payload directory created above is not removed here: deletion is
	 * only permitted through the ownership-proven wf_owned_delete
	 * (CI-DEL-01). The INTENT record stays and recovery reaches it by ID.
	 */
	wf_owned_release(o);
	if (m) {
		free(m->entries);
		free(m);
	}
	return wfo_set_result(report, WF_OWNED_MANUAL,
	        "live directory ownership uncertain", report->error);
}

WfOwnedResult wf_owned_live_create(WfLiveManifest *m,
        size_t parent_index,
        const wchar_t *leaf,
        int directory,
        const void *bytes,
        DWORD count,
        uint64_t limit,
        size_t *index,
        WfOwnedReport *report)
{
	WfOwnedReport local = {0};
	WfVerifiedOwned *o = NULL, *parent;
	DWORD wrote;
	bool created;
	WfOwnedResult room;

	if (!report) {
		report = &local;
	}
	memset(report, 0, sizeof *report);
	if (index) {
		*index = SIZE_MAX;
	}
	if (!m || !index || parent_index >= m->count || !wfo_leaf_valid(leaf) ||
	        (0 != directory && 1 != directory) || (count && !bytes) ||
	        count > limit || (directory && count)) {
		return wfo_set_result(report, WF_OWNED_MANUAL,
		        "invalid manifested child request", 0);
	}
	parent = m->entries[parent_index];
	if (!parent->live_directory || !wfo_live_matches(parent, report)) {
		return wfo_set_result(report, WF_OWNED_MANUAL,
		        "manifested parent changed", report->error);
	}
	room = manifest_reserve(m, report);
	if (WF_OWNED_VERIFIED != room) {
		return room;
	}
	o = calloc(1, sizeof *o);
	if (!o) {
		return wfo_set_result(report, WF_OWNED_MANUAL,
		        "child allocation", ERROR_NOT_ENOUGH_MEMORY);
	}
	o->parent = o->directory = o->owner = o->payload = o->destination =
	        INVALID_HANDLE_VALUE;
	o->kind = WF_OWNED_LIVE_TEMP_CHILD;
	o->live_manifest = m;
	o->live_directory = directory;
	o->live_limit = limit;
	o->live_path = wfo_join(parent->live_path, leaf);
	if (!o->live_path) {
		goto fail;
	}
	if (!nt_create_child(parent->payload, leaf, wcslen(leaf),
	            DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES |
	                    SYNCHRONIZE |
	                    (directory ? FILE_LIST_DIRECTORY : GENERIC_WRITE),
	            directory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE,
	            &o->payload, &created, report)) {
		goto fail;
	}
	if (!created ||
	        !GetFileInformationByHandleEx(o->payload, FileIdInfo,
	                &o->live_id, sizeof o->live_id)) {
		report->error = ERROR_INVALID_DATA;
		goto fail;
	}
	/* From here the child exists: it is manifested before anything else is
	 * attempted, and a later failure retains it. */
	*index = m->count;
	m->entries[m->count++] = o;
	if (!directory && count &&
	        (!WriteFile(o->payload, bytes, count, &wrote, NULL) ||
	                wrote != count)) {
		return wfo_set_result(report, WF_OWNED_RECOVERY_REQUIRED,
		        "created child retained after partial write",
		        GetLastError());
	}
	if (!wfo_live_matches(o, report)) {
		return wfo_set_result(report, WF_OWNED_MANUAL,
		        "created child proof failed; retained", report->error);
	}
	return wfo_set_result(report, WF_OWNED_VERIFIED,
	        "child atomically created and manifested by handle", 0);
fail:
	wf_owned_release(o);
	return wfo_set_result(report, WF_OWNED_MANUAL,
	        "child creation unavailable; no adopted path accepted",
	        report->error);
}

WfOwnedResult wf_owned_live_cleanup(
        WfLiveManifest *m, size_t *deleted, WfOwnedReport *report)
{
	WfOwnedReport local = {0};
	WfOwnedResult result = WF_OWNED_DELETED;
	size_t i;
	if (!report) {
		report = &local;
	}
	memset(report, 0, sizeof *report);
	if (deleted) {
		*deleted = 0;
	}
	if (!m || !deleted) {
		return wfo_set_result(
		        report, WF_OWNED_MANUAL, "no live manifest", 0);
	}
	for (i = m->count; i > 0; i--) {
		WfVerifiedOwned *o = m->entries[i - 1];
		if (INVALID_HANDLE_VALUE == o->payload) {
			continue;
		}
		if (WF_OWNED_DELETED == wf_owned_delete(o, report)) {
			(*deleted)++;
		} else {
			result = WF_OWNED_RECOVERY_REQUIRED;
		}
	}
	return result;
}
