/* The bulk rename in the application (DR-DRAFT-022, UI): the key and the places
 * it refuses, the pattern fields and their checking, the preview with its
 * verdicts, the one question and what must hold before anything is handed to the
 * worker, a run's cancel and its results in true words, the cursor afterwards, a
 * listing that changed under the preview, the release gate, the question about
 * journals left behind, and the undo stack left alone. The engine and the worker
 * are a fake: no disk, no thread. */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "edit/wf_ekeymap.h"
#include "input/wf_commands.h"
#include "input/wf_keymap.h"
#include "path/wf_path.h"
#include "ui/wf_app_priv.h"
#include "wf_test.h"

typedef struct
{
	uint32_t gen;
	wchar_t path[260];
	unsigned submits;
} Port;

static WfStatus p_submit(
        void *ctx, uint32_t gen, const wchar_t *path, const wchar_t *parent)
{
	Port *p = ctx;

	(void)parent;
	p->gen = gen;
	p->submits++;
	wcsncpy_s(p->path, 260, path, _TRUNCATE);
	return WF_OK;
}

static const wchar_t *const FILES[] = {
        L"a.txt", L"b.txt", L"c.txt", L"d.txt", L"e.txt"};

/* Answers the newest request. `ids`: the entries get a file identity (and so
 * does the folder); `flags` marks every entry. */
static void deliver_with(WfApp *a,
        Port *p,
        const wchar_t *const *names,
        size_t n,
        bool ids,
        uint32_t flags)
{
	WfDir *d = wf_dir_new();

	CHECK(d);
	for (size_t i = 0; i < n; i++) {
		CHECK(WF_OK ==
		        wf_dir_add(d, names[i], wcslen(names[i]), flags, 0, 0));
	}
	CHECK(WF_OK == wf_dir_sort(d, WF_SORT_NAME, 0));
	if (ids) {
		for (size_t i = 0; i < d->n; i++) {
			d->e[i].identity.kind = WF_ID_STRONG;
			d->e[i].identity.volume = 7;
			d->e[i].identity.id[0] = (uint8_t)(i + 1);
		}
		d->identity.kind = WF_ID_STRONG;
		d->identity.volume = 7;
		d->identity.id[0] = 0xD1;
	}
	wf_app_on_listing(a, p->gen, WF_OK, d, NULL);
}

static void deliver(WfApp *a, Port *p)
{
	deliver_with(a, p, FILES, sizeof FILES / sizeof *FILES, true, 0);
}

typedef struct
{
	unsigned prepares, runs, recovers, cancels, frees;
	WfBxReport pre; /* what prepare reports; pre.pre PLAN with refuse_row */
	int refuse_row; /* -1: none */
	WfBatchWhy refuse_why;
	bool force_ready; /* ready() says yes whatever the plan says */
	bool never_ready;
	bool pre_lies; /* pre says READY although a row is refused */
	WfBatchPlan plan;
	WfBatchRow row[32];
	WfBatchStep step[32];
	size_t n_src;
	wchar_t src_path[32][64];
	wchar_t src_to[32][64];
	WfFileId src_id[32];
	uint32_t run_gen, rec_gen;
	WfStatus run_st;
	size_t unfinished, progress;
	char token; /* its address is the "job" */
	bool job_live;
} Fake;

static struct WfBatchJob *f_prepare(
        void *ctx, const WfBxSource *src, size_t n, WfBxReport *rep)
{
	Fake *f = ctx;
	size_t steps = 0, skipped = 0;

	f->prepares++;
	f->n_src = n;
	for (size_t i = 0; i < n && i < 32; i++) {
		const wchar_t *leaf = wcsrchr(src[i].path, L'\\');

		wcscpy_s(f->src_path[i], 64, src[i].path);
		wcscpy_s(f->src_to[i], 64, src[i].to_name);
		f->src_id[i] = src[i].expected;
		f->row[i].why = !wcscmp(leaf + 1, src[i].to_name)
		        ? WF_BP_UNCHANGED
		        : WF_BP_OK;
		skipped += WF_BP_UNCHANGED == f->row[i].why;
		if (WF_BP_OK == f->row[i].why) {
			f->step[steps].item = (uint32_t)i;
			f->step[steps].temp = false;
			steps++;
		}
	}
	memset(&f->plan, 0, sizeof f->plan);
	f->plan.row = f->row;
	f->plan.n = n;
	f->plan.step = f->step;
	f->plan.n_steps = steps;
	f->plan.n_skipped = skipped;
	memset(rep, 0, sizeof *rep);
	rep->pre = WF_BX_PRE_READY;
	if (f->refuse_row >= 0) {
		f->row[f->refuse_row].why = f->refuse_why;
		f->plan.n_refused = 1;
		f->plan.n_skipped = skipped - (skipped > 0 && 0);
		rep->pre = WF_BX_PRE_PLAN;
		rep->pre_item = (size_t)f->refuse_row;
	}
	if (f->pre_lies) {
		rep->pre = WF_BX_PRE_READY;
	}
	if (f->pre.pre != WF_BX_PRE_READY) {
		rep->pre = f->pre.pre;
		rep->pre_item = f->pre.pre_item;
		rep->error = f->pre.error;
	}
	rep->steps_total = steps;
	f->job_live = true;
	return (struct WfBatchJob *)&f->token;
}

static void f_free(void *ctx, struct WfBatchJob *job)
{
	Fake *f = ctx;

	CHECK(job == (struct WfBatchJob *)&f->token);
	f->frees++;
	f->job_live = false;
}

static bool f_ready(void *ctx, const struct WfBatchJob *job)
{
	Fake *f = ctx;

	(void)job;
	if (f->never_ready) {
		return false;
	}
	return f->force_ready || (0 == f->plan.n_refused && f->job_live);
}

static const WfBatchPlan *f_plan(void *ctx, const struct WfBatchJob *job)
{
	Fake *f = ctx;

	(void)job;
	return &f->plan;
}

static WfStatus f_run(void *ctx, uint32_t gen, struct WfBatchJob *job)
{
	Fake *f = ctx;

	(void)job;
	if (WF_OK != f->run_st) {
		return f->run_st;
	}
	f->runs++;
	f->run_gen = gen;
	return WF_OK;
}

static WfStatus f_recover(void *ctx, uint32_t gen)
{
	Fake *f = ctx;

	f->recovers++;
	f->rec_gen = gen;
	return WF_OK;
}

static void f_cancel(void *ctx)
{
	((Fake *)ctx)->cancels++;
}

static size_t f_progress(void *ctx)
{
	return ((Fake *)ctx)->progress;
}

static size_t f_unfinished(void *ctx)
{
	return ((Fake *)ctx)->unfinished;
}

static WfBatchPort fake_port(Fake *f)
{
	WfBatchPort p = {f, f_prepare, f_free, f_ready, f_plan, f_run,
	        f_recover, f_cancel, f_progress, f_unfinished};

	return p;
}

static WfStatus e_submit(void *ctx, uint32_t gen, const WfEditRequest *req)
{
	(void)ctx;
	(void)gen;
	(void)req;
	return WF_OK;
}

typedef struct
{
	WfApp a;
	Port p;
	Fake f;
	bool live;
} T;

static void t_open(T *t, bool with_port, bool with_gate)
{
	memset(t, 0, sizeof *t);
	t->f.refuse_row = -1;
	wf_i18n_set_language(WF_UILANG_EN);
	CHECK(WF_OK ==
	        wf_app_init(&t->a, 100, 30, (WfFsPort){&t->p, p_submit},
	                L"C:\\Work"));
	deliver(&t->a, &t->p);
	if (with_port) {
		wf_app_set_batch(&t->a, fake_port(&t->f));
	}
	if (with_gate) {
		wf_app_set_edit(&t->a, (WfEditPort){&t->f, e_submit});
	}
	t->a.now_ms = 1000;
	t->live = true;
}

static void t_close(T *t)
{
	wf_app_destroy(&t->a);
	t->live = false;
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

static void mark(WfApp *a, const wchar_t *name)
{
	CHECK(WF_OK == wf_sel_toggle(&a->sel, a->cwd, name));
}

static const char *row_text(const WfFrame *f, int y)
{
	static char row[1024];

	row[0] = '\0';
	for (int x = 0; x < f->cols; x++) {
		strcat_s(row, sizeof row, f->cells[y * f->cols + x].utf8);
	}
	return row;
}

/* Whether the drawn screen shows `needle` anywhere. */
static bool screen_has(const WfApp *a, const char *needle)
{
	WfFrame *f = wf_frame_new(a->cols, a->rows, &WF_STYLE_BASE);
	bool found = false;

	CHECK(f);
	wf_app_draw(a, f);
	for (int y = 0; y < f->rows && !found; y++) {
		found = NULL != strstr(row_text(f, y), needle);
	}
	wf_frame_free(f);
	return found;
}

static const wchar_t *cur(const WfApp *a)
{
	const WfEntry *e = wf_view_current(&a->cur);

	return e ? e->name : L"";
}

/* The question is on screen and old enough to be answered. */
static void arm(WfApp *a)
{
	wf_app_on_presented(a);
	a->now_ms += 400;
	wf_app_tick(a);
}

/* M, then the pattern keeps {name} unless `tmpl` says otherwise, then Enter. */
static void to_preview(T *t, const wchar_t *tmpl)
{
	WfApp *a = &t->a;

	ch(a, 'M');
	CHECK(&WF_MODE_BATCH == a->mode && WF_BS_PATTERN == a->batch.stage);
	if (tmpl) {
		for (int i = 0; i < 6; i++) {
			key(a, WF_KEY_BACKSPACE);
		}
		type(a, tmpl);
	}
	key(a, WF_KEY_ENTER);
	CHECK(WF_BS_PREVIEW == a->batch.stage);
}

static void to_confirm(T *t)
{
	to_preview(t, L"{n:3}_{name}");
	key(&t->a, WF_KEY_ENTER);
	CHECK(&WF_MODE_CONFIRM == t->a.mode);
}

static void to_running(T *t)
{
	to_confirm(t);
	arm(&t->a);
	ch(&t->a, 'y');
	CHECK(WF_BS_RUNNING == t->a.batch.stage && t->a.batch.busy);
}

static WfBxDone done(const T *t, WfBxResult r)
{
	WfBxDone d;

	memset(&d, 0, sizeof d);
	d.gen = t->f.run_gen;
	d.rep.result = r;
	d.rep.steps_total = 5;
	d.rep.steps_done = 3;
	d.rep.rolled_back = 3;
	d.rep.failed_step = 3;
	d.rep.error = 32;
	return d;
}

static void m_opens_the_pattern_for_the_marked_entries_in_list_order(void)
{
	T t;

	t_open(&t, true, true);
	mark(&t.a, L"d.txt");
	mark(&t.a, L"b.txt");
	ch(&t.a, 'M');
	CHECK(&WF_MODE_BATCH == t.a.mode && WF_BS_PATTERN == t.a.batch.stage);
	CHECK(2 == t.a.batch.n);
	CHECK(!wcscmp(t.a.batch.item[0].name, L"b.txt") &&
	        !wcscmp(t.a.batch.item[1].name, L"d.txt"));
	CHECK(!wcscmp(t.a.batch.item[1].path, L"C:\\Work\\d.txt"));
	CHECK(!wcscmp(t.a.batch.f_text[WF_BF_TEMPLATE], L"{name}"));
	CHECK(!wcscmp(t.a.batch.f_text[WF_BF_START], L"1") &&
	        !wcscmp(t.a.batch.f_text[WF_BF_STEP], L"1"));
	CHECK(screen_has(&t.a, "Template") && screen_has(&t.a, "{name}"));
	CHECK(screen_has(&t.a, "2 item(s) in C:\\Work"));
	CHECK(0 ==
	        t.f.prepares); /* the plan is made at the preview, not before */
	t_close(&t);
}

static void without_marks_the_entry_under_the_cursor_is_the_item(void)
{
	T t;

	t_open(&t, true, true);
	key(&t.a, WF_KEY_DOWN);
	CHECK(!wcscmp(cur(&t.a), L"b.txt"));
	ch(&t.a, 'M');
	CHECK(&WF_MODE_BATCH == t.a.mode && 1 == t.a.batch.n);
	CHECK(!wcscmp(t.a.batch.item[0].name, L"b.txt"));
	t_close(&t);
}

static void refused(T *t, WfMsg why)
{
	ch(&t->a, 'M');
	CHECK(&WF_MODE_NORMAL == t->a.mode && WF_BS_IDLE == t->a.batch.stage);
	CHECK(0 == t->f.prepares && 0 == t->f.runs);
	CHECK(!strcmp(t->a.msg, wf_msg(why)));
}

static void it_refuses_where_it_must_and_changes_no_mode(void)
{
	T t;

	/* no engine behind it */
	t_open(&t, false, true);
	refused(&t, MSG_BATCH_OFF);
	t_close(&t);

	/* the release gate: no edit port, as for the name forms */
	t_open(&t, true, false);
	refused(&t, MSG_BATCH_GATED);
	t_close(&t);

	/* the listing is being loaded */
	t_open(&t, true, true);
	CHECK(WF_OK == wf_app_navigate(&t.a, L"C:\\Other", NULL, SIZE_MAX));
	CHECK(t.a.want_path);
	refused(&t, MSG_BATCH_LOADING);
	t_close(&t);

	/* inside an archive */
	t_open(&t, true, true);
	free(t.a.cwd);
	t.a.cwd = wf_path_dup_n(L"C:\\Work\\a.zip|dir", 17);
	CHECK(t.a.cwd);
	refused(&t, MSG_AR_READONLY);
	t_close(&t);

	/* an entry without a file identity, and a folder without one */
	t_open(&t, true, true);
	t.a.cur.dir->e[0].identity.kind = WF_ID_NONE;
	ch(&t.a, 'M');
	CHECK(&WF_MODE_NORMAL == t.a.mode && 0 == t.f.prepares);
	CHECK(NULL != strstr(t.a.msg, "a.txt") &&
	        NULL != strstr(t.a.msg, "BLOCKED"));
	t_close(&t);
	t_open(&t, true, true);
	t.a.cur.dir->identity.kind = WF_ID_NONE;
	refused(&t, MSG_OPS_BLOCKED_IDENTITY);
	t_close(&t);

	/* a link */
	t_open(&t, true, true);
	t.a.cur.dir->e[0].flags |= WF_ENT_LINK;
	ch(&t.a, 'M');
	CHECK(&WF_MODE_NORMAL == t.a.mode && 0 == t.f.prepares);
	CHECK(NULL != strstr(t.a.msg, "a.txt"));
	t_close(&t);

	/* an entry on another volume than its folder */
	t_open(&t, true, true);
	t.a.cur.dir->e[0].identity.volume = 8;
	ch(&t.a, 'M');
	CHECK(&WF_MODE_NORMAL == t.a.mode && 0 == t.f.prepares);
	t_close(&t);
}

static void a_selection_of_names_that_are_not_there_is_not_the_cursor_entry(
        void)
{
	T t;

	t_open(&t, true, true);
	CHECK(WF_OK == wf_sel_toggle(&t.a.sel, t.a.cwd, L"gone.txt"));
	refused(&t, MSG_BATCH_NONE);
	t_close(&t);
}

static void more_items_than_one_batch_may_hold_are_refused_with_the_number(void)
{
	T t;
	size_t n = WF_BATCH_MAX_ITEMS + 1;
	WfDir *d = wf_dir_new();
	char want[96];

	CHECK(d);
	memset(&t, 0, sizeof t);
	t.f.refuse_row = -1;
	CHECK(WF_OK ==
	        wf_app_init(&t.a, 100, 30, (WfFsPort){&t.p, p_submit},
	                L"C:\\Work"));
	for (size_t i = 0; i < n; i++) {
		wchar_t nm[24];

		swprintf(nm, 24, L"f%06zu", i);
		CHECK(WF_OK == wf_dir_add(d, nm, wcslen(nm), 0, 0, 0));
		d->e[i].identity.kind = WF_ID_STRONG;
		d->e[i].identity.volume = 7;
	}
	d->identity.kind = WF_ID_STRONG;
	d->identity.volume = 7;
	wf_app_on_listing(&t.a, t.p.gen, WF_OK, d, NULL);
	wf_app_set_batch(&t.a, fake_port(&t.f));
	wf_app_set_edit(&t.a, (WfEditPort){&t.f, e_submit});
	for (size_t i = 0; i < t.a.cur.dir->n; i++) {
		CHECK(WF_OK ==
		        wf_sel_toggle(
		                &t.a.sel, t.a.cwd, t.a.cur.dir->e[i].name));
	}
	ch(&t.a, 'M');
	(void)wf_msgf(want, sizeof want, MSG_BATCH_TOO_MANY,
	        (WfMsgArg[]){WF_ARG_N(n), WF_ARG_N(WF_BATCH_MAX_ITEMS)}, 2);
	CHECK(&WF_MODE_NORMAL == t.a.mode && !strcmp(t.a.msg, want));
	CHECK(NULL != strstr(t.a.msg, "20001") &&
	        NULL != strstr(t.a.msg, "20000"));
	t_close(&t);
}

static void the_fields_take_text_and_are_checked_as_you_type(void)
{
	T t;
	WfApp *a;

	t_open(&t, true, true);
	a = &t.a;
	mark(a, L"b.txt");
	ch(a, 'M');
	CHECK(0 == a->batch.pat_err);
	/* template: an unknown word in braces says where */
	for (int i = 0; i < 6; i++) {
		key(a, WF_KEY_BACKSPACE);
	}
	type(a, L"ab{x}");
	CHECK(1 == a->batch.pat_err && 2 == a->batch.pat_pos);
	CHECK(screen_has(a, "Template error at position 3"));
	key(a, WF_KEY_ENTER); /* nothing happens while it is wrong */
	CHECK(WF_BS_PATTERN == a->batch.stage && 0 == t.f.prepares);
	key(a, WF_KEY_BACKSPACE);
	key(a, WF_KEY_BACKSPACE);
	key(a, WF_KEY_BACKSPACE);
	CHECK(0 == a->batch.pat_err);
	/* Tab goes on, Shift+Tab back */
	key(a, WF_KEY_TAB);
	CHECK(WF_BF_FIND == a->batch.field);
	type(a, L"b");
	key(a, WF_KEY_TAB);
	CHECK(WF_BF_REPLACE == a->batch.field);
	type(a, L"B");
	key(a, WF_KEY_SHIFT_TAB);
	CHECK(WF_BF_FIND == a->batch.field);
	CHECK(!wcscmp(a->batch.f_text[WF_BF_FIND], L"b") &&
	        !wcscmp(a->batch.f_text[WF_BF_REPLACE], L"B"));
	/* the number fields take digits and a leading minus only */
	key(a, WF_KEY_TAB);
	key(a, WF_KEY_TAB);
	CHECK(WF_BF_START == a->batch.field);
	type(a, L"x");
	CHECK(!wcscmp(a->batch.f_text[WF_BF_START], L"1"));
	key(a, WF_KEY_BACKSPACE);
	CHECK(2 == a->batch.pat_err); /* empty is not a number */
	CHECK(screen_has(a, "First number: digits only"));
	type(a, L"-5");
	CHECK(0 == a->batch.pat_err);
	type(a, L"-");
	CHECK(!wcscmp(a->batch.f_text[WF_BF_START], L"-5"));
	key(a, WF_KEY_TAB);
	CHECK(WF_BF_STEP == a->batch.field);
	key(a, WF_KEY_BACKSPACE);
	CHECK(3 == a->batch.pat_err);
	type(a, L"2");
	CHECK(0 == a->batch.pat_err);
	/* the cases are chosen, not typed */
	key(a, WF_KEY_TAB);
	CHECK(WF_BF_STEM_CASE == a->batch.field);
	key(a, WF_KEY_RIGHT);
	CHECK(WF_BULK_CASE_LOWER == a->batch.stem_case);
	ch(a, 'u');
	CHECK(WF_BULK_CASE_UPPER == a->batch.stem_case);
	key(a, WF_KEY_LEFT);
	CHECK(WF_BULK_CASE_LOWER == a->batch.stem_case);
	key(a, WF_KEY_TAB);
	ch(a, ' ');
	CHECK(WF_BULK_CASE_LOWER == a->batch.ext_case);
	key(a, WF_KEY_TAB); /* wraps to the first */
	CHECK(WF_BF_TEMPLATE == a->batch.field);
	t_close(&t);
}

static void non_bmp_text_is_kept_whole_and_backspace_takes_the_pair(void)
{
	T t;

	t_open(&t, true, true);
	ch(&t.a, 'M');
	key(&t.a, WF_KEY_TAB);
	ch(&t.a, 0x1F600);
	CHECK(2 == wcslen(t.a.batch.f_text[WF_BF_FIND]));
	key(&t.a, WF_KEY_BACKSPACE);
	CHECK(0 == wcslen(t.a.batch.f_text[WF_BF_FIND]));
	ch(&t.a, 0x7);    /* a control character is not text */
	ch(&t.a, 0xD800); /* a lone surrogate is not a character */
	CHECK(0 == wcslen(t.a.batch.f_text[WF_BF_FIND]));
	t_close(&t);
}

static void esc_on_the_pattern_changes_nothing(void)
{
	T t;

	t_open(&t, true, true);
	mark(&t.a, L"a.txt");
	ch(&t.a, 'M');
	type(&t.a, L"zz");
	key(&t.a, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == t.a.mode && WF_BS_IDLE == t.a.batch.stage);
	CHECK(0 == t.f.prepares && 0 == t.f.runs && 0 == t.f.frees);
	CHECK(0 == t.a.batch.n && NULL == t.a.batch.item);
	CHECK(1 == wf_sel_count(&t.a.sel, t.a.cwd)); /* the marks stay */
	ch(&t.a, 'M'); /* and it can be opened again at once */
	CHECK(&WF_MODE_BATCH == t.a.mode);
	t_close(&t);
}

static void
the_preview_shows_before_after_counts_and_hands_the_names_to_the_plan(void)
{
	T t;

	t_open(&t, true, true);
	mark(&t.a, L"b.txt");
	mark(&t.a, L"d.txt");
	to_preview(&t, L"{n:3}_{name}");
	CHECK(1 == t.f.prepares && 2 == t.f.n_src);
	CHECK(!wcscmp(t.f.src_path[0], L"C:\\Work\\b.txt") &&
	        !wcscmp(t.f.src_to[0], L"001_b.txt"));
	CHECK(!wcscmp(t.f.src_path[1], L"C:\\Work\\d.txt") &&
	        !wcscmp(t.f.src_to[1], L"002_d.txt"));
	CHECK((t.f.src_id[0].kind == WF_ID_STRONG) &&
	        7 == t.f.src_id[0].volume);
	CHECK(2 == t.a.batch.n_renames);
	CHECK(screen_has(&t.a, "2 items: 2 to rename, 0 unchanged, 0 refused"));
	CHECK(screen_has(&t.a, "b.txt -> 001_b.txt"));
	CHECK(screen_has(&t.a, "d.txt -> 002_d.txt"));
	CHECK(screen_has(&t.a, "Ready. Enter runs it after one question"));
	t_close(&t);
}

static void the_start_and_step_and_cases_reach_the_names(void)
{
	T t;
	WfApp *a;

	t_open(&t, true, true);
	a = &t.a;
	mark(a, L"a.txt");
	mark(a, L"b.txt");
	ch(a, 'M');
	for (int i = 0; i < 6; i++) {
		key(a, WF_KEY_BACKSPACE);
	}
	type(a, L"IMG_{n:2}");
	key(a, WF_KEY_TAB);
	key(a, WF_KEY_TAB);
	key(a, WF_KEY_TAB);
	key(a, WF_KEY_BACKSPACE);
	type(a, L"10");
	key(a, WF_KEY_TAB);
	key(a, WF_KEY_BACKSPACE);
	type(a, L"5");
	key(a, WF_KEY_TAB);
	ch(a, 'l');
	key(a, WF_KEY_TAB);
	ch(a, 'u');
	key(a, WF_KEY_ENTER);
	CHECK(WF_BS_PREVIEW == a->batch.stage);
	CHECK(!wcscmp(t.f.src_to[0], L"IMG_10.TXT") &&
	        !wcscmp(t.f.src_to[1], L"IMG_15.TXT"));
	t_close(&t);
}

static void unchanged_names_are_skipped_and_all_unchanged_cannot_run(void)
{
	T t;

	t_open(&t, true, true);
	mark(&t.a, L"a.txt");
	to_preview(&t, NULL); /* {name}: every name stays */
	CHECK(0 == t.a.batch.n_renames && 0 == t.f.plan.n_steps);
	CHECK(screen_has(&t.a, "Nothing to do"));
	key(&t.a, WF_KEY_ENTER);
	CHECK(&WF_MODE_BATCH == t.a.mode && 0 == t.f.runs);
	t_close(&t);
}

static void a_refused_row_is_shown_with_its_reason_and_nothing_can_run(void)
{
	T t;

	t_open(&t, true, true);
	t.f.refuse_row = 1;
	t.f.refuse_why = WF_BP_EXISTS;
	mark(&t.a, L"a.txt");
	mark(&t.a, L"b.txt");
	to_preview(&t, L"x{n}");
	CHECK(screen_has(&t.a, "2 items: 1 to rename, 0 unchanged, 1 refused"));
	CHECK(screen_has(
	        &t.a, wf_msg_reason("a file or folder of that name exists")));
	CHECK(screen_has(&t.a, "Cannot run: 1 row(s) refused"));
	key(&t.a, WF_KEY_ENTER);
	CHECK(&WF_MODE_BATCH == t.a.mode && WF_BS_PREVIEW == t.a.batch.stage);
	CHECK(0 == t.f.runs);
	/* even when the engine's own ready() were to say yes, the refused row
	 * is looked at by the screen */
	t_close(&t);
	t_open(&t, true, true);
	t.f.refuse_row = 1;
	t.f.refuse_why = WF_BP_DUP_TARGET;
	t.f.force_ready = true; /* ready() and pre both claim it is fine ... */
	t.f.pre_lies = true;
	mark(&t.a, L"a.txt");
	mark(&t.a, L"b.txt");
	to_preview(&t, L"x{n}");
	key(&t.a, WF_KEY_ENTER); /* ... the refused row is still looked at */
	CHECK(&WF_MODE_BATCH == t.a.mode && 0 == t.f.runs);
	t_close(&t);
}

static void a_job_that_is_not_ready_cannot_run_and_the_reason_is_shown(void)
{
	T t;

	t_open(&t, true, true);
	t.f.pre.pre = WF_BX_PRE_CHANGED;
	t.f.pre.pre_item = 0;
	mark(&t.a, L"a.txt");
	to_preview(&t, L"x{n}");
	CHECK(screen_has(
	        &t.a, "Cannot run: a.txt: it is not the file the list showed"));
	key(&t.a, WF_KEY_ENTER);
	CHECK(&WF_MODE_BATCH == t.a.mode && 0 == t.f.runs);
	t_close(&t);
	/* ready() itself saying no */
	t_open(&t, true, true);
	t.f.never_ready = true;
	mark(&t.a, L"a.txt");
	to_preview(&t, L"x{n}");
	key(&t.a, WF_KEY_ENTER);
	CHECK(&WF_MODE_BATCH == t.a.mode && 0 == t.f.runs);
	t_close(&t);
}

static void a_name_the_pattern_cannot_make_keeps_the_pattern_on_screen(void)
{
	T t;
	WfApp *a;
	wchar_t longname[80];
	const wchar_t *names[2];

	t_open(&t, true, true);
	a = &t.a;
	for (int i = 0; i < 60; i++) {
		longname[i] = L'a';
	}
	wcscpy_s(longname + 60, 20, L".txt");
	names[0] = L"b.txt";
	names[1] = longname;
	CHECK(WF_OK == wf_app_navigate(a, L"C:\\Work", NULL, SIZE_MAX));
	deliver_with(a, &t.p, names, 2, true, 0);
	mark(a, L"b.txt");
	mark(a, longname);
	ch(a, 'M');
	CHECK(&WF_MODE_BATCH == a->mode && 2 == a->batch.n);
	key(a, WF_KEY_TAB);
	type(a, L"a"); /* find "a" ... */
	key(a, WF_KEY_TAB);
	for (int i = 0; i < 90; i++) {
		ch(a, 'x'); /* ... replace by 90 letters: 60 of them are 5,400 */
	}
	CHECK(0 == a->batch.pat_err);
	key(a, WF_KEY_ENTER);
	/* one row cannot be made at all: no plan is made, the fields stay */
	CHECK(WF_BS_PATTERN == a->batch.stage && 0 == t.f.prepares);
	CHECK(NULL != strstr(a->msg, "aaaaaaaa") &&
	        NULL != strstr(a->msg, "cannot make a name"));
	CHECK(screen_has(a, "cannot make a name"));
	t_close(&t);
}

static void scrolling_keeps_the_top_inside_the_list(void)
{
	T t;

	t_open(&t, true, true);
	for (size_t i = 0; i < 5; i++) {
		mark(&t.a, FILES[i]);
	}
	to_preview(&t, L"{n}");
	t.a.rows = 10;
	key(&t.a, WF_KEY_END);
	CHECK(t.a.batch.top <= 5);
	key(&t.a, WF_KEY_HOME);
	CHECK(0 == t.a.batch.top);
	key(&t.a, WF_KEY_UP);
	CHECK(0 == t.a.batch.top);
	key(&t.a, WF_KEY_PGDN);
	key(&t.a, WF_KEY_PGUP);
	CHECK(0 == t.a.batch.top);
	t_close(&t);
}

static void esc_on_the_preview_goes_back_to_the_fields_and_lets_the_plan_go(
        void)
{
	T t;

	t_open(&t, true, true);
	mark(&t.a, L"a.txt");
	to_preview(&t, L"x{n}");
	key(&t.a, WF_KEY_ESC);
	CHECK(WF_BS_PATTERN == t.a.batch.stage && &WF_MODE_BATCH == t.a.mode);
	CHECK(1 == t.f.frees && NULL == t.a.batch.job);
	key(&t.a,
	        WF_KEY_ENTER); /* the plan is made again from the same fields */
	CHECK(WF_BS_PREVIEW == t.a.batch.stage && 2 == t.f.prepares);
	key(&t.a, WF_KEY_ESC);
	key(&t.a, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == t.a.mode && WF_BS_IDLE == t.a.batch.stage);
	CHECK(0 == t.f.runs && t.f.frees == t.f.prepares);
	t_close(&t);
}

static void nothing_runs_without_the_question_and_its_answer(void)
{
	T t;

	t_open(&t, true, true);
	mark(&t.a, L"a.txt");
	to_preview(&t, L"x{n}");
	key(&t.a, WF_KEY_ENTER);
	CHECK(&WF_MODE_CONFIRM == t.a.mode && 0 == t.f.runs);
	CHECK(screen_has(&t.a, "BULK RENAME") &&
	        screen_has(&t.a, "1 to rename, 0 unchanged"));
	CHECK(screen_has(&t.a, "Folder: C:\\Work"));
	/* typed ahead of the question: it is not answered */
	ch(&t.a, 'y');
	CHECK(&WF_MODE_CONFIRM == t.a.mode && 0 == t.f.runs);
	wf_app_on_presented(&t.a);
	t.a.now_ms += 100; /* inside the guard */
	wf_app_tick(&t.a);
	ch(&t.a, 'y');
	CHECK(&WF_MODE_CONFIRM == t.a.mode && 0 == t.f.runs);
	t.a.now_ms += 400;
	wf_app_tick(&t.a);
	/* Enter never confirms */
	key(&t.a, WF_KEY_ENTER);
	CHECK(&WF_MODE_NORMAL == t.a.mode && 0 == t.f.runs);
	wf_app_tick(&t.a);
	CHECK(WF_BS_IDLE == t.a.batch.stage && NULL == t.a.batch.job);
	CHECK(1 ==
	        t.f.frees); /* the plan was let go: no folder stays held open */
	/* n and Esc do not either */
	to_confirm(&t);
	arm(&t.a);
	ch(&t.a, 'n');
	CHECK(&WF_MODE_NORMAL == t.a.mode && 0 == t.f.runs);
	t_close(&t);
	t_open(&t, true, true);
	mark(&t.a, L"a.txt");
	to_confirm(&t);
	key(&t.a, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == t.a.mode && 0 == t.f.runs);
	t_close(&t);
}

static void yes_hands_the_job_to_the_worker_once(void)
{
	T t;

	t_open(&t, true, true);
	mark(&t.a, L"a.txt");
	mark(&t.a, L"b.txt");
	to_running(&t);
	CHECK(1 == t.f.runs && 0 != t.f.run_gen);
	CHECK(NULL == t.a.batch.job &&
	        0 == t.f.frees); /* the worker owns it now */
	CHECK(&WF_MODE_BATCH == t.a.mode);
	CHECK(screen_has(&t.a, "Renaming: 0 of 2 steps done"));
	t.f.progress = 1;
	CHECK(WF_ACT_REDRAW == wf_app_tick(&t.a));
	CHECK(screen_has(&t.a, "Renaming: 1 of 2 steps done"));
	CHECK(0 == wf_app_batch_timeout(&t.a) - 100); /* it looks again soon */
	/* a second M while it runs is refused */
	t.a.mode = &WF_MODE_NORMAL;
	ch(&t.a, 'M');
	CHECK(!strcmp(t.a.msg, wf_msg(MSG_BATCH_BUSY)) && 1 == t.f.runs);
	t.a.mode = &WF_MODE_BATCH;
	t_close(&t);
}

static void a_worker_that_refuses_leaves_the_names_alone_and_says_busy(void)
{
	T t;

	t_open(&t, true, true);
	t.f.run_st = WF_E_INVAL;
	mark(&t.a, L"a.txt");
	to_confirm(&t);
	arm(&t.a);
	ch(&t.a, 'y');
	CHECK(0 == t.f.runs && !t.a.batch.busy);
	CHECK(!strcmp(t.a.msg, wf_msg(MSG_BATCH_BUSY)));
	CHECK(&WF_MODE_BATCH == t.a.mode && WF_BS_PREVIEW == t.a.batch.stage);
	t_close(&t);
}

static void esc_during_the_run_asks_the_worker_to_stop_once(void)
{
	T t;

	t_open(&t, true, true);
	mark(&t.a, L"a.txt");
	to_running(&t);
	key(&t.a, WF_KEY_ESC);
	CHECK(1 == t.f.cancels && t.a.batch.cancelling);
	CHECK(&WF_MODE_BATCH == t.a.mode && WF_BS_RUNNING == t.a.batch.stage);
	CHECK(screen_has(&t.a, "being put back"));
	key(&t.a, WF_KEY_ESC);
	CHECK(1 == t.f.cancels);
	ch(&t.a, 'q'); /* the screen is not left while it runs */
	CHECK(&WF_MODE_BATCH == t.a.mode && t.a.batch.busy);
	t_close(&t);
}

static void leaving_asks_while_a_batch_runs(void)
{
	T t;
	WfAction r;

	t_open(&t, true, true);
	mark(&t.a, L"a.txt");
	to_running(&t);
	r = wf_app_quit_request(&t.a);
	CHECK(WF_ACT_REDRAW == r && &WF_MODE_CONFIRM == t.a.mode);
	CHECK(screen_has(&t.a, "A batch rename is running"));
	t_close(&t);
}

static void the_gate_is_checked_again_before_anything_is_handed_over(void)
{
	T t;

	t_open(&t, true, true);
	mark(&t.a, L"a.txt");
	to_confirm(&t);
	arm(&t.a);
	t.a.edit.submit =
	        NULL; /* the gate closed while the question was open */
	ch(&t.a, 'y');
	CHECK(0 == t.f.runs && !t.a.batch.busy);
	CHECK(&WF_MODE_NORMAL == t.a.mode && WF_BS_IDLE == t.a.batch.stage);
	CHECK(!strcmp(t.a.msg, wf_msg(MSG_BATCH_GATED)));
	CHECK(1 == t.f.frees);
	t_close(&t);
}

static void a_listing_that_changed_under_the_preview_is_planned_again_not_run(
        void)
{
	T t;

	t_open(&t, true, true);
	mark(&t.a, L"a.txt");
	to_confirm(&t);
	/* the folder is read again behind the question (a watcher's refresh) */
	wf_app_refresh(&t.a, false);
	deliver(&t.a, &t.p);
	arm(&t.a);
	ch(&t.a, 'y');
	CHECK(0 == t.f.runs);
	CHECK(2 == t.f.prepares && 1 == t.f.frees);
	CHECK(&WF_MODE_BATCH == t.a.mode && WF_BS_PREVIEW == t.a.batch.stage);
	CHECK(!strcmp(t.a.msg, wf_msg(MSG_BATCH_STALE)));
	/* the person looks at it and confirms again: now it goes */
	key(&t.a, WF_KEY_ENTER);
	CHECK(&WF_MODE_CONFIRM == t.a.mode);
	arm(&t.a);
	ch(&t.a, 'y');
	CHECK(1 == t.f.runs);
	t_close(&t);
}

/* Each way the listing can differ from the one the plan was made from, alone. */
static void any_one_change_of_the_listing_stops_the_run(void)
{
	for (int how = 0; how < 4; how++) {
		T t;

		t_open(&t, true, true);
		mark(&t.a, L"a.txt");
		to_confirm(&t);
		switch (how) {
		case 0: t.a.nav_gen++; break;
		case 1: t.a.cwd_version++; break;
		case 2: t.a.cur.dir->identity.id[0] = 0xEE; break;
		default:
			t.a.want_path = wf_path_dup_n(L"C:\\Work", 7);
			CHECK(t.a.want_path);
			break;
		}
		arm(&t.a);
		ch(&t.a, 'y');
		CHECK(0 == t.f.runs);
		CHECK(2 == t.f.prepares && WF_BS_PREVIEW == t.a.batch.stage);
		t_close(&t);
	}
}

static void done_is_said_only_for_success(void)
{
	static const WfBxResult RES[] = {WF_BX_ROLLED_BACK,
	        WF_BX_RECOVERY_REQUIRED, WF_BX_FAILED_BEFORE};

	for (int lang = WF_UILANG_EN; lang <= WF_UILANG_JA; lang++) {
		const char *ok_word = WF_UILANG_EN == lang ? "Done" : "完了";
		T t;
		WfBxDone d;

		t_open(&t, true, true);
		mark(&t.a, L"a.txt");
		mark(&t.a, L"b.txt");
		to_running(&t);
		wf_i18n_set_language(lang);
		d = done(&t, WF_BX_SUCCESS);
		wf_app_on_batch(&t.a, &d);
		CHECK(WF_BS_RESULT == t.a.batch.stage && !t.a.batch.busy);
		CHECK(0 ==
		        strncmp(t.a.batch.result[0], ok_word, strlen(ok_word)));
		CHECK(NULL != strstr(t.a.batch.result[0], "2"));
		t_close(&t);
		for (size_t r = 0; r < sizeof RES / sizeof *RES; r++) {
			const char *line;

			t_open(&t, true, true);
			mark(&t.a, L"a.txt");
			mark(&t.a, L"b.txt");
			to_running(&t);
			wf_i18n_set_language(lang);
			d = done(&t, RES[r]);
			wf_app_on_batch(&t.a, &d);
			CHECK(WF_BS_RESULT == t.a.batch.stage);
			CHECK(t.a.batch.result_n >= 1);
			line = t.a.batch.result[0];
			CHECK(0 != strncmp(line, ok_word, strlen(ok_word)));
			CHECK(0 != strncmp(t.a.msg, ok_word, strlen(ok_word)));
			CHECK(screen_has(&t.a, line));
			t_close(&t);
		}
	}
	wf_i18n_set_language(WF_UILANG_EN);
}

static void each_result_has_its_own_honest_words_and_counts(void)
{
	T t;
	WfBxDone d;

	t_open(&t, true, true);
	mark(&t.a, L"a.txt");
	to_running(&t);
	d = done(&t, WF_BX_ROLLED_BACK);
	wf_app_on_batch(&t.a, &d);
	CHECK(NULL !=
	        strstr(t.a.batch.result[0],
	                "NOT done: stopped after 3 of 5 steps"));
	CHECK(NULL != strstr(t.a.batch.result[0], "All names are as before"));
	CHECK(NULL != strstr(t.a.batch.result[1], "step 4 failed (Win32 32)"));
	t_close(&t);

	t_open(&t, true, true);
	mark(&t.a, L"a.txt");
	to_running(&t);
	key(&t.a, WF_KEY_ESC);
	d = done(&t, WF_BX_ROLLED_BACK);
	wf_app_on_batch(&t.a, &d);
	CHECK(NULL != strstr(t.a.batch.result[0], "Cancelled: 3 of 5 steps"));
	t_close(&t);

	t_open(&t, true, true);
	mark(&t.a, L"a.txt");
	to_running(&t);
	d = done(&t, WF_BX_RECOVERY_REQUIRED);
	d.rep.not_restored = 2;
	d.rep.rolled_back = 1;
	wf_app_on_batch(&t.a, &d);
	CHECK(NULL !=
	        strstr(t.a.batch.result[0], "INCOMPLETE: 3 steps were done"));
	CHECK(NULL != strstr(t.a.batch.result[0], "1 put back, 2 could not"));
	CHECK(NULL != strstr(t.a.batch.result[2], "The journal was kept"));
	t_close(&t);

	t_open(&t, true, true);
	mark(&t.a, L"a.txt");
	to_running(&t);
	d = done(&t, WF_BX_FAILED_BEFORE);
	wf_app_on_batch(&t.a, &d);
	CHECK(NULL !=
	        strstr(t.a.batch.result[0],
	                "failed before the first rename (Win32 32)"));
	CHECK(NULL != strstr(t.a.batch.result[0], "Nothing was changed"));
	t_close(&t);
}

static void an_answer_that_is_not_the_request_in_flight_is_dropped(void)
{
	T t;
	WfBxDone d;

	t_open(&t, true, true);
	mark(&t.a, L"a.txt");
	to_running(&t);
	d = done(&t, WF_BX_SUCCESS);
	d.gen += 1;
	wf_app_on_batch(&t.a, &d);
	CHECK(t.a.batch.busy && WF_BS_RUNNING == t.a.batch.stage);
	d.gen = t.f.run_gen;
	wf_app_on_batch(&t.a, &d);
	CHECK(!t.a.batch.busy && WF_BS_RESULT == t.a.batch.stage);
	wf_app_on_batch(
	        &t.a, &d); /* a second copy of it: nothing is in flight */
	CHECK(WF_BS_RESULT == t.a.batch.stage);
	t_close(&t);
}

static void
after_success_the_folder_is_read_again_with_the_cursor_on_a_renamed_item(void)
{
	T t;
	WfBxDone d;
	static const wchar_t *const AFTER[] = {
	        L"001_a.txt", L"002_b.txt", L"c.txt", L"d.txt", L"e.txt"};

	t_open(&t, true, true);
	mark(&t.a, L"a.txt");
	mark(&t.a, L"b.txt");
	to_running(&t);
	d = done(&t, WF_BX_SUCCESS);
	t.p.submits = 0;
	wf_app_on_batch(&t.a, &d);
	CHECK(1 == t.p.submits && !wcscmp(t.p.path, L"C:\\Work"));
	CHECK(t.a.want_path && t.a.want_select &&
	        !wcscmp(t.a.want_select, L"001_a.txt"));
	CHECK(0 ==
	        wf_sel_count(
	                &t.a.sel, t.a.cwd)); /* the marks were of old names */
	deliver_with(&t.a, &t.p, AFTER, 5, true, 0);
	CHECK(!wcscmp(cur(&t.a), L"001_a.txt"));
	key(&t.a, WF_KEY_ESC); /* the result closes */
	CHECK(&WF_MODE_NORMAL == t.a.mode && WF_BS_IDLE == t.a.batch.stage);
	t_close(&t);
}

static void after_a_failure_the_folder_is_read_again_and_the_marks_stay(void)
{
	T t;
	WfBxDone d;

	t_open(&t, true, true);
	mark(&t.a, L"a.txt");
	to_running(&t);
	d = done(&t, WF_BX_ROLLED_BACK);
	t.p.submits = 0;
	wf_app_on_batch(&t.a, &d);
	CHECK(1 == t.p.submits);
	CHECK(NULL == t.a.want_select);
	CHECK(1 == wf_sel_count(&t.a.sel, t.a.cwd));
	t_close(&t);
}

static void
a_result_that_arrives_while_another_screen_is_up_goes_to_the_status_line(void)
{
	T t;
	WfBxDone d;

	t_open(&t, true, true);
	mark(&t.a, L"a.txt");
	to_running(&t);
	t.a.mode = &WF_MODE_NORMAL; /* something else has the screen */
	d = done(&t, WF_BX_ROLLED_BACK);
	wf_app_on_batch(&t.a, &d);
	CHECK(&WF_MODE_NORMAL == t.a.mode && WF_BS_IDLE == t.a.batch.stage);
	CHECK(NULL != strstr(t.a.msg, "NOT done"));
	t_close(&t);
}

static void another_screen_taking_over_lets_the_plan_go(void)
{
	T t;

	t_open(&t, true, true);
	mark(&t.a, L"a.txt");
	to_preview(&t, L"x{n}");
	t.a.mode = &WF_MODE_HELP;
	wf_app_tick(&t.a);
	CHECK(WF_BS_IDLE == t.a.batch.stage && NULL == t.a.batch.job);
	CHECK(1 == t.f.frees);
	t_close(&t);
}

static void destroying_the_app_lets_a_plan_that_was_not_run_go(void)
{
	T t;

	t_open(&t, true, true);
	mark(&t.a, L"a.txt");
	to_preview(&t, L"x{n}");
	CHECK(0 == t.f.frees);
	t_close(&t);
	CHECK(1 == t.f.frees);
}

static void journals_left_behind_are_asked_about_and_never_recovered_unasked(
        void)
{
	T t;
	WfBxDone d;

	t_open(&t, true, true);
	t.f.unfinished = 2;
	wf_app_batch_startup(&t.a);
	CHECK(0 == t.f.recovers && &WF_MODE_NORMAL == t.a.mode);
	wf_app_tick(&t.a);
	CHECK(&WF_MODE_CONFIRM == t.a.mode && 0 == t.f.recovers);
	CHECK(screen_has(&t.a, "UNFINISHED BATCH RENAME") &&
	        screen_has(&t.a, "2 journal(s) from an earlier run"));
	/* no is no: nothing is done, and nothing is asked again this session */
	arm(&t.a);
	ch(&t.a, 'n');
	CHECK(&WF_MODE_NORMAL == t.a.mode && 0 == t.f.recovers);
	wf_app_tick(&t.a);
	CHECK(&WF_MODE_NORMAL == t.a.mode && 0 == t.f.recovers);
	t_close(&t);

	t_open(&t, true, true);
	t.f.unfinished = 1;
	wf_app_batch_startup(&t.a);
	wf_app_tick(&t.a);
	CHECK(&WF_MODE_CONFIRM == t.a.mode);
	key(&t.a, WF_KEY_ESC);
	CHECK(0 == t.f.recovers);
	t_close(&t);

	t_open(&t, true, true);
	t.f.unfinished = 1;
	wf_app_batch_startup(&t.a);
	wf_app_tick(&t.a);
	ch(&t.a, 'y'); /* typed ahead: not an answer */
	CHECK(0 == t.f.recovers);
	arm(&t.a);
	ch(&t.a, 'y');
	CHECK(1 == t.f.recovers && t.a.batch.busy && t.a.batch.recover);
	CHECK(&WF_MODE_BATCH == t.a.mode &&
	        screen_has(&t.a, "Recovering unfinished"));
	key(&t.a, WF_KEY_ESC); /* a recovery is not stopped */
	CHECK(0 == t.f.cancels && t.a.batch.busy);
	memset(&d, 0, sizeof d);
	d.gen = t.f.rec_gen;
	d.recover = true;
	d.rec_total = 2;
	d.rec_open = 1;
	d.rec_rolled_back = 1;
	d.rep.result = WF_BX_RECOVERY_REQUIRED;
	d.rep.not_restored = 3;
	wf_app_on_batch(&t.a, &d);
	CHECK(WF_BS_RESULT == t.a.batch.stage);
	CHECK(NULL !=
	        strstr(t.a.batch.result[0],
	                "NOT fully recovered: 1 journal(s) kept; 3 name(s)"));
	CHECK(NULL == strstr(t.a.batch.result[0], "Recovered"));
	t_close(&t);

	t_open(&t, true, true);
	t.f.unfinished = 1;
	wf_app_batch_startup(&t.a);
	wf_app_tick(&t.a);
	arm(&t.a);
	ch(&t.a, 'y');
	memset(&d, 0, sizeof d);
	d.gen = t.f.rec_gen;
	d.recover = true;
	d.rec_total = 2;
	d.rec_done = 1;
	d.rec_rolled_back = 1;
	d.rep.result = WF_BX_ROLLED_BACK;
	wf_app_on_batch(&t.a, &d);
	CHECK(NULL != strstr(t.a.batch.result[0], "Recovered: 2 journal(s)"));
	CHECK(NULL !=
	        strstr(t.a.batch.result[0], "1 were complete, 1 were taken"));
	t_close(&t);
}

static void with_nothing_left_behind_nothing_is_asked(void)
{
	T t;

	t_open(&t, true, true);
	t.f.unfinished = 0;
	wf_app_batch_startup(&t.a);
	CHECK(0xFFFFFFFFu == wf_app_batch_timeout(&t.a));
	wf_app_tick(&t.a);
	CHECK(&WF_MODE_NORMAL == t.a.mode);
	t_close(&t);
}

static void the_question_about_journals_waits_for_a_free_screen(void)
{
	T t;

	t_open(&t, true, true);
	t.f.unfinished = 1;
	t.a.mode = &WF_MODE_HELP; /* the first-run screens, say */
	wf_app_batch_startup(&t.a);
	wf_app_tick(&t.a);
	CHECK(&WF_MODE_HELP == t.a.mode && 0 == t.f.recovers);
	t.a.mode = &WF_MODE_NORMAL;
	wf_app_tick(&t.a);
	CHECK(&WF_MODE_CONFIRM == t.a.mode);
	t_close(&t);
}

static void with_the_gate_closed_journals_are_only_mentioned(void)
{
	T t;

	t_open(&t, true, false);
	t.f.unfinished = 3;
	wf_app_batch_startup(&t.a);
	wf_app_tick(&t.a);
	CHECK(&WF_MODE_NORMAL == t.a.mode && 0 == t.f.recovers);
	CHECK(NULL != strstr(t.a.msg, "(3)") &&
	        NULL != strstr(t.a.msg, "blocked"));
	t_close(&t);
}

static void a_recovery_is_gated_again_when_the_answer_comes(void)
{
	T t;

	t_open(&t, true, true);
	t.f.unfinished = 1;
	wf_app_batch_startup(&t.a);
	wf_app_tick(&t.a);
	arm(&t.a);
	t.a.edit.submit = NULL;
	ch(&t.a, 'y');
	CHECK(0 == t.f.recovers && !t.a.batch.busy);
	t_close(&t);
}

/* DR-DRAFT-029 replaces the first decision (no Undo for a batch): a batch made
 * whole is one Undo entry, taken back as one batch by the same engine. */

/* A batch of a.txt -> 001_a.txt, finished with `r`, the result screen closed. */
static void run_one(T *t, WfBxResult r)
{
	WfBxDone d;

	mark(&t->a, L"a.txt");
	to_running(t);
	d = done(t, r);
	wf_app_on_batch(&t->a, &d);
	key(&t->a, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == t->a.mode);
}

static const WfJobLogEntry *last_row(const T *t)
{
	static const WfJobLogEntry none = {.head = {0}};

	return t->a.jobs.n ? &t->a.jobs.log[t->a.jobs.n - 1] : &none;
}

static void a_batch_made_whole_is_one_undo_entry_and_one_row_of_the_history(
        void)
{
	T t;
	const WfUndoEntry *u;
	const WfJobLogEntry *row;

	t_open(&t, true, true);
	run_one(&t, WF_BX_SUCCESS);
	CHECK(1 == t.a.undo_n);
	u = &t.a.undo[0];
	CHECK(WF_UNDO_BATCH == u->kind && 1 == u->n);
	CHECK(!wcscmp(u->folder, L"C:\\Work"));
	CHECK(!wcscmp(u->item[0].name, L"001_a.txt"));
	CHECK(!wcscmp(u->item[0].old_name, L"a.txt"));
	CHECK((u->item[0].id.kind == WF_ID_STRONG));
	row = last_row(&t);
	CHECK(row && strstr(row->head, "BULK RENAME: 1 item(s)"));
	CHECK(row && WF_HU_KEPT == row->undo && row->undo_uid == u->uid);
	ch(&t.a, 'J');
	CHECK(screen_has(&t.a, "BULK RENAME: 1 item(s)"));
	CHECK(screen_has(&t.a, "u: undo this"));
	t_close(&t);
}

static void u_asks_and_yes_runs_the_names_the_other_way_round(void)
{
	T t;
	WfBxDone d;
	WfFileId id;

	t_open(&t, true, true);
	run_one(&t, WF_BX_SUCCESS);
	id = t.a.undo[0].item[0].id;
	t.f.prepares = 0;
	ch(&t.a, 'u');
	CHECK(&WF_MODE_CONFIRM == t.a.mode); /* nothing before the answer */
	CHECK(0 == t.f.prepares && 1 == t.f.runs);
	arm(&t.a);
	ch(&t.a, 'y');
	CHECK(1 == t.f.prepares && 2 == t.f.runs);
	CHECK(1 == t.f.n_src);
	CHECK(!wcscmp(t.f.src_path[0], L"C:\\Work\\001_a.txt"));
	CHECK(!wcscmp(t.f.src_to[0], L"a.txt"));
	CHECK(wf_fileid_equal(&t.f.src_id[0], &id));
	CHECK(t.a.batch.busy && WF_BS_RUNNING == t.a.batch.stage);
	CHECK(1 == t.a.undo_n); /* accepted is not undone */
	d = done(&t, WF_BX_SUCCESS);
	wf_app_on_batch(&t.a, &d);
	CHECK(0 == t.a.undo_n);
	CHECK(strstr(t.a.msg, "Undone: 1 name(s) put back"));
	CHECK(!strstr(t.a.msg, "Done:"));
	/* the row says it was undone, and the Undo made no row of its own */
	CHECK(1 == t.a.jobs.n && WF_HU_DONE == last_row(&t)->undo);
	key(&t.a, WF_KEY_ESC);
	ch(&t.a, 'J');
	CHECK(screen_has(&t.a, "undone"));
	CHECK(!screen_has(&t.a, "u: undo this"));
	t_close(&t);
}

static void no_to_the_question_changes_nothing(void)
{
	T t;

	t_open(&t, true, true);
	run_one(&t, WF_BX_SUCCESS);
	t.f.prepares = 0;
	ch(&t.a, 'u');
	arm(&t.a);
	ch(&t.a, 'n');
	CHECK(0 == t.f.prepares && 1 == t.f.runs && 1 == t.a.undo_n);
	CHECK(!t.a.batch.busy);
	t_close(&t);
}

/* An item that is no longer the file that was renamed, an old name that is
 * taken, a job that is not ready: nothing runs and the entry stays. */
static void an_undo_that_cannot_be_whole_does_not_start(void)
{
	T t;

	t_open(&t, true, true);
	run_one(&t, WF_BX_SUCCESS);
	t.f.refuse_row = 0;
	t.f.refuse_why = WF_BP_EXISTS;
	ch(&t.a, 'u');
	arm(&t.a);
	ch(&t.a, 'y');
	CHECK(1 == t.f.runs && !t.a.batch.busy && 1 == t.a.undo_n);
	CHECK(strstr(t.a.msg, "Not undone, nothing changed"));
	CHECK(&WF_MODE_NORMAL == t.a.mode && WF_BS_IDLE == t.a.batch.stage);
	CHECK(WF_HU_KEPT == last_row(&t)->undo);

	t.f.refuse_row = -1;
	t.f.pre.pre = WF_BX_PRE_CHANGED;
	ch(&t.a, 'u');
	arm(&t.a);
	ch(&t.a, 'y');
	CHECK(1 == t.f.runs && 1 == t.a.undo_n);
	CHECK(strstr(t.a.msg, "Not undone, nothing changed"));

	t.f.pre.pre = WF_BX_PRE_READY;
	t.f.never_ready = true;
	ch(&t.a, 'u');
	arm(&t.a);
	ch(&t.a, 'y');
	CHECK(1 == t.f.runs && 1 == t.a.undo_n);
	t_close(&t);
}

static void an_undo_that_was_taken_back_or_left_open_keeps_the_entry(void)
{
	static const WfBxResult results[] = {WF_BX_ROLLED_BACK,
	        WF_BX_RECOVERY_REQUIRED, WF_BX_FAILED_BEFORE};

	for (size_t k = 0; k < sizeof results / sizeof results[0]; k++) {
		T t;
		WfBxDone d;

		t_open(&t, true, true);
		run_one(&t, WF_BX_SUCCESS);
		ch(&t.a, 'u');
		arm(&t.a);
		ch(&t.a, 'y');
		CHECK(2 == t.f.runs);
		d = done(&t, results[k]);
		wf_app_on_batch(&t.a, &d);
		CHECK(1 == t.a.undo_n);
		CHECK(!strstr(t.a.msg, "Undone"));
		CHECK(1 == t.a.jobs.n && WF_HU_KEPT == last_row(&t)->undo);
		t_close(&t);
	}
}

static void the_undo_stops_at_the_gate_and_at_a_busy_worker(void)
{
	T t;
	WfBxDone d;

	t_open(&t, true, true);
	run_one(&t, WF_BX_SUCCESS);
	ch(&t.a, 'u');
	arm(&t.a);
	t.a.edit.submit = NULL; /* the gate closes before the answer */
	t.f.prepares = 0;
	ch(&t.a, 'y');
	CHECK(0 == t.f.prepares && 1 == t.f.runs && 1 == t.a.undo_n);
	wf_app_set_edit(&t.a, (WfEditPort){&t.f, e_submit});

	t.f.run_st = WF_E_INVAL; /* the worker is busy */
	ch(&t.a, 'u');
	arm(&t.a);
	ch(&t.a, 'y');
	CHECK(1 == t.f.runs && !t.a.batch.busy && 1 == t.a.undo_n);
	t.f.run_st = WF_OK;

	ch(&t.a, 'u');
	arm(&t.a);
	ch(&t.a, 'y');
	CHECK(2 == t.f.runs && t.a.batch.busy);
	key(&t.a, WF_KEY_ESC); /* asks the worker to stop; the screen stays */
	CHECK(1 == t.f.cancels);
	d = done(&t, WF_BX_ROLLED_BACK);
	wf_app_on_batch(&t.a, &d);
	CHECK(1 == t.a.undo_n);
	t_close(&t);
}

/* A run that changed nothing, or did not finish, has a row that says why it
 * cannot be undone, and no Undo entry; one that never began has no row. */
static void a_batch_that_was_not_made_whole_cannot_be_undone(void)
{
	T t;

	t_open(&t, true, true);
	run_one(&t, WF_BX_ROLLED_BACK);
	CHECK(0 == t.a.undo_n && 1 == t.a.jobs.n);
	CHECK(WF_HU_NONE == last_row(&t)->undo);
	CHECK(strstr(last_row(&t)->head, "every name put back"));
	ch(&t.a, 'J');
	CHECK(screen_has(&t.a, "cannot be undone: nothing was changed"));
	key(&t.a, WF_KEY_ENTER);
	ch(&t.a, 'u');
	CHECK(&WF_MODE_NORMAL == t.a.mode); /* no question: nothing to undo */
	t_close(&t);

	t_open(&t, true, true);
	run_one(&t, WF_BX_RECOVERY_REQUIRED);
	CHECK(0 == t.a.undo_n && 1 == t.a.jobs.n);
	CHECK(strstr(last_row(&t)->head, "NOT complete"));
	ch(&t.a, 'J');
	CHECK(screen_has(&t.a, "it did not finish"));
	t_close(&t);

	t_open(&t, true, true);
	run_one(&t, WF_BX_FAILED_BEFORE);
	CHECK(0 == t.a.undo_n && 0 == t.a.jobs.n);
	t_close(&t);
}

/* More batches than Undo keeps: the row of the oldest says it has gone. */
static void a_row_whose_undo_entry_was_pushed_out_says_so(void)
{
	T t;

	t_open(&t, true, true);
	for (int k = 0; k < WF_UNDO_MAX + 1; k++) {
		run_one(&t, WF_BX_SUCCESS);
		deliver(&t.a, &t.p);
	}
	CHECK(WF_UNDO_MAX == t.a.undo_n && WF_UNDO_MAX + 1 == t.a.jobs.n);
	CHECK(WF_HU_EXPIRED == t.a.jobs.log[0].undo);
	CHECK(WF_HU_KEPT == t.a.jobs.log[1].undo);
	ch(&t.a, 'J');
	key(&t.a, WF_KEY_END);
	CHECK(screen_has(&t.a, "can no longer be undone"));
	t_close(&t);
}

/* The first screen row that shows `needle`, or -1. */
static int screen_row_of(const WfApp *a, const char *needle)
{
	WfFrame *f = wf_frame_new(a->cols, a->rows, &WF_STYLE_BASE);
	int found = -1;

	CHECK(f);
	wf_app_draw(a, f);
	for (int y = 0; y < f->rows && found < 0; y++) {
		if (strstr(row_text(f, y), needle)) {
			found = y;
		}
	}
	wf_frame_free(f);
	return found;
}

/* Undo takes the newest change first: the row of the older batch says so. */
static void the_older_of_two_batches_waits_for_the_newer(void)
{
	T t;

	t_open(&t, true, true);
	run_one(&t, WF_BX_SUCCESS);
	deliver(&t.a, &t.p); /* the listing that was asked for after the run */
	run_one(&t, WF_BX_SUCCESS);
	CHECK(2 == t.a.undo_n && 2 == t.a.jobs.n);
	ch(&t.a, 'J');
	/* newest first: the newest row is the one u takes now */
	CHECK(screen_row_of(&t.a, "u: undo this") > 0);
	CHECK(screen_row_of(&t.a, "can be undone after the newer changes") >
	        screen_row_of(&t.a, "u: undo this"));
	ch(&t.a, 'u'); /* u in the history is the same Undo */
	CHECK(&WF_MODE_CONFIRM == t.a.mode);
	CHECK(t.a.undo_ask_uid == t.a.undo[1].uid);
	t_close(&t);
}

static void the_key_is_m_in_every_profile_and_the_command_has_a_name(void)
{
	static const int PROFILES[] = {
	        WF_PROFILE_LEGACY, WF_PROFILE_NANO, WF_PROFILE_VIM};
	const WfCommandInfo *info = wf_cmd_by_name("fm.bulk_rename");

	CHECK(info && WF_FM_BULK_RENAME == info->id);
	CHECK(info && 0 != (info->ctx_mask & WF_CTX_BIT(WF_CTX_FM)));
	for (size_t i = 0; i < sizeof PROFILES / sizeof *PROFILES; i++) {
		WfKeyConfig cfg;
		WfKeymap *m = NULL;
		char text[64];

		wf_keyconfig_init(&cfg);
		cfg.profile = PROFILES[i];
		CHECK(WF_OK == wf_keymap_build_defaults(&cfg, WF_KM_PIERO, &m));
		if (m) {
			wf_keymap_keys_text(m, WF_CTX_FM, WF_FM_BULK_RENAME,
			        text, sizeof text);
			CHECK(!strcmp(text, "M"));
			wf_keymap_free(m);
		}
	}
}

static void every_word_of_the_screen_is_in_both_languages(void)
{
	static const WfBatchWhy WHY[] = {WF_BP_UNCHANGED, WF_BP_PATH_LONG,
	        WF_BP_DUP_TARGET, WF_BP_EXISTS, WF_BP_DUP_ITEM, WF_BP_NESTED,
	        WF_BP_VOLUME, WF_BP_NO_ID, WF_BP_LINK_PARENT, WF_BP_LINK_ITEM};
	WfBatchRow row[1];
	WfBatchPlan pl;
	char buf[160];

	memset(&pl, 0, sizeof pl);
	pl.row = row;
	pl.n = 1;
	for (size_t i = 0; i < sizeof WHY / sizeof *WHY; i++) {
		const char *en, *ja;

		row[0].why = WHY[i];
		wf_i18n_set_language(WF_UILANG_EN);
		en = wf_app_batch_row_text(&pl, 0, buf, sizeof buf);
		CHECK(!strcmp(en, wf_batch_why_text(WHY[i])));
		{
			char keep[160];

			snprintf(keep, sizeof keep, "%s", en);
			wf_i18n_set_language(WF_UILANG_JA);
			ja = wf_app_batch_row_text(&pl, 0, buf, sizeof buf);
			CHECK(ja[0] &&
			        strcmp(ja, keep)); /* a sentence of its own */
		}
	}
	row[0].why = WF_BP_OK;
	CHECK(!strcmp("", wf_app_batch_row_text(&pl, 0, buf, sizeof buf)));
	row[0].why = WF_BP_NAME;
	row[0].name = WF_NAME_EMPTY;
	wf_i18n_set_language(WF_UILANG_EN);
	CHECK(NULL !=
	        strstr(wf_app_batch_row_text(&pl, 0, buf, sizeof buf),
	                "bad name"));
	wf_i18n_set_language(WF_UILANG_EN);
}

/* Whether a row goes through a temporary name is known from the row; drawing the
 * 50 rows on screen does not look through the 20000 steps of the plan for each. */
static void a_row_knows_its_temporary_name(void)
{
	enum
	{
		N = 20000,
		SHOWN = 50,
		FRAMES = 200
	};
	WfBatchRow *row = calloc(N, sizeof *row);
	WfBatchStep *step = calloc(N, sizeof *step);
	WfBatchPlan pl;
	char buf[160];
	clock_t t0;
	double ms;
	size_t temps = 0;

	CHECK(row && step);
	memset(&pl, 0, sizeof pl);
	for (uint32_t i = 0; i < N; i++) {
		row[i].why = WF_BP_OK;
		step[i].item = i;
	}
	row[N - 1].temp = true; /* the last one, where a search is longest */
	step[N - 1].temp = true;
	pl.row = row;
	pl.n = N;
	pl.step = step;
	pl.n_steps = N;
	wf_i18n_set_language(WF_UILANG_EN);
	CHECK(strlen(wf_app_batch_row_text(&pl, N - 1, buf, sizeof buf)) > 0);
	CHECK(!strcmp("", wf_app_batch_row_text(&pl, 0, buf, sizeof buf)));
	t0 = clock();
	for (int f = 0; f < FRAMES; f++) {
		for (size_t i = N - SHOWN; i < N; i++) {
			temps += 0 !=
			        wf_app_batch_row_text(
			                &pl, i, buf, sizeof buf)[0];
		}
	}
	ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / (double)FRAMES;
	printf("info: %d rows on screen of a plan of %d steps: %.4f ms a "
	       "frame\n\n",
	        (int)SHOWN, (int)N, ms);
	CHECK(FRAMES == temps); /* only the last row, once a frame */
	free(row);
	free(step);
}

static void the_screens_draw_in_every_size_without_harm(void)
{
	static const int SIZES[][2] = {
	        {100, 30}, {40, 12}, {24, 8}, {20, 6}, {200, 60}};

	for (size_t s = 0; s < sizeof SIZES / sizeof *SIZES; s++) {
		T t;

		t_open(&t, true, true);
		mark(&t.a, L"a.txt");
		mark(&t.a, L"b.txt");
		t.a.cols = SIZES[s][0];
		t.a.rows = SIZES[s][1];
		t.f.refuse_row = 1;
		t.f.refuse_why = WF_BP_NESTED;
		ch(&t.a, 'M');
		(void)screen_has(&t.a, "x");
		key(&t.a, WF_KEY_ENTER);
		(void)screen_has(&t.a, "x");
		t.a.batch.stage = WF_BS_RUNNING;
		(void)screen_has(&t.a, "x");
		t.a.batch.stage = WF_BS_RESULT;
		t.a.batch.result_n = 1;
		snprintf(t.a.batch.result[0], sizeof t.a.batch.result[0], "x");
		(void)screen_has(&t.a, "x");
		t.a.batch.stage = WF_BS_PREVIEW;
		t_close(&t);
	}
}

int main(void)
{
	m_opens_the_pattern_for_the_marked_entries_in_list_order();
	without_marks_the_entry_under_the_cursor_is_the_item();
	it_refuses_where_it_must_and_changes_no_mode();
	a_selection_of_names_that_are_not_there_is_not_the_cursor_entry();
	more_items_than_one_batch_may_hold_are_refused_with_the_number();
	the_fields_take_text_and_are_checked_as_you_type();
	non_bmp_text_is_kept_whole_and_backspace_takes_the_pair();
	esc_on_the_pattern_changes_nothing();
	the_preview_shows_before_after_counts_and_hands_the_names_to_the_plan();
	the_start_and_step_and_cases_reach_the_names();
	unchanged_names_are_skipped_and_all_unchanged_cannot_run();
	a_refused_row_is_shown_with_its_reason_and_nothing_can_run();
	a_job_that_is_not_ready_cannot_run_and_the_reason_is_shown();
	a_name_the_pattern_cannot_make_keeps_the_pattern_on_screen();
	scrolling_keeps_the_top_inside_the_list();
	esc_on_the_preview_goes_back_to_the_fields_and_lets_the_plan_go();
	nothing_runs_without_the_question_and_its_answer();
	yes_hands_the_job_to_the_worker_once();
	a_worker_that_refuses_leaves_the_names_alone_and_says_busy();
	esc_during_the_run_asks_the_worker_to_stop_once();
	leaving_asks_while_a_batch_runs();
	the_gate_is_checked_again_before_anything_is_handed_over();
	a_listing_that_changed_under_the_preview_is_planned_again_not_run();
	any_one_change_of_the_listing_stops_the_run();
	done_is_said_only_for_success();
	each_result_has_its_own_honest_words_and_counts();
	an_answer_that_is_not_the_request_in_flight_is_dropped();
	after_success_the_folder_is_read_again_with_the_cursor_on_a_renamed_item();
	after_a_failure_the_folder_is_read_again_and_the_marks_stay();
	a_result_that_arrives_while_another_screen_is_up_goes_to_the_status_line();
	another_screen_taking_over_lets_the_plan_go();
	destroying_the_app_lets_a_plan_that_was_not_run_go();
	journals_left_behind_are_asked_about_and_never_recovered_unasked();
	with_nothing_left_behind_nothing_is_asked();
	the_question_about_journals_waits_for_a_free_screen();
	with_the_gate_closed_journals_are_only_mentioned();
	a_recovery_is_gated_again_when_the_answer_comes();
	a_batch_made_whole_is_one_undo_entry_and_one_row_of_the_history();
	u_asks_and_yes_runs_the_names_the_other_way_round();
	no_to_the_question_changes_nothing();
	an_undo_that_cannot_be_whole_does_not_start();
	an_undo_that_was_taken_back_or_left_open_keeps_the_entry();
	the_undo_stops_at_the_gate_and_at_a_busy_worker();
	a_batch_that_was_not_made_whole_cannot_be_undone();
	the_older_of_two_batches_waits_for_the_newer();
	a_row_whose_undo_entry_was_pushed_out_says_so();
	the_key_is_m_in_every_profile_and_the_command_has_a_name();
	every_word_of_the_screen_is_in_both_languages();
	a_row_knows_its_temporary_name();
	the_screens_draw_in_every_size_without_harm();
	return TEST_RESULT();
}
