#include <stdlib.h>
#include <string.h>

#include "../tools/p0/p0_lab.h"
#include "preview/wf_previewworker.h"
#include "wf_test.h"

static WfPreview *fetch(WfPreviewWorker *w, uint32_t gen)
{
	WfPreviewResult r = {0};
	DWORD wait = WaitForSingleObject(wf_previewworker_event(w), 3000);
	CHECK(WAIT_OBJECT_0 == wait);
	if (WAIT_OBJECT_0 != wait) {
		return NULL;
	}
	CHECK(wf_previewworker_poll(w, &r));
	CHECK(r.gen == gen);
	return r.preview;
}

int main(void)
{
	P0Lab lab;
	WfPreviewWorker *w = NULL;
	WfPreviewPort port;
	WfPreview *p;
	wchar_t file[1024], link[1024];
	HANDLE h;
	DWORD written, before, after;
	char *large;
	CHECK(p0_lab_open(&lab, "preview", 1));
	if (!lab.area) {
		return 1;
	}
	CHECK(p0_lab_path(&lab, L"text.txt", file, 1024));
	CHECK(p0_lab_path(&lab, L"link.txt", link, 1024));
	h = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	CHECK(WriteFile(h, "safe\033[31m\ntext", 14, &written, NULL) &&
	        14 == written);
	CloseHandle(h);
	CHECK(p0_area_track(lab.area, L"text.txt", 0));
	/* IsValidCodePage lazily retains Windows' Nls\\CodePage registry key.
	 * Initialize that OS cache before counting worker-owned handles. */
	CHECK(IsValidCodePage(932));
	CHECK(GetProcessHandleCount(GetCurrentProcess(), &before));
	CHECK(WF_OK == wf_previewworker_start(&w, WF_PREVIEW_BYTES, 932));
	if (!w) {
		p0_lab_close(&lab, 0);
		return 1;
	}
	port = wf_previewworker_port(w);
	CHECK(WF_OK == port.submit(port.ctx, 1, file, 0));
	p = fetch(w, 1);
	CHECK(p && WF_PREVIEW_TEXT == p->kind &&
	        !strcmp(p->text.data, "safe^[[31m"));
	wf_preview_free(p);
	h = CreateFileW(
	        file, GENERIC_WRITE, 0, NULL, TRUNCATE_EXISTING, 0, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	CHECK(WriteFile(h, "\x82\xa0", 2, &written, NULL) && 2 == written);
	CloseHandle(h);
	CHECK(WF_OK == port.submit(port.ctx, 101, file, 0));
	p = fetch(w, 101);
	CHECK(p && WF_PREVIEW_TEXT == p->kind &&
	        !strcmp(p->text.data, "\xe3\x81\x82"));
	wf_preview_free(p);
	/* No-follow applies even to an explicit request. */
	if (CreateSymbolicLinkW(
	            link, file, SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
		CHECK(p0_area_track(lab.area, L"link.txt", 0));
		CHECK(WF_OK == port.submit(port.ctx, 2, link, 1));
		p = fetch(w, 2);
		CHECK(p && WF_PREVIEW_REFUSED == p->kind);
		wf_preview_free(p);
	} else {
		puts("SKIP symlink: privilege unavailable (no coverage "
		     "claimed)");
	}
	large = malloc(WF_PREVIEW_BYTES + 4u);
	CHECK(NULL != large);
	if (large) {
		memset(large, 'z', WF_PREVIEW_BYTES + 4u);
		h = CreateFileW(file, GENERIC_WRITE, 0, NULL, TRUNCATE_EXISTING,
		        0, NULL);
		CHECK(INVALID_HANDLE_VALUE != h);
		CHECK(WriteFile(h, large, WF_PREVIEW_BYTES + 4u, &written,
		              NULL) &&
		        written == WF_PREVIEW_BYTES + 4u);
		CloseHandle(h);
		free(large);
		CHECK(WF_OK == port.submit(port.ctx, 3, file, 0));
		p = fetch(w, 3);
		CHECK(p && WF_PREVIEW_TEXT == p->kind && p->truncated);
		wf_preview_free(p);
	}
	/* A Shift-JIS file cut by the read limit in the middle of a two-byte
	 * character: the text before it is shown, as for a file that ends one byte
	 * earlier (the lead byte of "a" is 0x82, its end 0xA0). */
	for (int cut = 0; cut < 2; cut++) {
		size_t size = WF_PREVIEW_BYTES + 4u;
		unsigned char *sj = malloc(size);

		CHECK(NULL != sj);
		if (!sj) {
			break;
		}
		memset(sj, 'z', size);
		sj[WF_PREVIEW_BYTES - 1u - (size_t)cut] = 0x82;
		sj[WF_PREVIEW_BYTES - (size_t)cut] = 0xA0;
		h = CreateFileW(file, GENERIC_WRITE, 0, NULL, TRUNCATE_EXISTING,
		        0, NULL);
		CHECK(INVALID_HANDLE_VALUE != h);
		CHECK(WriteFile(h, sj, (DWORD)size, &written, NULL) &&
		        written == size);
		CloseHandle(h);
		free(sj);
		CHECK(WF_OK ==
		        port.submit(port.ctx, 200 + (uint32_t)cut, file, 0));
		p = fetch(w, 200 + (uint32_t)cut);
		CHECK(p && WF_PREVIEW_TEXT == p->kind && p->truncated);
		wf_preview_free(p);
	}
	CHECK(WF_OK == port.submit(port.ctx, 4, L"\\\\server\\share\\file", 0));
	p = fetch(w, 4);
	CHECK(p && WF_PREVIEW_REFUSED == p->kind);
	wf_preview_free(p);
	/* Many superseding requests must leave only the current generation
	 * visible. */
	for (uint32_t i = 5; i < 100; i++) {
		CHECK(WF_OK == port.submit(port.ctx, i, file, 0));
	}
	p = fetch(w, 99);
	CHECK(p && WF_PREVIEW_TEXT == p->kind);
	wf_preview_free(p);
	port.cancel(port.ctx);
	{
		WfWorkerStopReport report = wf_previewworker_stop(w);
		CHECK(WF_OK == report.status && !report.deferred);
		if (report.completion) {
			CloseHandle(report.completion);
		}
	}
	CHECK(GetProcessHandleCount(GetCurrentProcess(), &after));
	printf("preview normal handles=%lu -> %lu\n", before, after);
	CHECK(before == after);
	/* Repeat real acquisition, cancellation and teardown, including the
	 * file/ancestor pins. Each turn must return to exactly the baseline. */
	h = CreateFileW(
	        file, GENERIC_WRITE, 0, NULL, TRUNCATE_EXISTING, 0, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	if (INVALID_HANDLE_VALUE != h) {
		CHECK(WriteFile(h, "sample", 6, &written, NULL) &&
		        6 == written);
		CloseHandle(h);
	}
	for (unsigned cycle = 0; cycle < 100; cycle++) {
		CHECK(WF_OK ==
		        wf_previewworker_start(&w, WF_PREVIEW_BYTES, 932));
		if (!w) {
			break;
		}
		port = wf_previewworker_port(w);
		CHECK(WF_OK == port.submit(port.ctx, 1, file, false));
		port.cancel(port.ctx);
		CHECK(WF_OK == port.submit(port.ctx, 2, file, false));
		p = fetch(w, 2);
		CHECK(p && WF_PREVIEW_TEXT == p->kind);
		wf_preview_free(p);
		WfWorkerStopReport report = wf_previewworker_stop(w);
		CHECK(WF_OK == report.status && !report.deferred);
		if (report.completion) {
			CHECK(WAIT_OBJECT_0 ==
			        WaitForSingleObject(report.completion, 3000));
			CloseHandle(report.completion);
		}
		CHECK(GetProcessHandleCount(GetCurrentProcess(), &after));
		if (after != before) {
			printf("preview normal cycle=%u handles=%lu -> %lu\n",
			        cycle, before, after);
		}
		CHECK(after == before);
	}
	CHECK(0 == p0_lab_close(&lab, 0 != wf_test_failures));
	return TEST_RESULT();
}
