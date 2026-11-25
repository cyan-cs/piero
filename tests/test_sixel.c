#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>

#include "render/wf_sixel.h"
#include "wf_test.h"

/*
 * A small independent Sixel reader, written from the format description, used
 * to check what the encoder wrote by looking at the pixels it would paint.
 * It understands exactly what the encoder promises to emit: DCS q, raster
 * attributes, colour definitions and selections, repeats, '$' and '-'.
 */
typedef struct
{
	uint32_t *pix; /* w * h, 0x00RRGGBB, initialised to the background */
	uint32_t w, h;
	unsigned defs; /* colour definitions seen */
	unsigned max_index;
	int ok;
} Decoded;

static bool number(
        const char **p, const char *end, unsigned *out, char separator)
{
	unsigned v = 0;

	if (*p == end || **p < '0' || **p > '9') {
		return false;
	}
	while (*p < end && **p >= '0' && **p <= '9') {
		unsigned digit = (unsigned)(**p - '0');

		if (v > (UINT_MAX - digit) / 10) {
			return false;
		}
		v = v * 10 + digit;
		(*p)++;
	}
	if (separator) {
		if (*p == end || **p != separator) {
			return false;
		}
		(*p)++;
	}
	*out = v;
	return true;
}

static Decoded decode(const char *s, size_t n, uint32_t bg)
{
	Decoded d;
	const char *p = s, *end = s + n;
	uint32_t pal[256];
	unsigned cur = 0, x = 0, band = 0;

	memset(&d, 0, sizeof d);
	memset(pal, 0, sizeof pal);
	if (n < 6 || 0x1B != (unsigned char)p[0] || 'P' != p[1]) {
		return d;
	}
	p += 2;
	while (p < end && 'q' != *p) {
		p++;
	}
	if (p >= end) {
		return d;
	}
	p++;
	if (p < end && '"' == *p) {
		unsigned aspect;

		p++;
		if (!number(&p, end, &aspect, ';') ||
		        !number(&p, end, &aspect, ';') ||
		        !number(&p, end, &d.w, ';') ||
		        !number(&p, end, &d.h, 0)) {
			return d;
		}
	}
	if (0 == d.w || 0 == d.h || d.w > 4096 || d.h > 4096) {
		return d;
	}
	d.pix = malloc((size_t)d.w * d.h * sizeof *d.pix);
	if (!d.pix) {
		return d;
	}
	for (size_t i = 0; i < (size_t)d.w * d.h; i++) {
		d.pix[i] = bg;
	}
	while (p < end) {
		unsigned char c = (unsigned char)*p;

		if (0x1B == c) {
			d.ok = p + 1 < end && '\\' == p[1] && p + 2 == end;
			return d;
		}
		if ('#' == c) {
			p++;
			if (!number(&p, end, &cur, 0) || cur > 255) {
				return d;
			}
			if (cur > d.max_index) {
				d.max_index = cur;
			}
			if (p < end && ';' == *p) {
				unsigned kind, r, g, b;

				p++;
				if (!number(&p, end, &kind, ';') ||
				        !number(&p, end, &r, ';') ||
				        !number(&p, end, &g, ';') ||
				        !number(&p, end, &b, 0) || 2 != kind ||
				        r > 100 || g > 100 || b > 100) {
					return d;
				}
				pal[cur] = ((r * 255 + 50) / 100) << 16 |
				        ((g * 255 + 50) / 100) << 8 |
				        ((b * 255 + 50) / 100);
				d.defs++;
			}
			continue;
		}
		if ('$' == c) {
			x = 0;
			p++;
			continue;
		}
		if ('-' == c) {
			if (band >= (d.h - 1) / 6) {
				return d;
			}
			x = 0;
			band++;
			p++;
			continue;
		}
		if ('!' == c || (c >= 63 && c <= 126)) {
			unsigned count = 1;

			if ('!' == c) {
				p++;
				if (!number(&p, end, &count, 0) || !count ||
				        p == end) {
					return d;
				}
				c = (unsigned char)*p;
			}
			if (c < 63 || c > 126) {
				return d;
			}
			if (x > d.w || count > d.w - x) {
				return d;
			}
			p++;
			for (unsigned k = 0; k < count; k++, x++) {
				for (unsigned bit = 0; bit < 6; bit++) {
					unsigned y = band * 6 + bit;

					if ((c - 63) >> bit & 1) {
						if (x >= d.w || y >= d.h) {
							return d; /* painted outside the picture */
						}
						d.pix[(size_t)y * d.w + x] =
						        pal[cur];
					}
				}
			}
			continue;
		}
		return d; /* anything else is a bug in the encoder */
	}
	return d;
}

static uint32_t argb(unsigned a, unsigned r, unsigned g, unsigned b)
{
	return (uint32_t)a << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | b;
}

static void decoder_rejects_malformed_output(void)
{
	static const char good[] = "\x1bPq\"1;1;1;6#0;2;100;0;0~\x1b\\";
	static const char *const bad[] = {"\x1bPq\"1:1;1;6~\x1b\\",
	        "\x1bPq\"1;1;1;6#0;2;100;0\x1b\\",
	        "\x1bPq\"1;1;1;6#4294967296~\x1b\\",
	        "\x1bPq\"1;1;1;6!4294967297~\x1b\\", "\x1bPq\"1;1;1;6!0~\x1b\\",
	        "\x1bPq\"1;1;1;6!2?\x1b\\"};

	for (size_t n = 0; n < sizeof good - 1; n++) {
		Decoded d = decode(good, n, 0);

		CHECK(!d.ok);
		free(d.pix);
	}
	for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
		Decoded d = decode(bad[i], strlen(bad[i]), 0);

		CHECK(!d.ok);
		free(d.pix);
	}
	Decoded d = decode(good, sizeof good - 1, 0);
	CHECK(d.ok && d.pix && d.pix[0] == 0xff0000);
	free(d.pix);
}

static int near(uint32_t a, uint32_t b, int tol)
{
	for (int shift = 16; shift >= 0; shift -= 8) {
		int d = (int)((a >> shift) & 255) - (int)((b >> shift) & 255);

		if (d > tol || d < -tol) {
			return 0;
		}
	}
	return 1;
}

/* Encodes, decodes and compares every pixel with the opaque source within
 * the rounding of a percentage palette. */
static void roundtrip(const WfPixmap *pm, uint32_t bg, unsigned colors, int tol)
{
	WfBuf b;
	Decoded d;

	wf_buf_init(&b);
	CHECK(WF_OK == wf_sixel_encode(pm, bg, colors, &b));
	d = decode(wf_buf_cstr(&b), b.len, bg);
	CHECK(d.ok && d.w == pm->w && d.h == pm->h);
	if (d.ok && d.w == pm->w && d.h == pm->h) {
		for (size_t i = 0; i < (size_t)pm->w * pm->h; i++) {
			uint32_t want = wf_px_over(pm->px[i], bg);

			if (!near(d.pix[i], want, tol)) {
				printf("  %ux%u pixel %zu: got %06x want "
				       "%06x\n",
				        pm->w, pm->h, i, d.pix[i], want);
				CHECK_FAIL("pixel differs");
				break;
			}
		}
	}
	free(d.pix);
	wf_buf_free(&b);
}

static void few_colours_come_back_exactly_for_every_size(void)
{
	static const uint32_t pal[] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00,
	        0x00FFFF, 0xFF00FF, 0x808080, 0xFFFFFF};

	/* heights 1..14 cross every partial six-row band; widths 1..9 */
	for (uint32_t h = 1; h <= 14; h++) {
		for (uint32_t w = 1; w <= 9; w++) {
			WfPixmap *pm;

			CHECK(WF_OK == wf_pixmap_new(w, h, &pm));
			for (uint32_t i = 0; i < w * h; i++) {
				pm->px[i] =
				        0xFF000000u | pal[(i * 7 + i / w) % 8];
			}
			roundtrip(pm, 0x000000, 256, 2);
			wf_pixmap_free(pm);
		}
	}
}

static void a_long_run_is_run_length_coded(void)
{
	WfPixmap *pm;
	WfBuf b;

	CHECK(WF_OK == wf_pixmap_new(300, 6, &pm));
	for (size_t i = 0; i < 1800; i++) {
		pm->px[i] = argb(255, 200, 30, 30);
	}
	wf_buf_init(&b);
	CHECK(WF_OK == wf_sixel_encode(pm, 0x000000, 16, &b));
	CHECK(NULL != strstr(wf_buf_cstr(&b), "!300~"));
	CHECK(b.len < 80);
	wf_buf_free(&b);
	roundtrip(pm, 0x000000, 16, 2);
	wf_pixmap_free(pm);
}

static void the_palette_is_never_larger_than_asked(void)
{
	WfPixmap *pm;
	WfBuf b;
	Decoded d;

	CHECK(WF_OK == wf_pixmap_new(64, 40, &pm));
	for (uint32_t y = 0; y < 40; y++) {
		for (uint32_t x = 0; x < 64; x++) {
			pm->px[y * 64 + x] =
			        argb(255, x * 4, y * 6, (x + y) * 2);
		}
	}
	for (unsigned colors = 2; colors <= 256; colors *= 4) {
		wf_buf_init(&b);
		CHECK(WF_OK == wf_sixel_encode(pm, 0x101010, colors, &b));
		d = decode(wf_buf_cstr(&b), b.len, 0x101010);
		CHECK(d.ok && d.defs >= 1 && d.defs <= colors);
		free(d.pix);
		wf_buf_free(&b);
	}
	/* a request out of range is clamped, not honoured: never more than 256 */
	wf_buf_init(&b);
	CHECK(WF_OK == wf_sixel_encode(pm, 0x101010, 100000, &b));
	d = decode(wf_buf_cstr(&b), b.len, 0x101010);
	CHECK(d.ok && d.defs <= 256);
	free(d.pix);
	wf_buf_free(&b);
	/* with many colours the picture is close: a smooth gradient stays within
	 * the width of one 5-bit bin plus palette rounding */
	roundtrip(pm, 0x101010, 256, 24);
	wf_pixmap_free(pm);
}

static void background_pixels_are_not_painted(void)
{
	WfPixmap *pm;
	WfBuf painted, bare;
	Decoded d;

	CHECK(WF_OK == wf_pixmap_new(20, 12, &pm));
	/* transparent everywhere except one red pixel */
	pm->px[5 * 20 + 7] = argb(255, 255, 0, 0);
	wf_buf_init(&painted);
	CHECK(WF_OK == wf_sixel_encode(pm, 0x202020, 8, &painted));
	d = decode(wf_buf_cstr(&painted), painted.len, 0x202020);
	CHECK(d.ok);
	for (uint32_t y = 0; y < 12; y++) {
		for (uint32_t x = 0; x < 20; x++) {
			uint32_t want = 5 == y && 7 == x ? 0xFF0000 : 0x202020;

			CHECK(near(d.pix[y * 20 + x], want, 2));
		}
	}
	free(d.pix);
	/* the same picture on a red background paints nothing: one palette entry
	 * and no pixel data at all */
	wf_buf_init(&bare);
	for (size_t i = 0; i < 240; i++) {
		pm->px[i] = argb(255, 0x20, 0x20, 0x20);
	}
	CHECK(WF_OK == wf_sixel_encode(pm, 0x202020, 8, &bare));
	d = decode(wf_buf_cstr(&bare), bare.len, 0x202020);
	CHECK(d.ok && 0 == strchr(wf_buf_cstr(&bare), '~'));
	free(d.pix);
	wf_buf_free(&painted);
	wf_buf_free(&bare);
	wf_pixmap_free(pm);
}

static void appends_and_refuses_bad_input_without_touching_the_buffer(void)
{
	WfPixmap *pm;
	WfBuf b;
	size_t before;

	CHECK(WF_OK == wf_pixmap_new(4, 4, &pm));
	wf_buf_init(&b);
	CHECK(WF_OK == wf_buf_puts(&b, "keep"));
	before = b.len;
	CHECK(WF_E_INVAL == wf_sixel_encode(NULL, 0, 8, &b));
	CHECK(WF_E_INVAL == wf_sixel_encode(pm, 0, 8, NULL));
	CHECK(before == b.len && 0 == strcmp(wf_buf_cstr(&b), "keep"));
	CHECK(WF_OK == wf_sixel_encode(pm, 0x000000, 8, &b));
	CHECK(0 == strncmp(wf_buf_cstr(&b), "keep\x1bP", 6));
	CHECK(0 == strcmp(wf_buf_cstr(&b) + b.len - 2, "\x1b\\"));
	wf_buf_free(&b);
	wf_pixmap_free(pm);
}

static bool cancelled(void *ctx)
{
	(void)ctx;
	return true;
}

static void bounds_and_cancel(void)
{
	WfPixmap *p = NULL;
	WfBuf out = {0};
	CHECK(WF_OK == wf_pixmap_new(10, 24, &p));
	for (unsigned i = 180; i < 240; i++) {
		p->px[i] = 0xffff0000;
	}
	CHECK(WF_OK == wf_sixel_encode(p, 0, 256, &out));
	CHECK(strstr(out.data, "---#") != NULL); /* three transparent bands */
	wf_buf_clear(&out);
	CHECK(WF_OK == wf_buf_puts(&out, "keep"));
	CHECK(WF_E_CANCELLED ==
	        wf_sixel_encode_ex(p, 0, 256, true, cancelled, NULL, &out));
	CHECK(!strcmp(out.data, "keep"));
	p->w = UINT32_MAX;
	CHECK(WF_E_INVAL == wf_sixel_encode(p, 0, 256, &out));
	p->w = 16384;
	p->h = 256; /* rejected before reading tiny allocation */
	CHECK(WF_E_RANGE == wf_sixel_encode(p, 0, 256, &out));
	wf_pixmap_free(p);
	wf_buf_free(&out);
}

static void output_limit_rolls_back(void)
{
	WfPixmap *pm = NULL;
	WfBuf out = {0};
	uint32_t rng = 12345;
	CHECK(WF_OK == wf_pixmap_new(1600, 1000, &pm));
	if (!pm) {
		return;
	}
	for (size_t i = 0; i < 1600000; i++) {
		rng = rng * 1664525u + 1013904223u;
		pm->px[i] = 0xff000000u | (rng & 0xffffffu);
	}
	CHECK(WF_OK == wf_buf_puts(&out, "keep"));
	CHECK(WF_E_RANGE == wf_sixel_encode(pm, 0, 256, &out));
	CHECK(out.len == 4 && !strcmp(out.data, "keep"));
	wf_buf_free(&out);
	wf_pixmap_free(pm);
}

static uint64_t fnv1a(const char *p, size_t n)
{
	uint64_t h = 0xcbf29ce484222325ull;

	for (size_t i = 0; i < n; i++) {
		h = (h ^ (unsigned char)p[i]) * 0x100000001b3ull;
	}
	return h;
}

/* A picture that a test can make again: smooth, noisy, and with few colours. */
static WfPixmap *sample(int kind)
{
	WfPixmap *pm = NULL;
	uint32_t w = 3 == kind ? 40 : 96, h = 3 == kind ? 24 : 80, seed = 12345;

	CHECK(WF_OK == wf_pixmap_new(w, h, &pm));
	for (uint32_t y = 0; y < h; y++) {
		for (uint32_t x = 0; x < w; x++) {
			uint32_t r, g, b;

			seed = seed * 1664525u + 1013904223u;
			if (0 == kind) { /* a gradient */
				r = x * 255 / (w - 1);
				g = y * 255 / (h - 1);
				b = (x + y) * 255 / (w + h - 2);
			} else if (1 == kind) { /* noise */
				r = seed >> 24;
				g = (seed >> 16) & 255;
				b = (seed >> 8) & 255;
			} else if (2 ==
			        kind) { /* a gradient with a transparent corner */
				r = x * 2;
				g = y * 3;
				b = 128;
				if (x < 10 && y < 10) {
					pm->px[y * w + x] = 0;
					continue;
				}
			} else { /* six colours */
				static const uint32_t six[] = {0xFF0000,
				        0x00FF00, 0x0000FF, 0xFFFF00, 0x00FFFF,
				        0x808080};
				uint32_t c = six[(x / 5 + y / 4) % 6];

				r = c >> 16;
				g = (c >> 8) & 255;
				b = c & 255;
			}
			pm->px[y * w + x] = argb(255, r, g, b);
		}
	}
	return pm;
}

/* Fixed output fingerprints cover gradients, noise, transparency and palettes,
 * with and without dithering. Pixel round trips above check their meaning. */
static void encoding_matches_fixtures(void)
{
	static const unsigned colors[] = {2, 16, 64, 256};
	static const uint64_t want[4][4][2] = {
	        {{0xf8d27f3330ccb27bull, 0xc4319bf0d2a95e3dull},
	                {0x1876d8710ad9df12ull, 0x11a83ae3eb9428feull},
	                {0x38d1e89ea002ac83ull, 0x30faa58fa9d79788ull},
	                {0xd66999ff991ca2acull, 0xe457f18e343705d0ull}},
	        {{0x813c799ce685bdbeull, 0xf9517a7850177181ull},
	                {0x53243acf0edbfa7dull, 0xbe51aa7c65d8fc17ull},
	                {0x208058486adf2e1cull, 0xdcced05ff867030eull},
	                {0x976a6dae7ebd9c04ull, 0x9f0518356ac52d8cull}},
	        {{0x13baf9c729918073ull, 0x935443c10a17a2bdull},
	                {0xf15061595b71b386ull, 0xa552bc43fb5060e4ull},
	                {0xa530156b5bc51e11ull, 0xe5909e022e84f895ull},
	                {0xe60e63949679b4cfull, 0x14e3cd8baad5f137ull}},
	        {{0xf74e2f94208237bfull, 0xd5ecf55038a5be5eull},
	                {0x7ee692db298c3e40ull, 0x7ee692db298c3e40ull},
	                {0x7ee692db298c3e40ull, 0x7ee692db298c3e40ull},
	                {0x7ee692db298c3e40ull, 0x7ee692db298c3e40ull}}};
	size_t seen = 0;

	for (int kind = 0; kind < 4; kind++) {
		WfPixmap *pm = sample(kind);

		for (int c = 0; c < 4; c++) {
			for (int dither = 0; dither < 2; dither++) {
				WfBuf b;
				uint64_t got;

				wf_buf_init(&b);
				CHECK(WF_OK ==
				        wf_sixel_encode_ex(pm, 0x101010,
				                colors[c], dither != 0, NULL,
				                NULL, &b));
				got = fnv1a(wf_buf_cstr(&b), b.len);
				if (got != want[kind][c][dither]) {
					printf("  kind %d colours %u dither "
					       "%d: %016llx\n",
					        kind, colors[c], dither,
					        (unsigned long long)got);
					CHECK_FAIL("encoder output changed");
				}
				seen++;
				wf_buf_free(&b);
			}
		}
		wf_pixmap_free(pm);
	}
	CHECK(32 == seen);
}

/* The time of an encoding without dithering: it does not make the nearest-colour
 * table that only a dithered picture reads. Printed, not asserted. */
static void encoding_time_without_dither(void)
{
	WfPixmap *pm = NULL;
	clock_t t0;
	double ms = 0;

	CHECK(WF_OK == wf_pixmap_new(512, 384, &pm));
	for (uint32_t y = 0; y < 384; y++) {
		for (uint32_t x = 0; x < 512; x++) {
			pm->px[y * 512 + x] =
			        argb(255, x / 2, y * 2 / 3, (x + y) / 4);
		}
	}
	for (int i = 0; i < 5; i++) {
		WfBuf b;

		wf_buf_init(&b);
		t0 = clock();
		CHECK(WF_OK ==
		        wf_sixel_encode_ex(
		                pm, 0x101010, 256, false, NULL, NULL, &b));
		ms += (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
		wf_buf_free(&b);
	}
	printf("info: Sixel encoding of 512x384 at 256 colours, no dither: "
	       "%.2f ms\n",
	        ms / 5);
	wf_pixmap_free(pm);
}

int main(void)
{
	output_limit_rolls_back();
	decoder_rejects_malformed_output();
	encoding_matches_fixtures();
	encoding_time_without_dither();
	bounds_and_cancel();
	few_colours_come_back_exactly_for_every_size();
	a_long_run_is_run_length_coded();
	the_palette_is_never_larger_than_asked();
	background_pixels_are_not_painted();
	appends_and_refuses_bad_input_without_touching_the_buffer();
	return TEST_RESULT();
}
