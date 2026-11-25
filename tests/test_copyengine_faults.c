/*
 * What the copy engine does when the system fails it. The Win32 calls that
 * move data or give it a name are replaced by versions that fail on demand
 * and the engine's source is compiled into this test. After every injected
 * failure nothing may exist under the destination name that was not verified,
 * the destination that was already there must be exactly as it was, and no
 * temporary may remain: except the one case where the new copy may be the only
 * good one, which must stay and stay in the journal.
 */
#include <windows.h>

#include <bcrypt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tools/p0/p0_common.h"
#include "../tools/p0/p0_lab.h"
#include "wf_test.h"
#include "wf_final_path_fault.h"
#include "xfer/wf_recycle_win.h"
#include "xfer/wf_copyengine_win.h"
#include "path/wf_path.h"

static bool g_mock_bin, g_cancel, g_cancel_after_bin, g_uncertain_bin,
        g_corrupt_ads, g_fail_basic;
static DWORD g_first_stream_error, g_next_stream_error;
static unsigned g_bin_calls;
static WfCopyControl *g_control;
static unsigned g_join_n, g_fail_join_at;
static bool g_observe_nominal;
static bool g_lock_stage_commit;
static bool g_keep_stage_pin;
static HANDLE g_stage_pin = INVALID_HANDLE_VALUE;
static DWORD g_locked_commit_error;
/* Called with the staging folder's path right after a commit attempt failed
 * because of the harness' holder, which has let go again: the moment in which
 * a third party can act before the engine tries once more. */
static void (*g_commit_conflict)(const wchar_t *stage_path);
static P0Lab lab;

static HANDLE observed_create(LPCWSTR path,
        DWORD access,
        DWORD share,
        LPSECURITY_ATTRIBUTES security,
        DWORD creation,
        DWORD flags,
        HANDLE template_file)
{
	HANDLE pin = INVALID_HANDLE_VALUE;
	if (g_lock_stage_commit && (access & DELETE) &&
	        (flags & FILE_FLAG_BACKUP_SEMANTICS) &&
	        wcsstr(path, L".wfcopy-")) {
		g_lock_stage_commit = false;
		/* A real, known harness holder at the publication boundary.
		 * No injected return code, scheduling delay or retry is used. */
		pin = CreateFileW(path, FILE_LIST_DIRECTORY,
		        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
		        FILE_FLAG_BACKUP_SEMANTICS |
		                FILE_FLAG_OPEN_REPARSE_POINT,
		        NULL);
		CHECK(pin != INVALID_HANDLE_VALUE);
	}
	HANDLE h = CreateFileW(
	        path, access, share, security, creation, flags, template_file);
	DWORD error = h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
	if (pin != INVALID_HANDLE_VALUE) {
		g_locked_commit_error = error;
		FILE_ID_INFO id;
		CHECK(GetFileInformationByHandleEx(
		        pin, FileIdInfo, &id, sizeof id));
		printf("controlled holder: owner=test_harness stage=publish "
		       "api=CreateFileW access=%lu share=%lu creation=%lu "
		       "flags=%lu win32=%lu tid=%lu tick=%llu\n",
		        access, share, creation, flags, error,
		        GetCurrentThreadId(),
		        (unsigned long long)GetTickCount64());
		if (g_keep_stage_pin) {
			g_stage_pin = pin;
		} else {
			CloseHandle(pin); /* permit identity-checked rollback */
			if (g_commit_conflict) {
				g_commit_conflict(path);
			}
		}
	}
	if (h == INVALID_HANDLE_VALUE && g_observe_nominal) {
		size_t handles = 0;
		DWORD snapshot = p0_area_open_handles(lab.area, &handles);
		printf("nominal folder: api=CreateFileW access=%lu share=%lu "
		       "creation=%lu flags=%lu staging=%d win32=%lu "
		       "owned_handles=%zu snapshot=%lu tid=%lu tick=%llu\n",
		        access, share, creation, flags,
		        wcsstr(path, L".wfcopy-") != NULL, error, handles,
		        snapshot, GetCurrentThreadId(),
		        (unsigned long long)GetTickCount64());
		SetLastError(error);
	}
	if (h == INVALID_HANDLE_VALUE) {
		SetLastError(error);
	}
	return h;
}
static WfStatus fault_join(const wchar_t *a, const wchar_t *b, wchar_t **out)
{
	if (++g_join_n == g_fail_join_at) {
		*out = NULL;
		return WF_E_NOMEM;
	}
	return wf_path_join(a, b, out);
}
static bool mock_bin_init(void)
{
	return g_mock_bin || wf_recycle_thread_init();
}
static void mock_bin_done(void)
{
	if (!g_mock_bin) {
		wf_recycle_thread_done();
	}
}
static WfStatus mock_bin(const wchar_t *path,
        const WfFileId *id,
        const WfFileId *parent,
        WfRecycleReport *report)
{
	if (!g_mock_bin) {
		return wf_recycle(path, id, parent, report);
	}
	g_bin_calls++;
	report->attempted = true;
	g_cancel = g_cancel_after_bin;
	if (g_uncertain_bin) {
		report->error = ERROR_GEN_FAILURE;
		return WF_E_OS;
	}
	return WF_OK;
}
static WfStatus mock_guarded_bin(const wchar_t *path,
        const WfFileId *id,
        const WfFileId *parent,
        bool (*ready)(void *),
        void *context,
        WfRecycleReport *report)
{
	if (!g_mock_bin) {
		return wf_recycle_guarded(
		        path, id, parent, ready, context, report);
	}
	if (ready && !ready(context)) {
		report->error = ERROR_RETRY;
		return WF_E_INVAL;
	}
	return mock_bin(path, id, parent, report);
}
static HANDLE fault_stream_first(
        LPCWSTR path, STREAM_INFO_LEVELS level, LPVOID data, DWORD flags)
{
	if (g_first_stream_error) {
		SetLastError(g_first_stream_error);
		return INVALID_HANDLE_VALUE;
	}
	return FindFirstStreamW(path, level, data, flags);
}
static BOOL fault_stream_next(HANDLE h, LPVOID data)
{
	if (g_next_stream_error) {
		SetLastError(g_next_stream_error);
		return FALSE;
	}
	return FindNextStreamW(h, data);
}
static int g_writes;
static int g_fail_write_at; /* 1-based WriteFile call that fails: disk full */
static int g_corrupt_at; /* 1-based WriteFile call that stores a wrong byte */
static bool g_fail_flush;
static DWORD g_rename_error;     /* FileRenameInfo fails with this */
static DWORD g_rename_dir_error; /* ... only when the object is a folder */
static int g_ren_calls;          /* FileRenameInfo calls so far */
static int g_ren_fail_at;        /* 1-based one that fails (0: none) */
static bool g_ren_fail_after;    /* ... and every one after it */

static BOOL fault_write(
        HANDLE h, LPCVOID data, DWORD n, LPDWORD put, LPOVERLAPPED o)
{
	g_writes++;
	wchar_t path[2048];
	DWORD len =
	        GetFinalPathNameByHandleW(h, path, 2048, FILE_NAME_NORMALIZED);
	bool ads = g_corrupt_ads && len && len < 2048 && wcsstr(path, L":meta");
	if (g_fail_write_at == g_writes) {
		SetLastError(ERROR_DISK_FULL);
		return FALSE;
	}
	if ((g_corrupt_at == g_writes || ads) && n) {
		unsigned char *bad = malloc(n);
		BOOL ok;

		memcpy(bad, data, n);
		bad[n / 2] ^= 0x10;
		ok = WriteFile(h, bad, n, put, o);
		free(bad);
		return ok;
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

static BOOL fault_set_info(
        HANDLE h, FILE_INFO_BY_HANDLE_CLASS cls, LPVOID info, DWORD size)
{
	if (g_fail_basic && FileBasicInfo == cls) {
		SetLastError(ERROR_WRITE_FAULT);
		return FALSE;
	}
	if (cls == FileRenameInfo) {
		g_ren_calls++;
		if (g_ren_fail_at &&
		        (g_ren_calls == g_ren_fail_at ||
		                (g_ren_fail_after &&
		                        g_ren_calls > g_ren_fail_at))) {
			SetLastError(ERROR_ACCESS_DENIED);
			return FALSE;
		}
	}
	if (cls == FileRenameInfo && g_rename_error) {
		SetLastError(g_rename_error);
		return FALSE;
	}
	if (cls == FileRenameInfo && g_rename_dir_error) {
		FILE_STANDARD_INFO st;

		if (GetFileInformationByHandleEx(
		            h, FileStandardInfo, &st, sizeof st) &&
		        st.Directory) {
			SetLastError(g_rename_dir_error);
			return FALSE;
		}
	}
	BOOL ok = SetFileInformationByHandle(h, cls, info, size);
	if (!ok && g_observe_nominal) {
		DWORD error = GetLastError();
		printf("nominal folder: api=SetFileInformationByHandle "
		       "class=%d "
		       "win32=%lu tid=%lu tick=%llu\n",
		        (int)cls, error, GetCurrentThreadId(),
		        (unsigned long long)GetTickCount64());
		SetLastError(error);
	}
	return ok;
}

static int g_find_calls;
static int g_find_fail_at; /* 1-based FindNextFileW call that fails (0: none) */

/* A listing that stops with an error instead of the end of the folder. */
static BOOL fault_find_next(HANDLE h, LPWIN32_FIND_DATAW fd)
{
	g_find_calls++;
	if (g_find_fail_at && g_find_calls >= g_find_fail_at) {
		SetLastError(ERROR_GEN_FAILURE);
		return FALSE;
	}
	return FindNextFileW(h, fd);
}

#define wf_path_join fault_join
#define CreateFileW observed_create
#define wf_recycle_guarded mock_guarded_bin
#define wf_recycle mock_bin
#define wf_recycle_thread_init mock_bin_init
#define wf_recycle_thread_done mock_bin_done
#define FindFirstStreamW fault_stream_first
#define FindNextStreamW fault_stream_next
#define FindNextFileW fault_find_next
#define WriteFile fault_write
#define FlushFileBuffers fault_flush
#define SetFileInformationByHandle fault_set_info
#define WF_COPYENGINE_QUARANTINE_TEST
#define GetFinalPathNameByHandleW fault_final_path
#include "../src/xfer/wf_copyengine_win.c"
#undef GetFinalPathNameByHandleW
#undef SetFileInformationByHandle
#undef CreateFileW
#undef FlushFileBuffers
#undef WriteFile
#undef FindNextFileW
#undef FindFirstStreamW
#undef FindNextStreamW
#undef wf_recycle
#undef wf_recycle_guarded
#undef wf_path_join
#undef wf_recycle_thread_init
#undef wf_recycle_thread_done

#include "xfer/wf_probe_win.h"

static WfCopyKind g_kind = WF_CJ_COPY;

static wchar_t root[1024], src_dir[1024], dst_dir[1024], journal[1100];
static WfProbeOps ops;

static void reset(void)
{
	g_writes = g_fail_write_at = g_corrupt_at = 0;
	g_fail_flush = false;
	g_rename_error = g_rename_dir_error = 0;
	g_ren_calls = g_ren_fail_at = 0;
	g_ren_fail_after = false;
	g_find_calls = g_find_fail_at = 0;
	g_mock_bin = g_cancel = g_cancel_after_bin = g_corrupt_ads =
	        g_fail_basic = false;
	g_first_stream_error = g_next_stream_error = 0;
	g_bin_calls = 0;
	g_control = NULL;
	g_join_n = g_fail_join_at = 0;
	g_lock_stage_commit = false;
	g_locked_commit_error = 0;
	g_keep_stage_pin = false;
	g_commit_conflict = NULL;
	CHECK(g_stage_pin == INVALID_HANDLE_VALUE);
}

static bool put(const wchar_t *path, const char *data, size_t n)
{
	HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD w = 0;
	bool ok;

	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	ok = !n || (WriteFile(h, data, (DWORD)n, &w, NULL) && w == n);
	CloseHandle(h);
	return ok;
}

static bool content_is(const wchar_t *path, const char *text)
{
	char buf[256] = "";
	DWORD got = 0;
	HANDLE h = CreateFileW(path, GENERIC_READ,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0,
	        NULL);

	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	ReadFile(h, buf, sizeof buf - 1, &got, NULL);
	CloseHandle(h);
	buf[got] = 0;
	return !strcmp(buf, text);
}

static bool exists(const wchar_t *path)
{
	return INVALID_FILE_ATTRIBUTES != GetFileAttributesW(path);
}

static size_t temporaries(void)
{
	wchar_t pattern[1100];
	WIN32_FIND_DATAW fd;
	HANDLE f;
	size_t n = 0;

	swprintf(pattern, 1100, L"%ls\\.wfcopy-*", dst_dir);
	f = FindFirstFileW(pattern, &fd);
	if (INVALID_HANDLE_VALUE == f) {
		return 0;
	}
	do {
		n++;
	} while (FindNextFileW(f, &fd));
	FindClose(f);
	return n;
}

static WfCopyResult attempt(
        const wchar_t *name, WfCollide collide, WfCopyVerify verify)
{
	const wchar_t *names[1];
	WfCopySpec spec;
	WfCopyResult r;
	WfCopyJournal *j = wf_cj_open(journal);
	WfNodeInfo d;

	names[0] = name;
	memset(&spec, 0, sizeof spec);
	wf_slot_init(&spec.slot);
	CHECK(wf_xfer_capture(&ops, WF_XFER_COPY, src_dir, names, 1, 0,
	              &spec.slot) == WF_OK);
	spec.dest = _wcsdup(dst_dir);
	CHECK(WF_OK == ops.node(NULL, dst_dir, &d));
	spec.dest_id = d.id;
	spec.kind = g_kind;
	spec.verify = verify;
	spec.collide = collide;
	CHECK(WF_OK == wf_copyengine_run(&spec, g_control, j, &r));
	wf_cj_close(j);
	wf_copyspec_free(&spec);
	return r;
}

static void purge(const wchar_t *dir);
static bool control_go(void *ctx);

/* The first file of the folder under test, inside its staging folder. */
static void conflict_append_to_staged_file(const wchar_t *stage_path)
{
	wchar_t file[1200];
	HANDLE h;
	DWORD put = 0;

	swprintf(file, 1200, L"%ls\\a.txt", stage_path);
	h = CreateFileW(file, FILE_APPEND_DATA,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	if (INVALID_HANDLE_VALUE != h) {
		CHECK(WriteFile(h, "X", 1, &put, NULL) && 1 == put);
		CloseHandle(h);
	}
}

static void conflict_cancel(const wchar_t *stage_path)
{
	(void)stage_path;
	g_cancel = true;
}

static void conflict_take_final_name(const wchar_t *stage_path)
{
	wchar_t folder[1100], file[1200];

	(void)stage_path;
	swprintf(folder, 1100, L"%ls\\T", dst_dir);
	swprintf(file, 1200, L"%ls\\foreign.txt", folder);
	CHECK(CreateDirectoryW(folder, NULL));
	CHECK(put(file, "KEEP", 4));
}

static void failures_leave_no_trace_under_the_name(void)
{
	wchar_t src[1100], dst[1100];
	WfCopyResult r;
	static char big[3 * 1024 * 1024];

	memset(big, 'q', sizeof big);
	swprintf(src, 1100, L"%ls\\f.bin", src_dir);
	swprintf(dst, 1100, L"%ls\\f.bin", dst_dir);
	{
		HANDLE h = CreateFileW(
		        src, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
		DWORD w;

		WriteFile(h, big, sizeof big, &w, NULL);
		CloseHandle(h);
	}

	reset();
	g_fail_write_at = 2; /* the disk fills up part-way */
	r = attempt(L"f.bin", WF_COLLIDE_STOP, WF_VERIFY_SHA256);
	CHECK(0 == r.files_copied && WF_CF_NO_SPACE == r.failures[0].kind);
	CHECK(!exists(dst) && 0 == temporaries());
	wf_copyresult_free(&r);

	reset();
	g_corrupt_at = 2; /* the disk stores something else */
	r = attempt(L"f.bin", WF_COLLIDE_STOP, WF_VERIFY_SHA256);
	CHECK(0 == r.files_copied && WF_CF_VERIFY_FAILED == r.failures[0].kind);
	CHECK(!exists(dst) && 0 == temporaries());
	wf_copyresult_free(&r);

	reset();
	g_corrupt_at = 2; /* the same with the keyed method */
	r = attempt(L"f.bin", WF_COLLIDE_STOP, WF_VERIFY_HMAC_SHA256);
	CHECK(0 == r.files_copied && WF_CF_VERIFY_FAILED == r.failures[0].kind);
	CHECK(!exists(dst) && 0 == temporaries());
	wf_copyresult_free(&r);

	reset();
	g_fail_flush = true; /* cannot be made durable */
	r = attempt(L"f.bin", WF_COLLIDE_STOP, WF_VERIFY_SHA256);
	CHECK(0 == r.files_copied && WF_CF_WRITE_FAILED == r.failures[0].kind);
	CHECK(!exists(dst) && 0 == temporaries());
	wf_copyresult_free(&r);

	reset();
	g_rename_error = ERROR_ACCESS_DENIED; /* cannot be given its name */
	r = attempt(L"f.bin", WF_COLLIDE_STOP, WF_VERIFY_SHA256);
	CHECK(0 == r.files_copied && WF_CF_COMMIT_FAILED == r.failures[0].kind);
	CHECK(!exists(dst) && 0 == temporaries());
	wf_copyresult_free(&r);

	reset();
	g_corrupt_at = 1; /* with verification off a bad write is not noticed:
	                     the plan says so, and the test pins that down */
	r = attempt(L"f.bin", WF_COLLIDE_STOP, WF_VERIFY_NONE);
	CHECK(1 ==
	        r.files_copied); /* the configured opt-out, taken at its word */
	CHECK(exists(dst));
	wf_copyresult_free(&r);
}

/* A folder appears under its name whole or not at all, and nothing of an
 * unfinished one is left behind. */
static void a_folder_is_all_or_nothing(void)
{
	wchar_t p[1100], q[1100];
	WfCopyResult r;

	swprintf(p, 1100, L"%ls\\T", src_dir);
	CHECK(CreateDirectoryW(p, NULL));
	swprintf(p, 1100, L"%ls\\T\\sub", src_dir);
	CHECK(CreateDirectoryW(p, NULL));
	swprintf(p, 1100, L"%ls\\T\\a.txt", src_dir);
	CHECK(put(p, "AAA", 3));
	swprintf(p, 1100, L"%ls\\T\\sub\\b.txt", src_dir);
	CHECK(put(p, "BBBB", 4));
	swprintf(q, 1100, L"%ls\\T", dst_dir);

	reset(); /* the folder cannot be given its name at the end */
	g_rename_dir_error = ERROR_ACCESS_DENIED;
	r = attempt(L"T", WF_COLLIDE_STOP, WF_VERIFY_SHA256);
	CHECK(WF_CF_COMMIT_FAILED == r.failures[0].kind);
	CHECK(0 == r.files_copied && 0 == r.dirs_created && 0 == r.bytes);
	CHECK(!exists(q) && 0 == temporaries() && 0 == r.created_n);
	wf_copyresult_free(&r);

	reset(); /* a sharing violation at commit is not success or permission to overwrite */
	g_rename_dir_error = ERROR_SHARING_VIOLATION;
	r = attempt(L"T", WF_COLLIDE_STOP, WF_VERIFY_SHA256);
	CHECK(r.failure_total > 0 &&
	        r.failures[0].kind == WF_CF_COMMIT_FAILED &&
	        r.failures[0].error == ERROR_SHARING_VIOLATION);
	CHECK(r.files_copied == 0 && r.dirs_created == 0 && r.created_n == 0);
	CHECK(!exists(q) && temporaries() == 0);
	swprintf(p, 1100, L"%ls\\T\\a.txt", src_dir);
	CHECK(content_is(p, "AAA"));
	swprintf(p, 1100, L"%ls\\T\\sub\\b.txt", src_dir);
	CHECK(content_is(p, "BBBB"));
	wf_copyresult_free(&r);

	/* A real NTFS sharing conflict at the directory open that PASSES: the
	 * holder lets go at once, as a scanner or an indexer does. The commit is
	 * tried again, so the verified folder is published whole instead of
	 * being taken apart; its content and the source are checked here. */
	reset();
	char source_hash[P0_HASH_HEX], after_hash[P0_HASH_HEX];
	CHECK(p0_sha256_file(p, source_hash));
	g_lock_stage_commit = true;
	r = attempt(L"T", WF_COLLIDE_STOP, WF_VERIFY_SHA256);
	CHECK(!g_lock_stage_commit &&
	        g_locked_commit_error == ERROR_SHARING_VIOLATION);
	CHECK(0 == r.failure_total && 2 == r.files_copied &&
	        2 == r.dirs_created);
	CHECK(exists(q) && temporaries() == 0);
	swprintf(p, 1100, L"%ls\\T\\a.txt", dst_dir);
	CHECK(content_is(p, "AAA"));
	swprintf(p, 1100, L"%ls\\T\\sub\\b.txt", dst_dir);
	CHECK(content_is(p, "BBBB"));
	swprintf(p, 1100, L"%ls\\T\\sub\\b.txt", src_dir);
	CHECK(p0_sha256_file(p, after_hash));
	CHECK(strcmp(source_hash, after_hash) == 0);
	wf_copyresult_free(&r);
	purge(dst_dir); /* this test's own published copy */
	CHECK(!exists(q) && temporaries() == 0);

	/* The wait before a renewed attempt is a window for a third party. The
	 * staged tree must be shown to be what was verified, the name must not be
	 * taken over, and a cancel must be answered. Nothing may be published in
	 * any of these; the source stays as it was. */
	reset(); /* a writer appends to a verified, staged file */
	g_commit_conflict = conflict_append_to_staged_file;
	g_lock_stage_commit = true;
	r = attempt(L"T", WF_COLLIDE_STOP, WF_VERIFY_SHA256);
	CHECK(!g_lock_stage_commit &&
	        g_locked_commit_error == ERROR_SHARING_VIOLATION);
	CHECK(r.failure_total > 0 &&
	        r.failures[0].kind == WF_CF_VERIFY_FAILED &&
	        r.failures[0].error == ERROR_INVALID_DATA);
	CHECK(r.files_copied == 0 && r.dirs_created == 0 && !exists(q) &&
	        temporaries() == 0);
	swprintf(p, 1100, L"%ls\\T\\sub\\b.txt", src_dir);
	CHECK(p0_sha256_file(p, after_hash));
	CHECK(strcmp(source_hash, after_hash) == 0);
	wf_copyresult_free(&r);
	purge(dst_dir);

	reset(); /* the user cancels while the engine waits to try again */
	{
		WfCopyControl ctl = {NULL, control_go, NULL};

		g_control = &ctl;
		g_commit_conflict = conflict_cancel;
		g_lock_stage_commit = true;
		r = attempt(L"T", WF_COLLIDE_STOP, WF_VERIFY_SHA256);
	}
	CHECK(!g_lock_stage_commit && r.state == WF_CS_CANCELLED);
	CHECK(r.failure_total == 0 && r.files_copied == 0 && !exists(q) &&
	        temporaries() == 0); /* a cancel is not a failed commit */
	swprintf(p, 1100, L"%ls\\T\\sub\\b.txt", src_dir);
	CHECK(p0_sha256_file(p, after_hash));
	CHECK(strcmp(source_hash, after_hash) == 0);
	wf_copyresult_free(&r);
	purge(dst_dir);

	reset(); /* another folder takes the final name during the wait */
	g_commit_conflict = conflict_take_final_name;
	g_lock_stage_commit = true;
	r = attempt(L"T", WF_COLLIDE_STOP, WF_VERIFY_SHA256);
	CHECK(!g_lock_stage_commit && r.failure_total > 0 &&
	        r.failures[0].kind == WF_CF_EXISTS);
	CHECK(r.files_copied == 0 && temporaries() == 0);
	{
		wchar_t foreign
		        [1200]; /* not `p`: the cases below still use it */

		swprintf(foreign, 1200, L"%ls\\T\\foreign.txt", dst_dir);
		CHECK(content_is(
		        foreign, "KEEP")); /* never replaced or merged */
	}
	wf_copyresult_free(&r);
	purge(dst_dir);
	swprintf(p, 1100, L"%ls\\T\\sub\\b.txt", src_dir); /* as it was */

	reset(); /* a persistent real holder must be reported, never hidden */
	g_lock_stage_commit = g_keep_stage_pin = true;
	r = attempt(L"T", WF_COLLIDE_STOP, WF_VERIFY_SHA256);
	CHECK(g_stage_pin != INVALID_HANDLE_VALUE);
	CHECK(g_locked_commit_error == ERROR_SHARING_VIOLATION);
	CHECK(r.files_copied == 0 && r.dirs_created == 0 && !exists(q));
	bool staging_left = false;
	for (size_t i = 0; i < r.failure_total && i < WF_CJ_FAILURES; i++) {
		if (r.failures[i].kind == WF_CF_STAGING_LEFT) {
			staging_left = true;
		}
	}
	CHECK(staging_left && temporaries() == 1);
	CHECK(p0_sha256_file(p, after_hash));
	CHECK(strcmp(source_hash, after_hash) == 0);
	if (g_stage_pin != INVALID_HANDLE_VALUE) {
		CloseHandle(g_stage_pin);
		g_stage_pin = INVALID_HANDLE_VALUE;
	}
	wf_copyresult_free(&r);
	purge(dst_dir); /* only this test's owned, no-longer-pinned staging */
	CHECK(temporaries() == 0);

	reset(); /* a file inside fails half way */
	g_fail_write_at = 2;
	r = attempt(L"T", WF_COLLIDE_STOP, WF_VERIFY_SHA256);
	CHECK(r.failure_total >= 1 && 0 == r.files_copied &&
	        0 == r.dirs_created);
	CHECK(!exists(q) && 0 == temporaries());
	wf_copyresult_free(&r);

	reset(); /* nothing wrong: the whole folder, and no staging left */
	g_observe_nominal = true;
	r = attempt(L"T", WF_COLLIDE_STOP, WF_VERIFY_SHA256);
	g_observe_nominal = false;
	if (r.failure_total) {
		printf("folder commit: failure_total=%zu first_kind=%d "
		       "win32=%lu\n",
		        r.failure_total, (int)r.failures[0].kind,
		        r.failures[0].error);
	}
	CHECK(2 == r.files_copied && 2 == r.dirs_created &&
	        0 == r.failure_total);
	swprintf(p, 1100, L"%ls\\T\\sub\\b.txt", dst_dir);
	CHECK(content_is(p, "BBBB") && 0 == temporaries());
	wf_copyresult_free(&r);
	/* the name is taken now: refused before anything is made, never merged */
	r = attempt(L"T", WF_COLLIDE_STOP, WF_VERIFY_SHA256);
	CHECK(WF_CF_EXISTS == r.failures[0].kind && 0 == r.files_copied &&
	        0 == temporaries());
	wf_copyresult_free(&r);
	r = attempt(L"T", WF_COLLIDE_OVERWRITE, WF_VERIFY_SHA256);
	CHECK(WF_CF_EXISTS == r.failures[0].kind && 0 == temporaries());
	wf_copyresult_free(&r);
	purge(dst_dir);
}

static void empty_and_many_file_folders(void)
{
	wchar_t source[1100], destination[1100];
	WfCopyResult r;
	swprintf(source, 1100, L"%ls\\empty", src_dir);
	CHECK(CreateDirectoryW(source, NULL));
	reset();
	r = attempt(L"empty", WF_COLLIDE_STOP, WF_VERIFY_SHA256);
	CHECK(r.failure_total == 0 && r.files_copied == 0 &&
	        r.dirs_created == 1 && temporaries() == 0);
	wf_copyresult_free(&r);
	swprintf(destination, 1100, L"%ls\\empty", dst_dir);
	CHECK(exists(destination));

	swprintf(source, 1100, L"%ls\\many", src_dir);
	CHECK(CreateDirectoryW(source, NULL));
	for (unsigned i = 0; i < 64; i++) {
		char payload[64];
		int n = snprintf(payload, sizeof payload, "fixture-%u", i);
		swprintf(source, 1100, L"%ls\\many\\%u.txt", src_dir, i);
		CHECK(n > 0 && put(source, payload, (size_t)n));
	}
	reset();
	r = attempt(L"many", WF_COLLIDE_STOP, WF_VERIFY_SHA256);
	CHECK(r.failure_total == 0 && r.files_copied == 64 &&
	        r.dirs_created == 1 && temporaries() == 0);
	wf_copyresult_free(&r);
	for (unsigned i = 0; i < 64; i++) {
		char source_hash[P0_HASH_HEX], destination_hash[P0_HASH_HEX];
		char payload[64];
		snprintf(payload, sizeof payload, "fixture-%u", i);
		swprintf(source, 1100, L"%ls\\many\\%u.txt", src_dir, i);
		swprintf(destination, 1100, L"%ls\\many\\%u.txt", dst_dir, i);
		CHECK(content_is(source, payload));
		CHECK(p0_sha256_file(source, source_hash));
		CHECK(p0_sha256_file(destination, destination_hash));
		CHECK(strcmp(source_hash, destination_hash) == 0);
	}
	purge(dst_dir);
}

static void a_failed_replacement_keeps_the_old_file(void)
{
	wchar_t src[1100], dst[1100];
	WfCopyResult r;
	WfCopyJournalInfo rec;

	swprintf(src, 1100, L"%ls\\r.txt", src_dir);
	swprintf(dst, 1100, L"%ls\\r.txt", dst_dir);
	CHECK(put(src, "NEW DATA", 8));
	CHECK(put(dst, "OLD", 3));

	/* the old file could not be moved aside: nothing changed anywhere */
	reset();
	g_ren_fail_at = 1;
	r = attempt(L"r.txt", WF_COLLIDE_OVERWRITE, WF_VERIFY_SHA256);
	CHECK(0 == r.files_copied && WF_CF_COMMIT_FAILED == r.failures[0].kind);
	CHECK(content_is(dst, "OLD") && 0 == temporaries());
	wf_copyresult_free(&r);

	/* the temporary could not take the name: the old file is put back */
	reset();
	g_ren_fail_at = 2;
	r = attempt(L"r.txt", WF_COLLIDE_OVERWRITE, WF_VERIFY_SHA256);
	CHECK(0 == r.files_copied && WF_CF_COMMIT_FAILED == r.failures[0].kind);
	CHECK(content_is(dst, "OLD") && 0 == temporaries());
	wf_copyresult_free(&r);

	/* the dangerous failure: the copy could not take the name and the old
	 * file could not be put back. Both survive under hidden names the journal
	 * owns (the new data in the temporary, the old data beside it). */
	reset();
	g_ren_fail_at = 2;
	g_ren_fail_after = true;
	r = attempt(L"r.txt", WF_COLLIDE_OVERWRITE, WF_VERIFY_SHA256);
	CHECK(0 == r.files_copied && WF_CF_COMMIT_FAILED == r.failures[0].kind);
	CHECK(INVALID_FILE_ATTRIBUTES == GetFileAttributesW(dst) &&
	        2 == temporaries());
	wf_copyresult_free(&r);
	/* The journal still owns them. This process is the writer and is alive,
	 * so recovery leaves them (the killed-writer case is in
	 * test_copyengine_win); the fixture removes them by hand. */
	CHECK(wf_cj_inspect(journal, &rec));
	CHECK(rec.records > 0 && 2 == temporaries());
	reset();
	purge(dst_dir);
	CHECK(put(dst, "OLD", 3));

	/* a normal replacement still works afterwards */
	reset();
	r = attempt(L"r.txt", WF_COLLIDE_OVERWRITE, WF_VERIFY_SHA256);
	CHECK(1 == r.files_copied && 1 == r.overwritten &&
	        content_is(dst, "NEW DATA"));
	wf_copyresult_free(&r);
}

/* A listing that ends in an error is not a folder that ended: the copy is not
 * whole, nothing is kept under the name, and a move leaves the source alone. */
static void a_listing_that_fails_is_not_a_finished_folder(void)
{
	wchar_t tree[1100], f1[1100], f2[1100], f3[1100], made[1100];
	int pass;

	for (pass = 0; pass < 2; pass++) {
		WfCopyResult r;

		purge(src_dir);
		purge(dst_dir);
		swprintf(tree, 1100, L"%ls\\tree", src_dir);
		swprintf(f1, 1100, L"%ls\\a.txt", tree);
		swprintf(f2, 1100, L"%ls\\b.txt", tree);
		swprintf(f3, 1100, L"%ls\\c.txt", tree);
		swprintf(made, 1100, L"%ls\\tree", dst_dir);
		CHECK(CreateDirectoryW(tree, NULL));
		CHECK(put(f1, "aaa", 3) && put(f2, "bbb", 3) &&
		        put(f3, "ccc", 3));
		g_kind = pass ? WF_CJ_MOVE_ACROSS : WF_CJ_COPY;
		reset();
		g_find_fail_at =
		        2; /* "." and ".." come first: it dies mid-way */
		r = attempt(L"tree", WF_COLLIDE_STOP, WF_VERIFY_NONE);
		g_kind = WF_CJ_COPY;
		CHECK(r.failure_total >= 1 &&
		        WF_CF_SOURCE_UNREADABLE == r.failures[0].kind);
		CHECK(ERROR_GEN_FAILURE == r.failures[0].error);
		CHECK(!exists(made)); /* no half folder under the name */
		CHECK(0 == r.sources_recycled);
		CHECK(exists(f1) && exists(f2) && exists(f3));
		CHECK(0 == temporaries());
		wf_copyresult_free(&r);
	}
	reset();
}

static bool control_go(void *ctx)
{
	(void)ctx;
	return !g_cancel;
}

static unsigned g_change, g_write_error;
static void changed_after_copy(
        void *ctx, size_t files, uint64_t bytes, const wchar_t *name)
{
	wchar_t path[1200];
	(void)ctx;
	(void)bytes;
	(void)name;
	if (!files || !g_change) {
		return;
	}
	if (g_change == 4) {
		wchar_t aside[1200];
		swprintf(path, 1200, L"%ls\\ads.bin", src_dir);
		swprintf(aside, 1200, L"%ls\\previous.bin", src_dir);
		CHECK(MoveFileW(path, aside));
		CHECK(put(path, "replaced", 8));
		g_change = 0;
		return;
	}
	swprintf(path, 1200, L"%ls\\ads.bin%ls", src_dir,
	        g_change == 1           ? L""
	                : g_change == 2 ? L":meta"
	                                : L":added");
	HANDLE h = CreateFileW(path, GENERIC_WRITE,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        g_change == 3 ? CREATE_NEW : OPEN_EXISTING, 0, NULL);
	g_write_error = h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
	if (h != INVALID_HANDLE_VALUE) {
		DWORD put_n;
		WriteFile(h, "changed", 7, &put_n, NULL);
		CloseHandle(h);
	}
	g_change = 0;
}

static void stream_and_recycle_contracts(void)
{
	wchar_t src[1200], ads[1200], made[1200];
	WfCopyResult r;
	WfCopyControl ctl = {NULL, control_go, changed_after_copy};
	purge(src_dir);
	purge(dst_dir);
	swprintf(src, 1200, L"%ls\\ads.bin", src_dir);
	swprintf(ads, 1200, L"%ls:meta", src);
	swprintf(made, 1200, L"%ls\\ads.bin", dst_dir);
	CHECK(put(src, "main", 4) && put(ads, "stream", 6));
	for (unsigned fault = 0; fault < 4; fault++) {
		reset();
		if (fault == 0) {
			g_first_stream_error = ERROR_ACCESS_DENIED;
		}
		if (fault == 1) {
			g_next_stream_error = ERROR_IO_DEVICE;
		}
		if (fault == 2) {
			g_corrupt_ads = true;
		}
		if (fault == 3) {
			g_fail_basic = true;
		}
		r = attempt(L"ads.bin", WF_COLLIDE_STOP, WF_VERIFY_SHA256);
		CHECK(!r.files_copied && r.files_failed == 1 &&
		        r.failure_total && !exists(made));
		if (fault == 0) {
			CHECK(r.failures[0].error == ERROR_ACCESS_DENIED);
		}
		if (fault == 1) {
			CHECK(r.failures[0].error == ERROR_IO_DEVICE);
		}
		if (fault == 3) {
			CHECK(r.failures[0].kind == WF_CF_METADATA_FAILED);
		}
		wf_copyresult_free(&r);
	}
	reset();
	g_fail_join_at = 1;
	r = attempt(L"ads.bin", WF_COLLIDE_STOP, WF_VERIFY_SHA256);
	CHECK(r.files_failed == 1 && r.failure_total == 1 &&
	        r.failures[0].error == ERROR_NOT_ENOUGH_MEMORY);
	wf_copyresult_free(&r);
	reset();
	r = attempt(L"ads.bin", WF_COLLIDE_STOP, WF_VERIFY_SHA256);
	CHECK(r.files_copied == 1 && r.streams_kept == 1 && !r.failure_total);
	swprintf(ads, 1200, L"%ls:meta", made);
	CHECK(content_is(ads, "stream"));
	wf_copyresult_free(&r);
	purge(dst_dir);

	reset();
	g_kind = WF_CJ_RECYCLE;
	g_mock_bin = true;
	g_control = &ctl;
	g_cancel = true;
	r = attempt(L"ads.bin", WF_COLLIDE_STOP, WF_VERIFY_SHA256);
	CHECK(!g_bin_calls && !r.sources_recycled &&
	        r.state == WF_CS_CANCELLED);
	wf_copyresult_free(&r);
	reset();
	g_mock_bin = true;
	g_control = &ctl;
	g_cancel_after_bin = true;
	r = attempt(L"ads.bin", WF_COLLIDE_STOP, WF_VERIFY_SHA256);
	CHECK(g_bin_calls == 1 && r.sources_recycled == 1 &&
	        r.state == WF_CS_CANCELLED);
	wf_copyresult_free(&r);
	reset();
	g_mock_bin = g_uncertain_bin = true;
	g_control = &ctl;
	r = attempt(L"ads.bin", WF_COLLIDE_STOP, WF_VERIFY_SHA256);
	CHECK(g_bin_calls == 1 && !r.sources_recycled &&
	        r.sources_uncertain == 1 && !r.sources_untouched &&
	        r.failure_total);
	wf_copyresult_free(&r);
	g_uncertain_bin = false;

	for (unsigned change = 1; change <= 4; change++) {
		reset();
		g_kind = WF_CJ_MOVE_ACROSS;
		g_mock_bin = true;
		g_control = &ctl;
		g_change = change;
		g_write_error = 0;
		r = attempt(L"ads.bin", WF_COLLIDE_STOP, WF_VERIFY_SHA256);
		CHECK(r.files_copied == 1);
		if (change < 3) {
			CHECK(g_write_error == ERROR_SHARING_VIOLATION &&
			        g_bin_calls == 1 && r.sources_recycled == 1);
		} else {
			CHECK(!g_write_error && !g_bin_calls &&
			        r.sources_kept == 1 && r.failure_total);
		}
		wf_copyresult_free(&r);
		purge(dst_dir);
	}
	g_kind = WF_CJ_COPY;
	reset();
}

static void purge(const wchar_t *dir)
{
	wchar_t pattern[1100], child[1100];
	WIN32_FIND_DATAW fd;
	HANDLE f;
	size_t rl = wcslen(root);

	if (0 != wcsncmp(dir, root, rl) || L'\\' != dir[rl]) {
		return;
	}
	swprintf(pattern, 1100, L"%ls\\*", dir);
	f = FindFirstFileW(pattern, &fd);
	if (INVALID_HANDLE_VALUE == f) {
		return;
	}
	do {
		if (!wcscmp(fd.cFileName, L".") ||
		        !wcscmp(fd.cFileName, L"..")) {
			continue;
		}
		swprintf(child, 1100, L"%ls\\%ls", dir, fd.cFileName);
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			purge(child);
			RemoveDirectoryW(child);
			continue;
		}
		SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
		DeleteFileW(child);
	} while (FindNextFileW(f, &fd));
	FindClose(f);
}

int main(void)
{
	for (FinalPathFault fault = FINAL_PATH_OK; fault <= FINAL_PATH_GREW;
	        fault++) {
		final_path_fault = fault;
		wchar_t *path = final_path_of(INVALID_HANDLE_VALUE);
		CHECK((path != NULL) == (fault == FINAL_PATH_OK));
		free(path);
	}
	final_path_fault = FINAL_PATH_REAL;
	ops = wf_probe_win();
	CHECK(p0_lab_open(&lab, "copyfaults", 0));
	if (!lab.area) {
		return TEST_RESULT();
	}
	wcscpy_s(root, 1024, lab.root);
	while (wcslen(root) > 3 && L'\\' == root[wcslen(root) - 1]) {
		root[wcslen(root) - 1] = 0;
	}
	CHECK(p0_area_mkdir(lab.area, L"s"));
	CHECK(p0_area_mkdir(lab.area, L"d"));
	swprintf(src_dir, 1024, L"%ls\\s", root);
	swprintf(dst_dir, 1024, L"%ls\\d", root);
	swprintf(journal, 1100, L"%ls\\journal.txt", root);
	stream_and_recycle_contracts();
	purge(src_dir);
	failures_leave_no_trace_under_the_name();
	purge(dst_dir);
	a_failed_replacement_keeps_the_old_file();
	a_listing_that_fails_is_not_a_finished_folder();
	a_folder_is_all_or_nothing();
	empty_and_many_file_folders();
	purge(src_dir);
	purge(dst_dir);
	DeleteFileW(journal);
	CHECK(0 == p0_lab_close(&lab, 0 != wf_test_failures));
	return TEST_RESULT();
}
