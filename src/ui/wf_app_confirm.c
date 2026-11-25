#include <stdio.h>
#include <string.h>

#include "../text/wf_text.h"
#include "wf_app_priv.h"

void wf_app_on_presented(WfApp *app)
{
	WfConfirm *c = &app->confirm;

	if (&WF_MODE_CONFIRM == app->mode && WF_CONF_PENDING == c->state) {
		c->state = WF_CONF_GUARD;
		c->deadline = app->now_ms + WF_CONFIRM_GUARD_MS;
	}
}

uint32_t wf_app_timeout(const WfApp *app)
{
	const WfConfirm *c = &app->confirm;
	uint32_t next = wf_app_dirprev_timeout(app);
	uint32_t copy = wf_app_copy_timeout(app);
	uint32_t keys = wf_app_keys_timeout(app);
	uint32_t image = wf_app_image_timeout(app);
	uint32_t git = wf_app_git_timeout(app);
	if (wf_app_fuzzy_redo_due(app)) {
		return 0; /* the listing changed under a search: ask again now */
	}
	{
		uint32_t batch = wf_app_batch_timeout(app);

		if (batch < next) {
			next = batch; /* progress of a run; the question about old journals */
		}
	}
	if (app->msg[0] && app->msg_keep_until) {
		uint32_t message = app->now_ms >= app->msg_keep_until
		        ? 0
		        : (uint32_t)(app->msg_keep_until - app->now_ms);
		if (message < next) {
			next = message;
		}
	}
	if (app->preview_loading_due &&
	        (app->preview_busy || WF_DIRPREV_WAIT == app->dirprev_state ||
	                WF_DIRPREV_LOADING == app->dirprev_state ||
	                WF_IMG_BUSY == app->image.state)) {
		uint32_t loading = app->now_ms >= app->preview_loading_due
		        ? 0
		        : (uint32_t)(app->preview_loading_due - app->now_ms);
		if (loading < next) {
			next = loading;
		}
	}

	if (image < next) {
		next = image;
	}
	if (git < next) {
		next = git;
	}
	if (copy < next) {
		next = copy;
	}
	if (wf_app_wsl_next_ms(app) < next) {
		next = wf_app_wsl_next_ms(app);
	}
	if (keys < next) {
		next = keys;
	}

	if (&WF_MODE_CONFIRM == app->mode && WF_CONF_GUARD == c->state) {
		uint32_t guard = app->now_ms >= c->deadline
		        ? 0
		        : (uint32_t)(c->deadline - app->now_ms);

		return guard < next ? guard : next;
	}
	return next;
}

WfAction wf_app_tick(WfApp *app)
{
	WfConfirm *c = &app->confirm;
	/* Every tick runs on every call: a `||` chain would skip the ones after the
	 * first that wants a redraw, and they have clocks of their own to advance. */
	bool redraw = wf_app_dirprev_tick(app);

	redraw |= wf_app_fuzzy_tick(app);
	redraw |= wf_app_batch_tick(app);
	if (app->msg_keep_until && app->now_ms >= app->msg_keep_until) {
		app->msg_keep_until = 0;
		if (app->msg[0]) {
			app->msg[0] = '\0';
			redraw = true;
		}
	}
	if (app->preview_loading_due &&
	        app->now_ms >= app->preview_loading_due) {
		app->preview_loading_due =
		        0; /* consume once, never a zero-time busy loop */
		if (app->preview_busy ||
		        WF_DIRPREV_WAIT == app->dirprev_state ||
		        WF_DIRPREV_LOADING == app->dirprev_state ||
		        WF_IMG_BUSY == app->image.state) {
			redraw = true;
		}
	}

	redraw |= wf_app_image_tick(app);
	redraw |= wf_app_git_tick(app);
	redraw |= wf_app_copy_tick(app);
	redraw |= wf_app_wsl_tick(app);
	redraw |= wf_app_keys_tick(app);

	if (&WF_MODE_CONFIRM == app->mode && WF_CONF_GUARD == c->state &&
	        app->now_ms >= c->deadline) {
		c->state = WF_CONF_ARMED;
		redraw = true;
	}
	return redraw ? WF_ACT_REDRAW : WF_ACT_NONE;
}

static void copy_truncated(char *dst, const char *src, size_t n)
{
	size_t len =
	        n < WF_CONFIRM_LINE_BYTES - 1 ? n : WF_CONFIRM_LINE_BYTES - 1;

	while (len > 0 && len < n && 0x80 == ((unsigned char)src[len] & 0xC0)) {
		len--; /* do not cut a UTF-8 sequence in half */
	}
	memcpy(dst, src, len);
	dst[len] = '\0';
}

WfStatus wf_app_confirm_begin(WfApp *app,
        const char *const *lines,
        int n,
        void (*on_yes)(WfApp *, void *),
        void *ctx,
        const WfEvent *trigger)
{
	return wf_app_confirm_begin_ex(app, lines, n, 'y',
	        wf_msg(MSG_CONFIRM_YES_NO), on_yes, ctx, trigger);
}

WfStatus wf_app_confirm_begin_ex(WfApp *app,
        const char *const *lines,
        int n,
        uint32_t yes_ch,
        const char *armed_text,
        void (*on_yes)(WfApp *, void *),
        void *ctx,
        const WfEvent *trigger)
{
	WfConfirm *c = &app->confirm;
	WfConfirm next;
	int i;

	if (!app || (n > 0 && !lines) || n < 0 || n > WF_CONFIRM_MAX_LINES ||
	        !on_yes || !armed_text || yes_ch < 'a' || yes_ch > 'z' ||
	        'n' == yes_ch) {
		return WF_E_INVAL;
	}
	memset(&next, 0, sizeof next);
	next.yes_ch = yes_ch;
	snprintf(next.armed_text, sizeof next.armed_text, "%s", armed_text);
	for (i = 0; i < n; i++) {
		WfBuf clean;
		WfStatus st;

		if (!lines[i]) {
			return WF_E_INVAL;
		}
		wf_buf_init(&clean);
		st = wf_sanitize_utf8(lines[i], strlen(lines[i]), &clean);
		if (WF_OK != st) {
			wf_buf_free(&clean);
			return st;
		}
		copy_truncated(next.lines[i], wf_buf_cstr(&clean), clean.len);
		wf_buf_free(&clean);
	}
	next.n = n;
	next.on_yes = on_yes;
	next.ctx = ctx;
	next.state = WF_CONF_PENDING;
	if (trigger && WF_EV_CHAR == trigger->type) {
		next.held = true;
		next.held_ch = trigger->ch;
		next.held_vk = trigger->vk;
	}
	*c = next;
	app->mode = &WF_MODE_CONFIRM;
	app->flush_input = true; /* drop keys typed ahead of the modal */
	return WF_OK;
}

WfStatus wf_app_confirm_begin_alt(WfApp *app,
        const char *const *lines,
        int n,
        uint32_t yes_ch,
        uint32_t alt_ch,
        const char *armed_text,
        void (*on_yes)(WfApp *, void *),
        void (*on_alt)(WfApp *, void *),
        void *ctx)
{
	WfStatus st;

	if (alt_ch < 'a' || alt_ch > 'z' || 'n' == alt_ch || alt_ch == yes_ch ||
	        !on_alt) {
		return WF_E_INVAL;
	}
	st = wf_app_confirm_begin_ex(
	        app, lines, n, yes_ch, armed_text, on_yes, ctx, NULL);
	if (WF_OK == st) {
		app->confirm.alt_ch = alt_ch;
		app->confirm.on_alt = on_alt;
	}
	return st;
}

WfStatus wf_app_confirm_begin_alt2(WfApp *app,
        const char *const *lines,
        int n,
        uint32_t yes_ch,
        uint32_t alt_ch,
        uint32_t alt2_ch,
        const char *armed_text,
        void (*on_yes)(WfApp *, void *),
        void (*on_alt)(WfApp *, void *),
        void (*on_alt2)(WfApp *, void *),
        void *ctx)
{
	WfStatus st;

	if (alt2_ch < 'a' || alt2_ch > 'z' || 'n' == alt2_ch ||
	        alt2_ch == yes_ch || alt2_ch == alt_ch || !on_alt2) {
		return WF_E_INVAL;
	}
	st = wf_app_confirm_begin_alt(
	        app, lines, n, yes_ch, alt_ch, armed_text, on_yes, on_alt, ctx);
	if (WF_OK == st) {
		app->confirm.alt2_ch = alt2_ch;
		app->confirm.on_alt2 = on_alt2;
	}
	return st;
}

/* Arrows choose between "yes" and "no" only where the question is the plain
 * Y/N one: with another go key, another answer or its own button line, the
 * labels would not say what the choice does. */
static bool arrows_apply(const WfConfirm *c)
{
	return 'y' == c->yes_ch && !c->alt_ch && !c->alt2_ch &&
	        0 == strcmp(c->armed_text, wf_msg(MSG_CONFIRM_YES_NO));
}

static bool is_held_key(const WfConfirm *c, const WfEvent *ev)
{
	return c->held &&
	        ((ev->vk && ev->vk == c->held_vk) ||
	                (ev->ch && ev->ch == c->held_ch));
}

static void confirm_draw(const WfApp *app, WfFrame *f)
{
	const WfConfirm *c = &app->confirm;
	const char *shown[WF_CONFIRM_MAX_LINES + 3];
	char pick[96];
	int n = 0, i;

	for (i = 0; i < c->n; i++) {
		shown[n++] = c->lines[i];
	}
	shown[n++] = "";
	if (WF_CONF_ARMED == c->state && arrows_apply(c)) {
		/* the same line, with a mark at the one Enter would take */
		bool hint = f->cols >= 48;

		snprintf(pick, sizeof pick, "%s%s   %s%s%s%s",
		        c->pick_yes ? "> " : "  ", wf_msg(MSG_CONFIRM_PICK_YES),
		        c->pick_yes ? "  " : "> ", wf_msg(MSG_CONFIRM_PICK_NO),
		        hint ? "   " : "",
		        hint ? wf_msg(MSG_CONFIRM_PICK_HINT) : "");
		shown[n++] = pick;
	} else {
		shown[n++] = WF_CONF_ARMED == c->state
		        ? c->armed_text
		        : wf_msg(MSG_STATUS_HINT_LOCKED);
	}
	if (c->ime_hint) {
		shown[n++] = wf_msg(MSG_CONFIRM_IME_HINT);
	}

	if (wf_ui_alert(f, shown, n)) {
		return;
	}

	/*
	 * Spec 5.1, in three tiers. Whatever the size, a modal that can be
	 * answered shows what is being approved:
	 *   - the full box (above);
	 *   - from 20x8 up, a compact layout: the verb first, then as many of
	 *     the closing lines as fit, which always include the destination;
	 *   - below 20x8, only the full-screen "CONFIRM: Y/N" notice.
	 */
	wf_frame_fill(f, 0, 0, f->cols, f->rows, ' ', &WF_STYLE_BASE);
	if (f->cols < 20 || f->rows < 8) {
		wf_app_put_str(f, 0, 0, wf_msg(MSG_CONFIRM_SHORT),
		        &WF_STYLE_BASE, f->cols);
		return;
	}
	{
		int budget = f->rows - 1 -
		        (n - c->n -
		                1); /* rows left after the button/hint lines */
		int keep_tail, y = 0, first_tail;

		if (budget < 1) {
			budget = 1;
		}
		if (budget >= c->n) {
			first_tail = 1;
			keep_tail = c->n - 1;
		} else {
			keep_tail = budget - 1;
			first_tail = c->n - keep_tail;
		}
		if (c->n > 0) {
			wf_app_put_fit(f, 0, y++, c->lines[0], &WF_STYLE_BASE,
			        f->cols);
		}
		for (i = first_tail; i < c->n && keep_tail > 0;
		        i++, keep_tail--) {
			wf_app_put_fit(f, 0, y++, c->lines[i], &WF_STYLE_BASE,
			        f->cols);
		}
		for (i = c->n + 1; i < n;
		        i++) { /* button line, then the optional hint */
			wf_app_put_fit(
			        f, 0, y++, shown[i], &WF_STYLE_BASE, f->cols);
		}
	}
}

static WfAction answer_yes(WfApp *app)
{
	WfConfirm *c = &app->confirm;
	void (*yes)(WfApp *, void *) = c->on_yes;
	void *ctx = c->ctx;

	app->mode = &WF_MODE_NORMAL; /* before the callback, which may reopen a
	                                modal */
	yes(app, ctx);
	return WF_ACT_REDRAW;
}

static WfAction confirm_event(WfApp *app, const WfEvent *ev)
{
	WfConfirm *c = &app->confirm;

	wf_app_tick(app); /* GUARD -> ARMED if the clock says so */

	if (WF_EV_RELEASE == ev->type) {
		if (is_held_key(c, ev)) {
			c->held = false;
		}
		return WF_ACT_NONE;
	}
	if (WF_EV_KEY == ev->type && WF_KEY_ESC == ev->key) { /* always live */
		if (c->on_yes == wf_app_unc_yes) {
			wf_app_free_pending_navigation(app);
		}
		app->mode = &WF_MODE_NORMAL;
		return WF_ACT_REDRAW;
	}
	/* A question that cannot be shown cannot be answered "yes": below the
	 * size where the compact form fits (see confirm_draw), only Esc (above)
	 * and a resize do anything. */
	if (app->cols < 20 || app->rows < 8) {
		return WF_ACT_NONE;
	}
	if (WF_CONF_ARMED != c->state || WF_EV_RESIZE == ev->type) {
		return WF_ACT_NONE; /* typed during the guard: discard */
	}

	if (WF_EV_KEY == ev->type) {
		if (arrows_apply(c) &&
		        (WF_KEY_LEFT == ev->key || WF_KEY_UP == ev->key ||
		                WF_KEY_RIGHT == ev->key ||
		                WF_KEY_DOWN == ev->key)) {
			c->pick_yes =
			        WF_KEY_LEFT == ev->key || WF_KEY_UP == ev->key;
			return WF_ACT_REDRAW;
		}
		if (WF_KEY_ENTER == ev->key && c->pick_yes) {
			return answer_yes(
			        app); /* chosen with an arrow, shown */
		}
		if (WF_KEY_ENTER == ev->key) { /* Enter alone never confirms */
			if (c->on_yes == wf_app_unc_yes) {
				wf_app_free_pending_navigation(app);
			}
			app->mode = &WF_MODE_NORMAL;
			return WF_ACT_REDRAW;
		}
		return WF_ACT_NONE;
	}
	if (WF_EV_CHAR != ev->type || is_held_key(c, ev)) {
		return WF_ACT_NONE;
	}

	if (ev->ch == c->yes_ch || ev->ch == c->yes_ch - 'a' + 'A') {
		return answer_yes(app);
	}
	if (c->alt_ch &&
	        (ev->ch == c->alt_ch || ev->ch == c->alt_ch - 'a' + 'A')) {
		void (*alt)(WfApp *, void *) = c->on_alt;
		void *ctx = c->ctx;

		app->mode = &WF_MODE_NORMAL;
		alt(app, ctx);
		return WF_ACT_REDRAW;
	}
	if (c->alt2_ch &&
	        (ev->ch == c->alt2_ch || ev->ch == c->alt2_ch - 'a' + 'A')) {
		void (*alt2)(WfApp *, void *) = c->on_alt2;
		void *ctx = c->ctx;

		app->mode = &WF_MODE_NORMAL;
		alt2(app, ctx);
		return WF_ACT_REDRAW;
	}
	if ('n' == ev->ch || 'N' == ev->ch) {
		if (c->on_yes == wf_app_unc_yes) {
			wf_app_free_pending_navigation(app);
		}
		app->mode = &WF_MODE_NORMAL;
		return WF_ACT_REDRAW;
	}
	if (ev->ch > 0x7F) { /* a CJK/IME character where Y/N was expected */
		c->ime_hint = true;
		return WF_ACT_REDRAW;
	}
	return WF_ACT_NONE;
}

const WfMode WF_MODE_CONFIRM = {"CONFIRM", confirm_draw, confirm_event};
