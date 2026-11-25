/*
 * Windows.Data.Pdf from C. The engine is a WinRT class; this file is the only
 * place that knows that. What was established by experiment:
 *
 *  - an unpackaged Win32 process, and a capability-less AppContainer, can
 *    activate it;
 *  - the document is loaded from memory (no path), the page rendered into an
 *    in-memory stream as PNG;
 *  - async results are read through the fixed vtable slot of
 *    IAsyncOperation<T>::GetResults, so no parameterised-interface IID is
 *    needed, and IAsyncInfo::Close must NOT come before GetResults;
 *  - a missing or wrong password is 0x8007052B (ERROR_WRONG_PASSWORD).
 */
#define COBJMACROS
#include <windows.h>

#include <asyncinfo.h>
#include <roapi.h>
#include <shcore.h>
#include <shlwapi.h>
#include <stdlib.h>
#include <string.h>
#include <windows.data.pdf.h>
#include <windows.foundation.h>
#include <winstring.h>

#include "wf_decode_win.h"

#define PDF_TIMEOUT_MS 15000u
#define PNG_MAX_BYTES (64u << 20)

/* The WinRT headers declare these but no library defines them; the values are
 * the [uuid] attributes of the SDK's IDL files. */
const IID IID___x_ABI_CWindows_CData_CPdf_CIPdfDocumentStatics = {0x433A0B5F,
        0xC007, 0x4788, {0x90, 0xF2, 0x08, 0x14, 0x3D, 0x92, 0x25, 0x99}};
const IID IID___x_ABI_CWindows_CData_CPdf_CIPdfPageRenderOptions = {0x3C98056F,
        0xB7CF, 0x4C29, {0x9A, 0x04, 0x52, 0xD9, 0x02, 0x67, 0xF4, 0x25}};
const IID IID___x_ABI_CWindows_CStorage_CStreams_CIRandomAccessStream = {
        0x905A0FE1, 0xBC53, 0x11DF,
        {0x8C, 0x49, 0x00, 0x1E, 0x4F, 0xC6, 0x86, 0xDA}};

typedef __x_ABI_CWindows_CData_CPdf_CIPdfDocumentStatics Statics;
typedef __x_ABI_CWindows_CData_CPdf_CIPdfDocument Document;
typedef __x_ABI_CWindows_CData_CPdf_CIPdfPage Page;
typedef __x_ABI_CWindows_CData_CPdf_CIPdfPageRenderOptions Options;
typedef __x_ABI_CWindows_CStorage_CStreams_CIRandomAccessStream RaStream;

/* IAsyncOperation<T> after IInspectable's six slots: put_Completed,
 * get_Completed, GetResults. */
typedef struct
{
	void *base[6];
	void *put_completed, *get_completed;
	HRESULT(STDMETHODCALLTYPE *get_results)(void *self, void **result);
} AsyncOperationVtbl;

/* Waits for an asynchronous object, cancelling it past the deadline. The
 * object stays open: results are read by the caller, who releases it. */
static HRESULT await(void *op, DWORD timeout_ms)
{
	IAsyncInfo *info = NULL;
	AsyncStatus st = Started;
	ULONGLONG t0 = GetTickCount64();
	HRESULT hr = IUnknown_QueryInterface(
	        (IUnknown *)op, &IID_IAsyncInfo, (void **)&info);

	if (FAILED(hr)) {
		return hr;
	}
	for (;;) {
		hr = IAsyncInfo_get_Status(info, &st);
		if (FAILED(hr) || Started != st) {
			break;
		}
		if (GetTickCount64() - t0 > timeout_ms) {
			(void)IAsyncInfo_Cancel(info);
			hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
			break;
		}
		Sleep(1);
	}
	if (SUCCEEDED(hr) && Error == st) {
		HRESULT err = E_FAIL;

		(void)IAsyncInfo_get_ErrorCode(info, &err);
		hr = FAILED(err) ? err : E_FAIL;
	} else if (SUCCEEDED(hr) && Canceled == st) {
		hr = E_ABORT;
	}
	IAsyncInfo_Release(info);
	return hr;
}

static HRESULT activate(const wchar_t *cls, REFIID iid, void **out)
{
	HSTRING h = NULL;
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

static WfDecodeStatus status_of(HRESULT hr, int had_password)
{
	if (HRESULT_FROM_WIN32(ERROR_WRONG_PASSWORD) == hr) {
		return had_password ? WF_DSTAT_PASSWORD_WRONG
		                    : WF_DSTAT_PASSWORD_REQUIRED;
	}
	if (HRESULT_FROM_WIN32(ERROR_TIMEOUT) == hr || E_ABORT == hr) {
		return WF_DSTAT_TIMEOUT;
	}
	if (E_OUTOFMEMORY == hr) {
		return WF_DSTAT_INTERNAL;
	}
	/* 0x80048018 and the other PDF load failures: the file is not a PDF the
	 * engine can read. Anything unexpected at load time is the same for the
	 * user: this file cannot be shown. */
	return WF_DSTAT_CORRUPT;
}

WfDecodeStatus wf_pdf_render_png(const void *data,
        size_t n,
        uint32_t page_no,
        uint32_t box_w,
        uint32_t box_h,
        const wchar_t *password,
        void **png,
        size_t *png_n,
        WfDecodeInfo *info)
{
	WfDecodeStatus result = WF_DSTAT_INTERNAL;
	HRESULT hr, ro = E_FAIL;
	Statics *statics = NULL;
	IStream *mem = NULL, *png_stream = NULL;
	RaStream *input = NULL, *output = NULL;
	void *op = NULL, *render = NULL;
	Document *doc = NULL;
	Page *page = NULL;
	Options *opt = NULL;
	HSTRING cls = NULL, pw = NULL;
	UINT32 pages = 0;
	boolean locked = 0;
	__x_ABI_CWindows_CFoundation_CSize size = {0, 0};
	uint32_t src_w, src_h, dst_w, dst_h;
	STATSTG stat;
	LARGE_INTEGER zero = {0};
	unsigned char *bytes = NULL;
	ULONG got = 0;

	*png = NULL;
	*png_n = 0;
	if (!data || 0 == n || n > WF_DWIRE_INPUT_MAX) {
		return WF_DSTAT_UNSUPPORTED;
	}
	ro = RoInitialize(RO_INIT_MULTITHREADED);
	if (FAILED(ro) && RPC_E_CHANGED_MODE != ro) {
		return WF_DSTAT_INTERNAL;
	}
	mem = SHCreateMemStream((const BYTE *)data, (UINT)n);
	if (!mem) {
		goto done;
	}
	hr = CreateRandomAccessStreamOverStream(mem, BSOS_DEFAULT,
	        &IID___x_ABI_CWindows_CStorage_CStreams_CIRandomAccessStream,
	        (void **)&input);
	if (FAILED(hr)) {
		goto done;
	}
	hr = WindowsCreateString(RuntimeClass_Windows_Data_Pdf_PdfDocument,
	        (UINT32)wcslen(RuntimeClass_Windows_Data_Pdf_PdfDocument),
	        &cls);
	if (SUCCEEDED(hr)) {
		hr = RoGetActivationFactory(cls,
		        &IID___x_ABI_CWindows_CData_CPdf_CIPdfDocumentStatics,
		        (void **)&statics);
	}
	if (FAILED(hr)) {
		goto done;
	}
	if (password && password[0]) {
		hr = WindowsCreateString(
		        password, (UINT32)wcslen(password), &pw);
		if (SUCCEEDED(hr)) {
			hr = __x_ABI_CWindows_CData_CPdf_CIPdfDocumentStatics_LoadFromStreamWithPasswordAsync(
			        statics, input, pw,
			        (__FIAsyncOperation_1_Windows__CData__CPdf__CPdfDocument
			                        **)&op);
		}
	} else {
		hr = __x_ABI_CWindows_CData_CPdf_CIPdfDocumentStatics_LoadFromStreamAsync(
		        statics, input,
		        (__FIAsyncOperation_1_Windows__CData__CPdf__CPdfDocument
		                        **)&op);
	}
	if (SUCCEEDED(hr)) {
		hr = await(op, PDF_TIMEOUT_MS);
	}
	if (SUCCEEDED(hr)) {
		hr = (*(AsyncOperationVtbl **)op)
		             ->get_results(op, (void **)&doc);
	}
	if (FAILED(hr) || !doc) {
		result = status_of(FAILED(hr) ? hr : E_FAIL, NULL != pw);
		goto done;
	}
	(void)__x_ABI_CWindows_CData_CPdf_CIPdfDocument_get_PageCount(
	        doc, &pages);
	(void)__x_ABI_CWindows_CData_CPdf_CIPdfDocument_get_IsPasswordProtected(
	        doc, &locked);
	info->page_count = pages;
	if (0 == pages) {
		result = WF_DSTAT_CORRUPT;
		goto done;
	}
	if (page_no >= pages) {
		result = WF_DSTAT_PAGE_RANGE;
		goto done;
	}
	hr = __x_ABI_CWindows_CData_CPdf_CIPdfDocument_GetPage(
	        doc, page_no, &page);
	if (FAILED(hr) || !page) {
		result = WF_DSTAT_CORRUPT;
		goto done;
	}
	(void)__x_ABI_CWindows_CData_CPdf_CIPdfPage_get_Size(page, &size);
	if (!(size.Width >= 1.0f) || !(size.Height >= 1.0f) ||
	        size.Width > 200000.0f || size.Height > 200000.0f) {
		result = WF_DSTAT_CORRUPT;
		goto done;
	}
	src_w = (uint32_t)(size.Width + 0.5f);
	src_h = (uint32_t)(size.Height + 0.5f);
	info->src_w = src_w;
	info->src_h = src_h;
	/* a vector page is rendered at the size it will be shown at */
	wf_fit_box(src_w, src_h, box_w, box_h, 1, &dst_w, &dst_h);

	hr = activate(RuntimeClass_Windows_Data_Pdf_PdfPageRenderOptions,
	        &IID___x_ABI_CWindows_CData_CPdf_CIPdfPageRenderOptions,
	        (void **)&opt);
	if (SUCCEEDED(hr)) {
		__x_ABI_CWindows_CUI_CColor white = {255, 255, 255, 255};

		(void)__x_ABI_CWindows_CData_CPdf_CIPdfPageRenderOptions_put_DestinationWidth(
		        opt, dst_w);
		(void)__x_ABI_CWindows_CData_CPdf_CIPdfPageRenderOptions_put_DestinationHeight(
		        opt, dst_h);
		(void)__x_ABI_CWindows_CData_CPdf_CIPdfPageRenderOptions_put_BackgroundColor(
		        opt, white);
	}
	if (FAILED(hr)) {
		goto done;
	}
	hr = activate(L"Windows.Storage.Streams.InMemoryRandomAccessStream",
	        &IID___x_ABI_CWindows_CStorage_CStreams_CIRandomAccessStream,
	        (void **)&output);
	if (FAILED(hr)) {
		goto done;
	}
	hr = __x_ABI_CWindows_CData_CPdf_CIPdfPage_RenderWithOptionsToStreamAsync(
	        page, output, opt,
	        (__x_ABI_CWindows_CFoundation_CIAsyncAction **)&render);
	if (SUCCEEDED(hr)) {
		hr = await(render, PDF_TIMEOUT_MS);
	}
	if (FAILED(hr)) {
		result = status_of(hr, 0);
		if (WF_DSTAT_CORRUPT == result) {
			result =
			        WF_DSTAT_INTERNAL; /* the document loaded; rendering
				                      it failed */
		}
		goto done;
	}
	hr = CreateStreamOverRandomAccessStream(
	        (IUnknown *)output, &IID_IStream, (void **)&png_stream);
	if (FAILED(hr) ||
	        FAILED(IStream_Stat(png_stream, &stat, STATFLAG_NONAME)) ||
	        0 == stat.cbSize.QuadPart ||
	        stat.cbSize.QuadPart > PNG_MAX_BYTES) {
		goto done;
	}
	bytes = malloc((size_t)stat.cbSize.QuadPart);
	if (!bytes) {
		goto done;
	}
	if (FAILED(IStream_Seek(png_stream, zero, STREAM_SEEK_SET, NULL)) ||
	        FAILED(ISequentialStream_Read((ISequentialStream *)png_stream,
	                bytes, (ULONG)stat.cbSize.QuadPart, &got)) ||
	        got != stat.cbSize.QuadPart) {
		goto done;
	}
	*png = bytes;
	*png_n = got;
	bytes = NULL;
	result = WF_DSTAT_OK;
done:
	free(bytes);
	if (png_stream) {
		IStream_Release(png_stream);
	}
	if (render) {
		IUnknown_Release((IUnknown *)render);
	}
	if (output) {
		IUnknown_Release((IUnknown *)output);
	}
	if (opt) {
		IUnknown_Release((IUnknown *)opt);
	}
	if (page) {
		IUnknown_Release((IUnknown *)page);
	}
	if (doc) {
		IUnknown_Release((IUnknown *)doc);
	}
	if (op) {
		IUnknown_Release((IUnknown *)op);
	}
	if (statics) {
		IUnknown_Release((IUnknown *)statics);
	}
	if (pw) {
		WindowsDeleteString(pw);
	}
	if (cls) {
		WindowsDeleteString(cls);
	}
	if (input) {
		IUnknown_Release((IUnknown *)input);
	}
	if (mem) {
		IStream_Release(mem);
	}
	if (SUCCEEDED(ro)) {
		RoUninitialize();
	}
	(void)locked;
	return result;
}
