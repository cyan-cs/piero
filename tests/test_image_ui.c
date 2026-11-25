#include <stdlib.h>
#include <string.h>

#include "base/wf_macro.h"
#include "render/wf_pixcell.h"
#include "render/wf_sixel.h"
#include "render/wf_graphics.h"
#include "ui/wf_app_priv.h"
#include "wf_fake_fs.h"
#include "wf_test.h"

/*
 * Pictures and PDF pages in the preview, with the image service replaced by a
 * recorder: what the app asks for, and what it does with the answers.
 */

#define MAX_RQ 64

typedef struct
{
	WfImageRequest rq[MAX_RQ];
	wchar_t path[MAX_RQ][128];
	wchar_t pw[MAX_RQ][64];
	int n, cancels;
	bool fail_submit;
} Spy;

static WfStatus spy_submit(void *ctx, const WfImageRequest *rq)
{
	Spy *s = ctx;

	if (s->fail_submit || s->n >= MAX_RQ) {
		return WF_E_NOMEM;
	}
	s->rq[s->n] = *rq;
	wcscpy_s(s->path[s->n], 128, rq->path);
	s->rq[s->n].path = s->path[s->n];
	if (rq->password) {
		wcscpy_s(s->pw[s->n], 64, rq->password);
		s->rq[s->n].password = s->pw[s->n];
	}
	s->n++;
	return WF_OK;
}

static void spy_cancel(void *ctx)
{
	((Spy *)ctx)->cancels++;
}

static const WfImageRequest *last_fg(const Spy *s)
{
	for (int i = s->n - 1; i >= 0; i--) {
		if (!s->rq[i].prefetch) {
			return &s->rq[i];
		}
	}
	return NULL;
}

static int count_prefetch(const Spy *s)
{
	int n = 0;

	for (int i = 0; i < s->n; i++) {
		n += s->rq[i].prefetch ? 1 : 0;
	}
	return n;
}

static void key(WfApp *a, WfKey code)
{
	WfEvent ev = {WF_EV_KEY, 0, code, 0};

	a->mode->on_event(a, &ev);
}

static void ch(WfApp *a, uint32_t c)
{
	WfEvent ev = {WF_EV_CHAR, c, WF_KEY_NONE, 0};

	a->mode->on_event(a, &ev);
}

static void goto_name(WfApp *a, const wchar_t *name)
{
	for (int i = 0; i < 12; i++) {
		const WfEntry *e = wf_view_current(&a->cur);

		if (e && 0 == wcscmp(e->name, name)) {
			return;
		}
		ch(a, 'j');
	}
	CHECK_FAIL("could not reach the entry");
}

static WfImageAnswer answer_for(const WfImageRequest *rq,
        WfDecodeStatus st,
        uint32_t w,
        uint32_t h,
        uint32_t pages)
{
	WfImageAnswer an;

	memset(&an, 0, sizeof an);
	an.gen = rq->gen;
	an.status = st;
	an.page = rq->page;
	an.info.page_count = pages;
	an.info.src_w = w * 2;
	an.info.src_h = h * 2;
	if (WF_DSTAT_OK == st) {
		CHECK(WF_OK == wf_pixmap_new(w, h, &an.pm));
		for (uint32_t i = 0; i < w * h; i++) {
			an.pm->px[i] =
			        0xFF000000u | (i % w) << 16 | (i / w) << 8;
		}
	}
	return an;
}

static WfApp *make_app(Spy *spy, FakePort *fp, int style, WfApp *a)
{
	WfImagePort port = {spy, spy_submit, spy_cancel};

	fp->app = a;
	CHECK(WF_OK == wf_app_init(a, 120, 30, fake_port(fp), L"C:\\Pics"));
	wf_app_set_image(a, port, style);
	return a;
}

static const WfCell *cell(const WfFrame *f, int x, int y)
{
	return &f->cells[(size_t)y * (size_t)f->cols + (size_t)x];
}

static bool row_has(const WfFrame *f, int y, const char *needle)
{
	char row[8192] = "";

	for (int x = 0; x < f->cols; x++) {
		strcat_s(row, sizeof row, cell(f, x, y)->utf8);
	}
	return NULL != strstr(row, needle);
}

static bool screen_has(WfApp *a, const char *needle)
{
	WfFrame *f = wf_frame_new(a->cols, a->rows, &WF_STYLE_BASE);
	bool found = false;

	if (f) {
		wf_app_draw(a, f);
		for (int y = 0; y < f->rows && !found; y++) {
			found = row_has(f, y, needle);
		}
		wf_frame_free(f);
	}
	return found;
}

static void pictures_are_off_unless_asked_for(void)
{
	static WfApp a;
	static FakePort fp;
	memset(&fp, 0, sizeof fp);
	static Spy spy;
	memset(&spy, 0, sizeof spy);

	/* style DEFAULT (0): the setting was never resolved to a way of drawing */
	make_app(&spy, &fp, WF_IMAGE_DEFAULT, &a);
	goto_name(&a, L"a.png");
	CHECK(0 == spy.n && a.preview &&
	        WF_PREVIEW_IMAGE_BLOCKED == a.preview->kind);
	CHECK(a.preview && strstr(a.preview->detail, "Picture preview is off"));
	goto_name(&a, L"doc.pdf"); /* a PDF is a picture too */
	CHECK(0 == spy.n && a.preview &&
	        WF_PREVIEW_IMAGE_BLOCKED == a.preview->kind);
	wf_app_destroy(&a);

	/* drawing chosen, but no service behind it */
	{
		WfApp b;
		FakePort fp2 = {0};
		WfImagePort none = {0};

		fp2.app = &b;
		CHECK(WF_OK ==
		        wf_app_init(&b, 120, 30, fake_port(&fp2), L"C:\\Pics"));
		wf_app_set_image(&b, none, WF_IMAGE_HALFBLOCK);
		goto_name(&b, L"a.png");
		CHECK(b.preview && WF_PREVIEW_IMAGE_BLOCKED == b.preview->kind);
		wf_app_destroy(&b);
	}
}

static void the_cursor_on_a_picture_asks_for_it_at_the_pane_size(void)
{
	static WfApp a;
	static FakePort fp;
	memset(&fp, 0, sizeof fp);
	static Spy spy;
	memset(&spy, 0, sizeof spy);
	WfLayout lay;
	const WfImageRequest *rq;
	WfImageAnswer an;
	WfFrame *f;
	int px, py;

	make_app(&spy, &fp, WF_IMAGE_HALFBLOCK, &a);
	goto_name(&a, L"a.png");
	rq = last_fg(&spy);
	CHECK(rq && WF_DKIND_IMAGE == rq->kind && 0 == rq->page &&
	        !wcscmp(rq->path, L"C:\\Pics\\a.png") && !rq->password);
	wf_layout_compute(a.cols, a.rows, &a.ratios, &lay);
	/* the area is the pane less one information row; a cell is two pixels tall */
	CHECK(rq->box_w == (uint32_t)lay.pane[WF_PANE_PREVIEW].w &&
	        rq->box_h == (uint32_t)(2 * (lay.body_h - 1)));
	CHECK(WF_IMG_BUSY == a.image.state && wf_app_image_active(&a));
	CHECK(!screen_has(&a, "Loading picture"));
	a.now_ms += WF_PREVIEW_LOADING_MS;
	CHECK(wf_app_tick(&a) == WF_ACT_REDRAW);
	CHECK(screen_has(&a, "Loading picture"));

	an = answer_for(rq, WF_DSTAT_OK, 20, 10, 1);
	wf_app_on_image(&a, &an);
	CHECK(WF_IMG_READY == a.image.state);
	{
		WfPixmap *shown = a.image.pm;
		int requests = spy.n;
		WfImageAnswer duplicate =
		        answer_for(rq, WF_DSTAT_OK, 20, 10, 1);
		wf_app_on_image(&a, &duplicate);
		CHECK(a.image.pm == shown && spy.n == requests);
	}
	f = wf_frame_new(a.cols, a.rows, &WF_STYLE_BASE);
	wf_app_draw(&a, f);
	/* centred in the pane, drawn with half blocks, the info row says the size */
	px = lay.pane[WF_PANE_PREVIEW].x +
	        (lay.pane[WF_PANE_PREVIEW].w - 20) / 2;
	py = lay.body_y + (lay.body_h - 1 - 5) / 2;
	CHECK(0 == strcmp(cell(f, px, py)->utf8, "\xE2\x96\x80"));
	CHECK(row_has(f, lay.body_y + lay.body_h - 1, "40 x 20"));
	wf_frame_free(f);

	/* an answer for a request that was since replaced is dropped */
	an = answer_for(rq, WF_DSTAT_OK, 8, 8, 1);
	an.gen = rq->gen - 1;
	wf_app_on_image(&a, &an); /* takes and frees the pixmap */
	CHECK(WF_IMG_READY == a.image.state && 20 == a.image.pm->w);

	/* the next picture: the old one is gone at once, not shown for the new
	 * file, and the service was told to forget the previous request */
	spy.cancels = 0;
	ch(&a, 'j');
	CHECK(spy.cancels >= 1 && WF_IMG_BUSY == a.image.state && !a.image.pm);
	CHECK(!wcscmp(last_fg(&spy)->path, L"C:\\Pics\\b.png"));
	/* and a text file clears the picture state entirely */
	goto_name(&a, L"note.txt");
	CHECK(WF_IMG_NONE == a.image.state && !wf_app_image_active(&a));
	(void)px;
	wf_app_destroy(&a);
}

static void failures_say_what_happened(void)
{
	static const struct
	{
		WfDecodeStatus st;
		bool refused, unavailable;
		uint32_t os_error;
		const char *text;
	} cases[] = {{WF_DSTAT_CORRUPT, false, false, 0, "damaged or cut off"},
	        {WF_DSTAT_CORRUPT, false, false, 5, "(error 5)"},
	        {WF_DSTAT_UNSUPPORTED, false, false, 0,
	                "not available on this PC"},
	        {WF_DSTAT_UNSUPPORTED, true, false, 0, "cloud placeholder"},
	        {WF_DSTAT_TOO_LARGE, false, false, 0, "too large"},
	        {WF_DSTAT_TIMEOUT, false, false, 0, "took too long"},
	        {WF_DSTAT_HOST_LOST, false, false, 0, "stopped unexpectedly"},
	        {WF_DSTAT_HOST_LOST, false, true, 1260, "isolated decoder"},
	        {WF_DSTAT_INTERNAL, false, false, 0, "preview failed"}};

	for (size_t i = 0; i < WF_ARRAY_LEN(cases); i++) {
		static WfApp a;
		static FakePort fp;
		memset(&fp, 0, sizeof fp);
		static Spy spy;
		memset(&spy, 0, sizeof spy);
		WfImageAnswer an;

		make_app(&spy, &fp, WF_IMAGE_HALFBLOCK, &a);
		a.cols =
		        240; /* wide enough for the whole sentence in the pane */
		goto_name(&a, L"a.png");
		an = answer_for(last_fg(&spy), cases[i].st, 0, 0, 0);
		an.refused = cases[i].refused;
		an.unavailable = cases[i].unavailable;
		an.os_error = cases[i].os_error;
		wf_app_on_image(&a, &an);
		CHECK(WF_IMG_FAILED == a.image.state);
		if (!screen_has(&a, cases[i].text)) {
			printf("  case %zu: missing \"%s\"\n", i,
			        cases[i].text);
			CHECK_FAIL("failure text");
		}
		wf_app_destroy(&a);
	}
}

static void a_submit_that_fails_is_shown_not_hidden(void)
{
	static WfApp a;
	static FakePort fp;
	memset(&fp, 0, sizeof fp);
	static Spy spy;
	memset(&spy, 0, sizeof spy);

	spy.fail_submit = true;
	make_app(&spy, &fp, WF_IMAGE_HALFBLOCK, &a);
	goto_name(&a, L"a.png");
	CHECK(WF_IMG_FAILED == a.image.state &&
	        screen_has(&a, "preview failed"));
	wf_app_destroy(&a);
}

static void pdf_pages_turn_with_keys_and_counts(void)
{
	static WfApp a;
	static FakePort fp;
	memset(&fp, 0, sizeof fp);
	static Spy spy;
	memset(&spy, 0, sizeof spy);
	WfImageAnswer an;

	make_app(&spy, &fp, WF_IMAGE_HALFBLOCK, &a);
	goto_name(&a, L"doc.pdf");
	CHECK(WF_DKIND_PDF == last_fg(&spy)->kind && 0 == last_fg(&spy)->page);
	an = answer_for(last_fg(&spy), WF_DSTAT_OK, 30, 40, 10);
	wf_app_on_image(&a, &an);
	CHECK(WF_IMG_READY == a.image.state && 10 == a.image.info.page_count);
	CHECK(screen_has(&a, "page 1/10"));
	/* the next page is warmed in the service while this one is read */
	CHECK(count_prefetch(&spy) >= 1);
	{
		int found = 0;

		for (int i = 0; i < spy.n; i++) {
			found |= spy.rq[i].prefetch &&
			        WF_DKIND_PDF == spy.rq[i].kind &&
			        1 == spy.rq[i].page;
		}
		CHECK(found);
		CHECK(spy.rq[spy.n - 1].prefetch &&
		        spy.rq[spy.n - 1].kind == WF_DKIND_PDF &&
		        spy.rq[spy.n - 1].page == 1);
	}
	/* the keys of the full-screen preview are the same as the pane's */
	ch(&a, 'v');
	CHECK(&WF_MODE_PREVIEW == a.mode);
	{
		const WfImageRequest *rq = last_fg(&spy);

		/* full screen: a bigger area, so a new request at a new size */
		a.now_ms += 1000;
		(void)wf_app_tick(&a);
		rq = last_fg(&spy);
		CHECK(rq->box_w == (uint32_t)a.cols);
		an = answer_for(rq, WF_DSTAT_OK, 60, 50, 10);
		wf_app_on_image(&a, &an);
	}
	key(&a, WF_KEY_PGDN);
	CHECK(1 == last_fg(&spy)->page && WF_IMG_BUSY == a.image.state);
	an = answer_for(last_fg(&spy), WF_DSTAT_OK, 60, 50, 10);
	wf_app_on_image(&a, &an);
	CHECK(screen_has(&a, "page 2/10"));
	key(&a, WF_KEY_PGDN);
	CHECK(2 == last_fg(&spy)->page);
	an = answer_for(last_fg(&spy), WF_DSTAT_OK, 60, 50, 10);
	wf_app_on_image(&a, &an);
	key(&a, WF_KEY_PGUP);
	CHECK(1 == last_fg(&spy)->page);
	an = answer_for(last_fg(&spy), WF_DSTAT_OK, 60, 50, 10);
	wf_app_on_image(&a, &an);
	/* first and last page; the end does not run past */
	{
		WfEvent top = {WF_EV_KEY, 0, WF_KEY_CTRL_UP, 0};
		WfEvent bottom = {WF_EV_KEY, 0, WF_KEY_CTRL_DOWN, 0};

		a.mode->on_event(&a, &top);
		CHECK(0 == last_fg(&spy)->page);
		an = answer_for(last_fg(&spy), WF_DSTAT_OK, 60, 50, 10);
		wf_app_on_image(&a, &an);
		a.mode->on_event(&a, &bottom);
		CHECK(9 == last_fg(&spy)->page);
		an = answer_for(last_fg(&spy), WF_DSTAT_OK, 60, 50, 10);
		wf_app_on_image(&a, &an);
		{
			int before = spy.n;

			key(&a, WF_KEY_PGDN); /* already the last */
			CHECK(spy.n == before);
		}
	}
	/* a request past the end (the file changed under us) lands on the last */
	{
		key(&a, WF_KEY_PGUP); /* page 9 was valid in the old 10-page snapshot */
		WfImageRequest bad = *last_fg(&spy);
		CHECK(bad.page == 8 && a.image.state == WF_IMG_BUSY);
		an = answer_for(&bad, WF_DSTAT_PAGE_RANGE, 0, 0, 4);
		an.gen = a.image.gen;
		wf_app_on_image(&a, &an);
		CHECK(3 == last_fg(&spy)->page);
	}
	wf_app_destroy(&a);
}

/* The pane has the keys too, once it has the focus (Right). */
static void the_focused_pane_turns_pages(void)
{
	static WfApp a;
	static FakePort fp;
	memset(&fp, 0, sizeof fp);
	static Spy spy;
	memset(&spy, 0, sizeof spy);
	WfImageAnswer an;

	make_app(&spy, &fp, WF_IMAGE_HALFBLOCK, &a);
	goto_name(&a, L"doc.pdf");
	an = answer_for(last_fg(&spy), WF_DSTAT_OK, 30, 40, 2);
	wf_app_on_image(&a, &an);
	key(&a, WF_KEY_RIGHT);
	CHECK(&WF_MODE_PREVIEW_PANE == a.mode);
	key(&a, WF_KEY_PGDN);
	CHECK(1 == last_fg(&spy)->page && WF_IMG_BUSY == a.image.state);
	an = answer_for(last_fg(&spy), WF_DSTAT_OK, 40, 30, 2);
	wf_app_on_image(&a, &an);
	CHECK(screen_has(&a, "page 2/2"));
	key(&a, WF_KEY_PGUP);
	CHECK(0 == last_fg(&spy)->page);
	wf_app_destroy(&a);
}

/* Counts exist in the Vim profile only (a digit is a command key elsewhere). */
static void vim_counts_turn_several_pages_and_jump_to_one(void)
{
	static WfApp a;
	static FakePort fp;
	memset(&fp, 0, sizeof fp);
	static Spy spy;
	memset(&spy, 0, sizeof spy);
	WfKeyConfig keys;
	WfImageAnswer an;

	make_app(&spy, &fp, WF_IMAGE_HALFBLOCK, &a);
	wf_keyconfig_init(&keys);
	keys.profile = WF_PROFILE_VIM;
	CHECK(WF_OK == wf_app_set_keys(&a, &keys, 1));
	goto_name(&a, L"doc.pdf");
	ch(&a, 'v');
	a.now_ms += 1000;
	(void)wf_app_tick(&a);
	an = answer_for(last_fg(&spy), WF_DSTAT_OK, 60, 50, 10);
	wf_app_on_image(&a, &an);
	ch(&a, '3');
	key(&a, WF_KEY_PGDN);
	CHECK(3 == last_fg(&spy)->page); /* 0 + 3 */
	an = answer_for(last_fg(&spy), WF_DSTAT_OK, 60, 50, 10);
	wf_app_on_image(&a, &an);
	ch(&a, '2');
	key(&a, WF_KEY_PGUP);
	CHECK(1 == last_fg(&spy)->page);
	an = answer_for(last_fg(&spy), WF_DSTAT_OK, 60, 50, 10);
	wf_app_on_image(&a, &an);
	/* 7 G goes to page 7, as 7 G goes to line 7 in a text */
	ch(&a, '7');
	ch(&a, 'G');
	CHECK(6 == last_fg(&spy)->page);
	an = answer_for(last_fg(&spy), WF_DSTAT_OK, 60, 50, 10);
	wf_app_on_image(&a, &an);
	ch(&a, 'G'); /* no count: the last page */
	CHECK(9 == last_fg(&spy)->page);
	an = answer_for(last_fg(&spy), WF_DSTAT_OK, 60, 50, 10);
	wf_app_on_image(&a, &an);
	ch(&a, '9');
	ch(&a, '9');
	ch(&a, 'G'); /* past the end: the last page, not an error */
	CHECK(9 == last_fg(&spy)->page);
	wf_app_destroy(&a);
}

static void scrolling_down_turns_the_page_at_the_bottom(void)
{
	static WfApp a;
	static FakePort fp;
	memset(&fp, 0, sizeof fp);
	static Spy spy;
	memset(&spy, 0, sizeof spy);
	WfImageAnswer an;
	int guard = 0;

	make_app(&spy, &fp, WF_IMAGE_HALFBLOCK, &a);
	goto_name(&a, L"doc.pdf");
	ch(&a, 'v');
	a.now_ms += 1000;
	(void)wf_app_tick(&a);
	/* a tall page: 40 wide, 300 pixels high, far more than the screen */
	an = answer_for(last_fg(&spy), WF_DSTAT_OK, 40, 300, 3);
	wf_app_on_image(&a, &an);
	CHECK(0 == a.image.pan_y);
	while (0 == last_fg(&spy)->page && guard++ < 100) {
		key(&a, WF_KEY_DOWN);
	}
	/* it scrolled through the page, then the next page was asked for, from its top */
	CHECK(1 == last_fg(&spy)->page && guard > 3 && guard < 100);
	an = answer_for(last_fg(&spy), WF_DSTAT_OK, 40, 300, 3);
	wf_app_on_image(&a, &an);
	CHECK(0 == a.image.pan_y);
	/* and Up at the top of a page comes back to the bottom of the one before */
	key(&a, WF_KEY_UP);
	CHECK(0 == last_fg(&spy)->page);
	an = answer_for(last_fg(&spy), WF_DSTAT_OK, 40, 300, 3);
	wf_app_on_image(&a, &an);
	{
		uint32_t mx, my;

		wf_pixcell_pan_limits(a.image.pm, a.image.view_cols,
		        a.image.view_rows, &mx, &my);
		CHECK(my > 0 && a.image.pan_y == my);
	}
	wf_app_destroy(&a);
}

static void zoom_and_fit_change_the_request_not_the_picture(void)
{
	static WfApp a;
	static FakePort fp;
	memset(&fp, 0, sizeof fp);
	static Spy spy;
	memset(&spy, 0, sizeof spy);
	WfImageAnswer an;
	uint32_t w0, h0;
	WfEvent plus = {WF_EV_CHAR, '+', WF_KEY_NONE, 0};

	make_app(&spy, &fp, WF_IMAGE_HALFBLOCK, &a);
	goto_name(&a, L"a.png");
	ch(&a, 'v');
	a.now_ms += 1000;
	(void)wf_app_tick(&a);
	an = answer_for(last_fg(&spy), WF_DSTAT_OK, 80, 60, 1);
	wf_app_on_image(&a, &an);
	w0 = last_fg(&spy)->box_w;
	h0 = last_fg(&spy)->box_h;
	a.mode->on_event(&a, &plus);
	CHECK(last_fg(&spy)->box_w == w0 * 150 / 100 &&
	        last_fg(&spy)->box_h == h0 * 150 / 100 && 1 == a.image.zoom);
	an = answer_for(last_fg(&spy), WF_DSTAT_OK, 120, 90, 1);
	wf_app_on_image(&a, &an);
	CHECK(screen_has(&a, "150%"));
	ch(&a, '-');
	CHECK(0 == a.image.zoom && last_fg(&spy)->box_w == w0);
	ch(&a, '-'); /* nothing smaller than fit */
	CHECK(0 == a.image.zoom);
	for (int i = 0; i < 9; i++) {
		a.mode->on_event(&a, &plus);
	}
	CHECK(WF_IMAGE_ZOOM_STEPS - 1 ==
	        a.image.zoom); /* and not beyond the table */
	/* fit width asks for the width and as much height as is allowed */
	ch(&a, 'f');
	CHECK(a.image.fit_width && last_fg(&spy)->box_h > last_fg(&spy)->box_w);
	CHECK((uint64_t)last_fg(&spy)->box_w * last_fg(&spy)->box_h * 4 <=
	        WF_DWIRE_RASTER_MAX);
	ch(&a, 'f');
	CHECK(!a.image.fit_width);
	wf_app_destroy(&a);
}

static void panning_stays_inside_the_picture(void)
{
	static WfApp a;
	static FakePort fp;
	memset(&fp, 0, sizeof fp);
	static Spy spy;
	memset(&spy, 0, sizeof spy);
	WfImageAnswer an;

	make_app(&spy, &fp, WF_IMAGE_HALFBLOCK, &a);
	goto_name(&a, L"a.png");
	ch(&a, 'v');
	a.now_ms += 1000;
	(void)wf_app_tick(&a);
	/* wider and taller than the screen */
	an = answer_for(last_fg(&spy), WF_DSTAT_OK, 300, 200, 1);
	wf_app_on_image(&a, &an);
	for (int i = 0; i < 100; i++) {
		ch(&a, 'l');
	}
	{
		uint32_t mx, my;

		wf_pixcell_pan_limits(a.image.pm, a.image.view_cols,
		        a.image.view_rows, &mx, &my);
		CHECK(mx > 0 && a.image.pan_x == mx);
	}
	for (int i = 0; i < 200; i++) {
		ch(&a, 'h');
	}
	CHECK(0 == a.image.pan_x);
	key(&a, WF_KEY_END);
	CHECK(a.image.pan_x > 0);
	key(&a, WF_KEY_HOME);
	CHECK(0 == a.image.pan_x);
	/* Left at the left edge does nothing in full screen, as for text; in the
	 * pane it hands the keys back to the lists */
	key(&a, WF_KEY_LEFT);
	CHECK(&WF_MODE_PREVIEW == a.mode);
	key(&a, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == a.mode);
	key(&a, WF_KEY_RIGHT);
	CHECK(&WF_MODE_PREVIEW_PANE == a.mode);
	key(&a, WF_KEY_LEFT);
	CHECK(&WF_MODE_NORMAL == a.mode);
	wf_app_destroy(&a);
}

static void a_resize_asks_again_once_it_settles(void)
{
	static WfApp a;
	static FakePort fp;
	memset(&fp, 0, sizeof fp);
	static Spy spy;
	memset(&spy, 0, sizeof spy);
	WfImageAnswer an;
	WfEvent rs = {WF_EV_RESIZE, 0, WF_KEY_NONE, 0};
	int before;

	make_app(&spy, &fp, WF_IMAGE_HALFBLOCK, &a);
	goto_name(&a, L"a.png");
	an = answer_for(last_fg(&spy), WF_DSTAT_OK, 20, 20, 1);
	wf_app_on_image(&a, &an);
	before = spy.n;
	(void)rs;
	a.cols = 150; /* the program writes the new size into the app */
	a.rows = 40;
	a.now_ms += 10;
	(void)wf_app_tick(&a);
	CHECK(spy.n == before); /* not yet: the size may still be changing */
	CHECK(wf_app_image_timeout(&a) > 0 && wf_app_image_timeout(&a) < 1000);
	a.now_ms += 500;
	(void)wf_app_tick(&a);
	CHECK(spy.n == before + 1);
	{
		WfLayout lay;

		wf_layout_compute(a.cols, a.rows, &a.ratios, &lay);
		CHECK(last_fg(&spy)->box_w ==
		        (uint32_t)lay.pane[WF_PANE_PREVIEW].w);
	}
	wf_app_destroy(&a);
}

static void a_password_is_asked_for_typed_and_forgotten(void)
{
	static WfApp a;
	static FakePort fp;
	memset(&fp, 0, sizeof fp);
	static Spy spy;
	memset(&spy, 0, sizeof spy);
	WfImageAnswer an;
	const WfImageRequest *rq;

	make_app(&spy, &fp, WF_IMAGE_HALFBLOCK, &a);
	goto_name(&a, L"doc.pdf");
	ch(&a, 'v');
	a.now_ms += 1000;
	(void)wf_app_tick(&a);
	an = answer_for(last_fg(&spy), WF_DSTAT_PASSWORD_REQUIRED, 0, 0, 0);
	wf_app_on_image(&a, &an);
	CHECK(screen_has(&a, "press Enter to type the password"));
	key(&a, WF_KEY_ENTER);
	CHECK(&WF_MODE_IMAGE_PASSWORD == a.mode);
	ch(&a, 's');
	ch(&a, 'e');
	ch(&a, 0x3042); /* a Japanese letter is one character */
	CHECK(screen_has(&a, "Password: ***_") &&
	        !screen_has(&a, "Password: se"));
	key(&a, WF_KEY_BACKSPACE);
	ch(&a, 'c');
	key(&a, WF_KEY_ENTER);
	CHECK(&WF_MODE_PREVIEW == a.mode);
	rq = last_fg(&spy);
	CHECK(rq->password && 0 == wcscmp(rq->password, L"sec"));
	an = answer_for(rq, WF_DSTAT_PASSWORD_WRONG, 0, 0, 0);
	wf_app_on_image(&a, &an);
	CHECK(screen_has(&a, "Wrong password"));
	/* Esc leaves the prompt and the screen as it was */
	key(&a, WF_KEY_ENTER);
	ch(&a, 'x');
	key(&a, WF_KEY_ESC);
	CHECK(&WF_MODE_PREVIEW == a.mode && WF_IMG_FAILED == a.image.state);
	/* the right one: the page opens, and later pages carry the password too */
	key(&a, WF_KEY_ENTER);
	for (const char *p = "secret"; *p; p++) {
		ch(&a, (uint32_t)*p);
	}
	key(&a, WF_KEY_ENTER);
	an = answer_for(last_fg(&spy), WF_DSTAT_OK, 30, 30, 4);
	wf_app_on_image(&a, &an);
	key(&a, WF_KEY_PGDN);
	CHECK(last_fg(&spy)->password &&
	        0 == wcscmp(last_fg(&spy)->password, L"secret"));
	/* moving to another file forgets it for good */
	key(&a, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == a.mode);
	ch(&a, 'j');
	CHECK(0 == a.image.password[0] && 0 == a.image.typing_n);
	wf_app_destroy(&a);
}

static void ascii_style_draws_characters_not_blocks(void)
{
	static WfApp a;
	static FakePort fp;
	memset(&fp, 0, sizeof fp);
	static Spy spy;
	memset(&spy, 0, sizeof spy);
	WfImageAnswer an;
	WfFrame *f;
	WfLayout lay;

	make_app(&spy, &fp, WF_IMAGE_ASCII, &a);
	goto_name(&a, L"a.png");
	an = answer_for(last_fg(&spy), WF_DSTAT_OK, 40, 20, 1);
	wf_app_on_image(&a, &an);
	f = wf_frame_new(a.cols, a.rows, &WF_STYLE_BASE);
	wf_app_draw(&a, f);
	wf_layout_compute(a.cols, a.rows, &a.ratios, &lay);
	{
		bool block = false;

		for (int y = lay.body_y; y < lay.body_y + lay.body_h; y++) {
			for (int x = lay.pane[WF_PANE_PREVIEW].x; x < f->cols;
			        x++) {
				block |= 0 ==
				        strcmp(cell(f, x, y)->utf8,
				                "\xE2\x96\x80");
			}
		}
		CHECK(!block);
	}
	wf_frame_free(f);
	wf_app_destroy(&a);
}

static void a_pane_too_narrow_for_pictures_asks_for_nothing(void)
{
	static WfApp a;
	static FakePort fp;
	memset(&fp, 0, sizeof fp);
	static Spy spy;
	memset(&spy, 0, sizeof spy);
	WfImagePort port = {&spy, spy_submit, spy_cancel};

	fp.app = &a;
	CHECK(WF_OK == wf_app_init(&a, 50, 20, fake_port(&fp), L"C:\\Pics"));
	wf_app_set_image(&a, port, WF_IMAGE_HALFBLOCK);
	goto_name(&a, L"a.png");
	CHECK(0 ==
	        spy.n); /* no preview pane at this width: nothing to draw it in */
	a.cols = 160;
	a.rows = 40;
	a.now_ms += 1000;
	(void)wf_app_tick(&a);
	CHECK(1 == spy.n && !wcscmp(last_fg(&spy)->path, L"C:\\Pics\\a.png"));
	wf_app_destroy(&a);
}

static void sixel_requests_pixels_and_drops_stale_generations(void)
{
	static WfApp a;
	static FakePort fp;
	memset(&fp, 0, sizeof fp);
	static Spy spy;
	memset(&spy, 0, sizeof spy);
	WfImageAnswer an;
	WfImageRequest rq;
	WfFrame *f;
	make_app(&spy, &fp, WF_IMAGE_HALFBLOCK, &a);
	a.image_cell_w = 10;
	a.image_cell_h = 20;
	goto_name(&a, L"a.png");
	rq = *last_fg(&spy);
	CHECK(rq.sixel && rq.box_w == (uint32_t)a.image.view_cols * 10);
	CHECK(rq.box_h == (uint32_t)a.image.view_rows * 20);
	an = answer_for(&rq, WF_DSTAT_OK, 100, 120, 1);
	CHECK(WF_OK == wf_sixel_encode(an.pm, 0, 256, &an.sixel));
	an.sixel_w = 100;
	an.sixel_h = 120;
	wf_app_on_image(&a, &an);
	f = wf_frame_new(a.cols, a.rows, &WF_STYLE_BASE);
	wf_app_draw(&a, f);
	CHECK(f->graphic.len && f->graphic.w == 10 && f->graphic.h == 6);
	CHECK(f->graphic.x + f->graphic.w <= a.cols);
	CHECK(f->graphic.y + f->graphic.h < a.rows - 1);
	{
		WfBuf emitted = {0};
		FILE *capture = NULL;
		CHECK(WF_OK == wf_graphics_present(NULL, f, &emitted));
		CHECK(strstr(emitted.data, "\x1bP0;1;0q") != NULL);
		if (!fopen_s(&capture, "sixel-app-output.vt", "wb") &&
		        capture) {
			CHECK(fwrite(emitted.data, 1, emitted.len, capture) ==
			        emitted.len);
			fclose(capture);
		} else {
			CHECK_FAIL("could not save app output");
		}
		wf_buf_free(&emitted);
	}
	wf_frame_free(f);
	goto_name(&a, L"doc.pdf");
	CHECK(last_fg(&spy)->gen != rq.gen);
	an = answer_for(&rq, WF_DSTAT_OK, 10, 10, 1);
	CHECK(WF_OK == wf_buf_puts(&an.sixel, "stale"));
	wf_app_on_image(&a, &an);
	CHECK(!a.image.pm && !a.image.sixel.len);
	rq = *last_fg(&spy);
	an = answer_for(&rq, WF_DSTAT_OK, 100, 100, 2);
	wf_app_on_image(&a, &an); /* encoder refusal: safe text re-render */
	CHECK(a.image.sixel_failed && !last_fg(&spy)->sixel);
	CHECK(last_fg(&spy)->box_w == (uint32_t)a.image.view_cols);
	wf_app_destroy(&a);
}

static void image_targets_retry_without_modal_spinning(void)
{
	static WfApp a;
	static FakePort fp;
	memset(&fp, 0, sizeof fp);
	static Spy spy;
	memset(&spy, 0, sizeof spy);
	make_app(&spy, &fp, WF_IMAGE_HALFBLOCK, &a);
	a.image_cell_w = 10;
	a.image_cell_h = 20;
	goto_name(&a, L"a.png");
	WfImageRequest rq = *last_fg(&spy);
	WfImageAnswer an = answer_for(&rq, WF_DSTAT_OK, 2000, 2000, 1);
	CHECK(wf_buf_puts(&an.sixel, "payload") == WF_OK);
	an.sixel_w = an.sixel_h = 100;
	wf_app_on_image(&a, &an);
	a.image.pan_x = 100;
	uint32_t expected =
	        (uint32_t)(((uint64_t)100 +
	                           (uint64_t)a.image.view_cols * 10 / 2) *
	                65535 / 2000);
	a.image_cell_w = 20;
	(void)wf_app_image_tick(&a);
	CHECK(a.image.due != 0);
	a.mode = &WF_MODE_SETTINGS;
	a.now_ms += 121;
	CHECK(wf_app_image_timeout(&a) == WF_WAIT_FOREVER);
	(void)wf_app_image_tick(&a);
	CHECK(wf_app_image_timeout(&a) == WF_WAIT_FOREVER);
	a.mode = &WF_MODE_NORMAL;
	(void)wf_app_image_tick(&a);
	CHECK(a.image.focus_x == expected);
	rq = *last_fg(&spy);
	an = answer_for(&rq, WF_DSTAT_OK, 100, 100, 1);
	wf_app_on_image(&a, &an);
	CHECK(a.image.sixel_failed && !last_fg(&spy)->sixel);
	rq = *last_fg(&spy);
	an = answer_for(&rq, WF_DSTAT_OK, 100, 100, 1);
	wf_app_on_image(&a, &an);
	a.image_quality = 2;
	(void)wf_app_image_tick(&a);
	a.now_ms += 121;
	(void)wf_app_image_tick(&a);
	CHECK(!a.image.sixel_failed && last_fg(&spy)->sixel);
	wf_app_destroy(&a);
}

static void delayed_pdf_never_replaces_a_newer_selection(void)
{
	static WfApp a;
	FakePort fp = {0};
	Spy spy = {0};
	make_app(&spy, &fp, WF_IMAGE_HALFBLOCK, &a);
	goto_name(&a, L"doc.pdf");
	WfImageRequest pdf = *last_fg(&spy);
	ch(&a, 'k');
	ch(&a, 'k');
	goto_name(&a, L"a.png");
	WfImageRequest image = *last_fg(&spy);
	CHECK(pdf.gen != image.gen && spy.cancels > 0);
	WfImageAnswer newer = answer_for(&image, WF_DSTAT_OK, 8, 8, 1);
	wf_app_on_image(&a, &newer);
	WfPixmap *shown = a.image.pm;
	WfImageAnswer older = answer_for(&pdf, WF_DSTAT_OK, 8, 8, 18);
	older.sensitive = true;
	wf_app_on_image(&a, &older);
	CHECK(a.image.pm == shown && a.image.kind == WF_DKIND_IMAGE);
	/* A pending PDF can be left without waiting for the decoder or a clock. */
	goto_name(&a, L"doc.pdf");
	WfImageRequest pending = *last_fg(&spy);
	ch(&a, 'h');
	CHECK(wcscmp(a.cwd, L"C:\\Pics") != 0);
	WfImageAnswer after_folder =
	        answer_for(&pending, WF_DSTAT_OK, 8, 8, 18);
	after_folder.sensitive = true;
	wf_app_on_image(&a, &after_folder);
	CHECK(a.image.pm == NULL);
	wf_app_destroy(&a);
}

static void an_idle_pdf_keeps_its_picture_without_new_requests(void)
{
	static WfApp a;
	FakePort fp = {0};
	Spy spy = {0};
	make_app(&spy, &fp, WF_IMAGE_HALFBLOCK, &a);
	goto_name(&a, L"doc.pdf");
	WfImageAnswer page = answer_for(last_fg(&spy), WF_DSTAT_OK, 8, 8, 18);
	page.sensitive = true;
	wf_app_on_image(&a, &page);
	WfPixmap *shown = a.image.pm;
	int requests = spy.n;
	for (unsigned i = 0; i < 300; i++) {
		a.now_ms += 100;
		(void)wf_app_tick(&a);
		CHECK(a.image.pm == shown && a.image.state == WF_IMG_READY);
		CHECK(spy.n == requests);
	}
	wf_app_destroy(&a);
}

int main(void)
{
	image_targets_retry_without_modal_spinning();
	sixel_requests_pixels_and_drops_stale_generations();
	pictures_are_off_unless_asked_for();
	delayed_pdf_never_replaces_a_newer_selection();
	an_idle_pdf_keeps_its_picture_without_new_requests();
	the_cursor_on_a_picture_asks_for_it_at_the_pane_size();
	failures_say_what_happened();
	a_submit_that_fails_is_shown_not_hidden();
	pdf_pages_turn_with_keys_and_counts();
	the_focused_pane_turns_pages();
	vim_counts_turn_several_pages_and_jump_to_one();
	scrolling_down_turns_the_page_at_the_bottom();
	zoom_and_fit_change_the_request_not_the_picture();
	panning_stays_inside_the_picture();
	a_resize_asks_again_once_it_settles();
	a_password_is_asked_for_typed_and_forgotten();
	ascii_style_draws_characters_not_blocks();
	a_pane_too_narrow_for_pictures_asks_for_nothing();
	return TEST_RESULT();
}
