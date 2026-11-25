/*
 * Fuzzy search (`s`) through the real product (DR-DRAFT-023): the ten
 * situations of the request, on a small folder (the foreground path) and on a
 * folder of 4,500 names (the worker's path). Also measured and reported, not
 * judged against a made-up number: the delay from the last key to the results,
 * the CPU of an open search that waits, the handles and the private memory after
 * many searches. Included by p0_conpty_host.c after p0_watch.c, whose helpers
 * (and p0_dirsize.c's dirsize_row) it uses.
 */

#define FUZZY_CONFIG                                                           \
	"[meta]\r\nschema_version = 1\r\nonboarding_completed = true\r\n"      \
	"[updates]\r\nenabled = false\r\nconsent_recorded = true\r\n"

#define FUZZY_BIG 4500

static const wchar_t *const FUZZY_NAMES[] = {L"config.go", L"config_file.go",
        L"cache_file_generator.c", L"README.md", L"read_me.txt",
        L"HTTPServer.c", L"http_server.c", L"main.c", L"main_test.c",
        L"設定ファイル.txt", L"\xD842\xDFB7野家.txt"};

/* The first row of the screen that holds `name`, or -1. */
static int fuzzy_row_of(const char *screen, const char *name)
{
	int y = 0;
	const char *line = screen;

	while (*line) {
		const char *end = strchr(line, '\n');
		size_t n = end ? (size_t)(end - line) : strlen(line);
		char row[1024];

		if (n >= sizeof row) {
			n = sizeof row - 1;
		}
		memcpy(row, line, n);
		row[n] = '\0';
		if (strstr(row, name)) {
			return y;
		}
		y++;
		line = end ? end + 1 : line + n;
	}
	return -1;
}

/* Types `text` (UTF-8) one character at a time, as a person would. */
static void fuzzy_type(Gate *g, const char *text)
{
	for (const char *p = text; *p;) {
		size_t n = 1;

		while (0x80 == ((unsigned char)p[n] & 0xC0)) {
			n++;
		}
		pty_send(&g->pr.p, p, n);
		Sleep(25);
		p += n;
	}
}

/* How many SGR sequences of the stream from `from` on carry an underline
 * (parameter 4): the marked letters. */
static int fuzzy_underlines(Pty *p, size_t from)
{
	int n = 0;

	EnterCriticalSection(&p->lock);
	for (size_t i = from; i + 2 < p->raw_n; i++) {
		if (0x1B == p->raw[i] && '[' == p->raw[i + 1]) {
			size_t j = i + 2, start = j;
			bool under = false;

			while (j < p->raw_n && p->raw[j] != 'm' &&
			        (p->raw[j] == ';' ||
			                (p->raw[j] >= '0' &&
			                        p->raw[j] <= '9'))) {
				if (p->raw[j] == ';' || j + 1 == p->raw_n) {
					start = j + 1;
				} else if ('4' == p->raw[j] && (j == start) &&
				        (j + 1 < p->raw_n) &&
				        (p->raw[j + 1] == ';' ||
				                p->raw[j + 1] == 'm')) {
					under = true;
				}
				j++;
			}
			if (j < p->raw_n && 'm' == p->raw[j] && under) {
				n++;
			}
		}
	}
	LeaveCriticalSection(&p->lock);
	return n;
}

static size_t fuzzy_raw_mark(Pty *p)
{
	size_t n;

	EnterCriticalSection(&p->lock);
	n = p->raw_n;
	LeaveCriticalSection(&p->lock);
	return n;
}

static bool fuzzy_make(Gate *g, const wchar_t *rel)
{
	wchar_t path[1100];

	return gate_path(g, rel, path) && gate_write(path, "x");
}

static bool fuzzy_small_case(Ev *e)
{
	Gate *g = calloc(1, sizeof *g);
	Product *pr;
	bool ok = false;
	int fails = e->fails;
	char *before = NULL, *after = NULL;
	char seen[200];
	size_t mark;
	int r0, r1, r2;

	if (!g || !gate_wanted("fuzzy")) {
		free(g);
		return true;
	}
	if (!gate_open(g, e, "fuzzy") || !gate_write(g->cfg, FUZZY_CONFIG)) {
		goto done;
	}
	for (size_t i = 0; i < sizeof FUZZY_NAMES / sizeof *FUZZY_NAMES; i++) {
		wchar_t rel[128];

		swprintf(rel, 128, L"browse\\%ls", FUZZY_NAMES[i]);
		if (!fuzzy_make(g, rel)) {
			goto done;
		}
	}
	if (!gate_launch(g)) {
		goto done;
	}
	pr = &g->pr;
	before = gate_tree_text(g->browse);

	/* 1. s starts the search: the prompt and the mode */
	pty_send(&pr->p, "s", 1);
	ok = product_wait_ms(
	             &pr->p, "Search:", pr->screen, sizeof pr->screen, 8000) &&
	        NULL != strstr(pr->screen, "SEARCH");
	product_snapshot(pr, "fuzzy_started");
	ev_check(e, "fuzzy_s_starts_the_search", 0,
	        "Search: prompt, mode SEARCH", ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}

	/* 2. each letter updates the candidates, best first */
	mark = fuzzy_raw_mark(&pr->p);
	fuzzy_type(g, "cfg");
	ok = product_wait_ms(
	        &pr->p, "3 matches", pr->screen, sizeof pr->screen, 8000);
	r0 = fuzzy_row_of(pr->screen, "config.go");
	r1 = fuzzy_row_of(pr->screen, "config_file.go");
	r2 = fuzzy_row_of(pr->screen, "cache_file_generator.c");
	product_snapshot(pr, "fuzzy_cfg");
	snprintf(seen, sizeof seen, "rows %d, %d, %d", r0, r1, r2);
	ok = ok && r0 > 0 && r1 == r0 + 1 && r2 == r1 + 1 &&
	        fuzzy_row_of(pr->screen, "README.md") < 0 &&
	        fuzzy_row_of(pr->screen, "Search: cfg") > 0;
	ev_check(e, "fuzzy_results_update_as_typed", 0,
	        "config.go, config_file.go, cache_file_generator.c in order, "
	        "nothing else",
	        seen, ok);
	if (!ok) {
		goto done;
	}

	/* 3. the letters that matched are marked and the screen is not broken */
	{
		int marks = fuzzy_underlines(&pr->p, mark);

		snprintf(seen, sizeof seen, "%d underlined runs in the stream",
		        marks);
		ok = marks > 0 && fuzzy_row_of(pr->screen, "> config.go") > 0;
		ev_check(e, "fuzzy_matched_letters_are_marked", 0,
		        "underline attributes sent, the rows intact", seen, ok);
	}

	/* 4. the arrows choose */
	pty_send(&pr->p, "\x1b[B", 3);
	ok = product_wait_ms(&pr->p, "> config_file.go", pr->screen,
	        sizeof pr->screen, 5000);
	ev_check(e, "fuzzy_arrows_choose", 0, "> config_file.go",
	        ok ? "observed" : "missing", ok);
	pty_send(&pr->p, "\x1b[A", 3);
	ok = ok &&
	        product_wait_ms(&pr->p, "> config.go", pr->screen,
	                sizeof pr->screen, 5000);
	if (!ok) {
		goto done;
	}

	/* 5. Backspace searches again; nothing found is said */
	fuzzy_type(g, "x");
	ok = product_wait_ms(
	        &pr->p, "No matches", pr->screen, sizeof pr->screen, 5000);
	ev_check(e, "fuzzy_no_match_is_said", 0, "No matches",
	        ok ? "observed" : "missing", ok);
	pty_send(&pr->p, "\x7f", 1);
	ok = ok &&
	        product_wait_ms(&pr->p, "3 matches", pr->screen,
	                sizeof pr->screen, 5000);
	ev_check(e, "fuzzy_backspace_searches_again", 0, "3 matches again",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}

	/* 6. Enter goes to the chosen entry and keeps the cursor there */
	pty_send(&pr->p, "\x1b[B", 3);
	ok = product_wait_ms(&pr->p, "> config_file.go", pr->screen,
	        sizeof pr->screen, 5000);
	pty_send(&pr->p, "\r", 1);
	ok = ok &&
	        product_wait_ms(&pr->p, "NORMAL", pr->screen, sizeof pr->screen,
	                5000) &&
	        dirsize_row(g, "> config_file.go", "", 3000) &&
	        NULL != strstr(pr->screen, "README.md");
	product_snapshot(pr, "fuzzy_enter");
	ev_check(e, "fuzzy_enter_goes_to_the_entry", 0,
	        "NORMAL, the whole list back, the cursor on config_file.go",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}

	/* 7. Esc puts the cursor back where it was */
	pty_send(&pr->p, "s", 1);
	ok = product_wait_ms(
	        &pr->p, "Search:", pr->screen, sizeof pr->screen, 5000);
	fuzzy_type(g, "main");
	ok = ok &&
	        product_wait_ms(&pr->p, "> main.c", pr->screen,
	                sizeof pr->screen, 5000);
	pty_send(&pr->p, "\x1b[B", 3);
	ok = ok &&
	        product_wait_ms(&pr->p, "> main_test.c", pr->screen,
	                sizeof pr->screen, 5000);
	pty_send(&pr->p, "\x1b", 1);
	ok = ok &&
	        product_wait_ms(&pr->p, "NORMAL", pr->screen, sizeof pr->screen,
	                5000) &&
	        dirsize_row(g, "> config_file.go", "", 3000);
	product_snapshot(pr, "fuzzy_esc");
	ev_check(e, "fuzzy_esc_restores_the_cursor", 0,
	        "cursor back on config_file.go", ok ? "observed" : "missing",
	        ok);
	if (!ok) {
		goto done;
	}

	/* 8. Japanese names, and a character outside the BMP */
	pty_send(&pr->p, "s", 1);
	ok = product_wait_ms(
	        &pr->p, "Search:", pr->screen, sizeof pr->screen, 5000);
	fuzzy_type(g, "設定");
	ok = ok &&
	        product_wait_ms(&pr->p, "> 設定ファイル.txt", pr->screen,
	                sizeof pr->screen, 5000);
	ev_check(e, "fuzzy_finds_japanese_names", 0, "> 設定ファイル.txt",
	        ok ? "observed" : "missing", ok);
	pty_send(&pr->p, "\x1b", 1);
	ok = ok &&
	        product_wait_ms(
	                &pr->p, "NORMAL", pr->screen, sizeof pr->screen, 5000);
	pty_send(&pr->p, "s", 1);
	ok = ok &&
	        product_wait_ms(
	                &pr->p, "Search:", pr->screen, sizeof pr->screen, 5000);
	fuzzy_type(g, "野家");
	ok = ok &&
	        product_wait_ms(&pr->p,
	                "> \xF0\xA0\xAE\xB7"
	                "野家.txt",
	                pr->screen, sizeof pr->screen, 5000);
	product_snapshot(pr, "fuzzy_astral");
	ev_check(e, "fuzzy_finds_a_name_beyond_the_bmp", 0,
	        "the pair and 野家 drawn whole", ok ? "observed" : "missing",
	        ok);
	if (!ok) {
		goto done;
	}
	/* kana: Hiragana typed finds Katakana in the name */
	pty_send(&pr->p, "\x7f\x7f", 2);
	fuzzy_type(g, "ふぁ");
	ok = product_wait_ms(&pr->p, "設定ファイル.txt", pr->screen,
	        sizeof pr->screen, 5000);
	ev_check(e, "fuzzy_hiragana_finds_katakana", 0, "設定ファイル.txt",
	        ok ? "observed" : "missing", ok);
	pty_send(&pr->p, "\x1b", 1);
	ok = ok &&
	        product_wait_ms(
	                &pr->p, "NORMAL", pr->screen, sizeof pr->screen, 5000);
	if (!ok) {
		goto done;
	}

	/* 9. another program changes the folder while the search is open */
	pty_send(&pr->p, "s", 1);
	ok = product_wait_ms(
	        &pr->p, "Search:", pr->screen, sizeof pr->screen, 5000);
	fuzzy_type(g, "cfg");
	ok = ok &&
	        product_wait_ms(&pr->p, "3 matches", pr->screen,
	                sizeof pr->screen, 5000) &&
	        fuzzy_make(g, L"browse\\cfg_new.go") &&
	        product_wait_ms(&pr->p, "cfg_new.go", pr->screen,
	                sizeof pr->screen, 10000) &&
	        NULL != strstr(pr->screen, "4 matches") &&
	        NULL != strstr(pr->screen, "Search: cfg") &&
	        NULL != strstr(pr->screen, "SEARCH");
	product_snapshot(pr, "fuzzy_list_changed");
	ev_check(e, "fuzzy_list_change_researches_safely", 0,
	        "cfg_new.go appears, 4 matches, still searching",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	{
		wchar_t p[1100];

		ok = gate_path(g, L"browse\\cfg_new.go", p) && DeleteFileW(p) &&
		        watch_gone(g, "cfg_new.go", 10000) &&
		        NULL != strstr(pr->screen, "3 matches") &&
		        NULL != strstr(pr->screen, "Search: cfg");
		ev_check(e, "fuzzy_removed_file_leaves_the_results", 0,
		        "cfg_new.go gone, 3 matches",
		        ok ? "observed" : "missing", ok);
	}
	if (!ok) {
		goto done;
	}

	/* 10. fast typing and fast deleting: the screen answers, and shows the
	 * last letters' results */
	pty_send(&pr->p, "\x7f\x7f\x7f", 3);
	pty_send(
	        &pr->p, "configfile", 10); /* one burst: taken as pasted text */
	ok = product_wait_ms(&pr->p, "Search: configfile", pr->screen,
	             sizeof pr->screen, 5000) &&
	        product_wait_ms(&pr->p, "> config_file.go", pr->screen,
	                sizeof pr->screen, 5000);
	for (int i = 0; ok && i < 10; i++) {
		pty_send(&pr->p, "\x7f", 1);
	}
	ok = ok && watch_gone(g, "Search: c", 5000) &&
	        NULL != strstr(pr->screen, "Search:");
	pty_send(&pr->p, "\x1b", 1);
	ok = ok &&
	        product_wait_ms(
	                &pr->p, "NORMAL", pr->screen, sizeof pr->screen, 5000);
	ev_check(e, "fuzzy_fast_input_keeps_the_screen_responsive", 0,
	        "burst typed, deleted, left with Esc",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}

	/* 11. a search changes nothing on the disk */
	after = gate_tree_text(g->browse);
	/* the test's own cfg_new.go came and went: the tree is the first one */
	ok = before && after && !strcmp(before, after);
	ev_check(e, "fuzzy_changes_no_file", 0, "tree as before",
	        ok ? "unchanged" : "CHANGED", ok);
	ok = ok && gate_quit(g);
done:
	free(before);
	free(after);
	return gate_first_run_end(g, e, ok, fails);
}

/* 4,500 names: the worker's path (more than WF_FZ_SYNC_MAX). */
static bool fuzzy_big_case(Ev *e)
{
	Gate *g = calloc(1, sizeof *g);
	Product *pr;
	bool ok = false;
	int fails = e->fails;
	char seen[240];
	DWORD handles0 = 0, handles1 = 0;
	size_t private0 = 0, private1 = 0;
	double cpu0, cpu1, t0, delay;
	PROCESS_MEMORY_COUNTERS_EX mem;

	if (!g || !gate_wanted("fuzzybig")) {
		free(g);
		return true;
	}
	if (!gate_open(g, e, "fuzzybig") || !gate_write(g->cfg, FUZZY_CONFIG) ||
	        !gate_mkdir(g, L"browse\\big")) {
		goto done;
	}
	for (int i = 0; i < FUZZY_BIG; i++) {
		wchar_t rel[64];

		swprintf(rel, 64, L"browse\\big\\f%04d.dat", i);
		if (!fuzzy_make(g, rel)) {
			goto done;
		}
	}
	if (!fuzzy_make(g, L"browse\\big\\needle_special.txt") ||
	        !gate_launch(g)) {
		goto done;
	}
	pr = &g->pr;
	pty_send(&pr->p, "l", 1); /* into big/: the first entry */
	ok = product_wait_ms(
	        &pr->p, "| 4501 items", pr->screen, sizeof pr->screen, 20000);
	ev_check(e, "fuzzybig_folder_is_listed", 0, "4501 items",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	Sleep(800);
	GetProcessHandleCount(pr->p.pi.hProcess, &handles0);
	memset(&mem, 0, sizeof mem);
	K32GetProcessMemoryInfo(
	        pr->p.pi.hProcess, (PROCESS_MEMORY_COUNTERS *)&mem, sizeof mem);
	private0 = mem.PrivateUsage;

	/* the search finds the one name among 4,500 */
	pty_send(&pr->p, "s", 1);
	ok = product_wait_ms(
	        &pr->p, "Search:", pr->screen, sizeof pr->screen, 8000);
	fuzzy_type(g, "needl");
	t0 = now_ms();
	fuzzy_type(g, "e");
	delay = product_wait_ms(&pr->p, "> needle_special.txt", pr->screen,
	                sizeof pr->screen, 8000)
	        ? now_ms() - t0
	        : -1.0;
	snprintf(seen, sizeof seen,
	        "%s, %.0f ms after the last key (incl. the "
	        "25 ms key pause)",
	        delay >= 0 ? "shown" : "never shown", delay);
	ok = ok && delay >= 0 && NULL != strstr(pr->screen, "1 match");
	product_snapshot(pr, "fuzzybig_needle");
	ev_check(e, "fuzzybig_finds_the_name_among_4500", 0,
	        "> needle_special.txt, 1 match", seen, ok);
	if (!ok) {
		goto done;
	}

	/* an open search that waits uses no CPU */
	cpu0 = watch_cpu_ms(pr->p.pi.hProcess);
	Sleep(3000);
	cpu1 = watch_cpu_ms(pr->p.pi.hProcess);
	snprintf(seen, sizeof seen, "%.0f ms of CPU in 3000 ms", cpu1 - cpu0);
	ok = cpu0 >= 0 && cpu1 >= 0 && cpu1 - cpu0 < 150.0;
	ev_check(e, "fuzzybig_open_search_is_idle", 0,
	        "under 150 ms of CPU in 3 s", seen, ok);
	if (!ok) {
		goto done;
	}

	/* the list changes while the worker's search is open */
	ok = fuzzy_make(g, L"browse\\big\\needle_two.txt") &&
	        product_wait_ms(&pr->p, "needle_two.txt", pr->screen,
	                sizeof pr->screen, 15000) &&
	        NULL != strstr(pr->screen, "2 matches") &&
	        NULL != strstr(pr->screen, "Search: needle");
	product_snapshot(pr, "fuzzybig_list_changed");
	ev_check(e, "fuzzybig_list_change_researches_safely", 0,
	        "needle_two.txt appears, 2 matches",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}

	/* quick edits: only the last letters' answer is on screen */
	for (int i = 0; ok && i < 6; i++) {
		pty_send(&pr->p, "\x7f", 1);
		pty_send(&pr->p, "e", 1);
	}
	pty_send(&pr->p, "\x7f\x7f\x7f\x7f\x7f\x7f", 6);
	pty_send(&pr->p, "\x7f", 1);
	fuzzy_type(g, "f4499");
	ok = ok &&
	        product_wait_ms(&pr->p, "> f4499.dat", pr->screen,
	                sizeof pr->screen, 8000) &&
	        NULL == strstr(pr->screen, "needle_special");
	product_snapshot(pr, "fuzzybig_quick_edits");
	ev_check(e, "fuzzybig_quick_edits_show_the_last_answer", 0,
	        "> f4499.dat, nothing of the earlier letters",
	        ok ? "observed" : "missing", ok);
	pty_send(&pr->p, "\x1b", 1);
	ok = ok &&
	        product_wait_ms(
	                &pr->p, "NORMAL", pr->screen, sizeof pr->screen, 5000);
	if (!ok) {
		goto done;
	}

	/* many searches: handles and memory do not grow */
	for (int i = 0; ok && i < 30; i++) {
		pty_send(&pr->p, "s", 1);
		ok = product_wait_ms(
		        &pr->p, "Search:", pr->screen, sizeof pr->screen, 8000);
		fuzzy_type(g, "ee");
		ok = ok &&
		        product_wait_ms(&pr->p, "matches", pr->screen,
		                sizeof pr->screen, 8000);
		pty_send(&pr->p, "\x1b", 1);
		ok = ok &&
		        product_wait_ms(&pr->p, "NORMAL", pr->screen,
		                sizeof pr->screen, 8000);
	}
	Sleep(800);
	GetProcessHandleCount(pr->p.pi.hProcess, &handles1);
	memset(&mem, 0, sizeof mem);
	K32GetProcessMemoryInfo(
	        pr->p.pi.hProcess, (PROCESS_MEMORY_COUNTERS *)&mem, sizeof mem);
	private1 = mem.PrivateUsage;
	snprintf(seen, sizeof seen,
	        "handles %lu -> %lu, private bytes %zu -> %zu (%+lld) after 30 "
	        "searches",
	        (unsigned long)handles0, (unsigned long)handles1, private0,
	        private1, (long long)private1 - (long long)private0);
	/* an ASan program keeps what it frees in a quarantine of up to 256 MiB: its
	 * private bytes say nothing about a leak, so there the handles alone are judged
	 * (the sanitizer's own leak and use-after-free checks are the judge of memory) */
	ok = ok && handles1 <= handles0 + 4 &&
	        (GetEnvironmentVariableA("PIERO_E2E_SANITIZED", NULL, 0) ||
	                private1 < private0 + 4u * 1024u * 1024u);
	ev_check(e, "fuzzybig_repeated_searches_do_not_grow", 0,
	        "handles +4 at most, memory +4 MiB at most", seen, ok);
	ok = ok && gate_quit(g);
done:
	return gate_first_run_end(g, e, ok, fails);
}

static int scn_fuzzy(Ev *e)
{
	bool good = fuzzy_small_case(e);

	good = fuzzy_big_case(e) && good;
	if (!good || e->fails) {
		return 1;
	}
	return g_gate_skips ? 77 : 0;
}
