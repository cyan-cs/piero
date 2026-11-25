/*
 * Create/rename worker against the real filesystem (Windows only). Everything
 * lives in a p0 scratch area: only objects the run recorded are removed.
 *
 * What this pins down is the worker's receipt contract (one request at a time,
 * exactly one answer, nothing overwritten) and that the backend's identity
 * checks reach the product path. The name-rule matrix itself is covered by
 * test_edit_win.c.
 */
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "../tools/p0/p0_lab.h"
#include "fs/wf_editworker.h"
#include "wf_test.h"
#include "xfer/wf_probe_win.h"

static WfEditWorker *g_worker;
static uint32_t g_gen;

/* Submits, waits for the one answer, and checks nothing else arrives. */
static WfEditResult run(const WfEditRequest *req, WfStatus expect_submit)
{
	WfEditResult r;
	WfEditPort port = wf_editworker_port(g_worker);
	WfStatus st = port.submit(port.ctx, ++g_gen, req);

	memset(&r, 0, sizeof r);
	CHECK(st == expect_submit);
	if (WF_OK != st) {
		return r;
	}
	CHECK(WAIT_OBJECT_0 ==
	        WaitForSingleObject(wf_editworker_event(g_worker), 10000));
	CHECK(wf_editworker_poll(g_worker, &r));
	CHECK(r.gen == g_gen && r.kind == req->kind);
	CHECK(!wf_editworker_poll(g_worker, &r)); /* exactly one receipt */
	CHECK(WAIT_TIMEOUT ==
	        WaitForSingleObject(wf_editworker_event(g_worker), 0));
	return r;
}

static bool exists(const wchar_t *path)
{
	return INVALID_FILE_ATTRIBUTES != GetFileAttributesW(path);
}

static bool is_dir(const wchar_t *path)
{
	DWORD a = GetFileAttributesW(path);

	return INVALID_FILE_ATTRIBUTES != a && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool content_is(const wchar_t *path, const char *text)
{
	char buf[64] = "";
	DWORD got = 0;
	HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	ReadFile(h, buf, sizeof buf - 1, &got, NULL);
	CloseHandle(h);
	buf[got] = 0;
	return !strcmp(buf, text);
}

/* What the scenarios share: the owned fixture, the proven identity of the
 * listed folder and of an unrelated one, and the request being edited. Each
 * scenario leaves the folder in the state the next one starts from. */
typedef struct
{
	P0Lab lab;
	WfProbeOps ops;
	WfNodeInfo parent, wrong, item, dest;
	WfEditRequest req;
	wchar_t dir[1024], dest_dir[1024], a[1100], b[1100], c[1100];
} Env;

static const wchar_t uni[] = {
        0x65E5, 0x672C, 0xD842, 0xDFB7, L'.', L't', L'x', L't', 0};

/* The scratch folders, their identities, and the worker. */
static bool setup(Env *v)
{
	CHECK(p0_lab_open(&v->lab, "editworker", 0));
	if (!v->lab.area) {
		return false;
	}
	CHECK(p0_area_mkdir(v->lab.area, L"w"));
	CHECK(p0_area_mkdir(v->lab.area, L"other"));
	CHECK(p0_area_mkdir(v->lab.area, L"movedest"));
	CHECK(p0_lab_path(&v->lab, L"movedest", v->dest_dir, 1024));
	CHECK(WF_OK == v->ops.node(NULL, v->dest_dir, &v->dest) &&
	        (v->dest.id.kind == WF_ID_STRONG));
	CHECK(p0_lab_path(&v->lab, L"w", v->dir, 1024));
	CHECK(WF_OK == v->ops.node(NULL, v->dir, &v->parent) &&
	        (v->parent.id.kind == WF_ID_STRONG));
	CHECK(p0_lab_path(&v->lab, L"other", v->a, 1100));
	CHECK(WF_OK == v->ops.node(NULL, v->a, &v->wrong));
	CHECK(WF_OK == wf_editworker_start(&g_worker));
	return true;
}

/* A file is created, also with a name in the supplementary plane. */
static void creates_files(Env *v)
{
	WfEditResult r;
	wchar_t rel[128];

	memset(&v->req, 0, sizeof v->req);
	v->req.kind = WF_EDIT_CREATE_FILE;
	v->req.parent = v->dir;
	v->req.parent_id = v->parent.id;
	v->req.new_name = L"new.txt";
	r = run(&v->req, WF_OK);
	swprintf(v->a, 1100, L"%ls\\new.txt", v->dir);
	CHECK(WF_OK == r.outcome.st && r.outcome.renamed && exists(v->a));
	CHECK(!wcscmp(r.new_name, L"new.txt"));
	v->req.new_name = uni;
	r = run(&v->req, WF_OK);
	swprintf(v->b, 1100, L"%ls\\%ls", v->dir, uni);
	CHECK(WF_OK == r.outcome.st && exists(v->b));
	swprintf(rel, 128, L"w\\%ls", uni);
	CHECK(p0_area_track(v->lab.area, rel, 0));
}

/* An existing name is never overwritten: its content survives. */
static void never_overwrites(Env *v)
{
	WfEditResult r;
	HANDLE h = CreateFileW(v->a, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD wrote;

	CHECK(INVALID_HANDLE_VALUE != h);
	if (INVALID_HANDLE_VALUE != h) {
		CHECK(WriteFile(h, "keep me", 7, &wrote, NULL));
		CloseHandle(h);
	}
	v->req.new_name = L"new.txt";
	r = run(&v->req, WF_OK);
	CHECK(WF_OK != r.outcome.st && !r.outcome.renamed &&
	        (ERROR_FILE_EXISTS == r.outcome.error ||
	                ERROR_ALREADY_EXISTS == r.outcome.error));
	CHECK(content_is(v->a, "keep me"));
}

static void creates_a_directory(Env *v)
{
	WfEditResult r;

	v->req.kind = WF_EDIT_CREATE_DIR;
	v->req.new_name = L"sub";
	r = run(&v->req, WF_OK);
	swprintf(v->c, 1100, L"%ls\\sub", v->dir);
	CHECK(WF_OK == r.outcome.st &&
	        (GetFileAttributesW(v->c) & FILE_ATTRIBUTE_DIRECTORY));
	CHECK(p0_area_track(v->lab.area, L"w\\sub", 1));
}

/* Rename keeps the object (same identity) and refuses to replace. */
static void renames_in_place(Env *v)
{
	WfEditResult r;
	WfNodeInfo after;

	CHECK(WF_OK == v->ops.node(NULL, v->a, &v->item));
	v->req.kind = WF_EDIT_RENAME;
	v->req.old_name = L"new.txt";
	v->req.source_id = v->item.id;
	v->req.new_name = L"sub"; /* taken by the folder */
	r = run(&v->req, WF_OK);
	CHECK(WF_OK != r.outcome.st && !r.outcome.renamed && exists(v->a) &&
	        exists(v->c) && content_is(v->a, "keep me"));
	v->req.new_name = L"renamed.txt";
	r = run(&v->req, WF_OK);
	swprintf(v->b, 1100, L"%ls\\renamed.txt", v->dir);
	CHECK(WF_OK == r.outcome.st && r.outcome.renamed && !exists(v->a) &&
	        exists(v->b) && content_is(v->b, "keep me"));
	CHECK(WF_OK == v->ops.node(NULL, v->b, &after) &&
	        wf_fileid_equal(&after.id, &v->item.id));
	CHECK(p0_area_track(v->lab.area, L"w\\renamed.txt", 0)); /* cleanup */
}

/* A stale source or parent identity, or an unproven one, fails closed. */
static void stale_identities_fail_closed(Env *v)
{
	WfEditResult r;

	/* some other object's id as the source: nothing moves */
	v->req.old_name = L"renamed.txt";
	v->req.source_id = v->wrong.id;
	v->req.new_name = L"moved.txt";
	r = run(&v->req, WF_OK);
	swprintf(v->c, 1100, L"%ls\\moved.txt", v->dir);
	CHECK(WF_OK != r.outcome.st && !r.outcome.renamed && exists(v->b) &&
	        !exists(v->c));

	/* a stale parent identity fails closed too */
	v->req.source_id = v->item.id;
	v->req.parent_id = v->wrong.id;
	r = run(&v->req, WF_OK);
	CHECK(WF_OK != r.outcome.st && !r.outcome.renamed && exists(v->b) &&
	        !exists(v->c));

	/* an unproven parent identity is refused outright */
	v->req.parent_id = v->parent.id;
	v->req.parent_id.kind = WF_ID_NONE;
	r = run(&v->req, WF_OK);
	CHECK(WF_E_INVAL == r.outcome.st && exists(v->b) && !exists(v->c));
	v->req.parent_id = v->parent.id;
}

/* Name rules reach the receipt with a reason; a vanished source is reported,
 * not invented. */
static void names_and_missing_source(Env *v)
{
	WfEditResult r;

	v->req.new_name = L"CON";
	r = run(&v->req, WF_OK);
	CHECK(WF_OK != r.outcome.st && NULL != r.outcome.name_reason &&
	        exists(v->b));
	v->req.new_name = L"trailing.";
	r = run(&v->req, WF_OK);
	CHECK(WF_OK != r.outcome.st && NULL != r.outcome.name_reason);

	v->req.old_name = L"gone.txt";
	v->req.new_name = L"x.txt";
	r = run(&v->req, WF_OK);
	CHECK(WF_OK != r.outcome.st && !r.outcome.renamed);
}

/* A second request while one is unanswered is refused, never queued. */
static void second_request_is_refused(Env *v)
{
	WfEditPort port = wf_editworker_port(g_worker);
	WfEditRequest second;
	WfEditResult r;

	v->req.kind = WF_EDIT_CREATE_FILE;
	v->req.new_name = L"first.txt";
	second = v->req;
	CHECK(WF_OK == port.submit(port.ctx, ++g_gen, &v->req));
	second.new_name = L"second.txt";
	CHECK(WF_E_INVAL == port.submit(port.ctx, ++g_gen, &second));
	CHECK(WAIT_OBJECT_0 ==
	        WaitForSingleObject(wf_editworker_event(g_worker), 10000));
	/* the answer has not been polled: still busy */
	CHECK(WF_E_INVAL == port.submit(port.ctx, ++g_gen, &second));
	CHECK(wf_editworker_poll(g_worker, &r) && WF_OK == r.outcome.st);
	CHECK(p0_area_track(v->lab.area, L"w\\first.txt", 0));
	swprintf(v->c, 1100, L"%ls\\second.txt", v->dir);
	CHECK(!exists(v->c)); /* the refused request had no effect */
}

/* Invalid requests never reach the filesystem. */
static void invalid_requests_are_refused(Env *v)
{
	WfEditPort port = wf_editworker_port(g_worker);
	WfEditRequest bad = v->req;

	bad.new_name = NULL;
	CHECK(WF_E_INVAL == port.submit(port.ctx, ++g_gen, &bad));
	bad = v->req;
	bad.parent = NULL;
	CHECK(WF_E_INVAL == port.submit(port.ctx, ++g_gen, &bad));
	bad = v->req;
	bad.kind = WF_EDIT_RENAME;
	bad.old_name = NULL;
	CHECK(WF_E_INVAL == port.submit(port.ctx, ++g_gen, &bad));
	CHECK(WF_E_INVAL == port.submit(NULL, ++g_gen, &v->req));
	CHECK(WF_E_INVAL == port.submit(port.ctx, ++g_gen, NULL));
}

/* A request that moves `name` from the listed folder into movedest. */
static WfEditRequest move_request(
        const Env *v, const wchar_t *name, const WfFileId *id)
{
	WfEditRequest q;

	memset(&q, 0, sizeof q);
	q.kind = WF_EDIT_MOVE;
	q.parent = v->dir;
	q.parent_id = v->parent.id;
	q.old_name = name;
	q.source_id = *id;
	q.new_name = name;
	q.dest_parent = v->dest_dir;
	q.dest_parent_id = v->dest.id;
	return q;
}

static void make_file(
        Env *v, const wchar_t *name, const char *content, WfNodeInfo *node)
{
	WfEditRequest q;
	WfEditResult r;
	wchar_t path[1100];
	HANDLE h;
	DWORD wrote;

	memset(&q, 0, sizeof q);
	q.kind = WF_EDIT_CREATE_FILE;
	q.parent = v->dir;
	q.parent_id = v->parent.id;
	q.new_name = name;
	r = run(&q, WF_OK);
	CHECK(WF_OK == r.outcome.st);
	swprintf(path, 1100, L"%ls\\%ls", v->dir, name);
	h = CreateFileW(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	if (INVALID_HANDLE_VALUE != h) {
		CHECK(WriteFile(
		        h, content, (DWORD)strlen(content), &wrote, NULL));
		CloseHandle(h);
	}
	CHECK(WF_OK == v->ops.node(NULL, path, node));
}

/* Same-volume move: the object itself changes folder (same identity, same
 * content); nothing at the destination is ever replaced; stale or unproven
 * identities, a missing source, and a folder moved into itself all fail with
 * both sides unchanged. */
static void moves_between_folders(Env *v)
{
	WfEditRequest q;
	WfEditResult r;
	WfNodeInfo node, after;
	wchar_t src[1100], dst[1100];

	make_file(v, L"mv.txt", "move me", &node);
	swprintf(src, 1100, L"%ls\\mv.txt", v->dir);
	swprintf(dst, 1100, L"%ls\\mv.txt", v->dest_dir);
	q = move_request(v, L"mv.txt", &node.id);
	r = run(&q, WF_OK);
	CHECK(WF_OK == r.outcome.st && r.outcome.renamed && !exists(src) &&
	        exists(dst) && content_is(dst, "move me"));
	CHECK(WF_OK == v->ops.node(NULL, dst, &after) &&
	        wf_fileid_equal(&after.id, &node.id));
	CHECK(p0_area_track(v->lab.area, L"movedest\\mv.txt", 0));

	/* a name already in the destination is never replaced */
	make_file(v, L"mv.txt", "new content", &node);
	CHECK(p0_area_track(v->lab.area, L"w\\mv.txt", 0));
	q = move_request(v, L"mv.txt", &node.id);
	r = run(&q, WF_OK);
	CHECK(WF_OK != r.outcome.st && !r.outcome.renamed &&
	        (ERROR_FILE_EXISTS == r.outcome.error ||
	                ERROR_ALREADY_EXISTS == r.outcome.error));
	CHECK(content_is(src, "new content") && content_is(dst, "move me"));

	/* stale source identity: someone else's object */
	q = move_request(v, L"mv.txt", &v->wrong.id);
	r = run(&q, WF_OK);
	CHECK(WF_OK != r.outcome.st && !r.outcome.renamed &&
	        content_is(src, "new content"));
	/* stale destination folder identity */
	q = move_request(v, L"mv.txt", &node.id);
	q.dest_parent_id = v->wrong.id;
	r = run(&q, WF_OK);
	CHECK(WF_OK != r.outcome.st && !r.outcome.renamed && exists(src));
	/* unproven identities are refused outright */
	q = move_request(v, L"mv.txt", &node.id);
	q.dest_parent_id.kind = WF_ID_NONE;
	r = run(&q, WF_OK);
	CHECK(WF_OK != r.outcome.st && !r.outcome.renamed && exists(src));
	q = move_request(v, L"mv.txt", &node.id);
	q.source_id.kind = WF_ID_NONE;
	r = run(&q, WF_OK);
	CHECK(WF_OK != r.outcome.st && !r.outcome.renamed && exists(src));
	q = move_request(v, L"mv.txt", &node.id);
	q.parent_id.kind = WF_ID_NONE;
	r = run(&q, WF_OK);
	CHECK(WF_OK != r.outcome.st && !r.outcome.renamed && exists(src));
	/* a source that vanished */
	q = move_request(v, L"ghost.txt", &node.id);
	r = run(&q, WF_OK);
	CHECK(WF_OK != r.outcome.st && !r.outcome.renamed);
	/* a name the volume forbids is refused with a reason */
	q = move_request(v, L"mv.txt", &node.id);
	q.new_name = L"CON";
	r = run(&q, WF_OK);
	CHECK(WF_OK != r.outcome.st && NULL != r.outcome.name_reason &&
	        exists(src));

	/* a folder with content moves as one object; moving it into itself fails */
	{
		WfEditRequest d;
		WfNodeInfo dir_node, inner_node;
		wchar_t dir_src[1100], dir_dst[1100], inner[1100], child[1100],
		        child_dst[1100];
		HANDLE h;

		memset(&d, 0, sizeof d);
		d.kind = WF_EDIT_CREATE_DIR;
		d.parent = v->dir;
		d.parent_id = v->parent.id;
		d.new_name = L"mvdir";
		r = run(&d, WF_OK);
		CHECK(WF_OK == r.outcome.st);
		swprintf(dir_src, 1100, L"%ls\\mvdir", v->dir);
		swprintf(inner, 1100, L"%ls\\inner", dir_src);
		swprintf(child, 1100, L"%ls\\child.txt", dir_src);
		CHECK(CreateDirectoryW(inner, NULL));
		h = CreateFileW(child, GENERIC_WRITE, 0, NULL, CREATE_NEW,
		        FILE_ATTRIBUTE_NORMAL, NULL);
		CHECK(INVALID_HANDLE_VALUE != h);
		if (INVALID_HANDLE_VALUE != h) {
			CloseHandle(h);
		}
		CHECK(WF_OK == v->ops.node(NULL, dir_src, &dir_node));
		CHECK(WF_OK == v->ops.node(NULL, inner, &inner_node));

		/* into its own descendant: refused, nothing changes */
		q = move_request(v, L"mvdir", &dir_node.id);
		q.dest_parent = inner;
		q.dest_parent_id = inner_node.id;
		r = run(&q, WF_OK);
		CHECK(WF_OK != r.outcome.st && !r.outcome.renamed &&
		        is_dir(dir_src) && exists(child));

		q = move_request(v, L"mvdir", &dir_node.id);
		r = run(&q, WF_OK);
		swprintf(dir_dst, 1100, L"%ls\\mvdir", v->dest_dir);
		swprintf(child_dst, 1100, L"%ls\\child.txt", dir_dst);
		CHECK(WF_OK == r.outcome.st && !exists(dir_src) &&
		        is_dir(dir_dst) && exists(child_dst));
		CHECK(p0_area_track(v->lab.area, L"movedest\\mvdir", 1));
		CHECK(p0_area_track(v->lab.area, L"movedest\\mvdir\\inner", 1));
		CHECK(p0_area_track(
		        v->lab.area, L"movedest\\mvdir\\child.txt", 0));
	}
}

/* Nothing was superseded or dropped, and stop waits for what ran. */
static void stops_cleanly(void)
{
	WfWorkerStats s = wf_editworker_stats(g_worker);
	WfWorkerStopReport stop;

	CHECK(s.completed_requests >= 25 && 0 == s.superseded_requests &&
	        0 == s.discarded_results);
	stop = wf_editworker_stop(g_worker);
	CHECK(WF_OK == stop.status && !stop.deferred &&
	        stop.stats.completed_requests == s.completed_requests);
}

int main(void)
{
	Env v;

	memset(&v, 0, sizeof v);
	v.ops = wf_probe_win();
	if (!setup(&v)) {
		return 1;
	}
	creates_files(&v);
	never_overwrites(&v);
	creates_a_directory(&v);
	renames_in_place(&v);
	stale_identities_fail_closed(&v);
	names_and_missing_source(&v);
	second_request_is_refused(&v);
	invalid_requests_are_refused(&v);
	moves_between_folders(&v);
	stops_cleanly();
	CHECK(0 == p0_lab_close(&v.lab, 0 != wf_test_failures));
	return TEST_RESULT();
}
