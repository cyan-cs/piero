/*
 * Change notification through the real product: another program creates,
 * renames, rewrites and removes files in the folder on screen, and the screen
 * follows with no key pressed; the cursor stays on its entry; the preview of
 * the file under the cursor follows a rewrite; a burst of changes ends as one
 * settled listing. Also measured on the way (reported in the evidence, not
 * judged against a made-up number): the CPU a quiet program uses, the delay
 * from a change to the screen, and the handles after many folder changes.
 * Included by p0_conpty_host.c after p0_session.c, whose helpers it uses.
 */

#define WATCH_CONFIG                                                           \
	"[meta]\r\nschema_version = 1\r\nonboarding_completed = true\r\n"      \
	"[updates]\r\nenabled = false\r\nconsent_recorded = true\r\n"

#define WATCH_BURST 300

static double watch_cpu_ms(HANDLE process)
{
	FILETIME created, exited, kernel, user;
	ULARGE_INTEGER k, u;

	if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) {
		return -1.0;
	}
	k.LowPart = kernel.dwLowDateTime;
	k.HighPart = kernel.dwHighDateTime;
	u.LowPart = user.dwLowDateTime;
	u.HighPart = user.dwHighDateTime;
	return (double)(k.QuadPart + u.QuadPart) / 10000.0; /* 100 ns -> ms */
}

/* True once `text` is no longer anywhere on the screen, within `ms`. */
static bool watch_gone(Gate *g, const char *text, DWORD ms)
{
	double t0 = now_ms();

	do {
		pty_snapshot(&g->pr.p, g->pr.screen, sizeof g->pr.screen);
		if (!strstr(g->pr.screen, text)) {
			return true;
		}
		Sleep(20);
	} while (now_ms() - t0 < (double)ms);
	return false;
}

/* Waits for `text` and says how long it took, in milliseconds (-1: never). */
static double watch_delay(Gate *g, const char *text, DWORD ms)
{
	double t0 = now_ms();

	return product_wait_ms(
	               &g->pr.p, text, g->pr.screen, sizeof g->pr.screen, ms)
	        ? now_ms() - t0
	        : -1.0;
}

/* Writes `text` as the whole file, the way another program would. */
static bool watch_put(Gate *g, const wchar_t *rel, const char *text)
{
	wchar_t path[1100];

	return gate_path(g, rel, path) && gate_write(path, text);
}

/* Overwrites a file in place with `text` of the same length and puts its times
 * back, so that its row in the listing (size, time) looks untouched: only the
 * change notification says that the content differs. */
static bool watch_put_same_look(Gate *g, const wchar_t *rel, const char *text)
{
	wchar_t path[1100];
	HANDLE h;
	FILETIME created, accessed, written;
	DWORD wrote = 0;
	size_t len = strlen(text);
	bool ok;

	if (!gate_path(g, rel, path)) {
		return false;
	}
	h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	ok = GetFileTime(h, &created, &accessed, &written);
	ok = ok && WriteFile(h, text, (DWORD)len, &wrote, NULL) && wrote == len;
	ok = ok && SetFileTime(h, &created, &accessed, &written);
	CloseHandle(h);
	return ok;
}

static bool watch_case(Ev *e)
{
	Gate *g = calloc(1, sizeof *g);
	Product *pr;
	bool ok = false;
	int fails = e->fails;
	char seen[160];
	DWORD handles0 = 0, handles1 = 0;
	double cpu0, cpu1, delay;
	GateSnap d0, n0;
	wchar_t a[1100], b[1100];

	if (!g || !gate_wanted("watch")) {
		free(g);
		return true;
	}
	if (!gate_open(g, e, "watch") || !gate_write(g->cfg, WATCH_CONFIG) ||
	        !gate_launch(g)) {
		goto done;
	}
	pr = &g->pr;
	/* the program has settled; what it holds now is the baseline */
	Sleep(1500);
	GetProcessHandleCount(pr->p.pi.hProcess, &handles0);
	/* a quiet program uses no CPU: it waits on events, it does not poll */
	cpu0 = watch_cpu_ms(pr->p.pi.hProcess);
	Sleep(5000);
	cpu1 = watch_cpu_ms(pr->p.pi.hProcess);
	ok = cpu0 >= 0 && cpu1 >= 0;
	snprintf(seen, sizeof seen, "%.0f ms of CPU in 5000 ms", cpu1 - cpu0);
	ev_check(e, "watch_idle_cpu_is_measured", 0,
	        "a quiet program waits (no polling)", seen, ok);
	ok = ok && (cpu1 - cpu0) < 250.0; /* 5 %: far above a waiting program */
	ev_check(e, "watch_idle_cpu_is_small", 0, "under 250 ms of CPU in 5 s",
	        seen, ok);
	if (!ok) {
		goto done;
	}

	/* 1. a file made by another program appears with no key */
	ok = watch_put(g, L"browse\\ext1.txt", "external one");
	delay = ok ? watch_delay(g, "ext1.txt", 8000) : -1.0;
	snprintf(seen, sizeof seen, "%s after %.0f ms",
	        delay >= 0 ? "shown" : "never shown", delay);
	ok = delay >= 0;
	ev_check(e, "watch_created_file_appears", 0,
	        "ext1.txt shown with no key", seen, ok);
	if (!ok) {
		goto done;
	}

	/* 2. the cursor stays on its entry when the list changes under it */
	pty_send(&pr->p, "j", 1);
	ok = dirsize_row(g, "> ext1.txt", "", 8000);
	pty_send(&pr->p, "j", 1);
	ok = ok && dirsize_row(g, "> note.txt", "", 8000);
	ok = ok && watch_put(g, L"browse\\aaa.txt", "sorts above everything") &&
	        product_wait_ms(
	                &pr->p, "aaa.txt", pr->screen, sizeof pr->screen, 8000);
	ok = ok && dirsize_row(g, "> note.txt", "", 3000);
	ev_check(e, "watch_cursor_stays_on_its_entry", 0,
	        "> note.txt after aaa.txt came in above",
	        ok ? "observed" : "moved", ok);
	if (!ok) {
		goto done;
	}

	/* 3. a rename and a removal follow */
	ok = gate_path(g, L"browse\\ext1.txt", a) &&
	        gate_path(g, L"browse\\ext2.txt", b) && MoveFileW(a, b);
	ok = ok &&
	        product_wait_ms(&pr->p, "ext2.txt", pr->screen,
	                sizeof pr->screen, 8000) &&
	        watch_gone(g, "ext1.txt", 8000);
	ev_check(e, "watch_renamed_file_follows", 0,
	        "ext2.txt shown, ext1.txt gone", ok ? "observed" : "missing",
	        ok);
	ok = ok && gate_path(g, L"browse\\ext2.txt", a) && DeleteFileW(a) &&
	        watch_gone(g, "ext2.txt", 8000);
	ev_check(e, "watch_removed_file_goes", 0, "ext2.txt gone",
	        ok ? "observed" : "still shown", ok);
	if (!ok) {
		goto done;
	}

	/* 4. the preview of the file under the cursor follows a rewrite */
	ok = product_wait_ms(
	        &pr->p, "hello", pr->screen, sizeof pr->screen, 8000);
	ok = ok &&
	        watch_put(
	                g, L"browse\\note.txt", "REWRITTEN_BY_ANOTHER_PROGRAM");
	delay = ok ? watch_delay(g, "REWRITTEN_BY_ANOTHER_PROGRAM", 8000)
	           : -1.0;
	snprintf(seen, sizeof seen, "%s after %.0f ms",
	        delay >= 0 ? "shown" : "never shown", delay);
	ok = delay >= 0;
	ev_check(e, "watch_preview_follows_a_rewrite", 0,
	        "the new text in the preview with no key", seen, ok);
	ok = ok && dirsize_row(g, "> note.txt", "", 3000);
	ev_check(e, "watch_cursor_survives_the_rewrite", 0, "> note.txt",
	        ok ? "observed" : "moved", ok);
	if (!ok) {
		goto done;
	}

	/* 4b. the same, when the listing row cannot show it: same length, same
	 * times. The notification alone must reload the preview. The text is as
	 * long as the one written above (28 bytes). */
	_Static_assert(sizeof "REWRITTEN_BY_ANOTHER_PROGRAM" ==
	                sizeof "SECOND_WRITE_SAME_SIZE_ABCDE",
	        "the rewrite must keep the length");
	ok = watch_put_same_look(
	        g, L"browse\\note.txt", "SECOND_WRITE_SAME_SIZE_ABCDE");
	delay = ok ? watch_delay(g, "SECOND_WRITE_SAME_SIZE_ABCDE", 8000)
	           : -1.0;
	snprintf(seen, sizeof seen, "%s after %.0f ms",
	        delay >= 0 ? "shown" : "never shown", delay);
	ok = delay >= 0;
	ev_check(e, "watch_preview_follows_a_rewrite_that_looks_unchanged", 0,
	        "the new text in the preview although size and time are the "
	        "same",
	        seen, ok);
	if (!ok) {
		goto done;
	}

	/* 5. a burst of changes ends as one settled listing: every file is there */
	{
		double t0 = now_ms();
		char needle[48];

		for (int i = 0; ok && i < WATCH_BURST; i++) {
			wchar_t rel[64];

			swprintf(rel, 64, L"browse\\b%03d.txt", i);
			ok = watch_put(g, rel, "x");
		}
		/* aaa, doc, note and the burst */
		snprintf(needle, sizeof needle, "| %d items", WATCH_BURST + 3);
		ok = ok &&
		        product_wait_ms(&pr->p, needle, pr->screen,
		                sizeof pr->screen, 20000);
		snprintf(seen, sizeof seen,
		        "%s, %d files made and shown in %.0f ms",
		        ok ? "all there" : "NOT all there", WATCH_BURST,
		        now_ms() - t0);
		ev_check(e, "watch_burst_settles_to_the_whole_listing", 0,
		        "the listing holds every file once the burst is over",
		        seen, ok);
	}
	if (!ok) {
		goto done;
	}

	/* 6. moving between folders opens and closes a watch each time: the
	 * program's handles do not grow with it */
	for (int i = 0; ok && i < 25; i++) {
		pty_send(&pr->p, "h", 1);
		ok = product_wait_ms(
		        &pr->p, "dest/", pr->screen, sizeof pr->screen, 8000);
		pty_send(&pr->p, "l", 1);
		ok = ok &&
		        product_wait_ms(&pr->p, "note.txt", pr->screen,
		                sizeof pr->screen, 8000);
	}
	Sleep(800);
	GetProcessHandleCount(pr->p.pi.hProcess, &handles1);
	snprintf(seen, sizeof seen,
	        "%lu handles before, %lu after 25 round trips",
	        (unsigned long)handles0, (unsigned long)handles1);
	ok = ok && handles1 <= handles0 + 4;
	ev_check(e, "watch_handles_do_not_grow_with_navigation", 0,
	        "no more than 4 above the baseline", seen, ok);
	if (!ok) {
		goto done;
	}

	/* 7. nothing in the folder was changed by the program itself */
	ok = gate_snap(g->doc, &d0) && gate_same(&d0, &g->doc0);
	(void)n0;
	ev_check(e, "watch_program_changed_no_file", 0, "doc.txt as it was",
	        ok ? "unchanged" : "CHANGED", ok);
	ok = ok && gate_quit(g);
done:
	return gate_first_run_end(g, e, ok, fails);
}

/* The file under the editor is rewritten by another program: the editor says so,
 * once, with no key; the unsaved text is kept; a save asks the question it always
 * asked and the other program's text on the disk is not overwritten. */
static bool watch_editor_case(Ev *e)
{
	Gate *g = calloc(1, sizeof *g);
	Product *pr;
	bool ok = false;
	int fails = e->fails;
	char text[256], seen[160];
	double delay;

	if (!g || !gate_wanted("watch_editor")) {
		free(g);
		return true;
	}
	if (!gate_open(g, e, "watch_editor") ||
	        !gate_write(g->cfg, WATCH_CONFIG) || !gate_launch(g)) {
		goto done;
	}
	pr = &g->pr;
	pty_send(&pr->p, "e", 1);
	ok = product_wait_ms(
	        &pr->p, "Ln 1/3", pr->screen, sizeof pr->screen, 8000);
	pty_send(&pr->p, "Q", 1);
	ok = ok &&
	        product_wait_ms(&pr->p, "[Modified]", pr->screen,
	                sizeof pr->screen, 8000);
	if (!ok) {
		goto done;
	}
	/* another program rewrites the file */
	ok = gate_write(g->doc, "EXTERNALLY CHANGED\r\n");
	delay = ok ? watch_delay(g, "changed by another program", 8000) : -1.0;
	snprintf(seen, sizeof seen, "%s after %.0f ms",
	        delay >= 0 ? "said" : "never said", delay);
	ok = delay >= 0;
	product_snapshot(pr, "watch_editor_noticed");
	ev_check(e, "watch_editor_says_the_file_changed", 0,
	        "the editor says so with no key", seen, ok);
	if (!ok) {
		goto done;
	}
	/* the person's text is as it was */
	ok = NULL != strstr(pr->screen, "QGATE_SOURCE_MARKER") &&
	        NULL != strstr(pr->screen, "[Modified]");
	ev_check(e, "watch_editor_keeps_the_unsaved_text", 0,
	        "QGATE_SOURCE_MARKER, still [Modified]",
	        ok ? "kept" : "CHANGED", ok);
	/* saving asks, as it always did; cancelling leaves the other text alone */
	pty_send(&pr->p, "\x13", 1);
	ok = ok &&
	        product_wait_ms(&pr->p, "changed on disk", pr->screen,
	                sizeof pr->screen, 8000);
	ev_check(e, "watch_editor_save_asks_about_the_change", 0,
	        "the editor's conflict question", ok ? "asked" : "missing", ok);
	pty_send(&pr->p, "c", 1);
	Sleep(300);
	ok = ok && read_text_file(g->doc, text, sizeof text) &&
	        !strcmp(text, "EXTERNALLY CHANGED\r\n");
	ev_check(e, "watch_editor_other_programs_text_is_not_overwritten", 0,
	        "the disk still holds what the other program wrote",
	        ok ? "kept" : "OVERWRITTEN", ok);
	if (!ok) {
		goto done;
	}
	/* leave without saving: nothing of the editor's text reaches the file */
	pty_send(&pr->p, "\x18", 1);
	ok = product_wait_ms(
	        &pr->p, "Save changes to", pr->screen, sizeof pr->screen, 8000);
	pty_send(&pr->p, "d", 1);
	ok = ok &&
	        product_wait_ms(&pr->p, "NORMAL", pr->screen, sizeof pr->screen,
	                8000) &&
	        read_text_file(g->doc, text, sizeof text) &&
	        !strcmp(text, "EXTERNALLY CHANGED\r\n");
	ev_check(e, "watch_editor_discard_leaves_the_file_alone", 0,
	        "unchanged", ok ? "unchanged" : "changed", ok);
	ok = ok && gate_quit(g);
done:
	return gate_first_run_end(g, e, ok, fails);
}

/* The folder under the cursor shows what is inside it in the preview. Another
 * program adds a file inside that folder: the folder being watched has no new
 * entry, only the one under the cursor changed, and the preview must still
 * follow (the "preview" half of the notification). */
static bool watch_subfolder_case(Ev *e)
{
	Gate *g = calloc(1, sizeof *g);
	Product *pr;
	bool ok = false;
	int fails = e->fails;
	char seen[160];
	double delay;

	if (!g || !gate_wanted("watch_subfolder")) {
		free(g);
		return true;
	}
	if (!gate_open(g, e, "watch_subfolder") ||
	        !gate_write(g->cfg, WATCH_CONFIG) ||
	        !gate_mkdir(g, L"browse\\sub") ||
	        !watch_put(g, L"browse\\sub\\one.txt", "1") ||
	        !gate_launch(g)) {
		goto done;
	}
	pr = &g->pr;
	/* the first entry is the folder: its content is in the preview */
	ok = dirsize_row(g, "> sub", "", 8000) &&
	        product_wait_ms(
	                &pr->p, "one.txt", pr->screen, sizeof pr->screen, 8000);
	ev_check(e, "watch_subfolder_preview_starts_with_its_content", 0,
	        "> sub/ and one.txt in the preview",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	ok = watch_put(g, L"browse\\sub\\two.txt", "2");
	delay = ok ? watch_delay(g, "two.txt", 8000) : -1.0;
	snprintf(seen, sizeof seen, "%s after %.0f ms",
	        delay >= 0 ? "shown" : "never shown", delay);
	ok = delay >= 0;
	product_snapshot(pr, "watch_subfolder_followed");
	ev_check(e, "watch_subfolder_preview_follows_a_change_inside", 0,
	        "two.txt in the preview with no key", seen, ok);
	ok = ok && dirsize_row(g, "> sub", "", 3000) && gate_quit(g);
done:
	return gate_first_run_end(g, e, ok, fails);
}

static int scn_watch(Ev *e)
{
	bool good = watch_case(e);

	good = watch_editor_case(e) && good;
	good = watch_subfolder_case(e) && good;

	if (!good || e->fails) {
		return 1;
	}
	return g_gate_skips ? 77 : 0;
}
