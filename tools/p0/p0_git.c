/*
 * Git through the real product and the real Git of this machine (DR-DRAFT-038):
 * a folder that is no work tree starts no Git process and no Git worker; when it
 * becomes one the letters appear beside the files; a file opened with `e` shows
 * the diff against HEAD, unsaved typing included; the folders and the editor are
 * what they were where Git is missing; a folder outside the repository shows
 * nothing of it. Included by p0_conpty_host.c after p0_resources.c, whose
 * helpers (res_sample, res_children) it uses.
 */

/* How many processes of this name have `pid` as their parent. */
static size_t git_children_named(DWORD pid, const wchar_t *name)
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	PROCESSENTRY32W pe;
	size_t n = 0;

	if (INVALID_HANDLE_VALUE == snap) {
		return 0;
	}
	pe.dwSize = sizeof pe;
	for (BOOL ok = Process32FirstW(snap, &pe); ok;
	        ok = Process32NextW(snap, &pe)) {
		n += pe.th32ParentProcessID == pid &&
		        !_wcsicmp(pe.szExeFile, name);
	}
	CloseHandle(snap);
	return n;
}

/* Watches for `ms`: the most git.exe or wsl.exe children seen at one look. */
static size_t git_children_seen(DWORD pid, DWORD ms)
{
	ULONGLONG end = GetTickCount64() + ms;
	size_t most = 0;

	while (GetTickCount64() < end) {
		size_t n = git_children_named(pid, L"git.exe") +
		        git_children_named(pid, L"wsl.exe");

		most = n > most ? n : most;
		Sleep(4);
	}
	return most;
}

/* Runs `git -C dir <args>` and waits; true when it exits 0. */
static bool git_in(const wchar_t *dir, const wchar_t *args)
{
	wchar_t cmd[2048];
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	DWORD code = 1;

	swprintf(cmd, ARRAYSIZE(cmd), L"git -C \"%ls\" %ls", dir, args);
	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
	            NULL, NULL, &si, &pi)) {
		return false;
	}
	WaitForSingleObject(pi.hProcess, 60000);
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return 0 == code;
}

/* The letter the row of `name` ends with: the character just before the pane's
 * separator, after the name. 0 when the row is not on screen. */
static char git_row_letter(const char *screen, const char *name)
{
	const char *at = strstr(screen, name), *bar;

	if (!at) {
		return 0;
	}
	bar = strchr(at, '|');
	return bar && bar > at ? bar[-1] : 0;
}

/* Waits (bounded) until the row of `name` ends with `letter`. */
static bool git_wait_letter(Gate *g, const char *name, char letter, DWORD ms)
{
	ULONGLONG end = GetTickCount64() + ms;

	while (GetTickCount64() < end) {
		(void)product_wait_ms(
		        &g->pr.p, name, g->pr.screen, sizeof g->pr.screen, 300);
		if (git_row_letter(g->pr.screen, name) == letter) {
			return true;
		}
		Sleep(100);
	}
	return false;
}

/* The same, for a file written by Windows with Japanese in its name. */
#define GIT_JP_NAME "\xE5\x90\x8D\xE5\x89\x8D with space.txt"

static bool git_case(Ev *e)
{
	Gate *g = calloc(1, sizeof *g);
	bool ok = false, letters;
	int fails = e->fails;
	wchar_t repo[1100], a[1100], b[1100], jp[1100], dest_a[1100],
	        plain_name[1100];
	ResSample idle, now_s;
	DWORD pid;
	size_t seen_idle;
	char text[256];

	if (!g || !gate_wanted("git")) {
		free(g);
		return true;
	}
	if (!gate_open(g, e, "git") || !gate_path(g, L"browse\\a.txt", a) ||
	        !gate_write(a, "one\ntwo\n") ||
	        !gate_path(g, L"browse\\c.txt", plain_name) ||
	        !gate_write(plain_name, "clean\n") ||
	        !gate_write(g->cfg, DIRSIZE_CONFIG)) {
		goto done;
	}
	wcscpy_s(repo, ARRAYSIZE(repo), g->browse);
	{
		wchar_t root_dir[4] = {repo[0], L':', 92, 0};
		char info[256];

		snprintf(info, sizeof info, "drive type of %ls: %u", root_dir,
		        GetDriveTypeW(root_dir));
		ev_write_text(e, L"artifacts\\git-fixture-drive.txt", info);
	}
	g->wait_for = "a.txt";
	if (!gate_launch(g)) {
		goto done;
	}
	pid = GetProcessId(g->pr.p.pi.hProcess);
	Sleep(1500);

	/* 1. no work tree: no Git process, no Git worker */
	seen_idle = git_children_seen(pid, 1500);
	ok = res_sample(g, &idle);
	ev_check(e, "git_no_process_outside_a_work_tree", 0,
	        "no git.exe or wsl.exe, threads at rest",
	        ok && 0 == seen_idle && idle.threads <= 17 ? "observed"
	                                                   : "WRONG",
	        ok && 0 == seen_idle && idle.threads <= 17);
	if (!ok || 0 != seen_idle) {
		goto done;
	}
	ok = ' ' == git_row_letter(g->pr.screen, "a.txt");
	ev_check(e, "git_no_letters_outside_a_work_tree", 0, "no letter",
	        ok ? "observed" : "WRONG", ok);

	/* 2. the folder becomes a work tree: the letters come, the list never waited */
	ok = git_in(repo, L"init -q -b main") &&
	        git_in(repo, L"config user.name t") &&
	        git_in(repo, L"config user.email t@example.com") &&
	        git_in(repo, L"config core.autocrlf false") &&
	        git_in(repo, L"add -A") && git_in(repo, L"commit -q -m first");
	if (!ok) {
		ev_check(e, "git_fixture", 0,
		        "a repository made with Windows Git", "failed", 0);
		goto done;
	}
	/* one modified, one new and not added, one added */
	ok = gate_write(a, "one\nTWO\n") && gate_path(g, L"browse\\b.txt", b) &&
	        gate_write(b, "new\n");
	{
		wchar_t jp_rel[64];

		swprintf(jp_rel, 64, L"browse\\%ls",
		        L"\x540D\x524D with space.txt");
		ok = ok && gate_path(g, jp_rel, jp) &&
		        gate_write(jp, "added\n") &&
		        git_in(repo, L"add -- \"\x540D\x524D with space.txt\"");
	}
	letters = ok && git_wait_letter(g, "a.txt", 'M', 20000);
	ev_check(e, "git_modified_file_shows_M", 0, "M beside a.txt",
	        letters ? "observed" : "missing", letters);
	snprintf(text, sizeof text, "%s", g->pr.screen);
	product_snapshot(&g->pr, "git_letters");
	ok = letters && git_wait_letter(g, "b.txt", 'U', 5000) &&
	        git_wait_letter(g, GIT_JP_NAME, 'A', 5000) &&
	        git_wait_letter(g, "c.txt", ' ', 5000);
	ev_check(e, "git_untracked_added_and_clean_rows", 0,
	        "b.txt U, the Japanese-named file A, c.txt none",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	/* the name keeps its room beside the letter */
	ok = NULL != strstr(g->pr.screen, "a.txt") &&
	        NULL != strstr(g->pr.screen, GIT_JP_NAME);
	ev_check(e, "git_names_stay_whole_beside_the_letters", 0,
	        "names readable", ok ? "observed" : "WRONG", ok);

	/* 3. the editor: the diff against HEAD, and unsaved typing in it */
	pty_send(&g->pr.p, "e", 1); /* the cursor is on the first row: a.txt */
	ok = product_wait_ms(
	        &g->pr.p, "Ln 1/", g->pr.screen, sizeof g->pr.screen, 10000);
	ev_check(e, "git_editor_opens", 0, "the editor",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	ok = product_wait_ms(&g->pr.p, "DIFF  HEAD <-> EDITOR", g->pr.screen,
	        sizeof g->pr.screen, 15000);
	product_snapshot(&g->pr, "git_editor_diff");
	ev_check(e, "git_editor_shows_the_diff_pane", 0,
	        "DIFF  HEAD <-> EDITOR", ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	ok = product_wait_ms(&g->pr.p, "+TWO", g->pr.screen,
	             sizeof g->pr.screen, 5000) &&
	        NULL != strstr(g->pr.screen, "-two");
	ev_check(e, "git_diff_shows_the_change_against_head", 0,
	        "-two and +TWO", ok ? "observed" : "missing", ok);
	pty_send(&g->pr.p, "Q", 1); /* typed, not saved */
	ok = product_wait_ms(
	        &g->pr.p, "+Qone", g->pr.screen, sizeof g->pr.screen, 5000);
	ev_check(e, "git_diff_includes_unsaved_typing", 0,
	        "+Qone before any save", ok ? "observed" : "missing", ok);
	{
		char disk[256];

		ok = ok && read_text_file(a, disk, sizeof disk) &&
		        !strcmp(disk, "one\nTWO\n");
		ev_check(e, "git_diff_of_the_buffer_left_the_file_alone", 0,
		        "file unchanged on disk", ok ? "unchanged" : "CHANGED",
		        ok);
	}
	pty_send(&g->pr.p, "\x18", 1);
	ok = ok &&
	        product_wait_ms(&g->pr.p, "Save changes to a.txt?",
	                g->pr.screen, sizeof g->pr.screen, 5000);
	pty_send(&g->pr.p, "d", 1);
	ok = ok &&
	        product_wait_ms(&g->pr.p, "NORMAL", g->pr.screen,
	                sizeof g->pr.screen, 5000);
	if (!ok) {
		goto done;
	}
	/* an untracked file: everything in it is new */
	pty_send(&g->pr.p, "j", 1);
	Sleep(200);
	pty_send(&g->pr.p, "e", 1);
	ok = product_wait_ms(&g->pr.p, "DIFF  HEAD <-> EDITOR", g->pr.screen,
	             sizeof g->pr.screen, 15000) &&
	        product_wait_ms(&g->pr.p, "+new", g->pr.screen,
	                sizeof g->pr.screen, 5000);
	product_snapshot(&g->pr, "git_editor_untracked");
	ev_check(e, "git_untracked_file_is_all_added", 0, "+new",
	        ok ? "observed" : "missing", ok);
	pty_send(&g->pr.p, "\x18", 1);
	ok = ok &&
	        product_wait_ms(&g->pr.p, "NORMAL", g->pr.screen,
	                sizeof g->pr.screen, 5000);
	if (!ok) {
		goto done;
	}

	/* 4. a folder outside the repository shows nothing of it, even for a name that
	 * the repository has */
	{
		wchar_t dest[1100];
		char keys[1300], utf8[1100];

		ok = gate_path(g, L"dest", dest) &&
		        gate_path(g, L"dest\\a.txt", dest_a) &&
		        gate_write(dest_a, "x\n");
		WideCharToMultiByte(
		        CP_UTF8, 0, dest, -1, utf8, sizeof utf8, NULL, NULL);
		snprintf(keys, sizeof keys, "\x0c%s\r", utf8);
		pty_send(&g->pr.p, keys, strlen(keys));
		ok = ok &&
		        product_wait_ms(&g->pr.p, "\\dest", g->pr.screen,
		                sizeof g->pr.screen, 10000);
		Sleep(1500);
		(void)product_wait_ms(&g->pr.p, "a.txt", g->pr.screen,
		        sizeof g->pr.screen, 3000);
		ok = ok && ' ' == git_row_letter(g->pr.screen, "a.txt");
		product_snapshot(&g->pr, "git_outside_the_repository");
		ev_check(e, "git_nothing_of_the_repository_outside_it", 0,
		        "no letter for dest\\a.txt", ok ? "observed" : "WRONG",
		        ok);
	}
	/* 5. over and over: enter the repository, get its status, open a file, edit,
	 * look at the diff, close, leave. Nothing grows: threads, handles, memory, and
	 * no git.exe left behind. */
	{
		ResSample base = {0}, end = {0};
		char to_repo[1300], to_dest[1300], utf8[1100];
		wchar_t dest[1100];
		int cycles = 25;
		bool cycled = true;

		gate_path(g, L"dest", dest);
		WideCharToMultiByte(
		        CP_UTF8, 0, repo, -1, utf8, sizeof utf8, NULL, NULL);
		snprintf(to_repo, sizeof to_repo, "\x0c%s\r",
		        utf8); /* Ctrl+L, the path, Enter */
		WideCharToMultiByte(
		        CP_UTF8, 0, dest, -1, utf8, sizeof utf8, NULL, NULL);
		snprintf(to_dest, sizeof to_dest, "\x0c%s\r", utf8);
		for (int i = 0; i < cycles && cycled; i++) {
			if (3 ==
			        i) { /* what the first turns load is not growth */
				Sleep(2500);
				cycled = res_sample(g, &base);
			}
			pty_send(&g->pr.p, to_repo, strlen(to_repo));
			cycled = cycled &&
			        git_wait_letter(g, "a.txt", 'M', 15000);
			pty_send(&g->pr.p, "e", 1);
			cycled = cycled &&
			        product_wait_ms(&g->pr.p,
			                "DIFF  HEAD <-> EDITOR", g->pr.screen,
			                sizeof g->pr.screen, 15000);
			pty_send(&g->pr.p, "k", 1);
			cycled = cycled &&
			        product_wait_ms(&g->pr.p, "+k", g->pr.screen,
			                sizeof g->pr.screen, 8000);
			pty_send(&g->pr.p, "\x18", 1);
			cycled = cycled &&
			        product_wait_ms(&g->pr.p, "Save changes to ",
			                g->pr.screen, sizeof g->pr.screen,
			                5000);
			pty_send(&g->pr.p, "d", 1);
			cycled = cycled &&
			        product_wait_ms(&g->pr.p, "NORMAL",
			                g->pr.screen, sizeof g->pr.screen,
			                5000);
			pty_send(&g->pr.p, to_dest, strlen(to_dest));
			cycled = cycled &&
			        product_wait_ms(&g->pr.p, "\\dest",
			                g->pr.screen, sizeof g->pr.screen,
			                8000);
			if (!cycled) {
				char why[64];

				snprintf(why, sizeof why,
				        "git_stress_stopped_in_cycle_%d", i);
				product_snapshot(&g->pr, why);
			}
		}
		Sleep(2500);
		cycled = cycled && res_sample(g, &end);
		ok = cycled && end.threads <= base.threads &&
		        end.handles <= base.handles + 6 &&
		        (RES_SANITIZED ||
		                end.private_bytes <=
		                        base.private_bytes + 3.0 * 1048576.0) &&
		        0 == git_children_named(pid, L"git.exe") &&
		        0 == git_children_named(pid, L"wsl.exe");
		{
			char note[256];

			snprintf(note, sizeof note,
			        "threads %.0f -> %.0f, handles %.0f -> %.0f, "
			        "private %.0f -> %.0f after %d cycles",
			        base.threads, end.threads, base.handles,
			        end.handles, base.private_bytes,
			        end.private_bytes, cycles);
			ev_write_text(e, L"artifacts\\git-stress.txt", note);
		}
		ev_check(e, "git_repeated_use_leaves_nothing_behind", 0,
		        "25 cycles: threads and handles as before (6 more at "
		        "most), memory within 3 MiB, no git.exe",
		        ok ? "observed" : "WRONG", ok);
		if (!ok) {
			goto done;
		}
	}
	/* the program is as light as before and nothing is left running */
	Sleep(2500); /* the worker is released when it has nothing to do */
	ok = res_sample(g, &now_s) && now_s.threads <= idle.threads + 1 &&
	        0 == git_children_named(pid, L"git.exe") &&
	        0 == git_children_named(pid, L"wsl.exe");
	ev_check(e, "git_worker_and_processes_are_gone_when_idle", 0,
	        "threads back at rest, no git.exe", ok ? "observed" : "WRONG",
	        ok);
	ok = gate_quit(g) && ok;
done:
	return gate_first_run_end(g, e, ok, fails);
}

/* Without Git on PATH: the folders and the editor are as ever, with nothing said. */
static bool git_missing_case(Ev *e)
{
	Gate *g = calloc(1, sizeof *g);
	bool ok = false;
	int fails = e->fails;
	wchar_t repo[1100], a[1100], saved_path[32768], sys[MAX_PATH];
	DWORD pid;

	if (!g || !gate_wanted("gitmissing")) {
		free(g);
		return true;
	}
	if (!gate_open(g, e, "gitmissing") ||
	        !gate_path(g, L"browse\\a.txt", a) ||
	        !gate_write(a, "one\ntwo\n") ||
	        !gate_write(g->cfg, DIRSIZE_CONFIG)) {
		goto done;
	}
	wcscpy_s(repo, ARRAYSIZE(repo), g->browse);
	/* the repository is made with Git, then the child is started without it */
	ok = git_in(repo, L"init -q -b main") &&
	        git_in(repo, L"config user.name t") &&
	        git_in(repo, L"config user.email t@example.com") &&
	        git_in(repo, L"add -A") &&
	        git_in(repo, L"commit -q -m first") &&
	        gate_write(a, "one\nTWO\n");
	if (!ok) {
		ev_check(e, "gitmissing_fixture", 0, "a repository", "failed",
		        0);
		goto done;
	}
	GetEnvironmentVariableW(L"PATH", saved_path, ARRAYSIZE(saved_path));
	GetSystemDirectoryW(sys, MAX_PATH);
	SetEnvironmentVariableW(L"PATH",
	        sys); /* System32 only: no git.exe, no wsl.exe search */
	g->wait_for = "a.txt";
	ok = gate_launch(g);
	SetEnvironmentVariableW(L"PATH", saved_path);
	if (!ok) {
		goto done;
	}
	pid = GetProcessId(g->pr.p.pi.hProcess);
	/* a work tree, and no Git for it: the names are there, no letters, no message, no process */
	ok = 0 == git_children_seen(pid, 3000);
	ev_check(e, "gitmissing_no_process_is_started", 0, "no git.exe",
	        ok ? "observed" : "WRONG", ok);
	ok = ok && ' ' == git_row_letter(g->pr.screen, "a.txt") &&
	        NULL == strstr(g->pr.screen, "Git") &&
	        NULL == strstr(g->pr.screen, "git");
	ev_check(e, "gitmissing_the_list_is_as_ever_and_says_nothing", 0,
	        "a.txt, no letter, no message", ok ? "observed" : "WRONG", ok);
	if (!ok) {
		goto done;
	}
	pty_send(&g->pr.p, "e", 1); /* the cursor is on the first row: a.txt */
	ok = product_wait_ms(
	        &g->pr.p, "Ln 1/", g->pr.screen, sizeof g->pr.screen, 10000);
	Sleep(1500);
	ok = ok && NULL == strstr(g->pr.screen, "DIFF  HEAD");
	pty_send(&g->pr.p, "Z", 1);
	ok = ok &&
	        product_wait_ms(&g->pr.p, "[Modified]", g->pr.screen,
	                sizeof g->pr.screen, 5000);
	ev_check(e, "gitmissing_the_editor_is_whole_without_a_pane", 0,
	        "editing works, no DIFF", ok ? "observed" : "WRONG", ok);
	pty_send(&g->pr.p, "\x18", 1);
	ok = ok &&
	        product_wait_ms(&g->pr.p, "Save changes to a.txt?",
	                g->pr.screen, sizeof g->pr.screen, 5000);
	pty_send(&g->pr.p, "d", 1);
	ok = ok &&
	        product_wait_ms(&g->pr.p, "NORMAL", g->pr.screen,
	                sizeof g->pr.screen, 5000);
	product_snapshot(&g->pr, "gitmissing_before_quit");
	ok = gate_quit(g) && ok;
done:
	return gate_first_run_end(g, e, ok, fails);
}

static int scn_git(Ev *e)
{
	bool ok = git_case(e);

	ok = git_missing_case(e) && ok;
	return ok ? (g_gate_skips ? 77 : 0) : 1;
}
