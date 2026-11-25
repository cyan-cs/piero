/* Observe the real worker encoder and its cache independently of filesystem
 * policy and decoder-host availability. Same pattern as test_term_input. */
#include "../src/preview/wf_imageworker.c"
#include "wf_test.h"

int main(void)
{
	WfImageWorker *w = calloc(1, sizeof *w);
	Req r = {0};
	WfImageResult a = {0}, b = {0};
	uint64_t t;
	CHECK(w != NULL);
	InitializeSRWLock(&w->core.lock);
	w->serial = 1;
	r.serial = 1;
	r.sixel = true;
	r.view_w = 400;
	r.view_h = 400;
	CHECK(WF_OK == wf_pixmap_new(800, 600, &a.pm));
	for (size_t i = 0; i < 480000; i++) {
		a.pm->px[i] = 0xff000000u | (uint32_t)(i * 17u & 0xffffffu);
	}
	t = GetTickCount64();
	encode_view(w, &r, &a);
	printf("encode_ms=%llu bytes=%zu cache_bytes=%zu\n",
	        (unsigned long long)(GetTickCount64() - t), a.sixel.len,
	        w->encoded_used);
	CHECK(a.sixel.len && a.sixel_w == 400 && a.sixel_h == 400);
	b.pm = a.pm;
	t = GetTickCount64();
	encode_view(w, &r, &b);
	printf("cache_ms=%llu\n", (unsigned long long)(GetTickCount64() - t));
	CHECK(b.sixel.len == a.sixel.len &&
	        !memcmp(a.sixel.data, b.sixel.data, a.sixel.len));
	CHECK(w->encoded_used <= ENCODE_CACHE_BYTES);
	wf_buf_free(&b.sixel);
	r.pan_x = 300;
	r.pan_y = 100;
	encode_view(w, &r, &b);
	CHECK(b.sixel.len &&
	        (a.sixel.len != b.sixel.len ||
	                memcmp(a.sixel.data, b.sixel.data, a.sixel.len)));
	wf_buf_free(&b.sixel);
	w->serial = 2;
	encode_view(w, &r, &b);
	CHECK(!b.sixel.len); /* obsolete generation never encodes */
	for (size_t i = 0; i < ENCODE_SLOTS; i++) {
		encoded_clear(w, &w->encoded[i]);
	}
	CHECK(!w->encoded_used);
	{
		FileKey key = {0};
		WfDecodeInfo info = {0};
		Req pdf = {0};
		WfPixmap *page = NULL, *crop = NULL;
		WfBuf bytes = {0};
		pdf.kind = WF_DKIND_PDF;
		pdf.session = 99;
		pdf.serial = w->serial;
		pdf.box_w = pdf.box_h = 2;
		w->active_session = 99;
		w->opt.cache_bytes = 4096;
		CHECK(wf_pixmap_new(2, 2, &page) == WF_OK);
		cache_store(w, &key, &pdf, &info, page);
		CHECK(cache_find(w, &key, WF_DKIND_PDF, 0, 2, 2, 99));
		CHECK(!cache_find(w, &key, WF_DKIND_PDF, 0, 2, 2, 100));
		CHECK(wf_pixmap_new(2, 2, &crop) == WF_OK);
		CHECK(wf_buf_puts(&bytes, "private-pixels") == WF_OK);
		encoded_store(w, &pdf, crop, &bytes);
		CHECK(w->encoded_used && w->cache_used);
		pdf_cache_clear(w);
		CHECK(!w->encoded_used && !w->cache_used && !w->pdf_bound);
		wf_buf_free(&bytes);
	}
	{
		wchar_t missing[1024];
		WfImageResult failure = {0};
		CHECK(GetModuleFileNameW(NULL, missing, 1000));
		CHECK(wcscat_s(missing, 1024, L".nonexistent") == 0);
		w->opt.host_exe = missing;
		CHECK(!ensure_host(w, &failure));
		CHECK(failure.unavailable &&
		        failure.status == WF_DSTAT_HOST_LOST);
		CHECK(failure.os_error !=
		        ERROR_SUCCESS); /* preserve actual OS failure */
	}
	w->core.ready = CreateEventW(NULL, TRUE, FALSE, NULL);
	CHECK(w->core.ready != NULL);
	CHECK(WF_OK == wf_pixmap_new(10, 10, &w->result.pm));
	CHECK(WF_OK == wf_buf_puts(&w->result.sixel, "pending"));
	w->has_result = true;
	cancel(w);
	/* Cancellation invalidates immediately; retirement must not erase a
	 * potentially large result on the UI thread. */
	WfImageResult cancelled = {0};
	CHECK(w->result_serial != w->serial && w->purge_pdf);
	CHECK(!wf_imageworker_poll(w, &cancelled));
	CHECK(!cancelled.pm && !cancelled.sixel.data);
	CHECK(WaitForSingleObject(w->core.ready, 0) == WAIT_TIMEOUT);
	/* Same free path as final worker teardown: must tolerate cancellation. */
	wf_pixmap_free(w->result.pm);
	wf_buf_free(&w->result.sixel);
	CloseHandle(w->core.ready);
	wf_pixmap_free(a.pm);
	wf_buf_free(&a.sixel);
	free(w);
	return TEST_RESULT();
}
