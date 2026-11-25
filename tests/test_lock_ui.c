/*
 * The lock window in the application (DR-DRAFT-024): who has the file open, asking
 * a program to close, ending it behind two questions, and trying the failed
 * operation again. The lock worker, the copy engine and the editor are fakes that
 * record what they were asked, so no process is touched and no disk is used.
 * What must hold: nothing is asked of the worker before the person has answered
 * every question, closing never turns into ending, a stale answer changes
 * nothing, a retry performs nothing by itself (it puts the operation back at its
 * ordinary entry), and a renamed-over file is not renamed again.
 */
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "ui/wf_app_priv.h"
#include "wf_fake_fs.h"
#include "wf_test.h"

typedef struct
{
	unsigned queries, closes, ends;
	uint32_t gen;
	wchar_t path[260];
	uint32_t pid;
	uint64_t start;
	WfLockAppType type;
	WfStatus answer;
	unsigned submits; /* the fake copy engine */
	WfCopyKind kind;
	unsigned editor_saves;
	wchar_t editor_file[64];
} Fake;

static WfStatus f_query(void *ctx, uint32_t gen, const wchar_t *path)
{
	Fake *f = ctx;

	f->queries++;
	f->gen = gen;
	wcsncpy_s(f->path, 260, path, _TRUNCATE);
	return f->answer;
}

static WfStatus f_close(void *ctx,
        uint32_t gen,
        uint32_t pid,
        uint64_t start,
        WfLockAppType type)
{
	Fake *f = ctx;

	f->closes++;
	f->gen = gen;
	f->pid = pid;
	f->start = start;
	f->type = type;
	return f->answer;
}

static WfStatus
f_end(void *ctx, uint32_t gen, uint32_t pid, uint64_t start, WfLockAppType type)
{
	Fake *f = ctx;

	f->ends++;
	f->gen = gen;
	f->pid = pid;
	f->start = start;
	f->type = type;
	return f->answer;
}

static WfStatus fc_submit(void *ctx, const WfCopySpec *spec, uint32_t *job)
{
	Fake *f = ctx;

	f->submits++;
	f->kind = spec->kind;
	if (job) {
		*job = f->submits;
	}
	return WF_OK;
}

static wchar_t *fe_path(void *ctx)
{
	Fake *f = ctx;

	return f->editor_file[0] ? _wcsdup(f->editor_file) : NULL;
}

static int fe_save(void *ctx)
{
	((Fake *)ctx)->editor_saves++;
	return 1;
}

typedef struct
{
	WfApp app;
	FakePort fs;
	Fake fake;
} Rig;

static const WfEvent ESC = {WF_EV_KEY, 0, WF_KEY_ESC, 0};
static const WfEvent ENTER = {WF_EV_KEY, 0, WF_KEY_ENTER, 0};
static const WfEvent DOWN = {WF_EV_KEY, 0, WF_KEY_DOWN, 0};

static void ch(WfApp *a, uint32_t c)
{
	WfEvent e = {WF_EV_CHAR, c, WF_KEY_NONE, 0};

	a->mode->on_event(a, &e);
}

static void key(WfApp *a, const WfEvent *e)
{
	a->mode->on_event(a, e);
}

static void open_rig(Rig *r)
{
	memset(r, 0, sizeof *r);
	r->fs.app = &r->app;
	CHECK(WF_OK ==
	        wf_app_init(&r->app, 120, 30, fake_port(&r->fs),
	                L"C:\\Users\\dev"));
	for (size_t i = 0; i < r->app.cur.dir->n; i++) {
		WfFileId *id = &r->app.cur.dir->e[i].identity;

		id->kind = WF_ID_STRONG;
		id->volume = 7;
		id->id[0] = (uint8_t)(i + 20);
	}
	r->app.cur.dir->identity.kind = WF_ID_STRONG;
	r->app.cur.dir->identity.volume = 7;
	wf_app_set_lock(
	        &r->app, (WfLockPort){&r->fake, f_query, f_close, f_end});
	wf_app_set_copy(&r->app,
	        (WfCopyPort){&r->fake, fc_submit, NULL, NULL, NULL, NULL},
	        WF_VERIFY_SHA256);
	r->app.editor.ctx = &r->fake;
	wf_app_set_editor_lock(&r->app, fe_path, fe_save);
}

/* The window is on file2.txt (Docs, a folder, is first). */
static void ask_by_hand(Rig *r)
{
	ch(&r->app, 'j');
	ch(&r->app, 'L');
}

static WfLockHolder holder(uint32_t pid, const wchar_t *name)
{
	WfLockHolder h;

	memset(&h, 0, sizeof h);
	h.pid = pid;
	h.start = 132000000000000000ull + pid;
	h.identity_ok = true;
	h.type = WF_LOCKAPP_MAIN_WINDOW;
	wcscpy_s(h.app, WF_LOCK_APP_CHARS, name);
	wcscpy_s(h.exe, WF_LOCK_EXE_CHARS, L"C:\\Tools\\prog.exe");
	h.has_window = true;
	return h;
}

/* A query's answer for the newest request, with these holders. */
static void answer_query(Rig *r, const WfLockHolder *h, size_t n)
{
	WfLockAnswer a;

	memset(&a, 0, sizeof a);
	a.gen = r->fake.gen;
	a.req = WF_LOCKREQ_QUERY;
	a.st = WF_OK;
	if (n) {
		a.list.h = calloc(n, sizeof *a.list.h);
		memcpy(a.list.h, h, n * sizeof *h);
		a.list.n = n;
	}
	wf_app_on_lock(&r->app, &a);
}

static void answer_action(Rig *r, WfLockReq req, WfLockActionResult res)
{
	WfLockAnswer a;

	memset(&a, 0, sizeof a);
	a.gen = r->fake.gen;
	a.req = req;
	a.result = res;
	wf_app_on_lock(&r->app, &a);
}

static int row_has(const WfFrame *f, int y, const char *needle)
{
	char row[1024] = "";

	for (int x = 0; x < f->cols && strlen(row) + 24 < sizeof row; x++) {
		strcat_s(row, sizeof row, f->cells[y * f->cols + x].utf8);
	}
	return NULL != strstr(row, needle);
}

static int frame_has(const WfFrame *f, const char *needle)
{
	for (int y = 0; y < f->rows; y++) {
		if (row_has(f, y, needle)) {
			return 1;
		}
	}
	return 0;
}

static bool screen_has(WfApp *a, const char *needle)
{
	WfFrame *f = wf_frame_new(a->cols, a->rows, &WF_STYLE_BASE);
	int has;

	wf_app_draw(a, f);
	has = frame_has(f, needle);
	wf_frame_free(f);
	return has;
}

/* Answers the question that is on screen: it must be armed first (typed-ahead
 * input is dropped until the guard has passed). */
static void arm(WfApp *a)
{
	CHECK(&WF_MODE_CONFIRM == a->mode);
	a->confirm.state = WF_CONF_ARMED;
}

static void by_hand_asks_for_the_file_under_the_cursor(void)
{
	static Rig r;
	WfLockHolder two[2] = {holder(5824, L"Code.exe"), holder(77, L"Tool")};

	open_rig(&r);
	ch(&r.app, 'L'); /* the cursor is on a folder */
	CHECK(0 == r.fake.queries && &WF_MODE_NORMAL == r.app.mode);
	ask_by_hand(&r);
	CHECK(1 == r.fake.queries && &WF_MODE_LOCK == r.app.mode);
	CHECK(!wcscmp(r.fake.path, L"C:\\Users\\dev\\file2.txt"));
	CHECK(WF_LOCKOP_NONE == r.app.lock.op);
	CHECK(screen_has(&r.app, "Looking"));
	two[1].close_deny = WF_DENY_NO_WINDOW;
	two[1].end_deny = WF_DENY_CRITICAL;
	answer_query(&r, two, 2);
	CHECK(screen_has(&r.app, "Code.exe") && screen_has(&r.app, "PID 5824"));
	CHECK(screen_has(&r.app, "Force terminate: allowed"));
	CHECK(screen_has(&r.app, "[R] Refresh"));
	CHECK(!screen_has(
	        &r.app, "[T] Retry")); /* nothing failed: nothing to retry */
	key(&r.app, &DOWN);
	CHECK(screen_has(&r.app, "refused (critical process)"));
	CHECK(screen_has(&r.app, "not possible (no window to ask)"));
	ch(&r.app, 't');
	CHECK(screen_has(&r.app, "Nothing to try again"));
	key(&r.app, &ESC);
	CHECK(&WF_MODE_NORMAL == r.app.mode);
	wf_app_destroy(&r.app);
}

static void an_empty_answer_is_not_a_claim_of_no_lock(void)
{
	static Rig r;

	open_rig(&r);
	ask_by_hand(&r);
	answer_query(&r, NULL, 0);
	CHECK(screen_has(&r.app, "No program was found"));
	CHECK(screen_has(&r.app, "does not mean the file is free"));
	/* and an error is not an empty list */
	ch(&r.app, 'r');
	{
		WfLockAnswer a;

		memset(&a, 0, sizeof a);
		a.gen = r.fake.gen;
		a.req = WF_LOCKREQ_QUERY;
		a.st = WF_E_OS;
		a.error = 5;
		wf_app_on_lock(&r.app, &a);
	}
	CHECK(screen_has(&r.app, "Could not look (error 5)"));
	CHECK(!screen_has(&r.app, "No program was found"));
	wf_app_destroy(&r.app);
}

static void a_stale_answer_changes_nothing_and_requests_do_not_pile_up(void)
{
	static Rig r;
	WfLockHolder one = holder(10, L"A");
	WfLockAnswer stale;

	open_rig(&r);
	ask_by_hand(&r);
	CHECK(1 == r.fake.queries);
	ch(&r.app, 'r'); /* still out: not a second request */
	CHECK(1 == r.fake.queries && screen_has(&r.app, "Still working"));
	memset(&stale, 0, sizeof stale);
	stale.gen = r.fake.gen + 40;
	stale.req = WF_LOCKREQ_QUERY;
	stale.list.h = calloc(1, sizeof *stale.list.h);
	stale.list.h[0] = holder(999, L"Stale");
	stale.list.n = 1;
	wf_app_on_lock(&r.app, &stale);
	CHECK(0 == r.app.lock.list.n && r.app.lock.searching);
	answer_query(&r, &one, 1);
	CHECK(1 == r.app.lock.list.n && !r.app.lock.searching);
	ch(&r.app, 'r');
	CHECK(2 == r.fake.queries);
	wf_app_destroy(&r.app);
}

static void closing_is_asked_once_and_never_becomes_ending(void)
{
	static Rig r;
	WfLockHolder two[2] = {holder(5824, L"Code.exe"), holder(77, L"Tool")};

	open_rig(&r);
	ask_by_hand(&r);
	two[1].close_deny = WF_DENY_NO_WINDOW;
	answer_query(&r, two, 2);
	/* the row that cannot be asked is not asked */
	key(&r.app, &DOWN);
	ch(&r.app, 'c');
	CHECK(&WF_MODE_LOCK == r.app.mode && 0 == r.fake.closes);
	key(&r.app, &DOWN); /* back to the first */
	/* a question, and Esc ends it with nothing sent */
	ch(&r.app, 'c');
	CHECK(&WF_MODE_CONFIRM == r.app.mode && 0 == r.fake.closes);
	arm(&r.app);
	key(&r.app, &ESC);
	CHECK(0 == r.fake.closes && 0 == r.fake.ends);
	/* Enter never confirms */
	ask_by_hand(&r); /* L again (Esc closed the window) */
	r.fake.gen = r.app.lock.gen;
	answer_query(&r, two, 2);
	ch(&r.app, 'c');
	arm(&r.app);
	key(&r.app, &ENTER);
	CHECK(0 == r.fake.closes);
	/* typed ahead during the guard is dropped */
	ask_by_hand(&r);
	answer_query(&r, two, 2);
	ch(&r.app, 'c');
	ch(&r.app, 'y'); /* not armed yet */
	CHECK(0 == r.fake.closes && &WF_MODE_CONFIRM == r.app.mode);
	arm(&r.app);
	ch(&r.app, 'y');
	CHECK(1 == r.fake.closes && 5824 == r.fake.pid &&
	        r.fake.start == two[0].start &&
	        WF_LOCKAPP_MAIN_WINDOW == r.fake.type);
	CHECK(&WF_MODE_LOCK == r.app.mode);
	/* the program did not close: that is said, and nothing else is done */
	answer_action(&r, WF_LOCKREQ_CLOSE, WF_LOCKRES_STILL);
	CHECK(screen_has(&r.app, "still running"));
	CHECK(0 == r.fake.ends);
	/* the screen looked again by itself (one query), not in a loop */
	CHECK(r.fake.queries >= 4);
	wf_app_destroy(&r.app);
}

static void a_close_answer_after_the_window_was_left_reaches_the_status_line(
        void)
{
	static Rig r;
	WfLockHolder one = holder(5824, L"Code.exe");

	open_rig(&r);
	ask_by_hand(&r);
	answer_query(&r, &one, 1);
	ch(&r.app, 'c');
	arm(&r.app);
	ch(&r.app, 'y');
	key(&r.app, &ESC); /* leaves while the request is out */
	answer_action(&r, WF_LOCKREQ_CLOSE, WF_LOCKRES_EXITED);
	CHECK(strstr(r.app.msg, "has closed"));
	CHECK(0 == r.fake.ends);
	wf_app_destroy(&r.app);
}

static void ending_needs_two_questions_and_every_cancel_does_nothing(void)
{
	static Rig r;
	WfLockHolder two[2] = {holder(5824, L"Code.exe"), holder(77, L"Tool")};

	open_rig(&r);
	ask_by_hand(&r);
	two[1].end_deny = WF_DENY_SYSTEM;
	answer_query(&r, two, 2);
	/* refused rows have no question at all */
	key(&r.app, &DOWN);
	ch(&r.app, 'k');
	CHECK(&WF_MODE_LOCK == r.app.mode && 0 == r.fake.ends);
	CHECK(screen_has(&r.app, "refused (system process)"));
	key(&r.app, &DOWN);
	/* too small to show the warning: no question, no request */
	r.app.cols = 100;
	r.app.rows = 12;
	ch(&r.app, 'k');
	CHECK(&WF_MODE_LOCK == r.app.mode && 0 == r.fake.ends);
	CHECK(screen_has(&r.app, "Too small to show the warning"));
	r.app.cols = 120;
	r.app.rows = 30;
	/* step one: the warning is on screen, in full */
	ch(&r.app, 'k');
	CHECK(&WF_MODE_CONFIRM == r.app.mode);
	arm(&r.app);
	{
		WfFrame *f = wf_frame_new(44, 14, &WF_STYLE_BASE);

		r.app.cols = 44;
		r.app.rows = 14;
		wf_app_draw(&r.app, f);
		CHECK(frame_has(f, "FORCE TERMINATE") &&
		        frame_has(f, "WILL BE LOST") &&
		        frame_has(f, "PID 5824"));
		wf_frame_free(f);
		r.app.cols = 120;
		r.app.rows = 30;
	}
	key(&r.app, &ESC);
	CHECK(0 == r.fake.ends && &WF_MODE_NORMAL == r.app.mode);
	/* step one answered, step two cancelled */
	ask_by_hand(&r);
	answer_query(&r, two, 2);
	ch(&r.app, 'k');
	arm(&r.app);
	ch(&r.app, 'y');
	CHECK(&WF_MODE_CONFIRM == r.app.mode &&
	        0 == r.fake.ends); /* the second */
	CHECK(r.app.confirm.yes_ch == 'k');
	ch(&r.app, 'k'); /* typed during the guard of the second: dropped */
	CHECK(0 == r.fake.ends && &WF_MODE_CONFIRM == r.app.mode);
	arm(&r.app);
	ch(&r.app, 'y'); /* the first question's letter does not answer it */
	CHECK(0 == r.fake.ends && &WF_MODE_CONFIRM == r.app.mode);
	key(&r.app, &ENTER);
	CHECK(0 == r.fake.ends);
	/* both answered: one request, for that program */
	ask_by_hand(&r);
	answer_query(&r, two, 2);
	ch(&r.app, 'k');
	arm(&r.app);
	ch(&r.app, 'y');
	arm(&r.app);
	ch(&r.app, 'k');
	CHECK(1 == r.fake.ends && 5824 == r.fake.pid &&
	        r.fake.start == two[0].start);
	answer_action(&r, WF_LOCKREQ_END, WF_LOCKRES_EXITED);
	CHECK(screen_has(&r.app, "was ended"));
	wf_app_destroy(&r.app);
}

static void a_window_that_shrinks_between_the_questions_stops_it(void)
{
	static Rig r;
	WfLockHolder one = holder(5824, L"Code.exe");

	open_rig(&r);
	ask_by_hand(&r);
	answer_query(&r, &one, 1);
	ch(&r.app, 'k');
	arm(&r.app);
	ch(&r.app, 'y');
	arm(&r.app);
	r.app.cols = 30; /* too small to show the warning now */
	r.app.rows = 10;
	ch(&r.app, 'k');
	CHECK(0 == r.fake.ends);
	wf_app_destroy(&r.app);
}

static void a_worker_that_is_busy_refuses_and_nothing_is_assumed(void)
{
	static Rig r;
	WfLockHolder one = holder(5824, L"Code.exe");

	open_rig(&r);
	ask_by_hand(&r);
	answer_query(&r, &one, 1);
	r.fake.answer = WF_E_INVAL;
	ch(&r.app, 'k');
	arm(&r.app);
	ch(&r.app, 'y');
	arm(&r.app);
	ch(&r.app, 'k');
	CHECK(1 == r.fake.ends && 0 == r.app.lock.busy);
	CHECK(screen_has(&r.app, "Still working"));
	wf_app_destroy(&r.app);
}

static WfCopySpec spec_of(WfCopyKind kind)
{
	WfCopySpec s;

	memset(&s, 0, sizeof s);
	wf_slot_init(&s.slot);
	s.kind = kind;
	s.dest = kind == WF_CJ_RECYCLE ? NULL : _wcsdup(L"C:\\Pics");
	s.slot.mode = WF_XFER_COPY;
	s.slot.dir = _wcsdup(L"C:\\Users\\dev");
	s.slot.count = 3;
	s.slot.src = calloc(3, sizeof *s.slot.src);
	s.slot.src[0].name = _wcsdup(L"a.txt");
	s.slot.src[1].name = _wcsdup(L"b.txt");
	s.slot.src[2].name = _wcsdup(L"c.txt");
	s.slot.src[1].snap.id.kind = WF_ID_STRONG;
	s.slot.src[1].snap.id.volume = 7;
	s.slot.src[1].snap.id.id[0] = 99;
	return s;
}

static WfCopyResult failed_in(WfCopyKind kind,
        const wchar_t *path,
        unsigned long e,
        WfCopyFailFull *fail)
{
	WfCopyResult r;

	memset(&r, 0, sizeof r);
	memset(fail, 0, sizeof *fail);
	fail->path = _wcsdup(path);
	fail->kind = WF_CF_SOURCE_UNREADABLE;
	fail->error = e;
	r.job = 4;
	r.kind = kind;
	r.failure_total = 1;
	r.fail_all = fail;
	r.fail_all_n = 1;
	return r;
}

static void only_a_lock_failure_is_noted(void)
{
	static Rig r;
	WfCopySpec s = spec_of(WF_CJ_COPY);
	WfCopyFailFull fail;
	WfCopyResult res;

	open_rig(&r);
	wf_app_lock_note_job(&r.app, 4, &s);
	res = failed_in(WF_CJ_COPY, L"C:\\Users\\dev\\b.txt", 32, &fail);
	CHECK(wf_app_lock_job_result(&r.app, &res));
	CHECK(WF_LOCKOP_COPY == r.app.lock.op &&
	        !wcscmp(r.app.lock.path, L"C:\\Users\\dev\\b.txt"));
	free(fail.path);
	/* access denied is only "maybe", and other codes say nothing about programs */
	wf_app_lock_note_job(&r.app, 4, &s);
	res = failed_in(WF_CJ_COPY, L"C:\\Users\\dev\\b.txt", 5, &fail);
	CHECK(!wf_app_lock_job_result(&r.app, &res));
	free(fail.path);
	wf_app_lock_note_job(&r.app, 4, &s);
	res = failed_in(WF_CJ_COPY, L"C:\\Users\\dev\\b.txt", 112, &fail);
	CHECK(!wf_app_lock_job_result(&r.app, &res));
	free(fail.path);
	CHECK(0 ==
	        r.app.lock.jobs_n); /* nothing is kept for a job that ended */
	wf_copyspec_free(&s);
	wf_app_destroy(&r.app);
}

static void a_copy_is_put_back_at_the_paste_and_nothing_starts(void)
{
	static Rig r;
	WfCopySpec s = spec_of(WF_CJ_COPY);
	WfCopyFailFull fail;
	WfCopyResult res;
	WfLockHolder one = holder(5824, L"Code.exe");

	open_rig(&r);
	wf_app_lock_note_job(&r.app, 4, &s);
	res = failed_in(WF_CJ_COPY, L"C:\\Users\\dev\\b.txt", 32, &fail);
	CHECK(wf_app_lock_job_result(&r.app, &res));
	free(fail.path);
	CHECK(1 == r.app.lock.slot.count &&
	        !wcscmp(r.app.lock.slot.src[0].name, L"b.txt"));
	CHECK(!wcscmp(r.app.lock.dest, L"C:\\Pics"));
	ch(&r.app, 'L');
	CHECK(&WF_MODE_LOCK == r.app.mode && 1 == r.fake.queries);
	CHECK(!wcscmp(r.fake.path, L"C:\\Users\\dev\\b.txt"));
	CHECK(WF_LOCKOP_COPY == r.app.lock.op);
	answer_query(&r, &one, 1);
	CHECK(screen_has(&r.app, "Failed operation: copy"));
	CHECK(screen_has(&r.app, "[T] Retry"));
	ch(&r.app, 't');
	/* back at the ordinary entry: the slot, the policy for names (asked again),
	 * and no job */
	CHECK(&WF_MODE_NORMAL == r.app.mode);
	CHECK(WF_XFER_COPY == r.app.slot.mode && 1 == r.app.slot.count);
	CHECK(!r.app.slot.collisions_ok);
	CHECK(!wcscmp(r.app.slot.src[0].name, L"b.txt") &&
	        99 ==
	                r.app.slot.src[0]
	                        .snap.id.id[0]); /* the original identity */
	CHECK(0 == r.fake.submits);
	CHECK(WF_LOCKOP_NONE == r.app.lock.op); /* one retry per failure */
	wf_copyspec_free(&s);
	wf_app_destroy(&r.app);
}

static void a_delete_is_asked_again_with_the_ordinary_question(void)
{
	static Rig r;
	WfCopySpec s = spec_of(WF_CJ_RECYCLE);
	WfCopyFailFull fail;
	WfCopyResult res;

	open_rig(&r);
	wf_app_lock_note_job(&r.app, 4, &s);
	res = failed_in(WF_CJ_RECYCLE, L"C:\\Users\\dev\\c.txt", 32, &fail);
	CHECK(wf_app_lock_job_result(&r.app, &res));
	free(fail.path);
	CHECK(WF_LOCKOP_RECYCLE == r.app.lock.op);
	ch(&r.app, 'L');
	ch(&r.app, 't');
	CHECK(&WF_MODE_CONFIRM == r.app.mode && 0 == r.fake.submits);
	CHECK(1 == r.app.recycle_slot.count);
	arm(&r.app);
	key(&r.app, &ESC);
	CHECK(0 == r.fake.submits); /* cancelled: nothing deleted */
	wf_copyspec_free(&s);
	wf_app_destroy(&r.app);
}

static void set_rename_note(Rig *r, uint8_t idbyte)
{
	WfUndoEntry *u = &r->app.undo_pending;

	memset(u, 0, sizeof *u);
	u->kind = WF_UNDO_RENAME;
	u->folder = _wcsdup(L"C:\\Users\\dev");
	u->item = calloc(1, sizeof *u->item);
	u->item[0].name = _wcsdup(L"renamed.txt");
	u->item[0].old_name = _wcsdup(L"file10.txt");
	u->item[0].id.kind = WF_ID_STRONG;
	u->item[0].id.volume = 7;
	u->item[0].id.id[0] = idbyte;
	u->n = 1;
	r->app.undo_pending_set = true;
}

static void a_rename_is_asked_again_only_for_the_same_file(void)
{
	static Rig r;
	WfEditOutcome oc;
	size_t at = 0;

	open_rig(&r);
	for (size_t i = 0; i < r.app.cur.dir->n; i++) {
		if (!wcscmp(r.app.cur.dir->e[i].name, L"file10.txt")) {
			at = i;
		}
	}
	memset(&oc, 0, sizeof oc);
	oc.st = WF_E_OS;
	oc.error = 5; /* access denied is "maybe": not noted */
	set_rename_note(&r, (uint8_t)(at + 20));
	wf_app_lock_rename_failed(&r.app, &oc);
	CHECK(WF_LOCKOP_NONE == r.app.lock.op);
	oc.error = 32;
	wf_app_lock_rename_failed(&r.app, &oc);
	CHECK(WF_LOCKOP_RENAME == r.app.lock.op);
	CHECK(!wcscmp(r.app.lock.path, L"C:\\Users\\dev\\file10.txt"));
	ch(&r.app, 'L');
	ch(&r.app, 't');
	/* the prompt is open with the name that was asked for; nothing is renamed */
	CHECK(!strcmp(r.app.mode->name, "NAME"));
	CHECK(!wcscmp(r.app.path_input, L"renamed.txt"));
	wf_app_destroy(&r.app);

	/* the file at that place was replaced by another: refused */
	open_rig(&r);
	memset(&oc, 0, sizeof oc);
	oc.st = WF_E_OS;
	oc.error = 32;
	set_rename_note(&r, 200); /* not the id the listing has now */
	wf_app_lock_rename_failed(&r.app, &oc);
	CHECK(WF_LOCKOP_RENAME == r.app.lock.op);
	ch(&r.app, 'L');
	ch(&r.app, 't');
	CHECK(&WF_MODE_NORMAL == r.app.mode);
	CHECK(strstr(r.app.msg, "not the file that failed") ||
	        strstr(r.app.msg, "replaced"));
	wf_app_destroy(&r.app);
}

static void a_move_stopped_on_one_volume_keeps_what_was_not_moved(void)
{
	static Rig r;
	WfEditOutcome oc;
	WfCopySpec s = spec_of(WF_CJ_COPY);

	open_rig(&r);
	memset(&oc, 0, sizeof oc);
	oc.st = WF_E_OS;
	oc.error = 33;
	CHECK(WF_OK == wf_slot_clone(&r.app.move.slot, &s.slot));
	r.app.move.dest = _wcsdup(L"C:\\Pics");
	wf_app_lock_move_failed(&r.app, &oc, 1);
	CHECK(WF_LOCKOP_MOVE_SAME == r.app.lock.op);
	CHECK(!wcscmp(r.app.lock.path, L"C:\\Users\\dev\\b.txt"));
	CHECK(2 == r.app.lock.slot.count &&
	        !wcscmp(r.app.lock.slot.src[0].name, L"b.txt"));
	ch(&r.app, 'L');
	ch(&r.app, 't');
	CHECK(WF_XFER_MOVE == r.app.slot.mode && 2 == r.app.slot.count);
	CHECK(0 == r.fake.submits);
	wf_slot_free(&r.app.move.slot);
	free(r.app.move.dest);
	r.app.move.dest = NULL;
	wf_copyspec_free(&s);
	wf_app_destroy(&r.app);
}

static void the_editor_save_is_tried_again_by_the_editor(void)
{
	static Rig r;
	WfLockHolder one = holder(5824, L"Code.exe");

	open_rig(&r);
	wcscpy_s(r.fake.editor_file, 64, L"C:\\Users\\dev\\file10.txt");
	r.app.mode = &WF_MODE_EDITOR;
	wf_app_editor_lock_look(&r.app);
	CHECK(&WF_MODE_LOCK == r.app.mode && WF_LOCKOP_EDITOR == r.app.lock.op);
	CHECK(!wcscmp(r.fake.path, L"C:\\Users\\dev\\file10.txt"));
	answer_query(&r, &one, 1);
	key(&r.app, &ESC);
	CHECK(&WF_MODE_EDITOR == r.app.mode && 0 == r.fake.editor_saves);
	wf_app_editor_lock_look(&r.app);
	r.fake.gen = r.app.lock.gen;
	answer_query(&r, &one, 1);
	ch(&r.app, 't');
	CHECK(1 == r.fake.editor_saves && &WF_MODE_EDITOR == r.app.mode);
	wf_app_destroy(&r.app);
}

static void without_a_service_everything_says_so(void)
{
	static Rig r;

	open_rig(&r);
	wf_app_set_lock(&r.app, (WfLockPort){0});
	ch(&r.app, 'j');
	ch(&r.app, 'L');
	CHECK(&WF_MODE_NORMAL == r.app.mode &&
	        strstr(r.app.msg, "not available"));
	wf_app_destroy(&r.app);
}

static void the_status_lines_fit_in_both_languages(void)
{
	static const WfMsg ids[] = {MSG_OPS_IN_USE, MSG_LOCK_N_BUSY,
	        MSG_LOCK_TOO_SMALL, MSG_LOCK_UNAVAILABLE, MSG_LOCK_NO_FILE,
	        MSG_LOCK_RETRY_NONE, MSG_LOCK_RETRY_STAGED,
	        MSG_LOCK_RETRY_REPLACED, MSG_LOCK_RETRY_GO,
	        MSG_LOCK_RETRY_REFUSED, MSG_LOCK_N_NOPICK};
	static const int langs[] = {WF_UILANG_EN, WF_UILANG_JA};

	for (size_t l = 0; l < 2; l++) {
		for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
			char out[400];

			(void)wf_msgf_in((WfUiLang)langs[l], out, sizeof out,
			        ids[i],
			        (WfMsgArg[]){WF_ARG_S("123"), WF_ARG_S("x")},
			        2);
			CHECK(strlen(out) <= 95);
		}
		/* the lines of the questions are not cut by the confirmation box */
		{
			static const WfMsg q[] = {MSG_LOCK_EQ1_TITLE,
			        MSG_LOCK_EQ_LOSS, MSG_LOCK_EQ_NOTE,
			        MSG_LOCK_EQ2_TITLE, MSG_LOCK_CQ_TITLE,
			        MSG_LOCK_CQ_NOTE};

			for (size_t i = 0; i < sizeof q / sizeof q[0]; i++) {
				const char *t =
				        wf_msg_in((WfUiLang)langs[l], q[i]);

				CHECK(strlen(t) < WF_CONFIRM_LINE_BYTES);
			}
		}
	}
}

int main(void)
{
	wf_i18n_set_language(WF_UILANG_EN);
	by_hand_asks_for_the_file_under_the_cursor();
	an_empty_answer_is_not_a_claim_of_no_lock();
	a_stale_answer_changes_nothing_and_requests_do_not_pile_up();
	closing_is_asked_once_and_never_becomes_ending();
	a_close_answer_after_the_window_was_left_reaches_the_status_line();
	ending_needs_two_questions_and_every_cancel_does_nothing();
	a_window_that_shrinks_between_the_questions_stops_it();
	a_worker_that_is_busy_refuses_and_nothing_is_assumed();
	only_a_lock_failure_is_noted();
	a_copy_is_put_back_at_the_paste_and_nothing_starts();
	a_delete_is_asked_again_with_the_ordinary_question();
	a_rename_is_asked_again_only_for_the_same_file();
	a_move_stopped_on_one_volume_keeps_what_was_not_moved();
	the_editor_save_is_tried_again_by_the_editor();
	without_a_service_everything_says_so();
	the_status_lines_fit_in_both_languages();
	return TEST_RESULT();
}
