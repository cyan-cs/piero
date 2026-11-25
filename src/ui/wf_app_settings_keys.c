/*
 * Settings > Keyboard: the profile, and the list of every command with its
 * keys. The list is made from the command table and the keymap the draft
 * would give, so it cannot disagree with what the keys do. Only the Custom
 * profile can be changed; a change is checked as a whole (the same checks as
 * loading the file) before the draft takes it.
 *
 * Giving keys never runs anything: while the screen waits for a key, that key
 * is only recorded.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../edit/wf_ekeymap.h"
#include "wf_app_settings_priv.h"

enum
{
	KR_PROFILE,
	KR_BASE,
	KR_RESET_ALL,
	KR_HEADER,
	KR_CMD
};

typedef struct
{
	uint8_t kind;
	uint8_t ctx;
	uint16_t cmd;
} KRow;

#define KROWS 480u

/* What a command is for, so that a key can be found by its job. */
typedef enum
{
	KG_MOVE,
	KG_SELECT,
	KG_FIND,
	KG_VIEW,
	KG_FILES,
	KG_EDIT,
	KG_BUFFERS,
	KG_APP,
	KG_TABS,
	KG_COUNT
} KGroup;

static const WfMsg group_names[KG_COUNT] = {MSG_KG_MOVE, MSG_KG_SELECT,
        MSG_KG_FIND, MSG_KG_VIEW, MSG_KG_FILES, MSG_KG_EDIT, MSG_KG_BUFFERS,
        MSG_KG_APP, MSG_KG_TABS};

static KGroup group_of(uint16_t cmd)
{
	switch (cmd) {
	case WF_FM_NAV_UP:
	case WF_FM_NAV_DOWN:
	case WF_FM_NAV_PARENT:
	case WF_FM_NAV_ENTER:
	case WF_FM_NAV_RIGHT:
	case WF_FM_NAV_OPEN:
	case WF_FM_NAV_FIRST:
	case WF_FM_NAV_LAST:
	case WF_FM_HISTORY_BACK:
	case WF_FM_HISTORY_FORWARD:
	case WF_PV_UP:
	case WF_PV_DOWN:
	case WF_PV_PAN_LEFT:
	case WF_PV_PAN_RIGHT:
	case WF_PV_BACK:
	case WF_PV_EDGE_LEFT:
	case WF_PV_EDGE_RIGHT:
	case WF_PV_TOP:
	case WF_PV_BOTTOM:
	case WF_CMD_LEFT:
	case WF_CMD_RIGHT:
	case WF_CMD_UP:
	case WF_CMD_DOWN:
	case WF_CMD_HOME:
	case WF_CMD_END:
	case WF_CMD_PGUP:
	case WF_CMD_PGDN:
	case WF_CMD_DOC_START:
	case WF_CMD_DOC_END:
	case WF_CMD_WORD_LEFT:
	case WF_CMD_WORD_RIGHT:
	case WF_CMD_GOTO:           return KG_MOVE;
	case WF_FM_SELECT:
	case WF_FM_CANCEL:
	case WF_CMD_SELECT_ALL:
	case WF_CMD_MARK:           return KG_SELECT;
	case WF_FM_SMART_GO:
	case WF_FM_SMART_GO_PATH:
	case WF_FM_FILTER:
	case WF_FM_CMDLINE:
	case WF_CMD_FIND:
	case WF_CMD_FIND_BACK:
	case WF_CMD_FIND_NEXT:
	case WF_CMD_FIND_PREV:
	case WF_CMD_REPLACE:        return KG_FIND;
	case WF_FM_HIDDEN:
	case WF_FM_RELOAD:
	case WF_FM_INFO:
	case WF_FM_PREVIEW_FULL:
	case WF_FM_PREVIEW_LOAD:
	case WF_FM_SORT:
	case WF_FM_DIRSIZE:
	case WF_PV_CLOSE:
	case WF_PV_TOGGLE_FULL:
	case WF_PV_LOAD:            return KG_VIEW;
	case WF_FM_CREATE:
	case WF_FM_CREATE_FILE:
	case WF_FM_CREATE_FOLDER:
	case WF_FM_RENAME:
	case WF_FM_RECYCLE:
	case WF_FM_COPY_STAGE:
	case WF_FM_MOVE_STAGE:
	case WF_FM_PASTE:
	case WF_FM_CLIP_PASTE:
	case WF_FM_LOCKS:
	case WF_FM_CLIP_COPY:
	case WF_FM_FUZZY:
	case WF_FM_BULK_RENAME:
	case WF_FM_HASH:
	case WF_FM_DUPES:
	case WF_FM_TERMINAL:
	case WF_FM_UNDO:
	case WF_FM_JOBLOG:
	case WF_FM_BACKUPS:
	case WF_FM_EDIT:
	case WF_FM_QUEUE_PAUSE:
	case WF_FM_QUEUE_CANCEL:
	case WF_PV_EDIT:            return KG_FILES;
	case WF_FM_TAB_NEW:
	case WF_FM_TAB_CLOSE:
	case WF_FM_TAB_PREV:
	case WF_FM_TAB_NEXT:
	case WF_FM_PLACE_SET:
	case WF_FM_PLACE_GO:        return KG_TABS;
	case WF_CMD_SAVE:
	case WF_CMD_SAVE_AS:
	case WF_CMD_EXIT:
	case WF_CMD_INSERT_FILE:
	case WF_CMD_OPEN_FILE:
	case WF_CMD_MD_PREVIEW:
	case WF_CMD_NEXT_BUFFER:
	case WF_CMD_PREV_BUFFER:    return KG_BUFFERS;
	case WF_FM_SETTINGS:
	case WF_FM_HELP:
	case WF_FM_QUIT:
	case WF_CMD_HELP:
	case WF_CMD_CANCEL:
	case WF_CMD_VIM_CMDLINE:    return KG_APP;
	default:                    return KG_EDIT;
	}
}

static int legacy_id(const WfApp *app)
{
	return 1 == app->settings.draft.editor_keymap ? WF_KM_NANO
	                                              : WF_KM_PIERO;
}

/* The base keys the draft's profile stands on. */
static WfBase base_of(const WfKeyConfig *k)
{
	if (WF_PROFILE_VIM == k->profile) {
		return WF_BASE_VIM;
	}
	if (WF_PROFILE_CUSTOM == k->profile) {
		return WF_BASE_VIM == k->base ? WF_BASE_VIM : WF_BASE_NANO;
	}
	return WF_BASE_NANO;
}

void wf_set_keys_free(WfApp *app)
{
	wf_keymap_free(app->settings.kmap);
	app->settings.kmap = NULL;
}

/* The draft's keys have changed: the list follows. A draft that cannot be
 * built (a hand-edited file) shows the profile's own keys. */
void wf_set_keys_refresh(WfApp *app)
{
	WfKeymap *m = NULL;
	WfKeyIssue issue;
	WfStatus st = wf_keymap_build(
	        &app->settings.draft.keys, legacy_id(app), &m, &issue);

	if (WF_OK != st) {
		st = wf_keymap_build_defaults(
		        &app->settings.draft.keys, legacy_id(app), &m);
	}
	wf_set_keys_free(app);
	if (WF_OK == st) {
		app->settings.kmap = m;
	}
}

void wf_key_issue_text(const WfKeyIssue *issue, char *out, size_t cap)
{
	const WfCommandInfo *other = wf_cmd_info(issue->other);
	WfMsgArg args[1];

	switch (issue->kind) {
	case WF_KI_NONE: out[0] = '\0'; return;
	case WF_KI_BAD_KEY:
		args[0] = WF_ARG_S(issue->why ? wf_msg_reason(issue->why) : "");
		(void)wf_msgf(out, cap, MSG_KEYISSUE_BAD_KEY, args, 1);
		return;
	case WF_KI_SAME_KEYS:
		args[0] = WF_ARG_S(other ? wf_msg(other->desc) : "?");
		(void)wf_msgf(out, cap, MSG_KEYISSUE_SAME, args, 1);
		return;
	case WF_KI_PREFIX:
		args[0] = WF_ARG_S(other ? wf_msg(other->desc) : "?");
		(void)wf_msgf(out, cap, MSG_KEYISSUE_PREFIX, args, 1);
		return;
	case WF_KI_COUNT_DIGIT:
		snprintf(out, cap, "%s", wf_msg(MSG_KEYISSUE_DIGIT));
		return;
	case WF_KI_REQUIRED:
		args[0] = WF_ARG_S(wf_cmd_info(issue->cmd)
		                ? wf_msg(wf_cmd_info(issue->cmd)->desc)
		                : "?");
		(void)wf_msgf(out, cap, MSG_KEYISSUE_REQUIRED, args, 1);
		return;
	case WF_KI_UNKNOWN:
		snprintf(out, cap, "%s", wf_msg(MSG_KEYISSUE_UNKNOWN));
		return;
	}
}

static bool contains_fold(const char *hay, const char *needle)
{
	size_t n = strlen(needle), h = strlen(hay), i, k;

	if (!n) {
		return true;
	}
	for (i = 0; i + n <= h; i++) {
		for (k = 0; k < n; k++) {
			char a = hay[i + k], b = needle[k];

			if (a >= 'A' && a <= 'Z') {
				a = (char)(a - 'A' + 'a');
			}
			if (b >= 'A' && b <= 'Z') {
				b = (char)(b - 'A' + 'a');
			}
			if (a != b) {
				break;
			}
		}
		if (k == n) {
			return true;
		}
	}
	return false;
}

static size_t editor_contexts(const WfKeyConfig *k, WfKeyContext *out)
{
	if (WF_BASE_VIM == base_of(k) && WF_PROFILE_LEGACY != k->profile &&
	        WF_PROFILE_NANO != k->profile) {
		out[0] = WF_CTX_VIM_NORMAL;
		out[1] = WF_CTX_VIM_INSERT;
		return 2;
	}
	out[0] = WF_CTX_EDIT;
	return 1;
}

static bool matches(const WfApp *app, WfKeyContext ctx, const WfCommandInfo *ci)
{
	char keys[64];

	if (!app->settings.search[0]) {
		return true;
	}
	if (contains_fold(wf_msg(ci->desc), app->settings.search) ||
	        contains_fold(ci->name, app->settings.search)) {
		return true;
	}
	keys[0] = '\0';
	(void)wf_keymap_keys_text(
	        app->settings.kmap, ctx, ci->id, keys, sizeof keys);
	return contains_fold(keys, app->settings.search);
}

static size_t build_rows(const WfApp *app, KRow *rows, size_t cap)
{
	const WfKeyConfig *k = &app->settings.draft.keys;
	WfKeyContext ctxs[4];
	size_t nctx = 0, n = 0, c;
	bool custom = WF_PROFILE_CUSTOM == k->profile;

	if (!app->settings.search[0]) {
		rows[n++] = (KRow){KR_PROFILE, 0, 0};
		if (custom) {
			rows[n++] = (KRow){KR_BASE, 0, 0};
			rows[n++] = (KRow){KR_RESET_ALL, 0, 0};
		}
	}
	ctxs[nctx++] = WF_CTX_FM;
	ctxs[nctx++] = WF_CTX_PREVIEW;
	{
		WfKeyContext ed[2];
		size_t ne = editor_contexts(k, ed), i;

		for (i = 0; i < ne; i++) {
			ctxs[nctx++] = ed[i];
		}
	}
	for (c = 0; c < nctx; c++) {
		const WfCommandInfo *list[160];
		size_t total = wf_cmd_list(ctxs[c], list, 160), i;
		int g;

		/* one block per job, so a key is found by what it is for */
		for (g = 0; g < KG_COUNT; g++) {
			size_t header_at = n;
			bool any = false;

			if (n < cap) {
				rows[n++] = (KRow){KR_HEADER, (uint8_t)ctxs[c],
				        (uint16_t)g};
			}
			for (i = 0; i < total && i < 160 && n < cap; i++) {
				if (group_of(list[i]->id) == (KGroup)g &&
				        matches(app, ctxs[c], list[i])) {
					rows[n++] = (KRow){KR_CMD,
					        (uint8_t)ctxs[c], list[i]->id};
					any = true;
				}
			}
			if (!any) {
				n = header_at; /* nothing under that header */
			}
		}
	}
	return n;
}

static size_t keys_count(const WfApp *app)
{
	KRow rows[KROWS];

	return build_rows(app, rows, KROWS);
}

static const char *profile_name(const WfKeyConfig *k, char *buf, size_t cap)
{
	switch (k->profile) {
	case WF_PROFILE_NANO: return wf_msg(MSG_PROFILE_NANO);
	case WF_PROFILE_VIM:  return wf_msg(MSG_PROFILE_VIM);
	case WF_PROFILE_CUSTOM:
		if (WF_BASE_NANO == k->base &&
		        WF_BINDINGS_COMPAT == k->version) {
			return wf_msg(MSG_CUSTOM_COMPAT);
		}
		(void)wf_msgf(buf, cap, MSG_PROFILE_CUSTOM_BASED,
		        &WF_ARG_S(wf_msg(WF_BASE_VIM == k->base
		                        ? MSG_PROFILE_VIM
		                        : MSG_PROFILE_NANO)),
		        1);
		return buf;
	default: return wf_msg(MSG_PROFILE_STANDARD);
	}
}

static void keys_row(const WfApp *app, size_t i, WfSetRow *out)
{
	KRow rows[KROWS];
	size_t n = build_rows(app, rows, KROWS);
	const WfKeyConfig *k = &app->settings.draft.keys;
	KRow r;

	memset(out, 0, sizeof *out);
	if (i >= n) {
		return;
	}
	r = rows[i];
	switch (r.kind) {
	case KR_PROFILE: {
		char buf[64];

		snprintf(out->label, sizeof out->label, "%s",
		        wf_msg(MSG_SET_KEYS_PROFILE));
		snprintf(out->value, sizeof out->value, "%s",
		        profile_name(k, buf, sizeof buf));
		out->cycle = true;
		break;
	}
	case KR_BASE:
		snprintf(out->label, sizeof out->label, "%s",
		        wf_msg(MSG_SET_KEYS_BASE));
		snprintf(out->value, sizeof out->value, "%s",
		        wf_msg(WF_BASE_VIM == k->base ? MSG_PROFILE_VIM
		                                      : MSG_PROFILE_NANO));
		out->cycle = true;
		break;
	case KR_RESET_ALL:
		snprintf(out->label, sizeof out->label, "%s",
		        wf_msg(MSG_SET_KEYS_RESET_ALL));
		break;
	case KR_HEADER:
		snprintf(out->label, sizeof out->label, "%s > %s",
		        wf_msg(wf_context_label((WfKeyContext)r.ctx)),
		        wf_msg(group_names[r.cmd < KG_COUNT ? r.cmd : 0]));
		out->header = true;
		break;
	case KR_CMD: {
		const WfCommandInfo *ci = wf_cmd_info(r.cmd);
		char keys[64];

		keys[0] = '\0';
		(void)wf_keymap_keys_text(app->settings.kmap,
		        (WfKeyContext)r.ctx, r.cmd, keys, sizeof keys);
		snprintf(out->label, sizeof out->label, "%s",
		        ci ? wf_msg(ci->desc) : "?");
		snprintf(out->value, sizeof out->value, "%s%s",
		        keys[0] ? keys : wf_msg(MSG_COMMON_NONE),
		        WF_PROFILE_CUSTOM == k->profile &&
		                        wf_keyconfig_find(
		                                k, (WfKeyContext)r.ctx, r.cmd)
		                ? " *"
		                : "");
		break;
	}
	}
}

static KRow row_at(const WfApp *app, size_t i)
{
	KRow rows[KROWS];
	size_t n = build_rows(app, rows, KROWS);

	return i < n ? rows[i] : (KRow){KR_HEADER, 0, 0};
}

/* Takes `cand` as the draft's keys when the whole set builds; says why not
 * otherwise. */
static bool adopt(WfApp *app, const WfKeyConfig *cand)
{
	WfKeymap *m = NULL;
	WfKeyIssue issue;
	WfStatus st = wf_keymap_build(cand, legacy_id(app), &m, &issue);
	char text[112];

	if (WF_OK != st) {
		if (WF_E_INVAL == st) {
			wf_key_issue_text(&issue, text, sizeof text);
			wf_set_note(app, true, text);
		} else {
			wf_set_note(app, true, wf_status_name(st));
		}
		return false;
	}
	wf_keymap_free(m);
	app->settings.draft.keys = *cand;
	wf_set_keys_refresh(app);
	return true;
}

/* Gives (ctx, cmd) the keys of `o` (replacing its override); an override
 * that says what the base already does is dropped. */
static bool assign(WfApp *app, WfKeyContext ctx, uint16_t cmd, WfKeyOverride *o)
{
	WfKeyConfig *cand = malloc(sizeof *cand);
	bool ok, switched = false;

	if (!cand) {
		wf_set_note(app, true, wf_status_name(WF_E_NOMEM));
		return false;
	}
	*cand = app->settings.draft.keys;
	if (WF_PROFILE_CUSTOM != cand->profile) {
		/* any profile can be changed: it becomes Custom, on its own keys */
		cand->base = WF_PROFILE_VIM == cand->profile ? WF_BASE_VIM
		                                             : WF_BASE_NANO;
		cand->version = WF_PROFILE_LEGACY == cand->profile
		        ? WF_BINDINGS_COMPAT
		        : WF_BINDINGS_CURRENT;
		cand->profile = WF_PROFILE_CUSTOM;
		switched = true;
	}
	o->ctx = (uint8_t)ctx;
	o->cmd = cmd;
	/* Keep explicit choices even when they match a newer base's defaults.
	 * An older Custom profile may deliberately retain Standard navigation. */
	if (!wf_keyconfig_set(cand, o)) {
		wf_set_note_msg(app, true, MSG_KEYISSUE_FULL);
		free(cand);
		return false;
	}
	ok = adopt(app, cand);
	if (ok && switched) {
		char text[112];

		/* the Base and Restore rows appeared above the list */
		if (!app->settings.search[0]) {
			app->settings.item += 2;
		}
		wf_set_scroll_fit(app);
		(void)wf_msgf(text, sizeof text, MSG_SET_KEYS_TO_CUSTOM,
		        &WF_ARG_S(wf_msg(WF_BASE_VIM == cand->base
		                        ? MSG_PROFILE_VIM
		                        : MSG_PROFILE_NANO)),
		        1);
		wf_set_note(app, false, text);
	} else if (ok) {
		wf_set_note_msg(app, false, MSG_SET_KEYS_CHANGED);
	}
	free(cand);
	return ok;
}

static void capture_begin(WfApp *app, KRow r, bool add)
{
	app->settings.kb_ctx = r.ctx;
	app->settings.kb_cmd = r.cmd;
	app->settings.kb_add = add;
	app->settings.sub = WF_SS_CAPTURE;
	app->settings.note[0] = '\0';
}

/* The keys the command has now, as the override they would be. */
static WfKeyOverride current_of(const WfApp *app, KRow r)
{
	WfKeyOverride o;
	char text[64];

	memset(&o, 0, sizeof o);
	text[0] = '\0';
	(void)wf_keymap_keys_text(app->settings.kmap, (WfKeyContext)r.ctx,
	        r.cmd, text, sizeof text);
	if (!wf_keys_parse_list(text, &o)) {
		memset(&o, 0, sizeof o);
	}
	return o;
}

WfAction wf_set_keys_capture(WfApp *app, const WfEvent *ev)
{
	WfKeyContext ctx = (WfKeyContext)app->settings.kb_ctx;
	WfStroke s;
	WfKeyOverride o;
	WfSeq seq;
	WfKeyIssue issue;
	bool was_custom;
	KRow r = {KR_CMD, app->settings.kb_ctx, app->settings.kb_cmd};

	if (WF_EV_KEY == ev->type && WF_KEY_ESC == ev->key) {
		app->settings.sub =
		        WF_SS_BROWSE; /* Esc cancels; it is not bound */
		return WF_ACT_REDRAW;
	}
	if (!wf_stroke_from_event(ev, &s)) {
		return WF_ACT_NONE; /* a key the terminal reports but nothing names */
	}
	memset(&seq, 0, sizeof seq);
	seq.s[0] = s;
	seq.n = 1;
	issue = wf_keymap_check(app->settings.kmap, ctx, r.cmd, &seq);
	if (WF_KI_NONE != issue.kind) {
		char text[112];

		wf_key_issue_text(&issue, text, sizeof text);
		wf_set_note(app, true, text);
		app->settings.sub = WF_SS_BROWSE;
		return WF_ACT_REDRAW;
	}
	if (app->settings.kb_add) {
		o = current_of(app, r);
		if (o.n >= 2) {
			wf_set_note_msg(app, true, MSG_KEYISSUE_FULL);
			app->settings.sub = WF_SS_BROWSE;
			return WF_ACT_REDRAW;
		}
		o.seq[o.n++] = seq;
	} else {
		memset(&o, 0, sizeof o);
		o.seq[0] = seq;
		o.n = 1;
	}
	app->settings.sub = WF_SS_BROWSE;
	was_custom = WF_PROFILE_CUSTOM == app->settings.draft.keys.profile;
	if (assign(app, ctx, r.cmd, &o) && 1 == o.n) {
		/* room for a second key: keep listening; Esc ends it and the
		 * first key stays */
		app->settings.kb_add = true;
		app->settings.sub = WF_SS_CAPTURE;
		if (was_custom) {
			wf_set_note_msg(app, false, MSG_SET_KEYS_SECOND);
		}
	}
	return WF_ACT_REDRAW;
}

bool wf_set_keys_text_commit(WfApp *app)
{
	WfKeyOverride o;
	KRow r = {KR_CMD, app->settings.kb_ctx, app->settings.kb_cmd};
	const WfCommandInfo *ci = wf_cmd_info(r.cmd);
	bool ok;

	memset(&o, 0, sizeof o);
	if (!wf_keys_parse_list(app->settings.input, &o)) {
		wf_set_note_msg(app, true, MSG_KEYISSUE_PARSE);
		return false;
	}
	if (!o.n && ci && (ci->flags & WF_CF_REQUIRED)) {
		wf_set_note_msg(app, true, MSG_KEYISSUE_NEEDS_KEY);
		return false;
	}
	ok = assign(app, (WfKeyContext)r.ctx, r.cmd, &o);
	if (ok) {
		wf_set_text_end(app);
	}
	return ok;
}

void wf_set_keys_search_commit(WfApp *app)
{
	snprintf(app->settings.search, sizeof app->settings.search, "%s",
	        app->settings.input);
	wf_set_text_end(app);
	app->settings.item = 0;
	app->settings.top = 0;
	/* the first row that is not a header */
	{
		KRow rows[KROWS];
		size_t n = build_rows(app, rows, KROWS), i;

		for (i = 0; i < n; i++) {
			if (KR_HEADER != rows[i].kind) {
				app->settings.item = i;
				break;
			}
		}
	}
}

void wf_set_keys_reset_all(WfApp *app)
{
	WfKeyConfig *cand = malloc(sizeof *cand);

	if (!cand) {
		return;
	}
	*cand = app->settings.draft.keys;
	cand->count = 0;
	if (adopt(app, cand)) {
		wf_set_note_msg(app, false, MSG_SET_KEYS_RESET_DONE);
	}
	free(cand);
}

static void cycle_profile(WfApp *app, int dir)
{
	WfKeyConfig k = app->settings.draft.keys;
	int p = (k.profile + dir + 4) % 4;

	if (WF_PROFILE_CUSTOM == p && WF_PROFILE_CUSTOM != k.profile) {
		/* Custom starts from the keys that are in use */
		k.base = WF_PROFILE_VIM == k.profile ? WF_BASE_VIM
		                                     : WF_BASE_NANO;
		k.version = WF_PROFILE_LEGACY == k.profile
		        ? WF_BINDINGS_COMPAT
		        : WF_BINDINGS_CURRENT;
	}
	k.profile = p;
	if (adopt(app, &k)) {
		app->settings.note[0] = '\0';
	}
}

static void cycle_base(WfApp *app)
{
	WfKeyConfig k = app->settings.draft.keys;

	k.base = WF_BASE_VIM == k.base ? WF_BASE_NANO : WF_BASE_VIM;
	k.version = WF_BINDINGS_CURRENT;
	(void)adopt(app, &k);
}

static WfAction keys_event(WfApp *app, const WfEvent *ev)
{
	KRow r = row_at(app, app->settings.item);
	const WfKeyConfig *k = &app->settings.draft.keys;
	bool enter = WF_EV_KEY == ev->type && WF_KEY_ENTER == ev->key;
	int ch = WF_EV_CHAR == ev->type ? (int)ev->ch : 0;

	if ('/' == ch) {
		wf_set_text_begin(app, WF_TEXT_SEARCH, app->settings.search);
		return WF_ACT_REDRAW;
	}
	if (WF_EV_KEY == ev->type &&
	        (WF_KEY_PGDN == ev->key || WF_KEY_PGUP == ev->key)) {
		/* from block to block: the first command after the next header */
		KRow all[KROWS];
		size_t n = build_rows(app, all, KROWS), at = app->settings.item,
		       i;
		bool down = WF_KEY_PGDN == ev->key;

		if (down) {
			for (i = at + 1; i < n; i++) {
				if (KR_HEADER == all[i].kind && i + 1 < n) {
					app->settings.item = i + 1;
					break;
				}
			}
		} else {
			size_t here = at;

			while (here > 0 && KR_HEADER != all[here].kind) {
				here--;
			}
			for (i = here; i > 0; i--) {
				if (KR_HEADER == all[i - 1].kind) {
					app->settings.item = i;
					break;
				}
			}
			if (0 == i) { /* no block above: the top of the page */
				app->settings.item = 0;
			}
		}
		wf_set_scroll_fit(app);
		return WF_ACT_REDRAW;
	}
	if (WF_EV_KEY == ev->type && WF_KEY_ESC == ev->key &&
	        app->settings.search[0]) {
		app->settings.search[0] =
		        '\0'; /* Esc first clears the search */
		app->settings.item = 0;
		app->settings.top = 0;
		return WF_ACT_REDRAW;
	}
	switch (r.kind) {
	case KR_PROFILE:
		if (enter || ' ' == ch || '+' == ch) {
			cycle_profile(app, 1);
			return WF_ACT_REDRAW;
		}
		if ('-' == ch ||
		        (WF_EV_KEY == ev->type &&
		                WF_KEY_BACKSPACE == ev->key)) {
			cycle_profile(app, -1);
			return WF_ACT_REDRAW;
		}
		return WF_ACT_NONE;
	case KR_BASE:
		if (enter || ' ' == ch || '+' == ch || '-' == ch) {
			cycle_base(app);
			return WF_ACT_REDRAW;
		}
		return WF_ACT_NONE;
	case KR_RESET_ALL:
		if (enter) {
			wf_set_ask_yes_no(app, WF_SS_RESET_ALL);
			return WF_ACT_REDRAW;
		}
		return WF_ACT_NONE;
	case KR_CMD: break;
	default:     return WF_ACT_NONE;
	}
	/* a command: every profile can be changed (the first change makes it
	 * Custom); there is nothing to put back until then */
	if (WF_PROFILE_CUSTOM != k->profile &&
	        ('r' == ch ||
	                (WF_EV_KEY == ev->type && WF_KEY_DELETE == ev->key))) {
		return WF_ACT_NONE;
	}
	if (enter) {
		capture_begin(app, r, false);
		return WF_ACT_REDRAW;
	}
	if ('a' == ch) {
		capture_begin(app, r, true);
		return WF_ACT_REDRAW;
	}
	if ('e' == ch) {
		char text[64];

		text[0] = '\0';
		(void)wf_keymap_keys_text(app->settings.kmap,
		        (WfKeyContext)r.ctx, r.cmd, text, sizeof text);
		app->settings.kb_ctx = r.ctx;
		app->settings.kb_cmd = r.cmd;
		wf_set_text_begin(app, WF_TEXT_KEYS, text);
		return WF_ACT_REDRAW;
	}
	if ('r' == ch || (WF_EV_KEY == ev->type && WF_KEY_DELETE == ev->key)) {
		WfKeyConfig *cand = malloc(sizeof *cand);

		if (cand) {
			*cand = *k;
			(void)wf_keyconfig_remove(
			        cand, (WfKeyContext)r.ctx, r.cmd);
			if (adopt(app, cand)) {
				wf_set_note_msg(
				        app, false, MSG_SET_KEYS_RESET_ONE);
			}
			free(cand);
		}
		return WF_ACT_REDRAW;
	}
	return WF_ACT_NONE;
}

static const char *keys_help(const WfApp *app)
{
	KRow r = row_at(app, app->settings.item);

	switch (r.kind) {
	case KR_PROFILE:   return wf_msg(MSG_SET_KEYS_HELP_PROFILE);
	case KR_BASE:      return wf_msg(MSG_SET_KEYS_HELP_BASE);
	case KR_RESET_ALL: return wf_msg(MSG_SET_KEYS_HELP_RESET_ALL);
	case KR_CMD:       {
		const WfCommandInfo *ci = wf_cmd_info(r.cmd);

		return ci ? wf_msg(ci->desc) : "";
	}
	default: return "";
	}
}

static WfMsg keys_hint(const WfApp *app)
{
	KRow r = row_at(app, app->settings.item);

	if (KR_CMD == r.kind) {
		return MSG_SET_HINT_KEYS_CMD;
	}
	return MSG_SET_HINT_KEYS;
}

const WfSetPage WF_SETPAGE_KEYS = {
        keys_count, keys_row, keys_event, keys_help, keys_hint};
