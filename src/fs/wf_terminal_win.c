#include "wf_terminal_win.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../base/wf_macro.h"

BOOL (*wf_terminal_test_spawn)(const wchar_t *application,
        wchar_t *command_line,
        const wchar_t *directory,
        DWORD flags,
        DWORD *error);

#define COMMAND_CHARS (2 * MAX_PATH + 64)

bool wf_terminal_dir_ok(const wchar_t *dir)
{
	size_t n, i, part = 3;

	if (!dir) {
		return false;
	}
	n = wcslen(dir);
	if (n < 3 || n >= MAX_PATH || !(L'\\' == dir[2] && L':' == dir[1]) ||
	        !((dir[0] >= L'A' && dir[0] <= L'Z') ||
	                (dir[0] >= L'a' && dir[0] <= L'z'))) {
		return false;
	}
	for (i = 3; i <= n; i++) {
		wchar_t c = dir[i];

		if (L'\\' == c || L'\0' == c) { /* a component ends here */
			size_t len = i - part;

			if ((1 == len && L'.' == dir[part]) ||
			        (2 == len && L'.' == dir[part] &&
			                L'.' == dir[part + 1])) {
				return false;
			}
			part = i + 1;
			continue;
		}
		if (c < 0x20 || L'"' == c || L'*' == c || L'?' == c ||
		        L'<' == c || L'>' == c || L'|' == c || L':' == c ||
		        L'/' == c) {
			return false;
		}
	}
	return true;
}

bool wf_terminal_quote_arg(const wchar_t *arg, wchar_t *out, size_t cap)
{
	size_t at = 0, slashes = 0;

	if (!arg || !out || cap < 3) {
		return false;
	}
	out[at++] = L'"';
	for (; *arg; arg++) {
		if (L'\\' == *arg) {
			slashes++;
			if (at + 1 >= cap) {
				return false;
			}
			out[at++] = L'\\';
			continue;
		}
		if (L'"' ==
		        *arg) { /* the backslashes before it, and it, escaped */
			if (at + slashes + 2 >= cap) {
				return false;
			}
			for (; slashes; slashes--) {
				out[at++] = L'\\';
			}
			out[at++] = L'\\';
		}
		slashes = 0;
		if (at + 1 >= cap) {
			return false;
		}
		out[at++] = *arg;
	}
	if (at + slashes + 2 >
	        cap) { /* the ones before the closing quote double */
		return false;
	}
	for (; slashes; slashes--) {
		out[at++] = L'\\';
	}
	out[at++] = L'"';
	out[at] = L'\0';
	return true;
}

static BOOL spawn(const wchar_t *application,
        wchar_t *command_line,
        const wchar_t *directory,
        DWORD flags,
        DWORD *error)
{
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;

	if (wf_terminal_test_spawn) {
		return wf_terminal_test_spawn(
		        application, command_line, directory, flags, error);
	}
	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	memset(&pi, 0, sizeof pi);
	/* no handle is inherited: the new terminal never shares this console */
	if (!CreateProcessW(application, command_line, NULL, NULL, FALSE,
	            flags | CREATE_UNICODE_ENVIRONMENT, NULL, directory, &si,
	            &pi)) {
		*error = GetLastError();
		return FALSE;
	}
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return TRUE;
}

/* What became of one candidate: started, left out (not a failure: it is not
 * there, or it cannot take this folder), or failed to start (*error says why). */
typedef enum
{
	START_DONE,
	START_SKIPPED,
	START_FAILED
} Start;

/* `name` in a folder of the PATH, and only there: not the folder of this
 * program, not the current directory, not a relative or a share entry of the
 * PATH (a program put in the folder this one was started from is not run). An
 * entry must be a folder on a local drive: a network drive may not answer. */
static bool find_on_path(const wchar_t *name, wchar_t *out, size_t cap)
{
	DWORD need = GetEnvironmentVariableW(L"PATH", NULL, 0);
	wchar_t *path = need ? malloc(need * sizeof *path) : NULL, *at, *next;
	bool found = false;

	if (!path || !GetEnvironmentVariableW(L"PATH", path, need)) {
		free(path);
		return false;
	}
	for (at = path; at && !found; at = next) {
		wchar_t root[4];
		size_t n;
		UINT drive;
		DWORD attributes;

		next = wcschr(at, L';');
		if (next) {
			*next++ = L'\0';
		}
		n = wcslen(at);
		if (n >= 2 && L'"' == at[0] && L'"' == at[n - 1]) {
			at[n - 1] = L'\0'; /* "C:\a b" */
			at++;
			n -= 2;
		}
		while (n > 3 && L'\\' == at[n - 1]) {
			at[--n] = L'\0';
		}
		if (n < 3 || n + wcslen(name) + 2 >= cap || L':' != at[1] ||
		        L'\\' != at[2] ||
		        !((at[0] >= L'A' && at[0] <= L'Z') ||
		                (at[0] >= L'a' && at[0] <= L'z'))) {
			continue; /* empty, relative, a share, or too long */
		}
		root[0] = at[0];
		root[1] = L':';
		root[2] = L'\\';
		root[3] = L'\0';
		drive = GetDriveTypeW(root);
		if (DRIVE_FIXED != drive && DRIVE_REMOVABLE != drive) {
			continue;
		}
		swprintf(
		        out, cap, L"%ls%ls%ls", at, 3 == n ? L"" : L"\\", name);
		attributes = GetFileAttributesW(out);
		found = INVALID_FILE_ATTRIBUTES != attributes &&
		        !(attributes & FILE_ATTRIBUTE_DIRECTORY);
	}
	free(path);
	return found;
}

/* wt.exe -d "dir": Windows Terminal, in its own window, in `dir`. */
static Start start_wt(const wchar_t *dir, DWORD *error)
{
	wchar_t exe[MAX_PATH], quoted_exe[MAX_PATH + 3],
	        quoted_dir[MAX_PATH + 8], command[COMMAND_CHARS];

	if (wcschr(dir, L';') || !find_on_path(L"wt.exe", exe, MAX_PATH)) {
		return START_SKIPPED; /* wt reads ';' as the end of a command */
	}
	if (!wf_terminal_quote_arg(exe, quoted_exe, WF_ARRAY_LEN(quoted_exe)) ||
	        !wf_terminal_quote_arg(
	                dir, quoted_dir, WF_ARRAY_LEN(quoted_dir)) ||
	        (size_t)swprintf(command, WF_ARRAY_LEN(command), L"%ls -d %ls",
	                quoted_exe, quoted_dir) >= WF_ARRAY_LEN(command)) {
		*error = ERROR_FILENAME_EXCED_RANGE;
		return START_FAILED;
	}
	/* a window program: DETACHED_PROCESS keeps it off this console */
	return spawn(exe, command, dir, DETACHED_PROCESS, error) ? START_DONE
	                                                         : START_FAILED;
}

/* A console program of the system folder, in a console of its own. */
static Start start_console(
        const wchar_t *relative, const wchar_t *dir, DWORD *error)
{
	wchar_t exe[MAX_PATH], command[MAX_PATH + 16];
	UINT n = GetSystemDirectoryW(exe, MAX_PATH);

	if (0 == n || n + wcslen(relative) + 2 >= MAX_PATH) {
		*error = ERROR_FILENAME_EXCED_RANGE;
		return START_FAILED;
	}
	wcscat_s(exe, MAX_PATH, relative);
	/* the folder is the process's directory, and is in no command line */
	swprintf(command, WF_ARRAY_LEN(command), L"\"%ls\"", exe);
	return spawn(exe, command, dir, CREATE_NEW_CONSOLE, error)
	        ? START_DONE
	        : START_FAILED;
}

static WfStatus status_of(DWORD error)
{
	return ERROR_ACCESS_DENIED == error || ERROR_SHARING_VIOLATION == error
	        ? WF_E_ACCESS
	        : ERROR_FILE_NOT_FOUND == error ||
	                ERROR_PATH_NOT_FOUND == error ||
	                ERROR_DIRECTORY == error
	        ? WF_E_NOENT
	        : ERROR_NOT_ENOUGH_MEMORY == error || ERROR_OUTOFMEMORY == error
	        ? WF_E_NOMEM
	        : WF_E_OS;
}

/* The folder exists, is a folder, and can be entered. */
static WfStatus check_folder(const wchar_t *dir, DWORD *error)
{
	DWORD attributes = GetFileAttributesW(dir);
	HANDLE h;

	if (INVALID_FILE_ATTRIBUTES == attributes) {
		*error = GetLastError();
		return status_of(*error);
	}
	if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
		*error = ERROR_DIRECTORY;
		return WF_E_NOENT;
	}
	h = CreateFileW(dir, FILE_LIST_DIRECTORY,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		*error = GetLastError();
		return status_of(*error);
	}
	CloseHandle(h);
	return WF_OK;
}

/* Remembers why a candidate could not be started. */
static void why_failed(DWORD error, DWORD *first, DWORD *memory)
{
	*first = *first ? *first : error;
	if (!*memory &&
	        (ERROR_NOT_ENOUGH_MEMORY == error ||
	                ERROR_OUTOFMEMORY == error)) {
		*memory = error;
	}
}

WfStatus wf_terminal_open_here(const wchar_t *dir, DWORD *error)
{
	DWORD ignored = 0, first = 0, memory = 0, e = 0;
	Start started;
	WfStatus st;

	if (!error) {
		error = &ignored;
	}
	*error = 0;
	if (!wf_terminal_dir_ok(dir)) {
		return WF_E_INVAL;
	}
	st = check_folder(dir, error);
	if (WF_OK != st) {
		return st;
	}
	/* The first that starts is the only one: Windows Terminal, then PowerShell,
	 * then cmd.exe. A candidate left out says nothing about why the others
	 * failed; of the failures, an out-of-memory one is what is told if there
	 * was one, else the first. */
	started = start_wt(dir, &e);
	if (START_FAILED == started) {
		why_failed(e, &first, &memory);
	}
	if (START_DONE != started) {
		e = 0;
		started = start_console(
		        L"\\WindowsPowerShell\\v1.0\\powershell.exe", dir, &e);
		if (START_FAILED == started) {
			why_failed(e, &first, &memory);
		}
	}
	if (START_DONE != started) {
		e = 0;
		started = start_console(L"\\cmd.exe", dir, &e);
		if (START_FAILED == started) {
			why_failed(e, &first, &memory);
		}
	}
	if (START_DONE == started) {
		return WF_OK;
	}
	*error = memory ? memory : first;
	return memory ? WF_E_NOMEM : WF_E_OS;
}
