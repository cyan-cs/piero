#include "wf_gitbackend_win.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../fs/wf_loc.h"

typedef struct
{
	bool known;
	WfGitCap cap;
	uint64_t until; /* unavailable: when to ask again */
} Slot;

static struct
{
	SRWLOCK lock;
	Slot win;
	wchar_t win_exe[MAX_PATH];
	struct
	{
		char name[WF_WSL_NAME_MAX];
		Slot slot;
	} wsl[WF_GIT_DISTROS];
	size_t n_wsl;
	WfGitHooks hooks;
} g = {SRWLOCK_INIT, {0}, {0}, {{{0}, {0}}}, 0, {0}};

void wf_git_hooks_set(const WfGitHooks *hooks)
{
	AcquireSRWLockExclusive(&g.lock);
	memset(&g.hooks, 0, sizeof g.hooks);
	if (hooks) {
		g.hooks = *hooks;
	}
	ReleaseSRWLockExclusive(&g.lock);
	wf_git_cache_reset();
}

void wf_git_cache_reset(void)
{
	AcquireSRWLockExclusive(&g.lock);
	memset(&g.win, 0, sizeof g.win);
	memset(g.win_exe, 0, sizeof g.win_exe);
	memset(g.wsl, 0, sizeof g.wsl);
	g.n_wsl = 0;
	ReleaseSRWLockExclusive(&g.lock);
}

static uint64_t now_ms(void)
{
	return g.hooks.now_ms ? g.hooks.now_ms() : GetTickCount64();
}

static WfStatus run_process(const WfGitRun *run, WfGitRunResult *res)
{
	return g.hooks.run ? g.hooks.run(g.hooks.run_ctx, run, res)
	                   : wf_gitproc_run(run, res);
}

static bool exists_file(const wchar_t *path)
{
	DWORD a = GetFileAttributesW(path);

	return INVALID_FILE_ATTRIBUTES != a && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* The first absolute PATH entry that holds git.exe. A relative entry, an empty
 * one (both mean "here") and a network entry are never used: the program that
 * starts must not come from the folder the user is looking at. */
static bool scan_path_for_git(wchar_t *out, size_t cap)
{
	DWORD need = GetEnvironmentVariableW(L"PATH", NULL, 0);
	wchar_t *path, *p, *entry_end;
	bool found = false;

	if (!need) {
		return false;
	}
	path = malloc(need * sizeof *path);
	if (!path || !GetEnvironmentVariableW(L"PATH", path, need)) {
		free(path);
		return false;
	}
	for (p = path; *p && !found; p = entry_end + (*entry_end ? 1 : 0)) {
		size_t n;
		wchar_t candidate[MAX_PATH];
		const wchar_t *e = p;

		entry_end = wcschr(p, L';');
		if (!entry_end) {
			entry_end = p + wcslen(p);
		}
		n = (size_t)(entry_end - e);
		if (n >= 2 && L'"' == e[0] && L'"' == e[n - 1]) {
			e++;
			n -= 2;
		}
		while (n && L'\\' == e[n - 1]) {
			n--;
		}
		/* a drive path: "X:\..." */
		if (n < 3 || n + 9 >= MAX_PATH || L':' != e[1] ||
		        L'\\' != e[2] ||
		        !((e[0] >= L'A' && e[0] <= L'Z') ||
		                (e[0] >= L'a' && e[0] <= L'z'))) {
			continue;
		}
		memcpy(candidate, e, n * sizeof *e);
		wcscpy_s(candidate + n, MAX_PATH - n, L"\\git.exe");
		if (exists_file(candidate) && wcslen(candidate) < cap) {
			wcscpy_s(out, cap, candidate);
			found = true;
		}
		if (!*entry_end) {
			break;
		}
	}
	free(path);
	return found;
}

static bool find_windows_git(wchar_t *out, size_t cap)
{
	return g.hooks.find_windows_git ? g.hooks.find_windows_git(out, cap)
	                                : scan_path_for_git(out, cap);
}

/* The path of git.exe, learned once. */
static bool windows_exe(wchar_t *out, size_t cap)
{
	bool ok = false;
	wchar_t found[MAX_PATH];
	uint64_t now = now_ms();

	AcquireSRWLockShared(&g.lock);
	if (g.win.known && WF_GIT_AVAILABLE == g.win.cap) {
		wcscpy_s(out, cap, g.win_exe);
		ok = true;
	} else if (g.win.known && now < g.win.until) {
		ReleaseSRWLockShared(&g.lock);
		return false;
	}
	ReleaseSRWLockShared(&g.lock);
	if (ok) {
		return true;
	}
	ok = find_windows_git(found, ARRAYSIZE(found));
	AcquireSRWLockExclusive(&g.lock);
	g.win.known = true;
	g.win.cap = ok ? WF_GIT_AVAILABLE : WF_GIT_UNAVAILABLE;
	g.win.until = ok ? 0 : now + WF_GIT_RETRY_MS;
	if (ok) {
		wcscpy_s(g.win_exe, ARRAYSIZE(g.win_exe), found);
		wcscpy_s(out, cap, found);
	}
	ReleaseSRWLockExclusive(&g.lock);
	return ok;
}

static bool wsl_exe(wchar_t *out, size_t cap)
{
	wchar_t dir[MAX_PATH];
	UINT n = GetSystemDirectoryW(dir, ARRAYSIZE(dir));

	if (0 == n || n >= ARRAYSIZE(dir) - 12) {
		return false;
	}
	if (swprintf(out, cap, L"%ls\\wsl.exe", dir) < 0) {
		return false;
	}
	return exists_file(out);
}

static bool widen_ascii(const char *s, wchar_t *out, size_t cap)
{
	size_t n = strlen(s);

	if (n + 1 > cap) {
		return false;
	}
	for (size_t i = 0; i <= n; i++) {
		if ((unsigned char)s[i] > 0x7F) {
			return false;
		}
		out[i] = (wchar_t)(unsigned char)s[i];
	}
	return true;
}

static bool distro_ok(const char *name)
{
	WfLocId id;

	return name && name[0] && WF_OK == wf_locid_wsl(&id, name);
}

static WfGitCap probe_wsl(
        const char *distro, bool (*cancelled)(void *), void *cctx)
{
	wchar_t exe[MAX_PATH], d[WF_WSL_NAME_MAX];
	const wchar_t *args[] = {
	        L"-d", d, L"--exec", L"git", L"--version", NULL};
	WfGitRun run;
	WfGitRunResult res;
	WfGitCap cap = WF_GIT_UNAVAILABLE;

	if (!distro_ok(distro) || !widen_ascii(distro, d, ARRAYSIZE(d)) ||
	        !wsl_exe(exe, ARRAYSIZE(exe))) {
		return WF_GIT_UNAVAILABLE;
	}
	memset(&run, 0, sizeof run);
	run.exe = exe;
	run.args = args;
	run.timeout_ms = 30000; /* the distribution may have to start */
	run.max_out = 4096;
	run.cancelled = cancelled;
	run.cancel_ctx = cctx;
	if (WF_OK == run_process(&run, &res) && 0 == res.exit_code && res.out &&
	        0 == strncmp((const char *)res.out, "git version", 11)) {
		cap = WF_GIT_AVAILABLE;
	}
	wf_gitproc_result_free(&res);
	return cap;
}

static WfGitCap wsl_capability(
        const char *distro, bool (*cancelled)(void *), void *cctx)
{
	size_t i, slot = (size_t)-1;
	uint64_t now = now_ms();
	WfGitCap cap;

	AcquireSRWLockShared(&g.lock);
	for (i = 0; i < g.n_wsl; i++) {
		if (0 == strcmp(g.wsl[i].name, distro)) {
			Slot *s = &g.wsl[i].slot;

			if (WF_GIT_AVAILABLE == s->cap || now < s->until) {
				cap = s->cap;
				ReleaseSRWLockShared(&g.lock);
				return cap;
			}
			break;
		}
	}
	ReleaseSRWLockShared(&g.lock);
	cap = probe_wsl(distro, cancelled, cctx);
	AcquireSRWLockExclusive(&g.lock);
	for (i = 0; i < g.n_wsl; i++) {
		if (0 == strcmp(g.wsl[i].name, distro)) {
			slot = i;
			break;
		}
	}
	if ((size_t)-1 == slot && g.n_wsl < WF_GIT_DISTROS) {
		slot = g.n_wsl++;
		strcpy_s(g.wsl[slot].name, sizeof g.wsl[slot].name, distro);
	}
	if ((size_t)-1 != slot) {
		g.wsl[slot].slot.known = true;
		g.wsl[slot].slot.cap = cap;
		g.wsl[slot].slot.until =
		        WF_GIT_AVAILABLE == cap ? 0 : now + WF_GIT_RETRY_MS;
	}
	ReleaseSRWLockExclusive(&g.lock);
	return cap;
}

WfGitCap wf_git_capability(
        const WfGitEnv *env, bool (*cancelled)(void *), void *cancel_ctx)
{
	wchar_t exe[MAX_PATH];

	if (!env) {
		return WF_GIT_UNAVAILABLE;
	}
	switch (env->kind) {
	case WF_GENV_WINDOWS:
		return windows_exe(exe, ARRAYSIZE(exe)) ? WF_GIT_AVAILABLE
		                                        : WF_GIT_UNAVAILABLE;
	case WF_GENV_WSL:
		return distro_ok(env->distro)
		        ? wsl_capability(env->distro, cancelled, cancel_ctx)
		        : WF_GIT_UNAVAILABLE;
	case WF_GENV_OTHER: break;
	}
	return WF_GIT_UNAVAILABLE;
}

/* The options of every read-only run. The same for both environments: they go
 * to Git as arguments, so no environment has to be carried into WSL. */
static const wchar_t *const read_only[] = {L"--no-pager",
        L"--no-optional-locks", L"-c", L"core.fsmonitor=false", NULL};

WfStatus wf_git_exec(const WfGitEnv *env,
        const wchar_t *dir,
        const wchar_t *const *args,
        uint32_t timeout_ms,
        size_t max_out,
        bool (*cancelled)(void *),
        void *cancel_ctx,
        WfGitRunResult *res)
{
	static const wchar_t *const windows_env[] = {
	        L"GIT_TERMINAL_PROMPT=0", L"GIT_OPTIONAL_LOCKS=0", NULL};
	wchar_t exe[MAX_PATH], distro_w[WF_WSL_NAME_MAX], linux_w[4200];
	const wchar_t **argv;
	size_t n = 0, fixed = 0, extra = 0;
	WfGitRun run;
	WfStatus st;

	if (!env || !dir || !args || !res) {
		return WF_E_INVAL;
	}
	memset(res, 0, sizeof *res);
	for (size_t i = 0; args[i]; i++) {
		extra++;
	}
	argv = calloc(extra + 16, sizeof *argv);
	if (!argv) {
		return WF_E_NOMEM;
	}
	memset(&run, 0, sizeof run);
	run.timeout_ms = timeout_ms;
	run.max_out = max_out;
	run.cancelled = cancelled;
	run.cancel_ctx = cancel_ctx;
	if (WF_GENV_WINDOWS == env->kind) {
		if (!windows_exe(exe, ARRAYSIZE(exe))) {
			free(argv);
			return WF_E_NOENT; /* no Git here, and no other is tried */
		}
		run.exe = exe;
		run.cwd = dir;
		run.extra_env = windows_env;
	} else if (WF_GENV_WSL == env->kind) {
		char linux_path[4100];

		if (!distro_ok(env->distro) ||
		        !widen_ascii(
		                env->distro, distro_w, ARRAYSIZE(distro_w)) ||
		        WF_OK !=
		                wf_gitenv_wsl_linux_path(
		                        dir, linux_path, sizeof linux_path) ||
		        0 ==
		                MultiByteToWideChar(CP_UTF8,
		                        MB_ERR_INVALID_CHARS, linux_path, -1,
		                        linux_w, ARRAYSIZE(linux_w))) {
			free(argv);
			return WF_E_INVAL;
		}
		if (!wsl_exe(exe, ARRAYSIZE(exe))) {
			free(argv);
			return WF_E_NOENT;
		}
		run.exe = exe;
		argv[n++] = L"-d";
		argv[n++] = distro_w;
		argv[n++] = L"--cd";
		argv[n++] = linux_w;
		argv[n++] = L"--exec";
		argv[n++] = L"git";
	} else {
		free(argv);
		return WF_E_INVAL;
	}
	for (; read_only[fixed]; fixed++) {
		argv[n++] = read_only[fixed];
	}
	for (size_t i = 0; i < extra; i++) {
		argv[n++] = args[i];
	}
	argv[n] = NULL;
	run.args = argv;
	st = run_process(&run, res);
	free(argv);
	return st;
}
