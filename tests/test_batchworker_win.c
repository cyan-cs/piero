/*
 * The batch worker on real folders (DR-DRAFT-022, UI): a batch of 10 and of 1,000
 * files through the port the screen uses (prepare, run, poll), one request at a
 * time, Esc (cancel) in the middle of a run is taken back, stopping the worker
 * during a run puts the names back before it returns, and journals that a
 * process left behind are only counted until recovery is asked for, then
 * recovered by the worker (or kept and reported when damaged). Everything happens
 * in a folder of the temporary directory made for the run; the journals too.
 */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xfer/wf_batchexec_win.h"
#include "xfer/wf_batchworker.h"
#include "wf_test.h"

#define PATHLEN 1200
#define MAXF 2100
#define DEAD 0xdead

static wchar_t root[MAX_PATH + 40];
static wchar_t cur[PATHLEN];  /* the folder of this case */
static wchar_t jdir[PATHLEN]; /* its journal folder */
static int case_no;

static void fresh(void)
{
	case_no++;
	swprintf(cur, PATHLEN, L"%ls\\case%d", root, case_no);
	CreateDirectoryW(cur, NULL);
	swprintf(jdir, PATHLEN, L"%ls\\state%d", root, case_no);
	CreateDirectoryW(jdir,
	        NULL); /* the state folder: the batch\ one is made by the engine */
	swprintf(jdir, PATHLEN, L"%ls\\state%d\\batch", root, case_no);
}

static void put_file(const wchar_t *path, const char *text)
{
	HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD n;

	CHECK(INVALID_HANDLE_VALUE != h);
	if (INVALID_HANDLE_VALUE != h) {
		WriteFile(h, text, (DWORD)strlen(text), &n, NULL);
		CloseHandle(h);
	}
}

static bool exists(const wchar_t *path)
{
	return INVALID_FILE_ATTRIBUTES != GetFileAttributesW(path);
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
			id.kind = WF_ID_STRONG;
			id.volume = info.VolumeSerialNumber;
			memcpy(id.id, info.FileId.Identifier, 16);
		}
		CloseHandle(h);
	}
	return id;
}

static void wipe_tree(const wchar_t *dir)
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
				wipe_tree(f);
			} else {
				SetFileAttributesW(f, FILE_ATTRIBUTE_NORMAL);
				DeleteFileW(f);
			}
		} while (FindNextFileW(h, &d));
		FindClose(h);
	}
	RemoveDirectoryW(dir);
}

static struct
{
	size_t n;
	wchar_t old_path[MAXF][PATHLEN];
	wchar_t old_name[MAXF][24];
	wchar_t new_name[MAXF][24];
	wchar_t new_path[MAXF][PATHLEN];
	WfBxSource src[MAXF];
	WfFileId id[MAXF];
} set;

static void make_set(size_t n)
{
	set.n = n;
	for (size_t i = 0; i < n; i++) {
		char text[24];

		swprintf(set.old_name[i], 24, L"f%zu.txt", i);
		swprintf(set.new_name[i], 24, L"n%06zu.txt", i + 1);
		swprintf(set.old_path[i], PATHLEN, L"%ls\\%ls", cur,
		        set.old_name[i]);
		swprintf(set.new_path[i], PATHLEN, L"%ls\\%ls", cur,
		        set.new_name[i]);
		snprintf(text, sizeof text, "c%zu", i);
		put_file(set.old_path[i], text);
		set.id[i] = id_at(set.old_path[i]);
		set.src[i].path = set.old_path[i];
		set.src[i].expected = set.id[i];
		set.src[i].to_name = set.new_name[i];
	}
}

/* Every file is under its old names, or every one under its new, with its identity. */
static bool all_there(bool renamed)
{
	for (size_t i = 0; i < set.n; i++) {
		const wchar_t *here =
		        renamed ? set.new_path[i] : set.old_path[i];
		const wchar_t *gone =
		        renamed ? set.old_path[i] : set.new_path[i];
		WfFileId id = id_at(here);

		if (!exists(here) || exists(gone) ||
		        !(id.kind == WF_ID_STRONG) ||
		        id.volume != set.id[i].volume ||
		        memcmp(id.id, set.id[i].id, 16)) {
			return false;
		}
	}
	return true;
}

static WfBatchWorker *worker;
static WfBatchPort port;

static void start_worker(void)
{
	CHECK(WF_OK == wf_batchworker_start(&worker, jdir));
	port = wf_batchworker_port(worker);
}

static WfBxDone wait_done(DWORD ms)
{
	WfBxDone d;
	DWORD start = GetTickCount();

	memset(&d, 0, sizeof d);
	d.gen = 0xFFFFFFFFu;
	while (GetTickCount() - start < ms) {
		if (WAIT_OBJECT_0 ==
		                WaitForSingleObject(
		                        wf_batchworker_event(worker), 100) &&
		        wf_batchworker_poll(worker, &d)) {
			return d;
		}
	}
	CHECK_FAIL("the worker did not answer in time");
	return d;
}

static struct WfBatchJob *prepared(WfBxReport *rep)
{
	struct WfBatchJob *job = port.prepare(port.ctx, set.src, set.n, rep);

	CHECK(job && port.ready(port.ctx, job) && WF_BX_PRE_READY == rep->pre);
	return job;
}

static void run_plain(size_t n)
{
	WfBxReport rep;
	struct WfBatchJob *job;
	WfBxDone d;
	LARGE_INTEGER a, b, f;

	fresh();
	make_set(n);
	start_worker();
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&a);
	job = prepared(&rep);
	CHECK(0 == port.unfinished(port.ctx));
	CHECK(WF_OK == port.run(port.ctx, 7, job));
	d = wait_done(120000);
	QueryPerformanceCounter(&b);
	CHECK(7 == d.gen && !d.recover);
	CHECK(WF_BX_SUCCESS == d.rep.result && n == d.rep.steps_done);
	CHECK(n == port.progress(port.ctx));
	CHECK(all_there(true));
	CHECK(0 == port.unfinished(port.ctx));
	printf("through the worker: %zu files, %llu ms\n", n,
	        (unsigned long long)((b.QuadPart - a.QuadPart) * 1000 /
	                f.QuadPart));
	wf_batchworker_stop(worker);
}

static void ten_files(void)
{
	run_plain(10);
}

static void a_thousand_files(void)
{
	run_plain(1000);
}

static void one_request_at_a_time_and_nothing_runs_for_a_refused_plan(void)
{
	WfBxReport rep;
	struct WfBatchJob *job, *second;
	WfBxDone d;

	fresh();
	make_set(300);
	start_worker();
	job = prepared(&rep);
	second = prepared(&rep);
	CHECK(WF_OK == port.run(port.ctx, 1, job));
	/* queued, running, or its answer not yet taken: another is refused */
	CHECK(WF_E_INVAL == port.run(port.ctx, 2, second));
	CHECK(WF_E_INVAL == port.recover(port.ctx, 3));
	d = wait_done(60000);
	CHECK(1 == d.gen && WF_BX_SUCCESS == d.rep.result);
	port.job_free(port.ctx, second); /* still ours: the worker refused it */
	/* a plan with a refused row is not run: the files are already renamed
	 * here, so the same sources are now "missing" and the job is not ready */
	job = port.prepare(port.ctx, set.src, set.n, &rep);
	CHECK(job && !port.ready(port.ctx, job) && WF_BX_PRE_READY != rep.pre);
	CHECK(WF_E_INVAL == port.run(port.ctx, 4, job));
	port.job_free(port.ctx, job);
	CHECK(WF_E_INVAL == port.run(port.ctx, 0, NULL));
	wf_batchworker_stop(worker);
}

static bool wait_progress(size_t at_least)
{
	DWORD start = GetTickCount();

	while (GetTickCount() - start < 60000) {
		if (port.progress(port.ctx) >= at_least) {
			return true;
		}
		Sleep(0);
	}
	return false;
}

static void esc_in_the_middle_puts_every_name_back(void)
{
	WfBxReport rep;
	struct WfBatchJob *job;
	WfBxDone d;

	fresh();
	make_set(2000);
	start_worker();
	job = prepared(&rep);
	CHECK(WF_OK == port.run(port.ctx, 9, job));
	CHECK(wait_progress(1));
	port.cancel(port.ctx);
	d = wait_done(180000);
	CHECK(9 == d.gen);
	CHECK(WF_BX_ROLLED_BACK == d.rep.result);
	CHECK(d.rep.steps_done >= 1 && d.rep.steps_done < 2000);
	CHECK(d.rep.rolled_back == d.rep.steps_done && 0 == d.rep.not_restored);
	CHECK(all_there(false));
	CHECK(0 ==
	        port.unfinished(
	                port.ctx)); /* the journal ends: ROLLED_BACK is closed */
	printf("cancelled after %zu of 2000 steps, all put back\n",
	        d.rep.steps_done);
	wf_batchworker_stop(worker);
}

static void stopping_the_worker_during_a_run_puts_the_names_back_first(void)
{
	WfBxReport rep;
	struct WfBatchJob *job;

	fresh();
	make_set(2000);
	start_worker();
	job = prepared(&rep);
	CHECK(WF_OK == port.run(port.ctx, 3, job));
	CHECK(wait_progress(1));
	{
		WfWorkerStopReport r = wf_batchworker_stop(worker);

		CHECK(!r.deferred); /* it waited for the rollback */
	}
	CHECK(all_there(false));
	CHECK(0 == wf_batch_count_unfinished(jdir));
}

static void die_here(void *ctx, const char *point, size_t step)
{
	(void)ctx;
	if (!strcmp(point, "after_D") && 1 == step) {
		TerminateProcess(GetCurrentProcess(), DEAD);
	}
}

/* The child: the same batch of five, ended by TerminateProcess after step 1 is done. */
static int child_main(void)
{
	wchar_t folder[PATHLEN], journal[PATHLEN];
	WfBatchSource s[5];
	wchar_t paths[5][PATHLEN];
	wchar_t news[5][16];
	WfBatchReport rep;
	WfBatchJob *job;
	WfBatchHooks hooks = {die_here, NULL};
	volatile LONG cancel = 0;

	if (!GetEnvironmentVariableW(L"WF_BW_DIR", folder, PATHLEN) ||
	        !GetEnvironmentVariableW(L"WF_BW_JDIR", journal, PATHLEN)) {
		return 2;
	}
	for (int i = 0; i < 5; i++) {
		swprintf(paths[i], PATHLEN, L"%ls\\f%d.txt", folder, i);
		swprintf(news[i], 16, L"n%06d.txt", i + 1);
		s[i].path = paths[i];
		s[i].expected = id_at(paths[i]);
		s[i].to_name = news[i];
	}
	job = wf_batch_prepare(s, 5, NULL, &rep);
	if (!job || !wf_batch_job_ready(job)) {
		return 3;
	}
	wf_batch_execute(job, journal, &hooks, &cancel, &rep);
	return 4;
}

static bool run_child(void)
{
	wchar_t exe[MAX_PATH], cmd[MAX_PATH + 100];
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	DWORD code = 0;

	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	GetModuleFileNameW(NULL, exe, MAX_PATH);
	swprintf(cmd, MAX_PATH + 100, L"\"%ls\" --child", exe);
	SetEnvironmentVariableW(L"WF_BW_DIR", cur);
	SetEnvironmentVariableW(L"WF_BW_JDIR", jdir);
	if (!CreateProcessW(
	            NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
		return false;
	}
	WaitForSingleObject(pi.hProcess, 60000);
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return DEAD == code;
}

static bool the_one_journal(wchar_t out[PATHLEN])
{
	wchar_t pattern[PATHLEN];
	WIN32_FIND_DATAW d;
	HANDLE h;

	swprintf(pattern, PATHLEN, L"%ls\\*.pbj", jdir);
	h = FindFirstFileW(pattern, &d);
	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	swprintf(out, PATHLEN, L"%ls\\%ls", jdir, d.cFileName);
	FindClose(h);
	return true;
}

static void a_journal_left_behind_is_counted_and_recovered_only_when_asked(void)
{
	WfBxDone d;
	wchar_t journal[PATHLEN];

	fresh();
	make_set(5);
	CHECK(run_child());
	CHECK(the_one_journal(journal));
	/* at "startup": the worker only counts. Two steps had been done. */
	start_worker();
	CHECK(1 == port.unfinished(port.ctx));
	CHECK(exists(set.new_path[0]) && exists(set.new_path[1]) &&
	        !exists(set.old_path[0]) && !exists(set.old_path[1]));
	Sleep(300); /* nothing happens by itself */
	CHECK(exists(set.new_path[0]) && !exists(set.old_path[0]));
	CHECK(1 == port.unfinished(port.ctx));
	/* the person said yes */
	CHECK(WF_OK == port.recover(port.ctx, 5));
	d = wait_done(60000);
	CHECK(5 == d.gen && d.recover);
	CHECK(1 == d.rec_total && 1 == d.rec_rolled_back && 0 == d.rec_open &&
	        0 == d.rec_done);
	CHECK(WF_BX_ROLLED_BACK == d.rep.result);
	CHECK(all_there(false));
	CHECK(0 == port.unfinished(port.ctx));
	/* asking again finds nothing to do */
	CHECK(WF_OK == port.recover(port.ctx, 6));
	d = wait_done(60000);
	CHECK(6 == d.gen && 0 == d.rec_total && WF_BX_SUCCESS == d.rep.result);
	wf_batchworker_stop(worker);
}

static void a_damaged_journal_is_kept_and_reported_not_guessed_from(void)
{
	WfBxDone d;
	wchar_t journal[PATHLEN];
	HANDLE h;
	DWORD n;
	char c;

	fresh();
	make_set(5);
	CHECK(run_child());
	CHECK(the_one_journal(journal));
	h = CreateFileW(journal, GENERIC_READ | GENERIC_WRITE, 0, NULL,
	        OPEN_EXISTING, 0, NULL);
	SetFilePointer(h, 70, NULL, FILE_BEGIN);
	ReadFile(h, &c, 1, &n, NULL);
	c ^= 1;
	SetFilePointer(h, 70, NULL, FILE_BEGIN);
	WriteFile(h, &c, 1, &n, NULL);
	CloseHandle(h);
	start_worker();
	CHECK(1 == port.unfinished(port.ctx));
	CHECK(WF_OK == port.recover(port.ctx, 8));
	d = wait_done(60000);
	CHECK(1 == d.rec_total && 1 == d.rec_open && 0 == d.rec_rolled_back);
	CHECK(WF_BX_RECOVERY_REQUIRED == d.rep.result);
	/* nothing was guessed: the two renamed files stay renamed, the journal stays */
	CHECK(exists(set.new_path[0]) && exists(set.new_path[1]));
	CHECK(1 == port.unfinished(port.ctx));
	wf_batchworker_stop(worker);
}

int main(int argc, char **argv)
{
	wchar_t tmp[MAX_PATH];

	if (2 == argc && !strcmp(argv[1], "--child")) {
		return child_main();
	}
	GetTempPathW(MAX_PATH, tmp);
	swprintf(root, MAX_PATH + 40, L"%lspiero-batchworker-%lu", tmp,
	        GetCurrentProcessId());
	CreateDirectoryW(root, NULL);
	esc_in_the_middle_puts_every_name_back();
	stopping_the_worker_during_a_run_puts_the_names_back_first();
	one_request_at_a_time_and_nothing_runs_for_a_refused_plan();
	a_journal_left_behind_is_counted_and_recovered_only_when_asked();
	a_damaged_journal_is_kept_and_reported_not_guessed_from();
	ten_files();
	a_thousand_files();
	wipe_tree(root);
	return TEST_RESULT();
}
