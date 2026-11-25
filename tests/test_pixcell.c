#include <string.h>

#include "render/wf_pixcell.h"
#include "text/wf_text.h"
#include "wf_test.h"

static const WfCell *at(const WfFrame *f, int x, int y)
{
	return &f->cells[(size_t)y * (size_t)f->cols + (size_t)x];
}

static uint32_t argb(unsigned a, unsigned r, unsigned g, unsigned b)
{
	return (uint32_t)a << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | b;
}

static void the_grid_is_two_square_pixels_per_cell(void)
{
	uint32_t w, h;

	wf_pixcell_grid(40, 12, &w, &h);
	CHECK(40 == w && 24 == h);
	wf_pixcell_grid(0, 0, &w, &h);
	CHECK(1 == w && 2 == h);
}

static void halfblock_puts_the_upper_pixel_in_the_foreground(void)
{
	WfStyle blank = {0xEEEEEE, 0x101010, 0};
	WfFrame *f = wf_frame_new(10, 6, &blank);
	WfPixmap *pm = NULL;
	int uc, ur;

	CHECK(NULL != f);
	CHECK(WF_OK == wf_pixmap_new(2, 4, &pm));
	pm->px[0] = argb(255, 255, 0, 0);   /* (0,0) upper of cell (0,0) */
	pm->px[2] = argb(255, 0, 255, 0);   /* (0,1) lower of cell (0,0) */
	pm->px[1] = argb(255, 0, 0, 255);   /* (1,0) */
	pm->px[3] = argb(255, 255, 255, 0); /* (1,1) */
	pm->px[4] = argb(255, 1, 2, 3);     /* (0,2) upper of cell (0,1) */
	pm->px[6] = argb(255, 4, 5, 6);
	/* 4 x 2 cells... the picture is 2 wide, 4 tall: 2 cells wide, 2 high */
	wf_pixcell_draw(
	        f, 0, 0, 10, 6, pm, 0x101010, WF_PIXSTYLE_HALFBLOCK, &uc, &ur);
	CHECK(2 == uc && 2 == ur);
	/* centred: (10 - 2) / 2 = 4 columns in, (6 - 2) / 2 = 2 rows down */
	CHECK(0 == strcmp(at(f, 4, 2)->utf8, "\xE2\x96\x80"));
	CHECK(0xFF0000 == at(f, 4, 2)->fg_rgb &&
	        0x00FF00 == at(f, 4, 2)->bg_rgb);
	CHECK(0x0000FF == at(f, 5, 2)->fg_rgb &&
	        0xFFFF00 == at(f, 5, 2)->bg_rgb);
	CHECK(0x010203 == at(f, 4, 3)->fg_rgb &&
	        0x040506 == at(f, 4, 3)->bg_rgb);
	/* the margin is the background */
	CHECK(0 == strcmp(at(f, 0, 0)->utf8, " ") &&
	        0x101010 == at(f, 0, 0)->bg_rgb);
	CHECK(0x101010 == at(f, 9, 5)->bg_rgb);
	wf_pixmap_free(pm);
	wf_frame_free(f);
}

static void an_odd_number_of_pixel_rows_leaves_a_half_cell(void)
{
	WfStyle blank = {0, 0x202020, 0};
	WfFrame *f = wf_frame_new(4, 4, &blank);
	WfPixmap *pm;
	int uc, ur;

	CHECK(WF_OK == wf_pixmap_new(1, 3, &pm));
	pm->px[0] = argb(255, 10, 10, 10);
	pm->px[1] = argb(255, 20, 20, 20);
	pm->px[2] = argb(255, 30, 30, 30);
	wf_pixcell_draw(
	        f, 0, 0, 4, 4, pm, 0x202020, WF_PIXSTYLE_HALFBLOCK, &uc, &ur);
	CHECK(1 == uc && 2 == ur);
	/* the last pixel is the upper half of the last cell; the lower half is
	 * the background, not a stale or random colour */
	CHECK(0x0A0A0A == at(f, 1, 1)->fg_rgb &&
	        0x141414 == at(f, 1, 1)->bg_rgb);
	CHECK(0x1E1E1E == at(f, 1, 2)->fg_rgb);
	CHECK(0x202020 == at(f, 1, 2)->bg_rgb);
	wf_pixmap_free(pm);
	wf_frame_free(f);
}

static void transparency_shows_the_background(void)
{
	WfStyle blank = {0, 0x336699, 0};
	WfFrame *f = wf_frame_new(3, 3, &blank);
	WfPixmap *pm;

	CHECK(WF_OK == wf_pixmap_new(1, 2, &pm));
	pm->px[0] = 0; /* fully transparent */
	pm->px[1] = argb(255, 1, 2, 3);
	wf_pixcell_draw(
	        f, 0, 0, 3, 3, pm, 0x336699, WF_PIXSTYLE_HALFBLOCK, NULL, NULL);
	CHECK(0x336699 == at(f, 1, 1)->fg_rgb);
	CHECK(0x010203 == at(f, 1, 1)->bg_rgb);
	wf_pixmap_free(pm);
	wf_frame_free(f);
}

static void a_picture_larger_than_the_area_is_clipped_not_overrun(void)
{
	WfStyle blank = {0, 0, 0};
	WfFrame *f = wf_frame_new(5, 5, &blank);
	WfPixmap *pm;
	int uc, ur;

	CHECK(WF_OK == wf_pixmap_new(40, 40, &pm));
	for (size_t i = 0; i < 1600; i++) {
		pm->px[i] = argb(255, 9, 9, 9);
	}
	/* an area inside the frame; cells outside it must stay untouched */
	wf_pixcell_draw(f, 1, 1, 3, 3, pm, 0, WF_PIXSTYLE_HALFBLOCK, &uc, &ur);
	CHECK(3 == uc && 3 == ur);
	CHECK(0 == strcmp(at(f, 0, 0)->utf8, " ") && 0 == at(f, 0, 0)->bg_rgb);
	CHECK(0 == strcmp(at(f, 4, 4)->utf8, " "));
	CHECK(0x090909 == at(f, 3, 3)->fg_rgb);
	/* an area partly outside the frame is clipped by the frame */
	wf_pixcell_draw(
	        f, 3, 3, 9, 9, pm, 0, WF_PIXSTYLE_HALFBLOCK, NULL, NULL);
	/* no picture and a null frame are harmless */
	wf_pixcell_draw(
	        f, 0, 0, 5, 5, NULL, 0, WF_PIXSTYLE_HALFBLOCK, NULL, NULL);
	wf_pixcell_draw(NULL, 0, 0, 5, 5, pm, 0, WF_PIXSTYLE_ASCII, NULL, NULL);
	wf_pixcell_draw(f, 0, 0, 0, 5, pm, 0, WF_PIXSTYLE_ASCII, NULL, NULL);
	wf_pixmap_free(pm);
	wf_frame_free(f);
}

static void ascii_density_follows_brightness(void)
{
	WfStyle blank = {0, 0, 0};
	WfFrame *f = wf_frame_new(4, 2, &blank);
	WfPixmap *pm;

	CHECK(WF_OK == wf_pixmap_new(2, 2, &pm));
	pm->px[0] = argb(255, 255, 255, 255); /* white over white: densest */
	pm->px[2] = argb(255, 255, 255, 255);
	pm->px[1] = argb(255, 0, 0, 0); /* black over black: empty */
	pm->px[3] = argb(255, 0, 0, 0);
	wf_pixcell_draw(f, 0, 0, 4, 2, pm, 0, WF_PIXSTYLE_ASCII, NULL, NULL);
	CHECK(0 == strcmp(at(f, 1, 0)->utf8, "@"));
	CHECK(0xFFFFFF == at(f, 1, 0)->fg_rgb);
	CHECK(0 == strcmp(at(f, 2, 0)->utf8, " "));
	wf_pixmap_free(pm);
	wf_frame_free(f);
}

static void a_larger_picture_can_be_panned_and_never_past_its_edge(void)
{
	WfStyle blank = {0, 0, 0};
	WfFrame *f = wf_frame_new(4, 3, &blank);
	WfPixmap *pm = NULL;
	uint32_t mx, my;

	/* 10 x 8 pixels, each coloured by its position: red = x, green = y */
	CHECK(WF_OK == wf_pixmap_new(10, 8, &pm));
	for (uint32_t y = 0; y < 8; y++) {
		for (uint32_t x = 0; x < 10; x++) {
			pm->px[y * 10 + x] = argb(255, x, y, 0);
		}
	}
	/* the area is 4 cells wide and 3 high: 4 x 6 pixels */
	wf_pixcell_pan_limits(pm, 4, 3, &mx, &my);
	CHECK(6 == mx && 2 == my);
	wf_pixcell_draw_view(
	        f, 0, 0, 4, 3, pm, 0, 0, 0, WF_PIXSTYLE_HALFBLOCK, NULL, NULL);
	CHECK(0x000000 == at(f, 0, 0)->fg_rgb &&
	        0x000100 == at(f, 0, 0)->bg_rgb);
	CHECK(0x030000 == at(f, 3, 0)->fg_rgb);
	CHECK(0x000200 == at(f, 0, 1)->fg_rgb &&
	        0x030500 == at(f, 3, 2)->bg_rgb);
	/* moved right and down by 2 */
	wf_pixcell_draw_view(
	        f, 0, 0, 4, 3, pm, 2, 2, 0, WF_PIXSTYLE_HALFBLOCK, NULL, NULL);
	CHECK(0x020200 == at(f, 0, 0)->fg_rgb);
	CHECK(0x050700 == at(f, 3, 2)->bg_rgb);
	/* an offset beyond the edge is held at the edge */
	wf_pixcell_draw_view(f, 0, 0, 4, 3, pm, 999, 999, 0,
	        WF_PIXSTYLE_HALFBLOCK, NULL, NULL);
	CHECK(0x060200 == at(f, 0, 0)->fg_rgb);
	CHECK(0x090700 == at(f, 3, 2)->bg_rgb);
	/* a picture that fits is centred and its offset ignored */
	wf_pixmap_free(pm);
	CHECK(WF_OK == wf_pixmap_new(2, 2, &pm));
	pm->px[0] = argb(255, 1, 1, 1);
	wf_pixcell_pan_limits(pm, 4, 3, &mx, &my);
	CHECK(0 == mx && 0 == my);
	wf_pixcell_draw_view(
	        f, 0, 0, 4, 3, pm, 3, 3, 0, WF_PIXSTYLE_HALFBLOCK, NULL, NULL);
	CHECK(0x010101 == at(f, 1, 1)->fg_rgb);
	wf_pixmap_free(pm);
	wf_frame_free(f);
}

/* With ambiguous characters drawn two cells wide, a half block would tear the
 * picture (or turn into "?"): every cell is a space on the mean colour. */
static void a_wide_ambiguous_setting_never_draws_the_half_block(void)
{
	WfStyle blank = {0, 0, 0};
	WfFrame *f = wf_frame_new(4, 2, &blank);
	WfPixmap *pm;

	CHECK(WF_OK == wf_pixmap_new(2, 2, &pm));
	pm->px[0] = pm->px[1] = argb(255, 200, 0, 0);
	pm->px[2] = pm->px[3] = argb(255, 0, 0, 100);
	wf_set_ambiguous_width(2);
	wf_pixcell_draw(
	        f, 0, 0, 4, 2, pm, 0, WF_PIXSTYLE_HALFBLOCK, NULL, NULL);
	wf_set_ambiguous_width(1);
	/* centred: columns 1 and 2 */
	for (int x = 1; x < 3; x++) {
		CHECK(0 == strcmp(at(f, x, 0)->utf8, " "));
		CHECK(0x640032 == at(f, x, 0)->bg_rgb);
	}
	wf_pixmap_free(pm);
	wf_frame_free(f);
}

int main(void)
{
	the_grid_is_two_square_pixels_per_cell();
	halfblock_puts_the_upper_pixel_in_the_foreground();
	an_odd_number_of_pixel_rows_leaves_a_half_cell();
	transparency_shows_the_background();
	a_picture_larger_than_the_area_is_clipped_not_overrun();
	ascii_density_follows_brightness();
	a_larger_picture_can_be_panned_and_never_past_its_edge();
	a_wide_ambiguous_setting_never_draws_the_half_block();
	return TEST_RESULT();
}
