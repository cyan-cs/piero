#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../archive/wf_vpath.h"
#include "../fs/wf_loc.h"
#include "../fs/wf_sftpplace_win.h"
#include "../path/wf_path.h"
#include "../text/wf_text.h"
#include "wf_app_priv.h"

#define NAVIGATION_MEMORY_LIMIT 128u

typedef struct
{
	wchar_t *path,
	        *name; /* owned UI hints, never file-operation authority */
	size_t cursor, top;
} WfNavigationPosition;

struct WfNavigationMemory
{
	WfNavigationPosition
	        entries[NAVIGATION_MEMORY_LIMIT]; /* oldest first */
	size_t count;
};

void wf_app_navigation_memory_destroy(WfApp *app)
{
	WfNavigationMemory *memory = app->navigation_memory;

	if (!memory) {
		return;
	}
	for (size_t i = 0; i < memory->count; i++) {
		free(memory->entries[i].path);
		free(memory->entries[i].name);
	}
	free(memory);
	app->navigation_memory = NULL;
}

/* Touching an entry before saving the departing folder protects the target
 * from eviction when the bounded memory is full. No counters can wrap. */
static WfNavigationPosition *memory_find_and_touch(
        WfApp *app, const wchar_t *path)
{
	WfNavigationMemory *memory = app->navigation_memory;

	if (!memory) {
		return NULL;
	}
	for (size_t i = 0; i < memory->count; i++) {
		if (!wcscmp(memory->entries[i].path, path)) {
			WfNavigationPosition position = memory->entries[i];
			memmove(memory->entries + i, memory->entries + i + 1,
			        (memory->count - i - 1) *
			                sizeof *memory->entries);
			memory->entries[memory->count - 1] = position;
			return &memory->entries[memory->count - 1];
		}
	}
	return NULL;
}

static WfStatus memory_save(WfApp *app)
{
	WfNavigationMemory *memory;
	WfNavigationPosition *position;
	const WfEntry *entry = wf_view_current(&app->cur);
	wchar_t *path = NULL, *name;

	if (!app->cur.dir) {
		return WF_OK;
	}
	if (!app->navigation_memory) {
		app->navigation_memory =
		        calloc(1, sizeof *app->navigation_memory);
		if (!app->navigation_memory) {
			return WF_E_NOMEM;
		}
	}
	memory = app->navigation_memory;
	position = memory_find_and_touch(app, app->cwd);
	name = entry ? wf_path_dup(entry->name) : NULL;
	if (entry && !name) {
		return WF_E_NOMEM;
	}
	if (!position) {
		path = wf_path_dup(app->cwd);
		if (!path) {
			free(name);
			return WF_E_NOMEM;
		}
		if (NAVIGATION_MEMORY_LIMIT == memory->count) {
			free(memory->entries[0].path);
			free(memory->entries[0].name);
			memory->count--;
			memmove(memory->entries, memory->entries + 1,
			        memory->count * sizeof *memory->entries);
		}
		position = &memory->entries[memory->count++];
		position->path = path;
	} else {
		free(position->name);
	}
	position->name = name;
	position->cursor = app->cur.cursor;
	position->top = wf_view_top(&app->cur, wf_app_body_height(app));
	return WF_OK;
}

static void memory_restore(WfApp *app)
{
	WfView *view = &app->cur;
	WfNavigationPosition *position = memory_find_and_touch(app, app->cwd);
	/* a saved session (--continue) names the position of the folder it is
	 * putting back; it is read here, after the departing folder's position
	 * was remembered, so that it is not lost when that is this same folder */
	bool session = app->session_restore.active && app->cwd &&
	        !wcscmp(app->cwd, app->session_restore.dir);
	const wchar_t *name = app->want_select ? app->want_select
	        : session                      ? app->session_restore.entry
	        : position                     ? position->name
	                                       : NULL;
	size_t cursor = session ? app->session_restore.cursor
	        : position      ? position->cursor
	                        : 0;
	size_t top = session ? app->session_restore.top
	        : position   ? position->top
	                     : 0;
	size_t height = (size_t)wf_app_body_height(app), last_top;

	if (!view->nvis) {
		view->cursor = view->top = 0;
		return;
	}
	view->cursor = cursor < view->nvis ? cursor : view->nvis - 1;
	if (name) {
		for (size_t i = 0; i < view->nvis; i++) {
			if (!wcscmp(view->dir->e[view->vis[i]].name, name)) {
				view->cursor = i;
				break;
			}
		}
	}
	last_top = view->nvis > height ? view->nvis - height : 0;
	view->top = top < last_top ? top : last_top;
	view->top = wf_view_top(view, (int)height);
}

void wf_app_set_error(WfApp *app, WfStatus st)
{
	const char *what = wf_msg(WF_E_ACCESS == st ? MSG_NAV_ACCESS
	                : WF_E_NOENT == st          ? MSG_NAV_NOENT
	                : WF_E_NOMEM == st          ? MSG_NAV_NOMEM
	                : WF_E_FORMAT == st         ? MSG_NAV_AR_FORMAT
	                : WF_E_CORRUPT == st        ? MSG_NAV_AR_CORRUPT
	                : WF_E_ENCRYPTED == st      ? MSG_NAV_AR_ENCRYPTED
	                : WF_E_RANGE == st          ? MSG_NAV_AR_TOO_BIG
	                : WF_E_CHANGED == st        ? MSG_NAV_AR_CHANGED
	                                            : MSG_NAV_OTHER);

	(void)wf_msgf(app->msg, sizeof app->msg, MSG_NAV_ERROR,
	        (WfMsgArg[]){WF_ARG_S(wf_status_name(st)), WF_ARG_S(what)}, 2);
}

void wf_app_free_want(WfApp *app)
{
	free(app->want_path);
	free(app->want_select);
	free(app->want_history_path);
	app->want_path = NULL;
	app->want_select = NULL;
	app->want_history_path = NULL;
	app->want_history_index = SIZE_MAX;
	app->want_preview_refresh = false;
}

static void history_commit(WfApp *app)
{
	if (app->want_history_index < app->history_n) {
		app->history_index = app->want_history_index;
		return;
	}
	if (!app->want_history_path) {
		return;
	}
	while (app->history_n && app->history_n > app->history_index + 1) {
		WfHistoryEntry *entry = &app->history[--app->history_n];
		free(entry->path);
		free(entry->select);
	}
	if (WF_HISTORY_LIMIT == app->history_n) {
		free(app->history[0].path);
		free(app->history[0].select);
		memmove(app->history, app->history + 1,
		        (WF_HISTORY_LIMIT - 1) * sizeof app->history[0]);
		app->history_n--;
	}
	app->history[app->history_n] =
	        (WfHistoryEntry){app->want_history_path, NULL};
	app->want_history_path = NULL;
	app->history_index = app->history_n++;
}

/*
 * Asks for a listing of `path` (and its parent, for the left column). The
 * app keeps showing the old directory until a matching answer arrives, so
 * a failed or slow navigation never leaves a half-updated screen. A newer
 * request supersedes an older one; the old answer is dropped by generation.
 */
static WfStatus request(WfApp *app,
        const wchar_t *path,
        const wchar_t *select,
        size_t history_index,
        bool refresh_preview)
{
	wchar_t *p = wf_path_dup(path);
	wchar_t *sel = select ? wf_path_dup(select) : NULL;
	wchar_t *pp = NULL;
	wchar_t *history_path = NULL, *history_select = NULL;
	const WfEntry *current = wf_view_current(&app->cur);
	size_t plen;
	WfStatus st;

	if (!p || (select && !sel)) {
		goto nomem;
	}
	if (SIZE_MAX == history_index &&
	        (!app->history_n || wcscmp(path, app->cwd))) {
		history_path = wf_path_dup(path);
		if (!history_path) {
			goto nomem;
		}
	}
	if (current && app->history_n) {
		history_select = wf_path_dup(current->name);
		if (!history_select) {
			goto nomem;
		}
	}
	plen = wf_path_parent_len(p);
	if (plen && !(pp = wf_path_dup_n(p, plen))) {
		goto nomem;
	}

	wf_app_free_want(app);
	app->want_path = p;
	app->want_select = sel;
	app->want_history_path = history_path;
	app->want_history_index = history_index;
	app->want_preview_refresh = refresh_preview;
	if (app->history_n) {
		free(app->history[app->history_index].select);
		app->history[app->history_index].select = history_select;
	} else {
		free(history_select);
	}
	if (wf_vpath_is(p) && app->now_ms >= app->msg_keep_until) {
		wf_app_set_msg(app, wf_msg(MSG_AR_READING));
	}
	/* submit() may answer synchronously and consume want_*: touch nothing
	 * after */
	st = app->fs.submit(app->fs.ctx, ++app->nav_gen, p, pp);
	free(pp);
	if (WF_OK != st) {
		wf_app_free_want(app);
		return st;
	}
	return WF_OK;
nomem:
	free(p);
	free(sel);
	free(history_path);
	free(history_select);
	return WF_E_NOMEM;
}

void wf_app_on_listing(
        WfApp *app, uint32_t gen, WfStatus st, WfDir *dir, WfDir *parent)
{
	wchar_t *leaf;
	const wchar_t *l;
	size_t ln;
	WfStatus a, b;
	bool keep_preview;

	if (gen != app->nav_gen || !app->want_path) {
		wf_dir_free(dir);
		wf_dir_free(parent);
		return;
	}
	if (WF_OK != st || !dir) {
		bool sftp = wf_sftp_place_is(app->want_path);
		/* a saved session whose folder failed goes to the folder above it, after
		 * this request's own state is gone (the next request replaces it) */
		wchar_t *failed = app->session_restore.active && app->want_path
		        ? wf_path_dup(app->want_path)
		        : NULL;

		wf_dir_free(dir);
		wf_dir_free(parent);
		wf_app_set_error(app, WF_OK != st ? st : WF_E_OS);
		if (sftp) { /* ssh's own reason is more use than a code */
			char why[160], text[200];

			wf_sftp_place_note(why, sizeof why);
			if (why[0]) {
				(void)wf_msgf(text, sizeof text,
				        MSG_SFTP_FAILED, &WF_ARG_S(why), 1);
				wf_app_set_msg(app, text);
			}
		}
		wf_app_free_want(app);
		wf_app_go_listing_failed(app, gen, WF_OK != st ? st : WF_E_OS);
		wf_app_session_listing_failed(app, failed);
		free(failed);
		return;
	}
	/* Smart Go looked for a typed file in its folder: no such file, no move */
	if (wf_app_go_listing_vetoed(app, gen, dir)) {
		wf_dir_free(dir);
		wf_dir_free(parent);
		wf_app_free_want(app);
		wf_app_go_listing_failed(app, gen, WF_E_NOENT);
		return;
	}

	(void)memory_find_and_touch(app, app->want_path);
	if (WF_OK != memory_save(app)) {
		wf_dir_free(dir);
		wf_dir_free(parent);
		wf_app_set_error(app, WF_E_NOMEM);
		wf_app_free_want(app);
		return;
	}
	bool same_root = app->cur.dir &&
	        (app->cur.dir->identity.kind == WF_ID_STRONG) &&
	        (dir->identity.kind == WF_ID_STRONG) &&
	        app->cur.dir->identity.volume == dir->identity.volume &&
	        !memcmp(app->cur.dir->identity.id, dir->identity.id,
	                sizeof dir->identity.id);
	bool same_path = !wcscmp(app->want_path, app->cwd);
	keep_preview = same_path && same_root &&
	        (app->preview_id.kind == WF_ID_STRONG);
	if (same_path && app->cur.dir &&
	        (app->cur.dir->identity.kind == WF_ID_STRONG) &&
	        (dir->identity.kind == WF_ID_STRONG) && !same_root) {
		app->cwd_version++; /* retarget a watch pinned to the replaced folder */
	}
	bool refresh_preview = app->want_preview_refresh;
	if (!keep_preview) {
		wf_app_dircache_invalidate(app);
	}
	history_commit(app);
	if (0 != wcscmp(app->want_path, app->cwd)) {
		free(app->prev_dir);
		app->prev_dir = app->cwd; /* "-" in Smart Go goes back to it */
		app->cwd = app->want_path;
		app->want_path = NULL;
		app->cwd_version++;
		app->cur.filter[0] =
		        L'\0'; /* a filter belongs to one directory */
	}
	wf_app_tabs_arrived(app);
	wf_vpath_leaf(app->cwd, &l, &ln);
	leaf = wf_path_dup_n(l, ln);

	a = same_path ? wf_view_replace_keep(&app->cur, dir, app->want_select)
	              : wf_view_set_dir(&app->cur, dir, app->want_select);
	if (WF_OK == a) {
		memory_restore(app);
	}
	b = wf_view_set_dir(&app->parent, parent, leaf);
	if (app->cur.dir) {
		wf_sel_prune(&app->sel, app->cwd, app->cur.dir);
	}

	if (app->now_ms >= app->msg_keep_until) {
		app->msg[0] = '\0';
	}
	if (WF_OK != a || WF_OK != b) {
		wf_app_set_error(app, WF_OK != a ? a : b);
	}
	free(leaf);
	wf_app_free_want(app);
	if (!keep_preview) {
		wf_app_preview_clear(app);
	}
	wf_app_preview_sync(app, refresh_preview);
	wf_app_go_listing_done(app, gen);
	wf_app_session_listing_done(app);
	wf_app_git_note_listing(app, same_path);
}

void wf_app_reload(WfApp *app)
{
	wf_app_refresh(app, true);
}

void wf_app_refresh(WfApp *app, bool refresh_preview)
{
	/* A notification for the departing directory must not replace a user's
	 * pending navigation or reopen an unapproved network destination. */
	if (app->want_path || !app->cur.dir) {
		return;
	}
	WfStatus st = request(app, app->cwd, NULL, SIZE_MAX, refresh_preview);

	if (WF_OK != st) {
		wf_app_set_error(app, st);
	}
}

size_t wf_app_unc_root_length(const wchar_t *path)
{
	size_t i, start;
	if (!wcsncmp(path, L"\\\\?\\", 4)) {
		if (!((L'U' == path[4] || L'u' == path[4]) &&
		            (L'N' == path[5] || L'n' == path[5]) &&
		            (L'C' == path[6] || L'c' == path[6]) &&
		            L'\\' == path[7])) {
			return 0;
		}
		i = 8;
	} else {
		if (wcsncmp(path, L"\\\\", 2)) {
			return 0;
		}
		i = 2;
	}
	start = i;
	while (path[i] && L'\\' != path[i]) {
		i++;
	}
	if (i == start || !path[i]) {
		return 0;
	}
	start = ++i;
	while (path[i] && L'\\' != path[i]) {
		i++;
	}
	return i == start ? 0 : i;
}

bool wf_app_extended_drive(const wchar_t *path)
{
	return path && wcslen(path) >= 7 && !wcsncmp(path, L"\\\\?\\", 4) &&
	        ((path[4] >= L'A' && path[4] <= L'Z') ||
	                (path[4] >= L'a' && path[4] <= L'z')) &&
	        L':' == path[5] && L'\\' == path[6];
}

void wf_app_free_pending_navigation(WfApp *app)
{
	free(app->pending_path);
	free(app->pending_select);
	free(app->pending_unc_root);
	app->pending_path = app->pending_select = app->pending_unc_root = NULL;
}

void wf_app_unc_yes(WfApp *app, void *ctx)
{
	WfStatus st;
	(void)ctx;
	if (!app->pending_path || !app->pending_unc_root) {
		wf_app_free_pending_navigation(app);
		return;
	}
	free(app->unc_approved_root);
	app->unc_approved_root = app->pending_unc_root;
	app->pending_unc_root = NULL;
	st = request(app, app->pending_path, app->pending_select,
	        app->pending_history_index, false);
	wf_app_free_pending_navigation(app);
	if (WF_OK != st) {
		wf_app_set_error(app, st);
	}
}

WfStatus wf_app_navigate(WfApp *app,
        const wchar_t *path,
        const wchar_t *select,
        size_t history_index)
{
	size_t root;
	WfBuf display;
	const char *lines[5];
	WfStatus st;
	if (!app || !path) {
		return WF_E_INVAL;
	}
	root = wf_app_unc_root_length(path);
	if (!wcsncmp(path, L"\\\\.\\", 4) ||
	        (!wcsncmp(path, L"\\\\", 2) && !wf_app_extended_drive(path) &&
	                !root)) {
		return WF_E_INVAL;
	}
	/* "This PC" looks like a UNC root but is no network folder */
	if (!root || wf_pcroot_is(path) || !app->confirm_unc_navigation ||
	        (app->unc_approved_root &&
	                wcslen(app->unc_approved_root) == root &&
	                !wcsncmp(path, app->unc_approved_root, root))) {
		return request(app, path, select, history_index, false);
	}
	wf_app_free_pending_navigation(app);
	app->pending_path = wf_path_dup(path);
	app->pending_select = select ? wf_path_dup(select) : NULL;
	app->pending_unc_root = wf_path_dup_n(path, root);
	app->pending_history_index = history_index;
	if (!app->pending_path || !app->pending_unc_root ||
	        (select && !app->pending_select)) {
		wf_app_free_pending_navigation(app);
		return WF_E_NOMEM;
	}
	wf_buf_init(&display);
	st = wf_sanitize_utf16(path, wcslen(path), &display);
	if (WF_OK == st) {
		lines[0] = wf_msg(MSG_NAV_NETWORK_TITLE);
		lines[1] = wf_msg(MSG_NAV_NETWORK_1);
		lines[2] = wf_msg(MSG_NAV_NETWORK_2);
		lines[3] = wf_msg(MSG_XFER_DESTINATION);
		lines[4] = wf_buf_cstr(&display);
		st = wf_app_confirm_begin(
		        app, lines, 5, wf_app_unc_yes, NULL, NULL);
	}
	wf_buf_free(&display);
	if (WF_OK != st) {
		wf_app_free_pending_navigation(app);
		return st;
	}
	/* Supersede an older local request before displaying the network
	 * prompt. */
	wf_app_free_want(app);
	++app->nav_gen;
	return WF_OK;
}

void wf_app_go(WfApp *app, const wchar_t *path, const wchar_t *select)
{
	WfStatus st = wf_app_navigate(app, path, select, SIZE_MAX);

	if (WF_OK != st) {
		wf_app_set_error(app, st);
	}
}

void wf_app_enter_selected(WfApp *app)
{
	const WfEntry *e = wf_view_current(&app->cur);
	wchar_t *child, *root = NULL;
	WfStatus st;

	if (!e || !(e->flags & (WF_ENT_DIR | WF_ENT_ARCHIVE))) {
		return;
	}
	/* no archives inside archives */
	if ((e->flags & WF_ENT_ARCHIVE) && wf_vpath_is(app->cwd)) {
		return;
	}
	st = wf_path_join(app->cwd, e->name, &child);
	if (WF_OK == st && (e->flags & WF_ENT_ARCHIVE) &&
	        !(e->flags & WF_ENT_DIR)) {
		/* an archive file is entered as a folder: its top is "file|" */
		st = wf_vpath_root(child, &root);
		free(child);
		child = root;
	}
	if (WF_OK != st) {
		wf_app_set_error(app, st);
		return;
	}
	wf_app_go(app, child, NULL);
	free(child);
}

void wf_app_history_go(WfApp *app, bool forward)
{
	size_t target;
	WfStatus st;
	if (!app->history_n || (!forward && !app->history_index) ||
	        (forward && app->history_index + 1 >= app->history_n)) {
		return;
	}
	target = forward ? app->history_index + 1 : app->history_index - 1;
	st = wf_app_navigate(app, app->history[target].path, NULL, target);
	if (WF_OK != st) {
		wf_app_set_error(app, st);
	}
}

void wf_app_go_parent(WfApp *app)
{
	size_t plen = wf_path_parent_len(app->cwd);
	const wchar_t *leaf;
	size_t leaf_len;
	wchar_t *parent, *name;

	if (0 == plen) {
		/* above a drive or a distribution is "This PC", with that drive
		 * selected; above that there is nothing */
		name = wf_pcroot_leaf(app->cwd);
		if (name) {
			wf_app_go(app, WF_PCROOT, name);
			free(name);
		}
		return;
	}
	wf_vpath_leaf(app->cwd, &leaf, &leaf_len);
	parent = wf_path_dup_n(app->cwd, plen);
	name = wf_path_dup_n(leaf, leaf_len);
	if (parent && name) {
		wf_app_go(app, parent, name);
	}
	free(parent);
	free(name);
}

/* Folders that can be looked at but not changed: inside an archive. (A WSL
 * distribution can be changed, by its own operations: see wf_app_wsl.c.) */
bool wf_app_archive_guard(WfApp *app)
{
	if (wf_vpath_is(app->cwd)) {
		wf_app_set_msg(app, wf_msg(MSG_AR_READONLY));
		return true;
	}
	if (wf_sftp_place_is(app->cwd)) {
		wf_app_set_msg(app, wf_msg(MSG_SFTP_READONLY));
		return true;
	}
	if (wf_pcroot_is(app->cwd)) {
		wf_app_set_msg(app, wf_msg(MSG_PC_READONLY));
		return true;
	}
	return false;
}
