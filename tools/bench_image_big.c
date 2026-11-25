#define COBJMACROS
#include <windows.h>
#include <psapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wincodec.h>

#include "../tests/wf_imgfix.h"
#include "preview/wf_dechost_client_win.h"

/*
 * What a big or hostile picture costs the sandboxed decoder host (DR-DRAFT-039):
 * status, time and the host's peak memory, for each case, through the same
 * client the product uses (12 s limit, 512 MiB job). Run by hand; prints a table.
 *
 *   bench_image_big [keep]    "keep" leaves the fixture files for another tool
 */

#define TIMEOUT_MS 12000u

static wchar_t g_dir[MAX_PATH];

static const char *status_name(WfDecodeStatus s)
{
	switch (s) {
	case WF_DSTAT_OK:          return "OK";
	case WF_DSTAT_CORRUPT:     return "CORRUPT";
	case WF_DSTAT_UNSUPPORTED: return "UNSUPPORTED";
	case WF_DSTAT_TOO_LARGE:   return "TOO_LARGE";
	case WF_DSTAT_TIMEOUT:     return "TIMEOUT";
	case WF_DSTAT_HOST_LOST:   return "HOST_LOST";
	case WF_DSTAT_INTERNAL:    return "INTERNAL";
	case WF_DSTAT_ABORTED:     return "ABORTED";
	default:                   return "other";
	}
}

static HANDLE write_fixture(const wchar_t *name, const void *p, size_t n)
{
	wchar_t path[MAX_PATH * 2];
	HANDLE h;
	DWORD w;
	const uint8_t *q = p;

	swprintf_s(path, MAX_PATH * 2, L"%ls\\%ls", g_dir, name);
	h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
	        NULL, CREATE_ALWAYS, 0, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return h;
	}
	while (n) {
		DWORD k = n > (1u << 28) ? (1u << 28) : (DWORD)n;

		if (!WriteFile(h, q, k, &w, NULL)) {
			break;
		}
		q += k;
		n -= k;
	}
	SetFilePointer(h, 0, NULL, FILE_BEGIN);
	return h;
}

static void *wic_encode(
        const GUID *container, UINT w, UINT h, int noise, size_t *n)
{
	IWICImagingFactory *wic = NULL;
	IWICBitmapEncoder *enc = NULL;
	IWICBitmapFrameEncode *fe = NULL;
	IPropertyBag2 *bag = NULL;
	IStream *stream = NULL;
	IWICBitmap *bmp = NULL;
	WICPixelFormatGUID f = GUID_WICPixelFormat24bppBGR;
	WICRect all = {0, 0, (INT)w, (INT)h};
	uint8_t *pix = malloc((size_t)w * h * 4);
	void *out = NULL;

	uint32_t rng = 2463534242u;

	for (size_t y = 0; y < h; y++) {
		for (size_t x = 0; x < w; x++) {
			uint8_t *q = pix + (y * w + x) * 4;

			rng ^= rng << 13;
			rng ^= rng >> 17;
			rng ^= rng << 5;
			q[0] = noise ? (uint8_t)rng : (uint8_t)(x * 255 / w);
			q[1] = noise ? (uint8_t)(rng >> 8)
			             : (uint8_t)(y * 255 / h);
			q[2] = noise ? (uint8_t)(rng >> 16)
			             : (uint8_t)((x ^ y) & 255);
			q[3] = 255;
		}
	}
	CoInitializeEx(NULL, COINIT_MULTITHREADED);
	CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
	        &IID_IWICImagingFactory, (void **)&wic);
	CreateStreamOnHGlobal(NULL, TRUE, &stream);
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
		goto done;
	}
	{
		HGLOBAL g = NULL;
		STATSTG st;

		GetHGlobalFromStream(stream, &g);
		IStream_Stat(stream, &st, STATFLAG_NONAME);
		*n = (size_t)st.cbSize.QuadPart;
		out = malloc(*n);
		memcpy(out, GlobalLock(g), *n);
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
	return out;
}

static WfDecHost *g_host;
static WfDecHostOptions g_opt;

static void run_case(
        const char *label, const wchar_t *file, const void *bytes, size_t n)
{
	HANDLE f = write_fixture(file, bytes, n);
	HANDLE ph = NULL;
	WfPixmap *pm = NULL;
	WfDecodeInfo info;
	WfDecodeStatus st;
	ULONGLONG t0;
	PROCESS_MEMORY_COUNTERS_EX mc;
	double peak_mib = -1;
	DWORD err = 0;

	if (INVALID_HANDLE_VALUE == f) {
		printf("%-34s cannot write fixture\n", label);
		return;
	}
	if (!g_host || !wf_dechost_alive(g_host)) {
		wf_dechost_stop(g_host);
		g_host = NULL;
		if (WF_OK != wf_dechost_start(&g_opt, &g_host, &err)) {
			printf("%-34s host did not start (%lu)\n", label,
			        (unsigned long)err);
			CloseHandle(f);
			return;
		}
	}
	ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
	        wf_dechost_pid(g_host));
	memset(&info, 0, sizeof info);
	t0 = GetTickCount64();
	st = wf_dechost_decode(g_host, WF_DKIND_IMAGE, f, 120, 60, 0, NULL,
	        TIMEOUT_MS, NULL, 0, &pm, &info);
	{
		ULONGLONG ms = GetTickCount64() - t0;

		memset(&mc, 0, sizeof mc);
		if (ph &&
		        GetProcessMemoryInfo(ph, (PROCESS_MEMORY_COUNTERS *)&mc,
		                sizeof mc)) {
			peak_mib = (double)mc.PeakPagefileUsage / (1 << 20);
		}
		printf("%-34s %-11s %6llu ms  host peak %7.1f MiB  src %ux%u  "
		       "file %.1f MiB%s\n",
		        label, status_name(st), (unsigned long long)ms,
		        peak_mib, (unsigned)info.src_w, (unsigned)info.src_h,
		        (double)n / (1 << 20), pm ? "" : "");
	}
	wf_pixmap_free(pm);
	if (ph) {
		CloseHandle(ph);
	}
	CloseHandle(f);
}

int main(int argc, char **argv)
{
	wchar_t tmp[MAX_PATH], exe[MAX_PATH * 2], staged[MAX_PATH * 2];
	wchar_t *slash;
	DWORD err = 0;
	int sandbox = 1;
	size_t n;
	uint8_t *p;

	setvbuf(stdout, NULL, _IONBF, 0);
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "plain")) {
			sandbox = 0;
		}
	}
	GetTempPathW(MAX_PATH, tmp);
	swprintf_s(g_dir, MAX_PATH, L"%lspiero_bigimg_%lu", tmp,
	        GetCurrentProcessId());
	CreateDirectoryW(g_dir, NULL);
	GetModuleFileNameW(NULL, exe, MAX_PATH * 2);
	slash = wcsrchr(exe, L'\\');
	wcscpy_s(slash + 1, 32, L"wf_dechost.exe");
	memset(&g_opt, 0, sizeof g_opt);
	g_opt.sandbox = sandbox;
	g_opt.exe = exe;
	if (sandbox) {
		if (WF_OK !=
		        wf_dechost_stage(exe, staged, MAX_PATH * 2, &err)) {
			printf("cannot stage the host (%lu)\n",
			        (unsigned long)err);
			return 1;
		}
		g_opt.exe = staged;
	}
	printf("host: %s, %u s limit, 512 MiB job\n\n",
	        sandbox ? "AppContainer" : "plain process", TIMEOUT_MS / 1000);

	{
		static const struct
		{
			const char *name;
			uint32_t w, h;
			int color, il;
		} png[] = {
		        {"png gray 1920x1080", 1920, 1080, 0, 0},
		        {"png rgb 7680x4320 (8K)", 7680, 4320, 2, 0},
		        {"png rgb 7680x4320 interlaced", 7680, 4320, 2, 1},
		        {"png gray 15360x8640 (16K)", 15360, 8640, 0, 0},
		        {"png rgb 15360x8640 (16K)", 15360, 8640, 2, 0},
		        {"png rgb 15360x8640 interlaced", 15360, 8640, 2, 1},
		        {"png gray 15999x15999 (just in)", 15999, 15999, 0, 0},
		        {"png gray 16384x16384 (over)", 16384, 16384, 0, 0},
		        {"png rgb 30000x30000 (over)", 30000, 30000, 2, 0},
		        {"png gray 65535x65535", 65535, 65535, 0, 0},
		        {"png gray 2^31-1 x 2^31-1", 0x7FFFFFFFu, 0x7FFFFFFFu,
		                0, 0},
		        {"png gray 2^31 x 1", 0x80000000u, 1, 0, 0},
		        {"png gray 1 x 2^31-1", 1, 0x7FFFFFFFu, 0, 0},
		        {"png rgb 0 x 0", 0, 0, 2, 0},
		};

		for (size_t i = 0; i < sizeof png / sizeof *png; i++) {
			wchar_t name[64];

			p = fix_png_zeros(png[i].w, png[i].h, png[i].color,
			        png[i].il, &n);
			swprintf_s(name, 64, L"p%zu.png", i);
			run_case(png[i].name, name, p, n);
			free(p);
		}
	}
	{
		static const struct
		{
			const char *name;
			int32_t w, h;
			unsigned bits;
			uint64_t body;
		} bmp[] = {
		        {"bmp 40000x40000 no pixels", 40000, 40000, 24, 0},
		        {"bmp 2^31-1 x 2^31-1", 0x7FFFFFFF, 0x7FFFFFFF, 32, 0},
		        {"bmp w -2^31", (int32_t)0x80000000, 100, 24, 0},
		        {"bmp h -2^31", 100, (int32_t)0x80000000, 24, 0},
		        {"bmp 16K x 16K 24bit (cut)", 16384, 16384, 24,
		                1 << 20},
		        {"bmp 8K 24bit full", 7680, 4320, 24,
		                7680ull * 4320 * 3},
		        {"bmp 1x1 0 bits", 1, 1, 0, 8},
		        {"bmp 1x1 99 bits", 1, 1, 99, 8},
		};

		for (size_t i = 0; i < sizeof bmp / sizeof *bmp; i++) {
			wchar_t name[64];

			p = fix_bmp(bmp[i].w, bmp[i].h, bmp[i].bits,
			        bmp[i].body, &n);
			swprintf_s(name, 64, L"b%zu.bmp", i);
			run_case(bmp[i].name, name, p, n);
			free(p);
		}
	}
	{
		static const struct
		{
			const char *name;
			const GUID *c;
			UINT w, h;
			const wchar_t *file;
			int noise;
		} enc[] = {
		        {"jpeg 7680x4320 (8K)", &GUID_ContainerFormatJpeg, 7680,
		                4320, L"j8.jpg", 0},
		        {"jpeg 15360x8640 (16K)", &GUID_ContainerFormatJpeg,
		                15360, 8640, L"j16.jpg", 0},
		        {"tiff 7680x4320 (8K)", &GUID_ContainerFormatTiff, 7680,
		                4320, L"t8.tif", 0},
		        {"gif 4000x3000", &GUID_ContainerFormatGif, 4000, 3000,
		                L"g.gif", 0},
		        {"jpeg NOISE 7680x4320", &GUID_ContainerFormatJpeg,
		                7680, 4320, L"jn8.jpg", 1},
		        {"jpeg NOISE 15360x8640", &GUID_ContainerFormatJpeg,
		                15360, 8640, L"jn16.jpg", 1},
		        {"tiff NOISE 7680x4320", &GUID_ContainerFormatTiff,
		                7680, 4320, L"tn8.tif", 1},
		        {"gif NOISE 4000x3000", &GUID_ContainerFormatGif, 4000,
		                3000, L"gn.gif", 1},
		};

		for (size_t i = 0; i < sizeof enc / sizeof *enc; i++) {
			p = wic_encode(
			        enc[i].c, enc[i].w, enc[i].h, enc[i].noise, &n);
			if (!p) {
				printf("%-34s could not be made\n",
				        enc[i].name);
				continue;
			}
			run_case(enc[i].name, enc[i].file, p, n);
			free(p);
		}
	}
	wf_dechost_stop(g_host);
	if (argc > 1 && !strcmp(argv[argc - 1], "keep")) {
		wprintf(L"\nfixtures kept in %ls\n", g_dir);
	}
	return 0;
}
