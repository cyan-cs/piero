/*
 * Prototype, not product code: what can Windows Imaging Component do for a
 * preview, from plain C17?
 *
 *   wic_proto list                      codecs installed on this machine
 *   wic_proto file [max_w max_h out.png] decode, orient, scale to fit, report
 *
 * Reports the container format, frame count, size, the Exif orientation read
 * through the metadata query reader, whether the source has alpha, and the
 * time to decode and scale to the requested box (premultiplied BGRA, the
 * layout the terminal renderers want).
 */
#define COBJMACROS
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <wincodec.h>

static double now_ms(void)
{
	LARGE_INTEGER f, c;

	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c);
	return 1000.0 * (double)c.QuadPart / (double)f.QuadPart;
}

static void list_codecs(IWICImagingFactory *wic)
{
	IEnumUnknown *en = NULL;
	IUnknown *u;
	ULONG got;

	if (FAILED(IWICImagingFactory_CreateComponentEnumerator(
	            wic, WICDecoder, WICComponentEnumerateDefault, &en))) {
		return;
	}
	while (S_OK == IEnumUnknown_Next(en, 1, &u, &got) && got) {
		IWICBitmapCodecInfo *info = NULL;

		if (SUCCEEDED(IUnknown_QueryInterface(
		            u, &IID_IWICBitmapCodecInfo, (void **)&info))) {
			wchar_t name[96] = L"", ext[128] = L"";
			UINT n;

			IWICBitmapCodecInfo_GetFriendlyName(info, 96, name, &n);
			IWICBitmapCodecInfo_GetFileExtensions(
			        info, 128, ext, &n);
			wprintf(L"  %-34ls %ls\n", name, ext);
			IWICBitmapCodecInfo_Release(info);
		}
		IUnknown_Release(u);
	}
	IEnumUnknown_Release(en);
}

static int orientation_of(IWICBitmapFrameDecode *frame)
{
	IWICMetadataQueryReader *q = NULL;
	PROPVARIANT v;
	int o = 1;

	PropVariantInit(&v);
	if (SUCCEEDED(
	            IWICBitmapFrameDecode_GetMetadataQueryReader(frame, &q))) {
		/* JPEG: /app1/ifd; TIFF/HEIF-style: /ifd */
		if (SUCCEEDED(IWICMetadataQueryReader_GetMetadataByName(
		            q, L"/app1/ifd/{ushort=274}", &v)) ||
		        SUCCEEDED(IWICMetadataQueryReader_GetMetadataByName(
		                q, L"/ifd/{ushort=274}", &v))) {
			if (VT_UI2 == v.vt) {
				o = v.uiVal;
			}
		}
		PropVariantClear(&v);
		IWICMetadataQueryReader_Release(q);
	}
	return o;
}

/* A gradient with a transparent disc: opaque corners, alpha 0 in the middle. */
static BYTE *make_pixels(UINT w, UINT h)
{
	BYTE *p = malloc((size_t)w * h * 4);
	UINT x, y;

	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			BYTE *q = p + ((size_t)y * w + x) * 4;
			double dx = (double)x - w / 2.0,
			       dy = (double)y - h / 2.0;
			int hole = dx * dx + dy * dy < (double)h * h / 16.0;

			q[0] = (BYTE)(255 * x / w);
			q[1] = (BYTE)(255 * y / h);
			q[2] = (BYTE)(255 - 255 * x / w);
			q[3] = hole ? 0 : 255;
		}
	}
	return p;
}

static int generate(IWICImagingFactory *wic,
        const wchar_t *path,
        const GUID *container,
        UINT w,
        UINT h,
        int orientation)
{
	IWICStream *st = NULL;
	IWICBitmapEncoder *enc = NULL;
	IWICBitmapFrameEncode *fe = NULL;
	IPropertyBag2 *bag = NULL;
	WICPixelFormatGUID f = GUID_WICPixelFormat32bppBGRA;
	BYTE *pix = make_pixels(w, h);
	HRESULT hr;

	IWICImagingFactory_CreateStream(wic, &st);
	IWICStream_InitializeFromFilename(st, path, GENERIC_WRITE);
	hr = IWICImagingFactory_CreateEncoder(wic, container, NULL, &enc);
	if (FAILED(hr)) {
		return 0;
	}
	IWICBitmapEncoder_Initialize(
	        enc, (IStream *)st, WICBitmapEncoderNoCache);
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
	hr = IWICBitmapFrameEncode_WritePixels(fe, h, w * 4, w * 4 * h, pix);
	hr = SUCCEEDED(hr) ? IWICBitmapFrameEncode_Commit(fe) : hr;
	hr = SUCCEEDED(hr) ? IWICBitmapEncoder_Commit(enc) : hr;
	free(pix);
	return SUCCEEDED(hr);
}

int wmain(int argc, wchar_t **argv)
{
	IWICImagingFactory *wic = NULL;
	HRESULT hr;

	hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL,
	        CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&wic);
	if (FAILED(hr)) {
		printf("factory 0x%08lx\n", (unsigned long)hr);
		return 1;
	}
	if (argc < 2) {
		return 2;
	}
	if (!wcscmp(argv[1], L"list")) {
		list_codecs(wic);
		return 0;
	}
	if (!wcscmp(argv[1], L"gen") && argc > 2) {
		static const struct
		{
			const wchar_t *name;
			const GUID *id;
			int orientation;
		} kinds[] = {{L"grad.png", &GUID_ContainerFormatPng, 1},
		        {L"grad.jpg", &GUID_ContainerFormatJpeg, 1},
		        {L"rot6.jpg", &GUID_ContainerFormatJpeg, 6},
		        {L"grad.bmp", &GUID_ContainerFormatBmp, 1},
		        {L"grad.gif", &GUID_ContainerFormatGif, 1},
		        {L"grad.tif", &GUID_ContainerFormatTiff, 1},
		        {L"grad.wdp", &GUID_ContainerFormatWmp, 1}};
		wchar_t path[MAX_PATH];
		size_t i;

		for (i = 0; i < sizeof kinds / sizeof *kinds; i++) {
			swprintf(path, MAX_PATH, L"%ls\\%ls", argv[2],
			        kinds[i].name);
			wprintf(L"gen %-10ls %ls\n", kinds[i].name,
			        generate(wic, path, kinds[i].id, 2400, 1600,
			                kinds[i].orientation)
			                ? L"ok"
			                : L"FAILED");
		}
		return 0;
	}
	{
		UINT maxw = argc > 2 ? (UINT)_wtoi(argv[2]) : 160;
		UINT maxh = argc > 3 ? (UINT)_wtoi(argv[3]) : 90;
		IWICBitmapDecoder *dec = NULL;
		IWICBitmapFrameDecode *frame = NULL;
		IWICBitmapSource *src = NULL;
		IWICFormatConverter *conv = NULL;
		IWICBitmapScaler *scaler = NULL;
		IWICBitmapFlipRotator *rot = NULL;
		GUID container = {0}, pf = {0};
		UINT frames = 0, w = 0, h = 0, o;
		double t0 = now_ms(), t1;

		hr = IWICImagingFactory_CreateDecoderFromFilename(wic, argv[1],
		        NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
		        &dec);
		printf("decoder 0x%08lx\n", (unsigned long)hr);
		if (FAILED(hr)) {
			return 1;
		}
		IWICBitmapDecoder_GetContainerFormat(dec, &container);
		IWICBitmapDecoder_GetFrameCount(dec, &frames);
		IWICBitmapDecoder_GetFrame(dec, 0, &frame);
		IWICBitmapFrameDecode_GetSize(frame, &w, &h);
		IWICBitmapFrameDecode_GetPixelFormat(frame, &pf);
		o = (UINT)orientation_of(frame);
		printf("container=%08lx frames=%u size=%ux%u orientation=%u\n",
		        container.Data1, frames, w, h, o);

		/*
		 * Scale first, rotate the small result. A flip/rotator over the full
		 * size JPEG took ~22 s for 2400x1600; rotating the already reduced
		 * bitmap is instant. For orientations 5..8 the box is swapped, as the
		 * rotation will swap it back.
		 */
		src = (IWICBitmapSource *)frame;
		{
			UINT bw = o >= 5 ? maxh : maxw,
			     bh = o >= 5 ? maxw : maxh;
			double s = (double)bw / w;

			if ((double)bh / h < s) {
				s = (double)bh / h;
			}
			if (s < 1.0) {
				UINT nw = (UINT)(w * s + 0.5),
				     nh = (UINT)(h * s + 0.5);

				IWICImagingFactory_CreateBitmapScaler(
				        wic, &scaler);
				IWICBitmapScaler_Initialize(scaler, src,
				        nw ? nw : 1, nh ? nh : 1,
				        WICBitmapInterpolationModeFant);
				src = (IWICBitmapSource *)scaler;
			}
		}
		IWICImagingFactory_CreateFormatConverter(wic, &conv);
		hr = IWICFormatConverter_Initialize(conv, src,
		        &GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
		        NULL, 0.0, WICBitmapPaletteTypeCustom);
		src = (IWICBitmapSource *)conv;
		if (o > 1 && o <= 8) {
			WICBitmapTransformOptions t[9] = {0, 0,
			        WICBitmapTransformFlipHorizontal,
			        WICBitmapTransformRotate180,
			        WICBitmapTransformFlipVertical,
			        (WICBitmapTransformOptions)(WICBitmapTransformRotate270 |
			                WICBitmapTransformFlipHorizontal),
			        WICBitmapTransformRotate90,
			        (WICBitmapTransformOptions)(WICBitmapTransformRotate90 |
			                WICBitmapTransformFlipHorizontal),
			        WICBitmapTransformRotate270};
			IWICBitmap *reduced = NULL;

			IWICImagingFactory_CreateBitmapFromSource(
			        wic, src, WICBitmapCacheOnLoad, &reduced);
			IWICImagingFactory_CreateBitmapFlipRotator(wic, &rot);
			IWICBitmapFlipRotator_Initialize(
			        rot, (IWICBitmapSource *)reduced, t[o]);
			src = (IWICBitmapSource *)rot;
		}
		conv = (IWICFormatConverter *)
		        src; /* the size/copy below read src */
		{
			UINT cw, ch;
			BYTE *pix;
			size_t stride;

			IWICBitmapSource_GetSize(src, &cw, &ch);
			stride = (size_t)cw * 4;
			pix = malloc(stride * ch);
			hr = IWICBitmapSource_CopyPixels(src, NULL,
			        (UINT)stride, (UINT)(stride * ch), pix);
			t1 = now_ms();
			printf("out=%ux%u pbgra copy hr=0x%08lx total %.1f "
			       "ms\n",
			        cw, ch, (unsigned long)hr, t1 - t0);
			if (argc > 4) {
				IWICStream *st = NULL;
				IWICBitmapEncoder *enc = NULL;
				IWICBitmapFrameEncode *fe = NULL;
				WICPixelFormatGUID f =
				        GUID_WICPixelFormat32bppPBGRA;

				IWICImagingFactory_CreateStream(wic, &st);
				IWICStream_InitializeFromFilename(
				        st, argv[4], GENERIC_WRITE);
				IWICImagingFactory_CreateEncoder(wic,
				        &GUID_ContainerFormatPng, NULL, &enc);
				IWICBitmapEncoder_Initialize(enc, (IStream *)st,
				        WICBitmapEncoderNoCache);
				IWICBitmapEncoder_CreateNewFrame(
				        enc, &fe, NULL);
				IWICBitmapFrameEncode_Initialize(fe, NULL);
				IWICBitmapFrameEncode_SetSize(fe, cw, ch);
				IWICBitmapFrameEncode_SetPixelFormat(fe, &f);
				IWICBitmapFrameEncode_WritePixels(fe, ch,
				        (UINT)stride, (UINT)(stride * ch), pix);
				IWICBitmapFrameEncode_Commit(fe);
				IWICBitmapEncoder_Commit(enc);
			}
		}
	}
	return 0;
}
