#define COBJMACROS
#include <windows.h>
#include <psapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wincodec.h>

#include "preview/wf_decode_win.h"
#include "wf_imgfix.h"
#include "wf_test.h"

/*
 * A picture is untrusted input and the decoder is an attack surface (DR-DRAFT-039).
 * Whatever bytes arrive, the answer is a status, in bounded time and memory, and
 * a picture that is shown is a sane one. Three sets: files that claim absurd
 * sizes or break the container's own rules, byte-level damage to good files of
 * every format (a fixed seed, so a failure is the same failure again), and big
 * pictures that are fine and must be reduced rather than refused. In-process
 * here; the product runs the same code in the isolated host (test_dechost_win,
 * test_imageworker_win), which only adds walls.
 */

#if defined(__SANITIZE_ADDRESS__)
#	define HOSTILE_SANITIZED 1
#elif defined(__has_feature)
#	if __has_feature(address_sanitizer)
#		define HOSTILE_SANITIZED 1
#	endif
#endif
#ifndef HOSTILE_SANITIZED
#	define HOSTILE_SANITIZED 0
#endif

#define BOX_W 160u
#define BOX_H 90u
#define SLOW_MS 5000u /* the host's own limit is 12 s: well inside it */

static uint32_t g_rng = 0x9E3779B9u;

static uint32_t rnd(void)
{
	g_rng ^= g_rng << 13;
	g_rng ^= g_rng >> 17;
	g_rng ^= g_rng << 5;
	return g_rng;
}

static ULONGLONG g_slowest;
static const char *g_slowest_what = "";

/* Decodes and holds the answer to the rules every answer must keep. */
static WfDecodeStatus decode(const void *p, size_t n, const char *what)
{
	WfPixmap *pm = (WfPixmap *)(uintptr_t)1;
	WfDecodeInfo info;
	ULONGLONG t0 = GetTickCount64(), ms;
	WfDecodeStatus st;

	st = wf_decode_image(p, n, BOX_W, BOX_H, &pm, &info);
	ms = GetTickCount64() - t0;
	if (ms > g_slowest) {
		g_slowest = ms;
		g_slowest_what = what;
	}
	if (ms > SLOW_MS) {
		printf("SLOW %s: %llu ms\n", what, (unsigned long long)ms);
		CHECK_FAIL("a decode took longer than the bound");
	}
	if (WF_DSTAT_OK == st) {
		uint32_t sum = 0;

		CHECK(pm && pm->w >= 1 && pm->h >= 1 && pm->w <= BOX_W &&
		        pm->h <= BOX_H);
		if (pm) { /* every pixel is readable */
			for (size_t i = 0; i < (size_t)pm->w * pm->h; i++) {
				sum += pm->px[i];
			}
		}
		(void)sum;
		CHECK(info.src_w >= 1 && info.src_h >= 1);
	} else {
		CHECK(NULL == pm);
	}
	wf_pixmap_free(pm == (WfPixmap *)(uintptr_t)1 ? NULL : pm);
	return st;
}

static void must_refuse(const void *p, size_t n, const char *what)
{
	if (WF_DSTAT_OK == decode(p, n, what)) {
		printf("ACCEPTED %s\n", what);
		CHECK_FAIL("a hostile file was shown");
	}
}

static void png_with_a_size_that_is_absurd_is_refused(void)
{
	static const struct
	{
		const char *what;
		uint32_t w, h;
		int color, il;
	} c[] = {
	        {"png 16384x16384", 16384, 16384, 0, 0},
	        {"png 65535x65535", 65535, 65535, 2, 0},
	        {"png 2^31-1 square", 0x7FFFFFFFu, 0x7FFFFFFFu, 0, 0},
	        {"png 2^32-1 x 2^32-1", 0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0},
	        {"png 2^31 x 1", 0x80000000u, 1, 0, 0},
	        {"png 1 x 2^31-1", 1, 0x7FFFFFFFu, 0, 0},
	        {"png 0 x 0", 0, 0, 2, 0},
	        {"png 0 x 100", 0, 100, 0, 0},
	        {"png 100 x 0", 100, 0, 0, 0},
	        {"png interlaced 30000 square", 30000, 30000, 2, 1},
	};

	for (size_t i = 0; i < sizeof c / sizeof *c; i++) {
		size_t n;
		uint8_t *p =
		        fix_png_zeros(c[i].w, c[i].h, c[i].color, c[i].il, &n);

		must_refuse(p, n, c[i].what);
		free(p);
	}
}

static void png_with_a_broken_header_is_refused(void)
{
	static const struct
	{
		const char *what;
		unsigned depth, color, comp, filter, il;
	} c[] = {
	        {"depth 0", 0, 2, 0, 0, 0},
	        {"depth 3", 3, 2, 0, 0, 0},
	        {"depth 255", 255, 2, 0, 0, 0},
	        {"colour type 1", 8, 1, 0, 0, 0},
	        {"colour type 7", 8, 7, 0, 0, 0},
	        {"colour type 255", 8, 255, 0, 0, 0},
	        {"compression 1", 8, 2, 1, 0, 0},
	        {"filter 1", 8, 2, 0, 1, 0},
	        {"interlace 2", 8, 2, 0, 0, 2},
	        {"interlace 255", 8, 2, 0, 0, 255},
	};
	size_t base_n;
	uint8_t *base = fix_png_zeros(64, 48, 2, 0, &base_n);

	for (size_t i = 0; i < sizeof c / sizeof *c; i++) {
		/* the IHDR payload sits at 16..28; its CRC after it: made valid again,
		 * so it is the content that is refused and not the checksum */
		uint8_t *p = malloc(base_n);
		uint32_t crc;

		memcpy(p, base, base_n);
		p[24] = (uint8_t)c[i].depth;
		p[25] = (uint8_t)c[i].color;
		p[26] = (uint8_t)c[i].comp;
		p[27] = (uint8_t)c[i].filter;
		p[28] = (uint8_t)c[i].il;
		crc = fix_crc(p + 12, 17, 0);
		p[29] = (uint8_t)(crc >> 24);
		p[30] = (uint8_t)(crc >> 16);
		p[31] = (uint8_t)(crc >> 8);
		p[32] = (uint8_t)crc;
		must_refuse(p, base_n, c[i].what);
		free(p);
	}
	free(base);
}

static void png_chunks_that_overrun_or_repeat_are_refused(void)
{
	size_t n;
	uint8_t *p = fix_png_zeros(64, 48, 2, 0, &n);
	FixBuf b = {0};

	/* an IDAT whose length runs past the end of the file */
	b.p = malloc(n);
	memcpy(b.p, p, n);
	b.n = n;
	b.p[33] = 0xFF;
	b.p[34] = 0xFF;
	b.p[35] = 0xFF;
	b.p[36] = 0xF0;
	must_refuse(b.p, b.n, "idat length past the end");
	free(b.p);
	/* thousands of small chunks before the data: slow to walk is not allowed */
	{
		FixBuf m = {0};
		uint8_t text[8] = "k\0value";

		fix_put(&m, p, 33); /* signature and IHDR */
		for (int i = 0; i < 100000; i++) {
			fix_chunk(&m, "tEXt", text, sizeof text);
		}
		fix_put(&m, p + 33, n - 33);
		(void)decode(m.p, m.n,
		        "100000 text chunks"); /* any answer, in time */
		free(m.p);
	}
	/* no IEND, no IDAT, a second IHDR */
	must_refuse(p, n - 12, "no IEND");
	must_refuse(p, 8 + 25, "signature and IHDR only");
	free(p);
}

static void put_le16(FixBuf *b, unsigned v)
{
	fix_put8(b, v & 255);
	fix_put8(b, v >> 8);
}

static void bitmaps_that_lie_are_refused(void)
{
	static const struct
	{
		const char *what;
		int32_t w, h;
		unsigned bits;
		uint64_t body;
	} c[] = {
	        {"bmp 40000x40000, no pixels", 40000, 40000, 24, 0},
	        {"bmp 2^31-1 square, 32 bit", 0x7FFFFFFF, 0x7FFFFFFF, 32, 0},
	        {"bmp width -2^31", (int32_t)0x80000000, 100, 24, 64},
	        {"bmp height -2^31", 100, (int32_t)0x80000000, 24, 64},
	        {"bmp 16384x16384, cut", 16384, 16384, 24, 1 << 20},
	        {"bmp 0 bits", 1, 1, 0, 8},
	        {"bmp 99 bits", 1, 1, 99, 8},
	        {"bmp 0 x 0", 0, 0, 24, 8},
	        {"bmp 1 x 2^30", 1, 1 << 30, 32, 64},
	        {"bmp 2^30 x 4", 1 << 30, 4, 32, 64},
	};

	for (size_t i = 0; i < sizeof c / sizeof *c; i++) {
		size_t n;
		uint8_t *p = fix_bmp(c[i].w, c[i].h, c[i].bits, c[i].body, &n);

		must_refuse(p, n, c[i].what);
		free(p);
	}
	/* a palette that claims 4 billion colours, a header that claims 2 GiB, and
	 * RLE for a picture that is 100000 pixels wide */
	{
		size_t n;
		uint8_t *p = fix_bmp(8, 8, 8, 256, &n);

		p[46] = p[47] = p[48] = p[49] = 0xFF; /* biClrUsed */
		must_refuse(p, n, "bmp palette of 2^32-1 colours");
		free(p);
		p = fix_bmp(8, 8, 8, 256, &n);
		p[14] = p[15] = p[16] = 0xFF;
		p[17] = 0x7F; /* biSize */
		must_refuse(p, n, "bmp header of 2 GiB");
		free(p);
		p = fix_bmp(100000, 100000, 8, 64, &n);
		p[30] = 1; /* BI_RLE8 */
		must_refuse(p, n, "bmp rle 100000 square");
		free(p);
		p = fix_bmp(8, 8, 24, 192, &n);
		p[10] = p[11] = p[12] = 0xFF;
		p[13] = 0x7F; /* the pixels start 2 GiB in */
		must_refuse(p, n, "bmp pixels start past the file");
		free(p);
	}
}

static void gif_jpeg_and_tiff_that_lie_are_refused(void)
{
	/* GIF: a 65535 x 65535 screen and image with one empty block of data */
	{
		FixBuf b = {0};

		fix_put(&b, "GIF89a", 6);
		put_le16(&b, 65535);
		put_le16(&b, 65535);
		fix_put8(&b, 0x00);
		fix_put8(&b, 0);
		fix_put8(&b, 0);
		fix_put8(&b, 0x2C);
		put_le16(&b, 0);
		put_le16(&b, 0);
		put_le16(&b, 65535);
		put_le16(&b, 65535);
		fix_put8(&b, 0);
		fix_put8(&b, 8); /* LZW minimum code size */
		fix_put8(&b, 0); /* an empty block ends the data */
		fix_put8(&b, 0x3B);
		must_refuse(b.p, b.n, "gif 65535 square, no data");
		free(b.p);
	}
	/* JPEG: SOF0 for 65535 x 65535, one component, a scan that is nothing */
	{
		FixBuf b = {0};
		static const uint8_t sof[] = {0xFF, 0xC0, 0, 11, 8, 0xFF, 0xFF,
		        0xFF, 0xFF, 1, 1, 0x11, 0};
		static const uint8_t sos[] = {
		        0xFF, 0xDA, 0, 8, 1, 1, 0, 0, 63, 0};

		fix_put(&b, "\xFF\xD8", 2);
		fix_put(&b, sof, sizeof sof);
		fix_put(&b, sos, sizeof sos);
		fix_put(&b, "\0\0\0\0", 4);
		fix_put(&b, "\xFF\xD9", 2);
		must_refuse(b.p, b.n, "jpeg 65535 square, no tables");
		free(b.p);
	}
	/* TIFF: dimensions 2^32-1, a strip far outside the file, and an IFD that
	 * is its own next IFD */
	{
		FixBuf b = {0};
		static const struct
		{
			unsigned tag, type;
			uint32_t count, value;
		} e[] = {{256, 4, 1, 0xFFFFFFFFu}, {257, 4, 1, 0xFFFFFFFFu},
		        {258, 3, 1, 8}, {259, 3, 1, 1}, {262, 3, 1, 1},
		        {273, 4, 1, 0x7FFFFFF0u}, {277, 3, 1, 1},
		        {278, 4, 1, 0xFFFFFFFFu}, {279, 4, 1, 0xFFFFFFFFu}};

		fix_put(&b, "II*\0", 4);
		fix_le32(&b, 8);
		put_le16(&b, (unsigned)(sizeof e / sizeof *e));
		for (size_t i = 0; i < sizeof e / sizeof *e; i++) {
			put_le16(&b, e[i].tag);
			put_le16(&b, e[i].type);
			fix_le32(&b, e[i].count);
			fix_le32(&b, e[i].value);
		}
		fix_le32(&b, 8); /* next IFD: itself */
		must_refuse(b.p, b.n,
		        "tiff 2^32-1 square, strip outside, IFD loop");
		free(b.p);
	}
	/* an ICO whose one image is a 65535-pixel-wide PNG claim */
	{
		size_t n;
		uint8_t *png = fix_png_zeros(65535, 65535, 0, 0, &n);
		FixBuf b = {0};

		put_le16(&b, 0);
		put_le16(&b, 1);
		put_le16(&b, 1);
		fix_put8(&b, 0);
		fix_put8(&b, 0);
		fix_put8(&b, 0);
		fix_put8(&b, 0);
		put_le16(&b, 1);
		put_le16(&b, 32);
		fix_le32(&b, (uint32_t)n);
		fix_le32(&b, 22);
		fix_put(&b, png, n);
		must_refuse(b.p, b.n, "ico holding a 65535 square png");
		free(b.p);
		free(png);
	}
}

static void nothing_or_almost_nothing_is_refused(void)
{
	static const char *const heads[] = {"\x89PNG\r\n\x1a\n", "\xFF\xD8\xFF",
	        "GIF89a", "BM", "II*\0", "MM\0*", "RIFF\0\0\0\0WEBP",
	        "\0\0\1\0", "%PDF-1.7"};
	uint8_t big[4096];

	must_refuse("", 0, "empty");
	must_refuse("x", 1, "one byte");
	for (size_t i = 0; i < sizeof heads / sizeof *heads; i++) {
		must_refuse(heads[i], strlen(heads[i]) ? strlen(heads[i]) : 4,
		        "only a signature");
	}
	memset(big, 0, sizeof big);
	must_refuse(big, sizeof big, "zeros");
	memset(big, 0xFF, sizeof big);
	must_refuse(big, sizeof big, "0xFF");
	for (size_t i = 0; i < sizeof big; i++) {
		big[i] = (uint8_t)rnd();
	}
	must_refuse(big, sizeof big, "noise");
	/* a size that does not fit what the host takes */
	{
		WfPixmap *pm = NULL;

		CHECK(WF_DSTAT_OK !=
		        wf_decode_image(big, (size_t)WF_DWIRE_INPUT_MAX + 1,
		                BOX_W, BOX_H, &pm, NULL));
		CHECK(NULL == pm);
		CHECK(WF_DSTAT_OK !=
		        wf_decode_image(big, 100, 0, BOX_H, &pm, NULL));
		CHECK(WF_DSTAT_OK !=
		        wf_decode_image(big, 100, BOX_W, 0, &pm, NULL));
	}
}

typedef struct
{
	uint8_t *p;
	size_t n;
} Blob;

static Blob wic_encode(const GUID *container, UINT w, UINT h, int alpha)
{
	Blob blob = {NULL, 0};
	IWICImagingFactory *wic = NULL;
	IWICBitmapEncoder *enc = NULL;
	IWICBitmapFrameEncode *fe = NULL;
	IPropertyBag2 *bag = NULL;
	IStream *stream = NULL;
	IWICBitmap *bmp = NULL;
	WICPixelFormatGUID f = alpha ? GUID_WICPixelFormat32bppBGRA
	                             : GUID_WICPixelFormat24bppBGR;
	WICRect all = {0, 0, (INT)w, (INT)h};
	uint8_t *pix = malloc((size_t)w * h * 4);

	for (size_t i = 0; i < (size_t)w * h; i++) {
		pix[i * 4] = (uint8_t)(i * 7);
		pix[i * 4 + 1] = (uint8_t)(i / w * 5);
		pix[i * 4 + 2] = (uint8_t)(i % w * 3);
		pix[i * 4 + 3] = 255;
	}
	CoInitializeEx(NULL, COINIT_MULTITHREADED);
	CHECK(SUCCEEDED(CoCreateInstance(&CLSID_WICImagingFactory, NULL,
	        CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&wic)));
	CHECK(SUCCEEDED(CreateStreamOnHGlobal(NULL, TRUE, &stream)));
	if (FAILED(IWICImagingFactory_CreateEncoder(
	            wic, container, NULL, &enc)) ||
	        FAILED(IWICBitmapEncoder_Initialize(
	                enc, stream, WICBitmapEncoderNoCache)) ||
	        FAILED(IWICBitmapEncoder_CreateNewFrame(enc, &fe, &bag)) ||
	        FAILED(IWICBitmapFrameEncode_Initialize(fe, bag)) ||
	        FAILED(IWICBitmapFrameEncode_SetSize(fe, w, h)) ||
	        FAILED(IWICBitmapFrameEncode_SetPixelFormat(fe, &f)) ||
	        FAILED(IWICImagingFactory_CreateBitmapFromMemory(wic, w, h,
	                &GUID_WICPixelFormat32bppBGRA, w * 4, w * 4 * h, pix,
	                &bmp)) ||
	        FAILED(IWICBitmapFrameEncode_WriteSource(
	                fe, (IWICBitmapSource *)bmp, &all)) ||
	        FAILED(IWICBitmapFrameEncode_Commit(fe)) ||
	        FAILED(IWICBitmapEncoder_Commit(enc))) {
		CHECK_FAIL("could not make a sample picture");
		goto done;
	}
	{
		HGLOBAL g = NULL;
		STATSTG st;

		GetHGlobalFromStream(stream, &g);
		IStream_Stat(stream, &st, STATFLAG_NONAME);
		blob.n = (size_t)st.cbSize.QuadPart;
		blob.p = malloc(blob.n);
		memcpy(blob.p, GlobalLock(g), blob.n);
		GlobalUnlock(g);
	}
done:
	free(pix);
	if (bmp) {
		IWICBitmap_Release(bmp);
	}
	if (bag) {
		IPropertyBag2_Release(bag);
	}
	if (fe) {
		IWICBitmapFrameEncode_Release(fe);
	}
	if (enc) {
		IWICBitmapEncoder_Release(enc);
	}
	if (stream) {
		IStream_Release(stream);
	}
	if (wic) {
		IWICImagingFactory_Release(wic);
	}
	return blob;
}

/* A PNG inside the ICO container: the one icon format that is easy to make. */
static Blob ico_of(const Blob *png)
{
	FixBuf b = {0};
	Blob out;

	put_le16(&b, 0);
	put_le16(&b, 1);
	put_le16(&b, 1);
	fix_put8(&b, 64);
	fix_put8(&b, 48);
	fix_put8(&b, 0);
	fix_put8(&b, 0);
	put_le16(&b, 1);
	put_le16(&b, 32);
	fix_le32(&b, (uint32_t)png->n);
	fix_le32(&b, 22);
	fix_put(&b, png->p, png->n);
	out.p = b.p;
	out.n = b.n;
	return out;
}

/* One damaged copy of `good`: the kind of damage is chosen by the sequence. */
static Blob damaged(const Blob *good)
{
	Blob d;
	static const uint32_t evil[] = {0, 1, 0x7FFFFFFFu, 0x80000000u,
	        0xFFFFFFFFu, 0x00FFFFFFu, 0xFFFF0000u};
	unsigned kind = rnd() % 7;
	size_t at;

	d.p = malloc(good->n + 4096);
	memcpy(d.p, good->p, good->n);
	d.n = good->n;
	switch (kind) {
	case 0: /* one byte */ d.p[rnd() % d.n] = (uint8_t)rnd(); break;
	case 1: /* a few bytes, near the front where the headers are */
		for (int i = 0, k = 2 + (int)(rnd() % 4); i < k; i++) {
			d.p[rnd() % (d.n < 200 ? d.n : 200)] = (uint8_t)rnd();
		}
		break;
	case 2: /* cut off */ d.n = rnd() % d.n; break;
	case 3: /* a number that is a size, replaced by an evil one */
		at = rnd() % (d.n < 64 ? d.n - 4 : 60);
		memcpy(d.p + at, &evil[rnd() % 7], 4);
		break;
	case 4: /* a stretch repeated */
	{
		size_t from = rnd() % d.n, len = 1 + rnd() % 2000;

		if (from + len > d.n) {
			len = d.n - from;
		}
		memmove(d.p + from + len, d.p + from, d.n - from);
		d.n += len;
		break;
	}
	case 5: /* a stretch zeroed */
	{
		size_t from = rnd() % d.n, len = 1 + rnd() % 64;

		if (from + len > d.n) {
			len = d.n - from;
		}
		memset(d.p + from, 0, len);
		break;
	}
	default: /* a bit flipped anywhere */
		d.p[rnd() % d.n] ^= (uint8_t)(1u << (rnd() % 8));
		break;
	}
	return d;
}

#define DAMAGED_PER_FORMAT 600

static void damage_to_good_files_never_crashes_or_hangs(void)
{
	static const struct
	{
		const char *name;
		const GUID *c;
	} f[] = {{"png", &GUID_ContainerFormatPng},
	        {"jpeg", &GUID_ContainerFormatJpeg},
	        {"gif", &GUID_ContainerFormatGif},
	        {"bmp", &GUID_ContainerFormatBmp},
	        {"tiff", &GUID_ContainerFormatTiff}};
	size_t refused = 0, shown = 0, shown_of[6] = {0};

	for (size_t i = 0; i < sizeof f / sizeof *f + 1; i++) {
		Blob good = i < sizeof f / sizeof *f
		        ? wic_encode(f[i].c, 64, 48, 0)
		        : (Blob){NULL, 0};
		const char *name = i < sizeof f / sizeof *f ? f[i].name : "ico";

		if (i == sizeof f / sizeof *f) {
			Blob png =
			        wic_encode(&GUID_ContainerFormatPng, 64, 48, 1);

			good = ico_of(&png);
			free(png.p);
		}
		if (!good.p) {
			continue;
		}
		WfDecodeStatus first =
		        decode(good.p, good.n, name); /* the original */

		if (WF_DSTAT_OK != first) {
			printf("the original %s is not shown (status %d)\n",
			        name, (int)first);
			CHECK_FAIL("a good sample was refused");
		}
		for (int k = 0; k < DAMAGED_PER_FORMAT; k++) {
			Blob d = damaged(&good);

			if (WF_DSTAT_OK == decode(d.p, d.n, name)) {
				shown++;
				shown_of[i]++;
			} else {
				refused++;
			}
			free(d.p);
		}
		printf("  %s: %zu of %d damaged copies still shown\n", name,
		        shown_of[i], DAMAGED_PER_FORMAT);
		free(good.p);
	}
	printf("damage: %zu refused, %zu shown; slowest %llu ms (%s)\n",
	        refused, shown, (unsigned long long)g_slowest, g_slowest_what);
	/* PNG carries a checksum on every chunk: damage to it is noticed, and what
	 * is still shown is damage the format cannot see (a zeroed stretch of
	 * ancillary text). JPEG, GIF, BMP and TIFF have no such checksum: a flipped
	 * pixel or entropy bit is a picture with a flaw in it, and viewers show it */
	CHECK(shown_of[0] * 20 < DAMAGED_PER_FORMAT);
	CHECK(refused > 0);
}

static void big_pictures_are_reduced_not_refused(void)
{
	static const struct
	{
		const char *what;
		uint32_t w, h;
		int color, il;
	} c[] = {
	        {"8K rgb", 7680, 4320, 2, 0},
	        {"8K rgb interlaced", 7680, 4320, 2, 1},
	        {"16K rgb", 15360, 8640, 2, 0},
	        {"16K gray", 15360, 8640, 0, 0},
	        {"16K rgb interlaced", 15360, 8640, 2, 1},
	        {"just inside the pixel limit", 15999, 15999, 0, 0},
	        {"1 x 100000 (a thin strip)", 1, 100000, 0, 0},
	        {"100000 x 1", 100000, 1, 0, 0},
	};

	for (size_t i = 0; i < sizeof c / sizeof *c; i++) {
		size_t n;
		uint8_t *p =
		        fix_png_zeros(c[i].w, c[i].h, c[i].color, c[i].il, &n);
		WfPixmap *pm = NULL;
		WfDecodeInfo info;
		WfDecodeStatus st =
		        wf_decode_image(p, n, BOX_W, BOX_H, &pm, &info);

		if (c[i].w * (unsigned long long)c[i].h <=
		        WF_DECODE_SOURCE_PIXELS) {
			CHECK(WF_DSTAT_OK == st);
			if (pm) {
				CHECK(pm->w <= BOX_W && pm->h <= BOX_H);
				CHECK(info.src_w == c[i].w &&
				        info.src_h == c[i].h);
			}
		} else {
			CHECK(WF_DSTAT_OK != st && NULL == pm);
		}
		wf_pixmap_free(pm);
		free(p);
	}
	/* 16K from a real encoder is the same */
	{
		Blob big = wic_encode(&GUID_ContainerFormatJpeg, 8192, 4608, 0);
		WfPixmap *pm = NULL;

		CHECK(big.p);
		CHECK(WF_DSTAT_OK ==
		        wf_decode_image(big.p, big.n, BOX_W, BOX_H, &pm, NULL));
		CHECK(pm && pm->w <= BOX_W && pm->h <= BOX_H);
		wf_pixmap_free(pm);
		free(big.p);
	}
}

/* The box a picture is reduced to comes from the terminal's size (a few cells to a
 * few hundred, times the cell size): every box, down to 1 x 1 and up to the
 * most the protocol allows, gives a picture that is inside it and never a read or
 * write outside the buffers (this is where the address sanitizer looks). */
static void every_box_gives_a_picture_inside_it(void)
{
	static const uint32_t boxes[][2] = {{1, 1}, {1, 2}, {2, 1}, {2, 3},
	        {3, 2}, {1, 8192}, {8192, 1}, {7, 5}, {40, 12}, {160, 90},
	        {1000, 3}, {3, 1000}, {2048, 2048}, {8192, 8192}};
	static const struct
	{
		uint32_t w, h;
		int color, il;
	} pics[] = {{7680, 4320, 2, 0}, {15360, 8640, 2, 1}, {1, 100000, 0, 0},
	        {100000, 1, 0, 0}, {1, 1, 2, 0}, {3, 5, 0, 1}, {257, 3, 2, 1}};

	for (size_t i = 0; i < sizeof pics / sizeof *pics; i++) {
		size_t n;
		uint8_t *p = fix_png_zeros(
		        pics[i].w, pics[i].h, pics[i].color, pics[i].il, &n);

		for (size_t b = 0; b < sizeof boxes / sizeof *boxes; b++) {
			WfPixmap *pm = NULL;
			WfDecodeInfo info;
			WfDecodeStatus st = wf_decode_image(
			        p, n, boxes[b][0], boxes[b][1], &pm, &info);

			/* a box whose picture would be over the pixmap limit (16 Mi pixels:
			 * only 8192 x 8192 here) is refused as too large, never half made;
			 * the program asks for at most 4 Mi pixels (wf_app_image.c) */
			bool huge = 8192 == boxes[b][0] && 8192 == boxes[b][1];

			CHECK(WF_DSTAT_OK == st ||
			        (huge && WF_DSTAT_TOO_LARGE == st));
			CHECK((WF_DSTAT_OK == st) == (NULL != pm));
			if (pm) {
				CHECK(pm->w >= 1 && pm->h >= 1 &&
				        pm->w <= boxes[b][0] &&
				        pm->h <= boxes[b][1]);
				for (size_t k = 0; k < (size_t)pm->w * pm->h;
				        k++) {
					(void)pm->px
					        [k]; /* every pixel is readable */
				}
			}
			wf_pixmap_free(pm);
		}
		free(p);
	}
}

/* Everything above together must not have grown the process by gigabytes. */
static void the_whole_run_stayed_inside_a_memory_bound(void)
{
	PROCESS_MEMORY_COUNTERS_EX mc;

	memset(&mc, 0, sizeof mc);
	CHECK(GetProcessMemoryInfo(GetCurrentProcess(),
	        (PROCESS_MEMORY_COUNTERS *)&mc, sizeof mc));
	printf("peak private %.0f MiB\n",
	        (double)mc.PeakPagefileUsage / (1 << 20));
	/* under the address sanitizer freed memory is held back (a quarantine) and
	 * the bound means nothing: what it finds is the check there */
	if (!HOSTILE_SANITIZED) {
		CHECK(mc.PeakPagefileUsage < (size_t)700 << 20);
	}
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	png_with_a_size_that_is_absurd_is_refused();
	png_with_a_broken_header_is_refused();
	png_chunks_that_overrun_or_repeat_are_refused();
	bitmaps_that_lie_are_refused();
	gif_jpeg_and_tiff_that_lie_are_refused();
	nothing_or_almost_nothing_is_refused();
	big_pictures_are_reduced_not_refused();
	every_box_gives_a_picture_inside_it();
	damage_to_good_files_never_crashes_or_hangs();
	the_whole_run_stayed_inside_a_memory_bound();
	return TEST_RESULT();
}
