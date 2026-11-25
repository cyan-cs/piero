#include <stdio.h>
#include <string.h>
#include <time.h>

#include "fs/wf_dir.h"
#include "ui/wf_app.h"
#include "ui/wf_view.h"
#include "wf_fake_fs.h"
#include "wf_test.h"
#include "base/wf_macro.h"

static void test_natcmp(void)
{
	CHECK(wf_natcmp(L"file2", L"file10") < 0);
	CHECK(wf_natcmp(L"file10", L"file2") > 0);
	CHECK(0 ==
	        wf_natcmp(L"a02",
	                L"a2")); /* leading zeros: tie, ordinal breaks it */
	CHECK(wf_natcmp(L"a", L"B") < 0); /* ASCII case folded */
	CHECK(wf_natcmp(L"abc", L"abcd") < 0);
	CHECK(wf_natcmp(L"", L"a") < 0);
	CHECK(0 == wf_natcmp(L"x", L"x"));
	/* huge digit runs compare by length, no integer overflow */
	CHECK(wf_natcmp(L"n99999999999999999999999",
	              L"n100000000000000000000000") < 0);
}

static void test_empty_name(void)
{
	WfDir *d = wf_dir_new();

	CHECK(d);
	if (!d) {
		return;
	}
	CHECK(WF_OK == wf_dir_add(d, NULL, 0, 0, 0, 0));
	CHECK(d->n == 1 && !d->e[0].name[0] && !d->e[0].display[0]);
	wf_dir_free(d);
}

static int names_are(const WfDir *d, const wchar_t *const *want, size_t n)
{
	size_t i;

	if (d->n != n) {
		return 0;
	}
	for (i = 0; i < n; i++) {
		if (0 != wcscmp(d->e[i].name, want[i])) {
			return 0;
		}
	}
	return 1;
}

static void add(WfDir *d, const wchar_t *n, uint32_t fl, uint64_t size)
{
	CHECK(WF_OK == wf_dir_add(d, n, wcslen(n), fl, size, size));
}

static void test_sort(void)
{
	WfDir *d = wf_dir_new();
	static const wchar_t *const by_name[] = {
	        L"Docs", L"zdir", L"A", L"a", L"f2.txt", L"f10.txt"};

	add(d, L"f10.txt", 0, 5);
	add(d, L"a", 0, 1);
	add(d, L"zdir", WF_ENT_DIR, 0);
	add(d, L"f2.txt", 0, 3);
	add(d, L"A", 0, 1);
	add(d, L"Docs", WF_ENT_DIR, 0);

	CHECK(WF_OK == wf_dir_sort(d, WF_SORT_NAME, 0));
	/* dirs first; "A" and "a" tie naturally and fall to ordinal: 'A' < 'a'
	 */
	CHECK(names_are(d, by_name, 6));

	CHECK(WF_OK == wf_dir_sort(d, WF_SORT_SIZE, 1));
	/* descending flips everything except directories-first */
	CHECK(d->e[0].flags & WF_ENT_DIR && d->e[1].flags & WF_ENT_DIR);
	CHECK(0 == wcscmp(d->e[2].name, L"f10.txt"));
	wf_dir_free(d);
	wf_dir_free(NULL);
}

/* `[files] directories_first = false`: folders and files in one order. */
static void test_sort_directories_mixed(void)
{
	WfDir *d = wf_dir_new();
	static const wchar_t *const mixed[] = {
	        L"a.txt", L"Docs", L"m.txt", L"zdir"};
	static const wchar_t *const by_size_desc[] = {
	        L"m.txt", L"a.txt", L"zdir", L"Docs"};

	add(d, L"zdir", WF_ENT_DIR, 0);
	add(d, L"m.txt", 0, 9);
	add(d, L"Docs", WF_ENT_DIR, 0);
	add(d, L"a.txt", 0, 1);

	CHECK(WF_OK == wf_dir_sort_with(d, WF_SORT_NAME, 0, false));
	CHECK(names_are(d, mixed, 4));
	/* a folder still sorts by the key like a file: size 0 is the smallest */
	CHECK(WF_OK == wf_dir_sort_with(d, WF_SORT_SIZE, 1, false));
	CHECK(names_are(d, by_size_desc, 4));
	/* true is the ordinary sort */
	CHECK(WF_OK == wf_dir_sort_with(d, WF_SORT_NAME, 0, true));
	CHECK(d->e[0].flags & WF_ENT_DIR && d->e[1].flags & WF_ENT_DIR);
	CHECK(0 == wcscmp(d->e[2].name, L"a.txt"));
	wf_dir_free(d);
}

static void test_display_is_sanitized(void)
{
	WfDir *d = wf_dir_new();
	static const wchar_t evil[] = {L'a', 0x202E, L'b', 0x1B, L'['};

	CHECK(WF_OK == wf_dir_add(d, evil, 5, 0, 0, 0));
	CHECK(0 == strcmp(d->e[0].display, "a<RLO>b^[["));
	CHECK(0x202E == d->e[0].name[1]); /* the real name is untouched */
	wf_dir_free(d);
}

static void test_view(void)
{
	WfView v;
	WfDir *d = NULL;

	wf_view_init(&v);
	CHECK(WF_OK == fake_list(L"C:\\Users", &d));
	CHECK(WF_OK == wf_view_set_dir(&v, d, NULL));
	CHECK(1 == v.nvis); /* .hidden is not shown */
	CHECK(0 == wcscmp(wf_view_current(&v)->name, L"dev"));

	CHECK(WF_OK == wf_view_set_hidden(&v, 1));
	CHECK(2 == v.nvis);
	wf_view_move(&v, 99, 5);
	CHECK(1 == v.cursor);
	wf_view_move(&v, -99, 5);
	CHECK(0 == v.cursor);
	/* hiding again with the cursor on a hidden entry must not dangle */
	wf_view_move(&v, 1, 5);
	CHECK(WF_OK == wf_view_set_hidden(&v, 0));
	CHECK(1 == v.nvis && 0 == v.cursor);

	/* select-by-name, empty and NULL dirs */
	CHECK(WF_OK == fake_list(L"C:\\Users\\dev", &d));
	CHECK(WF_OK == wf_view_set_dir(&v, d, L"file2.txt"));
	CHECK(0 == wcscmp(wf_view_current(&v)->name, L"file2.txt"));
	CHECK(WF_OK == wf_view_set_dir(&v, NULL, NULL));
	CHECK(NULL == wf_view_current(&v));
	wf_view_move(&v, 1, 5); /* no crash on empty */

	/* a refresh swaps in the new listing and finds the entry again */
	CHECK(WF_OK == fake_list(L"C:\\Users\\dev", &d));
	CHECK(WF_OK == wf_view_replace_keep(&v, d, L"file2.txt"));
	CHECK(0 == wcscmp(wf_view_current(&v)->name, L"file2.txt"));
	CHECK(WF_OK == wf_view_replace_keep(&v, NULL, NULL));
	CHECK(NULL == wf_view_current(&v) && 0 == v.nvis);
	wf_view_free(&v);
}

/* The filter ignores case for more than ASCII letters. */
static void test_filter_ignores_case_beyond_ascii(void)
{
	WfView v;
	WfDir *d = wf_dir_new();
	const wchar_t a_umlaut[] = {0x00C4, L'r', L'g', L'e', L'r', 0};
	const wchar_t cyr[] = {
	        0x041F, 0x0440, 0x0438, 0x0432, 0x0435, 0x0442, 0};
	const wchar_t wide[] = {0xFF21, 0xFF22, L'.', L't', 0};
	const wchar_t q_umlaut[] = {0x00E4, L'R', L'G', 0};
	const wchar_t q_cyr[] = {0x043F, 0x0420, 0x0418, 0};
	const wchar_t q_wide[] = {0xFF41, 0xFF42, 0};

	add(d, a_umlaut, 0, 1);
	add(d, cyr, 0, 1);
	add(d, wide, 0, 1);
	add(d, L"plain.txt", 0, 1);
	wf_view_init(&v);
	CHECK(WF_OK == wf_view_set_dir(&v, d, NULL));
	CHECK(WF_OK == wf_view_set_filter(&v, q_umlaut));
	CHECK(1 == v.nvis && 0 == wcscmp(wf_view_current(&v)->name, a_umlaut));
	CHECK(WF_OK == wf_view_set_filter(&v, q_cyr));
	CHECK(1 == v.nvis && 0 == wcscmp(wf_view_current(&v)->name, cyr));
	CHECK(WF_OK == wf_view_set_filter(&v, q_wide));
	CHECK(1 == v.nvis && 0 == wcscmp(wf_view_current(&v)->name, wide));
	CHECK(WF_OK == wf_view_set_filter(&v, L"PLAIN"));
	CHECK(1 == v.nvis);
	wf_view_free(&v);
}

/* Japanese names (kana and CJK, which have no case) mixed with Latin letters
 * that do: the filter finds the Japanese part as it is and the Latin part
 * whatever its case, and a name of more units than the fold buffer holds is
 * found too. */
static void test_filter_finds_japanese_names(void)
{
	WfView v;
	WfDir *d = wf_dir_new();
	const wchar_t memo[] = {
	        0x30E1, 0x30E2, L'_', L'R', L'e', L'p', 0x5831, 0x544A, 0};
	const wchar_t other[] = {0x5199, 0x771F, L'.', L'p', L'n', L'g', 0};
	const wchar_t q_jp[] = {0x5831, 0x544A, 0};
	const wchar_t q_mixed[] = {L'R', L'E', L'P', 0x5831, 0};
	const wchar_t q_none[] = {0x30E1, 0x30E3, 0};
	wchar_t longname[400];
	size_t i;

	for (i = 0; i < 390; i++) {
		longname[i] = 0x5199;
	}
	longname[i++] = L'T';
	longname[i++] = L'a';
	longname[i++] = L'i';
	longname[i++] = L'L';
	longname[i] = 0;
	add(d, memo, 0, 1);
	add(d, other, 0, 1);
	add(d, longname, 0, 1);
	wf_view_init(&v);
	CHECK(WF_OK == wf_view_set_dir(&v, d, NULL));
	CHECK(WF_OK == wf_view_set_filter(&v, q_jp));
	CHECK(1 == v.nvis && 0 == wcscmp(wf_view_current(&v)->name, memo));
	CHECK(WF_OK == wf_view_set_filter(&v, L"rep"));
	CHECK(1 == v.nvis && 0 == wcscmp(wf_view_current(&v)->name, memo));
	CHECK(WF_OK == wf_view_set_filter(&v, q_mixed));
	CHECK(1 == v.nvis && 0 == wcscmp(wf_view_current(&v)->name, memo));
	CHECK(WF_OK == wf_view_set_filter(&v, q_none));
	CHECK(0 == v.nvis);
	CHECK(WF_OK == wf_view_set_filter(&v, L"tail"));
	CHECK(1 == v.nvis && 0 == wcscmp(wf_view_current(&v)->name, longname));
	wf_view_free(&v);
}

/* A filter made longer looks only at what matched before; the list must be the
 * one a fresh pass over the whole dir gives, however the filter got there:
 * typed letter by letter, cut back, replaced, with hidden names shown and not. */
static void test_filter_typed_equals_filter_set(void)
{
	static const wchar_t *const names[] = {L"alpha", L"Alphabet", L"beta",
	        L"alpine", L".alpha-hidden", L"gamma", L"ALP", L"al",
	        L"zeta.alp"};
	static const wchar_t *const steps[] = {L"a", L"al", L"alp", L"alph",
	        L"al", L"a", L"b", L"be", L"", L"alp", L"zeta", L"al", L"alpa"};
	WfDir *d = wf_dir_new();
	WfView typed, fresh;

	CHECK(d);
	for (size_t i = 0; i < WF_ARRAY_LEN(names); i++) {
		add(d, names[i], 0, 1);
		if (L'.' == names[i][0]) {
			d->e[d->n - 1].flags |= WF_ENT_HIDDEN;
		}
	}
	wf_view_init(&typed);
	wf_view_init(&fresh);
	CHECK(WF_OK == wf_view_set_dir(&typed, d, NULL));
	for (int hidden = 0; hidden < 2; hidden++) {
		CHECK(WF_OK == wf_view_set_hidden(&typed, 0 != hidden));
		for (size_t k = 0; k < WF_ARRAY_LEN(steps); k++) {
			WfDir *copy = wf_dir_new();

			CHECK(WF_OK == wf_view_set_filter(&typed, steps[k]));
			CHECK(copy);
			for (size_t i = 0; copy && i < d->n; i++) {
				add(copy, d->e[i].name, 0, 1);
				copy->e[i].flags = d->e[i].flags;
			}
			CHECK(WF_OK == wf_view_set_dir(&fresh, copy, NULL));
			CHECK(WF_OK == wf_view_set_hidden(&fresh, 0 != hidden));
			CHECK(WF_OK == wf_view_set_filter(&fresh, steps[k]));
			CHECK(typed.nvis == fresh.nvis);
			for (size_t i = 0; i < typed.nvis && i < fresh.nvis;
			        i++) {
				CHECK(typed.vis[i] == fresh.vis[i]);
			}
		}
	}
	wf_view_free(&typed);
	wf_view_free(&fresh);
}

/* The filter is folded once per pass; what matches does not change: the end of a
 * name, a name as long as the filter, a name shorter than it, a name too long
 * to be folded in one piece, no filter at all, and a filter cut at its limit. */
static void test_filter_edges(void)
{
	WfView v;
	WfDir *d = wf_dir_new();
	wchar_t longname[400];
	wchar_t longfilter[100];
	const wchar_t cyr_name[] = {0x041F, 0x0440, 0x0438, L'-', L'x', 0};
	const wchar_t cyr_q[] = {0x0438, L'-', L'X', 0};

	for (int i = 0; i < 390; i++) {
		longname[i] = L'a';
	}
	wcscpy_s(longname + 390, 10, L"-TheEnd");
	add(d, longname, 0, 1);
	add(d, L"abc", 0, 1);
	add(d, L"ab", 0, 1);
	add(d, L"a", 0, 1);
	add(d, cyr_name, 0, 1);
	wf_view_init(&v);
	CHECK(WF_OK == wf_view_set_dir(&v, d, NULL));
	CHECK(WF_OK == wf_view_set_filter(&v, L""));
	CHECK(5 == v.nvis); /* no filter: all */
	CHECK(WF_OK == wf_view_set_filter(&v, L"THEEND"));
	CHECK(1 == v.nvis && 0 == wcscmp(wf_view_current(&v)->name, longname));
	CHECK(WF_OK == wf_view_set_filter(&v, L"-theend"));
	CHECK(1 ==
	        v.nvis); /* a name longer than the folding buffer, matched at its end */
	CHECK(WF_OK == wf_view_set_filter(&v, L"abc"));
	CHECK(1 == v.nvis &&
	        0 ==
	                wcscmp(wf_view_current(&v)->name,
	                        L"abc")); /* as long as the filter */
	CHECK(WF_OK == wf_view_set_filter(&v, L"abcd"));
	CHECK(0 == v.nvis); /* longer than every name that starts so */
	CHECK(WF_OK == wf_view_set_filter(&v, L"b"));
	CHECK(2 ==
	        v.nvis); /* abc and ab: neither the long name nor the Cyrillic one has a b */
	CHECK(WF_OK == wf_view_set_filter(&v, cyr_q));
	CHECK(1 == v.nvis && 0 == wcscmp(wf_view_current(&v)->name, cyr_name));
	/* a filter over the limit is cut, as it always was */
	for (int i = 0; i < 99; i++) {
		longfilter[i] = L'a';
	}
	longfilter[99] = 0;
	CHECK(WF_OK == wf_view_set_filter(&v, longfilter));
	CHECK(1 ==
	        v.nvis); /* 63 units of 'a': only the long name has that many in a row */
	wf_view_free(&v);
}

static void test_view_scroll(void)
{
	WfView v;
	WfDir *d = wf_dir_new();
	wchar_t name[8];
	int i;

	for (i = 0; i < 50; i++) {
		swprintf(name, 8, L"f%02d", i);
		add(d, name, 0, 0);
	}
	wf_view_init(&v);
	CHECK(WF_OK == wf_view_set_dir(&v, d, NULL));
	for (i = 0; i < 20; i++) {
		wf_view_move(&v, 1, 10);
	}
	CHECK(20 == v.cursor &&
	        11 == v.top); /* cursor is the last visible row */
	wf_view_move(&v, -20, 10);
	CHECK(0 == v.top);
	wf_view_free(&v);
}

static void key(WfApp *a, uint32_t ch)
{
	WfEvent e = {WF_EV_CHAR, ch, WF_KEY_NONE, 0};

	a->mode->on_event(a, &e);
}

static void test_app_navigation(void)
{
	WfApp app;
	FakePort fp = {.app = &app};

	CHECK(WF_OK ==
	        wf_app_init(&app, 120, 30, fake_port(&fp), L"C:\\Users"));
	CHECK(0 == wcscmp(app.cwd, L"C:\\Users"));
	/* parent column shows C:\ with the cursor on "Users" */
	CHECK(0 == wcscmp(wf_view_current(&app.parent)->name, L"Users"));

	key(&app, 'l'); /* enter dev */
	CHECK(0 == wcscmp(app.cwd, L"C:\\Users\\dev") && '\0' == app.msg[0]);
	key(&app, 'j'); /* Docs/ is first (dirs first), then file2 < file10 */
	CHECK(0 == wcscmp(wf_view_current(&app.cur)->name, L"file2.txt"));
	key(&app, 'l'); /* a file: nothing happens */
	CHECK(0 == wcscmp(app.cwd, L"C:\\Users\\dev"));

	key(&app, 'h'); /* back; cursor returns to "dev" */
	CHECK(0 == wcscmp(app.cwd, L"C:\\Users"));
	CHECK(0 == wcscmp(wf_view_current(&app.cur)->name, L"dev"));
	key(&app, 'h');
	CHECK(0 == wcscmp(app.cwd, L"C:\\"));
	CHECK(NULL == app.parent.dir);
	/* above a drive is "This PC" with that drive selected; above that,
	 * nothing: it stays; Enter goes back into the drive */
	wf_pcplace_test_source(true, 1ul << 2, NULL);
	key(&app, 'h');
	CHECK(0 == wcscmp(app.cwd, WF_PCROOT));
	CHECK(0 == wcscmp(wf_view_current(&app.cur)->name, L"C:"));
	key(&app, 'h');
	CHECK(0 == wcscmp(app.cwd, WF_PCROOT));
	key(&app, 'l');
	CHECK(0 == wcscmp(app.cwd, L"C:\\"));
	wf_pcplace_test_source(false, 0, NULL);

	/* failure keeps the old listing and reports the reason */
	key(&app, 'k'); /* cursor was on Users; secret sorts before it */
	CHECK(0 == wcscmp(wf_view_current(&app.cur)->name, L"secret"));
	key(&app, 'l');
	CHECK(0 == wcscmp(app.cwd, L"C:\\"));
	CHECK(NULL != strstr(app.msg, "ACCESS"));
	key(&app, 'j'); /* any key clears the message */
	CHECK('\0' == app.msg[0]);

	key(&app, '.');
	CHECK(app.cur.show_hidden && 1 == app.parent.show_hidden);
	wf_app_destroy(&app);

	/* a bad start path is reported in the status bar, not by failing init
	 */
	CHECK(WF_OK ==
	        wf_app_init(&app, 120, 30, fake_port(&fp), L"C:\\nowhere"));
	CHECK(NULL != strstr(app.msg, "NOENT"));
	wf_app_destroy(&app);
}

static void test_view_edge_integer_bounds(void)
{
	WfView view = {0};
	view.nvis = SIZE_MAX;
	wf_view_move_edge(&view, 1, 5);
	CHECK(view.cursor == SIZE_MAX - 1 && view.top == SIZE_MAX - 5);
	wf_view_move_edge(&view, 0, 5);
	CHECK(0 == view.cursor && 0 == view.top);
	view.nvis = 0;
	wf_view_move_edge(&view, 1, 0);
	CHECK(0 == view.cursor && 0 == view.top);
	view.nvis = 1;
	wf_view_move_edge(&view, 1, -1);
	CHECK(0 == view.cursor && 0 == view.top);
}

static void test_sort_repeated_and_append(void)
{
	WfDir *d = wf_dir_new();
	CHECK(d != NULL);
	if (!d) {
		return;
	}
	const wchar_t *names[] = {
	        L"file10", L"File2", L"file2", L"日本語", L"alpha", L"Z"};
	for (size_t i = 0; i < 6; i++) {
		CHECK(wf_dir_add(d, names[i], wcslen(names[i]),
		              i & 1 ? WF_ENT_DIR : 0, (uint64_t)(i % 2),
		              (uint64_t)(i % 3)) == WF_OK);
	}
	const WfSortKey keys[] = {
	        WF_SORT_NAME, WF_SORT_SIZE, WF_SORT_DATE, WF_SORT_EXT};
	for (size_t key = 0; key < 4; key++) {
		for (unsigned direction = 0; direction < 2; direction++) {
			CHECK(wf_dir_sort(d, keys[key], direction != 0) ==
			        WF_OK);
			const wchar_t *order[6];
			for (size_t i = 0; i < 6; i++) {
				order[i] = d->e[i].name;
			}
			CHECK(wf_dir_sort(d, keys[key], direction != 0) ==
			        WF_OK);
			for (size_t i = 0; i < 6; i++) {
				CHECK(order[i] == d->e[i].name);
			}
		}
	}
	CHECK(wf_dir_sort(d, WF_SORT_NAME, false) == WF_OK);
	CHECK(wf_dir_add(d, L"0", 1, WF_ENT_DIR, 0, 0) == WF_OK);
	CHECK(wf_dir_sort(d, WF_SORT_NAME, false) == WF_OK);
	CHECK(!wcscmp(d->e[0].name, L"0"));
	wf_dir_free(d);
}

/* A walk that never draws a name does not have its display text made: no_display
 * leaves it NULL, and everything else about the entry is the same. */
static double fill_ms(bool no_display, size_t count)
{
	WfDir *d = wf_dir_new();
	clock_t t0;
	double ms;

	CHECK(d != NULL);
	d->limit = count + 1;
	d->no_display = no_display;
	t0 = clock();
	for (size_t i = 0; i < count; i++) {
		wchar_t name[32];

		swprintf(name, 32, L"a-file-name-%07zu.txt", i);
		CHECK(WF_OK == wf_dir_add(d, name, wcslen(name), 0, 1, 1));
	}
	ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
	CHECK(count == d->n);
	CHECK(no_display ? NULL == d->e[0].display : NULL != d->e[0].display);
	CHECK(0 == wcscmp(d->e[count - 1].name, L"a-file-name-0199999.txt") ||
	        count != 200000);
	wf_dir_free(d);
	return ms;
}

static void test_no_display_text(void)
{
	double with = 1e9, without = 1e9;

	for (int i = 0; i < 3; i++) { /* the best of three */
		double a = fill_ms(false, 200000), b = fill_ms(true, 200000);

		with = a < with ? a : with;
		without = b < without ? b : without;
	}
	printf("info: 200000 entries added: %.1f ms with display text, %.1f ms "
	       "without\n",
	        with, without);
	CHECK(without < with);
}

/* A listing sorted once as asked has the order of one sorted by name and then
 * again: the order ties are broken by is the name, so the first sort leaves
 * nothing behind that the second would keep. */
static WfDir *shuffled(size_t count)
{
	WfDir *d = wf_dir_new();
	uint32_t seed = 7;

	CHECK(d != NULL);
	d->limit = count + 1;
	d->no_display = true;
	for (size_t i = 0; i < count; i++) {
		wchar_t name[32];

		seed = seed * 1664525u + 1013904223u;
		swprintf(name, 32, L"n%08u.%s", seed >> 8,
		        0 == (seed & 3) ? L"txt" : L"dat");
		CHECK(WF_OK ==
		        wf_dir_add(d, name, wcslen(name),
		                0 == (seed & 31) ? WF_ENT_DIR : 0,
		                (seed >> 3) % 50, (seed >> 11) % 40));
	}
	return d;
}

static void test_one_sort_is_the_same_as_two(void)
{
	static const struct
	{
		WfSortKey key;
		bool desc, dirs;
	} spec[] = {{WF_SORT_DATE, false, true}, {WF_SORT_SIZE, true, true},
	        {WF_SORT_EXT, false, false}, {WF_SORT_NAME, true, true},
	        {WF_SORT_DATE, true, false}};
	enum
	{
		N = 20000
	};
	double twice_ms = 0, once_ms = 0;

	for (size_t s = 0; s < sizeof spec / sizeof *spec; s++) {
		WfDir *a = shuffled(N), *b = shuffled(N);
		clock_t t0 = clock();

		CHECK(WF_OK == wf_dir_sort(a, WF_SORT_NAME, false));
		CHECK(WF_OK ==
		        wf_dir_sort_with(
		                a, spec[s].key, spec[s].desc, spec[s].dirs));
		twice_ms += (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
		t0 = clock();
		CHECK(WF_OK ==
		        wf_dir_sort_with(
		                b, spec[s].key, spec[s].desc, spec[s].dirs));
		once_ms += (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
		CHECK(a->n == b->n && N == a->n);
		for (size_t i = 0; i < a->n; i++) {
			CHECK(0 == wcscmp(a->e[i].name, b->e[i].name));
		}
		wf_dir_free(a);
		wf_dir_free(b);
	}
	printf("info: %d entries, 5 orders: %.1f ms by name and then as asked, "
	       "%.1f ms as asked\n",
	        (int)N, twice_ms, once_ms);
	CHECK(once_ms < twice_ms);
}

int main(void)
{
	test_sort_repeated_and_append();
	test_view_edge_integer_bounds();
	test_natcmp();
	test_empty_name();
	test_sort();
	test_sort_directories_mixed();
	test_display_is_sanitized();
	test_view();
	test_view_scroll();
	test_filter_ignores_case_beyond_ascii();
	test_filter_finds_japanese_names();
	test_filter_edges();
	test_filter_typed_equals_filter_set();
	test_app_navigation();
	test_no_display_text();
	test_one_sort_is_the_same_as_two();
	return TEST_RESULT();
}
