/* The fuzzy search in the application (DR-DRAFT-023): the key, the live
 * results and the marked letters, selection, Enter and Esc, answers of other
 * generations, the worker's release when the list changes or the search ends,
 * paging past the first K, a folder change, characters outside the BMP. The
 * listing and the worker are fakes: no disk, no thread. */
#include <stdlib.h>
#include <string.h>

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

/* Answers the newest request with these names (a trailing '/' makes a folder,
 * a leading '.' a hidden file). */
static void deliver(WfApp *a, Port *p, const wchar_t *const *names, size_t n)
{
	WfDir *d = wf_dir_new();

	CHECK(d);
	for (size_t i = 0; i < n; i++) {
		size_t len = wcslen(names[i]);
		uint32_t fl = 0;

		if (len && L'/' == names[i][len - 1]) {
			len--;
			fl |= WF_ENT_DIR;
		}
		if (L'.' == names[i][0]) {
			fl |= WF_ENT_HIDDEN;
		}
		CHECK(WF_OK == wf_dir_add(d, names[i], len, fl, 0, 0));
	}
	CHECK(WF_OK == wf_dir_sort(d, WF_SORT_NAME, 0));
	wf_app_on_listing(a, p->gen, WF_OK, d, NULL);
}

typedef struct
{
	unsigned starts, releases;
	uint32_t gen;
	WfFzJob job;
	bool has;
	WfStatus answer;
} Worker;

static WfStatus w_start(void *ctx, uint32_t gen, const WfFzJob *job)
{
	Worker *w = ctx;

	w->starts++;
	w->gen = gen;
	w->job = *job;
	w->has = WF_OK == w->answer;
	return w->answer;
}

static void w_release(void *ctx)
{
	Worker *w = ctx;

	w->releases++;
	w->has = false;
}

/* What the worker would answer for its request, carrying `gen`. */
static WfFzAnswer w_answer(const Worker *w, uint32_t gen)
{
	WfFzAnswer a = {0};
	WfFzTopK top;
	WfFzStats st;
	WfFzScratch *x = wf_fz_scratch_new();

	CHECK(x && WF_OK == wf_fz_topk_init(&top, w->job.k));
	CHECK(WF_OK ==
	        wf_fz_search(&w->job.query, w->job.e, w->job.vis, w->job.nvis,
	                x, &top, NULL, NULL, &st));
	a.gen = gen;
	a.hit = top.hit;
	a.nhit = top.n;
	a.matched = st.matched;
	wf_fz_scratch_free(x);
	return a;
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

static void open_with(
        WfApp *a, Port *p, const wchar_t *const *names, size_t n, int rows)
{
	memset(p, 0, sizeof *p);
	CHECK(WF_OK ==
	        wf_app_init(
	                a, 120, rows, (WfFsPort){p, p_submit}, L"C:\\Work"));
	deliver(a, p, names, n);
}

static const wchar_t *cur(const WfApp *a)
{
	const WfEntry *e = wf_view_current(&a->cur);

	return e ? e->name : L"";
}

/* The text of row y of the screen. */
static const char *row_text(const WfFrame *f, int y)
{
	static char row[1024];

	row[0] = '\0';
	for (int x = 0; x < f->cols; x++) {
		strcat_s(row, sizeof row, f->cells[y * f->cols + x].utf8);
	}
	return row;
}

static WfFrame *screen(const WfApp *a)
{
	WfFrame *f = wf_frame_new(a->cols, a->rows, &WF_STYLE_BASE);

	CHECK(f);
	wf_app_draw(a, f);
	return f;
}

/* The row (of the current pane) that holds `needle`, or -1. */
static int find_row(const WfFrame *f, const char *needle)
{
	for (int y = 3; y < f->rows - 2; y++) {
		if (strstr(row_text(f, y), needle)) {
			return y;
		}
	}
	return -1;
}

static const wchar_t *const CORPUS[] = {L"HTTPServer.c", L"README.md",
        L"cache_file_generator.c", L"config.go", L"config_file.go",
        L"http_server.c", L"main.c", L"main_test.c", L"read_me.txt"};
#define NCORPUS (sizeof CORPUS / sizeof *CORPUS)

static bool hit_is(const WfApp *a, size_t i, const wchar_t *name)
{
	const WfView *v = &a->cur;

	return i < a->fz.nhit && a->fz.hit[i].index < v->nvis &&
	        !wcscmp(v->dir->e[v->vis[a->fz.hit[i].index]].name, name);
}

static void s_opens_the_search_and_slash_stays_the_filter(void)
{
	WfApp a;
	Port p;

	open_with(&a, &p, CORPUS, NCORPUS, 30);
	ch(&a, 's');
	CHECK(&WF_MODE_FUZZY == a.mode && a.fz.active);
	key(&a, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == a.mode && !a.fz.active);
	ch(&a, '/');
	CHECK(&WF_MODE_FILTER == a.mode); /* the filter is not displaced */
	key(&a, WF_KEY_ESC);
	/* the command is in the table under its stable name */
	CHECK(wf_cmd_info(WF_FM_FUZZY) &&
	        0 == strcmp(wf_cmd_info(WF_FM_FUZZY)->name, "fm.fuzzy") &&
	        wf_cmd_by_name("fm.fuzzy") == wf_cmd_info(WF_FM_FUZZY));
	wf_app_destroy(&a);
}

static void an_empty_list_says_so_and_does_not_open(void)
{
	WfApp a;
	Port p;

	open_with(&a, &p, NULL, 0, 30);
	ch(&a, 's');
	CHECK(&WF_MODE_NORMAL == a.mode && !a.fz.active);
	CHECK(NULL != strstr(a.msg, "Nothing to search"));
	wf_app_destroy(&a);
}

static void results_follow_every_letter(void)
{
	WfApp a;
	Port p;

	open_with(&a, &p, CORPUS, NCORPUS, 30);
	ch(&a, 's');
	type(&a, L"c");
	CHECK(a.fz.nhit >= 3);
	type(&a, L"f");
	type(&a, L"g");
	CHECK(3 == a.fz.matched && 3 == a.fz.nhit);
	CHECK(hit_is(&a, 0, L"config.go") && hit_is(&a, 1, L"config_file.go") &&
	        hit_is(&a, 2, L"cache_file_generator.c"));
	/* the folder's own cursor is on the chosen result (so the preview follows) */
	CHECK(!wcscmp(cur(&a), L"config.go"));
	{
		WfFrame *f = screen(&a);
		int r0 = find_row(f, "config.go"),
		    r1 = find_row(f, "config_file.go"),
		    r2 = find_row(f, "cache_file_generator.c");

		CHECK(r0 > 0 && r1 == r0 + 1 && r2 == r1 + 1);
		CHECK(NULL == strstr(row_text(f, r0 - 1), "README"));
		/* the prompt, and the count at its right */
		CHECK(NULL != strstr(row_text(f, f->rows - 2), "Search: cfg"));
		CHECK(NULL != strstr(row_text(f, f->rows - 2), "3 matches"));
		wf_frame_free(f);
	}
	/* Backspace searches again for the shorter letters */
	key(&a, WF_KEY_BACKSPACE);
	CHECK(!wcscmp(a.fz.query, L"cf") && a.fz.matched >= 3);
	key(&a, WF_KEY_BACKSPACE);
	key(&a, WF_KEY_BACKSPACE);
	CHECK(0 == a.fz.nhit && L'\0' == a.fz.query[0]);
	wf_app_destroy(&a);
}

static void backspace_removes_a_whole_pair(void)
{
	WfApp a;
	Port p;
	static const wchar_t *const names[] = {
	        L"\xD842\xDFB7野家.txt", L"x.txt"};

	open_with(&a, &p, names, 2, 30);
	ch(&a, 's');
	ch(&a, 0x20BB7); /* beyond the BMP: two units */
	CHECK(2 == wcslen(a.fz.query) && 1 == a.fz.nhit);
	key(&a, WF_KEY_BACKSPACE);
	CHECK(0 == wcslen(a.fz.query));
	ch(&a, 0x1F600);
	ch(&a, 'x');
	key(&a, WF_KEY_BACKSPACE);
	CHECK(2 == wcslen(a.fz.query));
	key(&a, WF_KEY_BACKSPACE);
	CHECK(0 == wcslen(a.fz.query));
	wf_app_destroy(&a);
}

static void no_matches_is_said(void)
{
	WfApp a;
	Port p;
	WfFrame *f;
	const wchar_t *before;

	open_with(&a, &p, CORPUS, NCORPUS, 30);
	before = cur(&a);
	ch(&a, 's');
	type(&a, L"zzz");
	CHECK(0 == a.fz.nhit && 0 == a.fz.matched);
	f = screen(&a);
	CHECK(find_row(f, "No matches") > 0); /* in the list */
	CHECK(NULL !=
	        strstr(row_text(f, f->rows - 2), "No matches")); /* prompt */
	wf_frame_free(f);
	/* the cursor is where it was; Enter with nothing found changes nothing */
	CHECK(!wcscmp(cur(&a), before));
	key(&a, WF_KEY_ENTER);
	CHECK(&WF_MODE_NORMAL == a.mode && !wcscmp(cur(&a), before));
	wf_app_destroy(&a);
}

static void an_empty_query_shows_the_folder_as_it_is(void)
{
	WfApp a;
	Port p;
	WfFrame *f;

	open_with(&a, &p, CORPUS, NCORPUS, 30);
	ch(&a, 's');
	f = screen(&a);
	CHECK(find_row(f, "README.md") > 0 && find_row(f, "main.c") > 0);
	CHECK(NULL != strstr(row_text(f, f->rows - 2), "Search:"));
	wf_frame_free(f);
	wf_app_destroy(&a);
}

static void arrows_choose_and_enter_keeps_the_cursor_there(void)
{
	WfApp a;
	Port p;

	open_with(&a, &p, CORPUS, NCORPUS, 30);
	ch(&a, 's');
	type(&a, L"cfg");
	key(&a, WF_KEY_DOWN);
	CHECK(!wcscmp(cur(&a), L"config_file.go") && 1 == a.fz.sel);
	key(&a, WF_KEY_DOWN);
	CHECK(!wcscmp(cur(&a), L"cache_file_generator.c"));
	key(&a, WF_KEY_DOWN); /* nothing below */
	CHECK(2 == a.fz.sel);
	key(&a, WF_KEY_UP);
	CHECK(!wcscmp(cur(&a), L"config_file.go"));
	key(&a, WF_KEY_CTRL_UP);
	CHECK(!wcscmp(cur(&a), L"config.go"));
	key(&a, WF_KEY_CTRL_DOWN);
	CHECK(!wcscmp(cur(&a), L"cache_file_generator.c"));
	key(&a, WF_KEY_ENTER);
	/* the cursor stays on the entry; nothing was opened or selected */
	CHECK(&WF_MODE_NORMAL == a.mode && !a.fz.active && NULL == a.fz.hit);
	CHECK(!wcscmp(cur(&a), L"cache_file_generator.c"));
	CHECK(!wcscmp(a.cwd, L"C:\\Work"));
	CHECK(NULL == wf_sel_folder(&a.sel, a.cwd) ||
	        0 == wf_sel_folder(&a.sel, a.cwd)->n);
	wf_app_destroy(&a);
}

static void esc_puts_back_the_cursor_and_the_scroll(void)
{
	WfApp a;
	Port p;
	wchar_t(*buf)[24] = malloc(80 * sizeof *buf);
	const wchar_t *names[80];
	size_t top, cursor;

	CHECK(buf);
	for (int i = 0; i < 80; i++) {
		swprintf_s(buf[i], 24, L"item%02d.txt", i);
		names[i] = buf[i];
	}
	open_with(&a, &p, names, 80, 12); /* a window of 7 rows */
	wf_view_move(&a.cur, 40, wf_app_body_height(&a));
	top = a.cur.top;
	cursor = a.cur.cursor;
	CHECK(40 == cursor && top > 0);
	ch(&a, 's');
	type(&a, L"item7"); /* results are far from row 40 */
	CHECK(a.fz.nhit > 0 && a.cur.cursor != cursor);
	key(&a, WF_KEY_DOWN);
	key(&a, WF_KEY_ESC);
	CHECK(&WF_MODE_NORMAL == a.mode);
	CHECK(a.cur.cursor == cursor && a.cur.top == top);
	CHECK(!wcscmp(cur(&a), L"item40.txt"));
	/* the search left nothing behind */
	CHECK(NULL == a.fz.hit && NULL == a.fz.orig_name &&
	        NULL == a.fz.scratch && L'\0' == a.fz.query[0]);
	wf_app_destroy(&a);
	free(buf);
}

static void hidden_names_are_not_searched_unless_shown(void)
{
	WfApp a;
	Port p;
	static const wchar_t *const names[] = {L".config", L"config.go"};

	open_with(&a, &p, names, 2, 30);
	ch(&a, 's');
	type(&a, L"conf");
	CHECK(1 == a.fz.matched && hit_is(&a, 0, L"config.go"));
	key(&a, WF_KEY_ESC);
	wf_app_set_hidden(&a, true);
	ch(&a, 's');
	type(&a, L"conf");
	CHECK(2 == a.fz.matched);
	wf_app_destroy(&a);
}

/* The characters drawn underlined in row y, in order; returns how many. */
static int underlined(const WfFrame *f, int y, char out[64])
{
	int n = 0;

	out[0] = '\0';
	for (int x = 0; x < f->cols; x++) {
		const WfCell *c = &f->cells[y * f->cols + x];

		if ((c->attributes & WF_ATTR_UNDERLINE) && c->utf8[0] &&
		        ' ' != c->utf8[0]) {
			strcat_s(out, 64, c->utf8);
			n++;
		}
	}
	return n;
}

static void the_letters_that_matched_are_marked(void)
{
	WfApp a;
	Port p;
	WfFrame *f;
	char got[64];
	static const wchar_t *const names[] = {L"ConFiG.go", L"zzz.txt"};

	open_with(&a, &p, names, 2, 30);
	ch(&a, 's');
	type(&a, L"cfg");
	f = screen(&a);
	{
		int r = find_row(f, "ConFiG.go");

		CHECK(r > 0);
		/* exactly the three letters, in order, as they are written */
		CHECK(3 == underlined(f, r, got) && !strcmp(got, "CFG"));
		/* the rest of the row is plain */
		CHECK(NULL != strstr(row_text(f, r), "ConFiG.go"));
	}
	wf_frame_free(f);
	wf_app_destroy(&a);
}

static void wide_and_astral_characters_are_not_broken(void)
{
	WfApp a;
	Port p;
	WfFrame *f;
	static const wchar_t *const names[] = {
	        L"\xD842\xDFB7野家.txt", L"設定ファイル.txt", L"a😀b.txt"};

	open_with(&a, &p, names, 3, 30);
	ch(&a, 's');
	type(&a, L"野家");
	CHECK(1 == a.fz.nhit);
	f = screen(&a);
	{
		int r = find_row(f,
		        "\xF0\xA0\xAE\xB7"
		        "野家.txt"); /* U+20BB7 in UTF-8 */
		char got[64];

		CHECK(r > 0);
		CHECK(2 == underlined(f, r, got) &&
		        !strcmp(got, "野家")); /* each whole */
	}
	wf_frame_free(f);
	/* Hiragana finds Katakana, and the row is drawn whole */
	key(&a, WF_KEY_ESC);
	ch(&a, 's');
	type(&a, L"ふぁ");
	CHECK(1 == a.fz.nhit && hit_is(&a, 0, L"設定ファイル.txt"));
	f = screen(&a);
	CHECK(find_row(f, "設定ファイル.txt") > 0);
	wf_frame_free(f);
	/* a pair in the middle of the letters: a😀b */
	key(&a, WF_KEY_ESC);
	ch(&a, 's');
	type(&a, L"ab");
	CHECK(1 == a.fz.nhit && hit_is(&a, 0, L"a😀b.txt"));
	f = screen(&a);
	CHECK(find_row(f,
	              "a\xF0\x9F\x98\x80"
	              "b.txt") > 0);
	wf_frame_free(f);
	wf_app_destroy(&a);
}

static void moving_down_past_the_best_k_asks_for_more(void)
{
	WfApp a;
	Port p;
	wchar_t(*buf)[24] = malloc(300 * sizeof *buf);
	const wchar_t *names[300];

	CHECK(buf);
	for (int i = 0; i < 300; i++) {
		swprintf_s(buf[i], 24, L"file%03d.txt", i);
		names[i] = buf[i];
	}
	open_with(&a, &p, names, 300, 30);
	ch(&a, 's');
	type(&a, L"file");
	CHECK(64 == a.fz.nhit && 300 == a.fz.matched);
	{
		WfFrame *f = screen(&a);

		CHECK(NULL != strstr(row_text(f, f->rows - 2), "64 of 300"));
		wf_frame_free(f);
	}
	key(&a, WF_KEY_CTRL_DOWN);
	CHECK(63 == a.fz.sel);
	key(&a, WF_KEY_DOWN); /* the 65th: the next four times as many */
	CHECK(256 == a.fz.nhit && 64 == a.fz.sel);
	CHECK(!wcscmp(cur(&a), L"file064.txt"));
	/* the order did not change: the first ones are the same */
	CHECK(hit_is(&a, 0, L"file000.txt") && hit_is(&a, 63, L"file063.txt"));
	key(&a, WF_KEY_CTRL_DOWN);
	key(&a, WF_KEY_DOWN);
	CHECK(300 == a.fz.nhit && 256 == a.fz.sel);
	key(&a, WF_KEY_CTRL_DOWN);
	CHECK(299 == a.fz.sel && !wcscmp(cur(&a), L"file299.txt"));
	key(&a, WF_KEY_DOWN);
	CHECK(299 == a.fz.sel);
	wf_app_destroy(&a);
	free(buf);
}

#define BIG (WF_FZ_SYNC_MAX + 400)

static wchar_t (*big_names(const wchar_t **out))[24]
{
	wchar_t(*buf)[24] = malloc(BIG * sizeof *buf);

	CHECK(buf);
	for (unsigned i = 0; i < BIG; i++) {
		swprintf_s(buf[i], 24, L"f%05u.dat", i);
		out[i] = buf[i];
	}
	return buf;
}

static void open_big(WfApp *a, Port *p, Worker *w, wchar_t (**buf)[24])
{
	const wchar_t **names = malloc(BIG * sizeof *names);

	CHECK(names);
	*buf = big_names(names);
	memset(w, 0, sizeof *w);
	open_with(a, p, names, BIG, 30);
	wf_app_set_fuzzy(a, (WfFuzzyPort){w, w_start, w_release});
	free(names);
}

static void a_big_list_goes_to_the_worker_and_only_the_newest_answer_counts(
        void)
{
	WfApp a;
	Port p;
	Worker w;
	wchar_t(*buf)[24];
	uint32_t g1, g2;
	WfFzAnswer old, fresh;

	open_big(&a, &p, &w, &buf);
	ch(&a, 's');
	type(&a, L"f0");
	CHECK(2 == w.starts && a.fz.pending && 0 == a.fz.nhit);
	g1 = w.gen;
	old = w_answer(&w, g1);
	type(&a, L"01");
	g2 = w.gen;
	CHECK(4 == w.starts && g2 != g1);
	/* the answer of "f0" comes late: refused, and nothing is shown */
	wf_app_on_fuzzy(&a, &old);
	CHECK(0 == a.fz.nhit && a.fz.pending && 1 == a.fz.dropped);
	CHECK(NULL == old.hit);
	fresh = w_answer(&w, g2);
	wf_app_on_fuzzy(&a, &fresh);
	CHECK(!a.fz.pending && a.fz.nhit > 0 && a.fz.matched > 0);
	CHECK(!wcscmp(cur(&a), L"f00100.dat") || !wcsncmp(cur(&a), L"f001", 4));
	/* while the next one is out the previous answer stays on screen */
	type(&a, L"2");
	CHECK(a.fz.pending && a.fz.nhit > 0);
	{
		WfFrame *f = screen(&a);

		CHECK(NULL != strstr(row_text(f, f->rows - 2), "searching"));
		wf_frame_free(f);
	}
	/* an answer when no search is open is refused too */
	key(&a, WF_KEY_ESC);
	{
		WfFzAnswer late = w_answer(&w, w.gen);

		wf_app_on_fuzzy(&a, &late);
		CHECK(2 == a.fz.dropped && NULL == late.hit);
	}
	wf_app_destroy(&a);
	free(buf);
}

/* While the answer to the newest query is out, the entries on screen are for
 * the one before: Enter does not take one of them. */
static void enter_waits_for_the_answer_to_the_current_query(void)
{
	WfApp a;
	Port p;
	Worker w;
	wchar_t(*buf)[24];
	WfFzAnswer first, second;

	open_big(&a, &p, &w, &buf);
	ch(&a, 's');
	type(&a, L"f0");
	first = w_answer(&w, w.gen);
	wf_app_on_fuzzy(&a, &first);
	CHECK(!a.fz.pending && a.fz.nhit > 0);
	type(&a,
	        L"9"); /* a new query is out; the old entries are still shown */
	CHECK(a.fz.pending && a.fz.nhit > 0);
	key(&a, WF_KEY_ENTER);
	CHECK(a.fz.active && a.fz.pending); /* it waits */
	second = w_answer(&w, w.gen);
	wf_app_on_fuzzy(&a, &second);
	CHECK(!a.fz.pending);
	key(&a, WF_KEY_ENTER);
	CHECK(!a.fz.active); /* the answer is here: it chooses */
	wf_app_destroy(&a);
	free(buf);
}

static void ending_the_search_releases_the_worker(void)
{
	WfApp a;
	Port p;
	Worker w;
	wchar_t(*buf)[24];

	open_big(&a, &p, &w, &buf);
	ch(&a, 's');
	type(&a, L"f1");
	CHECK(0 == w.releases);
	key(&a, WF_KEY_ESC);
	CHECK(1 == w.releases && !a.fz.active);
	ch(&a, 's');
	type(&a, L"f2");
	{
		WfFzAnswer r = w_answer(&w, w.gen);

		wf_app_on_fuzzy(&a, &r);
	}
	key(&a, WF_KEY_ENTER);
	CHECK(2 == w.releases);
	/* destroying with a search out releases it before the listing goes */
	ch(&a, 's');
	type(&a, L"f3");
	CHECK(2 == w.releases);
	wf_app_destroy(&a);
	CHECK(3 == w.releases);
	free(buf);
}

static void emptying_the_query_releases_and_restores(void)
{
	WfApp a;
	Port p;
	Worker w;
	wchar_t(*buf)[24];
	const wchar_t *before;

	open_big(&a, &p, &w, &buf);
	before = cur(&a);
	ch(&a, 's');
	type(&a, L"f");
	{
		WfFzAnswer r = w_answer(&w, w.gen);

		wf_app_on_fuzzy(&a, &r);
	}
	key(&a, WF_KEY_DOWN);
	key(&a, WF_KEY_BACKSPACE);
	CHECK(w.releases >= 1 && 0 == a.fz.nhit && !a.fz.pending);
	CHECK(!wcscmp(cur(&a), before));
	wf_app_destroy(&a);
	free(buf);
}

static void a_new_listing_stops_the_worker_and_the_search_is_asked_again(void)
{
	WfApp a;
	Port p;
	Worker w;
	wchar_t(*buf)[24];
	unsigned starts;
	const wchar_t *kept;
	wchar_t name[24];

	open_big(&a, &p, &w, &buf);
	ch(&a, 's');
	type(&a, L"f001");
	{
		WfFzAnswer r = w_answer(&w, w.gen);

		wf_app_on_fuzzy(&a, &r);
	}
	key(&a, WF_KEY_DOWN);
	key(&a, WF_KEY_DOWN);
	kept = cur(&a);
	wcscpy_s(name, 24, kept);
	starts = w.starts;
	{
		unsigned rel = w.releases;

		/* another program changed the folder: a refresh of the same one */
		wf_app_refresh(&a, false);
		CHECK(p.gen > 0);
		{
			const wchar_t **names = malloc(BIG * sizeof *names);

			CHECK(names);
			for (unsigned i = 0; i < BIG; i++) {
				names[i] = buf[i];
			}
			deliver(&a, &p, names, BIG);
			free(names);
		}
		/* the worker was stopped before the old listing was freed; its
		 * answer is gone and the screen has none until it is asked again */
		CHECK(w.releases > rel);
		CHECK(0 == a.fz.nhit && a.fz.dirty);
		CHECK(wf_app_timeout(&a) == 0);
	}
	CHECK(wf_app_fuzzy_tick(&a)); /* asks again, for the same letters */
	CHECK(w.starts == starts + 1 && a.fz.pending && !a.fz.dirty);
	{
		WfFzAnswer r = w_answer(&w, w.gen);

		wf_app_on_fuzzy(&a, &r);
	}
	/* the same name is chosen again, not the first row */
	CHECK(!wcscmp(cur(&a), name) && 2 == a.fz.sel);
	wf_app_destroy(&a);
	free(buf);
}

static void another_folder_ends_the_search(void)
{
	WfApp a;
	Port p;
	Worker w;
	wchar_t(*buf)[24];
	uint32_t old_gen;
	WfFzAnswer r;

	open_big(&a, &p, &w, &buf);
	ch(&a, 's');
	type(&a, L"f00");
	old_gen = w.gen;
	r = w_answer(&w, old_gen);
	CHECK(WF_OK == wf_app_navigate(&a, L"C:\\Other", NULL, SIZE_MAX));
	{
		static const wchar_t *const names[] = {L"f001.txt", L"other.c"};

		deliver(&a, &p, names, 2);
	}
	CHECK(wf_app_fuzzy_tick(&a));
	CHECK(&WF_MODE_NORMAL == a.mode && !a.fz.active && NULL == a.fz.hit);
	/* the answer of the old folder arrives afterwards: refused */
	wf_app_on_fuzzy(&a, &r);
	CHECK(1 == a.fz.dropped && NULL == r.hit);
	wf_app_destroy(&a);
	free(buf);
}

static void another_screen_taking_over_ends_the_search(void)
{
	WfApp a;
	Port p;
	Worker w;
	wchar_t(*buf)[24];

	open_big(&a, &p, &w, &buf);
	ch(&a, 's');
	type(&a, L"f01");
	a.mode = &WF_MODE_INFO; /* a window opened by something else */
	CHECK(!wf_app_fuzzy_tick(&a));
	CHECK(!a.fz.active && NULL == a.fz.hit && w.releases >= 1);
	CHECK(&WF_MODE_INFO == a.mode); /* the other screen is left alone */
	a.mode = &WF_MODE_NORMAL;
	ch(&a, 's'); /* a fresh search starts clean */
	CHECK(&WF_MODE_FUZZY == a.mode && L'\0' == a.fz.query[0]);
	key(&a, WF_KEY_ESC);
	wf_app_destroy(&a);
	free(buf);
}

/* A list too large for the screen's thread is searched by the worker or not at
 * all: when the worker refuses the request, the search says so, shows nothing and
 * is not pending; the whole list is never searched here instead. */
static void a_worker_that_refuses_is_told_and_nothing_is_searched_here(void)
{
	WfApp a;
	Port p;
	Worker w;
	wchar_t(*buf)[24];

	open_big(&a, &p, &w, &buf);
	w.answer = WF_E_OS; /* start() fails */
	ch(&a, 's');
	type(&a, L"f0000");
	CHECK(!a.fz.pending && 0 == a.fz.nhit);
	CHECK(NULL != strstr(a.msg, "this large"));
	CHECK(&WF_MODE_FUZZY ==
	        a.mode); /* the search is still open: a letter may make it small */
	/* the worker comes back: the next letter is searched there */
	w.answer = WF_OK;
	type(&a, L"1");
	CHECK(a.fz.pending);
	wf_app_destroy(&a);
	free(buf);
}

static void without_a_port_a_large_list_is_not_searched_here(void)
{
	WfApp a;
	Port p;
	const wchar_t **names = malloc(BIG * sizeof *names);
	wchar_t(*buf)[24];

	CHECK(names);
	buf = big_names(names);
	open_with(&a, &p, names, BIG, 30);
	ch(&a, 's');
	type(&a, L"f0100");
	CHECK(!a.fz.pending && 0 == a.fz.nhit);
	CHECK(NULL != strstr(a.msg, "this large"));
	wf_app_destroy(&a);
	free(buf);
	free(names);
}

int main(void)
{
	s_opens_the_search_and_slash_stays_the_filter();
	an_empty_list_says_so_and_does_not_open();
	results_follow_every_letter();
	backspace_removes_a_whole_pair();
	no_matches_is_said();
	an_empty_query_shows_the_folder_as_it_is();
	arrows_choose_and_enter_keeps_the_cursor_there();
	esc_puts_back_the_cursor_and_the_scroll();
	hidden_names_are_not_searched_unless_shown();
	the_letters_that_matched_are_marked();
	wide_and_astral_characters_are_not_broken();
	moving_down_past_the_best_k_asks_for_more();
	a_big_list_goes_to_the_worker_and_only_the_newest_answer_counts();
	enter_waits_for_the_answer_to_the_current_query();
	ending_the_search_releases_the_worker();
	emptying_the_query_releases_and_restores();
	a_new_listing_stops_the_worker_and_the_search_is_asked_again();
	another_folder_ends_the_search();
	another_screen_taking_over_ends_the_search();
	a_worker_that_refuses_is_told_and_nothing_is_searched_here();
	without_a_port_a_large_list_is_not_searched_here();
	return TEST_RESULT();
}
