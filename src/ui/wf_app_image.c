#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../base/wf_macro.h"
#include "../input/wf_commands.h"
#include "../path/wf_path.h"
#include "../render/wf_pixcell.h"
#include "../text/wf_text.h"
#include "wf_app_priv.h"

/*
 * Pictures and PDF pages in the preview pane and the full-screen preview.
 *
 * The app never decodes anything. It asks the image service (a port, like
 * every other worker here) for one picture at a pixel size, gets back a
 * pixmap no larger than that, and an encoded Sixel viewport when requested.
 * Text and pixel output have separate owners. The UI keeps:
 * the pixmap of the page on screen, which page, the zoom, and where it is
 * scrolled. Every request carries a generation; an answer for any other
 * generation is dropped, so a slow decode of a file the cursor has left never
 * shows up.
 *
 * The request, zoom and pan units are pixels. Sixel uses verified terminal
 * cell dimensions; the text fallback uses cols x 2*rows half-block pixels.
 */

#define RESIZE_DEBOUNCE_MS 120u

/* Zeroes secrets in a way the compiler may not drop. */
static void wipe(void *p, size_t n)
{
	volatile unsigned char *v = p;

	while (n--) {
		*v++ = 0;
	}
}

static void image_pixels_free(WfApp *app)
{
	WfPixmap *p = app->image.pm;
	if (p && app->image.kind == WF_DKIND_PDF) {
		wipe(p->px, wf_pixmap_bytes(p->w, p->h));
	}
	wf_pixmap_free(p);
	app->image.pm = NULL;
}

static void image_sixel_free(WfApp *app)
{
	WfBuf *b = &app->image.sixel;
	if (b->data && app->image.kind == WF_DKIND_PDF) {
		wipe(b->data, b->cap);
	}
	wf_buf_free(b);
}

static const uint32_t ZOOM_PCT[WF_IMAGE_ZOOM_STEPS] = {100, 150, 200, 300, 400};

static bool image_mode(const WfApp *app)
{
	return &WF_MODE_PREVIEW == app->mode ||
	        &WF_MODE_PREVIEW_PANE == app->mode ||
	        &WF_MODE_NORMAL == app->mode;
}

static bool fullscreen(const WfApp *app)
{
	return &WF_MODE_PREVIEW == app->mode ||
	        (&WF_MODE_IMAGE_PASSWORD == app->mode &&
	                app->image.pw_fullscreen);
}

/* The area the picture goes in, in cells: the preview pane, or the whole
 * screen between the title and the status line. False when there is none (the
 * pane is hidden on a narrow terminal). One row at the bottom of the area is
 * the information line when the area is tall enough. */
static bool viewport(const WfApp *app, int *cols, int *rows)
{
	WfLayout layout;
	int h;

	if (fullscreen(app)) {
		*cols = app->cols;
		h = app->rows - 2;
	} else {
		wf_layout_compute(app->cols, app->rows, &app->ratios, &layout);
		if (layout.minimal || 0 == layout.pane[WF_PANE_PREVIEW].w) {
			return false;
		}
		*cols = layout.pane[WF_PANE_PREVIEW].w;
		h = layout.body_h;
	}
	*rows = h >= 4 ? h - 1 : h;
	return *cols > 0 && *rows > 0;
}

static uint32_t cell_w(const WfApp *app)
{
	return app->image_cell_w && !app->image.sixel_failed ? app->image_cell_w
	                                                     : 1;
}

static uint32_t cell_h(const WfApp *app)
{
	return app->image_cell_h && !app->image.sixel_failed ? app->image_cell_h
	                                                     : 2;
}

static void compute_box(
        const WfApp *app, int cols, int rows, uint32_t *bw, uint32_t *bh)
{
	uint64_t w = (uint64_t)cell_w(app) * (uint64_t)cols *
	        ZOOM_PCT[app->image.zoom] / 100;
	uint64_t h = (uint64_t)cell_h(app) * (uint64_t)rows *
	        ZOOM_PCT[app->image.zoom] / 100;

	if (w < 1) {
		w = 1;
	}
	if (w > WF_DWIRE_BOX_MAX) {
		w = WF_DWIRE_BOX_MAX;
	}
	if (app->image.fit_width) {
		/* as tall as the reply may be: the width decides the size */
		h = WF_DWIRE_RASTER_MAX / 4 / w;
	}
	if (h < 1) {
		h = 1;
	}
	if (h > WF_DWIRE_BOX_MAX) {
		h = WF_DWIRE_BOX_MAX;
	}
	while (w * h * 4 > WF_DWIRE_RASTER_MAX) {
		w = w * 9 / 10 + 1;
		h = h * 9 / 10 + 1;
	}
	*bw = (uint32_t)w;
	*bh = (uint32_t)h;
}

static void fail_now(WfApp *app, WfDecodeStatus status)
{
	app->image.state = WF_IMG_FAILED;
	app->image.status = status;
	app->image.refused = false;
	app->image.unavailable = false;
	app->image.os_error = 0;
}

/* What is asked of the decoder for the picture on screen, from the state of the
 * app alone: nothing is changed here. */
static void render_spec(
        const WfApp *app, int cols, int rows, WfImageRequest *rq)
{
	memset(rq, 0, sizeof *rq);
	compute_box(app, cols, rows, &rq->box_w, &rq->box_h);
	rq->sixel = app->image_cell_w && app->image_cell_h &&
	        !app->image.sixel_failed;
	rq->view_w = (uint32_t)cols * cell_w(app);
	rq->view_h = (uint32_t)rows * cell_h(app);
	rq->pan_x = app->image.pan_x;
	rq->pan_y = app->image.pan_y;
	rq->bg_rgb = wf_theme_styles()->base.bg_rgb;
	rq->quality = app->image_quality;
	rq->max_colors = app->image_colors;
	rq->path = app->preview_path;
	rq->kind = app->image.kind;
	rq->session = app->image_session;
	rq->page = app->image.page;
	rq->password = app->image.password[0] ? app->image.password : NULL;
}

/* What the answer will be checked against: the request as it was made. */
static void remember_spec(WfApp *app, const WfImageRequest *rq)
{
	app->image.max_colors = rq->max_colors;
	app->image.cell_w = cell_w(app);
	app->image.cell_h = cell_h(app);
	app->image.bg_rgb = rq->bg_rgb;
	app->image.quality = rq->quality;
	app->image.box_w = rq->box_w;
	app->image.box_h = rq->box_h;
}

static void request(WfApp *app)
{
	WfImageRequest rq;
	int cols, rows;
	WfStatus st;

	if (!viewport(app, &cols, &rows)) {
		app->image.view_cols =
		        0; /* nowhere to draw it yet: the tick asks */
		app->image.view_rows = 0;
		return;
	}
	render_spec(app, cols, rows, &rq);
	remember_spec(app, &rq);
	image_sixel_free(app);
	rq.gen = ++app->image.gen;
	app->preview_stats.image_requests++;
	st = app->image_port.submit(app->image_port.ctx, &rq);
	app->image.view_cols = cols;
	app->image.view_rows = rows;
	app->image.view_full = fullscreen(app);
	app->image.due = 0;
	if (WF_OK != st) {
		fail_now(app, WF_DSTAT_INTERNAL);
		return;
	}
	app->image.state = WF_IMG_BUSY;
	app->preview_loading_due = app->now_ms + WF_PREVIEW_LOADING_MS;
}

bool wf_app_image_wanted(const WfApp *app, const wchar_t *name)
{
	return (WF_IMAGE_HALFBLOCK == app->image_style ||
	               WF_IMAGE_ASCII == app->image_style) &&
	        app->image_port.submit &&
	        (wf_preview_is_image(name) || wf_preview_is_pdf(name));
}

void wf_app_image_clear(WfApp *app)
{
	uint32_t gen = app->image.gen + 1;
	if (WF_IMG_NONE != app->image.state && app->image_port.cancel) {
		app->image_port.cancel(app->image_port.ctx);
	}
	image_pixels_free(app);
	image_sixel_free(app);
	/* the password belongs to one file: it goes with it */
	wipe(app->image.password, sizeof app->image.password);
	wipe(app->image.typing, sizeof app->image.typing);
	memset(&app->image, 0, sizeof app->image);
	app->image.gen = gen;
}

void wf_app_image_begin(WfApp *app, const wchar_t *name)
{
	wf_app_image_clear(app);
	if (app->image_session == UINT64_MAX) {
		fail_now(app, WF_DSTAT_INTERNAL);
		return;
	}
	app->image_session++;
	app->image.kind =
	        wf_preview_is_pdf(name) ? WF_DKIND_PDF : WF_DKIND_IMAGE;
	app->image.state = WF_IMG_BUSY;
	request(app);
}

bool wf_app_image_active(const WfApp *app)
{
	return WF_IMG_NONE != app->image.state;
}

static void pan_limits(const WfApp *app, uint32_t *mx, uint32_t *my)
{
	int cols = app->image.view_cols, rows = app->image.view_rows;

	uint32_t w = cols > 0 ? (uint32_t)cols * cell_w(app) : 0;
	uint32_t h = rows > 0 ? (uint32_t)rows * cell_h(app) : 0;
	*mx = app->image.pm && app->image.pm->w > w ? app->image.pm->w - w : 0;
	*my = app->image.pm && app->image.pm->h > h ? app->image.pm->h - h : 0;
}

static void clamp_pan(WfApp *app)
{
	uint32_t mx, my;

	pan_limits(app, &mx, &my);
	if (app->image.pan_x > mx) {
		app->image.pan_x = mx;
	}
	if (app->image.pan_y > my) {
		app->image.pan_y = my;
	}
}

/* The point of the picture in the middle of the area, as a fraction of the
 * picture, so that it can be put back in the middle of the next one. */
static void remember_focus(WfApp *app)
{
	const WfPixmap *pm = app->image.pm;
	uint64_t cx, cy;

	if (!pm) {
		app->image.focus_x = app->image.focus_y = 32768;
		return;
	}
	cx = (uint64_t)app->image.pan_x +
	        (uint64_t)app->image.pm_cols * app->image.pm_cell_w / 2;
	cy = (uint64_t)app->image.pan_y +
	        (uint64_t)app->image.pm_rows * app->image.pm_cell_h / 2;
	app->image.focus_x =
	        pm->w > (uint32_t)app->image.pm_cols * app->image.pm_cell_w
	        ? (uint32_t)(cx * 65535 / pm->w)
	        : 32768;
	app->image.focus_y =
	        pm->h > app->image.pm_cell_h * (uint32_t)app->image.pm_rows
	        ? (uint32_t)(cy * 65535 / pm->h)
	        : 32768;
	app->image.refocus = true;
}

static void apply_focus(WfApp *app)
{
	const WfPixmap *pm = app->image.pm;
	uint64_t cx, cy,
	        half_w = (uint64_t)app->image.view_cols * cell_w(app) / 2,
	        half_h = (uint64_t)app->image.view_rows * cell_h(app) / 2;

	cx = (uint64_t)app->image.focus_x * pm->w / 65535;
	cy = (uint64_t)app->image.focus_y * pm->h / 65535;
	app->image.pan_x = cx > half_w ? (uint32_t)(cx - half_w) : 0;
	app->image.pan_y = cy > half_h ? (uint32_t)(cy - half_h) : 0;
	app->image.refocus = false;
}

static void prefetch_neighbours(WfApp *app);

void wf_app_on_image(WfApp *app, const WfImageAnswer *answer)
{
	if (!answer) {
		return;
	}
	if (WF_IMG_BUSY != app->image.state || answer->gen != app->image.gen) {
		if (answer->sensitive) {
			if (answer->pm) {
				wipe(answer->pm->px,
				        wf_pixmap_bytes(
				                answer->pm->w, answer->pm->h));
			}
			if (answer->sixel.data) {
				wipe(answer->sixel.data, answer->sixel.cap);
			}
		}
		wf_pixmap_free(answer->pm);
		WfBuf discarded = answer->sixel;
		wf_buf_free(&discarded);
		return;
	}
	image_sixel_free(app);
	app->image.sixel = answer->sixel;
	app->image.sixel_w = answer->sixel_w;
	app->image.sixel_h = answer->sixel_h;
	app->image.status = answer->status;
	app->image.refused = answer->refused;
	app->image.unavailable = answer->unavailable;
	app->image.os_error = answer->os_error;
	/* the page count is worth keeping even from a refusal: "no such page"
	 * says how many there are, and a PDF may still be paged through */
	if (answer->info.page_count) {
		app->image.info = answer->info;
	}
	if (WF_DSTAT_PASSWORD_REQUIRED == answer->status) {
		wipe(app->image.password, sizeof app->image.password);
	}
	if (WF_DSTAT_OK != answer->status || !answer->pm) {
		if (answer->sensitive && answer->pm) {
			wipe(answer->pm->px,
			        wf_pixmap_bytes(answer->pm->w, answer->pm->h));
		}
		wf_pixmap_free(answer->pm);
		if (WF_DSTAT_PAGE_RANGE == answer->status &&
		        app->image.page > 0 && answer->info.page_count) {
			/* asked past the end: show the last page instead of an error */
			app->image.page = answer->info.page_count - 1;
			request(app);
			return;
		}
		image_pixels_free(app);
		app->image.pm = NULL;
		app->image.state = WF_IMG_FAILED;
		return;
	}
	image_pixels_free(app);
	app->image.pm = answer->pm;
	app->image.pm_cell_w = app->image.cell_w;
	app->image.pm_cell_h = app->image.cell_h;
	app->image.pm_cols = app->image.view_cols;
	app->image.pm_rows = app->image.view_rows;
	app->image.info = answer->info;
	app->image.page = answer->page;
	app->image.state = WF_IMG_READY;
	if (app->image_cell_w && !app->image.sixel_failed &&
	        !app->image.sixel.len) {
		app->image.sixel_failed = true;
		app->image.failed_cw = app->image_cell_w;
		app->image.failed_ch = app->image_cell_h;
		app->image.failed_cols = app->image.view_cols;
		app->image.failed_rows = app->image.view_rows;
		app->image.failed_quality = app->image_quality;
		app->image.failed_colors = app->image_colors;
		app->image.failed_bg = wf_theme_styles()->base.bg_rgb;
		app->image.failed_full = fullscreen(app);
		request(app); /* re-decode at text resolution, never crop a huge pixmap into cells */
		return;
	}
	if (app->image.refocus && !app->image_cell_w) {
		apply_focus(app);
	} else if (app->image.refocus) {
		uint32_t old_x = app->image.pan_x, old_y = app->image.pan_y;
		apply_focus(app);
		clamp_pan(app);
		if (old_x != app->image.pan_x || old_y != app->image.pan_y) {
			request(app);
			return;
		}
	}
	clamp_pan(app);
	if (app->image.pan_to_end) {
		uint32_t mx, my;

		pan_limits(app, &mx, &my);
		app->image.pan_y = my;
		app->image.pan_to_end = false;
		if (app->image_cell_w && my) {
			request(app);
			return;
		}
	}
	prefetch_neighbours(app);
}

static void prefetch_one(
        WfApp *app, const wchar_t *path, WfDecodeKind kind, uint32_t page)
{
	WfImageRequest rq;

	memset(&rq, 0, sizeof rq);
	rq.path = path;
	rq.kind = kind;
	rq.session = app->image_session;
	rq.page = page;
	rq.box_w = app->image.box_w;
	rq.box_h = app->image.box_h;
	rq.prefetch = true;
	(void)app->image_port.submit(app->image_port.ctx, &rq);
}

static void prefetch_neighbours(WfApp *app)
{
	if (WF_DKIND_PDF == app->image.kind && app->image.password[0] == 0 &&
	        app->image.page + 1 < app->image.info.page_count) {
		prefetch_one(app, app->preview_path, WF_DKIND_PDF,
		        app->image.page + 1);
		return; // The single speculative slot belongs to the next PDF page.
	}
	/* the entries either side, as the pane would ask for them: only when this
	 * is the pane (a full-screen box is not what the pane needs) */
	if (fullscreen(app) || !app->cur.dir) {
		return;
	}
	for (int d = -1; d <= 1; d += 2) {
		long at = (long)app->cur.cursor + d;
		const WfEntry *e;
		wchar_t *path = NULL;

		if (at < 0 || (size_t)at >= app->cur.nvis) {
			continue;
		}
		e = &app->cur.dir->e[app->cur.vis[at]];
		if ((e->flags & (WF_ENT_DIR | WF_ENT_LINK | WF_ENT_CLOUD)) ||
		        !wf_app_image_wanted(app, e->name) ||
		        WF_OK != wf_path_join(app->cwd, e->name, &path)) {
			continue;
		}
		prefetch_one(app, path,
		        wf_preview_is_pdf(e->name) ? WF_DKIND_PDF
		                                   : WF_DKIND_IMAGE,
		        0);
		free(path);
	}
}

static void goto_page(WfApp *app, uint32_t page, bool to_end)
{
	app->image.page = page;
	app->image.pan_x = 0;
	app->image.pan_y = 0;
	app->image.pan_to_end = to_end;
	app->image.refocus = false;
	request(app);
}

static void zoom_to(WfApp *app, uint32_t zoom)
{
	if (zoom >= WF_IMAGE_ZOOM_STEPS || zoom == app->image.zoom) {
		return;
	}
	remember_focus(app);
	app->image.zoom = zoom;
	request(app);
}

static uint32_t step_x(const WfApp *app)
{
	uint32_t s = (uint32_t)app->image.view_cols * cell_w(app) / 8;

	return s < 2 ? 2 : s;
}

static uint32_t step_y(const WfApp *app)
{
	uint32_t s = cell_h(app) * (uint32_t)app->image.view_rows / 4;

	return s < 2 ? 2 : s;
}

/* Down: scroll; at the bottom edge go on to the next page, like turning one. */
static void scroll_down(WfApp *app)
{
	uint32_t mx, my;

	pan_limits(app, &mx, &my);
	if (app->image.pan_y < my) {
		uint32_t room = my - app->image.pan_y, s = step_y(app);

		app->image.pan_y += room < s ? room : s;
	} else if (WF_DKIND_PDF == app->image.kind &&
	        app->image.page + 1 < app->image.info.page_count) {
		goto_page(app, app->image.page + 1, false);
	}
}

static void scroll_up(WfApp *app)
{
	uint32_t s = step_y(app);

	if (app->image.pan_y > 0) {
		app->image.pan_y -= app->image.pan_y < s ? app->image.pan_y : s;
	} else if (WF_DKIND_PDF == app->image.kind && app->image.page > 0) {
		goto_page(app, app->image.page - 1, true);
	}
}

static void begin_password(WfApp *app)
{
	wipe(app->image.typing, sizeof app->image.typing);
	app->image.typing_n = 0;
	app->image.pw_fullscreen = &WF_MODE_PREVIEW == app->mode;
	app->image.pw_from = app->mode;
	app->mode = &WF_MODE_IMAGE_PASSWORD;
}

static bool image_command(
        WfApp *app, uint16_t cmd, uint32_t count, bool counted)
{
	uint32_t pages, n = counted && count ? count : 1;
	uint32_t mx, my;

	if (WF_IMG_NONE == app->image.state) {
		return false;
	}
	/* there is nothing to move over until a picture has arrived, but the key
	 * is still the picture's, not the text preview's */
	pages = app->image.info.page_count ? app->image.info.page_count : 1;
	if (WF_IMG_BUSY == app->image.state && !app->image.pm &&
	        WF_PV_PASSWORD != cmd) {
		return WF_PV_BACK != cmd && WF_PV_CLOSE != cmd &&
		        WF_PV_TOGGLE_FULL != cmd && WF_PV_LOAD != cmd &&
		        WF_PV_EDIT != cmd;
	}
	pan_limits(app, &mx, &my);
	switch (cmd) {
	case WF_PV_PAGE_NEXT:
		if (WF_DKIND_PDF == app->image.kind &&
		        app->image.page + 1 < pages) {
			uint32_t to = app->image.page + n;

			goto_page(app, to < pages ? to : pages - 1, false);
		}
		return true;
	case WF_PV_PAGE_PREV:
		if (WF_DKIND_PDF == app->image.kind && app->image.page > 0) {
			goto_page(app,
			        app->image.page > n ? app->image.page - n : 0,
			        false);
		}
		return true;
	case WF_PV_TOP:
	case WF_PV_BOTTOM:
		if (WF_DKIND_PDF == app->image.kind) {
			uint32_t to = counted
			        ? (count ? count - 1 : 0)
			        : (WF_PV_TOP == cmd ? 0 : pages - 1);

			if (to >= pages) {
				to = pages - 1;
			}
			if (to != app->image.page) {
				goto_page(app, to, false);
			}
		} else {
			app->image.pan_y = WF_PV_TOP == cmd ? 0 : my;
		}
		return true;
	case WF_PV_DOWN: scroll_down(app); return true;
	case WF_PV_UP:   scroll_up(app); return true;
	case WF_PV_PAN_LEFT:
		app->image.pan_x -= app->image.pan_x < step_x(app)
		        ? app->image.pan_x
		        : step_x(app);
		return true;
	case WF_PV_PAN_RIGHT:
		app->image.pan_x += mx - app->image.pan_x < step_x(app)
		        ? mx - app->image.pan_x
		        : step_x(app);
		return true;
	case WF_PV_BACK:
		if (app->image.pan_x > 0) {
			app->image.pan_x -= app->image.pan_x < step_x(app)
			        ? app->image.pan_x
			        : step_x(app);
			return true;
		}
		return false; /* at the left edge Back leaves the pane, as in text */
	case WF_PV_EDGE_LEFT:  app->image.pan_x = 0; return true;
	case WF_PV_EDGE_RIGHT: app->image.pan_x = mx; return true;
	case WF_PV_ZOOM_IN:    zoom_to(app, app->image.zoom + 1); return true;
	case WF_PV_ZOOM_OUT:
		if (app->image.zoom > 0) {
			zoom_to(app, app->image.zoom - 1);
		}
		return true;
	case WF_PV_FIT:
		remember_focus(app);
		app->image.fit_width = !app->image.fit_width;
		app->image.pan_x = 0;
		app->image.pan_y = 0;
		app->image.refocus = false;
		request(app);
		return true;
	case WF_PV_PASSWORD:
		if (WF_IMG_FAILED == app->image.state &&
		        (WF_DSTAT_PASSWORD_REQUIRED == app->image.status ||
		                WF_DSTAT_PASSWORD_WRONG == app->image.status)) {
			begin_password(app);
		}
		return true;
	default: return false;
	}
}

bool wf_app_image_command(
        WfApp *app, uint16_t cmd, uint32_t count, bool counted)
{
	uint32_t x = app->image.pan_x, y = app->image.pan_y,
	         gen = app->image.gen;
	bool handled = image_command(app, cmd, count, counted);
	if (app->image_cell_w && !app->image.sixel_failed &&
	        gen == app->image.gen &&
	        (x != app->image.pan_x || y != app->image.pan_y)) {
		request(app);
	}
	return handled;
}

static void password_draw(const WfApp *app, WfFrame *f)
{
	char shown[WF_IMAGE_PASSWORD_MAX + 1];
	char line[WF_IMAGE_PASSWORD_MAX + 64];
	size_t n = app->image.typing_n;

	if (f->rows < 3) {
		return;
	}
	if (n > WF_IMAGE_PASSWORD_MAX) {
		n = WF_IMAGE_PASSWORD_MAX;
	}
	memset(shown, '*', n);
	shown[n] = '\0';
	snprintf(line, sizeof line, "%s%s_", wf_msg(MSG_IMG_PASSWORD_PROMPT),
	        shown);
	wf_frame_fill(f, 0, f->rows - 2, f->cols, 1, ' ', &wf_app_style_bar);
	wf_app_put_fit(f, 0, f->rows - 2, line, &wf_app_style_bar, f->cols);
	wf_frame_set_cursor(
	        f, wf_utf8_cells(line, strlen(line) - 1), f->rows - 2);
	wf_frame_fill(f, 0, f->rows - 1, f->cols, 1, ' ', &wf_app_style_bar);
	wf_app_put_fit(f, 0, f->rows - 1, wf_msg(MSG_IMG_HINT_PASSWORD),
	        &wf_app_style_bar, f->cols);
}

static void password_leave(WfApp *app)
{
	app->mode = app->image.pw_from ? app->image.pw_from : &WF_MODE_NORMAL;
	app->image.pw_from = NULL;
	wipe(app->image.typing, sizeof app->image.typing);
	app->image.typing_n = 0;
}

static WfAction password_event(WfApp *app, const WfEvent *ev)
{
	if (WF_EV_CHAR == ev->type) {
		wchar_t *t = app->image.typing;
		size_t n = app->image.typing_n;
		uint32_t cp = ev->ch;

		if (cp < 0x20 || 0x7F == cp) {
			return WF_ACT_REDRAW;
		}
		if (cp >= 0x10000) {
			if (n + 2 > WF_IMAGE_PASSWORD_MAX) {
				return WF_ACT_REDRAW;
			}
			cp -= 0x10000;
			t[n++] = (wchar_t)(0xD800 + (cp >> 10));
			t[n++] = (wchar_t)(0xDC00 + (cp & 0x3FF));
		} else if (n + 1 <= WF_IMAGE_PASSWORD_MAX) {
			t[n++] = (wchar_t)cp;
		}
		t[n] = L'\0';
		app->image.typing_n = n;
		return WF_ACT_REDRAW;
	}
	if (WF_EV_KEY != ev->type) {
		return WF_ACT_NONE;
	}
	switch (ev->key) {
	case WF_KEY_BACKSPACE:
		if (app->image.typing_n > 0) {
			wchar_t *t = app->image.typing;
			size_t n = --app->image.typing_n;

			/* a character outside the BMP is two units: take both */
			if (n > 0 && 0xDC00 == (t[n] & 0xFC00) &&
			        0xD800 == (t[n - 1] & 0xFC00)) {
				n = --app->image.typing_n;
			}
			t[n] = L'\0';
		}
		return WF_ACT_REDRAW;
	case WF_KEY_ENTER:
		if (app->image.typing_n > 0) {
			memcpy(app->image.password, app->image.typing,
			        sizeof app->image.password);
			password_leave(app);
			app->image.pan_x = app->image.pan_y = 0;
			request(app);
			return WF_ACT_REDRAW;
		}
		return WF_ACT_REDRAW;
	case WF_KEY_ESC: password_leave(app); return WF_ACT_REDRAW;
	default:         return WF_ACT_NONE;
	}
}

const WfMode WF_MODE_IMAGE_PASSWORD = {
        "PASSWORD", password_draw, password_event};

uint32_t wf_app_image_timeout(const WfApp *app)
{
	int cols, rows;
	if (!app->image.due || !image_mode(app) ||
	        !viewport(app, &cols, &rows)) {
		return WF_WAIT_FOREVER;
	}
	return app->now_ms >= app->image.due
	        ? 0
	        : (uint32_t)(app->image.due - app->now_ms);
}

bool wf_app_image_tick(WfApp *app)
{
	int cols, rows;
	bool full;

	if (WF_IMG_NONE == app->image.state || !image_mode(app) ||
	        !viewport(app, &cols, &rows)) {
		return false;
	}
	full = fullscreen(app);
	if (app->image.sixel_failed &&
	        (app->image.failed_cw != app->image_cell_w ||
	                app->image.failed_ch != app->image_cell_h ||
	                app->image.failed_cols != cols ||
	                app->image.failed_rows != rows ||
	                app->image.failed_quality != app->image_quality ||
	                app->image.failed_colors != app->image_colors ||
	                app->image.failed_bg !=
	                        wf_theme_styles()->base.bg_rgb ||
	                app->image.failed_full != full)) {
		app->image.sixel_failed = false;
	}
	if (app->image.due) {
		if (app->now_ms < app->image.due) {
			return false;
		}
		remember_focus(app);
		request(app);
		return true;
	}
	if (cols != app->image.view_cols || rows != app->image.view_rows ||
	        full != app->image.view_full ||
	        cell_w(app) != app->image.cell_w ||
	        cell_h(app) != app->image.cell_h ||
	        app->image.bg_rgb != wf_theme_styles()->base.bg_rgb ||
	        app->image.quality != app->image_quality ||
	        app->image.max_colors != app->image_colors) {
		/* the area changed (resized, or the pane went full screen): ask for a
		 * picture of the new size, once the size has settled; the first ask
		 * of an entry, which had no area, goes at once */
		if (0 == app->image.view_cols || full != app->image.view_full) {
			/* no size to wait out: the first ask, or a switch to or from
			 * full screen, which the user just asked for */
			remember_focus(app);
			request(app);
			return true;
		}
		app->image.due = app->now_ms + RESIZE_DEBOUNCE_MS;
	}
	return false;
}

static const char *failure_text(const WfApp *app, char *buf, size_t cap)
{
	char n[24];

	switch (app->image.status) {
	case WF_DSTAT_CORRUPT:
		if (app->image.os_error) {
			snprintf(n, sizeof n, "%u",
			        (unsigned)app->image.os_error);
			(void)wf_msgf(
			        buf, cap, MSG_IMG_ERR_OPEN, &WF_ARG_S(n), 1);
			return buf;
		}
		return wf_msg(MSG_IMG_ERR_CORRUPT);
	case WF_DSTAT_UNSUPPORTED:
		return wf_msg(app->image.refused ? MSG_IMG_ERR_REFUSED
		                                 : MSG_IMG_ERR_UNSUPPORTED);
	case WF_DSTAT_TOO_LARGE:         return wf_msg(MSG_IMG_ERR_TOO_LARGE);
	case WF_DSTAT_PASSWORD_REQUIRED: return wf_msg(MSG_IMG_ERR_PASSWORD);
	case WF_DSTAT_PASSWORD_WRONG: return wf_msg(MSG_IMG_ERR_PASSWORD_WRONG);
	case WF_DSTAT_PAGE_RANGE:
		snprintf(n, sizeof n, "%u",
		        (unsigned)app->image.info.page_count);
		(void)wf_msgf(buf, cap, MSG_IMG_ERR_PAGE, &WF_ARG_S(n), 1);
		return buf;
	case WF_DSTAT_TIMEOUT: return wf_msg(MSG_IMG_ERR_TIMEOUT);
	case WF_DSTAT_HOST_LOST:
		if (app->image.unavailable) {
			snprintf(n, sizeof n, "%u",
			        (unsigned)app->image.os_error);
			(void)wf_msgf(
			        buf, cap, MSG_IMG_ERR_NO_HOST, &WF_ARG_S(n), 1);
			return buf;
		}
		return wf_msg(MSG_IMG_ERR_HOST);
	default: return wf_msg(MSG_IMG_ERR_INTERNAL);
	}
}

static void info_line(const WfApp *app, char *out, size_t cap)
{
	char fit[64], size[48], c[24], d[24];
	WfMsgArg pct;

	pct = WF_ARG_N(ZOOM_PCT[app->image.zoom]);
	(void)wf_msgf(fit, sizeof fit,
	        app->image.fit_width ? MSG_IMG_FIT_WIDTH : MSG_IMG_FIT_PAGE,
	        &pct, 1);
	snprintf(size, sizeof size, "%u x %u", (unsigned)app->image.info.src_w,
	        (unsigned)app->image.info.src_h);
	if (WF_DKIND_PDF == app->image.kind) {
		snprintf(c, sizeof c, "%u", (unsigned)app->image.page + 1);
		snprintf(d, sizeof d, "%u",
		        (unsigned)app->image.info.page_count);
		(void)wf_msgf(out, cap, MSG_IMG_INFO_PDF,
		        (WfMsgArg[]){WF_ARG_S(c), WF_ARG_S(d), WF_ARG_S(size),
		                WF_ARG_S(fit)},
		        4);
	} else {
		(void)wf_msgf(out, cap, MSG_IMG_INFO_IMAGE,
		        (WfMsgArg[]){WF_ARG_S(size), WF_ARG_S(fit)}, 2);
	}
}

bool wf_app_image_draw(const WfApp *app, WfFrame *f, int x, int y, int w, int h)
{
	const WfThemeStyles *th = wf_theme_styles();
	int rows = h >= 4 ? h - 1 : h;
	char line[256];

	if (WF_IMG_NONE == app->image.state) {
		return false;
	}
	if (WF_IMG_FAILED == app->image.state) {
		char buf[256];

		wf_app_put_fit(f, x, y, failure_text(app, buf, sizeof buf),
		        &th->dim, w);
		return true;
	}
	if (!app->image.pm) {
		if (app->now_ms >= app->preview_loading_due) {
			wf_app_put_fit(
			        f, x, y, wf_msg(MSG_IMG_LOADING), &th->dim, w);
		}
		return true;
	}
	if (!app->image.sixel_failed && app->image_cell_w &&
	        app->image.sixel.len && app->image.cell_w == cell_w(app) &&
	        app->image.cell_h == cell_h(app) && app->image.view_cols == w &&
	        app->image.view_rows == rows) {
		int gw = (int)((app->image.sixel_w + cell_w(app) - 1) /
		        cell_w(app));
		int gh = (int)((app->image.sixel_h + cell_h(app) - 1) /
		        cell_h(app));
		f->graphic.data = app->image.sixel.data;
		f->graphic.len = app->image.sixel.len;
		f->graphic.gen = app->image.gen;
		f->graphic.x = x + (w - gw) / 2;
		f->graphic.y = y + (rows - gh) / 2;
		f->graphic.w = gw;
		f->graphic.h = gh;
	} else if (!app->image_cell_w || app->image.sixel_failed) {
		wf_pixcell_draw_view(f, x, y, w, rows, app->image.pm,
		        app->image.pan_x, app->image.pan_y, th->base.bg_rgb,
		        WF_IMAGE_ASCII == app->image_style
		                ? WF_PIXSTYLE_ASCII
		                : WF_PIXSTYLE_HALFBLOCK,
		        NULL, NULL);
	} else if (WF_IMG_READY == app->image.state && !app->image.sixel.len) {
		wf_app_put_fit(
		        f, x, y, wf_msg(MSG_IMG_ERR_INTERNAL), &th->dim, w);
	}
	if (h >= 4) {
		info_line(app, line, sizeof line);
		if (WF_IMG_BUSY == app->image.state) {
			size_t n = strlen(line);

			snprintf(line + n, sizeof line - n, "  ...");
		}
		wf_frame_fill(f, x, y + h - 1, w, 1, ' ', &th->base);
		wf_app_put_fit(f, x, y + h - 1, line, &th->dim, w);
	}
	return true;
}
