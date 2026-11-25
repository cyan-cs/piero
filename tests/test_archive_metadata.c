/* Real index reads with missing first-pass names force the code-page retry.
 * Reader status/name delivery, failed opens and source allocations are intercepted. */
#include <windows.h>
#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "wf_test.h"

static size_t owned;
static unsigned readers;
static bool failed_headers;
static bool failed_open, external_path_free;
static unsigned open_calls;
static enum
{
	RETRY_OK,
	RETRY_NOMEM,
	RETRY_CANCEL
} retry_mode;

static void *tracked_malloc(size_t bytes)
{
	void *p = malloc(bytes);
	owned += p != NULL;
	return p;
}

static void *tracked_calloc(size_t n, size_t bytes)
{
	void *p = calloc(n, bytes);
	owned += p != NULL;
	return p;
}

static void *tracked_realloc(void *old, size_t bytes)
{
	bool was_null = !old;
	void *p = realloc(old, bytes);
	owned += p && was_null;
	return p;
}

static void tracked_free(void *p)
{
	/* open_file's path copy comes from the linked path module, outside tracking. */
	if (p && external_path_free) {
		external_path_free = false;
		free(p);
		SetLastError(ERROR_FILE_NOT_FOUND);
		return;
	}
	if (p) {
		CHECK(owned > 0);
		owned--;
	}
	free(p);
}

static HANDLE test_create_file(LPCWSTR path,
        DWORD access,
        DWORD share,
        LPSECURITY_ATTRIBUTES attributes,
        DWORD creation,
        DWORD flags,
        HANDLE template_file)
{
	if (failed_open) {
		open_calls++;
		external_path_free = true;
		SetLastError(ERROR_ACCESS_DENIED);
		return INVALID_HANDLE_VALUE;
	}
	return CreateFileW(path, access, share, attributes, creation, flags,
	        template_file);
}

static struct archive *test_reader_new(void)
{
	readers++;
	return readers == 2 && retry_mode == RETRY_NOMEM ? NULL
	                                                 : archive_read_new();
}

static const char *test_pathname_utf8(struct archive_entry *entry)
{
	return readers == 1 ? NULL : archive_entry_pathname_utf8(entry);
}

static const char *test_pathname(struct archive_entry *entry)
{
	return readers == 1 ? NULL : archive_entry_pathname(entry);
}

static int test_next_header(struct archive *ar, struct archive_entry **entry)
{
	int status = archive_read_next_header(ar, entry);

	if (failed_headers &&
	        (status == ARCHIVE_OK || status == ARCHIVE_WARN)) {
		archive_set_error(ar, EIO, "header failed");
		return ARCHIVE_FAILED;
	}
	return status;
}

#define malloc tracked_malloc
#define calloc tracked_calloc
#define realloc tracked_realloc
#define free tracked_free
#define archive_read_new test_reader_new
#define archive_entry_pathname_utf8 test_pathname_utf8
#define archive_entry_pathname test_pathname
#define archive_read_next_header test_next_header
#define CreateFileW test_create_file
#ifndef WF_ARCHIVE_SOURCE
#	define WF_ARCHIVE_SOURCE "../src/archive/wf_archive.c"
#endif
#include WF_ARCHIVE_SOURCE
#undef CreateFileW
#undef archive_read_next_header
#undef archive_entry_pathname
#undef archive_entry_pathname_utf8
#undef archive_read_new
#undef free
#undef realloc
#undef calloc
#undef malloc

static bool cancel_retry(void *ctx)
{
	(void)ctx;
	return readers == 2 && retry_mode == RETRY_CANCEL;
}

static void retries_preserve_errors_and_release_the_first_index(void)
{
	HANDLE h =
	        CreateFileW(WF_TEST_ARCHIVE_FILE, GENERIC_READ, FILE_SHARE_READ,
	                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	Stamp stamp;
	WfArchiveCtl ctl = {cancel_retry, NULL};

	CHECK(h != INVALID_HANDLE_VALUE);
	if (h == INVALID_HANDLE_VALUE) {
		return;
	}
	CHECK(stamp_of(h, &stamp));
	wf_archive_set_codepage(932);
	for (int mode = RETRY_OK; mode <= RETRY_CANCEL; mode++) {
		WfArchive *a = NULL;
		LARGE_INTEGER start = {0};
		WfStatus expected = mode == RETRY_OK ? WF_OK
		        : mode == RETRY_NOMEM        ? WF_E_NOMEM
		                                     : WF_E_CANCELLED;

		readers = 0;
		retry_mode = mode;
		CHECK(SetFilePointerEx(h, start, NULL, FILE_BEGIN));
		CHECK(expected ==
		        build_index(WF_TEST_ARCHIVE_FILE, h, &stamp, &ctl, &a));
		CHECK(readers == 2);
		if (mode == RETRY_OK) {
			CHECK(a && a->info.entries > 0);
			if (a) {
				CHECK(a->info.bytes >=
				        a->cap * sizeof *a->nodes +
				                a->pool_cap * sizeof *a->pool +
				                a->hash_cap * sizeof *a->hash);
			}
		} else {
			CHECK(!a);
		}
		index_free(a);
		CHECK(!owned);
	}
	{
		WfArchive *a = NULL;
		LARGE_INTEGER start = {0};

		readers = 0;
		retry_mode = RETRY_OK;
		failed_headers = true;
		CHECK(SetFilePointerEx(h, start, NULL, FILE_BEGIN));
		CHECK(WF_E_CORRUPT ==
		        build_index(WF_TEST_ARCHIVE_FILE, h, &stamp, &ctl, &a));
		CHECK(!a);
		index_free(a);
		CHECK(!owned);
		failed_headers = false;
	}
	wf_archive_set_codepage(0);
	CloseHandle(h);
}

static void timestamp_bounds(void)
{
	struct archive_entry *e = archive_entry_new();

	CHECK(e);
	if (!e) {
		return;
	}
	archive_entry_set_mtime(e, 0, 0);
	CHECK(to_filetime(e) == 116444736000000000ull);
	archive_entry_set_mtime(e, -11644473600ll, 0);
	CHECK(to_filetime(e) == 0);
	archive_entry_set_mtime(e, 910692730085ll, 477580700);
	CHECK(to_filetime(e) == 9223372036854775807ull);
	archive_entry_set_mtime(e, 910692730085ll, 477580800);
	CHECK(to_filetime(e) == 0);
	archive_entry_set_mtime(e, 910692730085ll, 999999900);
	CHECK(to_filetime(e) == 0);
	archive_entry_free(e);
}

int main(void)
{
	struct archive *ar = archive_read_new();
	Source src = {0};
	HANDLE opened = NULL;
	Stamp stamp;

	failed_open = true;
	CHECK(open_file(L"C:\\archive.zip", &opened, &stamp) == WF_E_ACCESS);
	CHECK(open_calls == 1 && opened == INVALID_HANDLE_VALUE &&
	        !external_path_free && !owned);
	failed_open = false;

	CHECK(ar);
	if (ar) {
		archive_set_error(
		        ar, ENOMEM, "Unsupported format: allocation failed");
		CHECK(archive_failure(ar, &src) == WF_E_NOMEM);
		src.h = INVALID_HANDLE_VALUE;
		const void *bytes = NULL;
		CHECK(src_read(ar, &src, &bytes) < 0);
		CHECK(archive_failure(ar, &src) == WF_E_OS);
		memset(&src, 0, sizeof src);
		src.h = INVALID_HANDLE_VALUE;
		CHECK(src_seek(ar, &src, 0, SEEK_SET) < 0);
		CHECK(archive_failure(ar, &src) == WF_E_OS);
		src.io_error = ERROR_ACCESS_DENIED;
		CHECK(archive_failure(ar, &src) == WF_E_ACCESS);
		src.io_error = 0;
		archive_set_error(ar, EIO, "damaged entry");
		CHECK(archive_failure(ar, &src) == WF_E_CORRUPT);
		archive_read_free(ar);
	}
	timestamp_bounds();
	retries_preserve_errors_and_release_the_first_index();
	return TEST_RESULT();
}
