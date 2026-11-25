/* Reuse the real fixture and journal writer. Only the public Win32 boundary
 * is replaced in this test translation unit; production has no fault flags. */
#define wmain storage_fixture_main
#include "test_storage_win.c"
#undef wmain

typedef enum
{
	F_NONE,
	F_EX_UNSUPPORTED,
	F_UNSUPPORTED,
	F_SHARE_ALWAYS,
	F_SHARE_TWICE,
	F_CLEAR_FAIL,
	F_LEGACY_FAIL,
	F_RESTORE_FAIL,
	F_QUERY_FAIL,
	F_RECHECK,
	F_REPARSE,
	F_LINKS,
	F_SIZE,
	F_FINAL_PATH,
	F_VOLUME,
	F_FILESYSTEM,
	F_SWAP
} Fault;
static Fault g_fault;
static unsigned g_ex_calls, g_basic_calls, g_legacy_calls, g_sleeps,
        g_alloc_count, g_fail_alloc;
static DWORD g_delays[8];
static HANDLE g_disposition_handle;
static FILE_ID_INFO g_target_id;
static wchar_t g_swap_path[CAP], g_swap_original[CAP];
static FILE *g_fault_csv;

static int target_handle(HANDLE h)
{
	FILE_ID_INFO id;
	return GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof id) &&
	        id.VolumeSerialNumber == g_target_id.VolumeSerialNumber &&
	        !memcmp(id.FileId.Identifier, g_target_id.FileId.Identifier,
	                16);
}

BOOL test_setinfo(
        HANDLE h, FILE_INFO_BY_HANDLE_CLASS cls, void *data, DWORD bytes)
{
	if (cls == FileDispositionInfoEx || cls == FileDispositionInfo) {
		if (g_disposition_handle) {
			CHECK(h == g_disposition_handle);
		} else {
			g_disposition_handle = h;
		}
		CHECK(target_handle(h));
	}
	if (cls == FileDispositionInfoEx) {
		g_ex_calls++;
		if (F_RECHECK == g_fault) {
			SetLastError(ERROR_ACCESS_DENIED);
			return FALSE;
		}
		CHECK(((FILE_DISPOSITION_INFO_EX *)data)->Flags ==
		        (FILE_DISPOSITION_FLAG_DELETE |
		                FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
		                FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE));
		if (F_SHARE_ALWAYS == g_fault ||
		        (F_SHARE_TWICE == g_fault && g_ex_calls <= 2)) {
			SetLastError(ERROR_SHARING_VIOLATION);
			return FALSE;
		}
		if (F_EX_UNSUPPORTED == g_fault || F_UNSUPPORTED == g_fault ||
		        F_CLEAR_FAIL == g_fault || F_LEGACY_FAIL == g_fault ||
		        F_RESTORE_FAIL == g_fault) {
			SetLastError(ERROR_NOT_SUPPORTED);
			return FALSE;
		}
	}
	if (cls == FileBasicInfo) {
		g_basic_calls++;
		if ((F_CLEAR_FAIL == g_fault && 1 == g_basic_calls) ||
		        (F_RESTORE_FAIL == g_fault && 2 == g_basic_calls)) {
			SetLastError(ERROR_ACCESS_DENIED);
			return FALSE;
		}
	}
	if (cls == FileDispositionInfo) {
		g_legacy_calls++;
		if (F_UNSUPPORTED == g_fault || F_LEGACY_FAIL == g_fault ||
		        F_RESTORE_FAIL == g_fault) {
			SetLastError(F_UNSUPPORTED == g_fault
			                ? ERROR_NOT_SUPPORTED
			                : ERROR_DISK_FULL);
			return FALSE;
		}
	}
	return SetFileInformationByHandle(h, cls, data, bytes);
}

BOOL test_getinfo(
        HANDLE h, FILE_INFO_BY_HANDLE_CLASS cls, void *data, DWORD bytes)
{
	BOOL ok;
	if (F_QUERY_FAIL == g_fault && cls == FileStandardInfo &&
	        target_handle(h)) {
		SetLastError(ERROR_GEN_FAILURE);
		return FALSE;
	}
	ok = GetFileInformationByHandleEx(h, cls, data, bytes);
	if (ok && target_handle(h)) {
		if (F_REPARSE == g_fault && cls == FileAttributeTagInfo) {
			((FILE_ATTRIBUTE_TAG_INFO *)data)->FileAttributes |=
			        FILE_ATTRIBUTE_REPARSE_POINT;
		}
		if (F_LINKS == g_fault && cls == FileStandardInfo) {
			((FILE_STANDARD_INFO *)data)->NumberOfLinks = 2;
		}
		if (F_SIZE == g_fault && cls == FileStandardInfo) {
			((FILE_STANDARD_INFO *)data)->EndOfFile.QuadPart = 4097;
		}
	}
	return ok;
}

DWORD test_finalpath(HANDLE h, wchar_t *path, DWORD cap, DWORD flags)
{
	if (F_FINAL_PATH == g_fault) {
		SetLastError(ERROR_ACCESS_DENIED);
		return 0;
	}
	return GetFinalPathNameByHandleW(h, path, cap, flags);
}

BOOL test_volume(HANDLE h,
        wchar_t *name,
        DWORD n,
        DWORD *serial,
        DWORD *max,
        DWORD *flags,
        wchar_t *fs,
        DWORD fs_cap)
{
	BOOL ok;
	if (F_VOLUME == g_fault) {
		SetLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	ok = GetVolumeInformationByHandleW(
	        h, name, n, serial, max, flags, fs, fs_cap);
	if (ok && F_FILESYSTEM == g_fault) {
		wcscpy_s(fs, fs_cap, L"exFAT");
	}
	return ok;
}

HANDLE test_createfile(const wchar_t *path,
        DWORD access,
        DWORD share,
        SECURITY_ATTRIBUTES *sa,
        DWORD creation,
        DWORD flags,
        HANDLE template_file)
{
	if (F_SWAP == g_fault && (access & DELETE) &&
	        !wcscmp(path, g_swap_path)) {
		g_fault = F_NONE;
		CHECK(MoveFileExW(g_swap_path, g_swap_original, 0));
		CHECK(write_new(g_swap_path, "replacement user object", 23));
	}
	return CreateFileW(
	        path, access, share, sa, creation, flags, template_file);
}

void test_sleep(DWORD delay)
{
	CHECK(g_sleeps < 8);
	if (g_sleeps < 8) {
		g_delays[g_sleeps++] = delay;
	}
}

void *test_malloc(size_t size)
{
	if (++g_alloc_count == g_fail_alloc) {
		return NULL;
	}
	return malloc(size);
}
void *test_calloc(size_t count, size_t size)
{
	if (++g_alloc_count == g_fail_alloc) {
		return NULL;
	}
	return calloc(count, size);
}

#define SetFileInformationByHandle test_setinfo
#define GetFileInformationByHandleEx test_getinfo
#define GetFinalPathNameByHandleW test_finalpath
#define GetVolumeInformationByHandleW test_volume
#define CreateFileW test_createfile
#define Sleep test_sleep
#define malloc test_malloc
#define calloc test_calloc
#ifndef WF_OWNED_DELETE_SOURCE
#	define WF_OWNED_DELETE_SOURCE "../src/fs/owned_delete.c"
#endif
#ifndef WF_CANONICAL_SOURCE
#	define WF_CANONICAL_SOURCE "../src/path/wf_canonical_win.c"
#endif
#include WF_CANONICAL_SOURCE
#include WF_OWNED_DELETE_SOURCE
#undef calloc
#undef malloc
#undef Sleep
#undef CreateFileW
#undef GetVolumeInformationByHandleW
#undef GetFinalPathNameByHandleW
#undef GetFileInformationByHandleEx
#undef SetFileInformationByHandle

static void reset_fault(Fault fault)
{
	g_fault = fault;
	g_ex_calls = g_basic_calls = g_legacy_calls = g_sleeps = g_alloc_count =
	        g_fail_alloc = 0;
	g_disposition_handle = NULL;
}

static void fault_case(Fault fault)
{
	static const char *names[] = {"native_ex", "ex_unsupported_fallback",
	        "all_disposition_unsupported", "sharing_exhausted",
	        "sharing_then_success", "readonly_clear_failure",
	        "legacy_failure_restore", "readonly_restore_failure",
	        "query_failure", "delete_reverification", "injected_reparse",
	        "injected_links", "injected_size", "canonical_query_failure",
	        "volume_query_failure", "unsupported_filesystem",
	        "swap_before_named_acquisition"};
	Fixture f;
	WfOwnedReport r;
	WfVerifiedOwned *owned = NULL;
	WfOwnedResult result;
	int ready = fixture_create(&f, "fault", 0, 0, 0), deletes;
	int failures_before = wf_test_failures;
	CHECK(ready);
	if (!ready) {
		fixture_finish(&f, 1);
		return;
	}
	g_target_id = f.record.payload;
	CHECK(SetFileAttributesW(f.payload, FILE_ATTRIBUTE_READONLY));
	reset_fault(F_NONE);
	if (fault >= F_REPARSE) {
		g_fault = fault;
		if (F_SWAP == fault) {
			/* Production opens canonical paths; use that exact
			 * spelling. */
			swprintf(g_swap_path, CAP, L"%ls\\%ls",
			        f.record.parent_path, f.record.rel_payload);
			swprintf(g_swap_original, CAP,
			        L"%ls\\adopted-original.bin",
			        f.record.parent_path);
		}
		result = wf_owned_verify(
		        WF_OWNED_COPY_PAYLOAD, &f.record, &owned, &r);
		CHECK(WF_OWNED_MANUAL == result && !owned && exists(f.payload));
		CHECK(0 == g_ex_calls && 0 == g_legacy_calls);
	} else {
		CHECK(wf_owned_verify(WF_OWNED_COPY_PAYLOAD, &f.record, &owned,
		              &r) == WF_OWNED_VERIFIED);
		reset_fault(fault);
		if (F_RECHECK == fault) {
			wchar_t stranger[CAP];
			swprintf(stranger, CAP, L"%ls\\user.txt", f.txn);
			CHECK(write_new(stranger, "keep", 4));
		}
		result = wf_owned_delete(owned, &r);
		deletes = F_NONE == fault || F_EX_UNSUPPORTED == fault ||
		        F_SHARE_TWICE == fault;
		CHECK(result ==
		        (deletes ? WF_OWNED_DELETED
		                        : F_QUERY_FAIL == fault ||
		                                F_RECHECK == fault
		                        ? WF_OWNED_MANUAL
		                        : WF_OWNED_RECOVERY_REQUIRED));
		CHECK(exists(f.payload) == !deletes);
		if (F_SHARE_ALWAYS == fault) {
			CHECK(6 == r.attempts && 6 == g_ex_calls &&
			        5 == g_sleeps);
			CHECK(50 == g_delays[0] && 100 == g_delays[1] &&
			        200 == g_delays[2] && 400 == g_delays[3] &&
			        800 == g_delays[4]);
		}
		if (F_SHARE_TWICE == fault) {
			CHECK(3 == r.attempts && 2 == g_sleeps &&
			        50 == g_delays[0] && 100 == g_delays[1]);
		}
		if (F_EX_UNSUPPORTED == fault) {
			CHECK(r.used_fallback &&
			        ERROR_NOT_SUPPORTED == r.ex_error &&
			        1 == g_basic_calls && 1 == g_legacy_calls);
		}
		if (!deletes && F_RESTORE_FAIL != fault) {
			CHECK(GetFileAttributesW(f.payload) &
			        FILE_ATTRIBUTE_READONLY);
		}
		if (F_RESTORE_FAIL == fault) {
			CHECK(ERROR_ACCESS_DENIED == r.restore_error);
		}
		if (F_QUERY_FAIL == fault) {
			CHECK(0 == r.attempts && 0 == g_ex_calls);
		}
		if (F_RECHECK == fault) {
			CHECK(0 == r.attempts && 0 == g_ex_calls);
		}
	}
	printf("fault=%d result=%d err=%lu ex=%lu restore=%lu attempts=%u\n",
	        (int)fault, (int)result, r.error, r.ex_error, r.restore_error,
	        r.attempts);
	g_fault = F_NONE;
	wf_owned_release(owned);
	if (F_SWAP == fault || F_RECHECK == fault) {
		fixture_finish(&f, 1);
	} else {
		if (exists(f.payload)) {
			CHECK(SetFileAttributesW(
			        f.payload, FILE_ATTRIBUTE_NORMAL));
		}
		fixture_finish(&f, 0);
	}
	if (g_fault_csv) {
		fprintf(g_fault_csv,
		        "%s,%s,%d,%lu,%lu,%lu,%u,%u,injected_Win32_boundary_"
		        "with_real_owned_fixture\n",
		        names[fault],
		        failures_before == wf_test_failures ? "PASS" : "FAIL",
		        (int)result, r.error, r.ex_error, r.restore_error,
		        r.attempts, g_sleeps);
		fflush(g_fault_csv);
	}
}

static void allocation_failures(void)
{
	Fixture f;
	unsigned fail;
	DWORD before, after;
	CHECK(fixture_create(&f, "allocation", 0, 0, 0));
	g_target_id = f.record.payload;
	{
		int kind;
		for (kind = -1; kind <= 9; kind++) {
			WfVerifiedOwned *owned = NULL;
			WfOwnedReport report;
			if (WF_OWNED_COPY_PAYLOAD == kind) {
				continue;
			}
			CHECK(wf_owned_verify((WfOwnedKind)kind, &f.record,
			              &owned, &report) == WF_OWNED_MANUAL);
			CHECK(NULL == owned);
			wf_owned_release(owned);
		}
	}
	CHECK(GetProcessHandleCount(GetCurrentProcess(), &before));
	for (fail = 1; fail <= 40; fail++) {
		WfVerifiedOwned *owned = NULL;
		WfOwnedReport r;
		WfOwnedResult result;
		reset_fault(F_NONE);
		g_fail_alloc = fail;
		result = wf_owned_verify(
		        WF_OWNED_COPY_PAYLOAD, &f.record, &owned, &r);
		if (g_alloc_count >= fail) {
			CHECK(WF_OWNED_MANUAL == result && !owned);
		} else {
			CHECK(WF_OWNED_VERIFIED == result && owned);
		}
		wf_owned_release(owned);
		CHECK(exists(f.payload));
		if (g_fault_csv) {
			fprintf(g_fault_csv,
			        "allocation_%u,%s,%d,%lu,0,0,0,0,injected_"
			        "allocation_failure\n",
			        fail,
			        WF_OWNED_MANUAL == result ||
			                        WF_OWNED_VERIFIED == result
			                ? "PASS"
			                : "FAIL",
			        (int)result, r.error);
		}
	}
	g_fail_alloc = 0;
	CHECK(GetProcessHandleCount(GetCurrentProcess(), &after));
	CHECK(before == after);
	printf("allocation failures: 40 positions; handles before=%lu "
	       "after=%lu\n",
	        before, after);
	fixture_finish(&f, 0);
}

int wmain(int argc, wchar_t **argv)
{
	Fault fault;
	if (5 == argc && !wcscmp(argv[1], L"--child")) {
		return child_create(argv[2], argv[3], _wtoi(argv[4]));
	}
	CHECK(GetModuleFileNameW(NULL, g_exe, CAP));
	wcscpy_s(g_base, CAP, g_exe);
	*wcsrchr(g_base, L'\\') = 0;
	wcscat_s(g_base, CAP, L"\\storage-scratch");
	CHECK(p0_mkdirs(g_base));
	if (3 == argc && !wcscmp(argv[1], L"--evidence")) {
		if (INVALID_FILE_ATTRIBUTES != GetFileAttributesW(argv[2])) {
			return 2;
		}
		g_fault_csv = p0_fopen_w(argv[2]);
		CHECK(NULL != g_fault_csv);
		if (!g_fault_csv) {
			return 2;
		}
		fputs("case,result,class,win32_error,ex_error,restore_error,"
		      "attempts,sleeps,measurement_kind\n",
		        g_fault_csv);
	}
	for (fault = F_NONE; fault <= F_SWAP; fault++) {
		fault_case(fault);
	}
	allocation_failures();
	if (g_fault_csv) {
		fclose(g_fault_csv);
	}
	return TEST_RESULT();
}
