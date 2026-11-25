/*
 * Archives through the listing worker, as the app uses them: the answer for a
 * path inside an archive carries the generation it was asked with, the
 * newest request is the one answered, and an archive that cannot be read is
 * reported by status, not by an empty folder.
 */
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "archive/wf_archive.h"
#include "fs/wf_fsworker.h"
#include "wf_test.h"

#ifndef WF_TEST_ARCHIVE_DIR
#	error WF_TEST_ARCHIVE_DIR must name tests/archive
#endif

static wchar_t fixtures[1024];

static bool wait_result(WfFsWorker *w, WfFsResult *r, uint32_t gen)
{
	ULONGLONG deadline = GetTickCount64() + 20000;

	while (GetTickCount64() < deadline) {
		if (wf_fsworker_poll(w, r)) {
			if (r->gen == gen) {
				return true;
			}
			wf_dir_free(r->dir);
			wf_dir_free(r->parent);
			continue;
		}
		WaitForSingleObject(wf_fsworker_event(w), 100);
	}
	return false;
}

static bool has(const WfDir *d, const wchar_t *name, uint32_t flag)
{
	size_t i;

	for (i = 0; d && i < d->n; i++) {
		if (!wcscmp(d->e[i].name, name)) {
			return flag ? 0 != (d->e[i].flags & flag) : true;
		}
	}
	return false;
}

int main(void)
{
	WfFsWorker *w = NULL;
	WfFsPort port;
	WfFsResult r;
	wchar_t zip[1200], inner[1200], sevenz[1200], broken[1200],
	        nothing[1200];
	size_t i;

	MultiByteToWideChar(
	        CP_UTF8, 0, WF_TEST_ARCHIVE_DIR, -1, fixtures, 1024);
	for (i = 0; fixtures[i]; i++) {
		if (L'/' == fixtures[i]) {
			fixtures[i] = L'\\';
		}
	}
	swprintf(zip, 1200, L"%ls\\basic.zip|", fixtures);
	swprintf(inner, 1200, L"%ls\\basic.zip|\\src", fixtures);
	swprintf(sevenz, 1200, L"%ls\\basic.7z|", fixtures);
	swprintf(broken, 1200, L"%ls\\links.tar|\\real.txt", fixtures);
	swprintf(nothing, 1200, L"%ls\\missing.zip|", fixtures);
	wf_archive_set_codepage(932);
	CHECK(WF_OK == wf_fsworker_start(&w));
	port = wf_fsworker_port(w);

	/* the top of an archive, with the real folder beside it */
	CHECK(WF_OK == port.submit(port.ctx, 1, zip, fixtures));
	CHECK(wait_result(w, &r, 1));
	CHECK(WF_OK == r.st && r.dir && 4 == r.dir->n);
	CHECK(has(r.dir, L"README.md", 0) && has(r.dir, L"src", WF_ENT_DIR));
	CHECK(r.parent && has(r.parent, L"basic.zip", WF_ENT_ARCHIVE) &&
	        has(r.parent, L"single.txt.gz", WF_ENT_ARCHIVE));
	wf_dir_free(r.dir);
	wf_dir_free(r.parent);

	/* a folder inside it, its parent being the archive's top */
	CHECK(WF_OK == port.submit(port.ctx, 2, inner, zip));
	CHECK(wait_result(w, &r, 2));
	CHECK(WF_OK == r.st && r.dir && 2 == r.dir->n &&
	        has(r.dir, L"main.c", 0));
	CHECK(r.parent && has(r.parent, L"src", WF_ENT_DIR));
	CHECK(!has(r.dir, L"main.c", WF_ENT_ARCHIVE));
	wf_dir_free(r.dir);
	wf_dir_free(r.parent);

	/* two requests back to back: the newest is answered */
	CHECK(WF_OK == port.submit(port.ctx, 3, zip, fixtures));
	CHECK(WF_OK == port.submit(port.ctx, 4, sevenz, fixtures));
	CHECK(wait_result(w, &r, 4));
	CHECK(WF_OK == r.st && r.dir && 4 == r.dir->n);
	wf_dir_free(r.dir);
	wf_dir_free(r.parent);

	/* what is not a folder, and what is not there */
	CHECK(WF_OK == port.submit(port.ctx, 5, broken, NULL));
	CHECK(wait_result(w, &r, 5));
	CHECK(WF_E_NOENT == r.st && !r.dir); /* a file is not a folder */
	CHECK(WF_OK == port.submit(port.ctx, 6, nothing, NULL));
	CHECK(wait_result(w, &r, 6));
	CHECK(WF_E_NOENT == r.st && !r.dir);

	CHECK(WF_OK == wf_fsworker_stop(w).status);
	wf_archive_cache_clear();
	return TEST_RESULT();
}
