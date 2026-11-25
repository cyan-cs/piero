#include <string.h>

#include "render/wf_frame.h"
#include "render/wf_present.h"
#include "wf_test.h"

static const WfStyle S = {0xFFFFFF, 0x000000, 0};

static int has(const WfBuf *b, const char *needle)
{
	return NULL != strstr(wf_buf_cstr(b), needle);
}

static void put(WfFrame *f, int x, int y, const char *s, int max)
{
	wf_frame_put(f, x, y, s, strlen(s), &S, max);
}

static void test_put_basic(void)
{
	WfFrame *f = wf_frame_new(10, 2, &S);

	CHECK(NULL != f);
	CHECK(NULL == wf_frame_new(0, 5, &S));
	CHECK(NULL == wf_frame_new(40000, 5, &S));

	CHECK(3 == wf_frame_put(f, 0, 0, "abc", 3, &S, 10));
	CHECK(0 == strcmp(f->cells[1].utf8, "b"));
	/* clipped by max_cells and by the frame edge */
	CHECK(2 == wf_frame_put(f, 8, 0, "xyz", 3, &S, 10));
	CHECK(4 == wf_frame_put(f, 0, 1, "abcdef", 6, &S, 4));
	CHECK(0 == wf_frame_put(f, -1, 0, "a", 1, &S, 1));
	CHECK(0 == wf_frame_put(f, 0, 5, "a", 1, &S, 1));
	wf_frame_free(f);
}

static void test_wide(void)
{
	WfFrame *f = wf_frame_new(6, 1, &S);

	/* "あ" is two columns: head + continuation */
	CHECK(2 == wf_frame_put(f, 0, 0, "\xE3\x81\x82", 3, &S, 6));
	CHECK(2 == f->cells[0].width && 0 == f->cells[1].width);

	/* does not fit in 1 remaining column: becomes '?' */
	CHECK(2 == wf_frame_put(f, 0, 0, "a\xE3\x81\x82", 4, &S, 2));
	CHECK(0 == strcmp(f->cells[1].utf8, "?"));

	/* overwriting half of a wide pair must not leave an orphan */
	wf_frame_put(f, 0, 0, "\xE3\x81\x82", 3, &S, 6);
	wf_frame_put(f, 1, 0, "z", 1, &S, 1);
	CHECK(1 == f->cells[0].width && 0 == strcmp(f->cells[0].utf8, " "));
	CHECK(0 == strcmp(f->cells[1].utf8, "z"));
	wf_frame_free(f);
}

static void test_combining(void)
{
	WfFrame *f = wf_frame_new(6, 1, &S);
	/* e + combining acute: one cell */
	CHECK(1 == wf_frame_put(f, 0, 0, "e\xCC\x81", 3, &S, 6));
	CHECK(0 == strcmp(f->cells[0].utf8, "e\xCC\x81"));
	/* a stack of marks too long for a cell collapses to ASCII */
	{
		char big[64] = "e";
		size_t len = 1;
		int i;

		for (i = 0; i < 20; i++, len += 2) {
			memcpy(big + len, "\xCC\x81", 2);
		}
		CHECK(1 == wf_frame_put(f, 2, 0, big, len, &S, 4));
		CHECK(0 == strcmp(f->cells[2].utf8, "?"));
	}
	/* a mark with no base is dropped */
	CHECK(0 == wf_frame_put(f, 4, 0, "\xCC\x81", 2, &S, 2));
	wf_frame_free(f);
}

/* A mark takes no cell, so a row that is full still takes it: the cut is between
 * characters. Where the text is cut short by the width, or by the last column
 * of the frame, the mark of the last character that fits goes with it. */
static void test_combining_at_the_edge(void)
{
	WfFrame *f = wf_frame_new(4, 2, &S);

	/* exactly the width: the base takes the one cell, the accent follows */
	CHECK(1 == wf_frame_put(f, 0, 0, "e\xCC\x81", 3, &S, 1));
	CHECK(0 == strcmp(f->cells[0].utf8, "e\xCC\x81"));
	/* the next character that needs a cell is still not drawn */
	CHECK(1 == wf_frame_put(f, 1, 0, "e\xCC\x81x", 4, &S, 1));
	CHECK(0 == strcmp(f->cells[1].utf8, "e\xCC\x81"));
	CHECK(0 == strcmp(f->cells[2].utf8, " "));
	/* the last column of the frame, with room asked for that is not there */
	CHECK(1 == wf_frame_put(f, 3, 0, "o\xCC\x88x", 4, &S, 9));
	CHECK(0 == strcmp(f->cells[3].utf8, "o\xCC\x88"));
	/* a wide character with its voiced mark, in exactly two cells */
	CHECK(2 == wf_frame_put(f, 0, 1, "\xE3\x81\x8B\xE3\x82\x99", 6, &S, 2));
	CHECK(0 == strcmp(f->cells[0 + 4].utf8, "\xE3\x81\x8B\xE3\x82\x99"));
	/* a wide character that does not fit is still a "?", with no mark of its own */
	CHECK(1 == wf_frame_put(f, 3, 1, "\xE3\x81\x8B\xE3\x82\x99", 6, &S, 1));
	CHECK(0 == strcmp(f->cells[3 + 4].utf8, "?"));
	/* nothing to attach to: the mark first, in a row with no room, is dropped */
	CHECK(0 == wf_frame_put(f, 0, 0, "\xCC\x81", 2, &S, 0));
	wf_frame_free(f);
}

static void test_diff(void)
{
	WfFrame *a = wf_frame_new(10, 3, &S);
	WfFrame *b = wf_frame_new(10, 3, &S);
	WfBuf out;

	wf_buf_init(&out);
	/* Validate arguments before inspecting dimensions through next. */
	CHECK(WF_E_INVAL == wf_present_diff(a, NULL, &out));
	CHECK(WF_E_INVAL == wf_present_diff(a, b, NULL));
	CHECK(out.len == 0);

	/* identical frames: nothing to send */
	CHECK(WF_OK == wf_present_diff(a, b, &out) && 0 == out.len);

	/* one changed cell: one CUP to the right place, nothing else */
	put(b, 4, 1, "X", 1);
	CHECK(WF_OK == wf_present_diff(a, b, &out));
	CHECK(has(&out, "\x1b[2;5H") && has(&out, "X"));
	CHECK(!has(&out, "\x1b[1;"));

	/* full redraw writes every cell except the very last */
	wf_buf_clear(&out);
	put(b, 9, 2, "Z", 1);
	CHECK(WF_OK == wf_present_diff(NULL, b, &out));
	CHECK(has(&out, "\x1b[1;1H") && !has(&out, "Z"));

	/* size change clears the screen first */
	{
		WfFrame *c = wf_frame_new(8, 3, &S);

		wf_buf_clear(&out);
		CHECK(WF_OK == wf_present_diff(a, c, &out) &&
		        has(&out, "\x1b[2J"));
		wf_frame_free(c);
	}
	wf_frame_free(a);
	wf_frame_free(b);
	wf_buf_free(&out);
}

static void test_diff_wide(void)
{
	WfFrame *a = wf_frame_new(10, 1, &S);
	WfFrame *b = wf_frame_new(10, 1, &S);
	WfBuf out;

	wf_buf_init(&out);
	/* a: "あ" at 2..3 ; b: "a" at 2 and "b" at 3 -> head and tail both
	 * redrawn */
	put(a, 2, 0, "\xE3\x81\x82", 6);
	put(b, 2, 0, "ab", 6);
	CHECK(WF_OK == wf_present_diff(a, b, &out));
	CHECK(has(&out, "\x1b[1;3H") && has(&out, "a") && has(&out, "b"));

	/* only the continuation differs in styling: head is still resent */
	wf_buf_clear(&out);
	put(b, 2, 0, "\xE3\x81\x82", 6);
	{
		WfStyle other = {0x111111, 0x000000, 0};

		wf_frame_put(b, 2, 0, "\xE3\x81\x82", 3, &other, 2);
	}
	CHECK(WF_OK == wf_present_diff(a, b, &out) &&
	        has(&out, "\xE3\x81\x82"));
	wf_frame_free(a);
	wf_frame_free(b);
	wf_buf_free(&out);
}

/*
 * The terminal's own cursor. A terminal puts an IME composition at its
 * cursor, and cell output leaves the cursor at the last cell written (the
 * status line), so a frame with a text field in focus must end with the
 * cursor on the insertion cell, shown. The tests read the escape sequences
 * the presenter emits: they say where the cursor ends, not what an IME does.
 */
static size_t last_at(const WfBuf *out, const char *needle)
{
	const char *found = NULL, *p = wf_buf_cstr(out);

	while ((p = strstr(p, needle)) != NULL) {
		found = p++;
	}
	return found ? (size_t)(found - wf_buf_cstr(out)) : (size_t)-1;
}

static void test_native_cursor(void)
{
	WfFrame *a = wf_frame_new(20, 5, &S);
	WfFrame *b = wf_frame_new(20, 5, &S);
	WfBuf out;

	wf_buf_init(&out);

	/* the field is on row 1; the status line (row 4) changed too */
	wf_frame_set_cursor(b, 6, 1);
	put(b, 0, 4, "STATUS", 6);
	CHECK(WF_OK == wf_present_diff(a, b, &out));
	CHECK(has(&out, "\x1b[2;7H") && has(&out, "\x1b[?25h"));
	/* the last move is the cursor's, after the status text was written */
	CHECK(last_at(&out, "\x1b[2;7H") > last_at(&out, "STATUS"));
	CHECK(last_at(&out, "\x1b[?25h") > last_at(&out, "\x1b[2;7H"));

	/* nothing changed: an idle frame stays empty */
	wf_frame_free(a);
	a = wf_frame_new(20, 5, &S);
	put(a, 0, 4, "STATUS", 6);
	wf_frame_set_cursor(a, 6, 1);
	wf_buf_clear(&out);
	CHECK(WF_OK == wf_present_diff(a, b, &out) && 0 == out.len);

	/* only the cursor moved: one move, no show, no cell output */
	wf_frame_set_cursor(b, 9, 3);
	CHECK(WF_OK == wf_present_diff(a, b, &out));
	CHECK(!strcmp(wf_buf_cstr(&out), "\x1b[4;10H"));

	/* a cell changed elsewhere, the cursor did not: the cursor goes back */
	wf_frame_free(a);
	a = wf_frame_new(20, 5, &S);
	wf_frame_set_cursor(a, 6, 1);
	wf_frame_free(b);
	b = wf_frame_new(20, 5, &S);
	wf_frame_set_cursor(b, 6, 1);
	put(b, 3, 4, "Q", 1);
	wf_buf_clear(&out);
	CHECK(WF_OK == wf_present_diff(a, b, &out));
	CHECK(last_at(&out, "\x1b[2;7H") > last_at(&out, "Q"));
	CHECK(!has(&out, "\x1b[?25h")); /* it was already shown */

	/* leaving the field hides the cursor, once */
	wf_frame_free(b);
	b = wf_frame_new(20, 5, &S);
	wf_buf_clear(&out);
	CHECK(WF_OK == wf_present_diff(a, b, &out));
	CHECK(!strcmp(wf_buf_cstr(&out), "\x1b[?25l"));
	wf_frame_free(a);
	a = wf_frame_new(20, 5, &S);
	wf_buf_clear(&out);
	CHECK(WF_OK == wf_present_diff(b, a, &out) && 0 == out.len);

	/* a full redraw states the visibility, it does not assume it */
	wf_frame_set_cursor(a, 0, 0);
	wf_buf_clear(&out);
	CHECK(WF_OK == wf_present_diff(NULL, a, &out));
	CHECK(has(&out, "\x1b[?25h") && has(&out, "\x1b[1;1H"));
	wf_buf_clear(&out);
	CHECK(WF_OK == wf_present_diff(NULL, b, &out) &&
	        has(&out, "\x1b[?25l"));

	/* a resize clears the screen and redraws: state is stated again */
	{
		WfFrame *small = wf_frame_new(10, 3, &S);

		wf_frame_set_cursor(small, 2, 1);
		wf_buf_clear(&out);
		CHECK(WF_OK == wf_present_diff(a, small, &out));
		CHECK(has(&out, "\x1b[2J") && has(&out, "\x1b[?25h"));
		CHECK(last_at(&out, "\x1b[2;3H") > last_at(&out, "\x1b[2J"));
		wf_frame_free(small);
	}

	/* outside the frame is no cursor; the edge cells are cells */
	wf_frame_set_cursor(a, 20, 0);
	CHECK(!a->cursor.visible);
	wf_frame_set_cursor(a, 0, -1);
	CHECK(!a->cursor.visible);
	wf_frame_set_cursor(a, 19, 4);
	CHECK(a->cursor.visible && 19 == a->cursor.x && 4 == a->cursor.y);

	/* on a wide character: the cursor is on its first cell */
	wf_frame_free(b);
	b = wf_frame_new(20, 5, &S);
	put(b, 4, 2, "\xE3\x81\x82", 6);
	wf_frame_set_cursor(b, 4, 2);
	wf_frame_free(a);
	a = wf_frame_new(20, 5, &S);
	wf_buf_clear(&out);
	CHECK(WF_OK == wf_present_diff(a, b, &out));
	CHECK(last_at(&out, "\x1b[3;5H") > last_at(&out, "\xE3\x81\x82"));

	wf_frame_free(a);
	wf_frame_free(b);
	wf_buf_free(&out);
}

/* A frame is drawn over again: the same size keeps its memory, its graphic and
 * cursor start over, another size is a new blank frame, and a refusal changes
 * nothing. The widest frame the present code takes needs no allocation to diff. */
static void test_reuse_and_the_widest_present(void)
{
	WfFrame *f = NULL, *keep;
	WfFrame *a, *b;
	WfBuf out;
	WfStyle blank = {0xAAAAAA, 0x111111, 0};

	CHECK(wf_frame_reuse(&f, 20, 4, &blank)); /* none: a new blank frame */
	CHECK(f && 20 == f->cols && 4 == f->rows);
	CHECK(0 == strcmp(f->cells[0].utf8, " ") &&
	        blank.bg_rgb == f->cells[0].bg_rgb);
	put(f, 0, 0, "text", 20);
	f->cursor.visible = true;
	f->cursor.x = 3;
	f->graphic.len = 9;
	keep = f;
	CHECK(wf_frame_reuse(&f, 20, 4, &blank));
	CHECK(keep == f); /* the memory is the same */
	CHECK(!f->cursor.visible &&
	        0 == f->graphic.len); /* what is per frame starts over */
	CHECK(0 ==
	        strcmp(f->cells[0].utf8,
	                "t")); /* the cells are the drawer's to overwrite */
	CHECK(wf_frame_reuse(&f, 30, 5, &blank));
	CHECK(30 == f->cols && 5 == f->rows &&
	        0 == strcmp(f->cells[0].utf8, " "));
	keep = f;
	CHECK(!wf_frame_reuse(&f, 0, 5, &blank)); /* refused: unchanged */
	CHECK(!wf_frame_reuse(&f, WF_FRAME_MAX_DIM + 1, 5, &blank));
	CHECK(keep == f && 30 == f->cols);
	/* a side within its bound, and the whole over the budget: refused, nothing
	 * is allocated for it */
	CHECK(!wf_frame_reuse(&f, WF_FRAME_MAX_DIM, WF_FRAME_MAX_DIM, &blank));
	CHECK(NULL == wf_frame_new(WF_FRAME_MAX_DIM, WF_FRAME_MAX_DIM, &blank));
	CHECK(NULL ==
	        wf_frame_new(1025, 1024, &blank)); /* one over the cells */
	CHECK(keep == f && 30 == f->cols);
	{
		/* the budget itself is a frame */
		WfFrame *edge = wf_frame_new(2048, 512, &blank);

		CHECK(NULL != edge);
		wf_frame_free(edge);
	}
	wf_frame_free(f);
	/* the widest frame: a diff of it works, and an over-wide one is refused */
	a = wf_frame_new(WF_FRAME_MAX_DIM, 2, &S);
	b = wf_frame_new(WF_FRAME_MAX_DIM, 2, &S);
	CHECK(a && b);
	put(b, WF_FRAME_MAX_DIM - 5, 0, "wide", 5);
	wf_buf_init(&out);
	CHECK(WF_OK == wf_present_diff(a, b, &out));
	CHECK(has(&out, "wide"));
	wf_buf_free(&out);
	wf_frame_free(a);
	wf_frame_free(b);
}

int main(void)
{
	test_reuse_and_the_widest_present();
	/* Printable ASCII keeps its width/style; controls become harmless cells.
	 * Existing combining tests still exercise the UTF-8 handoff. */
	WfFrame *ascii = wf_frame_new(128, 1, &S);
	char bytes[128];
	for (unsigned i = 0; i < sizeof bytes; i++) {
		bytes[i] = (char)i;
	}
	CHECK(128 == wf_frame_put(ascii, 0, 0, bytes, sizeof bytes, &S, 128));
	for (unsigned i = 0; i < sizeof bytes; i++) {
		char expected = i < 32 || i == 127 ? '?' : bytes[i];
		CHECK(1 == ascii->cells[i].width &&
		        ascii->cells[i].utf8[0] == expected);
		CHECK(ascii->cells[i].fg_rgb == S.fg_rgb &&
		        ascii->cells[i].bg_rgb == S.bg_rgb);
	}
	wf_frame_free(ascii);
	WfFrame *controls = wf_frame_new(12, 1, &S);
	const char text[] = "A\nB\x1b\xc2\x9b";
	CHECK(wf_frame_put(controls, 0, 0, text, sizeof text - 1, &S, 12) == 5);
	CHECK(!strcmp(controls->cells[1].utf8, "?") &&
	        !strcmp(controls->cells[3].utf8, "?") &&
	        !strcmp(controls->cells[4].utf8, "?"));
	wf_frame_free(controls);
	test_put_basic();
	test_wide();
	test_combining();
	test_combining_at_the_edge();
	test_diff();
	test_diff_wide();
	test_native_cursor();
	return TEST_RESULT();
}
