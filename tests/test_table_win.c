/*
 * CSV and TSV through the preview worker on real files (DR-DRAFT-027): the same
 * file named .csv or .tsv comes back as columns, named .txt it comes back as it
 * is; a TSV keeps its tabs through the decoder; a Shift-JIS CSV goes through the
 * code page fallback and is still a table; a binary file named .csv is not one.
 * The files are made in the owned test area.
 */
#include <stdlib.h>
#include <string.h>

#include "../tools/p0/p0_lab.h"
#include "preview/wf_previewworker.h"
#include "wf_test.h"

static WfPreview *fetch(WfPreviewWorker *w, uint32_t gen)
{
	WfPreviewResult r = {0};
	DWORD wait = WaitForSingleObject(wf_previewworker_event(w), 5000);

	CHECK(WAIT_OBJECT_0 == wait);
	if (WAIT_OBJECT_0 != wait) {
		return NULL;
	}
	CHECK(wf_previewworker_poll(w, &r));
	CHECK(r.gen == gen);
	return r.preview;
}

static const char *line(const WfPreview *p, size_t i)
{
	return i < p->nlines ? p->text.data + p->lines[i] : "(no line)";
}

static void make(P0Lab *lab,
        const wchar_t *name,
        const void *bytes,
        size_t n,
        wchar_t *path)
{
	HANDLE h;
	DWORD written;

	CHECK(p0_lab_path(lab, name, path, 1024));
	h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	if (INVALID_HANDLE_VALUE != h) {
		CHECK(WriteFile(h, bytes, (DWORD)n, &written, NULL) &&
		        n == written);
		CloseHandle(h);
	}
	CHECK(p0_area_track(lab->area, name, 0));
}

int main(void)
{
	P0Lab lab;
	WfPreviewWorker *w = NULL;
	WfPreviewPort port;
	WfPreview *p;
	wchar_t csv[1024], tsv[1024], txt[1024], sjis[1024], bin[1024];
	const char *csv_text = "id,name\n1,Al\n22,\"B, C\"\n";
	const char *tsv_text = "id\tname\n1\tAl\n22\tB C\n";
	const unsigned char sjis_text[] = {0x96, 0xbc, 0x91, 0x4f, ',', 'x',
	        '\n', 'a', ',', 'b', '\n'}; /* "name" in Shift-JIS, then x */
	const unsigned char binary[] = {0, 1, 2, 3, ',', 0, 5, 6};

	CHECK(p0_lab_open(&lab, "table", 1));
	if (!lab.area) {
		return 1;
	}
	make(&lab, L"data.csv", csv_text, strlen(csv_text), csv);
	make(&lab, L"data.tsv", tsv_text, strlen(tsv_text), tsv);
	make(&lab, L"same.txt", csv_text, strlen(csv_text), txt);
	make(&lab, L"jp.csv", sjis_text, sizeof sjis_text, sjis);
	make(&lab, L"bin.csv", binary, sizeof binary, bin);
	CHECK(WF_OK == wf_previewworker_start(&w, WF_PREVIEW_BYTES, 932));
	if (!w) {
		p0_lab_close(&lab, 0);
		return 1;
	}
	port = wf_previewworker_port(w);

	CHECK(WF_OK == port.submit(port.ctx, 1, csv, 0));
	p = fetch(w, 1);
	CHECK(p && WF_PREVIEW_TEXT == p->kind && 4 == p->nlines);
	if (p) {
		CHECK(!strcmp(line(p, 0), "id | name"));
		CHECK(!strcmp(line(p, 3), "22 | B, C"));
	}
	wf_preview_free(p);

	CHECK(WF_OK == port.submit(port.ctx, 2, tsv, 0));
	p = fetch(w, 2);
	CHECK(p && WF_PREVIEW_TEXT == p->kind && 4 == p->nlines);
	if (p) {
		CHECK(!strcmp(line(p, 0), "id | name"));
		CHECK(!strcmp(line(p, 3), "22 | B C"));
	}
	wf_preview_free(p);

	/* the same bytes under another name are text, not a table */
	CHECK(WF_OK == port.submit(port.ctx, 3, txt, 0));
	p = fetch(w, 3);
	CHECK(p && WF_PREVIEW_TEXT == p->kind && 4 == p->nlines);
	if (p) {
		CHECK(!strcmp(line(p, 0), "id,name"));
		CHECK(!strcmp(line(p, 2), "22,\"B, C\""));
	}
	wf_preview_free(p);

	/* Shift-JIS: the fallback decodes it, and it is still a table */
	CHECK(WF_OK == port.submit(port.ctx, 4, sjis, 0));
	p = fetch(w, 4);
	CHECK(p && WF_PREVIEW_TEXT == p->kind);
	if (p && WF_PREVIEW_TEXT == p->kind) {
		CHECK(!strcmp(p->detail, "CP932 fallback"));
		CHECK(!strcmp(line(p, 0), "\xe5\x90\x8d\xe5\x89\x8d | x"));
		CHECK(!strcmp(line(p, 2), "a    | b"));
	}
	wf_preview_free(p);

	/* not text: no table, the usual verdict */
	CHECK(WF_OK == port.submit(port.ctx, 5, bin, 0));
	p = fetch(w, 5);
	CHECK(p && WF_PREVIEW_BINARY == p->kind);
	wf_preview_free(p);

	port.cancel(port.ctx);
	{
		WfWorkerStopReport report = wf_previewworker_stop(w);

		CHECK(WF_OK == report.status && !report.deferred);
		if (report.completion) {
			CloseHandle(report.completion);
		}
	}
	CHECK(0 == p0_lab_close(&lab, 0 != wf_test_failures));
	return TEST_RESULT();
}
