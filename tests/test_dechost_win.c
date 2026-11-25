#define COBJMACROS
#include <windows.h>
#include <tlhelp32.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wincodec.h>

#include "pdf_samples.h"
#include "preview/wf_dechost_client_win.h"
#include "preview/wf_dechost_win.h"
#include "wf_test.h"

/*
 * The decoder host, three ways: its loop driven directly over pipes (the
 * protocol), as an ordinary child process (the plumbing) and as an
 * AppContainer process (the isolation). The AppContainer part reports SKIP
 * (exit code 77) where the machine cannot make one: it is never counted as
 * passed then.
 */

#define SKIP_CODE 77

static wchar_t g_dir[MAX_PATH]; /* a private temp folder, removed at exit */
static wchar_t g_host_exe[MAX_PATH * 2];
static int g_skipped;

static void make_dir(void)
{
	wchar_t tmp[MAX_PATH];

	GetTempPathW(MAX_PATH, tmp);
	swprintf(g_dir, MAX_PATH, L"%lswfdh.%lu.%llu", tmp,
	        GetCurrentProcessId(), (unsigned long long)GetTickCount64());
	CHECK(CreateDirectoryW(g_dir, NULL));
}

static void remove_dir(void)
{
	wchar_t pat[MAX_PATH + 4];
	WIN32_FIND_DATAW fd;
	HANDLE f;

	swprintf(pat, MAX_PATH + 4, L"%ls\\*", g_dir);
	f = FindFirstFileW(pat, &fd);
	if (INVALID_HANDLE_VALUE != f) {
		do {
			wchar_t one[MAX_PATH * 2];

			if (L'.' == fd.cFileName[0]) {
				continue;
			}
			swprintf(one, MAX_PATH * 2, L"%ls\\%ls", g_dir,
			        fd.cFileName);
			DeleteFileW(one);
		} while (FindNextFileW(f, &fd));
		FindClose(f);
	}
	RemoveDirectoryW(g_dir);
}

static HANDLE open_fixture(const wchar_t *name, const void *bytes, size_t n)
{
	wchar_t path[MAX_PATH * 2];
	HANDLE w;
	DWORD put;

	swprintf(path, MAX_PATH * 2, L"%ls\\%ls", g_dir, name);
	w = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(INVALID_HANDLE_VALUE != w);
	CHECK(WriteFile(w, bytes, (DWORD)n, &put, NULL) && put == n);
	CloseHandle(w);
	return CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
}

/* A PNG, made by WIC. */
static void *make_png(UINT w, UINT h, size_t *n)
{
	IWICImagingFactory *wic = NULL;
	IWICBitmapEncoder *enc = NULL;
	IWICBitmapFrameEncode *fe = NULL;
	IStream *stream = NULL;
	IPropertyBag2 *bag = NULL;
	WICPixelFormatGUID f = GUID_WICPixelFormat32bppBGRA;
	unsigned char *pix = malloc((size_t)w * h * 4);
	void *out = NULL;

	for (size_t i = 0; i < (size_t)w * h; i++) {
		pix[i * 4] = (unsigned char)(i * 3);
		pix[i * 4 + 1] = (unsigned char)(i / w);
		pix[i * 4 + 2] = (unsigned char)(i % w);
		pix[i * 4 + 3] = 255;
	}
	CoInitializeEx(NULL, COINIT_MULTITHREADED);
	CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
	        &IID_IWICImagingFactory, (void **)&wic);
	CreateStreamOnHGlobal(NULL, TRUE, &stream);
	IWICImagingFactory_CreateEncoder(
	        wic, &GUID_ContainerFormatPng, NULL, &enc);
	IWICBitmapEncoder_Initialize(enc, stream, WICBitmapEncoderNoCache);
	IWICBitmapEncoder_CreateNewFrame(enc, &fe, &bag);
	IWICBitmapFrameEncode_Initialize(fe, bag);
	IWICBitmapFrameEncode_SetSize(fe, w, h);
	IWICBitmapFrameEncode_SetPixelFormat(fe, &f);
	IWICBitmapFrameEncode_WritePixels(fe, h, w * 4, w * 4 * h, pix);
	IWICBitmapFrameEncode_Commit(fe);
	IWICBitmapEncoder_Commit(enc);
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
	IPropertyBag2_Release(bag);
	IWICBitmapFrameEncode_Release(fe);
	IWICBitmapEncoder_Release(enc);
	IStream_Release(stream);
	IWICImagingFactory_Release(wic);
	free(pix);
	return out;
}

typedef struct
{
	HANDLE req_r, rep_w;
	int code;
} Loop;

static DWORD WINAPI loop_thread(LPVOID arg)
{
	Loop *l = arg;

	l->code = wf_dechost_main(l->req_r, l->rep_w);
	return 0;
}

static int send_all(HANDLE h, const void *p, size_t n)
{
	DWORD put;

	return WriteFile(h, p, (DWORD)n, &put, NULL) && put == n;
}

static int recv_all(HANDLE h, void *p, size_t n)
{
	size_t done = 0;

	while (done < n) {
		DWORD got = 0;

		if (!ReadFile(h, (char *)p + done, (DWORD)(n - done), &got,
		            NULL) ||
		        0 == got) {
			return 0;
		}
		done += got;
	}
	return 1;
}

static void the_loop_answers_requests_and_ends_cleanly(void)
{
	HANDLE req_r = NULL, req_w = NULL, rep_r = NULL, rep_w = NULL, file,
	       dup, thread;
	Loop loop;
	size_t png_n;
	void *png = make_png(120, 80, &png_n);
	WfDecodeRequest rq;
	unsigned char msg[WF_DWIRE_REQUEST_HEADER], hdr[WF_DWIRE_REPLY_HEADER];
	WfDecodeReply rp;
	unsigned char *raster;

	CHECK(CreatePipe(&req_r, &req_w, NULL, 0) &&
	        CreatePipe(&rep_r, &rep_w, NULL, 0));
	loop.req_r = req_r;
	loop.rep_w = rep_w;
	loop.code = -1;
	thread = CreateThread(NULL, 0, loop_thread, &loop, 0, NULL);
	file = open_fixture(L"loop.png", png, png_n);
	CHECK(DuplicateHandle(GetCurrentProcess(), file, GetCurrentProcess(),
	        &dup, FILE_GENERIC_READ, FALSE, 0));

	memset(&rq, 0, sizeof rq);
	rq.request = 7;
	rq.kind = WF_DKIND_IMAGE;
	rq.box_w = 60;
	rq.box_h = 60;
	rq.input_size = png_n;
	rq.input_handle = (uint64_t)(uintptr_t)dup;
	CHECK(sizeof msg ==
	        wf_dwire_request_encode(&rq, NULL, msg, sizeof msg));
	CHECK(send_all(req_w, msg, sizeof msg));
	CHECK(recv_all(rep_r, hdr, sizeof hdr));
	CHECK(wf_dwire_reply_header(hdr, &rq, &rp));
	CHECK(WF_DSTAT_OK == rp.status && 60 == rp.out_w && 40 == rp.out_h);
	raster = malloc((size_t)rp.payload);
	CHECK(recv_all(rep_r, raster, (size_t)rp.payload));
	free(raster);

	/* closing the request pipe between requests is a clean end */
	CloseHandle(req_w);
	CHECK(WAIT_OBJECT_0 == WaitForSingleObject(thread, 10000));
	CHECK(0 == loop.code);
	CloseHandle(thread);
	CloseHandle(file);
	CloseHandle(req_r);
	CloseHandle(rep_r);
	CloseHandle(rep_w);
	free(png);
}

static int run_loop_with(const void *bytes, size_t n)
{
	HANDLE req_r = NULL, req_w = NULL, rep_r = NULL, rep_w = NULL, thread;
	Loop loop;
	unsigned char drain[256];
	DWORD got;

	CHECK(CreatePipe(&req_r, &req_w, NULL, 0) &&
	        CreatePipe(&rep_r, &rep_w, NULL, 0));
	loop.req_r = req_r;
	loop.rep_w = rep_w;
	loop.code = -1;
	thread = CreateThread(NULL, 0, loop_thread, &loop, 0, NULL);
	if (n) {
		send_all(req_w, bytes, n);
	}
	CloseHandle(req_w);
	CHECK(WAIT_OBJECT_0 == WaitForSingleObject(thread, 10000));
	(void)got;
	(void)drain;
	CloseHandle(thread);
	CloseHandle(req_r);
	CloseHandle(rep_r);
	CloseHandle(rep_w);
	return loop.code;
}

static void a_peer_that_breaks_the_protocol_ends_the_host(void)
{
	unsigned char junk[200];
	unsigned char msg[WF_DWIRE_REQUEST_HEADER];
	WfDecodeRequest rq;

	memset(junk, 0xAB, sizeof junk);
	CHECK(2 == run_loop_with(junk, sizeof junk)); /* not a header */
	CHECK(2 == run_loop_with(junk, 10));          /* cut short */
	CHECK(0 == run_loop_with(NULL, 0));           /* nothing at all */
	/* a good header whose password never arrives */
	memset(&rq, 0, sizeof rq);
	rq.request = 1;
	rq.kind = WF_DKIND_PDF;
	rq.box_w = rq.box_h = 50;
	rq.input_size = 10;
	rq.password_units = 20;
	{
		uint16_t pw[20] = {0};
		unsigned char full[WF_DWIRE_REQUEST_HEADER + 40];

		CHECK(sizeof full ==
		        wf_dwire_request_encode(&rq, pw, full, sizeof full));
		CHECK(2 == run_loop_with(full, sizeof msg + 10));
	}
}

static void a_file_that_is_not_what_was_described_is_a_reply_not_a_crash(void)
{
	/* The request says 1000 bytes; the handle is a pipe (not a disk file).
	 * The host answers CORRUPT and carries on, then ends cleanly. */
	HANDLE req_r = NULL, req_w = NULL, rep_r = NULL, rep_w = NULL, thread,
	       pr = NULL, pw = NULL, dup;
	Loop loop;
	WfDecodeRequest rq;
	unsigned char msg[WF_DWIRE_REQUEST_HEADER], hdr[WF_DWIRE_REPLY_HEADER];
	WfDecodeReply rp;

	CHECK(CreatePipe(&req_r, &req_w, NULL, 0) &&
	        CreatePipe(&rep_r, &rep_w, NULL, 0) &&
	        CreatePipe(&pr, &pw, NULL, 0));
	CHECK(DuplicateHandle(GetCurrentProcess(), pr, GetCurrentProcess(),
	        &dup, 0, FALSE, DUPLICATE_SAME_ACCESS));
	loop.req_r = req_r;
	loop.rep_w = rep_w;
	thread = CreateThread(NULL, 0, loop_thread, &loop, 0, NULL);
	memset(&rq, 0, sizeof rq);
	rq.request = 3;
	rq.kind = WF_DKIND_IMAGE;
	rq.box_w = rq.box_h = 50;
	rq.input_size = 1000;
	rq.input_handle = (uint64_t)(uintptr_t)dup;
	CHECK(sizeof msg ==
	        wf_dwire_request_encode(&rq, NULL, msg, sizeof msg));
	CHECK(send_all(req_w, msg, sizeof msg));
	CHECK(recv_all(rep_r, hdr, sizeof hdr));
	CHECK(wf_dwire_reply_header(hdr, &rq, &rp) &&
	        WF_DSTAT_CORRUPT == rp.status && 0 == rp.payload);
	/* a handle number that does not exist at all */
	rq.request = 4;
	rq.input_handle = 0x7FFF0;
	CHECK(sizeof msg ==
	        wf_dwire_request_encode(&rq, NULL, msg, sizeof msg));
	CHECK(send_all(req_w, msg, sizeof msg));
	CHECK(recv_all(rep_r, hdr, sizeof hdr));
	CHECK(wf_dwire_reply_header(hdr, &rq, &rp) &&
	        WF_DSTAT_CORRUPT == rp.status);
	CloseHandle(req_w);
	WaitForSingleObject(thread, 10000);
	CloseHandle(thread);
	CloseHandle(req_r);
	CloseHandle(rep_r);
	CloseHandle(rep_w);
	CloseHandle(pr);
	CloseHandle(pw);
}

/* A 24-bit bitmap w x h (uncompressed: w * h * 3 bytes and a header). */
static unsigned char *make_bmp(uint32_t w, uint32_t h, size_t *n)
{
	size_t row = ((size_t)w * 3 + 3) & ~(size_t)3, size = 54 + row * h;
	unsigned char *b = calloc(1, size);

	b[0] = 'B';
	b[1] = 'M';
	memcpy(b + 2, &(uint32_t){(uint32_t)size}, 4);
	b[10] = 54;
	b[14] = 40;
	memcpy(b + 18, &w, 4);
	memcpy(b + 22, &h, 4);
	b[26] = 1;
	b[28] = 24;
	for (size_t i = 54; i < size; i++) {
		b[i] = (unsigned char)(i * 7);
	}
	*n = size;
	return b;
}

static void exercise_a_host(WfDecHost *h, int sandboxed)
{
	size_t png_n;
	void *png = make_png(200, 100, &png_n);
	HANDLE f_png = open_fixture(L"a.png", png, png_n);
	HANDLE f_pdf =
	        open_fixture(L"two.pdf", pdf_two_page, sizeof pdf_two_page);
	HANDLE f_enc =
	        open_fixture(L"enc.pdf", pdf_encrypted, sizeof pdf_encrypted);
	HANDLE f_bad =
	        open_fixture(L"bad.pdf", pdf_garbage, sizeof pdf_garbage);
	WfPixmap *pm = NULL;
	WfDecodeInfo info;

	CHECK(wf_dechost_alive(h));
	CHECK(sandboxed == wf_dechost_sandboxed(h));
	CHECK(0 != wf_dechost_pid(h));

	CHECK(WF_DSTAT_OK ==
	        wf_dechost_decode(h, WF_DKIND_IMAGE, f_png, 100, 100, 0, NULL,
	                20000, NULL, 0, &pm, &info));
	CHECK(pm && 100 == pm->w && 50 == pm->h && 1 == info.page_count &&
	        200 == info.src_w && 100 == info.src_h);
	wf_pixmap_free(pm);
	pm = NULL;

	CHECK(WF_DSTAT_OK ==
	        wf_dechost_decode(h, WF_DKIND_PDF, f_pdf, 300, 400, 0, NULL,
	                30000, NULL, 0, &pm, &info));
	CHECK(pm && 2 == info.page_count && pm->h <= 400 && pm->w <= 300);
	wf_pixmap_free(pm);
	pm = NULL;
	CHECK(WF_DSTAT_OK ==
	        wf_dechost_decode(h, WF_DKIND_PDF, f_pdf, 300, 400, 1, NULL,
	                30000, NULL, 0, &pm, &info));
	CHECK(pm && 300 == pm->w); /* the rotated page is wider than tall */
	wf_pixmap_free(pm);
	pm = NULL;

	/* answers that are refusals leave the host running */
	CHECK(WF_DSTAT_PAGE_RANGE ==
	                wf_dechost_decode(h, WF_DKIND_PDF, f_pdf, 100, 100, 5,
	                        NULL, 30000, NULL, 0, &pm, &info) &&
	        2 == info.page_count);
	CHECK(WF_DSTAT_PASSWORD_REQUIRED ==
	        wf_dechost_decode(h, WF_DKIND_PDF, f_enc, 100, 100, 0, NULL,
	                30000, NULL, 0, &pm, &info));
	CHECK(WF_DSTAT_PASSWORD_WRONG ==
	        wf_dechost_decode(h, WF_DKIND_PDF, f_enc, 100, 100, 0, L"wrong",
	                30000, NULL, 0, &pm, &info));
	CHECK(WF_DSTAT_OK ==
	        wf_dechost_decode(h, WF_DKIND_PDF, f_enc, 100, 100, 0,
	                L"secret", 30000, NULL, 0, &pm, &info));
	wf_pixmap_free(pm);
	pm = NULL;
	CHECK(WF_DSTAT_CORRUPT ==
	        wf_dechost_decode(h, WF_DKIND_PDF, f_bad, 100, 100, 0, NULL,
	                30000, NULL, 0, &pm, &info));
	CHECK(NULL == pm && wf_dechost_alive(h));

	/* a file with no handle: its bytes go down the pipe (an archive entry) */
	CHECK(WF_DSTAT_OK ==
	        wf_dechost_decode_bytes(h, WF_DKIND_IMAGE, png, png_n, 100, 100,
	                0, NULL, 20000, NULL, 0, &pm, &info));
	CHECK(pm && 100 == pm->w && 50 == pm->h);
	wf_pixmap_free(pm);
	pm = NULL;
	CHECK(WF_DSTAT_OK ==
	        wf_dechost_decode_bytes(h, WF_DKIND_PDF, pdf_two_page,
	                sizeof pdf_two_page, 200, 260, 1, NULL, 30000, NULL, 0,
	                &pm, &info));
	CHECK(pm && 2 == info.page_count);
	wf_pixmap_free(pm);
	pm = NULL;
	{
		/* a few megabytes: many pipe slices, and the host stays in step */
		size_t big_n;
		unsigned char *big = make_bmp(1400, 1000, &big_n);

		CHECK(big_n > 4u * 1000 * 1000);
		CHECK(WF_DSTAT_OK ==
		        wf_dechost_decode_bytes(h, WF_DKIND_IMAGE, big, big_n,
		                140, 100, 0, NULL, 30000, NULL, 0, &pm, &info));
		CHECK(pm && 140 == pm->w && 100 == pm->h && 1400 == info.src_w);
		wf_pixmap_free(pm);
		pm = NULL;
		free(big);
		/* noise is not a picture: unsupported or damaged, never shown */
		CHECK(WF_DSTAT_OK !=
		                wf_dechost_decode_bytes(h, WF_DKIND_IMAGE,
		                        pdf_garbage, sizeof pdf_garbage, 50, 50,
		                        0, NULL, 20000, NULL, 0, &pm, &info) &&
		        NULL == pm);
		CHECK(WF_DSTAT_CORRUPT ==
		        wf_dechost_decode_bytes(h, WF_DKIND_IMAGE, NULL, 5, 50,
		                50, 0, NULL, 20000, NULL, 0, &pm, &info));
		/* the same host still answers afterwards */
		CHECK(WF_DSTAT_OK ==
		        wf_dechost_decode(h, WF_DKIND_IMAGE, f_png, 64, 64, 0,
		                NULL, 20000, NULL, 0, &pm, NULL));
		wf_pixmap_free(pm);
		pm = NULL;
	}
	/* many in a row on the same host */
	for (int i = 0; i < 25; i++) {
		CHECK(WF_DSTAT_OK ==
		        wf_dechost_decode(h, WF_DKIND_IMAGE, f_png, 64, 64, 0,
		                NULL, 20000, NULL, 0, &pm, NULL));
		wf_pixmap_free(pm);
		pm = NULL;
	}
	CloseHandle(f_png);
	CloseHandle(f_pdf);
	CloseHandle(f_enc);
	CloseHandle(f_bad);
	free(png);
}

static void a_plain_host_process_decodes_and_stops(void)
{
	WfDecHostOptions opt = {g_host_exe, 0, 0};
	WfDecHost *h = NULL;
	DWORD err = 0;
	DWORD pid;
	HANDLE proc;

	CHECK(WF_OK == wf_dechost_start(&opt, &h, &err));
	if (!h) {
		printf("  start failed, error %lu\n", (unsigned long)err);
		return;
	}
	pid = wf_dechost_pid(h);
	exercise_a_host(h, 0);
	proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
	wf_dechost_stop(h);
	/* stopping really ended the process */
	if (proc) {
		CHECK(WAIT_OBJECT_0 == WaitForSingleObject(proc, 5000));
		CloseHandle(proc);
	}
}

static void a_host_that_is_too_slow_is_killed_and_replaced(void)
{
	WfDecHostOptions opt = {g_host_exe, 0, 0};
	WfDecHost *h = NULL, *second = NULL;
	size_t png_n;
	void *png = make_png(300, 200, &png_n);
	HANDLE f = open_fixture(L"slow.png", png, png_n);
	WfPixmap *pm = (WfPixmap *)1;
	DWORD pid, err;
	HANDLE proc;

	CHECK(WF_OK == wf_dechost_start(&opt, &h, &err));
	pid = wf_dechost_pid(h);
	proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
	/* A real small PNG can decode within 1 ms. Stall this test's own host
	 * thread instead of depending on machine speed to exercise the timeout. */
	HANDLE threads = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	HANDLE stalled[64];
	size_t nstalled = 0;
	THREADENTRY32 entry = {0};
	entry.dwSize = sizeof entry;
	if (threads != INVALID_HANDLE_VALUE && Thread32First(threads, &entry)) {
		do {
			if (entry.th32OwnerProcessID == pid && nstalled < 64) {
				HANDLE thread =
				        OpenThread(THREAD_SUSPEND_RESUME, FALSE,
				                entry.th32ThreadID);
				if (thread) {
					if (SuspendThread(thread) !=
					        (DWORD)-1) {
						stalled[nstalled++] = thread;
					} else {
						CloseHandle(thread);
					}
				}
			}
		} while (Thread32Next(threads, &entry));
	}
	if (threads != INVALID_HANDLE_VALUE) {
		CloseHandle(threads);
	}
	CHECK(nstalled != 0);
	if (!nstalled) {
		wf_dechost_stop(h);
		if (proc) {
			CloseHandle(proc);
		}
		CloseHandle(f);
		free(png);
		return;
	}
	CHECK(WF_DSTAT_TIMEOUT ==
	        wf_dechost_decode(h, WF_DKIND_IMAGE, f, 100, 100, 0, NULL, 1,
	                NULL, 0, &pm, NULL));
	CHECK(NULL == pm && !wf_dechost_alive(h));
	/* the kill was CONFIRMED before the call returned */
	CHECK(proc && WAIT_OBJECT_0 == WaitForSingleObject(proc, 0));
	for (size_t i = 0; i < nstalled; i++) {
		if (wf_dechost_alive(h)) {
			ResumeThread(stalled[i]);
		}
		CloseHandle(stalled[i]);
	}
	CHECK(WF_DSTAT_HOST_LOST ==
	        wf_dechost_decode(h, WF_DKIND_IMAGE, f, 100, 100, 0, NULL, 5000,
	                NULL, 0, &pm, NULL));
	wf_dechost_stop(h);
	CHECK(WF_OK == wf_dechost_start(&opt, &second, &err));
	CHECK(WF_DSTAT_OK ==
	        wf_dechost_decode(second, WF_DKIND_IMAGE, f, 100, 100, 0, NULL,
	                20000, NULL, 0, &pm, NULL));
	wf_pixmap_free(pm);
	wf_dechost_stop(second);
	if (proc) {
		CloseHandle(proc);
	}
	CloseHandle(f);
	free(png);
}

static void an_abandoned_request_gets_a_grace_then_the_host_is_killed(void)
{
	WfDecHostOptions opt = {g_host_exe, 0, 0};
	WfDecHost *h = NULL;
	HANDLE f =
	        open_fixture(L"abandon.pdf", pdf_two_page, sizeof pdf_two_page);
	HANDLE abandon = CreateEventW(NULL, TRUE, TRUE, NULL); /* already set */
	WfPixmap *pm = NULL;
	DWORD err;

	CHECK(WF_OK == wf_dechost_start(&opt, &h, &err));
	/* no grace: a PDF page takes longer than nothing, so it is cut off */
	CHECK(WF_DSTAT_ABORTED ==
	        wf_dechost_decode(h, WF_DKIND_PDF, f, 400, 500, 0, NULL, 20000,
	                abandon, 0, &pm, NULL));
	CHECK(NULL == pm && !wf_dechost_alive(h));
	wf_dechost_stop(h);
	/* a generous grace: the answer arrives in time, is returned as usual (the
	 * caller drops it) and the host lives on for the next request */
	CHECK(WF_OK == wf_dechost_start(&opt, &h, &err));
	CHECK(WF_DSTAT_OK ==
	        wf_dechost_decode(h, WF_DKIND_PDF, f, 400, 500, 0, NULL, 20000,
	                abandon, 20000, &pm, NULL));
	CHECK(NULL != pm && wf_dechost_alive(h));
	wf_pixmap_free(pm);
	pm = NULL;
	CHECK(WF_DSTAT_OK ==
	        wf_dechost_decode(h, WF_DKIND_PDF, f, 200, 250, 1, NULL, 20000,
	                NULL, 0, &pm, NULL));
	wf_pixmap_free(pm);
	wf_dechost_stop(h);
	CloseHandle(abandon);
	CloseHandle(f);
}

static void a_host_that_dies_is_noticed_at_once(void)
{
	WfDecHostOptions opt = {g_host_exe, 0, 0};
	WfDecHost *h = NULL;
	size_t png_n;
	void *png = make_png(64, 64, &png_n);
	HANDLE f = open_fixture(L"die.png", png, png_n);
	WfPixmap *pm = NULL;
	DWORD err, t0;
	HANDLE victim;

	CHECK(WF_OK == wf_dechost_start(&opt, &h, &err));
	CHECK(WF_DSTAT_OK ==
	        wf_dechost_decode(h, WF_DKIND_IMAGE, f, 32, 32, 0, NULL, 20000,
	                NULL, 0, &pm, NULL));
	wf_pixmap_free(pm);
	pm = NULL;
	victim = OpenProcess(
	        PROCESS_TERMINATE | SYNCHRONIZE, FALSE, wf_dechost_pid(h));
	CHECK(NULL != victim);
	TerminateProcess(victim, 5);
	WaitForSingleObject(victim, 5000);
	CloseHandle(victim);
	t0 = GetTickCount();
	CHECK(WF_DSTAT_HOST_LOST ==
	        wf_dechost_decode(h, WF_DKIND_IMAGE, f, 32, 32, 0, NULL, 20000,
	                NULL, 0, &pm, NULL));
	CHECK(GetTickCount() - t0 < 3000 &&
	        NULL == pm); /* not the 20 s deadline */
	CHECK(!wf_dechost_alive(h));
	wf_dechost_stop(h);
	CloseHandle(f);
	free(png);
}

static void an_appcontainer_host_decodes_files_it_could_never_open(void)
{
	wchar_t staged[MAX_PATH * 2];
	WfDecHostOptions opt;
	WfDecHost *h = NULL;
	DWORD err = 0;
	WfStatus st;

	st = wf_dechost_stage(g_host_exe, staged, MAX_PATH * 2, &err);
	if (WF_OK != st) {
		printf("SKIP: cannot stage the host (error %lu)\n",
		        (unsigned long)err);
		g_skipped = 1;
		return;
	}
	memset(&opt, 0, sizeof opt);
	opt.exe = staged;
	opt.sandbox = 1;
	st = wf_dechost_start(&opt, &h, &err);
	if (WF_OK != st) {
		printf("SKIP: AppContainer host did not start (error "
		       "0x%08lx)\n",
		        (unsigned long)err);
		g_skipped = 1;
		return;
	}
	/* the fixtures are in a private temp folder: the container has no access
	 * to it, so these only work because the file arrives as a handle */
	exercise_a_host(h, 1);
	wf_dechost_stop(h);
}

/* The time left to a deadline is worked out from one reading of the clock and
 * never wraps: past the deadline (even by one millisecond) it is 0, not the
 * largest number a wait takes (which waits for ever). */
static void the_time_left_never_wraps_into_infinity(void)
{
	CHECK(0 == wf_dechost_remaining_ms(1000, 1000));
	CHECK(0 == wf_dechost_remaining_ms(1000, 1001));
	CHECK(0 == wf_dechost_remaining_ms(1000, 5000000000ull));
	CHECK(1 == wf_dechost_remaining_ms(1001, 1000));
	CHECK(30000 == wf_dechost_remaining_ms(40000, 10000));
	CHECK(INFINITE != wf_dechost_remaining_ms(0x100000000ull + 7, 0));
}

int main(void)
{
	wchar_t *slash;
	the_time_left_never_wraps_into_infinity();

	setvbuf(stdout, NULL, _IONBF, 0);
	GetModuleFileNameW(NULL, g_host_exe, MAX_PATH * 2);
	slash = wcsrchr(g_host_exe, L'\\');
	if (slash) {
		wcscpy_s(slash + 1, 32, L"wf_dechost.exe");
	}
	make_dir();
	the_loop_answers_requests_and_ends_cleanly();
	a_peer_that_breaks_the_protocol_ends_the_host();
	a_file_that_is_not_what_was_described_is_a_reply_not_a_crash();
	a_plain_host_process_decodes_and_stops();
	a_host_that_is_too_slow_is_killed_and_replaced();
	an_abandoned_request_gets_a_grace_then_the_host_is_killed();
	a_host_that_dies_is_noticed_at_once();
	an_appcontainer_host_decodes_files_it_could_never_open();
	remove_dir();
	if (wf_test_failures) {
		return 1;
	}
	if (g_skipped) {
		puts("ok (AppContainer part SKIPPED)");
		return SKIP_CODE;
	}
	puts("ok");
	return 0;
}
