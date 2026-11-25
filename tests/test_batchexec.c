/*
 * The batch rename and move on real folders (DR-DRAFT-022): every rename keeps the
 * file (its bytes and its FileId), a refused plan changes nothing, a failure in the
 * middle is taken back, a file replaced meanwhile is never overwritten or
 * "restored over", and a process that ends at each point of the run is recovered
 * from what the disk and the journal say. Everything happens in a folder of the
 * temporary directory made for the run; nothing else is touched.
 */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xfer/wf_batchexec_win.h"
#include "xfer/wf_bulkname.h"
#include "wf_test.h"

#define PATHLEN 1200
#define MAXF 1000

static wchar_t root[MAX_PATH + 40];
static wchar_t cur[PATHLEN];  /* the folder of this case */
static wchar_t jdir[PATHLEN]; /* its journal folder */
static int case_no;

static const wchar_t *P(const wchar_t *name)
{
	static wchar_t ring[8][PATHLEN];
	static int at;
	wchar_t *out = ring[at++ & 7];

	swprintf(out, PATHLEN, L"%ls\\%ls", cur, name);
	return out;
}

static void fresh(void)
{
	case_no++;
	swprintf(cur, PATHLEN, L"%ls\\case%d", root, case_no);
	swprintf(jdir, PATHLEN, L"%ls\\journal%d", root, case_no);
	CreateDirectoryW(cur, NULL);
	CreateDirectoryW(jdir, NULL);
}

static void put_file(const wchar_t *name, const char *text)
{
	HANDLE h = CreateFileW(P(name), GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD n;

	CHECK(INVALID_HANDLE_VALUE != h);
	if (INVALID_HANDLE_VALUE != h) {
		WriteFile(h, text, (DWORD)strlen(text), &n, NULL);
		CloseHandle(h);
	}
}

static bool has(const wchar_t *name)
{
	return INVALID_FILE_ATTRIBUTES != GetFileAttributesW(P(name));
}

static bool content_is(const wchar_t *name, const char *text)
{
	char buf[256] = {0};
	DWORD n = 0;
	HANDLE h = CreateFileW(P(name), GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	bool ok;

	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	ReadFile(h, buf, sizeof buf - 1, &n, NULL);
	CloseHandle(h);
	ok = n == strlen(text) && !memcmp(buf, text, n);
	return ok;
}

static WfFileId id_at(const wchar_t *path)
{
	WfFileId id;
	FILE_ID_INFO info;
	HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);

	memset(&id, 0, sizeof id);
	if (INVALID_HANDLE_VALUE != h) {
		if (GetFileInformationByHandleEx(
		            h, FileIdInfo, &info, sizeof info)) {
			id.kind = WF_ID_STRONG;
			id.volume = info.VolumeSerialNumber;
			memcpy(id.id, info.FileId.Identifier, 16);
		}
		CloseHandle(h);
	}
	return id;
}

static bool same_id(const WfFileId *a, const WfFileId *b)
{
	return (a->kind == WF_ID_STRONG) && (b->kind == WF_ID_STRONG) &&
	        a->volume == b->volume && !memcmp(a->id, b->id, 16);
}

static void wipe_tree(const wchar_t *dir)
{
	wchar_t pattern[PATHLEN];
	WIN32_FIND_DATAW d;
	HANDLE h;

	swprintf(pattern, PATHLEN, L"%ls\\*", dir);
	h = FindFirstFileExW(
	        pattern, FindExInfoBasic, &d, FindExSearchNameMatch, NULL, 0);
	if (INVALID_HANDLE_VALUE != h) {
		do {
			wchar_t f[PATHLEN];

			if (!wcscmp(d.cFileName, L".") ||
			        !wcscmp(d.cFileName, L"..")) {
				continue;
			}
			swprintf(f, PATHLEN, L"%ls\\%ls", dir, d.cFileName);
			if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
				wipe_tree(f);
			} else {
				SetFileAttributesW(f, FILE_ATTRIBUTE_NORMAL);
				DeleteFileW(f);
			}
		} while (FindNextFileW(h, &d));
		FindClose(h);
	}
	RemoveDirectoryW(dir);
}

typedef struct
{
	size_t n;
	wchar_t old_name[MAXF][24];
	wchar_t new_name[MAXF][24];
	wchar_t old_path[MAXF][PATHLEN];
	WfFileId id[MAXF];
	char text[MAXF][24];
} Set;

static Set set;

/* Creates f0..f(n-1) with their own text and picks the new names n1..nN. */
static void make_set(size_t n)
{
	set.n = n;
	for (size_t i = 0; i < n; i++) {
		swprintf(set.old_name[i], 24, L"f%zu.txt", i);
		swprintf(set.new_name[i], 24, L"n%zu.txt", i + 1);
		snprintf(set.text[i], sizeof set.text[i], "c%zu", i);
		put_file(set.old_name[i], set.text[i]);
		wcscpy_s(set.old_path[i], PATHLEN, P(set.old_name[i]));
		set.id[i] = id_at(set.old_path[i]);
	}
}

static WfBatchSource src[MAXF];

static WfBatchJob *prepare(WfBatchReport *rep)
{
	for (size_t i = 0; i < set.n; i++) {
		src[i].path = set.old_path[i];
		src[i].expected = set.id[i];
		src[i].to_name = set.new_name[i];
	}
	return wf_batch_prepare(src, set.n, NULL, rep);
}

/* every file is under `names` (old or new), with its text and its identity */
static bool all_there(bool renamed)
{
	for (size_t i = 0; i < set.n; i++) {
		const wchar_t *here =
		        renamed ? set.new_name[i] : set.old_name[i];
		const wchar_t *gone =
		        renamed ? set.old_name[i] : set.new_name[i];
		WfFileId id = id_at(P(here));

		if (!has(here) || has(gone) || !content_is(here, set.text[i]) ||
		        !same_id(&id, &set.id[i])) {
			return false;
		}
	}
	return true;
}

typedef struct
{
	const char *point;
	size_t step;
	void (*act)(void);
	int hits;
} Trap;

static Trap trap;
static volatile LONG cancel_flag;

static void at(void *ctx, const char *point, size_t step)
{
	(void)ctx;
	if (trap.point && !strcmp(point, trap.point) && step == trap.step &&
	        trap.act) {
		trap.hits++;
		trap.act();
	}
}

static const WfBatchHooks HOOKS = {at, NULL};

static void arm(const char *point, size_t step, void (*act)(void))
{
	trap.point = point;
	trap.step = step;
	trap.act = act;
	trap.hits = 0;
}

static void execute(WfBatchJob *job, WfBatchReport *rep)
{
	cancel_flag = 0;
	wf_batch_execute(job, jdir, &HOOKS, &cancel_flag, rep);
}

static void run_plain(size_t n)
{
	WfBatchReport rep;
	WfBatchJob *job;
	LARGE_INTEGER a, b, f;

	fresh();
	make_set(n);
	arm(NULL, 0, NULL);
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&a);
	job = prepare(&rep);
	CHECK(job && wf_batch_job_ready(job) && WF_BATCH_READY == rep.pre);
	execute(job, &rep);
	QueryPerformanceCounter(&b);
	CHECK(WF_BATCH_SUCCESS == rep.result && n == rep.steps_done);
	CHECK(all_there(true));
	CHECK(0 == wf_batch_count_unfinished(jdir));
	printf("batch %zu: prepare %llu ms, journal %llu ms, renames %llu ms, "
	       "total %llu ms; "
	       "opens %zu, id queries %zu, renames %zu, enumerations %zu, "
	       "journal writes %zu flushes %zu\n",
	        n, (unsigned long long)rep.ms_prepare,
	        (unsigned long long)rep.ms_journal,
	        (unsigned long long)rep.ms_rename,
	        (unsigned long long)((b.QuadPart - a.QuadPart) * 1000 /
	                f.QuadPart),
	        rep.calls.opens, rep.calls.id_queries, rep.calls.renames,
	        rep.calls.enumerations, rep.log.writes, rep.log.flushes);
	wf_batch_job_free(job);
}

static void renames_keep_the_bytes_and_the_identity(void)
{
	run_plain(10);
	run_plain(100);
	run_plain(1000);
}

static void the_pattern_names_go_through_the_whole_way(void)
{
	WfBatchReport rep;
	WfBatchJob *job;
	WfBulkPattern pat = {L"photo_{n:3}", NULL, NULL, 1, 1,
	        WF_BULK_CASE_KEEP, WF_BULK_CASE_KEEP};

	fresh();
	make_set(12);
	for (size_t i = 0; i < set.n; i++) {
		wchar_t *name = NULL;

		CHECK(WF_OK ==
		        wf_bulk_apply(&pat, set.old_name[i], false, i, &name));
		wcscpy_s(set.new_name[i], 24, name);
		free(name);
	}
	job = prepare(&rep);
	execute(job, &rep);
	CHECK(WF_BATCH_SUCCESS == rep.result);
	CHECK(all_there(true) && has(L"photo_001.txt") &&
	        has(L"photo_012.txt"));
	wf_batch_job_free(job);
}

static void a_swap_a_cycle_and_a_change_of_case(void)
{
	WfBatchReport rep;
	WfBatchJob *job;
	WfBatchSource s[6];
	static const wchar_t *const names[] = {
	        L"a.txt", L"b.txt", L"x", L"y", L"z", L"readme.md"};
	static const wchar_t *const to[] = {
	        L"b.txt", L"a.txt", L"y", L"z", L"x", L"README.md"};
	wchar_t paths[6][PATHLEN];
	WfFileId ids[6];

	fresh();
	for (int i = 0; i < 6; i++) {
		char text[8];

		snprintf(text, sizeof text, "t%d", i);
		put_file(names[i], text);
		wcscpy_s(paths[i], PATHLEN, P(names[i]));
		ids[i] = id_at(paths[i]);
		s[i].path = paths[i];
		s[i].expected = ids[i];
		s[i].to_name = to[i];
	}
	job = wf_batch_prepare(s, 6, NULL, &rep);
	CHECK(job && wf_batch_job_ready(job) &&
	        2 == wf_batch_job_plan(job)->n_cycles);
	execute(job, &rep);
	CHECK(WF_BATCH_SUCCESS == rep.result);
	for (int i = 0; i < 6; i++) {
		char text[8];
		WfFileId id = id_at(P(to[i]));

		snprintf(text, sizeof text, "t%d", i);
		CHECK(content_is(to[i], text) && same_id(&id, &ids[i]));
	}
	/* the names as the folder lists them: the case of the new one */
	{
		WIN32_FIND_DATAW d;
		HANDLE h = FindFirstFileW(P(L"readme.md"), &d);

		CHECK(INVALID_HANDLE_VALUE != h &&
		        !wcscmp(d.cFileName, L"README.md"));
		if (INVALID_HANDLE_VALUE != h) {
			FindClose(h);
		}
	}
	CHECK(0 == wf_batch_count_unfinished(jdir));
	wf_batch_job_free(job);
}

static void a_move_into_a_folder_keeps_the_names(void)
{
	WfBatchReport rep;
	WfBatchJob *job;
	wchar_t dest[PATHLEN];

	fresh();
	make_set(4);
	swprintf(dest, PATHLEN, L"%ls\\dest", cur);
	CreateDirectoryW(dest, NULL);
	for (size_t i = 0; i < set.n; i++) {
		src[i].path = set.old_path[i];
		src[i].expected = set.id[i];
		src[i].to_name = NULL;
	}
	job = wf_batch_prepare(src, set.n, dest, &rep);
	CHECK(job && wf_batch_job_ready(job));
	execute(job, &rep);
	CHECK(WF_BATCH_SUCCESS == rep.result);
	for (size_t i = 0; i < set.n; i++) {
		wchar_t in[64];
		WfFileId id;

		swprintf(in, 64, L"dest\\%ls", set.old_name[i]);
		id = id_at(P(in));
		CHECK(has(in) && !has(set.old_name[i]) &&
		        content_is(in, set.text[i]) &&
		        same_id(&id, &set.id[i]));
	}
	wf_batch_job_free(job);
}

static void japanese_names_and_a_long_path(void)
{
	WfBatchReport rep;
	WfBatchJob *job;
	wchar_t deep[PATHLEN], ext[PATHLEN + 8];
	WfBatchSource s[2];
	wchar_t p0[PATHLEN], p1[PATHLEN];
	WfFileId id0, id1;

	fresh();
	/* a folder 300 units deep: a path over MAX_PATH, reached by the extended form */
	swprintf(deep, PATHLEN, L"%ls", cur);
	for (int i = 0; i < 6; i++) {
		wcscat_s(deep, PATHLEN, L"\\");
		for (int k = 0; k < 48; k++) {
			wcscat_s(deep, PATHLEN, L"d");
		}
		swprintf(ext, PATHLEN + 8, L"\\\\?\\%ls", deep);
		CHECK(CreateDirectoryW(ext, NULL) ||
		        GetLastError() == ERROR_ALREADY_EXISTS);
	}
	{
		wchar_t ja[] = L"\x5199\x771f\x30c7\x30fc\x30bf.txt";
		wchar_t nm[] = L"\x65c5\x884c_\xd83d\xde00.txt";
		HANDLE h;

		swprintf(p0, PATHLEN, L"\\\\?\\%ls\\%ls", deep, ja);
		swprintf(p1, PATHLEN, L"\\\\?\\%ls\\keep.txt", deep);
		h = CreateFileW(
		        p0, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
		CHECK(INVALID_HANDLE_VALUE != h);
		CloseHandle(h);
		h = CreateFileW(
		        p1, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
		CHECK(INVALID_HANDLE_VALUE != h);
		CloseHandle(h);
		id0 = id_at(p0);
		id1 = id_at(p1);
		s[0].path = p0;
		s[0].expected = id0;
		s[0].to_name = nm;
		s[1].path = p1;
		s[1].expected = id1;
		s[1].to_name = L"kept.txt";
		job = wf_batch_prepare(s, 2, NULL, &rep);
		CHECK(job && wf_batch_job_ready(job));
		execute(job, &rep);
		CHECK(WF_BATCH_SUCCESS == rep.result);
		swprintf(p0, PATHLEN, L"\\\\?\\%ls\\%ls", deep, nm);
		swprintf(p1, PATHLEN, L"\\\\?\\%ls\\kept.txt", deep);
		{
			WfFileId a = id_at(p0), b = id_at(p1);

			CHECK(same_id(&a, &id0) && same_id(&b, &id1));
		}
		wf_batch_job_free(job);
	}
}

static void a_refused_plan_changes_nothing(void)
{
	WfBatchReport rep;
	WfBatchJob *job;

	fresh();
	make_set(5);
	put_file(L"n3.txt", "somebody else's file"); /* a target that exists */
	job = prepare(&rep);
	CHECK(job && !wf_batch_job_ready(job) && WF_BATCH_PRE_PLAN == rep.pre);
	CHECK(2 == rep.pre_item);
	CHECK(WF_BP_EXISTS == wf_batch_job_plan(job)->row[2].why);
	execute(job, &rep);
	CHECK(WF_BATCH_FAILED_BEFORE_EXECUTION == rep.result);
	CHECK(content_is(L"n3.txt", "somebody else's file"));
	for (size_t i = 0; i < set.n; i++) {
		WfFileId id = id_at(P(set.old_name[i]));

		CHECK(has(set.old_name[i]) &&
		        content_is(set.old_name[i], set.text[i]) &&
		        same_id(&id, &set.id[i]));
	}
	CHECK(0 == wf_batch_count_unfinished(jdir));
	wf_batch_job_free(job);
}

/* A listing of the folder that stops before its end (access taken away) is not
 * the folder: the plan is refused, not made from the names that were read. */
static void a_listing_that_stops_early_refuses_the_plan(void)
{
	WfBatchReport rep;
	WfBatchJob *job;

	fresh();
	make_set(5);
	wf_batch_fail_listing_after(3);
	job = prepare(&rep);
	wf_batch_fail_listing_after(-1);
	CHECK(job && !wf_batch_job_ready(job) &&
	        WF_BATCH_PRE_ACCESS == rep.pre &&
	        ERROR_ACCESS_DENIED == rep.error);
	execute(job, &rep);
	CHECK(WF_BATCH_FAILED_BEFORE_EXECUTION == rep.result);
	CHECK(all_there(false));
	wf_batch_job_free(job);
	job = prepare(&rep); /* the same folder listed whole */
	CHECK(job && wf_batch_job_ready(job));
	wf_batch_job_free(job);
}

/* What the preview costs the screen: the folder is listed and each target is
 * looked at before the plan is shown, on the thread that reads the keys. One
 * rename in a folder of 20000 files. */
static void the_cost_of_preparing_in_a_big_folder(void)
{
	enum
	{
		FILES = 20000
	};
	WfBatchReport rep;
	WfBatchJob *job;
	LARGE_INTEGER a, b, freq;

	fresh();
	make_set(2);
	for (int i = 0; i < FILES; i++) {
		wchar_t name[32];

		swprintf(name, 32, L"bulk%05d.txt", i);
		put_file(name, "");
	}
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&a);
	job = prepare(&rep);
	QueryPerformanceCounter(&b);
	printf("info: prepare in a folder of %d files: %.1f ms\n", FILES,
	        (double)(b.QuadPart - a.QuadPart) * 1000.0 /
	                (double)freq.QuadPart);
	CHECK(job && wf_batch_job_ready(job));
	wf_batch_job_free(job);
}

static void a_missing_file_stops_the_whole_batch(void)
{
	WfBatchReport rep;
	WfBatchJob *job;

	fresh();
	make_set(6);
	DeleteFileW(set.old_path[3]);
	job = prepare(&rep);
	CHECK(!wf_batch_job_ready(job) && WF_BATCH_PRE_MISSING == rep.pre &&
	        3 == rep.pre_item);
	execute(job, &rep);
	CHECK(WF_BATCH_FAILED_BEFORE_EXECUTION == rep.result);
	for (size_t i = 0; i < set.n; i++) {
		CHECK(i == 3 ? !has(set.old_name[i]) : has(set.old_name[i]));
		CHECK(!has(set.new_name[i]));
	}
	wf_batch_job_free(job);
}

static void a_file_that_is_not_the_one_listed_is_refused(void)
{
	WfBatchReport rep;
	WfBatchJob *job;

	fresh();
	make_set(4);
	/* the listing showed f2.txt; another file now has that name */
	DeleteFileW(set.old_path[2]);
	put_file(L"f2.txt", "c2");
	job = prepare(&rep);
	CHECK(!wf_batch_job_ready(job) && WF_BATCH_PRE_CHANGED == rep.pre &&
	        2 == rep.pre_item);
	execute(job, &rep);
	CHECK(WF_BATCH_FAILED_BEFORE_EXECUTION == rep.result);
	for (size_t i = 0; i < set.n; i++) {
		CHECK(has(set.old_name[i]) && !has(set.new_name[i]));
	}
	wf_batch_job_free(job);
}

static void a_link_a_read_only_nothing_and_a_missing_destination(void)
{
	WfBatchReport rep;
	WfBatchJob *job;

	fresh();
	make_set(2);
	src[0].path = set.old_path[0];
	src[0].expected = set.id[0];
	src[0].to_name = NULL;
	job = wf_batch_prepare(src, 1, P(L"no such folder"), &rep);
	CHECK(!wf_batch_job_ready(job) && WF_BATCH_PRE_MISSING == rep.pre);
	wf_batch_job_free(job);
	job = wf_batch_prepare(NULL, 0, NULL, &rep);
	CHECK(!job && WF_BATCH_PRE_LIMITS == rep.pre);
	src[0].expected.kind = WF_ID_NONE;
	job = wf_batch_prepare(src, 1, NULL, &rep);
	CHECK(!wf_batch_job_ready(job));
	wf_batch_job_free(job);
	CHECK(has(set.old_name[0]) && has(set.old_name[1]));
}

static void make_the_intruder(void)
{
	put_file(L"n4.txt",
	        "an intruder"); /* appears after the plan, before step 3 */
}

static void a_name_that_appears_after_the_plan_rolls_back(void)
{
	WfBatchReport rep;
	WfBatchJob *job;

	fresh();
	make_set(6);
	job = prepare(&rep);
	CHECK(wf_batch_job_ready(job));
	arm("after_plan", 0, make_the_intruder);
	execute(job, &rep);
	CHECK(1 == trap.hits);
	CHECK(WF_BATCH_ROLLED_BACK == rep.result);
	CHECK(rep.steps_done >= 3 && rep.steps_done == rep.rolled_back &&
	        0 == rep.not_restored);
	for (size_t i = 0; i < set.n; i++) {
		WfFileId id = id_at(P(set.old_name[i]));

		CHECK(has(set.old_name[i]) &&
		        content_is(set.old_name[i], set.text[i]) &&
		        same_id(&id, &set.id[i]));
	}
	CHECK(content_is(L"n4.txt", "an intruder")); /* never touched */
	CHECK(!has(L"n1.txt") && !has(L"n2.txt") && !has(L"n3.txt") &&
	        !has(L"n5.txt") && !has(L"n6.txt"));
	CHECK(0 == wf_batch_count_unfinished(jdir)); /* ended, ROLLED_BACK */
	wf_batch_job_free(job);
}

static void swap_in_an_item_that_was_done(void)
{
	/* after step 1 failed: n1.txt (done by step 0) is replaced by another file */
	DeleteFileW(P(L"n1.txt"));
	put_file(L"n1.txt", "a stranger took the name");
}

static void a_file_replaced_during_the_run_is_not_restored_over(void)
{
	WfBatchReport rep;
	WfBatchJob *job;

	fresh();
	make_set(4);
	job = prepare(&rep);
	execute(NULL, &rep); /* misuse is a refusal, not a crash */
	CHECK(WF_BATCH_FAILED_BEFORE_EXECUTION == rep.result);
	put_file(L"n3.txt", "an intruder"); /* step 2 will fail on this */
	arm("after_failure", 2, swap_in_an_item_that_was_done);
	execute(job, &rep);
	CHECK(1 == trap.hits);
	CHECK(WF_BATCH_RECOVERY_REQUIRED == rep.result &&
	        rep.not_restored >= 1);
	/* the stranger's file is where it is, with its own text; nothing was overwritten */
	CHECK(content_is(L"n1.txt", "a stranger took the name"));
	CHECK(content_is(L"n3.txt", "an intruder"));
	CHECK(1 == wf_batch_count_unfinished(jdir)); /* the journal stays */
	/* the file that was not replaced was taken back; the replaced one was not
	 * "restored" over the stranger's file */
	CHECK(has(L"f1.txt") && content_is(L"f1.txt", "c1") && !has(L"f0.txt"));
	wf_batch_job_free(job);
}

static void stop_after_two_steps(void)
{
	InterlockedExchange(&cancel_flag, 1);
}

static void cancel_in_the_middle_is_taken_back(void)
{
	WfBatchReport rep;
	WfBatchJob *job;

	fresh();
	make_set(8);
	job = prepare(&rep);
	arm("after_D", 1, stop_after_two_steps);
	cancel_flag = 0;
	wf_batch_execute(job, jdir, &HOOKS, &cancel_flag, &rep);
	CHECK(1 == trap.hits);
	CHECK(WF_BATCH_ROLLED_BACK == rep.result && 2 == rep.steps_done &&
	        2 == rep.rolled_back);
	CHECK(all_there(false));
	CHECK(0 == wf_batch_count_unfinished(jdir));
	wf_batch_job_free(job);
}

static void a_file_that_is_in_use_stops_and_rolls_back(void)
{
	WfBatchReport rep;
	WfBatchJob *job;
	HANDLE lock;

	fresh();
	make_set(5);
	job = prepare(&rep);
	/* another program has f3.txt open and shares nothing */
	lock = CreateFileW(
	        set.old_path[3], GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
	CHECK(INVALID_HANDLE_VALUE != lock);
	arm(NULL, 0, NULL);
	execute(job, &rep);
	CHECK(WF_BATCH_ROLLED_BACK == rep.result && 3 == rep.failed_step &&
	        (ERROR_SHARING_VIOLATION == rep.error ||
	                ERROR_ACCESS_DENIED == rep.error));
	CloseHandle(lock);
	CHECK(all_there(false));
	wf_batch_job_free(job);
}

#define DEAD 0xdead

typedef struct
{
	const char *name, *point;
	size_t step;
	bool cancel_first; /* the child stops the run after step 2 to make it roll back */
	bool torn; /* the last line of the journal is cut after the kill */
	WfBatchResult want;
	bool renamed; /* where the files are afterwards */
} Crash;

static const Crash CRASHES[] = {
        {"after_plan", "after_plan", 0, false, false, WF_BATCH_ROLLED_BACK,
                false},
        {"after_B_1", "after_B", 1, false, false, WF_BATCH_ROLLED_BACK, false},
        {"after_B_last", "after_B", 4, false, false, WF_BATCH_ROLLED_BACK,
                false},
        {"after_rename_1_before_D", "after_rename", 1, false, false,
                WF_BATCH_ROLLED_BACK, false},
        {"after_D_1", "after_D", 1, false, false, WF_BATCH_ROLLED_BACK, false},
        {"after_D_1_torn", "after_D", 1, false, true, WF_BATCH_ROLLED_BACK,
                false},
        {"after_D_last", "after_D", 4, false, false, WF_BATCH_SUCCESS, true},
        {"before_E", "before_E", 0, false, false, WF_BATCH_SUCCESS, true},
        {"in_rollback_1", "in_rollback", 1, true, false, WF_BATCH_ROLLED_BACK,
                false},
};

static const Crash *child_crash;

static void die_here(void)
{
	TerminateProcess(GetCurrentProcess(), DEAD);
}

static void child_hook(void *ctx, const char *point, size_t step)
{
	(void)ctx;
	if (child_crash->cancel_first && !strcmp(point, "after_D") &&
	        2 == step) {
		InterlockedExchange(&cancel_flag, 1);
	}
	if (!strcmp(point, child_crash->point) && step == child_crash->step) {
		die_here();
	}
}

/* The part that runs in the child: the same batch of five, then it ends where told. */
static int child_main(const char *name)
{
	wchar_t folder[PATHLEN], journal[PATHLEN];
	WfBatchSource s[5];
	wchar_t paths[5][PATHLEN];
	wchar_t news[5][16];
	WfBatchReport rep;
	WfBatchJob *job;
	WfBatchHooks hooks = {child_hook, NULL};

	for (size_t i = 0; i < sizeof CRASHES / sizeof *CRASHES; i++) {
		if (!strcmp(CRASHES[i].name, name)) {
			child_crash = &CRASHES[i];
		}
	}
	if (!child_crash ||
	        !GetEnvironmentVariableW(L"WF_BT_DIR", folder, PATHLEN) ||
	        !GetEnvironmentVariableW(L"WF_BT_JDIR", journal, PATHLEN)) {
		return 2;
	}
	for (int i = 0; i < 5; i++) {
		swprintf(paths[i], PATHLEN, L"%ls\\f%d.txt", folder, i);
		swprintf(news[i], 16, L"n%d.txt", i + 1);
		s[i].path = paths[i];
		s[i].expected = id_at(paths[i]);
		s[i].to_name = news[i];
	}
	job = wf_batch_prepare(s, 5, NULL, &rep);
	if (!job || !wf_batch_job_ready(job)) {
		return 3;
	}
	wf_batch_execute(job, journal, &hooks, &cancel_flag, &rep);
	return 4; /* it was supposed to end before this */
}

static bool run_child(const char *name)
{
	wchar_t exe[MAX_PATH], cmd[MAX_PATH + 100];
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	DWORD code = 0;
	wchar_t wname[64];

	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	GetModuleFileNameW(NULL, exe, MAX_PATH);
	MultiByteToWideChar(CP_ACP, 0, name, -1, wname, 64);
	swprintf(cmd, MAX_PATH + 100, L"\"%ls\" --child %ls", exe, wname);
	SetEnvironmentVariableW(L"WF_BT_DIR", cur);
	SetEnvironmentVariableW(L"WF_BT_JDIR", jdir);
	if (!CreateProcessW(
	            NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
		return false;
	}
	WaitForSingleObject(pi.hProcess, 60000);
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return DEAD == code;
}

static bool the_one_journal(wchar_t out[PATHLEN])
{
	wchar_t pattern[PATHLEN];
	WIN32_FIND_DATAW d;
	HANDLE h;

	swprintf(pattern, PATHLEN, L"%ls\\*.pbj", jdir);
	h = FindFirstFileW(pattern, &d);
	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	swprintf(out, PATHLEN, L"%ls\\%ls", jdir, d.cFileName);
	FindClose(h);
	return true;
}

static void cut_last_bytes(const wchar_t *f, long long n)
{
	HANDLE h = CreateFileW(f, GENERIC_READ | GENERIC_WRITE, 0, NULL,
	        OPEN_EXISTING, 0, NULL);
	LARGE_INTEGER size, at;

	GetFileSizeEx(h, &size);
	at.QuadPart = size.QuadPart - n;
	SetFilePointerEx(h, at, NULL, FILE_BEGIN);
	SetEndOfFile(h);
	CloseHandle(h);
}

static void a_process_that_ends_at_each_point_is_recovered(void)
{
	for (size_t c = 0; c < sizeof CRASHES / sizeof *CRASHES; c++) {
		const Crash *k = &CRASHES[c];
		wchar_t journal[PATHLEN];
		WfBatchReport rep;
		WfBatchResult now;
		int before = wf_test_failures;

		fresh();
		make_set(5);
		CHECK(run_child(k->name));
		CHECK(the_one_journal(journal));
		/* before recovery piero would only say that something is unfinished */
		CHECK(1 == wf_batch_count_unfinished(jdir));
		if (k->torn) {
			cut_last_bytes(journal, 4);
		}
		wf_batch_recover(journal, NULL, &rep);
		now = rep.result;
		CHECK(now == k->want);
		CHECK(all_there(k->renamed));
		/* recovery is finished and says so; running it again changes nothing */
		CHECK(0 == wf_batch_count_unfinished(jdir));
		wf_batch_recover(journal, NULL, &rep);
		CHECK(rep.result == k->want);
		CHECK(all_there(k->renamed));
		if (wf_test_failures != before) {
			printf("  (crash point %s)\n", k->name);
		}
	}
}

static void a_journal_that_never_got_its_plan_changes_nothing(void)
{
	wchar_t journal[PATHLEN];
	WfBatchReport rep;
	HANDLE h;
	DWORD n;

	fresh();
	make_set(3);
	swprintf(journal, PATHLEN, L"%ls\\00000000000000000000000000000001.pbj",
	        jdir);
	h = CreateFileW(journal, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
	WriteFile(h, "PBJ1 3 3*", 9, &n, NULL);
	CloseHandle(h);
	wf_batch_recover(journal, NULL, &rep);
	CHECK(WF_BATCH_FAILED_BEFORE_EXECUTION == rep.result);
	CHECK(all_there(false));
}

static void a_damaged_journal_is_kept_and_nothing_is_done(void)
{
	wchar_t journal[PATHLEN];
	WfBatchReport rep;
	HANDLE h;
	DWORD n;
	char c;

	fresh();
	make_set(5);
	CHECK(run_child("after_D_1"));
	CHECK(the_one_journal(journal));
	h = CreateFileW(journal, GENERIC_READ | GENERIC_WRITE, 0, NULL,
	        OPEN_EXISTING, 0, NULL);
	SetFilePointer(h, 70, NULL, FILE_BEGIN);
	ReadFile(h, &c, 1, &n, NULL);
	c ^= 1;
	SetFilePointer(h, 70, NULL, FILE_BEGIN);
	WriteFile(h, &c, 1, &n, NULL);
	CloseHandle(h);
	wf_batch_recover(journal, NULL, &rep);
	CHECK(WF_BATCH_RECOVERY_REQUIRED == rep.result);
	/* two files had been renamed; they stay renamed: nothing was guessed */
	CHECK(has(L"n1.txt") && has(L"n2.txt") && !has(L"f0.txt") &&
	        !has(L"f1.txt"));
	CHECK(1 == wf_batch_count_unfinished(jdir));
}

int main(int argc, char **argv)
{
	wchar_t tmp[MAX_PATH];

	if (3 == argc && !strcmp(argv[1], "--child")) {
		return child_main(argv[2]);
	}
	GetTempPathW(MAX_PATH, tmp);
	swprintf(root, MAX_PATH + 40, L"%lspiero-batchexec-%lu", tmp,
	        GetCurrentProcessId());
	CreateDirectoryW(root, NULL);
	the_pattern_names_go_through_the_whole_way();
	a_swap_a_cycle_and_a_change_of_case();
	a_move_into_a_folder_keeps_the_names();
	japanese_names_and_a_long_path();
	a_refused_plan_changes_nothing();
	a_listing_that_stops_early_refuses_the_plan();
	a_missing_file_stops_the_whole_batch();
	a_file_that_is_not_the_one_listed_is_refused();
	a_link_a_read_only_nothing_and_a_missing_destination();
	a_name_that_appears_after_the_plan_rolls_back();
	a_file_replaced_during_the_run_is_not_restored_over();
	cancel_in_the_middle_is_taken_back();
	a_file_that_is_in_use_stops_and_rolls_back();
	a_process_that_ends_at_each_point_is_recovered();
	a_journal_that_never_got_its_plan_changes_nothing();
	a_damaged_journal_is_kept_and_nothing_is_done();
	/* Reach the refusal and recovery checks before the large fixtures. */
	renames_keep_the_bytes_and_the_identity();
	the_cost_of_preparing_in_a_big_folder();
	wipe_tree(root);
	return TEST_RESULT();
}
