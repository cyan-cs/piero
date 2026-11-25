/*
 * Settings > Appearance: the eight presets, the themes the user made, the
 * colours of one of those, and the colour mode.
 *
 * A preset is never edited: "duplicate" makes a custom theme from it, and that
 * is what the colours belong to. Every change shows at once (the draft's
 * palette is the live one while this screen is open) and is thrown away with
 * the draft if the screen is left without saving.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../text/wf_text.h"

#include "wf_app_settings_priv.h"

/* rows of the colours of one theme */
enum
{
	ED_NAME,
	ED_BASE,
	ED_COLOR0,
	ED_ROWS = ED_COLOR0 + WF_CUSTOM_COUNT
};

static const char *const COLOR_MODES = "auto|truecolor|256|16|none";

static WfConfig *draft(WfApp *app)
{
	return &app->settings.draft;
}

static const WfConfig *cdraft(const WfApp *app)
{
	return &app->settings.draft;
}

static bool editing(const WfApp *app)
{
	return app->settings.edit_theme >= 0 &&
	        app->settings.edit_theme < cdraft(app)->custom_count;
}

static WfCustomEntry *edited(WfApp *app)
{
	return editing(app) ? &draft(app)->customs[app->settings.edit_theme]
	                    : NULL;
}

/* The theme in force is this custom one. */
static bool custom_is_active(const WfConfig *c, const WfCustomEntry *e)
{
	const WfCustomEntry *sel = wf_config_custom_selected(c);

	return WF_THEME_CUSTOM == c->theme && sel == e;
}

static uint32_t slot_value(const WfThemePalette *p, size_t slot)
{
	switch (slot) {
	case WF_CUSTOM_BACKGROUND:           return p->background;
	case WF_CUSTOM_SURFACE:              return p->surface;
	case WF_CUSTOM_FOREGROUND:           return p->foreground;
	case WF_CUSTOM_ACCENT:               return p->accent;
	case WF_CUSTOM_MUTED:                return p->muted_foreground;
	case WF_CUSTOM_SELECTION_BACKGROUND: return p->selection_background;
	case WF_CUSTOM_SELECTION_FOREGROUND: return p->selection_foreground;
	case WF_CUSTOM_BORDER:               return p->border;
	case WF_CUSTOM_ERROR:                return p->error_foreground;
	case WF_CUSTOM_WARNING:              return p->warning_foreground;
	case WF_CUSTOM_SUCCESS:              return p->success_foreground;
	}
	return 0;
}

/* presets, customs, "new", colour mode */
static size_t list_count(const WfApp *app)
{
	return WF_THEME_PRESET_COUNT + (size_t)cdraft(app)->custom_count + 2;
}

static size_t themes_count(const WfApp *app)
{
	return editing(app) ? ED_ROWS : list_count(app);
}

static void list_row(const WfApp *app, size_t i, WfSetRow *out)
{
	const WfConfig *c = cdraft(app);
	size_t customs = (size_t)c->custom_count;

	if (i < WF_THEME_PRESET_COUNT) {
		snprintf(out->label, sizeof out->label, "%s",
		        wf_msg_reason(wf_theme_display_name((WfThemeId)i)));
		if (c->theme == (int)i) {
			snprintf(out->value, sizeof out->value, "%s",
			        wf_msg(MSG_SET_THEME_ACTIVE));
		}
		return;
	}
	i -= WF_THEME_PRESET_COUNT;
	if (i < customs) {
		const WfCustomEntry *e = &c->customs[i];

		snprintf(out->label, sizeof out->label, "%s", e->name);
		snprintf(out->value, sizeof out->value, "%s%s",
		        wf_msg(MSG_SET_THEME_CUSTOM),
		        custom_is_active(c, e) ? " *" : "");
		return;
	}
	i -= customs;
	if (0 == i) {
		snprintf(out->label, sizeof out->label, "+ %s",
		        wf_msg(MSG_SET_THEME_NEW));
		out->dim = true;
		return;
	}
	{
		size_t len = 0, n = 0;
		const char *t = COLOR_MODES;
		int v = c->color_policy;

		snprintf(out->label, sizeof out->label, "%s",
		        wf_msg(MSG_SETTINGS_ITEM_COLOR_MODE));
		for (; n < (size_t)v && NULL != (t = strchr(t, '|')); n++) {
			t++;
		}
		if (t) {
			const char *end = strchr(t, '|');

			len = end ? (size_t)(end - t) : strlen(t);
			if (0 == v) {
				snprintf(out->value, sizeof out->value, "%s",
				        wf_msg(MSG_COMMON_AUTO));
			} else if (4 == v) {
				snprintf(out->value, sizeof out->value, "%s",
				        wf_msg(MSG_COMMON_NONE));
			} else {
				snprintf(out->value, sizeof out->value, "%.*s",
				        (int)len, t);
			}
		}
		out->cycle = true;
	}
}

static void edit_row(const WfApp *app, size_t i, WfSetRow *out)
{
	const WfCustomEntry *e =
	        &cdraft(app)->customs[app->settings.edit_theme];

	if (ED_NAME == i) {
		snprintf(out->label, sizeof out->label, "%s",
		        wf_msg(MSG_SET_THEME_NAME));
		snprintf(out->value, sizeof out->value, "%s", e->name);
	} else if (ED_BASE == i) {
		snprintf(out->label, sizeof out->label, "%s",
		        wf_msg(MSG_SETTINGS_CUSTOM_BASE));
		snprintf(out->value, sizeof out->value, "%s",
		        wf_msg_reason(wf_theme_display_name(e->theme.base)));
		out->cycle = true;
	} else {
		size_t slot = i - ED_COLOR0;
		WfThemePalette p;
		char hex[8];

		wf_theme_build_custom(&e->theme, &p);
		out->rgb = slot_value(&p, slot);
		wf_color_format_hex(out->rgb, hex);
		snprintf(out->label, sizeof out->label, "%s",
		        wf_msg_reason(
		                wf_custom_slot_label((WfCustomSlot)slot)));
		snprintf(out->value, sizeof out->value, "%s%s", hex,
		        e->theme.color[slot] < 0
		                ? wf_msg(MSG_SETTINGS_CUSTOM_BASE_SUFFIX)
		                : "");
		out->swatch = true;
	}
}

static void themes_row(const WfApp *app, size_t i, WfSetRow *out)
{
	memset(out, 0, sizeof *out);
	if (editing(app)) {
		edit_row(app, i, out);
	} else {
		list_row(app, i, out);
	}
}

static void select_custom(WfApp *app, const WfCustomEntry *e)
{
	draft(app)->theme = WF_THEME_CUSTOM;
	snprintf(draft(app)->custom_theme, sizeof draft(app)->custom_theme,
	        "%s", e->id);
	wf_set_preview(app);
}

/* A custom theme that starts as a copy of preset `preset` or custom `from`. */
static void duplicate(WfApp *app, int preset, const WfCustomEntry *from)
{
	WfConfig *c = draft(app);
	WfCustomTheme theme;
	char name[WF_THEME_NAME_MAX], id[WF_THEME_ID_MAX], stem[24];
	const char *source_name;
	const char *source_id;

	if (c->custom_count >= WF_CUSTOM_THEMES) {
		wf_set_note_msg(app, true, MSG_SET_THEME_FULL);
		return;
	}
	if (from) {
		theme = from->theme;
		source_name = from->name;
		source_id = from->id;
	} else {
		wf_custom_init(&theme);
		theme.base = (WfThemeId)preset;
		source_name = wf_theme_display_name((WfThemeId)preset);
		source_id = wf_theme_id_text((WfThemeId)preset);
	}
	snprintf(stem, sizeof stem, "%.14s-copy", source_id);
	wf_config_custom_unique_id(c, stem, id);
	{
		char joined[128];
		size_t prefix =
		        wf_utf8_prefix(source_name, strlen(source_name), 24);
		snprintf(joined, sizeof joined, "%.*s %s", (int)prefix,
		        source_name, wf_msg(MSG_SET_THEME_COPY_SUFFIX));
		wf_utf8_elide(name, sizeof name, joined, SIZE_MAX);
	}
	if (!wf_config_custom_add(c, id, name, &theme)) {
		wf_set_note_msg(app, true, MSG_SET_THEME_FULL);
		return;
	}
	app->settings.edit_theme = c->custom_count - 1;
	app->settings.item = ED_COLOR0;
	app->settings.top = 0;
	select_custom(app, &c->customs[c->custom_count - 1]);
	wf_set_note_msg(app, false, MSG_SET_THEME_DUPLICATED);
}

static void edit_enter(WfApp *app, int index)
{
	app->settings.edit_theme = index;
	app->settings.item = ED_COLOR0;
	app->settings.top = 0;
	select_custom(app, &draft(app)->customs[index]);
	app->settings.note[0] = '\0';
}

bool wf_set_theme_back(WfApp *app)
{
	if (WF_SEC_SETTINGS != app->settings.section ||
	        WF_CAT_APPEARANCE != app->settings.category || !editing(app)) {
		return false;
	}
	app->settings.edit_theme = -1;
	app->settings.item =
	        WF_THEME_PRESET_COUNT + (size_t)draft(app)->custom_count;
	app->settings.top = 0;
	{
		/* the cursor goes back to the theme that was edited */
		const WfCustomEntry *sel =
		        wf_config_custom_selected(cdraft(app));

		if (sel) {
			app->settings.item = WF_THEME_PRESET_COUNT +
			        (size_t)(sel - cdraft(app)->customs);
		}
	}
	app->settings.note[0] = '\0';
	return true;
}

static void color_begin(WfApp *app, size_t slot)
{
	WfSetRow r;
	char hex[8];

	themes_row(app, ED_COLOR0 + slot, &r);
	wf_color_format_hex(r.rgb, hex);
	wf_set_text_begin(app, WF_TEXT_HEX, hex);
	app->settings.previous = edited(app)->theme.color[slot];
}

/* Only a complete HEX value previews. The original token (including "inherit")
 * remains available until Enter commits or Esc cancels the field. */
void wf_set_theme_color_preview(WfApp *app, bool cancel)
{
	WfCustomEntry *e = edited(app);
	size_t slot = app->settings.item - ED_COLOR0;
	uint32_t rgb;
	if (WF_TEXT_HEX != app->settings.text_kind || !e ||
	        slot >= WF_CUSTOM_COUNT) {
		return;
	}
	if (cancel) {
		e->theme.color[slot] = app->settings.previous;
	} else if (wf_color_parse_hex(app->settings.input, &rgb)) {
		e->theme.color[slot] = (int64_t)rgb;
	} else {
		return;
	}
	wf_set_preview(app);
}

static void warn_text(WfApp *app, const WfContrastIssue *issue)
{
	char text[160], ratio[16], minimum[16];

	snprintf(ratio, sizeof ratio, "%.1f", issue->ratio);
	snprintf(minimum, sizeof minimum, "%.1f", issue->minimum);
	(void)wf_msgf(text, sizeof text, MSG_SETTINGS_LOW_CONTRAST,
	        (WfMsgArg[]){WF_ARG_S(wf_msg_reason(issue->what)),
	                WF_ARG_S(ratio), WF_ARG_S(minimum)},
	        3);
	wf_set_note(app, true, text);
}

static bool name_taken(
        const WfConfig *c, const char *name, const WfCustomEntry *except)
{
	int i;

	for (i = 0; i < c->custom_count; i++) {
		if (&c->customs[i] != except &&
		        !strcmp(c->customs[i].name, name)) {
			return true;
		}
	}
	return false;
}

bool wf_set_theme_text_commit(WfApp *app)
{
	int ri = app->settings.rename_index;
	WfCustomEntry *e = WF_TEXT_NAME == app->settings.text_kind && ri >= 0 &&
	                ri < cdraft(app)->custom_count
	        ? &draft(app)->customs[ri]
	        : edited(app);
	size_t slot = app->settings.item - ED_COLOR0;

	if (!e) {
		wf_set_text_end(app);
		return false;
	}
	if (WF_TEXT_NAME == app->settings.text_kind) {
		const char *name = app->settings.input;
		size_t i;

		for (i = 0; name[i]; i++) {
			if ((unsigned char)name[i] < 0x20) {
				break;
			}
		}
		if (!name[0] || name[i]) {
			wf_set_note_msg(app, true, MSG_SET_THEME_NAME_BAD);
			return false;
		}
		if (name_taken(cdraft(app), name, e)) {
			wf_set_note_msg(app, true, MSG_SET_THEME_NAME_TAKEN);
			return false;
		}
		snprintf(e->name, sizeof e->name, "%s", name);
		app->settings.rename_index = -1;
		wf_set_text_end(app);
		return true;
	}
	{
		uint32_t rgb;
		WfContrastIssue issue[8];
		WfThemePalette p;

		if (!wf_color_parse_hex(app->settings.input, &rgb)) {
			wf_set_note_msg(app, true, MSG_SETTINGS_NOT_A_COLOR);
			return false;
		}
		e->theme.color[slot] = (int64_t)rgb;
		wf_set_text_end(app);
		wf_set_preview(app);
		wf_theme_build_custom(&e->theme, &p);
		if (wf_theme_check_contrast(&p, issue, 8) > 0) {
			warn_text(app, &issue[0]);
			app->settings.sub = WF_SS_WARN;
		} else {
			app->settings.note[0] = '\0';
		}
	}
	return true;
}

WfAction wf_set_theme_warn_event(WfApp *app, const WfEvent *ev)
{
	bool keep = WF_EV_CHAR == ev->type && ('y' == ev->ch || 'Y' == ev->ch);
	bool undo =
	        (WF_EV_CHAR == ev->type && ('n' == ev->ch || 'N' == ev->ch)) ||
	        (WF_EV_KEY == ev->type && WF_KEY_ESC == ev->key);
	WfCustomEntry *e = edited(app);

	if (!keep && !undo) {
		return WF_ACT_NONE;
	}
	if (undo && e) {
		e->theme.color[app->settings.item - ED_COLOR0] =
		        app->settings.previous;
		wf_set_preview(app);
	}
	app->settings.sub = WF_SS_BROWSE;
	app->settings.note[0] = '\0';
	return WF_ACT_REDRAW;
}

WfAction wf_set_theme_delete_event(WfApp *app, const WfEvent *ev)
{
	WfConfig *c = draft(app);
	char ans = 0;
	bool yes;

	switch (wf_choice_event(&app->settings.choice, ev, &ans)) {
	case WF_CHOICE_MOVED:  return WF_ACT_REDRAW;
	case WF_CHOICE_IDLE:   return WF_ACT_NONE;
	case WF_CHOICE_ANSWER: break;
	}
	yes = 'y' == ans;
	app->settings.sub = WF_SS_BROWSE;
	if (yes) {
		int index = editing(app)
		        ? app->settings.edit_theme
		        : (int)(app->settings.item - WF_THEME_PRESET_COUNT);

		if (index >= 0 && index < c->custom_count) {
			WfCustomEntry gone = c->customs[index];
			bool was_active =
			        custom_is_active(c, &c->customs[index]);

			(void)wf_config_custom_remove(c, gone.id);
			if (was_active) {
				c->theme = (int)gone.theme.base;
			} else if (WF_THEME_CUSTOM == c->theme &&
			        !wf_config_custom_selected(c)) {
				c->theme = (int)gone.theme.base;
			}
			app->settings.edit_theme = -1;
			{
				size_t n = list_count(app);

				if (app->settings.item >= n) {
					app->settings.item = n - 1;
				}
			}
			wf_set_preview(app);
			wf_set_note_msg(app, false, MSG_SET_THEME_DELETED);
		}
	}
	return WF_ACT_REDRAW;
}

static WfAction list_event(WfApp *app, const WfEvent *ev)
{
	WfConfig *c = draft(app);
	size_t i = app->settings.item, customs = (size_t)c->custom_count;
	bool enter = WF_EV_KEY == ev->type && WF_KEY_ENTER == ev->key;
	int ch = WF_EV_CHAR == ev->type ? (int)ev->ch : 0;
	bool is_preset = i < WF_THEME_PRESET_COUNT;
	bool is_custom = !is_preset && i < WF_THEME_PRESET_COUNT + customs;
	int custom_index = is_custom ? (int)(i - WF_THEME_PRESET_COUNT) : -1;
	bool is_new = i == WF_THEME_PRESET_COUNT + customs;

	if (is_preset && (enter || ' ' == ch)) {
		c->theme = (int)i;
		wf_set_preview(app);
		return WF_ACT_REDRAW;
	}
	if (is_custom && (enter || ' ' == ch)) {
		select_custom(app, &c->customs[custom_index]);
		return WF_ACT_REDRAW;
	}
	if (is_new && enter) {
		const WfCustomEntry *sel = WF_THEME_CUSTOM == c->theme
		        ? wf_config_custom_selected(c)
		        : NULL;

		duplicate(app, sel ? 0 : c->theme, sel);
		return WF_ACT_REDRAW;
	}
	if (!is_preset && !is_custom && !is_new) { /* colour mode */
		int v = c->color_policy, n = 5;

		if (enter || ' ' == ch || '+' == ch) {
			v = (v + 1) % n;
		} else if ('-' == ch ||
		        (WF_EV_KEY == ev->type &&
		                WF_KEY_BACKSPACE == ev->key)) {
			v = (v + n - 1) % n;
		} else {
			return WF_ACT_NONE;
		}
		c->color_policy = v;
		wf_set_preview(app);
		return WF_ACT_REDRAW;
	}
	if ('d' == ch && (is_preset || is_custom)) {
		duplicate(app, is_preset ? (int)i : 0,
		        is_custom ? &c->customs[custom_index] : NULL);
		return WF_ACT_REDRAW;
	}
	if (('e' == ch || (WF_EV_KEY == ev->type && WF_KEY_RIGHT == ev->key)) &&
	        (is_preset || is_custom)) {
		if (is_custom) {
			edit_enter(app, custom_index);
		} else {
			wf_set_note_msg(app, true, MSG_SET_THEME_PRESET_FIXED);
		}
		return WF_ACT_REDRAW;
	}
	if ('n' == ch && is_custom) {
		app->settings.rename_index = custom_index;
		wf_set_text_begin(
		        app, WF_TEXT_NAME, c->customs[custom_index].name);
		return WF_ACT_REDRAW;
	}
	if (('x' == ch ||
	            (WF_EV_KEY == ev->type && WF_KEY_DELETE == ev->key)) &&
	        (is_preset || is_custom)) {
		if (is_preset) {
			wf_set_note_msg(app, true, MSG_SET_THEME_PRESET_FIXED);
		} else {
			wf_set_ask_yes_no(app, WF_SS_DELETE);
		}
		return WF_ACT_REDRAW;
	}
	return WF_ACT_NONE;
}

static WfAction edit_event(WfApp *app, const WfEvent *ev)
{
	WfCustomEntry *e = edited(app);
	size_t i = app->settings.item;
	bool enter = WF_EV_KEY == ev->type && WF_KEY_ENTER == ev->key;
	int ch = WF_EV_CHAR == ev->type ? (int)ev->ch : 0;

	if (!e) {
		return WF_ACT_NONE;
	}
	if ('n' == ch || (ED_NAME == i && enter)) {
		app->settings.rename_index = -1;
		wf_set_text_begin(app, WF_TEXT_NAME, e->name);
		return WF_ACT_REDRAW;
	}
	if ('x' == ch) {
		wf_set_ask_yes_no(app, WF_SS_DELETE);
		return WF_ACT_REDRAW;
	}
	if (ED_BASE == i) {
		int step = 0;

		if (enter || ' ' == ch || '+' == ch) {
			step = 1;
		} else if ('-' == ch ||
		        (WF_EV_KEY == ev->type &&
		                WF_KEY_BACKSPACE == ev->key)) {
			step = -1;
		}
		if (step) {
			int n = WF_THEME_PRESET_COUNT;

			e->theme.base =
			        (WfThemeId)(((int)e->theme.base + step + n) %
			                n);
			wf_set_preview(app);
			return WF_ACT_REDRAW;
		}
		return WF_ACT_NONE;
	}
	if (i >= ED_COLOR0) {
		size_t slot = i - ED_COLOR0;

		if (enter) {
			color_begin(app, slot);
			return WF_ACT_REDRAW;
		}
		if (WF_EV_KEY == ev->type &&
		        (WF_KEY_DELETE == ev->key ||
		                WF_KEY_BACKSPACE == ev->key)) {
			e->theme.color[slot] = -1; /* the base theme's own */
			wf_set_preview(app);
			return WF_ACT_REDRAW;
		}
	}
	return WF_ACT_NONE;
}

static WfAction themes_event(WfApp *app, const WfEvent *ev)
{
	return editing(app) ? edit_event(app, ev) : list_event(app, ev);
}

static const char *themes_help(const WfApp *app)
{
	if (editing(app)) {
		return app->settings.item >= ED_COLOR0
		        ? wf_msg(MSG_SET_THEME_HELP_COLOR)
		        : "";
	}
	return wf_msg(MSG_SET_THEME_HELP_LIST);
}

static WfMsg themes_hint(const WfApp *app)
{
	return editing(app) ? MSG_SET_HINT_COLORS : MSG_SET_HINT_THEMES;
}

const WfSetPage WF_SETPAGE_THEMES = {
        themes_count, themes_row, themes_event, themes_help, themes_hint};
