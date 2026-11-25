/*
 * Folder sizes (`D`) through the real product: the totals of every folder in
 * the list appear at the end of each folder's row and the state line says how
 * many were counted; the files are as they were; `r` forgets the totals.
 * Included by p0_conpty_host.c after p0_state_gate.c, whose Gate helpers it
 * uses.
 */

#define DIRSIZE_CONFIG                                                         \
	"[meta]\r\nschema_version = 1\r\nonboarding_completed = true\r\n"      \
	"[updates]\r\nenabled = false\r\nconsent_recorded = true\r\n"

/* True when a screen line holds both `name` and `text` (`text` NULL: the
 * line must hold no "iB", the end of every size), within `ms`. */
static bool dirsize_row(Gate *g, const char *name, const char *text, DWORD ms)
{
	Product *pr = &g->pr;
	double t0 = now_ms();

	do {
		const char *line = pr->screen;

		pty_snapshot(&pr->p, pr->screen, sizeof pr->screen);
		while (*line) {
			const char *end = strchr(line, '\n');
			size_t n = end ? (size_t)(end - line) : strlen(line);
			char row[1024];

			if (n >= sizeof row) {
				n = sizeof row - 1;
			}
			memcpy(row, line, n);
			row[n] = '\0';
			if (strstr(row, name) &&
			        (text ? NULL != strstr(row, text)
			              : NULL == strstr(row, "iB"))) {
				return true;
			}
			line = end ? end + 1 : line + n;
		}
		Sleep(50);
	} while (now_ms() - t0 < (double)ms);
	return false;
}

static bool dirsize_make_files(Gate *g)
{
	static char block[2049];
	wchar_t a[1100], b[1100];

	memset(block, 'x', 2048);
	if (!gate_mkdir(g, L"browse\\big") ||
	        !gate_mkdir(g, L"browse\\big\\sub") ||
	        !gate_mkdir(g, L"browse\\mid") ||
	        !gate_path(g, L"browse\\big\\a.bin", a) ||
	        !gate_path(g, L"browse\\big\\sub\\b.bin", b) ||
	        !gate_write(a, block)) {
		return false;
	}
	block[1024] = '\0'; /* the second file is 1024 bytes */
	return gate_write(b, block);
}

static int scn_dirsize(Ev *e)
{
	Gate *g = calloc(1, sizeof *g);
	char *before = NULL, *after = NULL;
	bool ok = false;
	int fails = e->fails;

	if (!g || !gate_wanted("dirsize")) {
		free(g);
		return 0;
	}
	if (!gate_open(g, e, "dirsize") || !dirsize_make_files(g) ||
	        !gate_write(g->cfg, DIRSIZE_CONFIG) || !gate_launch(g)) {
		goto done;
	}
	ok = dirsize_row(g, "big/", NULL, 8000);
	ev_check(e, "dirsize_rows_start_without_sizes", 0,
	        "big/ listed, no size", ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	before = gate_tree_text(g->browse);
	pty_send(&g->pr.p, "D", 1);
	ok = dirsize_row(g, "big/", "3.0 KiB", 20000) &&
	        dirsize_row(g, "mid/", "0 B", 5000);
	product_snapshot(&g->pr, "dirsize_totals");
	ev_check(e, "dirsize_totals_are_drawn", 0,
	        "big/ 3.0 KiB (2048 + 1024), mid/ 0 B",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	ok = product_wait_ms(&g->pr.p, "Folder sizes counted: 2", g->pr.screen,
	        sizeof g->pr.screen, 5000);
	ev_check(e, "dirsize_state_line_says_how_many", 0,
	        "Folder sizes counted: 2", ok ? "observed" : "missing", ok);
	after = gate_tree_text(g->browse);
	ok = before && after && !strcmp(before, after);
	ev_check(e, "dirsize_changes_no_file", 0, "tree as before",
	        ok ? "unchanged" : "CHANGED", ok);
	if (!ok) {
		goto done;
	}
	/* r reloads the list and forgets the totals: a row of the old screen
	 * cannot satisfy this, because it holds the size until redrawn */
	pty_send(&g->pr.p, "r", 1);
	ok = dirsize_row(g, "big/", NULL, 8000);
	ev_check(e, "dirsize_r_forgets_the_totals", 0, "big/ without a size",
	        ok ? "observed" : "size still shown", ok);
	ok = ok && gate_quit(g);
done:
	free(before);
	free(after);
	return gate_first_run_end(g, e, ok, fails) ? (g_gate_skips ? 77 : 0)
	                                           : 1;
}
