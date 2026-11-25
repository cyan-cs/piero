/*
 * The Markdown preview and the two-pane editor through the real product
 * (DR-DRAFT-025): the preview of a .md file shows headings by their level
 * with the marks left out, a list and a table; in the editor the text is on
 * the left and its preview on the right, typing on the left changes the
 * right after a moment, Alt+M hides and shows the right pane, and no file is
 * changed by any of it. Included by p0_conpty_host.c after p0_state_gate.c.
 */

#define MD_CONFIG                                                              \
	"[meta]\r\nschema_version = 1\r\nonboarding_completed = true\r\n"      \
	"[updates]\r\nenabled = false\r\nconsent_recorded = true\r\n"

#define MD_TEXT                                                                \
	"# Big Heading\r\n\r\n## Sub Heading\r\n\r\nSome **strong** text.\r\n" \
	"\r\n- apple\r\n- banana\r\n\r\n| a | b |\r\n|---|---|\r\n| 1 | 2 "    \
	"|\r\n"

/* The number of times `needle` is on one screen line, for the line that has
 * the most. */
static int md_most_on_a_line(const char *screen, const char *needle)
{
	const char *line = screen;
	int best = 0;
	size_t nn = strlen(needle);

	while (*line) {
		const char *end = strchr(line, '\n');
		size_t n = end ? (size_t)(end - line) : strlen(line);
		int count = 0;

		for (size_t i = 0; i + nn <= n; i++) {
			if (!memcmp(line + i, needle, nn)) {
				count++;
			}
		}
		if (count > best) {
			best = count;
		}
		line = end ? end + 1 : line + n;
	}
	return best;
}

/* True when, somewhere in what the program wrote, `text` follows the escape
 * sequence `sgr` within `window` bytes: the text was drawn in that style. */
static bool md_drawn_with(Pty *p, const char *text, const char *sgr)
{
	size_t tn = strlen(text), sn = strlen(sgr), i;
	bool found = false;

	EnterCriticalSection(&p->lock);
	for (i = 0; i + tn <= p->raw_n && !found; i++) {
		if (!memcmp(p->raw + i, text, tn)) {
			size_t low = i > 160 ? i - 160 : 0;

			for (size_t k = i; k >= sn && k >= low; k--) {
				if (!memcmp(p->raw + k - sn, sgr, sn)) {
					found = true;
					break;
				}
			}
		}
	}
	LeaveCriticalSection(&p->lock);
	return found;
}

/* Waits until a screen line shows `needle` at least `times` times. */
static bool md_wait_count(Gate *g, const char *needle, int times, DWORD ms)
{
	Product *pr = &g->pr;
	double t0 = now_ms();

	do {
		pty_snapshot(&pr->p, pr->screen, sizeof pr->screen);
		if (md_most_on_a_line(pr->screen, needle) >= times) {
			return true;
		}
		Sleep(50);
	} while (now_ms() - t0 < (double)ms);
	return false;
}

static bool md_wait_text(Gate *g, const char *needle, DWORD ms)
{
	return product_wait_ms(
	        &g->pr.p, needle, g->pr.screen, sizeof g->pr.screen, ms);
}

static int scn_markdown(Ev *e)
{
	Gate *g = calloc(1, sizeof *g);
	Product *pr;
	char *before = NULL, *after = NULL;
	wchar_t md[1100];
	bool ok = false;
	int fails = e->fails;

	if (!g || !gate_wanted("markdown")) {
		free(g);
		return 0;
	}
	if (!gate_open(g, e, "markdown")) {
		goto done;
	}
	pr = &g->pr;
	if (!gate_path(g, L"browse\\readme.md", md) ||
	        !gate_write(md, MD_TEXT) || !gate_write(g->cfg, MD_CONFIG) ||
	        !gate_launch(g)) {
		goto done;
	}
	before = gate_tree_text(g->browse);
	/* the cursor goes to readme.md (doc.txt, note.txt, readme.md) */
	pty_send(&pr->p, "jj", 2);
	ok = md_wait_text(g, "Sub Heading", 10000);
	product_snapshot(pr, "markdown_preview");
	ev_check(e, "markdown_preview_shows_headings", 0,
	        "Sub Heading on screen", ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	ok = NULL == strstr(pr->screen, "## Sub") &&
	        NULL == strstr(pr->screen, "**strong**") &&
	        NULL != strstr(pr->screen, "- apple") &&
	        NULL != strstr(pr->screen, "| a   | b   |");
	ev_check(e, "markdown_preview_has_no_marks_but_a_list_and_a_table", 0,
	        "no ## or **, a list, a table", ok ? "observed" : "missing",
	        ok);
	/* the console host re-writes the attributes one by one: H1 turns bold
	 * and underline on, H2 only the underline off again, strong bold on */
	ok = md_drawn_with(&pr->p, "Big Heading", "\x1b[1m\x1b[4m") &&
	        md_drawn_with(&pr->p, "Sub Heading", "\x1b[24m") &&
	        md_drawn_with(&pr->p, "strong", "\x1b[1m");
	ev_check(e, "markdown_preview_styles_differ_by_level", 0,
	        "H1 bold+underline, H2 bold, strong bold",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	/* the editor: the source on the left, the same text read as Markdown on
	 * the right (160 columns) */
	pty_send(&pr->p, "e", 1);
	ok = md_wait_count(g, "Big Heading", 2, 10000);
	product_snapshot(pr, "markdown_two_panes");
	ev_check(e, "markdown_editor_has_two_panes", 0,
	        "a row with '# Big Heading' and 'Big Heading'",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	/* typing on the left: the right pane follows once the typing stops */
	pty_send(&pr->p, "ZZZ", 3);
	ok = md_wait_count(g, "ZZZ# Big Heading", 2, 5000);
	product_snapshot(pr, "markdown_two_panes_after_typing");
	ev_check(e, "markdown_right_pane_follows_typing", 0,
	        "the typed text on both sides", ok ? "observed" : "missing",
	        ok);
	if (!ok) {
		goto done;
	}
	/* Alt+M hides the right pane, and shows it again */
	pty_send(&pr->p,
	        "\x1b"
	        "m",
	        2);
	Sleep(400);
	pty_snapshot(&pr->p, pr->screen, sizeof pr->screen);
	ok = 1 == md_most_on_a_line(pr->screen, "ZZZ# Big Heading");
	ev_check(e, "markdown_alt_m_hides_the_preview", 0,
	        "the text on the left only", ok ? "observed" : "still two", ok);
	pty_send(&pr->p,
	        "\x1b"
	        "m",
	        2);
	ok = ok && md_wait_count(g, "ZZZ# Big Heading", 2, 5000);
	ev_check(e, "markdown_alt_m_shows_the_preview_again", 0,
	        "the text on both sides", ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	/* take the typing back and leave: nothing was saved */
	pty_send(&pr->p, "\x1a", 1);
	ok = md_wait_count(g, "# Big Heading", 2, 5000);
	pty_send(&pr->p, "\x1b", 1);
	ok = ok && md_wait_text(g, "doc.txt", 10000);
	ev_check(e, "markdown_editor_closes_unmodified", 0,
	        "back at the listing", ok ? "observed" : "missing", ok);
	after = gate_tree_text(g->browse);
	ok = ok && before && after && !strcmp(before, after);
	ev_check(e, "markdown_changes_no_file", 0, "tree as before",
	        ok ? "unchanged" : "CHANGED", ok);
	ok = ok && gate_quit(g);
done:
	free(before);
	free(after);
	return gate_first_run_end(g, e, ok, fails) ? (g_gate_skips ? 77 : 0)
	                                           : 1;
}
