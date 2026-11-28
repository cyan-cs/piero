/*
 * The search below a folder through the real product (DR-DRAFT-031): Tab in the
 * filter prompt widens "report ext:txt" to this folder and below, the results come
 * with the folders they are in, Enter shows one in its folder, Ctrl+A and y say that
 * results of several folders cannot be copied together, Delete asks one question for
 * all of them and `n` changes nothing. Included by p0_conpty_host.c after
 * p0_state_gate.c and p0_dirsize.c, whose helpers (gate_*, dirsize_row) it uses.
 */

static bool search_wait(Gate *g, const char *text, DWORD ms)
{
	return product_wait_ms(
	        &g->pr.p, text, g->pr.screen, sizeof g->pr.screen, ms);
}

/* Sends keys, and gives the program a moment: an Esc followed at once by another key
 * is read as Alt and that key, so every send is spaced from the next. */
static void search_send(Gate *g, const char *bytes, size_t n)
{
	pty_send(&g->pr.p, bytes, n);
	Sleep(250);
}

static bool search_make_files(Gate *g)
{
	wchar_t p[1100];

	return gate_path(g, L"browse\\a\\report one.txt", p) &&
	        gate_write(p, "1") &&
	        gate_path(g, L"browse\\b\\deep\\report two.txt", p) &&
	        gate_write(p, "22") && gate_path(g, L"browse\\note.txt", p) &&
	        gate_write(p, "333") && gate_path(g, L"browse\\report.md", p) &&
	        gate_write(p, "4444");
}

static bool search_case(Ev *e)
{
	Gate *g = calloc(1, sizeof *g);
	char *before = NULL;
	bool ok = false;
	int fails = e->fails;

	if (!g || !gate_wanted("search")) {
		free(g);
		return true;
	}
	if (!gate_open(g, e, "search") || !gate_mkdir(g, L"browse\\a") ||
	        !gate_mkdir(g, L"browse\\b") ||
	        !gate_mkdir(g, L"browse\\b\\deep") || !search_make_files(g) ||
	        !gate_write(g->cfg, DIRSIZE_CONFIG) || !gate_launch(g)) {
		goto done;
	}
	ok = search_wait(g, "note.txt", 8000) &&
	        dirsize_row(g, "note.txt", NULL, 8000);
	ev_check(e, "search_list_is_shown", 0, "the folder is listed",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	before = gate_tree_text(g->browse);

	/* an empty query and a wrong one change nothing */
	search_send(g, "/", 1);
	search_send(g, "\t", 1);
	ok = search_wait(g, "Type what to look for", 5000);
	ev_check(e, "search_empty_query_is_refused", 0, "Type what to look for",
	        ok ? "observed" : "missing", ok);
	search_send(g, "size>x", 6);
	search_send(g, "\t", 1);
	ok = ok && search_wait(g, "Not a valid search", 5000);
	ev_check(e, "search_wrong_query_is_refused", 0, "Not a valid search",
	        ok ? "observed" : "missing", ok);
	search_send(g, "\x1b", 1); /* drops the filter */
	ok = ok && search_wait(g, "NORMAL", 5000);

	/* the search: words and an extension */
	search_send(g, "/", 1);
	search_send(g, "report ext:txt", 14);
	search_send(g, "\t", 1);
	ok = ok && search_wait(g, "2 found", 15000) &&
	        search_wait(g, "finished", 15000);
	product_snapshot(&g->pr, "search_results");
	ev_check(e, "search_finds_below_the_folder", 0, "2 found, finished",
	        ok ? "observed" : "missing", ok);
	pty_snapshot(&g->pr.p, g->pr.screen, sizeof g->pr.screen);
	ok = ok && strstr(g->pr.screen, "a\\report one.txt") &&
	        strstr(g->pr.screen, "b\\deep\\report two.txt") &&
	        !strstr(g->pr.screen, "report.md") &&
	        !strstr(g->pr.screen, "note.txt");
	ev_check(e, "search_rows_show_the_folder_and_the_name", 0,
	        "a\\report one.txt and b\\deep\\report two.txt, nothing else",
	        ok ? "observed" : "WRONG", ok);
	if (!ok) {
		goto done;
	}

	/* Enter shows the second in its folder, with the cursor on it */
	search_send(g, "\x1b[B", 3);
	search_send(g, "\r", 1);
	ok = search_wait(g, "browse\\b\\deep", 8000) &&
	        dirsize_row(g, "> report two.txt", "", 8000);
	product_snapshot(&g->pr, "search_shown");
	ev_check(e, "search_enter_shows_it_in_its_folder", 0,
	        "browse\\b\\deep with the cursor on report two.txt",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}

	/* a search from there is a search of there: one result */
	search_send(g, "/", 1);
	search_send(g, "report", 6);
	search_send(g, "\t", 1);
	ok = search_wait(g, "1 found", 15000);
	ev_check(e, "search_starts_from_the_folder_on_screen", 0, "1 found",
	        ok ? "observed" : "missing", ok);
	search_send(
	        g, "\x1b", 1); /* the walk is over: this closes the window */
	ok = ok && search_wait(g, "NORMAL", 5000);
	search_send(g, "\x1b", 1); /* and this drops the filter */
	/* back up to the folder we began in */
	search_send(g, "h", 1);
	search_send(g, "h", 1);
	ok = ok && dirsize_row(g, "note.txt", "", 8000);
	if (!ok) {
		goto done;
	}

	/* the three of "report" are in three folders: copy refuses, delete asks once */
	search_send(g, "/", 1);
	search_send(g, "report", 6);
	search_send(g, "\t", 1);
	ok = search_wait(g, "3 found", 15000) &&
	        search_wait(g, "finished", 15000);
	search_send(g, "\x01", 1); /* Ctrl+A marks every result */
	ok = ok && search_wait(g, "3 marked", 5000);
	product_snapshot(&g->pr, "search_marked");
	search_send(g, "y", 1);
	ok = ok && search_wait(g, "one folder at a time", 5000);
	ev_check(e, "search_copy_of_several_folders_is_refused", 0,
	        "one folder at a time", ok ? "observed" : "missing", ok);
	search_send(g, "\x1b[3~", 4);
	ok = ok && search_wait(g, "3 item(s) in 3 folder(s)", 8000) &&
	        NULL != strstr(g->pr.screen, "RECYCLE");
	product_snapshot(&g->pr, "search_delete_question");
	ev_check(e, "search_delete_asks_once_for_every_folder", 0,
	        "RECYCLE, 3 item(s) in 3 folder(s)",
	        ok ? "observed" : "missing", ok);
	Sleep(500);
	search_send(g, "\x1b", 1); /* Esc: the question is dropped */
	ok = ok && search_wait(g, "NORMAL", 5000);
	{
		char *now = gate_tree_text(g->browse);

		ok = ok && now && before && !strcmp(now, before);
		free(now);
	}
	ev_check(e, "search_cancel_changes_nothing", 0, "the tree is as it was",
	        ok ? "unchanged" : "CHANGED", ok);
	ok = ok && gate_quit(g);
done:
	free(before);
	return gate_first_run_end(g, e, ok, fails);
}

static int scn_search(Ev *e)
{
	return search_case(e) ? (g_gate_skips ? 77 : 0) : 1;
}

/* F: the files below the folder that hold the same bytes (DR-DRAFT-032). Three equal
 * files in three folders, one of the same size and other bytes, an empty pair and a
 * file alone: the three are one group, and nothing else is listed. */
static bool dupes_make_files(Gate *g)
{
	wchar_t p[1100];

	return gate_path(g, L"browse\\a\\report one.txt", p) &&
	        gate_write(p, "the same words") &&
	        gate_path(g, L"browse\\b\\copy.txt", p) &&
	        gate_write(p, "the same words") &&
	        gate_path(g, L"browse\\b\\deep\\third.txt", p) &&
	        gate_write(p, "the same words") &&
	        gate_path(g, L"browse\\c\\odd.txt", p) &&
	        gate_write(p, "the same wordz") &&
	        gate_path(g, L"browse\\note.txt", p) && gate_write(p, "333") &&
	        gate_path(g, L"browse\\empty1.txt", p) && gate_write(p, "") &&
	        gate_path(g, L"browse\\empty2.txt", p) && gate_write(p, "");
}

static bool dupes_case(Ev *e)
{
	Gate *g = calloc(1, sizeof *g);
	char *before = NULL;
	bool ok = false;
	int fails = e->fails;

	if (!g || !gate_wanted("dupes")) {
		free(g);
		return true;
	}
	if (!gate_open(g, e, "dupes") || !gate_mkdir(g, L"browse\\a") ||
	        !gate_mkdir(g, L"browse\\b") || !gate_mkdir(g, L"browse\\c") ||
	        !gate_mkdir(g, L"browse\\b\\deep") || !dupes_make_files(g) ||
	        !gate_write(g->cfg, DIRSIZE_CONFIG) || !gate_launch(g)) {
		goto done;
	}
	/* A row can arrive before the rest of its VT frame. Use the same
	 * output-quiescence gate as subsequent steps before sending F. */
	ok = search_wait(g, "note.txt", 8000) &&
	        dirsize_row(g, "note.txt", NULL, 8000);
	ev_check(e, "dupes_list_is_shown", 0, "the folder is listed",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	before = gate_tree_text(g->browse);

	search_send(g, "F", 1);
	ok = search_wait(g, "Duplicate files below", 8000) &&
	        search_wait(g, "3 files in 1 groups", 20000) &&
	        search_wait(g, "finished", 20000);
	product_snapshot(&g->pr, "dupes_results");
	ev_check(e, "dupes_finds_the_three_equal_files", 0,
	        "3 files in 1 groups, finished", ok ? "observed" : "missing",
	        ok);
	pty_snapshot(&g->pr.p, g->pr.screen, sizeof g->pr.screen);
	ok = ok && strstr(g->pr.screen, "[1]") &&
	        strstr(g->pr.screen, "a\\report one.txt") &&
	        strstr(g->pr.screen, "b\\copy.txt") &&
	        strstr(g->pr.screen, "b\\deep\\third.txt") &&
	        !strstr(g->pr.screen, "odd.txt") &&
	        !strstr(g->pr.screen, "note.txt") &&
	        !strstr(g->pr.screen, "empty");
	ev_check(e, "dupes_lists_the_group_and_nothing_else", 0,
	        "the three paths with [1]; not the odd, the lone or the empty",
	        ok ? "observed" : "WRONG", ok);
	if (!ok) {
		goto done;
	}

	/* Ctrl+A is not offered for copies */
	search_send(g, "\x01", 1);
	ok = search_wait(g, "only recycled", 5000);
	ev_check(e, "dupes_ctrl_a_is_not_offered", 0, "only recycled",
	        ok ? "observed" : "missing", ok);

	/* two can be marked, the last of the group cannot */
	search_send(g, "   ", 3);
	ok = search_wait(g, "2 marked", 5000) &&
	        search_wait(g, "Each group keeps one file", 5000);
	product_snapshot(&g->pr, "dupes_marked");
	ev_check(e, "dupes_the_last_copy_cannot_be_marked", 0,
	        "2 marked, and the third refused", ok ? "observed" : "missing",
	        ok);
	search_send(g, "y", 1);
	ok = ok && search_wait(g, "only recycled", 5000);
	ev_check(e, "dupes_copy_is_not_offered", 0, "only recycled",
	        ok ? "observed" : "missing", ok);

	/* Delete asks once; Esc leaves everything as it was */
	search_send(g, "\x1b[3~", 4);
	ok = ok && search_wait(g, "2 item(s) in 2 folder(s)", 8000) &&
	        NULL != strstr(g->pr.screen, "RECYCLE");
	product_snapshot(&g->pr, "dupes_delete_question");
	ev_check(e, "dupes_delete_asks_once", 0,
	        "RECYCLE, 2 item(s) in 2 folder(s)",
	        ok ? "observed" : "missing", ok);
	Sleep(500);
	search_send(g, "\x1b", 1);
	ok = ok && search_wait(g, "NORMAL", 5000);
	{
		char *now = gate_tree_text(g->browse);

		ok = ok && now && before && !strcmp(now, before);
		free(now);
	}
	ev_check(e, "dupes_cancel_changes_nothing", 0, "the tree is as it was",
	        ok ? "unchanged" : "CHANGED", ok);
	ok = ok && gate_quit(g);
done:
	free(before);
	return gate_first_run_end(g, e, ok, fails);
}

static int scn_dupes(Ev *e)
{
	return dupes_case(e) ? (g_gate_skips ? 77 : 0) : 1;
}
