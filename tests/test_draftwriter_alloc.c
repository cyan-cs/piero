/* Reporting a failed write must not need another path allocation. */
#include <windows.h>
#include <stdlib.h>
#include <wchar.h>

#include "edit/wf_edraft_win.h"
#include "wf_test.h"

static LONG duplicates;
static bool fail_after_submit;

static wchar_t *draft_path_duplicate(const wchar_t *path)
{
	LONG attempt = InterlockedIncrement(&duplicates);

	return fail_after_submit && attempt > 1 ? NULL : _wcsdup(path);
}

static WfStatus failed_draft_write(
        const wchar_t *path, WfEncoding enc, const char *text, size_t n)
{
	(void)path;
	(void)enc;
	(void)text;
	(void)n;
	return WF_E_OS;
}

#define _wcsdup draft_path_duplicate
#define wf_draft_write failed_draft_write
#ifndef WF_DRAFTWRITER_SOURCE
#	define WF_DRAFTWRITER_SOURCE "../src/edit/wf_edraftw_win.c"
#endif
#include WF_DRAFTWRITER_SOURCE
#undef wf_draft_write
#undef _wcsdup

static void failure_path_is_transferred(void)
{
	WfDraftWriter *w = NULL;
	const wchar_t *path = L"C:\\work\\draft.txt";
	WfEncoding enc = {WF_ENC_UTF8, 0};

	CHECK(WF_OK == wf_draftw_start(&w));
	if (!w) {
		return;
	}
	CHECK(WF_E_INVAL == wf_draftw_submit(w, NULL, enc, "text", 4));
	CHECK(WF_E_INVAL == wf_draftw_submit(w, path, enc, NULL, 4));
	CHECK(WF_E_INVAL == wf_draftw_submit(NULL, path, enc, "text", 4));
	fail_after_submit = true;
	CHECK(WF_OK == wf_draftw_submit(w, path, enc, "text", 4));
	wf_draftw_settle(w, path, false);
	CHECK(wf_draftw_take_failure(w, path));
	CHECK(!wf_draftw_take_failure(w, path));
	(void)wf_draftw_stop(w, 5000);
	fail_after_submit = false;
}

static void admission_reserves_failure_capacity(void)
{
	WfDraftWriter *w = NULL;
	wchar_t paths[17][64];
	WfEncoding enc = {WF_ENC_UTF8, 0};

	CHECK(WF_OK == wf_draftw_start(&w));
	if (!w) {
		return;
	}
	for (unsigned i = 0; i < 17; ++i) {
		swprintf(paths[i], 64, L"C:\\work\\draft-%u.txt", i);
		WfStatus st = wf_draftw_submit(w, paths[i], enc, "text", 4);
		CHECK(st == (i < 16 ? WF_OK : WF_E_NOMEM));
		wf_draftw_settle(w, paths[i], false);
	}
	for (unsigned i = 0; i < 16; ++i) {
		CHECK(wf_draftw_take_failure(w, paths[i]));
	}
	CHECK(WF_OK == wf_draftw_submit(w, paths[16], enc, "retry", 5));
	wf_draftw_settle(w, paths[16], false);
	CHECK(wf_draftw_take_failure(w, paths[16]));
	(void)wf_draftw_stop(w, 5000);
}

int main(void)
{
	failure_path_is_transferred();
	admission_reserves_failure_capacity();
	return TEST_RESULT();
}
