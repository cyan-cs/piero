/*
 * The help (?): made from the command table and the keys that are really
 * bound, so it is never out of date. One category at a time (Left/Right
 * between them), the selected action explained in full below the list, and
 * typing narrows every category at once by key, action name or description.
 * It opens at the category of the screen it was asked from, and lays itself
 * out for a narrow screen.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "../base/wf_macro.h"
#include "../text/wf_text.h"
#include "wf_app_priv.h"

#define HELP_KEY_CELLS 26
#define HELP_NARROW_COLS 56 /* below this the keys and the text share a line */
#define HELP_CATS 4
#define HELP_ITEMS_MAX (HELP_CATS * 160)

typedef struct
{
	const WfCommandInfo *info;
	WfKeyContext ctx;
	char keys[96];
	int rank; /* a search: 0 a key is exactly this, 1 starts with it, 2 holds it, 3 other */
} HelpItem;

/* The categories of this profile: the file manager, the preview, and the
 * editor (one for the standard and nano keys, two for Vim's modes). */
static size_t help_cats(const WfApp *app, WfKeyContext out[HELP_CATS])
{
	size_t n = 0;

	out[n++] = WF_CTX_FM;
	out[n++] = WF_CTX_PREVIEW;
	if (wf_keymap_modal(app->keymap)) {
		out[n++] = WF_CTX_VIM_NORMAL;
		out[n++] = WF_CTX_VIM_INSERT;
	} else {
		out[n++] = WF_CTX_EDIT;
	}
	return n;
}

/* ASCII case-blind search; other bytes (Japanese, ...) match exactly. The
 * first match in `hay`, or NULL. */
static const char *find_text(const char *hay, const char *needle)
{
	size_t n = strlen(needle);

	if (0 == n) {
		return hay;
	}
	for (; *hay; hay++) {
		size_t i = 0;

		while (i < n && hay[i] &&
		        tolower((unsigned char)hay[i]) ==
		                tolower((unsigned char)needle[i])) {
			i++;
		}
		if (i == n) {
			return hay;
		}
	}
	return NULL;
}

static bool has_text(const char *hay, const char *needle)
{
	return NULL != find_text(hay, needle);
}

static bool item_matches(const HelpItem *it, const char *q)
{
	return !q[0] || has_text(it->keys, q) ||
	        has_text(wf_msg(it->info->desc), q) ||
	        has_text(it->info->name, q);
}

/* How well the item answers a search: one of its keys is the text itself (as
 * "g" for the key g), one begins with it, a key holds it, or only the name or
 * the words do. The keys are what a person looks for, so they come first. */
static int item_rank(const HelpItem *it, const char *q)
{
	const char *k = it->keys;
	size_t n = strlen(q);
	int best = has_text(it->keys, q) ? 2 : 3;

	if (!q[0]) {
		return 0;
	}
	while (*k) {
		size_t len = strcspn(k, ",");
		const char *next = k + len + (',' == k[len]);

		while (' ' == *k && len) {
			k++;
			len--;
		}
		if (len >= n && find_text(k, q) == k) {
			if (len == n) {
				return 0;
			}
			best = 1;
		}
		k = next;
	}
	return best;
}

/* The rows of the list: the current category, or with a search every
 * category's matches, best answers first. Static: the UI thread is the only
 * one here. */
static HelpItem g_items[HELP_ITEMS_MAX];
static HelpItem g_sorted[HELP_ITEMS_MAX];

static size_t help_collect(const WfApp *app)
{
	WfKeyContext cats[HELP_CATS];
	size_t nc = help_cats(app, cats), n = 0;

	for (size_t c = 0; c < nc; c++) {
		const WfCommandInfo *list[160];
		size_t k;

		if (!app->help.q[0] && (int)c != app->help.cat) {
			continue;
		}
		k = wf_cmd_list(cats[c], list, 160);
		for (size_t i = 0; i < k && n < HELP_ITEMS_MAX; i++) {
			HelpItem *it = &g_items[n];

			wf_keymap_keys_text(app->keymap, cats[c], list[i]->id,
			        it->keys, sizeof it->keys);
			if (!it->keys[0]) {
				continue;
			}
			it->info = list[i];
			it->ctx = cats[c];
			if (item_matches(it, app->help.q)) {
				it->rank = item_rank(it, app->help.q);
				n++;
			}
		}
	}
	if (app->help.q[0] && n > 1) { /* stable: by rank, else as listed */
		size_t k = 0;

		for (int r = 0; r < 4; r++) {
			for (size_t i = 0; i < n; i++) {
				if (g_items[i].rank == r) {
					g_sorted[k++] = g_items[i];
				}
			}
		}
		memcpy(g_items, g_sorted, n * sizeof g_items[0]);
	}
	return n;
}

static const char *profile_name(const WfApp *app, char *buf, size_t cap)
{
	WfProfile p = wf_keymap_profile(app->keymap);

	switch (p) {
	case WF_PROFILE_NANO: return wf_msg(MSG_PROFILE_NANO);
	case WF_PROFILE_VIM:  return wf_msg(MSG_PROFILE_VIM);
	case WF_PROFILE_CUSTOM:
		(void)wf_msgf(buf, cap, MSG_PROFILE_CUSTOM_BASED,
		        &WF_ARG_S(WF_BASE_VIM == wf_keymap_base(app->keymap)
		                        ? wf_msg(MSG_PROFILE_VIM)
		                        : wf_msg(MSG_PROFILE_NANO)),
		        1);
		return buf;
	case WF_PROFILE_LEGACY: break;
	}
	return wf_msg(MSG_PROFILE_STANDARD);
}

/* Rows for the list: the screen less the title, the search box, the tabs,
 * the rule, two lines of explanation and the footer. A very small screen drops
 * the explanation. */
static int list_rows(int rows)
{
	if (rows >= 10) {
		return rows - 8;
	}
	return rows > 5 ? rows - 5 : 1;
}

static void keep_in_view(WfApp *app, size_t n)
{
	size_t rows = (size_t)list_rows(app->rows);

	if (n && app->help.sel >= n) {
		app->help.sel = n - 1;
	}
	if (app->help.sel < app->help.top) {
		app->help.top = app->help.sel;
	}
	if (app->help.sel >= app->help.top + rows) {
		app->help.top = app->help.sel + 1 - rows;
	}
	if (0 == n) {
		app->help.top = app->help.sel = 0;
	}
}

/* `s` over the rows from y, wrapped at `width`, at most `max` rows. */
static void put_wrapped(WfFrame *f,
        int x,
        int y,
        int max,
        const char *s,
        int width,
        const WfStyle *st)
{
	size_t len = strlen(s), at = 0;

	for (int r = 0; r < max && at < len; r++) {
		char piece[256];
		size_t take = wf_utf8_prefix(s + at, len - at, (size_t)width);

		if (0 == take) {
			uint32_t cp;

			take = wf_utf8_decode(s + at, len - at, &cp);
			if (0 == take) {
				take = 1;
			}
		}
		if (take >= sizeof piece) {
			take = sizeof piece - 1;
		}
		memcpy(piece, s + at, take);
		piece[take] = '\0';
		wf_app_put_fit(f, x, y + r, piece, st, width);
		at += take;
	}
}

static void draw_tabs(const WfApp *app, WfFrame *f)
{
	WfKeyContext cats[HELP_CATS];
	size_t nc = help_cats(app, cats);
	int x = 1;

	wf_frame_fill(f, 0, 2, f->cols, 1, ' ', &WF_STYLE_BASE);
	for (size_t c = 0; c < nc; c++) {
		const char *label = wf_msg(wf_context_label(cats[c]));
		char tab[96];
		bool on = (int)c == app->help.cat && !app->help.q[0];
		int w;

		snprintf(tab, sizeof tab, on ? "[%s]" : " %s ", label);
		w = wf_utf8_cells(tab, strlen(tab));
		if (x + w > f->cols) {
			break;
		}
		wf_app_put_str(f, x, 2, tab,
		        on ? &wf_theme_styles()->accent : &WF_STYLE_BASE, w);
		x += w + 1;
	}
}

/* The part of `text` that the search found, in the colour of a match. `x` is
 * where the text starts, `w` the room it has. */
static void mark_match(
        WfFrame *f, int x, int y, int w, const char *text, const char *q)
{
	const char *at = q[0] ? find_text(text, q) : NULL;
	int before;
	size_t len = strlen(q);

	if (!at) {
		return;
	}
	before = wf_utf8_cells(text, (size_t)(at - text));
	if (before >= w) {
		return;
	}
	wf_frame_put(f, x + before, y, at, len, &wf_theme_styles()->match,
	        w - before);
}

/* The box the typing goes to: always there, so that a search is plainly the
 * way to find a key, with how many answers there are beside it. */
static void draw_search(const WfApp *app, WfFrame *f, size_t n)
{
	char line[160], found[48] = "";
	const WfThemeStyles *t = wf_theme_styles();
	int used, fw;

	wf_frame_fill(f, 0, 1, f->cols, 1, ' ', &WF_STYLE_BASE);
	if (app->help.q[0]) {
		(void)wf_msgf(line, sizeof line, MSG_HELP_SEARCH_BOX,
		        &WF_ARG_S(app->help.q), 1);
		used = wf_app_put_fit(f, 1, 1, line, &t->accent, f->cols - 2);
		wf_frame_set_cursor(f, 1 + used, 1);
		(void)wf_msgf(
		        found, sizeof found, MSG_HELP_FOUND, &WF_ARG_N(n), 1);
		fw = wf_utf8_cells(found, strlen(found));
		if (1 + used + 2 + fw < f->cols) {
			wf_app_put_fit(
			        f, f->cols - 1 - fw, 1, found, &t->dim, fw);
		}
	} else {
		(void)wf_app_put_fit(f, 1, 1, wf_msg(MSG_HELP_SEARCH_EMPTY),
		        &t->dim, f->cols - 2);
	}
}

static void help_draw(const WfApp *app, WfFrame *f)
{
	size_t n = help_collect(app);
	int rows = list_rows(f->rows), narrow = f->cols < HELP_NARROW_COLS;
	char name[96], title[160];

	if (f->rows < 4) {
		return;
	}
	wf_frame_fill(f, 0, 0, f->cols, f->rows - 1, ' ', &WF_STYLE_BASE);
	(void)wf_msgf(title, sizeof title, MSG_HELP_PROFILE_LINE,
	        &WF_ARG_S(profile_name(app, name, sizeof name)), 1);
	{
		char whole[320];

		snprintf(whole, sizeof whole, "%s  %s",
		        wf_msg(MSG_HELP_SCREEN_TITLE), title);
		wf_app_put_fit(f, 0, 0, whole, &wf_app_style_bar, f->cols);
	}
	draw_search(app, f, n);
	draw_tabs(app, f);
	for (int r = 0; r < rows; r++) {
		size_t i = app->help.top + (size_t)r;
		const HelpItem *it;
		bool on;
		const WfStyle *st;
		int y = 3 + r;

		if (i >= n) {
			break;
		}
		it = &g_items[i];
		on = i == app->help.sel;
		st = on ? &wf_theme_styles()->selection : &WF_STYLE_BASE;
		wf_frame_fill(f, 0, y, f->cols, 1, ' ', st);
		wf_app_put_str(f, 0, y, on ? ">" : " ", st, 1);
		if (narrow) {
			char one[256];

			snprintf(one, sizeof one, "%s  %s", it->keys,
			        wf_msg(it->info->desc));
			wf_app_put_fit(f, 2, y, one, st, f->cols - 3);
			mark_match(f, 2, y, f->cols - 3, one, app->help.q);
		} else {
			/* the keys stand out as a column of their own */
			WfStyle keys = *st;
			const char *desc = wf_msg(it->info->desc);
			int dw = f->cols - 3 - HELP_KEY_CELLS;

			if (!on) {
				keys = wf_theme_styles()->accent;
			}
			wf_app_put_fit(
			        f, 2, y, it->keys, &keys, HELP_KEY_CELLS);
			mark_match(f, 2, y, HELP_KEY_CELLS - 1, it->keys,
			        app->help.q);
			wf_app_put_fit(f, 2 + HELP_KEY_CELLS, y, desc, st, dw);
			mark_match(f, 2 + HELP_KEY_CELLS, y, dw, desc,
			        app->help.q);
			if (app->help.q[0]) {
				/* a search crosses the categories: say which one */
				const char *cat =
				        wf_msg(wf_context_label(it->ctx));
				int cw = wf_utf8_cells(cat, strlen(cat));
				int dcells = wf_utf8_cells(desc, strlen(desc));

				if (2 + HELP_KEY_CELLS + dcells + 2 + cw <
				        f->cols) {
					wf_app_put_fit(f, f->cols - 1 - cw, y,
					        cat,
					        on ? st
					           : &wf_theme_styles()->dim,
					        cw);
				}
			}
		}
	}
	if (0 == n) {
		wf_app_put_fit(f, 2, 3, wf_msg(MSG_HELP_NO_MATCH),
		        &WF_STYLE_BASE, f->cols - 3);
	}
	if (f->rows >= 10) {
		/* the explanation: what the selected action is, in full, and
		 * where it lives (its name is what a search can find too) */
		wf_frame_fill(f, 0, f->rows - 5, f->cols, 1, '-',
		        &wf_theme_styles()->dim);
		if (n && app->help.sel < n) {
			const HelpItem *it = &g_items[app->help.sel];
			char text[512];

			snprintf(text, sizeof text, "%s: %s  (%s, %s)",
			        it->keys, wf_msg(it->info->desc),
			        wf_msg(wf_context_label(it->ctx)),
			        it->info->name);
			put_wrapped(f, 1, f->rows - 4, 2, text, f->cols - 2,
			        &WF_STYLE_BASE);
		}
	}
	wf_frame_fill(f, 0, f->rows - 2, f->cols, 1, ' ', &wf_app_style_bar);
	wf_app_put_fit(f, 0, f->rows - 2,
	        wf_msg(app->help.q[0] ? MSG_HELP_SEARCH : MSG_HELP_SCROLL),
	        &wf_app_style_bar, f->cols);
}

void wf_app_help_begin(WfApp *app)
{
	WfKeyContext cats[HELP_CATS];
	size_t nc = help_cats(app, cats);

	app->help.top = 0;
	app->help.sel = 0;
	app->help.cat = 0;
	app->help.q[0] = '\0';
	app->help.back = NULL;
	/* it opens at the keys of the screen it was asked from, and closes back to
	 * that screen: a person who looked a key up goes on from where they were */
	if (&WF_MODE_PREVIEW == app->mode ||
	        &WF_MODE_PREVIEW_PANE == app->mode) {
		app->help.back = app->mode;
		for (size_t c = 0; c < nc; c++) {
			if (WF_CTX_PREVIEW == cats[c]) {
				app->help.cat = (int)c;
			}
		}
	}
	app->mode = &WF_MODE_HELP;
}

static void search_changed(WfApp *app)
{
	app->help.top = 0;
	app->help.sel = 0;
}

static WfAction help_event(WfApp *app, const WfEvent *ev)
{
	size_t n = help_collect(app);
	size_t page = (size_t)list_rows(app->rows);
	WfKeyContext cats[HELP_CATS];
	int nc = (int)help_cats(app, cats);

	if (WF_EV_RESIZE == ev->type) {
		keep_in_view(app, n);
		return WF_ACT_REDRAW;
	}
	if (WF_EV_RELEASE == ev->type) {
		return WF_ACT_NONE;
	}
	if (WF_EV_CHAR == ev->type && ev->ch >= 0x20 && 0x7F != ev->ch &&
	        (ev->ch < 0x80 || ev->ch > 0x9F)) {
		char u[4];
		size_t k = wf_utf8_encode(ev->ch, u), len = strlen(app->help.q);

		if (len + k < sizeof app->help.q) {
			memcpy(app->help.q + len, u, k);
			app->help.q[len + k] = '\0';
			search_changed(app);
		}
		return WF_ACT_REDRAW;
	}
	if (WF_EV_KEY == ev->type) {
		switch (ev->key) {
		case WF_KEY_BACKSPACE: {
			size_t len = strlen(app->help.q);

			while (len && 0x80 == (app->help.q[len - 1] & 0xC0)) {
				len--;
			}
			if (len) {
				app->help.q[len - 1] = '\0';
			}
			search_changed(app);
			return WF_ACT_REDRAW;
		}
		case WF_KEY_ESC:
			if (app->help.q[0]) { /* the first Esc clears the search */
				app->help.q[0] = '\0';
				search_changed(app);
				return WF_ACT_REDRAW;
			}
			break;
		case WF_KEY_LEFT:
		case WF_KEY_RIGHT:
		case WF_KEY_TAB:
			if (!app->help.q[0]) {
				app->help.cat = WF_KEY_LEFT == ev->key
				        ? (app->help.cat + nc - 1) % nc
				        : (app->help.cat + 1) % nc;
				search_changed(app);
			}
			return WF_ACT_REDRAW;
		case WF_KEY_DOWN:
			if (app->help.sel + 1 < n) {
				app->help.sel++;
			}
			keep_in_view(app, n);
			return WF_ACT_REDRAW;
		case WF_KEY_UP:
			if (app->help.sel) {
				app->help.sel--;
			}
			keep_in_view(app, n);
			return WF_ACT_REDRAW;
		case WF_KEY_PGDN:
			app->help.sel = app->help.sel + page < n
			        ? app->help.sel + page
			        : (n ? n - 1 : 0);
			keep_in_view(app, n);
			return WF_ACT_REDRAW;
		case WF_KEY_PGUP:
			app->help.sel =
			        app->help.sel > page ? app->help.sel - page : 0;
			keep_in_view(app, n);
			return WF_ACT_REDRAW;
		case WF_KEY_HOME:
			app->help.sel = 0;
			keep_in_view(app, n);
			return WF_ACT_REDRAW;
		case WF_KEY_END:
			app->help.sel = n ? n - 1 : 0;
			keep_in_view(app, n);
			return WF_ACT_REDRAW;
		default: break;
		}
	}
	app->mode = app->help.back ? app->help.back : &WF_MODE_NORMAL;
	app->help.back = NULL;
	return WF_ACT_REDRAW;
}

const WfMode WF_MODE_HELP = {"HELP", help_draw, help_event};
