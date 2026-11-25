/*
 * What the editor's save does when the system fails it. The Win32 calls that
 * move the data are replaced by versions that fail on demand, and the
 * module's own source is compiled into this test. After every injected
 * failure the original must be exactly what it was and no temporary may be
 * left behind; the single failure after which the new text exists only in
 * the temporary must say so and name it.
 */
#include <windows.h>

#include <bcrypt.h>
#include <winioctl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tools/p0/p0_lab.h"
#include "wf_test.h"

static bool g_fail_final_stamp;
static bool g_fail_hash;
static size_t g_largest_allocation;
static void *fault_malloc(size_t n)
{
	if (n > g_largest_allocation) {
		g_largest_allocation = n;
	}
	return n > (1u << 20) ? NULL : malloc(n);
}
static NTSTATUS WINAPI fault_finish_hash(
        BCRYPT_HASH_HANDLE h, PUCHAR bytes, ULONG n, ULONG flags)
{
	return g_fail_hash ? (NTSTATUS)0xC0000001L
	                   : BCryptFinishHash(h, bytes, n, flags);
}
static int g_write_calls;
static int g_fail_write_at; /* 1-based call that fails with disk full */
static bool g_short_write;  /* WriteFile reports fewer bytes than asked */
static bool g_corrupt;      /* the first write stores a wrong byte */
static bool g_fail_flush;
static int g_rename_calls;
static int g_fail_rename_at; /* 1-based rename that fails (0: none) */
static bool g_fail_restore;  /* ... and the one that would undo it fails too */
static DWORD g_rename_error; /* what they fail with */

static BOOL fault_write(
        HANDLE h, LPCVOID data, DWORD n, LPDWORD put, LPOVERLAPPED o)
{
	g_write_calls++;
	if (g_fail_write_at == g_write_calls) {
		SetLastError(ERROR_DISK_FULL);
		return FALSE;
	}
	if (g_short_write) {
		*put = 0;
		SetLastError(0);
		return TRUE;
	}
	if (g_corrupt && 1 == g_write_calls && n) {
		char bad[4096];
		DWORD k = n < sizeof bad ? n : (DWORD)sizeof bad;

		memcpy(bad, data, k);
		bad[0] ^= 0x01;
		return WriteFile(h, bad, k, put, o);
	}
	return WriteFile(h, data, n, put, o);
}

static BOOL fault_flush(HANDLE h)
{
	if (g_fail_flush) {
		SetLastError(ERROR_WRITE_FAULT);
		return FALSE;
	}
	return FlushFileBuffers(h);
}

static BOOL fault_setinfo(
        HANDLE h, FILE_INFO_BY_HANDLE_CLASS cls, LPVOID info, DWORD size)
{
	if (cls == FileRenameInfo) {
		g_rename_calls++;
		if (g_rename_calls == g_fail_rename_at ||
		        (g_fail_restore && g_rename_calls > g_fail_rename_at)) {
			SetLastError(g_rename_error);
			return FALSE;
		}
	}
	return SetFileInformationByHandle(h, cls, info, size);
}

static BOOL fault_info(
        HANDLE h, FILE_INFO_BY_HANDLE_CLASS cls, LPVOID data, DWORD n)
{
	if (g_fail_final_stamp && g_rename_calls >= 2 && cls == FileIdInfo) {
		SetLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	return GetFileInformationByHandleEx(h, cls, data, n);
}
#define GetFileInformationByHandleEx fault_info
#define malloc fault_malloc
#define BCryptFinishHash fault_finish_hash
#define WriteFile fault_write
#define FlushFileBuffers fault_flush
#define SetFileInformationByHandle fault_setinfo
#include "../src/edit/wf_efile_win.c"
#undef SetFileInformationByHandle
#undef FlushFileBuffers
#undef WriteFile
#undef GetFileInformationByHandleEx
#undef malloc
#undef BCryptFinishHash

static P0Lab lab;
static wchar_t dir[1024];

static void reset(void)
{
	g_write_calls = 0;
	g_fail_final_stamp = false;
	g_fail_write_at = 0;
	g_short_write = g_corrupt = g_fail_flush = g_fail_restore = false;
	g_rename_calls = g_fail_rename_at = 0;
	g_rename_error = 0;
}

static size_t temporaries(void)
{
	wchar_t pattern[1100];
	WIN32_FIND_DATAW fd;
	HANDLE f;
	size_t count = 0;

	swprintf(pattern, 1100, L"%ls\\.*.tmp", dir);
	f = FindFirstFileW(pattern, &fd);
	if (INVALID_HANDLE_VALUE == f) {
		return 0;
	}
	do {
		count++;
	} while (FindNextFileW(f, &fd));
	FindClose(f);
	return count;
}

static bool content_is(const wchar_t *path, const char *text)
{
	char buf[256] = "";
	DWORD got = 0;
	HANDLE h = CreateFileW(path, GENERIC_READ,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_ATTRIBUTE_NORMAL, NULL);

	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	ReadFile(h, buf, sizeof buf - 1, &got, NULL);
	CloseHandle(h);
	buf[got] = 0;
	return !strcmp(buf, text);
}

/* A file ready to be saved over, and its stamp. */
static void prepare(wchar_t *path, WfFileStamp *was)
{
	HANDLE h;
	DWORD put, error = 0;
	WfLoaded l;

	swprintf(path, 1100, L"%ls\\target.txt", dir);
	h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	WriteFile(h, "original text", 13, &put, NULL);
	CloseHandle(h);
	CHECK(WF_OK == wf_efile_load(path, 1u << 20, 0, NULL, &l) &&
	        WF_FL_OK == l.verdict);
	*was = l.stamp;
	wf_efile_loaded_free(&l);
	(void)error;
}

static WfSaveResult attempt(const wchar_t *path, const WfFileStamp *was)
{
	WfEncoding utf8 = {WF_ENC_UTF8, 0};
	WfSaveOptions opt = {false};
	WfSaveResult r;

	CHECK(wf_efile_save_over(path, was, "replacement text!", 17, utf8, &opt,
	              &r) == WF_OK);
	return r;
}

static void failures_before_the_swap_leave_the_original_alone(void)
{
	wchar_t path[1100];
	WfFileStamp was;
	WfSaveResult r;

	prepare(path, &was);

	reset();
	g_fail_write_at = 1; /* the disk fills up while writing */
	r = attempt(path, &was);
	CHECK(WF_SV_IO == r.verdict && ERROR_DISK_FULL == r.error &&
	        !r.kept_temp[0]);
	CHECK(content_is(path, "original text") && 0 == temporaries());

	reset();
	g_short_write = true; /* the write reports no progress */
	r = attempt(path, &was);
	CHECK(WF_SV_IO == r.verdict && !r.kept_temp[0]);
	CHECK(content_is(path, "original text") && 0 == temporaries());

	reset();
	g_fail_flush = true; /* the data cannot be made durable */
	r = attempt(path, &was);
	CHECK(WF_SV_IO == r.verdict && !r.kept_temp[0]);
	CHECK(content_is(path, "original text") && 0 == temporaries());

	reset();
	g_corrupt = true; /* the disk stored something else */
	r = attempt(path, &was);
	CHECK(WF_SV_IO == r.verdict && !r.kept_temp[0]);
	CHECK(content_is(path, "original text") && 0 == temporaries());
}

/* Removes what a test provoked, by the same proof the product uses. */
static void remove_kept(const wchar_t *path)
{
	WfFileStamp t;
	HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING, 0, NULL);
	DWORD error;

	CHECK(INVALID_HANDLE_VALUE != h && stamp_of(h, &t, &error));
	CloseHandle(h);
	CHECK(WF_OWNED_DELETED == wf_owned_delete_save_temp(path, &t.id, NULL));
}

static size_t leftovers(void)
{
	wchar_t pattern[1100];
	WIN32_FIND_DATAW fd;
	HANDLE f;
	size_t count = 0;

	swprintf(pattern, 1100, L"%ls\\.*", dir);
	f = FindFirstFileW(pattern, &fd);
	if (INVALID_HANDLE_VALUE == f) {
		return 0;
	}
	do {
		count += wcscmp(fd.cFileName, L".") &&
		        wcscmp(fd.cFileName, L"..");
	} while (FindNextFileW(f, &fd));
	FindClose(f);
	return count;
}

static void a_failed_swap_is_reported_for_what_it_is(void)
{
	wchar_t path[1100];
	WfFileStamp was;
	WfSaveResult r;

	prepare(path, &was);

	/* the original could not be moved aside: nothing changed anywhere */
	reset();
	g_fail_rename_at = 1;
	g_rename_error = ERROR_ACCESS_DENIED;
	r = attempt(path, &was);
	CHECK(WF_SV_IO == r.verdict && ERROR_ACCESS_DENIED == r.error);
	CHECK(!r.kept_temp[0] && !r.kept_original[0] &&
	        content_is(path, "original text") && 0 == leftovers());

	/* the new file could not take the name, and the original was put back */
	reset();
	g_fail_rename_at = 2;
	g_rename_error = ERROR_ACCESS_DENIED;
	r = attempt(path, &was);
	CHECK(WF_SV_IO == r.verdict && !r.kept_temp[0] && !r.kept_original[0]);
	CHECK(content_is(path, "original text") && 0 == leftovers());

	/* the dangerous one: the name is taken in between and the original cannot
	 * be put back either. Both survive, named: the new text in the
	 * temporary, the previous version where it was moved. */
	reset();
	g_fail_rename_at = 2;
	g_fail_restore = true;
	g_rename_error = ERROR_ACCESS_DENIED;
	r = attempt(path, &was);
	CHECK(WF_SV_IO == r.verdict && r.kept_temp[0] && r.kept_original[0]);
	CHECK(content_is(r.kept_temp, "replacement text!"));
	CHECK(content_is(r.kept_original, "original text"));
	CHECK(INVALID_FILE_ATTRIBUTES == GetFileAttributesW(path));
	remove_kept(r.kept_temp);
	remove_kept(r.kept_original);
	CHECK(0 == leftovers());
	/* put the fixture back for the rest */
	prepare(path, &was);
}

static void the_temporary_is_only_removed_when_it_is_the_one_created(void)
{
	wchar_t path[1100], other[1100];
	WfFileStamp was, ostamp;
	FILE_ID_INFO wrong;
	HANDLE h;
	DWORD put, error;

	prepare(path, &was);
	swprintf(other, 1100, L"%ls\\.somebody-elses.tmp", dir);
	h = CreateFileW(other, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	WriteFile(h, "mine", 4, &put, NULL);
	CloseHandle(h);
	h = CreateFileW(other, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0,
	        NULL);
	CHECK(stamp_of(h, &ostamp, &error));
	CloseHandle(h);
	/* a name that holds a different file than the recorded one is kept */
	wrong = was.id;
	CHECK(WF_OWNED_MANUAL ==
	        wf_owned_delete_save_temp(other, &wrong, NULL));
	CHECK(content_is(other, "mine"));
	/* a missing name is not an error */
	{
		wchar_t gone[1100];

		swprintf(gone, 1100, L"%ls\\.never.tmp", dir);
		CHECK(WF_OWNED_ABSENT ==
		        wf_owned_delete_save_temp(gone, &ostamp.id, NULL));
	}
	/* the real owner may remove it */
	CHECK(WF_OWNED_DELETED ==
	        wf_owned_delete_save_temp(other, &ostamp.id, NULL));
	CHECK(0 == temporaries());
}
static void committed_bytes_with_an_unknown_stamp_are_reported(void)
{
	wchar_t path[1100];
	WfFileStamp was;
	WfSaveResult r;
	prepare(path, &was);
	reset();
	g_fail_final_stamp = true;
	r = attempt(path, &was);
	CHECK(r.verdict == WF_SV_COMMITTED_UNVERIFIED && !r.stamp.valid);
	CHECK(r.error == ERROR_ACCESS_DENIED && r.kept_original[0]);
	reset();
	CHECK(content_is(path, "replacement text!"));
	CHECK(content_is(r.kept_original, "original text"));
	remove_kept(r.kept_original);
}

static void changed_size_and_hash_failure_never_start_a_save(void)
{
	wchar_t path[1100];
	WfFileStamp was;
	WfSaveResult r;
	prepare(path, &was);
	HANDLE h = CreateFileW(
	        path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	CHECK(h != INVALID_HANDLE_VALUE);
	if (h == INVALID_HANDLE_VALUE) {
		return;
	}
	/* Only the changed logical size matters; do not reserve 4 GiB of disk. */
	DWORD returned = 0;
	BOOL sparse = DeviceIoControl(
	        h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &returned, NULL);
	CHECK(sparse);
	if (!sparse) {
		CloseHandle(h);
		return;
	}
	LARGE_INTEGER huge;
	huge.QuadPart = 1LL << 32;
	CHECK(SetFilePointerEx(h, huge, NULL, FILE_BEGIN) && SetEndOfFile(h));
	CloseHandle(h);
	reset();
	g_largest_allocation = 0;
	r = attempt(path, &was);
	CHECK(r.verdict == WF_SV_CONFLICT && !g_write_calls && !g_rename_calls);
	CHECK(g_largest_allocation <= 65536);
	prepare(path, &was);
	reset();
	g_fail_hash = true;
	r = attempt(path, &was);
	g_fail_hash = false;
	CHECK(r.verdict == WF_SV_IO && !g_write_calls && !g_rename_calls);
	CHECK(content_is(path, "original text"));
}

static unsigned denied_writes;
static wchar_t late_stream[1200];

static void try_late_write(const wchar_t *path)
{
	HANDLE h = CreateFileW(path, GENERIC_WRITE,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING, 0, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		CHECK(ERROR_SHARING_VIOLATION == GetLastError());
		denied_writes++;
	} else {
		DWORD put = 0;
		/* This succeeds against the old implementation after its final hash. */
		CHECK(WriteFile(h, "X", 1, &put, NULL) && 1 == put);
		CloseHandle(h);
	}
}

static bool late_writes_between_the_renames(
        const wchar_t *dest, const wchar_t *aside)
{
	WIN32_FIND_DATAW fd;
	wchar_t pattern[1100], tmp[1100];
	HANDLE f, stream;
	DWORD put = 0;
	(void)dest;
	try_late_write(aside);
	swprintf(pattern, 1100, L"%ls\\.*.tmp", dir);
	f = FindFirstFileW(pattern, &fd);
	CHECK(INVALID_HANDLE_VALUE != f);
	if (INVALID_HANDLE_VALUE != f) {
		FindClose(f);
		swprintf(tmp, 1100, L"%ls\\%ls", dir, fd.cFileName);
		try_late_write(tmp);
	}
	/* Named streams are independently writable even with a base-stream
	 * guard. The original must survive, including this late addition. */
	swprintf(late_stream, 1200, L"%ls:late", aside);
	stream = CreateFileW(late_stream, GENERIC_WRITE,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        CREATE_NEW, 0, NULL);
	CHECK(INVALID_HANDLE_VALUE != stream);
	if (INVALID_HANDLE_VALUE != stream) {
		CHECK(WriteFile(stream, "late ADS", 8, &put, NULL) && 8 == put);
		CloseHandle(stream);
	}
	return false;
}

static void writes_after_verification_cannot_destroy_data(void)
{
	wchar_t path[1100];
	WfFileStamp was;
	WfSaveResult r;
	prepare(path, &was);
	reset();
	denied_writes = 0;
	wf_efile_test_after_aside = late_writes_between_the_renames;
	r = attempt(path, &was);
	wf_efile_test_after_aside = NULL;
	CHECK(2 == denied_writes);
	CHECK(WF_SV_OK == r.verdict && r.backup_made && r.kept_original[0]);
	CHECK(content_is(path, "replacement text!"));
	CHECK(content_is(r.kept_original, "original text"));
	CHECK(content_is(late_stream, "late ADS"));
	remove_kept(r.kept_original);

	/* A writer already open when the final check runs must also refuse. */
	prepare(path, &was);
	HANDLE writer = CreateFileW(path, GENERIC_WRITE,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING, 0, NULL);
	CHECK(INVALID_HANDLE_VALUE != writer);
	if (INVALID_HANDLE_VALUE != writer) {
		reset();
		r = attempt(path, &was);
		CHECK(WF_SV_IO == r.verdict && !g_rename_calls);
		CloseHandle(writer);
		CHECK(content_is(path, "original text"));
	}
}

int main(void)
{
	CHECK(p0_lab_open(&lab, "efilefaults", 0));
	if (!lab.area) {
		return TEST_RESULT();
	}
	CHECK(p0_area_mkdir(lab.area, L"w"));
	CHECK(p0_lab_path(&lab, L"w", dir, 1024));
	changed_size_and_hash_failure_never_start_a_save();
	failures_before_the_swap_leave_the_original_alone();
	CHECK(p0_area_track(lab.area, L"w\\target.txt", 0));
	a_failed_swap_is_reported_for_what_it_is();
	committed_bytes_with_an_unknown_stamp_are_reported();
	the_temporary_is_only_removed_when_it_is_the_one_created();
	writes_after_verification_cannot_destroy_data();
	CHECK(0 == p0_lab_close(&lab, 0 != wf_test_failures));
	return TEST_RESULT();
}
