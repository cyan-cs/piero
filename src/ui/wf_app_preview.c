#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../archive/wf_vpath.h"
#include "../path/wf_path.h"
#include "../text/wf_text.h"
#include "wf_app_priv.h"

/* Drops the directory listing and invalidates any request in flight: its
 * answer carries an older generation and is discarded on arrival. */
static void dirprev_clear(WfApp *app)
{
	wf_app_dircache_return(app);
	++app->dirprev_gen;
	app->dirprev_state = WF_DIRPREV_NONE;
	app->dirprev_status = WF_OK;
	app->dirprev_due = 0;
	(void)wf_view_set_dir(&app->dirprev, NULL, NULL);
}

void wf_app_md_create(WfApp *app)
{
	app->md = calloc(1, sizeof *app->md);
}

void wf_app_md_invalidate(WfApp *app)
{
	if (app->md) {
		wf_md_free(app->md->doc);
		app->md->doc = NULL;
	}
}

void wf_app_md_destroy(WfApp *app)
{
	wf_app_md_invalidate(app);
	free(app->md);
	app->md = NULL;
}

const WfMdDoc *wf_app_md_doc(const WfApp *app, int width)
{
	const WfPreview *p = app->preview;
	struct WfAppMd *c = app->md;
	WfBuf src;
	WfStatus st = WF_OK;

	if (!c || !p || WF_PREVIEW_TEXT != p->kind || width < 8 ||
	        !app->preview_path ||
	        !wf_md_name_is_markdown(app->preview_path)) {
		return NULL;
	}
	if (c->doc && c->width == width) {
		return c->doc;
	}
	wf_md_free(c->doc);
	c->doc = NULL;
	wf_buf_init(&src);
	for (size_t i = 0; i < p->nlines && WF_OK == st; i++) {
		const char *line = p->text.data + p->lines[i];

		st = wf_buf_append(&src, line, strlen(line));
		if (WF_OK == st) {
			st = wf_buf_putc(&src, '\n');
		}
	}
	if (WF_OK == st) {
		st = wf_md_render(wf_buf_cstr(&src), src.len, width, &c->doc);
	}
	wf_buf_free(&src);
	c->width = width;
	return WF_OK == st ? c->doc : NULL;
}

void wf_app_preview_clear(WfApp *app)
{
	wf_app_md_invalidate(app);
	wf_app_syntax_preview_reset(app);
	if (app->preview || app->dirprev.dir || app->image.pm) {
		app->preview_stats.cleared_snapshots++;
	}
	wf_app_image_clear(app);
	dirprev_clear(app);
	++app->preview_gen;
	if (app->preview_port.cancel) {
		app->preview_port.cancel(app->preview_port.ctx);
	}
	wf_preview_free(app->preview);
	app->preview = NULL;
	free(app->preview_path);
	app->preview_path = NULL;
	app->preview_top = 0;
	app->preview_left = 0;
	app->preview_busy = false;
	app->preview_loading_due = 0;
}

void wf_app_preview_sync(WfApp *app, bool explicit_load)
{
	const WfEntry *e = wf_view_current(&app->cur);
	wchar_t *path = NULL;
	WfStatus st;
	bool retain;

	if (explicit_load) {
		wf_app_dircache_invalidate(app);
	}
	if (app->dircache && app->dircache->count &&
	        (app->dircache->hidden != app->cur.show_hidden ||
	                wcscmp(app->dircache->filter, app->cur.filter))) {
		wf_app_dircache_invalidate(app);
	}
	if (!e || app->want_path) {
		wf_app_preview_clear(app);
		return;
	}
	st = wf_path_join(app->cwd, e->name, &path);
	if (WF_OK != st) {
		wf_app_preview_clear(app);
		return;
	}
	if (!explicit_load && app->preview_path &&
	        !wcscmp(path, app->preview_path) &&
	        e->flags == app->preview_flags &&
	        e->size == app->preview_size &&
	        e->mtime == app->preview_mtime &&
	        (e->identity.kind == WF_ID_STRONG) ==
	                (app->preview_id.kind == WF_ID_STRONG) &&
	        (!(e->identity.kind == WF_ID_STRONG) ||
	                (e->identity.volume == app->preview_id.volume &&
	                        !memcmp(e->identity.id, app->preview_id.id,
	                                sizeof e->identity.id)))) {
		free(path);
		return;
	}
	/* Only a proven same object can retain a previous read-only snapshot.
	 * It remains visibly marked as being updated, never operation authority. */
	retain = app->preview_path && !wcscmp(path, app->preview_path) &&
	        e->flags == app->preview_flags &&
	        (e->identity.kind == WF_ID_STRONG) &&
	        (app->preview_id.kind == WF_ID_STRONG) &&
	        e->identity.volume == app->preview_id.volume &&
	        !memcmp(e->identity.id, app->preview_id.id,
	                sizeof e->identity.id) &&
	        !wf_app_image_active(app) && (app->preview || app->dirprev.dir);
	if (retain) {
		app->preview_stats.retained_snapshots++;
		wf_app_dircache_invalidate(app);
		++app->preview_gen;
		if (app->preview_port.cancel) {
			app->preview_port.cancel(app->preview_port.ctx);
		}
		free(app->preview_path);
		app->preview_path = NULL;
		++app->dirprev_gen;
		app->dirprev_state = WF_DIRPREV_NONE;
		app->dirprev_due = 0;
		app->preview_busy = false;
	} else {
		wf_app_preview_clear(app);
	}
	app->preview_path = path;
	app->preview_flags = e->flags;
	app->preview_id = e->identity;
	app->preview_size = e->size;
	app->preview_mtime = e->mtime;
	app->preview_loading_due = app->now_ms + WF_PREVIEW_LOADING_MS;
	if (e->flags & (WF_ENT_DIR | WF_ENT_ARCHIVE)) {
		if (!app->dirprev_port.submit) {
			app->preview = wf_preview_notice(WF_PREVIEW_EMPTY,
			        wf_msg_in(
			                WF_UILANG_EN, MSG_REASON_PV_DIR_META));
		} else if (e->flags & WF_ENT_LINK) {
			app->preview = wf_preview_notice(WF_PREVIEW_REFUSED,
			        wf_msg_in(WF_UILANG_EN,
			                MSG_REASON_PV_LINK_TARGET));
		} else if (!explicit_load &&
		        !wf_preview_dir_auto_allowed(path, e->flags)) {
			app->preview = wf_preview_notice(WF_PREVIEW_REFUSED,
			        wf_msg_in(WF_UILANG_EN,
			                MSG_REASON_PV_PRESS_P_DIR));
		} else {
			if (wf_app_dircache_select(app)) {
				return;
			}
			/* Requested by wf_app_tick() once the cursor rests. */
			app->dirprev_state = WF_DIRPREV_WAIT;
			app->dirprev_due = app->now_ms + WF_DIRPREV_DEBOUNCE_MS;
		}
		return;
	}
	if (wf_preview_is_image(e->name) || wf_preview_is_pdf(e->name)) {
		if (!wf_app_image_wanted(app, e->name)) {
			/* off in the settings, or no service to ask: say so, and
			 * how to turn it on */
			app->preview =
			        wf_preview_notice(WF_PREVIEW_IMAGE_BLOCKED,
			                wf_msg_in(WF_UILANG_EN,
			                        MSG_REASON_PV_IMAGE_OFF));
			return;
		}
		if (!explicit_load &&
		        !wf_preview_auto_allowed(path, e->flags)) {
			app->preview = wf_preview_notice(WF_PREVIEW_REFUSED,
			        wf_msg_in(WF_UILANG_EN, MSG_REASON_PV_PRESS_P));
			return;
		}
		wf_app_image_begin(app, e->name);
		return;
	}
	if (!explicit_load && !wf_preview_auto_allowed(path, e->flags)) {
		app->preview = wf_preview_notice(WF_PREVIEW_REFUSED,
		        wf_msg_in(WF_UILANG_EN, MSG_REASON_PV_PRESS_P));
		return;
	}
	if (!app->preview_port.submit) {
		app->preview = wf_preview_notice(WF_PREVIEW_REFUSED,
		        wf_msg_in(WF_UILANG_EN,
		                MSG_REASON_PV_SERVICE_UNAVAILABLE));
		return;
	}
	app->preview_busy = true;
	app->preview_stats.text_requests++;
	st = app->preview_port.submit(
	        app->preview_port.ctx, app->preview_gen, path, explicit_load);
	if (WF_OK != st) {
		app->preview_busy = false;
		app->preview = wf_preview_notice(WF_PREVIEW_REFUSED,
		        wf_msg_in(WF_UILANG_EN, MSG_REASON_PV_CANNOT_QUEUE));
	}
}

void wf_app_set_preview(WfApp *app, WfPreviewPort port)
{
	wf_app_preview_clear(
	        app); /* cancel the old service before changing ownership */
	app->preview_port = port;
	wf_app_preview_sync(app, false);
}

void wf_app_set_image(WfApp *app, WfImagePort port, int style)
{
	wf_app_preview_clear(app); /* cancel the old service first */
	app->image_port = port;
	app->image_style = style;
	wf_app_preview_sync(app, false);
}

void wf_app_set_dirpreview(WfApp *app, WfDirPreviewPort port)
{
	wf_app_dircache_invalidate(app);
	wf_app_preview_clear(app);
	app->dirprev_full = port;
	app->dirprev_port = app->dirprev_off ? (WfDirPreviewPort){0} : port;
	wf_app_preview_sync(app, false);
}

void wf_app_dirpreview_enable(WfApp *app, bool on)
{
	wf_app_dircache_invalidate(app);
	wf_app_preview_clear(app);
	app->dirprev_off = !on;
	app->dirprev_port = on ? app->dirprev_full : (WfDirPreviewPort){0};
	wf_app_preview_sync(app, false);
}

/* Sends the listing request for the directory the pane is on. */
static void dirprev_submit(WfApp *app)
{
	WfStatus st;

	app->dirprev_due = 0;
	if (!app->preview_path || !app->dirprev_port.submit) {
		app->dirprev_state = WF_DIRPREV_NONE;
		return;
	}
	if (wf_app_dircache_submit(app)) {
		return;
	}
	app->dirprev_state = WF_DIRPREV_LOADING;
	app->preview_stats.directory_requests++;
	if ((app->preview_flags & WF_ENT_ARCHIVE) &&
	        !(app->preview_flags & WF_ENT_DIR)) {
		/* an archive file: list the inside of it */
		wchar_t *root = NULL;

		st = wf_vpath_root(app->preview_path, &root);
		if (WF_OK == st) {
			st = app->dirprev_port.submit(app->dirprev_port.ctx,
			        app->dirprev_gen, root, NULL);
			free(root);
		}
	} else {
		st = app->dirprev_port.submit(app->dirprev_port.ctx,
		        app->dirprev_gen, app->preview_path, NULL);
	}
	if (WF_OK != st) {
		app->dirprev_state = WF_DIRPREV_FAILED;
		app->dirprev_status = st;
	}
}

uint32_t wf_app_dirprev_timeout(const WfApp *app)
{
	if (WF_DIRPREV_WAIT != app->dirprev_state) {
		return WF_WAIT_FOREVER;
	}
	return app->now_ms >= app->dirprev_due
	        ? 0
	        : (uint32_t)(app->dirprev_due - app->now_ms);
}

bool wf_app_dirprev_tick(WfApp *app)
{
	if (WF_DIRPREV_WAIT != app->dirprev_state ||
	        app->now_ms < app->dirprev_due) {
		return false;
	}
	dirprev_submit(app);
	return WF_DIRPREV_LOADING != app->dirprev_state; /* an error to draw */
}

void wf_app_on_dirpreview(WfApp *app, uint32_t gen, WfStatus st, WfDir *dir)
{
	if (wf_app_dircache_result(app, gen, st, dir)) {
		return;
	}
	if (gen != app->dirprev_gen ||
	        WF_DIRPREV_LOADING != app->dirprev_state) {
		wf_dir_free(dir);
		return;
	}
	wf_app_dirpreview_show(app, st, dir);
}

void wf_app_dirpreview_show(WfApp *app, WfStatus st, WfDir *dir)
{
	if (WF_OK != st || !dir) {
		wf_dir_free(dir);
		app->dirprev_state = WF_DIRPREV_FAILED;
		app->dirprev_status = WF_OK != st ? st : WF_E_OS;
		return;
	}
	app->dirprev.show_hidden = app->cur.show_hidden;
	st = wf_view_set_dir(&app->dirprev, dir, NULL); /* takes `dir` */
	wf_app_dircache_displayed(app);
	if (WF_OK != st) {
		app->dirprev_state = WF_DIRPREV_FAILED;
		app->dirprev_status = st;
		return;
	}
	app->dirprev_state = WF_DIRPREV_READY;
}

void wf_app_on_preview(WfApp *app, uint32_t gen, WfPreview *preview)
{
	if (gen != app->preview_gen || !app->preview_busy) {
		wf_preview_free(preview);
		return;
	}
	WfPreview *old = app->preview;
	if (old && preview && old->kind == preview->kind &&
	        old->system_error == preview->system_error &&
	        old->truncated == preview->truncated &&
	        old->nlines == preview->nlines &&
	        old->text.len == preview->text.len &&
	        !strcmp(old->detail, preview->detail) &&
	        (!old->text.len ||
	                !memcmp(old->text.data, preview->text.data,
	                        old->text.len))) {
		wf_preview_free(preview);
		app->preview_busy = false;
		free(app->preview_keep.path); /* the view never moved */
		app->preview_keep.path = NULL;
		return; /* identical display, including its scroll position */
	}
	wf_app_md_invalidate(app);
	wf_preview_free(app->preview);
	app->preview = preview;
	wf_app_syntax_preview_start(app);
	app->preview_busy = false;
	app->preview_top = 0;
	app->preview_left = 0;
	if (app->preview_keep.path) { /* the same file, saved from the editor */
		if (app->preview_path &&
		        !wcscmp(app->preview_path, app->preview_keep.path)) {
			app->preview_top = app->preview_keep.top;
			app->preview_left = app->preview_keep.left;
		}
		free(app->preview_keep.path);
		app->preview_keep.path = NULL;
	}
}

void wf_app_preview_focus(WfApp *app)
{
	WfLayout layout;
	const WfEntry *entry = wf_view_current(&app->cur);

	if (app->want_path) {
		wf_app_set_msg(app, wf_msg(MSG_XFER_STALE_LISTING));
		return;
	}
	if (!entry || (entry->flags & WF_ENT_DIR)) {
		return;
	}
	wf_layout_compute(app->cols, app->rows, &app->ratios, &layout);
	if (layout.minimal || !layout.pane[WF_PANE_PREVIEW].w) {
		wf_app_set_msg(app, wf_msg(MSG_PREVIEW_PANE_UNAVAILABLE));
		return;
	}
	app->msg[0] = '\0';
	app->mode = &WF_MODE_PREVIEW_PANE;
}

size_t wf_app_preview_scroll_limit(const WfPreview *preview, int height)
{
	int rows;

	if (!preview || WF_PREVIEW_TEXT != preview->kind || height <= 1) {
		return 0;
	}
	/* Metadata occupies the first row; a truncation notice occupies the
	 * last. */
	rows = height - 1 - (0 != preview->truncated);
	if (rows <= 0 || preview->nlines <= (size_t)rows) {
		return 0;
	}
	return preview->nlines - (size_t)rows;
}

int wf_app_preview_text_width(const WfPreview *preview, int width)
{
	int digits = 1;
	int gutter;

	if (!preview) {
		return 0;
	}
	for (size_t n = preview->nlines; n >= 10; n /= 10) {
		digits++;
	}
	gutter = digits + 3;
	return width > gutter ? width - gutter : 0;
}

size_t wf_app_preview_pan_limit(const WfPreview *preview, int width)
{
	int text = wf_app_preview_text_width(preview, width);

	if (!preview || WF_PREVIEW_TEXT != preview->kind || text <= 0 ||
	        preview->max_cells <= (size_t)text) {
		return 0;
	}
	return preview->max_cells - (size_t)text;
}

/* How far a Markdown preview of `width` x `height` can scroll (rows of its
 * layout beyond what fits). */
static size_t md_scroll_limit(const WfApp *app, int width, int height)
{
	const WfMdDoc *d = wf_app_md_doc(app, width);
	bool truncated = (app->preview && app->preview->truncated) ||
	        (d && d->truncated);
	int rows = height - 1 - (truncated && height > 1);

	if (!d || rows <= 0 || d->nlines <= (size_t)rows) {
		return 0;
	}
	return d->nlines - (size_t)rows;
}

static size_t preview_pan_limit(const WfApp *app)
{
	WfLayout layout;

	if (&WF_MODE_PREVIEW == app->mode) {
		return wf_app_md_doc(app, app->cols)
		        ? 0
		        : wf_app_preview_pan_limit(app->preview, app->cols);
	}
	wf_layout_compute(app->cols, app->rows, &app->ratios, &layout);
	if (layout.minimal || !layout.pane[WF_PANE_PREVIEW].w ||
	        wf_app_md_doc(app, layout.pane[WF_PANE_PREVIEW].w)) {
		return 0;
	}
	return wf_app_preview_pan_limit(
	        app->preview, layout.pane[WF_PANE_PREVIEW].w);
}

/* The pane keeps its last row for the digest of the file when there is one. */
static int sha_row_taken(const WfApp *app, int height)
{
	char sha[96];

	return height > 2 && wf_app_preview_sha_line(app, sha, sizeof sha);
}

static size_t preview_scroll_limit(const WfApp *app)
{
	WfLayout layout;
	int body;

	if (&WF_MODE_PREVIEW == app->mode) {
		int h = app->rows > 2 ? app->rows - 2 : 0;

		h -= sha_row_taken(app, h);
		return wf_app_md_doc(app, app->cols)
		        ? md_scroll_limit(app, app->cols, h)
		        : wf_app_preview_scroll_limit(app->preview, h);
	}
	wf_layout_compute(app->cols, app->rows, &app->ratios, &layout);
	if (layout.minimal || !layout.pane[WF_PANE_PREVIEW].w) {
		return wf_app_preview_scroll_limit(app->preview, 0);
	}
	body = layout.body_h - sha_row_taken(app, layout.body_h);
	if (wf_app_md_doc(app, layout.pane[WF_PANE_PREVIEW].w)) {
		return md_scroll_limit(
		        app, layout.pane[WF_PANE_PREVIEW].w, body);
	}
	return wf_app_preview_scroll_limit(app->preview, body);
}

#define WF_PREVIEW_PAN_STEP 8u

static void preview_draw(const WfApp *app, WfFrame *f)
{
	char hint[320], title[384];
	if (f->rows < 2) {
		return;
	}

	wf_frame_fill(f, 0, 0, f->cols, f->rows - 1, ' ', &WF_STYLE_BASE);
	wf_app_binding_hint(app, true, hint, sizeof hint);
	snprintf(title, sizeof title, "%s | %s",
	        wf_msg(MSG_PANE_PREVIEW_READONLY), hint);
	wf_app_put_fit(f, 0, 0, title, &wf_app_style_bar, f->cols);
	wf_app_draw_preview(app, f, 0, 1, f->cols, f->rows - 2);
}

void wf_app_preview_scroll(WfApp *app, int delta)
{
	size_t limit = preview_scroll_limit(app);

	if (app->preview_top > limit) {
		app->preview_top = limit;
	}
	if (delta < 0) {
		size_t up = (size_t)(-delta);

		app->preview_top =
		        app->preview_top > up ? app->preview_top - up : 0;
	} else {
		size_t room = limit - app->preview_top;

		app->preview_top += (size_t)delta < room ? (size_t)delta : room;
	}
}

static WfAction preview_event(WfApp *app, const WfEvent *ev)
{
	size_t limit = preview_scroll_limit(app);
	size_t pan = preview_pan_limit(app);
	WfStroke stroke;
	WfKeyResult r;

	if (app->preview_top > limit) {
		app->preview_top = limit;
	}
	if (app->preview_left > pan) { /* the terminal may have been resized */
		app->preview_left = pan;
	}
	if (!wf_stroke_from_event(ev, &stroke)) {
		return WF_ACT_NONE;
	}
	r = wf_app_feed(app, WF_CTX_PREVIEW, stroke);
	switch (r.verdict) {
	case WF_KR_NONE:
		/* the help opens at the keys of the screen it was asked from */
		if (WF_EV_CHAR == ev->type && '?' == ev->ch) {
			wf_app_help_begin(app);
			return WF_ACT_REDRAW;
		}
		return WF_ACT_NONE;
	case WF_KR_PENDING:
	case WF_KR_CANCELLED: return WF_ACT_REDRAW;
	case WF_KR_REFUSED:
		wf_app_set_msg(app, wf_msg(MSG_KEYS_COUNT_REFUSED));
		return WF_ACT_REDRAW;
	case WF_KR_COMMAND: break;
	}
	/* a picture or a PDF page has its own idea of scrolling, paging and
	 * zooming; whatever it does not take (close, full screen, load, edit,
	 * back at the left edge) goes on to the text preview's handling */
	if (wf_app_image_active(app) &&
	        wf_app_image_command(app, r.cmd, r.count, r.counted)) {
		return WF_ACT_REDRAW;
	}
	switch (r.cmd) {
	case WF_PV_EDGE_LEFT:  app->preview_left = 0; break;
	case WF_PV_EDGE_RIGHT: app->preview_left = pan; break;
	case WF_PV_PAN_LEFT:
		app->preview_left = app->preview_left > WF_PREVIEW_PAN_STEP
		        ? app->preview_left - WF_PREVIEW_PAN_STEP
		        : 0;
		break;
	case WF_PV_BACK:
		/* pans back first; only at the left edge does it leave the pane */
		if (app->preview_left > 0) {
			app->preview_left =
			        app->preview_left > WF_PREVIEW_PAN_STEP
			        ? app->preview_left - WF_PREVIEW_PAN_STEP
			        : 0;
		} else if (&WF_MODE_PREVIEW_PANE == app->mode) {
			app->mode = &WF_MODE_NORMAL;
		} else {
			return WF_ACT_NONE;
		}
		break;
	case WF_PV_PAN_RIGHT:
		app->preview_left =
		        pan - app->preview_left > WF_PREVIEW_PAN_STEP
		        ? app->preview_left + WF_PREVIEW_PAN_STEP
		        : pan;
		break;
	case WF_PV_TOP:    app->preview_top = 0; break;
	case WF_PV_BOTTOM: app->preview_top = limit; break;
	case WF_PV_CLOSE:  app->mode = &WF_MODE_NORMAL; break;
	case WF_PV_TOGGLE_FULL:
		app->mode = &WF_MODE_PREVIEW_PANE == app->mode
		        ? &WF_MODE_PREVIEW
		        : &WF_MODE_NORMAL;
		limit = preview_scroll_limit(app);
		if (app->preview_top > limit) {
			app->preview_top = limit;
		}
		break;
	case WF_PV_LOAD: wf_app_preview_sync(app, true); break;
	case WF_PV_EDIT:
		/* the editor remembers it was opened from here, and a refusal
		 * leaves the preview as it was */
		wf_app_edit_begin(app);
		break;
	case WF_PV_DOWN:
		if (app->preview_top < limit) {
			app->preview_top++;
		}
		break;
	case WF_PV_UP:
		if (app->preview_top) {
			app->preview_top--;
		}
		break;
	default: return WF_ACT_NONE;
	}
	return WF_ACT_REDRAW;
}

const WfMode WF_MODE_PREVIEW = {"PREVIEW", preview_draw, preview_event};
/* Focus is represented by a mode, not a second independent focus flag.
 * NULL draw retains the three panes; both views share read-only input. */
const WfMode WF_MODE_PREVIEW_PANE = {"PREVIEW_PANE", NULL, preview_event};
