#include <windows.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "../tools/p0/p0_lab.h"
#include "diagnostics/wf_diag_format.h"
#include "fs/owned_delete.h"
#include "path/wf_canonical_win.h"
#include "wf_test.h"
#define CAP 2048
typedef struct
{
	P0Lab lab;
	WfCanonicalPath canonical;
	WfOwnedLogRecord record;
	wchar_t root[CAP], marker[CAP], object[CAP], alias[CAP];
} LogFixture;
static int log_fixture(LogFixture *f)
{
	HANDLE root, marker, object;
	WfDiagDiskMarker mark = {0};
	WfDiagDiskHeader header = {0};
	WfDiagDiskRecord entry = {4, 0x4788814eu};
	FILE_ID_INFO marker_id, object_id;
	DWORD wrote, error;
	size_t units;
	int ok = 0;
	memset(f, 0, sizeof *f);
	if (!p0_lab_open(&f->lab, "owned-log", 0) ||
	        !p0_lab_path(&f->lab, L"logs", f->root, CAP) ||
	        !p0_lab_path(
	                &f->lab, L"logs\\.wfile-log-root", f->marker, CAP) ||
	        !p0_lab_path(&f->lab, L"logs\\log0.wfl", f->object, CAP) ||
	        !p0_lab_path(&f->lab, L"logs\\user-alias", f->alias, CAP) ||
	        !CreateDirectoryW(f->root, NULL)) {
		return 0;
	}
	root = CreateFileW(f->root, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	if (INVALID_HANDLE_VALUE == root) {
		return 0;
	}
	if (!wf_canonical_from_handle(root, &f->canonical, &error) ||
	        !GetFileInformationByHandleEx(root, FileIdInfo, &f->record.root,
	                sizeof f->record.root)) {
		CloseHandle(root);
		return 0;
	}
	CloseHandle(root);
	f->record.root_path = f->canonical.path;
	f->record.root_nonce[0] = 71;
	f->record.object_nonce[0] = 72;
	f->record.generation = 1;
	units = wcslen(f->canonical.path);
	marker = CreateFileW(f->marker, GENERIC_WRITE | FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ, NULL, CREATE_NEW, 0, NULL);
	if (INVALID_HANDLE_VALUE == marker) {
		return 0;
	}
	if (!GetFileInformationByHandleEx(
	            marker, FileIdInfo, &marker_id, sizeof marker_id)) {
		CloseHandle(marker);
		return 0;
	}
	f->record.marker = marker_id;
	memcpy(mark.magic, "WFDROOT1", 8);
	mark.root = f->record.root;
	mark.marker = marker_id;
	memcpy(mark.nonce, f->record.root_nonce, 32);
	mark.path_units = (uint32_t)units;
	mark.crc32 = wf_diag_marker_crc(&mark, f->canonical.path);
	ok = WriteFile(marker, &mark, sizeof mark, &wrote, NULL) &&
	        wrote == sizeof mark &&
	        WriteFile(marker, f->canonical.path, (DWORD)(units * 2), &wrote,
	                NULL) &&
	        wrote == units * 2 && FlushFileBuffers(marker);
	CloseHandle(marker);
	if (!ok) {
		return 0;
	}
	object = CreateFileW(f->object, GENERIC_WRITE | FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ, NULL, CREATE_NEW, 0, NULL);
	if (INVALID_HANDLE_VALUE == object) {
		return 0;
	}
	if (!GetFileInformationByHandleEx(
	            object, FileIdInfo, &object_id, sizeof object_id)) {
		CloseHandle(object);
		return 0;
	}
	f->record.object = object_id;
	f->record.object_size = sizeof header + units * 2 + sizeof entry + 4;
	memcpy(header.magic, "WFDLOG01", 8);
	header.root = f->record.root;
	header.marker = marker_id;
	header.object = object_id;
	memcpy(header.root_nonce, f->record.root_nonce, 32);
	memcpy(header.object_nonce, f->record.object_nonce, 32);
	header.generation = 1;
	header.path_units = (uint32_t)units;
	header.crc32 = wf_diag_header_crc(&header, f->canonical.path);
	ok = WriteFile(object, &header, sizeof header, &wrote, NULL) &&
	        wrote == sizeof header &&
	        WriteFile(object, f->canonical.path, (DWORD)(units * 2), &wrote,
	                NULL) &&
	        wrote == units * 2 &&
	        WriteFile(object, &entry, sizeof entry, &wrote, NULL) &&
	        wrote == sizeof entry &&
	        WriteFile(object, "abc\n", 4, &wrote, NULL) && 4 == wrote &&
	        FlushFileBuffers(object);
	CloseHandle(object);
	return ok;
}
static void log_keep(LogFixture *f)
{
	wf_canonical_free(&f->canonical);
	CHECK(0 == p0_lab_close(&f->lab, 1));
}
static int corrupt(const wchar_t *path, LONGLONG offset)
{
	HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
	        FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	LARGE_INTEGER position;
	unsigned char byte = 0;
	DWORD count;
	int ok;
	if (INVALID_HANDLE_VALUE == h) {
		return 0;
	}
	position.QuadPart = offset;
	ok = SetFilePointerEx(h, position, NULL, FILE_BEGIN) &&
	        ReadFile(h, &byte, 1, &count, NULL) && 1 == count;
	byte ^= 1;
	ok = ok && SetFilePointerEx(h, position, NULL, FILE_BEGIN) &&
	        WriteFile(h, &byte, 1, &count, NULL) && 1 == count &&
	        FlushFileBuffers(h);
	CloseHandle(h);
	return ok;
}
static int mutate_header(LogFixture *f, unsigned which)
{
	HANDLE h = CreateFileW(f->object, GENERIC_READ | GENERIC_WRITE,
	        FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	WfDiagDiskHeader header;
	LARGE_INTEGER zero = {0};
	DWORD count;
	int ok;
	if (INVALID_HANDLE_VALUE == h) {
		return 0;
	}
	ok = ReadFile(h, &header, sizeof header, &count, NULL) &&
	        count == sizeof header;
	if (!ok) {
		CloseHandle(h);
		return 0;
	}
	switch (which) {
	case 0:  header.generation = 4; break;
	case 1:  header.slot = 1; break;
	case 2:  header.root.FileId.Identifier[0] ^= 1; break;
	case 3:  header.marker.FileId.Identifier[0] ^= 1; break;
	case 4:  header.object.FileId.Identifier[0] ^= 1; break;
	case 5:  header.root_nonce[0] ^= 1; break;
	case 6:  header.reserved = 1; break;
	case 7:  header.path_units--; break;
	default: CloseHandle(h); return 0;
	}
	header.crc32 = wf_diag_header_crc(&header, f->canonical.path);
	ok = SetFilePointerEx(h, zero, NULL, FILE_BEGIN) &&
	        WriteFile(h, &header, sizeof header, &count, NULL) &&
	        count == sizeof header && FlushFileBuffers(h);
	CloseHandle(h);
	return ok;
}
static int mutate_marker_nonce(LogFixture *f)
{
	HANDLE h = CreateFileW(f->marker, GENERIC_READ | GENERIC_WRITE,
	        FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	WfDiagDiskMarker marker;
	LARGE_INTEGER zero = {0};
	DWORD count;
	int ok;
	if (INVALID_HANDLE_VALUE == h) {
		return 0;
	}
	ok = ReadFile(h, &marker, sizeof marker, &count, NULL) &&
	        count == sizeof marker;
	if (!ok) {
		CloseHandle(h);
		return 0;
	}
	marker.nonce[0] ^= 1;
	marker.crc32 = wf_diag_marker_crc(&marker, f->canonical.path);
	ok = SetFilePointerEx(h, zero, NULL, FILE_BEGIN) &&
	        WriteFile(h, &marker, sizeof marker, &count, NULL) &&
	        count == sizeof marker && FlushFileBuffers(h);
	CloseHandle(h);
	return ok;
}
static int invalid_utf8(LogFixture *f)
{
	HANDLE h = CreateFileW(f->object, GENERIC_WRITE, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, 0, NULL);
	const uint8_t payload[4] = {0xff, 'b', 'c', '\n'};
	WfDiagDiskRecord record = {4, 0};
	LARGE_INTEGER position;
	DWORD wrote;
	int ok;
	if (INVALID_HANDLE_VALUE == h) {
		return 0;
	}
	position.QuadPart = (LONGLONG)(sizeof(WfDiagDiskHeader) +
	        wcslen(f->canonical.path) * sizeof(wchar_t));
	record.crc32 = wf_diag_crc32(payload, sizeof payload);
	ok = SetFilePointerEx(h, position, NULL, FILE_BEGIN) &&
	        WriteFile(h, &record, sizeof record, &wrote, NULL) &&
	        wrote == sizeof record &&
	        WriteFile(h, payload, sizeof payload, &wrote, NULL) &&
	        wrote == sizeof payload && FlushFileBuffers(h);
	CloseHandle(h);
	return ok;
}
int main(void)
{
	LogFixture f;
	WfVerifiedOwned *token = NULL;
	WfOwnedReport report;
	unsigned mutation;
	CHECK(log_fixture(&f));
	CHECK(WF_OWNED_MANUAL ==
	        wf_owned_verify(WF_OWNED_LOG, NULL, &token, &report));
	CHECK(WF_OWNED_VERIFIED ==
	        wf_owned_verify_log(&f.record, &token, &report));
	CHECK(WF_OWNED_DELETED == wf_owned_delete(token, &report));
	wf_owned_release(token);
	token = NULL;
	CHECK(INVALID_FILE_ATTRIBUTES == GetFileAttributesW(f.object));
	CHECK(INVALID_FILE_ATTRIBUTES != GetFileAttributesW(f.marker));
	log_keep(&f);
	CHECK(log_fixture(&f));
	CHECK(corrupt(f.object, (LONGLONG)f.record.object_size - 2));
	CHECK(WF_OWNED_MANUAL ==
	        wf_owned_verify_log(&f.record, &token, &report));
	CHECK(!token &&
	        INVALID_FILE_ATTRIBUTES != GetFileAttributesW(f.object));
	log_keep(&f);
	CHECK(log_fixture(&f));
	f.record.object_nonce[0] ^= 1;
	CHECK(WF_OWNED_MANUAL ==
	        wf_owned_verify_log(&f.record, &token, &report));
	CHECK(!token &&
	        INVALID_FILE_ATTRIBUTES != GetFileAttributesW(f.object));
	log_keep(&f);
	CHECK(log_fixture(&f));
	f.record.object_size++;
	CHECK(WF_OWNED_MANUAL ==
	        wf_owned_verify_log(&f.record, &token, &report));
	log_keep(&f);
	CHECK(log_fixture(&f));
	CHECK(CreateHardLinkW(f.alias, f.object, NULL));
	CHECK(WF_OWNED_MANUAL ==
	        wf_owned_verify_log(&f.record, &token, &report));
	CHECK(INVALID_FILE_ATTRIBUTES != GetFileAttributesW(f.alias) &&
	        INVALID_FILE_ATTRIBUTES != GetFileAttributesW(f.object));
	log_keep(&f);
	CHECK(log_fixture(&f));
	CHECK(corrupt(f.marker, offsetof(WfDiagDiskMarker, crc32)));
	CHECK(WF_OWNED_MANUAL ==
	        wf_owned_verify_log(&f.record, &token, &report));
	CHECK(INVALID_FILE_ATTRIBUTES != GetFileAttributesW(f.object));
	log_keep(&f);
	CHECK(log_fixture(&f));
	f.record.generation = 2;
	CHECK(WF_OWNED_MANUAL ==
	        wf_owned_verify_log(&f.record, &token, &report));
	log_keep(&f);
	CHECK(log_fixture(&f));
	CHECK(corrupt(f.object, offsetof(WfDiagDiskHeader, crc32)));
	CHECK(WF_OWNED_MANUAL ==
	        wf_owned_verify_log(&f.record, &token, &report));
	log_keep(&f);
	CHECK(log_fixture(&f));
	CHECK(mutate_marker_nonce(&f));
	CHECK(WF_OWNED_MANUAL ==
	        wf_owned_verify_log(&f.record, &token, &report));
	log_keep(&f);
	CHECK(log_fixture(&f));
	CHECK(invalid_utf8(&f));
	CHECK(WF_OWNED_MANUAL ==
	        wf_owned_verify_log(&f.record, &token, &report));
	log_keep(&f);
	for (mutation = 0; mutation < 8; mutation++) {
		CHECK(log_fixture(&f));
		CHECK(mutate_header(&f, mutation));
		CHECK(WF_OWNED_MANUAL ==
		        wf_owned_verify_log(&f.record, &token, &report));
		CHECK(!token &&
		        INVALID_FILE_ATTRIBUTES !=
		                GetFileAttributesW(f.object));
		log_keep(&f);
	}
	return TEST_RESULT();
}
