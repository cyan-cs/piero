/*
 * The diff pane of the editor (DR-DRAFT-038), with a fake file and a Git port
 * that only records what is asked: the base of the file at HEAD comes once, the
 * text (saved or not) is compared after the typing has stopped, an answer that is
 * not the newest is never shown, a narrow terminal and every failure leave the
 * editor as it is without Git, and the pane follows the cursor's line.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "edit/wf_egitpane.h"
#include "edit/wf_esession.h"
#include "edit/wf_esession_internal.h"
#include "git/wf_gitport.h"
#include "render/wf_theme.h"
#include "wf_test.h"

static const wchar_t *const FILE_A = L"C:\\repo\\a.c";
static const wchar_t *const FILE_B = L"C:\\repo\\b.c";
static const wchar_t *const FILE_MD = L"C:\\repo\\README.md";
static char data_a[8192] = "one\ntwo\nthree\n";
static char data_b[8192] = "b1\nb2\n";
static char data_md[8192] = "# title\n\ntext\n";

static const char *data_of(const wchar_t *p)
{
	return !wcscmp(p, FILE_A)    ? data_a
	        : !wcscmp(p, FILE_B) ? data_b
	                             : data_md;
}

static WfStatus fake_load(void *ctx,
        const wchar_t *path,
        size_t max,
        unsigned cp,
        const WfEncoding *force,
        WfLoaded *out)
{
	const char *d = data_of(path);
	size_t n = strlen(d);

	(void)ctx;
	(void)max;
	(void)cp;
	(void)force;
	memset(out, 0, sizeof *out);
	out->verdict = WF_FL_OK;
	out->enc.kind = WF_ENC_UTF8;
	out->utf8 = malloc(n + 1);
	memcpy(out->utf8, d, n + 1);
	out->utf8_n = n;
	out->stamp.valid = true;
	out->stamp.size = n;
	out->stamp.links = 1;
	return WF_OK;
}

static WfStatus fake_over(void *ctx,
        const wchar_t *p,
        const WfFileStamp *was,
        const char *u,
        size_t n,
        WfEncoding enc,
        const WfSaveOptions *opt,
        WfSaveResult *out)
{
	(void)ctx;
	(void)p;
	(void)was;
	(void)u;
	(void)n;
	(void)enc;
	(void)opt;
	memset(out, 0, sizeof *out);
	return WF_E_ACCESS;
}

static WfStatus fake_new(void *ctx,
        const wchar_t *p,
        const char *u,
        size_t n,
        WfEncoding enc,
        WfSaveResult *out)
{
	(void)ctx;
	(void)p;
	(void)u;
	(void)n;
	(void)enc;
	memset(out, 0, sizeof *out);
	return WF_E_ACCESS;
}

static WfStatus fake_clip_get(void *ctx, char **u, size_t *n)
{
	(void)ctx;
	*u = NULL;
	*n = 0;
	return WF_E_NOENT;
}

static WfStatus fake_clip_set(void *ctx, const char *u, size_t n)
{
	(void)ctx;
	(void)u;
	(void)n;
	return WF_OK;
}

typedef struct
{
	int bases, diffs, cancels;
	uint32_t base_gen, diff_gen;
	uint64_t base_id, diff_id;
	wchar_t base_path[260];
	char text[8192];
	size_t text_n;
	bool diff_has_base;
	WfStatus base_result, diff_result;
} Rec;

static Rec rec;

static WfStatus g_base(
        void *ctx, uint32_t gen, uint64_t id, const wchar_t *path)
{
	(void)ctx;
	rec.bases++;
	rec.base_gen = gen;
	rec.base_id = id;
	wcscpy_s(rec.base_path, 260, path);
	return rec.base_result;
}

static WfStatus g_diff(void *ctx,
        uint32_t gen,
        uint64_t id,
        WfGitBlob *base,
        const char *text,
        size_t n)
{
	(void)ctx;
	rec.diffs++;
	rec.diff_gen = gen;
	rec.diff_id = id;
	rec.diff_has_base = NULL != base;
	rec.text_n = n < sizeof rec.text - 1 ? n : sizeof rec.text - 1;
	memcpy(rec.text, text, rec.text_n);
	rec.text[rec.text_n] = '\0';
	return rec.diff_result;
}

static void g_cancel(void *ctx)
{
	(void)ctx;
	rec.cancels++;
}

static WfEditSession *S;
static int COLS = 120;
#define ROWS 24
static uint64_t now;

static void setup(bool with_git)
{
	WfEditConfig c;
	WfEditIo io = {NULL, fake_load, fake_over, fake_new, fake_clip_get,
	        fake_clip_set, NULL, NULL, NULL, NULL, NULL};

	wf_es_free(S);
	memset(&rec, 0, sizeof rec);
	rec.base_result = WF_OK;
	wf_es_config_default(&c);
	S = wf_es_new(&c, io);
	CHECK(S);
	if (with_git) {
		WfGitPort p = {NULL, NULL, NULL, g_base, g_diff, g_cancel};

		wf_es_set_git(S, p);
	}
	COLS = 120;
	now = 1000;
	wf_es_set_clock(S, now);
}

static bool open_file(const wchar_t *path)
{
	char why[160];

	return WF_OPEN_OK == wf_es_open(S, path, why, sizeof why);
}

static void type(const char *chars)
{
	for (; *chars; chars++) {
		WfEvent e = {WF_EV_CHAR, (uint32_t)(unsigned char)*chars,
		        WF_KEY_NONE, 0};

		(void)wf_es_event(S, &e, ROWS, COLS);
	}
}

/* The screen, one string per row, joined. */
static char screen[ROWS * 400];

static void draw(void)
{
	WfFrame *f = wf_frame_new(COLS, ROWS, &(WfStyle){0, 0, 0});

	CHECK(f);
	wf_es_draw(S, f);
	screen[0] = '\0';
	for (int y = 0; y < f->rows; y++) {
		char row[800] = "";

		for (int x = 0; x < f->cols; x++) {
			strcat_s(row, sizeof row,
			        f->cells[y * f->cols + x].utf8);
		}
		strcat_s(screen, sizeof screen, row);
		strcat_s(screen, sizeof screen, "\n");
	}
	wf_frame_free(f);
}

static bool screen_has(const char *s)
{
	draw();
	return NULL != strstr(screen, s);
}

static void tick(uint64_t at)
{
	now = at;
	wf_es_set_clock(S, now);
	(void)wf_es_tick(S, now);
}

/* The answer a worker gives for the lines of `base` against `text`. */
static WfGitAnswer diff_answer(
        uint32_t gen, uint64_t id, const char *base, const char *text)
{
	WfGitAnswer a;
	WfStatus st;

	memset(&a, 0, sizeof a);
	a.kind = WF_GIT_A_DIFF;
	a.gen = gen;
	a.id = id;
	st = wf_linediff(base, strlen(base), text, strlen(text), &a.diff);
	if (WF_OK == st) {
		a.state = WF_GITR_OK;
	} else {
		a.state = WF_GITR_TOO_LARGE;
		a.diff_too_many = true;
	}
	return a;
}

static WfGitAnswer base_answer(
        uint32_t gen, uint64_t id, WfGitState st, const char *content)
{
	WfGitAnswer a;

	memset(&a, 0, sizeof a);
	a.kind = WF_GIT_A_BASE;
	a.gen = gen;
	a.id = id;
	a.state = st;
	if (WF_GITR_OK == st) {
		a.base = wf_gitblob_new(content, strlen(content));
	}
	return a;
}

/* Opens A and brings the pane to the point where its first diff is shown. */
static void open_with_pane(const char *base)
{
	WfGitAnswer a;

	setup(true);
	CHECK(open_file(FILE_A));
	a = base_answer(rec.base_gen, rec.base_id, WF_GITR_OK, base);
	CHECK(wf_es_on_git(S, &a));
	wf_gitanswer_free(&a);
	draw(); /* the pane notices the text */
	tick(now + 1);
	CHECK(1 == rec.diffs);
}

static void without_git_the_editor_is_as_it_was(void)
{
	setup(false);
	CHECK(open_file(FILE_A));
	CHECK(0 == rec.bases);
	CHECK(!screen_has("DIFF"));
	type("x");
	tick(now + 1000);
	CHECK(0 == rec.diffs && !screen_has("DIFF"));
	CHECK(screen_has("xone")); /* the text went in */
	wf_es_free(S);
	S = NULL;
}

static void the_base_is_asked_once_with_the_path_and_the_pane_waits_for_it(void)
{
	WfGitAnswer a;

	setup(true);
	CHECK(open_file(FILE_A));
	CHECK(1 == rec.bases && !wcscmp(rec.base_path, FILE_A));
	CHECK(!screen_has("DIFF")); /* nothing shown until the base is there */
	tick(now + 500);
	CHECK(0 == rec.diffs);
	/* an answer for another generation or buffer is not taken */
	a = base_answer(rec.base_gen + 1, rec.base_id, WF_GITR_OK, "x\n");
	CHECK(!wf_es_on_git(S, &a));
	wf_gitanswer_free(&a);
	a = base_answer(rec.base_gen, rec.base_id + 99, WF_GITR_OK, "x\n");
	CHECK(!wf_es_on_git(S, &a));
	wf_gitanswer_free(&a);
	a = base_answer(
	        rec.base_gen, rec.base_id, WF_GITR_OK, "one\ntwo\nTHREE\n");
	CHECK(wf_es_on_git(S, &a)); /* the layout changes */
	wf_gitanswer_free(&a);
	CHECK(screen_has("DIFF  HEAD <-> EDITOR"));
	{ /* the text keeps to the left of the pane */
		const WfBuffer *b = S->buf[S->cur];

		CHECK(wf_es_git_cols(S, b) > 0);
		CHECK(wf_es_text_width(S, b) <= COLS - wf_es_git_cols(S, b));
	}
	/* and it is not asked again by drawing and ticking */
	draw();
	tick(now + 1);
	tick(now + 2);
	CHECK(1 == rec.bases && 1 == rec.diffs);
}

static void
the_diff_is_of_the_buffer_not_of_the_disk_and_waits_for_the_typing_to_stop(void)
{
	WfGitAnswer a;
	uint32_t first;

	open_with_pane("one\ntwo\nTHREE\n");
	first = rec.diff_gen;
	CHECK(!strcmp(rec.text, "one\ntwo\nthree\n")); /* the text as opened */
	a = diff_answer(
	        rec.diff_gen, rec.diff_id, "one\ntwo\nTHREE\n", rec.text);
	CHECK(wf_es_on_git(S, &a));
	wf_gitanswer_free(&a);
	CHECK(screen_has("-THREE") && screen_has("+three") &&
	        screen_has("+1 -1"));
	/* typing: nothing is asked until it has stopped for the debounce time */
	for (int i = 0; i < 20; i++) {
		type("z");
		tick(now + 10);
		draw();
	}
	CHECK(1 == rec.diffs);
	tick(now + WF_GP_DEBOUNCE_MS - 20);
	CHECK(1 == rec.diffs);
	tick(now + 30);
	CHECK(2 == rec.diffs && rec.diff_gen != first);
	CHECK(strstr(rec.text,
	        "zzzzzzzzzzzzzzzzzzzzone")); /* unsaved text, as it is now */
	/* no more asked while nothing changes */
	tick(now + 5000);
	CHECK(2 == rec.diffs);
}

static void an_answer_that_is_not_the_newest_is_never_shown(void)
{
	WfGitAnswer a, b;
	uint32_t old_gen;
	uint64_t id;

	open_with_pane("one\ntwo\nTHREE\n");
	old_gen = rec.diff_gen;
	id = rec.diff_id;
	type("Q");
	draw();
	tick(now + 1000);
	CHECK(2 == rec.diffs && rec.diff_gen != old_gen);
	/* the answer of the first request comes late */
	a = diff_answer(old_gen, id, "one\ntwo\nTHREE\n", "one\ntwo\nthree\n");
	CHECK(!wf_es_on_git(S, &a));
	wf_gitanswer_free(&a);
	CHECK(!screen_has("-THREE")); /* nothing of it */
	/* the newest is */
	b = diff_answer(rec.diff_gen, id, "one\ntwo\nTHREE\n", rec.text);
	CHECK(wf_es_on_git(S, &b));
	wf_gitanswer_free(&b);
	CHECK(screen_has("+Qone"));
	/* for a buffer that is closed, or never was: nothing */
	a = diff_answer(rec.diff_gen, id + 40, "a\n", "b\n");
	CHECK(!wf_es_on_git(S, &a));
	wf_gitanswer_free(&a);
}

static void a_diff_past_the_bounds_says_so_and_the_text_goes_on(void)
{
	WfGitAnswer a;

	open_with_pane("x\n");
	memset(&a, 0, sizeof a);
	a.kind = WF_GIT_A_DIFF;
	a.gen = rec.diff_gen;
	a.id = rec.diff_id;
	a.state = WF_GITR_TOO_LARGE;
	a.diff_too_many = true;
	CHECK(wf_es_on_git(S, &a));
	CHECK(screen_has("too many changes"));
	type("k");
	CHECK(screen_has("kone"));
}

static void a_narrow_terminal_has_no_pane_and_asks_for_no_diff(void)
{
	WfGitAnswer a;

	setup(true);
	COLS = WF_GP_MIN_COLS - 1;
	CHECK(open_file(FILE_A));
	a = base_answer(rec.base_gen, rec.base_id, WF_GITR_OK, "one\n");
	CHECK(wf_es_on_git(S, &a));
	wf_gitanswer_free(&a);
	draw();
	tick(now + 1000);
	CHECK(0 == rec.diffs && !screen_has("DIFF"));
	CHECK(screen_has("one")); /* the whole width is the text's */
	type("n");
	CHECK(screen_has("none"));
	/* wide again: the pane, and the diff, come */
	COLS = 120;
	draw();
	tick(now + 1000);
	CHECK(1 == rec.diffs && screen_has("DIFF"));
}

static void every_failure_leaves_the_editor_without_a_pane(void)
{
	static const WfGitState states[] = {WF_GITR_FAILED, WF_GITR_UNAVAILABLE,
	        WF_GITR_TOO_LARGE, WF_GITR_NOT_REPO};
	WfGitAnswer a;

	for (size_t i = 0; i < sizeof states / sizeof *states; i++) {
		setup(true);
		CHECK(open_file(FILE_A));
		a = base_answer(rec.base_gen, rec.base_id, states[i], "");
		CHECK(!wf_es_on_git(S, &a));
		wf_gitanswer_free(&a);
		draw();
		tick(now + 1000);
		CHECK(0 == rec.diffs && !screen_has("DIFF"));
		type("w");
		CHECK(screen_has("wone"));
	}
	/* the request itself cannot be made */
	setup(true);
	rec.base_result = WF_E_NOMEM;
	CHECK(open_file(FILE_A));
	tick(now + 1000);
	CHECK(0 == rec.diffs && !screen_has("DIFF"));
	type("v");
	CHECK(screen_has("vone"));
}

/* A diff request that cannot be made leaves no answer to wait for: the diff of
 * older text is taken down, and the request is made again after a pause. */
static void a_diff_request_that_fails_takes_down_the_old_diff_and_is_retried(
        void)
{
	WfGitAnswer a;
	int before;

	open_with_pane("one\n");
	a = diff_answer(rec.diff_gen, rec.diff_id, "one\n", "wone\n");
	CHECK(wf_es_on_git(S, &a));
	wf_gitanswer_free(&a);
	CHECK(screen_has("+1"));
	rec.diff_result = WF_E_NOMEM;
	type("v");
	draw(); /* the pane notices the text */
	tick(now + 1000);
	CHECK(2 == rec.diffs && !screen_has("+1"));
	before = rec.diffs;
	tick(now + 100); /* not yet */
	CHECK(before == rec.diffs);
	rec.diff_result = WF_OK;
	tick(now + 1000);
	CHECK(before + 1 == rec.diffs);
	a = diff_answer(rec.diff_gen, rec.diff_id, "one\n", "vwone\n");
	CHECK(wf_es_on_git(S, &a));
	wf_gitanswer_free(&a);
	CHECK(screen_has("+1"));
}

static void markdown_with_its_preview_has_no_diff_pane(void)
{
	WfGitAnswer a;

	setup(true);
	CHECK(open_file(FILE_MD));
	CHECK(1 == rec.bases);
	a = base_answer(rec.base_gen, rec.base_id, WF_GITR_OK, "# old\n");
	wf_es_on_git(S, &a);
	wf_gitanswer_free(&a);
	draw();
	tick(now + 1000);
	if (wf_es_md_cols(S, S->buf[S->cur]) >
	        0) { /* the preview is on: it has the place */
		CHECK(0 == rec.diffs && !screen_has("DIFF  HEAD"));
	} else {
		CHECK(1 == rec.diffs);
	}
}

static void switching_buffers_asks_again_for_the_one_shown(void)
{
	WfGitAnswer a;
	uint64_t id_a;
	int before;

	open_with_pane("one\ntwo\nTHREE\n");
	id_a = rec.diff_id;
	CHECK(open_file(FILE_B));
	CHECK(2 == rec.bases);
	a = base_answer(rec.base_gen, rec.base_id, WF_GITR_OK, "b1\n");
	CHECK(wf_es_on_git(S, &a));
	wf_gitanswer_free(&a);
	draw();
	tick(now + 1);
	CHECK(rec.diff_id != id_a && 2 == rec.diffs);
	before = rec.diffs;
	CHECK(open_file(FILE_A)); /* back to the first */
	draw();
	tick(now + 1);
	CHECK(rec.diffs == before + 1 && rec.diff_id == id_a);
}

/* The worker keeps one request for a base: when a second file is opened before the
 * first one's base came, the first one's was replaced and nothing will come for it.
 * Back on screen it asks again. */
static void
a_base_that_was_replaced_is_asked_for_again_when_its_buffer_is_shown(void)
{
	WfGitAnswer a;
	uint64_t id_a, id_b;
	uint32_t first_gen_a;

	setup(true);
	CHECK(open_file(FILE_A));
	id_a = rec.base_id;
	first_gen_a = rec.base_gen;
	CHECK(open_file(FILE_B)); /* before A's base came */
	id_b = rec.base_id;
	CHECK(id_a != id_b && 2 == rec.bases);
	a = base_answer(rec.base_gen, id_b, WF_GITR_OK, "b1\n");
	CHECK(wf_es_on_git(S, &a));
	wf_gitanswer_free(&a);
	draw();
	tick(now + 1);
	CHECK(2 == rec.bases);    /* B has its base: nothing more to ask */
	CHECK(open_file(FILE_A)); /* back to the first */
	draw();
	tick(now + 1);
	CHECK(3 == rec.bases && rec.base_id == id_a &&
	        rec.base_gen != first_gen_a);
	tick(now + 2);
	CHECK(3 == rec.bases); /* asked once, not at every tick */
	/* the answer of the request that was replaced is not taken; the new one is */
	a = base_answer(first_gen_a, id_a, WF_GITR_OK, "one\n");
	CHECK(!wf_es_on_git(S, &a));
	wf_gitanswer_free(&a);
	CHECK(!screen_has("DIFF  HEAD"));
	a = base_answer(rec.base_gen, id_a, WF_GITR_OK, "one\ntwo\nTHREE\n");
	CHECK(wf_es_on_git(S, &a));
	wf_gitanswer_free(&a);
	CHECK(screen_has("DIFF  HEAD <-> EDITOR"));
	tick(now + 3);
	CHECK(3 == rec.bases);
}

static void closing_the_session_cancels_what_is_asked(void)
{
	setup(true);
	CHECK(open_file(FILE_A));
	wf_es_free(S);
	S = NULL;
	CHECK(1 == rec.cancels);
}

static void make_numbered(
        char *out, size_t cap, int lines, const int *changed, int n_changed)
{
	size_t k = 0;

	for (int i = 0; i < lines; i++) {
		bool c = false;

		for (int j = 0; j < n_changed; j++) {
			c |= changed && changed[j] == i;
		}
		k += (size_t)sprintf_s(
		        out + k, cap - k, c ? "CHANGED %d\n" : "line %d\n", i);
	}
}

static void the_rows_are_hunks_with_context_close_ones_joined(void)
{
	static char a[20000], b[20000];
	static const int at[] = {10, 14, 100};
	WfGitPane p = {0};

	make_numbered(a, sizeof a, 200, NULL, 0);
	make_numbered(b, sizeof b, 200, at, 3);
	CHECK(WF_OK == wf_linediff(a, strlen(a), b, strlen(b), &p.diff));
	CHECK(3 == p.diff.n);
	CHECK(WF_OK == wf_gp_build_rows(&p, 3));
	{
		size_t headers = 0, adds = 0, dels = 0, ctx = 0;

		for (size_t i = 0; i < p.nrows; i++) {
			headers += 'H' == p.rows[i].kind;
			adds += '+' == p.rows[i].kind;
			dels += '-' == p.rows[i].kind;
			ctx += 'C' == p.rows[i].kind;
		}
		/* 10 and 14 are 3 lines apart: one hunk; 100 is far: another */
		CHECK(2 == headers && 3 == adds && 3 == dels);
		/* first: 3 before, 3 between (11 12 13), 3 after; second: 3 and 3 */
		CHECK(3 + 3 + 3 + 3 + 3 == ctx);
	}
	CHECK('H' == p.rows[0].kind &&
	        p.rows[0].b == 7); /* three lines before line 10 */
	/* the top of the pane follows the line the cursor is on */
	{
		size_t top = wf_gp_top_for(&p, 100, 10);

		CHECK(top > 0);
		/* the changed line 100 is among the ten rows shown */
		bool in_view = false;

		for (size_t i = top; i < top + 10 && i < p.nrows; i++) {
			in_view |= '+' == p.rows[i].kind && 100 == p.rows[i].b;
		}
		CHECK(in_view);
		CHECK(0 == wf_gp_top_for(&p, 0, 10));
		CHECK(wf_gp_top_for(&p, 199, 10) + 10 >= p.nrows ||
		        p.nrows <= 10);
	}
	wf_gp_free(&p);
	wf_gp_free(&p); /* safe to repeat */
	/* no changes: no rows, and nothing to scroll */
	CHECK(WF_OK == wf_linediff("a\n", 2, "a\n", 2, &p.diff));
	CHECK(WF_OK == wf_gp_build_rows(&p, 3) && 0 == p.nrows &&
	        0 == wf_gp_top_for(&p, 5, 10));
	wf_gp_free(&p);
	/* a change at the very start and at the very end: no context outside the text */
	CHECK(WF_OK == wf_linediff("a\nb\nc\n", 6, "X\nb\nZ\n", 6, &p.diff));
	CHECK(WF_OK == wf_gp_build_rows(&p, 3));
	CHECK('H' == p.rows[0].kind && 0 == p.rows[0].b);
	for (size_t i = 0; i < p.nrows; i++) {
		CHECK(p.rows[i].b <= 3);
	}
	wf_gp_free(&p);
}

int main(void)
{
	without_git_the_editor_is_as_it_was();
	the_base_is_asked_once_with_the_path_and_the_pane_waits_for_it();
	the_diff_is_of_the_buffer_not_of_the_disk_and_waits_for_the_typing_to_stop();
	an_answer_that_is_not_the_newest_is_never_shown();
	a_diff_past_the_bounds_says_so_and_the_text_goes_on();
	a_narrow_terminal_has_no_pane_and_asks_for_no_diff();
	every_failure_leaves_the_editor_without_a_pane();
	a_diff_request_that_fails_takes_down_the_old_diff_and_is_retried();
	markdown_with_its_preview_has_no_diff_pane();
	switching_buffers_asks_again_for_the_one_shown();
	a_base_that_was_replaced_is_asked_for_again_when_its_buffer_is_shown();
	closing_the_session_cancels_what_is_asked();
	the_rows_are_hunks_with_context_close_ones_joined();
	wf_es_free(S);
	return TEST_RESULT();
}
