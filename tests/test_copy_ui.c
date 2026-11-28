/* Copy, cross-volume move, recycle, pause/cancel and undo as the app drives
 * them. The copy engine is a fake that records what it was asked to do; the
 * transfer checks are the real ones against an in-memory file system. */
#include <time.h>
#include <stdlib.h>
#include <string.h>

#include "ui/wf_app_priv.h"
#include "text/wf_text.h"
#include "fs/wf_weakid_win.h"
#include "wf_fake_fs.h"
#include "wf_fake_xfer.h"
#include "wf_test.h"

static FakeProbe P;

typedef struct
{
	unsigned submits;
	WfCopyKind kind;
	WfCollide collide;
	WfCopyVerify verify;
	wchar_t dest[128], dir[128], first[64];
	wchar_t archive[128], inner0[128];
	size_t n_inner;
	WfFileId first_id;
	size_t count, total_files;
	uint64_t total_bytes;
	bool hydrate;
	WfStatus answer;
	unsigned pause_calls;
	bool paused;
	unsigned cancel_calls;
	bool cancel_all;
	bool busy;
	WfCopyProgress prog;
} FakeCopy;

static WfStatus fc_submit(void *ctx, const WfCopySpec *spec, uint32_t *job)
{
	FakeCopy *c = ctx;

	c->submits++;
	c->kind = spec->kind;
	c->collide = spec->collide;
	c->verify = spec->verify;
	c->hydrate = spec->hydrate;
	c->count = spec->slot.count;
	c->total_files = spec->total_files;
	c->total_bytes = spec->total_bytes;
	wcscpy_s(c->dest, 128, spec->dest ? spec->dest : L"");
	wcscpy_s(c->dir, 128, spec->slot.dir ? spec->slot.dir : L"");
	wcscpy_s(c->first, 64, spec->slot.count ? spec->slot.src[0].name : L"");
	if (spec->slot.count) {
		c->first_id = spec->slot.src[0].snap.id;
	}
	wcscpy_s(c->archive, 128, spec->archive ? spec->archive : L"");
	wcscpy_s(c->inner0, 128, spec->n_inner ? spec->inner[0] : L"");
	c->n_inner = spec->n_inner;
	if (job) {
		*job = c->submits;
	}
	return c->answer;
}

static void fc_pause(void *ctx, bool paused)
{
	FakeCopy *c = ctx;

	c->pause_calls++;
	c->paused = paused;
}

static bool fc_paused(void *ctx)
{
	return ((FakeCopy *)ctx)->paused;
}

static void fc_cancel(void *ctx, bool all)
{
	FakeCopy *c = ctx;

	c->cancel_calls++;
	c->cancel_all = all;
}

static bool fc_progress(void *ctx, WfCopyProgress *out)
{
	FakeCopy *c = ctx;

	*out = c->prog;
	return c->busy;
}

typedef struct
{
	unsigned calls;
	uint32_t gen;
	WfEditRequest req[4];
	wchar_t old_name[4][64], parent[4][128], dest[4][128], new_name[4][64];
} Spy;

static WfStatus spy_submit(void *ctx, uint32_t gen, const WfEditRequest *req)
{
	Spy *s = ctx;
	unsigned i = s->calls < 4 ? s->calls : 3;

	s->calls++;
	s->gen = gen;
	s->req[i] = *req;
	wcscpy_s(s->old_name[i], 64, req->old_name ? req->old_name : L"");
	wcscpy_s(s->new_name[i], 64, req->new_name ? req->new_name : L"");
	wcscpy_s(s->parent[i], 128, req->parent);
	wcscpy_s(s->dest[i], 128, req->dest_parent ? req->dest_parent : L"");
	return WF_OK;
}

static void ch(WfApp *a, uint32_t c)
{
	WfEvent e = {WF_EV_CHAR, c, WF_KEY_NONE, 0};

	a->mode->on_event(a, &e);
}

static void key(WfApp *a, WfKey k)
{
	WfEvent e = {WF_EV_KEY, 0, k, 0};

	a->mode->on_event(a, &e);
}

static void type(WfApp *a, const wchar_t *s)
{
	for (; *s; s++) {
		ch(a, (uint32_t)*s);
	}
}

/* Answers to a modal as a user would once its input guard has passed. */
static void answer(WfApp *a, uint32_t c)
{
	CHECK(&WF_MODE_CONFIRM == a->mode);
	a->confirm.state = WF_CONF_ARMED;
	ch(a, c);
}

static bool modal_says(const WfApp *a, const char *needle)
{
	for (int i = 0; i < a->confirm.n; i++) {
		if (strstr(a->confirm.lines[i], needle)) {
			return true;
		}
	}
	return false;
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

typedef struct
{
	WfApp app;
	FakePort fs;
	FakeXfer xf;
	FakeCopy copy;
	Spy spy;
} Rig;

static void rig_open(Rig *r, bool with_copy, bool with_edit)
{
	memset(r, 0, sizeof *r);
	r->fs.app = &r->app;
	r->xf.app = &r->app;
	r->xf.ops = fp_ops(&P);
	CHECK(wf_app_init(&r->app, 120, 30, fake_port(&r->fs),
	              L"C:\\Users\\dev") == WF_OK);
	prove_listing(&r->app, 7);
	wf_app_set_xfer(&r->app, fx_port(&r->xf));
	if (with_copy) {
		wf_app_set_copy(&r->app,
		        (WfCopyPort){&r->copy, fc_submit, fc_pause, fc_paused,
		                fc_cancel, fc_progress},
		        WF_VERIFY_SHA256);
	}
	if (with_edit) {
		wf_app_set_edit(&r->app, (WfEditPort){&r->spy, spy_submit});
	}
}

/* Browsing C:\Users\dev, file2.txt captured with y, then into C:\Users. */
static void rig_copy_ready(Rig *r)
{
	rig_open(r, true, true);
	ch(&r->app, 'j'); /* file2.txt (Docs is first) */
	ch(&r->app, 'y');
	CHECK(WF_XFER_COPY == r->app.slot.mode && 1 == r->app.slot.count);
	ch(&r->app, 'h');
	CHECK(!wcscmp(r->app.cwd, L"C:\\Users"));
	prove_listing(&r->app, 7);
}

static WfCopyResult result_of(WfCopyKind kind, WfCopyState state)
{
	WfCopyResult r;

	memset(&r, 0, sizeof r);
	r.job = 1;
	r.kind = kind;
	r.verify = WF_VERIFY_SHA256; /* what the rig tells its jobs to check */
	r.state = state;
	return r;
}

static void add_created(WfCopyResult *r, const wchar_t *path, uint8_t id)
{
	r->created = calloc(1, sizeof *r->created);
	CHECK(NULL != r->created);
	r->created[0].path = _wcsdup(path);
	r->created[0].id.kind = WF_ID_STRONG;
	r->created[0].id.volume = 7;
	r->created[0].id.id[0] = id;
	r->created_n = 1;
}

static void a_confirmed_check_starts_a_copy_job(void)
{
	static Rig r;
	WfCopyResult res = result_of(WF_CJ_COPY, WF_CS_DONE);

	world();
	rig_copy_ready(&r);
	ch(&r.app, 'p');
	CHECK(1 == r.copy.submits && WF_CJ_COPY == r.copy.kind &&
	        WF_COLLIDE_STOP == r.copy.collide &&
	        WF_VERIFY_SHA256 == r.copy.verify && !r.copy.hydrate);
	CHECK(!wcscmp(r.copy.dest, L"C:\\Users") &&
	        !wcscmp(r.copy.dir, L"C:\\Users\\dev") &&
	        !wcscmp(r.copy.first, L"file2.txt") && 1 == r.copy.count);
	CHECK((r.copy.first_id.kind == WF_ID_STRONG));
	CHECK(1 == r.copy.total_files && 20 == r.copy.total_bytes);
	CHECK(1 == r.app.copy_jobs);
	CHECK(WF_XFER_COPY == r.app.slot.mode); /* paste again is possible */
	/* progress is on the status line while the job runs */
	{
		WfBuf s;

		r.copy.busy = true;
		r.copy.prog.state = WF_CS_RUNNING;
		r.copy.prog.files_done = 1;
		r.copy.prog.files_total = 3;
		r.copy.prog.bytes_done = 2048;
		r.copy.prog.bytes_total = 4096;
		wf_buf_init(&s);
		wf_app_copy_status(&r.app, &s, 200);
		CHECK(strstr(wf_buf_cstr(&s), "working 1/3 files") &&
		        strstr(wf_buf_cstr(&s), "2.0 KiB of 4.0 KiB"));
		wf_buf_free(&s);
		/* a long name in a narrow line: the state, the count and the keys
		 * stay, and the name is what is cut to the room that is left */
		wcscpy_s(r.copy.prog.current,
		        sizeof r.copy.prog.current /
		                sizeof *r.copy.prog.current,
		        L"a-very-long-file-name-that-does-not-fit-in-the-line."
		        L"bin");
		for (int room = 45; room <= 75; room += 15) {
			wf_buf_init(&s);
			wf_app_copy_status(&r.app, &s, room);
			CHECK(wf_utf8_cells(wf_buf_cstr(&s), s.len) <= room);
			CHECK(strstr(wf_buf_cstr(&s), "1/3 files") &&
			        strstr(wf_buf_cstr(&s), "[z pause, C cancel]"));
			if (45 ==
			        room) { /* no room left for a name: none is shown */
				CHECK(!strstr(wf_buf_cstr(&s), "a-"));
			}
			wf_buf_free(&s);
		}
		wf_buf_init(&s);
		wf_app_copy_status(&r.app, &s, 70);
		CHECK(strstr(
		        wf_buf_cstr(&s), "a-ver")); /* the name is there, cut */
		wf_buf_free(&s);
		r.copy.prog.current[0] = 0;
		r.app.now_ms = 1000;
		CHECK(0 == wf_app_timeout(&r.app));
		CHECK(WF_ACT_REDRAW == wf_app_tick(&r.app));
		CHECK(wf_app_timeout(&r.app) > 0 &&
		        wf_app_timeout(&r.app) <= 200);
	}
	res.files_copied = 1;
	res.bytes = 20;
	wf_app_on_copy_result(&r.app, &res);
	CHECK(0 == r.app.copy_jobs &&
	        strstr(r.app.msg, "COPY done: 1 file(s)") &&
	        strstr(r.app.msg, "SHA-256"));
	CHECK(&WF_MODE_NORMAL ==
	        r.app.mode); /* a clean result needs no window */
	CHECK(wf_app_timeout(&r.app) > 0);
	r.app.now_ms = r.app.msg_keep_until;
	CHECK(WF_ACT_REDRAW == wf_app_tick(&r.app));
	CHECK(!r.app.msg[0] && WF_WAIT_FOREVER == wf_app_timeout(&r.app));
	wf_copyresult_free(&res);
	wf_app_destroy(&r.app);
}

static void without_a_copy_engine_the_transfer_still_stops(void)
{
	static Rig r;

	world();
	rig_open(&r, false, false);
	ch(&r.app, 'j');
	ch(&r.app, 'y');
	ch(&r.app, 'h');
	prove_listing(&r.app, 7);
	ch(&r.app, 'p');
	CHECK(strstr(r.app.msg, "not available in this build"));
	ch(&r.app, 'z'); /* no engine: nothing to pause */
	CHECK(strstr(r.app.msg, "not available"));
	wf_app_destroy(&r.app);
}

/* When the program knows why there is no copy service, that is what the
 * paste says, in the language in use, and never an internal gate name. */
static void a_known_reason_is_what_a_refused_copy_says(void)
{
	static Rig r;
	static const WfMsg reasons[] = {MSG_COPY_OFF_STATE_FOLDER,
	        MSG_COPY_OFF_RECORD, MSG_COPY_SAFETY_BLOCKED};
	static const WfUiLang languages[] = {WF_UILANG_EN, WF_UILANG_JA};

	for (size_t l = 0; l < 2; l++) {
		for (size_t i = 0; i < 3; i++) {
			world();
			rig_open(&r, false, false);
			wf_i18n_set_language(languages[l]);
			wf_app_set_copy_off(&r.app, reasons[i]);
			ch(&r.app, 'j');
			ch(&r.app, 'y');
			ch(&r.app, 'h');
			prove_listing(&r.app, 7);
			ch(&r.app, 'p');
			CHECK(!strcmp(r.app.msg, wf_msg(reasons[i])));
			CHECK(!strstr(r.app.msg,
			              "not available in this build") &&
			        !strstr(r.app.msg, "P0-STORAGE"));
			CHECK(WF_XB_IDLE ==
			        r.app.xfer_busy); /* nothing was started */
			wf_app_destroy(&r.app);
		}
	}
	wf_i18n_set_language(WF_UILANG_EN);
}

/* Delete with no copy service (Recycle runs on it): the reason, in the words
 * of a delete, at once. No question is asked that could not delete anything,
 * and no gate name is shown. */
static void a_known_reason_is_what_a_refused_recycle_says(void)
{
	static Rig r;
	static const struct
	{
		WfMsg copy_reason, says;
	} cases[] = {{MSG_COPY_OFF_STATE_FOLDER, MSG_RECYCLE_OFF_STATE_FOLDER},
	        {MSG_COPY_OFF_RECORD, MSG_RECYCLE_OFF_RECORD},
	        {MSG_COPY_SAFETY_BLOCKED, MSG_RECYCLE_OFF_SAFETY}};
	static const WfUiLang languages[] = {WF_UILANG_EN, WF_UILANG_JA};

	for (size_t l = 0; l < 2; l++) {
		for (size_t i = 0; i < 3; i++) {
			world();
			rig_open(&r, false, false);
			wf_i18n_set_language(languages[l]);
			wf_app_set_copy_off(&r.app, cases[i].copy_reason);
			ch(&r.app, 'j');
			key(&r.app, WF_KEY_DELETE);
			CHECK(!strcmp(r.app.msg, wf_msg(cases[i].says)));
			CHECK(&WF_MODE_NORMAL ==
			        r.app.mode); /* nothing was asked */
			CHECK(!strstr(r.app.msg, "gates") &&
			        !strstr(r.app.msg, "BLOCKED"));
			wf_app_destroy(&r.app);
		}
	}
	/* no reason known at all: the safe one */
	world();
	rig_open(&r, false, false);
	ch(&r.app, 'j');
	key(&r.app, WF_KEY_DELETE);
	CHECK(!strcmp(r.app.msg, wf_msg(MSG_RECYCLE_OFF_SAFETY)));
	wf_app_destroy(&r.app);
	wf_i18n_set_language(WF_UILANG_EN);
}

static void an_unproven_destination_starts_nothing(void)
{
	static Rig r;

	world();
	rig_copy_ready(&r);
	r.app.cur.dir->identity.kind = WF_ID_NONE;
	ch(&r.app, 'p');
	CHECK(0 == r.copy.submits && 0 == r.app.copy_jobs);
	CHECK(strstr(r.app.msg, "identity is unproven"));
	wf_app_destroy(&r.app);
}

/* A FAT destination has only a weak identity: files may be copied onto it,
 * a folder may not (it is built under a hidden name and renamed), and a
 * destination with no identity at all still starts nothing. */
static void a_weak_destination_takes_files_but_not_a_folder(void)
{
	static Rig r;

	world();
	rig_copy_ready(&r);
	wf_weak_fill(&r.app.cur.dir->identity, 7, 5);
	ch(&r.app, 'p');
	CHECK(1 == r.copy.submits); /* the file goes */
	wf_app_destroy(&r.app);

	world();
	rig_open(&r, true, true);
	ch(&r.app, 'y'); /* Docs, a folder, is the first row */
	CHECK(1 == r.app.slot.count &&
	        (r.app.slot.src[0].snap.flags & WF_ENT_DIR));
	ch(&r.app, 'h');
	prove_listing(&r.app, 7);
	wf_weak_fill(&r.app.cur.dir->identity, 7, 5);
	ch(&r.app, 'p');
	CHECK(0 == r.copy.submits && 0 == r.app.copy_jobs);
	CHECK(strstr(r.app.msg, "FAT"));
	wf_app_destroy(&r.app);
}

static void a_full_queue_is_reported_and_nothing_is_counted(void)
{
	static Rig r;

	world();
	rig_copy_ready(&r);
	r.copy.answer = WF_E_RANGE;
	ch(&r.app, 'p');
	CHECK(1 == r.copy.submits && 0 == r.app.copy_jobs &&
	        strstr(r.app.msg, "BUSY"));
	wf_app_destroy(&r.app);
}

static void an_existing_name_asks_and_the_answer_is_not_remembered(void)
{
	static Rig r;

	world();
	fp_file(&P, L"C:\\Users\\file2.txt", 5, 50);
	rig_copy_ready(&r);
	ch(&r.app, 'p');
	CHECK(&WF_MODE_CONFIRM == r.app.mode &&
	        modal_says(&r.app, "Already there") &&
	        modal_says(&r.app, "file2.txt"));
	CHECK(0 == r.copy.submits);
	CHECK(strstr(r.app.confirm.armed_text, "[S]") &&
	        strstr(r.app.confirm.armed_text, "[K]") &&
	        strstr(r.app.confirm.armed_text, "[O]"));
	/* Esc: no consent given; the next p asks again */
	key(&r.app, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == r.app.mode && !r.app.slot.collisions_ok);
	ch(&r.app, 'p');
	CHECK(&WF_MODE_CONFIRM == r.app.mode && 0 == r.copy.submits);

	/* Skip: checked again with the consent, then the job runs as SKIP */
	{
		int before = r.xf.n_preflight;

		answer(&r.app, 's');
		CHECK(r.xf.n_preflight == before + 1);
	}
	CHECK(1 == r.copy.submits && WF_COLLIDE_SKIP == r.copy.collide);
	CHECK(!r.app.slot.collisions_ok &&
	        WF_COLLIDE_STOP == r.app.pending_collide); /* used up */
	{
		WfCopyResult res = result_of(WF_CJ_COPY, WF_CS_DONE);

		res.files_skipped = 1;
		wf_app_on_copy_result(&r.app, &res);
		CHECK(strstr(r.app.msg, "1 skipped"));
	}
	prove_listing(&r.app, 7); /* the reload shows the folder again */

	/* Keep both: the same check, then the job runs as RENAME */
	ch(&r.app, 'p');
	CHECK(&WF_MODE_CONFIRM == r.app.mode);
	answer(&r.app, 'k');
	CHECK(2 == r.copy.submits && WF_COLLIDE_RENAME == r.copy.collide);
	CHECK(!r.app.slot.collisions_ok &&
	        WF_COLLIDE_STOP == r.app.pending_collide);
	{
		WfCopyResult res = result_of(WF_CJ_COPY, WF_CS_DONE);

		wf_app_on_copy_result(&r.app, &res);
	}
	prove_listing(&r.app, 8);

	/* Overwrite: the same check, then the job runs as OVERWRITE */
	ch(&r.app, 'p');
	CHECK(&WF_MODE_CONFIRM == r.app.mode);
	answer(&r.app, 'o');
	CHECK(3 == r.copy.submits && WF_COLLIDE_OVERWRITE == r.copy.collide);
	CHECK(!r.app.slot.collisions_ok &&
	        WF_COLLIDE_STOP == r.app.pending_collide);
	{
		WfCopyResult res = result_of(WF_CJ_COPY, WF_CS_DONE);

		wf_app_on_copy_result(&r.app, &res);
	}
	prove_listing(&r.app, 9);

	/* none of the three is remembered: the next p asks again, and N asks
	 * for nothing to be done */
	ch(&r.app, 'p');
	CHECK(&WF_MODE_CONFIRM == r.app.mode && 3 == r.copy.submits);
	answer(&r.app, 'n');
	CHECK(&WF_MODE_NORMAL == r.app.mode && 3 == r.copy.submits);
	wf_app_destroy(&r.app);
}

static void collisions_still_refuse_a_move_and_a_missing_engine(void)
{
	static Rig r;

	world();
	fp_file(&P, L"C:\\Users\\file2.txt", 5, 50);
	rig_open(&r, true, true);
	ch(&r.app, 'j');
	ch(&r.app, 'x');
	ch(&r.app, 'h');
	prove_listing(&r.app, 7);
	ch(&r.app, 'p');
	CHECK(&WF_MODE_NORMAL == r.app.mode && 0 == r.spy.calls &&
	        strstr(r.app.msg, "already exists"));
	wf_app_destroy(&r.app);

	rig_open(&r, false, false);
	ch(&r.app, 'j');
	ch(&r.app, 'y');
	ch(&r.app, 'h');
	prove_listing(&r.app, 7);
	ch(&r.app, 'p');
	CHECK(&WF_MODE_NORMAL == r.app.mode &&
	        strstr(r.app.msg, "already exists"));
	wf_app_destroy(&r.app);
}

static void make_second_volume(void)
{
	/* the source folder lives on volume B, the destination on A */
	P.nvols = 2;
	wcscpy_s(P.vols[1].prefix, 64, L"C:\\Users\\dev");
	P.vols[1].info = P.vols[0].info;
	P.vols[1].info.serial = 0xBBBB;
	for (size_t i = 0; i < P.count; i++) {
		if (!_wcsnicmp(P.n[i].path, L"C:\\Users\\dev", 12)) {
			P.n[i].vol = 2;
		}
	}
}

static void a_move_across_volumes_offers_move_or_copy(void)
{
	static Rig r;

	world();
	make_second_volume();
	rig_open(&r, true, true);
	ch(&r.app, 'j');
	ch(&r.app, 'x');
	ch(&r.app, 'h');
	prove_listing(&r.app, 7);
	ch(&r.app, 'p');
	CHECK(&WF_MODE_CONFIRM == r.app.mode &&
	        modal_says(&r.app, "different") && 0 == r.copy.submits);
	/* Esc leaves it a move; a later plain p asks again */
	key(&r.app, WF_KEY_ESC);
	CHECK(WF_XFER_MOVE == r.app.slot.mode && !r.app.pending_cross);

	ch(&r.app, 'p');
	answer(&r.app, 'm');
	/* checked again as a copy, then it runs as a move across volumes */
	CHECK(1 == r.copy.submits && WF_CJ_MOVE_ACROSS == r.copy.kind);
	CHECK(!r.app.pending_cross && WF_XFER_MOVE == r.app.slot.mode);
	wf_app_destroy(&r.app);

	/* Copy only */
	world();
	make_second_volume();
	rig_open(&r, true, true);
	ch(&r.app, 'j');
	ch(&r.app, 'x');
	ch(&r.app, 'h');
	prove_listing(&r.app, 7);
	ch(&r.app, 'p');
	answer(&r.app, 'c');
	CHECK(1 == r.copy.submits && WF_CJ_COPY == r.copy.kind &&
	        WF_XFER_COPY == r.app.slot.mode);
	wf_app_destroy(&r.app);

	/* without an engine the old offer remains */
	world();
	make_second_volume();
	rig_open(&r, false, false);
	ch(&r.app, 'j');
	ch(&r.app, 'x');
	ch(&r.app, 'h');
	prove_listing(&r.app, 7);
	ch(&r.app, 'p');
	CHECK(&WF_MODE_CONFIRM == r.app.mode &&
	        modal_says(&r.app, "Source and"));
	wf_app_destroy(&r.app);
}

static void delete_asks_and_sends_the_selection_to_the_bin(void)
{
	static Rig r;

	world();
	rig_open(&r, true, true);
	ch(&r.app, 'j'); /* file2.txt */
	ch(&r.app, ' ');
	size_t selected = wf_sel_count(&r.app.sel, r.app.cwd);
	CHECK(selected == 1);
	key(&r.app, WF_KEY_DELETE);
	/* it asks first, and says where the files go */
	CHECK(&WF_MODE_CONFIRM == r.app.mode && modal_says(&r.app, "RECYCLE") &&
	        modal_says(&r.app, "Recycle Bin"));
	CHECK(0 == r.copy.submits && 1 == r.app.recycle_slot.count);
	/* Esc does nothing and keeps the selection */
	key(&r.app, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == r.app.mode && 0 == r.copy.submits);
	CHECK(wf_sel_count(&r.app.sel, r.app.cwd) == selected);
	/* yes: one recycle job for the selection */
	key(&r.app, WF_KEY_DELETE);
	answer(&r.app, 'y');
	CHECK(1 == r.copy.submits && WF_CJ_RECYCLE == r.copy.kind);
	{
		WfCopyResult res = result_of(WF_CJ_RECYCLE, WF_CS_DONE);

		res.sources_recycled = 1;
		wf_app_on_copy_result(&r.app, &res);
		CHECK(strstr(r.app.msg, "RECYCLE done") &&
		        strstr(r.app.msg, "restore from there"));
	}
	wf_app_destroy(&r.app);

	/* an item whose identity was never proven is not recycled */
	world();
	rig_open(&r, true, true);
	r.app.cur.dir->e[1].identity.kind = WF_ID_NONE;
	ch(&r.app, 'j');
	key(&r.app, WF_KEY_DELETE);
	CHECK(&WF_MODE_NORMAL == r.app.mode && strstr(r.app.msg, "unproven") &&
	        0 == r.copy.submits);
	wf_app_destroy(&r.app);

	/* with no engine Delete keeps stopping at the gate */
	world();
	rig_open(&r, false, false);
	key(&r.app, WF_KEY_DELETE);
	CHECK(r.copy.submits == 0);
	wf_app_destroy(&r.app);
}

static void problems_open_a_report_and_any_key_closes_it(void)
{
	static Rig r;
	WfCopyResult res = result_of(WF_CJ_COPY, WF_CS_DONE);

	world();
	rig_copy_ready(&r);
	res.files_copied = 2;
	res.files_failed = 1;
	res.links_skipped = 1;
	res.failure_total = 14;
	for (int i = 0; i < WF_CJ_FAILURES; i++) {
		wcscpy_s(res.failures[i].name, 96, L"sub\\bad.bin");
		res.failures[i].kind = WF_CF_VERIFY_FAILED;
		res.failures[i].error = 5;
	}
	wf_app_on_copy_result(&r.app, &res);
	CHECK(&WF_MODE_REPORT == r.app.mode && r.app.report_n > 3);
	CHECK(strstr(r.app.report[0], "finished with problems"));
	{
		bool saw_fail = false, saw_more = false, saw_loss = false;

		for (int i = 0; i < r.app.report_n; i++) {
			saw_fail |= strstr(r.app.report[i],
			                    "did not read back") != NULL;
			saw_more |= strstr(r.app.report[i], " more") != NULL;
			saw_loss |= strstr(r.app.report[i], "Not preserved") !=
			        NULL;
		}
		CHECK(saw_fail && saw_more && saw_loss);
	}
	key(&r.app, WF_KEY_ENTER);
	CHECK(&WF_MODE_NORMAL == r.app.mode);

	/* a cancelled job says so, and says nothing was left behind */
	res = result_of(WF_CJ_COPY, WF_CS_CANCELLED);
	wf_app_on_copy_result(&r.app, &res);
	CHECK(&WF_MODE_REPORT == r.app.mode && strstr(r.app.msg, "cancelled") &&
	        strstr(r.app.msg, "no temporary left"));
	wf_app_destroy(&r.app);
}

/* DR-DRAFT-019 F9: a checked copy in which a read-back differed says how many files
 * were verified and how many did not read back the same, and the job log keeps it. */
static void a_failed_read_back_says_how_many_were_verified(void)
{
	static Rig r;
	WfCopyResult res = result_of(WF_CJ_COPY, WF_CS_DONE);

	world();
	rig_copy_ready(&r);
	res.files_copied = 2;
	res.files_failed = 1;
	res.failure_total = 1;
	wcscpy_s(res.failures[0].name, 96, L"bad.bin");
	res.failures[0].kind = WF_CF_VERIFY_FAILED;
	res.failures[0].error = 5;
	wf_app_on_copy_result(&r.app, &res);
	CHECK(&WF_MODE_REPORT == r.app.mode);
	CHECK(strstr(r.app.report[0], "COPY finished with problems"));
	CHECK(strstr(r.app.report[0], "2 verified"));
	CHECK(strstr(r.app.report[0], "1 did not read back the same"));
	/* the log of jobs keeps the same words */
	CHECK(r.app.jobs.n >= 1);
	CHECK(strstr(r.app.jobs.log[r.app.jobs.n - 1].head, "2 verified"));
	key(&r.app, WF_KEY_ENTER);

	/* another kind of failure keeps the ordinary words */
	res = result_of(WF_CJ_COPY, WF_CS_DONE);
	res.files_copied = 2;
	res.files_failed = 1;
	res.failure_total = 1;
	wcscpy_s(res.failures[0].name, 96, L"locked.bin");
	res.failures[0].kind = WF_CF_COMMIT_FAILED;
	res.failures[0].error = 32;
	wf_app_on_copy_result(&r.app, &res);
	CHECK(strstr(r.app.report[0], "2 ok, 1 failed"));
	CHECK(!strstr(r.app.report[0], "verified"));
	key(&r.app, WF_KEY_ENTER);

	/* a copy that was not told to check cannot have failed a read-back */
	res = result_of(WF_CJ_COPY, WF_CS_DONE);
	res.verify = WF_VERIFY_NONE;
	res.files_copied = 2;
	res.files_failed = 1;
	res.failure_total = 1;
	wcscpy_s(res.failures[0].name, 96, L"x.bin");
	res.failures[0].kind = WF_CF_VERIFY_FAILED;
	wf_app_on_copy_result(&r.app, &res);
	CHECK(strstr(r.app.report[0], "2 ok, 1 failed"));
	key(&r.app, WF_KEY_ENTER);
	wf_app_destroy(&r.app);
}

static void an_unknown_recycle_result_does_not_claim_the_source_survived(void)
{
	static Rig r;
	world();
	rig_copy_ready(&r);
	WfCopyResult result = result_of(WF_CJ_RECYCLE, WF_CS_DONE);
	result.sources_kept = result.failure_total = 1;
	result.failures[0].kind = WF_CF_NOT_REMOVED;
	for (int lang = WF_UILANG_EN; lang <= WF_UILANG_JA; lang++) {
		wf_i18n_set_language((WfUiLang)lang);
		wf_app_on_copy_result(&r.app, &result);
		CHECK(&WF_MODE_REPORT == r.app.mode);
		bool found = false, guidance = false;
		for (int i = 0; i < r.app.report_n; i++) {
			found |=
			        strstr(r.app.report[i],
			                wf_msg(MSG_JOB_CF_NOT_REMOVED)) != NULL;
		}
		CHECK(found);
		for (int i = 0; i < r.app.report_n; i++) {
			guidance |= !strcmp(r.app.report[i],
			        wf_msg(MSG_JOB_RES_CHECK_LOCATIONS));
		}
		CHECK(guidance);
		CHECK(!strstr(wf_msg(MSG_JOB_CF_NOT_REMOVED), "was kept"));
		key(&r.app, WF_KEY_ESC);
	}
	wf_i18n_set_language(WF_UILANG_EN);
	wf_app_destroy(&r.app);
}

static void pause_resume_and_cancel_reach_the_engine(void)
{
	static Rig r;

	world();
	rig_copy_ready(&r);
	ch(&r.app, 'z');
	CHECK(1 == r.copy.pause_calls && r.copy.paused && r.app.copy_paused);
	{
		WfBuf s;

		wf_buf_init(&s);
		wf_app_copy_status(&r.app, &s, 200);
		CHECK(strstr(wf_buf_cstr(&s), "PAUSED"));
		wf_buf_free(&s);
	}
	ch(&r.app, 'z');
	CHECK(2 == r.copy.pause_calls && !r.copy.paused);
	ch(&r.app, 'C');
	CHECK(0 == r.copy.cancel_calls &&
	        strstr(r.app.msg, "Nothing is running"));
	ch(&r.app, 'p');
	ch(&r.app, 'C');
	CHECK(1 == r.copy.cancel_calls && r.copy.cancel_all);
	/* a pause does not outlive the job it was meant for */
	ch(&r.app, 'z');
	CHECK(r.copy.paused);
	{
		WfCopyResult res = result_of(WF_CJ_COPY, WF_CS_CANCELLED);

		wf_app_on_copy_result(&r.app, &res);
		CHECK(!r.copy.paused && !r.app.copy_paused);
	}
	wf_app_destroy(&r.app);
}

static void undoing_a_copy_asks_and_sends_the_new_copies_to_the_bin(void)
{
	static Rig r;
	WfCopyResult res = result_of(WF_CJ_COPY, WF_CS_DONE);

	world();
	rig_copy_ready(&r);
	ch(&r.app, 'u');
	CHECK(strstr(r.app.msg, "Nothing to undo"));
	res.files_copied = 1;
	add_created(&res, L"C:\\Users\\file2.txt", 9);
	wf_app_on_copy_result(&r.app, &res);
	wf_copyresult_free(&res);
	CHECK(1 == r.app.undo_n &&
	        WF_UNDO_RECYCLE_CREATED == r.app.undo[0].kind);

	ch(&r.app, 'u');
	/* it asks first, and nothing is sent until it is answered */
	CHECK(&WF_MODE_CONFIRM == r.app.mode && 0 == r.copy.submits);
	key(&r.app, WF_KEY_ESC);
	CHECK(1 == r.app.undo_n); /* still there */

	ch(&r.app, 'u');
	answer(&r.app, 'y');
	CHECK(1 == r.copy.submits && WF_CJ_RECYCLE == r.copy.kind);

	/* nothing is undone while a job is still running */
	r.app.copy_jobs = 1;
	ch(&r.app, 'u');
	CHECK(strstr(r.app.msg, "BUSY") ||
	        strstr(r.app.msg, "Nothing to undo") ||
	        &WF_MODE_CONFIRM == r.app.mode);
	wf_app_destroy(&r.app);
}

/* Two copies were made; the Undo recycles both, one of them cannot be moved.
 * The entry keeps only that one, a result of some other job leaves it alone,
 * and a later push cannot make the running job update the wrong entry. */
static void a_partly_done_copy_undo_keeps_only_what_is_left(void)
{
	static Rig r;
	WfCopyResult res = result_of(WF_CJ_COPY, WF_CS_DONE);
	WfCopyResult rec;

	world();
	rig_copy_ready(&r);
	res.files_copied = 2;
	res.created = calloc(2, sizeof *res.created);
	CHECK(NULL != res.created);
	res.created[0].path = _wcsdup(L"C:\\Users\\file2.txt");
	res.created[0].id.kind = WF_ID_STRONG;
	res.created[0].id.volume = 7;
	res.created[0].id.id[0] = 9;
	res.created[1].path = _wcsdup(L"C:\\Users\\file3.txt");
	res.created[1].id.kind = WF_ID_STRONG;
	res.created[1].id.volume = 7;
	res.created[1].id.id[0] = 10;
	res.created_n = 2;
	wf_app_on_copy_result(&r.app, &res);
	wf_copyresult_free(&res);
	CHECK(1 == r.app.undo_n && 2 == r.app.undo[0].n);

	ch(&r.app, 'u');
	answer(&r.app, 'y');
	CHECK(1 == r.copy.submits && 1 == r.app.recycling_n);

	/* another job's result does not touch the entry */
	rec = result_of(WF_CJ_RECYCLE, WF_CS_DONE);
	rec.job = 99;
	rec.sources_recycled = 2;
	wf_app_on_copy_result(&r.app, &rec);
	CHECK(1 == r.app.undo_n && 2 == r.app.undo[0].n &&
	        1 == r.app.recycling_n);

	/* the job's own: the first was taken, the second was not (a long name
	 * cannot confuse this: the job says which source, not by name) */
	rec = result_of(WF_CJ_RECYCLE, WF_CS_CANCELLED);
	rec.job = 1;
	rec.item_done = calloc(2, 1);
	CHECK(NULL != rec.item_done);
	rec.item_n = 2;
	rec.item_done[0] = 1;
	wf_app_on_copy_result(&r.app, &rec);
	CHECK(0 == r.app.recycling_n && 1 == r.app.undo_n &&
	        1 == r.app.undo[0].n &&
	        !wcscmp(r.app.undo[0].item[0].name, L"file3.txt"));
	wf_copyresult_free(&rec);

	/* a second Undo of what is left, and now everything is taken */
	key(&r.app, WF_KEY_ESC); /* the report of the first */
	ch(&r.app, 'u');
	answer(&r.app, 'y');
	CHECK(2 == r.copy.submits && 1 == r.app.recycling_n);
	rec = result_of(WF_CJ_RECYCLE, WF_CS_DONE);
	rec.job = 2;
	rec.item_done = calloc(1, 1);
	CHECK(NULL != rec.item_done);
	rec.item_n = 1;
	rec.item_done[0] = 1;
	wf_app_on_copy_result(&r.app, &rec);
	CHECK(0 == r.app.undo_n && 0 == r.app.recycling_n);
	wf_copyresult_free(&rec);
	wf_app_destroy(&r.app);
}

/* A copy that merged made items in several folders. Undo keeps one entry for
 * each folder, with that folder's identity, so the entry can prove its folder
 * and every item in it; an item whose folder is not named falls back to the
 * destination's identity and one without an identity is not offered at all. */
static void a_merge_result_gives_one_undo_entry_for_each_folder(void)
{
	static Rig r;
	WfCopyResult res = result_of(WF_CJ_COPY, WF_CS_DONE);

	world();
	rig_copy_ready(&r);
	res.dest_id.kind = WF_ID_STRONG;
	res.dest_id.volume = 7;
	res.dest_id.id[0] = 1;
	res.created = calloc(3, sizeof *res.created);
	CHECK(NULL != res.created);
	for (int k = 0; k < 3; k++) {
		static const wchar_t *const path[] = {
		        L"C:\\Users\\assets\\b.png",
		        L"C:\\Users\\assets\\sub\\x.txt",
		        L"C:\\Users\\assets\\z.png"};

		res.created[k].path = _wcsdup(path[k]);
		res.created[k].id.kind = WF_ID_STRONG;
		res.created[k].id.volume = 7;
		res.created[k].id.id[0] = (uint8_t)(20 + k);
		res.created[k].parent.kind = WF_ID_STRONG;
		res.created[k].parent.volume = 7;
		res.created[k].parent.id[0] = 1 == k ? 3 : 2;
	}
	res.created_n = 3;
	wf_app_on_copy_result(&r.app, &res);
	wf_copyresult_free(&res);
	CHECK(2 == r.app.undo_n);
	for (size_t k = 0; k < r.app.undo_n; k++) {
		const WfUndoEntry *u = &r.app.undo[k];

		if (!wcscmp(u->folder, L"C:\\Users\\assets")) {
			CHECK(2 == u->n && 2 == u->folder_id.id[0]);
		} else {
			CHECK(!wcscmp(u->folder, L"C:\\Users\\assets\\sub") &&
			        1 == u->n && 3 == u->folder_id.id[0] &&
			        !wcscmp(u->item[0].name, L"x.txt"));
		}
	}

	/* one row in the history for the whole copy: undone in part it is still
	 * there to undo, and it is "undone" only when the last entry is */
	{
		uint32_t first = r.app.undo[0].uid, second = r.app.undo[1].uid;

		CHECK(first == r.app.undo[0].group &&
		        first == r.app.undo[1].group);
		CHECK(1 == r.app.jobs.n &&
		        WF_HU_KEPT == r.app.jobs.log[0].undo &&
		        first == r.app.jobs.log[0].undo_uid);
		wf_app_undo_retire(&r.app, second);
		CHECK(WF_HU_KEPT == r.app.jobs.log[0].undo);
		wf_app_undo_retire(&r.app, first);
		CHECK(WF_HU_DONE == r.app.jobs.log[0].undo);
	}
	wf_app_destroy(&r.app);

	/* five folders are all kept; more than the stack can spare are not kept at
	 * all (never five of nine), and the history says what Undo lacks */
	for (int folders = 5; folders <= 9; folders += 4) {
		WfCopyResult big = result_of(WF_CJ_COPY, WF_CS_DONE);

		world();
		rig_copy_ready(&r);
		big.dest_id.kind = WF_ID_STRONG;
		big.dest_id.volume = 7;
		big.dest_id.id[0] = 1;
		big.created = calloc((size_t)folders, sizeof *big.created);
		CHECK(NULL != big.created);
		for (int k = 0; k < folders; k++) {
			wchar_t p[64];

			swprintf(p, 64, L"C:\\Users\\d%d\\f.txt", k);
			big.created[k].path = _wcsdup(p);
			big.created[k].id.kind = WF_ID_STRONG;
			big.created[k].id.volume = 7;
			big.created[k].id.id[0] = (uint8_t)(40 + k);
			big.created[k].parent.kind = WF_ID_STRONG;
			big.created[k].parent.volume = 7;
			big.created[k].parent.id[0] = (uint8_t)(60 + k);
		}
		big.created_n = (size_t)folders;
		wf_app_on_copy_result(&r.app, &big);
		wf_copyresult_free(&big);
		CHECK(5 == folders ? 5 == r.app.undo_n &&
		                        WF_HU_KEPT == r.app.jobs.log[0].undo
		                   : 0 == r.app.undo_n &&
		                        WF_HU_NONE == r.app.jobs.log[0].undo);
		wf_app_destroy(&r.app);
	}
}

/* One of the entries of a copy that merged is pushed out of the full stack; the
 * other is then undone. Part of the copy cannot be undone any more, so its row
 * ends as expired, never as "undone". */
static void a_row_whose_entry_was_pushed_out_never_says_undone(void)
{
	static Rig r;
	WfCopyResult res = result_of(WF_CJ_COPY, WF_CS_DONE);
	uint32_t second;
	size_t done = 0, expired = 0;

	world();
	rig_copy_ready(&r);
	res.dest_id.kind = WF_ID_STRONG;
	res.dest_id.volume = 7;
	res.dest_id.id[0] = 1;
	res.created = calloc(2, sizeof *res.created);
	CHECK(NULL != res.created);
	for (int k = 0; k < 2; k++) {
		static const wchar_t *const path[] = {
		        L"C:\\Users\\m\\a.png", L"C:\\Users\\m\\sub\\b.png"};

		res.created[k].path = _wcsdup(path[k]);
		res.created[k].id.kind = WF_ID_STRONG;
		res.created[k].id.volume = 7;
		res.created[k].id.id[0] = (uint8_t)(20 + k);
		res.created[k].parent.kind = WF_ID_STRONG;
		res.created[k].parent.volume = 7;
		res.created[k].parent.id[0] = (uint8_t)(2 + k);
	}
	res.created_n = 2;
	wf_app_on_copy_result(&r.app, &res);
	wf_copyresult_free(&res);
	CHECK(2 == r.app.undo_n);
	second = r.app.undo[1].uid;

	/* fifteen more copies fill the stack and push the first entry out */
	for (int k = 0; k < WF_UNDO_MAX - 1; k++) {
		WfCopyResult more = result_of(WF_CJ_COPY, WF_CS_DONE);
		wchar_t p[64];

		swprintf(p, 64, L"C:\\Users\\o%d\\f.txt", k);
		more.job = 100 + (uint32_t)k;
		more.dest_id.kind = WF_ID_STRONG;
		more.dest_id.volume = 7;
		more.dest_id.id[0] = 1;
		add_created(&more, p, (uint8_t)(100 + k));
		more.created[0].parent.kind = WF_ID_STRONG;
		more.created[0].parent.volume = 7;
		more.created[0].parent.id[0] = 9;
		wf_app_on_copy_result(&r.app, &more);
		wf_copyresult_free(&more);
	}
	CHECK(WF_UNDO_MAX == r.app.undo_n);
	wf_app_undo_retire(&r.app, second);
	for (size_t k = 0; k < r.app.jobs.n; k++) {
		done += WF_HU_DONE == r.app.jobs.log[k].undo;
		expired += WF_HU_EXPIRED == r.app.jobs.log[k].undo;
	}
	CHECK(0 == done && 1 == expired);
	wf_app_destroy(&r.app);
}

/* y inside a ZIP picks entries; p in a real folder extracts them: one EXTRACT
 * job with the archive, the paths in it and the folder on screen, never a
 * transfer slot. x inside the archive stays refused. */
static void items_picked_in_a_zip_are_extracted_by_p_in_a_folder(void)
{
	static Rig r;

	world();
	rig_open(&r, true, true);
	/* the listing stands in for the archive's folder "d" */
	free(r.app.cwd);
	r.app.cwd = _wcsdup(L"C:\\z.zip|\\d");
	CHECK(NULL != r.app.cwd);
	ch(&r.app, 'x');
	CHECK(0 == r.app.arc_pick.n && WF_XFER_NONE == r.app.slot.mode);
	ch(&r.app, 'j'); /* file2.txt */
	ch(&r.app, 'y');
	CHECK(1 == r.app.arc_pick.n && WF_XFER_NONE == r.app.slot.mode);
	CHECK(!wcscmp(r.app.arc_pick.archive, L"C:\\z.zip"));
	CHECK(!wcscmp(r.app.arc_pick.inner[0], L"d\\file2.txt"));
	CHECK(!wcscmp(r.app.arc_pick.leaf[0], L"file2.txt"));
	/* p inside the archive: it is read only */
	ch(&r.app, 'p');
	CHECK(0 == r.copy.submits && strstr(r.app.msg, "Read-only"));
	/* into a real folder */
	free(r.app.cwd);
	r.app.cwd = _wcsdup(L"C:\\Users");
	prove_listing(&r.app, 7);
	ch(&r.app, 'p');
	/* the name is on screen already: the question, one answer for the job */
	CHECK(0 == r.copy.submits && &WF_MODE_CONFIRM == r.app.mode &&
	        modal_says(&r.app, "file2.txt"));
	answer(&r.app, 's');
	CHECK(1 == r.copy.submits && WF_CJ_EXTRACT == r.copy.kind &&
	        WF_COLLIDE_SKIP == r.copy.collide && 0 == r.copy.count &&
	        1 == r.copy.n_inner && !wcscmp(r.copy.archive, L"C:\\z.zip") &&
	        !wcscmp(r.copy.inner0, L"d\\file2.txt") &&
	        !wcscmp(r.copy.dest, L"C:\\Users"));
	/* a newer pick of real files replaces the pick from the archive */
	wf_app_destroy(&r.app);
	rig_open(&r, true, true);
	r.app.arc_pick.archive = _wcsdup(L"C:\\z.zip");
	r.app.arc_pick.inner = calloc(1, sizeof *r.app.arc_pick.inner);
	r.app.arc_pick.leaf = calloc(1, sizeof *r.app.arc_pick.leaf);
	r.app.arc_pick.inner[0] = _wcsdup(L"a");
	r.app.arc_pick.leaf[0] = _wcsdup(L"a");
	r.app.arc_pick.n = 1;
	ch(&r.app, 'j');
	ch(&r.app, 'y');
	CHECK(WF_XFER_COPY == r.app.slot.mode);
	CHECK(0 == r.app.arc_pick.n); /* the capture answered at once */
	wf_app_destroy(&r.app);
}

/* What came out of an archive says so: "EXTRACT done" for a clean one, and a
 * line with how many entries were not taken when some were. */
static void an_extraction_says_it_was_one(void)
{
	static Rig r;
	WfCopyResult res = result_of(WF_CJ_COPY, WF_CS_DONE);
	bool line = false;

	world();
	rig_copy_ready(&r);
	res.extracted = true;
	res.files_copied = 3;
	res.bytes = 30;
	wf_app_on_copy_result(&r.app, &res);
	CHECK(strstr(r.app.msg, "EXTRACT done: 3 file(s)") &&
	        !strstr(r.app.msg, "COPY done"));
	/* with entries left out: a window, and the count in it */
	res.failure_total = 2;
	res.files_failed = 2;
	res.failures[0].kind = WF_CF_ARCHIVE_NAME;
	wcscpy_s(res.failures[0].name, 96, L"nul.txt");
	res.failures[1].kind = WF_CF_ARCHIVE_COLLISION;
	wcscpy_s(res.failures[1].name, 96, L"Doc.TXT");
	wf_app_on_copy_result(&r.app, &res);
	for (int i = 0; i < r.app.report_n; i++) {
		line = line || strstr(r.app.report[i], "Entries not taken") ||
		        strstr(r.app.report[i], "not taken (listed below): 2");
	}
	CHECK(line);
	CHECK(0 ==
	        strcmp(wf_app_copyfail_text(WF_CF_ARCHIVE_NAME),
	                wf_msg(MSG_JOB_CF_ARCHIVE_NAME)));
	wf_copyresult_free(&res);
	wf_app_destroy(&r.app);
}

/* Many picks against the folder on screen are looked up in its names sorted
 * once; the answer is the same as comparing one by one, case ignored: a name
 * that is there asks the question, and when none is there it does not. */
static void many_picks_find_a_name_that_is_there_in_any_case(void)
{
	static Rig r;
	enum
	{
		N = 1500
	};

	for (int round = 0; round < 2; round++) {
		world();
		rig_open(&r, true, true);
		r.app.arc_pick.archive = _wcsdup(L"C:\\z.zip");
		r.app.arc_pick.inner = calloc(N, sizeof *r.app.arc_pick.inner);
		r.app.arc_pick.leaf = calloc(N, sizeof *r.app.arc_pick.leaf);
		CHECK(NULL != r.app.arc_pick.inner &&
		        NULL != r.app.arc_pick.leaf);
		for (int i = 0; i < N; i++) {
			wchar_t name[32];

			swprintf(name, 32, L"x%d", i);
			if (1 == round && 777 == i) {
				wcscpy_s(name, 32,
				        L"FILE2.TXT"); /* is there, in other case */
			}
			r.app.arc_pick.inner[i] = _wcsdup(name);
			r.app.arc_pick.leaf[i] = _wcsdup(name);
			r.app.arc_pick.n = (size_t)i + 1;
		}
		ch(&r.app, 'p');
		if (round) {
			CHECK(0 == r.copy.submits &&
			        &WF_MODE_CONFIRM == r.app.mode &&
			        modal_says(&r.app, "FILE2.TXT"));
		} else {
			CHECK(1 == r.copy.submits &&
			        WF_CJ_EXTRACT == r.copy.kind &&
			        WF_COLLIDE_STOP == r.copy.collide &&
			        (size_t)N == r.copy.n_inner);
		}
		wf_app_destroy(&r.app);
	}
}

/* The text of one row of a frame. */
static void backup_row(const WfFrame *f, int y, char *out, size_t cap)
{
	size_t n = 0;

	for (int x = 0; x < f->cols; x++) {
		const char *u = f->cells[y * f->cols + x].utf8;

		for (; *u && n + 1 < cap; u++) {
			out[n++] = *u;
		}
	}
	out[n] = '\0';
}

/* B lists the backups that sit beside the file under the cursor
 * (<name>~ to <name>~9), and nothing else. Delete in it goes through the
 * question and the job Delete uses. */
static void the_backups_window_lists_what_sits_beside_the_file(void)
{
	static Rig r;

	world();
	rig_open(&r, true, true);
	/* the backups, in the listing the app shows */
	CHECK(WF_OK == wf_dir_add(r.app.cur.dir, L"file2.txt~", 10, 0, 18, 18));
	CHECK(WF_OK ==
	        wf_dir_add(r.app.cur.dir, L"file2.txt~3", 11, 0, 17, 17));
	CHECK(WF_OK ==
	        wf_dir_add(r.app.cur.dir, L"file2.txt~10", 12, 0, 16, 16));
	for (size_t i = 0; i < r.app.cur.dir->n; i++) {
		WfEntry *e = &r.app.cur.dir->e[i];

		if (!(e->identity.kind ==
		            WF_ID_STRONG)) { /* as a real listing proves them */
			e->identity.kind = WF_ID_STRONG;
			e->identity.volume = 7;
			e->identity.id[0] = (uint8_t)(40 + i);
		}
	}
	CHECK(WF_OK == wf_view_resort(&r.app.cur, WF_SORT_NAME, false, true));
	ch(&r.app, 'B'); /* the cursor is on a folder first */
	CHECK(&WF_MODE_NORMAL == r.app.mode && strstr(r.app.msg, "file"));
	r.app.msg[0] = 0;
	for (size_t i = 0; i < r.app.cur.nvis;
	        i++) { /* file10.txt: no backups */
		if (!wcscmp(r.app.cur.dir->e[r.app.cur.vis[i]].name,
		            L"file10.txt")) {
			wf_view_move(&r.app.cur,
			        (long)i - (long)r.app.cur.cursor, 10);
		}
	}
	ch(&r.app, 'B');
	CHECK(&WF_MODE_NORMAL == r.app.mode && strstr(r.app.msg, "No backups"));
	{
		/* put the cursor on file2.txt by name */
		for (size_t i = 0; i < r.app.cur.nvis; i++) {
			if (!wcscmp(r.app.cur.dir->e[r.app.cur.vis[i]].name,
			            L"file2.txt")) {
				wf_view_move(&r.app.cur,
				        (long)i - (long)r.app.cur.cursor, 10);
			}
		}
	}
	ch(&r.app, 'B');
	CHECK(&WF_MODE_BACKUPS == r.app.mode && 2 == r.app.backups.n);
	CHECK(0 == wcscmp(r.app.backups.names[0], L"file2.txt~") &&
	        0 == wcscmp(r.app.backups.names[1], L"file2.txt~3"));
	key(&r.app, WF_KEY_DOWN);
	CHECK(1 == r.app.backups.pick);
	key(&r.app, WF_KEY_DOWN); /* wraps */
	CHECK(0 == r.app.backups.pick);
	{
		/* a row shows the size and the time of the entry of its name, wherever
		 * that is in the list now: the place noted when the window was opened is
		 * a hint that the name must confirm */
		WfFrame *f = wf_frame_new(100, 20, &WF_STYLE_BASE);
		char before[2][512], after[2][512];
		size_t a = r.app.backups.at[0], b = r.app.backups.at[1];
		WfEntry t;

		CHECK(0 ==
		                wcscmp(r.app.cur.dir->e[a].name,
		                        r.app.backups.names[0]) &&
		        0 ==
		                wcscmp(r.app.cur.dir->e[b].name,
		                        r.app.backups.names[1]));
		wf_app_draw(&r.app, f);
		backup_row(f, 2, before[0], sizeof before[0]);
		backup_row(f, 3, before[1], sizeof before[1]);
		t = r.app.cur.dir->e[a];
		r.app.cur.dir->e[a] = r.app.cur.dir->e[b];
		r.app.cur.dir->e[b] =
		        t; /* the list read anew in another order */
		wf_app_draw(&r.app, f);
		backup_row(f, 2, after[0], sizeof after[0]);
		backup_row(f, 3, after[1], sizeof after[1]);
		CHECK(!strcmp(before[0], after[0]) &&
		        !strcmp(before[1], after[1]));
		CHECK(strcmp(before[0], before[1]) != 0);
		t = r.app.cur.dir->e[a];
		r.app.cur.dir->e[a] = r.app.cur.dir->e[b];
		r.app.cur.dir->e[b] = t;
		wf_frame_free(f);
	}
	key(&r.app, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == r.app.mode);

	/* Delete on a row asks the question Delete asks, about that file */
	ch(&r.app, 'B');
	key(&r.app, WF_KEY_DOWN);
	key(&r.app, WF_KEY_DELETE);
	CHECK(&WF_MODE_CONFIRM == r.app.mode);
	CHECK(0 == wcscmp(wf_view_current(&r.app.cur)->name, L"file2.txt~3"));
	wf_app_destroy(&r.app);
}

/* Every failure of a job stays readable with its full path after the report
 * window is gone, for the last jobs, and the log window scrolls. */
static void the_job_log_keeps_every_failure_with_its_path(void)
{
	static Rig r;
	WfCopyResult res = result_of(WF_CJ_COPY, WF_CS_DONE);
	wchar_t longname[300];

	world();
	rig_copy_ready(&r);
	CHECK(0 == r.app.jobs.n);
	ch(&r.app, 'J');
	CHECK(&WF_MODE_JOBLOG == r.app.mode); /* empty, and says so */
	key(&r.app, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == r.app.mode);

	for (size_t i = 0; i < 299; i++) {
		longname[i] = L'a';
	}
	longname[299] = 0;
	res.files_failed = 40;
	res.failure_total = 40; /* more than the 12 the report window names */
	res.fail_all = calloc(40, sizeof *res.fail_all);
	CHECK(NULL != res.fail_all);
	for (size_t i = 0; i < 40; i++) {
		wchar_t path[400];

		swprintf(path, 400, L"C:\\Users\\%ls%zu",
		        i == 5 ? longname : L"f", i);
		res.fail_all[i].path = _wcsdup(path);
		res.fail_all[i].kind = WF_CF_EXISTS;
		res.fail_all[i].error = 80;
	}
	res.fail_all_n = 40;
	wf_app_on_copy_result(&r.app, &res);
	wf_copyresult_free(&res);
	CHECK(1 == r.app.jobs.n && 40 == r.app.jobs.log[0].fail_n &&
	        40 == r.app.jobs.log[0].total);
	CHECK(strlen(r.app.jobs.log[0].fail[5].path) > 300); /* not cut short */

	key(&r.app, WF_KEY_ESC); /* the report */
	CHECK(&WF_MODE_NORMAL == r.app.mode);
	ch(&r.app, 'J');
	CHECK(&WF_MODE_JOBLOG == r.app.mode);
	key(&r.app, WF_KEY_END);
	CHECK(&WF_MODE_JOBLOG == r.app.mode && r.app.jobs.top > 0);
	key(&r.app, WF_KEY_HOME);
	CHECK(0 == r.app.jobs.top);
	key(&r.app, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == r.app.mode);

	/* only the last WF_JOBLOG_MAX jobs are kept */
	for (size_t i = 0; i < WF_JOBLOG_MAX + 3; i++) {
		WfCopyResult ok = result_of(WF_CJ_COPY, WF_CS_DONE);

		ok.job = (uint32_t)(100 + i);
		wf_app_on_copy_result(&r.app, &ok);
	}
	CHECK(WF_JOBLOG_MAX == r.app.jobs.n &&
	        r.app.jobs.log[WF_JOBLOG_MAX - 1].job ==
	                100 + WF_JOBLOG_MAX + 2);
	wf_app_destroy(&r.app);
}

/* A failed path of Japanese letters in a window 600 cells wide: each line of it
 * is cut at a character, whatever the buffer that holds it, and every letter of
 * the path is on some line. */
static void a_wide_window_cuts_a_japanese_path_between_characters(void)
{
	enum
	{
		LETTERS = 1000,
		COLS_WIDE = 600
	};
	static Rig r;
	WfCopyResult res = result_of(WF_CJ_COPY, WF_CS_DONE);
	wchar_t *path = calloc(LETTERS + 8, sizeof *path);
	WfFrame *f = wf_frame_new(COLS_WIDE, 20, &WF_STYLE_BASE);
	size_t letters = 0;

	world();
	rig_copy_ready(&r);
	CHECK(path && f);
	path[0] = L'C';
	path[1] = L':';
	path[2] = L'\\';
	for (size_t i = 0; i < LETTERS; i++) {
		path[3 + i] =
		        0x3042; /* the letter A of hiragana, 3 bytes in UTF-8 */
	}
	res.files_failed = 1;
	res.failure_total = 1;
	res.fail_all = calloc(1, sizeof *res.fail_all);
	CHECK(NULL != res.fail_all);
	res.fail_all[0].path = _wcsdup(path);
	res.fail_all[0].kind = WF_CF_EXISTS;
	res.fail_all[0].error = 80;
	res.fail_all_n = 1;
	wf_app_on_copy_result(&r.app, &res);
	wf_copyresult_free(&res);
	r.app.cols = COLS_WIDE;
	ch(&r.app, 'J'); /* the report is on top: Esc, then the log */
	key(&r.app, WF_KEY_ESC);
	ch(&r.app, 'J');
	CHECK(&WF_MODE_JOBLOG == r.app.mode);
	wf_app_draw(&r.app, f);
	for (int y = 0; y < f->rows; y++) {
		for (int x = 0; x < f->cols; x++) {
			const char *u = f->cells[y * f->cols + x].utf8;

			if (!strcmp(u, "\xE3\x81\x82")) {
				letters++;
			}
			CHECK(strcmp(u, "\xEF\xBF\xBD") != 0);
		}
	}
	CHECK(LETTERS == (int)letters);
	wf_frame_free(f);
	free(path);
	wf_app_destroy(&r.app);
}

/* Scrolling a long failure history: each key counts the lines again only when the
 * log or the width has changed, and a frame walks as far as the window shows. */
static void scrolling_a_long_history_does_not_walk_it_all(void)
{
	static Rig r;
	WfFrame *f = wf_frame_new(120, 24, &WF_STYLE_BASE);
	clock_t t0;
	double ms;

	world();
	rig_copy_ready(&r);
	for (size_t j = 0; j < WF_JOBLOG_MAX; j++) {
		WfCopyResult res = result_of(WF_CJ_COPY, WF_CS_DONE);
		size_t n = 125;

		res.job = (uint32_t)(10 + j);
		res.files_failed = (uint32_t)n;
		res.failure_total = n;
		res.fail_all = calloc(n, sizeof *res.fail_all);
		CHECK(NULL != res.fail_all);
		for (size_t i = 0; i < n; i++) {
			wchar_t path[400];

			swprintf(path, 400,
			        L"C:\\Users\\dev\\folder%zu\\file%zu-%zu.txt",
			        j, i, j * n + i);
			res.fail_all[i].path = _wcsdup(path);
			res.fail_all[i].kind = WF_CF_EXISTS;
			res.fail_all[i].error = 80;
		}
		res.fail_all_n = n;
		wf_app_on_copy_result(&r.app, &res);
		wf_copyresult_free(&res);
	}
	ch(&r.app, 'J');
	key(&r.app, WF_KEY_ESC);
	ch(&r.app, 'J');
	CHECK(&WF_MODE_JOBLOG == r.app.mode);
	key(&r.app, WF_KEY_END);
	CHECK(r.app.jobs.lines_ok && r.app.jobs.top > 1000);
	t0 = clock();
	for (int k = 0; k < 200; k++) {
		key(&r.app, k % 2 ? WF_KEY_DOWN : WF_KEY_UP);
		wf_app_draw(&r.app, f);
	}
	ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
	printf("info: 200 scrolls and drawings of a log of %d lines: %.1f ms\n",
	        (int)r.app.jobs.lines_total, ms);
	t0 = clock();
	for (int k = 0; k < 200; k++) { /* as it was: every key counts it all */
		r.app.jobs.lines_ok = false;
		key(&r.app, k % 2 ? WF_KEY_DOWN : WF_KEY_UP);
		r.app.jobs.lines_ok = false;
		wf_app_draw(&r.app, f);
	}
	printf("info: the same with the counts dropped at every key: %.1f ms\n",
	        (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC);
	/* the lines it shows are the ones at the top: the same with and without the
	 * counts kept */
	{
		WfFrame *g = wf_frame_new(120, 24, &WF_STYLE_BASE);
		bool same = true;

		wf_app_draw(&r.app, f);
		r.app.jobs.lines_ok = false;
		wf_app_draw(&r.app, g);
		for (int i = 0; i < f->cols * f->rows; i++) {
			same = same &&
			        !strcmp(f->cells[i].utf8, g->cells[i].utf8);
		}
		CHECK(same);
		wf_frame_free(g);
	}
	{
		/* a job that ends while the log is open: the end of the log moves down */
		size_t before;
		WfCopyResult more = result_of(WF_CJ_COPY, WF_CS_DONE);

		key(&r.app, WF_KEY_END);
		before = r.app.jobs.top;
		more.job = 999;
		more.files_failed = 1;
		more.failure_total = 1;
		more.fail_all = calloc(1, sizeof *more.fail_all);
		CHECK(NULL != more.fail_all);
		more.fail_all[0].path = _wcsdup(L"C:\\late.txt");
		more.fail_all[0].kind = WF_CF_EXISTS;
		more.fail_all[0].error = 80;
		more.fail_all_n = 1;
		wf_app_on_copy_result(&r.app, &more);
		wf_copyresult_free(&more);
		key(&r.app, WF_KEY_END);
		CHECK(r.app.jobs.top != before);
	}
	wf_frame_free(f);
	wf_app_destroy(&r.app);
}

static WfEditOutcome receipt_ok(void)
{
	WfEditOutcome o = {WF_OK, 0, true, NULL, NULL, 0};

	return o;
}

static void a_move_can_be_undone_item_by_item(void)
{
	static Rig r;
	WfEditOutcome ok = receipt_ok();

	world();
	rig_open(&r, true, true);
	ch(&r.app, 'j');
	ch(&r.app, ' ');
	ch(&r.app, ' ');
	ch(&r.app, 'x');
	ch(&r.app, 'h');
	prove_listing(&r.app, 7);
	ch(&r.app, 'p');
	CHECK(1 == r.spy.calls);
	wf_app_on_edit(&r.app, r.spy.gen, &ok, L"file2.txt");
	wf_app_on_edit(&r.app, r.spy.gen, &ok, L"file10.txt");
	CHECK(strstr(r.app.msg, "MOVE done: 2 items") && 1 == r.app.undo_n &&
	        WF_UNDO_MOVE == r.app.undo[0].kind && 2 == r.app.undo[0].n);

	prove_listing(&r.app, 7);
	ch(&r.app, 'u');
	answer(&r.app, 'y');
	CHECK(3 == r.spy.calls && WF_EDIT_MOVE == r.spy.req[2].kind &&
	        !wcscmp(r.spy.parent[2], L"C:\\Users") &&
	        !wcscmp(r.spy.dest[2], L"C:\\Users\\dev") &&
	        !wcscmp(r.spy.old_name[2], L"file2.txt"));
	CHECK((r.spy.req[2].parent_id.kind == WF_ID_STRONG) &&
	        (r.spy.req[2].source_id.kind == WF_ID_STRONG) &&
	        (r.spy.req[2].dest_parent_id.kind == WF_ID_STRONG));
	/* kept while it runs (a failure must leave it to try again); a second u
	 * meanwhile is refused as busy */
	CHECK(1 == r.app.undo_n);
	ch(&r.app, 'u');
	CHECK(3 == r.spy.calls);
	wf_app_on_edit(&r.app, r.spy.gen, &ok, L"file2.txt");
	CHECK(4 == r.spy.calls && !wcscmp(r.spy.old_name[3], L"file10.txt"));
	wf_app_on_edit(&r.app, r.spy.gen, &ok, L"file10.txt");
	CHECK(strstr(r.app.msg, "UNDO done: 2 items moved back"));
	CHECK(0 == r.app.undo_n); /* an undo is not itself undoable */
	ch(&r.app, 'u');
	CHECK(strstr(r.app.msg, "Nothing to undo"));
	wf_app_destroy(&r.app);
}

static void a_rename_can_be_undone(void)
{
	static Rig r;
	WfEditOutcome ok = receipt_ok();
	const WfEntry *e;
	wchar_t old_name[64], renamed[80];

	world();
	rig_open(&r, true, true);
	ch(&r.app, 'j');
	e = wf_view_current(&r.app.cur);
	CHECK(NULL != e);
	wcscpy_s(old_name, 64, e->name);
	swprintf(renamed, 80, L"%ls2", old_name);
	ch(&r.app, 'R');
	key(&r.app, WF_KEY_END);
	type(&r.app, L"2");
	key(&r.app, WF_KEY_ENTER);
	CHECK(1 == r.spy.calls && WF_EDIT_RENAME == r.spy.req[0].kind);
	CHECK(0 == r.app.undo_n); /* only a rename that happened is kept */
	wf_app_on_edit(&r.app, r.spy.gen, &ok, renamed);
	CHECK(1 == r.app.undo_n && WF_UNDO_RENAME == r.app.undo[0].kind);

	ch(&r.app, 'u');
	answer(&r.app, 'y');
	CHECK(2 == r.spy.calls && WF_EDIT_RENAME == r.spy.req[1].kind &&
	        !wcscmp(r.spy.old_name[1], renamed) &&
	        !wcscmp(r.spy.new_name[1], old_name) &&
	        !wcscmp(r.spy.parent[1], L"C:\\Users\\dev"));
	CHECK((r.spy.req[1].source_id.kind == WF_ID_STRONG) &&
	        (r.spy.req[1].parent_id.kind == WF_ID_STRONG));
	CHECK(1 == r.app.undo_n); /* still there until it has happened */
	{
		WfEditOutcome denied = {WF_E_OS, 5, false, NULL, NULL, 0};

		/* refused (access denied): the entry stays, to try again */
		wf_app_on_edit(&r.app, r.spy.gen, &denied, old_name);
		CHECK(1 == r.app.undo_n);
	}
	ch(&r.app, 'u');
	answer(&r.app, 'y');
	CHECK(3 == r.spy.calls);
	wf_app_on_edit(&r.app, r.spy.gen, &ok, old_name);
	CHECK(strstr(r.app.msg, "UNDO done: renamed back") &&
	        0 == r.app.undo_n);

	/* a refused rename leaves nothing to undo */
	ch(&r.app, 'R');
	type(&r.app, L"3");
	key(&r.app, WF_KEY_ENTER);
	{
		WfEditOutcome denied = {WF_E_OS, 5, false, NULL, NULL, 0};

		wf_app_on_edit(&r.app, r.spy.gen, &denied, L"x");
	}
	CHECK(0 == r.app.undo_n);
	wf_app_destroy(&r.app);
}

/* DR-DRAFT-029: every change has a row in the history that says whether Undo can
 * still take it back, and when it cannot, why. */
/* The newest row; an empty history gives a row of nothing, so that a check that
 * fails is reported instead of the test ending in a crash. */
static const WfJobLogEntry *newest_row(const WfApp *a)
{
	static const WfJobLogEntry none = {.head = {0}};

	return a->jobs.n ? &a->jobs.log[a->jobs.n - 1] : &none;
}

static void a_rename_and_a_new_folder_have_rows_in_the_history(void)
{
	static Rig r;
	WfEditOutcome ok = receipt_ok();
	WfEditOutcome denied = {WF_E_OS, 5, false, NULL, NULL, 0};
	const WfEntry *e;
	wchar_t old_name[64], renamed[80];
	uint32_t uid;

	world();
	rig_open(&r, true, true);
	ch(&r.app, 'j');
	e = wf_view_current(&r.app.cur);
	wcscpy_s(old_name, 64, e->name);
	swprintf(renamed, 80, L"%ls2", old_name);

	/* a rename that is refused changed nothing: no row */
	ch(&r.app, 'R');
	key(&r.app, WF_KEY_END);
	type(&r.app, L"2");
	key(&r.app, WF_KEY_ENTER);
	wf_app_on_edit(&r.app, r.spy.gen, &denied, renamed);
	CHECK(0 == r.app.jobs.n);
	prove_listing(
	        &r.app, 7); /* the listing was read again after the answer */
	ch(&r.app, 'R');
	key(&r.app, WF_KEY_END);
	type(&r.app, L"2");
	key(&r.app, WF_KEY_ENTER);
	CHECK(0 == r.app.jobs.n); /* not before it has happened */
	wf_app_on_edit(&r.app, r.spy.gen, &ok, renamed);
	CHECK(1 == r.app.jobs.n && 1 == r.app.undo_n);
	CHECK(strstr(newest_row(&r.app)->head, "RENAME: "));
	CHECK(strstr(newest_row(&r.app)->head, " -> "));
	CHECK(0 == newest_row(&r.app)->job);
	CHECK(WF_HU_KEPT == newest_row(&r.app)->undo);
	uid = r.app.undo[0].uid;
	CHECK(newest_row(&r.app)->undo_uid == uid);

	/* an Undo that is refused leaves the row as it was; one that happens
	 * marks it, and makes no row of its own */
	ch(&r.app, 'u');
	answer(&r.app, 'y');
	wf_app_on_edit(&r.app, r.spy.gen, &denied, old_name);
	CHECK(1 == r.app.jobs.n && WF_HU_KEPT == r.app.jobs.log[0].undo);
	ch(&r.app, 'u');
	answer(&r.app, 'y');
	wf_app_on_edit(&r.app, r.spy.gen, &ok, old_name);
	CHECK(1 == r.app.jobs.n && WF_HU_DONE == r.app.jobs.log[0].undo);
	CHECK(0 == r.app.jobs.log[0].undo_uid);
	prove_listing(&r.app, 7);

	/* a new folder is a row that Undo does not take back */
	ch(&r.app, 'd');
	type(&r.app, L"made");
	key(&r.app, WF_KEY_ENTER);
	wf_app_on_edit(&r.app, r.spy.gen, &ok, L"made");
	prove_listing(&r.app, 7);
	CHECK(2 == r.app.jobs.n && 0 == r.app.undo_n);
	CHECK(strstr(newest_row(&r.app)->head, "NEW FOLDER: made"));
	CHECK(WF_HU_NONE == newest_row(&r.app)->undo &&
	        MSG_HIST_WHY_CREATE == newest_row(&r.app)->why);
	ch(&r.app, '%');
	type(&r.app, L"note.txt");
	key(&r.app, WF_KEY_ENTER);
	wf_app_on_edit(&r.app, r.spy.gen, &ok, L"note.txt");
	CHECK(3 == r.app.jobs.n);
	CHECK(strstr(newest_row(&r.app)->head, "NEW FILE: note.txt"));
	wf_app_destroy(&r.app);
}

static void a_move_has_a_row_and_its_undo_marks_it(void)
{
	static Rig r;
	WfEditOutcome ok = receipt_ok();

	world();
	rig_open(&r, true, true);
	ch(&r.app, 'j');
	ch(&r.app, ' ');
	ch(&r.app, ' ');
	ch(&r.app, 'x');
	ch(&r.app, 'h');
	prove_listing(&r.app, 7);
	ch(&r.app, 'p');
	wf_app_on_edit(&r.app, r.spy.gen, &ok, L"file2.txt");
	CHECK(0 == r.app.jobs.n); /* one row for the move, at its end */
	wf_app_on_edit(&r.app, r.spy.gen, &ok, L"file10.txt");
	CHECK(1 == r.app.jobs.n && 1 == r.app.undo_n);
	CHECK(strstr(newest_row(&r.app)->head, "MOVE: 2 item(s) to C:\\Users"));
	CHECK(WF_HU_KEPT == newest_row(&r.app)->undo &&
	        newest_row(&r.app)->undo_uid == r.app.undo[0].uid);

	prove_listing(&r.app, 7);
	ch(&r.app, 'u');
	answer(&r.app, 'y');
	wf_app_on_edit(&r.app, r.spy.gen, &ok, L"file2.txt");
	CHECK(WF_HU_KEPT == r.app.jobs.log[0].undo); /* one of two is back */
	wf_app_on_edit(&r.app, r.spy.gen, &ok, L"file10.txt");
	CHECK(1 == r.app.jobs.n && WF_HU_DONE == r.app.jobs.log[0].undo);
	wf_app_destroy(&r.app);
}

static void the_jobs_of_the_copy_engine_say_what_undo_can_do(void)
{
	static Rig r;
	WfCopyResult res;

	world();
	rig_copy_ready(&r);
	/* a copy that made something new */
	res = result_of(WF_CJ_COPY, WF_CS_DONE);
	res.files_copied = 1;
	add_created(&res, L"C:\\Users\\file2.txt", 9);
	wf_app_on_copy_result(&r.app, &res);
	wf_copyresult_free(&res);
	CHECK(1 == r.app.jobs.n && WF_HU_KEPT == newest_row(&r.app)->undo);
	CHECK(newest_row(&r.app)->undo_uid == r.app.undo[0].uid);
	CHECK(0 != newest_row(&r.app)->job);

	/* a copy that only replaced: nothing new to take back */
	res = result_of(WF_CJ_COPY, WF_CS_DONE);
	res.job = 2;
	res.files_copied = 1;
	res.overwritten = 1;
	wf_app_on_copy_result(&r.app, &res);
	CHECK(WF_HU_NONE == newest_row(&r.app)->undo &&
	        MSG_HIST_WHY_COPY_NOTHING == newest_row(&r.app)->why);

	/* a move to another volume, and the Recycle Bin */
	res = result_of(WF_CJ_MOVE_ACROSS, WF_CS_DONE);
	res.job = 3;
	res.files_copied = 1;
	res.sources_recycled = 1;
	wf_app_on_copy_result(&r.app, &res);
	CHECK(WF_HU_NONE == newest_row(&r.app)->undo &&
	        MSG_HIST_WHY_MOVE_ACROSS == newest_row(&r.app)->why);
	res = result_of(WF_CJ_RECYCLE, WF_CS_DONE);
	res.job = 4;
	res.sources_recycled = 1;
	wf_app_on_copy_result(&r.app, &res);
	CHECK(WF_HU_NONE == newest_row(&r.app)->undo &&
	        MSG_HIST_WHY_RECYCLE == newest_row(&r.app)->why);
	CHECK(4 == r.app.jobs.n && 1 == r.app.undo_n);

	/* more changes than Undo keeps: the row of the oldest says so */
	for (uint8_t i = 0; i < WF_UNDO_MAX; i++) {
		res = result_of(WF_CJ_COPY, WF_CS_DONE);
		res.job = 10u + i;
		res.files_copied = 1;
		add_created(&res, L"C:\\Users\\file2.txt", (uint8_t)(20 + i));
		wf_app_on_copy_result(&r.app, &res);
		wf_copyresult_free(&res);
	}
	CHECK(WF_UNDO_MAX == r.app.undo_n);
	CHECK(WF_HU_EXPIRED == r.app.jobs.log[0].undo &&
	        0 == r.app.jobs.log[0].undo_uid);
	CHECK(WF_HU_KEPT == newest_row(&r.app)->undo);
	/* only the row of the entry that went is marked; every other keeps its own */
	for (size_t i = 4; i < r.app.jobs.n; i++) {
		CHECK(WF_HU_KEPT == r.app.jobs.log[i].undo);
	}
	wf_app_destroy(&r.app);
}

static void the_undo_stack_is_bounded(void)
{
	static Rig r;
	WfCopyResult res;

	world();
	rig_copy_ready(&r);
	for (int i = 0; i < WF_UNDO_MAX + 4; i++) {
		res = result_of(WF_CJ_COPY, WF_CS_DONE);
		add_created(&res, L"C:\\Users\\file2.txt", (uint8_t)(i + 1));
		wf_app_on_copy_result(&r.app, &res);
		wf_copyresult_free(&res);
	}
	CHECK(WF_UNDO_MAX == r.app.undo_n);
	/* the newest is on top */
	CHECK(r.app.undo[WF_UNDO_MAX - 1].item[0].id.id[0] == WF_UNDO_MAX + 4);
	wf_app_destroy(&r.app);
}

static void cancelled_recycling_reports_confirmed_receipts(void)
{
	static Rig r;
	world();
	rig_copy_ready(&r);
	WfCopyResult res = result_of(WF_CJ_RECYCLE, WF_CS_CANCELLED);
	res.sources_recycled = 1;
	res.files_copied = 0;
	wf_app_on_copy_result(&r.app, &res);
	CHECK(strstr(r.app.msg, "1 verified in the Bin") != NULL);
	CHECK(strstr(r.app.msg, "0 file(s) done") == NULL);
	wf_app_destroy(&r.app);
}

int main(void)
{
	cancelled_recycling_reports_confirmed_receipts();
	a_confirmed_check_starts_a_copy_job();
	without_a_copy_engine_the_transfer_still_stops();
	a_known_reason_is_what_a_refused_copy_says();
	a_known_reason_is_what_a_refused_recycle_says();
	an_unproven_destination_starts_nothing();
	a_weak_destination_takes_files_but_not_a_folder();
	a_full_queue_is_reported_and_nothing_is_counted();
	an_existing_name_asks_and_the_answer_is_not_remembered();
	collisions_still_refuse_a_move_and_a_missing_engine();
	a_move_across_volumes_offers_move_or_copy();
	delete_asks_and_sends_the_selection_to_the_bin();
	problems_open_a_report_and_any_key_closes_it();
	a_failed_read_back_says_how_many_were_verified();
	an_unknown_recycle_result_does_not_claim_the_source_survived();
	pause_resume_and_cancel_reach_the_engine();
	undoing_a_copy_asks_and_sends_the_new_copies_to_the_bin();
	a_partly_done_copy_undo_keeps_only_what_is_left();
	a_merge_result_gives_one_undo_entry_for_each_folder();
	a_row_whose_entry_was_pushed_out_never_says_undone();
	items_picked_in_a_zip_are_extracted_by_p_in_a_folder();
	an_extraction_says_it_was_one();
	many_picks_find_a_name_that_is_there_in_any_case();
	the_backups_window_lists_what_sits_beside_the_file();
	the_job_log_keeps_every_failure_with_its_path();
	a_wide_window_cuts_a_japanese_path_between_characters();
	scrolling_a_long_history_does_not_walk_it_all();
	a_move_can_be_undone_item_by_item();
	a_rename_can_be_undone();
	a_rename_and_a_new_folder_have_rows_in_the_history();
	a_move_has_a_row_and_its_undo_marks_it();
	the_jobs_of_the_copy_engine_say_what_undo_can_do();
	the_undo_stack_is_bounded();
	return TEST_RESULT();
}
