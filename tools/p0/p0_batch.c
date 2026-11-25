/*
 * Bulk rename (`M`) through the real product (DR-DRAFT-022, UI), on real files of
 * an owned folder, with the program's own state folder (the journals) in the
 * owned area, never the user's profile:
 *
 *   batch_rename   marked files, a numbering pattern, the preview, the one
 *                  question, and the names, contents and cursor afterwards; the
 *                  journal is closed; u undoes nothing
 *   batch_refused  a batch with a duplicate target and one onto an existing name
 *                  cannot be run (and nothing changes); a wrong template; Esc at
 *                  the pattern; n at the question
 *   batch_cancel   2,500 files; Esc in the middle of the run: every name is put
 *                  back and the screen says "Cancelled", never "Done"
 *   batch_crash    the program is ended in the middle of the run; the next start
 *                  asks about the journal, "no" leaves everything as it is, "yes"
 *                  recovers and says so; after that nothing is asked
 *
 * Included by p0_conpty_host.c after p0_state_gate.c and p0_dirsize.c, whose
 * helpers (gate_*, dirsize_row) it uses.
 */

#define BATCH_BIG 2500

static bool bt_wait(Gate *g, const char *text, DWORD ms)
{
	return product_wait_ms(
	        &g->pr.p, text, g->pr.screen, sizeof g->pr.screen, ms);
}

/* Waits until the result of a run is on the screen, whatever it says. */
static bool bt_wait_result(Gate *g, DWORD ms)
{
	ULONGLONG end = GetTickCount64() + ms;

	do {
		pty_snapshot(&g->pr.p, g->pr.screen, sizeof g->pr.screen);
		if (strstr(g->pr.screen, "Bulk rename: result")) {
			return bt_wait(g, "Bulk rename: result", 2000);
		}
		Sleep(50);
	} while (GetTickCount64() < end);
	return false;
}

static void bt_type(Gate *g, const char *text)
{
	for (const char *p = text; *p; p++) {
		pty_send(&g->pr.p, p, 1);
		Sleep(25);
	}
}

/* Replaces the template field's {name} by `text` and goes to the preview. */
static void bt_template(Gate *g, const char *text)
{
	pty_send(&g->pr.p, "\x7f\x7f\x7f\x7f\x7f\x7f", 6);
	Sleep(50);
	bt_type(g, text);
	pty_send(&g->pr.p, "\r", 1);
}

static bool bt_make(Gate *g, const wchar_t *name, const char *text)
{
	wchar_t rel[160], path[1100];

	swprintf(rel, 160, L"browse\\%ls", name);
	return gate_path(g, rel, path) && gate_write(path, text);
}

static bool bt_file_is(Gate *g, const wchar_t *name, const char *text)
{
	wchar_t rel[160], path[1100];
	char buf[128] = {0};
	DWORD n = 0;
	HANDLE h;
	bool ok;

	swprintf(rel, 160, L"browse\\%ls", name);
	if (!gate_path(g, rel, path)) {
		return false;
	}
	h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	ReadFile(h, buf, sizeof buf - 1, &n, NULL);
	CloseHandle(h);
	ok = n == strlen(text) && !memcmp(buf, text, n);
	return ok;
}

static bool bt_exists(Gate *g, const wchar_t *name)
{
	wchar_t rel[160], path[1100];

	swprintf(rel, 160, L"browse\\%ls", name);
	return gate_path(g, rel, path) &&
	        INVALID_FILE_ATTRIBUTES != GetFileAttributesW(path);
}

/* The journals the program wrote: how many, and whether one says `end`. */
static int bt_journals(Gate *g, const char *end)
{
	wchar_t dir[1100], pattern[1200];
	WIN32_FIND_DATAW fd;
	HANDLE h;
	int n = 0;

	if (!gate_path(g, L"piero\\batch", dir)) {
		return -1;
	}
	swprintf(pattern, 1200, L"%ls\\*.pbj", dir);
	h = FindFirstFileW(pattern, &fd);
	if (INVALID_HANDLE_VALUE == h) {
		return 0;
	}
	do {
		wchar_t file[1300];
		char text[4096] = {0};
		DWORD got = 0;
		HANDLE f;

		if (!end) {
			n++;
			continue;
		}
		swprintf(file, 1300, L"%ls\\%ls", dir, fd.cFileName);
		f = CreateFileW(file, GENERIC_READ,
		        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
		        FILE_ATTRIBUTE_NORMAL, NULL);
		if (INVALID_HANDLE_VALUE != f) {
			LARGE_INTEGER size;

			if (GetFileSizeEx(f, &size) &&
			        size.QuadPart > (long long)sizeof text - 1) {
				LARGE_INTEGER at;

				at.QuadPart = size.QuadPart -
				        (long long)sizeof text + 1;
				SetFilePointerEx(f, at, NULL, FILE_BEGIN);
			}
			ReadFile(f, text, sizeof text - 1, &got, NULL);
			CloseHandle(f);
			n += NULL != strstr(text, end);
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	return n;
}

static bool batch_rename_case(Ev *e)
{
	Gate *g = calloc(1, sizeof *g);
	Product *pr;
	bool ok = false;
	int fails = e->fails;
	char *before = NULL, *mid = NULL, *after = NULL;

	if (!g || !gate_wanted("batch_rename")) {
		free(g);
		return true;
	}
	if (!gate_open(g, e, "batch_rename") ||
	        !bt_make(g, L"alpha.txt", "ALPHA") ||
	        !bt_make(g, L"beta.txt", "BETA") ||
	        !bt_make(g, L"gamma.txt", "GAMMA") ||
	        !bt_make(g, L"delta.txt", "DELTA") || !gate_launch(g)) {
		goto done;
	}
	pr = &g->pr;
	before = gate_tree_text(g->browse);
	/* alpha, beta and gamma are marked (the list is alpha beta delta doc gamma note) */
	pty_send(&pr->p, " ", 1);
	Sleep(60);
	pty_send(&pr->p, " ", 1);
	Sleep(60);
	pty_send(&pr->p, "\x1b[B", 3);
	Sleep(60);
	pty_send(&pr->p, "\x1b[B", 3);
	Sleep(60);
	pty_send(&pr->p, " ", 1);
	Sleep(60);
	pty_send(&pr->p, "M", 1);
	ok = bt_wait(g, "Template", 8000) &&
	        NULL != strstr(pr->screen, "3 item(s) in") &&
	        NULL != strstr(pr->screen, "{name}");
	product_snapshot(pr, "batch_pattern");
	ev_check(e, "batch_m_opens_the_pattern_for_the_marked_files", 0,
	        "Template field, 3 item(s)", ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	bt_template(g, "photo_{n:3}");
	ok = bt_wait(g, "Ready.", 15000) &&
	        NULL != strstr(pr->screen, "alpha.txt -> photo_001.txt") &&
	        NULL != strstr(pr->screen, "beta.txt -> photo_002.txt") &&
	        NULL != strstr(pr->screen, "gamma.txt -> photo_003.txt") &&
	        NULL !=
	                strstr(pr->screen,
	                        "3 items: 3 to rename, 0 unchanged, 0 refused");
	product_snapshot(pr, "batch_preview");
	ev_check(e, "batch_preview_shows_before_and_after", 0,
	        "three rows, before -> after, counts, Ready",
	        ok ? "observed" : "missing", ok);
	ok = ok && gate_tree_is(g->browse, before);
	ev_check(e, "batch_preview_changes_nothing", 0, "names as they were",
	        ok ? "unchanged" : "CHANGED", ok);
	if (!ok) {
		goto done;
	}
	pty_send(&pr->p, "\r", 1);
	ok = bt_wait(g, "Folder:", 8000) && bt_wait(g, "[Y] Yes", 8000) &&
	        NULL != strstr(pr->screen, "3 to rename, 0 unchanged");
	product_snapshot(pr, "batch_question");
	ev_check(e, "batch_one_question_before_anything", 0,
	        "the question with the counts", ok ? "observed" : "missing",
	        ok);
	mid = gate_tree_text(g->browse);
	ok = ok && mid &&
	        !strcmp(mid,
	                before); /* nothing yet: the answer is still to come */
	ev_check(e, "batch_nothing_changes_before_the_answer", 0,
	        "names as they were while the question is open",
	        ok ? "unchanged" : "CHANGED", ok);
	if (!ok) {
		goto done;
	}
	pty_send(&pr->p, "y", 1);
	ok = bt_wait(g, "Done: 3 item(s) renamed", 15000);
	product_snapshot(pr, "batch_done");
	ev_check(e, "batch_result_says_done_for_a_success", 0,
	        "Done: 3 item(s) renamed", ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	ok = bt_file_is(g, L"photo_001.txt", "ALPHA") &&
	        bt_file_is(g, L"photo_002.txt", "BETA") &&
	        bt_file_is(g, L"photo_003.txt", "GAMMA") &&
	        !bt_exists(g, L"alpha.txt") && !bt_exists(g, L"beta.txt") &&
	        !bt_exists(g, L"gamma.txt") &&
	        bt_file_is(g, L"delta.txt", "DELTA");
	ev_check(e, "batch_names_and_contents_on_disk", 0,
	        "photo_001..003 hold ALPHA, BETA, GAMMA; delta untouched",
	        ok ? "observed" : "WRONG", ok);
	if (!ok) {
		goto done;
	}
	ok = 1 == bt_journals(g, NULL) && 1 == bt_journals(g, "\nE SUCCESS*");
	ev_check(e, "batch_journal_is_closed_as_success", 0,
	        "one journal, its end line SUCCESS",
	        ok ? "observed" : "missing", ok);
	pty_send(&pr->p, "\x1b", 1);
	ok = bt_wait(g, "NORMAL", 8000) &&
	        dirsize_row(g, "> photo_001.txt", "", 6000);
	product_snapshot(pr, "batch_cursor");
	ev_check(e, "batch_cursor_is_on_a_renamed_item", 0, "> photo_001.txt",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	after = gate_tree_text(g->browse);
	/* the history (J) lists the batch and says that u can take it back */
	pty_send(&pr->p, "J", 1);
	ok = bt_wait(g, "BULK RENAME: 3 item(s)", 8000) &&
	        NULL != strstr(pr->screen, "u: undo this");
	product_snapshot(pr, "batch_history");
	ev_check(e, "batch_history_row_says_u_can_undo_it", 0,
	        "BULK RENAME: 3 item(s) with u: undo this",
	        ok ? "observed" : "missing", ok);
	pty_send(&pr->p, "\x1b", 1);
	ok = ok && bt_wait(g, "NORMAL", 8000);
	if (!ok) {
		goto done;
	}
	/* u asks first, and nothing changes before the answer */
	pty_send(&pr->p, "u", 1);
	ok = bt_wait(g, "Undo the bulk rename?", 8000);
	Sleep(600);
	{
		char *now = gate_tree_text(g->browse);

		ok = ok && now && after && !strcmp(now, after);
		free(now);
	}
	ev_check(e, "batch_undo_asks_and_changes_nothing_before_the_answer", 0,
	        "the question is open and the names are as they were",
	        ok ? "unchanged" : "CHANGED", ok);
	if (!ok) {
		goto done;
	}
	Sleep(400); /* the guard of every question */
	pty_send(&pr->p, "y", 1);
	ok = bt_wait(g, "Undone: 3 name(s) put back", 15000);
	product_snapshot(pr, "batch_undone");
	ev_check(e, "batch_undo_says_undone_for_a_success", 0,
	        "Undone: 3 name(s) put back", ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	ok = bt_file_is(g, L"alpha.txt", "ALPHA") &&
	        bt_file_is(g, L"beta.txt", "BETA") &&
	        bt_file_is(g, L"gamma.txt", "GAMMA") &&
	        !bt_exists(g, L"photo_001.txt") &&
	        !bt_exists(g, L"photo_002.txt") &&
	        !bt_exists(g, L"photo_003.txt") &&
	        bt_file_is(g, L"delta.txt", "DELTA");
	ev_check(e, "batch_undo_puts_every_name_and_content_back", 0,
	        "alpha, beta, gamma hold their contents again; photo_* are "
		"gone",
	        ok ? "observed" : "WRONG", ok);
	ok = ok && 2 == bt_journals(g, NULL) &&
	        2 == bt_journals(g, "\nE SUCCESS*");
	ev_check(e, "batch_undo_has_its_own_closed_journal", 0,
	        "two journals, both ended SUCCESS", ok ? "observed" : "missing",
	        ok);
	pty_send(&pr->p, "\x1b", 1);
	ok = ok && bt_wait(g, "NORMAL", 8000);
	pty_send(&pr->p, "u", 1);
	ok = ok && bt_wait(g, "Nothing to undo", 8000);
	ev_check(e, "batch_undone_once_is_undone_for_good", 0,
	        "a second u has nothing to undo", ok ? "observed" : "missing",
	        ok);
	ok = ok && gate_quit(g);
done:
	free(before);
	free(mid);
	free(after);
	return gate_first_run_end(g, e, ok, fails);
}

static bool batch_refused_case(Ev *e)
{
	Gate *g = calloc(1, sizeof *g);
	Product *pr;
	bool ok = false;
	int fails = e->fails;
	char *before = NULL;

	if (!g || !gate_wanted("batch_refused")) {
		free(g);
		return true;
	}
	if (!gate_open(g, e, "batch_refused") ||
	        !bt_make(g, L"x1.txt", "ONE") ||
	        !bt_make(g, L"x2.txt", "TWO") || !gate_launch(g)) {
		goto done;
	}
	pr = &g->pr;
	before = gate_tree_text(g->browse);
	/* the list is doc note x1 x2: mark x1 and x2 */
	pty_send(&pr->p, "\x1b[B", 3);
	Sleep(60);
	pty_send(&pr->p, "\x1b[B", 3);
	Sleep(60);
	pty_send(&pr->p, " ", 1);
	Sleep(60);
	pty_send(&pr->p, " ", 1);
	Sleep(60);
	pty_send(&pr->p, "M", 1);
	ok = bt_wait(g, "Template", 8000) &&
	        NULL != strstr(pr->screen, "2 item(s) in");
	if (!ok) {
		goto done;
	}
	bt_template(g, "same");
	ok = bt_wait(g, "Cannot run:", 15000) &&
	        NULL != strstr(pr->screen, "another item gets the same name") &&
	        NULL != strstr(pr->screen, "2 refused");
	product_snapshot(pr, "batch_refused_preview");
	ev_check(e, "batch_duplicate_target_is_refused_with_its_reason", 0,
	        "Cannot run, 2 refused, the reason on the rows",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	pty_send(&pr->p, "\r", 1);
	Sleep(900);
	pty_snapshot(&pr->p, pr->screen, sizeof pr->screen);
	ok = NULL == strstr(pr->screen, "Folder:") &&
	        NULL == strstr(pr->screen, "[Y] Yes") &&
	        NULL != strstr(pr->screen, "Cannot run:") &&
	        gate_tree_is(g->browse, before);
	product_snapshot(pr, "batch_refused_enter");
	ev_check(e, "batch_refused_rows_cannot_be_run", 0,
	        "Enter asks nothing; names as they were",
	        ok ? "nothing asked, unchanged" : "ASKED or CHANGED", ok);
	if (!ok) {
		goto done;
	}
	pty_send(&pr->p, "\x1b", 1); /* the fields */
	ok = bt_wait(g, "Template", 5000);
	pty_send(&pr->p, "\x1b", 1); /* nothing */
	ok = ok && bt_wait(g, "NORMAL", 5000) &&
	        gate_tree_is(g->browse, before);
	ev_check(e, "batch_esc_leaves_no_trace", 0,
	        "NORMAL, names as they were", ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	/* onto a name that exists and is not part of the batch */
	pty_send(&pr->p, "\x1b", 1); /* clears the marks */
	Sleep(300);
	pty_send(&pr->p, "M", 1); /* x2, under the cursor */
	ok = bt_wait(g, "Template", 8000) &&
	        NULL != strstr(pr->screen, "1 item(s) in");
	if (!ok) {
		goto done;
	}
	bt_template(g, "x1");
	ok = bt_wait(g, "Cannot run:", 15000) &&
	        NULL !=
	                strstr(pr->screen,
	                        "a file or folder of that name exists");
	product_snapshot(pr, "batch_refused_exists");
	ev_check(e, "batch_existing_name_is_refused", 0,
	        "x2.txt -> x1.txt refused: that name exists",
	        ok ? "observed" : "missing", ok);
	pty_send(&pr->p, "\r", 1);
	Sleep(700);
	pty_snapshot(&pr->p, pr->screen, sizeof pr->screen);
	ok = ok && NULL == strstr(pr->screen, "Folder:") &&
	        gate_tree_is(g->browse, before);
	ev_check(e, "batch_existing_name_cannot_be_run", 0,
	        "nothing asked, nothing changed",
	        ok ? "observed" : "ASKED or CHANGED", ok);
	if (!ok) {
		goto done;
	}
	pty_send(&pr->p, "\x1b", 1);
	ok = bt_wait(g, "Template", 5000);
	/* a template that is wrong says where, and does not go on */
	pty_send(&pr->p, "\x7f\x7f", 2);
	Sleep(50);
	bt_type(g, "{bad}");
	ok = ok && bt_wait(g, "Template error at position", 5000);
	pty_send(&pr->p, "\r", 1);
	Sleep(500);
	pty_snapshot(&pr->p, pr->screen, sizeof pr->screen);
	ok = ok && NULL != strstr(pr->screen, "Template error at position") &&
	        NULL == strstr(pr->screen, "Cannot run:");
	ev_check(e, "batch_wrong_template_is_shown_and_not_taken", 0,
	        "error position shown; no preview", ok ? "observed" : "missing",
	        ok);
	pty_send(&pr->p, "\x1b", 1);
	ok = ok && bt_wait(g, "NORMAL", 5000);
	if (!ok) {
		goto done;
	}
	/* a good batch, answered with n */
	pty_send(&pr->p, "M", 1);
	ok = bt_wait(g, "Template", 8000);
	bt_template(g, "z{n}");
	ok = ok && bt_wait(g, "Ready.", 15000);
	pty_send(&pr->p, "\r", 1);
	ok = ok && bt_wait(g, "[Y] Yes", 8000);
	pty_send(&pr->p, "n", 1);
	ok = ok && bt_wait(g, "NORMAL", 5000);
	Sleep(300);
	ok = ok && gate_tree_is(g->browse, before);
	ev_check(e, "batch_no_to_the_question_changes_nothing", 0,
	        "NORMAL, names as they were", ok ? "observed" : "missing", ok);
	ok = ok && gate_quit(g);
done:
	free(before);
	return gate_first_run_end(g, e, ok, fails);
}

static bool bt_fill(Gate *g)
{
	for (int i = 0; i < BATCH_BIG; i++) {
		wchar_t name[40];

		swprintf(name, 40, L"f%04d.dat", i);
		if (!bt_make(g, name, "d")) {
			return false;
		}
	}
	return true;
}

/* How many entries the folder has (the marks go one to a row). */
static int bt_count(Gate *g)
{
	wchar_t pattern[1200];
	WIN32_FIND_DATAW fd;
	HANDLE h;
	int n = 0;

	swprintf(pattern, 1200, L"%ls\\*", g->browse);
	h = FindFirstFileW(pattern, &fd);
	if (INVALID_HANDLE_VALUE == h) {
		return -1;
	}
	do {
		n += wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..");
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	return n;
}

/* Marks every row (the same key many times is not taken for a paste) and goes
 * to the question, answers nothing. */
static bool bt_to_question(Gate *g)
{
	Product *pr = &g->pr;
	int n = bt_count(g);
	char *spaces;

	if (n <= 0 || !(spaces = malloc((size_t)n))) {
		return false;
	}
	memset(spaces, ' ', (size_t)n);
	pty_send(&pr->p, spaces, (size_t)n);
	free(spaces);
	Sleep(1500);
	pty_send(&pr->p, "M", 1);
	if (!bt_wait(g, "Template", 15000)) {
		return false;
	}
	bt_template(g, "n{n:5}");
	if (!bt_wait(g, "Ready.", 90000)) {
		return false;
	}
	pty_send(&pr->p, "\r", 1);
	return bt_wait(g, "Folder:", 10000) && bt_wait(g, "[Y] Yes", 10000);
}

static bool batch_cancel_case(Ev *e)
{
	Gate *g = calloc(1, sizeof *g);
	Product *pr;
	bool ok = false;
	int fails = e->fails;
	char *before = NULL, *after = NULL;

	if (!g || !gate_wanted("batch_cancel")) {
		free(g);
		return true;
	}
	if (!gate_open(g, e, "batch_cancel") || !bt_fill(g) ||
	        !gate_launch(g)) {
		goto done;
	}
	pr = &g->pr;
	before = gate_tree_text(g->browse);
	ok = bt_to_question(g);
	ev_check(e, "batchcancel_the_question_for_2500_files", 0,
	        "preview made, question open", ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	pty_send(&pr->p, "y", 1);
	Sleep(150); /* a hundred steps or so are done */
	pty_send(&pr->p, "\x1b", 1);
	ok = bt_wait_result(g, 120000) &&
	        NULL != strstr(g->pr.screen, "Cancelled:");
	product_snapshot(pr, "batchcancel_result");
	ev_check(e, "batchcancel_result_says_cancelled", 0,
	        "Cancelled: ... every one was put back",
	        ok ? "observed" : "missing", ok);
	ok = ok && NULL == strstr(pr->screen, "Done:") &&
	        NULL != strstr(pr->screen, "every one was put back");
	ev_check(e, "batchcancel_never_says_done", 0, "no Done on the screen",
	        ok ? "observed" : "DONE shown", ok);
	if (!ok) {
		goto done;
	}
	after = gate_tree_text(g->browse);
	ok = after && before && !strcmp(after, before);
	ev_check(e, "batchcancel_every_name_is_back", 0,
	        "names, sizes and contents as they were",
	        ok ? "unchanged" : "CHANGED", ok);
	ok = ok && 1 == bt_journals(g, NULL) &&
	        1 == bt_journals(g, "\nE ROLLED_BACK*");
	ev_check(e, "batchcancel_journal_ends_rolled_back", 0,
	        "one journal, its end line ROLLED_BACK",
	        ok ? "observed" : "missing", ok);
	pty_send(&pr->p, "\x1b", 1);
	ok = ok && bt_wait(g, "NORMAL", 8000) && gate_quit(g);
done:
	free(before);
	free(after);
	return gate_first_run_end(g, e, ok, fails);
}

static bool batch_crash_case(Ev *e)
{
	Gate *g = calloc(1, sizeof *g);
	Product *pr;
	bool ok = false;
	int fails = e->fails;
	char *before = NULL, *partial = NULL, *now = NULL;
	wchar_t first[1100], last[1100];
	double t0;

	if (!g || !gate_wanted("batch_crash")) {
		free(g);
		return true;
	}
	if (!gate_open(g, e, "batch_crash") || !bt_fill(g) || !gate_launch(g)) {
		goto done;
	}
	pr = &g->pr;
	before = gate_tree_text(g->browse);
	if (!bt_to_question(g) ||
	        !gate_path(g, L"browse\\n00001.txt",
	                first) || /* doc.txt, the first row */
	        !gate_path(g, L"browse\\n02502.txt",
	                last)) { /* note.txt, the last */
		goto done;
	}
	pty_send(&pr->p, "y", 1);
	/* the first rename is on the disk: end the program now, as a crash would */
	t0 = now_ms();
	while (now_ms() - t0 < 30000 &&
	        INVALID_FILE_ATTRIBUTES == GetFileAttributesW(first)) {
		Sleep(2);
	}
	TerminateProcess(pr->p.pi.hProcess, 99); /* the harness's own child */
	WaitForSingleObject(pr->p.pi.hProcess, 5000);
	pty_finish(&pr->p);
	pr->started = false;
	partial = gate_tree_text(g->browse);
	ok = partial && before && strcmp(partial, before) &&
	        INVALID_FILE_ATTRIBUTES != GetFileAttributesW(first) &&
	        INVALID_FILE_ATTRIBUTES == GetFileAttributesW(last);
	ev_check(e, "batchcrash_ended_in_the_middle", 0,
	        "some names changed, not all",
	        ok ? "observed" : "ran to the end or never began", ok);
	ok = ok && 1 == bt_journals(g, NULL) && 0 == bt_journals(g, "\nE ");
	ev_check(e, "batchcrash_journal_has_no_end", 0,
	        "one journal, no end line", ok ? "observed" : "WRONG", ok);
	if (!ok) {
		goto done;
	}
	/* the next start: the question, never an answer of its own */
	g->wait_for = "UNFINISHED BATCH RENAME";
	ok = gate_launch(g) &&
	        bt_wait(g, "journal(s) from an earlier run", 8000);
	pr = &g->pr;
	product_snapshot(pr, "batchcrash_question");
	ev_check(e, "batchcrash_next_start_asks_about_the_journal", 0,
	        "UNFINISHED BATCH RENAME, 1 journal",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	Sleep(1500); /* time enough for an automatic recovery, if there were one */
	now = gate_tree_text(g->browse);
	ok = now && !strcmp(now, partial);
	free(now);
	now = NULL;
	ev_check(e, "batchcrash_nothing_is_recovered_unasked", 0,
	        "names still as the crash left them",
	        ok ? "observed" : "RECOVERED BY ITSELF", ok);
	if (!ok) {
		goto done;
	}
	ok = bt_wait(g, "[Y] Yes", 5000);
	pty_send(&pr->p, "n", 1);
	ok = ok && bt_wait(g, "NORMAL", 5000);
	Sleep(400);
	now = gate_tree_text(g->browse);
	ok = ok && now && !strcmp(now, partial) && 1 == bt_journals(g, NULL);
	free(now);
	now = NULL;
	ev_check(e, "batchcrash_no_leaves_everything", 0,
	        "names and the journal untouched", ok ? "observed" : "CHANGED",
	        ok);
	ok = ok && gate_quit(g);
	if (!ok) {
		goto done;
	}
	/* asked again, and this time yes */
	g->wait_for = "UNFINISHED BATCH RENAME";
	ok = gate_launch(g) && bt_wait(g, "[Y] Yes", 8000);
	pr = &g->pr;
	pty_send(&pr->p, "y", 1);
	ok = ok && bt_wait(g, "Recovered:", 120000);
	product_snapshot(pr, "batchcrash_recovered");
	ev_check(e, "batchcrash_recovery_is_reported", 0,
	        "Recovered: 1 journal(s) ... taken back",
	        ok ? "observed" : "missing", ok);
	ok = ok && NULL != strstr(pr->screen, "1 were taken back");
	now = gate_tree_text(g->browse);
	ok = ok && now && !strcmp(now, before);
	free(now);
	now = NULL;
	ev_check(e, "batchcrash_every_name_is_back", 0,
	        "names, sizes and contents as before the batch",
	        ok ? "unchanged" : "CHANGED", ok);
	ok = ok && 1 == bt_journals(g, "\nE ROLLED_BACK*");
	pty_send(&pr->p, "\x1b", 1);
	ok = ok && bt_wait(g, "NORMAL", 8000) && gate_quit(g);
	if (!ok) {
		goto done;
	}
	/* and now nothing is asked */
	g->wait_for = "doc.txt";
	ok = gate_launch(g) &&
	        NULL == strstr(g->pr.screen, "UNFINISHED BATCH RENAME");
	pr = &g->pr;
	ev_check(e, "batchcrash_nothing_asked_afterwards", 0,
	        "the folder, no question", ok ? "observed" : "ASKED AGAIN", ok);
	ok = ok && gate_quit(g);
done:
	free(before);
	free(partial);
	free(now);
	return gate_first_run_end(g, e, ok, fails);
}

static int scn_batch(Ev *e)
{
	bool good = batch_rename_case(e);

	good = batch_refused_case(e) && good;
	good = batch_cancel_case(e) && good;
	good = batch_crash_case(e) && good;
	if (!good || e->fails) {
		return 1;
	}
	return g_gate_skips ? 77 : 0;
}
