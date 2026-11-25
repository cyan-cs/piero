#include "wf_process_control_win.h"

#include <tlhelp32.h>

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define WF_ANCESTOR_MAX 64u

uint64_t wf_proc_ft64(FILETIME ft)
{
	return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

static uint32_t self_of(const WfProcEnv *env)
{
	return env && env->self_pid ? env->self_pid : GetCurrentProcessId();
}

void wf_proc_snap_free(WfProcSnap *s)
{
	free(s->ids);
	free(s->parents);
	memset(s, 0, sizeof *s);
}

/* Reads the process list into `s` (once: a later call finds it read). */
static void snap_read(WfProcSnap *s)
{
	HANDLE snap;
	PROCESSENTRY32W e;
	size_t cap = 0;

	if (s->read) {
		return;
	}
	s->read = true;
	s->reads++;
	snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (INVALID_HANDLE_VALUE == snap) {
		return;
	}
	e.dwSize = sizeof e;
	if (Process32FirstW(snap, &e)) {
		do {
			if (s->n == cap) {
				size_t grown = cap ? cap * 2 : 256;
				DWORD *a = realloc(s->ids, grown * sizeof *a);
				DWORD *b;

				if (a) {
					s->ids = a;
				}
				b = realloc(s->parents, grown * sizeof *b);
				if (b) {
					s->parents = b;
				}
				if (!a || !b) {
					goto out;
				}
				cap = grown;
			}
			s->ids[s->n] = e.th32ProcessID;
			s->parents[s->n] = e.th32ParentProcessID;
			s->n++;
		} while (Process32NextW(snap, &e));
	}
	s->ok = s->n > 0 && ERROR_NO_MORE_FILES == GetLastError();
out:
	CloseHandle(snap);
}

/* Is `pid` one of the parents (or the parent's parents, ...) of `self`? Sets
 * *known false when the list is incomplete or the chain cannot reach a root.
 * A reused parent PID may cause an unrelated process to be refused. */
static bool is_ancestor(WfProcSnap *s, uint32_t self, uint32_t pid, bool *known)
{
	uint32_t cur = self;

	snap_read(s);
	*known = false;
	if (!s->ok) {
		return false;
	}
	for (unsigned step = 0; step < WF_ANCESTOR_MAX && cur; step++) {
		uint32_t next = 0;
		bool found = false;

		for (size_t i = 0; i < s->n; i++) {
			if (s->ids[i] == cur) {
				next = s->parents[i];
				found = true;
				break;
			}
		}
		if (!found) {
			return false;
		}
		if (!next) {
			*known = true;
			return false;
		}
		if (next == pid) {
			*known = true;
			return true;
		}
		cur = next;
	}
	return false;
}

typedef struct
{
	HANDLE h;
	WfProcFacts f;
	wchar_t exe[WF_LOCK_EXE_CHARS];
} Inspected;

static bool token_user(HANDLE token, BYTE *buf, DWORD cap)
{
	DWORD got = 0;

	return GetTokenInformation(token, TokenUser, buf, cap, &got) &&
	        got <= cap;
}

/* The user of the process `h` is the user of this one. */
static void same_user_of(HANDLE h, WfProcFacts *f)
{
	HANDLE mine = NULL, theirs = NULL;
	BYTE a[256], b[256];

	f->user_known = false;
	f->same_user = false;
	if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mine) &&
	        OpenProcessToken(h, TOKEN_QUERY, &theirs) &&
	        token_user(mine, a, sizeof a) &&
	        token_user(theirs, b, sizeof b)) {
		f->user_known = true;
		f->same_user = EqualSid(((TOKEN_USER *)(void *)a)->User.Sid,
		        ((TOKEN_USER *)(void *)b)->User.Sid);
	}
	if (mine) {
		CloseHandle(mine);
	}
	if (theirs) {
		CloseHandle(theirs);
	}
}

static BOOL CALLBACK window_of(HWND w, LPARAM param)
{
	DWORD *pid_and_count = (DWORD *)param;
	DWORD owner = 0;

	(void)GetWindowThreadProcessId(w, &owner);
	if (owner == pid_and_count[0] && IsWindowVisible(w)) {
		pid_and_count[1]++;
	}
	return TRUE;
}

static bool has_visible_window(uint32_t pid)
{
	DWORD state[2] = {pid, 0};

	(void)EnumWindows(window_of, (LPARAM)state);
	return state[1] > 0;
}

static bool under_windows_dir(const wchar_t *exe)
{
	wchar_t dir[MAX_PATH];
	UINT n = GetSystemWindowsDirectoryW(dir, MAX_PATH);

	if (0 == n || n >= MAX_PATH) {
		return true; /* unknown: assume it is, which refuses more */
	}
	return 0 == _wcsnicmp(exe, dir, n) &&
	        (L'\\' == exe[n] || L'\0' == exe[n]);
}

static const wchar_t *leaf_of(const wchar_t *path)
{
	const wchar_t *slash = wcsrchr(path, L'\\');

	return slash ? slash + 1 : path;
}

/* Reads the facts of the process behind `in->h`. `in->f.pid` and the
 * identity come from the caller's pid and start. */
static void gather(Inspected *in,
        uint64_t start,
        WfLockAppType type,
        bool can_terminate,
        const WfProcEnv *env,
        WfProcSnap *snap)
{
	WfProcFacts *f = &in->f;
	FILETIME created, exited, kernel, user;
	DWORD len = WF_LOCK_EXE_CHARS, session = 0;
	BOOL critical = FALSE;
	PROCESS_PROTECTION_LEVEL_INFORMATION prot;
	bool ancestor_known = false, ok = true;

	f->self_pid = self_of(env);
	f->type = type;
	f->can_terminate = can_terminate;
	f->identity_ok = start &&
	        GetProcessTimes(in->h, &created, &exited, &kernel, &user) &&
	        wf_proc_ft64(created) == start;
	in->exe[0] = L'\0';
	if (!QueryFullProcessImageNameW(in->h, 0, in->exe, &len)) {
		in->exe[0] = L'\0';
		ok = false;
	}
	if (!ProcessIdToSessionId(f->pid, &session)) {
		ok = false;
	}
	f->session = session;
	if (!IsProcessCritical(in->h, &critical)) {
		ok = false;
	}
	f->critical = critical;
	memset(&prot, 0, sizeof prot);
	if (!GetProcessInformation(
	            in->h, ProcessProtectionLevelInfo, &prot, sizeof prot)) {
		ok = false;
	}
	f->is_protected = PROTECTION_LEVEL_NONE != prot.ProtectionLevel;
	same_user_of(in->h, f);
	f->ancestor = is_ancestor(snap, f->self_pid, f->pid, &ancestor_known);
	if (!ancestor_known) {
		ok = false;
	}
	f->system_image = in->exe[0] && under_windows_dir(in->exe) &&
	        wf_lock_core_image_name(leaf_of(in->exe));
	if (!in->exe[0]) {
		f->system_image = false; /* unknown image: facts_ok is false */
	}
	f->has_window = has_visible_window(f->pid);
	f->facts_ok = ok;
}

/* Opens the process for the check (and, when asked, for ending it). */
static bool open_for(uint32_t pid, bool terminate, HANDLE *out)
{
	DWORD base = PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE;

	*out = OpenProcess(
	        base | (terminate ? PROCESS_TERMINATE : 0), FALSE, pid);
	return NULL != *out;
}

void wf_proc_describe_with(
        WfLockHolder *h, const WfProcEnv *env, WfProcSnap *snap)
{
	Inspected in;
	bool can_terminate = true;

	memset(&in, 0, sizeof in);
	in.f.pid = h->pid;
	h->identity_ok = false;
	h->exe[0] = L'\0';
	h->has_window = false;
	h->session = 0;
	if (!open_for(h->pid, true, &in.h)) {
		can_terminate = false;
		if (!open_for(h->pid, false, &in.h)) {
			WfProcFacts none;

			memset(&none, 0, sizeof none);
			none.pid = h->pid;
			none.self_pid = self_of(env);
			none.type = h->type;
			h->close_deny = wf_lock_decide(&none, false);
			h->end_deny = wf_lock_decide(&none, true);
			return;
		}
	}
	if (snap) {
		gather(&in, h->start, h->type, can_terminate, env, snap);
	} else {
		WfProcSnap own = {0};

		gather(&in, h->start, h->type, can_terminate, env, &own);
		wf_proc_snap_free(&own);
	}
	h->identity_ok = in.f.identity_ok;
	wcsncpy_s(h->exe, WF_LOCK_EXE_CHARS, in.exe, _TRUNCATE);
	h->has_window = in.f.has_window;
	h->session = in.f.session;
	h->close_deny = wf_lock_decide(&in.f, false);
	h->end_deny = wf_lock_decide(&in.f, true);
	CloseHandle(in.h);
}

void wf_proc_describe(WfLockHolder *h, const WfProcEnv *env)
{
	wf_proc_describe_with(h, env, NULL);
}

typedef struct
{
	DWORD pid;
	DWORD posted;
} CloseState;

static BOOL CALLBACK close_window_of(HWND w, LPARAM param)
{
	CloseState *s = (CloseState *)param;
	DWORD owner = 0;

	(void)GetWindowThreadProcessId(w, &owner);
	if (owner == s->pid && IsWindowVisible(w) &&
	        PostMessageW(w, WM_CLOSE, 0, 0)) {
		s->posted++;
	}
	return TRUE;
}

/* The checked handle for an action, or the refusal. */
static bool open_checked(uint32_t pid,
        uint64_t start,
        WfLockAppType type,
        bool ending,
        const WfProcEnv *env,
        Inspected *in,
        WfProcAction *refused)
{
	WfLockDeny deny;

	memset(in, 0, sizeof *in);
	memset(refused, 0, sizeof *refused);
	in->f.pid = pid;
	if (!open_for(pid, ending, &in->h)) {
		DWORD e = GetLastError();

		if (ERROR_INVALID_PARAMETER == e) {
			refused->result = WF_LOCKRES_GONE; /* no such process */
		} else if (ERROR_ACCESS_DENIED == e) {
			refused->result = WF_LOCKRES_DENIED;
			refused->deny =
			        ending ? WF_DENY_NO_ACCESS : WF_DENY_UNKNOWN;
		} else {
			refused->result = WF_LOCKRES_FAILED;
			refused->error = e;
		}
		return false;
	}
	{
		WfProcSnap fresh = {
		        0}; /* an action reads the list for itself */

		gather(in, start, type, ending, env, &fresh);
		wf_proc_snap_free(&fresh);
	}
	deny = wf_lock_decide(&in->f, ending);
	/* closing needs a window to ask; ending does not */
	if (WF_DENY_NONE != deny) {
		refused->result = WF_LOCKRES_DENIED;
		refused->deny = deny;
		CloseHandle(in->h);
		in->h = NULL;
		return false;
	}
	return true;
}

/* Waits for the process on its own handle. */
static WfLockActionResult wait_exit(HANDLE proc, DWORD wait_ms, HANDLE cancel)
{
	HANDLE both[2] = {proc, cancel};
	DWORD r = WaitForMultipleObjects(cancel ? 2 : 1, both, FALSE, wait_ms);

	if (WAIT_OBJECT_0 == r) {
		return WF_LOCKRES_EXITED;
	}
	return WF_LOCKRES_STILL;
}

WfProcAction wf_proc_request_close(uint32_t pid,
        uint64_t start,
        WfLockAppType type,
        DWORD wait_ms,
        HANDLE cancel,
        const WfProcEnv *env)
{
	Inspected in;
	WfProcAction out;
	CloseState s;

	if (!open_checked(pid, start, type, false, env, &in, &out)) {
		return out;
	}
	s.pid = pid;
	s.posted = 0;
	(void)EnumWindows(close_window_of, (LPARAM)&s);
	if (0 == s.posted) {
		out.result = WF_LOCKRES_DENIED;
		out.deny = WF_DENY_NO_WINDOW;
	} else {
		out.result = wait_exit(in.h, wait_ms, cancel);
	}
	CloseHandle(in.h);
	return out;
}

WfProcAction wf_proc_end(uint32_t pid,
        uint64_t start,
        WfLockAppType type,
        DWORD wait_ms,
        const WfProcEnv *env)
{
	Inspected in;
	WfProcAction out;

	if (!open_checked(pid, start, type, true, env, &in, &out)) {
		return out;
	}
	if (!TerminateProcess(in.h, 1)) {
		out.result = WF_LOCKRES_FAILED;
		out.error = GetLastError();
	} else {
		out.result = wait_exit(in.h, wait_ms, NULL);
	}
	CloseHandle(in.h);
	return out;
}
