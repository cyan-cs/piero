/*
 * `piero --continue` through the real product (DR-DRAFT-018): what a run
 * saves is what the next run puts back (folder, cursor, scroll, selection),
 * with the files untouched; and the cases that must not break anything: no
 * session, a damaged one, a folder that is gone, a newer version's file, and
 * a start folder given together with --continue. Included by
 * p0_conpty_host.c after p0_dirsize.c, whose row helper it uses.
 */

#define SESSION_CONFIG_ON                                                      \
	"[meta]\r\nschema_version = 1\r\nonboarding_completed = true\r\n"      \
	"[updates]\r\nenabled = false\r\nconsent_recorded = true\r\n"          \
	"[session]\r\npersist = true\r\n"

#define SESSION_CONFIG_OFF                                                     \
	"[meta]\r\nschema_version = 1\r\nonboarding_completed = true\r\n"      \
	"[updates]\r\nenabled = false\r\nconsent_recorded = true\r\n"

#define SESSION_FILES 60

/* A slot file's text for a session in `dir` (ASCII paths: the lab's). */
static bool session_record(const wchar_t *dir, char *out, size_t cap)
{
	char body[2400];
	size_t n = 0;

	n += (size_t)snprintf(body, sizeof body, "dir ");
	for (const wchar_t *p = dir; *p; p++) {
		if (*p >= 0x80 || n + 4 >= sizeof body) {
			return false;
		}
		if (L'\\' == *p) {
			body[n++] = '\\';
		}
		body[n++] = (char)*p;
	}
	body[n++] = '\n';
	body[n] = '\0';
	return (size_t)snprintf(out, cap,
	               "piero-session 1\nseq 1\nsize %zu\n--\n%s", n,
	               body) < cap;
}

/* Puts `text` in the state folder as `file`, in a folder made the way the
 * program makes its own (the program refuses one that is not private). */
static bool session_seed(Gate *g, const wchar_t *file, const char *text)
{
	wchar_t rel[64], path[1100];

	swprintf(rel, 64, L"piero\\%ls", file);
	return gate_mkdir_private(g, L"piero") && gate_path(g, rel, path) &&
	        gate_write(path, text);
}

/* The next start is `piero --continue`, given no folder. */
static void session_start_continue(Gate *g, const char *wait_for)
{
	g->extra_args = L"--continue";
	g->no_start_path = true;
	g->wait_for = wait_for;
}

static char *session_file_hash(Gate *g, const wchar_t *file)
{
	wchar_t rel[64], path[1100];
	GateSnap s;
	char *text;

	swprintf(rel, 64, L"piero\\%ls", file);
	if (!gate_path(g, rel, path) || !gate_snap(path, &s)) {
		return _strdup("absent");
	}
	text = _strdup(s.sha);
	return text;
}

static bool session_round_trip(Ev *e)
{
	Gate *g = calloc(1, sizeof *g);
	Product *pr;
	char name[96];
	char *tree = NULL, *tree_after = NULL;
	bool ok = false;
	int fails = e->fails;

	if (!g || !gate_wanted("session_round_trip")) {
		free(g);
		return true;
	}
	if (!gate_open(g, e, "session_round_trip")) {
		goto done;
	}
	pr = &g->pr;
	for (int i = 1; i <= SESSION_FILES; i++) {
		wchar_t rel[64], path[1100];

		swprintf(rel, 64, L"browse\\file%02d.txt", i);
		if (!gate_path(g, rel, path) || !gate_write(path, "x")) {
			goto done;
		}
	}
	if (!gate_snap(g->doc, &g->doc0) || !gate_snap(g->note, &g->note0) ||
	        !gate_write(g->cfg, SESSION_CONFIG_ON) || !gate_launch(g)) {
		goto done;
	}
	tree = gate_tree_text(g->browse);
	/* 30 rows down: the list scrolls; one entry is marked (which moves the
	 * cursor on), then two more rows down */
	pty_send(&pr->p, "jjjjjjjjjjjjjjjjjjjjjjjjjjjjjj", 30);
	ok = dirsize_row(g, "> file30.txt", "", 8000);
	if (!ok) {
		ev_check(e, "session_run_one_cursor_moved", 0, "> file30.txt",
		        "missing", 0);
		goto done;
	}
	/* one key at a time, each seen, so that no read of the console holds
	 * several and is taken for a paste */
	pty_send(&pr->p, " ", 1);
	ok = dirsize_row(g, "*file30.txt", "", 8000);
	pty_send(&pr->p, "j", 1);
	ok = ok && dirsize_row(g, "> file32.txt", "", 8000);
	pty_send(&pr->p, "j", 1);
	ok = ok && dirsize_row(g, "> file33.txt", "", 8000);
	product_snapshot(pr, "session_run_one_before_quit");
	ev_check(e, "session_run_one_state", 0,
	        "cursor on file33.txt, file30.txt marked",
	        ok ? "observed" : "missing", ok);
	if (!ok || !gate_quit(g)) {
		ok = false;
		goto done;
	}
	/* the second run asks for it back, and is given no folder */
	g->extra_args = NULL;
	session_start_continue(g, "Session restored");
	ok = gate_launch(g);
	product_snapshot(pr, "session_run_two_restored");
	ev_check(e, "session_continue_is_announced", 0, "Session restored",
	        ok ? "observed" : "missing", ok);
	ok = ok && dirsize_row(g, "> file33.txt", "", 8000);
	ev_check(e, "session_cursor_is_back", 0, "> file33.txt",
	        ok ? "observed" : "missing", ok);
	ok = ok && dirsize_row(g, "*file30.txt", "", 3000);
	ev_check(e, "session_selection_is_back", 0, "*file30.txt marked",
	        ok ? "observed" : "missing", ok);
	/* the list is scrolled as it was: the first entry is not on screen */
	{
		pty_snapshot(&pr->p, pr->screen, sizeof pr->screen);
		ok = ok && NULL == strstr(pr->screen, "doc.txt") &&
		        NULL != strstr(pr->screen, "file33.txt");
		ev_check(e, "session_scroll_is_back", 0,
		        "doc.txt scrolled off, file33.txt shown",
		        ok ? "observed" : "not scrolled", ok);
	}
	if (!ok) {
		goto done;
	}
	/* nothing was written to the folder that was browsed */
	tree_after = gate_tree_text(g->browse);
	{
		GateSnap d, n;

		ok = tree && tree_after && !strcmp(tree, tree_after) &&
		        gate_snap(g->doc, &d) && gate_same(&d, &g->doc0) &&
		        gate_snap(g->note, &n) && gate_same(&n, &g->note0);
	}
	snprintf(name, sizeof name, "session_changes_no_file");
	ev_check(e, name, 0, "tree, SHA-256 and file id as before",
	        ok ? "unchanged" : "CHANGED", ok);
	ok = ok && gate_quit(g);
done:
	free(tree);
	free(tree_after);
	return gate_first_run_end(g, e, ok, fails);
}

/* The folder the program starts in is the folder that was saved: the listing
 * that arrives is the same folder's, and the saved cursor must still win over
 * the cursor the program had there (it is the common way to use --continue). */
static bool session_same_folder(Ev *e)
{
	Gate *g = calloc(1, sizeof *g);
	Product *pr;
	char config[1600];
	bool ok = false;
	int fails = e->fails;

	if (!g || !gate_wanted("session_same_folder")) {
		free(g);
		return true;
	}
	if (!gate_open(g, e, "session_same_folder")) {
		goto done;
	}
	pr = &g->pr;
	for (int i = 1; i <= SESSION_FILES; i++) {
		wchar_t rel[64], path[1100];

		swprintf(rel, 64, L"browse\\file%02d.txt", i);
		if (!gate_path(g, rel, path) || !gate_write(path, "x")) {
			goto done;
		}
	}
	if (!gate_write(g->cfg, SESSION_CONFIG_ON) || !gate_launch(g)) {
		goto done;
	}
	pty_send(&pr->p, "jjjjjjjjjjjjjjjjjjjjjjjjjjjjjj", 30);
	ok = dirsize_row(g, "> file30.txt", "", 8000);
	if (!ok || !gate_quit(g)) {
		ok = false;
		goto done;
	}
	/* the second start opens this very folder by itself (the setting), and
	 * is asked to continue */
	snprintf(config, sizeof config,
	        "%s[behavior]\r\nstart_directory = %ls\r\n", SESSION_CONFIG_ON,
	        g->browse);
	ok = gate_write(g->cfg, config);
	session_start_continue(g, "Session restored");
	ok = ok && gate_launch(g);
	ev_check(e, "session_same_folder_is_announced", 0, "Session restored",
	        ok ? "observed" : "missing", ok);
	ok = ok && dirsize_row(g, "> file30.txt", "", 8000);
	product_snapshot(pr, "session_same_folder_restored");
	ev_check(e, "session_same_folder_cursor_is_back", 0, "> file30.txt",
	        ok ? "observed" : "the program's own cursor won", ok);
	ok = ok && gate_quit(g);
done:
	return gate_first_run_end(g, e, ok, fails);
}

/* --forget-session empties what a run saved, and the next --continue finds
 * nothing; it does not run while another piero holds the right to write. */
static bool session_forget(Ev *e)
{
	Gate *g = calloc(1, sizeof *g);
	Product *pr;
	char out[512];
	bool ok = false;
	int fails = e->fails;
	wchar_t a[1100];

	if (!g || !gate_wanted("session_forget")) {
		free(g);
		return true;
	}
	if (!gate_open(g, e, "session_forget") ||
	        !gate_write(g->cfg, SESSION_CONFIG_ON) || !gate_launch(g)) {
		goto done;
	}
	pr = &g->pr;
	pty_send(&pr->p, "j", 1);
	ok = dirsize_row(g, "> note.txt", "", 8000) && gate_quit(g);
	if (!ok) {
		goto done;
	}
	/* a session file exists now, with something in it */
	ok = gate_path(g, L"piero\\session.a", a) &&
	        (INVALID_FILE_ATTRIBUTES != GetFileAttributesW(a));
	ev_check(e, "session_forget_a_session_was_saved", 0, "session.a exists",
	        ok ? "exists" : "missing", ok);
	ok = ok &&
	        0 == gate_cli(g, L"--forget-session", NULL, out, sizeof out) &&
	        NULL != strstr(out, "is empty now");
	ev_check(e, "session_forget_command", 0, "exit 0, the session is empty",
	        ok ? "emptied" : "failed", ok);
	if (!ok) {
		goto done;
	}
	{
		HANDLE h = CreateFileW(a, GENERIC_READ,
		        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
		        FILE_ATTRIBUTE_NORMAL, NULL);
		LARGE_INTEGER size = {0};

		ok = INVALID_HANDLE_VALUE != h && GetFileSizeEx(h, &size) &&
		        0 == size.QuadPart;
		if (INVALID_HANDLE_VALUE != h) {
			CloseHandle(h);
		}
		ev_check(e, "session_forget_content_is_gone", 0,
		        "session.a is empty (the file stays)",
		        ok ? "empty" : "NOT EMPTY", ok);
	}
	session_start_continue(g, "No saved session");
	ok = ok && gate_launch(g);
	ev_check(e, "session_forget_next_continue_finds_nothing", 0,
	        "No saved session", ok ? "observed" : "missing", ok);
	ok = ok && gate_quit(g);
done:
	return gate_first_run_end(g, e, ok, fails);
}

/* The text of the slot a reader would trust (the higher "seq"), or false. */
static bool session_newest(Gate *g, char *text, size_t cap)
{
	static const wchar_t *const slot[2] = {
	        L"piero\\session.a", L"piero\\session.b"};
	unsigned long long best = 0;
	bool found = false;

	for (int i = 0; i < 2; i++) {
		wchar_t path[1100];
		char one[4096];
		const char *at;
		unsigned long long seq;

		if (!gate_path(g, slot[i], path) ||
		        !read_text_file(path, one, sizeof one) ||
		        !(at = strstr(one, "\nseq "))) {
			continue;
		}
		seq = strtoull(at + 5, NULL, 10);
		if (!found || seq > best) {
			best = seq;
			found = true;
			snprintf(text, cap, "%s", one);
		}
	}
	return found;
}

/* True when the newest saved session says the editor holds unsaved text: the
 * "1" after the four numbers of the edit record. */
static bool session_says_modified(Gate *g)
{
	char text[4096];
	const char *at;
	char *end;

	if (!session_newest(g, text, sizeof text) ||
	        !(at = strstr(text, "\nedit "))) {
		return false;
	}
	at += 6;
	for (int k = 0; k < 4; k++) {
		strtoull(at, &end, 10);
		at = end;
	}
	return 1 == strtoull(at, &end, 10);
}

/* True when the newest saved session has an edit record at all. */
static bool session_has_editor(Gate *g)
{
	char text[4096];

	return session_newest(g, text, sizeof text) && strstr(text, "\nedit ");
}

/* The editor is open on a file, text is typed and not saved, the program is
 * killed; `--continue` opens the editor again where the cursor was, asks about
 * the draft as it always does, and the file on the disk was never touched. */
static bool session_editor(Ev *e)
{
	Gate *g = calloc(1, sizeof *g);
	Product *pr;
	bool ok = false;
	int fails = e->fails;
	double t0;

	if (!g || !gate_wanted("session_editor")) {
		free(g);
		return true;
	}
	if (!gate_open(g, e, "session_editor")) {
		goto done;
	}
	pr = &g->pr;
	if (!gate_snap(g->doc, &g->doc0) || !gate_snap(g->note, &g->note0) ||
	        !gate_write(g->cfg, SESSION_CONFIG_ON) || !gate_launch(g)) {
		goto done;
	}
	/* the editor opens doc.txt; the cursor goes to line 2, column 3; one
	 * character is typed there and the text is left unsaved */
	pty_send(&pr->p, "e", 1);
	ok = product_wait_ms(
	        &pr->p, "Ln 1/3", pr->screen, sizeof pr->screen, 8000);
	pty_send(&pr->p, "\x1b[B", 3);
	ok = ok &&
	        product_wait_ms(
	                &pr->p, "Ln 2/3", pr->screen, sizeof pr->screen, 8000);
	pty_send(&pr->p, "\x1b[C", 3);
	ok = ok &&
	        product_wait_ms(
	                &pr->p, "Col 2", pr->screen, sizeof pr->screen, 8000);
	pty_send(&pr->p, "\x1b[C", 3);
	ok = ok &&
	        product_wait_ms(
	                &pr->p, "Col 3", pr->screen, sizeof pr->screen, 8000);
	pty_send(&pr->p, "Q", 1);
	ok = ok &&
	        product_wait_ms(&pr->p, "[Modified]", pr->screen,
	                sizeof pr->screen, 8000);
	ok = ok &&
	        product_wait_ms(
	                &pr->p, "Col 4", pr->screen, sizeof pr->screen, 8000);
	product_snapshot(pr, "session_editor_before_kill");
	ev_check(e, "session_editor_unsaved_text_typed", 0,
	        "Ln 2, Col 4, [Modified]", ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	/* the checkpoint comes a second after the change, and only says
	 * "modified" once the draft behind it was written */
	t0 = now_ms();
	ok = false;
	while (now_ms() - t0 < 12000.0 && !(ok = session_says_modified(g))) {
		Sleep(100);
	}
	ev_check(e, "session_editor_checkpoint_says_modified", 0,
	        "an edit record with modified = 1", ok ? "observed" : "missing",
	        ok);
	if (!ok) {
		goto done;
	}
	/* the program is killed: no exit, no last write */
	TerminateProcess(pr->p.pi.hProcess, 3);
	WaitForSingleObject(pr->p.pi.hProcess, 5000);
	pty_finish(&pr->p);
	pr->started = false;
	/* the next start asks for the session back */
	g->extra_args = NULL;
	session_start_continue(g, "kept from last time");
	ok = gate_launch(g);
	product_snapshot(pr, "session_editor_after_continue");
	ev_check(e, "session_editor_asks_about_the_draft", 0,
	        "the editor's own question about the unsaved text",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	/* the question is answered by the person: restore, and the cursor is
	 * where it was, not at the end of the restored text */
	pty_send(&pr->p, "r", 1);
	ok = product_wait_ms(&pr->p, "Ln 2/3, Col 4", pr->screen,
	             sizeof pr->screen, 8000) &&
	        NULL != strstr(pr->screen, "Q");
	product_snapshot(pr, "session_editor_draft_restored");
	ev_check(e, "session_editor_text_and_cursor_are_back", 0,
	        "the draft's text with Q, at Ln 2, Col 4 (not the end of the "
	        "text)",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	/* leaving: the editor asks, the person discards; the file never changed */
	pty_send(&pr->p, "\x18", 1);
	ok = product_wait_ms(
	        &pr->p, "Save changes to", pr->screen, sizeof pr->screen, 8000);
	pty_send(&pr->p, "d", 1);
	ok = ok &&
	        product_wait_ms(
	                &pr->p, "NORMAL", pr->screen, sizeof pr->screen, 8000);
	if (ok) {
		GateSnap d, n;

		ok = gate_snap(g->doc, &d) && gate_same(&d, &g->doc0) &&
		        gate_snap(g->note, &n) && gate_same(&n, &g->note0);
		ev_check(e, "session_editor_changes_no_file", 0,
		        "SHA-256 and file id as before",
		        ok ? "unchanged" : "CHANGED", ok);
	}
	ok = ok && gate_quit(g);
	/* the killed program released its right to write by ending: the second
	 * one wrote, and its last state has no editor in it */
	if (ok) {
		ok = !session_has_editor(g);
		ev_check(e, "session_editor_second_run_wrote_its_last_state", 0,
		        "the newest session has no editor",
		        ok ? "observed" : "still the killed run's", ok);
	}
done:
	return gate_first_run_end(g, e, ok, fails);
}

/* The harness holds session.lock the way a running piero does; a piero started
 * now says it does not save, and writes nothing. */
static bool session_other_holds_the_lock(Ev *e)
{
	Gate *g = calloc(1, sizeof *g);
	HANDLE lock = INVALID_HANDLE_VALUE;
	wchar_t path[1100];
	bool ok = false;
	int fails = e->fails;

	if (!g || !gate_wanted("session_other_holds_the_lock")) {
		free(g);
		return true;
	}
	if (!gate_open(g, e, "session_other_holds_the_lock") ||
	        !gate_write(g->cfg, SESSION_CONFIG_ON) ||
	        !session_seed(g, L"session.lock", "")) {
		goto done;
	}
	ok = gate_path(g, L"piero\\session.lock", path);
	lock = ok ? CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
	                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)
	          : INVALID_HANDLE_VALUE;
	ok = INVALID_HANDLE_VALUE != lock;
	if (!ok) {
		goto done;
	}
	g->wait_for = "Another piero saves";
	ok = gate_launch(g);
	ev_check(e, "session_other_writer_is_told", 0,
	        "Another piero saves the session", ok ? "observed" : "missing",
	        ok);
	if (ok) {
		pty_send(&g->pr.p, "j", 1);
		ok = dirsize_row(g, "> note.txt", "", 8000);
	}
	ok = ok && gate_quit(g);
	if (ok) {
		wchar_t a[1100];

		ok = gate_path(g, L"piero\\session.a", a) &&
		        INVALID_FILE_ATTRIBUTES == GetFileAttributesW(a);
		ev_check(e, "session_other_writer_nothing_written", 0,
		        "no session file while another holds the lock",
		        ok ? "none" : "WRITTEN", ok);
	}
done:
	if (INVALID_HANDLE_VALUE != lock) {
		CloseHandle(lock);
	}
	return gate_first_run_end(g, e, ok, fails);
}

/* One start with `--continue` over a seeded state folder; `notice` must show,
 * the start is normal, and `file` (if any) is exactly as it was. */
static bool session_case(Ev *e,
        const char *tag,
        const wchar_t *file,
        const char *seed,
        const char *notice,
        bool seed_is_record_of_gone_folder)
{
	Gate *g = calloc(1, sizeof *g);
	char text[2600], *before = NULL, *after = NULL;
	bool ok = false;
	int fails = e->fails;
	char name[96];

	if (!g || !gate_wanted(tag)) {
		free(g);
		return true;
	}
	if (!gate_open(g, e, tag) || !gate_write(g->cfg, SESSION_CONFIG_OFF)) {
		goto done;
	}
	if (seed_is_record_of_gone_folder) {
		wchar_t gone[1100];

		if (!gate_path(g, L"browse\\gone\\deep", gone) ||
		        !session_record(gone, text, sizeof text)) {
			goto done;
		}
		seed = text;
	}
	if (file && !session_seed(g, file, seed)) {
		goto done;
	}
	before = file ? session_file_hash(g, file) : _strdup("none");
	session_start_continue(g, notice);
	ok = gate_launch(g);
	snprintf(name, sizeof name, "%s_notice", tag);
	product_snapshot(&g->pr, name);
	ev_check(e, name, 0, notice, ok ? "observed" : "missing", ok);
	if (ok && seed_is_record_of_gone_folder) {
		/* the nearest folder that exists is shown: the fixture's */
		ok = dirsize_row(g, "doc.txt", "", 8000);
		snprintf(name, sizeof name, "%s_shows_the_parent", tag);
		ev_check(e, name, 0, "browse folder listed",
		        ok ? "observed" : "missing", ok);
	}
	ok = ok && gate_quit(g);
	/* with saving off and nothing seeded, no session file comes to be */
	if (ok && !file) {
		static const wchar_t *const made[] = {L"piero\\session.a",
		        L"piero\\session.b", L"piero\\session.lock"};
		bool none = true;

		for (size_t i = 0; i < sizeof made / sizeof *made; i++) {
			wchar_t path[1100];

			if (gate_path(g, made[i], path) &&
			        INVALID_FILE_ATTRIBUTES !=
			                GetFileAttributesW(path)) {
				none = false;
			}
		}
		snprintf(name, sizeof name, "%s_wrote_nothing", tag);
		ev_check(e, name, 0, "no session file with persist off",
		        none ? "none" : "FILES WRITTEN", none);
		ok = none;
	}
	/* what was there is left exactly as it was */
	after = file ? session_file_hash(g, file) : _strdup("none");
	if (ok && file && !seed_is_record_of_gone_folder) {
		ok = before && after && !strcmp(before, after);
		snprintf(name, sizeof name, "%s_file_is_left_as_it_was", tag);
		ev_check(e, name, 0, "same SHA-256",
		        ok ? "unchanged" : "CHANGED", ok);
	}
done:
	free(before);
	free(after);
	return gate_first_run_end(g, e, ok, fails);
}

/* --continue together with a folder is an error, not a choice of one. */
static bool session_args_conflict(Ev *e)
{
	Gate *g = calloc(1, sizeof *g);
	char out[512];
	bool ok = false;
	int fails = e->fails;
	int code;

	if (!g || !gate_wanted("session_args_conflict")) {
		free(g);
		return true;
	}
	if (!gate_open(g, e, "session_args_conflict")) {
		goto done;
	}
	code = gate_cli(g, L"--continue C:\\", NULL, out, sizeof out);
	ok = 2 == code && NULL != strstr(out, "invalid arguments");
	ev_check(e, "session_continue_with_a_folder_is_refused", 0,
	        "exit 2, invalid arguments", ok ? "refused" : "ACCEPTED", ok);
done:
	return gate_first_run_end(g, e, ok, fails);
}

static int scn_session(Ev *e)
{
	bool good = true;

	good = session_round_trip(e) && good;
	good = session_same_folder(e) && good;
	good = session_forget(e) && good;
	good = session_editor(e) && good;
	good = session_other_holds_the_lock(e) && good;
	good = session_case(e, "session_none", NULL, NULL, "No saved session",
	               false) &&
	        good;
	good = session_case(e, "session_damaged", L"session.a",
	               "piero-session 1\nseq 1\nsize 99\n--\nnot a session",
	               "is damaged", false) &&
	        good;
	good = session_case(e, "session_newer_version", L"session.a",
	               "piero-session 9\nsomething newer\n", "newer piero",
	               false) &&
	        good;
	good = session_case(e, "session_folder_gone", L"session.a", NULL,
	               "nearest folder", true) &&
	        good;
	good = session_args_conflict(e) && good;
	if (!good || e->fails) {
		return 1;
	}
	return g_gate_skips ? 77 : 0;
}
