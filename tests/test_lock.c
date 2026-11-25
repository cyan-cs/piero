/*
 * Who has a file open, and closing or ending one program (DR-DRAFT-024).
 *
 * Pure: the error classes and the policy, rule by rule. Windows: Restart Manager
 * against helper programs this test starts itself (tests/lock_helper.c; never a
 * program of the user's): the PID and start time match, the answer changes when the
 * helper ends, nothing leaks; asking to close (a helper that closes, one that
 * refuses, one that asks first, one with no window); ending (refused for a wrong
 * start time, for "self", for a system process; done for the helper only); the
 * worker. The system processes are only described, never acted on.
 */
#include <windows.h>

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "process/wf_lock.h"
#include "process/wf_lockworker_win.h"
#include "process/wf_process_control_win.h"
#include "process/wf_restart_manager_win.h"
#include "wf_test.h"

static const wchar_t *g_helper;

static void classes(void)
{
	CHECK(WF_LOCK_IN_USE == wf_lock_classify(32));
	CHECK(WF_LOCK_IN_USE == wf_lock_classify(33));
	CHECK(WF_LOCK_IN_USE == wf_lock_classify(1224));
	CHECK(WF_LOCK_MAYBE == wf_lock_classify(5));
	CHECK(WF_LOCK_NOT == wf_lock_classify(0));
	CHECK(WF_LOCK_NOT == wf_lock_classify(2));
	CHECK(WF_LOCK_NOT == wf_lock_classify(80));
	CHECK(WF_LOCK_NOT == wf_lock_classify(112));
}

static WfProcFacts good(void)
{
	WfProcFacts f;

	memset(&f, 0, sizeof f);
	f.pid = 5000;
	f.self_pid = 4000;
	f.identity_ok = true;
	f.facts_ok = true;
	f.session = 1;
	f.user_known = f.same_user = true;
	f.type = WF_LOCKAPP_MAIN_WINDOW;
	f.can_terminate = true;
	f.has_window = true;
	return f;
}

static void policy(void)
{
	WfProcFacts f = good();

	CHECK(WF_DENY_NONE == wf_lock_decide(&f, true));
	CHECK(WF_DENY_NONE == wf_lock_decide(&f, false));
	CHECK(WF_DENY_UNKNOWN == wf_lock_decide(NULL, true));
	f = good();
	f.pid = f.self_pid;
	CHECK(WF_DENY_SELF == wf_lock_decide(&f, true));
	CHECK(WF_DENY_SELF == wf_lock_decide(&f, false));
	f = good();
	f.ancestor = true;
	CHECK(WF_DENY_ANCESTOR == wf_lock_decide(&f, true));
	f = good();
	f.identity_ok = false;
	CHECK(WF_DENY_IDENTITY == wf_lock_decide(&f, true));
	CHECK(WF_DENY_IDENTITY == wf_lock_decide(&f, false));
	f = good();
	f.facts_ok = false;
	CHECK(WF_DENY_UNKNOWN == wf_lock_decide(&f, true));
	f = good();
	f.pid = 4;
	CHECK(WF_DENY_SYSTEM == wf_lock_decide(&f, true));
	f = good();
	f.session = 0;
	CHECK(WF_DENY_SYSTEM == wf_lock_decide(&f, true));
	f = good();
	f.critical = true;
	CHECK(WF_DENY_CRITICAL == wf_lock_decide(&f, true));
	f = good();
	f.is_protected = true;
	CHECK(WF_DENY_PROTECTED == wf_lock_decide(&f, true));
	f = good();
	f.type = WF_LOCKAPP_SERVICE;
	CHECK(WF_DENY_KIND == wf_lock_decide(&f, true));
	f.type = WF_LOCKAPP_EXPLORER;
	CHECK(WF_DENY_KIND == wf_lock_decide(&f, false));
	f.type = WF_LOCKAPP_CRITICAL;
	CHECK(WF_DENY_KIND == wf_lock_decide(&f, true));
	f = good();
	f.user_known = false;
	CHECK(WF_DENY_UNKNOWN == wf_lock_decide(&f, true));
	f = good();
	f.same_user = false;
	CHECK(WF_DENY_OTHER_USER == wf_lock_decide(&f, true));
	f = good();
	f.system_image = true;
	CHECK(WF_DENY_SYSTEM_IMAGE == wf_lock_decide(&f, true));
	f = good();
	f.can_terminate = false;
	CHECK(WF_DENY_NO_ACCESS == wf_lock_decide(&f, true));
	CHECK(WF_DENY_NONE ==
	        wf_lock_decide(&f, false)); /* closing needs no right */
	f = good();
	f.has_window = false;
	CHECK(WF_DENY_NO_WINDOW == wf_lock_decide(&f, false));
	CHECK(WF_DENY_NONE ==
	        wf_lock_decide(&f, true)); /* ending needs no window */
	/* the order: self first, then the host, then identity */
	f = good();
	f.pid = f.self_pid;
	f.identity_ok = false;
	f.critical = true;
	CHECK(WF_DENY_SELF == wf_lock_decide(&f, true));
	f = good();
	f.ancestor = true;
	f.identity_ok = false;
	CHECK(WF_DENY_ANCESTOR == wf_lock_decide(&f, true));
	CHECK(wf_lock_core_image_name(L"CSRSS.exe"));
	CHECK(wf_lock_core_image_name(L"svchost.exe"));
	CHECK(!wf_lock_core_image_name(L"notepad.exe"));
	CHECK(!wf_lock_core_image_name(NULL));
	{
		WfLockHolder a = {0}, b = {0};

		a.pid = b.pid = 7;
		a.start = b.start = 99;
		CHECK(wf_lock_same_process(&a, &b));
		b.start = 100;
		CHECK(!wf_lock_same_process(&a, &b));
		a.start = b.start = 0;
		CHECK(!wf_lock_same_process(&a, &b)); /* unknown is not equal */
	}
}

typedef struct
{
	HANDLE proc;
	DWORD pid;
	uint64_t start;
	wchar_t file[MAX_PATH], ready[MAX_PATH];
} Helper;

static void temp_name(wchar_t *out, const wchar_t *tag)
{
	wchar_t dir[MAX_PATH];
	static unsigned n;

	GetTempPathW(MAX_PATH, dir);
	swprintf(out, MAX_PATH, L"%lspiero-lock-%lu-%u-%ls", dir,
	        GetCurrentProcessId(), n++, tag);
}

/* Every helper is in a job that ends them all when this test ends, however it
 * ends (a failing check may exit at once): none is left behind. */
static HANDLE g_job;

static void job_assign(HANDLE process)
{
	if (!g_job) {
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim;

		g_job = CreateJobObjectW(NULL, NULL);
		memset(&lim, 0, sizeof lim);
		lim.BasicLimitInformation.LimitFlags =
		        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		if (g_job) {
			SetInformationJobObject(g_job,
			        JobObjectExtendedLimitInformation, &lim,
			        sizeof lim);
		}
	}
	if (g_job) {
		(void)AssignProcessToJobObject(g_job, process);
	}
}

static bool helper_start(Helper *h, const wchar_t *mode)
{
	wchar_t cmd[3 * MAX_PATH];
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	FILETIME c, e, k, u;

	memset(h, 0, sizeof *h);
	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	temp_name(h->file, L"file.dat");
	temp_name(h->ready, L"ready");
	DeleteFileW(h->ready);
	swprintf(cmd, 3 * MAX_PATH, L"\"%ls\" %ls \"%ls\" \"%ls\"", g_helper,
	        mode, h->file, h->ready);
	if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
	            NULL, NULL, &si, &pi)) {
		return false;
	}
	CloseHandle(pi.hThread);
	job_assign(pi.hProcess);
	h->proc = pi.hProcess;
	h->pid = pi.dwProcessId;
	for (int i = 0; i < 200; i++) {
		if (INVALID_FILE_ATTRIBUTES != GetFileAttributesW(h->ready)) {
			break;
		}
		Sleep(50);
	}
	GetProcessTimes(h->proc, &c, &e, &k, &u);
	h->start = wf_proc_ft64(c);
	return INVALID_FILE_ATTRIBUTES != GetFileAttributesW(h->ready);
}

static bool alive(const Helper *h)
{
	return WAIT_TIMEOUT == WaitForSingleObject(h->proc, 0);
}

static void helper_stop(Helper *h)
{
	if (h->proc) {
		TerminateProcess(h->proc, 0);
		WaitForSingleObject(h->proc, 5000);
		CloseHandle(h->proc);
	}
	DeleteFileW(h->file);
	DeleteFileW(h->ready);
	memset(h, 0, sizeof *h);
}

static const WfLockHolder *find(const WfLockList *l, DWORD pid)
{
	for (size_t i = 0; i < l->n; i++) {
		if (l->h[i].pid == pid) {
			return &l->h[i];
		}
	}
	return NULL;
}

static void finds_the_helper(void)
{
	Helper h;
	WfLockList l;
	unsigned long err = 0;
	const WfLockHolder *f;

	CHECK(helper_start(&h, L"hold"));
	CHECK(WF_OK == wf_rm_query(h.file, NULL, &l, &err));
	f = find(&l, h.pid);
	CHECK(NULL != f);
	if (f) {
		CHECK(f->start ==
		        h.start); /* PID and start time are the process's */
		CHECK(f->identity_ok);
		CHECK(f->exe[0] != L'\0');
		CHECK(WF_DENY_NONE == f->end_deny); /* ending is allowed */
		CHECK(WF_DENY_NO_WINDOW ==
		        f->close_deny); /* no window to ask */
	}
	wf_locklist_free(&l);
	/* a program that is gone is no longer named, and nothing is guessed */
	{
		wchar_t gone[MAX_PATH];

		wcscpy_s(gone, MAX_PATH, h.file);
		helper_stop(&h);
		CHECK(WF_E_NOENT == wf_rm_query(gone, NULL, &l, &err));
	}
	CHECK(0 == l.n);
}

static void answer_changes_when_the_holder_ends(void)
{
	Helper h;
	WfLockList l;
	unsigned long err;
	bool gone = false;

	CHECK(helper_start(&h, L"hold"));
	TerminateProcess(h.proc, 0);
	WaitForSingleObject(h.proc, 5000);
	for (int i = 0; i < 40 && !gone; i++) {
		CHECK(WF_OK == wf_rm_query(h.file, NULL, &l, &err));
		gone = NULL == find(&l, h.pid);
		wf_locklist_free(&l);
		if (!gone) {
			Sleep(50);
		}
	}
	CHECK(gone);
	helper_stop(&h);
}

static void odd_requests_are_handled(void)
{
	WfLockList l;
	unsigned long err = 0;
	wchar_t dir[MAX_PATH], longp[400];
	Helper h;

	GetTempPathW(MAX_PATH, dir);
	CHECK(WF_E_INVAL == wf_rm_query(NULL, NULL, &l, &err));
	CHECK(WF_E_INVAL == wf_rm_query(L"relative\\x.txt", NULL, &l, &err));
	CHECK(WF_E_INVAL ==
	        wf_rm_query(L"\\\\server\\share\\x.txt", NULL, &l, &err));
	CHECK(WF_E_INVAL == wf_rm_query(dir, NULL, &l, &err)); /* a folder */
	CHECK(0 == l.n);
	wcscpy_s(longp, 400, L"C:\\");
	for (int i = 0; i < 40; i++) {
		wcscat_s(longp, 400, L"abcdefgh\\");
	}
	CHECK(WF_E_RANGE == wf_rm_query(longp, NULL, &l, &err));
	/* a file nobody has open: an empty list, which is not "no lock" */
	CHECK(helper_start(&h, L"hold"));
	CHECK(WF_OK == wf_rm_query(h.ready, NULL, &l, &err));
	CHECK(NULL == find(&l, h.pid));
	wf_locklist_free(&l);
	helper_stop(&h);
}

static void nothing_leaks(void)
{
	Helper h;
	WfLockList l;
	unsigned long err;
	DWORD before = 0, after = 0;

	CHECK(helper_start(&h, L"hold"));
	CHECK(WF_OK == wf_rm_query(h.file, NULL, &l, &err)); /* warm up */
	wf_locklist_free(&l);
	GetProcessHandleCount(GetCurrentProcess(), &before);
	for (int i = 0; i < 60; i++) {
		CHECK(WF_OK == wf_rm_query(h.file, NULL, &l, &err));
		wf_locklist_free(&l);
	}
	GetProcessHandleCount(GetCurrentProcess(), &after);
	CHECK(0 == wf_rm_open_sessions());
	CHECK(after <= before + 3);
	helper_stop(&h);
}

static void closing(void)
{
	Helper h;
	WfProcAction a;

	/* a program with a window that closes */
	CHECK(helper_start(&h, L"close"));
	a = wf_proc_request_close(
	        h.pid, h.start, WF_LOCKAPP_MAIN_WINDOW, 5000, NULL, NULL);
	CHECK(WF_LOCKRES_EXITED == a.result);
	CHECK(!alive(&h));
	helper_stop(&h);
	/* one that ignores it: still running afterwards, and nothing is escalated */
	CHECK(helper_start(&h, L"ignore"));
	a = wf_proc_request_close(
	        h.pid, h.start, WF_LOCKAPP_MAIN_WINDOW, 1200, NULL, NULL);
	CHECK(WF_LOCKRES_STILL == a.result);
	CHECK(alive(&h));
	/* one that asks first: the first request is not an exit */
	helper_stop(&h);
	CHECK(helper_start(&h, L"unsaved"));
	a = wf_proc_request_close(
	        h.pid, h.start, WF_LOCKAPP_MAIN_WINDOW, 800, NULL, NULL);
	CHECK(WF_LOCKRES_STILL == a.result);
	CHECK(alive(&h));
	helper_stop(&h);
	/* no window: nothing to ask, nothing done */
	CHECK(helper_start(&h, L"hold"));
	a = wf_proc_request_close(
	        h.pid, h.start, WF_LOCKAPP_UNKNOWN, 800, NULL, NULL);
	CHECK(WF_LOCKRES_DENIED == a.result && WF_DENY_NO_WINDOW == a.deny);
	CHECK(alive(&h));
	/* a wrong start time (the PID may have been reused) */
	helper_stop(&h);
	CHECK(helper_start(&h, L"close"));
	a = wf_proc_request_close(
	        h.pid, h.start + 1, WF_LOCKAPP_MAIN_WINDOW, 800, NULL, NULL);
	CHECK(WF_LOCKRES_DENIED == a.result && WF_DENY_IDENTITY == a.deny);
	CHECK(alive(&h));
	helper_stop(&h);
}

static void ending(void)
{
	Helper h;
	WfProcAction a;
	WfProcEnv env;
	WfLockHolder me = {0}, sys = {0};

	CHECK(helper_start(&h, L"hold"));
	/* PID and start time must both be right */
	a = wf_proc_end(h.pid, h.start + 1, WF_LOCKAPP_UNKNOWN, 3000, NULL);
	CHECK(WF_LOCKRES_DENIED == a.result && WF_DENY_IDENTITY == a.deny);
	CHECK(alive(&h));
	a = wf_proc_end(h.pid, 0, WF_LOCKAPP_UNKNOWN, 3000, NULL);
	CHECK(WF_LOCKRES_DENIED == a.result && WF_DENY_IDENTITY == a.deny);
	CHECK(alive(&h));
	/* a service or the shell is never ended, whatever RM called it */
	a = wf_proc_end(h.pid, h.start, WF_LOCKAPP_SERVICE, 3000, NULL);
	CHECK(WF_LOCKRES_DENIED == a.result && WF_DENY_KIND == a.deny);
	CHECK(alive(&h));
	/* "self" is refused, here by naming the helper as self */
	env.self_pid = h.pid;
	a = wf_proc_end(h.pid, h.start, WF_LOCKAPP_UNKNOWN, 3000, &env);
	CHECK(WF_LOCKRES_DENIED == a.result && WF_DENY_SELF == a.deny);
	CHECK(alive(&h));
	/* the real test process is described as self; never acted on */
	me.pid = GetCurrentProcessId();
	{
		FILETIME c, e, k, u;

		GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
		me.start = wf_proc_ft64(c);
	}
	wf_proc_describe(&me, NULL);
	CHECK(WF_DENY_SELF == me.end_deny && WF_DENY_SELF == me.close_deny);
	{
		/* the holders of one look share one reading of the process list; one
		 * described alone reads it for itself, and says the same */
		WfLockHolder again = me, third = me;
		WfProcSnap snap = {0};

		wf_proc_describe_with(&again, NULL, &snap);
		wf_proc_describe_with(&third, NULL, &snap);
		CHECK(1 == snap.reads && snap.ok && snap.n > 0);
		CHECK(again.end_deny == me.end_deny &&
		        third.close_deny == me.close_deny);
		wf_proc_snap_free(&snap);
	}
	/* the System process: described only */
	sys.pid = 4;
	sys.start = 1;
	wf_proc_describe(&sys, NULL);
	CHECK(WF_DENY_NONE != sys.end_deny && WF_DENY_NONE != sys.close_deny);
	/* the program that started this test (a shell or a build tool) is the
	 * host of piero in a real run; here it is only described */
	/* the helper, with the right PID and start time, is ended and seen to go */
	a = wf_proc_end(h.pid, h.start, WF_LOCKAPP_UNKNOWN, 5000, NULL);
	CHECK(WF_LOCKRES_EXITED == a.result);
	CHECK(!alive(&h));
	/* a process that is gone */
	a = wf_proc_end(h.pid, h.start, WF_LOCKAPP_UNKNOWN, 1000, NULL);
	CHECK(WF_LOCKRES_GONE == a.result || WF_LOCKRES_DENIED == a.result);
	helper_stop(&h);
}

static bool take(WfLockWorker *w, WfLockAnswer *a, DWORD ms)
{
	if (WAIT_OBJECT_0 != WaitForSingleObject(wf_lockworker_event(w), ms)) {
		return false;
	}
	return wf_lockworker_poll(w, a);
}

static void worker(void)
{
	WfLockWorker *w = NULL;
	WfLockPort p;
	WfLockAnswer a;
	Helper h;
	DWORD before = 0, after = 0;

	CHECK(WF_OK == wf_lockworker_start(&w, NULL));
	p = wf_lockworker_port(w);
	CHECK(helper_start(&h, L"close"));
	CHECK(WF_E_INVAL ==
	        p.query(p.ctx, 0, h.file)); /* a generation is needed */
	CHECK(WF_E_INVAL == p.end(p.ctx, 5, h.pid, 0, WF_LOCKAPP_UNKNOWN));
	CHECK(WF_OK == p.query(p.ctx, 7, h.file));
	/* one request at a time: another is refused, not queued */
	CHECK(WF_E_INVAL == p.query(p.ctx, 8, h.file));
	CHECK(take(w, &a, 10000));
	CHECK(7 == a.gen && WF_LOCKREQ_QUERY == a.req && WF_OK == a.st);
	CHECK(NULL != find(&a.list, h.pid));
	wf_lockanswer_free(&a);
	/* close through the worker, then the list is empty of it */
	CHECK(WF_OK ==
	        p.close(p.ctx, 9, h.pid, h.start, WF_LOCKAPP_MAIN_WINDOW));
	CHECK(take(w, &a, 15000));
	CHECK(9 == a.gen && WF_LOCKREQ_CLOSE == a.req &&
	        WF_LOCKRES_EXITED == a.result);
	wf_lockanswer_free(&a);
	helper_stop(&h);
	/* stopping while a program is being waited for is prompt */
	CHECK(helper_start(&h, L"ignore"));
	CHECK(WF_OK ==
	        p.close(p.ctx, 11, h.pid, h.start, WF_LOCKAPP_MAIN_WINDOW));
	Sleep(300);
	{
		ULONGLONG t0 = GetTickCount64();
		WfWorkerStopReport r;

		GetProcessHandleCount(GetCurrentProcess(), &before);
		r = wf_lockworker_stop(w);
		CHECK(GetTickCount64() - t0 < 4000);
		CHECK(!r.deferred);
		if (r.completion) {
			CloseHandle(r.completion);
		}
		GetProcessHandleCount(GetCurrentProcess(), &after);
		CHECK(after <= before);
	}
	CHECK(alive(&h)); /* it was only asked */
	helper_stop(&h);
}

int wmain(int argc, wchar_t **argv)
{
	static wchar_t beside[MAX_PATH];

	if (argc >= 2) {
		g_helper = argv[1];
	} else { /* the mutation harness copies the helper next to the test */
		wchar_t *slash;

		GetModuleFileNameW(NULL, beside, MAX_PATH);
		slash = wcsrchr(beside, L'\\');
		if (slash) {
			wcscpy_s(slash + 1,
			        (size_t)(beside + MAX_PATH - slash - 1),
			        L"lock_helper.exe");
		}
		g_helper = beside;
	}
	classes();
	policy();
	finds_the_helper();
	answer_changes_when_the_holder_ends();
	odd_requests_are_handled();
	nothing_leaks();
	closing();
	ending();
	worker();
	return TEST_RESULT();
}
