/* Worker startup failure must not make the UI flush draft data synchronously. */
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "base/wf_macro.h"
#include "edit/wf_edraft_win.h"
#include "edit/wf_edraftw_win.h"
#include "edit/wf_esession.h"
#include "wf_test.h"

static WfStatus start_error;
static unsigned starts, direct_writes;

static WfStatus draft_start(WfDraftWriter **out)
{
	starts++;
	*out = NULL;
	return start_error == WF_OK ? wf_draftw_start(out) : start_error;
}

static WfStatus direct_draft_write(
        const wchar_t *path, WfEncoding enc, const char *text, size_t n)
{
	(void)path;
	(void)enc;
	(void)text;
	(void)n;
	direct_writes++;
	return WF_OK;
}

#define wf_draftw_start draft_start
#define wf_draft_write direct_draft_write
#ifndef WF_EDIT_IO_SOURCE
#	define WF_EDIT_IO_SOURCE "../src/edit/wf_eio_win.c"
#endif
#include WF_EDIT_IO_SOURCE
#undef wf_draft_write
#undef wf_draftw_start

static void failed_start_keeps_the_error(WfStatus error)
{
	const wchar_t *path = L"C:\\work\\note.txt";
	const WfEncoding utf8 = {WF_ENC_UTF8, 0};
	WfEditIo io = wf_edit_io_system();

	g_drafts_tried = false;
	starts = direct_writes = 0;
	start_error = error;
	CHECK(error == io.draft_write(io.ctx, path, utf8, "text", 4));
	CHECK(error == io.draft_write(io.ctx, path, utf8, "next", 4));
	CHECK(starts == 1 && !direct_writes && !g_drafts);
	wf_edit_io_drafts_stop(5000);
	CHECK(WF_E_INVAL == io.draft_write(io.ctx, path, utf8, "late", 4));
	CHECK(starts == 1 && !direct_writes);
}

static void successful_start_uses_the_worker(void)
{
	wchar_t temp[MAX_PATH], dir[MAX_PATH + 80];
	DWORD length = GetTempPathW(WF_ARRAY_LEN(temp), temp);
	const wchar_t *path = L"C:\\work\\note.txt";
	WfEncoding utf8 = {WF_ENC_UTF8, 0}, back;
	WfEditIo io = wf_edit_io_system();
	char *text = NULL;
	size_t n = 0;
	int written;

	if (!length || length >= WF_ARRAY_LEN(temp)) {
		CHECK_FAIL("cannot locate temporary directory");
		return;
	}
	written = swprintf(dir, WF_ARRAY_LEN(dir), L"%lswf-edit-draft-%lu-%llu",
	        temp, (unsigned long)GetCurrentProcessId(),
	        (unsigned long long)GetTickCount64());
	if (written <= 0 || (size_t)written >= WF_ARRAY_LEN(dir) ||
	        !CreateDirectoryW(dir, NULL)) {
		CHECK_FAIL("cannot create an owned draft fixture");
		return;
	}
	wf_draft_set_dir(dir);
	g_drafts_tried = false;
	starts = direct_writes = 0;
	start_error = WF_OK;
	CHECK(WF_OK == io.draft_write(io.ctx, path, utf8, "text", 4));
	CHECK(starts == 1 && !direct_writes && g_drafts);
	CHECK(WF_OK == io.draft_read(io.ctx, path, &back, &text, &n));
	CHECK(text && n == 4 && !memcmp(text, "text", 4));
	free(text);
	io.draft_remove(io.ctx, path);
	wf_edit_io_drafts_stop(5000);
	wf_draft_set_dir(NULL);
	CHECK(RemoveDirectoryW(dir));
}

int main(void)
{
	/* The regression mutant restores a call to this fallback stub. */
	(void)direct_draft_write;
	failed_start_keeps_the_error(WF_E_NOMEM);
	failed_start_keeps_the_error(WF_E_OS);
	successful_start_uses_the_worker();
	return TEST_RESULT();
}
