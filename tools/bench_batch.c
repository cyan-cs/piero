/* Benchmark of the batch rename engine against the same number of single renames
 * (DR-DRAFT-022, Measurement). A developer tool: it prints what it measured and
 * asserts nothing about speed. Every measurement runs in a child process of its
 * own, so that the peak working set and the handle count belong to that run only.
 * The files are made in a folder of the temporary directory and removed after.
 *
 *   bench_batch.exe [sizes...]      default: 10 100 1000 10000
 *
 * Per size, three runs, each in a fresh folder of N small files on one volume:
 *   engine    wf_batch_prepare + wf_batch_execute, journal on, every check on
 *   single    N calls of wf_edit_rename_checked, no journal (what the current
 *             rename does, called N times)
 *   rollback  the engine, stopped by the cancel flag after half of the steps, which
 *             then puts every name back
 * Reported: wall time (QueryPerformanceCounter around the call, file creation
 * excluded), CPU time of the process (kernel + user) over the call, the peak
 * working set of the child, the handle count before and after, and for the engine
 * the engine's own counts of calls (opens, FileId queries, renames, folder
 * enumerations) and of journal writes and flushes, and its phases in ms.
 */
#include <windows.h>
#include <psapi.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs/wf_edit_win.h"
#include "xfer/wf_batchexec_win.h"

#ifndef WF_BENCH_CONFIGURATION
#	define WF_BENCH_CONFIGURATION "?"
#endif

#define PATHLEN 1200

static double now_ms(void)
{
	LARGE_INTEGER c, f;

	QueryPerformanceCounter(&c);
	QueryPerformanceFrequency(&f);
	return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static double cpu_ms(void)
{
	FILETIME created, exited, kernel, user;
	ULARGE_INTEGER k, u;

	GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
	k.LowPart = kernel.dwLowDateTime;
	k.HighPart = kernel.dwHighDateTime;
	u.LowPart = user.dwLowDateTime;
	u.HighPart = user.dwHighDateTime;
	return (double)(k.QuadPart + u.QuadPart) / 10000.0;
}

static DWORD handles(void)
{
	DWORD n = 0;

	GetProcessHandleCount(GetCurrentProcess(), &n);
	return n;
}

static WfFileId id_at(const wchar_t *path)
{
	WfFileId id;
	FILE_ID_INFO info;
	HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);

	memset(&id, 0, sizeof id);
	if (INVALID_HANDLE_VALUE != h) {
		if (GetFileInformationByHandleEx(
		            h, FileIdInfo, &info, sizeof info)) {
			id.valid = true;
			id.volume = info.VolumeSerialNumber;
			memcpy(id.id, info.FileId.Identifier, 16);
		}
		CloseHandle(h);
	}
	return id;
}

static void wipe(const wchar_t *dir)
{
	wchar_t pattern[PATHLEN];
	WIN32_FIND_DATAW d;
	HANDLE h;

	swprintf(pattern, PATHLEN, L"%ls\\*", dir);
	h = FindFirstFileExW(
	        pattern, FindExInfoBasic, &d, FindExSearchNameMatch, NULL, 0);
	if (INVALID_HANDLE_VALUE != h) {
		do {
			wchar_t f[PATHLEN];

			if (!wcscmp(d.cFileName, L".") ||
			        !wcscmp(d.cFileName, L"..")) {
				continue;
			}
			swprintf(f, PATHLEN, L"%ls\\%ls", dir, d.cFileName);
			if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
				wipe(f);
			} else {
				DeleteFileW(f);
			}
		} while (FindNextFileW(h, &d));
		FindClose(h);
	}
	RemoveDirectoryW(dir);
}

typedef struct
{
	size_t n;
	wchar_t root[PATHLEN], dir[PATHLEN], journal[PATHLEN];
	wchar_t (*old_name)[24];
	wchar_t (*new_name)[24];
	wchar_t (*old_path)[PATHLEN];
	WfFileId *id;
	WfFileId parent;
} Fixture;

static bool make_fixture(Fixture *f, size_t n)
{
	wchar_t tmp[MAX_PATH];

	memset(f, 0, sizeof *f);
	f->n = n;
	GetTempPathW(MAX_PATH, tmp);
	swprintf(f->root, PATHLEN, L"%lspiero-bench-batch-%lu", tmp,
	        GetCurrentProcessId());
	swprintf(f->dir, PATHLEN, L"%ls\\files", f->root);
	swprintf(f->journal, PATHLEN, L"%ls\\journal", f->root);
	CreateDirectoryW(f->root, NULL);
	CreateDirectoryW(f->dir, NULL);
	f->old_name = calloc(n, sizeof *f->old_name);
	f->new_name = calloc(n, sizeof *f->new_name);
	f->old_path = calloc(n, sizeof *f->old_path);
	f->id = calloc(n, sizeof *f->id);
	if (!f->old_name || !f->new_name || !f->old_path || !f->id) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		HANDLE h;

		swprintf(f->old_name[i], 24, L"f%06zu.txt", i);
		swprintf(f->new_name[i], 24, L"b%06zu.txt", i);
		swprintf(f->old_path[i], PATHLEN, L"%ls\\%ls", f->dir,
		        f->old_name[i]);
		h = CreateFileW(f->old_path[i], GENERIC_WRITE, 0, NULL,
		        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
		if (INVALID_HANDLE_VALUE == h) {
			return false;
		}
		WriteFile(h, "x", 1, &(DWORD){0}, NULL);
		CloseHandle(h);
		f->id[i] = id_at(f->old_path[i]);
	}
	f->parent = id_at(f->dir);
	return true;
}

static void free_fixture(Fixture *f)
{
	wipe(f->root);
	free(f->old_name);
	free(f->new_name);
	free(f->old_path);
	free(f->id);
}

static volatile LONG cancel_flag;
static size_t cancel_at;

static void stop_halfway(void *ctx, const char *point, size_t step)
{
	(void)ctx;
	if (!strcmp(point, "after_D") && step + 1 == cancel_at) {
		InterlockedExchange(&cancel_flag, 1);
	}
}

static int child(const char *mode, size_t n)
{
	Fixture f;
	double t0, c0;
	DWORD h0, h1;
	PROCESS_MEMORY_COUNTERS pmc;
	WfBatchReport rep;
	size_t renamed = 0;
	double wall, cpu;

	if (!make_fixture(&f, n)) {
		return 2;
	}
	memset(&rep, 0, sizeof rep);
	h0 = handles();
	t0 = now_ms();
	c0 = cpu_ms();
	if (!strcmp(mode, "single")) {
		for (size_t i = 0; i < n; i++) {
			WfEditReport er;

			memset(&er, 0, sizeof er);
			if (WF_OK ==
			        wf_edit_rename_checked(f.dir, &f.parent,
			                f.old_name[i], &f.id[i], f.new_name[i],
			                &er)) {
				renamed++;
			}
		}
	} else {
		WfBatchSource *src = calloc(n, sizeof *src);
		WfBatchJob *job;
		WfBatchHooks hooks = {stop_halfway, NULL};

		for (size_t i = 0; i < n; i++) {
			src[i].path = f.old_path[i];
			src[i].expected = f.id[i];
			src[i].to_name = f.new_name[i];
		}
		cancel_at = !strcmp(mode, "rollback") ? n / 2 : 0;
		job = wf_batch_prepare(src, n, NULL, &rep);
		if (job && wf_batch_job_ready(job)) {
			double mid = now_ms();
			WfBatchReport ex;

			memset(&ex, 0, sizeof ex);
			wf_batch_execute(
			        job, f.journal, &hooks, &cancel_flag, &ex);
			rep.result = ex.result;
			rep.steps_done = ex.steps_done;
			rep.rolled_back = ex.rolled_back;
			rep.calls.opens += ex.calls.opens;
			rep.calls.id_queries += ex.calls.id_queries;
			rep.calls.renames += ex.calls.renames;
			rep.calls.enumerations += ex.calls.enumerations;
			rep.log = ex.log;
			rep.ms_journal = ex.ms_journal;
			rep.ms_rename = ex.ms_rename;
			rep.ms_rollback = ex.ms_rollback;
			(void)mid;
			renamed = ex.steps_done;
		}
		wf_batch_job_free(job);
		free(src);
	}
	wall = now_ms() - t0;
	cpu = cpu_ms() - c0;
	h1 = handles();
	memset(&pmc, 0, sizeof pmc);
	pmc.cb = sizeof pmc;
	GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
	printf("RESULT mode=%s n=%zu wall_ms=%.1f cpu_ms=%.1f peak_ws_kib=%zu "
	       "handles_before=%lu "
	       "handles_after=%lu done=%zu result=%d prepare_ms=%llu "
	       "journal_ms=%llu rename_ms=%llu "
	       "rollback_ms=%llu opens=%zu id_queries=%zu renames=%zu "
	       "enumerations=%zu "
	       "log_writes=%zu log_flushes=%zu rolled_back=%zu\n",
	        mode, n, wall, cpu, (size_t)(pmc.PeakWorkingSetSize / 1024),
	        (unsigned long)h0, (unsigned long)h1, renamed, (int)rep.result,
	        (unsigned long long)rep.ms_prepare,
	        (unsigned long long)rep.ms_journal,
	        (unsigned long long)rep.ms_rename,
	        (unsigned long long)rep.ms_rollback, rep.calls.opens,
	        rep.calls.id_queries, rep.calls.renames, rep.calls.enumerations,
	        rep.log.writes, rep.log.flushes, rep.rolled_back);
	free_fixture(&f);
	return 0;
}

static bool run_child(const wchar_t *exe, const char *mode, size_t n)
{
	wchar_t cmd[MAX_PATH + 100];
	wchar_t wmode[32];
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	DWORD code = 1;

	MultiByteToWideChar(CP_ACP, 0, mode, -1, wmode, 32);
	swprintf(
	        cmd, MAX_PATH + 100, L"\"%ls\" --child %ls %zu", exe, wmode, n);
	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	fflush(stdout);
	if (!CreateProcessW(
	            NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
		return false;
	}
	WaitForSingleObject(pi.hProcess, 30 * 60 * 1000);
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return 0 == code;
}

int main(int argc, char **argv)
{
	size_t sizes[8] = {10, 100, 1000, 10000};
	size_t n_sizes = 4;
	wchar_t exe[MAX_PATH];
	char vol[64] = "?";
	wchar_t tmp[MAX_PATH];

	if (4 == argc && !strcmp(argv[1], "--child")) {
		return child(argv[2], (size_t)strtoull(argv[3], NULL, 10));
	}
	if (argc > 1) {
		n_sizes = 0;
		for (int i = 1; i < argc && n_sizes < 8; i++) {
			sizes[n_sizes++] = (size_t)strtoull(argv[i], NULL, 10);
		}
	}
	GetModuleFileNameW(NULL, exe, MAX_PATH);
	GetTempPathW(MAX_PATH, tmp);
	{
		wchar_t fs[32] = L"?", root[4] = {tmp[0], L':', L'\\', 0};

		GetVolumeInformationW(root, NULL, 0, NULL, NULL, NULL, fs, 32);
		snprintf(vol, sizeof vol, "%lc: %ls", tmp[0], fs);
	}
	printf("configuration %s; temp volume %s; each line is one child "
	       "process\n",
	        WF_BENCH_CONFIGURATION, vol);
	for (size_t s = 0; s < n_sizes; s++) {
		if (!run_child(exe, "engine", sizes[s]) ||
		        !run_child(exe, "single", sizes[s]) ||
		        !run_child(exe, "rollback", sizes[s])) {
			fprintf(stderr, "a child failed at n=%zu\n", sizes[s]);
			return 1;
		}
	}
	return 0;
}
