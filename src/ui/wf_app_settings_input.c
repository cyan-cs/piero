/* The settings screen: the text field, a number, and the values of a choice. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../edit/wf_ekeymap.h"
#include "../text/wf_text.h"
#include "wf_app_settings_priv.h"
#include "../base/wf_macro.h"

void wf_set_text_begin(WfApp *app, int kind, const char *initial)
{
	app->settings.text_kind = kind;
	snprintf(app->settings.input, sizeof app->settings.input, "%s",
	        initial ? initial : "");
	app->settings.input_n = strlen(app->settings.input);
	app->settings.input_cur = app->settings.input_n;
	app->settings.input_fresh = WF_TEXT_HEX == kind;
	app->settings.sub = WF_SS_TEXT;
	app->settings.note[0] = '\0';
}

void wf_set_text_end(WfApp *app)
{
	app->settings.sub = WF_SS_BROWSE;
}

static bool hex_char(uint32_t c)
{
	return '#' == c || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
	        (c >= 'A' && c <= 'F');
}

static void number_commit(WfApp *app);

/* A text field of the settings is being edited: pasted characters are text. */
bool wf_app_settings_takes_text(const WfApp *app)
{
	return &WF_MODE_SETTINGS == app->mode &&
	        WF_SS_TEXT == app->settings.sub;
}

/* The start of the character (a letter with its accents counts as one) that
 * ends at byte `at`. */
static size_t char_before(const char *s, size_t n, size_t at)
{
	return wf_utf8_cluster_prev(s, n, at);
}

/* The end of the character that starts at byte `at`. */
size_t wf_set_char_after(const char *s, size_t n, size_t at)
{
	return wf_utf8_cluster_next(s, n, at);
}

WfAction wf_set_text_event(WfApp *app, const WfEvent *ev)
{
	char *in = app->settings.input;
	size_t *n = &app->settings.input_n;
	size_t *cur = &app->settings.input_cur;
	size_t max = WF_TEXT_HEX == app->settings.text_kind ? 7
	        : WF_TEXT_NUMBER == app->settings.text_kind ? 6
	        : WF_TEXT_NAME == app->settings.text_kind
	        ? WF_THEME_NAME_MAX - 1
	        : sizeof app->settings.input - 1;

	if (WF_EV_CHAR == ev->type) {
		uint32_t c = ev->ch;
		char enc[4];
		size_t k;

		if (c < 0x20 || 0x7F == c) {
			return WF_ACT_REDRAW;
		}
		if (WF_TEXT_HEX == app->settings.text_kind && !hex_char(c)) {
			return WF_ACT_REDRAW;
		}
		if (WF_TEXT_NUMBER == app->settings.text_kind &&
		        (c < '0' || c > '9')) {
			return WF_ACT_REDRAW;
		}
		if (app->settings.input_fresh) {
			*n = 0;
			*cur = 0;
			in[0] = '\0';
			app->settings.input_fresh = false;
		}
		k = wf_utf8_encode(c, enc);
		if (k && *n + k <= max) {
			memmove(in + *cur + k, in + *cur, *n - *cur + 1);
			memcpy(in + *cur, enc, k);
			*n += k;
			*cur += k;
		} else if (k) {
			char text[96];

			/* said, not silently dropped */
			(void)wf_msgf(text, sizeof text, MSG_SET_TEXT_FULL,
			        &WF_ARG_N(max), 1);
			wf_set_note(app, true, text);
		}
		wf_set_theme_color_preview(app, false);
		return WF_ACT_REDRAW;
	}
	if (WF_EV_KEY != ev->type) {
		return WF_ACT_NONE;
	}
	switch (ev->key) {
	case WF_KEY_BACKSPACE: {
		size_t from = char_before(in, *n, *cur);

		app->settings.input_fresh = false;
		memmove(in + from, in + *cur, *n - *cur + 1);
		*n -= *cur - from;
		*cur = from;
		wf_set_theme_color_preview(app, false);
		return WF_ACT_REDRAW;
	}
	case WF_KEY_DELETE: {
		size_t to = wf_set_char_after(in, *n, *cur);

		app->settings.input_fresh = false;
		memmove(in + *cur, in + to, *n - to + 1);
		*n -= to - *cur;
		wf_set_theme_color_preview(app, false);
		return WF_ACT_REDRAW;
	}
	case WF_KEY_LEFT:
		app->settings.input_fresh = false;
		*cur = char_before(in, *n, *cur);
		return WF_ACT_REDRAW;
	case WF_KEY_RIGHT:
		app->settings.input_fresh = false;
		*cur = wf_set_char_after(in, *n, *cur);
		return WF_ACT_REDRAW;
	case WF_KEY_HOME:
		app->settings.input_fresh = false;
		*cur = 0;
		return WF_ACT_REDRAW;
	case WF_KEY_END:
		app->settings.input_fresh = false;
		*cur = *n;
		return WF_ACT_REDRAW;
	case WF_KEY_ENTER:
		switch (app->settings.text_kind) {
		case WF_TEXT_SEARCH: wf_set_keys_search_commit(app); break;
		case WF_TEXT_KEYS:   (void)wf_set_keys_text_commit(app); break;
		case WF_TEXT_NUMBER: number_commit(app); break;
		default:             (void)wf_set_theme_text_commit(app); break;
		}
		return WF_ACT_REDRAW;
	case WF_KEY_ESC:
		wf_set_theme_color_preview(app, true);
		wf_set_text_end(app);
		app->settings.note[0] = '\0';
		return WF_ACT_REDRAW;
	default: return WF_ACT_NONE;
	}
}

/* Enter in the number field: a whole number inside the item's range, else the
 * field stays open and says what is allowed. */
static void number_commit(WfApp *app)
{
	const Item *it = wf_set_item_at(app, app->settings.item);
	const char *in = app->settings.input;
	long v = 0;

	if (it && K_INT == it->kind && app->settings.input_n) {
		for (; *in; in++) {
			v = v * 10 + (*in - '0');
			if (v > it->max) {
				break;
			}
		}
	}
	if (!it || !app->settings.input_n || v < it->min || v > it->max) {
		char text[96];

		if (it) {
			(void)wf_msgf(text, sizeof text, MSG_SET_NUMBER_RANGE,
			        (WfMsgArg[]){
			                WF_ARG_N(it->min), WF_ARG_N(it->max)},
			        2);
			wf_set_note(app, true, text);
		}
		return;
	}
	wf_set_put_int(&app->settings.draft, it, (int)v);
	wf_set_text_end(app);
	app->settings.note[0] = '\0';
}

WfAction wf_set_pick_event(WfApp *app, const WfEvent *ev)
{
	const Item *it = wf_set_item_at(app, app->settings.item);
	int n = it && K_CHOICE == it->kind ? wf_set_choice_count(it->choices)
	                                   : 0;
	int *cur = &app->settings.pick;
	int page = wf_set_view_rows(app) > 2 ? wf_set_view_rows(app) - 2 : 1;

	if (!n) {
		app->settings.sub = WF_SS_BROWSE;
		return WF_ACT_REDRAW;
	}
	if (WF_EV_KEY != ev->type) {
		return WF_ACT_NONE;
	}
	switch (ev->key) {
	case WF_KEY_UP:   *cur = *cur > 0 ? *cur - 1 : 0; break;
	case WF_KEY_DOWN: *cur = *cur + 1 < n ? *cur + 1 : n - 1; break;
	case WF_KEY_PGUP: *cur = *cur > page ? *cur - page : 0; break;
	case WF_KEY_PGDN: *cur = *cur + page < n ? *cur + page : n - 1; break;
	case WF_KEY_HOME: *cur = 0; break;
	case WF_KEY_END:  *cur = n - 1; break;
	case WF_KEY_ENTER:
		wf_set_put_int(&app->settings.draft, it, *cur);
		if (MSG_SETTINGS_ITEM_LANGUAGE == it->label) {
			wf_set_preview(app);
		}
		app->settings.sub = WF_SS_BROWSE;
		break;
	case WF_KEY_LEFT:
	case WF_KEY_ESC:  app->settings.sub = WF_SS_BROWSE; break;
	default:          return WF_ACT_NONE;
	}
	return WF_ACT_REDRAW;
}
