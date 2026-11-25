#include <stdlib.h>
#include <string.h>

#include "../tools/p0/p0_common.h"
#include "../tools/p0/p0_lab.h"
#include "diagnostics/wf_diag.h"
#include "diagnostics/wf_diag_format.h"
#include "path/cwd_neutralize.h"
#include "wf_test.h"

static unsigned char *read_all(const wchar_t *path, size_t *bytes)
{
	HANDLE h = CreateFileW(path, GENERIC_READ,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING, 0, NULL);
	LARGE_INTEGER size;
	DWORD got;
	unsigned char *data = NULL;
	if (INVALID_HANDLE_VALUE == h) {
		return NULL;
	}
	if (GetFileSizeEx(h, &size) && size.QuadPart >= 0 &&
	        size.QuadPart <= WF_DIAG_FILE_BYTES) {
		*bytes = (size_t)size.QuadPart;
		data = malloc(*bytes + 1);
		if (data &&
		        (!ReadFile(h, data, (DWORD)*bytes, &got, NULL) ||
		                got != *bytes)) {
			free(data);
			data = NULL;
		}
		if (data) {
			data[*bytes] = 0;
		}
	}
	CloseHandle(h);
	return data;
}
static int contains(const unsigned char *bytes, size_t count, const char *text)
{
	size_t n = strlen(text);
	for (size_t i = 0; n <= count && i <= count - n; i++) {
		if (!memcmp(bytes + i, text, n)) {
			return 1;
		}
	}
	return 0;
}
static void track_root(P0Lab *lab, const wchar_t *name)
{
	wchar_t rel[256], path[1024];
	CHECK(p0_area_track(lab->area, name, 1));
	swprintf(rel, 256, L"%ls\\.wfile-log-root", name);
	CHECK(p0_area_track(lab->area, rel, 0));
	for (unsigned i = 0; i < 3; i++) {
		swprintf(rel, 256, L"%ls\\log%u.wfl", name, i);
		CHECK(p0_lab_path(lab, rel, path, 1024));
		if (INVALID_FILE_ATTRIBUTES != GetFileAttributesW(path)) {
			CHECK(p0_area_track(lab->area, rel, 0));
		}
	}
}
static void privacy_and_crc(P0Lab *lab)
{
	wchar_t root[1024], file[1024];
	WfDiagnostics *log = NULL;
	DWORD error;
	size_t n = 0;
	unsigned char *data;
	WfDiagDiskHeader header;
	char before[65], after[65];
	WfDiagEvent event = {WF_DIAG_WARNING, WF_DIAG_CONFIG, WF_DIAG_FAILED,
	        42, 5, (int32_t)0x80070005u, L"C:\\secret\\body.txt"};
	CHECK(p0_lab_path(lab, L"basic", root, 1024));
	CHECK(wf_diag_open(root, WF_DIAG_WARNING, 0, &log, &error));
	if (!log) {
		return;
	}
	event.level = WF_DIAG_INFO;
	CHECK(wf_diag_write(log, &event, &error));
	event.level = WF_DIAG_WARNING;
	CHECK(wf_diag_write(log, &event, &error));
	wf_diag_close(log);
	log = NULL;
	CHECK(p0_lab_path(lab, L"basic\\log0.wfl", file, 1024));
	data = read_all(file, &n);
	CHECK(NULL != data);
	if (data) {
		memcpy(&header, data, sizeof header);
		size_t offset = sizeof header +
		        (size_t)header.path_units * sizeof(wchar_t);
		CHECK(wf_diag_records_valid(data + offset, n - offset));
		CHECK(contains(data + offset, n - offset,
		        "op=42 stage=config result=failed win32=5 "
		        "hresult=0x80070005"));
		CHECK(!contains(data + offset, n - offset, "secret") &&
		        !contains(data + offset, n - offset, "level=info"));
		free(data);
	}
	CHECK(wf_diag_open(root, WF_DIAG_DEBUG, 0, &log, &error));
	CHECK(NULL != log);
	if (log) {
		CHECK(wf_diag_write(log, &event, &error));
		wf_diag_close(log);
		log = NULL;
	}
	HANDLE h = CreateFileW(
	        file, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	LARGE_INTEGER where;
	DWORD wrote;
	CHECK(INVALID_HANDLE_VALUE != h);
	where.QuadPart = -2;
	CHECK(SetFilePointerEx(h, where, NULL, FILE_END));
	CHECK(WriteFile(h, "X", 1, &wrote, NULL) && 1 == wrote);
	CloseHandle(h);
	CHECK(p0_sha256_file(file, before));
	CHECK(!wf_diag_open(root, WF_DIAG_WARNING, 0, &log, &error) && !log);
	CHECK(p0_sha256_file(file, after) && !strcmp(before, after));
	track_root(lab, L"basic");
}
static void path_sanitization_and_rotation(P0Lab *lab)
{
	wchar_t root[1024], file[1024], *path = malloc(32768 * sizeof *path);
	WfDiagnostics *log = NULL;
	DWORD error;
	WfDiagEvent event = {
	        WF_DIAG_WARNING, WF_DIAG_LIST, WF_DIAG_OK, 1, 0, 0, NULL};
	CHECK(NULL != path);
	if (!path) {
		return;
	}
	CHECK(p0_lab_path(lab, L"rotation", root, 1024));
	CHECK(wf_diag_open(root, WF_DIAG_DEBUG, 1, &log, &error));
	if (!log) {
		free(path);
		return;
	}
	event.path = L"C:\\日本語\n\x1b[31m";
	CHECK(wf_diag_write(log, &event, &error));
	CHECK(p0_lab_path(lab, L"rotation\\log0.wfl", file, 1024));
	{
		size_t count = 0;
		unsigned char *data = read_all(file, &count);
		WfDiagDiskHeader header;
		CHECK(NULL != data);
		if (data) {
			memcpy(&header, data, sizeof header);
			size_t offset = sizeof header +
			        (size_t)header.path_units * sizeof(wchar_t);
			CHECK(contains(
			        data + offset, count - offset, "日本語^J^["));
			free(data);
		}
	}
	/* 32767-unit paths force five generations with only ~130 real writes.
	 */
	for (size_t i = 0; i < 32767; i++) {
		path[i] = L'x';
	}
	path[0] = L'C';
	path[1] = L':';
	path[2] = L'\\';
	path[32767] = 0;
	event.path = path;
	for (unsigned i = 0; i < 130; i++) {
		CHECK(wf_diag_write(log, &event, &error));
	}
	wf_diag_close(log);
	free(path);
	uint64_t minimum = UINT64_MAX, maximum = 0;
	for (unsigned i = 0; i < 3; i++) {
		wchar_t rel[64];
		size_t n = 0;
		WfDiagDiskHeader header;
		unsigned char *data;
		swprintf(rel, 64, L"rotation\\log%u.wfl", i);
		CHECK(p0_lab_path(lab, rel, file, 1024));
		data = read_all(file, &n);
		CHECK(NULL != data && n <= 1048576);
		if (data) {
			memcpy(&header, data, sizeof header);
			CHECK(header.slot == i);
			if (header.generation < minimum) {
				minimum = header.generation;
			}
			if (header.generation > maximum) {
				maximum = header.generation;
			}
			free(data);
		}
	}
	CHECK(maximum >= 5 && maximum - minimum == 2);
	track_root(lab, L"rotation");
}
static void preserves_unknown(P0Lab *lab)
{
	wchar_t root[1024], file[1024];
	HANDLE pin, h;
	DWORD error, wrote;
	WfDiagnostics *log = NULL;
	char before[65], after[65];
	CHECK(p0_lab_path(lab, L"unknown", root, 1024));
	CHECK(wf_private_directory_open(root, 0, &pin, &error));
	CloseHandle(pin);
	CHECK(p0_area_track(lab->area, L"unknown", 1));
	CHECK(p0_lab_path(lab, L"unknown\\keep.txt", file, 1024));
	h = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	CHECK(WriteFile(h, "USER DATA", 9, &wrote, NULL) && 9 == wrote);
	CloseHandle(h);
	CHECK(p0_area_track(lab->area, L"unknown\\keep.txt", 0));
	CHECK(p0_sha256_file(file, before));
	CHECK(!wf_diag_open(root, WF_DIAG_WARNING, 0, &log, &error) && !log);
	CHECK(p0_sha256_file(file, after) && !strcmp(before, after));
}

typedef struct
{
	WfDiagnostics *log;
	unsigned producer;
	LONG failures;
} Producer;
static DWORD WINAPI producer(void *context)
{
	Producer *p = context;
	DWORD error;
	for (unsigned i = 0; i < 50; i++) {
		WfDiagEvent event = {WF_DIAG_INFO, WF_DIAG_LIST, WF_DIAG_OK,
		        (uint64_t)p->producer * 1000 + i, 0, 0, NULL};
		if (!wf_diag_write(p->log, &event, &error)) {
			InterlockedIncrement(&p->failures);
		}
	}
	return 0;
}
static void concurrent_records(P0Lab *lab)
{
	wchar_t root[1024], file[1024];
	WfDiagnostics *log = NULL;
	DWORD error;
	HANDLE threads[4];
	Producer producers[4];
	unsigned char *data;
	size_t n = 0, records = 0;
	CHECK(p0_lab_path(lab, L"threads", root, 1024));
	CHECK(wf_diag_open(root, WF_DIAG_INFO, 0, &log, &error));
	if (!log) {
		return;
	}
	for (unsigned i = 0; i < 4; i++) {
		producers[i] = (Producer){log, i, 0};
		threads[i] =
		        CreateThread(NULL, 0, producer, &producers[i], 0, NULL);
		CHECK(NULL != threads[i]);
	}
	for (unsigned i = 0; i < 4; i++) {
		if (!threads[i]) {
			continue;
		}
		DWORD waited = WaitForSingleObject(threads[i], 30000);
		CHECK(WAIT_OBJECT_0 == waited);
		if (WAIT_OBJECT_0 != waited) {
			/* Keep live contexts; an unfinished run is not a mutant kill. */
			fflush(NULL);
			ExitProcess(2);
		}
		CHECK(0 == producers[i].failures);
		CloseHandle(threads[i]);
	}
	wf_diag_close(log);
	CHECK(p0_lab_path(lab, L"threads\\log0.wfl", file, 1024));
	data = read_all(file, &n);
	CHECK(NULL != data);
	if (data) {
		WfDiagDiskHeader header;
		memcpy(&header, data, sizeof header);
		size_t offset = sizeof header +
		        (size_t)header.path_units * sizeof(wchar_t);
		CHECK(wf_diag_records_valid(data + offset, n - offset));
		while (offset + sizeof(WfDiagDiskRecord) <= n) {
			WfDiagDiskRecord record;
			memcpy(&record, data + offset, sizeof record);
			offset += sizeof record + record.bytes;
			records++;
		}
		CHECK(offset == n && 200 == records);
		free(data);
	}
	track_root(lab, L"threads");
}
static void hardlink_preservation(P0Lab *lab)
{
	wchar_t root[1024], file[1024], alias[1024];
	WfDiagnostics *log = NULL;
	DWORD error;
	char before[65], after[65];
	CHECK(p0_lab_path(lab, L"hardlinks", root, 1024));
	CHECK(wf_diag_open(root, WF_DIAG_WARNING, 0, &log, &error));
	if (!log) {
		return;
	}
	wf_diag_close(log);
	log = NULL;
	CHECK(p0_lab_path(lab, L"hardlinks\\log0.wfl", file, 1024));
	CHECK(p0_lab_path(lab, L"outside-link", alias, 1024));
	CHECK(CreateHardLinkW(alias, file, NULL));
	CHECK(p0_area_track(lab->area, L"outside-link", 0));
	CHECK(p0_sha256_file(file, before));
	CHECK(!wf_diag_open(root, WF_DIAG_WARNING, 0, &log, &error) && !log);
	CHECK(p0_sha256_file(file, after) && !strcmp(before, after));
	track_root(lab, L"hardlinks");
}

static void invalid_utf8_preservation(P0Lab *lab)
{
	wchar_t root[1024], file[1024];
	WfDiagnostics *log = NULL;
	DWORD error, wrote;
	WfDiagEvent event = {
	        WF_DIAG_WARNING, WF_DIAG_STARTUP, WF_DIAG_OK, 0, 0, 0, NULL};
	unsigned char *data;
	size_t size = 0;
	char before[65], after[65];
	HANDLE h;
	CHECK(p0_lab_path(lab, L"utf8", root, 1024));
	CHECK(wf_diag_open(root, WF_DIAG_WARNING, 0, &log, &error));
	if (!log) {
		return;
	}
	CHECK(wf_diag_write(log, &event, &error));
	wf_diag_close(log);
	log = NULL;
	CHECK(p0_lab_path(lab, L"utf8\\log0.wfl", file, 1024));
	data = read_all(file, &size);
	CHECK(NULL != data);
	if (!data) {
		return;
	}
	WfDiagDiskHeader header;
	WfDiagDiskRecord record;
	memcpy(&header, data, sizeof header);
	size_t offset =
	        sizeof header + (size_t)header.path_units * sizeof(wchar_t);
	memcpy(&record, data + offset, sizeof record);
	data[offset + sizeof record] = 0xff;
	record.crc32 =
	        wf_diag_crc32(data + offset + sizeof record, record.bytes);
	memcpy(data + offset, &record, sizeof record);
	h = CreateFileW(file, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	CHECK(WriteFile(h, data, (DWORD)size, &wrote, NULL) && wrote == size);
	CloseHandle(h);
	free(data);
	CHECK(p0_sha256_file(file, before));
	CHECK(!wf_diag_open(root, WF_DIAG_WARNING, 0, &log, &error) && !log);
	if (log) {
		wf_diag_close(log);
	}
	CHECK(p0_sha256_file(file, after) && !strcmp(before, after));
	track_root(lab, L"utf8");
}

static void runtime_unknown_refusal(P0Lab *lab)
{
	wchar_t root[1024], file[1024], unknown[1024];
	WfDiagnostics *log = NULL;
	DWORD error, wrote;
	WfDiagEvent event = {
	        WF_DIAG_WARNING, WF_DIAG_STARTUP, WF_DIAG_OK, 0, 0, 0, NULL};
	unsigned char *before, *after;
	size_t a = 0, b = 0;
	HANDLE h;
	CHECK(p0_lab_path(lab, L"runtime", root, 1024));
	CHECK(wf_diag_open(root, WF_DIAG_WARNING, 0, &log, &error));
	if (!log) {
		return;
	}
	CHECK(wf_diag_write(log, &event, &error));
	CHECK(p0_lab_path(lab, L"runtime\\log0.wfl", file, 1024));
	before = read_all(file, &a);
	CHECK(NULL != before);
	CHECK(p0_lab_path(lab, L"runtime\\adopted.txt", unknown, 1024));
	h = CreateFileW(unknown, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	CHECK(WriteFile(h, "keep", 4, &wrote, NULL) && 4 == wrote);
	CloseHandle(h);
	CHECK(!wf_diag_write(log, &event, &error) &&
	        ERROR_INVALID_DATA == error);
	CHECK(!wf_diag_write(log, &event, &error));
	wf_diag_close(log);
	after = read_all(file, &b);
	CHECK(before && after && a == b && !memcmp(before, after, a));
	free(before);
	free(after);
	track_root(lab, L"runtime");
	CHECK(p0_area_track(lab->area, L"runtime\\adopted.txt", 0));
}

static void sharing_error_is_preserved(P0Lab *lab)
{
	wchar_t root[1024];
	WfDiagnostics *first = NULL, *second = NULL;
	DWORD error;
	WfDiagEvent event = {
	        WF_DIAG_WARNING, WF_DIAG_STARTUP, WF_DIAG_OK, 0, 0, 0, NULL};
	CHECK(p0_lab_path(lab, L"sharing", root, 1024));
	CHECK(wf_diag_open(root, WF_DIAG_WARNING, 0, &first, &error));
	if (!first) {
		return;
	}
	CHECK(!wf_diag_open(root, WF_DIAG_WARNING, 0, &second, &error));
	CHECK(NULL == second && ERROR_SHARING_VIOLATION == error);
	CHECK(wf_diag_write(first, &event, &error));
	wf_diag_close(first);
	wf_diag_close(second);
	track_root(lab, L"sharing");
}
int main(void)
{
	P0Lab lab;
	DWORD h0, h1;
	CHECK(p0_lab_open(&lab, "diag", 0));
	if (!lab.area) {
		return 1;
	}
	/* Windows security/crypto APIs lazily retain process-wide handles. Warm
	 * those APIs once before testing that repeated logger lifetimes leak
	 * none. */
	{
		wchar_t root[1024], file[1024];
		WfDiagnostics *log = NULL;
		DWORD error;
		char hash[65];
		CHECK(p0_lab_path(&lab, L"warmup", root, 1024));
		CHECK(wf_diag_open(root, WF_DIAG_WARNING, 0, &log, &error));
		wf_diag_close(log);
		CHECK(p0_lab_path(&lab, L"warmup\\log0.wfl", file, 1024));
		CHECK(p0_sha256_file(file, hash));
		track_root(&lab, L"warmup");
	}
	CHECK(GetProcessHandleCount(GetCurrentProcess(), &h0));
	privacy_and_crc(&lab);
	path_sanitization_and_rotation(&lab);
	preserves_unknown(&lab);
	concurrent_records(&lab);
	hardlink_preservation(&lab);
	invalid_utf8_preservation(&lab);
	runtime_unknown_refusal(&lab);
	sharing_error_is_preserved(&lab);
	CHECK(GetProcessHandleCount(GetCurrentProcess(), &h1));
	printf("diag handles %lu -> %lu\n", h0, h1);
	CHECK(GetProcessHandleCount(GetCurrentProcess(), &h1) && h0 == h1);
	CHECK(0 == p0_lab_close(&lab, 0 != wf_test_failures));
	return TEST_RESULT();
}
