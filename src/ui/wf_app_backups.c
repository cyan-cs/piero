/*
 * The backups of a file (B): a replacing save keeps the previous version as
 * <name>~, else <name>~2 ... <name>~9. This window lists the ones that sit
 * beside the file the cursor is on, with size and time, so they can be looked
 * at, opened in the editor to compare or to restore from (Save As over the
 * original), or sent to the Recycle Bin.
 *
 * It lists names that match the scheme; it cannot tell a backup piero made
 * from a file the user named the same way, so nothing here removes anything
 * without the question Delete asks, which names the file. Removal goes
 * through the product's Recycle job (its File ID checks and its result), not
 * a second deletion path.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../path/wf_path.h"
#include "../text/wf_text.h"
#include "wf_app_priv.h"
#include "wf_info.h"

#define BACKUPS_MAX 9

void wf_app_backups_destroy(WfApp *app)
{
	for (size_t i = 0; i < app->backups.n; i++) {
		free(app->backups.names[i]);
	}
	free(app->backups.base);
	memset(&app->backups, 0, sizeof app->backups);
}

/* The name a backup is of: "a.txt~3" and "a.txt~" are backups of "a.txt". */
static size_t base_len(const wchar_t *name)
{
	size_t n = wcslen(name);

	if (n >= 2 && L'~' == name[n - 1]) {
		return n - 1;
	}
	if (n >= 3 && name[n - 1] >= L'2' && name[n - 1] <= L'9' &&
	        L'~' == name[n - 2]) {
		return n - 2;
	}
	return n;
}

/* Where the entry of that name is in the list, or SIZE_MAX. */
static size_t find_index(const WfApp *app, const wchar_t *name)
{
	for (size_t i = 0; app->cur.dir && i < app->cur.dir->n; i++) {
		if (!wcscmp(app->cur.dir->e[i].name, name)) {
			return i;
		}
	}
	return SIZE_MAX;
}

/* The entry of backup `i`: where it was when the window was opened, if the
 * entry there still has its name (one comparison; a frame does not search the
 * list), else searched for again, as the list may have been read anew. */
static const WfEntry *backup_entry(const WfApp *app, size_t i)
{
	const WfDir *d = app->cur.dir;
	const wchar_t *name = app->backups.names[i];
	size_t at = app->backups.at[i];

	if (d && at < d->n && !wcscmp(d->e[at].name, name)) {
		return &d->e[at];
	}
	at = find_index(app, name);
	return SIZE_MAX == at ? NULL : &d->e[at];
}

/* "<base>~" for k = 1, "<base>~k" for k = 2..9. */
static wchar_t *backup_name(const wchar_t *base, size_t k)
{
	size_t n = wcslen(base);
	wchar_t *out = malloc((n + 3) * sizeof *out);

	if (!out) {
		return NULL;
	}
	memcpy(out, base, n * sizeof *out);
	out[n] = L'~';
	if (k > 1) {
		out[n + 1] = (wchar_t)(L'0' + k);
		out[n + 2] = L'\0';
	} else {
		out[n + 1] = L'\0';
	}
	return out;
}

void wf_app_backups_begin(WfApp *app)
{
	const WfEntry *cur = wf_view_current(&app->cur);
	size_t len;
	char text[160], shown[96];
	WfBuf b;

	wf_app_backups_destroy(app);
	if (!cur || (cur->flags & WF_ENT_DIR)) {
		wf_app_set_msg(app, wf_msg(MSG_BACKUPS_NOT_A_FILE));
		return;
	}
	len = base_len(cur->name);
	app->backups.base = wf_path_dup_n(cur->name, len);
	if (!app->backups.base) {
		wf_app_set_error(app, WF_E_NOMEM);
		return;
	}
	for (size_t k = 1; k <= BACKUPS_MAX; k++) {
		wchar_t *name = backup_name(app->backups.base, k);
		size_t at;

		if (!name) {
			wf_app_backups_destroy(app);
			wf_app_set_error(app, WF_E_NOMEM);
			return;
		}
		at = find_index(app, name);
		if (SIZE_MAX != at &&
		        !(app->cur.dir->e[at].flags &
		                (WF_ENT_DIR | WF_ENT_LINK))) {
			app->backups.at[app->backups.n] = at;
			app->backups.names[app->backups.n++] = name;
		} else {
			free(name);
		}
	}
	if (0 == app->backups.n) {
		wf_buf_init(&b);
		(void)wf_sanitize_utf16(
		        app->backups.base, wcslen(app->backups.base), &b);
		wf_utf8_elide(shown, sizeof shown, wf_buf_cstr(&b), 40);
		wf_buf_free(&b);
		(void)wf_msgf(text, sizeof text, MSG_BACKUPS_NONE,
		        &WF_ARG_S(shown), 1);
		wf_app_set_msg(app, text);
		wf_app_backups_destroy(app);
		return;
	}
	app->backups.pick = 0;
	app->mode = &WF_MODE_BACKUPS;
}

static void row_text(const WfApp *app, size_t i, char *out, size_t cap)
{
	const WfEntry *e = backup_entry(app, i);
	char size[32] = "-", when[40] = "-", name[80];
	WfBuf b;

	wf_buf_init(&b);
	(void)wf_sanitize_utf16(
	        app->backups.names[i], wcslen(app->backups.names[i]), &b);
	wf_utf8_elide(name, sizeof name, wf_buf_cstr(&b), 40);
	wf_buf_free(&b);
	if (e) {
		WfFileTime t = e->mtime;

		wf_app_fmt_bytes(e->size, size, sizeof size);
		if (app->local_time) {
			t = app->local_time(t);
		}
		wf_info_format_time(t, when, sizeof when);
	}
	snprintf(out, cap, "%c %-10s %-20s %s",
	        i == app->backups.pick ? '>' : ' ', size, when, name);
}

static void backups_draw(const WfApp *app, WfFrame *f)
{
	char title[160], shown[96], row[200];
	WfBuf b;

	if (f->rows < 4) {
		return;
	}
	wf_buf_init(&b);
	(void)wf_sanitize_utf16(app->backups.base ? app->backups.base : L"",
	        app->backups.base ? wcslen(app->backups.base) : 0, &b);
	wf_utf8_elide(shown, sizeof shown, wf_buf_cstr(&b), 40);
	wf_buf_free(&b);
	(void)wf_msgf(
	        title, sizeof title, MSG_BACKUPS_TITLE, &WF_ARG_S(shown), 1);
	wf_frame_fill(f, 0, 0, f->cols, f->rows - 1, ' ', &WF_STYLE_BASE);
	wf_app_put_fit(f, 0, 0, title, &wf_app_style_bar, f->cols);
	for (size_t i = 0; i < app->backups.n && (int)i < f->rows - 3; i++) {
		row_text(app, i, row, sizeof row);
		wf_app_put_fit(f, 1, 2 + (int)i, row,
		        i == app->backups.pick ? &wf_theme_styles()->selection
		                               : &WF_STYLE_BASE,
		        f->cols - 2);
	}
	wf_frame_fill(f, 0, f->rows - 2, f->cols, 1, ' ', &wf_app_style_bar);
	wf_app_put_fit(f, 0, f->rows - 2, wf_msg(MSG_BACKUPS_HINT),
	        &wf_app_style_bar, f->cols);
}

/* Puts the list's cursor on the entry so that Delete's own machinery acts on
 * it: the same question, the same job, the same checks. */
static bool cursor_to(WfApp *app, const wchar_t *name)
{
	for (size_t i = 0; i < app->cur.nvis; i++) {
		if (!wcscmp(app->cur.dir->e[app->cur.vis[i]].name, name)) {
			wf_view_move(&app->cur, (long)i - (long)app->cur.cursor,
			        wf_app_body_height(app));
			return true;
		}
	}
	return false;
}

static WfAction backups_event(WfApp *app, const WfEvent *ev)
{
	size_t n = app->backups.n;

	if (WF_EV_RESIZE == ev->type) {
		return WF_ACT_REDRAW;
	}
	if (WF_EV_RELEASE == ev->type) {
		return WF_ACT_NONE;
	}
	if (WF_EV_KEY == ev->type) {
		switch (ev->key) {
		case WF_KEY_UP:
			app->backups.pick = (app->backups.pick + n - 1) % n;
			return WF_ACT_REDRAW;
		case WF_KEY_DOWN:
			app->backups.pick = (app->backups.pick + 1) % n;
			return WF_ACT_REDRAW;
		case WF_KEY_HOME: app->backups.pick = 0; return WF_ACT_REDRAW;
		case WF_KEY_END:
			app->backups.pick = n - 1;
			return WF_ACT_REDRAW;
		case WF_KEY_ENTER: {
			wchar_t *name = wf_path_dup(
			        app->backups.names[app->backups.pick]);

			app->mode = &WF_MODE_NORMAL;
			if (name) {
				wf_app_edit_begin_named(app, name);
				free(name);
			}
			return WF_ACT_REDRAW;
		}
		case WF_KEY_DELETE: {
			bool found = cursor_to(
			        app, app->backups.names[app->backups.pick]);

			app->mode = &WF_MODE_NORMAL;
			if (found) {
				wf_app_recycle_begin(app, ev);
			}
			return WF_ACT_REDRAW;
		}
		case WF_KEY_ESC:
			app->mode = &WF_MODE_NORMAL;
			return WF_ACT_REDRAW;
		default: break;
		}
	}
	if (WF_EV_CHAR == ev->type && ('q' == ev->ch || 'Q' == ev->ch)) {
		app->mode = &WF_MODE_NORMAL;
		return WF_ACT_REDRAW;
	}
	return WF_ACT_NONE;
}

const WfMode WF_MODE_BACKUPS = {"BACKUPS", backups_draw, backups_event};
