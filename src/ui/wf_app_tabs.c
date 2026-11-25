/*
 * Tabs and places (DR-DRAFT-030; F10 of DR-DRAFT-019).
 *
 * A tab is a folder and the name under the cursor in it. The tab on screen is
 * the live state of the file manager (cwd, cursor); the others are the folder
 * and cursor name they had when they were left. Going to a tab is the ordinary
 * navigation to its folder with that name selected, so nothing here reads a
 * folder, keeps a listing or touches the selection (which is kept per folder
 * anyway). A tab strip is drawn in the title row only when there is more than
 * one tab. Tabs are for this run; they are not saved in the session.
 *
 * A place is a folder kept under a letter, `m` then the letter saves the folder
 * on screen and `'` then the letter goes there. They live in the configuration
 * file ([places]), written with the same save as the settings; nothing else
 * changes in the file.
 */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../archive/wf_vpath.h"
#include "../fs/wf_sftpplace_win.h"
#include "../path/wf_path.h"
#include "../text/wf_text.h"
#include "wf_app_priv.h"

size_t wf_app_tab_count(const WfApp *app)
{
	return app->tabs.n ? app->tabs.n : 1;
}

bool wf_app_archive_in_view(const WfApp *app)
{
	size_t i;

	if (wf_vpath_is(app->cwd)) {
		return true;
	}
	/* the tab on screen has no record: its folder is `cwd` */
	for (i = 0; i < app->tabs.n; i++) {
		if (i != app->tabs.cur && app->tabs.t[i].path &&
		        wf_vpath_is(app->tabs.t[i].path)) {
			return true;
		}
	}
	return false;
}

static void tab_free(WfApp *app, size_t i)
{
	free(app->tabs.t[i].path);
	free(app->tabs.t[i].name);
	app->tabs.t[i].path = app->tabs.t[i].name = NULL;
}

void wf_app_tabs_destroy(WfApp *app)
{
	for (size_t i = 0; i < WF_TABS_MAX; i++) {
		tab_free(app, i);
	}
	app->tabs.n = app->tabs.cur = 0;
}

static wchar_t *dup_or_null(const wchar_t *s)
{
	return s ? wf_path_dup(s) : NULL;
}

/* The tab on screen takes what the screen shows now. False: no memory (the
 * record keeps what it had). */
static bool store_current(WfApp *app)
{
	const WfEntry *e = wf_view_current(&app->cur);
	wchar_t *path = dup_or_null(app->cwd),
	        *name = dup_or_null(e ? e->name : NULL);

	if (!path || (e && !name)) {
		free(path);
		free(name);
		return false;
	}
	tab_free(app, app->tabs.cur);
	app->tabs.t[app->tabs.cur].path = path;
	app->tabs.t[app->tabs.cur].name = name;
	return true;
}

static void say_tab(WfApp *app, WfMsg id)
{
	char text[96];
	char a[16], b[16];

	snprintf(a, sizeof a, "%zu", app->tabs.cur + 1);
	snprintf(b, sizeof b, "%zu", wf_app_tab_count(app));
	(void)wf_msgf(text, sizeof text, id,
	        (WfMsgArg[]){WF_ARG_S(a), WF_ARG_S(b)}, 2);
	wf_app_set_msg(app, text);
}

void wf_app_cmd_tab_new(WfApp *app)
{
	size_t at;
	wchar_t *path, *name;
	const WfEntry *e;

	if (!app->cwd || app->want_path) {
		return; /* the folder is being replaced: the key can be pressed again */
	}
	if (wf_app_tab_count(app) >= WF_TABS_MAX) {
		char text[96], n[16];

		snprintf(n, sizeof n, "%u", (unsigned)WF_TABS_MAX);
		(void)wf_msgf(text, sizeof text, MSG_TAB_FULL, &WF_ARG_S(n), 1);
		wf_app_set_msg(app, text);
		return;
	}
	if (!app->tabs.n) {
		app->tabs.n = 1;
		app->tabs.cur = 0;
	}
	e = wf_view_current(&app->cur);
	path = dup_or_null(app->cwd);
	name = dup_or_null(e ? e->name : NULL);
	if (!path || (e && !name) || !store_current(app)) {
		free(path);
		free(name);
		if (1 == app->tabs.n) {
			app->tabs.n = 0; /* nothing was made */
		}
		wf_app_set_error(app, WF_E_NOMEM);
		return;
	}
	/* the new tab goes right after this one, and is the one on screen */
	at = app->tabs.cur + 1;
	for (size_t i = app->tabs.n; i > at; i--) {
		app->tabs.t[i] = app->tabs.t[i - 1];
	}
	app->tabs.t[at].path = path;
	app->tabs.t[at].name = name;
	app->tabs.n++;
	app->tabs.cur = at;
	say_tab(app, MSG_TAB_OPENED);
}

/* Goes to the folder of tab `to`, with its cursor name selected. The tab on
 * screen keeps what it shows, and stays the tab on screen until the folder of
 * `to` has arrived (wf_app_tabs_arrived): a folder that cannot be read leaves
 * the screen and the record of the tab it shows together. */
static void tab_go(WfApp *app, size_t to)
{
	WfStatus st;

	if (!store_current(app)) {
		wf_app_set_error(app, WF_E_NOMEM);
		return;
	}
	app->tabs.going =
	        true; /* before: the listing may be answered at once */
	app->tabs.to = to;
	app->tabs.closing = false;
	app->tabs.walk_gen = 0;
	st = wf_app_navigate(
	        app, app->tabs.t[to].path, app->tabs.t[to].name, SIZE_MAX);
	if (WF_OK != st) {
		app->tabs.going = false;
		wf_app_set_error(app, st);
	} else {
		app->tabs.walk_gen = app->nav_gen;
	}
}

/* The folder of tab `to` is on screen, and the walk was a close: the tab that
 * was on screen goes now, and `to` is the tab on screen. */
static void finish_close(WfApp *app, size_t to)
{
	size_t n = app->tabs.n, at = app->tabs.close_at;

	app->tabs.closing = false;
	if (at >= n || at == to || to >= n) {
		return; /* the tabs are not what the close was asked of */
	}
	tab_free(app, at);
	for (size_t i = at; i + 1 < n; i++) {
		app->tabs.t[i] = app->tabs.t[i + 1];
	}
	app->tabs.t[n - 1].path = app->tabs.t[n - 1].name = NULL;
	app->tabs.n = n - 1;
	app->tabs.cur = at < to ? to - 1 : to;
	if (1 == app->tabs.n) {
		wf_app_tabs_destroy(app); /* back to a single tab: no strip */
	}
	say_tab(app, MSG_TAB_CLOSED);
}

/* A folder has just become the one on screen: if it is the one a tab walk was
 * going to, that tab is now the one on screen. */
void wf_app_tabs_arrived(WfApp *app)
{
	size_t to = app->tabs.to;

	if (!app->tabs.going) {
		return;
	}
	app->tabs.going = false;
	if (app->tabs.walk_gen && app->tabs.walk_gen != app->nav_gen) {
		app->tabs.closing =
		        false; /* a newer request has replaced the walk */
		return;
	}
	if (to < app->tabs.n && app->tabs.t[to].path &&
	        !wcscmp(app->tabs.t[to].path, app->cwd)) {
		if (app->tabs.closing) {
			finish_close(app, to);
			return;
		}
		app->tabs.cur = to;
		say_tab(app, MSG_TAB_GOT);
	}
	app->tabs.closing = false;
}

void wf_app_cmd_tab_move(WfApp *app, int by)
{
	size_t n = wf_app_tab_count(app), to;

	if (n < 2) {
		wf_app_set_msg(app, wf_msg(MSG_TAB_ONLY));
		return;
	}
	if (!app->cwd || app->want_path) {
		return;
	}
	to = by < 0 ? (app->tabs.cur + n - 1) % n : (app->tabs.cur + 1) % n;
	tab_go(app, to);
}

/* Closes the tab on screen, and goes to the one next to it. The tab is not
 * removed until the neighbour's folder is on screen (wf_app_tabs_arrived): a
 * folder that cannot be read leaves every tab, the one on screen included, as
 * it was, and the screen and the records agree. */
void wf_app_cmd_tab_close(WfApp *app)
{
	size_t n = wf_app_tab_count(app), at = app->tabs.cur, to;
	wchar_t *path, *name;
	const WfTab *next;
	WfStatus st;

	if (n < 2) {
		wf_app_set_msg(app, wf_msg(MSG_TAB_LAST));
		return;
	}
	if (!app->cwd || app->want_path) {
		return;
	}
	/* the neighbour that is on screen next: the one before, else the next */
	to = at > 0 ? at - 1 : 1;
	next = &app->tabs.t[to];
	path = dup_or_null(next->path);
	name = dup_or_null(next->name);
	if (!path || (!name && next->name)) {
		free(path);
		free(name);
		wf_app_set_error(app, WF_E_NOMEM);
		return;
	}
	app->tabs.going =
	        true; /* before: the listing may be answered at once */
	app->tabs.to = to;
	app->tabs.closing = true;
	app->tabs.close_at = at;
	app->tabs.walk_gen = 0;
	st = wf_app_navigate(app, path, name, SIZE_MAX);
	if (WF_OK != st) {
		app->tabs.going = false;
		app->tabs.closing = false;
		wf_app_set_error(app, st);
	} else {
		app->tabs.walk_gen = app->nav_gen;
	}
	free(path);
	free(name);
}

/* The label of tab `i`: its number and the last part of its folder. */
static void tab_label(
        const WfApp *app, size_t i, bool current, char *out, size_t cap)
{
	const wchar_t *path = current ? app->cwd : app->tabs.t[i].path;
	const wchar_t *leaf = path;
	size_t len = path ? wcslen(path) : 0;
	WfBuf b;
	char name[40];

	if (path) {
		wf_path_leaf(path, &leaf, &len);
		if (!len) { /* a root: the whole of it */
			leaf = path;
			len = wcslen(path);
		}
	}
	wf_buf_init(&b);
	if (leaf && WF_OK == wf_sanitize_utf16(leaf, len, &b)) {
		wf_utf8_elide(name, sizeof name, wf_buf_cstr(&b), 12);
	} else {
		snprintf(name, sizeof name, "?");
	}
	wf_buf_free(&b);
	snprintf(out, cap, current ? "[%zu:%s]" : "%zu:%s", i + 1, name);
}

/* Cells a tab takes in the strip, with the space after it. */
static int tab_cells(const WfApp *app, size_t i)
{
	char label[80];

	tab_label(app, i, i == app->tabs.cur, label, sizeof label);
	return wf_utf8_cells(label, strlen(label)) + 1;
}

int wf_app_tabs_strip(const WfApp *app, WfFrame *f, int x, int width)
{
	int used = 0;
	size_t first = 0;

	if (wf_app_tab_count(app) < 2 || width < 5) {
		return 0;
	}
	if (width < 8) {
		/* no room for the labels: where the current tab is among them */
		char pos[16];
		int w;

		snprintf(pos, sizeof pos, "[%zu/%zu]", app->tabs.cur + 1,
		        app->tabs.n);
		w = wf_utf8_cells(pos, strlen(pos));
		if (w > width) {
			return 0;
		}
		wf_app_put_fit(f, x, 0, pos, &wf_theme_styles()->bar_bold, w);
		return w + 1;
	}
	/* The strip starts at the first tab from which the current one is still
	 * seen, so that the tab being worked in is never the one that is cut off;
	 * ".." in front says that tabs are hidden on the left. */
	while (first < app->tabs.cur) {
		int need = first > 0 ? 3 : 0;

		for (size_t i = first; i <= app->tabs.cur; i++) {
			need += tab_cells(app, i);
		}
		if (need <= width) {
			break;
		}
		first++;
	}
	if (first > 0) {
		wf_app_put_fit(f, x, 0, "..", &wf_app_style_bar, 2);
		used = 3;
	}
	for (size_t i = first; i < app->tabs.n; i++) {
		char label[80];
		int w;

		tab_label(app, i, i == app->tabs.cur, label, sizeof label);
		w = wf_utf8_cells(label, strlen(label));
		if (i == app->tabs.cur && used + w + 1 > width) {
			/* alone it is too wide: it is cut, not left out */
			wf_app_put_fit(f, x + used, 0, label,
			        &wf_theme_styles()->bar_bold, width - used);
			used = width;
			break;
		}
		if (used + w + 1 > width) {
			/* no room for the rest: say so, never draw half a label */
			if (used + 2 <= width) {
				wf_app_put_fit(f, x + used, 0, "..",
				        &wf_app_style_bar, 2);
				used += 2;
			}
			break;
		}
		wf_app_put_fit(f, x + used, 0, label,
		        i == app->tabs.cur ? &wf_theme_styles()->bar_bold
		                           : &wf_app_style_bar,
		        w);
		used += w + 1;
	}
	return used;
}

/* The same labels as one text, for the tests and for a screen reader. */
void wf_app_tabs_text(const WfApp *app, char *out, size_t cap)
{
	size_t used = 0;

	out[0] = '\0';
	for (size_t i = 0; wf_app_tab_count(app) >= 2 && i < app->tabs.n; i++) {
		char label[80];
		int w;

		tab_label(app, i, i == app->tabs.cur, label, sizeof label);
		w = snprintf(
		        out + used, cap - used, "%s%s", used ? " " : "", label);
		if (w < 0 || (size_t)w >= cap - used) {
			out[cap - 1] = '\0';
			return;
		}
		used += (size_t)w;
	}
}

static bool utf8_of(const wchar_t *w, char *out, size_t cap)
{
	int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, -1, out,
	        (int)cap, NULL, NULL);

	return n > 0;
}

void wf_app_cmd_place_set(WfApp *app)
{
	if (!app->cwd || app->want_path) {
		return;
	}
	app->mode = &WF_MODE_PLACE_SET;
	app->msg[0] = '\0';
}

void wf_app_cmd_place_go(WfApp *app)
{
	app->mode = &WF_MODE_PLACE_GO;
	app->msg[0] = '\0';
}

static void place_save(WfApp *app, char letter)
{
	WfConfigChange ch;
	char key[2] = {letter, 0}, why[96], text[96], shown[64];
	char utf8[WF_PLACE_BYTES];
	WfBuf b;
	size_t slot = (size_t)(letter - 'a');

	if (wf_vpath_is(app->cwd) || wf_sftp_place_is(app->cwd)) {
		wf_app_set_msg(app, wf_msg(MSG_PLACE_REFUSED_KIND));
		return;
	}
	if (!app->config || !app->settings_port.save) {
		wf_app_set_msg(app, wf_msg(MSG_PLACE_NO_CONFIG));
		return;
	}
	if (app->config->schema_version > WF_CONFIG_SCHEMA_VERSION) {
		wf_app_set_msg(app, wf_msg(MSG_SETTINGS_WHY_NEWER_SCHEMA));
		return;
	}
	if (!utf8_of(app->cwd, utf8, sizeof utf8) ||
	        strlen(utf8) >= sizeof ch.value) {
		wf_app_set_msg(app, wf_msg(MSG_PLACE_TOO_LONG));
		return;
	}
	memset(&ch, 0, sizeof ch);
	ch.section = "places";
	ch.key = key;
	snprintf(ch.value, sizeof ch.value, "%s", utf8);
	why[0] = '\0';
	if (WF_OK !=
	        app->settings_port.save(
	                app->settings_port.ctx, &ch, 1, why, sizeof why)) {
		(void)wf_msgf(text, sizeof text, MSG_PLACE_NOT_SAVED,
		        (WfMsgArg[]){WF_ARG_S(key), WF_ARG_S(why)}, 2);
		wf_app_set_msg(app, text);
		return;
	}
	snprintf(app->config->places[slot], sizeof app->config->places[slot],
	        "%s", utf8);
	wf_buf_init(&b);
	(void)wf_sanitize_utf16(app->cwd, wcslen(app->cwd), &b);
	wf_utf8_elide(shown, sizeof shown, wf_buf_cstr(&b), 56);
	wf_buf_free(&b);
	(void)wf_msgf(text, sizeof text, MSG_PLACE_SAVED,
	        (WfMsgArg[]){WF_ARG_S(key), WF_ARG_S(shown)}, 2);
	wf_app_set_msg(app, text);
	app->msg_keep_until = app->now_ms + WF_RECEIPT_KEEP_MS;
}

static void place_go(WfApp *app, char letter)
{
	wchar_t path[WF_PLACE_BYTES];
	char key[2] = {letter, 0}, text[96];
	const char *utf8;
	WfStatus st;
	size_t used = 0;

	utf8 = app->config ? app->config->places[letter - 'a'] : "";
	if (!utf8[0]) {
		(void)wf_msgf(
		        text, sizeof text, MSG_PLACE_UNSET, &WF_ARG_S(key), 1);
		wf_app_set_msg(app, text);
		return;
	}
	if (WF_OK !=
	        wf_utf8_to_utf16(utf8, strlen(utf8), path,
	                sizeof path / sizeof path[0] - 1, &used)) {
		wf_app_set_msg(app, wf_msg(MSG_PLACE_TOO_LONG));
		return;
	}
	path[used] = 0;
	st = wf_app_navigate(app, path, NULL, SIZE_MAX);
	if (WF_OK != st) {
		wf_app_set_error(app, st);
	}
}

static void place_draw(const WfApp *app, WfFrame *f)
{
	const bool set = &WF_MODE_PLACE_SET == app->mode;
	const char *lines[16];
	char item[WF_PLACE_ITEMS][160];
	char more[48];
	int n = 0;
	size_t shown = 0, have = 0;

	lines[n++] = wf_msg(set ? MSG_PLACE_SET_TITLE : MSG_PLACE_GO_TITLE);
	if (app->config) {
		for (size_t i = 0; i < WF_PLACES; i++) {
			have += app->config->places[i][0] ? 1u : 0u;
		}
	}
	for (size_t i = 0;
	        app->config && i < WF_PLACES && shown < WF_PLACE_ITEMS; i++) {
		WfBuf b;
		char path[96], key[2] = {(char)('a' + i), 0};

		if (!app->config->places[i][0]) {
			continue;
		}
		wf_buf_init(&b);
		(void)wf_sanitize_utf8(app->config->places[i],
		        strlen(app->config->places[i]), &b);
		wf_utf8_elide(path, sizeof path, wf_buf_cstr(&b), 56);
		wf_buf_free(&b);
		(void)wf_msgf(item[shown], sizeof item[shown], MSG_PLACE_ITEM,
		        (WfMsgArg[]){WF_ARG_S(key), WF_ARG_S(path)}, 2);
		lines[n++] = item[shown++];
	}
	if (have > shown) {
		char c[16];

		snprintf(c, sizeof c, "%zu", have - shown);
		(void)wf_msgf(
		        more, sizeof more, MSG_PLACE_MORE, &WF_ARG_S(c), 1);
		lines[n++] = more;
	}
	if (!have) {
		lines[n++] = wf_msg(MSG_PLACE_NONE);
		lines[n++] = wf_msg(MSG_PLACE_NONE_HELP);
	}
	lines[n++] = wf_msg(set ? MSG_PLACE_SET_HELP : MSG_PLACE_GO_HELP);
	wf_ui_alert_or_text(f, lines, n);
}

static WfAction place_event(WfApp *app, const WfEvent *ev)
{
	const bool set = &WF_MODE_PLACE_SET == app->mode;

	if (WF_EV_RESIZE == ev->type) {
		return WF_ACT_REDRAW;
	}
	if (WF_EV_RELEASE == ev->type) {
		return WF_ACT_NONE;
	}
	if (WF_EV_CHAR == ev->type && ev->ch >= 'a' && ev->ch <= 'z') {
		app->mode = &WF_MODE_NORMAL;
		if (set) {
			place_save(app, (char)ev->ch);
		} else {
			place_go(app, (char)ev->ch);
		}
		return WF_ACT_REDRAW;
	}
	if (WF_EV_KEY == ev->type || WF_EV_CHAR == ev->type) {
		/* Esc, or any other key: nothing is saved and nowhere is gone to */
		app->mode = &WF_MODE_NORMAL;
		return WF_ACT_REDRAW;
	}
	return WF_ACT_NONE;
}

const WfMode WF_MODE_PLACE_SET = {"PLACE", place_draw, place_event};
const WfMode WF_MODE_PLACE_GO = {"PLACE", place_draw, place_event};
