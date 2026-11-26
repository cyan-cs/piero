/*
 * Pictures through the real product, under the conditions a person creates and
 * an attacker chooses (DR-DRAFT-039): 300 pictures stepped through at key-repeat
 * speed in both directions, a move away in the middle of a decode, 8K and 16K
 * pictures, files that are damaged, cut off, empty or claim absurd sizes, a storm
 * of terminal resizes, text and Markdown and PDF next to them, and the decoder
 * coming and going 8 times with the program's threads, handles and private bytes
 * counted at the start and the end. Included by p0_conpty_host.c after p0_git.c,
 * whose and p0_resources.c's helpers it uses.
 *
 * Folder layout (the list order is the order of the cursor moves):
 *   p000.bmp .. p299.bmp   picture k is (16 + k % 64) x (8 + k / 64): its size is
 *                          on screen, so what is shown says which file it is
 *   q1-8k.png, q2-16k.png  pictures that are fine and big
 *   q3-bomb.png            30000 x 30000: refused by size
 *   q4-liar.bmp            a header claiming 40000 x 40000 and no pixels
 *   q4b-ok.bmp             8 x 8, between the two refusals so that one is not
 *                          taken for the other (they say the same words)
 *   q5-cut.png             an 8K picture cut in half
 *   r1.txt, r2.md, r3.pdf  not pictures (r3 is a PDF page)
 */

#include <ctype.h>

#define IMG_N 300u

static void img_dims(unsigned k, unsigned *w, unsigned *h)
{
	*w = 16 + k % 64;
	*h = 8 + k / 64;
}

static size_t img_bmp(unsigned char *out, unsigned w, unsigned h)
{
	size_t row = ((size_t)w * 3 + 3) & ~(size_t)3, size = 54 + row * h;

	memset(out, 0, size);
	out[0] = 'B';
	out[1] = 'M';
	out[2] = (unsigned char)size;
	out[3] = (unsigned char)(size >> 8);
	out[10] = 54;
	out[14] = 40;
	out[18] = (unsigned char)w;
	out[22] = (unsigned char)h;
	out[26] = 1;
	out[28] = 24;
	for (unsigned y = 0; y < h; y++) {
		for (unsigned x = 0; x < w; x++) {
			unsigned char *px =
			        out + 54 + (size_t)y * row + (size_t)x * 3;

			px[0] = (unsigned char)(40 + x);
			px[1] = (unsigned char)(y * 20);
			px[2] = (unsigned char)(x * 4);
		}
	}
	return size;
}

/* A PNG of zero pixels that claims w x h: see tests/wf_imgfix.h. */
#include "../../tests/wf_imgfix.h"

/* Whether the screen says "w x h" as a whole (not as the end of 116 x 8). */
static bool img_screen_has(const char *s, unsigned w, unsigned h)
{
	char needle[40];
	size_t n;

	snprintf(needle, sizeof needle, "%u x %u", w, h);
	n = strlen(needle);
	for (const char *p = s; (p = strstr(p, needle)) != NULL; p++) {
		char before = p > s ? p[-1] : ' ', after = p[n];

		if (!isdigit((unsigned char)before) &&
		        !isdigit((unsigned char)after)) {
			return true;
		}
	}
	return false;
}

/* Waits (bounded) for the picture of that size; peak: the most the decoder
 * process was seen to hold meanwhile (0 when not wanted). */
static bool img_wait_dims(
        Gate *g, unsigned w, unsigned h, DWORD ms, double *peak)
{
	ULONGLONG end = GetTickCount64() + ms;
	DWORD pid = GetProcessId(g->pr.p.pi.hProcess);
	char needle[40];

	snprintf(needle, sizeof needle, "%u x %u", w, h);
	while (GetTickCount64() < end) {
		(void)product_wait_ms(&g->pr.p, needle, g->pr.screen,
		        sizeof g->pr.screen, 150);
		if (peak) {
			double b = res_children_bytes(pid);

			*peak = b > *peak ? b : *peak;
		}
		if (img_screen_has(g->pr.screen, w, h)) {
			return true;
		}
	}
	return false;
}

/* Moves the cursor by `steps` rows (negative: up) as a held key does: in bursts
 * with no wait for the screen between them. */
static void img_move(Gate *g, int steps)
{
	char burst[64];
	int left = steps < 0 ? -steps : steps;

	memset(burst, steps < 0 ? 'k' : 'j', sizeof burst);
	while (left > 0) {
		int n = left < (int)sizeof burst ? left : (int)sizeof burst;

		pty_write(&g->pr.p, burst, (size_t)n);
		left -= n;
		Sleep(2);
	}
}

static bool img_alive(Gate *g)
{
	return WAIT_TIMEOUT == WaitForSingleObject(g->pr.p.pi.hProcess, 0);
}

/* Resize invalidates the rendered image. Wait for the final frame's complete
 * predicate, including pixels: dimensions alone can arrive before decoding.
 * The black 16K fixture is drawn as blanks; only the gradient has visible cells. */
static bool img_wait_resized(Gate *g, unsigned w, unsigned h, bool black)
{
	ULONGLONG end = GetTickCount64() + 30000;

	do {
		pty_snapshot(&g->pr.p, g->pr.screen, sizeof g->pr.screen);
		if (!img_alive(g)) {
			return false;
		}
		if (img_screen_has(g->pr.screen, w, h) &&
		        strstr(g->pr.screen, "NORMAL") &&
		        (black ||
		                count_text(g->pr.screen, "\xE2\x96\x80") > 20 ||
		                count_text(g->pr.screen, "=") > 10)) {
			return true;
		}
		Sleep(50);
	} while (GetTickCount64() < end);
	return false;
}

/* The picture of file k is shown and no other of the 300 is (a picture that comes
 * late for a file already passed would show its own size here). */
static bool img_only(Gate *g, unsigned k)
{
	unsigned w, h;

	img_dims(k, &w, &h);
	pty_snapshot(&g->pr.p, g->pr.screen, sizeof g->pr.screen);
	if (!img_screen_has(g->pr.screen, w, h)) {
		return false;
	}
	for (unsigned o = 0; o < IMG_N; o++) {
		unsigned ow, oh;

		img_dims(o, &ow, &oh);
		if (o != k && (ow != w || oh != h) &&
		        img_screen_has(g->pr.screen, ow, oh)) {
			return false;
		}
	}
	return true;
}

static bool img_make_files(Gate *g)
{
	unsigned char bmp[54 + 4 * 128 * 16];
	wchar_t rel[96];
	size_t n;
	uint8_t *p;

	for (unsigned k = 0; k < IMG_N; k++) {
		unsigned w, h;

		img_dims(k, &w, &h);
		n = img_bmp(bmp, w, h);
		swprintf(rel, 96, L"browse\\p%03u.bmp", k);
		if (!product_file_bytes(&g->pr.lab, rel, bmp, n)) {
			return false;
		}
	}
	p = fix_png_zeros(7680, 4320, 2, 0, &n);
	if (!product_file_bytes(&g->pr.lab, L"browse\\q1-8k.png", p, n)) {
		return false;
	}
	free(p);
	p = fix_png_zeros(15360, 8640, 2, 1, &n);
	if (!product_file_bytes(&g->pr.lab, L"browse\\q2-16k.png", p, n)) {
		return false;
	}
	free(p);
	p = fix_png_zeros(30000, 30000, 0, 0, &n);
	if (!product_file_bytes(&g->pr.lab, L"browse\\q3-bomb.png", p, n)) {
		return false;
	}
	free(p);
	p = fix_png_zeros(7680, 4320, 2, 0, &n);
	if (!product_file_bytes(&g->pr.lab, L"browse\\q5-cut.png", p, n / 2)) {
		return false;
	}
	free(p);
	p = fix_bmp(40000, 40000, 24, 0, &n);
	if (!product_file_bytes(&g->pr.lab, L"browse\\q4-liar.bmp", p, n)) {
		return false;
	}
	free(p);
	{
		unsigned char ok_bmp[54 + 8 * 24];

		if (!product_file_bytes(&g->pr.lab, L"browse\\q4b-ok.bmp",
		            ok_bmp, img_bmp(ok_bmp, 8, 8))) {
			return false;
		}
	}
	return product_file_bytes(&g->pr.lab, L"browse\\r1.txt",
	               "hello text preview\n", 19) &&
	        product_file_bytes(&g->pr.lab, L"browse\\r2.md",
	                "# Markdown Heading\n\nsome body\n", 30) &&
	        product_file_bytes(&g->pr.lab, L"browse\\r3.pdf", pdf_two_page,
	                sizeof pdf_two_page);
}

static bool images_case(Ev *e)
{
	Gate *g = calloc(1, sizeof *g);
	static ResReport rep;
	bool ok = false, all;
	int fails = e->fails;
	DWORD pid;
	ResSample idle, base, end_s;
	unsigned cur = 0;
	double peak = 0, t0;

	memset(&idle, 0, sizeof idle);
	memset(&base, 0, sizeof base);
	memset(&end_s, 0, sizeof end_s);

	if (!g || !gate_wanted("images")) {
		free(g);
		return true;
	}
	if (!gate_open(g, e, "images") ||
	        !gate_write(g->cfg,
	                DIRSIZE_CONFIG "[preview]\r\nimages = true\r\n") ||
	        !img_make_files(g) ||
	        !SetEnvironmentVariableW(L"PIERO_IDLE_RELEASE_MS", L"3000")) {
		goto done;
	}
	g->wait_for = "p000.bmp";
	if (!gate_launch(g)) {
		goto done;
	}
	pid = GetProcessId(g->pr.p.pi.hProcess);
	Sleep(1500);
	rep.n = 0;
	rep.text[0] = 0;
	if (!res_sample(g, &idle)) {
		goto done;
	}
	res_line(&rep, "idle", &idle);

	/* 1. the first picture, and a picture 5 rows down: shown, with its own size */
	{
		unsigned w, h;

		img_move(g,
		        2); /* the fixture's doc.txt and note.txt come first */
		img_dims(0, &w, &h);
		ok = img_wait_dims(g, w, h, 30000, NULL);
		ev_check(e, "images_first_picture_is_shown", 0, "16 x 8",
		        ok ? "observed" : "missing", ok);
		if (!ok) {
			product_snapshot(&g->pr, "images_first_missing");
			goto done;
		}
		img_move(g, 5);
		cur = 5;
		img_dims(cur, &w, &h);
		ok = img_wait_dims(g, w, h, 10000, NULL) && img_only(g, cur);
		ev_check(e, "images_each_picture_shows_its_own_size", 0,
		        "21 x 8 and no other", ok ? "observed" : "WRONG", ok);
		if (!ok) {
			goto done;
		}
	}

	/* 2. the list stepped through at key-repeat speed, 8 passes of 299 rows, and
	 * after each the last row's picture is shown and no picture of a row on the
	 * way is (nothing late comes back for a row that was passed) */
	all = true;
	t0 = now_ms();
	{
		static const unsigned stops[8] = {
		        299, 0, 299, 150, 0, 299, 7, 299};
		unsigned w, h;
		double worst = 0;

		for (int i = 0; i < 8 && all; i++) {
			double p0 = now_ms();

			img_move(g, (int)stops[i] - (int)cur);
			cur = stops[i];
			img_dims(cur, &w, &h);
			all = img_wait_dims(g, w, h, 30000, NULL);
			worst = now_ms() - p0 > worst ? now_ms() - p0 : worst;
			Sleep(1200); /* anything late arrives in this time */
			all = all && img_only(g, cur) && img_alive(g);
			if (!all) {
				char why[48];

				snprintf(why, sizeof why,
				        "images_pass_%d_stopped", i);
				product_snapshot(&g->pr, why);
			}
		}
		res_add(&rep, "fast_passes_total_ms=%.0f worst_pass_ms=%.0f\n",
		        now_ms() - t0, worst);
	}
	ev_check(e, "images_fast_navigation_settles_on_the_last_picture", 0,
	        "8 passes over 300 pictures: the last row's picture, no stale "
	        "one",
	        all ? "observed" : "WRONG", all);
	if (!all) {
		goto done;
	}

	/* 3. a move away in the middle of a decode: the picture of the row the cursor
	 * left is never shown, however long after */
	all = true;
	for (int i = 0; i < 16 && all; i++) {
		unsigned w, h;
		bool deep = 0 == i % 3;

		pty_write(&g->pr.p, deep ? "jjkk" : "jk", deep ? 4 : 2);
		Sleep(500);
		img_dims(cur, &w, &h);
		pty_snapshot(&g->pr.p, g->pr.screen, sizeof g->pr.screen);
		all = img_screen_has(g->pr.screen, w, h) &&
		        !img_screen_has(g->pr.screen, 7680, 4320) &&
		        !img_screen_has(g->pr.screen, 15360, 8640);
	}
	Sleep(1500);
	pty_snapshot(&g->pr.p, g->pr.screen, sizeof g->pr.screen);
	all = all && !img_screen_has(g->pr.screen, 7680, 4320) &&
	        !img_screen_has(g->pr.screen, 15360, 8640) && img_alive(g);
	if (!all) {
		product_snapshot(&g->pr, "images_late_picture");
	}
	ev_check(e, "images_a_move_during_a_decode_never_shows_the_old_picture",
	        0, "16 moves to a big picture and back: it is never on screen",
	        all ? "observed" : "WRONG", all);
	if (!all) {
		goto done;
	}

	/* 4. the big ones and the files that lie, one after the other */
	pty_write(&g->pr.p, "j", 1); /* q1: 8K */
	ok = img_wait_dims(g, 7680, 4320, 30000, NULL);
	ev_check(e, "images_8k_is_shown_reduced", 0, "7680 x 4320",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		product_snapshot(&g->pr, "images_8k_missing");
		goto done;
	}
	pty_write(&g->pr.p, "j", 1); /* q2: 16K, interlaced */
	peak = 0;
	t0 = now_ms();
	ok = img_wait_dims(g, 15360, 8640, 30000, &peak);
	t0 = now_ms() - t0;
	res_add(&rep,
	        "decoder_peak_private_bytes_16k=%.0f shown_after_ms=%.0f\n",
	        peak, t0);
	ev_check(e, "images_16k_is_shown_reduced_within_the_memory_wall", 0,
	        "15360 x 8640 within 512 MiB and 12 s",
	        ok && peak < 512.0 * 1048576.0 && t0 < 12000.0 ? "observed"
	                                                       : "WRONG",
	        ok && peak < 512.0 * 1048576.0 && t0 < 12000.0);
	if (!ok) {
		product_snapshot(&g->pr, "images_16k_missing");
		goto done;
	}
	/* the program answers a key at once while it has just decoded 16K */
	{
		double t1 = now_ms();

		pty_write(&g->pr.p, "j", 1); /* q3: 30000 x 30000 */
		ok = picture_expect(&g->pr, "images_bomb_is_refused_by_size",
		        "too large", 20000);
		t1 = now_ms() - t1;
		res_add(&rep, "bomb_refused_after_ms=%.0f\n", t1);
		if (!ok) {
			goto done;
		}
		picture_no_internal_names(
		        &g->pr, "images_refusals_have_no_internal_names");
	}
	pty_write(
	        &g->pr.p, "j", 1); /* q4: a header that claims 40000 x 40000 */
	ok = picture_expect(&g->pr, "images_lying_header_is_refused",
	        "damaged or cut off", 20000);
	if (!ok) {
		goto done;
	}
	pty_write(&g->pr.p, "j",
	        1); /* q4b: a picture, so that what follows is new */
	ok = img_wait_dims(g, 8, 8, 20000, NULL);
	pty_write(&g->pr.p, "j", 1); /* q5: cut in half */
	ok = ok &&
	        picture_expect(&g->pr, "images_cut_picture_is_refused",
	                "damaged or cut off", 20000);
	if (!ok) {
		goto done;
	}
	/* not pictures: the previews that were always there */
	pty_write(&g->pr.p, "j", 1);
	ok = picture_expect(&g->pr, "images_text_preview_is_as_ever",
	        "hello text preview", 20000);
	pty_write(&g->pr.p, "j", 1);
	ok = ok &&
	        picture_expect(&g->pr, "images_markdown_preview_is_as_ever",
	                "Markdown Heading", 20000) &&
	        NULL == strstr(g->pr.screen, "# Markdown Heading");
	pty_write(&g->pr.p, "j", 1);
	ok = ok &&
	        picture_expect(&g->pr, "images_pdf_preview_is_as_ever",
	                "page 1/2", 30000);
	if (!ok) {
		goto done;
	}
	cur = IMG_N + 8; /* r3 */

	/* 5. a storm of terminal resizes, on a big picture and on a small one: the
	 * program lives, the picture is drawn again for the final size */
	{
		static const int sizes[][2] = {{60, 20}, {200, 60}, {40, 12},
		        {100, 30}, {20, 5}, {160, 40}, {80, 24}, {30, 10},
		        {140, 45}, {50, 15}};
		static const unsigned stay[2] = {IMG_N + 1, 150};
		unsigned w, h;

		all = true;
		for (int round = 0; round < 2 && all; round++) {
			img_move(g, (int)stay[round] - (int)cur);
			cur = stay[round];
			if (IMG_N + 1 == cur) {
				w = 15360;
				h = 8640;
			} else {
				img_dims(cur, &w, &h);
			}
			ok = img_wait_dims(g, w, h, 30000, NULL);
			for (int i = 0; ok && i < 40; i++) {
				pty_resize(&g->pr.p, sizes[i % 10][0],
				        sizes[i % 10][1]);
				Sleep(30 + (DWORD)(i % 3) * 40);
			}
			pty_resize(&g->pr.p, 120, 30);
			all = ok && img_wait_resized(g, w, h, IMG_N + 1 == cur);
			if (!all) {
				char why[48];

				printf("images resize: round=%d initial=%d "
				       "alive=%d "
				       "dimensions=%d normal=%d blocks=%zu "
				       "ascii=%zu\n",
				        round, ok, img_alive(g),
				        img_screen_has(g->pr.screen, w, h),
				        NULL != strstr(g->pr.screen, "NORMAL"),
				        count_text(
				                g->pr.screen, "\xE2\x96\x80"),
				        count_text(g->pr.screen, "="));
				fflush(stdout);
				snprintf(why, sizeof why,
				        "images_resize_round_%d", round);
				product_snapshot(&g->pr, why);
			}
		}
		ev_check(e, "images_resize_storm_leaves_a_sound_screen", 0,
		        "2 x 40 resizes: alive, the picture drawn at the final "
		        "size",
		        all ? "observed" : "WRONG", all);
		if (!all) {
			goto done;
		}
	}

	/* 6. the decoder comes and goes 8 times: 7 rows up from r1 is p299 (past the
	 * big ones), then back to r1 and wait for the release */
	{
		bool gone = true, cycled = true;
		unsigned w, h;

		img_move(g, (int)(IMG_N + 6) - (int)cur); /* r1 */
		cur = IMG_N + 6;
		cycled = product_wait_ms(&g->pr.p, "hello text preview",
		        g->pr.screen, sizeof g->pr.screen, 20000);
		Sleep(4500);
		for (int i = 0; i < 8 && cycled; i++) {
			img_move(g, -7);
			img_dims(299, &w, &h);
			cycled = img_wait_dims(g, w, h, 30000, NULL);
			img_move(g, 7);
			cycled = cycled &&
			        product_wait_ms(&g->pr.p, "hello text preview",
			                g->pr.screen, sizeof g->pr.screen,
			                20000);
			Sleep(4500); /* the idle time is 3 s */
			if (i >= 1) {
				gone = gone && 0 == res_children(pid);
			}
			if (1 == i && !res_sample(g, &base)) {
				cycled = false;
			}
		}
		cycled = cycled && res_sample(g, &end_s);
		ok = cycled && gone && end_s.threads <= base.threads &&
		        end_s.handles <= base.handles + 4 &&
		        (RES_SANITIZED ||
		                end_s.private_bytes <=
		                        base.private_bytes + 4.0 * 1048576.0);
		res_line(&rep, "after_cycle_2", &base);
		res_line(&rep, "after_cycle_8", &end_s);
		ev_check(e, "images_decoder_cycles_leave_nothing_behind", 0,
		        "8 cycles: the decoder process gone after each idle "
		        "time; "
		        "threads and handles (4 more at most) and private "
		        "bytes (4 "
		        "MiB) as after the 2nd",
		        ok ? "observed" : "WRONG", ok);
		if (!ok) {
			goto done;
		}
		cur = IMG_N + 6;
	}

	/* 7. the program is as light as at the start, and the list is still the
	 * file manager it was: a folder listing is not held up by anything */
	Sleep(1500);
	ok = res_sample(g, &end_s) && end_s.threads <= idle.threads + 1 &&
	        0 == res_children(pid) && img_alive(g);
	res_line(&rep, "end", &end_s);
	ev_check(e, "images_the_program_is_as_light_as_at_the_start", 0,
	        "threads as at rest (1 more at most), no decoder process",
	        ok ? "observed" : "WRONG", ok);
	ev_write_text(e, L"artifacts\\images-stress.txt", rep.text);
done:
	return gate_first_run_end(g, e, ok, fails);
}

/* With pictures off (the setting a person may choose): the preview says so, and
 * the text next to it is as ever. */
static bool images_off_case(Ev *e)
{
	Gate *g = calloc(1, sizeof *g);
	bool ok = false;
	int fails = e->fails;
	unsigned char bmp[54 + 4 * 24 * 8];

	if (!g || !gate_wanted("images-off")) {
		free(g);
		return true;
	}
	if (!gate_open(g, e, "images_off") ||
	        !gate_write(g->cfg,
	                DIRSIZE_CONFIG "[preview]\r\nimages = false\r\n") ||
	        !product_file_bytes(&g->pr.lab, L"browse\\a.bmp", bmp,
	                img_bmp(bmp, 24, 8)) ||
	        !product_file_bytes(
	                &g->pr.lab, L"browse\\b.txt", "plain words\n", 12)) {
		goto done;
	}
	g->wait_for = "a.bmp";
	if (!gate_launch(g)) {
		goto done;
	}
	ok = picture_expect(
	        &g->pr, "images_off_says_so", "Picture preview is off", 20000);
	pty_write(&g->pr.p, "j", 1);
	ok = ok &&
	        picture_expect(&g->pr, "images_off_text_is_as_ever",
	                "plain words", 20000) &&
	        0 == res_children(GetProcessId(g->pr.p.pi.hProcess));
	ev_check(e, "images_off_starts_no_decoder", 0,
	        "no child process while pictures are off",
	        ok ? "observed" : "WRONG", ok);
done:
	return gate_first_run_end(g, e, ok, fails);
}

static int scn_images(Ev *e)
{
	int prerequisite = require_appcontainer(e);
	if (prerequisite) {
		return prerequisite;
	}
	bool ok = images_case(e);

	ok = images_off_case(e) && ok;
	return ok ? (g_gate_skips ? 77 : 0) : 1;
}
