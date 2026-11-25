#include <stdlib.h>
#include <string.h>

#include "base/wf_macro.h"
#include "preview/wf_pixmap.h"
#include "wf_test.h"

static uint32_t argb(unsigned a, unsigned r, unsigned g, unsigned b)
{
	return (uint32_t)a << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | b;
}

static void sizes_are_bounded(void)
{
	WfPixmap *p = (WfPixmap *)1;

	CHECK(0 == wf_pixmap_bytes(0, 5) && 0 == wf_pixmap_bytes(5, 0));
	CHECK(0 == wf_pixmap_bytes(WF_PIXMAP_MAX_SIDE + 1, 1));
	CHECK(0 == wf_pixmap_bytes(WF_PIXMAP_MAX_SIDE, WF_PIXMAP_MAX_SIDE));
	CHECK(0 == wf_pixmap_bytes(4097, 4097)); /* just over 16 Mpixel */
	CHECK(4u * 4096 * 4096 == wf_pixmap_bytes(4096, 4096));
	CHECK(WF_E_INVAL == wf_pixmap_new(0, 3, &p) && NULL == p);
	CHECK(WF_E_INVAL == wf_pixmap_new(3, 3, NULL));
	CHECK(WF_OK == wf_pixmap_new(3, 2, &p) && 3 == p->w && 2 == p->h);
	for (size_t i = 0; i < 6; i++) {
		CHECK(0 == p->px[i]); /* transparent */
	}
	wf_pixmap_free(p);
	wf_pixmap_free(NULL);
}

static void from_bgra_honours_the_stride(void)
{
	unsigned char raw[2 * 12];
	WfPixmap *p;

	for (size_t i = 0; i < sizeof raw; i++) {
		raw[i] = (unsigned char)(i + 1);
	}
	/* 2 pixels per row, rows 12 bytes apart: the last 4 bytes are padding */
	CHECK(WF_OK == wf_pixmap_from_bgra(raw, 2, 2, 12, &p));
	CHECK(p->px[0] == 0x04030201u && p->px[1] == 0x08070605u);
	CHECK(p->px[2] == 0x100F0E0Du && p->px[3] == 0x14131211u);
	wf_pixmap_free(p);
	CHECK(WF_E_INVAL == wf_pixmap_from_bgra(raw, 4, 1, 8, &p) && NULL == p);
	CHECK(WF_E_INVAL == wf_pixmap_from_bgra(NULL, 1, 1, 4, &p));
}

static void fit_keeps_the_aspect_ratio(void)
{
	uint32_t w, h;

	wf_fit_box(1000, 500, 100, 100, 0, &w, &h);
	CHECK(100 == w && 50 == h);
	wf_fit_box(500, 1000, 100, 100, 0, &w, &h);
	CHECK(50 == w && 100 == h);
	/* smaller than the box: kept, unless enlarging is asked for */
	wf_fit_box(40, 20, 100, 100, 0, &w, &h);
	CHECK(40 == w && 20 == h);
	wf_fit_box(40, 20, 100, 100, 1, &w, &h);
	CHECK(100 == w && 50 == h);
	/* a 1 pixel wide strip never rounds to nothing */
	wf_fit_box(1, 5000, 80, 24, 0, &w, &h);
	CHECK(1 == w && 24 == h);
	wf_fit_box(5000, 1, 80, 24, 0, &w, &h);
	CHECK(80 == w && 1 == h);
	/* zero sizes are survivable */
	wf_fit_box(0, 0, 0, 0, 1, &w, &h);
	CHECK(1 == w && 1 == h);
	/* the result is never beyond the box, whatever the rounding */
	for (uint32_t sw = 1; sw < 40; sw += 3) {
		for (uint32_t sh = 1; sh < 40; sh += 5) {
			wf_fit_box(sw, sh, 17, 9, 1, &w, &h);
			CHECK(w >= 1 && w <= 17 && h >= 1 && h <= 9);
		}
	}
}

static void shrinking_averages_the_area(void)
{
	WfPixmap *src, *dst;

	/* a 4x4 checkerboard of black and white averages to mid grey */
	CHECK(WF_OK == wf_pixmap_new(4, 4, &src));
	for (uint32_t y = 0; y < 4; y++) {
		for (uint32_t x = 0; x < 4; x++) {
			src->px[y * 4 + x] = (x + y) & 1
			        ? argb(255, 255, 255, 255)
			        : argb(255, 0, 0, 0);
		}
	}
	CHECK(WF_OK == wf_pixmap_scale(src, 1, 1, &dst));
	CHECK(255 == dst->px[0] >> 24);
	for (int shift = 16; shift >= 0; shift -= 8) {
		unsigned c = (dst->px[0] >> shift) & 255;

		CHECK(127 == c || 128 == c);
	}
	wf_pixmap_free(dst);
	/* 2x2 halves: each output pixel is the mean of a 2x2 block */
	CHECK(WF_OK == wf_pixmap_scale(src, 2, 2, &dst));
	for (int i = 0; i < 4; i++) {
		unsigned c = dst->px[i] & 255;

		CHECK(127 == c || 128 == c);
	}
	wf_pixmap_free(dst);
	wf_pixmap_free(src);
}

static void a_flat_colour_stays_exactly_that_colour(void)
{
	/* any ratio, shrinking or enlarging, in both directions at once */
	static const uint32_t sizes[][2] = {
	        {1, 1}, {2, 3}, {7, 5}, {13, 29}, {64, 33}, {100, 7}};
	WfPixmap *src, *dst;

	CHECK(WF_OK == wf_pixmap_new(37, 23, &src));
	for (size_t i = 0; i < 37u * 23u; i++) {
		src->px[i] = argb(200, 150, 60, 20);
	}
	for (size_t i = 0; i < WF_ARRAY_LEN(sizes); i++) {
		CHECK(WF_OK ==
		        wf_pixmap_scale(src, sizes[i][0], sizes[i][1], &dst));
		for (size_t k = 0; k < (size_t)sizes[i][0] * sizes[i][1]; k++) {
			if (dst->px[k] != argb(200, 150, 60, 20)) {
				printf("  %ux%u pixel %zu = %08x\n",
				        sizes[i][0], sizes[i][1], k,
				        dst->px[k]);
				CHECK_FAIL("flat colour changed");
				break;
			}
		}
		wf_pixmap_free(dst);
	}
	wf_pixmap_free(src);
}

static void enlarging_is_linear_and_stays_in_range(void)
{
	WfPixmap *src, *dst;

	CHECK(WF_OK == wf_pixmap_new(2, 1, &src));
	src->px[0] = argb(255, 0, 0, 0);
	src->px[1] = argb(255, 255, 255, 255);
	CHECK(WF_OK == wf_pixmap_scale(src, 8, 1, &dst));
	/* monotone from black to white, ends exact */
	CHECK(0 == (dst->px[0] & 255) && 255 == (dst->px[7] & 255));
	for (int i = 1; i < 8; i++) {
		CHECK((dst->px[i] & 255) >= (dst->px[i - 1] & 255));
		CHECK(255 == dst->px[i] >> 24);
	}
	wf_pixmap_free(dst);
	wf_pixmap_free(src);
}

static void transparent_pixels_do_not_bleed_their_colour(void)
{
	WfPixmap *src, *dst;

	/* opaque red next to fully transparent "blue": premultiplied, the blue
	 * carries no colour, so the mean is half-transparent pure red */
	CHECK(WF_OK == wf_pixmap_new(2, 1, &src));
	src->px[0] = argb(255, 255, 0, 0);
	src->px[1] = 0; /* premultiplied transparent */
	CHECK(WF_OK == wf_pixmap_scale(src, 1, 1, &dst));
	CHECK(127 <= (dst->px[0] >> 24) && 128 >= (dst->px[0] >> 24));
	CHECK(127 <= ((dst->px[0] >> 16) & 255) &&
	        128 >= ((dst->px[0] >> 16) & 255));
	CHECK(0 == (dst->px[0] & 0xFFFF));
	wf_pixmap_free(dst);
	wf_pixmap_free(src);
}

static void compositing_over_a_background(void)
{
	/* opaque wins, transparent shows the background, half mixes */
	CHECK(0x102030 == wf_px_over(argb(255, 0x10, 0x20, 0x30), 0xFFFFFF));
	CHECK(0xA0B0C0 == wf_px_over(0, 0xA0B0C0));
	/* premultiplied half-white over black is mid grey */
	CHECK(0x808080 == wf_px_over(argb(128, 128, 128, 128), 0x000000));
	/* premultiplied half-black over white: white * (1 - 128/255) */
	{
		uint32_t c = wf_px_over(argb(128, 0, 0, 0), 0xFFFFFF);

		CHECK(0x7F == (c & 255) || 0x80 == (c & 255));
	}
	/* never wraps even for an invalid pixel whose colour exceeds its alpha */
	CHECK(0xFFFFFF == wf_px_over(argb(10, 255, 255, 255), 0xFFFFFF));
}

/* A 3 wide, 2 tall picture whose pixels are numbered row by row:
 *     1 2 3
 *     4 5 6                                                                */
static WfPixmap *numbered(void)
{
	WfPixmap *p;

	CHECK(WF_OK == wf_pixmap_new(3, 2, &p));
	for (uint32_t i = 0; i < 6; i++) {
		p->px[i] = i + 1;
	}
	return p;
}

static int pixels_are(
        const WfPixmap *p, uint32_t w, uint32_t h, const char *digits)
{
	if (!p || p->w != w || p->h != h) {
		return 0;
	}
	for (uint32_t i = 0; i < w * h; i++) {
		if (p->px[i] != (uint32_t)(digits[i] - '0')) {
			return 0;
		}
	}
	return 1;
}

static void exif_orientations_turn_the_picture_the_way_the_camera_meant(void)
{
	static const struct
	{
		int o;
		uint32_t w, h;
		const char *want;
	} cases[] = {{1, 3, 2, "123456"}, {2, 3, 2, "321654"},
	        {3, 3, 2, "654321"}, {4, 3, 2, "456123"}, {5, 2, 3, "142536"},
	        {6, 2, 3, "415263"}, {7, 2, 3, "635241"}, {8, 2, 3, "362514"}};
	WfPixmap *src = numbered(), *out;

	for (size_t i = 0; i < WF_ARRAY_LEN(cases); i++) {
		CHECK(WF_OK == wf_pixmap_orient(src, cases[i].o, &out));
		if (!pixels_are(out, cases[i].w, cases[i].h, cases[i].want)) {
			printf("  orientation %d wrong\n", cases[i].o);
			CHECK_FAIL("orientation");
		}
		wf_pixmap_free(out);
	}
	CHECK(WF_E_INVAL == wf_pixmap_orient(src, 0, &out) && NULL == out);
	CHECK(WF_E_INVAL == wf_pixmap_orient(src, 9, &out));
	CHECK(WF_E_INVAL == wf_pixmap_orient(NULL, 1, &out));
	CHECK(WF_E_INVAL == wf_pixmap_orient(src, 1, NULL));
	wf_pixmap_free(src);
}

static void four_quarter_turns_come_back_to_the_start(void)
{
	WfPixmap *a = numbered(), *b;

	for (int i = 0; i < 4; i++) {
		CHECK(WF_OK == wf_pixmap_orient(a, 6, &b));
		wf_pixmap_free(a);
		a = b;
	}
	CHECK(pixels_are(a, 3, 2, "123456"));
	wf_pixmap_free(a);
}

static void huge_requests_fail_instead_of_allocating(void)
{
	WfPixmap *src, *dst = (WfPixmap *)1;

	CHECK(WF_OK == wf_pixmap_new(8, 8, &src));
	CHECK(WF_E_INVAL == wf_pixmap_scale(src, 0, 5, &dst) && NULL == dst);
	CHECK(WF_E_INVAL ==
	        wf_pixmap_scale(src, WF_PIXMAP_MAX_SIDE + 1, 5, &dst));
	CHECK(WF_E_INVAL == wf_pixmap_scale(NULL, 5, 5, &dst));
	wf_pixmap_free(src);
}

int main(void)
{
	sizes_are_bounded();
	from_bgra_honours_the_stride();
	fit_keeps_the_aspect_ratio();
	shrinking_averages_the_area();
	a_flat_colour_stays_exactly_that_colour();
	enlarging_is_linear_and_stays_in_range();
	transparent_pixels_do_not_bleed_their_colour();
	compositing_over_a_background();
	exif_orientations_turn_the_picture_the_way_the_camera_meant();
	four_quarter_turns_come_back_to_the_start();
	huge_requests_fail_instead_of_allocating();
	return TEST_RESULT();
}
