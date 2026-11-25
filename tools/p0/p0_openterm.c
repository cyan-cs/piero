/*
 * Open Terminal Here through the real product: `!` starts a terminal in the folder
 * on screen, not the entry under the cursor, and the program goes on working.
 * No terminal window is opened: the program that is found as wt.exe, first on a
 * PATH made for this one run, is tests/terminal_probe.exe, which writes the
 * directory it was started in and its arguments to a file and exits. Included by
 * p0_conpty_host.c after p0_tabs.c, whose helpers (gate_*, dirsize_row) it uses.
 */

#define OPENTERM_CONFIG                                                        \
	"[meta]\r\nschema_version = 1\r\nonboarding_completed = true\r\n"      \
	"[updates]\r\nenabled = false\r\nconsent_recorded = true\r\n"

/* The probe's report (UTF-16, a line to each of the directory and the
 * arguments) as text, or NULL if it did not come within `ms`. */
static wchar_t *openterm_report(const wchar_t *file, DWORD ms)
{
	ULONGLONG end = GetTickCount64() + ms;

	do {
		HANDLE h = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ,
		        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

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
	} while (GetTickCount64() < end);
	return NULL;
}

static bool openterm_case(Ev *e)
{
	Gate *g = calloc(1, sizeof *g);
	bool ok = false;
	int fails = e->fails;
	wchar_t host[MAX_PATH * 2], probe[MAX_PATH * 2 + 40], exe[1100],
	        out[1100], tools[1100], want[2400], *report, *slash;
	wchar_t *path_was = NULL, *path_now = NULL;
	DWORD need;

	if (!g || !gate_wanted("terminal_here")) {
		free(g);
		return true;
	}
	if (!gate_open(g, e, "terminal_here") ||
	        !gate_write(g->cfg, OPENTERM_CONFIG) ||
	        !gate_mkdir(g, L"browse\\sub") || !gate_mkdir(g, L"tools") ||
	        !gate_path(g, L"tools", tools) ||
	        !gate_path(g, L"tools\\wt.exe", exe) ||
	        !gate_path(g, L"probe.out", out)) {
		goto done;
	}
	GetModuleFileNameW(NULL, host, MAX_PATH * 2);
	slash = wcsrchr(host, L'\\');
	if (slash) {
		*slash = L'\0';
	}
	swprintf(probe, MAX_PATH * 2 + 40, L"%ls\\terminal_probe.exe", host);
	ok = CopyFileW(probe, exe, FALSE);
	ev_check(e, "terminal_here_probe_stands_for_wt", 0,
	        "terminal_probe.exe found", ok ? "found" : "missing", ok);
	if (!ok) {
		goto done;
	}
	/* the program is started with a PATH that finds the probe as wt.exe */
	need = GetEnvironmentVariableW(L"PATH", NULL, 0);
	path_was = calloc(need + 2, sizeof(wchar_t));
	path_now = calloc(need + wcslen(tools) + 4, sizeof(wchar_t));
	ok = path_was && path_now &&
	        GetEnvironmentVariableW(L"PATH", path_was, need);
	if (ok) {
		swprintf(path_now, need + wcslen(tools) + 4, L"%ls;%ls", tools,
		        path_was);
		ok = SetEnvironmentVariableW(L"PATH", path_now) &&
		        SetEnvironmentVariableW(L"WF_TERMINAL_PROBE_OUT", out);
	}
	ok = ok && gate_launch(g);
	if (path_was) {
		SetEnvironmentVariableW(L"PATH", path_was);
	}
	SetEnvironmentVariableW(L"WF_TERMINAL_PROBE_OUT", NULL);
	if (!ok) {
		goto done;
	}
	/* the cursor goes to a file: the folder opened is still the one on screen */
	pty_send(&g->pr.p, "j", 1);
	ok = dirsize_row(g, "> doc.txt", "", 8000);
	pty_send(&g->pr.p, "!", 1);
	report = openterm_report(out, 10000);
	swprintf(want, 2400, L"%ls\n-d\n%ls", g->browse, g->browse);
	ok = ok && report && !wcscmp(report, want);
	if (report && wcscmp(report, want)) {
		wprintf(L"  want [%ls] got [%ls]\n", want, report);
	}
	free(report);
	product_snapshot(&g->pr, "terminal_here_opened");
	ev_check(e, "terminal_here_starts_wt_in_the_folder_on_screen", 0,
	        "wt.exe -d <browse>, started in <browse>, not the entry under "
	        "the cursor",
	        ok ? "observed" : "WRONG", ok);
	if (!ok) {
		goto done;
	}
	/* the program is as it was: the message, the cursor, and it still answers */
	ok = product_wait_ms(&g->pr.p, "Terminal opened", g->pr.screen,
	        sizeof g->pr.screen, 6000);
	ev_check(e, "terminal_here_says_so", 0, "Terminal opened in ...",
	        ok ? "observed" : "missing", ok);
	pty_send(&g->pr.p, "j", 1);
	ok = ok && dirsize_row(g, "> note.txt", "", 8000);
	ev_check(e, "terminal_here_program_goes_on", 0,
	        "the next key moves the cursor", ok ? "observed" : "stuck", ok);
	ok = ok && gate_quit(g);
done:
	free(path_was);
	free(path_now);
	return gate_first_run_end(g, e, ok, fails);
}

static int scn_openterm(Ev *e)
{
	return openterm_case(e) ? (g_gate_skips ? 77 : 0) : 1;
}
