/*
 * What an Undo entry keeps after a job took some of its items back: the first
 * `done` of a move go, the rest stay for another try; a recycle job that took
 * some of what a copy made leaves the others. This is written down after the
 * work was done, and it needs no memory of its own: nothing here can fail, so
 * an entry is never left naming what is no longer there.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "ui/wf_app.h"
#include "ui/wf_app_priv.h"
#include "wf_fake_fs.h"
#include "wf_test.h"

static wchar_t *dup(const wchar_t *s)
{
	size_t n = wcslen(s) + 1;
	wchar_t *p = malloc(n * sizeof *p);

	memcpy(p, s, n * sizeof *p);
	return p;
}

static uint32_t push_entry(WfApp *app, WfUndoKind kind, size_t n)
{
	WfUndoEntry e;

	memset(&e, 0, sizeof e);
	e.kind = kind;
	e.folder = dup(L"C:\\dest");
	e.origin = dup(L"C:\\src");
	e.item = calloc(n, sizeof *e.item);
	e.n = n;
	for (size_t i = 0; i < n; i++) {
		wchar_t name[16];

		swprintf(name, 16, L"item%zu", i);
		e.item[i].name = dup(name);
		e.item[i].old_name = dup(name);
	}
	return wf_app_undo_push(app, &e);
}

static void the_first_done_items_go_and_the_rest_stay(void)
{
	WfApp app;
	FakePort fp = {.app = &app};
	uint32_t uid;

	CHECK(WF_OK ==
	        wf_app_init(&app, 120, 30, fake_port(&fp), L"C:\\Users\\dev"));
	uid = push_entry(&app, WF_UNDO_MOVE, 5);
	CHECK(uid && 1 == app.undo_n);
	/* nothing came back: whole */
	app.undo_inflight = uid;
	wf_app_undo_done(&app, 0);
	CHECK(1 == app.undo_n && 5 == app.undo[0].n && 0 == app.undo_inflight);
	/* two came back: the other three stay, in order */
	app.undo_inflight = uid;
	wf_app_undo_done(&app, 2);
	CHECK(1 == app.undo_n && 3 == app.undo[0].n);
	CHECK(!wcscmp(app.undo[0].item[0].name, L"item2") &&
	        !wcscmp(app.undo[0].item[1].name, L"item3") &&
	        !wcscmp(app.undo[0].item[2].name, L"item4"));
	CHECK(!wcscmp(app.undo[0].item[0].old_name, L"item2"));
	/* a stale answer for an entry that is not in flight changes nothing */
	app.undo_inflight = uid + 99;
	wf_app_undo_done(&app, 1);
	CHECK(3 == app.undo[0].n);
	/* all of what is left came back: the entry goes */
	app.undo_inflight = uid;
	wf_app_undo_done(&app, 3);
	CHECK(0 == app.undo_n);
	wf_app_destroy(&app);
}

static void a_recycle_that_took_some_leaves_the_others(void)
{
	WfApp app;
	FakePort fp = {.app = &app};
	WfCopyResult r;
	uint32_t uid;

	CHECK(WF_OK ==
	        wf_app_init(&app, 120, 30, fake_port(&fp), L"C:\\Users\\dev"));
	uid = push_entry(&app, WF_UNDO_RECYCLE_CREATED, 4);
	memset(&r, 0, sizeof r);
	r.job = 21;
	r.item_n = 4;
	r.item_done = calloc(4, 1);
	r.item_done[0] = 1;
	r.item_done[2] = 1;
	app.recycling[0].job = 21;
	app.recycling[0].uid = uid;
	app.recycling_n = 1;
	wf_app_undo_recycle_result(&app, &r);
	CHECK(1 == app.undo_n && 2 == app.undo[0].n);
	CHECK(!wcscmp(app.undo[0].item[0].name, L"item1") &&
	        !wcscmp(app.undo[0].item[1].name, L"item3"));
	/* without the job's report the entry stays whole */
	app.recycling[0].job = 22;
	app.recycling[0].uid = uid;
	app.recycling_n = 1;
	r.job = 22;
	free(r.item_done);
	r.item_done = NULL;
	r.item_n = 0;
	wf_app_undo_recycle_result(&app, &r);
	CHECK(1 == app.undo_n && 2 == app.undo[0].n);
	wf_app_destroy(&app);
}

int main(void)
{
	the_first_done_items_go_and_the_rest_stay();
	a_recycle_that_took_some_leaves_the_others();
	return TEST_RESULT();
}
