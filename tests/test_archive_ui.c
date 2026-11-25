/*
 * Archives in the three columns, through the app with a fake file system:
 * entering one, moving around inside, getting back out onto the file, the
 * cursor remembered per folder of the archive, an answer for an archive that
 * was left in the meantime thrown away, and every key that would change
 * something refused with a message instead of reaching a port.
 */
#include <stdio.h>
#include <string.h>

#include "archive/wf_vpath.h"
#include "ui/wf_app_priv.h"
#include "wf_test.h"
#include "base/wf_macro.h"

#define DIR_PATH L"C:\\d"
#define ZIP_TOP L"C:\\d\\p.zip|"
#define OTHER_TOP L"C:\\d\\q.zip|"

typedef struct
{
	WfApp *app;
	bool deferred;
	int queued;
	struct
	{
		uint32_t gen;
		wchar_t path[128];
	} pending[16];
	wchar_t asked[16][128];
	wchar_t asked_parent[16][128];
	int asks;
	int edit_calls, copy_calls, capture_calls, launch_calls, editor_calls;
} Fs;

static WfStatus listing(const wchar_t *path, WfDir **out)
{
	WfDir *d = wf_dir_new();
	WfStatus st = WF_OK;

	if (!d) {
		return WF_E_NOMEM;
	}
	if (!wcscmp(path, DIR_PATH)) {
		st = wf_dir_add(d, L"p.zip", 5, 0, 100, 0);
		if (WF_OK == st) {
			st = wf_dir_add(d, L"q.zip", 5, 0, 100, 0);
		}
		if (WF_OK == st) {
			st = wf_dir_add(d, L"x.txt", 5, 0, 10, 0);
		}
		if (WF_OK == st) {
			st = wf_dir_add(d, L"bad.zip", 7, 0, 10, 0);
		}
	} else if (!wcscmp(path, ZIP_TOP) || !wcscmp(path, OTHER_TOP)) {
		st = wf_dir_add(d, L"src", 3, WF_ENT_DIR, 0, 0);
		if (WF_OK == st) {
			st = wf_dir_add(d, L"README.md", 9, 0, 40, 0);
		}
		if (WF_OK == st) {
			st = wf_dir_add(d, L"nested.zip", 10, 0, 40, 0);
		}
		/* what the archive lister does */
		for (size_t i = 0; i < d->n; i++) {
			d->e[i].flags &= ~(uint32_t)WF_ENT_ARCHIVE;
		}
	} else if (!wcscmp(path, L"C:\\d\\p.zip|\\src")) {
		st = wf_dir_add(d, L"a.c", 3, 0, 5, 0);
		if (WF_OK == st) {
			st = wf_dir_add(d, L"b.c", 3, 0, 5, 0);
		}
		if (WF_OK == st) {
			st = wf_dir_add(d, L"c.c", 3, 0, 5, 0);
		}
	} else if (!wcscmp(path, L"C:\\d\\bad.zip|")) {
		st = WF_E_CORRUPT;
	} else {
		st = WF_E_NOENT;
	}
	if (WF_OK == st) {
		st = wf_dir_sort(d, WF_SORT_NAME, 0);
	}
	if (WF_OK != st) {
		wf_dir_free(d);
		return st;
	}
	*out = d;
	return WF_OK;
}

static void answer(Fs *fs, uint32_t gen, const wchar_t *path)
{
	WfDir *d = NULL;
	WfStatus st = listing(path, &d);

	wf_app_on_listing(fs->app, gen, st, d, NULL);
}

static WfStatus submit(
        void *ctx, uint32_t gen, const wchar_t *path, const wchar_t *parent)
{
	Fs *fs = ctx;

	if (fs->asks < 16) {
		wcscpy_s(fs->asked[fs->asks], 128, path);
		wcscpy_s(
		        fs->asked_parent[fs->asks], 128, parent ? parent : L"");
		fs->asks++;
	}
	if (fs->deferred) {
		fs->pending[fs->queued].gen = gen;
		wcscpy_s(fs->pending[fs->queued++].path, 128, path);
	} else {
		answer(fs, gen, path);
	}
	return WF_OK;
}

static WfStatus edit_submit(void *ctx, uint32_t gen, const WfEditRequest *r)
{
	(void)gen;
	(void)r;
	((Fs *)ctx)->edit_calls++;
	return WF_OK;
}

static WfStatus copy_submit(void *ctx, const WfCopySpec *s, uint32_t *job)
{
	(void)s;
	(void)job;
	((Fs *)ctx)->copy_calls++;
	return WF_OK;
}

static WfStatus capture(void *ctx,
        uint32_t gen,
        WfTransferMode mode,
        const wchar_t *dir,
        const wchar_t *const *names,
        size_t n,
        size_t hidden)
{
	(void)gen;
	(void)mode;
	(void)dir;
	(void)names;
	(void)n;
	(void)hidden;
	((Fs *)ctx)->capture_calls++;
	return WF_OK;
}

static WfStatus launch_submit(void *ctx, uint32_t gen, const WfLaunchRequest *r)
{
	(void)gen;
	(void)r;
	((Fs *)ctx)->launch_calls++;
	return WF_OK;
}

static int editor_open(void *ctx, const wchar_t *path, char *why, size_t cap)
{
	(void)path;
	(void)why;
	(void)cap;
	((Fs *)ctx)->editor_calls++;
	return 0;
}

static void key(WfApp *app, WfKey code)
{
	WfEvent ev = {WF_EV_KEY, 0, code, 0};

	app->mode->on_event(app, &ev);
}

static void ch(WfApp *app, uint32_t cp)
{
	WfEvent ev = {WF_EV_CHAR, cp, WF_KEY_NONE, 0};

	app->mode->on_event(app, &ev);
}

static void open_app(WfApp *app, Fs *fs)
{
	WfFsPort port = {fs, submit};

	memset(fs, 0, sizeof *fs);
	fs->app = app;
	CHECK(WF_OK == wf_app_init(app, 160, 20, port, DIR_PATH));
	app->edit = (WfEditPort){fs, edit_submit};
	app->copy = (WfCopyPort){fs, copy_submit, NULL, NULL, NULL, NULL};
	app->xfer = (WfXferPort){fs, capture, NULL};
	app->launch = (WfLaunchPort){fs, launch_submit};
	app->editor =
	        (WfEditorPort){fs, editor_open, NULL, NULL, NULL, NULL, NULL};
}

static const wchar_t *under_cursor(const WfApp *app)
{
	const WfEntry *e = wf_view_current(&app->cur);

	return e ? e->name : L"";
}

static void move_to(WfApp *app, const wchar_t *name)
{
	int guard = 0;

	key(app, WF_KEY_CTRL_UP);
	while (wcscmp(under_cursor(app), name) && guard++ < 20) {
		key(app, WF_KEY_DOWN);
	}
	CHECK(!wcscmp(under_cursor(app), name));
}

static void paths_are_told_apart(void)
{
	wchar_t *file;
	const wchar_t *inner;

	CHECK(wf_vpath_is(L"C:\\d\\p.zip|") && !wf_vpath_is(L"C:\\d\\p.zip"));
	CHECK(wf_vpath_split(L"C:\\d\\p.zip|\\a\\b.txt", &file, &inner) &&
	        !wcscmp(file, L"C:\\d\\p.zip") && !wcscmp(inner, L"a\\b.txt"));
	free(file);
	CHECK(wf_vpath_split(L"C:\\d\\p.zip|", &file, &inner) && !*inner);
	free(file);
	CHECK(!wf_vpath_split(L"C:\\d", &file, &inner));
	CHECK(!wf_vpath_split(L"|x", &file, &inner));
	CHECK(wf_archive_name_recognized(L"a.ZIP") &&
	        wf_archive_name_recognized(L"x.tar.gz") &&
	        wf_archive_name_recognized(L"y.tzst") &&
	        wf_archive_name_recognized(L"z.7z") &&
	        !wf_archive_name_recognized(L"zip") &&
	        !wf_archive_name_recognized(L".zip") &&
	        !wf_archive_name_recognized(L"a.zipx") &&
	        !wf_archive_name_recognized(L"a.txt"));
	{
		const wchar_t *leaf;
		size_t n;

		wf_vpath_leaf(L"C:\\d\\p.zip|", &leaf, &n);
		CHECK(5 == n && !wcsncmp(leaf, L"p.zip", 5));
		wf_vpath_leaf(L"C:\\d\\p.zip|\\src", &leaf, &n);
		CHECK(3 == n && !wcsncmp(leaf, L"src", 3));
	}
}

static void an_archive_is_entered_browsed_and_left(void)
{
	WfApp app;
	Fs fs;

	open_app(&app, &fs);
	CHECK(!wcscmp(under_cursor(&app), L"bad.zip"));
	move_to(&app, L"p.zip");
	CHECK(app.cur.dir->e[app.cur.vis[app.cur.cursor]].flags &
	        WF_ENT_ARCHIVE);
	key(&app, WF_KEY_ENTER);
	CHECK(!wcscmp(app.cwd, ZIP_TOP));
	CHECK(!wcscmp(fs.asked[fs.asks - 1], ZIP_TOP) &&
	        !wcscmp(fs.asked_parent[fs.asks - 1], DIR_PATH));
	/* nested.zip inside an archive is just a file */
	move_to(&app, L"nested.zip");
	key(&app, WF_KEY_ENTER);
	CHECK(!wcscmp(app.cwd, ZIP_TOP));
	move_to(&app, L"src");
	key(&app, WF_KEY_RIGHT);
	CHECK(!wcscmp(app.cwd, L"C:\\d\\p.zip|\\src"));
	CHECK(!wcscmp(fs.asked_parent[fs.asks - 1], ZIP_TOP));
	move_to(&app, L"c.c");
	key(&app, WF_KEY_LEFT);
	CHECK(!wcscmp(app.cwd, ZIP_TOP) && !wcscmp(under_cursor(&app), L"src"));
	/* back in: the cursor is where it was left */
	key(&app, WF_KEY_RIGHT);
	CHECK(!wcscmp(under_cursor(&app), L"c.c"));
	key(&app, WF_KEY_LEFT);
	key(&app, WF_KEY_LEFT);
	CHECK(!wcscmp(app.cwd, DIR_PATH) &&
	        !wcscmp(under_cursor(&app), L"p.zip"));
	CHECK(!wcscmp(fs.asked[fs.asks - 1], DIR_PATH));
	/* the same name in another archive is another folder */
	move_to(&app, L"q.zip");
	key(&app, WF_KEY_ENTER);
	CHECK(!wcscmp(app.cwd, OTHER_TOP) &&
	        !wcscmp(under_cursor(&app), L"src"));
	wf_app_destroy(&app);
}

static void a_bad_archive_says_so_and_stays(void)
{
	WfApp app;
	Fs fs;

	open_app(&app, &fs);
	move_to(&app, L"bad.zip");
	key(&app, WF_KEY_ENTER);
	CHECK(!wcscmp(app.cwd, DIR_PATH));
	CHECK(NULL != strstr(app.msg, "invalid archive"));
	wf_app_destroy(&app);
}

static void nothing_inside_an_archive_can_be_changed(void)
{
	static const uint32_t chars[] = {'a', 'R', 'x', 'p', 'e', '%', 'd'};
	WfApp app;
	Fs fs;
	size_t i;

	open_app(&app, &fs);
	move_to(&app, L"p.zip");
	key(&app, WF_KEY_ENTER);
	CHECK(!wcscmp(app.cwd, ZIP_TOP));
	move_to(&app, L"README.md");
	for (i = 0; i < WF_ARRAY_LEN(chars); i++) {
		app.msg[0] = '\0';
		ch(&app, chars[i]);
		CHECK(NULL != strstr(app.msg, "Read-only archive") ||
		        '%' == chars[i] || 'd' == chars[i]);
		CHECK(&WF_MODE_NORMAL == app.mode);
	}
	/* y is not a change: it picks what a later p extracts into a folder */
	app.msg[0] = '\0';
	ch(&app, 'y');
	CHECK(NULL != strstr(app.msg, "picked from the archive") &&
	        1 == app.arc_pick.n);
	ch(&app, 'p'); /* ... and p inside the archive is still refused */
	CHECK(NULL != strstr(app.msg, "Read-only archive"));
	app.msg[0] = '\0';
	key(&app, WF_KEY_DELETE);
	CHECK(NULL != strstr(app.msg, "Read-only archive"));
	app.msg[0] = '\0';
	key(&app, WF_KEY_F2);
	CHECK(NULL != strstr(app.msg, "Read-only archive"));
	app.msg[0] = '\0';
	key(&app, WF_KEY_ENTER); /* open with the default program */
	CHECK(NULL != strstr(app.msg, "Read-only archive"));
	CHECK(0 == fs.edit_calls && 0 == fs.copy_calls &&
	        0 == fs.capture_calls && 0 == fs.launch_calls &&
	        0 == fs.editor_calls);
	CHECK(&WF_MODE_NORMAL == app.mode);
	wf_app_destroy(&app);
}

static void an_answer_for_an_archive_left_behind_is_dropped(void)
{
	WfApp app;
	Fs fs;

	open_app(&app, &fs);
	fs.deferred = true;
	fs.queued = 0;
	move_to(&app, L"p.zip");
	key(&app, WF_KEY_ENTER); /* A: p.zip */
	move_to(&app, L"q.zip");
	key(&app, WF_KEY_ENTER);
	/* the app still shows the folder: nothing is applied until an answer
	 * for the newest request arrives */
	CHECK(!wcscmp(app.cwd, DIR_PATH));
	CHECK(2 == fs.queued);
	answer(&fs, fs.pending[1].gen, fs.pending[1].path);
	CHECK(!wcscmp(app.cwd, OTHER_TOP));
	answer(&fs, fs.pending[0].gen, fs.pending[0].path); /* the late one */
	CHECK(!wcscmp(app.cwd, OTHER_TOP));
	wf_app_destroy(&app);
}

static void every_archive_message_is_in_both_languages(void)
{
	static const WfMsg ids[] = {MSG_AR_READONLY, MSG_AR_READING,
	        MSG_MODE_ARCHIVE, MSG_NAV_AR_FORMAT, MSG_NAV_AR_CORRUPT,
	        MSG_NAV_AR_ENCRYPTED, MSG_NAV_AR_TOO_BIG, MSG_NAV_AR_CHANGED,
	        MSG_REASON_AR_ENCRYPTED, MSG_REASON_AR_LINK,
	        MSG_REASON_AR_TOO_BIG, MSG_REASON_AR_CHANGED,
	        MSG_REASON_AR_CORRUPT, MSG_REASON_AR_FORMAT,
	        MSG_REASON_AR_UNAVAILABLE, MSG_REASON_AR_ENTRY,
	        MSG_REASON_AR_ENTRY_PARTIAL, MSG_REASON_AR_SINGLE,
	        MSG_REASON_AR_SINGLE_PARTIAL};
	size_t i;

	for (i = 0; i < WF_ARRAY_LEN(ids); i++) {
		const char *en = wf_msg_in(WF_UILANG_EN, ids[i]);
		const char *ja = wf_msg_in(WF_UILANG_JA, ids[i]);

		CHECK(en && *en && ja && *ja && 0 != strcmp(en, ja));
	}
	/* the worker's English reasons are the catalogue's keys */
	CHECK(strcmp(wf_msg_reason(
	                     "Encrypted archive entry (no password support)"),
	              "Encrypted archive entry (no password support)") != 0 ||
	        WF_UILANG_EN == wf_i18n_language());
}

int main(void)
{
	paths_are_told_apart();
	an_archive_is_entered_browsed_and_left();
	a_bad_archive_says_so_and_stays();
	nothing_inside_an_archive_can_be_changed();
	an_answer_for_an_archive_left_behind_is_dropped();
	every_archive_message_is_in_both_languages();
	return TEST_RESULT();
}
