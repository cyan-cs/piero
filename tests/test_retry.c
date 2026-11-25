#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <wchar.h>

#include "ui/wf_app.h"
#include "ui/wf_app_priv.h"
#include "wf_fake_fs.h"
#include "wf_test.h"
#include "base/wf_macro.h"

static wchar_t *dup(const wchar_t *s)
{
	size_t n = wcslen(s) + 1;
	wchar_t *p = malloc(n * sizeof *p);

	memcpy(p, s, n * sizeof *p);
	return p;
}

/* A copy of three sources in which one failed: "r" offers just that one
 * again, and starts nothing. */
static void failed_items_are_offered_again(void)
{
	WfApp app;
	FakePort fp = {.app = &app};
	WfCopySpec spec;
	WfCopyResult r;
	WfCopyFailFull fail;
	WfEvent rkey = {WF_EV_CHAR, 'r', WF_KEY_NONE, 0};

	CHECK(WF_OK ==
	        wf_app_init(&app, 120, 30, fake_port(&fp), L"C:\\Users\\dev"));
	memset(&spec, 0, sizeof spec);
	wf_slot_init(&spec.slot);
	spec.kind = WF_CJ_COPY;
	spec.dest = dup(L"D:\\out");
	spec.slot.mode = WF_XFER_COPY;
	spec.slot.dir = dup(L"C:\\src");
	spec.slot.count = 3;
	spec.slot.src = calloc(3, sizeof *spec.slot.src);
	spec.slot.src[0].name = dup(L"a.txt");
	spec.slot.src[1].name = dup(L"folder");
	spec.slot.src[2].name = dup(L"c.txt");
	wf_app_retry_note(&app, 7, &spec);
	wf_copyspec_free(&spec);

	memset(&r, 0, sizeof r);
	memset(&fail, 0, sizeof fail);
	fail.path =
	        dup(L"C:\\src\\folder\\deep\\x.bin"); /* inside the folder */
	fail.kind = WF_CF_WRITE_FAILED;
	fail.error = 5;
	r.job = 7;
	r.kind = WF_CJ_COPY;
	r.failure_total = 1;
	r.fail_all = &fail;
	r.fail_all_n = 1;
	wf_app_joblog_add(&app, "1 failed", &r, 0, 0);
	free(fail.path);

	CHECK(1 == app.jobs.n && 1 == app.jobs.log[0].retry.count &&
	        !wcscmp(app.jobs.log[0].retry.src[0].name, L"folder"));
	CHECK(0 == app.retry_src_n); /* ownership moved to the entry */

	app.mode = &WF_MODE_JOBLOG;
	CHECK(WF_ACT_REDRAW == app.mode->on_event(&app, &rkey));
	CHECK(&WF_MODE_NORMAL == app.mode && 1 == app.slot.count &&
	        WF_XFER_COPY == app.slot.mode &&
	        !wcscmp(app.slot.src[0].name, L"folder") && 0 == app.copy_jobs);
	wf_app_destroy(&app);
}

/* A job that failed nothing leaves nothing to offer. */
static void clean_job_leaves_nothing(void)
{
	WfApp app;
	FakePort fp = {.app = &app};
	WfCopySpec spec;
	WfCopyResult r;

	CHECK(WF_OK ==
	        wf_app_init(&app, 120, 30, fake_port(&fp), L"C:\\Users\\dev"));
	memset(&spec, 0, sizeof spec);
	wf_slot_init(&spec.slot);
	spec.kind = WF_CJ_COPY;
	spec.dest = dup(L"D:\\out");
	spec.slot.dir = dup(L"C:\\src");
	spec.slot.count = 1;
	spec.slot.src = calloc(1, sizeof *spec.slot.src);
	spec.slot.src[0].name = dup(L"a.txt");
	wf_app_retry_note(&app, 3, &spec);
	wf_copyspec_free(&spec);
	memset(&r, 0, sizeof r);
	r.job = 3;
	wf_app_joblog_add(&app, "done", &r, 0, 0);
	CHECK(0 == app.jobs.log[0].retry.count && 0 == app.retry_src_n);
	wf_app_destroy(&app);
}

/* The notes are kept oldest first: when there are too many it is the oldest that
 * is given up, and a note that is taken out leaves the others in their order. */
static void notes_stay_oldest_first(void)
{
	WfApp app;
	FakePort fp = {.app = &app};
	WfCopyResult r;

	CHECK(WF_OK ==
	        wf_app_init(&app, 120, 30, fake_port(&fp), L"C:\\Users\\dev"));
	for (uint32_t job = 1; job <= 10;
	        job++) { /* two more than there is room for */
		WfCopySpec spec;

		memset(&spec, 0, sizeof spec);
		wf_slot_init(&spec.slot);
		spec.kind = WF_CJ_COPY;
		spec.dest = dup(L"D:\\out");
		spec.slot.dir = dup(L"C:\\src");
		spec.slot.count = 1;
		spec.slot.src = calloc(1, sizeof *spec.slot.src);
		spec.slot.src[0].name = dup(L"a.txt");
		wf_app_retry_note(&app, job, &spec);
		wf_copyspec_free(&spec);
	}
	CHECK(8 == app.retry_src_n);
	for (size_t i = 0; i < 8; i++) {
		CHECK(i + 3 ==
		        app.retry_src[i].job); /* 1 and 2 were given up */
	}
	memset(&r, 0, sizeof r);
	r.job = 5; /* a clean job: its note goes, from the middle */
	wf_app_joblog_add(&app, "done", &r, 0, 0);
	CHECK(7 == app.retry_src_n);
	for (size_t i = 0, want = 3; i < 7; i++, want++) {
		want += 5 == want;
		CHECK(want == app.retry_src[i].job);
	}
	wf_app_destroy(&app);
}

/* Many sources and many failed paths: each failed path is placed in its source
 * by the name, not by asking every source. Sources whose names differ only in
 * case are the same here, a folder's own failures count for the folder, and a
 * path of another folder or of a name that is not a source counts for nothing. */
static void failures_are_placed_in_their_sources_by_name(void)
{
	enum
	{
		SOURCES = 20000,
		FAILED = 4000
	};
	WfApp app;
	FakePort fp = {.app = &app};
	WfCopySpec spec;
	WfCopyResult r;
	WfCopyFailFull *fail = calloc(FAILED, sizeof *fail);
	clock_t t0;
	double ms;

	CHECK(fail != NULL);
	CHECK(WF_OK ==
	        wf_app_init(&app, 120, 30, fake_port(&fp), L"C:\\Users\\dev"));
	memset(&spec, 0, sizeof spec);
	wf_slot_init(&spec.slot);
	spec.kind = WF_CJ_COPY;
	spec.dest = dup(L"D:\\out");
	spec.slot.dir = dup(L"C:\\src");
	spec.slot.count = SOURCES;
	spec.slot.src = calloc(SOURCES, sizeof *spec.slot.src);
	for (size_t i = 0; i < SOURCES; i++) {
		wchar_t name[24];

		swprintf(name, 24, L"s%05zu", i);
		spec.slot.src[i].name = dup(name);
	}
	wf_app_retry_note(&app, 9, &spec);
	wf_copyspec_free(&spec);
	memset(&r, 0, sizeof r);
	for (size_t f = 0; f < FAILED; f++) {
		wchar_t path[64];

		/* every fifth source, the path in lower or upper case, in the folder it is */
		if (0 == f % 2) {
			swprintf(path, 64, L"C:\\src\\s%05zu\\deep\\x.bin",
			        f * 5);
		} else {
			swprintf(path, 64, L"c:\\SRC\\S%05zu", f * 5);
		}
		fail[f].path = dup(path);
		fail[f].kind = WF_CF_WRITE_FAILED;
	}
	free(fail[3].path); /* one of another folder, one of no source */
	fail[3].path = dup(L"C:\\elsewhere\\s00015");
	free(fail[5].path);
	fail[5].path = dup(L"C:\\src\\not-a-source\\x");
	r.job = 9;
	r.kind = WF_CJ_COPY;
	r.failure_total = FAILED;
	r.fail_all = fail;
	r.fail_all_n = FAILED;
	t0 = clock();
	wf_app_joblog_add(&app, "failed", &r, 0, 0);
	ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
	printf("info: %d failed paths among %d sources placed: %.1f ms\n",
	        (int)FAILED, (int)SOURCES, ms);
	CHECK(1 == app.jobs.n && FAILED - 2 == app.jobs.log[0].retry.count);
	{
		/* the offered ones are those of the failures, in the order of the slot */
		size_t at = 0;

		for (size_t f = 0;
		        f < FAILED && at < app.jobs.log[0].retry.count; f++) {
			wchar_t want[24];

			if (3 == f || 5 == f) {
				continue;
			}
			swprintf(want, 24, L"s%05zu", f * 5);
			CHECK(!wcscmp(
			        app.jobs.log[0].retry.src[at].name, want));
			at++;
		}
	}
	CHECK(ms < 500.0);
	for (size_t f = 0; f < FAILED; f++) {
		free(fail[f].path);
	}
	free(fail);
	wf_app_destroy(&app);
}

/* A failure that lies in the source that sorts first (names that differ only in
 * case are one source here, and both are offered again): the walk back from
 * the hit ends at the first element without stepping before it. */
static void a_failure_in_the_first_source_places_it_and_its_case_twin(void)
{
	WfApp app;
	FakePort fp = {.app = &app};
	WfCopySpec spec;
	WfCopyResult r;
	WfCopyFailFull fail;

	CHECK(WF_OK ==
	        wf_app_init(&app, 120, 30, fake_port(&fp), L"C:\\Users\\dev"));
	memset(&spec, 0, sizeof spec);
	wf_slot_init(&spec.slot);
	spec.kind = WF_CJ_COPY;
	spec.dest = dup(L"D:\\out");
	spec.slot.dir = dup(L"C:\\src");
	spec.slot.count = 3;
	spec.slot.src = calloc(3, sizeof *spec.slot.src);
	spec.slot.src[0].name = dup(L"A.txt");
	spec.slot.src[1].name = dup(L"a.txt");
	spec.slot.src[2].name = dup(L"b.txt");
	wf_app_retry_note(&app, 11, &spec);
	wf_copyspec_free(&spec);
	memset(&r, 0, sizeof r);
	memset(&fail, 0, sizeof fail);
	fail.path = dup(L"C:\\SRC\\a.TXT");
	fail.kind = WF_CF_WRITE_FAILED;
	r.job = 11;
	r.kind = WF_CJ_COPY;
	r.failure_total = 1;
	r.fail_all = &fail;
	r.fail_all_n = 1;
	wf_app_joblog_add(&app, "1 failed", &r, 0, 0);
	free(fail.path);
	CHECK(1 == app.jobs.n && 2 == app.jobs.log[0].retry.count);
	CHECK(!wcscmp(app.jobs.log[0].retry.src[0].name, L"A.txt") &&
	        !wcscmp(app.jobs.log[0].retry.src[1].name, L"a.txt"));
	wf_app_destroy(&app);
}

int main(void)
{
	failed_items_are_offered_again();
	clean_job_leaves_nothing();
	notes_stay_oldest_first();
	failures_are_placed_in_their_sources_by_name();
	a_failure_in_the_first_source_places_it_and_its_case_twin();
	return TEST_RESULT();
}
