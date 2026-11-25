/* Deterministic retirement barrier: no decoder, user files or sleeps. */
#include <windows.h>
#include <stdio.h>
#include "preview/wf_imageworker.h"
#include "wf_test.h"
#include "wf_final_path_fault.h"
#include "ui/wf_app_priv.h"
#include "wf_fake_fs.h"

static SRWLOCK *observed_lock;
static HANDLE erasing, release_erase;
static WfPixmap *blocked_pixels;
static LONG frees, locked_frees;

static void observed_free(WfPixmap *p)
{
	if (p) {
		InterlockedIncrement(&frees);
		bool unlocked = TryAcquireSRWLockExclusive(observed_lock) != 0;
		if (unlocked) {
			ReleaseSRWLockExclusive(observed_lock);
		} else {
			InterlockedIncrement(&locked_frees);
		}
		if (p == blocked_pixels) {
			SetEvent(erasing);
			if (unlocked) {
				CHECK(WaitForSingleObject(release_erase,
				              5000) == WAIT_OBJECT_0);
			}
		}
	}
	wf_pixmap_free(p);
}
#define wf_pixmap_free observed_free
#define GetFinalPathNameByHandleW fault_final_path
#ifndef WF_IMAGEWORKER_SOURCE
#	define WF_IMAGEWORKER_SOURCE "../src/preview/wf_imageworker.c"
#endif
#include WF_IMAGEWORKER_SOURCE
#undef wf_pixmap_free
#undef GetFinalPathNameByHandleW

static void final_path_failures_are_refused(void)
{
	DWORD error = 0;

	final_path_fault = FINAL_PATH_OK;
	CHECK(check_guid_path(INVALID_HANDLE_VALUE, &error) == OPEN_OK);
	final_path_fault = FINAL_PATH_QUERY_FAILED;
	CHECK(check_guid_path(INVALID_HANDLE_VALUE, &error) == OPEN_FAILED);
	CHECK(error == ERROR_ACCESS_DENIED);
	final_path_fault = FINAL_PATH_READ_FAILED;
	CHECK(check_guid_path(INVALID_HANDLE_VALUE, &error) == OPEN_FAILED);
	CHECK(error == ERROR_CRC);
	final_path_fault = FINAL_PATH_GREW;
	CHECK(check_guid_path(INVALID_HANDLE_VALUE, &error) == OPEN_FAILED);
	CHECK(error == ERROR_FILENAME_EXCED_RANGE);
	final_path_fault = FINAL_PATH_REAL;
}

typedef struct
{
	WfImageWorker *worker;
	Req request;
	WfImageResult result;
} Publication;

static DWORD WINAPI publish_thread(void *ctx)
{
	Publication *p = ctx;
	publish(p->worker, &p->request, &p->result);
	return 0;
}

int main(void)
{
	final_path_failures_are_refused();
	WfImageWorker *w = calloc(1, sizeof *w);
	CHECK(w != NULL);
	if (!w) {
		return TEST_RESULT();
	}
	w->core.references = 1;
	InitializeSRWLock(&w->core.lock);
	InitializeConditionVariable(&w->core.wake);
	w->core.ready = CreateEventW(NULL, TRUE, FALSE, NULL);
	w->abandon = CreateEventW(NULL, TRUE, FALSE, NULL);
	erasing = CreateEventW(NULL, TRUE, FALSE, NULL);
	release_erase = CreateEventW(NULL, TRUE, FALSE, NULL);
	CHECK(w->core.ready && w->abandon && erasing && release_erase);
	observed_lock = &w->core.lock;
	CHECK(WF_OK == wf_pixmap_new(1, 1, &w->result.pm));
	w->result.sensitive = true;
	w->has_result = true;
	blocked_pixels = w->result.pm;
	Publication p = {.worker = w};
	CHECK(WF_OK == wf_pixmap_new(1, 1, &p.result.pm));
	p.result.sensitive = true;
	HANDLE thread = CreateThread(NULL, 0, publish_thread, &p, 0, NULL);
	CHECK(thread != NULL);
	CHECK(WaitForSingleObject(erasing, 5000) == WAIT_OBJECT_0);
	LONG before = frees;
	WfImagePort port = wf_imageworker_port(w);
	static WfApp app;
	FakePort fs = {.app = &app};
	CHECK(wf_app_init(&app, 120, 30, fake_port(&fs), L"C:\\Pics") == WF_OK);
	wf_app_set_image(&app, port, WF_IMAGE_HALFBLOCK);
	for (unsigned i = 0; i < 3; i++) {
		WfEvent down = {.type = WF_EV_CHAR, .ch = 'j'};
		(void)app.mode->on_event(&app, &down);
	}
	CHECK(app.image.kind == WF_DKIND_PDF && app.image.state == WF_IMG_BUSY);
	LARGE_INTEGER frequency, begin, end;
	LONGLONG maximum = 0;
	QueryPerformanceFrequency(&frequency);
	for (unsigned i = 0; i < 128; i++) {
		WfEvent key = {.type = WF_EV_CHAR, .ch = i % 2 ? 'j' : 'k'};
		QueryPerformanceCounter(&begin);
		(void)app.mode->on_event(&app, &key);
		QueryPerformanceCounter(&end);
		if (end.QuadPart - begin.QuadPart > maximum) {
			maximum = end.QuadPart - begin.QuadPart;
		}
	}
	printf("128 UI navigation events while retirement blocked: max %.3f "
	       "ms\n",
	        (double)maximum * 1000.0 / (double)frequency.QuadPart);
	wf_app_destroy(&app);
	WfImageRequest rq = {.gen = 1,
	        .path = L"C:\\synthetic.pdf",
	        .kind = WF_DKIND_PDF,
	        .box_w = 100,
	        .box_h = 100,
	        .session = 1};
	QueryPerformanceCounter(&begin);
	for (unsigned i = 0; i < 1000; i++) {
		rq.gen++;
		CHECK(port.submit(port.ctx, &rq) == WF_OK);
		port.cancel(port.ctx);
		WfImageResult result;
		CHECK(!wf_imageworker_poll(w, &result));
	}
	QueryPerformanceCounter(&end);
	/* All calls returned while the worker's destructor is still blocked. */
	CHECK(WaitForSingleObject(thread, 0) == WAIT_TIMEOUT);
	CHECK(frees == before && locked_frees == 0);
	CHECK(!w->has_fg && !w->has_pf && w->has_result);
	CHECK(WaitForSingleObject(w->core.ready, 0) == WAIT_TIMEOUT);
	printf("1000 submit/cancel/poll cycles with retirement blocked: %.3f "
	       "ms\n",
	        (double)(end.QuadPart - begin.QuadPart) * 1000.0 /
	                (double)frequency.QuadPart);
	SetEvent(release_erase);
	CHECK(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
	CloseHandle(thread);
	blocked_pixels = NULL;
	worker_release(w);
	CloseHandle(erasing);
	CloseHandle(release_erase);
	return TEST_RESULT();
}
