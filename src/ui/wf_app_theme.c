/*
 * The theme screen: the first thing a new user sees, and the chooser the
 * settings screen opens. A large "piero" in plain ASCII, the eight presets in
 * two columns, and the colours of whichever is under the cursor applied to the
 * whole screen at once. Nothing is written until Enter.
 */
#include <stdio.h>
#include <string.h>

#include "../text/wf_text.h"
#include "wf_app_priv.h"
#include "../base/wf_macro.h"

/* Static data: the logo is never built at draw time. */
static const char *const logo[] = {
        "       _                 ",
        " _ __ (_) ___ _ __ ___   ",
        "| '_ \\| |/ _ \\ '__/ _ \\  ",
        "| |_) | |  __/ | | (_) | ",
        "| .__/|_|\\___|_|  \\___/  ",
        "|_|                       ",
};
#define LOGO_ROWS ((int)(WF_ARRAY_LEN(logo)))

#define WF_FIRST_RUN_ENTER_GUARD_MS 700u

static void language_preview(WfApp *app);

#define GRID_COLS 2
#define GRID_ROWS (WF_THEME_PRESET_COUNT / GRID_COLS)
#define CELL_W 12

static void put_centered(WfFrame *f, int y, const char *s, const WfStyle *st)
{
	int w = wf_utf8_cells(s, strlen(s));
	int x = (f->cols - w) / 2;

	if (y < 0 || y >= f->rows) {
		return;
	}
	wf_app_put_str(f, x < 0 ? 0 : x, y, s, st, f->cols);
}

/* The cell text: "> Spade" padded to the cell, or "  Spade". */
static void cell_text(size_t index, bool selected, char out[CELL_W + 1])
{
	snprintf(out, CELL_W + 1, "%c %-*s", selected ? '>' : ' ', CELL_W - 2,
	        wf_theme_display_name((WfThemeId)index));
}

static void draw_cell(WfFrame *f, int x, int y, size_t index, bool selected)
{
	const WfThemeStyles *th = wf_theme_styles();
	char text[CELL_W + 1];

	cell_text(index, selected, text);
	wf_app_put_str(
	        f, x, y, text, selected ? &th->selection : &th->base, CELL_W);
}

/*
 * Rows needed at each size. Full: logo, subtitle, heading, the grid and the
 * key hints with air between them. Medium drops the air. Small trades the big
 * logo for one line of text. Smallest shows only the chooser, in one column
 * that scrolls with the cursor.
 */
typedef enum
{
	LAYOUT_FULL,
	LAYOUT_MEDIUM,
	LAYOUT_SMALL,
	LAYOUT_TINY
} Layout;

static Layout layout_for(int cols, int rows)
{
	if (cols >= 30 && rows >= 21) {
		return LAYOUT_FULL;
	}
	if (cols >= 30 && rows >= 15) {
		return LAYOUT_MEDIUM;
	}
	if (cols >= 24 && rows >= 9) {
		return LAYOUT_SMALL;
	}
	return LAYOUT_TINY;
}

/*
 * The language screen: every language in its own words, "Auto" (the language
 * of this Windows) first and chosen to begin with. Moving shows the screen in
 * that language at once. The list is the languages table, so it grows by
 * itself.
 */
static void language_draw(const WfApp *app, WfFrame *f)
{
	const WfThemeStyles *th = wf_theme_styles();
	int rows_needed = WF_LANGSET_COUNT, y, i, first = 0, shown;
	bool logo_fits =
	        f->cols >= 30 && f->rows >= LOGO_ROWS + 2 + 3 + rows_needed + 3;
	bool heading_fits = f->rows >= rows_needed + 4;
	int sel = app->picker.lang;

	wf_frame_fill(f, 0, 0, f->cols, f->rows, ' ', &th->base);
	{
		int total = rows_needed + (heading_fits ? 4 : 0) +
		        (logo_fits ? LOGO_ROWS + 2 : 0);

		y = (f->rows - total) / 2;
		if (y < 0) {
			y = 0;
		}
	}
	if (logo_fits) {
		int w = 0, x;

		for (i = 0; i < LOGO_ROWS; i++) {
			if ((int)strlen(logo[i]) > w) {
				w = (int)strlen(logo[i]);
			}
		}
		x = (f->cols - w) / 2;
		if (x < 0) {
			x = 0;
		}
		for (i = 0; i < LOGO_ROWS; i++) {
			wf_app_put_str(
			        f, x, y + i, logo[i], &th->accent, f->cols - x);
		}
		y += LOGO_ROWS + 2;
	}
	if (heading_fits) {
		put_centered(f, y, wf_msg(MSG_LANG_CHOOSE), &th->base);
		y += 2;
	}
	/* a window of the list that always holds the chosen row */
	shown = f->rows - y - (heading_fits ? 2 : 0);
	if (shown < 1) {
		shown = 1;
	}
	if (shown > rows_needed) {
		shown = rows_needed;
	}
	if (sel >= first + shown) {
		first = sel - shown + 1;
	}
	for (i = 0; i < shown && y + i < f->rows; i++) {
		char text[96], line[120];
		bool on = first + i == sel;
		int w;

		wf_app_language_text(app, first + i, text, sizeof text);
		snprintf(line, sizeof line, "%c %s", on ? '>' : ' ', text);
		w = wf_utf8_cells(line, strlen(line));
		wf_app_put_fit(f, (f->cols - (w > 36 ? 36 : w)) / 2, y + i,
		        line, on ? &th->selection : &th->base,
		        f->cols < 36 ? f->cols : 36);
	}
	if (heading_fits) {
		put_centered(f, y + shown + 1, wf_msg(MSG_LANG_KEYS), &th->dim);
	}
}

/* The keys screen: Nano-style or Vim-style. */
static void keys_draw(const WfApp *app, WfFrame *f)
{
	const WfThemeStyles *th = wf_theme_styles();
	static const WfMsg names[2] = {MSG_KEYS_OPT_NANO, MSG_KEYS_OPT_VIM};
	bool logo_fits = f->cols >= 30 && f->rows >= LOGO_ROWS + 2 + 3 + 2 + 3;
	bool heading_fits = f->rows >= 6;
	int y, i,
	        total = 2 + (heading_fits ? 4 : 0) +
	        (logo_fits ? LOGO_ROWS + 2 : 0);

	wf_frame_fill(f, 0, 0, f->cols, f->rows, ' ', &th->base);
	y = (f->rows - total) / 2;
	if (y < 0) {
		y = 0;
	}
	if (logo_fits) {
		int w = 0, x;

		for (i = 0; i < LOGO_ROWS; i++) {
			if ((int)strlen(logo[i]) > w) {
				w = (int)strlen(logo[i]);
			}
		}
		x = (f->cols - w) / 2;
		if (x < 0) {
			x = 0;
		}
		for (i = 0; i < LOGO_ROWS; i++) {
			wf_app_put_str(
			        f, x, y + i, logo[i], &th->accent, f->cols - x);
		}
		y += LOGO_ROWS + 2;
	}
	if (heading_fits) {
		put_centered(f, y, wf_msg(MSG_KEYS_CHOOSE), &th->base);
		y += 2;
	}
	for (i = 0; i < 2 && y + i < f->rows; i++) {
		char line[160];
		bool on = i == app->picker.keys_sel;
		int w;

		snprintf(line, sizeof line, "%c %s", on ? '>' : ' ',
		        wf_msg(names[i]));
		w = wf_utf8_cells(line, strlen(line));
		wf_app_put_fit(f, (f->cols - (w > f->cols ? f->cols : w)) / 2,
		        y + i, line, on ? &th->selection : &th->base, f->cols);
	}
	if (heading_fits) {
		put_centered(f, y + 3, wf_msg(MSG_KEYS_PICK_HINT), &th->dim);
	}
}

static void picker_draw(const WfApp *app, WfFrame *f)
{
	const WfThemeStyles *th = wf_theme_styles();
	Layout layout = layout_for(f->cols, f->rows);
	const char *heading = wf_msg(MSG_THEME_CHOOSE);
	size_t sel = app->picker.index;
	int y, i, grid_w, gx;

	if (WF_PICKER_LANGUAGE == app->picker.stage) {
		language_draw(app, f);
		return;
	}
	if (WF_PICKER_KEYS == app->picker.stage) {
		keys_draw(app, f);
		return;
	}
	wf_frame_fill(f, 0, 0, f->cols, f->rows, ' ', &th->base);
	if (LAYOUT_TINY == layout) {
		/* one column, windowed around the cursor */
		int first = (int)sel - f->rows / 2;

		if (first < 0) {
			first = 0;
		}
		if (first + f->rows > WF_THEME_PRESET_COUNT) {
			first = WF_THEME_PRESET_COUNT - f->rows;
		}
		if (first < 0) {
			first = 0;
		}
		for (i = 0; i < f->rows && first + i < WF_THEME_PRESET_COUNT;
		        i++) {
			draw_cell(f, 0, i, (size_t)(first + i),
			        (size_t)(first + i) == sel);
		}
		return;
	}
	/* total height of what is shown */
	{
		int total = GRID_ROWS;

		if (LAYOUT_FULL == layout) {
			total += LOGO_ROWS + 2 + 1 + 2 + 1 + 1 + 2 + 2;
		} else if (LAYOUT_MEDIUM == layout) {
			total += LOGO_ROWS + 1 + 1 + 1 + 1;
		} else {
			total += 1 + 1 + 1;
		}
		y = (f->rows - total) / 2;
		if (y < 0) {
			y = 0;
		}
	}
	if (LAYOUT_SMALL == layout) {
		put_centered(f, y++, "piero", &th->accent);
		put_centered(f, y++, heading, &th->base);
	} else {
		int w = 0, x;

		for (i = 0; i < LOGO_ROWS; i++) {
			int n = (int)strlen(logo[i]);

			if (n > w) {
				w = n;
			}
		}
		x = (f->cols - w) / 2;
		if (x < 0) {
			x = 0;
		}
		for (i = 0; i < LOGO_ROWS; i++) {
			wf_app_put_str(
			        f, x, y + i, logo[i], &th->accent, f->cols - x);
		}
		y += LOGO_ROWS + (LAYOUT_FULL == layout ? 2 : 1);
		put_centered(f, y, wf_msg(MSG_THEME_SUBTITLE), &th->dim);
		y += layout == LAYOUT_FULL ? 3 : 2;
		put_centered(f, y++, heading, &th->base);
		if (LAYOUT_FULL == layout) {
			y++;
		}
	}
	grid_w = GRID_COLS * CELL_W + 4;
	gx = (f->cols - grid_w) / 2;
	if (gx < 0) {
		gx = 0;
	}
	for (i = 0; i < WF_THEME_PRESET_COUNT; i++) {
		draw_cell(f, gx + (i % GRID_COLS) * (CELL_W + 4),
		        y + i / GRID_COLS, (size_t)i, (size_t)i == sel);
	}
	y += GRID_ROWS;
	if (LAYOUT_SMALL == layout) {
		return;
	}
	{
		const char *esc =
		        wf_msg(app->picker.onboarding ? MSG_THEME_KEY_DEFAULT
		                                      : MSG_THEME_KEY_KEEP);

		if (LAYOUT_FULL == layout) {
			put_centered(f, y + 2, wf_msg(MSG_THEME_KEY_SELECT),
			        &th->base);
			put_centered(f, y + 3, esc, &th->dim);
		} else {
			put_centered(f, y,
			        wf_msg(app->picker.onboarding
			                        ? MSG_THEME_ROW_DEFAULT
			                        : MSG_THEME_ROW_KEEP),
			        &th->base);
		}
	}
}

static void preview(const WfApp *app)
{
	wf_theme_activate(
	        &wf_theme_preset((WfThemeId)app->picker.index)->palette);
}

void wf_app_setup_begin(WfApp *app, bool onboarding)
{
	/* a first run starts at the default; running the setup again, or
	 * opening the chooser, starts at the theme in force */
	WfThemeId current = !app->config ? WF_THEME_DEFAULT
	        : onboarding && !app->config->onboarding_completed
	        ? WF_THEME_DEFAULT
	        : onboarding ? (WfThemeId)app->config->theme
	                     : (WfThemeId)app->settings.draft.theme;

	app->picker.onboarding = onboarding;
	/* a first run (or --setup) asks the language first; the chooser that
	 * settings opens is only about the theme */
	app->picker.stage = onboarding ? WF_PICKER_LANGUAGE : WF_PICKER_THEME;
	app->picker.lang =
	        app->config ? app->config->lang_setting : WF_LANGSET_AUTO;
	app->picker.lang_before = app->picker.lang;
	app->picker.keys = -1;
	app->picker.keys_sel =
	        app->config && WF_PROFILE_VIM == app->config->keys.profile ? 1
	                                                                   : 0;
	app->picker.before = current;
	app->picker.index =
	        (unsigned)current < WF_THEME_PRESET_COUNT ? (size_t)current : 0;
	app->mode = &WF_MODE_SETUP;
	preview(app);
	if (onboarding) {
		language_preview(app);
	}
}

/* What was in force before the screen opened. */
static void restore(WfApp *app)
{
	WfThemePalette p;

	if (app->picker.onboarding || !app->config) {
		wf_theme_activate(&wf_theme_preset(WF_THEME_DEFAULT)->palette);
		return;
	}
	{
		WfCustomTheme custom;
		const WfCustomEntry *e =
		        wf_config_custom_selected(&app->settings.draft);

		if (e) {
			custom = e->theme;
		} else {
			wf_custom_init(&custom);
		}
		wf_theme_palette_of(app->picker.before, &custom, &p);
	}
	wf_theme_activate(&p);
}

void wf_app_set_detected_language(WfApp *app, WfUiLang lang)
{
	app->lang_detected = lang;
}

/* The language screen follows the choice at once. */
static void language_preview(WfApp *app)
{
	wf_i18n_set_language(wf_i18n_resolve(
	        (WfLangSetting)app->picker.lang, app->lang_detected));
	app->msg[0] = '\0';
}

static WfAction picker_finish(WfApp *app, WfThemeId id)
{
	if (app->picker.onboarding) {
		wf_app_onboarding_done(app, id);
		/* someone pressing Enter through the screens keeps pressing it:
		 * the first moments of the file lists ignore it (and anything
		 * already typed ahead is dropped) */
		app->enter_guard_until =
		        app->now_ms + WF_FIRST_RUN_ENTER_GUARD_MS;
		app->flush_input = true;
		app->mode = &WF_MODE_NORMAL;
		return WF_ACT_REDRAW;
	}
	app->settings.draft.theme = (int)id;
	app->settings.sub = WF_SS_BROWSE;
	app->mode = &WF_MODE_SETTINGS;
	wf_app_settings_preview(app);
	return WF_ACT_REDRAW;
}

static WfAction picker_event(WfApp *app, const WfEvent *ev)
{
	size_t *i = &app->picker.index;

	if (WF_EV_KEY != ev->type) {
		return WF_ACT_NONE;
	}
	if (WF_PICKER_LANGUAGE == app->picker.stage) {
		switch (ev->key) {
		case WF_KEY_UP:
			if (app->picker.lang > 0) {
				app->picker.lang--;
			}
			language_preview(app);
			return WF_ACT_REDRAW;
		case WF_KEY_DOWN:
			if (app->picker.lang + 1 < WF_LANGSET_COUNT) {
				app->picker.lang++;
			}
			language_preview(app);
			return WF_ACT_REDRAW;
		case WF_KEY_ESC:
			/* "use the default": the language this screen began with */
			app->picker.lang = app->picker.lang_before;
			language_preview(app);
			app->picker.stage = WF_PICKER_KEYS;
			return WF_ACT_REDRAW;
		case WF_KEY_ENTER:
			app->picker.stage = WF_PICKER_KEYS;
			return WF_ACT_REDRAW;
		default: return WF_ACT_NONE;
		}
	}
	if (WF_PICKER_KEYS == app->picker.stage) {
		switch (ev->key) {
		case WF_KEY_UP:
		case WF_KEY_DOWN:
			app->picker.keys_sel = !app->picker.keys_sel;
			return WF_ACT_REDRAW;
		case WF_KEY_ENTER:
			app->picker.keys = app->picker.keys_sel;
			app->picker.stage = WF_PICKER_THEME;
			return WF_ACT_REDRAW;
		case WF_KEY_ESC: /* the profile stays as it is */
			app->picker.keys = -1;
			app->picker.stage = WF_PICKER_THEME;
			return WF_ACT_REDRAW;
		default: return WF_ACT_NONE;
		}
	}
	switch (ev->key) {
	case WF_KEY_LEFT:
		if (*i % GRID_COLS) {
			(*i)--;
		}
		break;
	case WF_KEY_RIGHT:
		if (*i % GRID_COLS + 1 < GRID_COLS) {
			(*i)++;
		}
		break;
	case WF_KEY_UP:
		if (*i >= GRID_COLS) {
			*i -= GRID_COLS;
		}
		break;
	case WF_KEY_DOWN:
		if (*i + GRID_COLS < WF_THEME_PRESET_COUNT) {
			*i += GRID_COLS;
		}
		break;
	case WF_KEY_ENTER: return picker_finish(app, (WfThemeId)*i);
	case WF_KEY_ESC:
		if (app->picker.onboarding) {
			return picker_finish(app,
			        (unsigned)app->picker.before <
			                        WF_THEME_PRESET_COUNT
			                ? app->picker.before
			                : WF_THEME_DEFAULT);
		}
		restore(app);
		return picker_finish(app, app->picker.before);
	default: return WF_ACT_NONE;
	}
	preview(app);
	return WF_ACT_REDRAW;
}

const WfMode WF_MODE_SETUP = {"SETUP", picker_draw, picker_event};
