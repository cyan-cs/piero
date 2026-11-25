#define COBJMACROS
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wincodec.h>

#include "pdf_samples.h"
#include "wf_imgfix.h"
#include "preview/wf_imageworker.h"
#include "wf_test.h"
#include "../tools/p0/p0_lab.h"

/*
 * The image service with a real decoder host behind it. The host is an
 * ordinary child process here (the AppContainer is covered by
 * test_dechost_win): what is under test is the service's own behaviour.
 */

static wchar_t g_dir[MAX_PATH];
static P0Lab g_lab;
static bool g_fault_first;

/* Milliseconds since the process started, printed unbuffered at each stage so
 * that a run which is killed for being slow still says where it was. */
static ULONGLONG g_t0;
static void stamp(const char *stage)
{
	printf("[+%llu ms] %s\n", (unsigned long long)(GetTickCount64() - g_t0),
	        stage);
}
static wchar_t g_host_exe[MAX_PATH * 2];

static bool make_dir(void)
{
	if (!p0_lab_open(&g_lab, "imageworker", 1)) {
		CHECK_FAIL("could not create a private filesystem fixture");
		return false;
	}
	if (!p0_area_mkdir(g_lab.area, L"images") ||
	        !p0_lab_path(&g_lab, L"images", g_dir, MAX_PATH)) {
		CHECK_FAIL("could not create image fixture directory");
		return false;
	}
	return true;
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
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
				RemoveDirectoryW(one);
			} else {
				DeleteFileW(one);
			}
		} while (FindNextFileW(f, &fd));
		FindClose(f);
	}
	RemoveDirectoryW(g_dir);
}

static void path_of(const wchar_t *name, wchar_t *out)
{
	swprintf(out, MAX_PATH * 2, L"%ls\\%ls", g_dir, name);
}

static void write_file(const wchar_t *name, const void *bytes, size_t n)
{
	wchar_t path[MAX_PATH * 2];
	HANDLE w;
	DWORD put;

	path_of(name, path);
	w = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(INVALID_HANDLE_VALUE != w);
	CHECK(WriteFile(w, bytes, (DWORD)n, &put, NULL) && put == n);
	CloseHandle(w);
}

/* A PNG whose colour depends on `shade`, so two files differ. */
static void *make_png(UINT w, UINT h, unsigned char shade, size_t *n)
{
	IWICImagingFactory *wic = NULL;
	IWICBitmapEncoder *enc = NULL;
	IWICBitmapFrameEncode *fe = NULL;
	IStream *stream = NULL;
	IPropertyBag2 *bag = NULL;
	WICPixelFormatGUID f = GUID_WICPixelFormat32bppBGRA;
	unsigned char *pix = malloc((size_t)w * h * 4);
	void *out;
	HGLOBAL g = NULL;
	STATSTG st;

	for (size_t i = 0; i < (size_t)w * h; i++) {
		pix[i * 4] = shade;
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
	GetHGlobalFromStream(stream, &g);
	IStream_Stat(stream, &st, STATFLAG_NONAME);
	*n = (size_t)st.cbSize.QuadPart;
	out = malloc(*n);
	memcpy(out, GlobalLock(g), *n);
	GlobalUnlock(g);
	IPropertyBag2_Release(bag);
	IWICBitmapFrameEncode_Release(fe);
	IWICBitmapEncoder_Release(enc);
	IStream_Release(stream);
	IWICImagingFactory_Release(wic);
	free(pix);
	return out;
}

static WfImageWorker *start_worker(const wchar_t *host)
{
	WfImageWorkerOptions o;
	WfImageWorker *w = NULL;

	memset(&o, 0, sizeof o);
	o.host_exe = host       ? host
	        : g_fault_first ? L"C:\\no\\such\\host.exe"
	                        : g_host_exe;
	o.sandbox = 0;
	o.timeout_ms = 20000;
	CHECK(WF_OK == wf_imageworker_start(&w, &o));
	return w;
}

static WfImageRequest request(uint32_t gen,
        const wchar_t *path,
        WfDecodeKind kind,
        uint32_t w,
        uint32_t h,
        uint32_t page)
{
	WfImageRequest r;

	memset(&r, 0, sizeof r);
	r.gen = gen;
	r.path = path;
	r.kind = kind;
	r.session = 1;
	r.box_w = w;
	r.box_h = h;
	r.page = page;
	return r;
}

/* Waits for the one answer (polling the event the app waits on). */
static int wait_result(WfImageWorker *w, WfImageResult *out, DWORD ms)
{
	memset(out, 0, sizeof *out);
	if (WAIT_OBJECT_0 != WaitForSingleObject(wf_imageworker_event(w), ms)) {
		return 0;
	}
	return wf_imageworker_poll(w, out) ? 1 : 0;
}

static void a_request_is_answered_and_a_repeat_comes_from_the_cache(void)
{
	stamp("first case: starting the worker");
	WfImageWorker *w = start_worker(NULL);
	stamp("first case: worker started");
	if (!w) {
		return;
	}
	wchar_t path[MAX_PATH * 2];
	size_t n;
	void *png = make_png(300, 200, 10, &n);
	stamp("first case: png made");
	WfImagePort port = wf_imageworker_port(w);
	WfImageRequest rq;
	WfImageResult res = {0};

	write_file(L"a.png", png, n);
	path_of(L"a.png", path);
	stamp("first case: png written");
	rq = request(5, path, WF_DKIND_IMAGE, 150, 150, 0);
	CHECK(WF_OK == port.submit(port.ctx, &rq));
	stamp("first case: request submitted");
	CHECK(wait_result(w, &res, 30000));
	stamp("first case: result (or timeout) returned");
	if (res.status != WF_DSTAT_OK || !res.pm) {
		CHECK_FAIL("decode precondition failed");
		goto done;
	}
	printf("first image result: status=%d refused=%d unavailable=%d "
	       "os_error=%lu\n",
	        (int)res.status, res.refused, res.unavailable, res.os_error);
	CHECK(5 == res.gen && WF_DSTAT_OK == res.status && res.pm &&
	        150 == res.pm->w && 100 == res.pm->h && !res.from_cache);
	CHECK(300 == res.info.src_w && 200 == res.info.src_h);
	wf_pixmap_free(res.pm);
	res.pm = NULL;

	rq = request(6, path, WF_DKIND_IMAGE, 150, 150, 0);
	CHECK(WF_OK == port.submit(port.ctx, &rq));
	CHECK(wait_result(w, &res, 30000));
	if (res.status != WF_DSTAT_OK || !res.pm) {
		CHECK_FAIL("decode precondition failed");
		goto done;
	}
	CHECK(6 == res.gen && WF_DSTAT_OK == res.status && res.from_cache);
	wf_pixmap_free(res.pm);
	res.pm = NULL;
	/* another size is another picture */
	rq = request(7, path, WF_DKIND_IMAGE, 90, 90, 0);
	CHECK(WF_OK == port.submit(port.ctx, &rq));
	CHECK(wait_result(w, &res, 30000));
	if (res.status != WF_DSTAT_OK || !res.pm) {
		CHECK_FAIL("decode precondition failed");
		goto done;
	}
	CHECK(!res.from_cache && 90 == res.pm->w);
	wf_pixmap_free(res.pm);
	res.pm = NULL;
	{
		WfImageWorkerStats s = wf_imageworker_stats(w);

		CHECK(1 == s.cache_hits && 2 == s.cache_misses &&
		        1 == s.host_starts);
	}
done:
	wf_pixmap_free(res.pm);
	stamp("first case: stopping the worker");
	CHECK(0 == wf_imageworker_stop(w).status);
	stamp("first case: worker stopped");
	free(png);
}

static void a_replaced_file_never_hits_the_old_cache_entry(void)
{
	WfImageWorker *w = start_worker(NULL);
	if (!w) {
		return;
	}
	wchar_t path[MAX_PATH * 2];
	size_t n1, n2;
	void *one = make_png(200, 200, 10, &n1);
	void *two = make_png(200, 200, 200, &n2);
	WfImagePort port = wf_imageworker_port(w);
	WfImageRequest rq;
	WfImageResult res = {0};
	uint32_t first_pixel;

	path_of(L"swap.png", path);
	write_file(L"swap.png", one, n1);
	rq = request(1, path, WF_DKIND_IMAGE, 100, 100, 0);
	CHECK(WF_OK == port.submit(port.ctx, &rq));
	CHECK(wait_result(w, &res, 30000) && WF_DSTAT_OK == res.status);
	if (res.status != WF_DSTAT_OK || !res.pm) {
		CHECK_FAIL("decode precondition failed");
		goto done;
	}
	first_pixel = res.pm->px[0];
	wf_pixmap_free(res.pm);
	res.pm = NULL;

	Sleep(30); /* a distinct write time on a coarse clock */
	write_file(L"swap.png", two, n2);
	rq = request(2, path, WF_DKIND_IMAGE, 100, 100, 0);
	CHECK(WF_OK == port.submit(port.ctx, &rq));
	CHECK(wait_result(w, &res, 30000) && WF_DSTAT_OK == res.status);
	if (res.status != WF_DSTAT_OK || !res.pm) {
		CHECK_FAIL("decode precondition failed");
		goto done;
	}
	CHECK(!res.from_cache && res.pm->px[0] != first_pixel);
	wf_pixmap_free(res.pm);
	res.pm = NULL;
done:
	wf_pixmap_free(res.pm);
	wf_imageworker_stop(w);
	free(one);
	free(two);
}

static void only_the_newest_request_is_answered(void)
{
	WfImageWorker *w = start_worker(NULL);
	if (!w) {
		return;
	}
	wchar_t path[MAX_PATH * 2];
	size_t n;
	void *png = make_png(400, 300, 50, &n);
	WfImagePort port = wf_imageworker_port(w);
	WfImageResult res = {0};
	int answers = 0;
	uint32_t last = 0;

	write_file(L"many.png", png, n);
	path_of(L"many.png", path);
	for (uint32_t g = 1; g <= 12; g++) {
		WfImageRequest rq =
		        request(g, path, WF_DKIND_IMAGE, 60 + g, 60 + g, 0);

		CHECK(WF_OK == port.submit(port.ctx, &rq));
	}
	/* collect for a while: whatever arrives, the last one must be among
	 * them and nothing older may arrive after it */
	for (int i = 0; i < 100 && last != 12; i++) {
		if (wait_result(w, &res, 300)) {
			CHECK(res.gen > last);
			last = res.gen;
			answers++;
			wf_pixmap_free(res.pm);
			res.pm = NULL;
		}
	}
	CHECK(12 == last && answers >= 1 && answers <= 12);
	CHECK(!wait_result(w, &res, 200)); /* nothing stale comes later */
	wf_imageworker_stop(w);
	free(png);
}

/* The answer of the last request of a storm is the picture, whichever loop took
 * it: non-zero when it is not. */
static int judge_last(const WfImageResult *res, uint32_t last_gen)
{
	if (res->gen != last_gen) {
		return 0;
	}
	if (WF_DSTAT_OK != res->status || !res->pm) {
		printf("the last answer: status %d\n", (int)res->status);
		return 1;
	}
	return 0;
}

/* A resize storm: a big picture asked for again at a new size every few
 * milliseconds, 60 times, and then the last. Every one but the last may be given
 * up, and the host given up on with it; what must hold is that nothing arrives
 * out of order, the last request is answered with the picture (not with "the
 * decoder stopped"), and the host is still one that decodes. */
static void a_storm_of_requests_for_a_big_picture_ends_with_the_last_answered(void)
{
	WfImageWorker *w = start_worker(NULL);
	wchar_t path[MAX_PATH * 2];
	size_t n;
	uint8_t *png = fix_png_zeros(15360, 8640, 2, 1, &n);
	WfImagePort port;
	WfImageResult res = {0};
	uint32_t last = 0, last_gen = 61;
	int answers = 0, failed_last = 0;

	if (!w) {
		free(png);
		return;
	}
	port = wf_imageworker_port(w);
	write_file(L"storm.png", png, n);
	path_of(L"storm.png", path);
	for (uint32_t g = 1; g <= last_gen; g++) {
		uint32_t side = 40 + (g * 37) % 400;
		WfImageRequest rq = request(g, path, WF_DKIND_IMAGE, side,
		        side / 2 + 5, 0);

		CHECK(WF_OK == port.submit(port.ctx, &rq));
		Sleep(15 + (g % 4) * 15);
		while (wait_result(w, &res, 0)) {
			CHECK(res.gen > last);
			last = res.gen;
			answers++;
			failed_last |= judge_last(&res, last_gen);
			wf_pixmap_free(res.pm);
			res.pm = NULL;
		}
	}
	for (int i = 0; i < 100 && last != last_gen; i++) {
		if (wait_result(w, &res, 300)) {
			CHECK(res.gen > last);
			last = res.gen;
			answers++;
			failed_last |= judge_last(&res, last_gen);
			wf_pixmap_free(res.pm);
			res.pm = NULL;
		}
	}
	CHECK(last_gen == last && !failed_last && answers >= 1);
	CHECK(!wait_result(w, &res, 200));
	wf_imageworker_stop(w);
	free(png);
}

static void a_cancelled_request_delivers_nothing(void)
{
	WfImageWorker *w = start_worker(NULL);
	if (!w) {
		return;
	}
	wchar_t path[MAX_PATH * 2];
	size_t n;
	void *png = make_png(400, 300, 70, &n);
	WfImagePort port = wf_imageworker_port(w);
	WfImageRequest rq;
	WfImageResult res = {0};

	write_file(L"cancel.png", png, n);
	path_of(L"cancel.png", path);
	rq = request(1, path, WF_DKIND_IMAGE, 100, 100, 0);
	CHECK(WF_OK == port.submit(port.ctx, &rq));
	port.cancel(port.ctx);
	CHECK(!wait_result(w, &res, 1500));
	/* and the service is still good afterwards */
	rq = request(2, path, WF_DKIND_IMAGE, 100, 100, 0);
	CHECK(WF_OK == port.submit(port.ctx, &rq));
	CHECK(wait_result(w, &res, 30000) && 2 == res.gen &&
	        WF_DSTAT_OK == res.status);
	if (res.status != WF_DSTAT_OK || !res.pm) {
		CHECK_FAIL("decode precondition failed");
		goto done;
	}
	wf_pixmap_free(res.pm);
	res.pm = NULL;
done:
	wf_pixmap_free(res.pm);
	wf_imageworker_stop(w);
	free(png);
}

static void a_prefetch_warms_the_cache_and_answers_nothing(void)
{
	WfImageWorker *w = start_worker(NULL);
	if (!w) {
		return;
	}
	wchar_t path[MAX_PATH * 2];
	size_t n;
	void *png = make_png(300, 300, 90, &n);
	WfImagePort port = wf_imageworker_port(w);
	WfImageRequest rq;
	WfImageResult res = {0};

	write_file(L"pre.png", png, n);
	path_of(L"pre.png", path);
	rq = request(0, path, WF_DKIND_IMAGE, 120, 120, 0);
	rq.prefetch = true;
	CHECK(WF_OK == port.submit(port.ctx, &rq));
	CHECK(!wait_result(w, &res, 1500)); /* nobody asked for an answer */
	rq = request(3, path, WF_DKIND_IMAGE, 120, 120, 0);
	CHECK(WF_OK == port.submit(port.ctx, &rq));
	CHECK(wait_result(w, &res, 30000) && res.from_cache);
	wf_pixmap_free(res.pm);
	res.pm = NULL;
	wf_imageworker_stop(w);
	free(png);
}

static void pdf_pages_and_their_errors(void)
{
	WfImageWorker *w = start_worker(NULL);
	if (!w) {
		return;
	}
	wchar_t path[MAX_PATH * 2], enc[MAX_PATH * 2];
	WfImagePort port = wf_imageworker_port(w);
	WfImageRequest rq;
	WfImageResult res = {0};

	write_file(L"d.pdf", pdf_two_page, sizeof pdf_two_page);
	write_file(L"e.pdf", pdf_encrypted, sizeof pdf_encrypted);
	path_of(L"d.pdf", path);
	path_of(L"e.pdf", enc);
	rq = request(1, path, WF_DKIND_PDF, 300, 400, 1);
	CHECK(WF_OK == port.submit(port.ctx, &rq));
	CHECK(wait_result(w, &res, 30000) && WF_DSTAT_OK == res.status);
	if (res.status != WF_DSTAT_OK || !res.pm) {
		CHECK_FAIL("decode precondition failed");
		goto done;
	}
	CHECK(2 == res.info.page_count && 1 == res.page && 300 == res.pm->w);
	wf_pixmap_free(res.pm);
	res.pm = NULL;
	rq = request(2, path, WF_DKIND_PDF, 300, 400, 9);
	CHECK(WF_OK == port.submit(port.ctx, &rq));
	CHECK(wait_result(w, &res, 30000) &&
	        WF_DSTAT_PAGE_RANGE == res.status && 2 == res.info.page_count);
	rq = request(3, enc, WF_DKIND_PDF, 300, 400, 0);
	CHECK(WF_OK == port.submit(port.ctx, &rq));
	CHECK(wait_result(w, &res, 30000) &&
	        WF_DSTAT_PASSWORD_REQUIRED == res.status);
	rq = request(4, enc, WF_DKIND_PDF, 300, 400, 0);
	rq.password = L"secret";
	CHECK(WF_OK == port.submit(port.ctx, &rq));
	CHECK(wait_result(w, &res, 30000) && WF_DSTAT_OK == res.status);
	if (res.status != WF_DSTAT_OK || !res.pm) {
		CHECK_FAIL("decode precondition failed");
		goto done;
	}
	wf_pixmap_free(res.pm);
	res.pm = NULL;
	rq = request(5, enc, WF_DKIND_PDF, 200, 300, 0);
	rq.password = L"secret";
	CHECK(WF_OK == port.submit(port.ctx, &rq));
	CHECK(wait_result(w, &res, 30000) && res.status == WF_DSTAT_OK &&
	        res.pm);
	if (!res.pm) {
		goto done;
	}
	wf_pixmap_free(res.pm);
	res.pm = NULL;
	rq.gen++;
	CHECK(WF_OK == port.submit(port.ctx, &rq));
	CHECK(wait_result(w, &res, 30000) && res.status == WF_DSTAT_OK &&
	        res.from_cache && res.pm);
	if (!res.pm) {
		goto done;
	}
	wf_pixmap_free(res.pm);
	res.pm = NULL;
	port.cancel(port.ctx);
	rq.gen++;
	rq.session = 2;
	rq.password = NULL;
	CHECK(WF_OK == port.submit(port.ctx, &rq));
	CHECK(wait_result(w, &res, 30000) &&
	        res.status == WF_DSTAT_PASSWORD_REQUIRED && !res.from_cache &&
	        !res.pm);
	rq.gen++;
	rq.password = L"secret";
	CHECK(WF_OK == port.submit(port.ctx, &rq));
	CHECK(wait_result(w, &res, 30000) && res.status == WF_DSTAT_OK &&
	        res.pm);
	if (!res.pm) {
		goto done;
	}
	wf_pixmap_free(res.pm);
	res.pm = NULL;
	CHECK(DeleteFileW(enc));
	write_file(L"e.pdf", pdf_encrypted, sizeof pdf_encrypted);
	rq.gen++;
	CHECK(WF_OK == port.submit(port.ctx, &rq));
	CHECK(wait_result(w, &res, 30000) &&
	        res.status == WF_DSTAT_PASSWORD_REQUIRED && !res.from_cache &&
	        !res.pm);

done:
	wf_pixmap_free(res.pm);
	wf_imageworker_stop(w);
}

static void files_the_policy_refuses_and_files_that_are_missing(void)
{
	WfImageWorker *w = start_worker(NULL);
	if (!w) {
		return;
	}
	wchar_t path[MAX_PATH * 2], dir[MAX_PATH * 2];
	WfImagePort port = wf_imageworker_port(w);
	WfImageRequest rq;
	WfImageResult res = {0};

	path_of(L"nothing.png", path);
	rq = request(1, path, WF_DKIND_IMAGE, 100, 100, 0);
	CHECK(WF_OK == port.submit(port.ctx, &rq));
	CHECK(wait_result(w, &res, 30000) && WF_DSTAT_OK != res.status);
	CHECK(!res.refused && ERROR_FILE_NOT_FOUND == res.os_error && !res.pm);
	/* a folder is never read: the policy or the system says no */
	path_of(L"folder.png", dir);
	CHECK(CreateDirectoryW(dir, NULL));
	rq = request(2, dir, WF_DKIND_IMAGE, 100, 100, 0);
	CHECK(WF_OK == port.submit(port.ctx, &rq));
	CHECK(wait_result(w, &res, 30000) && WF_DSTAT_OK != res.status);
	CHECK((res.refused || 0 != res.os_error) && !res.pm);
	/* nonsense is refused at the door, not queued */
	rq = request(3, NULL, WF_DKIND_IMAGE, 100, 100, 0);
	CHECK(WF_OK != port.submit(port.ctx, &rq));
	rq = request(3, path, WF_DKIND_IMAGE, 0, 100, 0);
	CHECK(WF_OK != port.submit(port.ctx, &rq));
	rq = request(3, path, WF_DKIND_IMAGE, 9000, 9000, 0);
	CHECK(WF_OK != port.submit(port.ctx, &rq));
	rq = request(3, path, (WfDecodeKind)7, 100, 100, 0);
	CHECK(WF_OK != port.submit(port.ctx, &rq));
	wf_pixmap_free(res.pm);
	wf_imageworker_stop(w);
	RemoveDirectoryW(dir);
}

static void no_host_means_a_reported_reason_not_a_silent_fallback(void)
{
	WfImageWorker *w = start_worker(L"C:\\no\\such\\host.exe");
	if (!w) {
		return;
	}
	wchar_t path[MAX_PATH * 2];
	size_t n;
	void *png = make_png(50, 50, 1, &n);
	WfImagePort port = wf_imageworker_port(w);
	WfImageRequest rq;
	WfImageResult res = {0};

	write_file(L"nohost.png", png, n);
	path_of(L"nohost.png", path);
	rq = request(1, path, WF_DKIND_IMAGE, 50, 50, 0);
	CHECK(WF_OK == port.submit(port.ctx, &rq));
	CHECK(wait_result(w, &res, 30000));
	CHECK(res.unavailable && WF_DSTAT_OK != res.status && !res.pm);
	wf_pixmap_free(res.pm);
	wf_imageworker_stop(w);
	free(png);
}

/* DR-DRAFT-037: the worker is released when idle and made again by the next
 * request. Idle is: nothing in flight, nothing waiting, no answer to poll. A
 * worker made again has no cache and no host, and answers the same. */
static void a_released_worker_is_made_again_and_answers_the_same(void)
{
	wchar_t path[MAX_PATH * 2];
	size_t n, round;
	void *png = make_png(300, 200, 40, &n);
	WfImageResult res;

	write_file(L"idle.png", png, n);
	path_of(L"idle.png", path);
	CHECK(wf_imageworker_idle(NULL));
	for (round = 0; round < 3; round++) {
		WfImageWorker *w = start_worker(NULL);
		WfImagePort port;
		WfImageRequest rq = request((uint32_t)(10 + round), path,
		        WF_DKIND_IMAGE, 150, 150, 0);
		WfImageWorkerStats s;

		if (!w) {
			break;
		}
		port = wf_imageworker_port(w);
		CHECK(wf_imageworker_idle(w)); /* just made: nothing to lose */
		CHECK(WF_OK == port.submit(port.ctx, &rq));
		CHECK(!wf_imageworker_idle(
		        w)); /* asked: not stopped under the request */
		CHECK(wait_result(w, &res, 30000));
		CHECK(res.gen == (uint32_t)(10 + round) &&
		        WF_DSTAT_OK == res.status && res.pm &&
		        !res.from_cache); /* a new worker has no cache */
		wf_pixmap_free(res.pm);
		/* the answer is taken: idle, with the host and the cache still there */
		CHECK(wf_imageworker_idle(w));
		s = wf_imageworker_stats(w);
		CHECK(1 == s.host_starts && 1 == s.cache_misses);
		CHECK(0 == wf_imageworker_stop(w).status);
	}
	/* an answer that waits to be polled keeps the worker */
	{
		WfImageWorker *w = start_worker(NULL);
		WfImagePort port;
		WfImageRequest rq =
		        request(20, path, WF_DKIND_IMAGE, 150, 150, 0);

		if (w) {
			port = wf_imageworker_port(w);
			CHECK(WF_OK == port.submit(port.ctx, &rq));
			CHECK(WAIT_OBJECT_0 ==
			        WaitForSingleObject(
			                wf_imageworker_event(w), 30000));
			CHECK(!wf_imageworker_idle(
			        w)); /* done, but not polled */
			CHECK(wf_imageworker_poll(w, &res));
			wf_pixmap_free(res.pm);
			CHECK(wf_imageworker_idle(w));
			CHECK(0 == wf_imageworker_stop(w).status);
		}
	}
	free(png);
}

static void stopping_with_work_in_flight_is_prompt(void)
{
	WfImageWorker *w = start_worker(NULL);
	if (!w) {
		return;
	}
	wchar_t path[MAX_PATH * 2];
	WfImagePort port = wf_imageworker_port(w);
	WfImageRequest rq;
	DWORD t0;
	WfWorkerStopReport rep;

	write_file(L"stop.pdf", pdf_two_page, sizeof pdf_two_page);
	path_of(L"stop.pdf", path);
	rq = request(1, path, WF_DKIND_PDF, 800, 1000, 0);
	CHECK(WF_OK == port.submit(port.ctx, &rq));
	Sleep(20);
	t0 = GetTickCount();
	rep = wf_imageworker_stop(w);
	CHECK(GetTickCount() - t0 < 4000);
	CHECK(0 == rep.deferred);
}

static uint32_t crc32_of(const unsigned char *b, size_t n)
{
	uint32_t c = 0xFFFFFFFFu;

	for (size_t i = 0; i < n; i++) {
		c ^= b[i];
		for (int k = 0; k < 8; k++) {
			c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
		}
	}
	return ~c;
}

static void put16(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
}

static void put32(unsigned char *p, uint32_t v)
{
	put16(p, v);
	put16(p + 2, v >> 16);
}

/* A ZIP of stored (uncompressed) entries; returns its size. */
static size_t make_zip(unsigned char *out,
        const char *const *names,
        const unsigned char *const *data,
        const size_t *sizes,
        size_t count)
{
	size_t at = 0, cd, cd_start;
	size_t offs[4];

	for (size_t i = 0; i < count; i++) {
		size_t nl = strlen(names[i]);

		offs[i] = at;
		put32(out + at, 0x04034B50u);
		put16(out + at + 4, 10);
		put16(out + at + 6, 0);
		put16(out + at + 8, 0);
		put32(out + at + 10, 0x00210000u);
		put32(out + at + 14, crc32_of(data[i], sizes[i]));
		put32(out + at + 18, (uint32_t)sizes[i]);
		put32(out + at + 22, (uint32_t)sizes[i]);
		put16(out + at + 26, (uint32_t)nl);
		put16(out + at + 28, 0);
		memcpy(out + at + 30, names[i], nl);
		memcpy(out + at + 30 + nl, data[i], sizes[i]);
		at += 30 + nl + sizes[i];
	}
	cd_start = at;
	for (size_t i = 0; i < count; i++) {
		size_t nl = strlen(names[i]);

		put32(out + at, 0x02014B50u);
		put16(out + at + 4, 20);
		put16(out + at + 6, 10);
		put16(out + at + 8, 0);
		put16(out + at + 10, 0);
		put32(out + at + 12, 0x00210000u);
		put32(out + at + 16, crc32_of(data[i], sizes[i]));
		put32(out + at + 20, (uint32_t)sizes[i]);
		put32(out + at + 24, (uint32_t)sizes[i]);
		put16(out + at + 28, (uint32_t)nl);
		memset(out + at + 30, 0, 16);
		put32(out + at + 42, (uint32_t)offs[i]);
		memcpy(out + at + 46, names[i], nl);
		at += 46 + nl;
	}
	cd = at - cd_start;
	put32(out + at, 0x06054B50u);
	memset(out + at + 4, 0, 4);
	put16(out + at + 8, (uint32_t)count);
	put16(out + at + 10, (uint32_t)count);
	put32(out + at + 12, (uint32_t)cd);
	put32(out + at + 16, (uint32_t)cd_start);
	put16(out + at + 20, 0);
	return at + 22;
}

static void pictures_and_pdf_inside_an_archive(void)
{
	WfImageWorker *w = start_worker(NULL);
	if (!w) {
		return;
	}
	WfImagePort port = wf_imageworker_port(w);
	size_t n1, n2, zn;
	void *png1 = make_png(60, 40, 10, &n1);
	void *png2 = make_png(60, 40, 200, &n2);
	const char *names[3] = {"a.png", "b.png", "d.pdf"};
	const unsigned char *data[3] = {png1, png2, pdf_two_page};
	size_t sizes[3] = {n1, n2, sizeof pdf_two_page};
	unsigned char *zip = malloc(n1 + n2 + sizeof pdf_two_page + 1024);
	wchar_t zpath[MAX_PATH * 2], entry[MAX_PATH * 2 + 32];
	WfImageRequest rq;
	WfImageResult res = {0};
	uint32_t first_pixel;

	CHECK(zip);
	zn = make_zip(zip, names, data, sizes, 3);
	write_file(L"pics.zip", zip, zn);
	path_of(L"pics.zip", zpath);
	swprintf(entry, MAX_PATH * 2 + 32, L"%ls|a.png", zpath);
	rq = request(1, entry, WF_DKIND_IMAGE, 60, 40, 0);
	CHECK(WF_OK == port.submit(port.ctx, &rq));
	CHECK(wait_result(w, &res, 30000) && WF_DSTAT_OK == res.status &&
	        res.pm);
	if (res.status != WF_DSTAT_OK || !res.pm) {
		CHECK_FAIL("decode precondition failed");
		goto done;
	}
	CHECK(60 == res.info.src_w && 40 == res.info.src_h && !res.from_cache);
	first_pixel = res.pm->px[0];
	wf_pixmap_free(res.pm);
	res.pm = NULL;
	/* the same entry again: cache */
	rq = request(2, entry, WF_DKIND_IMAGE, 60, 40, 0);
	CHECK(WF_OK == port.submit(port.ctx, &rq));
	CHECK(wait_result(w, &res, 30000) && WF_DSTAT_OK == res.status &&
	        res.from_cache);
	if (res.status != WF_DSTAT_OK || !res.pm) {
		CHECK_FAIL("decode precondition failed");
		goto done;
	}
	wf_pixmap_free(res.pm);
	res.pm = NULL;
	/* another entry of the same archive: not the first one's picture */
	swprintf(entry, MAX_PATH * 2 + 32, L"%ls|b.png", zpath);
	rq = request(3, entry, WF_DKIND_IMAGE, 60, 40, 0);
	CHECK(WF_OK == port.submit(port.ctx, &rq));
	CHECK(wait_result(w, &res, 30000) && WF_DSTAT_OK == res.status &&
	        !res.from_cache && res.pm->px[0] != first_pixel);
	if (res.status != WF_DSTAT_OK || !res.pm) {
		CHECK_FAIL("decode precondition failed");
		goto done;
	}
	wf_pixmap_free(res.pm);
	res.pm = NULL;
	/* a PDF page out of the archive */
	swprintf(entry, MAX_PATH * 2 + 32, L"%ls|d.pdf", zpath);
	rq = request(4, entry, WF_DKIND_PDF, 300, 400, 1);
	CHECK(WF_OK == port.submit(port.ctx, &rq));
	CHECK(wait_result(w, &res, 30000) && WF_DSTAT_OK == res.status);
	if (res.status != WF_DSTAT_OK || !res.pm) {
		CHECK_FAIL("decode precondition failed");
		goto done;
	}
	CHECK(2 == res.info.page_count && 1 == res.page);
	wf_pixmap_free(res.pm);
	res.pm = NULL;
	/* a name that is not in it */
	swprintf(entry, MAX_PATH * 2 + 32, L"%ls|nope.png", zpath);
	rq = request(5, entry, WF_DKIND_IMAGE, 60, 40, 0);
	CHECK(WF_OK == port.submit(port.ctx, &rq));
	CHECK(wait_result(w, &res, 30000) && WF_DSTAT_OK != res.status &&
	        !res.pm);
done:
	wf_pixmap_free(res.pm);
	wf_imageworker_stop(w);
	free(zip);
	free(png1);
	free(png2);
}

int main(int argc, char **argv)
{
	wchar_t *slash;

	g_fault_first = argc > 1 && !strcmp(argv[1], "--fault-first");
	setvbuf(stdout, NULL, _IONBF, 0);
	g_t0 = GetTickCount64();
	stamp("main");
	GetModuleFileNameW(NULL, g_host_exe, MAX_PATH * 2);
	slash = wcsrchr(g_host_exe, L'\\');
	if (slash) {
		wcscpy_s(slash + 1, 32, L"wf_dechost.exe");
	}
	if (!make_dir()) {
		return TEST_RESULT();
	}
	stamp("fixture directory ready");
	a_request_is_answered_and_a_repeat_comes_from_the_cache();
	stamp("first case finished");
	if (wf_test_failures) {
		goto suite_done;
	}
	a_replaced_file_never_hits_the_old_cache_entry();
	only_the_newest_request_is_answered();
	a_storm_of_requests_for_a_big_picture_ends_with_the_last_answered();
	a_cancelled_request_delivers_nothing();
	a_prefetch_warms_the_cache_and_answers_nothing();
	pdf_pages_and_their_errors();
	files_the_policy_refuses_and_files_that_are_missing();
	no_host_means_a_reported_reason_not_a_silent_fallback();
	a_released_worker_is_made_again_and_answers_the_same();
	stopping_with_work_in_flight_is_prompt();
	pictures_and_pdf_inside_an_archive();
suite_done:
	stamp("removing the fixture");
	remove_dir();
	CHECK(0 == p0_lab_close(&g_lab, wf_test_failures != 0));
	stamp("fixture closed");
	return TEST_RESULT();
}
