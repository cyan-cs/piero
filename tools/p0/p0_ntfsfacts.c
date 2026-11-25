/*
 * NTFS facts in the details window (`i`) through the real product: a file with
 * an alternate stream and a second name shows both, the window changes no file,
 * and Esc closes it (DR-DRAFT-026). Included by p0_conpty_host.c after
 * p0_state_gate.c and p0_dirsize.c, whose helpers (gate_*) it uses.
 */

static bool ntfs_make_files(Gate *g)
{
	wchar_t a[1100], b[1100], s[1120];

	if (!gate_path(g, L"browse\\a.txt", a) ||
	        !gate_path(g, L"browse\\b.txt", b) || !gate_write(a, "main")) {
		return false;
	}
	swprintf(s, 1120, L"%ls:alpha", a);
	if (!gate_write(s, "12345")) {
		return false;
	}
	return CreateHardLinkW(b, a, NULL) != 0;
}

static int scn_ntfsfacts(Ev *e)
{
	Gate *g = calloc(1, sizeof *g);
	char *before = NULL, *after = NULL;
	bool ok = false;
	int fails = e->fails;

	if (!g || !gate_wanted("ntfsfacts")) {
		free(g);
		return 0;
	}
	if (!gate_open(g, e, "ntfsfacts") || !ntfs_make_files(g) ||
	        !gate_write(g->cfg, DIRSIZE_CONFIG) || !gate_launch(g)) {
		goto done;
	}
	ok = dirsize_row(g, "a.txt", NULL, 8000);
	ev_check(e, "ntfsfacts_list_is_shown", 0, "a.txt listed",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	before = gate_tree_text(g->browse);
	pty_send(&g->pr.p, "i", 1);
	ok = product_wait_ms(&g->pr.p, "alpha  5 bytes", g->pr.screen,
	        sizeof g->pr.screen, 8000);
	product_snapshot(&g->pr, "ntfsfacts_window");
	ev_check(e, "ntfsfacts_stream_name_and_size", 0, "alpha  5 bytes",
	        ok ? "observed" : "missing", ok);
	ok = ok && NULL != strstr(g->pr.screen, "this one and 1 other");
	ev_check(e, "ntfsfacts_second_name_counted", 0,
	        "2 (this one and 1 other hard links)",
	        ok ? "observed" : "missing", ok);
	ok = ok && NULL != strstr(g->pr.screen, "NTFS");
	ev_check(e, "ntfsfacts_volume_is_named", 0, "NTFS in the Volume line",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	pty_send(&g->pr.p, "\x1b", 1);
	after = gate_tree_text(g->browse);
	ok = before && after && !strcmp(before, after);
	ev_check(e, "ntfsfacts_changes_no_file", 0, "tree as before",
	        ok ? "unchanged" : "CHANGED", ok);
	ok = ok && gate_quit(g);
done:
	free(before);
	free(after);
	return gate_first_run_end(g, e, ok, fails) ? (g_gate_skips ? 77 : 0)
	                                           : 1;
}
