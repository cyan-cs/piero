#include <stdio.h>
#include <string.h>

#include "wf_fake_fs.h"
#include "wf_fake_xfer.h"
#include "wf_test.h"

static FakeProbe P;

static void ch(WfApp *a, uint32_t c)
{
	WfEvent e = {WF_EV_CHAR, c, WF_KEY_NONE, 0};

	a->mode->on_event(a, &e);
}

static void esc(WfApp *a)
{
	WfEvent e = {WF_EV_KEY, 0, WF_KEY_ESC, 0};

	a->mode->on_event(a, &e);
}

/* The app browses fake_fs (C:\Users\dev: Docs/ file2.txt file10.txt) while
 * preflight sees the same names in P, plus whatever a test adds. */
static void world(void)
{
	fp_init(&P);
	fp_dir(&P, L"C:\\Users", 2);
	fp_dir(&P, L"C:\\Users\\dev", 3);
	fp_dir(&P, L"C:\\Users\\dev\\Docs", 4);
	fp_file(&P, L"C:\\Users\\dev\\file2.txt", 20, 5);
	fp_file(&P, L"C:\\Users\\dev\\file10.txt", 100, 6);
}

typedef struct
{
	WfApp app;
	FakePort fs;
	FakeXfer xf;
	FakeExec ex;
} Rig;

static void rig_open(Rig *r, const wchar_t *start, int with_exec)
{
	memset(r, 0, sizeof *r);
	r->fs.app = &r->app;
	r->xf.app = &r->app;
	r->xf.ops = fp_ops(&P);
	CHECK(WF_OK == wf_app_init(&r->app, 120, 30, fake_port(&r->fs), start));
	wf_app_set_xfer(&r->app, fx_port(&r->xf));
	if (with_exec) {
		wf_app_set_exec(&r->app, fe_port(&r->ex));
	}
}

/* Gets past the modal's type-ahead guard the way the event loop would. */
static void arm(WfApp *a)
{
	a->now_ms = 1000;
	wf_app_on_presented(a);
	a->now_ms = 2000;
	wf_app_tick(a);
}

static const wchar_t *slot_name(const WfApp *a, size_t i)
{
	return a->slot.src[i].name;
}

static int status_has(WfApp *a, const char *needle)
{
	WfFrame *f = wf_frame_new(a->cols, a->rows, &WF_STYLE_BASE);
	char row[1024] = "";
	int x, found;

	wf_app_draw(a, f);
	for (x = 0; x < f->cols; x++) {
		strcat_s(row, sizeof row,
		        f->cells[(f->rows - 1) * f->cols + x].utf8);
	}
	found = NULL != strstr(row, needle);
	wf_frame_free(f);
	return found;
}

static int screen_has(WfApp *a, const char *needle)
{
	WfFrame *f = wf_frame_new(a->cols, a->rows, &WF_STYLE_BASE);
	int y, x, found = 0;

	wf_app_draw(a, f);
	for (y = 0; y < f->rows && !found; y++) {
		char row[1024] = "";

		for (x = 0; x < f->cols; x++) {
			strcat_s(row, sizeof row,
			        f->cells[y * f->cols + x].utf8);
		}
		found = NULL != strstr(row, needle);
	}
	wf_frame_free(f);
	return found;
}

static void test_capture_cursor_and_selection(void)
{
	static Rig r;

	world();
	rig_open(&r, L"C:\\Users\\dev", 0);

	ch(&r.app, 'y'); /* cursor is on Docs/ */
	CHECK(WF_XFER_COPY == r.app.slot.mode && 1 == r.app.slot.count);
	CHECK(0 == wcscmp(slot_name(&r.app, 0), L"Docs"));
	CHECK(status_has(&r.app, "COPY 1 item"));

	/* a selection wins over the cursor and is moved, not copied */
	ch(&r.app, 'j'); /* file2.txt */
	ch(&r.app, ' '); /* select file2.txt, cursor -> file10.txt */
	ch(&r.app, ' '); /* select file10.txt */
	ch(&r.app, 'x');
	CHECK(WF_XFER_MOVE == r.app.slot.mode && 2 == r.app.slot.count);
	CHECK(0 == wcscmp(slot_name(&r.app, 0), L"file2.txt"));
	CHECK(0 == wcscmp(slot_name(&r.app, 1), L"file10.txt"));
	CHECK(0 == r.app.slot.hidden_excluded);
	CHECK(2 ==
	        wf_sel_count(
	                &r.app.sel, r.app.cwd)); /* selection is left alone */
	CHECK(status_has(&r.app, "MOVE 2 items"));
	wf_app_destroy(&r.app);
}

static void test_hidden_selection_rules(void)
{
	static Rig r;

	world();
	rig_open(&r, L"C:\\Users\\dev", 0);
	ch(&r.app, ' '); /* Docs */
	ch(&r.app, ' '); /* file2.txt */

	/* filter that hides every selected entry: refuse, never fall back to
	 * the cursor */
	ch(&r.app, '/');
	ch(&r.app, '1');
	ch(&r.app, '0'); /* only file10.txt matches */
	esc(&r.app);     /* keep "10"? Esc discards it; re-apply via Enter */
	ch(&r.app, '/');
	ch(&r.app, '1');
	ch(&r.app, '0');
	{
		WfEvent en = {WF_EV_KEY, 0, WF_KEY_ENTER, 0};

		r.app.mode->on_event(&r.app, &en);
	}
	CHECK(1 == r.app.cur.nvis);
	ch(&r.app, 'y');
	CHECK(WF_XFER_NONE == r.app.slot.mode);
	CHECK(NULL != strstr(r.app.msg, "hidden"));
	CHECK(0 == r.xf.n_capture);

	/* partially visible: act on the visible part and remember what was left
	 * out */
	ch(&r.app, 'k'); /* nothing to move; cursor stays */
	ch(&r.app, ' '); /* also select file10.txt, the only visible entry */
	ch(&r.app, 'y');
	CHECK(WF_XFER_COPY == r.app.slot.mode && 1 == r.app.slot.count);
	CHECK(0 == wcscmp(slot_name(&r.app, 0), L"file10.txt"));
	CHECK(2 == r.app.slot.hidden_excluded); /* Docs and file2.txt */
	wf_app_destroy(&r.app);
}

static void test_capture_failure_keeps_old_slot(void)
{
	static Rig r;

	world();
	rig_open(&r, L"C:\\Users\\dev", 0);
	ch(&r.app, 'y');
	CHECK(1 == r.app.slot.count);

	fp_find(&P, L"C:\\Users\\dev\\file2.txt")->path[0] =
	        L'Z'; /* vanished */
	ch(&r.app, 'j');
	ch(&r.app, 'y');
	CHECK(NULL != strstr(r.app.msg, "NOENT"));
	CHECK(1 == r.app.slot.count &&
	        0 == wcscmp(slot_name(&r.app, 0), L"Docs"));
	CHECK(WF_XB_IDLE == r.app.xfer_busy);
	wf_app_destroy(&r.app);
}

static void test_unavailable_and_empty(void)
{
	static Rig r;

	world();
	rig_open(&r, L"C:\\Users\\dev", 0);
	ch(&r.app, 'p');
	CHECK(NULL != strstr(r.app.msg, "empty"));
	CHECK(0 == r.xf.n_preflight);

	wf_app_set_xfer(&r.app, (WfXferPort){0});
	ch(&r.app, 'y');
	CHECK(NULL != strstr(r.app.msg, "not available") &&
	        WF_XFER_NONE == r.app.slot.mode);
	wf_app_destroy(&r.app);
}

static void test_escape_order(void)
{
	static Rig r;

	world();
	rig_open(&r, L"C:\\Users\\dev", 0);
	ch(&r.app, ' ');
	ch(&r.app, 'y');
	CHECK(WF_XFER_COPY == r.app.slot.mode &&
	        1 == wf_sel_count(&r.app.sel, r.app.cwd));
	esc(&r.app);
	CHECK(0 == wf_sel_count(&r.app.sel, r.app.cwd) &&
	        WF_XFER_COPY == r.app.slot.mode);
	esc(&r.app);
	CHECK(WF_XFER_NONE == r.app.slot.mode && 0 == r.app.slot.count);
	esc(&r.app); /* nothing left: harmless */
	wf_app_destroy(&r.app);
}

static void test_same_folder_and_collision(void)
{
	static Rig r;

	world();
	rig_open(&r, L"C:\\Users\\dev", 1);
	ch(&r.app, 'y');
	ch(&r.app, 'p'); /* paste where it came from */
	CHECK(NULL != strstr(r.app.msg, "COPY FAILED"));
	CHECK(NULL != strstr(r.app.msg, "same folder"));
	CHECK(0 == r.ex.calls);

	ch(&r.app, 'h'); /* now in C:\Users, which already has a "Docs" */
	fp_dir(&P, L"C:\\Users\\Docs", 50);
	ch(&r.app, 'p');
	CHECK(NULL != strstr(r.app.msg, "destination already exists"));
	CHECK(NULL != strstr(r.app.msg, "[Docs]"));
	CHECK(0 == r.ex.calls &&
	        WF_XFER_COPY == r.app.slot.mode); /* slot survives a refusal */
	wf_app_destroy(&r.app);
}

static void test_ok_without_engine(void)
{
	static Rig r;

	world();
	rig_open(&r, L"C:\\Users\\dev", 0);
	ch(&r.app, 'y');
	ch(&r.app, 'h');
	ch(&r.app, 'p');
	CHECK(NULL != strstr(r.app.msg, "Checked"));
	CHECK(NULL != strstr(r.app.msg, "not available in this build"));
	CHECK(WF_XFER_COPY == r.app.slot.mode); /* nothing was consumed */
	CHECK(WF_XB_IDLE == r.app.xfer_busy);
	wf_app_destroy(&r.app);
}

static void test_ok_with_engine_seam(void)
{
	static Rig r;

	world();
	rig_open(&r, L"C:\\Users\\dev", 1);
	ch(&r.app, 'y');
	ch(&r.app, 'h');
	ch(&r.app, 'p');
	CHECK(1 == r.ex.calls && 0 == r.ex.hydrate && 1 == r.ex.count);
	CHECK(0 == wcscmp(r.ex.dest, L"C:\\Users"));

	r.ex.result = WF_E_OS;
	ch(&r.app, 'p');
	CHECK(2 == r.ex.calls && NULL != strstr(r.app.msg, "FAILED"));
	wf_app_destroy(&r.app);
}

static void test_cross_volume_copy_instead(void)
{
	static Rig r;

	world();
	rig_open(&r, L"C:\\Users\\dev", 1);
	fp_find(&P, L"C:\\Users")->vol =
	        7; /* destination lives on another volume */
	ch(&r.app, 'x');
	ch(&r.app, 'h');
	ch(&r.app, 'p');
	CHECK(&WF_MODE_CONFIRM == r.app.mode && 0 == r.ex.calls);
	CHECK(screen_has(&r.app, "different") || r.app.confirm.n > 0);
	CHECK('c' == r.app.confirm.yes_ch);
	arm(&r.app);

	ch(&r.app, 'y'); /* a stray yes must do nothing */
	CHECK(&WF_MODE_CONFIRM == r.app.mode && 0 == r.ex.calls);

	ch(&r.app, 'c');
	CHECK(&WF_MODE_NORMAL == r.app.mode);
	CHECK(WF_XFER_COPY ==
	        r.app.slot.mode); /* switched, and re-checked as a copy */
	CHECK(1 == r.ex.calls);

	/* declining leaves the move in the slot */
	world();
	wf_app_destroy(&r.app);
	rig_open(&r, L"C:\\Users\\dev", 1);
	fp_find(&P, L"C:\\Users")->vol = 7;
	ch(&r.app, 'x');
	ch(&r.app, 'h');
	ch(&r.app, 'p');
	esc(&r.app);
	CHECK(&WF_MODE_NORMAL == r.app.mode &&
	        WF_XFER_MOVE == r.app.slot.mode && 0 == r.ex.calls);
	wf_app_destroy(&r.app);
}

static void test_cloud_hydrate_confirmation(void)
{
	static Rig r;

	world();
	rig_open(&r, L"C:\\Users\\dev", 1);
	fp_find(&P, L"C:\\Users\\dev\\file2.txt")->flags |= WF_ENT_CLOUD;
	fp_find(&P, L"C:\\Users\\dev\\file2.txt")->size = 3ull << 30;
	ch(&r.app, 'j'); /* file2.txt */
	ch(&r.app, 'y');
	ch(&r.app, 'h');
	ch(&r.app, 'p');
	CHECK(&WF_MODE_CONFIRM == r.app.mode && 0 == r.ex.calls);
	CHECK('h' == r.app.confirm.yes_ch);
	CHECK(NULL != strstr(r.app.confirm.lines[1], "1 cloud-only"));
	CHECK(NULL != strstr(r.app.confirm.lines[2], "3.0 GiB"));
	arm(&r.app);
	ch(&r.app, 'y'); /* yes must not start a download */
	CHECK(&WF_MODE_CONFIRM == r.app.mode && 0 == r.ex.calls);
	ch(&r.app, 'H');
	CHECK(&WF_MODE_NORMAL == r.app.mode && 1 == r.ex.calls &&
	        1 == r.ex.hydrate);

	/* a same-volume move reads no data, so it never asks about the cloud */
	ch(&r.app, 'l'); /* no-op */
	ch(&r.app, 'x');
	CHECK(WF_XFER_MOVE == r.app.slot.mode);
	wf_app_destroy(&r.app);
}

static void test_big_move_confirmation(void)
{
	static Rig r;
	static wchar_t names_store[120][16];
	const wchar_t *names[120];
	int i;
	WfTransferSlot s;

	world();
	rig_open(&r, L"C:\\Users", 1);
	for (i = 0; i < 120; i++) {
		wchar_t p[64];

		swprintf(names_store[i], 16, L"bulk%03d", i);
		swprintf(p, 64, L"C:\\Users\\dev\\%ls", names_store[i]);
		fp_file(&P, p, 1, 1000 + (uint64_t)i);
		names[i] = names_store[i];
	}

	/* 99 items: no extra question */
	CHECK(wf_xfer_capture(&r.xf.ops, WF_XFER_MOVE, L"C:\\Users\\dev", names,
	              99, 0, &s) == WF_OK);
	wf_slot_free(&r.app.slot);
	r.app.slot = s;
	ch(&r.app, 'p');
	CHECK(&WF_MODE_NORMAL == r.app.mode && 1 == r.ex.calls);

	/* 100 items: asks, shows counts, destination, and what was left out */
	CHECK(wf_xfer_capture(&r.xf.ops, WF_XFER_MOVE, L"C:\\Users\\dev", names,
	              100, 3, &s) == WF_OK);
	wf_slot_free(&r.app.slot);
	r.app.slot = s;
	ch(&r.app, 'p');
	CHECK(&WF_MODE_CONFIRM == r.app.mode && 1 == r.ex.calls);
	CHECK(0 == strcmp(r.app.confirm.lines[0], "MOVE"));
	CHECK(0 == strcmp(r.app.confirm.lines[1], "Move 100 selected items?"));
	CHECK(strcmp(r.app.confirm.lines[2],
	              "Files 100  Directories 0  Links 0") == 0);
	CHECK(0 ==
	        strcmp(r.app.confirm.lines[3],
	                "Hidden selections excluded: 3"));
	CHECK(0 == strcmp(r.app.confirm.lines[4], "Destination:"));
	CHECK(0 == strcmp(r.app.confirm.lines[5], "C:\\Users"));
	CHECK('y' == r.app.confirm.yes_ch);

	esc(&r.app); /* No / Esc / Enter all leave things untouched */
	CHECK(1 == r.ex.calls);
	ch(&r.app, 'p');
	arm(&r.app);
	ch(&r.app, 'Y');
	CHECK(2 == r.ex.calls && 100 == r.ex.count &&
	        &WF_MODE_NORMAL == r.app.mode);
	wf_app_destroy(&r.app);
}

static void test_busy_and_stale_results(void)
{
	static Rig r;
	WfPreflightReport rep;

	world();
	rig_open(&r, L"C:\\Users\\dev", 1);
	r.xf.defer = 1;

	ch(&r.app, 'y');
	CHECK(WF_XB_CAPTURE == r.app.xfer_busy && 1 == r.xf.n_capture);
	CHECK(status_has(&r.app, "reading selection"));
	ch(&r.app, 'y'); /* second request while one is running is refused */
	CHECK(1 == r.xf.n_capture && NULL != strstr(r.app.msg, "busy"));
	ch(&r.app, 'p');
	CHECK(0 == r.xf.n_preflight);

	/* an answer for some other request is ignored */
	{
		WfTransferSlot s;

		CHECK(wf_xfer_capture(&r.xf.ops, WF_XFER_COPY,
		              L"C:\\Users\\dev",
		              (const wchar_t *const[]){L"Docs"}, 1, 0,
		              &s) == WF_OK);
		wf_app_on_capture(&r.app, r.xf.last_gen + 9, WF_OK, &s);
		CHECK(0 == s.count &&
		        WF_XFER_NONE ==
		                r.app.slot.mode); /* freed, not adopted */
		CHECK(WF_XB_CAPTURE == r.app.xfer_busy);

		CHECK(wf_xfer_capture(&r.xf.ops, WF_XFER_COPY,
		              L"C:\\Users\\dev",
		              (const wchar_t *const[]){L"Docs"}, 1, 0,
		              &s) == WF_OK);
		wf_app_on_capture(&r.app, r.xf.last_gen, WF_OK, &s);
		CHECK(WF_XFER_COPY == r.app.slot.mode &&
		        WF_XB_IDLE == r.app.xfer_busy);
	}

	ch(&r.app, 'h');
	ch(&r.app, 'p');
	CHECK(WF_XB_PREFLIGHT == r.app.xfer_busy &&
	        status_has(&r.app, "checking"));
	memset(&rep, 0, sizeof rep);
	rep.reason = WF_PF_OK;
	wf_app_on_preflight(&r.app, r.xf.last_gen + 1, WF_OK, &rep); /* stale */
	CHECK(WF_XB_PREFLIGHT == r.app.xfer_busy && 0 == r.ex.calls);
	wf_app_on_preflight(&r.app, r.xf.last_gen, WF_OK, &rep);
	CHECK(WF_XB_IDLE == r.app.xfer_busy && 1 == r.ex.calls);

	/* an infrastructure failure is reported, not mistaken for success */
	ch(&r.app, 'p');
	wf_app_on_preflight(&r.app, r.xf.last_gen, WF_E_NOMEM, &rep);
	CHECK(NULL != strstr(r.app.msg, "NOMEM") && 1 == r.ex.calls);
	wf_app_destroy(&r.app);
}

int main(void)
{
	test_capture_cursor_and_selection();
	test_hidden_selection_rules();
	test_capture_failure_keeps_old_slot();
	test_unavailable_and_empty();
	test_escape_order();
	test_same_folder_and_collision();
	test_ok_without_engine();
	test_ok_with_engine_seam();
	test_cross_volume_copy_instead();
	test_cloud_hydrate_confirmation();
	test_big_move_confirmation();
	test_busy_and_stale_results();
	return TEST_RESULT();
}
