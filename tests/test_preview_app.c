#include <string.h>

#include "ui/wf_app_priv.h"
#include "wf_fake_fs.h"
#include "wf_test.h"

typedef struct
{
	uint32_t gen;
	int calls, cancels, explicit_load;
	wchar_t path[128];
} PreviewSpy;
static WfStatus preview_submit(
        void *ctx, uint32_t gen, const wchar_t *path, bool explicit_load)
{
	PreviewSpy *s = ctx;
	s->calls++;
	s->gen = gen;
	s->explicit_load = explicit_load;
	wcscpy_s(s->path, 128, path);
	return WF_OK;
}
static void preview_cancel(void *ctx)
{
	((PreviewSpy *)ctx)->cancels++;
}
static void ch(WfApp *a, uint32_t c)
{
	WfEvent ev = {WF_EV_CHAR, c, WF_KEY_NONE, 0};
	a->mode->on_event(a, &ev);
}
static WfPreview *text(const char *s)
{
	WfPreview *p = NULL;
	CHECK(WF_OK == wf_preview_decode(s, strlen(s), 0, &p));
	return p;
}

static void test_filter_updates_preview_selection(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	PreviewSpy spy = {0};
	WfPreviewPort port = {&spy, preview_submit, preview_cancel};
	WfEvent up = {WF_EV_KEY, 0, WF_KEY_UP, 0};
	WfEvent down = {WF_EV_KEY, 0, WF_KEY_DOWN, 0};
	WfEvent backspace = {WF_EV_KEY, 0, WF_KEY_BACKSPACE, 0};
	WfEvent enter = {WF_EV_KEY, 0, WF_KEY_ENTER, 0};
	WfEvent escape = {WF_EV_KEY, 0, WF_KEY_ESC, 0};
	uint32_t previous;

	CHECK(WF_OK ==
	        wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\Users\\dev"));
	wf_app_set_preview(&a, port);
	ch(&a, '/');
	ch(&a, 'f');
	CHECK(&WF_MODE_FILTER == a.mode && a.preview_busy && 1 == spy.calls);
	CHECK(!wcscmp(spy.path, L"C:\\Users\\dev\\file2.txt"));
	previous = spy.gen;
	a.mode->on_event(&a, &down);
	CHECK(&WF_MODE_FILTER == a.mode && 2 == spy.calls &&
	        !wcscmp(spy.path, L"C:\\Users\\dev\\file10.txt"));
	wf_app_on_preview(&a, previous, text("old candidate"));
	CHECK(a.preview_busy && NULL == a.preview);
	a.mode->on_event(&a, &up);
	CHECK(3 == spy.calls &&
	        !wcscmp(spy.path, L"C:\\Users\\dev\\file2.txt"));
	ch(&a, 'z');
	CHECK(0 == a.cur.nvis && !a.preview_busy && NULL == a.preview);
	a.mode->on_event(&a, &backspace);
	CHECK(2 == a.cur.nvis && a.preview_busy && 4 == spy.calls);
	a.mode->on_event(&a, &enter);
	CHECK(&WF_MODE_NORMAL == a.mode && !wcscmp(a.cur.filter, L"f") &&
	        4 == spy.calls);
	ch(&a, '/');
	a.mode->on_event(&a, &escape);
	CHECK(&WF_MODE_NORMAL == a.mode && !a.cur.filter[0] && 3 == a.cur.nvis);
	wf_app_destroy(&a);
}

static void key(WfApp *a, WfKey code)
{
	WfEvent ev = {WF_EV_KEY, 0, code, 0};
	a->mode->on_event(a, &ev);
}

static int frame_row_has(const WfFrame *f, int y, const char *needle)
{
	char row[8192] = "";

	for (int x = 0; x < f->cols; x++) {
		strcat_s(row, sizeof row, f->cells[y * f->cols + x].utf8);
	}
	return NULL != strstr(row, needle);
}

static void test_right_focuses_read_only_pane(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	PreviewSpy spy = {0};
	WfPreviewPort port = {&spy, preview_submit, preview_cancel};
	WfFrame *frame;
	size_t cursor, last;

	CHECK(WF_OK ==
	        wf_app_init(&a, 160, 30, fake_port(&fp), L"C:\\Users\\dev"));
	wf_app_set_preview(&a, port);
	ch(&a, 'j');
	wf_app_on_preview(
	        &a, spy.gen, text("PANE_FIRST\nPANE_SECOND\nPANE_LAST"));
	cursor = a.cur.cursor;
	CHECK(WF_OK == wf_sel_toggle(&a.sel, a.cwd, L"file2.txt"));
	key(&a, WF_KEY_RIGHT);
	CHECK(!strcmp(a.mode->name, "PREVIEW_PANE") && a.cur.cursor == cursor);
	if (strcmp(a.mode->name, "PREVIEW_PANE")) {
		wf_app_destroy(&a);
		return;
	}
	CHECK(1 == spy.calls && !a.want_path);
	frame = wf_frame_new(160, 30, &WF_STYLE_BASE);
	CHECK(NULL != frame);
	if (frame) {
		wf_app_draw(&a, frame);
		CHECK(frame_row_has(frame, 2, "Current"));
		CHECK(frame_row_has(frame, 2, "Preview [READ ONLY]"));
	}
	key(&a, WF_KEY_DOWN);
	CHECK(0 == a.preview_top && a.cur.cursor == cursor);
	if (frame) {
		wf_app_draw(&a, frame);
		CHECK(frame_row_has(frame, 5, "PANE_SECOND"));
	}
	key(&a, WF_KEY_UP);
	key(&a, WF_KEY_UP);
	CHECK(0 == a.preview_top && a.cur.cursor == cursor);
	for (int i = 0; i < 20; i++) {
		key(&a, WF_KEY_DOWN);
	}
	last = 0;
	CHECK(a.preview_top == last);
	ch(&a, 'i'); /* editing is not enabled by focusing the pane */
	ch(&a, 'y');
	ch(&a, 'p');
	CHECK(!strcmp(a.mode->name, "PREVIEW_PANE") && a.preview_top == last &&
	        WF_XFER_NONE == a.slot.mode && !a.want_path);
	key(&a, WF_KEY_LEFT);
	CHECK(&WF_MODE_NORMAL == a.mode && a.cur.cursor == cursor);
	CHECK(1 == wf_sel_count(&a.sel, a.cwd));
	key(&a, WF_KEY_RIGHT);
	CHECK(a.preview_top == last && 1 == spy.calls);
	key(&a, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == a.mode && a.cur.cursor == cursor);
	CHECK(1 == wf_sel_count(&a.sel, a.cwd));
	key(&a, WF_KEY_RIGHT);
	ch(&a, 'v');
	CHECK(&WF_MODE_PREVIEW == a.mode);
	key(&a, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == a.mode);
	key(&a, WF_KEY_ENTER);
	CHECK(NULL != strstr(a.msg, "opening files is not set up"));
	wf_frame_free(frame);
	wf_app_destroy(&a);
}

static void test_pane_focus_preserves_navigation_and_fetch_policy(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	PreviewSpy spy = {0};
	WfPreviewPort port = {&spy, preview_submit, preview_cancel};

	CHECK(WF_OK ==
	        wf_app_init(&a, 160, 30, fake_port(&fp), L"C:\\Users\\dev"));
	wf_app_set_preview(&a, port);
	fp.defer = 1;
	key(&a, WF_KEY_RIGHT); /* a directory keeps its existing navigation
	                          meaning */
	CHECK(&WF_MODE_NORMAL == a.mode && a.want_path && 1 == fp.nq &&
	        !wcscmp(a.want_path, L"C:\\Users\\dev\\Docs"));
	ch(&a, 'j');
	key(&a, WF_KEY_RIGHT);
	CHECK(&WF_MODE_NORMAL == a.mode && 0 == spy.calls);
	CHECK(NULL != strstr(a.msg, "Updating"));
	fake_deliver(
	        &fp, 0); /* failed navigation preserves the committed listing */
	key(&a, WF_KEY_RIGHT); /* focus while the text result is still pending
	                        */
	CHECK(!strcmp(a.mode->name, "PREVIEW_PANE") && a.preview_busy &&
	        1 == spy.calls);
	if (strcmp(a.mode->name, "PREVIEW_PANE")) {
		wf_app_destroy(&a);
		return;
	}
	wf_app_on_preview(&a, spy.gen, text("async first\nasync second"));
	CHECK(!strcmp(a.mode->name, "PREVIEW_PANE") && !a.preview_busy);
	key(&a, WF_KEY_DOWN);
	CHECK(0 == a.preview_top);
	key(&a, WF_KEY_ESC);
	a.cols = 40;
	key(&a, WF_KEY_RIGHT);
	CHECK(&WF_MODE_NORMAL == a.mode && NULL != strstr(a.msg, "widen"));
	a.cols = 160;
	a.cur.dir->e[a.cur.vis[a.cur.cursor]].flags |= WF_ENT_CLOUD;
	key(&a, WF_KEY_RIGHT);
	CHECK(!strcmp(a.mode->name, "PREVIEW_PANE") && 1 == spy.calls &&
	        a.preview && WF_PREVIEW_REFUSED == a.preview->kind);
	key(&a, WF_KEY_DOWN);
	CHECK(0 == a.preview_top);
	key(&a, WF_KEY_ESC);
	ch(&a, '/');
	ch(&a, 'z');
	key(&a, WF_KEY_ENTER);
	key(&a, WF_KEY_RIGHT);
	CHECK(&WF_MODE_NORMAL == a.mode && 0 == a.cur.nvis);
	wf_app_destroy(&a);
}

static WfPreview *numbered_text(size_t lines, int truncated)
{
	char bytes[4096];
	size_t used = 0;
	WfPreview *p = NULL;

	CHECK(lines <= 100);
	if (lines > 100) {
		return NULL;
	}
	for (size_t i = 1; i <= lines; i++) {
		used += (size_t)snprintf(bytes + used, sizeof bytes - used,
		        "%srow%02zu", 1 == i ? "" : "\n", i);
	}
	CHECK(WF_OK == wf_preview_decode(bytes, used, truncated, &p));
	return p;
}

static void test_line_numbers_and_bottom_scroll_limit(void)
{
	for (int fullscreen = 0; fullscreen < 2; fullscreen++) {
		for (int truncated = 0; truncated < 2; truncated++) {
			WfApp a;
			FakePort fp = {.app = &a};
			PreviewSpy spy = {0};
			WfPreviewPort port = {
			        &spy, preview_submit, preview_cancel};
			WfFrame *f;
			size_t limit = (size_t)(fullscreen ? 5 : 8) +
			        (size_t)truncated;
			int first = fullscreen ? 2 : 4;
			int bottom = (fullscreen ? 8 : 7) - truncated;

			CHECK(wf_app_init(&a, 160, 10, fake_port(&fp),
			              L"C:\\Users\\dev") == WF_OK);
			wf_app_set_preview(&a, port);
			ch(&a, 'j');
			wf_app_on_preview(
			        &a, spy.gen, numbered_text(12, truncated));
			if (fullscreen) {
				ch(&a, 'v');
			} else {
				key(&a, WF_KEY_RIGHT);
			}
			f = wf_frame_new(160, 10, &WF_STYLE_BASE);
			CHECK(NULL != f && NULL != a.preview);
			if (!f || !a.preview) {
				wf_frame_free(f);
				wf_app_destroy(&a);
				continue;
			}
			wf_app_draw(&a, f);
			CHECK(frame_row_has(f, first, " 1 | row01"));
			for (int i = 0; i < 30; i++) {
				key(&a, WF_KEY_DOWN);
			}
			CHECK(a.preview_top == limit);
			wf_app_draw(&a, f);
			CHECK(frame_row_has(f, bottom, "12 | row12"));
			if (truncated) {
				CHECK(frame_row_has(f, bottom + 1,
				        "[bounded preview; truncated]"));
			}
			key(&a, WF_KEY_UP);
			CHECK(a.preview_top == limit - 1);
			key(&a, WF_KEY_CTRL_UP);
			CHECK(0 == a.preview_top);
			key(&a, WF_KEY_CTRL_DOWN);
			CHECK(a.preview_top == limit);
			for (int i = 0; i < 30; i++) {
				key(&a, WF_KEY_UP);
			}
			CHECK(0 == a.preview_top);
			wf_frame_free(f);
			wf_app_destroy(&a);
		}
	}
}

/* A digest made with # shows on the last row of the pane, for the file it is of
 * (DR-DRAFT-027); the text keeps the other rows and still scrolls to its end. */
static void test_pane_shows_the_digest_of_its_file(void)
{
	static const uint8_t abc[32] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01,
	        0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
	        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10,
	        0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
	WfApp a;
	FakePort fp = {.app = &a};
	PreviewSpy spy = {0};
	WfPreviewPort port = {&spy, preview_submit, preview_cancel};
	const WfEntry *e;
	WfFrame *f;

	CHECK(WF_OK ==
	        wf_app_init(&a, 160, 10, fake_port(&fp), L"C:\\Users\\dev"));
	wf_app_set_preview(&a, port);
	ch(&a, 'j');
	wf_app_on_preview(&a, spy.gen, numbered_text(12, 0));
	e = wf_view_current(&a.cur);
	CHECK(e);
	f = wf_frame_new(160, 10, &WF_STYLE_BASE);
	CHECK(f);
	if (!e || !f) {
		return;
	}
	wf_app_draw_preview(&a, f, 0, 1, 80, 7);
	CHECK(!frame_row_has(f, 7, "SHA-256"));
	CHECK(frame_row_has(f, 7, "row06"));

	/* a digest of this file takes the last row */
	a.hash.has = true;
	a.hash.path = _wcsdup(L"C:\\Users\\dev\\file2.txt");
	a.hash.size = e->size;
	a.hash.last_write = e->mtime;
	memcpy(a.hash.digest, abc, 32);
	wf_app_draw_preview(&a, f, 0, 1, 80, 7);
	CHECK(frame_row_has(f, 7,
	        "SHA-256 "
	        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015"
		"ad"));
	CHECK(frame_row_has(f, 6, "row05"));
	CHECK(!frame_row_has(f, 7, "row06"));

	/* the end of the text can still be reached: 12 rows, 4 of them shown */
	for (int i = 0; i < 20; i++) {
		key(&a, WF_KEY_RIGHT);
		key(&a, WF_KEY_DOWN);
	}
	wf_frame_free(f);
	f = wf_frame_new(160, 10, &WF_STYLE_BASE);
	wf_app_draw(&a, f);
	CHECK(frame_row_has(f, 6, "12 | row12"));
	CHECK(frame_row_has(f, 7, "SHA-256 ba7816bf"));
	wf_frame_free(f);

	/* a digest of another size is not shown */
	a.hash.size = e->size + 1;
	f = wf_frame_new(160, 10, &WF_STYLE_BASE);
	wf_app_draw(&a, f);
	CHECK(!frame_row_has(f, 7, "SHA-256"));
	wf_frame_free(f);
	wf_app_destroy(&a);
}

static void test_scroll_clamps_after_resize_and_fullscreen(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	PreviewSpy spy = {0};
	WfPreviewPort port = {&spy, preview_submit, preview_cancel};
	WfFrame *f;

	CHECK(WF_OK ==
	        wf_app_init(&a, 160, 10, fake_port(&fp), L"C:\\Users\\dev"));
	wf_app_set_preview(&a, port);
	ch(&a, 'j');
	wf_app_on_preview(&a, spy.gen, numbered_text(12, 0));
	key(&a, WF_KEY_RIGHT);
	for (int i = 0; i < 20; i++) {
		key(&a, WF_KEY_DOWN);
	}
	CHECK(8 == a.preview_top);
	ch(&a, 'v'); /* Full screen has three more text rows. */
	CHECK(&WF_MODE_PREVIEW == a.mode && 5 == a.preview_top);
	a.rows = 30; /* Resize events update dimensions in the main event loop.
	              */
	f = wf_frame_new(160, 30, &WF_STYLE_BASE);
	CHECK(NULL != f);
	if (f) {
		wf_app_draw(&a, f); /* The renderer must clamp before another
		                       key arrives. */
		CHECK(frame_row_has(f, 2, " 1 | row01"));
		CHECK(frame_row_has(f, 13, "12 | row12"));
		wf_frame_free(f);
	}
	key(&a, WF_KEY_DOWN);
	CHECK(0 == a.preview_top);
	a.rows = 2;
	key(&a, WF_KEY_DOWN);
	CHECK(0 == a.preview_top);
	wf_app_destroy(&a);
}

static const char *row_text(const WfFrame *f, int y, char *out, size_t cap)
{
	out[0] = 0;
	for (int x = 0; x < f->cols; x++) {
		strcat_s(out, cap, f->cells[y * f->cols + x].utf8);
	}
	return out;
}

/* Horizontal panning: long lines, clamping, resize, reset. */
static void test_horizontal_pan(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	PreviewSpy spy = {0};
	WfPreviewPort port = {&spy, preview_submit, preview_cancel};
	WfFrame *frame;
	char row[8192];
	/* 60 columns on the first line, a short second line; 30-column terminal */
	const char *src =
	        "0123456789ABCDEFGHIJ0123456789abcdefghij0123456789KLMNOPQRST"
	        "\nshort";

	CHECK(WF_OK ==
	        wf_app_init(&a, 30, 10, fake_port(&fp), L"C:\\Users\\dev"));
	wf_app_set_preview(&a, port);
	ch(&a, 'j');
	wf_app_on_preview(&a, spy.gen, text(src));
	CHECK(a.preview && 60 == a.preview->max_cells);
	a.mode = &WF_MODE_PREVIEW;
	frame = wf_frame_new(30, 10, &WF_STYLE_BASE);
	CHECK(NULL != frame);
	if (!frame) {
		return;
	}
	/* gutter "1 | " is 4 columns, leaving 26 for text */
	CHECK(26 == wf_app_preview_text_width(a.preview, 30));
	CHECK(34 == wf_app_preview_pan_limit(a.preview, 30));
	wf_app_draw(&a, frame);
	CHECK(strstr(row_text(frame, 2, row, sizeof row),
	        "0123456789ABCDEFGHIJ012345"));
	key(&a, WF_KEY_RIGHT); /* 8 columns per step */
	CHECK(8 == a.preview_left);
	wf_app_draw(&a, frame);
	CHECK(strstr(row_text(frame, 2, row, sizeof row), "89ABCDEF") &&
	        strstr(row_text(frame, 1, row, sizeof row), "[col 9]"));
	CHECK(NULL == strstr(row_text(frame, 3, row, sizeof row), "short"));
	ch(&a, 'l');
	ch(&a, 'l');
	ch(&a, 'l');
	ch(&a, 'l');
	CHECK(34 == a.preview_left); /* clamped at the widest line */
	wf_app_draw(&a, frame);
	CHECK(strstr(row_text(frame, 2, row, sizeof row), "KLMNOPQRST"));
	key(&a, WF_KEY_HOME);
	CHECK(0 == a.preview_left);
	key(&a, WF_KEY_END);
	CHECK(34 == a.preview_left);
	ch(&a, 'h');
	CHECK(26 == a.preview_left);
	/* a wider terminal lowers the limit; the offset follows it */
	a.cols = 100;
	key(&a, WF_KEY_DOWN);
	CHECK(0 == a.preview_left);
	/* a new preview starts at the left edge */
	a.preview_left = 5;
	wf_app_preview_clear(&a);
	CHECK(0 == a.preview_left);
	wf_frame_free(frame);
	wf_app_destroy(&a);
}

/* A wide character split by the left edge must not draw half a glyph. */
static void test_pan_through_wide_characters(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	PreviewSpy spy = {0};
	WfPreviewPort port = {&spy, preview_submit, preview_cancel};
	WfFrame *frame;
	char row[8192];
	/* 日本語テスト文字列 (8 wide glyphs = 16 columns, then "end") */
	const char *wide = "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x83\x86"
	                   "\xe3\x82\xb9\xe3\x83\x88\xe6\x96\x87\xe5\xad\x97"
	                   "tail-tail-tail-tail";

	CHECK(WF_OK ==
	        wf_app_init(&a, 20, 10, fake_port(&fp), L"C:\\Users\\dev"));
	wf_app_set_preview(&a, port);
	ch(&a, 'j');
	wf_app_on_preview(&a, spy.gen, text(wide));
	a.mode = &WF_MODE_PREVIEW;
	frame = wf_frame_new(20, 10, &WF_STYLE_BASE);
	CHECK(NULL != frame);
	if (!frame) {
		return;
	}
	CHECK(a.preview && a.preview->max_cells == 16 + 19);
	/* offset 1 cuts the first glyph (日): its right half becomes a blank */
	a.preview_left = 1;
	wf_app_draw(&a, frame);
	row_text(frame, 2, row, sizeof row);
	CHECK(NULL == strstr(row, "\xe6\x97\xa5")); /* no 日 */
	CHECK(NULL != strstr(row, "\xe6\x9c\xac")); /* 本 intact */
	/* offset 2 starts exactly at the second glyph */
	a.preview_left = 2;
	wf_app_draw(&a, frame);
	row_text(frame, 2, row, sizeof row);
	CHECK(NULL != strstr(row, "\xe6\x9c\xac") &&
	        !strstr(row, "\xe6\x97\xa5"));
	wf_frame_free(frame);
	wf_app_destroy(&a);
}

int main(void)
{
	test_horizontal_pan();
	test_pan_through_wide_characters();
	test_line_numbers_and_bottom_scroll_limit();
	test_scroll_clamps_after_resize_and_fullscreen();
	test_pane_shows_the_digest_of_its_file();
	test_right_focuses_read_only_pane();
	test_pane_focus_preserves_navigation_and_fetch_policy();
	test_filter_updates_preview_selection();
	WfApp a;
	FakePort fp = {.app = &a};
	PreviewSpy s = {0};
	WfPreviewPort port = {&s, preview_submit, preview_cancel};
	uint32_t old;
	CHECK(WF_OK ==
	        wf_app_init(&a, 120, 30, fake_port(&fp), L"C:\\Users\\dev"));
	wf_app_set_preview(&a, port);
	ch(&a, 'j'); /* Docs -> file2.txt */
	CHECK(a.preview_busy && 1 == s.calls);
	CHECK(!wcscmp(s.path, L"C:\\Users\\dev\\file2.txt"));
	old = s.gen;
	ch(&a, 'j'); /* newer file10 */
	wf_app_on_preview(&a, old, text("stale"));
	CHECK(a.preview_busy &&
	        (!a.preview || WF_PREVIEW_TEXT != a.preview->kind));
	wf_app_on_preview(&a, s.gen, text("current\nsecond"));
	CHECK(!a.preview_busy && a.preview &&
	        !strcmp(a.preview->text.data, "current"));
	ch(&a, 'v');
	CHECK(&WF_MODE_PREVIEW == a.mode);
	ch(&a, 'j');
	CHECK(0 == a.preview_top);
	ch(&a, 'v');
	CHECK(&WF_MODE_NORMAL == a.mode);
	/* Cloud consent applies to only one selection; cursor does not fetch.
	 */
	a.cur.dir->e[a.cur.vis[a.cur.cursor]].flags |= WF_ENT_CLOUD;
	wf_app_reload(&a);
	/* fake listing replaces cloud metadata; set it on the replacement */
	a.cur.dir->e[a.cur.vis[a.cur.cursor]].flags |= WF_ENT_CLOUD;
	ch(&a, 'k');
	ch(&a, 'j');
	CHECK(!a.preview_busy && a.preview &&
	        WF_PREVIEW_REFUSED == a.preview->kind);
	old = (uint32_t)s.calls;
	ch(&a, 'P');
	CHECK(a.preview_busy && (uint32_t)s.calls == old + 1 &&
	        s.explicit_load);
	ch(&a, 'k');
	wf_app_on_preview(&a, s.gen - 1, text("download result"));
	CHECK(!a.preview || WF_PREVIEW_TEXT != a.preview->kind);
	{
		WfPreview *failure =
		        wf_preview_notice(WF_PREVIEW_REFUSED, "read failed");
		WfFrame *frame = wf_frame_new(120, 30, &WF_STYLE_BASE);
		int found = 0;
		CHECK(failure && frame);
		if (failure && frame) {
			failure->system_error = 23;
			wf_app_on_preview(&a, s.gen, failure);
			wf_app_draw(&a, frame);
			for (int y = 0; y < frame->rows; y++) {
				char row[8192] = "";
				for (int x = 0; x < frame->cols; x++) {
					strcat_s(row, sizeof row,
					        frame->cells[y * frame->cols +
					                     x]
					                .utf8);
				}
				found |= strstr(row, "ERROR WIN32_23:") != NULL;
			}
			CHECK(found);
		} else {
			wf_preview_free(failure);
		}
		wf_frame_free(frame);
	}
	wf_app_destroy(&a);
	CHECK(s.cancels > 0);
	return TEST_RESULT();
}
