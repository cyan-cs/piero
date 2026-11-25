#define COBJMACROS
#include <windows.h>

#include <stdlib.h>
#include <string.h>
#include <wincodec.h>

#include "wf_decode_win.h"
#include "wf_image_check.h"

/*
 * Pictures through WIC, reduced while decoding. Order of work, and why:
 *
 *   1. structural checks on PNG and ICO before WIC sees the bytes (the PNG
 *      decoder accepts a file cut to 100 bytes; P0-A03);
 *   2. only the containers in the allow list, decided by the decoder that WIC
 *      picked from the CONTENT, never by the file name;
 *   3. refuse a source with more pixels than a sane picture has;
 *   4. scale to the box FIRST, rotate after: rotating the full-size source
 *      through WIC took 22 s for a 2400x1600 JPEG, rotating the reduced
 *      bitmap takes microseconds (wf_pixmap_orient).
 */

/* Containers this host decodes. HEIF/AVIF, WebP and JPEG XL exist only where
 * the OS or a Store extension provides them; asking WIC is how we find out. */
static int container_allowed(const GUID *c)
{
	return IsEqualGUID(c, &GUID_ContainerFormatPng) ||
	        IsEqualGUID(c, &GUID_ContainerFormatJpeg) ||
	        IsEqualGUID(c, &GUID_ContainerFormatBmp) ||
	        IsEqualGUID(c, &GUID_ContainerFormatGif) ||
	        IsEqualGUID(c, &GUID_ContainerFormatTiff) ||
	        IsEqualGUID(c, &GUID_ContainerFormatIco) ||
	        IsEqualGUID(c, &GUID_ContainerFormatWmp) ||
	        IsEqualGUID(c, &GUID_ContainerFormatWebp) ||
	        IsEqualGUID(c, &GUID_ContainerFormatHeif) ||
	        IsEqualGUID(c, &GUID_ContainerFormatJpegXL);
}

static int orientation_of(IWICBitmapFrameDecode *frame)
{
	IWICMetadataQueryReader *q = NULL;
	PROPVARIANT v;
	int o = 1;

	PropVariantInit(&v);
	if (FAILED(IWICBitmapFrameDecode_GetMetadataQueryReader(frame, &q))) {
		return 1;
	}
	if (SUCCEEDED(IWICMetadataQueryReader_GetMetadataByName(
	            q, L"/app1/ifd/{ushort=274}", &v)) ||
	        SUCCEEDED(IWICMetadataQueryReader_GetMetadataByName(
	                q, L"/ifd/{ushort=274}", &v))) {
		if (VT_UI2 == v.vt && v.uiVal >= 1 && v.uiVal <= 8) {
			o = v.uiVal;
		}
	}
	PropVariantClear(&v);
	IWICMetadataQueryReader_Release(q);
	return o;
}

static int format_has_alpha(IWICImagingFactory *wic, const GUID *format)
{
	IWICComponentInfo *ci = NULL;
	IWICPixelFormatInfo2 *pi = NULL;
	BOOL alpha = FALSE;

	if (SUCCEEDED(
	            IWICImagingFactory_CreateComponentInfo(wic, format, &ci))) {
		if (SUCCEEDED(IWICComponentInfo_QueryInterface(
		            ci, &IID_IWICPixelFormatInfo2, (void **)&pi))) {
			(void)IWICPixelFormatInfo2_SupportsTransparency(
			        pi, &alpha);
			IWICPixelFormatInfo2_Release(pi);
		}
		IWICComponentInfo_Release(ci);
	}
	return alpha ? 1 : 0;
}

/* Whether the first bytes are the signature of a picture format we decode. A file
 * that starts like one and that WIC will not open is a damaged one: saying "this
 * format is not available on this PC" of it would be wrong. */
static int has_picture_signature(const unsigned char *d, size_t n)
{
	return (n >= 8 && 0 == memcmp(d, "\x89PNG\r\n\x1a\n", 8)) ||
	        (n >= 3 && 0xFF == d[0] && 0xD8 == d[1] && 0xFF == d[2]) ||
	        (n >= 6 && 0 == memcmp(d, "GIF8", 4)) ||
	        (n >= 14 && 'B' == d[0] && 'M' == d[1]) ||
	        (n >= 8 &&
	                (0 == memcmp(d, "II*\0", 4) ||
	                        0 == memcmp(d, "MM\0*", 4))) ||
	        (n >= 12 && 0 == memcmp(d, "RIFF", 4) &&
	                0 == memcmp(d + 8, "WEBP", 4));
}

static WfDecodeStatus status_of_wic(HRESULT hr)
{
	if (WINCODEC_ERR_COMPONENTNOTFOUND == hr ||
	        WINCODEC_ERR_UNKNOWNIMAGEFORMAT == hr ||
	        WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT == hr) {
		return WF_DSTAT_UNSUPPORTED;
	}
	if (E_OUTOFMEMORY == hr) {
		return WF_DSTAT_INTERNAL;
	}
	return WF_DSTAT_CORRUPT;
}

/* Why WIC would not make a decoder for these bytes. A missing codec (WebP, HEIF
 * and JPEG XL depend on what is installed) says nothing about the file, and low
 * memory says nothing about it either: only a format WIC does not recognise
 * AFTER the bytes start like a picture is a damaged picture. */
WfDecodeStatus wf_decode_open_failure(HRESULT hr, const void *data, size_t n)
{
	if (WINCODEC_ERR_UNKNOWNIMAGEFORMAT == hr &&
	        has_picture_signature(data, n)) {
		return WF_DSTAT_CORRUPT;
	}
	return status_of_wic(hr);
}

/* `checked` is 0 for bytes the PDF engine produced itself. */
static WfDecodeStatus decode_wic(const unsigned char *data,
        size_t n,
        uint32_t box_w,
        uint32_t box_h,
        int checked,
        WfPixmap **out,
        WfDecodeInfo *info)
{
	WfDecodeStatus result = WF_DSTAT_INTERNAL;
	HRESULT hr, co;
	IWICImagingFactory *wic = NULL;
	IWICStream *stream = NULL;
	IWICBitmapDecoder *dec = NULL;
	IWICBitmapFrameDecode *frame = NULL;
	IWICBitmapScaler *scaler = NULL;
	IWICFormatConverter *conv = NULL;
	IWICBitmapSource *src;
	WfPixmap *scaled = NULL, *oriented = NULL;
	GUID container = {0}, format = {0};
	UINT w = 0, h = 0;
	uint32_t tw, th, bw, bh;
	int orient;
	const char *why;

	*out = NULL;
	if (!data || 0 == n || n > WF_DWIRE_INPUT_MAX || 0 == box_w ||
	        0 == box_h) {
		return WF_DSTAT_UNSUPPORTED;
	}
	if (checked) {
		if (n >= 8 && 0 == memcmp(data, "\x89PNG\r\n\x1a\n", 8) &&
		        !wf_png_structure_ok(data, n, &why)) {
			return WF_DSTAT_CORRUPT;
		}
		if (n >= 4 && 0 == data[0] && 0 == data[1] && 0 == data[3] &&
		        (1 == data[2] || 2 == data[2]) &&
		        !wf_ico_structure_ok(data, n, &why)) {
			return WF_DSTAT_CORRUPT;
		}
		/* WIC shows a JPEG or GIF that was cut off as the part that
		 * arrived, with no error: measured, not assumed */
		if (n >= 3 && 0xFF == data[0] && 0xD8 == data[1] &&
		        0xFF == data[2] &&
		        !wf_jpeg_structure_ok(data, n, &why)) {
			return WF_DSTAT_CORRUPT;
		}
		if (n >= 6 && 0 == memcmp(data, "GIF8", 4) &&
		        !wf_gif_structure_ok(data, n, &why)) {
			return WF_DSTAT_CORRUPT;
		}
		if (n >= 12 && 0 == memcmp(data, "RIFF", 4) &&
		        0 == memcmp(data + 8, "WEBP", 4) &&
		        !wf_webp_structure_ok(data, n, &why)) {
			return WF_DSTAT_CORRUPT;
		}
	}
	co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	if (FAILED(co) && RPC_E_CHANGED_MODE != co) {
		return WF_DSTAT_INTERNAL;
	}
	hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL,
	        CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&wic);
	if (FAILED(hr) ||
	        FAILED(IWICImagingFactory_CreateStream(wic, &stream)) ||
	        FAILED(IWICStream_InitializeFromMemory(
	                stream, (BYTE *)(uintptr_t)data, (DWORD)n))) {
		goto done;
	}
	hr = IWICImagingFactory_CreateDecoderFromStream(wic, (IStream *)stream,
	        NULL, WICDecodeMetadataCacheOnDemand, &dec);
	if (FAILED(hr)) {
		result = wf_decode_open_failure(hr, data, n);
		goto done;
	}
	(void)IWICBitmapDecoder_GetContainerFormat(dec, &container);
	if (!container_allowed(&container)) {
		result = WF_DSTAT_UNSUPPORTED;
		goto done;
	}
	if (FAILED(IWICBitmapDecoder_GetFrame(dec, 0, &frame)) ||
	        FAILED(IWICBitmapFrameDecode_GetSize(frame, &w, &h))) {
		result = WF_DSTAT_CORRUPT;
		goto done;
	}
	if (0 == w || 0 == h) {
		result = WF_DSTAT_CORRUPT;
		goto done;
	}
	if ((unsigned long long)w * h > WF_DECODE_SOURCE_PIXELS) {
		result = WF_DSTAT_TOO_LARGE;
		goto done;
	}
	orient = checked ? orientation_of(frame) : 1;
	(void)IWICBitmapFrameDecode_GetPixelFormat(frame, &format);
	info->page_count = 1;
	info->src_w = orient >= 5 ? h : w;
	info->src_h = orient >= 5 ? w : h;
	info->flags = format_has_alpha(wic, &format) ? WF_DFLAG_ALPHA : 0;

	/* the box, as seen before the turn; 5..8 will swap it back */
	bw = orient >= 5 ? box_h : box_w;
	bh = orient >= 5 ? box_w : box_h;
	wf_fit_box(w, h, bw, bh, 0, &tw, &th);
	src = (IWICBitmapSource *)frame;
	if (tw != w || th != h) {
		if (FAILED(IWICImagingFactory_CreateBitmapScaler(
		            wic, &scaler)) ||
		        FAILED(IWICBitmapScaler_Initialize(scaler, src, tw, th,
		                WICBitmapInterpolationModeFant))) {
			goto done;
		}
		src = (IWICBitmapSource *)scaler;
	}
	if (FAILED(IWICImagingFactory_CreateFormatConverter(wic, &conv)) ||
	        FAILED(IWICFormatConverter_Initialize(conv, src,
	                &GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
	                NULL, 0.0, WICBitmapPaletteTypeCustom))) {
		result = WF_DSTAT_CORRUPT;
		goto done;
	}
	if (WF_OK != wf_pixmap_new(tw, th, &scaled)) {
		result = WF_DSTAT_TOO_LARGE;
		goto done;
	}
	hr = IWICFormatConverter_CopyPixels(conv, NULL, tw * 4,
	        (UINT)wf_pixmap_bytes(tw, th), (BYTE *)scaled->px);
	if (FAILED(hr)) {
		/* a decoder that fails half-way (a damaged JPEG or GIF) is not
		 * shown as the part of the picture it managed */
		result = status_of_wic(hr);
		goto done;
	}
	if (orient > 1) {
		if (WF_OK != wf_pixmap_orient(scaled, orient, &oriented)) {
			goto done;
		}
		wf_pixmap_free(scaled);
		scaled = oriented;
		oriented = NULL;
	}
	*out = scaled;
	scaled = NULL;
	result = WF_DSTAT_OK;
done:
	wf_pixmap_free(scaled);
	if (conv) {
		IWICFormatConverter_Release(conv);
	}
	if (scaler) {
		IWICBitmapScaler_Release(scaler);
	}
	if (frame) {
		IWICBitmapFrameDecode_Release(frame);
	}
	if (dec) {
		IWICBitmapDecoder_Release(dec);
	}
	if (stream) {
		IWICStream_Release(stream);
	}
	if (wic) {
		IWICImagingFactory_Release(wic);
	}
	if (SUCCEEDED(co)) {
		CoUninitialize();
	}
	return result;
}

WfDecodeStatus wf_decode_image(const void *data,
        size_t n,
        uint32_t box_w,
        uint32_t box_h,
        WfPixmap **out,
        WfDecodeInfo *info)
{
	WfDecodeInfo scratch;

	if (!out) {
		return WF_DSTAT_INTERNAL;
	}
	if (!info) {
		info = &scratch;
	}
	memset(info, 0, sizeof *info);
	return decode_wic(data, n, box_w, box_h, 1, out, info);
}

WfDecodeStatus wf_decode_pdf(const void *data,
        size_t n,
        uint32_t page,
        uint32_t box_w,
        uint32_t box_h,
        const wchar_t *password,
        WfPixmap **out,
        WfDecodeInfo *info)
{
	WfDecodeInfo scratch, shown;
	void *png = NULL;
	size_t png_n = 0;
	WfDecodeStatus st;

	if (!out) {
		return WF_DSTAT_INTERNAL;
	}
	*out = NULL;
	if (!info) {
		info = &scratch;
	}
	memset(info, 0, sizeof *info);
	st = wf_pdf_render_png(
	        data, n, page, box_w, box_h, password, &png, &png_n, info);
	if (WF_DSTAT_OK != st) {
		return st;
	}
	/* the engine's own PNG: decoded at the size it was rendered, no turn */
	memset(&shown, 0, sizeof shown);
	st = decode_wic(png, png_n, box_w, box_h, 0, out, &shown);
	free(png);
	return st;
}
