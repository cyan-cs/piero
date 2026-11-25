/*
 * The settings screen: three columns (sections, entries, details).
 *
 * The screen edits a copy (the draft) of the live configuration. Only the
 * theme, the colour mode and the language are shown live while editing;
 * everything else takes effect when the draft is saved. Saving writes just the
 * settings that differ from the live configuration, through the settings port,
 * and only a successful save changes the live configuration. Esc with unsaved
 * changes asks; discarding puts back exactly what was in force. Esc never
 * saves.
 *
 * Only settings that exist and do something are listed. Items that act at the
 * next start say so; what is fixed for safety is shown, not offered.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../edit/wf_ekeymap.h"
#include "../text/wf_text.h"
#include "wf_app_settings_priv.h"
#include "../base/wf_macro.h"

#define F(member) offsetof(WfConfig, member)

const WfMsg wf_set_section_names[WF_SEC_COUNT] = {
        MSG_SET_SEC_SETTINGS, MSG_SET_SEC_LICENSES, MSG_SET_SEC_ABOUT};

const WfMsg wf_set_category_names[WF_CAT_COUNT] = {MSG_SETTINGS_CAT_GENERAL,
        MSG_SETTINGS_CAT_KEYS, MSG_SETTINGS_CAT_APPEARANCE,
        MSG_SETTINGS_CAT_FILES, MSG_SETTINGS_CAT_PREVIEW,
        MSG_SETTINGS_CAT_EDITOR, MSG_SETTINGS_CAT_ARCHIVES,
        MSG_SETTINGS_CAT_OPERATIONS, MSG_UPDATES_TITLE};

/* Shown for a choice's values: the message for each, or the raw word. */
static const WfMsg sort_labels[] = {
        MSG_SORT_NAME, MSG_SORT_SIZE, MSG_SORT_DATE, MSG_SORT_EXT};

static const Item items[] = {
        {WF_CAT_GENERAL, MSG_SETTINGS_ITEM_LANGUAGE, K_CHOICE, F(lang_setting),
                wf_langset_choices, 0, 0, 0, NULL, MSG_SETTINGS_HELP_LANGUAGE,
                WF_MSG_COUNT, INFO_NONE},
        {WF_CAT_GENERAL, MSG_SETTINGS_ITEM_SESSION, K_BOOL, F(session_persist),
                NULL, 0, 0, 0, NULL, MSG_SETTINGS_HELP_SESSION, WF_MSG_COUNT,
                INFO_NONE},
        {WF_CAT_FILES, MSG_SETTINGS_ITEM_HIDDEN, K_BOOL, F(show_hidden), NULL,
                0, 0, 0, NULL, MSG_SETTINGS_HELP_HIDDEN, WF_MSG_COUNT,
                INFO_NONE},
        {WF_CAT_FILES, MSG_SETTINGS_ITEM_SORT_BY, K_CHOICE, F(sort_by),
                "name|size|date|ext", 0, 0, 0, sort_labels,
                MSG_SETTINGS_HELP_SORT_BY, WF_MSG_COUNT, INFO_NONE},
        {WF_CAT_FILES, MSG_SETTINGS_ITEM_SORT_DESC, K_BOOL, F(sort_descending),
                NULL, 0, 0, 0, NULL, MSG_SETTINGS_HELP_SORT_DESC, WF_MSG_COUNT,
                INFO_NONE},
        {WF_CAT_FILES, MSG_SETTINGS_ITEM_GIT_FOLDERS, K_BOOL,
                F(git_folder_color), NULL, 0, 0, 0, NULL,
                MSG_SETTINGS_HELP_GIT_FOLDERS, WF_MSG_COUNT, INFO_NONE},
        {WF_CAT_PREVIEW, MSG_SETTINGS_ITEM_DIR_PREVIEW, K_BOOL, F(dir_preview),
                NULL, 0, 0, 0, NULL, MSG_SETTINGS_HELP_DIR_PREVIEW,
                WF_MSG_COUNT, INFO_NONE},
        {WF_CAT_PREVIEW, MSG_SETTINGS_ITEM_IMAGES, K_BOOL, F(images), NULL, 0,
                0, 0, NULL, MSG_SETTINGS_HELP_IMAGES, MSG_SETTINGS_NEXT_START,
                INFO_NONE},
        {WF_CAT_PREVIEW, MSG_SETTINGS_ITEM_GRAPHICS, K_CHOICE, F(graphics),
                "auto|sixel|text", 0, 0, 0, NULL, MSG_SETTINGS_HELP_GRAPHICS,
                WF_MSG_COUNT, INFO_NONE},
        {WF_CAT_PREVIEW, MSG_SETTINGS_ITEM_IMAGE_QUALITY, K_CHOICE,
                F(image_quality), "high|balanced|fast", 0, 0, 0, NULL,
                MSG_SETTINGS_HELP_IMAGE_QUALITY, WF_MSG_COUNT, INFO_NONE},
        {WF_CAT_PREVIEW, MSG_SETTINGS_ITEM_IMAGE_STYLE, K_CHOICE,
                F(image_style), "default|halfblock|ascii|off", 0, 0, 0, NULL,
                MSG_SETTINGS_HELP_IMAGE_STYLE, MSG_SETTINGS_NEXT_START,
                INFO_NONE},
        {WF_CAT_PREVIEW, MSG_SETTINGS_ITEM_TEXT_PREVIEW, K_BOOL,
                F(preview_enabled), NULL, 0, 0, 0, NULL,
                MSG_SETTINGS_HELP_TEXT_PREVIEW, MSG_SETTINGS_NEXT_START,
                INFO_NONE},
        {WF_CAT_PREVIEW, MSG_SETTINGS_ITEM_TEXT_LIMIT, K_INT, F(text_max_kib),
                NULL, 16, 1024, 16, NULL, MSG_SETTINGS_HELP_TEXT_LIMIT,
                MSG_SETTINGS_NEXT_START, INFO_NONE},
        {WF_CAT_EDITOR, MSG_SETTINGS_ITEM_KEYMAP, K_CHOICE, F(editor_keymap),
                "piero|nano", 0, 0, 0, NULL, MSG_SETTINGS_HELP_KEYMAP,
                WF_MSG_COUNT, INFO_NONE},
        {WF_CAT_EDITOR, MSG_SETTINGS_ITEM_TAB_WIDTH, K_INT, F(editor_tab_width),
                NULL, 1, 16, 1, NULL, MSG_SETTINGS_HELP_TAB_WIDTH, WF_MSG_COUNT,
                INFO_NONE},
        {WF_CAT_EDITOR, MSG_SETTINGS_ITEM_LINE_NUMBERS, K_BOOL,
                F(editor_line_numbers), NULL, 0, 0, 0, NULL,
                MSG_SETTINGS_HELP_LINE_NUMBERS, WF_MSG_COUNT, INFO_NONE},
        {WF_CAT_EDITOR, MSG_SETTINGS_ITEM_AUTO_INDENT, K_BOOL,
                F(editor_auto_indent), NULL, 0, 0, 0, NULL,
                MSG_SETTINGS_HELP_AUTO_INDENT, WF_MSG_COUNT, INFO_NONE},
        {WF_CAT_EDITOR, MSG_SETTINGS_ITEM_SOFT_WRAP, K_BOOL,
                F(editor_soft_wrap), NULL, 0, 0, 0, NULL,
                MSG_SETTINGS_HELP_SOFT_WRAP, WF_MSG_COUNT, INFO_NONE},
        {WF_CAT_EDITOR, MSG_SETTINGS_ITEM_SYNTAX, K_BOOL, F(editor_syntax),
                NULL, 0, 0, 0, NULL, MSG_SETTINGS_HELP_SYNTAX, WF_MSG_COUNT,
                INFO_NONE},
        {WF_CAT_EDITOR, MSG_SETTINGS_ITEM_DRAFTS, K_BOOL,
                F(editor_recovery_draft), NULL, 0, 0, 0, NULL,
                MSG_SETTINGS_HELP_DRAFTS, WF_MSG_COUNT, INFO_NONE},
        {WF_CAT_EDITOR, MSG_SETTINGS_ITEM_EDITOR_LIMIT, K_INT,
                F(editor_max_mib), NULL, 1, 256, 4, NULL,
                MSG_SETTINGS_HELP_EDITOR_LIMIT, WF_MSG_COUNT, INFO_NONE},
        {WF_CAT_EDITOR, MSG_SETTINGS_ITEM_UNDO, K_INT, F(editor_undo_mib), NULL,
                1, 256, 4, NULL, MSG_SETTINGS_HELP_UNDO, WF_MSG_COUNT,
                INFO_NONE},
        {WF_CAT_EDITOR, MSG_SETTINGS_ITEM_BACKUP, K_INFO, 0, NULL, 0, 0, 0,
                NULL, MSG_SETTINGS_HELP_BACKUP, WF_MSG_COUNT,
                INFO_EDITOR_BACKUP},
        {WF_CAT_ARCHIVES, MSG_SETTINGS_ITEM_ARCHIVE_MODE, K_INFO, 0, NULL, 0, 0,
                0, NULL, MSG_SETTINGS_HELP_ARCHIVE_MODE, WF_MSG_COUNT,
                INFO_ARCHIVE_MODE},
        {WF_CAT_ARCHIVES, MSG_SETTINGS_ITEM_ARCHIVE_NAMES, K_INFO, 0, NULL, 0,
                0, 0, NULL, MSG_SETTINGS_HELP_ARCHIVE_NAMES, WF_MSG_COUNT,
                INFO_ARCHIVE_NAMES},
        {WF_CAT_OPERATIONS, MSG_SETTINGS_ITEM_DELETE, K_INFO, 0, NULL, 0, 0, 0,
                NULL, MSG_SETTINGS_HELP_DELETE, WF_MSG_COUNT, INFO_DELETE},
        {WF_CAT_OPERATIONS, MSG_SETTINGS_ITEM_COPY_VERIFY, K_INFO, 0, NULL, 0,
                0, 0, NULL, MSG_SETTINGS_HELP_COPY_VERIFY, WF_MSG_COUNT,
                INFO_COPY_VERIFY},
};
#define ITEM_COUNT (WF_ARRAY_LEN(items))

/* Settings are ints (enums included): read and written through memcpy, the
 * way the configuration reader does. */
int wf_set_get_int(const WfConfig *c, const Item *it)
{
	int v;

	memcpy(&v, (const char *)c + it->offset, sizeof v);
	return v;
}

void wf_set_put_int(WfConfig *c, const Item *it, int v)
{
	memcpy((char *)c + it->offset, &v, sizeof v);
}

static const char *choice_at(const char *choices, int index, size_t *len)
{
	for (;;) {
		const char *end = strchr(choices, '|');

		if (index-- == 0) {
			*len = end ? (size_t)(end - choices) : strlen(choices);
			return choices;
		}
		if (!end) {
			return NULL;
		}
		choices = end + 1;
	}
}

int wf_set_choice_count(const char *choices)
{
	int n = 1;

	for (; *choices; choices++) {
		if ('|' == *choices) {
			n++;
		}
	}
	return n;
}

void wf_set_note(WfApp *app, bool is_error, const char *text)
{
	snprintf(app->settings.note, sizeof app->settings.note, "%s", text);
	app->settings.note_is_error = is_error;
}

void wf_set_note_msg(WfApp *app, bool is_error, WfMsg id)
{
	wf_set_note(app, is_error, wf_msg(id));
}

/* The language the draft asks for, on screen. A note written in the old
 * language would be wrong afterwards, so it goes. */
void wf_set_apply_language(WfApp *app, const WfConfig *c)
{
	WfUiLang now = wf_i18n_resolve(
	        (WfLangSetting)c->lang_setting, app->lang_detected);

	if (now == wf_i18n_language()) {
		return;
	}
	wf_i18n_set_language(now);
	app->settings.note[0] = '\0';
	app->msg[0] = '\0';
}

static size_t items_in(int category, size_t *first)
{
	size_t i, n = 0;

	*first = ITEM_COUNT;
	for (i = 0; i < ITEM_COUNT; i++) {
		if (items[i].category == category) {
			if (ITEM_COUNT == *first) {
				*first = i;
			}
			n++;
		}
	}
	return n;
}

const Item *wf_set_item_at(const WfApp *app, size_t row)
{
	size_t first, n = items_in(app->settings.category, &first);

	return row < n ? &items[first + row] : NULL;
}

bool wf_set_is_dirty(const WfApp *app)
{
	return app->config &&
	        wf_config_differs(app->config, &app->settings.draft);
}

void wf_set_palette_of(const WfConfig *c, WfThemePalette *p)
{
	wf_config_palette(c, p);
}

/* What the screen shows live: the theme, the colour mode and the language. */
void wf_set_preview(WfApp *app)
{
	WfThemePalette p;

	wf_set_palette_of(&app->settings.draft, &p);
	wf_theme_activate(&p);
	wf_set_apply_language(app, &app->settings.draft);
	if (app->settings_port.apply) {
		app->settings_port.apply(
		        app->settings_port.ctx, &app->settings.draft, false);
	}
}

void wf_app_settings_preview(WfApp *app)
{
	wf_set_preview(app);
}

/* The live configuration, back on screen. */
static void show_live(WfApp *app)
{
	WfThemePalette p;

	wf_set_palette_of(app->config, &p);
	wf_theme_activate(&p);
	wf_set_apply_language(app, app->config);
	if (app->settings_port.apply) {
		app->settings_port.apply(
		        app->settings_port.ctx, app->config, false);
	}
}

void wf_app_set_settings(WfApp *app, WfConfig *config, WfSettingsPort port)
{
	app->config = config;
	app->settings_port = port;
}

void wf_app_set_version(WfApp *app, const char *version)
{
	app->version = version;
}

void wf_app_set_license_dir(WfApp *app, const wchar_t *dir)
{
	free(app->license_dir);
	app->license_dir = dir ? _wcsdup(dir) : NULL;
}

static void doc_free(WfApp *app)
{
	free(app->settings.doc);
	app->settings.doc = NULL;
	app->settings.doc_n = 0;
}

/* The entry of a Licenses or About section is a text. */
static void doc_load(WfApp *app)
{
	doc_free(app);
	app->settings.top = 0;
	if (WF_SEC_ABOUT == app->settings.section && app->recovery.refresh) {
		app->recovery.refresh(app->recovery.context);
	}
	if (WF_SEC_SETTINGS == app->settings.section) {
		return;
	}
	app->settings.doc = wf_about_text(app, app->settings.section,
	        (size_t)app->settings.category, &app->settings.doc_n);
}

static void entry_changed(WfApp *app)
{
	app->settings.item = 0;
	app->settings.top = 0;
	app->settings.edit_theme = -1;
	app->settings.rename_index = -1;
	app->settings.search[0] = '\0';
	doc_load(app);
}

const WfSetPage *wf_set_page_of(const WfApp *app);

void wf_app_settings_begin(WfApp *app)
{
	wf_i18n_refresh();
	if (!app->config) {
		wf_app_set_msg(app, wf_msg(MSG_SETTINGS_NOT_AVAILABLE));
		return;
	}
	app->settings.draft = *app->config;
	app->settings.draft.start_directory = NULL;
	app->settings.sub = WF_SS_BROWSE;
	app->settings.reload_pending = false;
	app->settings.section = WF_SEC_SETTINGS;
	app->settings.focus = WF_FOCUS_CATEGORY;
	app->settings.category = WF_CAT_GENERAL;
	app->settings.note[0] = '\0';
	app->settings.note_is_error = false;
	app->settings.input[0] = '\0';
	app->settings.input_n = 0;
	app->settings.input_cur = 0;
	wf_about_scan(app);
	wf_set_keys_refresh(app);
	entry_changed(app);
	if (app->settings_mem.valid) {
		/* back to the section, category, row and scroll it was left on */
		app->settings.section = app->settings_mem.section;
		app->settings.category = app->settings_mem.category;
		app->settings.focus = app->settings_mem.focus;
		entry_changed(app);
		{
			const WfSetPage *pg = wf_set_page_of(app);

			/* any page: the row only if the page still has it */
			if (pg && app->settings_mem.item < pg->count(app)) {
				app->settings.item = app->settings_mem.item;
				app->settings.top = app->settings_mem.top;
			}
		}
	}
	app->mode = &WF_MODE_SETTINGS;
}

static WfAction leave(WfApp *app)
{
	app->settings_mem.valid = true;
	app->settings_mem.section = app->settings.section;
	app->settings_mem.category = app->settings.category;
	app->settings_mem.focus = app->settings.focus;
	app->settings_mem.item = app->settings.item;
	app->settings_mem.top = app->settings.top;
	show_live(app);
	doc_free(app);
	wf_set_keys_free(app);
	app->mode = &WF_MODE_NORMAL;
	return WF_ACT_REDRAW;
}

const WfSetPage *wf_set_page_of(const WfApp *app);

static void info_text(const WfApp *app, Info info, char *out, size_t cap)
{
	const WfConfig *c = &app->settings.draft;

	switch (info) {
	case INFO_EDITOR_BACKUP:
		snprintf(out, cap, "%s", wf_msg(MSG_SETTINGS_VALUE_BACKUP));
		break;
	case INFO_ARCHIVE_MODE:
		snprintf(out, cap, "%s", wf_msg(MSG_SETTINGS_VALUE_ARCHIVE_RO));
		break;
	case INFO_ARCHIVE_NAMES:
		snprintf(out, cap, "%s %d", wf_msg(MSG_SETTINGS_VALUE_CODEPAGE),
		        c->text_fallback_encoding);
		break;
	case INFO_DELETE:
		snprintf(out, cap, "%s",
		        wf_msg(MSG_SETTINGS_VALUE_RECYCLE_ONLY));
		break;
	case INFO_COPY_VERIFY: {
		size_t len = 0;
		const char *t = choice_at(
		        "none|sha256|hmac-sha256", c->copy_verify, &len);

		snprintf(out, cap, "%.*s", t ? (int)len : 0, t ? t : "");
		break;
	}
	case INFO_NONE: out[0] = '\0'; break;
	}
}

/* The value `v` of a choice item as the user reads it. The language names
 * are written in their own words; the rest are a message or the raw word. */
void wf_set_choice_label(
        const WfApp *app, const Item *it, int v, char *out, size_t cap)
{
	size_t len = 0;
	const char *text;

	if (MSG_SETTINGS_ITEM_LANGUAGE == it->label) {
		wf_app_language_text(app, v, out, cap);
		return;
	}
	text = it->choices ? choice_at(it->choices, v, &len) : NULL;
	if (text && it->labels && WF_MSG_COUNT != it->labels[v]) {
		snprintf(out, cap, "%s", wf_msg(it->labels[v]));
	} else if (text) {
		snprintf(out, cap, "%.*s", (int)len, text);
	} else {
		snprintf(out, cap, "%s", wf_msg(MSG_COMMON_NOT_AVAILABLE));
	}
}

static size_t items_count(const WfApp *app)
{
	size_t first;

	return items_in(app->settings.category, &first);
}

static bool editor_keys_overridden(const WfApp *app, const Item *it)
{
	return it && it->label == MSG_SETTINGS_ITEM_KEYMAP &&
	        app->settings.draft.keys.profile != WF_PROFILE_LEGACY;
}

static void items_row(const WfApp *app, size_t i, WfSetRow *out)
{
	const Item *it = wf_set_item_at(app, i);
	const WfConfig *d = &app->settings.draft;
	int v;

	memset(out, 0, sizeof *out);
	if (!it) {
		return;
	}
	snprintf(out->label, sizeof out->label, "%s", wf_msg(it->label));
	v = K_INFO == it->kind ? 0 : wf_set_get_int(d, it);
	out->cycle = K_INFO != it->kind;
	out->dim = K_INFO == it->kind;
	switch (it->kind) {
	case K_BOOL:
		snprintf(out->value, sizeof out->value, "%s",
		        wf_msg(v ? MSG_COMMON_ON : MSG_COMMON_OFF));
		break;
	case K_CHOICE:
		wf_set_choice_label(app, it, v, out->value, sizeof out->value);
		break;
	case K_INT: snprintf(out->value, sizeof out->value, "%d", v); break;
	case K_INFO:
		info_text(app, it->info, out->value, sizeof out->value);
		break;
	}
	if (editor_keys_overridden(app, it)) {
		out->cycle = false;
		out->dim = true;
		snprintf(out->value, sizeof out->value, "%s",
		        wf_msg(MSG_SET_EDITOR_KEYS_PROFILE));
	}
	if (WF_MSG_COUNT != it->when) {
		size_t used = strlen(out->value);

		snprintf(out->value + used, sizeof out->value - used, "   (%s)",
		        wf_msg(it->when));
	}
}

static void change_value(WfApp *app, const Item *it, int dir)
{
	WfConfig *d = &app->settings.draft;
	int v = wf_set_get_int(d, it);

	switch (it->kind) {
	case K_BOOL:   v = !v; break;
	case K_CHOICE: {
		int n = wf_set_choice_count(it->choices);

		v = (v + dir + n) % n;
		break;
	}
	case K_INT: {
		long next = (long)v + (long)dir * it->step;

		if (next < it->min) {
			next = it->min;
		}
		if (next > it->max) {
			next = it->max;
		}
		v = (int)next;
		break;
	}
	case K_INFO: return;
	}
	wf_set_put_int(d, it, v);
	if (MSG_SETTINGS_ITEM_LANGUAGE == it->label) {
		wf_set_preview(app);
	}
}

static void pick_begin(WfApp *app, const Item *it)
{
	app->settings.pick = wf_set_get_int(&app->settings.draft, it);
	app->settings.sub = WF_SS_PICK;
	app->settings.note[0] = '\0';
}

/*
 * Enter (or Space) is the one key that changes a row: a yes/no switches, a
 * choice opens the list of its values, a number takes typing. Left and Right
 * never change a value, so Left always leaves the column; the old "step with
 * the arrows" made Left unusable on every row that had a value. The
 * +/- keys and Backspace still step a value for those who like it.
 */
static WfAction items_event(WfApp *app, const WfEvent *ev)
{
	const Item *it = wf_set_item_at(app, app->settings.item);
	bool enter = WF_EV_KEY == ev->type && WF_KEY_ENTER == ev->key;
	bool space = WF_EV_CHAR == ev->type && ' ' == ev->ch;
	bool right = WF_EV_KEY == ev->type && WF_KEY_RIGHT == ev->key;
	int step = 0;

	if (!it) {
		return WF_ACT_NONE;
	}
	if (WF_EV_CHAR == ev->type && ('-' == ev->ch || '+' == ev->ch)) {
		step = '-' == ev->ch ? -1 : 1;
	} else if (WF_EV_KEY == ev->type && WF_KEY_BACKSPACE == ev->key) {
		step = -1;
	}
	if (editor_keys_overridden(app, it)) {
		if (enter || space || step || right) {
			wf_set_note_msg(
			        app, false, MSG_SET_EDITOR_KEYS_PROFILE_HELP);
			return WF_ACT_REDRAW;
		}
		return WF_ACT_NONE;
	}
	if (K_INFO == it->kind) {
		if (enter || space || step) {
			wf_set_note_msg(app, false, MSG_SETTINGS_FIXED);
			return WF_ACT_REDRAW;
		}
		return WF_ACT_NONE;
	}
	if (K_CHOICE == it->kind && (enter || space || right)) {
		pick_begin(app, it);
		return WF_ACT_REDRAW;
	}
	if (K_INT == it->kind && (enter || space)) {
		char now[16];

		snprintf(now, sizeof now, "%d",
		        wf_set_get_int(&app->settings.draft, it));
		wf_set_text_begin(app, WF_TEXT_NUMBER, now);
		app->settings.input_fresh =
		        true; /* typing replaces the number */
		return WF_ACT_REDRAW;
	}
	if (K_BOOL == it->kind && (enter || space)) {
		step = 1;
	}
	if (!step) {
		return WF_ACT_NONE;
	}
	change_value(app, it, step);
	return WF_ACT_REDRAW;
}

static const char *items_help(const WfApp *app)
{
	const Item *it = wf_set_item_at(app, app->settings.item);

	return editor_keys_overridden(app, it)
	        ? wf_msg(MSG_SET_EDITOR_KEYS_PROFILE_HELP)
	        : it && WF_MSG_COUNT != it->help ? wf_msg(it->help)
	                                         : "";
}

static WfMsg items_hint(const WfApp *app)
{
	const Item *it = wf_set_item_at(app, app->settings.item);

	switch (it && !editor_keys_overridden(app, it) ? it->kind : K_INFO) {
	case K_BOOL:   return MSG_SET_HINT_ITEM_TOGGLE;
	case K_CHOICE: return MSG_SET_HINT_ITEM_CHOOSE;
	case K_INT:    return MSG_SET_HINT_ITEM_NUMBER;
	case K_INFO:   break;
	}
	return MSG_SET_HINT_ITEM_FIXED;
}

static const WfSetPage ITEMS_PAGE = {
        items_count, items_row, items_event, items_help, items_hint};

/* The page the details of the current entry are made by; NULL for a text. */
const WfSetPage *wf_set_page_of(const WfApp *app)
{
	if (WF_SEC_SETTINGS != app->settings.section) {
		return NULL;
	}
	switch (app->settings.category) {
	case WF_CAT_UPDATES:    return &WF_SETPAGE_UPDATES;
	case WF_CAT_KEYBOARD:   return &WF_SETPAGE_KEYS;
	case WF_CAT_APPEARANCE: return &WF_SETPAGE_THEMES;
	default:                return &ITEMS_PAGE;
	}
}

/* The room the names need (a marker, the longest name, one space), not a
 * fixed share of the screen: a language with longer names gets more, and
 * the details column gets whatever is left. */
static int names_w(const WfMsg *names, size_t n, int lo, int hi)
{
	int w = lo;

	for (size_t i = 0; i < n; i++) {
		const char *s = wf_msg(names[i]);
		int cells = wf_utf8_cells(s, strlen(s)) + 2;

		if (cells > w) {
			w = cells;
		}
	}
	return w > hi ? hi : w;
}

#define DETAILS_MIN 28

/* x and width of the three columns; a width of 0 means "not shown". Wide:
 * all three side by side. Medium: two (sections and entries, or entries and
 * details, whichever holds the focus). Narrow: one at a time, a hierarchy. */
void wf_set_layout(const WfApp *app, int cols, int x[3], int w[3])
{
	int focus = app->settings.focus;
	int sec = names_w(wf_set_section_names, WF_SEC_COUNT, 10, 20);
	int cat = names_w(wf_set_category_names, WF_CAT_COUNT, 14, 28);

	w[0] = w[1] = w[2] = 0;
	x[0] = x[1] = x[2] = 0;
	if (cols >= sec + cat + 2 + DETAILS_MIN) {
		w[0] = sec;
		x[1] = x[0] + w[0] + 1;
		w[1] = cat;
		x[2] = x[1] + w[1] + 1;
		w[2] = cols - x[2];
	} else if (cols >= cat + 1 + DETAILS_MIN - 8) {
		if (WF_FOCUS_SECTION == focus) {
			w[0] = sec;
			x[1] = x[0] + w[0] + 1;
			w[1] = cols - x[1];
		} else {
			w[1] = cat;
			x[2] = x[1] + w[1] + 1;
			w[2] = cols - x[2];
		}
	} else {
		w[focus] = cols;
	}
}

int wf_set_view_rows(const WfApp *app)
{
	return app->rows > 7 ? app->rows - 6 : 1;
}

/* One screen row of `doc`, wrapped at `width` cells: returns where the next
 * row starts and sets [*line, *line + *len) to this row's bytes. */
const char *wf_set_doc_row(const char *p,
        const char *end,
        int width,
        const char **line,
        size_t *len)
{
	const char *start = p;
	int cells = 0;

	*line = p;
	while (p < end && '\n' != *p) {
		uint32_t cp;
		size_t k = wf_utf8_decode(p, (size_t)(end - p), &cp);
		int w;

		if (!k) {
			k = 1;
		}
		w = '\t' == *p ? 1 : wf_utf8_cells(p, k);
		if (cells + w > width && p > start) {
			break;
		}
		cells += w;
		p += k;
	}
	*len = (size_t)(p - start);
	if (p < end && '\n' == *p) {
		p++;
	} else if (p < end && 0 == cells) {
		p++;
	}
	return p;
}

size_t wf_set_doc_rows(const WfApp *app, int width)
{
	const char *p = app->settings.doc, *end, *line;
	size_t n = 0, len;

	if (!p) {
		return 0;
	}
	end = p + app->settings.doc_n;
	while (p < end) {
		p = wf_set_doc_row(p, end, width, &line, &len);
		n++;
	}
	return n;
}

static int details_width(const WfApp *app)
{
	int x[3], w[3];

	wf_set_layout(app, app->cols, x, w);
	return w[2] > 4 ? w[2] - 2 : 1;
}

static size_t details_count(const WfApp *app)
{
	const WfSetPage *pg = wf_set_page_of(app);

	return pg ? pg->count(app) : 0;
}

static bool row_is_header(const WfApp *app, size_t i)
{
	const WfSetPage *pg = wf_set_page_of(app);
	WfSetRow r;

	if (!pg) {
		return false;
	}
	pg->row(app, i, &r);
	return r.header;
}

void wf_set_scroll_fit(WfApp *app)
{
	size_t h = (size_t)wf_set_view_rows(app);

	if (app->settings.item < app->settings.top) {
		app->settings.top = app->settings.item;
	} else if (app->settings.item >= app->settings.top + h) {
		app->settings.top = app->settings.item + 1 - h;
	}
}

static void move_row(WfApp *app, int dir)
{
	size_t n = details_count(app), at = app->settings.item;

	if (!n) {
		return;
	}
	for (;;) {
		if (dir < 0 ? 0 == at : at + 1 >= n) {
			return;
		}
		at = dir < 0 ? at - 1 : at + 1;
		if (!row_is_header(app, at)) {
			break;
		}
	}
	app->settings.item = at;
	wf_set_scroll_fit(app);
}

static void first_row(WfApp *app)
{
	size_t n = details_count(app), i;

	app->settings.item = 0;
	for (i = 0; i < n; i++) {
		if (!row_is_header(app, i)) {
			app->settings.item = i;
			break;
		}
	}
	app->settings.top = 0;
	if (WF_SEC_SETTINGS == app->settings.section &&
	        WF_CAT_APPEARANCE == app->settings.category) {
		/* the cursor starts on the theme in use */
		const WfConfig *c = &app->settings.draft;
		const WfCustomEntry *sel = wf_config_custom_selected(c);

		if (WF_THEME_CUSTOM == c->theme && sel) {
			app->settings.item = WF_THEME_PRESET_COUNT +
			        (size_t)(sel - c->customs);
		} else if (c->theme >= 0 && c->theme < WF_THEME_PRESET_COUNT) {
			app->settings.item = (size_t)c->theme;
		}
		wf_set_scroll_fit(app);
	}
}

static void move_doc(WfApp *app, long delta)
{
	size_t total = wf_set_doc_rows(app, details_width(app)),
	       h = (size_t)wf_set_view_rows(app),
	       max = total > h ? total - h : 0;
	long next = (long)app->settings.top + delta;

	if (next < 0) {
		next = 0;
	}
	if ((size_t)next > max) {
		next = (long)max;
	}
	app->settings.top = (size_t)next;
}

size_t wf_set_entry_count(const WfApp *app)
{
	return WF_SEC_SETTINGS == app->settings.section
	        ? (size_t)WF_CAT_COUNT
	        : wf_about_count(app, app->settings.section);
}

static void select_section(WfApp *app, int s)
{
	app->settings.section = s;
	app->settings.category = 0;
	entry_changed(app);
	app->settings.item = 0;
	if (WF_SEC_SETTINGS == s) {
		first_row(app);
	}
}

static void select_category(WfApp *app, int c)
{
	app->settings.category = c;
	entry_changed(app);
	first_row(app);
}

/* The unsaved-changes question: S, D or C, or Up/Down and Enter, which starts
 * on "keep editing" so that a stray Enter loses nothing. */
static void ask_discard(WfApp *app)
{
	app->settings.sub = WF_SS_DISCARD;
	wf_choice_begin(&app->settings.choice, "sdc", 2);
}

void wf_set_ask_yes_no(WfApp *app, WfSettingsSub sub)
{
	app->settings.sub = sub;
	wf_choice_begin(&app->settings.choice, "yn", 1); /* Enter says No */
}

static WfAction discard_or_leave(WfApp *app)
{
	app->settings.reload_pending = false;
	if (wf_set_is_dirty(app)) {
		ask_discard(app);
		return WF_ACT_REDRAW;
	}
	return leave(app);
}

static WfAction reload_file(WfApp *app)
{
	WfConfig *fresh, *before;

	if (!app->settings_port.reload) {
		return WF_ACT_NONE;
	}
	fresh = malloc(sizeof *fresh);
	before = malloc(sizeof *before);
	if (!fresh || !before) {
		free(fresh);
		free(before);
		return WF_ACT_NONE;
	}
	wf_config_init(fresh);
	if (WF_OK == app->settings_port.reload(app->settings_port.ctx, fresh)) {
		char *start = app->config->start_directory;

		*before = *app->config;
		*app->config = *fresh;
		app->config->start_directory = start;
		app->settings.draft = *app->config;
		app->settings.draft.start_directory = NULL;
		wf_set_apply_live_changes(app, before);
		wf_set_keys_refresh(app);
		entry_changed(app);
		wf_set_note_msg(app, false, MSG_SETTINGS_RELOADED);
	} else {
		wf_set_note_msg(app, true, MSG_SETTINGS_RELOAD_FAILED);
	}
	wf_config_dispose(fresh);
	free(fresh);
	free(before);
	return WF_ACT_REDRAW;
}

static void move_focus(WfApp *app, int dir)
{
	int f = app->settings.focus + dir;

	if (f < WF_FOCUS_SECTION || f > WF_FOCUS_DETAILS) {
		return;
	}
	if (WF_FOCUS_DETAILS == f && WF_SEC_SETTINGS == app->settings.section &&
	        !details_count(app)) {
		return;
	}
	app->settings.focus = f;
	if (WF_FOCUS_DETAILS == f && WF_SEC_ABOUT == app->settings.section) {
		doc_load(app);
	}
	if (WF_FOCUS_DETAILS == f && WF_SEC_SETTINGS == app->settings.section &&
	        row_is_header(app, app->settings.item)) {
		first_row(app);
	}
}

static WfAction browse_event(WfApp *app, const WfEvent *ev)
{
	const WfSetPage *pg = wf_set_page_of(app);
	int focus = app->settings.focus;
	long page = wf_set_view_rows(app) > 1 ? wf_set_view_rows(app) - 1 : 1;

	if (WF_EV_CTRL == ev->type && 's' == ev->ch) {
		(void)wf_set_save_draft(app);
		return WF_ACT_REDRAW;
	}
	if (WF_FOCUS_DETAILS == focus && pg) {
		WfAction a = pg->event(app, ev);

		if (WF_ACT_NONE != a) {
			return a;
		}
	}
	if (WF_EV_CHAR == ev->type && ('r' == ev->ch || 'R' == ev->ch) &&
	        !(WF_FOCUS_DETAILS == focus && &WF_SETPAGE_KEYS == pg)) {
		if (wf_set_is_dirty(app)) {
			app->settings.reload_pending = true;
			ask_discard(app);
			return WF_ACT_REDRAW;
		}
		return reload_file(app);
	}
	if (WF_EV_KEY != ev->type) {
		return WF_ACT_NONE;
	}
	switch (ev->key) {
	case WF_KEY_UP:
	case WF_KEY_DOWN: {
		int dir = WF_KEY_UP == ev->key ? -1 : 1;

		if (WF_FOCUS_SECTION == focus) {
			int s = app->settings.section + dir;

			if (s >= 0 && s < WF_SEC_COUNT) {
				select_section(app, s);
			}
		} else if (WF_FOCUS_CATEGORY == focus) {
			long c = (long)app->settings.category + dir;

			if (c >= 0 && (size_t)c < wf_set_entry_count(app)) {
				select_category(app, (int)c);
			}
		} else if (pg) {
			move_row(app, dir);
		} else {
			move_doc(app, dir);
		}
		break;
	}
	case WF_KEY_PGUP:
	case WF_KEY_PGDN:
		if (WF_FOCUS_DETAILS == focus && !pg) {
			move_doc(app, WF_KEY_PGUP == ev->key ? -page : page);
		} else if (WF_FOCUS_DETAILS == focus) {
			for (long i = 0; i < page; i++) {
				move_row(app, WF_KEY_PGUP == ev->key ? -1 : 1);
			}
		}
		break;
	case WF_KEY_HOME:
		if (WF_FOCUS_DETAILS == focus) {
			if (pg) {
				first_row(app);
			} else {
				app->settings.top = 0;
			}
		}
		break;
	case WF_KEY_END:
		if (WF_FOCUS_DETAILS == focus) {
			if (pg) {
				for (size_t i = details_count(app); i > 0;
				        i--) {
					move_row(app, 1);
				}
			} else {
				move_doc(app, 1 << 20);
			}
		}
		break;
	case WF_KEY_LEFT:
		/* Left is "back" on every row of every page: no value is changed
		 * by the arrows (a choice opens with Enter or Right). */
		if (WF_FOCUS_DETAILS == focus && wf_set_theme_back(app)) {
			break;
		}
		move_focus(app, -1);
		break;
	case WF_KEY_RIGHT:
		if (WF_FOCUS_DETAILS != focus) {
			move_focus(app, 1);
		}
		break;
	case WF_KEY_ENTER:
		if (WF_FOCUS_DETAILS == focus) {
			if (!pg && WF_SEC_ABOUT == app->settings.section) {
				doc_load(app);
			}
			break; /* Enter in the details was the page's */
		}
		move_focus(app, 1);
		break;
	case WF_KEY_TAB:
		move_focus(app, WF_FOCUS_DETAILS == focus ? -1 : 1);
		break;
	case WF_KEY_SHIFT_TAB: move_focus(app, -1); break;
	case WF_KEY_ESC:
		if (WF_FOCUS_DETAILS == focus && wf_set_theme_back(app)) {
			break;
		}
		return discard_or_leave(app);
	default: return WF_ACT_NONE;
	}
	return WF_ACT_REDRAW;
}

static WfAction finish_discard(WfApp *app)
{
	bool reload = app->settings.reload_pending;
	app->settings.reload_pending = false;
	if (!reload) {
		return leave(app);
	}
	app->settings.sub = WF_SS_BROWSE;
	return reload_file(app);
}

static WfAction discard_event(WfApp *app, const WfEvent *ev)
{
	char ans = 0;

	if (WF_EV_CTRL == ev->type && 's' == ev->ch) {
		ans = 's'; /* Ctrl+S saves here too */
	} else {
		switch (wf_choice_event(&app->settings.choice, ev, &ans)) {
		case WF_CHOICE_MOVED:  return WF_ACT_REDRAW;
		case WF_CHOICE_IDLE:   return WF_ACT_NONE;
		case WF_CHOICE_ANSWER: break;
		}
	}
	switch (ans) {
	case 's':
		if (wf_set_save_draft(app)) {
			return finish_discard(app);
		}
		app->settings.sub = WF_SS_BROWSE;
		return WF_ACT_REDRAW;
	case 'd': return finish_discard(app);
	default:  /* 'c': keep editing */
		app->settings.reload_pending = false;
		app->settings.sub = WF_SS_BROWSE;
		return WF_ACT_REDRAW;
	}
}

static WfAction reset_all_event(WfApp *app, const WfEvent *ev)
{
	char ans = 0;

	switch (wf_choice_event(&app->settings.choice, ev, &ans)) {
	case WF_CHOICE_MOVED:  return WF_ACT_REDRAW;
	case WF_CHOICE_IDLE:   return WF_ACT_NONE;
	case WF_CHOICE_ANSWER: break;
	}
	if ('y' == ans) {
		wf_set_keys_reset_all(app);
	}
	app->settings.sub = WF_SS_BROWSE;
	return WF_ACT_REDRAW;
}

/*
 * With a Japanese IME in full-width mode a key arrives as "ｙ", "１" or the
 * ideographic space. Where a character is a command or a number it means the
 * ASCII one. Free text (a name, a search) and the key capture are left as
 * typed: there the character itself is the input.
 */
static WfEvent ascii_for_commands(const WfApp *app, const WfEvent *ev)
{
	WfEvent out = *ev;
	/* The questions that can lose something (discard, delete, reset) are
	 * answered by the real letter or by Up/Down and Enter, never by a
	 * full-width letter that happened to arrive. */
	bool question = WF_SS_DISCARD == app->settings.sub ||
	        WF_SS_DELETE == app->settings.sub ||
	        WF_SS_RESET_ALL == app->settings.sub;
	bool commands = WF_SS_TEXT == app->settings.sub
	        ? (WF_TEXT_HEX == app->settings.text_kind ||
	                  WF_TEXT_NUMBER == app->settings.text_kind)
	        : WF_SS_CAPTURE != app->settings.sub && !question;

	if (commands && WF_EV_CHAR == ev->type) {
		if (ev->ch >= 0xFF01 && ev->ch <= 0xFF5E) {
			out.ch = ev->ch - 0xFEE0;
		} else if (0x3000 == ev->ch) {
			out.ch = ' ';
		}
	}
	return out;
}

static WfAction settings_event(WfApp *app, const WfEvent *raw)
{
	WfEvent folded = ascii_for_commands(app, raw);
	const WfEvent *ev = &folded;

	if (WF_EV_RESIZE == ev->type || WF_EV_RELEASE == ev->type) {
		return WF_ACT_NONE;
	}
	switch (app->settings.sub) {
	case WF_SS_BROWSE:    return browse_event(app, ev);
	case WF_SS_PICK:      return wf_set_pick_event(app, ev);
	case WF_SS_TEXT:      return wf_set_text_event(app, ev);
	case WF_SS_CAPTURE:   return wf_set_keys_capture(app, ev);
	case WF_SS_WARN:      return wf_set_theme_warn_event(app, ev);
	case WF_SS_DELETE:    return wf_set_theme_delete_event(app, ev);
	case WF_SS_RESET_ALL: return reset_all_event(app, ev);
	case WF_SS_DISCARD:   return discard_event(app, ev);
	}
	return WF_ACT_NONE;
}

/* "Auto (日本語)", "English", "日本語": a language is always written in its
 * own words, Auto in the words of the current language. */
void wf_app_language_text(const WfApp *app, int setting, char *out, size_t cap)
{
	if (setting > 0 && setting < WF_LANGSET_COUNT) {
		(void)wf_msgf(out, cap, MSG_LANG_NAMED,
		        (WfMsgArg[]){
		                WF_ARG_S(wf_i18n_lang_available(setting - 1)
		                                ? wf_i18n_lang_name(setting - 1)
		                                : wf_msg(MSG_COMMON_NOT_AVAILABLE)),
		                WF_ARG_S(wf_i18n_setting_id(setting))},
		        2);
		return;
	}
	(void)wf_msgf(out, cap, MSG_LANG_AUTO,
	        &WF_ARG_S(wf_i18n_lang_name(app->lang_detected)), 1);
}

const WfMode WF_MODE_SETTINGS = {"SETTINGS", wf_set_draw, settings_event};
