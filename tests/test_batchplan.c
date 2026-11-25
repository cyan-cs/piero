/*
 * Batch rename/move planning (DR-DRAFT-022): refusal cases and step ordering,
 * checked against a model folder without changing the filesystem.
 */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xfer/wf_batchplan.h"
#include "wf_test.h"

#define FOLDER L"C:\\Work"
#define MAXN 32

static const uint8_t BATCH[16] = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0xab};

static WfFileId fid(unsigned n)
{
	WfFileId id;

	memset(&id, 0, sizeof id);
	id.volume = 7;
	id.id[0] = (uint8_t)n;
	id.id[1] = (uint8_t)(n >> 8);
	id.kind = WF_ID_STRONG;
	return id;
}

typedef struct
{
	WfBatchParent parent[2];
	size_t n_parent;
	const wchar_t *names[2][MAXN];
	WfBatchItem item[MAXN];
	size_t n;
} World;

/* One folder with `names`; every name is also an existing file. */
static void world(World *w, const wchar_t *const *names, size_t n)
{
	memset(w, 0, sizeof *w);
	w->n_parent = 1;
	w->parent[0].path = FOLDER;
	w->parent[0].id = fid(1000);
	w->parent[0].component_max = 255;
	for (size_t i = 0; i < n; i++) {
		w->names[0][i] = names[i];
	}
	w->parent[0].names = w->names[0];
	w->parent[0].n_names = n;
}

static void second_folder(
        World *w, const wchar_t *path, const wchar_t *const *names, size_t n)
{
	w->n_parent = 2;
	w->parent[1].path = path;
	w->parent[1].id = fid(1001);
	w->parent[1].component_max = 255;
	for (size_t i = 0; i < n; i++) {
		w->names[1][i] = names[i];
	}
	w->parent[1].names = w->names[1];
	w->parent[1].n_names = n;
}

/* item `old` of the folder gets `to` in the same folder. */
static void ren(World *w, const wchar_t *old, const wchar_t *to, bool dir)
{
	WfBatchItem *it = &w->item[w->n];

	it->parent = 0;
	it->to_parent = 0;
	it->name = old;
	it->to_name = to;
	it->dir = dir;
	it->id = fid((unsigned)w->n + 1);
	w->n++;
}

static WfStatus plan(World *w, WfBatchPlan *p)
{
	return wf_batch_plan(w->parent, w->n_parent, w->item, w->n, BATCH, p);
}

/* Plays the steps on a model of the folder; false at the first step that would
 * land on a name that is taken, or that is not where the item is. */
static bool play(const World *w, const WfBatchPlan *p)
{
	const wchar_t *at[MAXN];
	wchar_t *own[MAXN] = {0};
	bool ok = true;

	for (size_t i = 0; i < w->n; i++) {
		at[i] = w->item[i].name;
	}
	for (size_t s = 0; s < p->n_steps && ok; s++) {
		const WfBatchStep *st = &p->step[s];
		const wchar_t *name = st->to_name;

		for (size_t i = 0; i < w->n; i++) {
			if (i != st->item && at[i] && !_wcsicmp(at[i], name) &&
			        w->item[i].to_parent == st->to_parent) {
				ok = false; /* taken */
			}
		}
		at[st->item] = name;
	}
	for (size_t i = 0; i < w->n && ok; i++) {
		ok = at[i] && !wcscmp(at[i], w->item[i].to_name);
	}
	(void)own;
	return ok;
}

static void a_plain_rename_is_one_step_each(void)
{
	static const wchar_t *const names[] = {
	        L"a.txt", L"b.txt", L"other.txt"};
	World w;
	WfBatchPlan p;

	world(&w, names, 3);
	ren(&w, L"a.txt", L"x1.txt", false);
	ren(&w, L"b.txt", L"x2.txt", false);
	CHECK(WF_OK == plan(&w, &p));
	CHECK(0 == p.n_refused && 2 == p.n_steps && 0 == p.n_cycles);
	CHECK(play(&w, &p));
	wf_batch_plan_free(&p);
}

static void a_name_that_does_not_change_is_skipped(void)
{
	static const wchar_t *const names[] = {L"a.txt", L"b.txt"};
	World w;
	WfBatchPlan p;

	world(&w, names, 2);
	ren(&w, L"a.txt", L"a.txt", false);
	ren(&w, L"b.txt", L"c.txt", false);
	CHECK(WF_OK == plan(&w, &p));
	CHECK(0 == p.n_refused && 1 == p.n_skipped && 1 == p.n_steps);
	CHECK(WF_BP_UNCHANGED == p.row[0].why);
	wf_batch_plan_free(&p);
}

static void a_swap_goes_through_a_temporary_name(void)
{
	static const wchar_t *const names[] = {L"a.txt", L"b.txt"};
	World w;
	WfBatchPlan p;

	world(&w, names, 2);
	ren(&w, L"a.txt", L"b.txt", false);
	ren(&w, L"b.txt", L"a.txt", false);
	CHECK(WF_OK == plan(&w, &p));
	CHECK(0 == p.n_refused && 3 == p.n_steps && 1 == p.n_cycles);
	CHECK(1 == p.step[0].temp && !p.step[1].temp && !p.step[2].temp);
	CHECK(0 ==
	        wcsncmp(p.step[0].to_name,
	                L".piero-batch-0102030405060708090a0b0c0d0e0fab-", 46));
	CHECK(p.step[0].item == p.step[2].item);
	/* the row says it too, so that a screen need not look through the steps */
	CHECK(p.row[p.step[0].item].temp && !p.row[1 - p.step[0].item].temp);
	CHECK(play(&w, &p));
	wf_batch_plan_free(&p);
}

static void a_cycle_of_three_and_a_chain_into_it(void)
{
	static const wchar_t *const names[] = {L"a", L"b", L"c", L"d"};
	World w;
	WfBatchPlan p;

	world(&w, names, 4);
	ren(&w, L"a", L"b", false);
	ren(&w, L"b", L"c", false);
	ren(&w, L"c", L"a", false);
	ren(&w, L"d", L"e", false);
	CHECK(WF_OK == plan(&w, &p));
	CHECK(0 == p.n_refused && 5 == p.n_steps && 1 == p.n_cycles);
	{ /* one member of the cycle has a temporary step; the chain item has none */
		size_t temps = 0;

		for (size_t i = 0; i < p.n; i++) {
			temps += p.row[i].temp;
		}
		CHECK(1 == temps && !p.row[3].temp);
	}
	CHECK(play(&w, &p));
	wf_batch_plan_free(&p);
}

static void a_chain_runs_from_the_free_end(void)
{
	static const wchar_t *const names[] = {L"a", L"b", L"c"};
	World w;
	WfBatchPlan p;

	world(&w, names, 3);
	ren(&w, L"a", L"b", false); /* waits for b */
	ren(&w, L"b", L"c", false); /* waits for c */
	ren(&w, L"c", L"z", false);
	CHECK(WF_OK == plan(&w, &p));
	CHECK(0 == p.n_refused && 3 == p.n_steps && 0 == p.n_cycles);
	CHECK(2 == p.step[0].item && 1 == p.step[1].item &&
	        0 == p.step[2].item);
	CHECK(play(&w, &p));
	wf_batch_plan_free(&p);
}

static void a_name_that_exists_is_a_collision_unless_it_is_vacated(void)
{
	static const wchar_t *const names[] = {L"a.txt", L"b.txt", L"keep.txt"};
	World w;
	WfBatchPlan p;

	world(&w, names, 3);
	ren(&w, L"a.txt", L"keep.txt", false);
	CHECK(WF_OK == plan(&w, &p));
	CHECK(1 == p.n_refused && WF_BP_EXISTS == p.row[0].why &&
	        0 == p.n_steps);
	wf_batch_plan_free(&p);
	/* the owner of the name stays (its name is not changed): still a collision */
	world(&w, names, 3);
	ren(&w, L"a.txt", L"b.txt", false);
	ren(&w, L"b.txt", L"b.txt", false);
	CHECK(WF_OK == plan(&w, &p));
	CHECK(WF_BP_EXISTS == p.row[0].why && 1 == p.n_refused);
	wf_batch_plan_free(&p);
}

static void two_items_with_the_same_target_are_refused(void)
{
	static const wchar_t *const names[] = {L"a", L"b"};
	World w;
	WfBatchPlan p;

	world(&w, names, 2);
	ren(&w, L"a", L"same", false);
	ren(&w, L"b", L"same", false);
	CHECK(WF_OK == plan(&w, &p));
	CHECK(2 == p.n_refused && WF_BP_DUP_TARGET == p.row[0].why &&
	        WF_BP_DUP_TARGET == p.row[1].why && 0 == p.n_steps);
	wf_batch_plan_free(&p);
	/* only the case differs: the same name in a folder that ignores case */
	world(&w, names, 2);
	ren(&w, L"a", L"Same", false);
	ren(&w, L"b", L"sAME", false);
	CHECK(WF_OK == plan(&w, &p));
	CHECK(2 == p.n_refused && WF_BP_DUP_TARGET == p.row[0].why);
	wf_batch_plan_free(&p);
	/* and in a folder that does not, they are two names */
	world(&w, names, 2);
	w.parent[0].case_sensitive = true;
	ren(&w, L"a", L"Same", false);
	ren(&w, L"b", L"sAME", false);
	CHECK(WF_OK == plan(&w, &p));
	CHECK(0 == p.n_refused && 2 == p.n_steps);
	wf_batch_plan_free(&p);
}

static void case_follows_the_folder(void)
{
	static const wchar_t *const names[] = {L"a", L"abc"};
	World w;
	WfBatchPlan p;

	world(&w, names, 2);
	ren(&w, L"a", L"ABC",
	        false); /* abc exists, and the folder ignores case */
	CHECK(WF_OK == plan(&w, &p));
	CHECK(1 == p.n_refused && WF_BP_EXISTS == p.row[0].why);
	wf_batch_plan_free(&p);
	world(&w, names, 2);
	w.parent[0].case_sensitive = true;
	ren(&w, L"a", L"ABC", false);
	CHECK(WF_OK == plan(&w, &p));
	CHECK(0 == p.n_refused && 1 == p.n_steps);
	wf_batch_plan_free(&p);
}

static void a_change_of_case_alone_is_one_rename(void)
{
	static const wchar_t *const names[] = {L"readme.md", L"x"};
	World w;
	WfBatchPlan p;

	world(&w, names, 2);
	ren(&w, L"readme.md", L"README.md", false);
	CHECK(WF_OK == plan(&w, &p));
	CHECK(0 == p.n_refused && 1 == p.n_steps && 0 == p.n_cycles &&
	        !p.step[0].temp);
	wf_batch_plan_free(&p);
	/* and not when another item takes the name it leaves in another case */
	world(&w, names, 2);
	ren(&w, L"readme.md", L"README.md", false);
	ren(&w, L"x", L"readme.md", false);
	CHECK(WF_OK == plan(&w, &p));
	CHECK(2 == p.n_refused);
	wf_batch_plan_free(&p);
}

static void names_that_cannot_be_written_are_refused(void)
{
	static const wchar_t *const names[] = {
	        L"a", L"b", L"c", L"d", L"e", L"f"};
	static const wchar_t lone[] = {L'x', 0xd800, L'y', 0};
	static const wchar_t astral[] = {L'x', 0xd83d, 0xde00, 0};
	World w;
	WfBatchPlan p;

	world(&w, names, 6);
	ren(&w, L"a", L"", false);
	ren(&w, L"b", L"NUL.txt", false);
	ren(&w, L"c", L"bad:name", false);
	ren(&w, L"d", L"dot.", false);
	ren(&w, L"e", lone, false);
	ren(&w, L"f", astral, false); /* a pair is fine */
	CHECK(WF_OK == plan(&w, &p));
	CHECK(5 == p.n_refused);
	for (int i = 0; i < 5; i++) {
		CHECK(WF_BP_NAME == p.row[i].why &&
		        WF_NAME_OK != p.row[i].name);
	}
	CHECK(WF_BP_OK == p.row[5].why);
	CHECK(0 == p.n_steps);
	wf_batch_plan_free(&p);
}

static void unicode_names_are_kept(void)
{
	static const wchar_t *const names[] = {
	        L"\x5199\x771f.jpg", L"\x30c6\x30b9\x30c8.txt"};
	World w;
	WfBatchPlan p;

	world(&w, names, 2);
	ren(&w, names[0], L"\x65c5\x884c_001.jpg", false);
	ren(&w, names[1], L"\x30c6\x30b9\x30c8_2.txt", false);
	CHECK(WF_OK == plan(&w, &p));
	CHECK(0 == p.n_refused && 2 == p.n_steps);
	CHECK(!wcscmp(p.step[0].to_name, L"\x65c5\x884c_001.jpg") ||
	        !wcscmp(p.step[1].to_name, L"\x65c5\x884c_001.jpg"));
	wf_batch_plan_free(&p);
}

static void a_path_that_is_too_long_is_refused(void)
{
	static const wchar_t *const names[] = {L"a"};
	static wchar_t big[256];
	World w;
	WfBatchPlan p;
	static wchar_t far_path[WF_BATCH_PATH_MAX];

	for (size_t i = 0; i < 255; i++) {
		big[i] = L'n';
	}
	world(&w, names, 1);
	for (size_t i = 0; i + 1 < WF_BATCH_PATH_MAX - 200; i++) {
		far_path[i] = L'p';
	}
	far_path[0] = L'C';
	far_path[1] = L':';
	far_path[2] = L'\\';
	w.parent[0].path = far_path;
	ren(&w, L"a", big, false);
	CHECK(WF_OK == plan(&w, &p));
	CHECK(1 == p.n_refused && WF_BP_PATH_LONG == p.row[0].why);
	wf_batch_plan_free(&p);
}

static void the_same_file_twice_is_refused(void)
{
	static const wchar_t *const names[] = {L"a", L"b"};
	World w;
	WfBatchPlan p;

	world(&w, names, 2);
	ren(&w, L"a", L"x", false);
	ren(&w, L"b", L"y", false);
	w.item[1].id = w.item[0].id;
	CHECK(WF_OK == plan(&w, &p));
	CHECK(2 == p.n_refused && WF_BP_DUP_ITEM == p.row[0].why);
	wf_batch_plan_free(&p);
}

static void an_item_without_an_identity_is_refused(void)
{
	static const wchar_t *const names[] = {L"a"};
	World w;
	WfBatchPlan p;

	world(&w, names, 1);
	ren(&w, L"a", L"x", false);
	w.item[0].id.kind = WF_ID_NONE;
	CHECK(WF_OK == plan(&w, &p));
	CHECK(1 == p.n_refused && WF_BP_NO_ID == p.row[0].why);
	wf_batch_plan_free(&p);
}

static void another_volume_and_a_link_folder_are_refused(void)
{
	static const wchar_t *const names[] = {L"a", L"b"};
	World w;
	WfBatchPlan p;

	world(&w, names, 2);
	ren(&w, L"a", L"x", false);
	ren(&w, L"b", L"y", false);
	w.item[1].id.volume = 8;
	CHECK(WF_OK == plan(&w, &p));
	CHECK(WF_BP_OK == p.row[0].why && WF_BP_VOLUME == p.row[1].why);
	wf_batch_plan_free(&p);
	world(&w, names, 2);
	w.parent[0].link = true;
	ren(&w, L"a", L"x", false);
	CHECK(WF_OK == plan(&w, &p));
	CHECK(WF_BP_LINK_PARENT == p.row[0].why);
	wf_batch_plan_free(&p);
	world(&w, names, 2);
	ren(&w, L"a", L"x", false);
	w.item[0].link = true;
	CHECK(WF_OK == plan(&w, &p));
	CHECK(WF_BP_LINK_ITEM == p.row[0].why && 1 == p.n_refused);
	wf_batch_plan_free(&p);
}

static void a_folder_and_what_is_inside_it_are_not_a_batch(void)
{
	static const wchar_t *const top[] = {L"dir", L"other"};
	static const wchar_t *const inside[] = {L"f.txt"};
	World w;
	WfBatchPlan p;

	world(&w, top, 2);
	second_folder(&w, FOLDER L"\\dir", inside, 1);
	ren(&w, L"dir", L"dir2", true);
	w.item[1].parent = 1;
	w.item[1].to_parent = 1;
	w.item[1].name = L"f.txt";
	w.item[1].to_name = L"g.txt";
	w.item[1].id = fid(50);
	w.n = 2;
	CHECK(WF_OK == plan(&w, &p));
	CHECK(WF_BP_NESTED == p.row[1].why && p.n_refused >= 1 &&
	        0 == p.n_steps);
	wf_batch_plan_free(&p);
}

static void a_folder_is_not_moved_into_itself(void)
{
	static const wchar_t *const top[] = {L"dir", L"other"};
	static const wchar_t *const inside[] = {L"sub"};
	World w;
	WfBatchPlan p;

	world(&w, top, 2);
	second_folder(&w, FOLDER L"\\dir\\sub", inside, 1);
	ren(&w, L"dir", L"dir", true);
	w.item[0].to_parent = 1; /* into dir\sub, which is inside dir */
	CHECK(WF_OK == plan(&w, &p));
	CHECK(WF_BP_NESTED == p.row[0].why);
	wf_batch_plan_free(&p);
}

static void a_move_keeps_the_name_and_checks_the_target_folder(void)
{
	static const wchar_t *const top[] = {L"a.txt", L"b.txt"};
	static const wchar_t *const there[] = {L"b.txt"};
	World w;
	WfBatchPlan p;

	world(&w, top, 2);
	second_folder(&w, L"C:\\Elsewhere", there, 1);
	ren(&w, L"a.txt", L"a.txt", false);
	w.item[0].to_parent = 1;
	ren(&w, L"b.txt", L"b.txt", false);
	w.item[1].to_parent = 1;
	CHECK(WF_OK == plan(&w, &p));
	CHECK(WF_BP_OK == p.row[0].why && WF_BP_EXISTS == p.row[1].why);
	wf_batch_plan_free(&p);
}

static void limits_and_misuse_are_errors_not_plans(void)
{
	static const wchar_t *const names[] = {L"a"};
	World w;
	WfBatchPlan p;

	world(&w, names, 1);
	ren(&w, L"a", L"x", false);
	CHECK(WF_E_INVAL == wf_batch_plan(NULL, 1, w.item, 1, BATCH, &p));
	CHECK(WF_E_INVAL == wf_batch_plan(w.parent, 1, w.item, 0, BATCH, &p));
	CHECK(WF_E_RANGE ==
	        wf_batch_plan(w.parent, 1, w.item, WF_BATCH_MAX_ITEMS + 1,
	                BATCH, &p));
	w.item[0].to_parent = 5;
	CHECK(WF_E_INVAL == wf_batch_plan(w.parent, 1, w.item, 1, BATCH, &p));
}

/* 10,000 renames, two cycles among them, in the time of a unit test. */
static void ten_thousand_items_are_planned_quickly(void)
{
	enum
	{
		N = 10000
	};
	static wchar_t names_buf[N][16], to_buf[N][16];
	static const wchar_t *name_ptr[N];
	static WfBatchItem item[N];
	WfBatchParent parent;
	WfBatchPlan p;
	ULONGLONG t0;

	for (int i = 0; i < N; i++) {
		swprintf(names_buf[i], 16, L"f%05d.txt", i);
		swprintf(to_buf[i], 16, L"g%05d.txt", i);
		name_ptr[i] = names_buf[i];
	}
	/* items 0 and 1 swap */
	wcscpy_s(to_buf[0], 16, names_buf[1]);
	wcscpy_s(to_buf[1], 16, names_buf[0]);
	memset(&parent, 0, sizeof parent);
	parent.path = FOLDER;
	parent.id = fid(1000);
	parent.component_max = 255;
	parent.names = name_ptr;
	parent.n_names = N;
	for (int i = 0; i < N; i++) {
		item[i].name = names_buf[i];
		item[i].to_name = to_buf[i];
		item[i].id = fid((unsigned)i + 1);
	}
	t0 = GetTickCount64();
	CHECK(WF_OK == wf_batch_plan(&parent, 1, item, N, BATCH, &p));
	CHECK(0 == p.n_refused && N + 1 == p.n_steps && 1 == p.n_cycles);
	CHECK(GetTickCount64() - t0 < 3000);
	wf_batch_plan_free(&p);
}

int main(void)
{
	a_plain_rename_is_one_step_each();
	a_name_that_does_not_change_is_skipped();
	a_swap_goes_through_a_temporary_name();
	a_cycle_of_three_and_a_chain_into_it();
	a_chain_runs_from_the_free_end();
	a_name_that_exists_is_a_collision_unless_it_is_vacated();
	two_items_with_the_same_target_are_refused();
	case_follows_the_folder();
	a_change_of_case_alone_is_one_rename();
	names_that_cannot_be_written_are_refused();
	unicode_names_are_kept();
	a_path_that_is_too_long_is_refused();
	the_same_file_twice_is_refused();
	an_item_without_an_identity_is_refused();
	another_volume_and_a_link_folder_are_refused();
	a_folder_and_what_is_inside_it_are_not_a_batch();
	a_folder_is_not_moved_into_itself();
	a_move_keeps_the_name_and_checks_the_target_folder();
	limits_and_misuse_are_errors_not_plans();
	ten_thousand_items_are_planned_quickly();
	return TEST_RESULT();
}
