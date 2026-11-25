/* Smart Go (`g`, Ctrl+L): typing, the five-row candidate list, Tab, Enter,
 * errors, paste, and what must not change before Enter. The candidate worker,
 * the clipboard and the environment are fakes, so no disk is touched. */
#include <stdlib.h>
#include <string.h>

#include "ui/wf_app_priv.h"
#include "wf_fake_fs.h"
#include "wf_test.h"

static void ch(WfApp *a, uint32_t c)
{
	WfEvent e = {WF_EV_CHAR, c, WF_KEY_NONE, 0};
	a->mode->on_event(a, &e);
}

static void ctrl(WfApp *a, char c)
{
	WfEvent e = {WF_EV_CTRL, (uint32_t)c, WF_KEY_NONE, 0};
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

static const wchar_t *cur_name(WfApp *a)
{
	const WfEntry *e = wf_view_current(&a->cur);

	return e ? e->name : L"(none)";
}

typedef struct
{
	unsigned starts, mores, cancels;
	uint32_t gen;
	wchar_t dir[260], prefix[64];
	bool hidden, dirs_only;
	char *clip;
} Fake;

static WfStatus f_start(void *ctx,
        uint32_t gen,
        const wchar_t *dir,
        const wchar_t *prefix,
        bool hidden,
        bool dirs_only)
{
	Fake *f = ctx;

	f->starts++;
	f->gen = gen;
	f->hidden = hidden;
	f->dirs_only = dirs_only;
	wcsncpy_s(f->dir, 260, dir, _TRUNCATE);
	wcsncpy_s(f->prefix, 64, prefix, _TRUNCATE);
	return WF_OK;
}

static WfStatus f_more(void *ctx, uint32_t gen)
{
	Fake *f = ctx;

	f->mores++;
	return gen == f->gen ? WF_OK : WF_E_INVAL;
}

static void f_cancel(void *ctx)
{
	((Fake *)ctx)->cancels++;
}

static WfStatus f_clip(void *ctx, char **utf8, size_t *n)
{
	Fake *f = ctx;

	if (!f->clip) {
		return WF_E_NOENT;
	}
	*utf8 = _strdup(f->clip);
	*n = strlen(f->clip);
	return *utf8 ? WF_OK : WF_E_NOMEM;
}

static wchar_t *e_home(void *ctx)
{
	(void)ctx;
	return _wcsdup(L"C:\\Users\\dev");
}

static wchar_t *e_known(void *ctx, const wchar_t *alias)
{
	(void)ctx;
	return !wcscmp(alias, L"home") ? _wcsdup(L"C:\\Users\\dev")
	        : !wcscmp(alias, L"documents")
	        ? _wcsdup(L"C:\\Users\\dev\\Docs")
	        : NULL;
}

static wchar_t *e_getenv(void *ctx, const wchar_t *name)
{
	(void)ctx;
	return !wcscmp(name, L"TEMP") ? _wcsdup(L"C:\\Users") : NULL;
}

/* Delivers every queued listing, including ones the delivery queues. */
static void settle(FakePort *fp)
{
	for (int guard = 0; fp->nq && guard < 16; guard++) {
		int n = fp->nq;
		int i;

		for (i = 0; i < n; i++) {
			fake_deliver(fp, i);
		}
		memmove(fp->q, fp->q + n,
		        (size_t)(fp->nq - n) * sizeof fp->q[0]);
		fp->nq -= n;
	}
}

static void batch(WfApp *a,
        Fake *f,
        uint32_t gen,
        bool first,
        bool more,
        const wchar_t *const *names,
        const uint32_t *flags,
        size_t n)
{
	WfCandBatch b;

	memset(&b, 0, sizeof b);
	b.gen = gen;
	b.st = WF_OK;
	b.n = n;
	b.first = first;
	b.more = more;
	for (size_t i = 0; i < n; i++) {
		wcscpy_s(b.items[i].name, WF_CAND_NAME, names[i]);
		b.items[i].flags = flags ? flags[i] : WF_ENT_DIR;
	}
	(void)f;
	wf_app_on_candidates(a, &b);
}

static void open_go(WfApp *a, FakePort *fp, Fake *f, const wchar_t *at)
{
	memset(f, 0, sizeof *f);
	fp->defer = 1;
	CHECK(WF_OK == wf_app_init(a, 120, 30, fake_port(fp), at));
	settle(fp);
	wf_app_set_candidates(a, (WfCandPort){f, f_start, f_more, f_cancel});
	wf_app_set_clipboard(a, (WfClipPort){f, f_clip});
	wf_app_set_smartgo_env(a, (WfSmartEnv){f, e_home, e_known, e_getenv});
}

/* Opens `g`; the empty-line search it starts is not counted. */
static void begin(WfApp *a, Fake *f)
{
	wf_app_path_prompt_begin(a);
	f->starts = 0;
}

static bool input_is(const WfApp *a, const wchar_t *text)
{
	return a->go.text && !wcscmp(a->go.text, text);
}

static bool note_has(const WfApp *a, const char *needle)
{
	return NULL != strstr(a->go.note, needle);
}

static void ctrl_l_prefills_selects_and_replaces(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Fake f;

	open_go(&a, &fp, &f, L"C:\\Users");
	key(&a, WF_KEY_CTRL_L);
	CHECK(&WF_MODE_GO == a.mode && input_is(&a, L"C:\\Users\\") &&
	        a.go.all_selected && a.go.cursor == a.go.len);
	ch(&a, 'x'); /* typing over the selection replaces all of it */
	CHECK(input_is(&a, L"x") && !a.go.all_selected);
	key(&a, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == a.mode && !wcscmp(a.cwd, L"C:\\Users"));
	wf_app_destroy(&a);

	/* A root already ends in a separator: none is added twice. */
	open_go(&a, &fp, &f, L"C:\\");
	key(&a, WF_KEY_CTRL_L);
	CHECK(input_is(&a, L"C:\\"));
	key(&a, WF_KEY_BACKSPACE);
	CHECK(0 == a.go.len && !a.go.all_selected);
	wf_app_destroy(&a);
}

static void typing_asks_for_the_newest_search_and_drops_stale_answers(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Fake f;
	const wchar_t *const names[] = {L"Docs", L"Pictures"};

	open_go(&a, &fp, &f, L"C:\\Users\\dev");
	begin(&a, &f);
	CHECK(&WF_MODE_GO == a.mode && 0 == a.go.len && 0 == f.starts);
	type(&a, L"~");
	CHECK(1 == f.starts && !wcscmp(f.dir, L"C:\\Users\\dev") &&
	        !wcscmp(f.prefix, L"") && WF_GR_FIRST == a.go.read);
	{
		uint32_t first = f.gen;

		type(&a, L"\\D");
		CHECK(2 == f.starts && f.gen > first &&
		        !wcscmp(f.prefix, L"D"));
		/* the answer of the first search arrives late: dropped */
		batch(&a, &f, first, true, false, names, NULL, 2);
		CHECK(0 == a.go.cand_n && WF_GR_FIRST == a.go.read);
		batch(&a, &f, f.gen, true, false, names, NULL, 1);
		CHECK(1 == a.go.cand_n && WF_GR_IDLE == a.go.read &&
		        !wcscmp(a.go.cand[0].name, L"Docs"));
	}
	/* Esc ends the search at the worker */
	{
		unsigned c = f.cancels;

		key(&a, WF_KEY_ESC);
		CHECK(f.cancels > c && &WF_MODE_NORMAL == a.mode);
	}
	wf_app_destroy(&a);
}

/* The numbers of the searches go on when the line is closed and opened again:
 * an answer for the folder of the first opening is not taken in the second. */
static void a_reopened_line_does_not_take_an_old_answer(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Fake f;
	const wchar_t *const names[] = {L"Docs", L"Pictures"};
	uint32_t old;

	open_go(&a, &fp, &f, L"C:\\Users\\dev");
	begin(&a, &f);
	type(&a, L"~");
	old = f.gen;
	key(&a, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == a.mode);
	begin(&a, &f);
	type(&a,
	        L"~"); /* a search of the second opening, as the first one was */
	CHECK(f.gen != old);
	batch(&a, &f, old, true, false, names, NULL, 2);
	CHECK(0 == a.go.cand_n);
	batch(&a, &f, f.gen, true, false, names, NULL, 2);
	CHECK(2 == a.go.cand_n);
	wf_app_destroy(&a);
}

static void nothing_changes_before_enter(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Fake f;

	open_go(&a, &fp, &f, L"C:\\Users");
	begin(&a, &f);
	type(&a, L"C:\\Users\\dev");
	key(&a, WF_KEY_TAB);
	key(&a, WF_KEY_DOWN);
	CHECK(0 == fp.nq && !wcscmp(a.cwd, L"C:\\Users") && 1 == a.history_n);
	key(&a, WF_KEY_ESC);
	CHECK(0 == fp.nq && !wcscmp(a.cwd, L"C:\\Users") && 1 == a.history_n &&
	        !wcscmp(cur_name(&a), L"dev"));
	wf_app_destroy(&a);
}

static void enter_goes_to_the_typed_path_never_to_a_candidate_unasked(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Fake f;
	const wchar_t *const names[] = {L"Users", L"secret"};

	open_go(&a, &fp, &f, L"C:\\");
	begin(&a, &f);
	type(&a, L"U");
	batch(&a, &f, f.gen, true, false, names, NULL, 1);
	key(&a, WF_KEY_ENTER); /* "U" is not a folder, and Users was not chosen */
	settle(&fp);
	CHECK(!wcscmp(a.cwd, L"C:\\") && &WF_MODE_GO == a.mode &&
	        !a.go.committing && note_has(&a, "not found") &&
	        input_is(&a, L"U"));
	/* an explicit choice goes there */
	batch(&a, &f, f.gen, true, false, names, NULL, 1);
	key(&a, WF_KEY_DOWN);
	CHECK(a.go.has_sel && 0 == a.go.sel);
	key(&a, WF_KEY_ENTER);
	settle(&fp);
	CHECK(&WF_MODE_NORMAL == a.mode && !wcscmp(a.cwd, L"C:\\Users"));
	wf_app_destroy(&a);
}

static void tilde_lists_home_and_enter_goes_home(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Fake f;

	open_go(&a, &fp, &f, L"C:\\");
	begin(&a, &f);
	type(&a, L"~");
	CHECK(1 == f.starts && !wcscmp(f.dir, L"C:\\Users\\dev"));
	key(&a, WF_KEY_ENTER);
	settle(&fp);
	CHECK(&WF_MODE_NORMAL == a.mode && !wcscmp(a.cwd, L"C:\\Users\\dev"));
	wf_app_destroy(&a);
}

static void tab_completes_without_going_and_cycles(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Fake f;
	const wchar_t *const names[] = {L"Docs", L"Downloads", L"dotfiles"};

	open_go(&a, &fp, &f, L"C:\\");
	begin(&a, &f);
	type(&a, L"~\\Do");
	batch(&a, &f, f.gen, true, false, names, NULL, 3);
	CHECK(3 == a.go.cand_n);
	key(&a, WF_KEY_TAB);
	CHECK(input_is(&a, L"~\\Docs\\") && !wcscmp(a.cwd, L"C:\\") &&
	        0 == fp.nq);
	wf_app_destroy(&a);

	open_go(&a, &fp, &f, L"C:\\");
	begin(&a, &f);
	type(&a, L"~\\Do");
	batch(&a, &f, f.gen, true, false, names, NULL, 3);
	key(&a, WF_KEY_DOWN);
	key(&a, WF_KEY_DOWN); /* Downloads */
	key(&a, WF_KEY_TAB);
	CHECK(input_is(&a, L"~\\Downloads\\"));
	wf_app_destroy(&a);

	/* Shift+Tab with nothing chosen takes the last candidate */
	open_go(&a, &fp, &f, L"C:\\");
	begin(&a, &f);
	type(&a, L"~\\Do");
	batch(&a, &f, f.gen, true, false, names, NULL, 3);
	key(&a, WF_KEY_SHIFT_TAB);
	CHECK(input_is(&a, L"~\\dotfiles\\"));
	wf_app_destroy(&a);

	/* no candidates: Tab does nothing to the text */
	open_go(&a, &fp, &f, L"C:\\");
	begin(&a, &f);
	type(&a, L"~\\zz");
	batch(&a, &f, f.gen, true, false, names, NULL, 0);
	key(&a, WF_KEY_TAB);
	CHECK(input_is(&a, L"~\\zz") && note_has(&a, "No matches"));
	wf_app_destroy(&a);
}

static void down_scrolls_the_window_and_asks_for_more_once(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Fake f;
	static wchar_t text[30][8];
	const wchar_t *names[30];

	for (int i = 0; i < 30; i++) {
		swprintf(text[i], 8, L"a%d", i + 1);
		names[i] = text[i];
	}
	open_go(&a, &fp, &f, L"C:\\");
	begin(&a, &f);
	type(&a, L"~\\a");
	batch(&a, &f, f.gen, true, true, names, NULL, WF_CAND_BATCH);
	CHECK(24 == a.go.cand_n);
	for (int i = 0; i < 24; i++) {
		key(&a, WF_KEY_DOWN);
	}
	CHECK(23 == a.go.sel && 14 == a.go.top &&
	        0 == f.mores); /* ten rows show */
	key(&a, WF_KEY_DOWN);  /* the end of what was read */
	CHECK(1 == f.mores && WF_GR_MORE == a.go.read);
	key(&a, WF_KEY_DOWN); /* waiting: not asked twice */
	CHECK(1 == f.mores);
	batch(&a, &f, f.gen, false, false, names + 24, NULL, 6);
	CHECK(30 == a.go.cand_n && 24 == a.go.sel && 15 == a.go.top &&
	        WF_GR_IDLE == a.go.read && !a.go.has_more);
	for (int i = 0; i < 5; i++) {
		key(&a, WF_KEY_DOWN);
	}
	key(&a, WF_KEY_DOWN); /* the very end: no more asking */
	CHECK(29 == a.go.sel && 1 == f.mores);
	key(&a, WF_KEY_UP);
	CHECK(28 == a.go.sel);
	wf_app_destroy(&a);
}

static void a_file_goes_to_its_folder_with_the_file_chosen(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Fake f;
	const wchar_t *const names[] = {L"file2.txt"};
	const uint32_t flags[] = {0};

	open_go(&a, &fp, &f, L"C:\\");
	begin(&a, &f);
	type(&a, L"C:\\Users\\dev\\file2");
	batch(&a, &f, f.gen, true, false, names, flags, 1);
	key(&a, WF_KEY_DOWN);
	key(&a, WF_KEY_ENTER);
	settle(&fp);
	CHECK(&WF_MODE_NORMAL == a.mode && !wcscmp(a.cwd, L"C:\\Users\\dev") &&
	        !wcscmp(cur_name(&a), L"file2.txt"));
	wf_app_destroy(&a);

	/* a typed file name, whole */
	open_go(&a, &fp, &f, L"C:\\");
	begin(&a, &f);
	type(&a, L"C:\\Users\\dev\\file2.txt");
	key(&a, WF_KEY_ENTER);
	settle(&fp);
	CHECK(&WF_MODE_NORMAL == a.mode && !wcscmp(a.cwd, L"C:\\Users\\dev") &&
	        !wcscmp(cur_name(&a), L"file2.txt"));
	wf_app_destroy(&a);

	/* a file that does not exist: no move, the input stays */
	open_go(&a, &fp, &f, L"C:\\");
	begin(&a, &f);
	type(&a, L"C:\\Users\\dev\\nothing.txt");
	key(&a, WF_KEY_ENTER);
	settle(&fp);
	CHECK(&WF_MODE_GO == a.mode && !wcscmp(a.cwd, L"C:\\") &&
	        note_has(&a, "not found") &&
	        input_is(&a, L"C:\\Users\\dev\\nothing.txt"));
	wf_app_destroy(&a);
}

static void errors_keep_the_input_open(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Fake f;

	open_go(&a, &fp, &f, L"C:\\Users");
	begin(&a, &f);
	type(&a, L"C:\\bad<name");
	key(&a, WF_KEY_ENTER);
	CHECK(&WF_MODE_GO == a.mode && a.go.note_is_error && 0 == fp.nq &&
	        !wcscmp(a.cwd, L"C:\\Users"));
	ctrl(&a, 'u');
	type(&a, L"C:name");
	key(&a, WF_KEY_ENTER);
	CHECK(&WF_MODE_GO == a.mode && a.go.note_is_error && 0 == fp.nq);
	ctrl(&a, 'u');
	type(&a, L"C:\\secret");
	key(&a, WF_KEY_ENTER);
	settle(&fp);
	CHECK(&WF_MODE_GO == a.mode && !wcscmp(a.cwd, L"C:\\Users") &&
	        note_has(&a, "denied") && input_is(&a, L"C:\\secret"));
	ctrl(&a, 'u');
	type(&a, L"%NOPE%");
	key(&a, WF_KEY_ENTER);
	CHECK(&WF_MODE_GO == a.mode && note_has(&a, "NOPE"));
	ctrl(&a, 'u');
	key(&a, WF_KEY_ENTER); /* nothing typed */
	CHECK(&WF_MODE_GO == a.mode && a.go.note_is_error && 0 == fp.nq);
	wf_app_destroy(&a);
}

static void dash_goes_back_and_empty_input_lists_folders(void)
{
	unsigned starts;
	WfApp a;
	FakePort fp = {.app = &a};
	Fake f;

	open_go(&a, &fp, &f, L"C:\\Users");
	begin(&a, &f);
	type(&a, L"-");
	key(&a, WF_KEY_ENTER);
	CHECK(&WF_MODE_GO == a.mode && note_has(&a, "revious") && 0 == fp.nq);
	key(&a, WF_KEY_ESC);
	/* go somewhere, then back */
	begin(&a, &f);
	type(&a, L"C:\\Users\\dev");
	key(&a, WF_KEY_ENTER);
	settle(&fp);
	CHECK(!wcscmp(a.cwd, L"C:\\Users\\dev") &&
	        !wcscmp(a.prev_dir, L"C:\\Users"));
	wf_app_path_prompt_begin(&a);
	/* empty: the folders directly under the shown one are asked for */
	starts = f.starts;
	CHECK(WF_GO_FS == a.go.source && f.starts >= 1 && a.go.dirs_only &&
	        f.dirs_only && /* the worker is told: it sends folders only */
	        !wcscmp(f.dir, L"C:\\Users\\dev") && !wcscmp(f.prefix, L""));
	{
		const wchar_t *const names[] = {L"Docs", L"Music"};
		const uint32_t flags[] = {WF_ENT_DIR, WF_ENT_DIR};

		batch(&a, &f, f.gen, true, false, names, flags, 2);
		CHECK(2 == a.go.cand_n && !wcscmp(a.go.cand[0].name, L"Docs"));
		key(&a, WF_KEY_DOWN);
		key(&a, WF_KEY_DOWN);
		key(&a, WF_KEY_TAB); /* completes: no move */
		CHECK(input_is(&a, L"Music\\") &&
		        !wcscmp(a.cwd, L"C:\\Users\\dev"));
	}
	(void)starts;
	key(&a, WF_KEY_ESC);
	begin(&a, &f);
	type(&a, L"-");
	key(&a, WF_KEY_ENTER);
	settle(&fp);
	CHECK(&WF_MODE_NORMAL == a.mode && !wcscmp(a.cwd, L"C:\\Users"));
	wf_app_destroy(&a);
}

static void aliases_are_offered_and_resolved(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Fake f;

	open_go(&a, &fp, &f, L"C:\\");
	begin(&a, &f);
	type(&a, L"@");
	CHECK(WF_GO_ALIAS == a.go.source && WF_SMART_ALIASES == a.go.cand_n &&
	        0 == f.starts);
	type(&a, L"hom");
	CHECK(1 == a.go.cand_n && !wcscmp(a.go.cand[0].name, L"home"));
	key(&a, WF_KEY_DOWN);
	key(&a, WF_KEY_ENTER);
	settle(&fp);
	CHECK(&WF_MODE_NORMAL == a.mode && !wcscmp(a.cwd, L"C:\\Users\\dev"));
	wf_app_destroy(&a);
}

static void paste_strips_quotes_and_refuses_several_lines(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Fake f;

	open_go(&a, &fp, &f, L"C:\\");
	begin(&a, &f);
	f.clip = "\"C:\\Users\\dev\"\r\n";
	ctrl(&a, 'v');
	CHECK(input_is(&a, L"C:\\Users\\dev"));
	ctrl(&a, 'u');
	f.clip = "C:\\Users\r\nC:\\other";
	ctrl(&a, 'v');
	CHECK(0 == a.go.len && a.go.note_is_error && note_has(&a, "one line"));
	f.clip = NULL;
	ctrl(&a, 'v');
	CHECK(0 == a.go.len && a.go.note_is_error);
	/* nothing was run or navigated */
	CHECK(0 == fp.nq && !wcscmp(a.cwd, L"C:\\"));
	wf_app_destroy(&a);
}

static void editing_keys(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Fake f;

	open_go(&a, &fp, &f, L"C:\\");
	begin(&a, &f);
	type(&a, L"C:\\Users\\dev");
	key(&a, WF_KEY_CTRL_BACKSPACE); /* a whole element */
	CHECK(input_is(&a, L"C:\\Users\\"));
	key(&a, WF_KEY_CTRL_BACKSPACE);
	CHECK(input_is(&a, L"C:\\"));
	key(&a, WF_KEY_HOME);
	key(&a, WF_KEY_DELETE);
	CHECK(input_is(&a, L":\\"));
	key(&a, WF_KEY_END);
	key(&a, WF_KEY_LEFT);
	ch(&a, 'x');
	CHECK(input_is(&a, L":x\\"));
	/* surrogate pairs are never split */
	ctrl(&a, 'u');
	ch(&a, 0x1F600);
	ch(&a, 'z');
	CHECK(3 == a.go.len && 0xD83D == a.go.text[0] &&
	        0xDE00 == a.go.text[1]);
	key(&a, WF_KEY_LEFT);
	key(&a, WF_KEY_LEFT);
	CHECK(0 == a.go.cursor);
	key(&a, WF_KEY_DELETE);
	CHECK(1 == a.go.len && L'z' == a.go.text[0]);
	ch(&a, 0xD800);
	ch(&a, 0x110000);
	ch(&a, 0x07);
	CHECK(1 == a.go.len);
	wf_app_destroy(&a);
}

static void it_draws_at_every_size(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Fake f;
	const wchar_t *const names[] = {
	        L"alpha", L"beta", L"gamma", L"delta", L"epsilon"};

	open_go(&a, &fp, &f, L"C:\\Users\\dev");
	begin(&a, &f);
	type(&a, L"~\\");
	batch(&a, &f, f.gen, true, true, names, NULL, 5);
	key(&a, WF_KEY_DOWN);
	for (int cols = 1; cols <= 140; cols += (cols < 20 ? 1 : 13)) {
		for (int rows = 1; rows <= 40; rows += (rows < 12 ? 1 : 7)) {
			WfFrame *fr;

			a.cols = cols;
			a.rows = rows;
			fr = wf_frame_new(cols, rows, &WF_STYLE_BASE);
			CHECK(NULL != fr);
			if (!fr) {
				continue;
			}
			wf_app_draw(&a, fr);
			wf_frame_free(fr);
		}
	}
	wf_app_destroy(&a);
}

int main(void)
{
	ctrl_l_prefills_selects_and_replaces();
	typing_asks_for_the_newest_search_and_drops_stale_answers();
	a_reopened_line_does_not_take_an_old_answer();
	nothing_changes_before_enter();
	enter_goes_to_the_typed_path_never_to_a_candidate_unasked();
	tilde_lists_home_and_enter_goes_home();
	tab_completes_without_going_and_cycles();
	down_scrolls_the_window_and_asks_for_more_once();
	a_file_goes_to_its_folder_with_the_file_chosen();
	errors_keep_the_input_open();
	dash_goes_back_and_empty_input_lists_folders();
	aliases_are_offered_and_resolved();
	paste_strips_quotes_and_refuses_several_lines();
	editing_keys();
	it_draws_at_every_size();
	return TEST_RESULT();
}
