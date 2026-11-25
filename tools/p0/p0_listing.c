/*
 * The order of the listing through the real product: `[files]
 * directories_first`. A folder named so that it sorts between two files shows
 * above both of them by default, and between them when the setting is off.
 * The order must also survive a change of sort from the sort menu (S), which
 * orders the list a second time. Included by p0_conpty_host.c after
 * p0_state_gate.c, whose Gate helpers it uses.
 */

#define LISTING_CONFIG                                                         \
	"[meta]\r\nschema_version = 1\r\nonboarding_completed = true\r\n"      \
	"[updates]\r\nenabled = false\r\nconsent_recorded = true\r\n"

/* The row (counted from 0) on which `text` first appears, or -1. */
static int listing_row(const char *screen, const char *text)
{
	const char *hit = strstr(screen, text);
	int row = 0;
	const char *p;

	if (!hit) {
		return -1;
	}
	for (p = screen; p < hit; p++) {
		row += '\n' == *p;
	}
	return row;
}

/* True when `above` is listed higher than `below`, within `ms`. */
static bool listing_order(
        Gate *g, const char *above, const char *below, DWORD ms, int *rows)
{
	Product *pr = &g->pr;
	double t0 = now_ms();

	do {
		int a, b;

		pty_snapshot(&pr->p, pr->screen, sizeof pr->screen);
		a = listing_row(pr->screen, above);
		b = listing_row(pr->screen, below);
		if (a >= 0 && b >= 0 && a < b) {
			*rows = a;
			return true;
		}
		Sleep(50);
	} while (now_ms() - t0 < (double)ms);
	return false;
}

/* One start of the program with the given setting. `mixed`: the folder is
 * expected between doc.txt and note.txt, otherwise above both. */
static bool listing_case(
        Ev *e, const char *tag, const char *setting, bool mixed)
{
	Gate *g = calloc(1, sizeof *g);
	char name[96];
	int row;
	bool ok = false;
	int fails = e->fails;

	if (!g || !gate_wanted(tag)) {
		free(g);
		return true;
	}
	if (!gate_open(g, e, tag) || !gate_mkdir(g, L"browse\\mid")) {
		goto done;
	}
	{
		char text[512];

		snprintf(text, sizeof text, "%s%s", LISTING_CONFIG, setting);
		if (!gate_write(g->cfg, text)) {
			goto done;
		}
	}
	if (!gate_launch(g)) {
		goto done;
	}
	/* the first listing: ordered by the program's own sort of a new folder */
	snprintf(name, sizeof name, "%s_listing_order", tag);
	ok = mixed ? listing_order(g, "doc.txt", "mid/", 5000, &row) &&
	                listing_order(g, "mid/", "note.txt", 5000, &row)
	           : listing_order(g, "mid/", "doc.txt", 5000, &row) &&
	                listing_order(g, "doc.txt", "note.txt", 5000, &row);
	product_snapshot(&g->pr, name);
	ev_check(e, name, 0,
	        mixed ? "doc.txt, mid/, note.txt" : "mid/, doc.txt, note.txt",
	        ok ? "as expected" : "other order", ok);
	if (!ok) {
		goto done;
	}
	/* the sort menu orders the same list again: size, then name. A letter
	 * applies at once and the menu stays open until Esc. */
	pty_send(&g->pr.p, "S", 1);
	if (!product_wait_ms(
	            &g->pr.p, "[n]", g->pr.screen, sizeof g->pr.screen, 5000)) {
		ev_check(e, "sort_menu_opens", 0, "menu with [n] name",
		        "missing", 0);
		goto done;
	}
	/* by size the folder (0 bytes) comes first and the smaller file next, in
	 * both settings: an order that differs from the one before and after, so
	 * the checks cannot be satisfied by a screen that was not redrawn */
	pty_send(&g->pr.p, "s", 1);
	snprintf(name, sizeof name, "%s_order_by_size", tag);
	ok = listing_order(g, "mid/", "note.txt", 5000, &row) &&
	        listing_order(g, "note.txt", "doc.txt", 5000, &row);
	product_snapshot(&g->pr, name);
	ev_check(e, name, 0, "mid/, note.txt, doc.txt",
	        ok ? "as expected" : "other order", ok);
	if (!ok) {
		goto done;
	}
	pty_send(&g->pr.p, "n", 1);
	snprintf(name, sizeof name, "%s_order_after_the_sort_menu", tag);
	ok = mixed ? listing_order(g, "doc.txt", "mid/", 5000, &row) &&
	                listing_order(g, "mid/", "note.txt", 5000, &row)
	           : listing_order(g, "mid/", "doc.txt", 5000, &row) &&
	                listing_order(g, "doc.txt", "note.txt", 5000, &row);
	product_snapshot(&g->pr, name);
	ev_check(e, name, 0,
	        mixed ? "doc.txt, mid/, note.txt" : "mid/, doc.txt, note.txt",
	        ok ? "as expected" : "other order", ok);
	/* leave the menu; the exit that follows would otherwise only close it */
	pty_send(&g->pr.p, "\x1b", 1);
	ok = ok && gate_quit(g);
done:
	return gate_first_run_end(g, e, ok, fails);
}

static int scn_listing(Ev *e)
{
	bool good = true;

	good = listing_case(e, "listing_directories_first", "", false) && good;
	good = listing_case(e, "listing_directories_mixed",
	               "[files]\r\ndirectories_first = false\r\n", true) &&
	        good;
	if (!good || e->fails) {
		return 1;
	}
	return g_gate_skips ? 77 : 0;
}
