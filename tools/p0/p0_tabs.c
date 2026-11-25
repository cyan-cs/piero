/*
 * Tabs and places through the real product (DR-DRAFT-030): t opens a tab and the
 * strip appears in the title row, [ and ] move between tabs and each keeps its
 * folder, w closes one and the strip goes; m then a letter writes [places] to the
 * configuration file and nothing else in it, ' then the letter goes there, also in
 * the next run. Included by p0_conpty_host.c after p0_state_gate.c and
 * p0_dirsize.c, whose helpers (gate_*, dirsize_row) it uses.
 */

static char *tabs_read_config(Gate *g)
{
	HANDLE h = CreateFileW(g->cfg, GENERIC_READ,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	char *text = calloc(1, 8192);
	DWORD got = 0;

	if (INVALID_HANDLE_VALUE == h || !text) {
		if (INVALID_HANDLE_VALUE != h) {
			CloseHandle(h);
		}
		free(text);
		return NULL;
	}
	ReadFile(h, text, 8191, &got, NULL);
	CloseHandle(h);
	return text;
}

static bool tabs_wait(Gate *g, const char *text, DWORD ms)
{
	return product_wait_ms(
	        &g->pr.p, text, g->pr.screen, sizeof g->pr.screen, ms);
}

/* The title row shows the folder: true when it shows browse and not browse\a. */
static bool tabs_at_browse(Gate *g, DWORD ms)
{
	ULONGLONG end = GetTickCount64() + ms;

	do {
		pty_snapshot(&g->pr.p, g->pr.screen, sizeof g->pr.screen);
		if (strstr(g->pr.screen, "browse") &&
		        !strstr(g->pr.screen, "browse\\a")) {
			return true;
		}
		Sleep(50);
	} while (GetTickCount64() < end);
	return false;
}

static bool tabs_case(Ev *e)
{
	Gate *g = calloc(1, sizeof *g);
	char *cfg = NULL, *before = NULL;
	bool ok = false;
	int fails = e->fails;

	if (!g || !gate_wanted("tabs")) {
		free(g);
		return true;
	}
	if (!gate_open(g, e, "tabs") || !gate_mkdir(g, L"browse\\a") ||
	        !gate_mkdir(g, L"browse\\b") ||
	        !gate_write(g->cfg, DIRSIZE_CONFIG) || !gate_launch(g)) {
		goto done;
	}
	ok = dirsize_row(g, "a/", NULL, 8000);
	ev_check(e, "tabs_list_is_shown", 0, "a/ and b/ listed",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	before = gate_tree_text(g->browse);
	ok = NULL == strstr(g->pr.screen, "[1:") &&
	        NULL == strstr(g->pr.screen, "1:browse");
	ev_check(e, "tabs_no_strip_with_one_tab", 0, "no tab strip",
	        ok ? "observed" : "a strip is drawn", ok);

	/* t: a second tab, here; l: into a folder in it */
	pty_send(&g->pr.p, "t", 1);
	ok = tabs_wait(g, "1:browse", 6000) &&
	        NULL != strstr(g->pr.screen, "[2:browse]");
	product_snapshot(&g->pr, "tabs_two");
	ev_check(e, "tabs_t_opens_a_tab_and_the_strip_appears", 0,
	        "1:browse [2:browse] in the title row",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	pty_send(&g->pr.p, "l", 1);
	ok = tabs_wait(g, "[2:a]", 6000);
	ev_check(e, "tabs_the_new_tab_goes_into_a_folder", 0, "[2:a]",
	        ok ? "observed" : "missing", ok);
	pty_send(&g->pr.p, "[", 1);
	ok = ok && tabs_wait(g, "[1:browse]", 6000) &&
	        NULL != strstr(g->pr.screen, "2:a");
	ev_check(e, "tabs_the_first_tab_kept_its_folder", 0, "[1:browse] 2:a",
	        ok ? "observed" : "missing", ok);
	pty_send(&g->pr.p, "]", 1);
	ok = ok && tabs_wait(g, "[2:a]", 6000);
	ev_check(e, "tabs_the_second_tab_kept_its_own", 0, "1:browse [2:a]",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	pty_send(&g->pr.p, "w", 1);
	ok = tabs_wait(g, "Tab closed", 6000);
	pty_snapshot(&g->pr.p, g->pr.screen, sizeof g->pr.screen);
	ok = ok && NULL == strstr(g->pr.screen, "[1:") &&
	        NULL == strstr(g->pr.screen, "1:browse");
	ev_check(e, "tabs_w_closes_it_and_the_strip_goes", 0,
	        "Tab closed, no strip", ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}

	/* m a: the folder is kept in the configuration file, and nothing else in it */
	pty_send(&g->pr.p, "m", 1);
	ok = tabs_wait(g, "Keep this folder as a place", 6000);
	pty_send(&g->pr.p, "a", 1);
	ok = ok && tabs_wait(g, "Place a = ", 6000);
	product_snapshot(&g->pr, "tabs_place_saved");
	ev_check(e, "tabs_m_then_a_keeps_the_folder", 0, "Place a = ...",
	        ok ? "observed" : "missing", ok);
	cfg = tabs_read_config(g);
	ok = ok && cfg && strstr(cfg, "[places]") && strstr(cfg, "\na = ") &&
	        strstr(cfg, "browse") && strstr(cfg, "[updates]") &&
	        strstr(cfg, "enabled = false") &&
	        strstr(cfg, "onboarding_completed = true");
	ev_check(e, "tabs_the_file_has_the_place_and_everything_it_had", 0,
	        "[places] a = ...browse and the old keys",
	        ok ? "observed" : "WRONG", ok);
	if (!ok) {
		goto done;
	}
	/* ' a from another folder goes back */
	pty_send(&g->pr.p, "'", 1);
	ok = tabs_wait(g, "Go to a place", 6000);
	pty_send(&g->pr.p, "\x1b", 1);
	ok = ok && tabs_wait(g, "NORMAL", 6000);
	pty_send(&g->pr.p, "l", 1);
	ok = ok &&
	        tabs_wait(g, "browse\\a", 6000); /* into a: the title says so */
	pty_send(&g->pr.p, "'", 1);
	pty_send(&g->pr.p, "a", 1);
	ok = ok && tabs_at_browse(g, 8000);
	ev_check(e, "tabs_apostrophe_goes_to_the_place", 0,
	        "the title shows browse again, not browse\\a",
	        ok ? "observed" : "missing", ok);
	{
		char *now = gate_tree_text(g->browse);

		ok = ok && now && before && !strcmp(now, before);
		free(now);
	}
	ev_check(e, "tabs_change_no_file", 0, "tree as before",
	        ok ? "unchanged" : "CHANGED", ok);
	ok = ok && gate_quit(g);
	if (!ok) {
		goto done;
	}

	/* the next run knows the place: from inside a folder, ' a goes to it */
	ok = gate_launch(g) && dirsize_row(g, "a/", NULL, 8000);
	pty_send(&g->pr.p, "l", 1);
	ok = ok && tabs_wait(g, "browse\\a", 6000);
	pty_send(&g->pr.p, "'", 1);
	ok = ok && tabs_wait(g, "a  ", 6000) &&
	        NULL != strstr(g->pr.screen, "browse");
	product_snapshot(&g->pr, "tabs_places_listed");
	pty_send(&g->pr.p, "a", 1);
	ok = ok && tabs_at_browse(g, 8000);
	ev_check(e, "tabs_the_place_is_still_there_in_the_next_run", 0,
	        "' lists it and a goes to it", ok ? "observed" : "missing", ok);
	ok = ok && gate_quit(g);
done:
	free(cfg);
	free(before);
	return gate_first_run_end(g, e, ok, fails);
}

static int scn_tabs(Ev *e)
{
	return tabs_case(e) ? (g_gate_skips ? 77 : 0) : 1;
}
