/*
 * Who has the file open (L), through the real product (DR-DRAFT-024). A harmless
 * helper program (tests/lock_helper.exe, started here, never a program of the
 * user's) holds a file without sharing, so piero's own rename and copy fail with
 * "in use". Then, in piero.exe: L names the helper (its PID is checked against the
 * one the host started), a close is requested and the lock is gone, and the
 * operation is put back and succeeds; for a helper that refuses to close, the
 * refusal is reported and nothing else happens, cancelling the two questions of
 * "force terminate" leaves it running, and answering both ends that helper only.
 * Source and destination files are checked by SHA-256.
 *
 * Included by p0_conpty_host.c after p0_state_gate.c, whose helpers it uses.
 */

static wchar_t g_lock_helper[MAX_PATH * 2];

typedef struct
{
	HANDLE proc;
	DWORD pid;
	wchar_t file[1100], ready[1100];
} LkHelper;

static bool lk_start(LkHelper *h,
        Gate *g,
        const wchar_t *mode,
        const wchar_t *name,
        const char *content,
        char *sha)
{
	wchar_t cmd[3300];
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;

	memset(h, 0, sizeof *h);
	if (!g_lock_helper[0]) {
		return false;
	}
	swprintf(h->file, 1100, L"%ls\\%ls", g->browse, name);
	swprintf(h->ready, 1100, L"%ls\\helper-%ls.ready", g->area, name);
	/* the bytes are hashed before the helper takes the file (it shares nothing) */
	if (!gate_write(h->file, content) || !p0_sha256_file(h->file, sha)) {
		return false;
	}
	DeleteFileW(h->ready);
	swprintf(cmd, 3300, L"\"%ls\" %ls \"%ls\" \"%ls\"", g_lock_helper, mode,
	        h->file, h->ready);
	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
	            NULL, NULL, &si, &pi)) {
		return false;
	}
	CloseHandle(pi.hThread);
	h->proc = pi.hProcess;
	h->pid = pi.dwProcessId;
	for (int i = 0; i < 200; i++) {
		if (INVALID_FILE_ATTRIBUTES != GetFileAttributesW(h->ready)) {
			return true;
		}
		Sleep(50);
	}
	return false;
}

static bool lk_alive(const LkHelper *h)
{
	return h->proc && WAIT_TIMEOUT == WaitForSingleObject(h->proc, 0);
}

static void lk_stop(LkHelper *h)
{
	if (h->proc) {
		TerminateProcess(h->proc,
		        0); /* only ever the helper this host started */
		WaitForSingleObject(h->proc, 5000);
		CloseHandle(h->proc);
		h->proc = NULL;
	}
}

static bool lk_wait(Gate *g, const char *text, const char *name, DWORD ms)
{
	Product *pr = &g->pr;
	bool ok = product_wait_ms(
	        &pr->p, text, pr->screen, sizeof pr->screen, ms);
	char label[96];

	snprintf(label, sizeof label, "%s_%s", g->tag, name);
	ev_check(pr->e, label, 0, text, ok ? "seen" : "MISSING", ok);
	return ok;
}

/* The question's guard (300 ms) must pass before its answer counts. */
static void lk_after_guard(void)
{
	Sleep(700);
}

static bool lk_file_is(const wchar_t *path, const char *sha)
{
	char now[P0_HASH_HEX];

	return p0_sha256_file(path, now) && !strcmp(now, sha);
}

static bool lock_rename_case(Ev *e)
{
	Gate *g = calloc(1, sizeof *g);
	Product *pr;
	LkHelper h;
	bool ok = false;
	int fails = e->fails;
	char sha[P0_HASH_HEX] = "", pidtext[24];
	wchar_t renamed[1100];

	if (!g || !gate_wanted("lock_rename")) {
		free(g);
		return true;
	}
	memset(&h, 0, sizeof h);
	if (!gate_open(g, e, "lock_rename") || !gate_launch(g)) {
		goto done;
	}
	pr = &g->pr;
	swprintf(renamed, 1100, L"%ls\\renamed.txt", g->browse);
	ok = lk_start(
	        &h, g, L"close", L"held.txt", "HELD_FILE_MARKER\r\n", sha);
	ev_check(e, "lock_rename_helper_holds_the_file", 0,
	        "helper started and ready", ok ? "ready" : "NOT started", ok);
	if (!ok) {
		goto done;
	}
	pty_send(&pr->p, "r", 1);
	ok = lk_wait(g, "held.txt", "rename_listing_shows_the_file", 8000);
	/* doc.txt, held.txt, note.txt: one down is held.txt */
	pty_send(&pr->p, "j", 1);
	Sleep(200);
	pty_send(&pr->p, "\x1bOQ", 3);
	ok = ok && lk_wait(g, "Rename: Enter renames", "rename_form", 8000);
	pty_send(&pr->p, "renamed\r", 8);
	ok = ok &&
	        lk_wait(g, "In use by another program", "failure_says_in_use",
	                8000);
	ok = ok && INVALID_FILE_ATTRIBUTES == GetFileAttributesW(renamed) &&
	        INVALID_FILE_ATTRIBUTES != GetFileAttributesW(h.file);
	ev_check(e, "lock_rename_nothing_changed", 0,
	        "held.txt still there, unchanged", ok ? "unchanged" : "CHANGED",
	        ok);
	if (!ok) {
		goto done;
	}
	/* L: the helper is named, with its own PID (the one this host started) */
	pty_send(&pr->p, "L", 1);
	snprintf(pidtext, sizeof pidtext, "PID %lu", (unsigned long)h.pid);
	ok = lk_wait(g, "File in use", "window_opens", 15000) &&
	        lk_wait(g, pidtext, "window_names_the_helper_by_pid", 15000);
	ok = ok && lk_wait(g, "Started:", "window_shows_the_start_time", 3000);
	ok = ok && lk_wait(g, "[T] Retry", "window_offers_retry", 3000);
	product_snapshot(pr, "lock_rename_window");
	if (!ok) {
		goto done;
	}
	/* Esc of the question: nothing is asked of the helper */
	pty_send(&pr->p, "c", 1);
	ok = lk_wait(g, "Ask this program to close", "close_question", 5000);
	lk_after_guard();
	pty_send(&pr->p, "\x1b", 1);
	Sleep(800);
	ok = ok && lk_alive(&h);
	ev_check(e, "lock_rename_cancelled_question_asks_nothing", 0,
	        "helper still running", ok ? "running" : "ENDED", ok);
	if (!ok) {
		goto done;
	}
	/* the real request: the helper closes, the lock goes, the screen says so */
	pty_send(&pr->p, "L", 1);
	ok = lk_wait(g, pidtext, "window_again", 15000);
	pty_send(&pr->p, "c", 1);
	ok = ok &&
	        lk_wait(g, "Ask this program to close", "close_question_again",
	                5000);
	lk_after_guard();
	pty_send(&pr->p, "y", 1);
	ok = ok && lk_wait(g, "has closed", "close_is_verified", 15000);
	ok = ok && WAIT_OBJECT_0 == WaitForSingleObject(h.proc, 5000);
	ev_check(e, "lock_rename_helper_really_exited", 0, "process gone",
	        ok ? "gone" : "STILL RUNNING", ok);
	ok = ok &&
	        lk_wait(g, "No program was found", "lock_is_gone_on_look_again",
	                15000);
	if (!ok) {
		goto done;
	}
	/* the folder is listed again after the helper let go (the watch); a form opened
	 * before that is refused as "listing changed", which is safe but not the test */
	Sleep(2500);
	/* T: the rename prompt is back with the name that was asked for; the
	 * rename itself runs only on Enter, with its ordinary checks */
	pty_send(&pr->p, "t", 1);
	ok = lk_wait(g, "Rename: Enter renames",
	        "retry_opens_the_ordinary_prompt", 8000);
	ok = ok && INVALID_FILE_ATTRIBUTES == GetFileAttributesW(renamed);
	ev_check(e, "lock_rename_retry_does_nothing_by_itself", 0,
	        "not renamed yet", ok ? "not renamed" : "RENAMED", ok);
	pty_send(&pr->p, "\r", 1);
	Sleep(800);
	ok = ok && INVALID_FILE_ATTRIBUTES != GetFileAttributesW(renamed) &&
	        INVALID_FILE_ATTRIBUTES == GetFileAttributesW(h.file) &&
	        lk_file_is(renamed, sha);
	ev_check(e, "lock_rename_retry_succeeds", 0,
	        "renamed.txt there, same bytes, held.txt gone",
	        ok ? "renamed" : "NOT renamed", ok);
	ok = ok && gate_quit(g);
done:
	lk_stop(&h);
	return gate_first_run_end(g, e, ok, fails);
}

static bool lock_copy_case(Ev *e)
{
	Gate *g = calloc(1, sizeof *g);
	Product *pr;
	LkHelper h;
	bool ok = false;
	int fails = e->fails;
	char sha[P0_HASH_HEX] = "", pidtext[24];
	wchar_t copy[1100];

	if (!g || !gate_wanted("lock_copy")) {
		free(g);
		return true;
	}
	memset(&h, 0, sizeof h);
	if (!gate_open(g, e, "lock_copy") || !gate_launch(g)) {
		goto done;
	}
	pr = &g->pr;
	swprintf(copy, 1100, L"%ls\\held.txt", g->dest);
	ok = lk_start(&h, g, L"ignore", L"held.txt", "COPY_ME_MARKER\r\n", sha);
	ev_check(e, "lock_copy_helper_holds_the_file", 0,
	        "helper started and ready", ok ? "ready" : "NOT started", ok);
	if (!ok) {
		goto done;
	}
	pty_send(&pr->p, "r", 1);
	ok = lk_wait(g, "held.txt", "copy_listing_shows_the_file", 8000);
	pty_send(&pr->p, "j", 1);
	Sleep(200);
	pty_send(&pr->p, "y", 1);
	Sleep(300);
	ok = ok && gate_goto(g, g->dest, "lock_copy_to_dest");
	pty_send(&pr->p, "p", 1);
	/* the copy cannot read the file: the report says it may be in use */
	ok = ok &&
	        lk_wait(g, "Press L to see which",
	                "copy_report_hints_at_a_lock", 30000);
	ok = ok && INVALID_FILE_ATTRIBUTES == GetFileAttributesW(copy);
	ev_check(e, "lock_copy_nothing_was_copied", 0,
	        "no copy at the destination", ok ? "none" : "A COPY EXISTS",
	        ok);
	if (!ok) {
		goto done;
	}
	pty_send(&pr->p, "L", 1);
	snprintf(pidtext, sizeof pidtext, "PID %lu", (unsigned long)h.pid);
	ok = lk_wait(g, pidtext, "copy_window_names_the_helper", 15000);
	ok = ok &&
	        lk_wait(g, "Failed operation: copy",
	                "copy_window_says_what_failed", 3000);
	if (!ok) {
		goto done;
	}
	/* a close that is refused is reported, and nothing else is done */
	pty_send(&pr->p, "c", 1);
	ok = lk_wait(
	        g, "Ask this program to close", "copy_close_question", 5000);
	lk_after_guard();
	pty_send(&pr->p, "y", 1);
	ok = ok &&
	        lk_wait(g, "still running", "refused_close_is_reported", 20000);
	ok = ok && lk_alive(&h);
	/* the window looks again by itself after an answer; wait for the list */
	ok = ok &&
	        lk_wait(g, "Started:", "list_is_back_after_the_refusal", 15000);
	ev_check(e, "lock_copy_refused_close_leaves_it_running", 0,
	        "helper alive", ok ? "alive" : "ENDED", ok);
	if (!ok) {
		goto done;
	}
	/* force terminate: the first question, cancelled */
	pty_send(&pr->p, "k", 1);
	ok = lk_wait(g, "FORCE TERMINATE", "end_first_question", 5000) &&
	        lk_wait(g, "WILL BE LOST", "end_warns_about_unsaved_data",
	                3000);
	product_snapshot(pr, "lock_end_first_question");
	lk_after_guard();
	pty_send(&pr->p, "\x1b", 1);
	Sleep(800);
	ok = ok && lk_alive(&h);
	ev_check(e, "lock_copy_cancel_at_first_question_ends_nothing", 0,
	        "helper alive", ok ? "alive" : "ENDED", ok);
	/* the second question, cancelled */
	pty_send(&pr->p, "L", 1);
	ok = ok && lk_wait(g, pidtext, "copy_window_again", 15000);
	pty_send(&pr->p, "k", 1);
	ok = ok &&
	        lk_wait(g, "FORCE TERMINATE", "end_first_question_again", 5000);
	lk_after_guard();
	pty_send(&pr->p, "y", 1);
	ok = ok && lk_wait(g, "LAST CHANCE", "end_second_question", 5000);
	product_snapshot(pr, "lock_end_second_question");
	lk_after_guard();
	pty_send(&pr->p, "\x1b", 1);
	Sleep(800);
	ok = ok && lk_alive(&h);
	ev_check(e, "lock_copy_cancel_at_second_question_ends_nothing", 0,
	        "helper alive", ok ? "alive" : "ENDED", ok);
	if (!ok) {
		goto done;
	}
	/* both answered: that helper, and only it, is ended and seen to be gone */
	pty_send(&pr->p, "L", 1);
	ok = lk_wait(g, pidtext, "copy_window_third", 15000);
	pty_send(&pr->p, "k", 1);
	ok = ok &&
	        lk_wait(g, "FORCE TERMINATE", "end_first_question_third", 5000);
	lk_after_guard();
	pty_send(&pr->p, "y", 1);
	ok = ok && lk_wait(g, "LAST CHANCE", "end_second_question_third", 5000);
	lk_after_guard();
	pty_send(&pr->p, "k", 1);
	ok = ok && lk_wait(g, "was ended", "end_is_verified", 15000);
	ok = ok && WAIT_OBJECT_0 == WaitForSingleObject(h.proc, 5000);
	ev_check(e, "lock_copy_helper_really_ended", 0, "process gone",
	        ok ? "gone" : "STILL RUNNING", ok);
	ok = ok &&
	        lk_wait(g, "No program was found", "lock_gone_after_end",
	                15000);
	if (!ok) {
		goto done;
	}
	/* T puts the copy back at the paste: nothing runs until p, which checks
	 * everything again, and the result is a faithful copy */
	pty_send(&pr->p, "t", 1);
	ok = lk_wait(g, "ready again", "retry_stages_the_copy", 8000);
	Sleep(500);
	ok = ok && INVALID_FILE_ATTRIBUTES == GetFileAttributesW(copy);
	ev_check(e, "lock_copy_retry_starts_nothing", 0, "no copy yet",
	        ok ? "none" : "A COPY EXISTS", ok);
	pty_send(&pr->p, "p", 1);
	for (int i = 0; ok && i < 200 &&
	        INVALID_FILE_ATTRIBUTES == GetFileAttributesW(copy);
	        i++) {
		Sleep(50);
	}
	Sleep(500);
	ok = ok && lk_file_is(copy, sha) &&
	        lk_file_is(h.file, sha); /* the helper is gone */
	ev_check(e, "lock_copy_retry_copies_byte_for_byte", 0,
	        "copy and source: SHA-256 as before",
	        ok ? "equal" : "DIFFERENT", ok);
	ok = ok && gate_quit(g);
done:
	lk_stop(&h);
	return gate_first_run_end(g, e, ok, fails);
}

static int scn_lock(Ev *e)
{
	bool good = true;

	if (!g_lock_helper[0] ||
	        INVALID_FILE_ATTRIBUTES == GetFileAttributesW(g_lock_helper)) {
		fputs("lock scenario: --helper <lock_helper.exe> is required\n",
		        stderr);
		return 77;
	}
	good = lock_rename_case(e) && good;
	Sleep(3000); /* the owned drive is released a moment after the case ends */
	good = lock_copy_case(e) && good;
	if (!good || e->fails) {
		return 1;
	}
	return g_gate_skips ? 77 : 0;
}
