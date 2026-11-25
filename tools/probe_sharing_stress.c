/* Opt-in diagnostic for the intermittent ERROR_SHARING_VIOLATION (32) that the
 * folder Copy has shown once in a while.
 *
 * It copies a small tree many times into fresh destinations of its own and,
 * while each copy runs, a watcher asks the Restart Manager which processes
 * hold files of that destination. A failure then comes with an answer to the
 * one question that separates the candidates: was the holder this process
 * (a handle the engine or the harness kept) or another one (a scanner, the
 * indexer, a sync client)? Only process names, ids and counts are recorded,
 * never file contents. Only fresh owned fixture data is copied: no aliases,
 * ACL changes, arbitrary paths, overwrites or automatic cleanup.
 *
 * The engine is compiled into this probe with two observing wrappers
 * (CreateFileW, SetFileInformationByHandle). For every iteration the first
 * failing call is recorded at the instant it fails: which API, the
 * GetLastError value read right after it, how many times the commit rename was
 * tried, the File ID of the folder being renamed and the processes the Restart
 * Manager then names as holding files below it. No process is ever stopped or
 * signalled.
 *
 * A run that does not fail proves nothing about the cause; it is reported as
 * "no failure observed", never as fixed. */
#include <windows.h>
#include <restartmanager.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "p0/p0_lab.h"
#include "p0/p0_common.h"
#include "xfer/wf_probe_win.h"

#define DIRS 4u
#define FILE_BYTES 4096u
#define HOLDERS_MAX 32u
#define WATCH_FILES_MAX 256u

typedef struct
{
	DWORD pid;
	wchar_t name[64];
	unsigned seen;
} Holder;

static CRITICAL_SECTION lock;
static Holder holders[HOLDERS_MAX];
static size_t holder_count;
static wchar_t watch_dir[1024];
static volatile LONG stop_watching;
static volatile LONG rm_errors;

static void note_holder(DWORD pid, const wchar_t *name);
static void holders_text(const wchar_t *dir, char *out, size_t cap);

typedef struct
{
	bool set;
	char api[40];
	DWORD error; /* GetLastError() read right after the failing call */
	unsigned rename_attempt; /* 1-based commit rename that failed first */
	char file_id[48];  /* volume serial : file id of the renamed folder */
	char holders[400]; /* Restart Manager's answer at that instant */
	char open_children
	        [160]; /* children another opener holds without delete sharing */
} FirstFailure;

static FirstFailure first;
static unsigned rename_calls; /* folder renames tried this iteration */
static unsigned rename_failures;

static void id_text(HANDLE h, char *out, size_t cap)
{
	FILE_ID_INFO id;
	size_t n;

	out[0] = '\0';
	if (!GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof id)) {
		snprintf(out, cap, "unavailable(%lu)", GetLastError());
		return;
	}
	n = (size_t)snprintf(out, cap,
	        "%016llx:", (unsigned long long)id.VolumeSerialNumber);
	for (size_t i = 0; i < 16 && n + 2 < cap; i++) {
		n += (size_t)snprintf(
		        out + n, cap - n, "%02x", id.FileId.Identifier[i]);
	}
}

/*
 * A folder cannot be renamed while a file inside it is open without delete
 * sharing, and then the rename fails with ERROR_ACCESS_DENIED (5). Which
 * child is held? Open each exclusively with DELETE access: a sharing
 * violation names it. The probe only ever does this inside its own scratch
 * folder, and every handle is closed at once.
 */
static void held_children(const wchar_t *dir, char *out, size_t cap)
{
	wchar_t pattern[1100];
	WIN32_FIND_DATAW fd;
	HANDLE find;
	size_t n = 0, held = 0, seen = 0;

	out[0] = '\0';
	if (swprintf(pattern, _countof(pattern), L"%ls\\*", dir) < 0) {
		return;
	}
	find = FindFirstFileExW(
	        pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
	if (INVALID_HANDLE_VALUE == find) {
		snprintf(out, cap, "list(%lu)", GetLastError());
		return;
	}
	do {
		wchar_t child[1100];
		HANDLE h;
		DWORD error;

		if (!wcscmp(fd.cFileName, L".") ||
		        !wcscmp(fd.cFileName, L"..")) {
			continue;
		}
		seen++;
		if (swprintf(child, _countof(child), L"%ls\\%ls", dir,
		            fd.cFileName) < 0) {
			continue;
		}
		h = CreateFileW(child, DELETE, 0, NULL, OPEN_EXISTING,
		        FILE_FLAG_BACKUP_SEMANTICS |
		                FILE_FLAG_OPEN_REPARSE_POINT,
		        NULL);
		if (INVALID_HANDLE_VALUE != h) {
			CloseHandle(h);
			continue;
		}
		error = GetLastError();
		held++;
		if (n + 24 < cap) {
			n += (size_t)snprintf(out + n, cap - n, "%s%lu;",
			        ERROR_SHARING_VIOLATION == error ? "held:"
			                                         : "err:",
			        error);
		}
	} while (FindNextFileW(find, &fd));
	FindClose(find);
	if (n + 24 < cap) {
		(void)snprintf(out + n, cap - n, " %zu of %zu", held, seen);
	}
}

static void record_first(const char *api,
        DWORD error,
        HANDLE directory_handle,
        const wchar_t *path_hint)
{
	wchar_t path[1100];

	if (first.set) {
		return;
	}
	first.set = true;
	snprintf(first.api, sizeof first.api, "%s", api);
	first.error = error;
	first.rename_attempt = rename_calls;
	first.holders[0] = '\0';
	if (directory_handle && INVALID_HANDLE_VALUE != directory_handle) {
		id_text(directory_handle, first.file_id, sizeof first.file_id);
		if (GetFinalPathNameByHandleW(directory_handle, path, 1100,
		            FILE_NAME_NORMALIZED) &&
		        wcslen(path) < 1100) {
			holders_text(path, first.holders, sizeof first.holders);
			held_children(path, first.open_children,
			        sizeof first.open_children);
		}
	} else if (path_hint) {
		snprintf(first.file_id, sizeof first.file_id, "-");
		holders_text(path_hint, first.holders, sizeof first.holders);
	}
	SetLastError(error); /* the engine reads it next */
}

static HANDLE probe_create(LPCWSTR path,
        DWORD access,
        DWORD share,
        LPSECURITY_ATTRIBUTES security,
        DWORD creation,
        DWORD flags,
        HANDLE template_file)
{
	HANDLE h = CreateFileW(
	        path, access, share, security, creation, flags, template_file);

	if (INVALID_HANDLE_VALUE == h) {
		DWORD error = GetLastError();

		/* only the engine's staging folder and what is inside it */
		if (wcsstr(path, L".wfcopy-")) {
			record_first("CreateFileW(staging)", error, NULL, path);
		}
		SetLastError(error);
	}
	return h;
}

static BOOL probe_set_info(
        HANDLE h, FILE_INFO_BY_HANDLE_CLASS cls, LPVOID info, DWORD size)
{
	FILE_STANDARD_INFO st;
	bool folder = FileRenameInfo == cls &&
	        GetFileInformationByHandleEx(
	                h, FileStandardInfo, &st, sizeof st) &&
	        st.Directory;
	BOOL ok;

	if (folder) {
		rename_calls++;
	}
	ok = SetFileInformationByHandle(h, cls, info, size);
	if (!ok && folder) {
		DWORD error = GetLastError(); /* before anything else runs */

		rename_failures++;
		record_first("SetFileInformationByHandle(FileRenameInfo)",
		        error, h, NULL);
		SetLastError(error);
	}
	return ok;
}

#define CreateFileW probe_create
#define SetFileInformationByHandle probe_set_info
#include "../src/xfer/wf_copyengine_win.c"
#undef SetFileInformationByHandle
#undef CreateFileW

static void note_holder(DWORD pid, const wchar_t *name)
{
	EnterCriticalSection(&lock);
	for (size_t i = 0; i < holder_count; i++) {
		if (holders[i].pid == pid) {
			holders[i].seen++;
			LeaveCriticalSection(&lock);
			return;
		}
	}
	if (holder_count < HOLDERS_MAX) {
		holders[holder_count].pid = pid;
		wcsncpy_s(holders[holder_count].name,
		        _countof(holders[holder_count].name), name, _TRUNCATE);
		holders[holder_count].seen = 1;
		holder_count++;
	}
	LeaveCriticalSection(&lock);
}

/* Appends the files under `dir` (one level of folders, as the fixture has). */
static void gather(const wchar_t *dir, wchar_t (*files)[1100], UINT *n)
{
	wchar_t pattern[1100];
	WIN32_FIND_DATAW fd;
	HANDLE f;

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
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			if (!(fd.dwFileAttributes &
			            FILE_ATTRIBUTE_REPARSE_POINT)) {
				wchar_t sub[1100];

				swprintf(sub, 1100, L"%ls\\%ls", dir,
				        fd.cFileName);
				gather(sub, files, n);
			}
		} else if (*n < WATCH_FILES_MAX) {
			swprintf(files[(*n)++], 1100, L"%ls\\%ls", dir,
			        fd.cFileName);
		}
	} while (FindNextFileW(f, &fd));
	FindClose(f);
}

/* "pid:name(self)" for every process the Restart Manager says holds a file
 * below `dir`, in the order it lists them. Read-only. */
static void holders_text(const wchar_t *dir, char *out, size_t cap)
{
	static wchar_t files[WATCH_FILES_MAX][1100];
	const wchar_t *names[WATCH_FILES_MAX];
	UINT n = 0, needed = 0, have = 16;
	DWORD session = 0, reasons = 0;
	WCHAR key[CCH_RM_SESSION_KEY + 1] = {0};
	RM_PROCESS_INFO info[16];
	size_t used = 0;

	out[0] = '\0';
	gather(dir, files, &n);
	if (!n) {
		snprintf(out, cap, "none-listed(no files below)");
		return;
	}
	if (ERROR_SUCCESS != RmStartSession(&session, 0, key)) {
		snprintf(out, cap, "rm-unavailable");
		return;
	}
	for (UINT i = 0; i < n; i++) {
		names[i] = files[i];
	}
	if (ERROR_SUCCESS ==
	                RmRegisterResources(
	                        session, n, names, 0, NULL, 0, NULL) &&
	        ERROR_SUCCESS ==
	                RmGetList(session, &needed, &have, info, &reasons)) {
		for (UINT i = 0; i < have && used + 80 < cap; i++) {
			used += (size_t)snprintf(out + used, cap - used,
			        "%lu:%ls%s ", info[i].Process.dwProcessId,
			        info[i].strAppName,
			        info[i].Process.dwProcessId ==
			                        GetCurrentProcessId()
			                ? "(self)"
			                : "");
		}
		if (!have) {
			snprintf(out, cap, "none");
		}
	} else {
		snprintf(out, cap, "rm-error");
	}
	RmEndSession(session);
}

/*
 * A benign observer for comparison. It polls the same files as the Restart
 * Manager watcher but only asks for their attributes, with every sharing flag
 * set: an attribute-only open takes no part in sharing checks, so it cannot
 * make a rename fail. If the failures seen with the Restart Manager watcher
 * vanish with this one, they came from the Restart Manager's own opens (an
 * observer effect) and not from a holder that was there anyway.
 */
static DWORD WINAPI attribute_watcher(LPVOID arg)
{
	static wchar_t files[WATCH_FILES_MAX][1100];

	(void)arg;
	while (!stop_watching) {
		wchar_t dir[1024];
		UINT n = 0;

		EnterCriticalSection(&lock);
		wcscpy_s(dir, 1024, watch_dir);
		LeaveCriticalSection(&lock);
		if (dir[0]) {
			gather(dir, files, &n);
		}
		for (UINT i = 0; i < n; i++) {
			HANDLE h = CreateFileW(files[i], FILE_READ_ATTRIBUTES,
			        FILE_SHARE_READ | FILE_SHARE_WRITE |
			                FILE_SHARE_DELETE,
			        NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
			        NULL);

			if (INVALID_HANDLE_VALUE != h) {
				CloseHandle(h);
			}
		}
		Sleep(1);
	}
	return 0;
}

static DWORD WINAPI watcher(LPVOID arg)
{
	static wchar_t files[WATCH_FILES_MAX][1100];
	const wchar_t *names[WATCH_FILES_MAX];

	(void)arg;
	while (!stop_watching) {
		wchar_t dir[1024];
		UINT n = 0;
		DWORD session = 0;
		WCHAR key[CCH_RM_SESSION_KEY + 1] = {0};

		EnterCriticalSection(&lock);
		wcscpy_s(dir, 1024, watch_dir);
		LeaveCriticalSection(&lock);
		if (dir[0]) {
			gather(dir, files, &n);
		}
		if (n && ERROR_SUCCESS == RmStartSession(&session, 0, key)) {
			UINT needed = 0, have = 0;
			DWORD reasons = 0;
			RM_PROCESS_INFO info[16];
			DWORD e;

			for (UINT i = 0; i < n; i++) {
				names[i] = files[i];
			}
			have = 16;
			e = RmRegisterResources(
			        session, n, names, 0, NULL, 0, NULL);
			if (ERROR_SUCCESS == e) {
				e = RmGetList(session, &needed, &have, info,
				        &reasons);
			}
			if (ERROR_SUCCESS == e || ERROR_MORE_DATA == e) {
				for (UINT i = 0; i < have && i < 16; i++) {
					note_holder(info[i].Process.dwProcessId,
					        info[i].strAppName);
				}
			} else {
				InterlockedIncrement(&rm_errors);
			}
			RmEndSession(session);
		}
		Sleep(1);
	}
	return 0;
}

static bool write_source(P0Lab *lab, const wchar_t *relative, unsigned seed)
{
	wchar_t path[1024];
	unsigned char data[FILE_BYTES];
	HANDLE h;
	DWORD written = 0;
	bool ok;

	if (!p0_lab_path(lab, relative, path, 1024)) {
		return false;
	}
	for (size_t i = 0; i < sizeof data; i++) {
		data[i] = (unsigned char)(i * 131u + seed * 17u);
	}
	h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	ok = p0_area_track(lab->area, relative, 0) != 0;
	ok = ok && WriteFile(h, data, sizeof data, &written, NULL) &&
	        written == sizeof data && FlushFileBuffers(h);
	CloseHandle(h);
	return ok;
}

typedef struct
{
	unsigned iterations, copied, failed, hash_mismatch, hash_unreadable,
	        retried;
	unsigned by_error[4]; /* 32, 5, other, engine status */
} Totals;

/* A scanner may hold a fresh file for a moment: the read is tried again
 * before it is called unreadable. */
static bool hash_retry(const wchar_t *path, char hex[P0_HASH_HEX])
{
	for (unsigned attempt = 0; attempt < 8; attempt++) {
		if (p0_sha256_file(path, hex)) {
			return true;
		}
		Sleep(25);
	}
	return false;
}

typedef enum
{
	TREES_SAME,
	TREES_DIFFERENT,
	TREES_UNREADABLE
} TreesResult;

static TreesResult trees_match(
        const wchar_t *from, const wchar_t *to, unsigned files)
{
	for (unsigned d = 0; d < DIRS; d++) {
		for (unsigned f = 0; f < files / DIRS; f++) {
			wchar_t a[1100], b[1100];
			char ha[P0_HASH_HEX], hb[P0_HASH_HEX];

			swprintf(a, 1100, L"%ls\\tree\\d%u\\f%u.bin", from, d,
			        f);
			swprintf(b, 1100, L"%ls\\tree\\d%u\\f%u.bin", to, d, f);
			/* "could not be hashed" and "hashed and different" are
			 * different findings: only the second is corruption */
			if (!hash_retry(a, ha)) {
				printf("  hash_unreadable source %ls (error "
				       "%lu)\n",
				        a, GetLastError());
				return TREES_UNREADABLE;
			}
			if (!hash_retry(b, hb)) {
				printf("  hash_unreadable copy %ls (error "
				       "%lu)\n",
				        b, GetLastError());
				return TREES_UNREADABLE;
			}
			if (strcmp(ha, hb)) {
				printf("  hash_DIFFERENT %ls vs %ls\n", a, b);
				return TREES_DIFFERENT;
			}
		}
	}
	return TREES_SAME;
}

static void print_holders(const char *when)
{
	EnterCriticalSection(&lock);
	printf("holders %s: distinct=%zu rm_errors=%ld\n", when, holder_count,
	        (long)rm_errors);
	for (size_t i = 0; i < holder_count; i++) {
		printf("  holder pid=%lu self=%d seen=%u name=%ls\n",
		        holders[i].pid, holders[i].pid == GetCurrentProcessId(),
		        holders[i].seen, holders[i].name);
	}
	LeaveCriticalSection(&lock);
}

static bool run_one(P0Lab *lab,
        WfProbeOps *ops,
        WfCopyJournal *journal,
        unsigned index,
        unsigned files,
        Totals *t)
{
	wchar_t rel[64], dest[1024], source[1024];
	const wchar_t *names[] = {L"tree"};
	WfCopySpec spec = {0};
	WfPreflightReport report = {0};
	WfCopyResult result = {0};
	WfNodeInfo node;
	WfStatus st;
	bool ok = false;

	swprintf(rel, 64, L"out%u", index);
	if (!p0_area_mkdir(lab->area, rel) ||
	        !p0_lab_path(lab, rel, dest, 1024) ||
	        !p0_lab_path(lab, L"source", source, 1024)) {
		return false;
	}
	EnterCriticalSection(&lock);
	wcscpy_s(watch_dir, 1024, dest);
	LeaveCriticalSection(&lock);

	st = wf_xfer_capture(
	        ops, WF_XFER_COPY, source, names, 1, 0, &spec.slot);
	if (WF_OK == st) {
		st = wf_preflight_run(ops, &spec.slot, dest,
		        &WF_PREFLIGHT_DEFAULT, NULL, &report);
	}
	if (WF_OK != st || !wf_preflight_can_start(st, &report, 0) ||
	        WF_OK != ops->node(NULL, dest, &node)) {
		t->by_error[3]++;
		printf("iteration=%u preflight_status=%d\n", index, (int)st);
		wf_copyspec_free(&spec);
		return false;
	}
	spec.dest = _wcsdup(dest);
	spec.dest_id = node.id;
	spec.kind = WF_CJ_COPY;
	spec.collide = WF_COLLIDE_STOP;
	spec.verify = WF_VERIFY_SHA256;
	spec.total_files = report.tree_files;
	spec.total_bytes = report.total_bytes;
	memset(&first, 0, sizeof first);
	rename_calls = rename_failures = 0;
	st = wf_copyengine_run(&spec, NULL, journal, &result);
	if (first.set) {
		/* a first failing call is worth a line even when a later attempt
		 * succeeded: that is the retry doing its work */
		printf("iteration=%u first_failure api=%s win32=%lu "
		       "rename_attempt=%u/%u file_id=%s holders=[%s] "
		       "children=[%s] outcome=%s\n",
		        index, first.api, first.error, first.rename_attempt,
		        rename_calls, first.file_id, first.holders,
		        first.open_children,
		        WF_CS_DONE == result.state && !result.failure_total
		                ? "completed-after-retry"
		                : "FAILED");
		t->retried++;
	}
	if (WF_OK == st && WF_CS_DONE == result.state &&
	        !result.failure_total) {
		TreesResult same = trees_match(source, dest, files);

		t->copied++;
		ok = true;
		if (TREES_DIFFERENT == same) {
			t->hash_mismatch++;
			printf("iteration=%u CONTENT DIFFERS (corruption)\n",
			        index);
			ok = false;
		} else if (TREES_UNREADABLE == same) {
			t->hash_unreadable++;
			ok = false;
		}
	} else {
		t->failed++;
		printf("iteration=%u engine_status=%d state=%d failures=%zu\n",
		        index, (int)st, (int)result.state,
		        result.failure_total);
		for (size_t i = 0;
		        i < result.failure_total && i < WF_CJ_FAILURES; i++) {
			DWORD e = result.failures[i].error;

			t->by_error[32 == e ? 0 : 5 == e ? 1 : 2]++;
			printf("  failure kind=%d error=%lu\n",
			        (int)result.failures[i].kind, e);
		}
		print_holders("at first failure (so far)");
	}
	wf_copyresult_free(&result);
	wf_copyspec_free(&spec);
	return ok;
}

int wmain(int argc, wchar_t **argv)
{
	unsigned iterations = 200, files = 40;
	P0Lab lab = {0};
	WfProbeOps ops;
	WfCopyJournal *journal;
	wchar_t journal_path[1024];
	HANDLE thread;
	Totals t = {0};
	bool setup, watch = true, attribute_only = false;

	for (int i = 1; i < argc; i++) {
		if (!wcscmp(argv[i], L"--iterations") && i + 1 < argc) {
			iterations = (unsigned)_wtoi(argv[++i]);
		} else if (!wcscmp(argv[i], L"--files") && i + 1 < argc) {
			files = (unsigned)_wtoi(argv[++i]);
		} else if (!wcscmp(argv[i], L"--no-watch")) {
			watch = false; /* control: no Restart Manager activity */
		} else if (!wcscmp(argv[i], L"--attribute-watch")) {
			attribute_only = true; /* a benign observer, no RM */
		} else {
			fputs("usage: probe_sharing_stress [--iterations N] "
			      "[--files M] [--no-watch | --attribute-watch]\n",
			        stderr);
			return 2;
		}
	}
	files = files / DIRS * DIRS;
	if (!iterations || !files || files > WATCH_FILES_MAX - DIRS) {
		fputs("iterations or files out of range\n", stderr);
		return 2;
	}
	InitializeCriticalSection(&lock);
	if (!p0_lab_open(&lab, "sharing-stress", 0)) {
		printf("fixture_error=%lu\n", lab.error);
		return 2;
	}
	setup = p0_area_mkdir(lab.area, L"source") &&
	        p0_area_mkdir(lab.area, L"source\\tree") &&
	        p0_area_mkdir(lab.area, L"receipts");
	for (unsigned d = 0; setup && d < DIRS; d++) {
		wchar_t rel[64];

		swprintf(rel, 64, L"source\\tree\\d%u", d);
		setup = p0_area_mkdir(lab.area, rel);
		for (unsigned f = 0; setup && f < files / DIRS; f++) {
			swprintf(rel, 64, L"source\\tree\\d%u\\f%u.bin", d, f);
			setup = write_source(&lab, rel, d * 1000 + f);
		}
	}
	if (!setup ||
	        !p0_lab_path(
	                &lab, L"receipts\\journal.txt", journal_path, 1024)) {
		p0_lab_close(&lab, 1);
		fputs("fixture_setup=FAIL\n", stderr);
		return 2;
	}
	ops = wf_probe_win();
	journal = wf_cj_open(journal_path);
	if (!journal) {
		p0_lab_close(&lab, 1);
		fputs("journal_open=FAIL\n", stderr);
		return 2;
	}
	thread = watch ? CreateThread(NULL, 0,
	                         attribute_only ? attribute_watcher : watcher,
	                         NULL, 0, NULL)
	               : NULL;
	printf("watcher=%s\n",
	        !watch ? "off (control)"
	                : attribute_only
	                ? "attributes-only (no restart manager)"
	                : "restart-manager");
	for (unsigned i = 0; i < iterations; i++) {
		t.iterations++;
		(void)run_one(&lab, &ops, journal, i, files, &t);
	}
	InterlockedExchange(&stop_watching, 1);
	if (thread) {
		WaitForSingleObject(thread, 5000);
		CloseHandle(thread);
	}
	wf_cj_close(journal);
	printf("iterations=%u copied=%u failed=%u content_differs=%u "
	       "hash_unreadable=%u iterations_with_a_failing_first_call=%u "
	       "err32=%u err5=%u other=%u setup_or_preflight=%u\n",
	        t.iterations, t.copied, t.failed, t.hash_mismatch,
	        t.hash_unreadable, t.retried, t.by_error[0], t.by_error[1],
	        t.by_error[2], t.by_error[3]);
	print_holders("over the whole run");
	if (!t.failed && !t.hash_mismatch && !t.hash_unreadable) {
		puts("verdict=NO_FAILURE_OBSERVED (says nothing about the "
		     "cause)");
	} else {
		puts("verdict=FAILURE_OBSERVED (see the holders above)");
	}
	p0_lab_close(&lab, 1); /* never sweep fixtures or recorded targets */
	puts("fixture_retained=1");
	return t.failed || t.hash_mismatch || t.hash_unreadable ? 1 : 0;
}
