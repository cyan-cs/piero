#include "wf_gitproc_win.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../fs/wf_terminal_win.h"

#define SLICE_BYTES                                                            \
	(                                                                      \
	        (size_t)256                                                    \
	        << 10) /* read at most this much of a pipe, then look at the clock */
#define POLL_MS 2u    /* the wait between looks at the pipes */
#define CANCEL_MS 20u /* how often `cancelled` is asked */
#define AFTER_EXIT_MS                                                          \
	300u /* a helper that keeps the pipe open is not waited for */
#define COMMAND_MAX 32000u /* CreateProcessW takes 32767 */

typedef struct
{
	HANDLE read; /* ours: not inherited */
	bool open;
} Pipe;

/* The variables that would send Git to some other repository than the folder
 * it is run in. */
static const wchar_t *const drop_env[] = {L"GIT_DIR", L"GIT_WORK_TREE",
        L"GIT_INDEX_FILE", L"GIT_COMMON_DIR", L"GIT_NAMESPACE"};

static bool name_is(const wchar_t *entry, const wchar_t *name)
{
	size_t n = wcslen(name);

	return CSTR_EQUAL ==
	        CompareStringOrdinal(entry, (int)n, name, (int)n, TRUE) &&
	        L'=' == entry[n];
}

/* The environment block: ours, without `drop_env` and without any name that
 * `extra` sets, then `extra`. malloc'd; NULL on failure. */
static wchar_t *build_env(const wchar_t *const *extra)
{
	wchar_t *base = GetEnvironmentStringsW(), *out, *w;
	size_t total = 1;

	if (!base) {
		return NULL;
	}
	for (const wchar_t *p = base; *p; p += wcslen(p) + 1) {
		total += wcslen(p) + 1;
	}
	for (size_t i = 0; extra && extra[i]; i++) {
		total += wcslen(extra[i]) + 1;
	}
	out = malloc(total * sizeof *out);
	if (!out) {
		FreeEnvironmentStringsW(base);
		return NULL;
	}
	w = out;
	for (const wchar_t *p = base; *p; p += wcslen(p) + 1) {
		bool skip = false;

		for (size_t i = 0; i < ARRAYSIZE(drop_env) && !skip; i++) {
			skip = name_is(p, drop_env[i]);
		}
		for (size_t i = 0; extra && extra[i] && !skip; i++) {
			const wchar_t *eq = wcschr(extra[i], L'=');
			size_t n = eq ? (size_t)(eq - extra[i]) : 0;

			skip = n &&
			        CSTR_EQUAL ==
			                CompareStringOrdinal(p, (int)n,
			                        extra[i], (int)n, TRUE) &&
			        L'=' == p[n];
		}
		if (!skip) {
			size_t n = wcslen(p) + 1;

			memcpy(w, p, n * sizeof *w);
			w += n;
		}
	}
	for (size_t i = 0; extra && extra[i]; i++) {
		size_t n = wcslen(extra[i]) + 1;

		memcpy(w, extra[i], n * sizeof *w);
		w += n;
	}
	*w = L'\0';
	FreeEnvironmentStringsW(base);
	return out;
}

/* True when CommandLineToArgvW would not read `arg` back as one argument
 * without quotes. */
static bool needs_quotes(const wchar_t *arg)
{
	return !*arg || wcspbrk(arg, L" \t\n\v\"");
}

/* "exe" arg arg; malloc'd. An argument is in quotes only when it has to be:
 * wsl.exe reads its own options from the raw line and does not know "-d" or
 * "--exec" in quotes. NULL: no memory, or longer than CreateProcessW takes. */
static wchar_t *build_command(const wchar_t *exe, const wchar_t *const *args)
{
	size_t cap = 0, at = 0;
	wchar_t *line;
	bool ok;

	cap += 2 * wcslen(exe) + 4;
	for (size_t i = 0; args && args[i]; i++) {
		cap += 2 * wcslen(args[i]) + 4;
	}
	if (cap > COMMAND_MAX * 2) {
		return NULL;
	}
	line = malloc(cap * sizeof *line);
	if (!line) {
		return NULL;
	}
	if (!wf_terminal_quote_arg(exe, line, cap)) {
		free(line);
		return NULL;
	}
	at = wcslen(line);
	for (size_t i = 0; args && args[i]; i++) {
		line[at++] = L' ';
		if (needs_quotes(args[i])) {
			ok = wf_terminal_quote_arg(
			        args[i], line + at, cap - at);
		} else {
			ok = 0 == wcscpy_s(line + at, cap - at, args[i]);
		}
		if (!ok) {
			free(line);
			return NULL;
		}
		at += wcslen(line + at);
	}
	if (at >= COMMAND_MAX) {
		free(line);
		return NULL;
	}
	return line;
}

static bool make_pipe(Pipe *p, HANDLE *child_end)
{
	SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
	HANDLE r = NULL, w = NULL;

	if (!CreatePipe(&r, &w, &sa, 1u << 16)) {
		return false;
	}
	/* the read end is ours alone */
	if (!SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0)) {
		CloseHandle(r);
		CloseHandle(w);
		return false;
	}
	p->read = r;
	p->open = true;
	*child_end = w;
	return true;
}

/* Takes what the pipe holds now into `buf` (grown), at most `keep_max` bytes of
 * it. Returns the bytes taken from the pipe. A broken pipe closes it.
 *
 * What stops it is said in `*fail`, and the causes are kept apart:
 *  - `limit_fails` (the output the caller wants whole): more than `keep_max` is
 *    WF_E_RANGE, and no more is read;
 *  - memory that cannot be had for what was read is WF_E_NOMEM;
 * and both end the read. Without `limit_fails` (the error output, only its start
 * is kept) what is over `keep_max`, or cannot be kept for want of memory, is
 * read and dropped, so that a child that writes a great deal is never left
 * waiting on a full pipe; `*dropped` says that some was dropped. */
static size_t drain(Pipe *p,
        uint8_t **buf,
        size_t *n,
        size_t *cap,
        size_t keep_max,
        bool limit_fails,
        WfStatus *fail,
        bool *dropped)
{
	size_t taken = 0;
	DWORD avail;

	/* A child that never stops writing must not keep this loop: it takes a slice
	 * and goes back to the caller, who looks at the time limit, the cancel and the
	 * other pipe. */
	while (taken < SLICE_BYTES && p->open &&
	        PeekNamedPipe(p->read, NULL, 0, NULL, &avail, NULL) &&
	        avail > 0) {
		uint8_t tmp[16384];
		DWORD got = 0;
		DWORD want = avail < sizeof tmp ? avail : (DWORD)sizeof tmp;

		if (!ReadFile(p->read, tmp, want, &got, NULL) || 0 == got) {
			break;
		}
		taken += got;
		if (*n + got > keep_max) {
			got = *n < keep_max ? (DWORD)(keep_max - *n) : 0;
			if (limit_fails) { /* over the limit: no more is read, the child is ended */
				*fail = WF_E_RANGE;
				break;
			}
			*dropped = true;
		}
		if (got) {
			if (*n + got + 1 > *cap) {
				size_t want_cap = *cap ? *cap : 4096;
				uint8_t *grown;

				while (want_cap < *n + got + 1) {
					want_cap *= 2;
				}
				grown = realloc(*buf, want_cap);
				if (!grown) {
					if (limit_fails) { /* not "too big": no memory */
						*fail = WF_E_NOMEM;
						return taken;
					}
					/* a diagnostic that cannot be kept is dropped, and the
					 * pipe still read */
					*dropped = true;
					got = 0;
					keep_max = *n;
					continue;
				}
				*buf = grown;
				*cap = want_cap;
			}
			memcpy(*buf + *n, tmp, got);
			*n += got;
		}
	}
	if (p->open) {
		/* broken pipe: nobody writes any more */
		if (!PeekNamedPipe(p->read, NULL, 0, NULL, &avail, NULL)) {
			CloseHandle(p->read);
			p->open = false;
		}
	}
	return taken;
}

void wf_gitproc_result_free(WfGitRunResult *res)
{
	if (res) {
		free(res->out);
		memset(res, 0, sizeof *res);
	}
}

WfStatus wf_gitproc_run(const WfGitRun *run, WfGitRunResult *res)
{
	Pipe out = {0}, err = {0};
	HANDLE in_r = INVALID_HANDLE_VALUE, out_w = INVALID_HANDLE_VALUE,
	       err_w = INVALID_HANDLE_VALUE, job = NULL;
	HANDLE inherit[3];
	PROCESS_INFORMATION pi;
	STARTUPINFOEXW si;
	SIZE_T attr_size = 0;
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit;
	wchar_t *line = NULL, *env = NULL;
	uint8_t *ebuf = NULL;
	size_t ecap = 0, en = 0, ocap = 0;
	bool started = false, killed = false, err_dropped = false;
	WfStatus out_fail = WF_OK;
	WfStatus st = WF_E_OS;
	ULONGLONG t0, exited_at = 0, last_cancel = 0;
	SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};

	if (!run || !run->exe || !res) {
		return WF_E_INVAL;
	}
	memset(res, 0, sizeof *res);
	memset(&pi, 0, sizeof pi);
	memset(&si, 0, sizeof si);
	line = build_command(run->exe, run->args);
	env = build_env(run->extra_env);
	if (!line || !env) {
		st = WF_E_NOMEM;
		goto done;
	}
	in_r = CreateFileW(L"NUL", GENERIC_READ,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	job = CreateJobObjectW(NULL, NULL);
	if (INVALID_HANDLE_VALUE == in_r || !job || !make_pipe(&out, &out_w) ||
	        !make_pipe(&err, &err_w)) {
		res->os_error = GetLastError();
		goto done;
	}
	memset(&limit, 0, sizeof limit);
	limit.BasicLimitInformation.LimitFlags =
	        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
	            &limit, sizeof limit)) {
		res->os_error = GetLastError();
		goto done;
	}
	inherit[0] = in_r;
	inherit[1] = out_w;
	inherit[2] = err_w;
	InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
	si.lpAttributeList = HeapAlloc(GetProcessHeap(), 0, attr_size);
	if (!si.lpAttributeList) {
		res->os_error = ERROR_NOT_ENOUGH_MEMORY;
		goto done;
	}
	if (!InitializeProcThreadAttributeList(
	            si.lpAttributeList, 1, 0, &attr_size)) {
		res->os_error = GetLastError();
		HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
		si.lpAttributeList = NULL;
		goto done;
	}
	if (!UpdateProcThreadAttribute(si.lpAttributeList, 0,
	            PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit,
	            NULL, NULL)) {
		res->os_error = GetLastError();
		goto done;
	}
	si.StartupInfo.cb = sizeof si;
	si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	si.StartupInfo.hStdInput = in_r;
	si.StartupInfo.hStdOutput = out_w;
	si.StartupInfo.hStdError = err_w;
	if (!CreateProcessW(run->exe, line, NULL, NULL, TRUE,
	            EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW |
	                    CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
	            env, run->cwd, &si.StartupInfo, &pi)) {
		res->os_error = GetLastError();
		st = ERROR_FILE_NOT_FOUND == res->os_error ||
		                ERROR_PATH_NOT_FOUND == res->os_error
		        ? WF_E_NOENT
		        : WF_E_OS;
		goto done;
	}
	if (!AssignProcessToJobObject(job, pi.hProcess)) {
		res->os_error = GetLastError();
		TerminateProcess(pi.hProcess, 1);
		goto done;
	}
	started = true;
	ResumeThread(pi.hThread);
	/* the child's ends belong to the child now */
	CloseHandle(in_r);
	in_r = INVALID_HANDLE_VALUE;
	CloseHandle(out_w);
	out_w = INVALID_HANDLE_VALUE;
	CloseHandle(err_w);
	err_w = INVALID_HANDLE_VALUE;

	t0 = GetTickCount64();
	st = WF_OK;
	for (;;) {
		size_t taken;
		ULONGLONG now = GetTickCount64();
		bool exited =
		        WAIT_OBJECT_0 == WaitForSingleObject(pi.hProcess, 0);

		taken = drain(&out, &res->out, &res->out_n, &ocap,
		        run->max_out ? run->max_out : (size_t)-1 - 1, true,
		        &out_fail, NULL);
		taken += drain(&err, &ebuf, &en, &ecap, sizeof res->err - 1,
		        false, NULL, &err_dropped);
		if (WF_OK != out_fail) {
			st = out_fail; /* too much, or no memory: said as what it was */
			killed = true;
		} else if (exited && !exited_at) {
			exited_at = now;
		}
		if (exited &&
		        ((!out.open && !err.open) ||
		                (0 == taken &&
		                        now - exited_at >= AFTER_EXIT_MS))) {
			break;
		}
		if (!killed && now - t0 >= run->timeout_ms && run->timeout_ms) {
			st = WF_E_TIMEOUT;
			killed = true;
		}
		if (!killed && run->cancelled &&
		        now - last_cancel >= CANCEL_MS) {
			last_cancel = now;
			if (run->cancelled(run->cancel_ctx)) {
				st = WF_E_CANCELLED;
				killed = true;
			}
		}
		if (killed) {
			TerminateJobObject(job, 1);
			WaitForSingleObject(pi.hProcess, 2000);
			break;
		}
		if (!taken && !exited) {
			WaitForSingleObject(pi.hProcess, POLL_MS);
		}
	}
	if (WF_OK == st) {
		DWORD code = 0;

		GetExitCodeProcess(pi.hProcess, &code);
		res->exit_code = (int)code;
	}
	res->err_dropped = err_dropped;
	if (en) {
		size_t n = en < sizeof res->err - 1 ? en : sizeof res->err - 1;

		memcpy(res->err, ebuf, n);
		for (size_t i = 0; i < n; i++) {
			if ((unsigned char)res->err[i] < 0x20) {
				res->err[i] = ' ';
			}
		}
		res->err[n] = '\0';
	}

done:
	if (si.lpAttributeList) {
		DeleteProcThreadAttributeList(si.lpAttributeList);
		HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
	}
	if (started && st != WF_OK && !killed) {
		TerminateJobObject(job, 1);
	}
	if (pi.hThread) {
		CloseHandle(pi.hThread);
	}
	if (pi.hProcess) {
		CloseHandle(pi.hProcess);
	}
	if (job) {
		CloseHandle(job); /* KILL_ON_JOB_CLOSE ends anything left */
	}
	if (INVALID_HANDLE_VALUE != in_r && in_r) {
		CloseHandle(in_r);
	}
	if (INVALID_HANDLE_VALUE != out_w && out_w) {
		CloseHandle(out_w);
	}
	if (INVALID_HANDLE_VALUE != err_w && err_w) {
		CloseHandle(err_w);
	}
	if (out.open) {
		CloseHandle(out.read);
	}
	if (err.open) {
		CloseHandle(err.read);
	}
	free(ebuf);
	free(line);
	free(env);
	if (res->out) {
		res->out[res->out_n] = '\0';
	}
	if (WF_OK != st) {
		free(res->out);
		res->out = NULL;
		res->out_n = 0;
	}
	return st;
}
