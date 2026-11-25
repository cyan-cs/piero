/* The preview pane lists the directory under the cursor: what is requested,
 * when, what is shown for each answer, and that nothing the user navigates
 * with is touched. The listing port is a spy that the test answers itself. */
#include <stdlib.h>
#include <string.h>

#include "ui/wf_app_priv.h"
#include "wf_fake_fs.h"
#include "wf_test.h"

typedef struct
{
	unsigned calls;
	uint32_t gen;
	wchar_t path[128];
	WfStatus answer;
} Spy;

static WfStatus spy_submit(
        void *ctx, uint32_t gen, const wchar_t *path, const wchar_t *parent)
{
	Spy *s = ctx;

	CHECK(NULL == parent); /* a preview never asks for the parent too */
	s->calls++;
	s->gen = gen;
	wcscpy_s(s->path, 128, path);
	return s->answer;
}

typedef struct
{
	WfApp app;
	FakePort fs;
	Spy spy;
} Rig;

static void ch(WfApp *a, uint32_t c)
{
	WfEvent e = {WF_EV_CHAR, c, WF_KEY_NONE, 0};

	a->mode->on_event(a, &e);
}

/* Moves the clock and runs the time-driven part, as the loop does. */
static void advance(WfApp *a, uint32_t ms)
{
	a->now_ms += ms;
	(void)wf_app_tick(a);
}

/* Browsing C:\Users\dev (Docs/, file10.txt, file2.txt) with a spy port. */
static void rig(Rig *r, bool with_port)
{
	memset(r, 0, sizeof *r);
	r->fs.app = &r->app;
	CHECK(wf_app_init(&r->app, 120, 30, fake_port(&r->fs),
	              L"C:\\Users\\dev") == WF_OK);
	if (with_port) {
		wf_app_set_dirpreview(
		        &r->app, (WfDirPreviewPort){&r->spy, spy_submit});
	}
}

static WfDir *listing(unsigned files, unsigned folders, unsigned hidden)
{
	WfDir *d = wf_dir_new();
	wchar_t name[32];
	unsigned i;

	CHECK(NULL != d);
	for (i = 0; i < folders; i++) {
		swprintf(name, 32, L"folder%02u", i);
		CHECK(WF_OK ==
		        wf_dir_add(d, name, wcslen(name), WF_ENT_DIR, 0, 0));
	}
	for (i = 0; i < files; i++) {
		swprintf(name, 32, L"file%02u.txt", i);
		CHECK(WF_OK == wf_dir_add(d, name, wcslen(name), 0, 10, 0));
	}
	for (i = 0; i < hidden; i++) {
		swprintf(name, 32, L".dot%02u", i);
		CHECK(WF_OK ==
		        wf_dir_add(d, name, wcslen(name), WF_ENT_HIDDEN, 1, 0));
	}
	CHECK(WF_OK == wf_dir_sort(d, WF_SORT_NAME, false));
	return d;
}

static bool screen_has(const WfApp *a, const char *needle)
{
	WfFrame *f = wf_frame_new(a->cols, a->rows, &WF_STYLE_BASE);
	bool found = false;
	int y, x;

	wf_app_draw(a, f);
	for (y = 0; y < f->rows && !found; y++) {
		char row[2048] = "";

		for (x = 0; x < f->cols; x++) {
			strcat_s(row, sizeof row,
			        f->cells[y * f->cols + x].utf8);
		}
		found = NULL != strstr(row, needle);
	}
	wf_frame_free(f);
	return found;
}

/* Cursor on Docs/: nothing goes out until the cursor rests. */
static void the_request_is_debounced_and_names_the_folder(void)
{
	static Rig r;

	rig(&r, true);
	CHECK(WF_DIRPREV_WAIT == r.app.dirprev_state && 0 == r.spy.calls);
	CHECK(!screen_has(&r.app, "Loading..."));
	CHECK(wf_app_timeout(&r.app) <= WF_DIRPREV_DEBOUNCE_MS);
	advance(&r.app, WF_DIRPREV_DEBOUNCE_MS - 1);
	CHECK(0 == r.spy.calls);
	advance(&r.app, 1);
	CHECK(1 == r.spy.calls && WF_DIRPREV_LOADING == r.app.dirprev_state);
	CHECK(!wcscmp(r.spy.path, L"C:\\Users\\dev\\Docs"));
	CHECK(wf_app_timeout(&r.app) ==
	        WF_PREVIEW_LOADING_MS - WF_DIRPREV_DEBOUNCE_MS);
	advance(&r.app, WF_PREVIEW_LOADING_MS - WF_DIRPREV_DEBOUNCE_MS);
	CHECK(screen_has(&r.app, "Loading..."));
	CHECK(WF_WAIT_FOREVER == wf_app_timeout(&r.app));
	wf_app_destroy(&r.app);
}

static void the_listing_is_shown_with_folders_marked(void)
{
	static Rig r;

	rig(&r, true);
	advance(&r.app, WF_DIRPREV_DEBOUNCE_MS);
	wf_app_on_dirpreview(&r.app, r.spy.gen, WF_OK, listing(2, 1, 0));
	CHECK(WF_DIRPREV_READY == r.app.dirprev_state);
	CHECK(screen_has(&r.app, "3 items"));
	CHECK(screen_has(&r.app, "folder00/")); /* folders carry the slash */
	CHECK(screen_has(&r.app, "file00.txt"));
	CHECK(!screen_has(&r.app, "Loading..."));
	wf_app_destroy(&r.app);
}

/* The folder on screen is read again where it is (the watch, the reload): the
 * count and the list stay as they were, nothing says "updating" over them and
 * the status line does not turn to LOADING; that is for going somewhere else. */
static void reading_the_same_folder_again_changes_nothing_on_screen(void)
{
	static Rig r;

	rig(&r, true);
	advance(&r.app, WF_DIRPREV_DEBOUNCE_MS);
	wf_app_on_dirpreview(&r.app, r.spy.gen, WF_OK, listing(2, 1, 0));
	CHECK(screen_has(&r.app, "3 items") && screen_has(&r.app, "READY"));
	r.fs.defer = 1;
	wf_app_refresh(&r.app, true);
	CHECK(NULL != r.app.want_path);
	CHECK(screen_has(&r.app, "3 items") &&
	        screen_has(&r.app, "folder00/") && screen_has(&r.app, "READY"));
	CHECK(!screen_has(&r.app, "Updating") &&
	        !screen_has(&r.app, "LOADING"));
	wf_app_destroy(&r.app);
}

/* Holding j/k: every folder passed is superseded; only the one the cursor
 * rests on is requested. */
static void a_moving_cursor_does_not_enumerate_every_folder(void)
{
	static Rig r;

	rig(&r, true);
	ch(&r.app, 'j'); /* file2.txt: no listing wanted */
	CHECK(WF_DIRPREV_NONE == r.app.dirprev_state);
	ch(&r.app, 'k'); /* Docs again */
	CHECK(WF_DIRPREV_WAIT == r.app.dirprev_state);
	advance(&r.app, 10);
	ch(&r.app, 'j');
	ch(&r.app, 'k');
	advance(&r.app, WF_DIRPREV_DEBOUNCE_MS);
	CHECK(1 == r.spy.calls);
	wf_app_destroy(&r.app);
}

/* A late answer for a folder the cursor has left must not be shown. */
static void a_stale_answer_is_dropped(void)
{
	static Rig r;
	uint32_t old;

	rig(&r, true);
	advance(&r.app, WF_DIRPREV_DEBOUNCE_MS);
	old = r.spy.gen;
	ch(&r.app, 'j'); /* leaves Docs: a text file is under the cursor */
	wf_app_on_dirpreview(&r.app, old, WF_OK, listing(5, 0, 0));
	CHECK(WF_DIRPREV_NONE == r.app.dirprev_state &&
	        NULL == r.app.dirprev.dir);

	/* back on Docs: a new request, a new generation; the old answer, even
	 * arriving after the new one, changes nothing */
	ch(&r.app, 'k');
	advance(&r.app, WF_DIRPREV_DEBOUNCE_MS);
	CHECK(2 == r.spy.calls && r.spy.gen != old);
	wf_app_on_dirpreview(&r.app, r.spy.gen, WF_OK, listing(1, 0, 0));
	wf_app_on_dirpreview(&r.app, old, WF_OK, listing(9, 0, 0));
	CHECK(WF_DIRPREV_READY == r.app.dirprev_state &&
	        1 == r.app.dirprev.dir->n);
	wf_app_destroy(&r.app);
}

static void empty_denied_and_failed_listings_say_so(void)
{
	static Rig r;

	rig(&r, true);
	advance(&r.app, WF_DIRPREV_DEBOUNCE_MS);
	wf_app_on_dirpreview(&r.app, r.spy.gen, WF_OK, listing(0, 0, 0));
	CHECK(screen_has(&r.app, "Empty directory"));
	wf_app_destroy(&r.app);

	rig(&r, true);
	advance(&r.app, WF_DIRPREV_DEBOUNCE_MS);
	wf_app_on_dirpreview(&r.app, r.spy.gen, WF_E_ACCESS, NULL);
	CHECK(WF_DIRPREV_FAILED == r.app.dirprev_state);
	CHECK(screen_has(&r.app, "Access denied"));
	wf_app_destroy(&r.app);

	rig(&r, true);
	advance(&r.app, WF_DIRPREV_DEBOUNCE_MS);
	wf_app_on_dirpreview(&r.app, r.spy.gen, WF_E_NOENT, NULL);
	CHECK(screen_has(&r.app, "Directory not found"));
	wf_app_destroy(&r.app);

	/* the service refusing the request is shown, not swallowed */
	rig(&r, false);
	r.spy.answer = WF_E_NOMEM;
	wf_app_set_dirpreview(&r.app, (WfDirPreviewPort){&r.spy, spy_submit});
	advance(&r.app, WF_DIRPREV_DEBOUNCE_MS);
	CHECK(WF_DIRPREV_FAILED == r.app.dirprev_state);
	CHECK(screen_has(&r.app, "Out of memory"));
	wf_app_destroy(&r.app);
}

static void hidden_entries_follow_the_setting(void)
{
	static Rig r;

	rig(&r, true);
	advance(&r.app, WF_DIRPREV_DEBOUNCE_MS);
	wf_app_on_dirpreview(&r.app, r.spy.gen, WF_OK, listing(0, 0, 2));
	CHECK(screen_has(&r.app, "Only hidden items"));
	ch(&r.app, '.'); /* show hidden */
	CHECK(2 == r.app.dirprev.nvis && screen_has(&r.app, ".dot00"));
	CHECK(screen_has(&r.app, "2 items"));
	wf_app_destroy(&r.app);
}

/* Links are never followed; remote and cloud folders wait for P. */
static void links_and_remote_folders_are_not_listed_by_themselves(void)
{
	static Rig r;
	WfEntry *e;

	rig(&r, true);
	e = &r.app.cur.dir->e[0];
	CHECK(e->flags & WF_ENT_DIR);
	e->flags |= WF_ENT_LINK;
	wf_app_preview_clear(&r.app);
	wf_app_preview_sync(&r.app, false);
	advance(&r.app, 1000);
	CHECK(0 == r.spy.calls && screen_has(&r.app, "not listed"));
	ch(&r.app, 'P'); /* even when asked, a link is not followed */
	advance(&r.app, 1000);
	CHECK(0 == r.spy.calls);
	wf_app_destroy(&r.app);

	rig(&r, true);
	r.app.cur.dir->e[0].flags |= WF_ENT_CLOUD;
	wf_app_preview_clear(&r.app);
	wf_app_preview_sync(&r.app, false);
	advance(&r.app, 1000);
	CHECK(0 == r.spy.calls && screen_has(&r.app, "Press P"));
	ch(&r.app, 'P');
	advance(&r.app, WF_DIRPREV_DEBOUNCE_MS);
	CHECK(1 == r.spy.calls);
	wf_app_destroy(&r.app);

	/* a folder on a share */
	memset(&r, 0, sizeof r);
	r.fs.app = &r.app;
	CHECK(wf_app_init(&r.app, 120, 30, fake_port(&r.fs),
	              L"C:\\Users\\dev") == WF_OK);
	wf_app_set_dirpreview(&r.app, (WfDirPreviewPort){&r.spy, spy_submit});
	free(r.app.cwd);
	r.app.cwd = _wcsdup(L"\\\\server\\share");
	wf_app_preview_clear(&r.app);
	wf_app_preview_sync(&r.app, false);
	advance(&r.app, 1000);
	CHECK(0 == r.spy.calls && screen_has(&r.app, "Press P"));
	wf_app_destroy(&r.app);
}

/* Looking at a folder changes nothing the user navigates or operates with. */
static void previewing_changes_no_navigation_state(void)
{
	static Rig r;
	wchar_t cwd[128];
	size_t history, cursor, top, memory;

	rig(&r, true);
	ch(&r.app, 'j'); /* file2.txt */
	ch(&r.app, ' '); /* selected, cursor moves to file10.txt */
	ch(&r.app, 'k');
	ch(&r.app, 'k'); /* back on Docs */
	wcscpy_s(cwd, 128, r.app.cwd);
	history = r.app.history_n;
	cursor = r.app.cur.cursor;
	top = r.app.cur.top;
	memory = NULL != r.app.navigation_memory;
	CHECK(1 == wf_sel_count(&r.app.sel, r.app.cwd));

	advance(&r.app, WF_DIRPREV_DEBOUNCE_MS);
	wf_app_on_dirpreview(&r.app, r.spy.gen, WF_OK, listing(40, 10, 0));
	CHECK(WF_DIRPREV_READY == r.app.dirprev_state);
	CHECK(!wcscmp(r.app.cwd, cwd) && r.app.history_n == history);
	CHECK(r.app.cur.cursor == cursor && r.app.cur.top == top);
	CHECK((NULL != r.app.navigation_memory) == (0 != memory));
	CHECK(1 == wf_sel_count(&r.app.sel, r.app.cwd));
	CHECK(&WF_MODE_NORMAL == r.app.mode);
	CHECK(WF_XFER_NONE == r.app.slot.mode);
	wf_app_destroy(&r.app);
}

/* Without a service (tests, a failed start) the pane says what it always did. */
static void without_a_port_directories_show_metadata_only(void)
{
	static Rig r;

	rig(&r, false);
	CHECK(WF_DIRPREV_NONE == r.app.dirprev_state);
	CHECK(screen_has(&r.app, "Directory (metadata only)"));
	wf_app_destroy(&r.app);
}

/* A long list never draws past the pane, and says how many are not shown. */
static void a_long_list_is_cut_and_counted(void)
{
	static Rig r;

	rig(&r, true);
	advance(&r.app, WF_DIRPREV_DEBOUNCE_MS);
	wf_app_on_dirpreview(&r.app, r.spy.gen, WF_OK, listing(100, 0, 0));
	CHECK(screen_has(&r.app, "100 items") && screen_has(&r.app, "more"));
	CHECK(screen_has(&r.app, "file00.txt"));
	CHECK(!screen_has(&r.app, "file99.txt"));
	wf_app_destroy(&r.app);
}

int main(void)
{
	the_request_is_debounced_and_names_the_folder();
	the_listing_is_shown_with_folders_marked();
	reading_the_same_folder_again_changes_nothing_on_screen();
	a_moving_cursor_does_not_enumerate_every_folder();
	a_stale_answer_is_dropped();
	empty_denied_and_failed_listings_say_so();
	hidden_entries_follow_the_setting();
	links_and_remote_folders_are_not_listed_by_themselves();
	previewing_changes_no_navigation_state();
	without_a_port_directories_show_metadata_only();
	a_long_list_is_cut_and_counted();
	return TEST_RESULT();
}
