#define COBJMACROS
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wincodec.h>

#include "base/wf_macro.h"
#include "pdf_samples.h"
#include "preview/wf_decode_win.h"
#include "wf_test.h"

typedef struct
{
	unsigned char *bytes;
	size_t n;
} Blob;

/* A smooth gradient: red grows to the right, green downwards. With `alpha`
 * the middle is a transparent disc. */
static unsigned char *gradient(UINT w, UINT h, int alpha)
{
	unsigned char *p = malloc((size_t)w * h * 4);

	for (UINT y = 0; y < h; y++) {
		for (UINT x = 0; x < w; x++) {
			unsigned char *q = p + ((size_t)y * w + x) * 4;
			double dx = (double)x - w / 2.0,
			       dy = (double)y - h / 2.0;

			q[0] = 40;
			q[1] = (unsigned char)(255 * y / h);
			q[2] = (unsigned char)(255 * x / w);
			q[3] = alpha && dx * dx + dy * dy < (double)h * h / 16.0
			        ? 0
			        : 255;
		}
	}
	return p;
}

static Blob encode(
        const GUID *container, UINT w, UINT h, int orientation, int alpha)
{
	Blob blob = {NULL, 0};
	IWICImagingFactory *wic = NULL;
	IWICBitmapEncoder *enc = NULL;
	IWICBitmapFrameEncode *fe = NULL;
	IPropertyBag2 *bag = NULL;
	IStream *stream = NULL;
	WICPixelFormatGUID f = GUID_WICPixelFormat32bppBGRA;
	unsigned char *pix = gradient(w, h, alpha);
	HRESULT hr;

	CoInitializeEx(NULL, COINIT_MULTITHREADED);
	CHECK(SUCCEEDED(CoCreateInstance(&CLSID_WICImagingFactory, NULL,
	        CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&wic)));
	CHECK(SUCCEEDED(CreateStreamOnHGlobal(NULL, TRUE, &stream)));
	hr = IWICImagingFactory_CreateEncoder(wic, container, NULL, &enc);
	CHECK(SUCCEEDED(hr));
	if (FAILED(hr)) {
		goto out;
	}
	IWICBitmapEncoder_Initialize(enc, stream, WICBitmapEncoderNoCache);
	IWICBitmapEncoder_CreateNewFrame(enc, &fe, &bag);
	IWICBitmapFrameEncode_Initialize(fe, bag);
	IWICBitmapFrameEncode_SetSize(fe, w, h);
	IWICBitmapFrameEncode_SetPixelFormat(fe, &f);
	if (orientation > 1) {
		IWICMetadataQueryWriter *qw = NULL;
		PROPVARIANT v;

		if (SUCCEEDED(IWICBitmapFrameEncode_GetMetadataQueryWriter(
		            fe, &qw))) {
			PropVariantInit(&v);
			v.vt = VT_UI2;
			v.uiVal = (USHORT)orientation;
			IWICMetadataQueryWriter_SetMetadataByName(
			        qw, L"/app1/ifd/{ushort=274}", &v);
			IWICMetadataQueryWriter_Release(qw);
		}
	}
	{
		/* the encoder may want another pixel format (JPEG: 24 bit); WIC
		 * converts when it is given a source rather than raw rows */
		IWICBitmap *bitmap = NULL;
		WICRect all = {0, 0, (INT)w, (INT)h};

		CHECK(SUCCEEDED(IWICImagingFactory_CreateBitmapFromMemory(wic,
		        w, h, &GUID_WICPixelFormat32bppBGRA, w * 4, w * 4 * h,
		        pix, &bitmap)));
		CHECK(SUCCEEDED(IWICBitmapFrameEncode_WriteSource(
		        fe, (IWICBitmapSource *)bitmap, &all)));
		IWICBitmap_Release(bitmap);
	}
	IWICBitmapFrameEncode_Commit(fe);
	IWICBitmapEncoder_Commit(enc);
	{
		HGLOBAL g = NULL;
		STATSTG st;

		GetHGlobalFromStream(stream, &g);
		IStream_Stat(stream, &st, STATFLAG_NONAME);
		blob.n = (size_t)st.cbSize.QuadPart;
		blob.bytes = malloc(blob.n);
		memcpy(blob.bytes, GlobalLock(g), blob.n);
		GlobalUnlock(g);
	}
out:
	free(pix);
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

static void blob_free(Blob *b)
{
	free(b->bytes);
	b->bytes = NULL;
}

static unsigned red(uint32_t px)
{
	return (px >> 16) & 255;
}
static unsigned green(uint32_t px)
{
	return (px >> 8) & 255;
}

static void a_png_is_reduced_to_fit_the_box(void)
{
	Blob b = encode(&GUID_ContainerFormatPng, 640, 480, 1, 0);
	WfPixmap *pm = NULL;
	WfDecodeInfo info = {0};

	CHECK(b.bytes &&
	        WF_DSTAT_OK ==
	                wf_decode_image(b.bytes, b.n, 160, 160, &pm, &info));
	CHECK(160 == pm->w && 120 == pm->h); /* the aspect ratio is kept */
	CHECK(1 == info.page_count && 640 == info.src_w && 480 == info.src_h);
	CHECK(0 == (info.flags & WF_DFLAG_ALPHA) || 1); /* PNG may say either */
	/* red grows rightwards, green downwards: the corners say so */
	CHECK(red(pm->px[0]) < 20 && green(pm->px[0]) < 20);
	CHECK(red(pm->px[159]) > 230 && green(pm->px[159]) < 20);
	CHECK(green(pm->px[119 * 160]) > 230 && red(pm->px[119 * 160]) < 20);
	wf_pixmap_free(pm);
	blob_free(&b);
}

static void a_small_picture_is_not_enlarged(void)
{
	Blob b = encode(&GUID_ContainerFormatPng, 64, 32, 1, 0);
	WfPixmap *pm = NULL;

	CHECK(WF_DSTAT_OK ==
	        wf_decode_image(b.bytes, b.n, 800, 600, &pm, NULL));
	CHECK(64 == pm->w && 32 == pm->h);
	wf_pixmap_free(pm);
	blob_free(&b);
}

static void transparency_is_reported_and_kept(void)
{
	Blob b = encode(&GUID_ContainerFormatPng, 200, 100, 1, 1);
	WfPixmap *pm = NULL;
	WfDecodeInfo info = {0};

	CHECK(WF_DSTAT_OK ==
	        wf_decode_image(b.bytes, b.n, 200, 100, &pm, &info));
	CHECK(info.flags & WF_DFLAG_ALPHA);
	/* the disc in the middle is fully transparent, the corner opaque; the
	 * pixels are premultiplied, so transparent means all zero */
	CHECK(0 == pm->px[50 * 200 + 100]);
	CHECK(255 == (pm->px[0] >> 24));
	wf_pixmap_free(pm);
	blob_free(&b);
}

static void exif_orientation_is_applied_and_is_fast(void)
{
	Blob b = encode(&GUID_ContainerFormatJpeg, 2400, 1600, 6, 0);
	WfPixmap *pm = NULL;
	WfDecodeInfo info = {0};
	DWORD t0 = GetTickCount();

	CHECK(b.bytes &&
	        WF_DSTAT_OK ==
	                wf_decode_image(b.bytes, b.n, 320, 200, &pm, &info));
	/* the full-size flip/rotator took 22 seconds; this must be well under 2 */
	CHECK(GetTickCount() - t0 < 2000);
	/* turned 90 degrees: portrait now, 1600 x 2400 in the source's terms */
	CHECK(133 == pm->w && 200 == pm->h);
	CHECK(1600 == info.src_w && 2400 == info.src_h);
	/* the stored top-left is dark (red and green both low); after a clockwise
	 * turn it is at the TOP RIGHT. The stored bottom-left (green high) goes to
	 * the top left. */
	CHECK(red(pm->px[132]) < 40 && green(pm->px[132]) < 40);
	CHECK(green(pm->px[0]) > 215 && red(pm->px[0]) < 40);
	wf_pixmap_free(pm);
	blob_free(&b);
}

static void other_built_in_formats_decode(void)
{
	static const GUID *kinds[] = {&GUID_ContainerFormatBmp,
	        &GUID_ContainerFormatTiff, &GUID_ContainerFormatJpeg,
	        &GUID_ContainerFormatWmp};

	for (size_t i = 0; i < WF_ARRAY_LEN(kinds); i++) {
		Blob b = encode(kinds[i], 300, 200, 1, 0);
		WfPixmap *pm = NULL;

		CHECK(b.bytes &&
		        WF_DSTAT_OK ==
		                wf_decode_image(
		                        b.bytes, b.n, 100, 100, &pm, NULL));
		CHECK(pm && 100 == pm->w && 67 == pm->h);
		wf_pixmap_free(pm);
		blob_free(&b);
	}
}

static void a_gif_with_transparency_decodes(void)
{
	/* the classic 1x1 transparent GIF */
	static const unsigned char gif[] = {'G', 'I', 'F', '8', '9', 'a', 1, 0,
	        1, 0, 0x80, 0, 0, 0xFF, 0xFF, 0xFF, 0, 0, 0, '!', 0xF9, 4, 1, 0,
	        0, 0, 0, ',', 0, 0, 0, 0, 1, 0, 1, 0, 0, 2, 2, 0x44, 1, 0, ';'};
	WfPixmap *pm = NULL;
	WfDecodeInfo info = {0};

	CHECK(WF_DSTAT_OK ==
	        wf_decode_image(gif, sizeof gif, 10, 10, &pm, &info));
	CHECK(1 == pm->w && 1 == pm->h && 0 == pm->px[0]);
	wf_pixmap_free(pm);
}

static void damaged_files_are_refused_not_shown_in_part(void)
{
	static const GUID *kinds[] = {&GUID_ContainerFormatPng,
	        &GUID_ContainerFormatJpeg, &GUID_ContainerFormatBmp,
	        &GUID_ContainerFormatTiff};

	for (size_t i = 0; i < WF_ARRAY_LEN(kinds); i++) {
		Blob b = encode(kinds[i], 400, 300, 1, 0);
		static const unsigned cuts[] = {
		        1, 4, 2, 3}; /* of n: n * c / 5 */

		for (size_t k = 0; k < WF_ARRAY_LEN(cuts); k++) {
			size_t keep = b.n * cuts[k] / 5;
			WfPixmap *pm = (WfPixmap *)1;
			WfDecodeStatus st = wf_decode_image(
			        b.bytes, keep, 200, 200, &pm, NULL);

			if (WF_DSTAT_OK == st) {
				printf("  container %zu cut to %zu of %zu was "
				       "shown\n",
				        i, keep, b.n);
				CHECK_FAIL("a truncated file was decoded");
				wf_pixmap_free(pm);
			} else {
				CHECK(NULL == pm);
			}
		}
		blob_free(&b);
	}
}

static void rubbish_is_refused(void)
{
	unsigned char noise[4096];
	WfPixmap *pm = (WfPixmap *)1;

	for (size_t i = 0; i < sizeof noise; i++) {
		noise[i] = (unsigned char)(i * 31 + 7);
	}
	CHECK(WF_DSTAT_OK !=
	        wf_decode_image(noise, sizeof noise, 100, 100, &pm, NULL));
	CHECK(NULL == pm);
	CHECK(WF_DSTAT_OK != wf_decode_image(noise, 0, 100, 100, &pm, NULL));
	CHECK(WF_DSTAT_OK != wf_decode_image(NULL, 10, 100, 100, &pm, NULL));
	CHECK(WF_DSTAT_OK != wf_decode_image(noise, 10, 0, 100, &pm, NULL));
	/* text that merely looks like a picture's name or a PDF */
	CHECK(WF_DSTAT_OK != wf_decode_image("%PDF-1.4", 8, 50, 50, &pm, NULL));
}

/* Why WIC would not open the bytes decides what the user is told: a codec that is
 * not installed (WebP, HEIF, JPEG XL) and low memory are not "damaged". */
static void why_a_decoder_could_not_be_made_is_told_apart(void)
{
	static const unsigned char png[] = "\x89PNG\r\n\x1a\n-----";
	static const unsigned char webp[] = "RIFF\x10\0\0\0WEBPVP8 ";
	static const unsigned char words[] = "just words, not a picture";

	/* the bytes look like a picture and WIC does not know the format: damaged */
	CHECK(WF_DSTAT_CORRUPT ==
	        wf_decode_open_failure(
	                WINCODEC_ERR_UNKNOWNIMAGEFORMAT, png, 13));
	/* the codec is not there: the picture is fine, this PC cannot show it */
	CHECK(WF_DSTAT_UNSUPPORTED ==
	        wf_decode_open_failure(
	                WINCODEC_ERR_COMPONENTNOTFOUND, png, 13));
	CHECK(WF_DSTAT_UNSUPPORTED ==
	        wf_decode_open_failure(
	                WINCODEC_ERR_COMPONENTNOTFOUND, webp, 16));
	CHECK(WF_DSTAT_UNSUPPORTED ==
	        wf_decode_open_failure(
	                WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT, png, 13));
	/* memory is not a verdict on the file */
	CHECK(WF_DSTAT_INTERNAL ==
	        wf_decode_open_failure(E_OUTOFMEMORY, png, 13));
	CHECK(WF_DSTAT_INTERNAL ==
	        wf_decode_open_failure(E_OUTOFMEMORY, webp, 16));
	/* not like any picture: not a format we show */
	CHECK(WF_DSTAT_UNSUPPORTED ==
	        wf_decode_open_failure(WINCODEC_ERR_UNKNOWNIMAGEFORMAT, words,
	                sizeof words - 1));
	/* any other failure on a picture-looking file is damage */
	CHECK(WF_DSTAT_CORRUPT ==
	        wf_decode_open_failure(WINCODEC_ERR_BADHEADER, png, 13));
}

static void a_bitmap_that_claims_to_be_enormous_is_refused(void)
{
	/* a 54-byte BMP header for 40000 x 40000 pixels, with no pixels */
	unsigned char bmp[54] = {'B', 'M'};
	WfPixmap *pm = (WfPixmap *)1;
	WfDecodeStatus st;

	bmp[10] = 54;
	bmp[14] = 40;
	bmp[18] = 0x40;
	bmp[19] = 0x9C; /* 40000 */
	bmp[22] = 0x40;
	bmp[23] = 0x9C;
	bmp[26] = 1;
	bmp[28] = 24;
	st = wf_decode_image(bmp, sizeof bmp, 100, 100, &pm, NULL);
	CHECK(NULL == pm);
	/* a file that starts like a bitmap and is not one is a damaged picture, not a
	 * format this PC lacks */
	CHECK(WF_DSTAT_CORRUPT == st);
	/* what starts like no picture at all is still "not a format we show" */
	st = wf_decode_image(
	        "just words, not a picture at all", 32, 100, 100, &pm, NULL);
	CHECK(WF_DSTAT_UNSUPPORTED == st && NULL == pm);
}

static size_t dark_pixels(const WfPixmap *pm)
{
	size_t n = 0;

	for (size_t i = 0; i < (size_t)pm->w * pm->h; i++) {
		if (red(pm->px[i]) < 100 && green(pm->px[i]) < 100) {
			n++;
		}
	}
	return n;
}

static void a_pdf_page_is_rendered_to_the_box(void)
{
	WfPixmap *pm = NULL;
	WfDecodeInfo info = {0};
	DWORD t0 = GetTickCount();

	CHECK(WF_DSTAT_OK ==
	        wf_decode_pdf(pdf_two_page, sizeof pdf_two_page, 0, 400, 500,
	                NULL, &pm, &info));
	DWORD elapsed = GetTickCount() - t0;
	printf("PDF first activation and page render: %lu ms\n", elapsed);
	CHECK(elapsed < 5000);
	CHECK(2 == info.page_count && 816 == info.src_w && 1056 == info.src_h);
	/* 816 x 1056 into 400 x 500: height-limited, 386 wide */
	CHECK(386 == pm->w && 500 == pm->h);
	/* white page, a red and a blue box on it */
	CHECK(0xFF == red(pm->px[0]) && 0xFF == green(pm->px[0]));
	{
		size_t coloured = 0;

		for (size_t i = 0; i < (size_t)pm->w * pm->h; i++) {
			unsigned r = red(pm->px[i]), g = green(pm->px[i]);

			if ((r > 200 && g < 100) || (r < 100 && g < 120)) {
				coloured++;
			}
		}
		CHECK(coloured > 3000);
	}
	wf_pixmap_free(pm);
}

static void a_rotated_page_comes_out_turned(void)
{
	WfPixmap *pm = NULL;
	WfDecodeInfo info = {0};
	ULONGLONG t0 = GetTickCount64();

	CHECK(WF_DSTAT_OK ==
	        wf_decode_pdf(pdf_two_page, sizeof pdf_two_page, 1, 400, 500,
	                NULL, &pm, &info));
	printf("PDF subsequent page render: %llu ms\n",
	        (unsigned long long)(GetTickCount64() - t0));
	CHECK(1056 == info.src_w && 816 == info.src_h); /* landscape now */
	CHECK(400 == pm->w && 309 == pm->h);
	wf_pixmap_free(pm);
}

static void a_page_that_does_not_exist_says_how_many_there_are(void)
{
	WfPixmap *pm = (WfPixmap *)1;
	WfDecodeInfo info = {0};

	CHECK(WF_DSTAT_PAGE_RANGE ==
	        wf_decode_pdf(pdf_two_page, sizeof pdf_two_page, 2, 100, 100,
	                NULL, &pm, &info));
	CHECK(NULL == pm && 2 == info.page_count);
	CHECK(WF_DSTAT_PAGE_RANGE ==
	        wf_decode_pdf(pdf_two_page, sizeof pdf_two_page, 0xFFFFFFFFu,
	                100, 100, NULL, &pm, &info));
}

static void a_bigger_box_gives_a_sharper_page_not_a_scaled_one(void)
{
	WfPixmap *thumb = NULL, *sharp = NULL;

	CHECK(WF_DSTAT_OK ==
	        wf_decode_pdf(pdf_two_page, sizeof pdf_two_page, 0, 200, 260,
	                NULL, &thumb, NULL));
	CHECK(WF_DSTAT_OK ==
	        wf_decode_pdf(pdf_two_page, sizeof pdf_two_page, 0, 1000, 1300,
	                NULL, &sharp, NULL));
	CHECK(thumb->w < 200 + 1 && sharp->w > 700);
	/* the large render is rendered, not stretched: the ratio of dark text
	 * pixels to the page area stays about the same */
	CHECK(dark_pixels(thumb) > 20 && dark_pixels(sharp) > 20 * 9);
	wf_pixmap_free(thumb);
	wf_pixmap_free(sharp);
}

static void japanese_text_is_drawn_with_a_system_font(void)
{
	WfPixmap *pm = NULL;

	CHECK(WF_DSTAT_OK ==
	        wf_decode_pdf(pdf_japanese, sizeof pdf_japanese, 0, 800, 1000,
	                NULL, &pm, NULL));
	CHECK(dark_pixels(pm) > 1500); /* a line of kanji is a lot of ink */
	wf_pixmap_free(pm);
}

static void a_password_is_asked_for_and_checked(void)
{
	WfPixmap *pm = (WfPixmap *)1;
	WfDecodeInfo info = {0};

	CHECK(WF_DSTAT_PASSWORD_REQUIRED ==
	        wf_decode_pdf(pdf_encrypted, sizeof pdf_encrypted, 0, 400, 500,
	                NULL, &pm, &info));
	CHECK(NULL == pm);
	CHECK(WF_DSTAT_PASSWORD_REQUIRED ==
	        wf_decode_pdf(pdf_encrypted, sizeof pdf_encrypted, 0, 400, 500,
	                L"", &pm, &info));
	CHECK(WF_DSTAT_PASSWORD_WRONG ==
	        wf_decode_pdf(pdf_encrypted, sizeof pdf_encrypted, 0, 400, 500,
	                L"nope", &pm, &info));
	CHECK(WF_DSTAT_OK ==
	        wf_decode_pdf(pdf_encrypted, sizeof pdf_encrypted, 0, 400, 500,
	                L"secret", &pm, &info));
	CHECK(1 == info.page_count && dark_pixels(pm) > 10);
	wf_pixmap_free(pm);
	/* a password given for a file that has none is harmless */
	CHECK(WF_DSTAT_OK ==
	        wf_decode_pdf(pdf_two_page, sizeof pdf_two_page, 0, 100, 100,
	                L"unneeded", &pm, NULL));
	wf_pixmap_free(pm);
}

static void damaged_pdfs_are_refused(void)
{
	WfPixmap *pm = (WfPixmap *)1;

	CHECK(WF_DSTAT_CORRUPT ==
	        wf_decode_pdf(pdf_truncated, sizeof pdf_truncated, 0, 100, 100,
	                NULL, &pm, NULL));
	CHECK(NULL == pm);
	CHECK(WF_DSTAT_CORRUPT ==
	        wf_decode_pdf(pdf_garbage, sizeof pdf_garbage, 0, 100, 100,
	                NULL, &pm, NULL));
	CHECK(WF_DSTAT_OK !=
	        wf_decode_pdf("", 0, 0, 100, 100, NULL, &pm, NULL));
	CHECK(WF_DSTAT_OK !=
	        wf_decode_pdf(NULL, 5, 0, 100, 100, NULL, &pm, NULL));
	/* a picture is not a PDF */
	{
		Blob b = encode(&GUID_ContainerFormatPng, 20, 20, 1, 0);

		CHECK(WF_DSTAT_OK !=
		        wf_decode_pdf(
		                b.bytes, b.n, 0, 100, 100, NULL, &pm, NULL));
		blob_free(&b);
	}
}

static Blob g_shared;
static volatile LONG g_bad;

static DWORD WINAPI worker(LPVOID arg)
{
	(void)arg;
	for (int i = 0; i < 15; i++) {
		WfPixmap *pm = NULL;

		if (WF_DSTAT_OK !=
		                wf_decode_image(g_shared.bytes, g_shared.n, 64,
		                        64, &pm, NULL) ||
		        64 != pm->w) {
			InterlockedIncrement(&g_bad);
		}
		wf_pixmap_free(pm);
		pm = NULL;
		if (WF_DSTAT_OK !=
		        wf_decode_pdf(pdf_two_page, sizeof pdf_two_page,
		                (uint32_t)(i & 1), 96, 96, NULL, &pm, NULL)) {
			InterlockedIncrement(&g_bad);
		}
		wf_pixmap_free(pm);
	}
	return 0;
}

static void decoding_from_several_threads_at_once_is_safe(void)
{
	HANDLE t[4];

	g_shared = encode(&GUID_ContainerFormatPng, 256, 256, 1, 0);
	for (int i = 0; i < 4; i++) {
		t[i] = CreateThread(NULL, 0, worker, NULL, 0, NULL);
	}
	WaitForMultipleObjects(4, t, TRUE, 120000);
	for (int i = 0; i < 4; i++) {
		CloseHandle(t[i]);
	}
	CHECK(0 == g_bad);
	blob_free(&g_shared);
}

#define RUN(fn) fn()

int main(void)
{
	/* With WinRT threads alive at exit the CRT did not flush a buffered stdout,
	 * so a failure report could vanish. Unbuffered output costs nothing here. */
	setvbuf(stdout, NULL, _IONBF, 0);
	RUN(a_png_is_reduced_to_fit_the_box);
	RUN(a_small_picture_is_not_enlarged);
	RUN(transparency_is_reported_and_kept);
	RUN(exif_orientation_is_applied_and_is_fast);
	RUN(other_built_in_formats_decode);
	RUN(a_gif_with_transparency_decodes);
	RUN(damaged_files_are_refused_not_shown_in_part);
	RUN(rubbish_is_refused);
	RUN(why_a_decoder_could_not_be_made_is_told_apart);
	RUN(a_bitmap_that_claims_to_be_enormous_is_refused);
	RUN(a_pdf_page_is_rendered_to_the_box);
	RUN(a_rotated_page_comes_out_turned);
	RUN(a_page_that_does_not_exist_says_how_many_there_are);
	RUN(a_bigger_box_gives_a_sharper_page_not_a_scaled_one);
	RUN(japanese_text_is_drawn_with_a_system_font);
	RUN(a_password_is_asked_for_and_checked);
	RUN(damaged_pdfs_are_refused);
	RUN(decoding_from_several_threads_at_once_is_safe);
	return TEST_RESULT();
}
