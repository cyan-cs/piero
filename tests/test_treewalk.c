/*
 * The walk of the folders below one folder (DR-DRAFT-031): nearest first, each folder
 * read once, links and cloud folders shown but not entered, hidden entries only when
 * asked, an unreadable folder counted and passed, a stop from outside and one from
 * the visitor, a path that gets too long, too many folders waiting. A fake tree in
 * memory stands in for the disk.
 */
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "search/wf_treewalk.h"
#include "wf_test.h"

typedef struct
{
	const wchar_t *name;
	uint32_t flags;
} Item;

typedef struct
{
	const wchar_t *path;
	WfStatus st; /* what listing it says */
	bool truncated;
	Item item[8];
} Node;

#define D WF_ENT_DIR

static const Node TREE[] = {
        {L"R", WF_OK, false, {{L"a", D}, {L"b", D}, {L"f1.txt", 0}, {NULL, 0}}},
        {L"R\\a", WF_OK, false, {{L"a1", D}, {L"f2.txt", 0}, {NULL, 0}}},
        {L"R\\b", WF_OK, false, {{L"f3.txt", 0}, {NULL, 0}}},
        {L"R\\a\\a1", WF_OK, false, {{L"f4.txt", 0}, {NULL, 0}}},
        {NULL, WF_OK, false, {{NULL, 0}}}};

typedef struct
{
	const Node *tree;
	wchar_t seen[64][48]; /* names visited, in order */
	size_t depth[64];
	WfFileId dir_id[64];
	size_t n_seen;
	wchar_t listed[64][48]; /* folders listed, in order */
	size_t n_listed;
	size_t stop_after, cancel_after; /* 0: never */
	bool cancel_now;
	size_t visits;
	WfFileId id_of_child; /* what the fake gives the folder `a` */
} Run;

static WfStatus fake_list(void *ctx, const wchar_t *dir, WfDir **out)
{
	Run *r = ctx;

	if (r->n_listed < 64) {
		wcsncpy_s(r->listed[r->n_listed], 48, dir, _TRUNCATE);
	}
	r->n_listed++;
	for (const Node *n = r->tree; n->path; n++) {
		if (!wcscmp(n->path, dir)) {
			WfDir *d;

			if (WF_OK != n->st) {
				return n->st;
			}
			d = wf_dir_new();
			CHECK(NULL != d);
			for (const Item *it = n->item; it->name; it++) {
				CHECK(WF_OK ==
				        wf_dir_add(d, it->name,
				                wcslen(it->name), it->flags, 3,
				                5));
				if (!wcscmp(it->name, L"a")) {
					d->e[d->n - 1].identity =
					        r->id_of_child;
				}
			}
			d->truncated = n->truncated;
			*out = d;
			return WF_OK;
		}
	}
	return WF_E_NOENT;
}

static bool visit(void *ctx, const WfWalkItem *item)
{
	Run *r = ctx;

	if (r->n_seen < 64) {
		wcsncpy_s(r->seen[r->n_seen], 48, item->e->name, _TRUNCATE);
		r->depth[r->n_seen] = item->depth;
		r->dir_id[r->n_seen] = item->dir_id;
	}
	r->n_seen++;
	r->visits++;
	return !(r->stop_after && r->visits >= r->stop_after);
}

static bool cancelled(void *ctx)
{
	Run *r = ctx;

	return r->cancel_now ||
	        (r->cancel_after && r->visits >= r->cancel_after);
}

static WfWalkOps ops(bool hidden)
{
	WfWalkOps o = {fake_list, visit, cancelled, hidden, 0};

	return o;
}

static void order_is_nearest_first_and_each_folder_is_read_once(void)
{
	Run r = {.tree = TREE};
	WfWalkOps o = ops(false);
	WfWalkStats s;
	WfFileId none = {0};
	static const wchar_t *want[] = {
	        L"a", L"b", L"f1.txt", L"a1", L"f2.txt", L"f3.txt", L"f4.txt"};
	static const size_t depth[] = {0, 0, 0, 1, 1, 1, 2};

	CHECK(WF_OK == wf_walk(&o, &r, L"R", none, &s));
	CHECK(7 == r.n_seen);
	for (size_t i = 0; i < 7 && i < r.n_seen; i++) {
		CHECK(!wcscmp(r.seen[i], want[i]));
		CHECK(depth[i] == r.depth[i]);
	}
	CHECK(4 == r.n_listed && 4 == s.folders && 7 == s.entries);
	CHECK(!wcscmp(r.listed[0], L"R") && !wcscmp(r.listed[1], L"R\\a") &&
	        !wcscmp(r.listed[2], L"R\\b") &&
	        !wcscmp(r.listed[3], L"R\\a\\a1"));
	CHECK(0 == s.unreadable && 0 == s.not_entered && !s.pending_full &&
	        !s.cancelled && !s.stopped);
}

static void a_child_folder_carries_the_identity_its_listing_showed(void)
{
	Run r = {.tree = TREE};
	WfWalkOps o = ops(false);
	WfFileId root = {.kind = WF_ID_STRONG, .volume = 7};
	WfWalkStats s;

	r.id_of_child.kind = WF_ID_STRONG;
	r.id_of_child.volume = 7;
	r.id_of_child.id[0] = 0x2a;
	CHECK(WF_OK == wf_walk(&o, &r, L"R", root, &s));
	/* the first three entries are in R: its identity is the one given */
	CHECK((r.dir_id[0].kind == WF_ID_STRONG) && 7 == r.dir_id[0].volume &&
	        0 == r.dir_id[0].id[0]);
	/* a1 and f2.txt are in R\a: the one R's listing showed for a */
	CHECK((r.dir_id[3].kind == WF_ID_STRONG) && 0x2a == r.dir_id[3].id[0]);
	CHECK((r.dir_id[4].kind == WF_ID_STRONG) && 0x2a == r.dir_id[4].id[0]);
	/* b's was not set: not valid, never taken for a proof */
	CHECK(!(r.dir_id[5].kind == WF_ID_STRONG));
}

static void links_and_cloud_folders_are_shown_and_not_entered(void)
{
	static const Node tree[] = {
	        {L"R", WF_OK, false,
	                {{L"link", D | WF_ENT_LINK},
	                        {L"cloud", D | WF_ENT_CLOUD}, {L"real", D},
	                        {L"flink", WF_ENT_LINK}, {NULL, 0}}},
	        {L"R\\link", WF_OK, false, {{L"never1", 0}, {NULL, 0}}},
	        {L"R\\cloud", WF_OK, false, {{L"never2", 0}, {NULL, 0}}},
	        {L"R\\real", WF_OK, false, {{L"yes", 0}, {NULL, 0}}},
	        {NULL, WF_OK, false, {{NULL, 0}}}};
	Run r = {.tree = tree};
	WfWalkOps o = ops(false);
	WfWalkStats s;
	WfFileId none = {0};

	CHECK(WF_OK == wf_walk(&o, &r, L"R", none, &s));
	CHECK(5 == r.n_seen); /* link, cloud, real, flink, yes */
	CHECK(2 == s.not_entered && 2 == s.folders);
	for (size_t i = 0; i < r.n_listed; i++) {
		CHECK(wcscmp(r.listed[i], L"R\\link") &&
		        wcscmp(r.listed[i], L"R\\cloud"));
	}
}

static void hidden_entries_only_when_asked(void)
{
	static const Node tree[] = {
	        {L"R", WF_OK, false,
	                {{L".git", D | WF_ENT_HIDDEN}, {L"vis", D},
	                        {L"secret.txt", WF_ENT_HIDDEN}, {NULL, 0}}},
	        {L"R\\.git", WF_OK, false, {{L"config", 0}, {NULL, 0}}},
	        {L"R\\vis", WF_OK, false, {{L"x.txt", 0}, {NULL, 0}}},
	        {NULL, WF_OK, false, {{NULL, 0}}}};
	WfFileId none = {0};
	WfWalkStats s;
	Run r = {.tree = tree};
	WfWalkOps o = ops(false);

	CHECK(WF_OK == wf_walk(&o, &r, L"R", none, &s));
	CHECK(2 == r.n_seen && !wcscmp(r.seen[0], L"vis") &&
	        !wcscmp(r.seen[1], L"x.txt"));
	CHECK(2 == s.folders);

	memset(&r, 0, sizeof r);
	r.tree = tree;
	o = ops(true);
	CHECK(WF_OK == wf_walk(&o, &r, L"R", none, &s));
	CHECK(5 == r.n_seen && 3 == s.folders);
}

static void an_unreadable_folder_is_counted_and_the_rest_goes_on(void)
{
	static const Node tree[] = {
	        {L"R", WF_OK, false, {{L"locked", D}, {L"open", D}, {NULL, 0}}},
	        {L"R\\locked", WF_E_ACCESS, false, {{NULL, 0}}},
	        {L"R\\open", WF_OK, false, {{L"here.txt", 0}, {NULL, 0}}},
	        {NULL, WF_OK, false, {{NULL, 0}}}};
	Run r = {.tree = tree};
	WfWalkOps o = ops(false);
	WfWalkStats s;
	WfFileId none = {0};

	CHECK(WF_OK == wf_walk(&o, &r, L"R", none, &s));
	CHECK(1 == s.unreadable && 2 == s.folders && 3 == r.n_seen);

	/* the root itself cannot be read */
	memset(&r, 0, sizeof r);
	r.tree = tree;
	CHECK(WF_OK == wf_walk(&o, &r, L"R\\locked", none, &s));
	CHECK(1 == s.unreadable && 0 == s.folders && 0 == r.n_seen);
	CHECK(WF_OK == wf_walk(&o, &r, L"R\\absent", none, &s));
	CHECK(1 == s.unreadable);
}

/* What a folder says about itself is counted and passed. Out of memory is not
 * about the folder: the walk ends with it, and is not reported as having met an
 * unreadable folder. A cancelled listing is the walk cancelled. */
static void memory_and_cancellation_in_a_listing_end_the_walk(void)
{
	static const Node nomem[] = {
	        {L"R", WF_OK, false, {{L"a", D}, {L"b", D}, {NULL, 0}}},
	        {L"R\\a", WF_E_NOMEM, false, {{NULL, 0}}},
	        {L"R\\b", WF_OK, false, {{L"f.txt", 0}, {NULL, 0}}},
	        {NULL, WF_OK, false, {{NULL, 0}}}};
	static const Node cut[] = {
	        {L"R", WF_OK, false, {{L"a", D}, {L"b", D}, {NULL, 0}}},
	        {L"R\\a", WF_E_CANCELLED, false, {{NULL, 0}}},
	        {L"R\\b", WF_OK, false, {{L"f.txt", 0}, {NULL, 0}}},
	        {NULL, WF_OK, false, {{NULL, 0}}}};
	Run r = {.tree = nomem};
	WfWalkOps o = ops(false);
	WfWalkStats s;
	WfFileId none = {0};

	CHECK(WF_E_NOMEM == wf_walk(&o, &r, L"R", none, &s));
	CHECK(0 == s.unreadable && !s.cancelled);
	CHECK(1 == s.folders); /* b was never read: the walk is over */
	memset(&r, 0, sizeof r);
	r.tree = cut;
	CHECK(WF_OK == wf_walk(&o, &r, L"R", none, &s));
	CHECK(s.cancelled && 0 == s.unreadable && 1 == s.folders);
}

static void a_listing_cut_short_is_counted(void)
{
	static const Node tree[] = {{L"R", WF_OK, true, {{L"x", 0}, {NULL, 0}}},
	        {NULL, WF_OK, false, {{NULL, 0}}}};
	Run r = {.tree = tree};
	WfWalkOps o = ops(false);
	WfWalkStats s;
	WfFileId none = {0};

	CHECK(WF_OK == wf_walk(&o, &r, L"R", none, &s));
	CHECK(1 == s.partial && 1 == r.n_seen);
}

static void the_visitor_and_the_caller_can_stop_it(void)
{
	Run r = {.tree = TREE};
	WfWalkOps o = ops(false);
	WfWalkStats s;
	WfFileId none = {0};

	r.stop_after = 4;
	CHECK(WF_OK == wf_walk(&o, &r, L"R", none, &s));
	CHECK(4 == r.n_seen && s.stopped && !s.cancelled);
	CHECK(2 == r.n_listed); /* R and R\a: not one folder more */

	memset(&r, 0, sizeof r);
	r.tree = TREE;
	r.cancel_after = 4;
	CHECK(WF_OK == wf_walk(&o, &r, L"R", none, &s));
	CHECK(s.cancelled && !s.stopped && r.n_seen < 7);

	memset(&r, 0, sizeof r);
	r.tree = TREE;
	r.cancel_now = true;
	CHECK(WF_OK == wf_walk(&o, &r, L"R", none, &s));
	CHECK(s.cancelled && 0 == r.n_listed && 0 == r.n_seen);
}

/* A chain of folders that makes a path longer than the walk will enter. */
static const Node *long_tree;
static WfStatus chain_list(void *ctx, const wchar_t *dir, WfDir **out)
{
	WfDir *d = wf_dir_new();
	static wchar_t name[210];

	(void)ctx;
	(void)long_tree;
	for (int i = 0; i < 200; i++) {
		name[i] = L'n';
	}
	name[200] = 0;
	CHECK(NULL != d);
	CHECK(WF_OK == wf_dir_add(d, name, 200, WF_ENT_DIR, 0, 0));
	CHECK(WF_OK == wf_dir_add(d, L"leaf.txt", 8, 0, 1, 1));
	*out = d;
	(void)dir;
	return WF_OK;
}

static size_t chain_visits;
static bool chain_visit(void *ctx, const WfWalkItem *item)
{
	(void)ctx;
	(void)item;
	chain_visits++;
	return true;
}

static void a_path_that_gets_too_long_is_not_entered(void)
{
	WfWalkOps o = {chain_list, chain_visit, NULL, false, 0};
	WfWalkStats s;
	WfFileId none = {0};

	chain_visits = 0;
	CHECK(WF_OK == wf_walk(&o, NULL, L"R", none, &s));
	/* 201 units a level: the walk stops going down near WF_WALK_MAX_PATH */
	CHECK(s.not_entered >= 1);
	CHECK(s.folders > 100 && s.folders < 200);
	CHECK(2 * s.folders ==
	        chain_visits); /* two entries in each folder read */
}

/* More folders waiting than the walk keeps: it says it did not finish. */
static WfStatus wide_list(void *ctx, const wchar_t *dir, WfDir **out)
{
	WfDir *d = wf_dir_new();

	(void)ctx;
	CHECK(NULL != d);
	if (!wcscmp(dir, L"R")) {
		for (unsigned i = 0; i < WF_WALK_MAX_PENDING + 50; i++) {
			wchar_t name[16];

			swprintf(name, 16, L"d%u", i);
			CHECK(WF_OK ==
			        wf_dir_add(d, name, wcslen(name), WF_ENT_DIR, 0,
			                0));
		}
	}
	*out = d;
	return WF_OK;
}

static size_t wide_visits;
static bool wide_visit(void *ctx, const WfWalkItem *item)
{
	(void)ctx;
	(void)item;
	wide_visits++;
	return true;
}

static void too_many_waiting_folders_is_said(void)
{
	WfWalkOps o = {wide_list, wide_visit, NULL, false, 0};
	WfWalkStats s;
	WfFileId none = {0};

	wide_visits = 0;
	CHECK(WF_OK == wf_walk(&o, NULL, L"R", none, &s));
	CHECK(s.pending_full);
	CHECK(WF_WALK_MAX_PENDING + 1 ==
	        s.folders); /* the root and what was kept */
	CHECK(WF_WALK_MAX_PENDING + 50 ==
	        wide_visits); /* every entry was still seen */
}

/* The paths that wait are bounded in bytes too, not only in number. */
static WfStatus long_names_list(void *ctx, const wchar_t *dir, WfDir **out)
{
	WfDir *d = wf_dir_new();

	(void)ctx;
	CHECK(NULL != d);
	if (!wcscmp(dir, L"R")) {
		for (unsigned i = 0; i < 3000; i++) {
			wchar_t name[128];

			swprintf(name, 128, L"%0100u", i); /* 100 units */
			CHECK(WF_OK ==
			        wf_dir_add(d, name, wcslen(name), WF_ENT_DIR, 0,
			                0));
		}
	}
	*out = d;
	return WF_OK;
}

static void waiting_paths_are_bounded_in_bytes(void)
{
	WfWalkOps o = {long_names_list, wide_visit, NULL, false, 4000};
	WfWalkStats s;
	WfFileId none = {0};
	/* "R\" and 100 units of name, and the NUL */
	const size_t each = (2 + 100 + 1) * sizeof(wchar_t);

	wide_visits = 0;
	CHECK(WF_OK == wf_walk(&o, NULL, L"R", none, &s));
	CHECK(s.pending_full); /* it says it did not finish */
	CHECK(1 + 4000 / each ==
	        s.folders);         /* the root and what the budget held */
	CHECK(3000 == wide_visits); /* every entry was still seen */
}

static void misuse_is_refused(void)
{
	Run r = {.tree = TREE};
	WfWalkOps o = ops(false);
	WfWalkOps none = {NULL, visit, NULL, false, 0};
	WfFileId id = {0};

	CHECK(WF_E_INVAL == wf_walk(NULL, &r, L"R", id, NULL));
	CHECK(WF_E_INVAL == wf_walk(&none, &r, L"R", id, NULL));
	CHECK(WF_E_INVAL == wf_walk(&o, &r, NULL, id, NULL));
	CHECK(WF_E_INVAL == wf_walk(&o, &r, L"", id, NULL));
	CHECK(WF_OK ==
	        wf_walk(&o, &r, L"R", id, NULL)); /* stats are optional */
}

int main(void)
{
	order_is_nearest_first_and_each_folder_is_read_once();
	a_child_folder_carries_the_identity_its_listing_showed();
	links_and_cloud_folders_are_shown_and_not_entered();
	hidden_entries_only_when_asked();
	an_unreadable_folder_is_counted_and_the_rest_goes_on();
	memory_and_cancellation_in_a_listing_end_the_walk();
	a_listing_cut_short_is_counted();
	the_visitor_and_the_caller_can_stop_it();
	a_path_that_gets_too_long_is_not_entered();
	too_many_waiting_folders_is_said();
	waiting_paths_are_bounded_in_bytes();
	misuse_is_refused();
	return TEST_RESULT();
}
