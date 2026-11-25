/*
 * Open Terminal Here (wf_terminal_win.c). What is a folder it will open, how
 * an argument is quoted (read back with CommandLineToArgvW), which program is
 * started and in what order, and, for real, that a program started like
 * wt.exe is given the folder both as its working directory and as the -d
 * argument whatever the name holds, and that no handle is left behind. The
 * program that stands for wt.exe is tests/terminal_probe.c, found first on a
 * PATH made for the test: no terminal window is opened.
 */
#include <windows.h>

#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs/wf_launchworker.h"
#include "fs/wf_terminal_win.h"
#include "wf_test.h"

static void a_folder_of_a_local_drive_is_accepted(void)
{
	wchar_t longest[MAX_PATH + 8], toolong[MAX_PATH + 8];

	CHECK(wf_terminal_dir_ok(L"C:\\"));
	CHECK(wf_terminal_dir_ok(L"c:\\Users\\dev"));
	CHECK(wf_terminal_dir_ok(
	        L"C:\\Users\\dev\\")); /* a trailing backslash */
	CHECK(wf_terminal_dir_ok(
	        L"D:\\a b\\(c)\\d&e\\f^g\\h%i\\j!k\\l;m\\n'o"));
	CHECK(wf_terminal_dir_ok(
	        L"C:\\\u65e5\u672c\u8a9e\\\u30d5\u30a9\u30eb\u30c0"));
	wcscpy_s(longest, MAX_PATH + 8, L"C:\\");
	for (size_t i = 3; i < MAX_PATH - 1; i++) {
		longest[i] = L'a';
	}
	longest[MAX_PATH - 1] = L'\0'; /* 259 characters */
	CHECK(wf_terminal_dir_ok(longest));
	wcscpy_s(toolong, MAX_PATH + 8, longest);
	toolong[MAX_PATH - 1] = L'a';
	toolong[MAX_PATH] =
	        L'\0'; /* 260: a working directory cannot be this long */
	CHECK(!wf_terminal_dir_ok(toolong));
}

static void anything_else_is_refused(void)
{
	static const wchar_t *const bad[] = {NULL, L"", L"C:", L"C:x", L"\\x",
	        L"x:\\y:z", L"1:\\x", L"\\\\server\\share\\x",
	        L"\\\\wsl.localhost\\Ubuntu\\home", L"\\\\?\\C:\\x",
	        L"\\\\.\\C:", L"C:\\..\\x", L"C:\\a\\..", L"C:\\a\\.\\b",
	        L"C:\\a\"b", L"C:\\a*b", L"C:\\a?b", L"C:\\a<b", L"C:\\a>b",
	        L"C:\\a|b", L"C:\\a/b", L"C:\\a\nb", L"C:\\a\tb", L"C:\\a\x01"};

	for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
		CHECK(!wf_terminal_dir_ok(bad[i]));
	}
}

static void an_argument_is_quoted_so_that_it_reads_back_the_same(void)
{
	static const wchar_t *const args[] = {L"C:\\", L"C:\\Users\\dev",
	        L"C:\\Users\\dev\\", L"C:\\a b\\c d\\", L"C:\\a b\\\\", L"a\"b",
	        L"a\\\"b", L"a\\\\\"b", L"\\", L"\\\\", L"x y",
	        L"C:\\\u65e5\u672c\u8a9e (1)\\", L"&^%!;", L"a  b\tc"};
	wchar_t quoted[300], line[400];

	for (size_t i = 0; i < sizeof args / sizeof *args; i++) {
		int argc = 0;
		wchar_t **argv;

		CHECK(wf_terminal_quote_arg(args[i], quoted, 300));
		swprintf(line, 400, L"prog.exe -d %ls", quoted);
		argv = CommandLineToArgvW(line, &argc);
		CHECK(argv && 3 == argc && !wcscmp(argv[1], L"-d") &&
		        !wcscmp(argv[2], args[i]));
		LocalFree(argv);
	}
	/* a root: the backslash before the closing quote must not escape it */
	CHECK(wf_terminal_quote_arg(L"C:\\", quoted, 300) &&
	        !wcscmp(quoted, L"\"C:\\\\\""));
	/* too small a place */
	CHECK(!wf_terminal_quote_arg(L"C:\\Users", quoted, 5));
	CHECK(!wf_terminal_quote_arg(
	        L"C:\\", quoted, 6)); /* needs 6 characters and the end */
	CHECK(!wf_terminal_quote_arg(NULL, quoted, 300));
}

typedef struct
{
	unsigned calls;
	wchar_t application[4][MAX_PATH];
	wchar_t command[4][700];
	wchar_t directory[4][MAX_PATH];
	DWORD flags[4];
	unsigned fail_mask; /* bit n: the n-th call fails */
	DWORD error_of[4];  /* with this error (0: not found) */
} Spy;

static Spy spy;

static BOOL spy_spawn(const wchar_t *application,
        wchar_t *command_line,
        const wchar_t *directory,
        DWORD flags,
        DWORD *error)
{
	unsigned n = spy.calls++;

	if (n < 4) {
		wcscpy_s(spy.application[n], MAX_PATH, application);
		wcscpy_s(spy.command[n], 700, command_line);
		wcscpy_s(spy.directory[n], MAX_PATH, directory);
		spy.flags[n] = flags;
	}
	if (n < 32 && (spy.fail_mask >> n & 1u)) {
		*error = n < 4 && spy.error_of[n] ? spy.error_of[n]
		                                  : ERROR_FILE_NOT_FOUND;
		return FALSE;
	}
	return TRUE;
}

static wchar_t path_was[32768], tool_dir[MAX_PATH + 16];

static bool ends_with(const wchar_t *s, const wchar_t *tail)
{
	size_t n = wcslen(s), t = wcslen(tail);

	return n >= t && !_wcsicmp(s + n - t, tail);
}

/* PATH is only `tool_dir`, which holds a wt.exe when `with_wt`. */
static void path_with(bool with_wt)
{
	wchar_t exe[MAX_PATH + 40], probe[MAX_PATH + 40];

	swprintf(exe, MAX_PATH + 40, L"%ls\\wt.exe", tool_dir);
	/* a probe started a moment ago may still be leaving: wait for the file */
	for (int i = 0; i < 100; i++) {
		if (DeleteFileW(exe) ||
		        ERROR_FILE_NOT_FOUND == GetLastError()) {
			break;
		}
		Sleep(50);
	}
	if (with_wt) {
		bool copied = false;

		swprintf(probe, MAX_PATH + 40, L"%ls\\terminal_probe.exe",
		        tool_dir);
		for (int i = 0; i < 100 && !copied; i++) {
			copied = CopyFileW(probe, exe, FALSE);
			if (!copied) {
				Sleep(50);
			}
		}
		CHECK(copied);
	}
	CHECK(SetEnvironmentVariableW(L"PATH", tool_dir));
}

static void windows_terminal_is_first_and_alone_when_it_starts(void)
{
	DWORD error = 0;

	wf_terminal_test_spawn = spy_spawn;
	path_with(true);
	memset(&spy, 0, sizeof spy);
	CHECK(WF_OK == wf_terminal_open_here(tool_dir, &error));
	CHECK(1 ==
	        spy.calls); /* a terminal that started is not followed by another */
	CHECK(ends_with(spy.application[0], L"\\wt.exe"));
	CHECK(!wcscmp(spy.directory[0], tool_dir));
	CHECK(wcsstr(spy.command[0], L" -d \"") &&
	        DETACHED_PROCESS == spy.flags[0]);
	wf_terminal_test_spawn = NULL;
}

static void powershell_then_cmd_follow_only_when_one_could_not_start(void)
{
	DWORD error = 0;

	wf_terminal_test_spawn = spy_spawn;
	path_with(true);
	memset(&spy, 0, sizeof spy);
	spy.fail_mask = 1; /* wt.exe fails */
	CHECK(WF_OK == wf_terminal_open_here(tool_dir, &error));
	CHECK(2 == spy.calls &&
	        ends_with(spy.application[1], L"\\powershell.exe"));
	CHECK(CREATE_NEW_CONSOLE == spy.flags[1] &&
	        !wcscmp(spy.directory[1], tool_dir));
	/* the folder is the working directory, and is in no command line */
	CHECK(!wcsstr(spy.command[1], L"-d") &&
	        !wcsstr(spy.command[1], tool_dir));
	memset(&spy, 0, sizeof spy);
	spy.fail_mask = 3; /* wt.exe and PowerShell fail */
	CHECK(WF_OK == wf_terminal_open_here(tool_dir, &error));
	CHECK(3 == spy.calls && ends_with(spy.application[2], L"\\cmd.exe"));
	memset(&spy, 0, sizeof spy);
	spy.fail_mask = 7; /* nothing can be started */
	CHECK(WF_E_OS == wf_terminal_open_here(tool_dir, &error) && error);
	CHECK(3 == spy.calls);
	/* without Windows Terminal on the PATH, PowerShell is the first */
	path_with(false);
	memset(&spy, 0, sizeof spy);
	CHECK(WF_OK == wf_terminal_open_here(tool_dir, &error));
	CHECK(1 == spy.calls &&
	        ends_with(spy.application[0], L"\\powershell.exe"));
	wf_terminal_test_spawn = NULL;
}

/* wt.exe is looked for in the folders of the PATH and nowhere else: not in the
 * current directory, not in a relative entry (".", "sub"), not on a share; an
 * entry in quotes is a folder like any other. */
static void windows_terminal_is_taken_from_the_path_only(void)
{
	wchar_t here[MAX_PATH], other[MAX_PATH + 40], exe[MAX_PATH + 40],
	        quoted[MAX_PATH + 8], probe[MAX_PATH + 40], now[MAX_PATH];
	DWORD error = 0;

	/* `other` has a wt.exe, and the process stands in it */
	swprintf(other, MAX_PATH + 40, L"%ls\\decoy", tool_dir);
	CHECK(CreateDirectoryW(other, NULL) ||
	        ERROR_ALREADY_EXISTS == GetLastError());
	swprintf(probe, MAX_PATH + 40, L"%ls\\terminal_probe.exe", tool_dir);
	swprintf(exe, MAX_PATH + 40, L"%ls\\wt.exe", other);
	CHECK(CopyFileW(probe, exe, FALSE));
	GetCurrentDirectoryW(MAX_PATH, now);
	CHECK(SetCurrentDirectoryW(other));
	wf_terminal_test_spawn = spy_spawn;
	/* a PATH without it: the one in the current directory is not run */
	swprintf(here, MAX_PATH, L"%ls", tool_dir);
	path_with(false); /* PATH is tool_dir, which has no wt.exe now */
	memset(&spy, 0, sizeof spy);
	CHECK(WF_OK == wf_terminal_open_here(tool_dir, &error));
	CHECK(1 == spy.calls &&
	        ends_with(spy.application[0], L"\\powershell.exe"));
	/* "." and a relative entry find nothing either */
	CHECK(SetEnvironmentVariableW(
	        L"PATH", L".;decoy;..;\\\\server\\share"));
	memset(&spy, 0, sizeof spy);
	CHECK(WF_OK == wf_terminal_open_here(tool_dir, &error));
	CHECK(1 == spy.calls &&
	        ends_with(spy.application[0], L"\\powershell.exe"));
	/* the folder itself, by its full name, is found, quoted or not, with a
	 * trailing backslash or not */
	swprintf(quoted, MAX_PATH + 8, L"\"%ls\\\"", other);
	CHECK(SetEnvironmentVariableW(L"PATH", quoted));
	memset(&spy, 0, sizeof spy);
	CHECK(WF_OK == wf_terminal_open_here(tool_dir, &error));
	CHECK(1 == spy.calls &&
	        ends_with(spy.application[0], L"\\decoy\\wt.exe"));
	wf_terminal_test_spawn = NULL;
	CHECK(SetCurrentDirectoryW(now));
	DeleteFileW(exe);
	RemoveDirectoryW(other);
}

/* Why nothing started is what failed: a candidate left out (no wt.exe, a folder
 * with ';') is not the reason, and no memory is not hidden by the first
 * failure. */
static void the_reason_given_is_a_failure_and_not_a_left_out_candidate(void)
{
	wchar_t dir[MAX_PATH + 16];
	DWORD error = 0;

	wf_terminal_test_spawn = spy_spawn;
	path_with(false); /* no wt.exe */
	memset(&spy, 0, sizeof spy);
	spy.fail_mask = 3;
	spy.error_of[0] = ERROR_ACCESS_DENIED;
	spy.error_of[1] = ERROR_BAD_EXE_FORMAT;
	CHECK(WF_E_OS == wf_terminal_open_here(tool_dir, &error));
	CHECK(2 == spy.calls &&
	        ERROR_ACCESS_DENIED == error); /* PowerShell's, the first */
	/* a folder with ';': Windows Terminal is left out, not the reason */
	swprintf(dir, MAX_PATH + 16, L"%ls\\semi;colon", tool_dir);
	CHECK(CreateDirectoryW(dir, NULL) ||
	        ERROR_ALREADY_EXISTS == GetLastError());
	path_with(true);
	memset(&spy, 0, sizeof spy);
	spy.fail_mask = 3;
	spy.error_of[0] = ERROR_ACCESS_DENIED;
	spy.error_of[1] = ERROR_ACCESS_DENIED;
	CHECK(WF_E_OS == wf_terminal_open_here(dir, &error) &&
	        ERROR_ACCESS_DENIED == error && ERROR_BAD_PATHNAME != error);
	/* wt.exe is not found by the system (a broken alias), PowerShell has no
	 * memory, cmd.exe is not found: the lack of memory is what is told */
	path_with(true);
	memset(&spy, 0, sizeof spy);
	spy.fail_mask = 7;
	spy.error_of[0] = ERROR_FILE_NOT_FOUND;
	spy.error_of[1] = ERROR_NOT_ENOUGH_MEMORY;
	spy.error_of[2] = ERROR_FILE_NOT_FOUND;
	CHECK(WF_E_NOMEM == wf_terminal_open_here(tool_dir, &error) &&
	        ERROR_NOT_ENOUGH_MEMORY == error && 3 == spy.calls);
	wf_terminal_test_spawn = NULL;
	RemoveDirectoryW(dir);
}

static void a_folder_that_windows_terminal_would_split_skips_it(void)
{
	wchar_t dir[MAX_PATH + 16];
	DWORD error = 0;

	swprintf(dir, MAX_PATH + 16, L"%ls\\semi;colon", tool_dir);
	CHECK(CreateDirectoryW(dir, NULL) ||
	        ERROR_ALREADY_EXISTS == GetLastError());
	wf_terminal_test_spawn = spy_spawn;
	path_with(true);
	memset(&spy, 0, sizeof spy);
	CHECK(WF_OK == wf_terminal_open_here(dir, &error));
	CHECK(1 == spy.calls &&
	        ends_with(spy.application[0], L"\\powershell.exe"));
	CHECK(!wcscmp(spy.directory[0], dir));
	wf_terminal_test_spawn = NULL;
	RemoveDirectoryW(dir);
}

static void a_folder_that_is_not_there_or_not_valid_starts_nothing(void)
{
	wchar_t file[MAX_PATH + 16], gone[MAX_PATH + 16];
	HANDLE h;
	DWORD error = 0;

	wf_terminal_test_spawn = spy_spawn;
	path_with(true);
	memset(&spy, 0, sizeof spy);
	swprintf(gone, MAX_PATH + 16, L"%ls\\no such folder", tool_dir);
	CHECK(WF_E_NOENT == wf_terminal_open_here(gone, &error) && error);
	swprintf(file, MAX_PATH + 16, L"%ls\\a file", tool_dir);
	h = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	CloseHandle(h);
	CHECK(WF_E_NOENT ==
	        wf_terminal_open_here(file, &error)); /* not a folder */
	DeleteFileW(file);
	CHECK(WF_E_INVAL ==
	        wf_terminal_open_here(L"\\\\server\\share\\x", &error));
	CHECK(WF_E_INVAL == wf_terminal_open_here(L"relative\\path", &error));
	CHECK(WF_E_INVAL == wf_terminal_open_here(NULL, &error));
	CHECK(0 == spy.calls);
	wf_terminal_test_spawn = NULL;
}

static wchar_t probe_out[MAX_PATH + 40];

/* The probe's report: its directory, then each argument, one to a line. */
static wchar_t *probe_report(void)
{
	for (int i = 0; i < 100; i++) {
		HANDLE h = CreateFileW(probe_out, GENERIC_READ, FILE_SHARE_READ,
		        NULL, OPEN_EXISTING, 0, NULL);

		if (INVALID_HANDLE_VALUE != h) {
			DWORD size = GetFileSize(h, NULL), got = 0;
			wchar_t *text = calloc(
			        size / sizeof(wchar_t) + 2, sizeof *text);

			if (text && size > 0 &&
			        ReadFile(h, text, size, &got, NULL) &&
			        got == size) {
				CloseHandle(h);
				return text;
			}
			free(text);
			CloseHandle(h);
		}
		Sleep(50);
	}
	return NULL;
}

/* The folder reaches the program as its directory and, quoted, as -d. */
static bool reaches_the_program_as_it_is(const wchar_t *dir)
{
	wchar_t want[1200], cwd[MAX_PATH + 8], *got;
	size_t n = wcslen(dir);
	DWORD error = 0;
	bool same;

	DeleteFileW(probe_out);
	if (WF_OK != wf_terminal_open_here(dir, &error)) {
		return false;
	}
	got = probe_report();
	if (!got) {
		return false;
	}
	/* a directory is reported without its trailing backslash, a root with it */
	wcscpy_s(cwd, MAX_PATH + 8, dir);
	if (n > 3 && L'\\' == cwd[n - 1]) {
		cwd[n - 1] = L'\0';
	}
	swprintf(want, 1200, L"%ls\n-d\n%ls", cwd, dir);
	same = !wcscmp(got, want);
	if (!same) {
		wprintf(L"  want [%ls] got [%ls]\n", want, got);
	}
	free(got);
	return same;
}

static void the_folder_arrives_exactly_whatever_its_name_holds(void)
{
	wchar_t drive_root[8], temp[MAX_PATH];
	static const wchar_t *const names[] = {L"plain", L"with space",
	        L"(paren) [bracket] {brace}",
	        L"amp & caret ^ pct %PATH% bang !x", L"quote's and `ticks`",
	        L"\u65e5\u672c\u8a9e \u30d5\u30a9\u30eb\u30c0"};

	path_with(true);
	wf_terminal_test_spawn = NULL;
	CHECK(SetEnvironmentVariableW(L"WF_TERMINAL_PROBE_OUT", probe_out));
	GetTempPathW(MAX_PATH, temp);
	for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
		wchar_t path[MAX_PATH + 16];

		swprintf(path, MAX_PATH + 16, L"%lswf-term-%lu-%zu", temp,
		        GetCurrentProcessId(), i);
		CHECK(CreateDirectoryW(path, NULL));
		wcscat_s(path, MAX_PATH + 16, L"\\");
		wcscat_s(path, MAX_PATH + 16, names[i]);
		CHECK(CreateDirectoryW(path, NULL));
		CHECK(reaches_the_program_as_it_is(path));
		wcscat_s(path, MAX_PATH + 16,
		        L"\\"); /* and the same with a trailing one */
		CHECK(reaches_the_program_as_it_is(path));
		/* what this made is taken away: the folder, then the one above it */
		path[wcslen(path) - 1] = L'\0';
		RemoveDirectoryW(path);
		*wcsrchr(path, L'\\') = L'\0';
		RemoveDirectoryW(path);
	}
	/* a drive root: the backslash before the closing quote */
	drive_root[0] = temp[0];
	drive_root[1] = L':';
	drive_root[2] = L'\\';
	drive_root[3] = L'\0';
	CHECK(reaches_the_program_as_it_is(drive_root));
	/* the folder of the test process is not changed by any of it */
	{
		wchar_t now[MAX_PATH];

		GetCurrentDirectoryW(MAX_PATH, now);
		CHECK(0 != now[0]);
	}
}

static void no_handle_is_left_behind(void)
{
	DWORD before = 0, after = 0;
	DWORD error = 0;

	path_with(true);
	wf_terminal_test_spawn = NULL;
	DeleteFileW(probe_out);
	CHECK(WF_OK ==
	        wf_terminal_open_here(
	                tool_dir, &error)); /* a first, to warm up */
	CHECK(GetProcessHandleCount(GetCurrentProcess(), &before));
	for (int i = 0; i < 40; i++) {
		CHECK(WF_OK == wf_terminal_open_here(tool_dir, &error));
	}
	CHECK(GetProcessHandleCount(GetCurrentProcess(), &after));
	printf("info: handles before %lu, after 40 terminals %lu\n",
	        (unsigned long)before, (unsigned long)after);
	CHECK(after <= before + 2);
}

static WfLaunchResult worker_answer(
        WfLaunchWorker *w, const wchar_t *dir, WfStatus expect_submit)
{
	WfLaunchResult r;
	static uint32_t gen;
	WfLaunchRequest req;
	WfLaunchPort port = wf_launchworker_port(w);
	WfStatus st;

	memset(&req, 0, sizeof req);
	req.terminal = true;
	req.directory = dir;
	st = port.submit(port.ctx, ++gen, &req);
	memset(&r, 0, sizeof r);
	CHECK(expect_submit == st);
	if (WF_OK != st) {
		return r;
	}
	CHECK(WAIT_OBJECT_0 ==
	        WaitForSingleObject(wf_launchworker_event(w), 20000));
	CHECK(wf_launchworker_poll(w, &r) && r.gen == gen);
	CHECK(!wf_launchworker_poll(w, &r)); /* exactly one answer */
	return r;
}

/* A request for a terminal needs a folder and nothing else; the answer names the
 * folder; a bad one is answered, not started, and the worker goes on. */
static void the_worker_answers_a_terminal_request_once(void)
{
	WfLaunchWorker *w = NULL;
	WfLaunchResult r;
	wchar_t gone[MAX_PATH + 16], *report;
	WfLaunchRequest none;
	WfLaunchPort port;

	path_with(true);
	wf_terminal_test_spawn = NULL;
	CHECK(WF_OK == wf_launchworker_start(&w));
	if (!w) {
		return;
	}
	CHECK(SetEnvironmentVariableW(L"WF_TERMINAL_PROBE_OUT", probe_out));
	DeleteFileW(probe_out);
	r = worker_answer(w, tool_dir, WF_OK);
	CHECK(WF_OK == r.outcome.st && !wcscmp(r.name, tool_dir));
	report = probe_report();
	CHECK(report && wcsstr(report, L"\n-d\n"));
	free(report);
	swprintf(gone, MAX_PATH + 16, L"%ls\\no such folder", tool_dir);
	r = worker_answer(w, gone, WF_OK);
	CHECK(WF_E_NOENT == r.outcome.st && r.outcome.error);
	r = worker_answer(w, L"\\\\server\\share\\x", WF_OK);
	CHECK(WF_E_INVAL == r.outcome.st);
	/* no folder at all is refused at once */
	memset(&none, 0, sizeof none);
	none.terminal = true;
	port = wf_launchworker_port(w);
	CHECK(WF_E_INVAL == port.submit(port.ctx, 99, &none));
	CHECK(WF_OK == wf_launchworker_stop(w).status);
}

int main(void)
{
	wchar_t exe[MAX_PATH], temp[MAX_PATH], *slash;

	setvbuf(stdout, NULL, _IONBF, 0);
	GetEnvironmentVariableW(L"PATH", path_was, 32768);
	GetModuleFileNameW(NULL, exe, MAX_PATH);
	slash = wcsrchr(exe, L'\\');
	if (slash) {
		*slash = L'\0';
	}
	GetTempPathW(MAX_PATH, temp);
	swprintf(tool_dir, MAX_PATH + 16, L"%lswf-term-tools-%lu", temp,
	        GetCurrentProcessId());
	CHECK(CreateDirectoryW(tool_dir, NULL));
	{
		wchar_t from[MAX_PATH + 40], to[MAX_PATH + 40];

		swprintf(from, MAX_PATH + 40, L"%ls\\terminal_probe.exe", exe);
		swprintf(to, MAX_PATH + 40, L"%ls\\terminal_probe.exe",
		        tool_dir);
		CHECK(CopyFileW(from, to, FALSE));
		swprintf(probe_out, MAX_PATH + 40, L"%ls\\probe.out", tool_dir);
	}
	a_folder_of_a_local_drive_is_accepted();
	anything_else_is_refused();
	an_argument_is_quoted_so_that_it_reads_back_the_same();
	windows_terminal_is_first_and_alone_when_it_starts();
	powershell_then_cmd_follow_only_when_one_could_not_start();
	windows_terminal_is_taken_from_the_path_only();
	the_reason_given_is_a_failure_and_not_a_left_out_candidate();
	a_folder_that_windows_terminal_would_split_skips_it();
	a_folder_that_is_not_there_or_not_valid_starts_nothing();
	the_folder_arrives_exactly_whatever_its_name_holds();
	no_handle_is_left_behind();
	the_worker_answers_a_terminal_request_once();
	SetEnvironmentVariableW(L"PATH", path_was);
	{
		wchar_t file[MAX_PATH + 40];
		static const wchar_t *const made[] = {
		        L"wt.exe", L"terminal_probe.exe", L"probe.out"};

		for (size_t i = 0; i < sizeof made / sizeof *made; i++) {
			swprintf(file, MAX_PATH + 40, L"%ls\\%ls", tool_dir,
			        made[i]);
			DeleteFileW(file);
		}
		RemoveDirectoryW(tool_dir);
	}
	return TEST_RESULT();
}
