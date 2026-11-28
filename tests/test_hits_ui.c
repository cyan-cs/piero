/*
 * The results of a search below a folder in the application (DR-DRAFT-031): Tab in
 * the filter prompt makes the query and asks the worker (and refuses an empty or
 * wrong one), batches grow the list, the screen says what it is doing and what it
 * did not do, Esc stops and then closes, Enter shows a result in its folder, y and
 * x take the results of one folder (and say when they are in more), Delete asks one
 * question for any folders and sends one job for each, and nothing is sent that
 * is not proven. F lists the duplicates (DR-DRAFT-032) in the same window, grouped:
 * the last file of a group cannot be marked, only Delete is offered, and a Delete
 * that would remove a whole group is refused. The worker, the transfer and the copy
 * service are fakes.
 */
#include <stdio.h>
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "search/wf_search.h"
#include "ui/wf_app.h"
#include "ui/wf_app_priv.h"
#include "wf_fake_fs.h"
#include "wf_fake_probe.h"
#include "wf_fake_xfer.h"
#include "wf_test.h"

typedef struct
{
	int starts, cancels;
	int dupe_starts;
	wchar_t dupe_root[260];
	bool dupe_hidden;
	uint32_t gen;
	wchar_t root[260];
	WfFileId root_id;
	WfQuery query;
	bool hidden;
	WfStatus answer;
} FakeSearch;

static WfStatus fs_start(void *ctx,
        uint32_t gen,
        const wchar_t *root,
        WfFileId root_id,
        const WfQuery *query,
        bool hidden)
{
	FakeSearch *s = ctx;

	s->starts++;
	s->gen = gen;
	wcsncpy_s(s->root, 260, root, _TRUNCATE);
	s->root_id = root_id;
	s->query = *query;
	s->hidden = hidden;
	return s->answer;
}

static WfStatus fd_start(void *ctx,
        uint32_t gen,
        const wchar_t *root,
        WfFileId root_id,
        bool hidden)
{
	FakeSearch *s = ctx;

	(void)root_id;
	s->dupe_starts++;
	s->gen = gen;
	wcsncpy_s(s->dupe_root, 260, root, _TRUNCATE);
	s->dupe_hidden = hidden;
	return s->answer;
}

static void fs_cancel(void *ctx)
{
	((FakeSearch *)ctx)->cancels++;
}

typedef struct
{
	unsigned submits,
	        fail_from; /* the n-th submit (1 up) and the ones after fail */
	WfCopyKind kind[8];
	wchar_t dir[8][128], first[8][64];
	size_t count[8];
	WfFileId dir_id[8], first_id[8];
} FakeCopy;

static WfStatus fc_submit(void *ctx, const WfCopySpec *spec, uint32_t *job)
{
	FakeCopy *c = ctx;
	unsigned i = c->submits < 8 ? c->submits : 7;

	if (c->fail_from && c->submits + 1 >= c->fail_from) {
		return WF_E_INVAL;
	}
	c->submits++;
	c->kind[i] = spec->kind;
	wcscpy_s(c->dir[i], 128, spec->slot.dir);
	c->dir_id[i] = spec->slot.dir_id;
	wcscpy_s(c->first[i], 64, spec->slot.src[0].name);
	c->first_id[i] = spec->slot.src[0].snap.id;
	c->count[i] = spec->slot.count;
	if (job) {
		*job = c->submits;
	}
	return WF_OK;
}

static void fc_pause(void *ctx, bool paused)
{
	(void)ctx;
	(void)paused;
}

static bool fc_paused(void *ctx)
{
	(void)ctx;
	return false;
}

static void fc_cancel(void *ctx, bool all)
{
	(void)ctx;
	(void)all;
}

static bool fc_progress(void *ctx, WfCopyProgress *out)
{
	(void)ctx;
	(void)out;
	return false;
}

static FakeProbe P;

typedef struct
{
	WfApp a;
	FakePort fs;
	FakeXfer xf;
	FakeSearch sr;
	FakeCopy copy;
} Rig;

static void ch(WfApp *a, uint32_t c)
{
	WfEvent e = {WF_EV_CHAR, c, WF_KEY_NONE, 0};

	(void)a->mode->on_event(a, &e);
}

static void key(WfApp *a, WfKey k)
{
	WfEvent e = {WF_EV_KEY, 0, k, 0};

	(void)a->mode->on_event(a, &e);
}

static void ctrl(WfApp *a, uint32_t c)
{
	WfEvent e = {WF_EV_CTRL, c, WF_KEY_NONE, 0};

	(void)a->mode->on_event(a, &e);
}

static void type(WfApp *a, const wchar_t *s)
{
	for (; *s; s++) {
		ch(a, (uint32_t)*s);
	}
}

static void answer(WfApp *a, uint32_t c)
{
	CHECK(&WF_MODE_CONFIRM == a->mode);
	a->confirm.state = WF_CONF_ARMED;
	ch(a, c);
}

static void prove_listing(WfApp *a, uint8_t vol)
{
	WfDir *d = a->cur.dir;

	d->identity.kind = WF_ID_STRONG;
	d->identity.volume = vol;
	d->identity.id[0] = 11;
	for (size_t i = 0; i < d->n; i++) {
		d->e[i].identity.kind = WF_ID_STRONG;
		d->e[i].identity.volume = vol;
		d->e[i].identity.id[0] = (uint8_t)(i + 20);
	}
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

static void rig_open(Rig *r, bool with_search)
{
	memset(r, 0, sizeof *r);
	r->fs.app = &r->a;
	r->xf.app = &r->a;
	r->xf.ops = fp_ops(&P);
	CHECK(WF_OK ==
	        wf_app_init(
	                &r->a, 120, 30, fake_port(&r->fs), L"C:\\Users\\dev"));
	prove_listing(&r->a, 7);
	wf_app_set_xfer(&r->a, fx_port(&r->xf));
	wf_app_set_copy(&r->a,
	        (WfCopyPort){&r->copy, fc_submit, fc_pause, fc_paused,
	                fc_cancel, fc_progress},
	        WF_VERIFY_SHA256);
	if (with_search) {
		WfSearchPort port = {&r->sr, fs_start, fs_cancel, fd_start};

		wf_app_set_search(&r->a, port);
	}
}

typedef struct
{
	const wchar_t *dir;
	const wchar_t *name;
	bool is_dir;
	uint64_t size;
	bool proven;
} H;

static WfFileId fid(uint8_t n, bool valid)
{
	WfFileId id;

	memset(&id, 0, sizeof id);
	id.kind = (valid) ? WF_ID_STRONG : WF_ID_NONE;
	id.volume = 7;
	id.id[0] = n;
	return id;
}

static WfSearchBatch *batch(uint32_t gen, const H *h, size_t n, bool done)
{
	WfSearchBatch *b = calloc(1, sizeof *b);
	WfHitDir *last = NULL;

	b->gen = gen;
	b->hit = calloc(n ? n : 1, sizeof *b->hit);
	b->n = n;
	b->done = done;
	for (size_t i = 0; i < n; i++) {
		if (!last || wcscmp(last->path, h[i].dir)) {
			WfHitDir *d = wf_hitdir_new(
			        h[i].dir, fid((uint8_t)(100 + i), h[i].proven));

			wf_hitdir_unref(last);
			last = d;
		}
		b->hit[i].dir = wf_hitdir_ref(last);
		b->hit[i].name = _wcsdup(h[i].name);
		b->hit[i].id = fid((uint8_t)(50 + i), h[i].proven);
		b->hit[i].flags = h[i].is_dir ? WF_ENT_DIR : 0;
		b->hit[i].size = h[i].size;
		b->hit[i].mtime = 132000000000000000ull;
	}
	wf_hitdir_unref(last);
	return b;
}

static void deliver(WfApp *a, uint32_t gen, const H *h, size_t n, bool done)
{
	WfSearchBatch *b = batch(gen, h, n, done);

	wf_app_on_search(a, b);
	free(b);
}

static bool screen_has(const WfApp *a, const char *needle)
{
	WfFrame *f = wf_frame_new(a->cols, a->rows, &WF_STYLE_BASE);
	bool found = false;
	char line[1024];

	CHECK(f);
	wf_app_draw(a, f);
	for (int y = 0; y < f->rows && !found; y++) {
		line[0] = '\0';
		for (int x = 0; x < f->cols; x++) {
			strcat_s(line, sizeof line,
			        f->cells[y * f->cols + x].utf8);
		}
		found = NULL != strstr(line, needle);
	}
	wf_frame_free(f);
	return found;
}

static const H TWO_FOLDERS[] = {
        {L"C:\\Users\\dev", L"file2.txt", false, 20, true},
        {L"C:\\Users\\dev", L"file10.txt", false, 100, true},
        {L"C:\\Users\\dev\\Docs", L"readme.txt", false, 2048, true},
        {L"C:\\Users\\dev\\Docs", L"notes", true, 0, true},
};

static void started(Rig *r, const wchar_t *filter)
{
	ch(&r->a, '/');
	type(&r->a, filter);
	key(&r->a, WF_KEY_TAB);
}

static void tab_in_the_filter_makes_the_query_and_asks_the_worker(void)
{
	static Rig r;

	world();
	rig_open(&r, true);
	started(&r, L"file ext:txt size>1k");
	CHECK(&WF_MODE_HITS == r.a.mode && r.a.hits.open && r.a.hits.running);
	CHECK(1 == r.sr.starts && !wcscmp(r.sr.root, L"C:\\Users\\dev"));
	CHECK(1 == r.sr.query.n_word && 1 == r.sr.query.n_ext &&
	        r.sr.query.has_gt && 1024 == r.sr.query.gt);
	CHECK(!r.sr.hidden && (r.sr.root_id.kind == WF_ID_STRONG));
	CHECK(screen_has(
	        &r.a, "Search below C:\\Users\\dev: file ext:txt size>1k"));
	CHECK(screen_has(&r.a, "Nothing found yet") &&
	        screen_has(&r.a, "searching"));
	/* a second search ends the first one and takes the screen again */
	key(&r.a, WF_KEY_ESC);
	key(&r.a, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == r.a.mode && !r.a.hits.open);
	CHECK(r.sr.cancels >= 2);
	wf_app_destroy(&r.a);
}

static void an_empty_or_wrong_query_asks_for_nothing(void)
{
	static Rig r;

	world();
	rig_open(&r, true);
	ch(&r.a, '/');
	key(&r.a, WF_KEY_TAB);
	CHECK(0 == r.sr.starts && &WF_MODE_FILTER == r.a.mode);
	CHECK(strstr(r.a.msg, "Type what to look for"));
	type(&r.a, L"size>x");
	key(&r.a, WF_KEY_TAB);
	CHECK(0 == r.sr.starts && &WF_MODE_FILTER == r.a.mode);
	CHECK(strstr(r.a.msg, "Not a valid search") &&
	        strstr(r.a.msg, "position 1"));
	/* the filter is still what it was: it can be corrected */
	CHECK(!wcscmp(r.a.cur.filter, L"size>x"));
	wf_app_destroy(&r.a);

	/* a build without the worker, and a worker that refuses */
	rig_open(&r, false);
	started(&r, L"file");
	CHECK(&WF_MODE_FILTER == r.a.mode && strstr(r.a.msg, "not available"));
	wf_app_destroy(&r.a);
	rig_open(&r, true);
	r.sr.answer = WF_E_INVAL;
	started(&r, L"file");
	CHECK(&WF_MODE_FILTER == r.a.mode && !r.a.hits.open && !r.a.hits.root);
	CHECK(strstr(r.a.msg, "not available"));
	wf_app_destroy(&r.a);
}

static void batches_grow_the_list_and_stale_ones_are_dropped(void)
{
	static Rig r;

	world();
	rig_open(&r, true);
	started(&r, L"txt");
	deliver(&r.a, r.sr.gen + 5, TWO_FOLDERS, 2,
	        false); /* not the request out */
	CHECK(0 == r.a.hits.n);
	deliver(&r.a, r.sr.gen, TWO_FOLDERS, 2, false);
	CHECK(2 == r.a.hits.n && r.a.hits.running && !r.a.hits.done);
	CHECK(screen_has(&r.a, "2 found") && screen_has(&r.a, "searching"));
	deliver(&r.a, r.sr.gen, TWO_FOLDERS + 2, 2, true);
	CHECK(4 == r.a.hits.n && !r.a.hits.running && r.a.hits.done);
	CHECK(screen_has(&r.a, "4 found") && screen_has(&r.a, "finished"));
	/* the rows: the folder below the root and the name, the size and the day */
	CHECK(screen_has(&r.a, "file2.txt") && screen_has(&r.a, "file10.txt"));
	CHECK(screen_has(&r.a, "Docs\\readme.txt") &&
	        screen_has(&r.a, "Docs\\notes\\"));
	CHECK(screen_has(&r.a, "<folder>") && screen_has(&r.a, "2.0 KiB"));
	CHECK(screen_has(
	        &r.a, "2019-")); /* 132000000000000000 ticks: a day in 2019 */
	/* nothing arrives after the end */
	deliver(&r.a, r.sr.gen, TWO_FOLDERS, 1, false);
	CHECK(4 == r.a.hits.n);
	wf_app_destroy(&r.a);
}

static void a_same_folder_refresh_is_not_reported_as_ready(void)
{
	static Rig r;
	world();
	rig_open(&r, true);
	r.fs.defer = 1;
	r.a.now_ms = 100;
	wf_app_refresh(&r.a, false);
	CHECK(r.a.want_path && r.fs.nq == 1);
	CHECK(screen_has(&r.a, "LOADING"));
	CHECK(!screen_has(&r.a, "READY"));
	ch(&r.a, '/');
	key(&r.a, WF_KEY_TAB);
	CHECK(!r.sr.starts);
	CHECK(strstr(r.a.msg, "still being read"));
	fake_deliver(&r.fs, 0);
	CHECK(!r.a.want_path);
	CHECK(strstr(r.a.msg, "still being read"));
	ch(&r.a, 'x');
	CHECK(!r.a.msg[0]);
	CHECK(screen_has(&r.a, "FILTER READY"));
	key(&r.a, WF_KEY_TAB);
	CHECK(r.sr.starts == 1 && r.a.hits.running && !r.a.hits.n);
	r.a.now_ms += 10000;
	deliver(&r.a, r.sr.gen, TWO_FOLDERS, 1, true);
	CHECK(r.a.hits.done && r.a.hits.n == 1);
	wf_app_destroy(&r.a);
}

static void the_account_of_what_was_not_done_is_on_the_screen(void)
{
	static Rig r;
	WfSearchBatch *b;

	world();
	rig_open(&r, true);
	started(&r, L"txt");
	b = batch(r.sr.gen, TWO_FOLDERS, 1, true);
	b->walk.folders = 12;
	b->walk.unreadable = 3;
	b->walk.not_entered = 2;
	b->walk.pending_full = true;
	b->at_least = true;
	wf_app_on_search(&r.a, b);
	free(b);
	CHECK(screen_has(&r.a, "12 folders read"));
	CHECK(screen_has(&r.a, "3 folders could not be read"));
	CHECK(screen_has(&r.a, "2 links or cloud folders were not entered"));
	CHECK(screen_has(&r.a, "too many folders to read"));
	wf_app_destroy(&r.a);

	rig_open(&r, true);
	started(&r, L"txt");
	b = batch(r.sr.gen, TWO_FOLDERS, 0, true);
	b->st = WF_E_NOMEM;
	b->at_least = true;
	wf_app_on_search(&r.a, b);
	free(b);
	CHECK(screen_has(&r.a, "stopped: ") &&
	        screen_has(&r.a, "Nothing found"));
	wf_app_destroy(&r.a);
}

static void the_cursor_the_marks_and_esc(void)
{
	static Rig r;

	world();
	rig_open(&r, true);
	started(&r, L"txt");
	deliver(&r.a, r.sr.gen, TWO_FOLDERS, 4, false);
	CHECK(0 == r.a.hits.cursor);
	key(&r.a, WF_KEY_DOWN);
	ch(&r.a, 'j');
	CHECK(2 == r.a.hits.cursor);
	key(&r.a, WF_KEY_UP);
	ch(&r.a, 'k');
	CHECK(0 == r.a.hits.cursor);
	key(&r.a, WF_KEY_UP); /* not past the first */
	CHECK(0 == r.a.hits.cursor);
	key(&r.a, WF_KEY_END);
	CHECK(3 == r.a.hits.cursor);
	key(&r.a, WF_KEY_DOWN); /* nor past the last */
	CHECK(3 == r.a.hits.cursor);
	key(&r.a, WF_KEY_HOME);
	CHECK(0 == r.a.hits.cursor);
	key(&r.a, WF_KEY_PGDN);
	CHECK(3 == r.a.hits.cursor);
	key(&r.a, WF_KEY_PGUP);
	CHECK(0 == r.a.hits.cursor);

	/* Space marks and goes down; the count is on the screen */
	ch(&r.a, ' ');
	ch(&r.a, ' ');
	CHECK(2 == r.a.hits.nsel && r.a.hits.sel[0] && r.a.hits.sel[1] &&
	        !r.a.hits.sel[2] && 2 == r.a.hits.cursor);
	CHECK(screen_has(&r.a, "2 marked"));
	key(&r.a, WF_KEY_UP);
	ch(&r.a, ' '); /* unmark the second */
	CHECK(1 == r.a.hits.nsel && !r.a.hits.sel[1]);
	ctrl(&r.a, 'a');
	CHECK(4 == r.a.hits.nsel && r.a.hits.sel[3]);

	/* Esc stops the walk and keeps the list; the next closes it */
	key(&r.a, WF_KEY_ESC);
	CHECK(&WF_MODE_HITS == r.a.mode && !r.a.hits.running &&
	        4 == r.a.hits.n);
	CHECK(screen_has(&r.a, "stopped with Esc"));
	CHECK(r.sr.cancels >= 1);
	deliver(&r.a, r.sr.gen, TWO_FOLDERS, 1,
	        false); /* what still came is dropped */
	CHECK(4 == r.a.hits.n);
	key(&r.a, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == r.a.mode && !r.a.hits.open && 0 == r.a.hits.n);
	/* the filter is as it was: widened, not lost */
	CHECK(!wcscmp(r.a.cur.filter, L"txt"));
	wf_app_destroy(&r.a);
}

static void enter_shows_the_result_in_its_folder(void)
{
	static Rig r;

	world();
	rig_open(&r, true);
	started(&r, L"txt");
	deliver(&r.a, r.sr.gen, TWO_FOLDERS, 4, true);
	key(&r.a, WF_KEY_DOWN);
	key(&r.a, WF_KEY_ENTER);
	CHECK(&WF_MODE_NORMAL == r.a.mode && !r.a.hits.open);
	CHECK(!wcscmp(r.a.cwd, L"C:\\Users\\dev"));
	/* the cursor is on the result, once the listing is read again */
	prove_listing(&r.a, 7);
	CHECK(wf_view_current(&r.a.cur) &&
	        !wcscmp(wf_view_current(&r.a.cur)->name, L"file10.txt"));
	wf_app_destroy(&r.a);
}

static void y_and_x_take_the_results_of_one_folder(void)
{
	static Rig r;

	world();
	rig_open(&r, true);
	started(&r, L"txt");
	deliver(&r.a, r.sr.gen, TWO_FOLDERS, 4, true);
	/* two folders marked: refused, said, nothing asked for */
	ctrl(&r.a, 'a');
	ch(&r.a, 'y');
	CHECK(0 == r.xf.n_capture && &WF_MODE_HITS == r.a.mode);
	CHECK(strstr(r.a.msg, "one folder at a time") &&
	        strstr(r.a.msg, "2 folders"));
	ch(&r.a, 'x');
	CHECK(0 == r.xf.n_capture);

	/* the two of one folder: captured by name in that folder, then the list closes */
	ch(&r.a, ' '); /* Ctrl+A marked all: unmark by pressing Space on each */
	key(&r.a, WF_KEY_HOME);
	ctrl(&r.a, 'a');
	for (size_t i = 0; i < 4; i++) {
		r.a.hits.sel[i] = i < 2;
	}
	r.a.hits.nsel = 2;
	ch(&r.a, 'y');
	CHECK(1 == r.xf.n_capture && &WF_MODE_NORMAL == r.a.mode);
	CHECK(WF_XFER_COPY == r.a.slot.mode && 2 == r.a.slot.count);
	CHECK(!wcscmp(r.a.slot.dir, L"C:\\Users\\dev"));

	/* with nothing marked it is the one under the cursor; x is a move */
	started(&r, L"txt");
	deliver(&r.a, r.sr.gen, TWO_FOLDERS, 4, true);
	key(&r.a, WF_KEY_DOWN);
	ch(&r.a, 'x');
	CHECK(2 == r.xf.n_capture && WF_XFER_MOVE == r.a.slot.mode &&
	        1 == r.a.slot.count &&
	        !wcscmp(r.a.slot.src[0].name, L"file10.txt"));
	wf_app_destroy(&r.a);
}

static void delete_asks_once_and_sends_one_job_for_each_folder(void)
{
	static Rig r;

	world();
	rig_open(&r, true);
	started(&r, L"txt");
	deliver(&r.a, r.sr.gen, TWO_FOLDERS, 4, true);
	ctrl(&r.a, 'a');
	key(&r.a, WF_KEY_DELETE);
	CHECK(&WF_MODE_CONFIRM == r.a.mode && 0 == r.copy.submits);
	CHECK(!r.a.hits.open &&
	        0 == r.a.hits.n); /* the question has the screen */
	CHECK(2 == r.a.hits.n_pending && 4 == r.a.hits.items_pending);
	{
		bool said = false;

		for (int i = 0; i < r.a.confirm.n; i++) {
			said |= NULL !=
			        strstr(r.a.confirm.lines[i],
			                "4 item(s) in 2 folder(s)");
		}
		CHECK(said);
	}
	answer(&r.a, 'y');
	CHECK(2 == r.copy.submits);
	CHECK(WF_CJ_RECYCLE == r.copy.kind[0] &&
	        WF_CJ_RECYCLE == r.copy.kind[1]);
	CHECK(!wcscmp(r.copy.dir[0], L"C:\\Users\\dev") &&
	        2 == r.copy.count[0] && !wcscmp(r.copy.first[0], L"file2.txt"));
	CHECK(!wcscmp(r.copy.dir[1], L"C:\\Users\\dev\\Docs") &&
	        2 == r.copy.count[1] &&
	        !wcscmp(r.copy.first[1], L"readme.txt"));
	CHECK((r.copy.dir_id[0].kind == WF_ID_STRONG) &&
	        (r.copy.first_id[0].kind == WF_ID_STRONG) &&
	        (r.copy.dir_id[1].kind == WF_ID_STRONG) &&
	        (r.copy.first_id[1].kind == WF_ID_STRONG));
	CHECK(strstr(r.a.msg, "Sending 4 item(s) in 2 folder(s)"));
	CHECK(0 == r.a.hits.n_pending && NULL == r.a.hits.pending);
	wf_app_destroy(&r.a);
}

static void delete_of_the_one_under_the_cursor_and_after_a_no(void)
{
	static Rig r;

	world();
	rig_open(&r, true);
	started(&r, L"txt");
	deliver(&r.a, r.sr.gen, TWO_FOLDERS, 4, true);
	key(&r.a, WF_KEY_DOWN);
	key(&r.a, WF_KEY_DELETE);
	CHECK(&WF_MODE_CONFIRM == r.a.mode && 1 == r.a.hits.n_pending);
	answer(&r.a, 'n');
	CHECK(0 == r.copy.submits && &WF_MODE_NORMAL == r.a.mode);
	/* what was kept for the question goes with the next one, or with the app */
	started(&r, L"txt");
	deliver(&r.a, r.sr.gen, TWO_FOLDERS, 4, true);
	key(&r.a, WF_KEY_DELETE);
	CHECK(&WF_MODE_CONFIRM == r.a.mode && 1 == r.a.hits.n_pending);
	answer(&r.a, 'y');
	CHECK(1 == r.copy.submits && !wcscmp(r.copy.first[0], L"file2.txt"));
	wf_app_destroy(&r.a);
}

static void a_full_queue_sends_part_and_says_so(void)
{
	static Rig r;

	world();
	rig_open(&r, true);
	r.copy.fail_from = 2; /* the first folder goes, the second does not */
	started(&r, L"txt");
	deliver(&r.a, r.sr.gen, TWO_FOLDERS, 4, true);
	ctrl(&r.a, 'a');
	key(&r.a, WF_KEY_DELETE);
	answer(&r.a, 'y');
	CHECK(1 == r.copy.submits);
	CHECK(strstr(r.a.msg, "Only 1 of 2 folders"));
	wf_app_destroy(&r.a);
}

static void nothing_unproven_is_sent_and_too_many_folders_are_refused(void)
{
	static Rig r;
	static H many[WF_HITS_MAX_FOLDERS + 2];
	static wchar_t dirs[WF_HITS_MAX_FOLDERS + 2][40];
	H unproven[] = {{L"C:\\Users\\dev", L"file2.txt", false, 20, false}};

	world();
	rig_open(&r, true);
	started(&r, L"txt");
	deliver(&r.a, r.sr.gen, unproven, 1, true);
	key(&r.a, WF_KEY_DELETE);
	CHECK(&WF_MODE_HITS == r.a.mode && 0 == r.a.hits.n_pending);
	CHECK(strstr(r.a.msg, "without file identities"));
	wf_app_destroy(&r.a);

	rig_open(&r, true);
	started(&r, L"txt");
	for (size_t i = 0; i < WF_HITS_MAX_FOLDERS + 2; i++) {
		swprintf(dirs[i], 40, L"C:\\Users\\dev\\d%zu", i);
		many[i].dir = dirs[i];
		many[i].name = L"a.txt";
		many[i].proven = true;
	}
	deliver(&r.a, r.sr.gen, many, WF_HITS_MAX_FOLDERS + 2, true);
	ctrl(&r.a, 'a');
	key(&r.a, WF_KEY_DELETE);
	CHECK(&WF_MODE_HITS == r.a.mode && 0 == r.copy.submits);
	CHECK(strstr(r.a.msg, "up to 64 folders") &&
	        strstr(r.a.msg, "these are in 66"));
	wf_app_destroy(&r.a);
}

static const H TWO_GROUPS[] = {
        {L"C:\\Users\\dev", L"file2.txt", false, 20, true},
        {L"C:\\Users\\dev\\Docs", L"readme.txt", false, 20, true},
        {L"C:\\Users\\dev", L"file10.txt", false, 100, true},
        {L"C:\\Users\\dev\\Docs", L"copy of file10.txt", false, 100, true},
};

static void deliver_dupes(WfApp *a,
        uint32_t gen,
        const H *h,
        size_t n,
        bool done,
        uint64_t read_done,
        uint64_t read_total)
{
	WfSearchBatch *b = batch(gen, h, n, done);

	b->dupes = true;
	b->read_done = read_done;
	b->read_total = read_total;
	for (size_t i = 0; i < n; i++) {
		b->hit[i].group = (uint32_t)(i / 2 + 1);
	}
	if (done) {
		b->groups = n / 2;
		b->files_in_groups = n;
		b->skipped = 1;
	}
	wf_app_on_search(a, b);
	free(b);
}

static void f_asks_the_worker_for_duplicates_and_shows_the_groups(void)
{
	static Rig r;

	world();
	rig_open(&r, true);
	ch(&r.a, 'F');
	CHECK(&WF_MODE_HITS == r.a.mode && r.a.hits.open && r.a.hits.running &&
	        r.a.hits.dupes);
	CHECK(1 == r.sr.dupe_starts && 0 == r.sr.starts &&
	        !wcscmp(r.sr.dupe_root, L"C:\\Users\\dev") &&
	        !r.sr.dupe_hidden);
	CHECK(screen_has(&r.a, "Duplicate files below C:\\Users\\dev"));
	CHECK(screen_has(&r.a, "listing folders"));
	deliver_dupes(&r.a, r.sr.gen, NULL, 0, false, 2, 5);
	CHECK(screen_has(&r.a, "reading 2 of 5 files"));
	deliver_dupes(&r.a, r.sr.gen, TWO_GROUPS, 4, false, 4, 5);
	CHECK(screen_has(&r.a, "4 files in 2 groups") &&
	        screen_has(&r.a, "[1]") && screen_has(&r.a, "[2]") &&
	        screen_has(&r.a, "file2.txt") && screen_has(&r.a, "Docs"));
	/* a search result batch is not a duplicate one, and the other way round */
	{
		WfSearchBatch *b = batch(r.sr.gen, TWO_FOLDERS, 4, true);
		size_t before = r.a.hits.n;

		wf_app_on_search(&r.a, b);
		free(b);
		CHECK(before == r.a.hits.n);
	}
	deliver_dupes(&r.a, r.sr.gen, NULL, 0, true, 5, 5);
	CHECK(screen_has(&r.a, "finished") &&
	        screen_has(&r.a, "1 files could not be compared"));
	CHECK(screen_has(&r.a, "mark the copies to remove"));
	key(&r.a, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == r.a.mode && !r.a.hits.open && !r.a.hits.dupes);
	wf_app_destroy(&r.a);
}

static void f_without_a_worker_or_on_an_unsearchable_folder_says_so(void)
{
	static Rig r;

	world();
	rig_open(&r, false);
	ch(&r.a, 'F');
	CHECK(&WF_MODE_NORMAL == r.a.mode && strstr(r.a.msg, "not available"));
	wf_app_destroy(&r.a);
	rig_open(&r, true);
	r.sr.answer = WF_E_INVAL; /* the worker refuses */
	ch(&r.a, 'F');
	CHECK(&WF_MODE_NORMAL == r.a.mode && !r.a.hits.open &&
	        1 == r.sr.dupe_starts);
	wf_app_destroy(&r.a);
}

static void
the_last_file_of_a_group_cannot_be_marked_and_only_delete_is_offered(void)
{
	static Rig r;

	world();
	rig_open(&r, true);
	ch(&r.a, 'F');
	deliver_dupes(&r.a, r.sr.gen, TWO_GROUPS, 4, true, 5, 5);
	ch(&r.a, ' '); /* marks file2.txt and goes down */
	CHECK(1 == r.a.hits.nsel && r.a.hits.sel[0] && 1 == r.a.hits.cursor);
	ch(&r.a, ' '); /* readme.txt would be the last of group 1 */
	CHECK(1 == r.a.hits.nsel && !r.a.hits.sel[1]);
	CHECK(screen_has(&r.a, "Each group keeps one file"));
	ch(&r.a,
	        ' '); /* group 2 is not touched yet: fine (the cursor went on) */
	CHECK(2 == r.a.hits.nsel && r.a.hits.sel[2]);
	/* Ctrl+A, y and x are not offered, and change nothing */
	ctrl(&r.a, 'a');
	CHECK(2 == r.a.hits.nsel && screen_has(&r.a, "only recycled"));
	ch(&r.a, 'y');
	ch(&r.a, 'x');
	CHECK(&WF_MODE_HITS == r.a.mode && 2 == r.a.hits.nsel);
	CHECK(screen_has(&r.a, "only recycled") && 0 == r.copy.submits);
	/* unmarking is always allowed */
	key(&r.a, WF_KEY_UP);
	key(&r.a, WF_KEY_UP);
	key(&r.a, WF_KEY_UP);
	ch(&r.a, ' ');
	CHECK(1 == r.a.hits.nsel && !r.a.hits.sel[0]);
	wf_app_destroy(&r.a);
}

/* The other files of a group are looked for in the group only: the first file of
 * the second group has the last file of the first group above it, and that one
 * is not a file it may keep. */
static void a_neighbour_group_is_not_a_copy_to_keep(void)
{
	static Rig r;

	world();
	rig_open(&r, true);
	ch(&r.a, 'F');
	deliver_dupes(&r.a, r.sr.gen, TWO_GROUPS, 4, true, 5, 5);
	r.a.hits.cursor = 3;
	ch(&r.a, ' '); /* the second file of group 2: its first is free */
	CHECK(1 == r.a.hits.nsel && r.a.hits.sel[3]);
	r.a.hits.cursor = 2;
	ch(&r.a,
	        ' '); /* the first of group 2 would be the last: group 1 is not it */
	CHECK(1 == r.a.hits.nsel && !r.a.hits.sel[2]);
	r.a.hits.cursor = 0;
	ch(&r.a, ' '); /* the first of group 1: its second is free */
	CHECK(2 == r.a.hits.nsel && r.a.hits.sel[0]);
	r.a.hits.cursor = 1;
	ch(&r.a,
	        ' '); /* the second of group 1 would be the last; group 2 is below */
	CHECK(2 == r.a.hits.nsel && !r.a.hits.sel[1]);
	wf_app_destroy(&r.a);
}

static void delete_sends_the_marked_copies_and_never_a_whole_group(void)
{
	static Rig r;

	world();
	rig_open(&r, true);
	ch(&r.a, 'F');
	deliver_dupes(&r.a, r.sr.gen, TWO_GROUPS, 4, true, 5, 5);
	/* the copies in Docs go: one of each group stays */
	key(&r.a, WF_KEY_DOWN);
	ch(&r.a, ' '); /* readme.txt */
	key(&r.a, WF_KEY_DOWN);
	ch(&r.a, ' '); /* copy of file10.txt */
	CHECK(2 == r.a.hits.nsel);
	key(&r.a, WF_KEY_DELETE);
	CHECK(&WF_MODE_CONFIRM == r.a.mode && 1 == r.a.hits.n_pending &&
	        2 == r.a.hits.items_pending);
	answer(&r.a, 'y');
	CHECK(1 == r.copy.submits && WF_CJ_RECYCLE == r.copy.kind[0] &&
	        !wcscmp(r.copy.dir[0], L"C:\\Users\\dev\\Docs") &&
	        2 == r.copy.count[0]);
	wf_app_destroy(&r.a);

	/* the one under the cursor alone is one of two: fine */
	rig_open(&r, true);
	ch(&r.a, 'F');
	deliver_dupes(&r.a, r.sr.gen, TWO_GROUPS, 4, true, 5, 5);
	key(&r.a, WF_KEY_DELETE);
	CHECK(&WF_MODE_CONFIRM == r.a.mode && 1 == r.a.hits.n_pending);
	answer(&r.a, 'n');
	CHECK(0 == r.copy.submits);
	wf_app_destroy(&r.a);
}

static void a_whole_group_is_never_recycled_even_if_the_marks_got_there(void)
{
	static Rig r;

	world();
	rig_open(&r, true);
	ch(&r.a, 'F');
	deliver_dupes(&r.a, r.sr.gen, TWO_GROUPS, 4, true, 5, 5);
	/* the marks are set behind the rule's back, as a bug elsewhere could */
	r.a.hits.sel[0] = r.a.hits.sel[1] = true;
	r.a.hits.nsel = 2;
	key(&r.a, WF_KEY_DELETE);
	CHECK(&WF_MODE_HITS == r.a.mode && 0 == r.a.hits.n_pending &&
	        0 == r.copy.submits);
	CHECK(screen_has(&r.a, "Every file of a group is marked"));
	wf_app_destroy(&r.a);
}

/* How long the Delete key takes with a large number of results chosen: the
 * folders are counted once and the groups are checked once, so it grows with the
 * number of results and not with its square. */
static double delete_time_ms(Rig *r)
{
	clock_t t0 = clock();

	key(&r->a, WF_KEY_DELETE);
	return (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
}

static void many_folders_are_counted_without_a_comparison_of_each_with_each(
        void)
{
	enum
	{
		N = 20000
	};
	static Rig r;
	H *many = calloc(N, sizeof *many);
	wchar_t(*dirs)[40] = calloc(N, sizeof *dirs);
	double ms;

	CHECK(many && dirs);
	world();
	rig_open(&r, true);
	started(&r, L"txt");
	for (size_t i = 0; i < N; i++) {
		swprintf(dirs[i], 40, L"C:\\Users\\dev\\d%zu", i);
		many[i].dir = dirs[i];
		many[i].name = L"a.txt";
		many[i].proven = true;
	}
	deliver(&r.a, r.sr.gen, many, N, true);
	ctrl(&r.a, 'a');
	ms = delete_time_ms(&r);
	printf("info: Delete with %d results in %d folders: %.1f ms\n", (int)N,
	        (int)N, ms);
	CHECK(&WF_MODE_HITS == r.a.mode && 0 == r.copy.submits);
	CHECK(strstr(r.a.msg, "up to 64 folders") &&
	        strstr(r.a.msg, "these are in 20000"));
	CHECK(ms < 1000.0);
	wf_app_destroy(&r.a);
	free(many);
	free(dirs);
}

/* The duplicate window with one group of N files in one folder. */
static void one_group_of(Rig *r, size_t n, H *files, wchar_t (*names)[24])
{
	WfSearchBatch *b;

	world();
	rig_open(r, true);
	ch(&r->a, 'F');
	for (size_t i = 0; i < n; i++) {
		swprintf(names[i], 24, L"copy%05zu.txt", i);
		files[i].dir = L"C:\\Users\\dev";
		files[i].name = names[i];
		files[i].size = 10;
		files[i].proven = true;
	}
	b = batch(r->sr.gen, files, n, true);
	b->dupes = true;
	b->groups = 1;
	b->files_in_groups = n;
	for (size_t i = 0; i < n; i++) {
		b->hit[i].group = 1;
	}
	wf_app_on_search(&r->a, b);
	free(b);
}

static void a_group_of_many_files_is_checked_in_one_pass(void)
{
	enum
	{
		N = 20000
	};
	static Rig r;
	H *files = calloc(N, sizeof *files);
	wchar_t(*names)[24] = calloc(N, sizeof *names);
	double ms;

	CHECK(files && names);
	/* every one but the last is marked, as the rule allows */
	one_group_of(&r, N, files, names);
	for (size_t i = 0; i + 1 < N; i++) {
		r.a.hits.sel[i] = true;
	}
	r.a.hits.nsel = N - 1;
	ms = delete_time_ms(&r);
	printf("info: Delete of %d of the %d files of one group: %.1f ms\n",
	        (int)(N - 1), (int)N, ms);
	CHECK(&WF_MODE_CONFIRM == r.a.mode && 1 == r.a.hits.n_pending);
	if (1 == r.a.hits.n_pending) {
		CHECK(N - 1 == r.a.hits.pending[0].count);
		CHECK(N - 1 == r.a.hits.items_pending);
	}
	CHECK(ms < 1000.0);
	answer(&r.a, 'n');
	wf_app_destroy(&r.a);
	/* every one of them: refused, as a whole group is never recycled */
	one_group_of(&r, N, files, names);
	for (size_t i = 0; i < N; i++) {
		r.a.hits.sel[i] = true;
	}
	r.a.hits.nsel = N;
	ms = delete_time_ms(&r);
	printf("info: Delete of all %d files of one group (refused): %.1f ms\n",
	        (int)N, ms);
	CHECK(&WF_MODE_HITS == r.a.mode && 0 == r.a.hits.n_pending &&
	        0 == r.copy.submits);
	CHECK(ms < 1000.0);
	wf_app_destroy(&r.a);
	free(files);
	free(names);
}

/* Space down a group of N files: each press marks one, and the last one cannot be
 * marked (the rule needs another that is not marked, found without a walk of the
 * whole group each time). */
static void marking_a_group_one_by_one_does_not_walk_it_each_time(void)
{
	enum
	{
		N = 20000
	};
	static Rig r;
	H *files = calloc(N, sizeof *files);
	wchar_t(*names)[24] = calloc(N, sizeof *names);
	clock_t t0;
	double ms;

	CHECK(files && names);
	one_group_of(&r, N, files, names);
	t0 = clock();
	for (size_t i = 0; i < N; i++) {
		ch(&r.a, ' '); /* marks and goes down; the last is refused */
	}
	ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
	printf("info: Space on each of the %d files of one group: %.1f ms\n",
	        (int)N, ms);
	CHECK(N - 1 == r.a.hits.nsel && !r.a.hits.sel[N - 1]);
	for (size_t i = 0; i + 1 < N; i++) {
		CHECK(r.a.hits.sel[i]);
	}
	CHECK(ms < 20000.0);
	wf_app_destroy(&r.a);
	free(files);
	free(names);
}

/* The question about many results in a few folders makes room for the results of
 * each folder, not for all of them in every folder. */
static void a_slot_has_room_for_its_own_results_only(void)
{
	enum
	{
		FOLDERS = 64,
		PER = 500,
		N = FOLDERS * PER
	};
	static Rig r;
	H *many = calloc(N, sizeof *many);
	wchar_t(*dirs)[40] = calloc(FOLDERS, sizeof *dirs);
	wchar_t(*names)[24] = calloc(N, sizeof *names);
	PROCESS_MEMORY_COUNTERS pmc;
	size_t before, after;

	CHECK(many && dirs && names);
	world();
	rig_open(&r, true);
	started(&r, L"txt");
	for (size_t f = 0; f < FOLDERS; f++) {
		swprintf(dirs[f], 40, L"C:\\Users\\dev\\d%zu", f);
	}
	for (size_t i = 0; i < N; i++) {
		swprintf(names[i], 24, L"f%06zu.txt", i);
		many[i].dir = dirs[i % FOLDERS]; /* the folders alternate */
		many[i].name = names[i];
		many[i].proven = true;
	}
	deliver(&r.a, r.sr.gen, many, N, true);
	ctrl(&r.a, 'a');
	GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
	before = pmc.PeakPagefileUsage;
	key(&r.a, WF_KEY_DELETE);
	GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
	after = pmc.PeakPagefileUsage;
	printf("info: Delete of %d results in %d folders: peak memory +%zu "
	       "KiB\n",
	        (int)N, (int)FOLDERS,
	        after > before ? (after - before) / 1024 : (size_t)0);
	CHECK(&WF_MODE_CONFIRM == r.a.mode && FOLDERS == r.a.hits.n_pending);
	for (size_t i = 0;
	        i < r.a.hits.n_pending && FOLDERS == r.a.hits.n_pending; i++) {
		CHECK(PER == r.a.hits.pending[i].count);
	}
	CHECK(after < before + 16u * 1024u * 1024u);
	wf_app_destroy(&r.a);
	free(many);
	free(dirs);
	free(names);
}

/* Each folder's slot holds its own results, in the order they were chosen, and the
 * slots come in the order the folders first appear. */
static void the_slots_hold_each_folders_results_in_the_order_chosen(void)
{
	static Rig r;
	static const H mixed[] = {
	        {L"C:\\Users\\dev\\b", L"b1.txt", false, 1, true},
	        {L"C:\\Users\\dev\\a", L"a1.txt", false, 1, true},
	        {L"C:\\Users\\dev\\b", L"b2.txt", false, 1, true},
	        {L"C:\\Users\\dev\\c", L"c1.txt", false, 1, true},
	        {L"C:\\Users\\dev\\a", L"a2.txt", false, 1, true},
	        {L"C:\\Users\\dev\\b", L"b3.txt", false, 1, true},
	};

	world();
	rig_open(&r, true);
	started(&r, L"txt");
	deliver(&r.a, r.sr.gen, mixed, 6, true);
	ctrl(&r.a, 'a');
	key(&r.a, WF_KEY_DELETE);
	CHECK(&WF_MODE_CONFIRM == r.a.mode && 3 == r.a.hits.n_pending &&
	        6 == r.a.hits.items_pending);
	CHECK(!wcscmp(r.a.hits.pending[0].dir, L"C:\\Users\\dev\\b") &&
	        3 == r.a.hits.pending[0].count &&
	        !wcscmp(r.a.hits.pending[0].src[0].name, L"b1.txt") &&
	        !wcscmp(r.a.hits.pending[0].src[1].name, L"b2.txt") &&
	        !wcscmp(r.a.hits.pending[0].src[2].name, L"b3.txt"));
	CHECK(!wcscmp(r.a.hits.pending[1].dir, L"C:\\Users\\dev\\a") &&
	        2 == r.a.hits.pending[1].count &&
	        !wcscmp(r.a.hits.pending[1].src[1].name, L"a2.txt"));
	CHECK(!wcscmp(r.a.hits.pending[2].dir, L"C:\\Users\\dev\\c") &&
	        1 == r.a.hits.pending[2].count);
	wf_app_destroy(&r.a);
}

static void destroying_the_app_with_results_and_a_question_kept_leaks_nothing(
        void)
{
	static Rig r;

	world();
	rig_open(&r, true);
	started(&r, L"txt");
	deliver(&r.a, r.sr.gen, TWO_FOLDERS, 4, false);
	wf_app_destroy(&r.a);

	rig_open(&r, true);
	started(&r, L"txt");
	deliver(&r.a, r.sr.gen, TWO_FOLDERS, 4, true);
	ctrl(&r.a, 'a');
	key(&r.a, WF_KEY_DELETE);
	CHECK(&WF_MODE_CONFIRM == r.a.mode);
	wf_app_destroy(&r.a);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	tab_in_the_filter_makes_the_query_and_asks_the_worker();
	a_same_folder_refresh_is_not_reported_as_ready();
	an_empty_or_wrong_query_asks_for_nothing();
	batches_grow_the_list_and_stale_ones_are_dropped();
	the_account_of_what_was_not_done_is_on_the_screen();
	the_cursor_the_marks_and_esc();
	enter_shows_the_result_in_its_folder();
	y_and_x_take_the_results_of_one_folder();
	delete_asks_once_and_sends_one_job_for_each_folder();
	delete_of_the_one_under_the_cursor_and_after_a_no();
	a_full_queue_sends_part_and_says_so();
	f_asks_the_worker_for_duplicates_and_shows_the_groups();
	f_without_a_worker_or_on_an_unsearchable_folder_says_so();
	the_last_file_of_a_group_cannot_be_marked_and_only_delete_is_offered();
	a_neighbour_group_is_not_a_copy_to_keep();
	delete_sends_the_marked_copies_and_never_a_whole_group();
	a_whole_group_is_never_recycled_even_if_the_marks_got_there();
	nothing_unproven_is_sent_and_too_many_folders_are_refused();
	many_folders_are_counted_without_a_comparison_of_each_with_each();
	a_group_of_many_files_is_checked_in_one_pass();
	marking_a_group_one_by_one_does_not_walk_it_each_time();
	the_slots_hold_each_folders_results_in_the_order_chosen();
	a_slot_has_room_for_its_own_results_only();
	destroying_the_app_with_results_and_a_question_kept_leaks_nothing();
	return TEST_RESULT();
}
