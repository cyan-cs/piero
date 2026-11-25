/* Same-volume move from the UI: x captures, p checks, and then the items go
 * through the edit port one at a time. The port is a fake that records each
 * request and lets the test decide each receipt. */
#include <string.h>

#include "wf_fake_fs.h"
#include "wf_fake_xfer.h"
#include "wf_test.h"

static FakeProbe P;

typedef struct
{
	unsigned calls;
	uint32_t gen;
	WfEditRequest req[4];
	wchar_t old_name[4][64], parent[4][128], dest[4][128];
	WfStatus answer;
} Spy;

static WfStatus spy_submit(void *ctx, uint32_t gen, const WfEditRequest *req)
{
	Spy *s = ctx;
	unsigned i = s->calls < 4 ? s->calls : 3;

	s->calls++;
	s->gen = gen;
	s->req[i] = *req;
	wcscpy_s(s->old_name[i], 64, req->old_name);
	wcscpy_s(s->parent[i], 128, req->parent);
	wcscpy_s(s->dest[i], 128, req->dest_parent ? req->dest_parent : L"");
	s->req[i].old_name = s->old_name[i];
	s->req[i].parent = s->parent[i];
	s->req[i].dest_parent = s->dest[i];
	return s->answer;
}

static void ch(WfApp *a, uint32_t c)
{
	WfEvent e = {WF_EV_CHAR, c, WF_KEY_NONE, 0};

	a->mode->on_event(a, &e);
}

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
	Spy spy;
} Rig;

/* Browsing C:\Users\dev with file2.txt and file10.txt selected, then x, then
 * into C:\Users and p: the move has been checked and started (if allowed). */
static void rig_move(Rig *r, bool proven_destination, bool with_edit)
{
	memset(r, 0, sizeof *r);
	r->fs.app = &r->app;
	r->xf.app = &r->app;
	r->xf.ops = fp_ops(&P);
	CHECK(wf_app_init(&r->app, 120, 30, fake_port(&r->fs),
	              L"C:\\Users\\dev") == WF_OK);
	wf_app_set_xfer(&r->app, fx_port(&r->xf));
	if (with_edit) {
		wf_app_set_edit(&r->app, (WfEditPort){&r->spy, spy_submit});
	}
	ch(&r->app, 'j'); /* file2.txt */
	ch(&r->app, ' ');
	ch(&r->app, ' '); /* file10.txt */
	ch(&r->app, 'x');
	CHECK(WF_XFER_MOVE == r->app.slot.mode && 2 == r->app.slot.count);
	ch(&r->app, 'h'); /* into C:\Users */
	CHECK(!wcscmp(r->app.cwd, L"C:\\Users"));
	r->app.cur.dir->identity.kind =
	        (proven_destination) ? WF_ID_STRONG : WF_ID_NONE;
	r->app.cur.dir->identity.volume = 7;
	r->app.cur.dir->identity.id[0] = 77;
	ch(&r->app, 'p');
}

static WfEditOutcome ok_receipt(void)
{
	WfEditOutcome o = {WF_OK, 0, true, NULL, NULL, 0};

	return o;
}

static void items_are_sent_one_at_a_time_with_their_identities(void)
{
	static Rig r;
	WfEditOutcome ok = ok_receipt();

	world();
	rig_move(&r, true, true);
	CHECK(1 == r.spy.calls && r.app.edit_busy && r.app.move.active);
	CHECK(WF_EDIT_MOVE == r.spy.req[0].kind &&
	        !wcscmp(r.spy.parent[0], L"C:\\Users\\dev") &&
	        !wcscmp(r.spy.dest[0], L"C:\\Users") &&
	        !wcscmp(r.spy.old_name[0], L"file2.txt"));
	CHECK((r.spy.req[0].parent_id.kind == WF_ID_STRONG) &&
	        (r.spy.req[0].source_id.kind == WF_ID_STRONG) &&
	        (r.spy.req[0].dest_parent_id.kind == WF_ID_STRONG) &&
	        77 == r.spy.req[0].dest_parent_id.id[0]);
	CHECK(strstr(r.app.msg, "Moving"));
	/* the second item is not sent until the first was answered */
	CHECK(1 == r.spy.calls);
	wf_app_on_edit(&r.app, r.spy.gen, &ok, L"file2.txt");
	CHECK(2 == r.spy.calls && r.app.edit_busy &&
	        !wcscmp(r.spy.old_name[1], L"file10.txt"));
	CHECK(r.spy.req[1].source_id.id[0] != r.spy.req[0].source_id.id[0]);
	wf_app_on_edit(&r.app, r.spy.gen, &ok, L"file10.txt");
	CHECK(!r.app.edit_busy && !r.app.move.active);
	CHECK(strstr(r.app.msg, "MOVE done: 2 items"));
	CHECK(0 == r.app.slot.count && WF_XFER_NONE == r.app.slot.mode);
	wf_app_destroy(&r.app);
}

static void a_failure_stops_the_sequence_and_reports_how_far(void)
{
	static Rig r;
	WfEditOutcome ok = ok_receipt();
	WfEditOutcome exists = {WF_E_OS, 183, false, NULL, NULL, 0};

	world();
	rig_move(&r, true, true);
	wf_app_on_edit(&r.app, r.spy.gen, &ok, L"file2.txt");
	wf_app_on_edit(&r.app, r.spy.gen, &exists, L"file10.txt");
	CHECK(2 == r.spy.calls && !r.app.move.active && !r.app.edit_busy);
	CHECK(strstr(r.app.msg, "stopped after 1 of 2") &&
	        strstr(r.app.msg, "already exists"));
	CHECK(0 == r.app.slot.count); /* something moved: the slot is stale */
	wf_app_destroy(&r.app);

	/* a failure on the first item moved nothing: the slot is kept */
	world();
	rig_move(&r, true, true);
	wf_app_on_edit(&r.app, r.spy.gen, &exists, L"file2.txt");
	CHECK(1 == r.spy.calls && !r.app.move.active);
	CHECK(strstr(r.app.msg, "stopped after 0 of 2"));
	CHECK(WF_XFER_MOVE == r.app.slot.mode && 2 == r.app.slot.count);
	wf_app_destroy(&r.app);

	/* a changed item (identity no longer matches) is reported as such */
	world();
	rig_move(&r, true, true);
	{
		WfEditOutcome changed = {WF_E_INVAL, 0, false, NULL, NULL, 0};

		wf_app_on_edit(&r.app, r.spy.gen, &changed, L"file2.txt");
	}
	CHECK(strstr(r.app.msg, "changed since"));
	wf_app_destroy(&r.app);
}

static void nothing_starts_without_proven_identities(void)
{
	static Rig r;

	world();
	rig_move(
	        &r, false, true); /* the listing never proved the destination */
	CHECK(0 == r.spy.calls && !r.app.move.active && !r.app.edit_busy);
	CHECK(strstr(r.app.msg, "MOVE blocked"));
	CHECK(2 == r.app.slot.count); /* nothing was consumed */
	wf_app_destroy(&r.app);

	/* the port refusing to take the request leaves nothing pending */
	world();
	memset(&r, 0, sizeof r);
	r.spy.answer = WF_E_INVAL;
	r.fs.app = &r.app;
	r.xf.app = &r.app;
	r.xf.ops = fp_ops(&P);
	CHECK(wf_app_init(&r.app, 120, 30, fake_port(&r.fs),
	              L"C:\\Users\\dev") == WF_OK);
	wf_app_set_xfer(&r.app, fx_port(&r.xf));
	wf_app_set_edit(&r.app, (WfEditPort){&r.spy, spy_submit});
	ch(&r.app, 'x');
	ch(&r.app, 'h');
	r.app.cur.dir->identity.kind = WF_ID_STRONG;
	ch(&r.app, 'p');
	CHECK(1 == r.spy.calls && !r.app.move.active && !r.app.edit_busy);
	CHECK(strstr(r.app.msg, "could not start"));
	wf_app_destroy(&r.app);
}

static void without_an_edit_port_the_move_stays_blocked(void)
{
	static Rig r;

	world();
	rig_move(&r, true, false);
	CHECK(0 == r.spy.calls && !r.app.move.active);
	CHECK(strstr(r.app.msg, "not available in this build"));
	CHECK(2 == r.app.slot.count);
	wf_app_destroy(&r.app);
}

int main(void)
{
	items_are_sent_one_at_a_time_with_their_identities();
	a_failure_stops_the_sequence_and_reports_how_far();
	nothing_starts_without_proven_identities();
	without_an_edit_port_the_move_stays_blocked();
	return TEST_RESULT();
}
