/* Folder sizes (D) in the application: what the key asks for, what it does not
 * ask for (links, archives), how the totals arrive and are drawn at the end of
 * a row, the state line, Esc, a newer listing, r, and the details window. The
 * worker is a fake, so no disk is touched. */
#include <stdlib.h>
#include <string.h>

#include "ui/wf_app_priv.h"
#include "wf_fake_fs.h"
#include "wf_test.h"

typedef struct
{
	unsigned starts, cancels;
	uint32_t gen;
	wchar_t dir[260], name[8][64];
	size_t n;
	WfStatus answer; /* what start() returns */
} Fake;

static WfStatus f_start(void *ctx,
        uint32_t gen,
        const wchar_t *dir,
        const wchar_t *const *names,
        size_t n)
{
	Fake *f = ctx;

	f->starts++;
	f->gen = gen;
	f->n = n;
	wcsncpy_s(f->dir, 260, dir, _TRUNCATE);
	for (size_t i = 0; i < n && i < 8; i++) {
		wcsncpy_s(f->name[i], 64, names[i], _TRUNCATE);
	}
	return f->answer;
}

static void f_cancel(void *ctx)
{
	((Fake *)ctx)->cancels++;
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

static void open_at(WfApp *a, FakePort *fp, Fake *f, const wchar_t *at)
{
	memset(f, 0, sizeof *f);
	CHECK(WF_OK == wf_app_init(a, 120, 30, fake_port(fp), at));
	wf_app_set_sizes(a, (WfSizePort){f, f_start, f_cancel});
}

/* The row of the current pane that holds `needle`, or "". */
static const char *row_with(const WfApp *a, const char *needle)
{
	static char row[1024];
	WfFrame *f = wf_frame_new(120, 30, &WF_STYLE_BASE);

	wf_app_draw(a, f);
	for (int y = 0; y < f->rows; y++) {
		row[0] = '\0';
		for (int x = 0; x < f->cols; x++) {
			strcat_s(row, sizeof row,
			        f->cells[y * f->cols + x].utf8);
		}
		if (strstr(row, needle)) {
			wf_frame_free(f);
			return row;
		}
	}
	wf_frame_free(f);
	return "";
}

static bool status_has(const WfApp *a, const char *needle)
{
	return NULL != strstr(a->msg, needle);
}

static WfSizeUpdate update(
        uint32_t gen, size_t index, uint64_t bytes, bool finished, bool partial)
{
	WfSizeUpdate u = {gen, index, bytes, 3, 2, finished, partial, WF_OK};

	return u;
}

static void d_asks_for_the_folders_of_this_list(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Fake f;

	open_at(&a, &fp, &f, L"C:\\Pics");
	ch(&a, 'D');
	CHECK(1 == f.starts && !wcscmp(f.dir, L"C:\\Pics"));
	CHECK(1 == f.n && !wcscmp(f.name[0], L"Sub"));
	CHECK(wf_app_dirsize_running(&a));
	CHECK(status_has(&a, "Counting folder sizes: 0 of 1"));
	wf_app_destroy(&a);
}

static void totals_are_drawn_at_the_end_of_the_row(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Fake f;
	WfSizeUpdate u;

	open_at(&a, &fp, &f, L"C:\\Pics");
	CHECK(row_with(&a, "Sub/")[0] != 0); /* the row is on screen */
	CHECK(NULL == strstr(row_with(&a, "Sub/"), "KiB"));
	ch(&a, 'D');
	/* running totals say "at least" */
	u = update(f.gen, 0, 1536, false, false);
	wf_app_on_sizes(&a, &u);
	CHECK(NULL != strstr(row_with(&a, "Sub/"), "1.5 KiB+"));
	CHECK(wf_app_dirsize_running(&a));
	/* the last word has no mark */
	u = update(f.gen, 0, 1536, true, false);
	wf_app_on_sizes(&a, &u);
	CHECK(NULL != strstr(row_with(&a, "Sub/"), "1.5 KiB"));
	CHECK(NULL == strstr(row_with(&a, "Sub/"), "KiB+"));
	CHECK(!wf_app_dirsize_running(&a));
	CHECK(status_has(&a, "Folder sizes counted: 1"));
	wf_app_destroy(&a);
}

static void a_count_that_was_cut_short_says_so(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Fake f;
	WfSizeUpdate u;

	open_at(&a, &fp, &f, L"C:\\Pics");
	ch(&a, 'D');
	u = update(f.gen, 0, 2048, true, true);
	wf_app_on_sizes(&a, &u);
	CHECK(NULL != strstr(row_with(&a, "Sub/"), "2.0 KiB+"));
	CHECK(status_has(&a, "only in part: 1"));
	/* a folder nothing could be read from is "-", not "0 B" */
	ch(&a, 'r');
	ch(&a, 'D');
	u = update(f.gen, 0, 0, true, true);
	u.files = u.folders = 0;
	u.st = WF_E_ACCESS;
	wf_app_on_sizes(&a, &u);
	CHECK(NULL == strstr(row_with(&a, "Sub/"), "0 B"));
	wf_app_destroy(&a);
}

static void an_old_request_is_ignored(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Fake f;
	WfSizeUpdate u;

	open_at(&a, &fp, &f, L"C:\\Pics");
	ch(&a, 'D');
	u = update(f.gen + 7, 0, 999999, true, false);
	wf_app_on_sizes(&a, &u);
	CHECK(wf_app_dirsize_running(&a));
	CHECK(NULL == strstr(row_with(&a, "Sub/"), "KiB"));
	/* an index outside the request is not a row either */
	u = update(f.gen, 5, 999999, true, false);
	wf_app_on_sizes(&a, &u);
	CHECK(wf_app_dirsize_running(&a));
	wf_app_destroy(&a);
}

static void esc_stops_the_count_and_keeps_what_was_counted(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Fake f;
	WfSizeUpdate u;

	open_at(&a, &fp, &f, L"C:\\Pics");
	ch(&a, 'D');
	u = update(f.gen, 0, 4096, false, false);
	wf_app_on_sizes(&a, &u);
	key(&a, WF_KEY_ESC);
	CHECK(1 == f.cancels && !wf_app_dirsize_running(&a));
	CHECK(status_has(&a, "Stopped. Folder sizes counted: 0 of 1"));
	/* what the folder had reached stays, marked "at least" */
	CHECK(NULL != strstr(row_with(&a, "Sub/"), "4.0 KiB+"));
	/* the worker may still deliver one more: it is not this count's */
	u = update(f.gen, 0, 8192, true, false);
	wf_app_on_sizes(&a, &u);
	CHECK(NULL != strstr(row_with(&a, "Sub/"), "4.0 KiB+"));
	/* a second Esc does what Esc always did */
	key(&a, WF_KEY_ESC);
	CHECK(1 == f.cancels);
	wf_app_destroy(&a);
}

static void a_link_is_not_counted_and_nothing_to_count_is_said(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Fake f;

	open_at(&a, &fp, &f, L"C:\\Pics");
	for (size_t i = 0; i < a.cur.dir->n; i++) {
		if (a.cur.dir->e[i].flags & WF_ENT_DIR) {
			a.cur.dir->e[i].flags |= WF_ENT_LINK;
		}
	}
	ch(&a, 'D');
	CHECK(0 == f.starts && !wf_app_dirsize_running(&a));
	CHECK(status_has(&a, "No folders to count here"));
	wf_app_destroy(&a);
}

static void where_it_cannot_run_it_says_so(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Fake f;

	open_at(&a, &fp, &f, L"C:\\Pics");
	wf_app_set_sizes(&a, (WfSizePort){0});
	ch(&a, 'D');
	CHECK(status_has(&a, "cannot be counted in this place"));
	wf_app_set_sizes(&a, (WfSizePort){&f, f_start, f_cancel});
	f.answer = WF_E_OS;
	ch(&a, 'D');
	CHECK(status_has(&a, "could not be started"));
	CHECK(!wf_app_dirsize_running(&a));
	CHECK(NULL == strstr(row_with(&a, "Sub/"), "KiB"));
	wf_app_destroy(&a);
}

static void totals_belong_to_their_folder_and_r_forgets_them(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Fake f;
	WfSizeUpdate u;

	open_at(&a, &fp, &f, L"C:\\Pics");
	ch(&a, 'D');
	u = update(f.gen, 0, 1536, true, false);
	wf_app_on_sizes(&a, &u);
	CHECK(NULL != strstr(row_with(&a, "Sub/"), "1.5 KiB"));
	/* the same name in another folder shows nothing */
	CHECK(NULL == wf_dirsizes_find(&a.dirsize, L"C:\\Users\\dev", L"Sub"));
	CHECK(NULL != wf_dirsizes_find(&a.dirsize, L"C:\\Pics", L"Sub"));
	ch(&a, 'r');
	CHECK(NULL == strstr(row_with(&a, "Sub/"), "KiB"));
	wf_app_destroy(&a);
}

static void a_count_for_a_folder_that_is_gone_from_the_screen_is_dropped(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Fake f;
	WfSizeUpdate u;

	open_at(&a, &fp, &f, L"C:\\Pics");
	ch(&a, 'D');
	free(a.cwd);
	a.cwd = _wcsdup(L"C:\\Users\\dev"); /* the person went elsewhere */
	u = update(f.gen, 0, 1536, false, false);
	wf_app_on_sizes(&a, &u);
	CHECK(!wf_app_dirsize_running(&a) && f.cancels >= 1);
	wf_app_destroy(&a);
}

static bool info_has(const WfApp *a, const char *needle)
{
	for (int i = 0; i < a->info.n; i++) {
		if (strstr(a->info.text[i], needle)) {
			return true;
		}
	}
	return false;
}

static void the_details_window_gives_the_total(void)
{
	WfApp a;
	FakePort fp = {.app = &a};
	Fake f;
	WfSizeUpdate u;

	open_at(&a, &fp, &f, L"C:\\Pics");
	wf_app_info_begin(&a);
	CHECK(!info_has(&a, "1,536 bytes"));
	key(&a, WF_KEY_ESC);
	ch(&a, 'D');
	u = update(f.gen, 0, 1536, true, false);
	wf_app_on_sizes(&a, &u);
	wf_app_info_begin(&a);
	CHECK(info_has(&a, "1,536 bytes (1.50 KiB), 3 files, 2 folders"));
	key(&a, WF_KEY_ESC);
	/* a partial count is "at least" in the window too */
	u = update(f.gen, 0, 1536, true, true);
	ch(&a, 'r');
	ch(&a, 'D');
	u = update(f.gen, 0, 1536, true, true);
	wf_app_on_sizes(&a, &u);
	wf_app_info_begin(&a);
	CHECK(info_has(&a, "at least"));
	wf_app_destroy(&a);
}

static void the_short_form_is_ascii_and_scaled(void)
{
	WfSizeUpdate u = update(1, 0, 0, true, false);
	char s[32];

	wf_dirsizes_short(&u, s, sizeof s);
	CHECK(!strcmp(s, "0 B"));
	u.bytes = 1023;
	wf_dirsizes_short(&u, s, sizeof s);
	CHECK(!strcmp(s, "1023 B"));
	u.bytes = 1024;
	wf_dirsizes_short(&u, s, sizeof s);
	CHECK(!strcmp(s, "1.0 KiB"));
	u.bytes = 5ull * 1024 * 1024 * 1024 + 512ull * 1024 * 1024;
	wf_dirsizes_short(&u, s, sizeof s);
	CHECK(!strcmp(s, "5.5 GiB"));
	u.bytes = UINT64_MAX;
	wf_dirsizes_short(&u, s, sizeof s);
	CHECK(NULL != strstr(s, "EiB"));
	u.partial = true;
	wf_dirsizes_short(&u, s, sizeof s);
	CHECK('+' == s[strlen(s) - 1]);
	/* nothing readable at all */
	u = update(1, 0, 0, true, true);
	u.files = u.folders = 0;
	u.st = WF_E_ACCESS;
	wf_dirsizes_short(&u, s, sizeof s);
	CHECK(!strcmp(s, "-"));
}

int main(void)
{
	d_asks_for_the_folders_of_this_list();
	totals_are_drawn_at_the_end_of_the_row();
	a_count_that_was_cut_short_says_so();
	an_old_request_is_ignored();
	esc_stops_the_count_and_keeps_what_was_counted();
	a_link_is_not_counted_and_nothing_to_count_is_said();
	where_it_cannot_run_it_says_so();
	totals_belong_to_their_folder_and_r_forgets_them();
	a_count_for_a_folder_that_is_gone_from_the_screen_is_dropped();
	the_details_window_gives_the_total();
	the_short_form_is_ascii_and_scaled();
	return TEST_RESULT();
}
