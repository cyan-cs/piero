/*
 * Smart Go: `g` (empty) and Ctrl+L (the shown folder, selected).
 *
 * The line the user types is understood by wf_smart_resolve() (relative
 * paths, ~, @aliases, %VARIABLES%, UNC ...); what can complete it is read by a
 * worker in WF_CAND_BATCH-sized batches. The visible rows depend on the
 * terminal size and the panel layout.
 * Typing and choosing are separate things: Enter goes to the typed path unless
 * the user has moved onto a candidate; Tab fills the line in without going
 * anywhere. Nothing changes in the file manager until Enter, and then only
 * through the ordinary navigation (wf_app_navigate), which keeps the old folder
 * on screen until the new listing has arrived. If that fails the line is still
 * here to be corrected.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../fs/wf_wsl_win.h"
#include "../path/wf_path.h"
#include "../text/wf_text.h"
#include "wf_app_priv.h"
#include "../base/wf_macro.h"
#include "../base/wf_mem.h"

static size_t go_rows(const WfApp *app)
{
	if (app->rows < 10) {
		return app->rows > 5 ? (size_t)app->rows - 5 : 0;
	}
	if (app->rows >=
	        14) { /* the framed, floating panel: ten rows at most */
		return (size_t)app->rows - 8 < 10 ? (size_t)app->rows - 8 : 10;
	}
	return app->rows - 6 < WF_GO_ROWS ? (size_t)app->rows - 6 : WF_GO_ROWS;
}

static bool scalar_ok(uint32_t cp)
{
	return cp >= 32 && 127 != cp && cp <= 0x10FFFF &&
	        !(cp >= 0xD800 && cp <= 0xDFFF);
}

static WfStatus text_reserve(WfApp *app, size_t need)
{
	wchar_t *grown;
	size_t cap = app->go.cap ? app->go.cap : 64;

	if (need + 1 <= app->go.cap) {
		return WF_OK;
	}
	while (cap < need + 1) {
		if (cap > SIZE_MAX / 2 / sizeof *grown) {
			return WF_E_RANGE;
		}
		cap *= 2;
	}
	grown = realloc(app->go.text, cap * sizeof *grown);
	if (!grown) {
		return WF_E_NOMEM;
	}
	app->go.text = grown;
	app->go.cap = cap;
	return WF_OK;
}

/* Replaces [start, end) by text[0, n); the caret goes after it. */
static WfStatus text_replace(
        WfApp *app, size_t start, size_t end, const wchar_t *text, size_t n)
{
	size_t tail;
	WfStatus st;

	if (start > end || end > app->go.len) {
		return WF_E_RANGE;
	}
	tail = app->go.len - end;
	if (n > SIZE_MAX / sizeof(wchar_t) - start - tail - 2) {
		return WF_E_RANGE;
	}
	st = text_reserve(app, start + n + tail);
	if (WF_OK != st) {
		return st;
	}
	memmove(app->go.text + start + n, app->go.text + end,
	        (tail + 1) * sizeof *app->go.text);
	if (n) {
		memcpy(app->go.text + start, text, n * sizeof *text);
	}
	app->go.len = start + n + tail;
	app->go.cursor = start + n;
	return WF_OK;
}

static void take_selection(WfApp *app)
{
	if (!app->go.all_selected) {
		return;
	}
	app->go.all_selected = false;
	app->go.len = app->go.cursor = 0;
	if (app->go.text) {
		app->go.text[0] = L'\0';
	}
}

static void set_note(WfApp *app, bool is_error, const char *text)
{
	snprintf(app->go.note, sizeof app->go.note, "%s", text);
	app->go.note_is_error = is_error;
}

static void cands_free(WfApp *app)
{
	size_t i;

	for (i = 0; i < app->go.cand_n; i++) {
		free(app->go.cand[i].name);
		free(app->go.cand[i].path);
	}
	free(app->go.cand);
	app->go.cand = NULL;
	app->go.cand_n = app->go.cand_cap = 0;
	app->go.sel = app->go.top = 0;
	app->go.has_sel = false;
}

static WfStatus cands_add(
        WfApp *app, const wchar_t *name, const wchar_t *path, uint32_t flags)
{
	WfGoCand c;

	if (app->go.cand_n == app->go.cand_cap) {
		size_t cap = app->go.cand_cap ? app->go.cand_cap * 2 : 16;
		WfGoCand *grown =
		        wf_realloc_array(app->go.cand, cap, sizeof *grown);

		if (!grown) {
			return WF_E_NOMEM;
		}
		app->go.cand = grown;
		app->go.cand_cap = cap;
	}
	c.name = wf_path_dup(name);
	c.path = path ? wf_path_dup(path) : NULL;
	c.flags = flags;
	if (!c.name || (path && !c.path)) {
		free(c.name);
		free(c.path);
		return WF_E_NOMEM;
	}
	app->go.cand[app->go.cand_n++] = c;
	return WF_OK;
}

static void query_clear(WfApp *app)
{
	free(app->go.q_dir);
	free(app->go.q_prefix);
	app->go.q_dir = app->go.q_prefix = NULL;
}

static void search_stop(WfApp *app)
{
	if (app->cand_port.cancel) {
		app->cand_port.cancel(app->cand_port.ctx);
	}
	query_clear(app);
	app->go.read = WF_GR_IDLE;
	app->go.has_more = false;
	app->go.advance_after_more = false;
	app->go.gen++; /* anything still in flight is now stale */
}

static void go_free(WfApp *app)
{
	if (app->go.open) {
		search_stop(app);
	}
	uint32_t gen;

	cands_free(app);
	free(app->go.text);
	free(app->go.leaf);
	/* the numbers of the requests go on across a close and an open: the worker
	 * tells a stale answer by its number, and a count that started again would
	 * give an answer for the old folder to the new one */
	gen = app->go.gen;
	memset(&app->go, 0, sizeof app->go);
	app->go.gen = gen;
}

void wf_app_path_state_destroy(WfApp *app)
{
	go_free(app);
}

void wf_app_set_candidates(WfApp *app, WfCandPort port)
{
	app->cand_port = port;
}

void wf_app_set_clipboard(WfApp *app, WfClipPort port)
{
	app->clip = port;
}

void wf_app_set_smartgo_env(WfApp *app, WfSmartEnv env)
{
	app->go_env = env;
}

static bool starts_with_ci(const wchar_t *name, const wchar_t *prefix)
{
	size_t n = wcslen(prefix);

	return wcslen(name) >= n && !_wcsnicmp(name, prefix, n);
}

static void load_aliases(WfApp *app, const wchar_t *prefix)
{
	size_t i;

	cands_free(app);
	app->go.source = WF_GO_ALIAS;
	for (i = 0; i < WF_SMART_ALIASES; i++) {
		const wchar_t *name = wf_smart_alias_name(i);

		if (starts_with_ci(name, prefix) &&
		        WF_OK != cands_add(app, name, NULL, WF_ENT_DIR)) {
			return;
		}
	}
}

static void go_update(WfApp *app)
{
	WfSmartResult r;
	WfStatus st;
	bool hidden, dirs_only = false;
	const wchar_t *search_dir = NULL, *search_prefix = NULL;

	set_note(app, false, "");
	app->go.has_sel = false;
	st = wf_smart_resolve(&app->go_env, app->cwd, app->prev_dir,
	        app->go.text ? app->go.text : L"", app->go.len, &r);
	if (WF_OK != st) {
		search_stop(app);
		cands_free(app);
		set_note(app, true, wf_msg(MSG_ERROR_OUT_OF_MEMORY));
		return;
	}
	switch (r.error) {
	case WF_SG_EMPTY:
		/* nothing typed: the folders directly under the shown one */
		dirs_only = true;
		search_dir = app->cwd;
		search_prefix = L"";
		goto search;
	case WF_SG_ALIAS_PARTIAL:
		search_stop(app);
		load_aliases(app, r.prefix);
		break;
	case WF_SG_OK:
		search_dir = r.list_dir;
		search_prefix = r.prefix;
	search:
		hidden = app->cur.show_hidden || L'.' == search_prefix[0];
		if (WF_GO_FS == app->go.source && app->go.q_dir &&
		        !wcscmp(app->go.q_dir, search_dir) &&
		        !wcscmp(app->go.q_prefix, search_prefix) &&
		        app->go.q_hidden == hidden &&
		        app->go.dirs_only == dirs_only) {
			break; /* the same search is already in force */
		}
		search_stop(app);
		cands_free(app);
		app->go.source = WF_GO_FS;
		if (!app->cand_port.start) {
			break;
		}
		app->go.q_dir = wf_path_dup(search_dir);
		app->go.q_prefix = wf_path_dup(search_prefix);
		app->go.q_hidden = hidden;
		app->go.dirs_only = dirs_only;
		if (!app->go.q_dir || !app->go.q_prefix) {
			query_clear(app);
			set_note(app, true, wf_msg(MSG_ERROR_OUT_OF_MEMORY));
			break;
		}
		app->go.list_status = WF_OK;
		app->go.read = WF_GR_FIRST;
		app->go.gen++;
		if (app->cand_port.start(app->cand_port.ctx, app->go.gen,
		            app->go.q_dir, app->go.q_prefix, hidden,
		            dirs_only) != WF_OK) {
			app->go.read = WF_GR_IDLE;
			query_clear(app);
			set_note(app, true, wf_msg(MSG_SMART_GO_CANNOT_LIST));
		}
		break;
	default:
		/* not (yet) a path: nothing to offer, and no scolding while typing */
		search_stop(app);
		cands_free(app);
		app->go.source = WF_GO_NONE;
		break;
	}
	wf_smart_result_free(&r);
}

void wf_app_on_candidates(WfApp *app, const WfCandBatch *b)
{
	size_t i, first_new;

	if (!app->go.open || !b || b->gen != app->go.gen ||
	        WF_GO_FS != app->go.source) {
		return;
	}
	if (b->first) {
		cands_free(app);
	}
	first_new = app->go.cand_n;
	app->go.read = WF_GR_IDLE;
	app->go.list_status = b->st;
	app->go.has_more = b->more && WF_OK == b->st;
	for (i = 0; i < b->n && app->go.cand_n < WF_GO_CAND_MAX; i++) {
		if (WF_OK !=
		        cands_add(app, b->items[i].name, NULL,
		                b->items[i].flags)) {
			set_note(app, true, wf_msg(MSG_ERROR_OUT_OF_MEMORY));
			break;
		}
	}
	if (app->go.cand_n >= WF_GO_CAND_MAX) {
		app->go.has_more = false;
	}
	if (WF_OK != b->st && 0 == app->go.cand_n) {
		set_note(app, true,
		        wf_msg(WF_E_ACCESS == b->st ? MSG_ERROR_ACCESS_DENIED
		                        : WF_E_NOENT == b->st
		                        ? MSG_ERROR_PATH_NOT_FOUND
		                        : MSG_SMART_GO_CANNOT_LIST));
	}
	if (app->go.advance_after_more) {
		app->go.advance_after_more = false;
		if (app->go.cand_n > first_new) {
			app->go.sel = first_new;
			app->go.has_sel = true;
		}
	}
	if (app->go.has_sel) {
		if (app->go.sel < app->go.top) {
			app->go.top = app->go.sel;
		} else if (app->go.sel >= app->go.top + go_rows(app)) {
			app->go.top = app->go.sel + 1 - go_rows(app);
		}
	}
}

static void go_close(WfApp *app)
{
	go_free(app);
	app->mode = &WF_MODE_NORMAL;
}

static void go_open(WfApp *app, const wchar_t *initial, bool select_all)
{
	size_t n = wcslen(initial);

	go_free(app);
	app->go.open = true;
	if (WF_OK != text_reserve(app, n)) {
		go_free(app);
		wf_app_set_error(app, WF_E_NOMEM);
		return;
	}
	memcpy(app->go.text, initial, (n + 1) * sizeof *initial);
	app->go.len = app->go.cursor = n;
	app->go.all_selected = select_all && n > 0;
	app->mode = &WF_MODE_GO;
	app->msg[0] = '\0';
	go_update(app);
}

void wf_app_path_prompt_begin(WfApp *app)
{
	go_open(app, L"", false);
}

void wf_app_path_bar_begin(WfApp *app)
{
	size_t n = wcslen(app->cwd);
	bool separator = n && L'\\' != app->cwd[n - 1];
	wchar_t *text;

	if (n > SIZE_MAX / sizeof *text - 2) {
		wf_app_set_error(app, WF_E_RANGE);
		return;
	}
	text = malloc((n + 2) * sizeof *text);
	if (!text) {
		wf_app_set_error(app, WF_E_NOMEM);
		return;
	}
	memcpy(text, app->cwd, n * sizeof *text);
	if (separator) {
		text[n++] = L'\\';
	}
	text[n] = L'\0';
	go_open(app, text, true);
	free(text);
}

/* Goes to `path`. `file_leaf` is a file to pick there (the path was a file's
 * folder); `maybe_leaf` is the last element of a typed path that may turn out
 * to be a file: if `path` is not a folder, its folder is tried with it. The
 * listing may arrive before navigate returns, so the wait is armed first. */
static void commit(WfApp *app,
        const wchar_t *path,
        const wchar_t *file_leaf,
        const wchar_t *maybe_leaf)
{
	const wchar_t *leaf = file_leaf ? file_leaf : maybe_leaf;
	WfStatus st;

	free(app->go.leaf);
	app->go.leaf = leaf ? wf_path_dup(leaf) : NULL;
	app->go.fallback_tried = NULL != file_leaf;
	app->go.committing = true;
	app->go.commit_gen = app->nav_gen + 1;
	st = wf_app_navigate(app, path, file_leaf, SIZE_MAX);
	if (WF_OK != st) {
		app->go.committing = false;
		set_note(app, true, wf_msg(MSG_SMART_GO_INVALID_PATH));
		return;
	}
	if (&WF_MODE_GO != app->mode) { /* a network confirmation took over */
		go_free(app);
		return;
	}
	if (app->go.committing) {
		search_stop(app);
		set_note(app, false, wf_msg(MSG_SMART_GO_GOING));
	}
}

/* The text of the chosen candidate as a path to visit. */
static wchar_t *candidate_path(const WfApp *app, const WfGoCand *c)
{
	wchar_t *path = NULL;

	if (c->path) {
		return wf_path_dup(c->path);
	}
	if (WF_GO_FS == app->go.source && app->go.q_dir &&
	        WF_OK == wf_path_join(app->go.q_dir, c->name, &path)) {
		return path;
	}
	return NULL;
}

/* The engine names its errors; the words are chosen here. */
static WfMsg smart_error_msg(WfSmartError e)
{
	switch (e) {
	case WF_SG_DRIVE_RELATIVE: return MSG_SMART_GO_DRIVE_RELATIVE;
	case WF_SG_ENV_UNDEFINED:  return MSG_SMART_GO_ENV_UNDEFINED;
	case WF_SG_NO_HOME:        return MSG_SMART_GO_NO_HOME;
	case WF_SG_NO_PREVIOUS:    return MSG_SMART_GO_NO_PREVIOUS;
	case WF_SG_UNKNOWN_ALIAS:  return MSG_SMART_GO_UNKNOWN_ALIAS;
	case WF_SG_TOO_LONG:       return MSG_SMART_GO_TOO_LONG;
	case WF_SG_NOMEM:          return MSG_ERROR_OUT_OF_MEMORY;
	case WF_SG_WSL_NAME:       return MSG_SMART_GO_WSL_NAME;
	case WF_SG_SFTP_NAME:      return MSG_SMART_GO_SFTP_NAME;
	default:                   return MSG_SMART_GO_INVALID_PATH;
	}
}

static WfAction go_enter(WfApp *app)
{
	if (app->go.committing) {
		return WF_ACT_NONE;
	}
	if (app->go.has_sel && app->go.sel < app->go.cand_n) {
		const WfGoCand *c = &app->go.cand[app->go.sel];

		if (WF_GO_ALIAS == app->go.source) {
			/* an alias goes through the same resolution as typed */
			WfSmartResult r;
			wchar_t text[64];

			swprintf(text, 64, L"@%ls", c->name);
			if (wf_smart_resolve(&app->go_env, app->cwd,
			            app->prev_dir, text, wcslen(text),
			            &r) == WF_OK &&
			        WF_SG_OK == r.error) {
				commit(app, r.target, NULL, NULL);
			} else {
				set_note(app, true,
				        wf_msg(MSG_SMART_GO_FOLDER_UNAVAILABLE));
			}
			wf_smart_result_free(&r);
			return WF_ACT_REDRAW;
		}
		{
			wchar_t *path = candidate_path(app, c);

			if (!path) {
				set_note(app, true,
				        wf_msg(MSG_ERROR_OUT_OF_MEMORY));
				return WF_ACT_REDRAW;
			}
			if (c->flags & WF_ENT_DIR) {
				commit(app, path, NULL, NULL);
			} else { /* a file: its folder, with the file picked */
				size_t plen = wf_path_parent_len(path);
				wchar_t *parent =
				        plen ? wf_path_dup_n(path, plen) : NULL;

				if (parent) {
					commit(app, parent, c->name, NULL);
				} else {
					set_note(app, true,
					        wf_msg(MSG_SMART_GO_INVALID_PATH));
				}
				free(parent);
			}
			free(path);
		}
		return WF_ACT_REDRAW;
	}
	{
		WfSmartResult r;
		WfStatus st = wf_smart_resolve(&app->go_env, app->cwd,
		        app->prev_dir, app->go.text ? app->go.text : L"",
		        app->go.len, &r);

		if (WF_OK != st) {
			set_note(app, true, wf_msg(MSG_ERROR_OUT_OF_MEMORY));
		} else if (WF_SG_EMPTY == r.error) {
			set_note(app, true, wf_msg(MSG_SMART_GO_TYPE_A_PATH));
		} else if (WF_SG_OK != r.error) {
			char text[128];
			WfBuf name;

			if (WF_SG_WSL_NAME ==
			        r.error) { /* the registered ones */
				wf_wsl_names_line(r.name, WF_ARRAY_LEN(r.name));
			}
			wf_buf_init(&name);
			wf_sanitize_utf16(r.name, wcslen(r.name), &name);
			(void)wf_msgf(text, sizeof text,
			        smart_error_msg(r.error),
			        &WF_ARG_S(wf_buf_cstr(&name)), 1);
			wf_buf_free(&name);
			set_note(app, true, text);
		} else {
			/* a path ending in a separator can only be a folder */
			bool folder = app->go.len &&
			        (L'\\' == app->go.text[app->go.len - 1] ||
			                L'/' == app->go.text[app->go.len - 1]);
			size_t plen = wf_path_parent_len(r.target);
			const wchar_t *leaf;
			size_t leaf_n;

			wf_path_leaf(r.target, &leaf, &leaf_n);
			if (!folder && plen && leaf_n) {
				/* may be a file: tried as a folder first, then as an entry
				 * of its folder */
				wchar_t *l = wf_path_dup_n(leaf, leaf_n);

				commit(app, r.target, NULL, l);
				free(l);
			} else {
				commit(app, r.target, NULL, NULL);
			}
		}
		wf_smart_result_free(&r);
	}
	return WF_ACT_REDRAW;
}

static WfAction go_tab(WfApp *app, bool backwards)
{
	const WfGoCand *c;
	size_t idx;

	if (app->go.committing) {
		return WF_ACT_NONE;
	}
	if (!app->go.cand_n) {
		set_note(app, false,
		        wf_msg(WF_GR_FIRST == app->go.read
		                        ? MSG_SMART_GO_SEARCHING
		                        : MSG_SMART_GO_NO_MATCHES));
		return WF_ACT_REDRAW;
	}
	if (app->go.has_sel) {
		idx = backwards
		        ? (app->go.sel ? app->go.sel - 1 : app->go.cand_n - 1)
		        : app->go.sel;
	} else {
		idx = backwards ? app->go.cand_n - 1 : 0;
	}
	c = &app->go.cand[idx];
	if (!app->go.len) {
		/* nothing typed: the folder's name becomes the first element */
		size_t n = wcslen(c->name);
		wchar_t *name = wf_malloc_array((n + 2), sizeof *name);

		if (!name) {
			return WF_ACT_REDRAW;
		}
		memcpy(name, c->name, n * sizeof *name);
		name[n++] = L'\\';
		name[n] = L'\0';
		if (WF_OK == text_replace(app, 0, 0, name, n)) {
			app->go.all_selected = false;
			go_update(app);
		}
		free(name);
		return WF_ACT_REDRAW;
	}
	{
		WfSmartResult r;
		wchar_t *out;
		size_t out_n, out_cursor, sep_at;
		wchar_t sep = L'\\';

		for (sep_at = app->go.len; sep_at-- > 0;) {
			if (L'/' == app->go.text[sep_at] ||
			        L'\\' == app->go.text[sep_at]) {
				sep = app->go.text[sep_at];
				break;
			}
		}
		if (wf_smart_resolve(&app->go_env, app->cwd, app->prev_dir,
		            app->go.text, app->go.len, &r) != WF_OK) {
			return WF_ACT_REDRAW;
		}
		if (WF_SG_OK != r.error && WF_SG_ALIAS_PARTIAL != r.error) {
			wf_smart_result_free(&r);
			return WF_ACT_REDRAW;
		}
		if (WF_SG_ALIAS_PARTIAL == r.error) {
			r.insert_separator = false;
		}
		if (wf_smart_complete(app->go.text, app->go.len,
		            app->go.cursor < r.prefix_start ? app->go.len
		                                            : app->go.cursor,
		            &r, c->name, 0 != (c->flags & WF_ENT_DIR), sep,
		            &out, &out_n, &out_cursor) == WF_OK) {
			free(app->go.text);
			app->go.text = out;
			app->go.len = out_n;
			app->go.cap = out_n + 1;
			app->go.cursor = out_cursor;
			app->go.all_selected = false;
		}
		wf_smart_result_free(&r);
	}
	go_update(app);
	return WF_ACT_REDRAW;
}

static WfAction go_edit_changed(WfApp *app)
{
	app->go.all_selected = false;
	go_update(app);
	return WF_ACT_REDRAW;
}

static void keep_selection_visible(WfApp *app)
{
	if (app->go.sel < app->go.top) {
		app->go.top = app->go.sel;
	} else if (app->go.sel >= app->go.top + go_rows(app)) {
		app->go.top = app->go.sel + 1 - go_rows(app);
	}
}

static WfAction go_down(WfApp *app)
{
	if (!app->go.cand_n) {
		return WF_ACT_REDRAW;
	}
	if (!app->go.has_sel) {
		app->go.has_sel = true;
		app->go.sel = 0;
	} else if (app->go.sel + 1 < app->go.cand_n) {
		app->go.sel++;
	} else if (app->go.has_more && WF_GR_MORE != app->go.read &&
	        app->cand_port.more) {
		/* Only one additional batch may be outstanding. */
		app->go.read = WF_GR_MORE;
		app->go.advance_after_more = true;
		if (WF_OK !=
		        app->cand_port.more(app->cand_port.ctx, app->go.gen)) {
			app->go.read = WF_GR_IDLE;
			app->go.advance_after_more = false;
		}
	}
	keep_selection_visible(app);
	return WF_ACT_REDRAW;
}

static WfAction go_up(WfApp *app)
{
	if (!app->go.has_sel) {
		return WF_ACT_REDRAW;
	}
	if (app->go.sel) {
		app->go.sel--;
	} else {
		app->go.has_sel = false; /* back to the typed line */
	}
	keep_selection_visible(app);
	return WF_ACT_REDRAW;
}

static WfAction go_paste(WfApp *app)
{
	char *utf8 = NULL;
	size_t n = 0, units;
	wchar_t *wide, *clean = NULL;
	size_t clean_n = 0;
	WfStatus st;

	if (!app->clip.get) {
		set_note(app, true, wf_msg(MSG_SMART_GO_PASTE_UNAVAILABLE));
		return WF_ACT_REDRAW;
	}
	if (WF_OK != app->clip.get(app->clip.ctx, &utf8, &n) || !utf8) {
		set_note(app, true, wf_msg(MSG_SMART_GO_NOTHING_TO_PASTE));
		return WF_ACT_REDRAW;
	}
	wide = wf_malloc_array((n + 1), sizeof *wide);
	if (!wide) {
		free(utf8);
		set_note(app, true, wf_msg(MSG_ERROR_OUT_OF_MEMORY));
		return WF_ACT_REDRAW;
	}
	st = wf_utf8_to_utf16(utf8, n, wide, n + 1, &units);
	free(utf8);
	if (WF_OK != st) {
		free(wide);
		set_note(app, true, wf_msg(MSG_SMART_GO_CANNOT_PASTE));
		return WF_ACT_REDRAW;
	}
	st = wf_smart_clean_paste(wide, units, &clean, &clean_n);
	free(wide);
	if (WF_E_INVAL == st) {
		set_note(app, true, wf_msg(MSG_SMART_GO_PASTE_ONE_LINE));
		return WF_ACT_REDRAW;
	}
	if (WF_OK != st) {
		set_note(app, true, wf_msg(MSG_ERROR_OUT_OF_MEMORY));
		return WF_ACT_REDRAW;
	}
	take_selection(app);
	st = text_replace(app, app->go.cursor, app->go.cursor, clean, clean_n);
	free(clean);
	if (WF_OK != st) {
		set_note(app, true, wf_msg(MSG_ERROR_OUT_OF_MEMORY));
		return WF_ACT_REDRAW;
	}
	return go_edit_changed(app);
}

static WfAction go_event(WfApp *app, const WfEvent *ev)
{
	WfStatus st;

	if (WF_EV_RELEASE == ev->type || WF_EV_RESIZE == ev->type) {
		return WF_ACT_NONE;
	}
	if (app->go.committing) {
		if (WF_EV_KEY == ev->type && WF_KEY_ESC == ev->key) {
			/* give up waiting: the listing, if it arrives, still counts
			 * as an ordinary navigation */
			go_close(app);
			return WF_ACT_REDRAW;
		}
		return WF_ACT_NONE;
	}
	if (WF_EV_CHAR == ev->type) {
		wchar_t unit[2];
		size_t n = 1;

		if (!scalar_ok(ev->ch)) {
			return WF_ACT_NONE;
		}
		if (ev->ch >= 0x10000) {
			uint32_t cp = ev->ch - 0x10000;

			unit[0] = (wchar_t)(0xD800 + (cp >> 10));
			unit[1] = (wchar_t)(0xDC00 + (cp & 0x3FF));
			n = 2;
		} else {
			unit[0] = (wchar_t)ev->ch;
		}
		take_selection(app);
		st = text_replace(app, app->go.cursor, app->go.cursor, unit, n);
		if (WF_OK != st) {
			set_note(app, true, wf_msg(MSG_ERROR_OUT_OF_MEMORY));
			return WF_ACT_REDRAW;
		}
		return go_edit_changed(app);
	}
	if (WF_EV_CTRL == ev->type) {
		if ('u' == ev->ch) {
			take_selection(app);
			app->go.len = app->go.cursor = 0;
			if (app->go.text) {
				app->go.text[0] = L'\0';
			}
			return go_edit_changed(app);
		}
		if ('v' == ev->ch) {
			return go_paste(app);
		}
		return WF_ACT_NONE;
	}
	if (WF_EV_KEY != ev->type) {
		return WF_ACT_NONE;
	}
	switch (ev->key) {
	case WF_KEY_ESC:       go_close(app); return WF_ACT_REDRAW;
	case WF_KEY_ENTER:     return go_enter(app);
	case WF_KEY_TAB:       return go_tab(app, false);
	case WF_KEY_SHIFT_TAB: return go_tab(app, true);
	case WF_KEY_DOWN:      return go_down(app);
	case WF_KEY_UP:        return go_up(app);
	case WF_KEY_LEFT:
		app->go.cursor = app->go.all_selected
		        ? 0
		        : wf_utf16_prev_boundary(app->go.text, app->go.cursor);
		app->go.all_selected = false;
		return WF_ACT_REDRAW;
	case WF_KEY_RIGHT:
		app->go.cursor = app->go.all_selected
		        ? app->go.len
		        : wf_utf16_next_boundary(
		                  app->go.text, app->go.len, app->go.cursor);
		app->go.all_selected = false;
		return WF_ACT_REDRAW;
	case WF_KEY_HOME:
		app->go.cursor = 0;
		app->go.all_selected = false;
		return WF_ACT_REDRAW;
	case WF_KEY_END:
		app->go.cursor = app->go.len;
		app->go.all_selected = false;
		return WF_ACT_REDRAW;
	case WF_KEY_BACKSPACE:
	case WF_KEY_DELETE:    {
		size_t a, b;

		if (app->go.all_selected) {
			take_selection(app);
			return go_edit_changed(app);
		}
		if (WF_KEY_BACKSPACE == ev->key) {
			a = wf_utf16_prev_boundary(
			        app->go.text, app->go.cursor);
			b = app->go.cursor;
		} else {
			a = app->go.cursor;
			b = wf_utf16_next_boundary(
			        app->go.text, app->go.len, a);
		}
		if (a == b) {
			return WF_ACT_REDRAW;
		}
		(void)text_replace(app, a, b, L"", 0);
		return go_edit_changed(app);
	}
	case WF_KEY_CTRL_BACKSPACE: {
		size_t a;

		if (app->go.all_selected) {
			take_selection(app);
			return go_edit_changed(app);
		}
		a = wf_smart_element_start(app->go.text, app->go.cursor);
		if (a == app->go.cursor) {
			return WF_ACT_REDRAW;
		}
		(void)text_replace(app, a, app->go.cursor, L"", 0);
		return go_edit_changed(app);
	}
	default: return WF_ACT_NONE;
	}
}

static bool dir_has_entry(const WfDir *dir, const wchar_t *name)
{
	size_t i;

	for (i = 0; i < dir->n; i++) {
		if (!_wcsicmp(dir->e[i].name, name)) {
			return true;
		}
	}
	return false;
}

bool wf_app_go_listing_vetoed(WfApp *app, uint32_t gen, const WfDir *dir)
{
	/* only the retry that looks for a typed file in its folder */
	if (!app->go.open || !app->go.committing || gen != app->go.commit_gen ||
	        !app->go.fallback_tried || !app->go.leaf || !dir) {
		return false;
	}
	if (dir_has_entry(dir, app->go.leaf)) {
		return false;
	}
	return true;
}

static void reopen_input(WfApp *app, const char *why)
{
	app->go.committing = false;
	set_note(app, true, why);
	app->go.has_sel = false;
	go_update(app);
	set_note(app, true, why); /* go_update clears it */
}

void wf_app_go_listing_failed(WfApp *app, uint32_t gen, WfStatus st)
{
	if (!app->go.open || !app->go.committing || gen != app->go.commit_gen) {
		return;
	}
	/* a name that is not a folder may be a file of its folder */
	if (WF_E_NOENT == st && !app->go.fallback_tried && app->go.leaf) {
		WfSmartResult r;
		wchar_t *parent = NULL;

		if (wf_smart_resolve(&app->go_env, app->cwd, app->prev_dir,
		            app->go.text, app->go.len, &r) == WF_OK &&
		        WF_SG_OK == r.error && r.target) {
			size_t plen = wf_path_parent_len(r.target);

			if (plen) {
				parent = wf_path_dup_n(r.target, plen);
			}
		}
		wf_smart_result_free(&r);
		if (parent) {
			app->go.fallback_tried = true;
			app->msg[0] = '\0';
			app->go.commit_gen = app->nav_gen + 1;
			if (wf_app_navigate(app, parent, app->go.leaf,
			            SIZE_MAX) == WF_OK) {
				free(parent);
				if (&WF_MODE_GO != app->mode) {
					go_free(app); /* a confirmation took over */
				}
				return;
			}
			free(parent);
		}
	}
	app->msg[0] = '\0';
	reopen_input(app,
	        wf_msg(WF_E_ACCESS == st           ? MSG_ERROR_ACCESS_DENIED
	                        : WF_E_NOENT == st ? MSG_ERROR_PATH_NOT_FOUND
	                                           : MSG_SMART_GO_CANNOT_READ));
}

void wf_app_go_listing_done(WfApp *app, uint32_t gen)
{
	if (!app->go.open || !app->go.committing || gen != app->go.commit_gen) {
		return;
	}
	go_close(app);
}

static size_t clip_left(const char *s, size_t len, int budget)
{
	size_t start = len;
	int cells = 0;

	while (start > 0) {
		size_t prev = start - 1;
		uint32_t cp;

		while (prev > 0 && 0x80 == ((unsigned char)s[prev] & 0xC0)) {
			prev--;
		}
		wf_utf8_decode(s + prev, start - prev, &cp);
		if (cells + wf_cp_width(cp) > budget) {
			break;
		}
		cells += wf_cp_width(cp);
		start = prev;
	}
	return start;
}

static size_t clip_right(const char *s, size_t len, int budget)
{
	size_t end = 0;
	int cells = 0;

	while (end < len) {
		uint32_t cp;
		size_t n = wf_utf8_decode(s + end, len - end, &cp);
		int w = wf_cp_width(cp);

		if (cells + w > budget) {
			break;
		}
		cells += w;
		end += n;
	}
	return end;
}

/* The input line, inside the box: columns x0 .. x0 + w - 1. */
static void draw_input(
        const WfApp *app, WfFrame *f, int x0, int w, int y, const WfStyle *st)
{
	const char *label = wf_msg(MSG_SMART_GO_LABEL);
	const int lw = wf_utf8_cells(label, strlen(label));
	WfBuf before, after;
	int budget = w - lw - 1, prefix_cells;
	size_t pstart, aend;
	WfStyle text = *st;

	wf_buf_init(&before);
	wf_buf_init(&after);
	wf_sanitize_utf16(
	        app->go.text ? app->go.text : L"", app->go.cursor, &before);
	wf_sanitize_utf16(app->go.text ? app->go.text + app->go.cursor : L"",
	        app->go.len - app->go.cursor, &after);
	if (budget < 1) {
		budget = 1;
	}
	pstart = clip_left(wf_buf_cstr(&before), before.len, budget - 1);
	prefix_cells = wf_utf8_cells(
	        wf_buf_cstr(&before) + pstart, before.len - pstart);
	aend = clip_right(wf_buf_cstr(&after), after.len,
	        budget - 1 - prefix_cells > 0 ? budget - 1 - prefix_cells : 0);
	if (app->go.all_selected) {
		text.attributes |= WF_ATTR_REVERSE;
	}
	wf_frame_fill(f, x0, y, w, 1, ' ', st);
	wf_app_put_str(f, x0, y, label, st, w);
	{
		int x = x0 + lw;

		x += wf_frame_put(f, x, y, wf_buf_cstr(&before) + pstart,
		        before.len - pstart, &text, x0 + w - x);
		if (x < x0 + w) {
			wf_frame_put(f, x, y, "_", 1, st, 1);
			wf_frame_set_cursor(f, x, y);
		}
		x++;
		if (x < x0 + w && aend) {
			wf_frame_put(f, x, y, wf_buf_cstr(&after), aend, &text,
			        x0 + w - x);
		}
	}
	wf_buf_free(&before);
	wf_buf_free(&after);
}

/*
 * A panel that floats over the list: the folder behind it stays on screen
 * (and, when Esc is pressed, is exactly as it was: nothing here touches the
 * list, its selection or its scroll position). Box of at most 80 columns,
 * centred, with a frame when there is height for one.
 */
static void go_draw(const WfApp *app, WfFrame *f)
{
	const WfThemeStyles *th = wf_theme_styles();
	WfStyle panel = th->panel, dim = th->panel, warn = th->panel,
	        sel = th->selection;
	int list_rows = (int)go_rows(app), i, y, bx, bw, top, in_x, in_w, h;
	bool frame;
	const char *hint = wf_msg(MSG_SMART_GO_HINT);
	char state[40] = "";

	dim.fg_rgb = wf_theme_active()->muted_foreground;
	warn.fg_rgb = wf_theme_active()->warning_foreground;
	if (f->rows < 4 || f->cols < 12) {
		return;
	}
	bw = f->cols - 4 > 80 ? 80 : f->cols - 4;
	if (bw < 36) {
		bw = f->cols;
	}
	bx = (f->cols - bw) / 2;
	frame = f->rows >= 14 && bw < f->cols;
	h = list_rows + 2 + (frame ? 2 : 0); /* input, rows, hint */
	top = frame ? 3 : f->rows - 1 - h;
	if (top < 1) {
		top = 1;
	}
	wf_frame_fill(f, bx, top, bw, h, ' ', &panel);
	in_x = bx + (frame ? 2 : 0);
	in_w = bw - (frame ? 4 : 0);
	if (frame) {
		int r;

		for (r = 0; r < h; r++) {
			bool edge = 0 == r || h - 1 == r;

			wf_frame_put(
			        f, bx, top + r, edge ? "+" : "|", 1, &dim, 1);
			wf_frame_put(f, bx + bw - 1, top + r, edge ? "+" : "|",
			        1, &dim, 1);
		}
		for (r = 1; r < bw - 1; r++) {
			wf_frame_put(f, bx + r, top, "-", 1, &dim, 1);
			wf_frame_put(f, bx + r, top + h - 1, "-", 1, &dim, 1);
		}
		y = top + 1;
	} else {
		y = top;
	}
	draw_input(app, f, in_x, in_w, y, &th->prompt_label);
	if (WF_GR_MORE == app->go.read) {
		snprintf(state, sizeof state, "%s",
		        wf_msg(MSG_SMART_GO_LOADING_MORE));
	} else if (WF_GR_FIRST == app->go.read && !app->go.cand_n) {
		snprintf(state, sizeof state, "%s",
		        wf_msg(MSG_SMART_GO_SEARCHING));
	}
	if (state[0] && in_w > 30) {
		int w = wf_utf8_cells(state, strlen(state));

		wf_app_put_str(
		        f, in_x + in_w - w - 1, y, state, &th->prompt, w);
	}

	y++;
	for (i = 0; i < list_rows; i++) {
		size_t idx = app->go.top + (size_t)i;
		const WfGoCand *c;
		bool chosen;
		WfBuf name;
		char shown[WF_CAND_NAME * 4 + 4];
		const WfStyle *st;

		if (idx >= app->go.cand_n) {
			break;
		}
		c = &app->go.cand[idx];
		chosen = app->go.has_sel && idx == app->go.sel;
		st = chosen ? &sel : &panel;
		wf_buf_init(&name);
		wf_sanitize_utf16(c->name, wcslen(c->name), &name);
		snprintf(shown, sizeof shown, "%s%s", wf_buf_cstr(&name),
		        (c->flags & WF_ENT_DIR)
		                ? (WF_GO_ALIAS == app->go.source ? "" : "/")
		                : "");
		wf_buf_free(&name);
		wf_frame_fill(f, in_x, y + i, in_w, 1, ' ', st);
		wf_app_put_str(f, in_x, y + i, chosen ? ">" : " ", st, 1);
		if (WF_GO_ALIAS == app->go.source) {
			char alias[WF_CAND_NAME + 4];

			snprintf(alias, sizeof alias, "@%s", shown);
			wf_app_put_fit(f, in_x + 2, y + i, alias, st, in_w - 3);
		} else {
			wf_app_put_fit(f, in_x + 2, y + i, shown, st, in_w - 3);
		}
	}
	/* when nothing is listed, say why */
	if (!app->go.cand_n && list_rows > 0) {
		const char *why = NULL;

		if (WF_GR_FIRST == app->go.read) {
			why = wf_msg(MSG_SMART_GO_SEARCHING);
		} else if (WF_GO_FS == app->go.source && app->go.q_dir &&
		        WF_OK == app->go.list_status) {
			why = wf_msg(MSG_SMART_GO_NO_MATCHES);
		}
		if (why) {
			wf_app_put_str(f, in_x + 2, y, why, &dim, in_w - 3);
		}
	}
	/* the hint row doubles as the place for a short error */
	{
		const char *line = app->go.note[0] ? app->go.note : hint;
		const WfStyle *ls = app->go.note[0]
		        ? (app->go.note_is_error ? &warn : &panel)
		        : &dim;

		wf_app_put_fit(f, in_x, y + list_rows, line, ls, in_w);
	}
}

const WfMode WF_MODE_GO = {"PATH", go_draw, go_event};
