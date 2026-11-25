#include <stdio.h>
#include <string.h>

#include "../archive/wf_vpath.h"
#include "../fs/wf_loc.h"
#include "../fs/wf_sftpplace_win.h"
#include "../path/wf_path.h"
#include "../render/wf_mddraw.h"
#include "../text/wf_text.h"
#include "wf_app_priv.h"
#include "../base/wf_macro.h"

#define style_dim (wf_theme_styles()->dim)
#define style_dir (wf_theme_styles()->directory)

void wf_app_put_str(WfFrame *f,
        int x,
        int y,
        const char *s,
        const WfStyle *st,
        int max_cells)
{
	wf_frame_put(f, x, y, s, strlen(s), st, max_cells);
}

/*
 * Writes `s` in at most max_cells columns. Too long: the middle is replaced
 * by "..." so the start and the end (what tells paths and names apart) stay
 * visible (spec 4.2). Cuts fall on UTF-8 boundaries.
 */
int wf_app_put_fit(WfFrame *f,
        int x,
        int y,
        const char *s,
        const WfStyle *st,
        int max_cells)
{
	size_t len = strlen(s), head = 0, tail = 0;
	int budget, cells = 0, total;

	if (max_cells < 8) {
		return wf_frame_put(f, x, y, s, len, st, max_cells);
	}
	total = wf_utf8_cells(s, len);
	if (total <= max_cells) {
		return wf_frame_put(f, x, y, s, len, st, max_cells);
	}
	budget = max_cells - 3;
	while (head < len) {
		uint32_t cp;
		size_t n = wf_utf8_decode(s + head, len - head, &cp);
		int width = wf_cp_width(cp);
		if (cells + width > budget / 2) {
			break;
		}
		cells += width;
		head += n;
	}
	/* Drop whole scalars until the suffix fits; never start on a combining mark. */
	{
		int remaining = total;
		while (tail < len) {
			uint32_t cp;
			size_t n = wf_utf8_decode(s + tail, len - tail, &cp);
			int width = wf_cp_width(cp);
			if (remaining <= budget - cells && width) {
				break;
			}
			remaining -= width;
			tail += n;
		}
	}
	int used = wf_frame_put(f, x, y, s, head, st, cells);
	used += wf_frame_put(f, x + cells, y, "...", 3, st, 3);
	used += wf_frame_put(f, x + cells + 3, y, s + tail, len - tail, st,
	        max_cells - cells - 3);
	return used;
}

/* Text being typed, with the cursor at its end: drawn in `width` cells (one is
 * kept for the cursor) from the start when it fits, else from the right so that
 * what was typed last is on screen, with "<" in front to say that the start is
 * cut. Returns the cell where the cursor goes. Cuts fall on scalars, and a cut
 * never leaves a combining mark first. */
int wf_app_put_input_tail(WfFrame *f,
        int x,
        int y,
        const char *s,
        size_t n,
        int width,
        const WfStyle *st)
{
	size_t at = 0;
	int cells, room;

	if (width < 2) {
		return x;
	}
	cells = wf_utf8_cells(s, n);
	if (cells + 1 <= width) {
		wf_frame_put(f, x, y, s, n, st, width);
		return x + cells;
	}
	room = width - 2; /* the mark and the cursor */
	while (at < n && cells > room) {
		uint32_t cp;
		size_t step = wf_utf8_decode(s + at, n - at, &cp);

		if (0 == step) {
			step = 1;
		}
		cells -= wf_cp_width(cp);
		at += step;
	}
	while (at < n) { /* not on a combining mark */
		uint32_t cp;
		size_t step = wf_utf8_decode(s + at, n - at, &cp);

		if (wf_cp_width(cp) || 0 == step) {
			break;
		}
		at += step;
	}
	wf_frame_put(f, x, y, "<", 1, st, 1);
	wf_frame_put(f, x + 1, y, s + at, n - at, st, room);
	return x + 1 + wf_utf8_cells(s + at, n - at);
}

void wf_app_vrule(WfFrame *f, int x, int y, int h)
{
	wf_frame_fill(f, x, y, 1, h, '|', &wf_theme_styles()->border);
}

bool wf_ui_alert(WfFrame *f, const char *const *lines, int n)
{
	/* spec 5.1: width = min(64, cols-8), floor 36; the 2-cell halo must fit
	 */
	int w;
	int h = n + 4; /* rule, blank, lines, blank, rule */
	int x, y, i;

	if (!f || (n > 0 && !lines) || n < 0) {
		return false;
	}
	w = f->cols - 8 < 64 ? f->cols - 8 : 64;
	if (w < 36) {
		w = 36;
	}
	if (w + 4 > f->cols || f->rows < h + 4) {
		return false;
	}
	x = (f->cols - w) / 2;
	y = (f->rows - h) / 2;

	wf_frame_fill(f, x - 2, y - 2, w + 4, h + 4, ' ', &WF_STYLE_BASE);
	wf_frame_fill(f, x, y, w, 1, '=', &WF_STYLE_BASE);
	wf_frame_fill(f, x, y + h - 1, w, 1, '=', &WF_STYLE_BASE);
	for (i = 0; i < n; i++) {
		int len = wf_utf8_cells(lines[i], strlen(lines[i]));
		int pad = len < w ? (w - len) / 2 : 0;

		wf_app_put_fit(f, x + pad, y + 2 + i, lines[i], &WF_STYLE_BASE,
		        w - pad);
	}
	return true;
}

void wf_ui_alert_or_text(WfFrame *f, const char *const *lines, int n)
{
	int rows, shown;

	if (!f || n <= 0 || wf_ui_alert(f, lines, n)) {
		return;
	}
	rows = f->rows;
	wf_frame_fill(f, 0, 0, f->cols, rows, ' ', &WF_STYLE_BASE);
	/* the last line is the one that says how to leave; it keeps its row */
	shown = n <= rows ? n : rows - 1;
	for (int i = 0; i < shown; i++) {
		wf_app_put_fit(f, 0, i, lines[i], &WF_STYLE_BASE, f->cols);
	}
	if (n > rows && rows >= 1) {
		wf_app_put_fit(
		        f, 0, rows - 1, lines[n - 1], &WF_STYLE_BASE, f->cols);
	}
}

int wf_app_body_height(const WfApp *app)
{
	WfLayout lay;

	wf_layout_compute(app->cols, app->rows, &app->ratios, &lay);
	return lay.body_h > 0 ? lay.body_h : 1;
}

/* `sel` non-NULL adds a two-column marker gutter ("* " for selected).
 * `sizes` non-NULL writes what D counted at the end of a folder's row, when
 * the pane is wide enough to keep a readable name beside it. */
static void draw_list(WfFrame *f,
        int x,
        int y,
        int w,
        int h,
        const WfView *v,
        bool gutter,
        bool show_cursor,
        bool active,
        const WfNameSet *sel,
        const WfDirSizes *sizes,
        const wchar_t *sizes_dir,
        const WfApp *git)
{
	size_t top = wf_view_top(v, h);
	int row;

	for (row = 0; row < h && top + (size_t)row < v->nvis; row++) {
		size_t i = top + (size_t)row;
		const WfEntry *e = &v->dir->e[v->vis[i]];
		bool is_dir = 0 != (e->flags & WF_ENT_DIR);
		WfStyle st = is_dir ? style_dir : WF_STYLE_BASE;
		int cx = x, used;

		if (show_cursor && i == v->cursor) {
			st = active ? wf_theme_styles()->selection
			            : wf_theme_styles()->cursor_line;
			if (is_dir) {
				st.attributes |= WF_ATTR_BOLD;
			}
			wf_frame_fill(f, x, y + row, w, 1, ' ', &st);
		}
		if (gutter) {
			if (show_cursor && i == v->cursor) {
				wf_frame_put(f, cx, y + row, ">", 1, &st, 1);
			}
			if (wf_nameset_has(sel, e->name)) {
				wf_frame_put(
				        f, cx + 1, y + row, "*", 1, &st, 1);
			}
			cx += 2;
		}
		if (cx - x >= w) {
			continue;
		}
		char total[32];
		int total_w = 0;
		/* Git's letter at the right edge, when there is one and the pane is wide
		 * enough to keep a name beside it */
		char gl = git && w >= 24 ? wf_app_git_letter(git, e) : 0;
		int gw = gl ? 2 : 0;
		bool on_cursor = show_cursor && i == v->cursor;
		/* The name takes the colour of the letter. A folder with no letter of
		 * its own is tinted when a file below it has changed. */
		const WfStyle *gs = NULL;
		WfStyle name_style = st;

		if (gl) {
			const WfThemeStyles *t = wf_theme_styles();

			gs = 'D' == gl || '!' == gl ? &t->error
			        : 'M' == gl         ? &t->warning
			        : 'A' == gl         ? &t->note
			        : 'R' == gl         ? &t->accent
			                            : &style_dim;
		} else if (git && w >= 24 && is_dir &&
		        (!git->config || git->config->git_folder_color) &&
		        wf_app_git_dir_changed(git, e)) {
			gs = &wf_theme_styles()->warning;
		}
		if (gs && !on_cursor) {
			name_style = *gs;
			name_style.attributes |= st.attributes;
		}

		if (sizes && is_dir) {
			const WfSizeUpdate *u =
			        wf_dirsizes_find(sizes, sizes_dir, e->name);

			if (u) {
				wf_dirsizes_short(u, total, sizeof total);
				total_w = (int)strlen(total);
				if (w - (cx - x) - gw < total_w + 10) {
					total_w =
					        0; /* no room for a name too */
				}
			}
		}
		used = wf_app_put_fit(f, cx, y + row, e->display, &name_style,
		        w - (cx - x) - (is_dir ? 1 : 0) -
		                (total_w ? total_w + 1 : 0) - gw);
		if (is_dir && cx - x + used < w) {
			wf_frame_put(
			        f, cx + used, y + row, "/", 1, &name_style, 1);
		}
		if (total_w) {
			wf_frame_put(f, x + w - gw - total_w, y + row, total,
			        (size_t)total_w,
			        show_cursor && i == v->cursor ? &st
			                                      : &style_dim,
			        total_w);
		}
		if (gl) {
			wf_frame_put(f, x + w - 1, y + row, &gl, 1,
			        on_cursor ? &st : gs, 1);
		}
	}
}

/* The mode as the user reads it. Mode names stay stable identifiers; only
 * the label is translated. */
static const char *mode_label(const WfApp *app)
{
	static const struct
	{
		const char *name;
		WfMsg label;
	} labels[] = {{"NORMAL", MSG_MODE_NORMAL},
	        {"CONFIRM", MSG_MODE_CONFIRM}, {"REPORT", MSG_MODE_REPORT},
	        {"EDIT", MSG_MODE_EDIT}, {"PATH", MSG_MODE_PATH},
	        {"HELP", MSG_MODE_HELP}, {"INFO", MSG_MODE_INFO},
	        {"FILTER", MSG_MODE_FILTER}, {"PREVIEW", MSG_MODE_PREVIEW},
	        {"PREVIEW_PANE", MSG_MODE_PREVIEW_PANE},
	        {"SETTINGS", MSG_MODE_SETTINGS}, {"SETUP", MSG_MODE_SETUP},
	        {"NAME", MSG_MODE_NAME}, {"CREATE", MSG_MODE_CREATE},
	        {"PASSWORD", MSG_MODE_PASSWORD}, {"CMDLINE", MSG_MODE_CMDLINE},
	        {"SORT", MSG_MODE_SORT}, {"FUZZY", MSG_MODE_FUZZY},
	        {"BATCH", MSG_MODE_BATCH}, {"PLACE", MSG_MODE_PLACE},
	        {"HITS", MSG_MODE_HITS}};

	if (!strcmp(app->mode->name, "NORMAL") && wf_vpath_is(app->cwd)) {
		return wf_msg(MSG_MODE_ARCHIVE);
	}
	if (!strcmp(app->mode->name, "NORMAL") && wf_wsl_unc_is(app->cwd)) {
		return wf_msg(MSG_MODE_WSL);
	}
	if (!strcmp(app->mode->name, "NORMAL") && wf_sftp_place_is(app->cwd)) {
		return wf_msg(MSG_MODE_SFTP);
	}
	if (!strcmp(app->mode->name, "NORMAL") && wf_pcroot_is(app->cwd)) {
		return wf_msg(MSG_MODE_PC);
	}
	for (size_t i = 0; i < WF_ARRAY_LEN(labels); i++) {
		if (!strcmp(app->mode->name, labels[i].name)) {
			return wf_msg(labels[i].label);
		}
	}
	return app->mode->name;
}

static void draw_title(const WfApp *app, WfFrame *f)
{
	WfBuf path;
	char label[128];
	WfProfile p = wf_keymap_profile(app->keymap);
	WfMsg profile = WF_PROFILE_VIM == p ? MSG_PROFILE_VIM
	        : WF_PROFILE_NANO == p      ? MSG_PROFILE_NANO
	        : WF_PROFILE_CUSTOM == p    ? MSG_PROFILE_CUSTOM
	                                    : MSG_PROFILE_STANDARD;
	snprintf(label, sizeof label, "%s | %s", mode_label(app),
	        wf_msg(profile));
	int mode_w = wf_utf8_cells(label, strlen(label));
	if (app->recovery.unknown || app->recovery.unresolved) {
		const char *badge = wf_msg(MSG_SAFETY_REVIEW);
		int badge_w = wf_utf8_cells(badge, strlen(badge));
		if (f->cols >= mode_w + badge_w + 12) {
			size_t used = strlen(label);
			snprintf(label + used, sizeof label - used, " | %s",
			        badge);
		} else {
			snprintf(label, sizeof label, "%s", badge);
		}
		mode_w = wf_utf8_cells(label, strlen(label));
	}
	if (mode_w > f->cols) {
		mode_w = f->cols;
	}

	wf_frame_fill(f, 0, 0, f->cols, 1, ' ', &wf_app_style_bar);
	wf_buf_init(&path);
	{
		/* the tab strip only when there is more than one tab; the folder
		 * keeps what is left */
		int room = f->cols - mode_w - 2;
		int strip = wf_app_tabs_strip(app, f, 0,
		        room > 24            ? room / 2
		                : room >= 12 ? 7
		                             : 0);

		if (wf_pcroot_is(app->cwd)) {
			wf_buf_puts(&path, wf_msg(MSG_PC_TITLE));
		}
		if (path.len ||
		        WF_OK ==
		                wf_sanitize_utf16(
		                        app->cwd, wcslen(app->cwd), &path)) {
			wf_app_put_fit(f, strip ? strip + 1 : 0, 0,
			        wf_buf_cstr(&path), &wf_app_style_bar,
			        room - (strip ? strip + 1 : 0));
		}
	}
	wf_buf_free(&path);
	wf_app_put_fit(f, f->cols - mode_w, 0, label,
	        &wf_theme_styles()->bar_bold, mode_w);
}

/* The part of `s` left after dropping `cells` display columns. A wide scalar
 * cut in half is dropped and *pad reports the columns of it that remain
 * visible to the right of the cut. `s` is sanitized, NUL-terminated UTF-8. */
static const char *skip_cells(const char *s, size_t cells, int *pad)
{
	size_t used = 0;

	*pad = 0;
	while (*s) {
		size_t avail = 0, n;
		uint32_t cp;
		int width;

		while (avail < 4 && s[avail]) {
			avail++;
		}
		n = wf_utf8_decode(s, avail, &cp);
		width = wf_cp_width(cp);
		if (used >= cells && width) {
			break;
		}
		if (used + (size_t)width > cells) {
			*pad = (int)(used + (size_t)width - cells);
			s += n;
			break;
		}
		used += (size_t)width;
		s += n;
	}
	return s;
}

/* Why a directory could not be listed, in the words the pane shows. */
static const char *dirprev_failure(WfStatus st)
{
	switch (st) {
	case WF_E_ACCESS: return wf_msg(MSG_ERROR_ACCESS_DENIED);
	case WF_E_NOENT:  return wf_msg(MSG_ERROR_DIR_NOT_FOUND);
	case WF_E_NOMEM:  return wf_msg(MSG_ERROR_OUT_OF_MEMORY);
	default:          return wf_msg(MSG_ERROR_CANNOT_LIST);
	}
}

/* The listing of the directory under the cursor: a count line, then its
 * entries in the pane's own view (no cursor). Returns false when the pane is
 * not showing a directory listing. */
/* LOADING is for going to another folder (or the first listing). A folder that
 * is read again where it is (the watch, the reload) shows what it showed until
 * the new reading differs, and says nothing meanwhile. */
static bool navigating(const WfApp *app)
{
	return app->want_path &&
	        (!app->cwd || wcscmp(app->want_path, app->cwd));
}

/* A folder on screen with nothing in the list says why, in one quiet line at
 * the top of the pane: the folder is empty, or what is in it is hidden, or the
 * filter leaves nothing. Without it the pane is blank and the reason has to be
 * read from the counts at the bottom. */
static void draw_list_empty(const WfApp *app, WfFrame *f, int x, int y, int w)
{
	WfMsg id;

	if (!app->cur.dir || app->cur.nvis || navigating(app) || w < 1) {
		return;
	}
	id = 0 == app->cur.dir->n    ? MSG_DIR_EMPTY
	        : app->cur.filter[0] ? MSG_LIST_NO_MATCH
	                             : MSG_DIR_ONLY_HIDDEN;
	wf_app_put_fit(f, x, y, wf_msg(id), &style_dim, w);
}

static bool draw_dirprev(
        const WfApp *app, WfFrame *f, int x, int y, int w, int h)
{
	const WfView *v = &app->dirprev;
	char head[64];
	size_t shown, hidden;

	/* A folder that is read again while it is shown keeps what it shows until
	 * the new reading differs: nothing says "updating" over the count. */
	switch (app->dirprev_state) {
	case WF_DIRPREV_NONE: return false;
	case WF_DIRPREV_WAIT:
	case WF_DIRPREV_LOADING:
		if (v->dir) {
			break; /* retained same-object snapshot, labeled below */
		}
		if (app->now_ms >= app->preview_loading_due) {
			wf_app_put_fit(f, x, y, wf_msg(MSG_DIR_LOADING),
			        &style_dim, w);
		}
		return true;
	case WF_DIRPREV_FAILED:
		wf_app_put_str(f, x, y, dirprev_failure(app->dirprev_status),
		        &style_dim, w);
		return true;
	case WF_DIRPREV_READY: break;
	}
	if (!v->dir || 0 == v->dir->n) {
		wf_app_put_fit(f, x, y, wf_msg(MSG_DIR_EMPTY), &style_dim, w);
		return true;
	}
	if (0 == v->nvis) {
		wf_app_put_fit(
		        f, x, y, wf_msg(MSG_DIR_ONLY_HIDDEN), &style_dim, w);
		return true;
	}
	hidden = v->dir->n - v->nvis;
	{
		size_t n = wf_msgf(head, sizeof head,
		        wf_msg_count(MSG_DIR_ITEMS_ONE, MSG_DIR_ITEMS_OTHER,
		                (long long)v->nvis),
		        &WF_ARG_N(v->nvis), 1);

		if (v->dir->truncated) {
			n += wf_msgf(head + n, sizeof head - n, MSG_DIR_PARTIAL,
			        NULL, 0);
		}
		if (hidden) {
			(void)wf_msgf(head + n, sizeof head - n,
			        MSG_DIR_SOME_HIDDEN, NULL, 0);
		}
	}
	wf_app_put_fit(f, x, y, head, &style_dim, w);
	if (h <= 1) {
		return true;
	}
	draw_list(f, x, y + 1, w, h - 1, v, false, false, false, NULL, NULL,
	        NULL, NULL);
	shown = (size_t)(h - 1);
	if (v->nvis > shown) {
		char more[32];

		(void)wf_msgf(more, sizeof more, MSG_DIR_MORE,
		        &WF_ARG_N(v->nvis - shown + 1), 1);
		wf_frame_fill(f, x, y + h - 1, w, 1, ' ', &WF_STYLE_BASE);
		wf_app_put_fit(f, x, y + h - 1, more, &style_dim, w);
	}
	return true;
}

/* One line of the text preview: in the colours the syntax worker answered with
 * when it has them for this text (DR-DRAFT-033), else in the plain style. `text`
 * is the part of line `line` that is visible (it starts `text - line0` bytes into
 * it). The colours are classes of bytes; the class decides the style, the cells
 * are the renderer's. */
static void draw_text_line(const WfApp *app,
        WfFrame *f,
        int x,
        int y,
        const char *line0,
        const char *text,
        size_t line,
        int width)
{
	uint8_t cls[WF_PREVIEW_LINE_BYTES + 1];
	size_t from = (size_t)(text - line0), n = strlen(text), i = 0;
	int used = 0;

	if (from + n > WF_PREVIEW_LINE_BYTES ||
	        !wf_app_syntax_preview_line(app, line, from, n, cls)) {
		wf_app_put_str(f, x, y, text, &WF_STYLE_BASE, width);
		return;
	}
	while (i < n && used < width) {
		size_t j = i + 1;
		WfStyle st;

		while (j < n && cls[j] == cls[i]) {
			j++;
		}
		/* a colour never splits a character */
		while (j < n && 0x80 == ((unsigned char)text[j] & 0xc0)) {
			j++;
		}
		st = wf_theme_syntax_style(cls[i]);
		used += wf_frame_put(
		        f, x + used, y, text + i, j - i, &st, width - used);
		i = j;
	}
}

/* The digest of the file under the cursor (#), as "SHA-256 <hex>", when it holds
 * for the file as listed now: the last row of a text preview is kept for it. */
bool wf_app_preview_sha_line(const WfApp *app, char *out, size_t cap)
{
	char hex[65];
	WfMsgArg arg = WF_ARG_S(hex);

	if (!app->hash.has ||
	        !wf_app_hash_line(
	                app, wf_view_current(&app->cur), hex, sizeof hex) ||
	        !strcmp(hex, wf_msg(MSG_INFO_SHA_RUNNING))) {
		return false;
	}
	(void)wf_msgf(out, cap, MSG_PREVIEW_SHA, &arg, 1);
	return true;
}

void wf_app_draw_preview(
        const WfApp *app, WfFrame *f, int x, int y, int w, int h)
{
	const WfPreview *p = app->preview;
	size_t top, limit, left;
	int row, text_rows, digits = 1, gutter;
	char sha[96];
	if (w <= 0 || h <= 0) {
		return;
	}
	if (draw_dirprev(app, f, x, y, w, h)) {
		return;
	}
	if (wf_app_image_draw(app, f, x, y, w, h)) {
		return;
	}
	if (app->preview_busy && !p) {
		if (app->now_ms >= app->preview_loading_due) {
			wf_app_put_fit(f, x, y,
			        wf_msg(MSG_PREVIEW_LOADING_TEXT), &style_dim,
			        w);
		}
		return;
	}
	if (!p) {
		wf_app_put_fit(
		        f, x, y, wf_msg(MSG_PREVIEW_NONE), &style_dim, w);
		return;
	}
	if (p->system_error) {
		char detail[160];
		char code[16];

		snprintf(code, sizeof code, "%lu",
		        (unsigned long)p->system_error);
		(void)wf_msgf(detail, sizeof detail, MSG_PREVIEW_WIN32_ERROR,
		        (WfMsgArg[]){WF_ARG_S(code),
		                WF_ARG_S(wf_msg_reason(p->detail))},
		        2);
		wf_app_put_fit(f, x, y, detail, &style_dim, w);
	} else {
		wf_app_put_fit(
		        f, x, y, wf_msg_reason(p->detail), &style_dim, w);
	}
	if (WF_PREVIEW_TEXT != p->kind) {
		return;
	}
	if (h > 2 && wf_app_preview_sha_line(app, sha, sizeof sha)) {
		wf_app_put_fit(f, x, y + h - 1, sha, &style_dim, w);
		h--;
	}
	{ /* a Markdown file reads as Markdown (DR-DRAFT-025) */
		const WfMdDoc *md = wf_app_md_doc(app, w);

		if (md) {
			bool truncated = p->truncated || md->truncated;
			int rows = h - 1 - (truncated && h > 1);
			size_t mtop = app->preview_top;
			size_t mlimit = rows > 0 && md->nlines > (size_t)rows
			        ? md->nlines - (size_t)rows
			        : 0;

			if (mtop > mlimit) {
				mtop = mlimit;
			}
			if (rows > 0) {
				wf_md_draw(f, x, y + 1, w, rows, md, mtop);
			}
			if (truncated && h > 1) {
				wf_app_put_fit(f, x, y + h - 1,
				        wf_msg(MSG_PREVIEW_TRUNCATED),
				        &style_dim, w);
			}
			return;
		}
	}
	limit = wf_app_preview_scroll_limit(p, h);
	top = app->preview_top < limit ? app->preview_top : limit;
	left = wf_app_preview_pan_limit(p, w);
	if (app->preview_left < left) {
		left = app->preview_left;
	}
	if (left) { /* say that the text is panned */
		char col[48], num[24];
		int len;

		snprintf(num, sizeof num, "%zu", left + 1);
		(void)wf_msgf(
		        col, sizeof col, MSG_PREVIEW_COLUMN, &WF_ARG_S(num), 1);
		len = wf_utf8_cells(col, strlen(col));
		if (len > 0 && w > len + 20) {
			wf_app_put_str(f, x + w - len, y, col, &style_dim, len);
		}
	}
	text_rows = h - 1 - (p->truncated && h > 1);
	for (size_t n = p->nlines; n >= 10; n /= 10) {
		digits++;
	}
	gutter = digits + 3;
	for (row = 0; row < text_rows && top + (size_t)row < p->nlines; row++) {
		size_t line = top + (size_t)row;
		char number[32];

		snprintf(number, sizeof number, "%*zu | ", digits, line + 1);
		wf_app_put_str(f, x, y + row + 1, number, &style_dim, w);
		if (w > gutter) {
			int pad;
			const char *text = skip_cells(
			        p->text.data + p->lines[line], left, &pad);

			if (pad) {
				wf_app_put_str(f, x + gutter, y + row + 1, " ",
				        &WF_STYLE_BASE, pad);
			}
			if (w - gutter > pad) {
				draw_text_line(app, f, x + gutter + pad,
				        y + row + 1,
				        p->text.data + p->lines[line], text,
				        line, w - gutter - pad);
			}
		}
	}
	if (p->truncated && h > 1) {
		wf_app_put_fit(f, x, y + h - 1, wf_msg(MSG_PREVIEW_TRUNCATED),
		        &style_dim, w);
	}
}

/* selected_visible: selected names that are on screen right now. */
static size_t selected_visible_count(const WfApp *app, const WfNameSet *sel)
{
	size_t i, n = 0;

	if (!sel) {
		return 0;
	}
	for (i = 0; i < app->cur.nvis; i++) {
		if (wf_nameset_has(
		            sel, app->cur.dir->e[app->cur.vis[i]].name)) {
			n++;
		}
	}
	return n;
}

static size_t hidden_by_attribute(const WfApp *app)
{
	size_t n = 0;

	if (app->cur.dir && !app->cur.show_hidden) {
		for (size_t i = 0; i < app->cur.dir->n; i++) {
			n += 0 != (app->cur.dir->e[i].flags & WF_ENT_HIDDEN);
		}
	}
	return n;
}

/* The counts of the list (the hidden entries) are of this list and folder. */
static bool list_counts_current(const WfApp *app)
{
	return app->counts.valid && app->counts.view == app->cur.gen &&
	        app->counts.cwd == app->cwd_version;
}

/* The count of the selected names is also of this selection. */
static bool counts_current(const WfApp *app)
{
	return list_counts_current(app) && app->counts.sel == app->sel.gen;
}

void wf_app_sync_counts(WfApp *app)
{
	if (counts_current(app)) {
		return;
	}
	app->counts.sel_visible =
	        selected_visible_count(app, wf_sel_folder(&app->sel, app->cwd));
	if (!list_counts_current(app)) { /* a selection does not change these */
		app->counts.attr_hidden = hidden_by_attribute(app);
	}
	app->counts.view = app->cur.gen;
	app->counts.sel = app->sel.gen;
	app->counts.cwd = app->cwd_version;
	app->counts.valid = true;
}

bool wf_app_counts_current(const WfApp *app)
{
	return counts_current(app);
}

void wf_app_counts_toggled(WfApp *app, bool had, bool was_selected)
{
	if (!had || (was_selected && 0 == app->counts.sel_visible)) {
		return; /* not kept, or not what it should be: counted at the next sync */
	}
	app->counts.sel_visible += was_selected ? (size_t)-1 : 1;
	app->counts.sel = app->sel.gen;
}

/* The same numbers for a frame: from the counts when they are of this list,
 * else counted now (and not kept: the frame may not change the app). */
static size_t selected_visible(const WfApp *app, const WfNameSet *sel)
{
	return counts_current(app) ? app->counts.sel_visible
	                           : selected_visible_count(app, sel);
}

static size_t attr_hidden_count(const WfApp *app)
{
	return list_counts_current(app) ? app->counts.attr_hidden
	                                : hidden_by_attribute(app);
}

/* Said whenever the list is cut by a filter that is no longer being typed: a
 * short mark that comes before the counts and the hints, because it is what
 * tells that some of the files are out of sight. */
static const char *filter_mark(const WfApp *app)
{
	return app->cur.filter[0] && strcmp(app->mode->name, "FILTER")
	        ? wf_msg(MSG_STATUS_FILTER_ON)
	        : "";
}

static bool status_append_if_fit(WfBuf *s, const WfBuf *part, int max_cells)
{
	int used = wf_utf8_cells(wf_buf_cstr(s), s->len);
	int added = wf_utf8_cells(wf_buf_cstr(part), part->len);

	if (used > max_cells || added > max_cells - used) {
		return false;
	}
	return WF_OK == wf_buf_append(s, wf_buf_cstr(part), part->len);
}

static const char *status_hint(const WfApp *app)
{
	const char *mode = app->mode->name;

	if (&WF_MODE_PREVIEW_PANE == app->mode) {
		return wf_msg(MSG_STATUS_HINT_PANE);
	}
	if (!strcmp(mode, "FILTER")) {
		return wf_msg(MSG_STATUS_HINT_FILTER);
	}
	if (!strcmp(mode, "FUZZY")) {
		return wf_msg(MSG_STATUS_HINT_FUZZY);
	}
	if (!strcmp(mode, "BATCH")) {
		return wf_msg(MSG_STATUS_HINT_BATCH_PATTERN);
	}
	if (!strcmp(mode, "PATH")) {
		return wf_msg(MSG_STATUS_HINT_PATH);
	}
	if (!strcmp(mode, "NAME")) {
		return wf_msg(app->edit.submit ? MSG_STATUS_HINT_NAME_APPLY
		                               : MSG_STATUS_HINT_NAME_GATE);
	}
	if (!strcmp(mode, "CREATE")) {
		return wf_msg(app->edit.submit
		                ? MSG_STATUS_HINT_CREATE
		                : MSG_STATUS_HINT_CREATE_BLOCKED);
	}
	if (!strcmp(mode, "PREVIEW")) {
		return wf_msg(MSG_STATUS_HINT_PREVIEW);
	}
	if (!strcmp(mode, "CONFIRM")) {
		return WF_CONF_ARMED == app->confirm.state
		        ? app->confirm.armed_text
		        : wf_msg(MSG_STATUS_HINT_LOCKED);
	}
	return &WF_MODE_NORMAL == app->mode ? wf_msg(MSG_STATUS_HINT_NORMAL)
	                                    : "";
}

/* Compose from highest to lowest priority. Messages and recovery are handled
 * by draw_status first so they cannot be displaced by normal details. */
/* `mark`: the short filter mark comes first. `*filter_told`: the line says what
 * the filter is (the long form), so that the mark is not needed. */
static void status_build(
        const WfApp *app, WfBuf *s, int max_cells, bool mark, bool *filter_told)
{
	const WfNameSet *sel = wf_sel_folder(&app->sel, app->cwd);
	size_t total = sel ? sel->n : 0, hidden_items, filtered_items;
	bool busy;
	WfBuf part;

	wf_buf_init(&part);
	wf_buf_puts(&part, mode_label(app));
	if (wf_keystate_active(&app->keys)) {
		char typed[48];
		wf_buf_putc(&part, ' ');
		snprintf(typed, sizeof typed, "%u", (unsigned)app->keys.count);
		if (app->keys.count) {
			wf_buf_puts(&part, typed);
		}
		wf_seq_format(&app->keys.pending, typed, sizeof typed);
		wf_buf_puts(&part, typed);
	}
	wf_buf_putc(&part, ' ');
	wf_buf_puts(&part,
	        wf_msg(navigating(app) ? MSG_STATUS_LOADING
	                               : MSG_STATUS_READY));
	status_append_if_fit(s, &part, max_cells);

	wf_buf_clear(&part);
	if (mark) {
		wf_buf_puts(&part, filter_mark(app));
		status_append_if_fit(s, &part, max_cells);
	}

	wf_buf_clear(&part);
	wf_app_xfer_status(app, &part);
	status_append_if_fit(s, &part, max_cells);

	/* a running job owns the line: its progress, not the key hints */
	wf_buf_clear(&part);
	wf_app_copy_status(
	        app, &part, max_cells - wf_utf8_cells(wf_buf_cstr(s), s->len));
	busy = part.len > 0;
	status_append_if_fit(s, &part, max_cells);

	if (total) {
		char text[192];
		size_t vis = selected_visible(app, sel);
		size_t hidden = total > vis ? total - vis : 0;
		(void)wf_msgf(text, sizeof text, MSG_STATUS_SELECTED,
		        (WfMsgArg[]){WF_ARG_N(total), WF_ARG_N(vis),
		                WF_ARG_N(hidden)},
		        3);
		wf_buf_clear(&part);
		wf_buf_puts(&part, text);
		if (!status_append_if_fit(s, &part, max_cells)) {
			(void)wf_msgf(text, sizeof text,
			        MSG_STATUS_MARKED_SHORT, &WF_ARG_N(total), 1);
			wf_buf_clear(&part);
			wf_buf_puts(&part, text);
			status_append_if_fit(s, &part, max_cells);
		}
	}

	/* Discoverability precedes optional counts/history. The first hint (Help
	 * or Close) is retained even when the rest does not fit. */
	if (!busy &&
	        (&WF_MODE_NORMAL == app->mode ||
	                &WF_MODE_PREVIEW == app->mode ||
	                &WF_MODE_PREVIEW_PANE == app->mode)) {
		char hint[320];
		wf_app_binding_hint(
		        app, &WF_MODE_NORMAL != app->mode, hint, sizeof hint);
		wf_buf_clear(&part);
		wf_buf_puts(&part, " | ");
		wf_buf_puts(&part, hint);
		if (!status_append_if_fit(s, &part, max_cells)) {
			char *end = strstr(hint, "  ");
			if (end) {
				*end = '\0';
			}
			wf_buf_clear(&part);
			wf_buf_puts(&part, " | ");
			wf_buf_puts(&part, hint);
			status_append_if_fit(s, &part, max_cells);
		}
	}

	hidden_items = app->cur.dir && app->cur.dir->n >= app->cur.nvis
	        ? app->cur.dir->n - app->cur.nvis
	        : 0;
	/* what is out of view is two different things: hidden by attribute (the
	 * Hidden setting) and filtered out (the filter); they are counted apart */
	filtered_items = 0;
	if (hidden_items && app->cur.dir) {
		size_t attr = attr_hidden_count(app);

		filtered_items = app->cur.filter[0] && hidden_items > attr
		        ? hidden_items - attr
		        : 0;
		hidden_items -= filtered_items;
	}
	wf_buf_clear(&part);
	{
		char text[192];

		if (filtered_items) {
			(void)wf_msgf(text, sizeof text,
			        MSG_STATUS_ITEMS_HIDDEN_FILTERED,
			        (WfMsgArg[]){WF_ARG_N(app->cur.nvis),
			                WF_ARG_N(hidden_items),
			                WF_ARG_N(filtered_items)},
			        3);
		} else {
			(void)wf_msgf(text, sizeof text,
			        MSG_STATUS_ITEMS_HIDDEN,
			        (WfMsgArg[]){WF_ARG_N(app->cur.nvis),
			                WF_ARG_N(hidden_items)},
			        2);
		}
		wf_buf_puts(&part, text);

		if (app->cur.filter[0]) {
			WfBuf name;

			wf_buf_init(&name);
			wf_sanitize_utf16(app->cur.filter,
			        wcslen(app->cur.filter), &name);
			(void)wf_msgf(text, sizeof text, MSG_STATUS_FILTER,
			        &WF_ARG_S(wf_buf_cstr(&name)), 1);
			wf_buf_free(&name);
			wf_buf_puts(&part, text);
		}
	}
	if (app->cur.dir && app->cur.dir->truncated) {
		wf_buf_puts(&part, wf_msg(MSG_STATUS_TRUNCATED));
	}
	{
		/* the counts are added whether or not a filter is on; only whether
		 * the filter is told by them depends on it */
		bool fit = status_append_if_fit(s, &part, max_cells);

		*filter_told = app->cur.filter[0] && fit;
		if (fit) {
			filtered_items = 0; /* the short form is not needed */
		}
	}
	if (!*filter_told && filtered_items) {
		/* the longer count did not fit: the short one, with the filter,
		 * is better than neither */
		char text[192];

		wf_buf_clear(&part);
		(void)wf_msgf(text, sizeof text, MSG_STATUS_ITEMS_HIDDEN,
		        (WfMsgArg[]){WF_ARG_N(app->cur.nvis),
		                WF_ARG_N(hidden_items + filtered_items)},
		        2);
		wf_buf_puts(&part, text);
		if (app->cur.filter[0]) {
			WfBuf name;

			wf_buf_init(&name);
			wf_sanitize_utf16(app->cur.filter,
			        wcslen(app->cur.filter), &name);
			(void)wf_msgf(text, sizeof text, MSG_STATUS_FILTER,
			        &WF_ARG_S(wf_buf_cstr(&name)), 1);
			wf_buf_free(&name);
			wf_buf_puts(&part, text);
		}
		*filter_told = app->cur.filter[0] &&
		        status_append_if_fit(s, &part, max_cells);
	}

	if (&WF_MODE_NORMAL != app->mode && &WF_MODE_PREVIEW != app->mode &&
	        &WF_MODE_PREVIEW_PANE != app->mode) {
		wf_buf_clear(&part);
		if (status_hint(
		            app)[0]) { /* a mode with no hint leaves no bare bar */
			wf_buf_puts(&part, " | ");
			wf_buf_puts(&part, status_hint(app));
			status_append_if_fit(s, &part, max_cells);
		}
	}
	wf_buf_free(&part);
}

/* The status line. A filter that is on is told by its text when that fits; when
 * it does not, by the short mark, which then comes before the counts and the
 * hints: some of the files are out of sight, and that is not to be missed. */
static void status_text(const WfApp *app, WfBuf *s, int max_cells)
{
	bool told = false;

	status_build(app, s, max_cells, false, &told);
	if (!told && filter_mark(app)[0]) {
		wf_buf_clear(s);
		status_build(app, s, max_cells, true, &told);
	}
}

static void draw_status(const WfApp *app, WfFrame *f)
{
	WfBuf s;
	wf_frame_fill(f, 0, f->rows - 1, f->cols, 1, ' ', &wf_app_style_bar);
	if (app->msg[0]) {
		int used = wf_app_put_fit(f, 0, f->rows - 1, app->msg,
		        &wf_app_style_bar, f->cols);
		const char *more = wf_msg(MSG_STATUS_DETAILS_HINT);

		/* a failed job's detail is one key away; say so when there is room */
		if (app->jobs.n && app->jobs.log[app->jobs.n - 1].total &&
		        used + wf_utf8_cells(more, strlen(more)) <= f->cols) {
			wf_app_put_fit(f, used, f->rows - 1, more,
			        &wf_app_style_bar, f->cols - used);
		}
		return;
	}

	wf_buf_init(&s);
	status_text(app, &s, f->cols);
	wf_frame_put(f, 0, f->rows - 1, wf_buf_cstr(&s), s.len,
	        &wf_app_style_bar, f->cols);
	wf_buf_free(&s);
}

static void draw_minimal_status(const WfApp *app, WfFrame *f)
{
	WfBuf xfer;

	if (f->rows < 2) {
		return;
	}
	const char *state =
	        wf_msg(navigating(app) ? MSG_STATUS_LOADING : MSG_STATUS_READY);
	int state_cells = wf_utf8_cells(state, strlen(state));

	wf_frame_fill(f, 0, f->rows - 1, f->cols, 1, ' ', &wf_app_style_bar);
	if (app->msg[0]) {
		wf_app_put_fit(f, 0, f->rows - 1, app->msg, &wf_app_style_bar,
		        f->cols);
		return;
	}

	wf_app_put_str(f, 0, f->rows - 1, state, &wf_app_style_bar, f->cols);
	wf_buf_init(&xfer);
	wf_buf_puts(&xfer, filter_mark(app));
	wf_app_xfer_status(app, &xfer);
	wf_app_copy_status(app, &xfer, f->cols - state_cells);
	if (xfer.len && state_cells < f->cols) {
		wf_app_put_str(f, state_cells, f->rows - 1, wf_buf_cstr(&xfer),
		        &wf_app_style_bar, f->cols - state_cells);
	}
	wf_buf_free(&xfer);
}

void wf_app_draw(const WfApp *app, WfFrame *f)
{
	const char *titles[WF_PANE_COUNT] = {wf_msg(MSG_PANE_PARENT),
	        wf_msg(MSG_PANE_CURRENT), wf_msg(MSG_PANE_PREVIEW)};
	WfLayout lay;
	int i;

	wf_frame_fill(f, 0, 0, f->cols, f->rows, ' ', &WF_STYLE_BASE);
	wf_layout_compute(f->cols, f->rows, &app->ratios, &lay);
	if (lay.minimal) {
		const char *badge =
		        app->recovery.unknown || app->recovery.unresolved
		        ? wf_msg(MSG_SAFETY_REVIEW)
		        : "";
		int badge_w = wf_utf8_cells(badge, strlen(badge));
		wf_app_put_fit(f, 0, 0, wf_msg(MSG_SCREEN_TOO_SMALL),
		        &WF_STYLE_BASE,
		        badge_w ? f->cols - badge_w - 1 : f->cols);
		if (badge_w) {
			int x = f->cols > badge_w ? f->cols - badge_w : 0;
			wf_app_put_fit(
			        f, x, 0, badge, &wf_app_style_bar, f->cols - x);
		}
		draw_minimal_status(app, f);
		if (app->mode->draw) {
			app->mode->draw(
			        app, f); /* a modal must still be answerable */
		}
		return;
	}

	draw_title(app, f);
	wf_frame_fill(f, 0, 1, f->cols, 1, '-', &style_dim);

	for (i = 0; i < WF_PANE_COUNT; i++) {
		if (0 == lay.pane[i].w) {
			continue;
		}
		if (WF_PANE_PREVIEW == i &&
		        &WF_MODE_PREVIEW_PANE == app->mode) {
			wf_app_put_fit(f, lay.pane[i].x, 2,
			        wf_msg(MSG_PANE_PREVIEW_READONLY),
			        &wf_app_style_bar, lay.pane[i].w);
		} else {
			wf_app_put_fit(f, lay.pane[i].x, 2, titles[i],
			        (WF_PANE_CURRENT == i &&
			                &WF_MODE_PREVIEW_PANE != app->mode)
			                ? &wf_theme_styles()->accent
			                : &style_dim,
			        lay.pane[i].w);
		}
		if (lay.pane[i].x > 0) {
			wf_app_vrule(f, lay.pane[i].x - 1, 2, lay.body_h + 1);
		}
	}
	if (lay.pane[WF_PANE_PARENT].w > 0 && app->parent.dir) {
		draw_list(f, lay.pane[WF_PANE_PARENT].x, lay.body_y,
		        lay.pane[WF_PANE_PARENT].w, lay.body_h, &app->parent,
		        false, true, false, NULL, NULL, NULL, NULL);
	}
	if (lay.pane[WF_PANE_CURRENT].w > 0 && app->cur.dir) {
		draw_list(f, lay.pane[WF_PANE_CURRENT].x, lay.body_y,
		        lay.pane[WF_PANE_CURRENT].w, lay.body_h, &app->cur,
		        true, true, &WF_MODE_PREVIEW_PANE != app->mode,
		        wf_sel_folder(&app->sel, app->cwd), &app->dirsize,
		        app->cwd, app);
		draw_list_empty(app, f, lay.pane[WF_PANE_CURRENT].x, lay.body_y,
		        lay.pane[WF_PANE_CURRENT].w);
	}
	/* The fullscreen view draws its own preview. Laying out the covered pane
	 * first would replace the Markdown cache at a different width each frame. */
	if (&WF_MODE_PREVIEW != app->mode) {
		wf_app_draw_preview(app, f, lay.pane[WF_PANE_PREVIEW].x,
		        lay.body_y, lay.pane[WF_PANE_PREVIEW].w, lay.body_h);
	}

	wf_frame_fill(f, 0, f->rows - 2, f->cols, 1, '-', &style_dim);
	draw_status(app, f);

	if (app->mode->draw) {
		app->mode->draw(app, f);
	}
}
