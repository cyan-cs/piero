/*
 * Prototype, not product code: can a plain Win32 C17 process render a PDF page
 * with Windows.Data.Pdf?
 *
 *   pdf_proto <file.pdf> [page=0] [width=800] [out.png]
 *
 * The file is read into memory and handed to the engine as a stream, so the
 * process never needs a path (the shape a sandboxed worker would use). The
 * page is rendered to PNG bytes in an in-memory stream and written out.
 * Asynchronous operations are waited on through IAsyncInfo; their results are
 * read through the fixed vtable slot of IAsyncOperation<T>::GetResults, so no
 * parameterised-interface IID has to be computed.
 */
#define COBJMACROS
#include <windows.h>

#include <roapi.h>
#include <shcore.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.data.pdf.h>
#include <windows.foundation.h>
#include <winstring.h>

/* The WinRT headers declare these IIDs but no import library defines them;
 * the values are the [uuid] attributes of the IDL files in the SDK. */
const IID IID___x_ABI_CWindows_CData_CPdf_CIPdfDocumentStatics = {0x433A0B5F,
        0xC007, 0x4788, {0x90, 0xF2, 0x08, 0x14, 0x3D, 0x92, 0x25, 0x99}};
const IID IID___x_ABI_CWindows_CData_CPdf_CIPdfPageRenderOptions = {0x3C98056F,
        0xB7CF, 0x4C29, {0x9A, 0x04, 0x52, 0xD9, 0x02, 0x67, 0xF4, 0x25}};
const IID IID___x_ABI_CWindows_CStorage_CStreams_CIRandomAccessStream = {
        0x905A0FE1, 0xBC53, 0x11DF,
        {0x8C, 0x49, 0x00, 0x1E, 0x4F, 0xC6, 0x86, 0xDA}};

#define PDF_NS Windows_CData_CPdf
typedef __x_ABI_CWindows_CData_CPdf_CIPdfDocumentStatics Statics;
typedef __x_ABI_CWindows_CData_CPdf_CIPdfDocument Document;
typedef __x_ABI_CWindows_CData_CPdf_CIPdfPage Page;
typedef __x_ABI_CWindows_CData_CPdf_CIPdfPageRenderOptions Options;
typedef __x_ABI_CWindows_CStorage_CStreams_CIRandomAccessStream RStream;

static double now_ms(void)
{
	LARGE_INTEGER f, c;

	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c);
	return 1000.0 * (double)c.QuadPart / (double)f.QuadPart;
}

/* IAsyncOperation<T> / IAsyncAction share IInspectable's six slots, then
 * put_Completed, get_Completed, GetResults (operation only). */
typedef struct
{
	HRESULT(STDMETHODCALLTYPE *qi)(void *, REFIID, void **);
	ULONG(STDMETHODCALLTYPE *addref)(void *);
	ULONG(STDMETHODCALLTYPE *release)(void *);
	HRESULT(STDMETHODCALLTYPE *iids)(void *, ULONG *, IID **);
	HRESULT(STDMETHODCALLTYPE *rtname)(void *, HSTRING *);
	HRESULT(STDMETHODCALLTYPE *trust)(void *, TrustLevel *);
	void *put_completed, *get_completed;
	HRESULT(STDMETHODCALLTYPE *get_results)(void *, void **);
} AsyncOpVtbl;

/* Waits for an async object; returns its error HRESULT (S_OK on success). */
static HRESULT await(void *op, DWORD timeout_ms)
{
	IAsyncInfo *info = NULL;
	AsyncStatus st = Started;
	DWORD t0 = GetTickCount();
	HRESULT hr, err = S_OK;

	hr = IUnknown_QueryInterface(
	        (IUnknown *)op, &IID_IAsyncInfo, (void **)&info);
	if (FAILED(hr)) {
		return hr;
	}
	for (;;) {
		hr = IAsyncInfo_get_Status(info, &st);
		if (FAILED(hr) || st != Started) {
			break;
		}
		if (GetTickCount() - t0 > timeout_ms) {
			IAsyncInfo_Cancel(info);
			hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
			break;
		}
		Sleep(1);
	}
	if (SUCCEEDED(hr)) {
		if (Error == st) {
			IAsyncInfo_get_ErrorCode(info, &err);
			hr = FAILED(err) ? err : E_FAIL;
		} else if (Canceled == st) {
			hr = E_ABORT;
		}
	}
	IAsyncInfo_Release(info);
	return hr;
}

static HRESULT activate(const wchar_t *cls, REFIID iid, void **out)
{
	HSTRING h;
	IInspectable *obj = NULL;
	HRESULT hr = WindowsCreateString(cls, (UINT32)wcslen(cls), &h);

	if (FAILED(hr)) {
		return hr;
	}
	hr = RoActivateInstance(h, &obj);
	WindowsDeleteString(h);
	if (FAILED(hr)) {
		return hr;
	}
	hr = IUnknown_QueryInterface((IUnknown *)obj, iid, out);
	IUnknown_Release((IUnknown *)obj);
	return hr;
}

static BYTE *read_file(const wchar_t *path, DWORD *n)
{
	HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, 0, NULL);
	LARGE_INTEGER sz;
	BYTE *buf;
	DWORD got;

	if (INVALID_HANDLE_VALUE == h) {
		return NULL;
	}
	if (!GetFileSizeEx(h, &sz) || sz.QuadPart > (256ll << 20)) {
		CloseHandle(h);
		return NULL;
	}
	buf = malloc((size_t)sz.QuadPart + 1);
	if (!buf || !ReadFile(h, buf, (DWORD)sz.QuadPart, &got, NULL)) {
		free(buf);
		CloseHandle(h);
		return NULL;
	}
	CloseHandle(h);
	*n = got;
	return buf;
}

int wmain(int argc, wchar_t **argv)
{
	const wchar_t *path = argc > 1 ? argv[1] : NULL;
	UINT32 page_no = argc > 2 ? (UINT32)_wtoi(argv[2]) : 0;
	UINT32 width = argc > 3 ? (UINT32)_wtoi(argv[3]) : 800;
	const wchar_t *out_path =
	        argc > 4 && wcscmp(argv[4], L"-") ? argv[4] : NULL;
	const wchar_t *password = argc > 5 ? argv[5] : NULL;
	HRESULT hr;
	HSTRING cls = NULL;
	Statics *statics = NULL;
	IStream *mem = NULL;
	RStream *ras = NULL;
	void *op = NULL;
	Document *doc = NULL;
	Page *page = NULL;
	BYTE *data;
	DWORD n = 0;
	double t0, t1, t2, t3;
	UINT32 pages = 0;
	boolean locked = 0;

	if (!path) {
		fputs("usage: pdf_proto file.pdf [page] [width] [out.png]\n",
		        stderr);
		return 2;
	}
	hr = RoInitialize(RO_INIT_MULTITHREADED);
	printf("RoInitialize        0x%08lx\n", (unsigned long)hr);
	data = read_file(path, &n);
	if (!data) {
		puts("read failed");
		return 1;
	}
	mem = SHCreateMemStream(data, n);
	hr = CreateRandomAccessStreamOverStream(mem, BSOS_DEFAULT,
	        &IID___x_ABI_CWindows_CStorage_CStreams_CIRandomAccessStream,
	        (void **)&ras);
	printf("RAStream over mem   0x%08lx\n", (unsigned long)hr);
	if (FAILED(hr)) {
		return 1;
	}

	WindowsCreateString(RuntimeClass_Windows_Data_Pdf_PdfDocument,
	        (UINT32)wcslen(RuntimeClass_Windows_Data_Pdf_PdfDocument),
	        &cls);
	hr = RoGetActivationFactory(cls,
	        &IID___x_ABI_CWindows_CData_CPdf_CIPdfDocumentStatics,
	        (void **)&statics);
	WindowsDeleteString(cls);
	printf("activation factory  0x%08lx\n", (unsigned long)hr);
	if (FAILED(hr)) {
		return 1;
	}

	t0 = now_ms();
	if (password) {
		HSTRING pw = NULL;

		WindowsCreateString(password, (UINT32)wcslen(password), &pw);
		hr = __x_ABI_CWindows_CData_CPdf_CIPdfDocumentStatics_LoadFromStreamWithPasswordAsync(
		        statics, ras, pw,
		        (__FIAsyncOperation_1_Windows__CData__CPdf__CPdfDocument
		                        **)&op);
		WindowsDeleteString(pw);
	} else {
		hr = __x_ABI_CWindows_CData_CPdf_CIPdfDocumentStatics_LoadFromStreamAsync(
		        statics, ras,
		        (__FIAsyncOperation_1_Windows__CData__CPdf__CPdfDocument
		                        **)&op);
	}
	if (SUCCEEDED(hr)) {
		hr = await(op, 10000);
	}
	printf("LoadFromStreamAsync 0x%08lx (%.1f ms)\n", (unsigned long)hr,
	        now_ms() - t0);
	if (FAILED(hr)) {
		return 1;
	}
	hr = ((AsyncOpVtbl *)*(void **)op)->get_results(op, (void **)&doc);
	printf("GetResults          0x%08lx\n", (unsigned long)hr);
	if (FAILED(hr) || !doc) {
		return 1;
	}
	__x_ABI_CWindows_CData_CPdf_CIPdfDocument_get_PageCount(doc, &pages);
	__x_ABI_CWindows_CData_CPdf_CIPdfDocument_get_IsPasswordProtected(
	        doc, &locked);
	printf("pages=%u password_protected=%d\n", pages, (int)locked);
	if (page_no >= pages) {
		puts("page out of range");
		return 1;
	}

	t1 = now_ms();
	hr = __x_ABI_CWindows_CData_CPdf_CIPdfDocument_GetPage(
	        doc, page_no, &page);
	printf("GetPage             0x%08lx (%.1f ms)\n", (unsigned long)hr,
	        now_ms() - t1);
	if (FAILED(hr)) {
		return 1;
	}
	{
		__x_ABI_CWindows_CFoundation_CSize sz = {0, 0};
		__x_ABI_CWindows_CData_CPdf_CPdfPageRotation rot;
		Options *opt = NULL;
		RStream *outs = NULL;
		void *act = NULL;
		BYTE *png;
		IStream *is = NULL;
		STATSTG stat;
		ULONG got = 0;

		__x_ABI_CWindows_CData_CPdf_CIPdfPage_get_Size(page, &sz);
		__x_ABI_CWindows_CData_CPdf_CIPdfPage_get_Rotation(page, &rot);
		printf("page size %.1f x %.1f pt, rotation=%d\n", sz.Width,
		        sz.Height, (int)rot);

		hr = activate(
		        RuntimeClass_Windows_Data_Pdf_PdfPageRenderOptions,
		        &IID___x_ABI_CWindows_CData_CPdf_CIPdfPageRenderOptions,
		        (void **)&opt);
		printf("options activate    0x%08lx\n", (unsigned long)hr);
		if (SUCCEEDED(hr)) {
			double scale =
			        (double)width / (sz.Width > 1 ? sz.Width : 1);

			__x_ABI_CWindows_CData_CPdf_CIPdfPageRenderOptions_put_DestinationWidth(
			        opt, width);
			__x_ABI_CWindows_CData_CPdf_CIPdfPageRenderOptions_put_DestinationHeight(
			        opt, (UINT32)(sz.Height * scale + 0.5));
		}
		hr = activate(
		        L"Windows.Storage.Streams.InMemoryRandomAccessStream",
		        &IID___x_ABI_CWindows_CStorage_CStreams_CIRandomAccessStream,
		        (void **)&outs);
		printf("memory stream       0x%08lx\n", (unsigned long)hr);
		if (FAILED(hr)) {
			return 1;
		}
		t2 = now_ms();
		hr = __x_ABI_CWindows_CData_CPdf_CIPdfPage_RenderWithOptionsToStreamAsync(
		        page, outs, opt,
		        (__x_ABI_CWindows_CFoundation_CIAsyncAction **)&act);
		if (SUCCEEDED(hr)) {
			hr = await(act, 15000);
		}
		t3 = now_ms();
		printf("RenderToStream      0x%08lx (%.1f ms)\n",
		        (unsigned long)hr, t3 - t2);
		if (FAILED(hr)) {
			return 1;
		}
		hr = CreateStreamOverRandomAccessStream(
		        (IUnknown *)outs, &IID_IStream, (void **)&is);
		if (FAILED(hr)) {
			printf("stream over ras     0x%08lx\n",
			        (unsigned long)hr);
			return 1;
		}
		IStream_Stat(is, &stat, STATFLAG_NONAME);
		png = malloc((size_t)stat.cbSize.QuadPart);
		{
			LARGE_INTEGER z = {0};

			IStream_Seek(is, z, STREAM_SEEK_SET, NULL);
		}
		ISequentialStream_Read((ISequentialStream *)is, png,
		        (ULONG)stat.cbSize.QuadPart, &got);
		printf("png bytes=%lu signature=%s\n", (unsigned long)got,
		        got > 8 && !memcmp(png + 1, "PNG", 3) ? "ok" : "BAD");
		if (out_path) {
			HANDLE f = CreateFileW(out_path, GENERIC_WRITE, 0, NULL,
			        CREATE_NEW, 0, NULL);
			DWORD w;

			if (INVALID_HANDLE_VALUE != f) {
				WriteFile(f, png, got, &w, NULL);
				CloseHandle(f);
			}
		}
	}
	return 0;
}
