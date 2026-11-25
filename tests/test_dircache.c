/* UI-thread ownership and scheduling with a deterministic listing service.
 * These snapshots are real allocations; no filesystem freshness is implied. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "ui/wf_app_priv.h"
#include "path/wf_path.h"
#include "wf_test.h"

static bool fail_cache, fail_path;
static void *cache_alloc(size_t n, size_t bytes)
{
	return fail_cache ? NULL : calloc(n, bytes);
}
static WfStatus cache_join(const wchar_t *a, const wchar_t *b, wchar_t **out)
{
	if (fail_path) {
		*out = NULL;
		return WF_E_NOMEM;
	}
	return wf_path_join(a, b, out);
}
#define calloc cache_alloc
#define wf_path_join cache_join
#include "../src/ui/wf_dircache.c"
#undef wf_path_join
#undef calloc

typedef struct
{
	unsigned calls;
	uint32_t gen;
	wchar_t path[256];
} Spy;
static WfApp app;
static Spy spy;

static WfFileId id(unsigned n)
{
	WfFileId r = {0};
	r.kind = WF_ID_STRONG;
	r.volume = 1;
	memcpy(r.id, &n, sizeof n);
	return r;
}
static WfStatus request(
        void *ctx, uint32_t gen, const wchar_t *path, const wchar_t *parent)
{
	Spy *s = ctx;
	(void)parent;
	s->calls++;
	s->gen = gen;
	wcscpy_s(s->path, 256, path);
	return WF_OK;
}
static void close_app(void)
{
	wf_app_preview_clear(&app);
	wf_app_dircache_destroy(&app);
	wf_view_free(&app.dirprev);
	wf_view_free(&app.cur);
	free(app.cwd);
	memset(&app, 0, sizeof app);
	memset(&spy, 0, sizeof spy);
}
static void open_app(bool cache)
{
	close_app();
	fail_cache = fail_path = false;
	app.cwd = _wcsdup(L"C:\\cache");
	app.now_ms = 100;
	app.dirprev_port = (WfDirPreviewPort){&spy, request};
	WfDir *d = wf_dir_new();
	CHECK(d != NULL);
	if (!d) {
		return;
	}
	d->identity = id(1000);
	for (unsigned n = 0; n < 30; n++) {
		wchar_t name[32];
		swprintf(name, 32, L"f%02u", n);
		CHECK(wf_dir_add(d, name, wcslen(name), WF_ENT_DIR, 0, 0) ==
		        WF_OK);
		d->e[d->n - 1].identity = id(n + 1);
	}
	CHECK(wf_view_set_dir(&app.cur, d, NULL) == WF_OK);
	app.cur.cursor = 10;
	if (cache) {
		wf_app_dircache_enable(&app, true);
	}
	wf_app_preview_sync(&app, false);
}
static void select_folder(size_t n)
{
	app.cur.cursor = n;
	wf_app_preview_sync(&app, false);
}
static void advance(void)
{
	app.now_ms += WF_DIRPREV_DEBOUNCE_MS;
	(void)wf_app_dirprev_tick(&app);
}
static WfDir *answer(size_t files)
{
	unsigned n = 0;
	const wchar_t *leaf = wcsrchr(spy.path, L'\\');
	CHECK(leaf && swscanf_s(leaf + 1, L"f%u", &n) == 1);
	WfDir *d = wf_dir_new();
	CHECK(d != NULL);
	if (!d) {
		return NULL;
	}
	d->identity = id(n + 1);
	for (size_t i = 0; i < files; i++) {
		wchar_t name[32];
		swprintf(name, 32, L"name%zu", i);
		CHECK(wf_dir_add(d, name, wcslen(name), 0, 1, 0) == WF_OK);
	}
	return d;
}
static void complete(size_t files)
{
	uint32_t gen = spy.gen;
	WfDir *d = answer(files);
	wf_app_on_dirpreview(&app, gen, WF_OK, d);
}
static void fill(void)
{
	advance();
	unsigned count = 0;
	while (app.dircache && app.dircache->flight && count++ < 12) {
		complete(2);
	}
	CHECK(count <= 9);
}
static bool contains(unsigned n)
{
	WfFileId want = id(n + 1);
	for (size_t i = 0; i < WF_DIRCACHE_SLOTS; i++) {
		if (wf_fileid_equal(&app.dircache->entry[i].id, &want)) {
			return true;
		}
	}
	return false;
}
static void sliding_and_reversal(void)
{
	open_app(true);
	CHECK(spy.calls == 0);
	fill();
	CHECK(spy.calls == 9 && app.dircache->count == 9);
	CHECK(wf_app_dirprev_timeout(&app) == WF_WAIT_FOREVER);
	CHECK(contains(7) && contains(15) && !contains(6) && !contains(16));
	CHECK(app.dirprev_state == WF_DIRPREV_READY);
	unsigned before = spy.calls;
	select_folder(11);
	CHECK(app.dirprev_state == WF_DIRPREV_READY && app.dircache->hits == 1);
	CHECK(spy.calls == before + 1 && !contains(7) && contains(16));
	complete(2);
	before = spy.calls;
	select_folder(10);
	CHECK(app.dirprev_state == WF_DIRPREV_READY &&
	        app.dircache->direction == -1);
	CHECK(contains(5) && contains(13) && !contains(14));
	CHECK(spy.calls <= before + 1);
	CHECK(wf_app_dircache_bytes(&app) <= WF_DIRCACHE_BYTES);
	close_app();
}
static void newest_request_and_invalidation(void)
{
	open_app(true);
	advance();
	uint32_t old = spy.gen;
	WfDir *stale = answer(1);
	select_folder(25);
	advance();
	wf_app_on_dirpreview(&app, old, WF_OK, stale);
	CHECK(app.dirprev_state == WF_DIRPREV_LOADING);
	complete(2);
	CHECK(app.dirprev_state == WF_DIRPREV_READY);
	wf_app_dircache_invalidate(&app);
	CHECK(app.dircache->count == 0 && !app.dircache->flight);
	CHECK(app.dirprev_state == WF_DIRPREV_WAIT);
	old = spy.gen;
	stale = answer(1);
	advance();
	wf_app_on_dirpreview(&app, old, WF_OK, stale);
	CHECK(app.dirprev_state == WF_DIRPREV_LOADING);
	complete(2);
	CHECK(app.dirprev_state == WF_DIRPREV_READY);
	close_app();
}
static void failures_never_become_empty_success(void)
{
	open_app(true);
	advance();
	wf_app_on_dirpreview(&app, spy.gen, WF_E_ACCESS, NULL);
	CHECK(app.dirprev_state == WF_DIRPREV_FAILED &&
	        app.dirprev_status == WF_E_ACCESS);
	close_app();
	open_app(true);
	advance();
	WfDir *d = answer(1);
	d->identity = id(999);
	wf_app_on_dirpreview(&app, spy.gen, WF_OK, d);
	CHECK(app.dirprev_state == WF_DIRPREV_FAILED);
	close_app();
	open_app(false);
	fail_cache = true;
	wf_app_dircache_enable(&app, true);
	CHECK(!app.dircache);
	advance();
	complete(1);
	CHECK(app.dirprev_state == WF_DIRPREV_READY);
	close_app();
	open_app(true);
	fail_path = true;
	select_folder(12);
	advance();
	complete(1);
	CHECK(app.dirprev_state == WF_DIRPREV_READY);
	close_app();
	fail_path = false;
}
static void ttl_limits_and_incomplete_lists(void)
{
	open_app(true);
	fill();
	app.now_ms += WF_DIRCACHE_TTL + 1;
	select_folder(11);
	CHECK(app.dirprev_state == WF_DIRPREV_WAIT);
	advance();
	complete(20001);
	CHECK(app.dirprev_state == WF_DIRPREV_READY);
	CHECK(wf_app_dircache_bytes(&app) <= WF_DIRCACHE_BYTES);
	while (app.dircache->flight) {
		uint32_t gen = spy.gen;
		WfDir *d = answer(1);
		d->truncated = true;
		wf_app_on_dirpreview(&app, gen, WF_OK, d);
	}
	CHECK(app.dircache->rejected > 0);
	close_app();
	open_app(true);
	advance();
	complete(20001);
	unsigned attempts = 0;
	while (app.dircache->flight && attempts++ < 10) {
		complete(20001);
		CHECK(wf_app_dircache_bytes(&app) <= WF_DIRCACHE_BYTES);
	}
	CHECK(attempts <= 8 && app.dirprev_state == WF_DIRPREV_READY);
	CHECK(app.dircache->evicted + app.dircache->rejected > 0);
	close_app();
	open_app(true);
	advance();
	complete(70000);
	CHECK(app.dirprev_state == WF_DIRPREV_READY &&
	        app.dirprev.dir->n == 70000);
	CHECK(wf_app_dircache_bytes(&app) > WF_DIRCACHE_BYTES &&
	        !app.dircache->flight);
	close_app();
	WfDir *d = wf_dir_new();
	CHECK(d != NULL);
	if (!d) {
		return;
	}
	d->byte_limit = 4096;
	CHECK(wf_dir_add(d, L"bounded", 7, 0, 0, 0) == WF_E_RANGE);
	CHECK(d->owned_bytes <= 4096);
	wf_dir_free(d);
}
static void rapid_cursor_and_settings(void)
{
	open_app(true);
	fill();
	for (unsigned i = 0; i < 100; i++) {
		select_folder(i % 30);
	}
	CHECK(app.dircache->count <= 9 &&
	        wf_app_dircache_bytes(&app) <= WF_DIRCACHE_BYTES);
	wf_app_dircache_invalidate(&app);
	select_folder(10);
	advance();
	complete(2);
	wf_app_set_hidden(&app, true);
	CHECK(app.dircache->count == 0);
	close_app();
	open_app(true);
	app.cur.dir->e[11].flags |= WF_ENT_LINK;
	app.cur.dir->e[12].flags = 0;
	select_folder(9);
	CHECK(!contains(11) && !contains(12));
	/* Moving upward: the three backward folders are 10, 13 and 14. */
	CHECK(contains(14) && !contains(15));
	close_app();
}
static void promotion_reload_and_service_removal(void)
{
	open_app(true);
	advance();
	complete(2);
	CHECK(app.dircache->flight != 0);
	unsigned calls = spy.calls;
	select_folder(11);
	CHECK(app.dirprev_state == WF_DIRPREV_LOADING && spy.calls == calls);
	complete(2);
	CHECK(app.dirprev_state == WF_DIRPREV_READY);
	wf_app_preview_sync(&app, true);
	CHECK(app.dirprev_state == WF_DIRPREV_WAIT);
	advance();
	CHECK(spy.calls > calls);
	uint32_t old = spy.gen;
	WfDir *d = answer(1);
	wf_app_dirpreview_enable(&app, false);
	wf_app_on_dirpreview(&app, old, WF_OK, d);
	CHECK(app.dirprev_state == WF_DIRPREV_NONE && app.dircache->count == 0);
	close_app();
}
static void speculative_results_leave_display_unchanged(void)
{
	open_app(true);
	advance();
	complete(2);
	WfDir *visible = app.dirprev.dir;
	uint32_t preview_gen = app.preview_gen, image_gen = app.image.gen;
	while (app.dircache->flight) {
		CHECK(app.dircache->flight_slot != app.dircache->current);
		complete(2);
		CHECK(app.dirprev.dir == visible &&
		        app.dirprev_state == WF_DIRPREV_READY);
		CHECK(app.preview_gen == preview_gen &&
		        app.image.gen == image_gen);
	}
	wf_app_dircache_forget_neighbours(&app);
	CHECK(app.dirprev.dir == visible &&
	        app.dirprev_state == WF_DIRPREV_READY);
	CHECK(app.dircache->count == 1 && !app.dircache->flight);
	CHECK(wf_app_timeout(&app) == WF_WAIT_FOREVER);
	close_app();
}

/* A long list with a few folders far apart: the folders next to the cursor are
 * still the ones chosen, however many files lie between (walking over them costs
 * about 0.4 ms for 200000 entries, which is why nothing was added to avoid it). */
static void neighbours_are_found_without_walking_over_the_files(void)
{
	enum
	{
		N = 200000,
		STEP = 50000,
		MOVES = 200
	};
	static const size_t at[] = {0, STEP, 2 * STEP, 3 * STEP, N - 1};
	WfDir *d;
	clock_t t0;
	double ms;

	close_app();
	app.cwd = _wcsdup(L"C:\\cache");
	app.now_ms = 100;
	app.dirprev_port = (WfDirPreviewPort){&spy, request};
	d = wf_dir_new();
	CHECK(d != NULL);
	d->limit = N + 1;
	d->identity = id(1000);
	for (size_t i = 0; i < N; i++) {
		wchar_t name[32];
		bool folder = false;

		for (size_t k = 0; k < sizeof at / sizeof *at; k++) {
			folder = folder || i == at[k];
		}
		swprintf(name, 32, L"n%06zu", i);
		CHECK(wf_dir_add(d, name, wcslen(name), folder ? WF_ENT_DIR : 0,
		              1, 0) == WF_OK);
		d->e[d->n - 1].identity = id((unsigned)i + 1);
	}
	CHECK(wf_view_set_dir(&app.cur, d, NULL) == WF_OK);
	app.cur.cursor = 2 * STEP;
	wf_app_dircache_enable(&app, true);
	wf_app_preview_sync(&app, false);
	/* in the middle: the folder before it and after it, and the ones beyond */
	CHECK(app.dircache->count == 5);
	CHECK(contains((unsigned)(2 * STEP)) && contains((unsigned)STEP) &&
	        contains((unsigned)(3 * STEP)) && contains(0) &&
	        contains(N - 1));
	t0 = clock();
	for (int m = 0; m < MOVES; m++) {
		select_folder(at[1 + m % 3]); /* from folder to folder */
	}
	ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
	printf("info: %d moves of the cursor between folders of a list of %d: "
	       "%.1f ms\n",
	        (int)MOVES, (int)N, ms);
	CHECK(app.dircache->count >= 3);
	close_app();
}

int main(void)
{
	speculative_results_leave_display_unchanged();
	sliding_and_reversal();
	newest_request_and_invalidation();
	failures_never_become_empty_success();
	ttl_limits_and_incomplete_lists();
	rapid_cursor_and_settings();
	promotion_reload_and_service_removal();
	neighbours_are_found_without_walking_over_the_files();
	close_app();
	return TEST_RESULT();
}
