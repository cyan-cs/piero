/*
 * The duplicate files engine (DR-DRAFT-032): grouping by size first (no file read
 * when every size is different), then by the first bytes, then by the whole file,
 * and only reading what the step before could not tell apart; empty files left out;
 * files that cannot be read left out and counted; a stop from outside and from the
 * group callback; progress. The bytes are stand-ins: a file is a size and two numbers
 * (what its first bytes are and what the rest is).
 */
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "search/wf_dupes.h"
#include "wf_test.h"

typedef struct
{
	const wchar_t *name;
	uint64_t size;
	unsigned head, tail; /* the first bytes' kind, and the rest's */
	WfStatus fail;       /* hash() answers this (WF_OK: it works) */
} F;

typedef struct
{
	const F *f;
	size_t n;
	WfDupFile *rec;
	size_t reads, prefix_reads, full_reads;
	size_t groups;
	const WfDupFile *group[64][8]; /* the members, as indexes into rec */
	size_t group_n[64];
	size_t cancel_after_reads, stop_after_groups;
	uint64_t last_done;
	bool progress_backwards;
	size_t progress_calls;
	uint64_t last_total;
} Run;

static const F *find_f(const Run *r, const wchar_t *name)
{
	for (size_t i = 0; i < r->n; i++) {
		if (!wcscmp(r->f[i].name, name)) {
			return &r->f[i];
		}
	}
	return NULL;
}

static WfStatus fake_hash(
        void *ctx, const WfDupFile *file, uint64_t limit, uint8_t digest[32])
{
	Run *r = ctx;
	const F *f = find_f(r, file->name);
	bool prefix_only = limit && limit < f->size;

	r->reads++;
	if (prefix_only) {
		r->prefix_reads++;
	} else {
		r->full_reads++;
	}
	if (WF_OK != f->fail) {
		return f->fail;
	}
	memset(digest, 0, 32);
	memcpy(digest, &f->head, sizeof f->head);
	if (!prefix_only) {
		memcpy(digest + 8, &f->tail, sizeof f->tail);
	}
	memcpy(digest + 16, &(uint64_t){prefix_only ? 0 : 1}, 8);
	return WF_OK;
}

static bool on_group(void *ctx, const WfDupFile *const *file, size_t n)
{
	Run *r = ctx;

	if (r->groups < 64) {
		r->group_n[r->groups] = n < 8 ? n : 8;
		for (size_t i = 0; i < n && i < 8; i++) {
			r->group[r->groups][i] = file[i];
		}
	}
	r->groups++;
	return !(r->stop_after_groups && r->groups >= r->stop_after_groups);
}

static void on_progress(
        void *ctx, WfDupePhase phase, uint64_t done, uint64_t total)
{
	Run *r = ctx;

	(void)phase;
	if (done < r->last_done) {
		r->progress_backwards = true;
	}
	r->last_done = done;
	r->last_total = total;
	r->progress_calls++;
}

static bool is_cancelled(void *ctx)
{
	Run *r = ctx;

	return r->cancel_after_reads && r->reads >= r->cancel_after_reads;
}

static WfDupeOps ops(void)
{
	WfDupeOps o = {fake_hash, on_group, on_progress, is_cancelled};

	return o;
}

static WfStatus run(Run *r, const F *f, size_t n, WfDupeStats *st)
{
	WfDupeOps o = ops();
	WfStatus s;

	r->f = f;
	r->n = n;
	r->rec = calloc(n ? n : 1, sizeof *r->rec);
	for (size_t i = 0; i < n; i++) {
		r->rec[i].name = f[i].name;
		r->rec[i].size = f[i].size;
		r->rec[i].id.kind = WF_ID_STRONG;
		r->rec[i].id.id[0] = (uint8_t)(i + 1);
	}
	s = wf_dupes_run(r->rec, n, &o, r, st);
	return s;
}

static void finish(Run *r)
{
	free(r->rec);
	r->rec = NULL;
}

static bool member(const Run *r, size_t g, size_t k, const wchar_t *name)
{
	return g < r->groups && k < r->group_n[g] &&
	        !wcscmp(r->group[g][k]->name, name);
}

#define KB 1024u

static void nothing_to_compare_reads_nothing(void)
{
	Run r = {0};
	WfDupeStats st;
	static const F files[] = {{L"a", 10, 1, 1, WF_OK},
	        {L"b", 11, 1, 1, WF_OK}, {L"c", 12, 1, 1, WF_OK},
	        {L"d", 0, 1, 1, WF_OK}, {L"e", 0, 1, 1, WF_OK}};

	CHECK(WF_OK == run(&r, files, 5, &st));
	CHECK(0 == r.reads && 0 == r.groups && 0 == st.candidates &&
	        0 == st.groups);
	finish(&r);
	memset(&r, 0, sizeof r);
	CHECK(WF_OK == run(&r, files, 0, &st));
	CHECK(0 == r.reads && 0 == r.groups);
	finish(&r);
	/* empty files are all alike and say nothing: never a group */
	memset(&r, 0, sizeof r);
	CHECK(WF_OK == run(&r, files + 3, 2, &st));
	CHECK(0 == r.reads && 0 == r.groups);
	finish(&r);
}

static void small_files_are_read_whole_once_and_grouped(void)
{
	Run r = {0};
	WfDupeStats st;
	static const F files[] = {
	        {L"x1", 100, 7, 7, WF_OK}, /* a pair */
	        {L"y1", 200, 1, 1, WF_OK},
	        {L"x2", 100, 7, 7, WF_OK},
	        {L"y2", 200, 1, 2, WF_OK}, /* same size, other bytes */
	        {L"z", 300, 9, 9, WF_OK},  /* alone */
	        {L"x3", 100, 7, 8, WF_OK}, /* same size, other bytes than x1 */
	};

	CHECK(WF_OK == run(&r, files, 6, &st));
	CHECK(1 == r.groups && 2 == r.group_n[0]);
	CHECK(member(&r, 0, 0, L"x1") && member(&r, 0, 1, L"x2"));
	/* size 100 (3 files) and 200 (2) are read whole, once each: 5 reads; z not at all */
	CHECK(5 == r.reads && 5 == r.full_reads && 0 == r.prefix_reads);
	CHECK(5 == st.candidates && 5 == st.hashed && 1 == st.groups &&
	        2 == st.files_in_groups);
	CHECK(0 == st.skipped && !st.cancelled && !st.stopped);
	finish(&r);
}

static void large_files_are_read_whole_only_when_their_first_bytes_are_alike(
        void)
{
	Run r = {0};
	WfDupeStats st;
	static const F files[] = {
	        /* size 1 MiB: all alike at the start, two of them to the end */
	        {L"a", 1024 * KB, 5, 5, WF_OK},
	        {L"b", 1024 * KB, 5, 5, WF_OK},
	        {L"c", 1024 * KB, 5, 6, WF_OK},
	        /* size 2 MiB: every start is different: never read whole */
	        {L"d", 2048 * KB, 1, 1, WF_OK},
	        {L"e", 2048 * KB, 2, 1, WF_OK},
	        {L"f", 2048 * KB, 3, 1, WF_OK},
	        /* the size of the prefix: it is the file already */
	        {L"g", WF_DUPE_PREFIX, 4, 4, WF_OK},
	        {L"h", WF_DUPE_PREFIX, 4, 4, WF_OK},
	        /* one byte more than the prefix: the first bytes first */
	        {L"i", WF_DUPE_PREFIX + 1, 4, 4, WF_OK},
	        {L"j", WF_DUPE_PREFIX + 1, 4, 4, WF_OK},
	};

	CHECK(WF_OK == run(&r, files, 10, &st));
	CHECK(3 == r.groups);
	/* ascending size: g h (64 KiB), i j (64 KiB + 1), a b (1 MiB) */
	CHECK(member(&r, 0, 0, L"g") && member(&r, 0, 1, L"h"));
	CHECK(member(&r, 1, 0, L"i") && member(&r, 1, 1, L"j"));
	CHECK(member(&r, 2, 0, L"a") && member(&r, 2, 1, L"b"));
	/* reads: g h whole (2); i j first bytes then whole (2 + 2); a b c first bytes
	 * (3), and since all three start alike all three whole (3), which is how c
	 * is told from a and b; d e f first bytes only (3): nothing else of them */
	CHECK(15 == r.reads);
	CHECK(2 + 2 + 3 == r.full_reads);
	CHECK(2 + 3 + 3 == r.prefix_reads);
	CHECK(10 == st.candidates && 15 == st.hashed && 3 == st.groups);
	finish(&r);
}

static void three_files_two_alike_make_one_group_of_two(void)
{
	Run r = {0};
	WfDupeStats st;
	static const F files[] = {{L"a", 500, 1, 1, WF_OK},
	        {L"b", 500, 1, 2, WF_OK}, {L"c", 500, 1, 1, WF_OK}};

	CHECK(WF_OK == run(&r, files, 3, &st));
	CHECK(1 == r.groups && 2 == r.group_n[0]);
	CHECK(member(&r, 0, 0, L"a") && member(&r, 0, 1, L"c"));
	finish(&r);
}

static void two_groups_of_one_size_and_a_big_one(void)
{
	Run r = {0};
	WfDupeStats st;
	static F many[1000];
	static wchar_t names[1000][8];

	for (int i = 0; i < 1000; i++) {
		swprintf(names[i], 8, L"m%d", i);
		many[i].name = names[i];
		many[i].size = 800;
		many[i].head = 3;
		many[i].tail = 3;
	}
	CHECK(WF_OK == run(&r, many, 1000, &st));
	CHECK(1 == st.groups && 1000 == st.files_in_groups && 1000 == r.reads);
	finish(&r);

	{
		static const F files[] = {{L"a1", 50, 1, 1, WF_OK},
		        {L"b1", 50, 2, 2, WF_OK}, {L"a2", 50, 1, 1, WF_OK},
		        {L"b2", 50, 2, 2, WF_OK}};

		memset(&r, 0, sizeof r);
		CHECK(WF_OK == run(&r, files, 4, &st));
		CHECK(2 == r.groups);
		CHECK(member(&r, 0, 0, L"a1") && member(&r, 0, 1, L"a2"));
		CHECK(member(&r, 1, 0, L"b1") && member(&r, 1, 1, L"b2"));
		finish(&r);
	}
}

static void a_file_that_cannot_be_read_is_left_out_and_counted(void)
{
	Run r = {0};
	WfDupeStats st;
	static const F files[] = {{L"a", 500, 1, 1, WF_OK},
	        {L"b", 500, 1, 1, WF_E_ACCESS}, {L"c", 500, 1, 1, WF_OK},
	        {L"d", 700, 1, 1, WF_OK}, {L"e", 700, 1, 1, WF_E_CHANGED}};

	CHECK(WF_OK == run(&r, files, 5, &st));
	CHECK(1 == r.groups && 2 == r.group_n[0]);
	CHECK(member(&r, 0, 0, L"a") && member(&r, 0, 1, L"c"));
	CHECK(2 == st.skipped);
	/* d is alone once e is out: no group, and nothing is made of the pair that was */
	finish(&r);
}

/* Whatever is left out in the middle, the walk over the runs of equal digests
 * moves on: every run that is there is found, in order, once. */
static void runs_after_a_left_out_file_are_all_found_once(void)
{
	Run r = {0};
	WfDupeStats st;
	/* sizes 100: a b c d (b unreadable) alike; 200: e f; 300: g h i, i unreadable;
	 * 400: j alone; 500: k l (both unreadable) */
	static const F files[] = {{L"a", 100, 1, 1, WF_OK},
	        {L"b", 100, 1, 1, WF_E_ACCESS}, {L"c", 100, 1, 1, WF_OK},
	        {L"d", 100, 1, 1, WF_OK}, {L"e", 200, 2, 2, WF_OK},
	        {L"f", 200, 2, 2, WF_OK}, {L"g", 300, 3, 3, WF_OK},
	        {L"h", 300, 3, 3, WF_OK}, {L"i", 300, 3, 3, WF_E_CHANGED},
	        {L"j", 400, 4, 4, WF_OK}, {L"k", 500, 5, 5, WF_E_ACCESS},
	        {L"l", 500, 5, 5, WF_E_ACCESS}};

	CHECK(WF_OK == run(&r, files, 12, &st));
	CHECK(3 == r.groups); /* abc d, e f, g h */
	CHECK(st.groups == 3 && st.files_in_groups == 7 && 4 == st.skipped);
	CHECK(3 == r.group_n[0] || 3 == r.group_n[1] || 3 == r.group_n[2]);
	finish(&r);
}

static void it_stops_when_asked_from_outside_and_by_the_callback(void)
{
	Run r = {0};
	WfDupeStats st;
	static const F files[] = {{L"a", 100, 1, 1, WF_OK},
	        {L"b", 100, 1, 1, WF_OK}, {L"c", 200, 2, 2, WF_OK},
	        {L"d", 200, 2, 2, WF_OK}, {L"e", 300, 3, 3, WF_OK},
	        {L"f", 300, 3, 3, WF_OK}};

	r.cancel_after_reads = 2;
	CHECK(WF_OK == run(&r, files, 6, &st));
	CHECK(st.cancelled && !st.stopped);
	CHECK(1 == r.groups); /* the first size was done, then no more reads */
	CHECK(r.reads <= 3);
	finish(&r);

	memset(&r, 0, sizeof r);
	r.stop_after_groups = 2;
	CHECK(WF_OK == run(&r, files, 6, &st));
	CHECK(st.stopped && !st.cancelled && 2 == r.groups);
	CHECK(4 == r.reads); /* the third size was never read */
	finish(&r);
}

static void progress_counts_the_files_to_read_and_never_goes_back(void)
{
	Run r = {0};
	WfDupeStats st;
	static const F files[] = {{L"a", 1024 * KB, 1, 1, WF_OK},
	        {L"b", 1024 * KB, 1, 1, WF_OK}, {L"c", 100, 5, 5, WF_OK},
	        {L"d", 100, 5, 5, WF_OK}};

	CHECK(WF_OK == run(&r, files, 4, &st));
	CHECK(!r.progress_backwards);
	CHECK(r.progress_calls >= 1 + r.reads);
	CHECK(r.last_done == r.reads && r.last_done <= r.last_total);
	finish(&r);
}

static void misuse_is_refused(void)
{
	Run r = {0};
	WfDupeOps good = ops();
	WfDupeOps no_hash = {NULL, on_group, NULL, NULL};
	WfDupeOps no_group = {fake_hash, NULL, NULL, NULL};
	WfDupFile one = {0};

	CHECK(WF_E_INVAL == wf_dupes_run(&one, 1, NULL, &r, NULL));
	CHECK(WF_E_INVAL == wf_dupes_run(&one, 1, &no_hash, &r, NULL));
	CHECK(WF_E_INVAL == wf_dupes_run(&one, 1, &no_group, &r, NULL));
	CHECK(WF_E_INVAL == wf_dupes_run(NULL, 2, &good, &r, NULL));
	CHECK(WF_OK == wf_dupes_run(NULL, 0, &good, &r, NULL));
	CHECK(WF_OK == wf_dupes_run(&one, 1, &good, &r, NULL));
}

int main(void)
{
	nothing_to_compare_reads_nothing();
	small_files_are_read_whole_once_and_grouped();
	large_files_are_read_whole_only_when_their_first_bytes_are_alike();
	three_files_two_alike_make_one_group_of_two();
	two_groups_of_one_size_and_a_big_one();
	a_file_that_cannot_be_read_is_left_out_and_counted();
	runs_after_a_left_out_file_are_all_found_once();
	it_stops_when_asked_from_outside_and_by_the_callback();
	progress_counts_the_files_to_read_and_never_goes_back();
	misuse_is_refused();
	return TEST_RESULT();
}
