/*
 * Extracting from a ZIP onto the real file system (Windows only), in a scratch
 * area. What this pins down: the items that were chosen arrive under their own
 * names with what is below them, the collision policies and the merge of a
 * folder that is there work as for any copy, a name that is not safe or an entry
 * whose data is wrong is left out and reported while the rest goes on, nothing
 * the archive names is ever written outside the destination, and a cancelled or
 * failed job leaves neither staging nor partial files behind.
 */
#include <windows.h>

#include <stdio.h>
#include <string.h>
#ifdef _DEBUG
#	include <crtdbg.h>
#endif

#include "../tools/p0/p0_lab.h"
#include "archive/wf_archive.h"
#include "base/wf_macro.h"
#include "wf_test.h"
#include "wf_zipwriter.h"
#include "xfer/wf_copyengine_win.h"
#include "xfer/wf_extract_win.h"
#include "xfer/wf_probe_win.h"

static P0Lab lab;
static wchar_t root[1024], zip_path[1100], dst_dir[1024];
static WfProbeOps ops;
static wchar_t made_zip[40][1100]; /* the archives this test wrote */
static size_t n_zip;

static void path_in(const wchar_t *base, const wchar_t *rel, wchar_t *out)
{
	swprintf(out, 1100, L"%ls\\%ls", base, rel);
}

static bool exists(const wchar_t *p)
{
	return INVALID_FILE_ATTRIBUTES != GetFileAttributesW(p);
}

static bool put(const wchar_t *path, const void *data, size_t n)
{
	HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD w = 0;
	bool ok;

	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	ok = !n || (WriteFile(h, data, (DWORD)n, &w, NULL) && w == n);
	CloseHandle(h);
	return ok;
}

static bool holds(const wchar_t *path, const char *text)
{
	char got[64] = "";
	DWORD n = 0;
	HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, 0, NULL);

	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	ReadFile(h, got, 63, &n, NULL);
	CloseHandle(h);
	return n == strlen(text) && !memcmp(got, text, n);
}

/* Entries of `dir` whose names start with ".wfextract-" or ".wfcopy-". */
static size_t leftovers(const wchar_t *dir)
{
	static const wchar_t *const pat[] = {L"\\.wfextract-*", L"\\.wfcopy-*"};
	size_t n = 0;

	for (size_t i = 0; i < WF_ARRAY_LEN(pat); i++) {
		wchar_t p[1100];
		WIN32_FIND_DATAW fd;
		HANDLE f;

		swprintf(p, 1100, L"%ls%ls", dir, pat[i]);
		f = FindFirstFileW(p, &fd);
		if (INVALID_HANDLE_VALUE == f) {
			continue;
		}
		do {
			n++;
		} while (FindNextFileW(f, &fd));
		FindClose(f);
	}
	return n;
}

static size_t entries_in(const wchar_t *dir)
{
	wchar_t p[1100];
	WIN32_FIND_DATAW fd;
	HANDLE f;
	size_t n = 0;

	swprintf(p, 1100, L"%ls\\*", dir);
	f = FindFirstFileW(p, &fd);
	if (INVALID_HANDLE_VALUE == f) {
		return 0;
	}
	do {
		n += wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..");
	} while (FindNextFileW(f, &fd));
	FindClose(f);
	return n;
}

static void purge(const wchar_t *dir)
{
	wchar_t p[1100], c[1100];
	WIN32_FIND_DATAW fd;
	HANDLE f;

	if (wcsncmp(dir, root, wcslen(root))) {
		return;
	}
	swprintf(p, 1100, L"%ls\\*", dir);
	f = FindFirstFileW(p, &fd);
	if (INVALID_HANDLE_VALUE == f) {
		return;
	}
	do {
		if (!wcscmp(fd.cFileName, L".") ||
		        !wcscmp(fd.cFileName, L"..")) {
			continue;
		}
		swprintf(c, 1100, L"%ls\\%ls", dir, fd.cFileName);
		SetFileAttributesW(c, FILE_ATTRIBUTE_NORMAL);
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			purge(c);
			RemoveDirectoryW(c);
		} else {
			DeleteFileW(c);
		}
	} while (FindNextFileW(f, &fd));
	FindClose(f);
}

static void fresh(void)
{
	static unsigned n;
	wchar_t rel[32];

	purge(dst_dir);
	n++;
	swprintf(rel, 32, L"d%u", n);
	CHECK(p0_area_mkdir(lab.area, rel));
	swprintf(dst_dir, 1024, L"%ls\\%ls", root, rel);
}

static bool save_zip(ZipOut *z, const wchar_t *name)
{
	uint8_t *data;
	size_t n;
	bool ok;

	zip_finish(z, &data, &n);
	path_in(root, name, zip_path);
	wcscpy_s(made_zip[n_zip++], 1100, zip_path);
	ok = put(zip_path, data, n);
	free(data);
	return ok;
}

static WfCopySpec spec_for(
        const wchar_t *const *inner, size_t n, WfCollide collide)
{
	WfCopySpec s;
	WfNodeInfo d;

	memset(&s, 0, sizeof s);
	wf_slot_init(&s.slot);
	s.kind = WF_CJ_EXTRACT;
	s.archive = _wcsdup(zip_path);
	s.inner = calloc(n ? n : 1, sizeof *s.inner);
	for (size_t i = 0; i < n; i++) {
		s.inner[i] = _wcsdup(inner[i]);
	}
	s.n_inner = n;
	s.dest = _wcsdup(dst_dir);
	CHECK(WF_OK == ops.node(NULL, dst_dir, &d));
	s.dest_id = d.id;
	s.verify = WF_VERIFY_SHA256;
	s.collide = collide;
	return s;
}

static WfCopyResult extract(const wchar_t *const *inner,
        size_t n,
        WfCollide collide,
        const WfCopyControl *ctl)
{
	WfCopySpec s = spec_for(inner, n, collide);
	WfCopyResult r;

	CHECK(WF_OK == wf_extract_run(&s, ctl, NULL, &r));
	for (size_t i = 0; i < r.failure_total && i < WF_CJ_FAILURES; i++) {
		printf("  failure: %ls: %s (%lu)\n", r.failures[i].name,
		        wf_copyfail_text(r.failures[i].kind),
		        r.failures[i].error);
	}
	wf_copyspec_free(&s);
	return r;
}

static bool failed_with(
        const WfCopyResult *r, WfCopyFailKind k, const wchar_t *name)
{
	for (size_t i = 0; i < r->failure_total && i < WF_CJ_FAILURES; i++) {
		if (r->failures[i].kind == k &&
		        !wcscmp(r->failures[i].name, name)) {
			return true;
		}
	}
	return false;
}

static void a_chosen_folder_arrives_under_its_name_and_is_registered_for_undo(
        void)
{
	ZipOut z;
	const wchar_t *one[] = {L"a\\b"};
	wchar_t p[1100];
	WfCopyResult r;

	memset(&z, 0, sizeof z);
	zip_add_s(&z, "top.txt", "top");
	zip_add_s(&z, "a/b/c.txt", "ccc");
	zip_add_s(&z, "a/b/sub/d.txt", "ddd");
	zip_add_s(&z, "a/e.txt", "eee");
	CHECK(save_zip(&z, L"sel.zip"));
	fresh();
	r = extract(one, 1, WF_COLLIDE_STOP, NULL);
	CHECK(WF_CS_DONE == r.state && 0 == r.failure_total);
	CHECK(WF_CJ_COPY == r.kind && 2 == r.files_copied);
	path_in(dst_dir, L"b\\c.txt", p);
	CHECK(holds(p, "ccc"));
	path_in(dst_dir, L"b\\sub\\d.txt", p);
	CHECK(holds(p, "ddd"));
	path_in(dst_dir, L"top.txt", p);
	CHECK(!exists(p));
	path_in(dst_dir, L"e.txt", p);
	CHECK(!exists(p));
	CHECK(0 == leftovers(dst_dir) && 1 == entries_in(dst_dir));
	/* what was made is registered for Undo, with the folder it is in */
	CHECK(1 == r.created_n && r.created[0].dir &&
	        (r.created[0].id.kind == WF_ID_STRONG) &&
	        (r.created[0].parent.kind == WF_ID_STRONG));
	wf_copyresult_free(&r);
	/* the archive itself was only read */
	CHECK(exists(zip_path));
}

static void a_folder_that_is_there_is_merged_and_the_policy_decides_files(void)
{
	ZipOut z;
	const wchar_t *one[] = {L"b"};
	wchar_t p[1100], dir[1100];
	WfCopyResult r;

	memset(&z, 0, sizeof z);
	zip_add_s(&z, "b/c.txt", "NEW c");
	zip_add_s(&z, "b/d.txt", "NEW d");
	CHECK(save_zip(&z, L"merge.zip"));

	for (int k = 0; k < 4; k++) {
		static const WfCollide pol[] = {WF_COLLIDE_SKIP,
		        WF_COLLIDE_OVERWRITE, WF_COLLIDE_RENAME,
		        WF_COLLIDE_STOP};

		fresh();
		path_in(dst_dir, L"b", dir);
		CHECK(CreateDirectoryW(dir, NULL));
		path_in(dst_dir, L"b\\c.txt", p);
		CHECK(put(p, "OLD c", 5));
		path_in(dst_dir, L"b\\keep.txt", p);
		CHECK(put(p, "keep", 4));
		r = extract(one, 1, pol[k], NULL);
		path_in(dst_dir, L"b\\keep.txt", p);
		CHECK(holds(p, "keep")); /* nothing that was there is lost */
		path_in(dst_dir, L"b\\d.txt", p);
		/* under stop a folder that is there is not entered at all */
		CHECK(WF_COLLIDE_STOP == pol[k] ? !exists(p)
		                                : holds(p, "NEW d"));
		path_in(dst_dir, L"b\\c.txt", p);
		if (WF_COLLIDE_OVERWRITE == pol[k]) {
			CHECK(holds(p, "NEW c") && 1 == r.overwritten);
		} else {
			CHECK(holds(p, "OLD c"));
		}
		if (WF_COLLIDE_RENAME == pol[k]) {
			path_in(dst_dir, L"b\\c (2).txt", p);
			CHECK(holds(p, "NEW c"));
		}
		if (WF_COLLIDE_STOP == pol[k]) {
			CHECK(r.failure_total >= 1);
		} else {
			CHECK(0 == r.failure_total);
		}
		/* the folder that was there is not ours to undo */
		for (size_t i = 0; i < r.created_n; i++) {
			CHECK(!r.created[i].dir);
		}
		CHECK(0 == leftovers(dir));
		CHECK(0 == leftovers(dst_dir));
		wf_copyresult_free(&r);
	}
}

static void unsafe_entries_are_reported_and_nothing_leaves_the_destination(void)
{
	ZipOut z;
	const wchar_t *whole[] = {L""};
	wchar_t p[1100];
	WfCopyResult r;

	memset(&z, 0, sizeof z);
	zip_add_s(&z, "ok.txt", "ok");
	zip_add_s(&z, "../escape.txt", "x");
	zip_add_s(&z, "sub/../../escape2.txt", "x");
	zip_add_s(&z, "/abs.txt", "x");
	zip_add_s(&z, "C:\\drive.txt", "x");
	zip_add_s(&z, "nul.txt", "x");
	zip_add_s(&z, "Doc.txt", "first");
	zip_add_s(&z, "doc.TXT", "second");
	CHECK(save_zip(&z, L"hostile.zip"));
	fresh();
	r = extract(whole, 1, WF_COLLIDE_STOP, NULL);
	CHECK(WF_CS_DONE == r.state);
	path_in(dst_dir, L"ok.txt", p);
	CHECK(holds(p, "ok"));
	path_in(dst_dir, L"Doc.txt", p);
	CHECK(holds(p, "first"));
	CHECK(failed_with(&r, WF_CF_ARCHIVE_COLLISION, L"doc.TXT"));
	CHECK(failed_with(&r, WF_CF_ARCHIVE_NAME, L"nul.txt"));
	CHECK(failed_with(&r, WF_CF_ARCHIVE_NAME, L"abs.txt"));
	/* nowhere above or beside the destination: not in the scratch root, not
	 * in the drive's root, not in the destination */
	path_in(root, L"escape.txt", p);
	CHECK(!exists(p));
	path_in(root, L"escape2.txt", p);
	CHECK(!exists(p));
	path_in(dst_dir, L"escape.txt", p);
	CHECK(!exists(p));
	path_in(dst_dir, L"abs.txt", p);
	CHECK(!exists(p));
	CHECK(2 == entries_in(dst_dir) && 0 == leftovers(dst_dir));
	wf_copyresult_free(&r);
}

static void a_wrong_checksum_leaves_that_file_out_and_the_rest_goes_on(void)
{
	ZipOut z;
	const wchar_t *whole[] = {L""};
	wchar_t p[1100];
	WfCopyResult r;

	memset(&z, 0, sizeof z);
	zip_add_s(&z, "good.txt", "good");
	zip_crc_xor = 1;
	zip_add_s(&z, "bad.txt", "bad!");
	zip_crc_xor = 0;
	zip_add_s(&z, "after.txt", "after");
	CHECK(save_zip(&z, L"crc.zip"));
	fresh();
	r = extract(whole, 1, WF_COLLIDE_STOP, NULL);
	CHECK(WF_CS_DONE == r.state && 2 == r.files_copied);
	CHECK(failed_with(&r, WF_CF_ARCHIVE_DATA, L"bad.txt"));
	path_in(dst_dir, L"bad.txt", p);
	CHECK(!exists(p));
	path_in(dst_dir, L"after.txt", p);
	CHECK(holds(p, "after"));
	CHECK(0 == leftovers(dst_dir));
	wf_copyresult_free(&r);
}

typedef struct
{
	int chunks;
	int cancel_after;
} Hooks;

static bool checkpoint(void *ctx)
{
	Hooks *h = ctx;

	h->chunks++;
	return !(h->cancel_after && h->chunks > h->cancel_after);
}

static void a_cancel_leaves_nothing_in_the_destination(void)
{
	ZipOut z;
	const wchar_t *whole[] = {L""};
	size_t big = (size_t)6 << 20;
	char *data = malloc(big);
	Hooks h = {0, 3};
	WfCopyControl ctl = {&h, checkpoint, NULL};
	WfCopyResult r;
	wchar_t p[1100];

	CHECK(NULL != data);
	if (!data) {
		return;
	}
	memset(data, 'q', big);
	memset(&z, 0, sizeof z);
	zip_add_s(&z, "first.txt", "first");
	zip_add(&z, "big.bin", 7, data, big, big, 0x800);
	zip_add_s(&z, "last.txt", "last");
	free(data);
	CHECK(save_zip(&z, L"cancel.zip"));
	fresh();
	r = extract(whole, 1, WF_COLLIDE_STOP, &ctl);
	CHECK(WF_CS_CANCELLED == r.state);
	CHECK(0 ==
	        entries_in(dst_dir)); /* neither staging nor a partial file */
	path_in(dst_dir, L"last.txt", p);
	CHECK(!exists(p));
	wf_copyresult_free(&r);
}

static void an_item_that_is_not_there_or_a_file_that_is_not_a_zip_fails_clean(
        void)
{
	ZipOut z;
	const wchar_t *gone[] = {L"nope"};
	const wchar_t *whole[] = {L""};
	WfCopyResult r;

	memset(&z, 0, sizeof z);
	zip_add_s(&z, "a.txt", "a");
	CHECK(save_zip(&z, L"plain.zip"));
	fresh();
	r = extract(gone, 1, WF_COLLIDE_STOP, NULL);
	CHECK(WF_CS_FAILED == r.state && 0 == entries_in(dst_dir));
	wf_copyresult_free(&r);
	path_in(root, L"notzip.zip", zip_path);
	wcscpy_s(made_zip[n_zip++], 1100, zip_path);
	CHECK(put(zip_path, "this is not a zip file at all", 29));
	r = extract(whole, 1, WF_COLLIDE_STOP, NULL);
	CHECK(WF_CS_FAILED == r.state && 0 == entries_in(dst_dir));
	wf_copyresult_free(&r);
}

static bool exists_x(const wchar_t *p)
{
	wchar_t x[4200];

	swprintf(x, 4200, L"\\\\?\\%ls", p);
	return INVALID_FILE_ATTRIBUTES != GetFileAttributesW(x);
}

static void a_path_longer_than_the_old_limit_is_extracted(void)
{
	ZipOut z;
	const wchar_t *whole[] = {L""};
	char name[2200] = "";
	wchar_t rel[2200] = L"", full[4200];
	WfCopyResult r;

	for (int i = 0; i < 12; i++) { /* a parent of 1452 units */
		char part[160];

		memset(part, 'a' + i, 120);
		part[120] = '/';
		part[121] = 0;
		strcat_s(name, sizeof name, part);
		for (size_t k = 0; k < 121; k++) {
			wcsncat_s(
			        rel, 2200, (wchar_t[]){(wchar_t)part[k], 0}, 1);
		}
	}
	strcat_s(name, sizeof name, "leaf.txt");
	wcscat_s(rel, 2200, L"leaf.txt");
	for (size_t k = 0; rel[k]; k++) {
		if (L'/' == rel[k]) {
			rel[k] = L'\\';
		}
	}
	memset(&z, 0, sizeof z);
	zip_add_s(&z, name, "deep");
	CHECK(save_zip(&z, L"long.zip"));
	fresh();
	r = extract(whole, 1, WF_COLLIDE_STOP, NULL);
	CHECK(WF_CS_DONE == r.state && 0 == r.failure_total);
	swprintf(full, 4200, L"%ls\\%ls", dst_dir, rel);
	CHECK(wcslen(full) > 260 && exists_x(full));
	CHECK(0 == leftovers(dst_dir));
	wf_copyresult_free(&r);
	/* the plain clean-up cannot reach a path this long: take it down, the file
	 * and then each folder, by the extended form */
	for (;;) {
		wchar_t x[4300], *cut;

		swprintf(x, 4300, L"\\\\?\\%ls", full);
		if (!DeleteFileW(x) && !RemoveDirectoryW(x)) {
			break;
		}
		cut = wcsrchr(full, L'\\');
		if (!cut || (size_t)(cut - full) <= wcslen(dst_dir)) {
			break;
		}
		*cut = 0;
	}
}

/* A file where a folder goes and a folder where a file goes: each policy is
 * the engine's own, and nothing that was there is replaced by the other kind. */
static void a_file_and_a_folder_never_replace_each_other(void)
{
	ZipOut z;
	const wchar_t *whole[] = {L""};
	wchar_t p[1100];
	WfCopyResult r;

	memset(&z, 0, sizeof z);
	zip_add_s(&z, "was_file/inside.txt", "in");
	zip_add_s(&z, "was_folder", "a file");
	CHECK(save_zip(&z, L"clash.zip"));
	for (int k = 0; k < 4; k++) {
		static const WfCollide pol[] = {WF_COLLIDE_STOP,
		        WF_COLLIDE_SKIP, WF_COLLIDE_RENAME,
		        WF_COLLIDE_OVERWRITE};

		fresh();
		path_in(dst_dir, L"was_file", p);
		CHECK(put(p, "file stays", 10));
		path_in(dst_dir, L"was_folder", p);
		CHECK(CreateDirectoryW(p, NULL));
		path_in(dst_dir, L"was_folder\\keep.txt", p);
		CHECK(put(p, "keep", 4));
		r = extract(whole, 1, pol[k], NULL);
		path_in(dst_dir, L"was_file", p);
		CHECK(holds(p, "file stays")); /* still the file */
		path_in(dst_dir, L"was_folder\\keep.txt", p);
		CHECK(holds(
		        p, "keep")); /* still the folder, with what it held */
		if (WF_COLLIDE_RENAME == pol[k]) {
			path_in(dst_dir, L"was_file (2)\\inside.txt", p);
			CHECK(holds(p, "in"));
			path_in(dst_dir, L"was_folder (2)", p);
			CHECK(exists(p));
		} else {
			CHECK(r.failure_total >= 1 ||
			        WF_COLLIDE_SKIP == pol[k]);
		}
		CHECK(0 == leftovers(dst_dir));
		wf_copyresult_free(&r);
	}
}

static unsigned
        g_blocks_written; /* a test sets it to 0 before using the hook */

static bool fail_second_block(void)
{
	return 2 == ++g_blocks_written;
}

static void a_write_that_fails_leaves_that_file_out_and_the_rest_goes_on(void)
{
	ZipOut z;
	const wchar_t *whole[] = {L""};
	size_t big = (size_t)1 << 20;
	char *data = malloc(big);
	wchar_t p[1100];
	WfCopyResult r;

	CHECK(NULL != data);
	if (!data) {
		return;
	}
	memset(data, 'w', big);
	memset(&z, 0, sizeof z);
	zip_add_s(&z, "a_first.txt", "first");
	zip_add(&z, "b_big.bin", 9, data, big, big, 0x800);
	zip_add_s(&z, "c_last.txt", "last");
	free(data);
	CHECK(save_zip(&z, L"writefail.zip"));
	fresh();
	g_blocks_written = 0;
	wf_extract_test_fail_write =
	        fail_second_block; /* the 2nd block written */
	r = extract(whole, 1, WF_COLLIDE_STOP, NULL);
	wf_extract_test_fail_write = NULL;
	CHECK(WF_CS_DONE == r.state && 2 == r.files_copied);
	CHECK(failed_with(&r, WF_CF_NO_SPACE, L"b_big.bin"));
	path_in(dst_dir, L"b_big.bin", p);
	CHECK(!exists(p));
	path_in(dst_dir, L"c_last.txt", p);
	CHECK(holds(p, "last"));
	CHECK(0 == leftovers(dst_dir) && 2 == entries_in(dst_dir));
	wf_copyresult_free(&r);
}

static void a_staging_left_by_a_crash_is_named_and_not_touched(void)
{
	ZipOut z;
	const wchar_t *whole[] = {L""};
	wchar_t p[1100];
	WfCopyResult r;
	bool named = false;

	memset(&z, 0, sizeof z);
	zip_add_s(&z, "x.txt", "x");
	CHECK(save_zip(&z, L"left.zip"));
	fresh();
	path_in(dst_dir, L".wfextract-0123456789abcdef.dir", p);
	CHECK(CreateDirectoryW(p, NULL));
	path_in(dst_dir, L".wfextract-0123456789abcdef.dir\\kept.txt", p);
	CHECK(put(p, "from a crash", 12));
	r = extract(whole, 1, WF_COLLIDE_STOP, NULL);
	for (size_t i = 0; i < r.failure_total && i < WF_CJ_FAILURES; i++) {
		named = named ||
		        (WF_CF_STAGING_LEFT == r.failures[i].kind &&
		                !wcscmp(r.failures[i].name,
		                        L".wfextract-0123456789abcdef.dir"));
	}
	CHECK(named && 1 == r.files_copied);
	CHECK(holds(
	        p, "from a crash")); /* not removed: it is not ours to prove */
	path_in(dst_dir, L"x.txt", p);
	CHECK(holds(p, "x"));
	CHECK(r.extracted);
	wf_copyresult_free(&r);
}

static void many_entries_are_taken_in_reasonable_time(void)
{
	ZipOut z;
	const wchar_t *whole[] = {L""};
	WfCopyResult r;
	ULONGLONG t0, ms;
	enum
	{
		N = 4000
	};

	memset(&z, 0, sizeof z);
	for (int i = 0; i < N; i++) {
		char name[64];

		snprintf(name, sizeof name, "d%03d/f%05d.txt", i % 50, i);
		zip_add_s(&z, name, "x");
	}
	CHECK(save_zip(&z, L"many.zip"));
	fresh();
	t0 = GetTickCount64();
	r = extract(whole, 1, WF_COLLIDE_STOP, NULL);
	ms = GetTickCount64() - t0;
	printf("  %d entries extracted and put in place in %llu ms\n", N,
	        (unsigned long long)ms);
	CHECK(WF_CS_DONE == r.state && 0 == r.failure_total &&
	        (size_t)N == r.files_copied);
	CHECK(ms < 120000);
	CHECK(0 == leftovers(dst_dir));
	wf_copyresult_free(&r);
}

static bool make_junction(const wchar_t *link, const wchar_t *target)
{
	wchar_t cmd[2400];
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	DWORD code = 1;

	swprintf(cmd, 2400, L"cmd.exe /c mklink /J \"%ls\" \"%ls\" >nul", link,
	        target);
	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL,
	            NULL, &si, &pi)) {
		WaitForSingleObject(pi.hProcess, 10000);
		GetExitCodeProcess(pi.hProcess, &code);
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
	}
	return 0 == code && exists(link);
}

/* A folder at the destination that is really a junction is not entered: what
 * it points at gets nothing, and the failure says the destination is unsafe. */
static void a_junction_at_the_destination_is_not_entered(void)
{
	ZipOut z;
	const wchar_t *whole[] = {L""};
	wchar_t target[1100], link[1100], p[1100];
	WfCopyResult r;

	memset(&z, 0, sizeof z);
	zip_add_s(&z, "via/inside.txt", "must not land");
	zip_add_s(&z, "other.txt", "fine");
	CHECK(save_zip(&z, L"junction.zip"));
	fresh();
	path_in(dst_dir, L"target", target);
	CHECK(CreateDirectoryW(target, NULL));
	path_in(dst_dir, L"via", link);
	if (!make_junction(link, target)) {
		printf("  NOT TESTED: a junction could not be made here\n");
		return;
	}
	r = extract(whole, 1, WF_COLLIDE_SKIP, NULL);
	path_in(dst_dir, L"target\\inside.txt", p);
	CHECK(!exists(p) && 0 == entries_in(target));
	path_in(dst_dir, L"other.txt", p);
	CHECK(holds(p, "fine"));
	CHECK(r.failure_total >= 1 || r.files_skipped >= 1);
	RemoveDirectoryW(link); /* the link itself, never its target */
	wf_copyresult_free(&r);
}

/* Leaks: the same extraction, cancelled and failed runs among them, over and
 * over; once it has settled, the heap in use must not grow. Checked with the
 * debug heap where there is one (the debug runtime); elsewhere it only runs. */
static void repeated_runs_do_not_grow_the_heap(void)
{
	ZipOut z;
	const wchar_t *whole[] = {L""};
	const wchar_t *gone[] = {L"nope"};
	Hooks h = {0, 1};
	WfCopyControl ctl = {&h, checkpoint, NULL};
#ifdef _DEBUG
	_CrtMemState before, after, diff;
#endif

	memset(&z, 0, sizeof z);
	zip_add_s(&z, "ok.txt", "ok");
	zip_add_s(&z, "../escape.txt", "x");
	zip_add_s(&z, "sub/aux.c", "x");
	zip_add_s(&z, "sub/file.txt", "file");
	zip_crc_xor = 1;
	zip_add_s(&z, "bad.txt", "bad");
	zip_crc_xor = 0;
	CHECK(save_zip(&z, L"leak.zip"));
	fresh(); /* one folder: the test area remembers every folder it makes */
	for (int round = 0; round < 40; round++) {
		WfCopyResult r;

		if (5 == round) {
#ifdef _DEBUG
			wf_archive_cache_clear();
			_CrtMemCheckpoint(&before);
#endif
		}
		purge(dst_dir);
		r = extract(whole, 1, WF_COLLIDE_STOP, NULL);
		wf_copyresult_free(&r);
		h.chunks = 0;
		purge(dst_dir);
		r = extract(whole, 1, WF_COLLIDE_STOP, &ctl); /* cancelled */
		wf_copyresult_free(&r);
		r = extract(gone, 1, WF_COLLIDE_STOP, NULL); /* failed */
		wf_copyresult_free(&r);
	}
#ifdef _DEBUG
	wf_archive_cache_clear();
	_CrtMemCheckpoint(&after);
	CHECK(!_CrtMemDifference(&diff, &before, &after) ||
	        diff.lSizes[_NORMAL_BLOCK] <= 0);
#endif
}

/* Measured, not claimed: one 64 MiB file taken out of a ZIP (staged, put in
 * place, verified by SHA-256) against the copy engine moving the same file
 * between two folders. Printed for the log; only a generous bound is checked. */
static void the_cost_of_extracting_is_measured_against_a_plain_copy(void)
{
	ZipOut z;
	const wchar_t *whole[] = {L""};
	const wchar_t *names[] = {L"big.bin"};
	size_t big = (size_t)64 << 20;
	unsigned char *data = malloc(big);
	wchar_t src[1100], src_file[1100];
	WfCopyResult r;
	WfCopySpec cs;
	ULONGLONG t0, t_extract, t_copy;

	CHECK(NULL != data);
	if (!data) {
		return;
	}
	for (size_t i = 0; i < big; i++) {
		data[i] = (unsigned char)((i * 131 + (i >> 8)) & 0xFF);
	}
	memset(&z, 0, sizeof z);
	zip_add(&z, "big.bin", 7, data, big, big, 0x800);
	CHECK(save_zip(&z, L"bench.zip"));
	fresh();
	t0 = GetTickCount64();
	r = extract(whole, 1, WF_COLLIDE_STOP, NULL);
	t_extract = GetTickCount64() - t0;
	CHECK(WF_CS_DONE == r.state && 1 == r.files_copied);
	wf_copyresult_free(&r);
	/* the same file as a plain file, copied by the engine */
	purge(dst_dir);
	swprintf(src, 1100, L"%ls\\benchsrc", root);
	CHECK(CreateDirectoryW(src, NULL));
	swprintf(src_file, 1100, L"%ls\\big.bin", src);
	CHECK(put(src_file, data, big));
	free(data);
	memset(&cs, 0, sizeof cs);
	wf_slot_init(&cs.slot);
	CHECK(WF_OK ==
	        wf_xfer_capture(
	                &ops, WF_XFER_COPY, src, names, 1, 0, &cs.slot));
	cs.kind = WF_CJ_COPY;
	cs.dest = _wcsdup(dst_dir);
	{
		WfNodeInfo d;

		CHECK(WF_OK == ops.node(NULL, dst_dir, &d));
		cs.dest_id = d.id;
	}
	cs.verify = WF_VERIFY_SHA256;
	cs.collide = WF_COLLIDE_STOP;
	t0 = GetTickCount64();
	CHECK(WF_OK == wf_copyengine_run(&cs, NULL, NULL, &r));
	t_copy = GetTickCount64() - t0;
	CHECK(WF_CS_DONE == r.state && 1 == r.files_copied);
	wf_copyresult_free(&r);
	wf_copyspec_free(&cs);
	printf("info: 64 MiB: extract %llu ms, plain copy %llu ms (the same "
	       "verification)\n",
	        (unsigned long long)t_extract, (unsigned long long)t_copy);
	CHECK(t_extract < 120000);
	purge(src);
	RemoveDirectoryW(src);
	purge(dst_dir);
}

static HANDLE g_blocker = INVALID_HANDLE_VALUE;

/* Called just before a thrown-away file is removed: another program has it
 * open, so the removal cannot succeed. */
static void block_the_discard(const wchar_t *path_x)
{
	g_blocker = CreateFileW(path_x, GENERIC_READ, 0, NULL, OPEN_EXISTING,
	        FILE_ATTRIBUTE_NORMAL, NULL);
}

/* A file that is not whole and cannot be removed must not be copied: nothing is
 * published from that staging folder, and the job says it failed. */
static void a_thrown_away_file_that_stays_stops_the_publication(void)
{
	ZipOut z;
	const wchar_t *whole[] = {L""};
	size_t big = (size_t)1 << 20;
	char *data = malloc(big);
	wchar_t p[1100];
	WfCopyResult r;
	bool left = false;

	CHECK(NULL != data);
	if (!data) {
		return;
	}
	memset(data, 'w', big);
	memset(&z, 0, sizeof z);
	zip_add_s(&z, "a_first.txt", "first");
	zip_add(&z, "b_big.bin", 9, data, big, big, 0x800);
	zip_add_s(&z, "c_last.txt", "last");
	free(data);
	CHECK(save_zip(&z, L"unclean.zip"));
	fresh();
	g_blocks_written = 0;
	wf_extract_test_fail_write = fail_second_block;
	wf_extract_test_before_discard = block_the_discard;
	/* a cancel after a few checkpoints: if the job did go on to copy from a
	 * staging folder it cannot read, it would only wait; it is stopped */
	r = extract(whole, 1, WF_COLLIDE_STOP,
	        &(WfCopyControl){&(Hooks){0, 20}, checkpoint, NULL});
	wf_extract_test_fail_write = NULL;
	wf_extract_test_before_discard = NULL;
	if (INVALID_HANDLE_VALUE != g_blocker) {
		CloseHandle(g_blocker);
		g_blocker = INVALID_HANDLE_VALUE;
	}
	CHECK(WF_CS_FAILED == r.state && 0 == r.files_copied);
	for (size_t i = 0; i < r.failure_total && i < WF_CJ_FAILURES; i++) {
		left = left || WF_CF_STAGING_LEFT == r.failures[i].kind;
	}
	CHECK(left); /* the file that stayed is said to be left */
	path_in(dst_dir, L"a_first.txt", p);
	CHECK(!exists(p));
	path_in(dst_dir, L"c_last.txt", p);
	CHECK(!exists(p)); /* nothing was published */
	wf_copyresult_free(&r);
	purge(dst_dir); /* the staging folder that was left, by the test's hand */
}

static bool g_swap_refused, g_dest_move_refused, g_swap_tried;
static wchar_t g_dest_for_hook[1100];

/* During the write of the second file: try to move a staged folder that is
 * complete (nothing in it is open, so only the hold on the folder itself can
 * refuse), and to move the destination away. Both are held, so both must
 * fail. */
static unsigned g_swap_calls;

static bool try_to_swap_while_writing(void)
{
	wchar_t pat[1100], from[1100], to[1100], stage[1100], moved[1100];
	WIN32_FIND_DATAW fd;
	HANDLE f;

	if (g_swap_tried || ++g_swap_calls < 2) {
		return false;
	}
	swprintf(pat, 1100, L"%ls\\.wfextract-*", g_dest_for_hook);
	f = FindFirstFileW(pat, &fd);
	if (INVALID_HANDLE_VALUE == f) {
		return false;
	}
	FindClose(f);
	g_swap_tried = true;
	swprintf(stage, 1100, L"%ls\\%ls", g_dest_for_hook, fd.cFileName);
	swprintf(from, 1100, L"%ls\\other", stage);
	swprintf(to, 1100, L"%ls\\other-aside", stage);
	g_swap_refused = !MoveFileW(from, to) && exists(from);
	swprintf(moved, 1100, L"%ls-moved", g_dest_for_hook);
	g_dest_move_refused =
	        !MoveFileW(g_dest_for_hook, moved) && exists(g_dest_for_hook);
	if (!g_dest_move_refused && exists(moved)) {
		MoveFileW(moved, g_dest_for_hook); /* put it back */
	}
	return false; /* the write goes on */
}

static void the_destination_and_the_staged_folders_are_held(void)
{
	ZipOut z;
	const wchar_t *whole[] = {L""};
	WfCopyResult r;
	WfCopySpec s;
	WfNodeInfo other;
	wchar_t p[1100];

	memset(&z, 0, sizeof z);
	zip_add_s(&z, "other/first.txt", "first");
	zip_add_s(&z, "sub/inner.txt", "inner");
	CHECK(save_zip(&z, L"held.zip"));
	fresh();
	wcscpy_s(g_dest_for_hook, 1100, dst_dir);
	g_swap_tried = g_swap_refused = g_dest_move_refused = false;
	g_swap_calls = 0;
	wf_extract_test_fail_write = try_to_swap_while_writing;
	r = extract(whole, 1, WF_COLLIDE_STOP, NULL);
	wf_extract_test_fail_write = NULL;
	CHECK(g_swap_tried && g_swap_refused && g_dest_move_refused);
	CHECK(WF_CS_DONE == r.state && 2 == r.files_copied);
	path_in(dst_dir, L"sub\\inner.txt", p);
	CHECK(holds(p, "inner"));
	wf_copyresult_free(&r);

	/* a destination that is not the folder that was chosen is refused before
	 * anything is made in it */
	fresh();
	s = spec_for(whole, 1, WF_COLLIDE_STOP);
	CHECK(WF_OK == ops.node(NULL, root, &other)); /* some other folder */
	s.dest_id = other.id;
	CHECK(WF_OK == wf_extract_run(&s, NULL, NULL, &r));
	CHECK(WF_CS_FAILED == r.state && 0 == entries_in(dst_dir));
	CHECK(failed_with(&r, WF_CF_DEST_UNSAFE, L""));
	wf_copyresult_free(&r);
	wf_copyspec_free(&s);
}

/* A cancel in the second stage (the staged items being put in place) is a
 * cancel of a copy: what was published before it stays, whole, and is in the
 * result for Undo; the item that was being copied is not there; nothing is left
 * hidden. */
static void a_cancel_while_putting_in_place_keeps_only_whole_items(void)
{
	ZipOut z;
	const wchar_t *whole[] = {L""};
	size_t big = (size_t)6 << 20;
	char *data = malloc(big);
	Hooks h = {0, 0};
	WfCopyControl ctl = {&h, checkpoint, NULL};
	WfCopyResult r;
	wchar_t p[1100];

	CHECK(NULL != data);
	if (!data) {
		return;
	}
	memset(data, 'q', big);
	memset(&z, 0, sizeof z);
	zip_add_s(&z, "a_first.txt", "first");
	zip_add(&z, "z_big.bin", 9, data, big, big, 0x800);
	free(data);
	CHECK(save_zip(&z, L"phase2.zip"));
	fresh();
	r = extract(
	        whole, 1, WF_COLLIDE_STOP, &ctl); /* counts the checkpoints */
	CHECK(WF_CS_DONE == r.state && h.chunks > 20);
	wf_copyresult_free(&r);
	purge(dst_dir);
	h.cancel_after = h.chunks - 3; /* inside the last item's copy */
	h.chunks = 0;
	r = extract(whole, 1, WF_COLLIDE_STOP, &ctl);
	CHECK(WF_CS_CANCELLED == r.state);
	path_in(dst_dir, L"a_first.txt", p);
	CHECK(holds(
	        p, "first")); /* published before the cancel: stays, whole */
	path_in(dst_dir, L"z_big.bin", p);
	CHECK(!exists(p));
	CHECK(1 == r.created_n && !r.created[0].dir);
	CHECK(1 == entries_in(dst_dir) && 0 == leftovers(dst_dir));
	wf_copyresult_free(&r);
}

int main(void)
{
	ops = wf_probe_win();
	CHECK(p0_lab_open(&lab, "extract", 0));
	if (!lab.area) {
		return TEST_RESULT();
	}
	wcscpy_s(root, 1024, lab.root);
	while (wcslen(root) > 3 && L'\\' == root[wcslen(root) - 1]) {
		root[wcslen(root) - 1] = 0;
	}
	a_chosen_folder_arrives_under_its_name_and_is_registered_for_undo();
	a_folder_that_is_there_is_merged_and_the_policy_decides_files();
	unsafe_entries_are_reported_and_nothing_leaves_the_destination();
	a_wrong_checksum_leaves_that_file_out_and_the_rest_goes_on();
	a_cancel_leaves_nothing_in_the_destination();
	an_item_that_is_not_there_or_a_file_that_is_not_a_zip_fails_clean();
	a_path_longer_than_the_old_limit_is_extracted();
	a_file_and_a_folder_never_replace_each_other();
	a_write_that_fails_leaves_that_file_out_and_the_rest_goes_on();
	a_staging_left_by_a_crash_is_named_and_not_touched();
	many_entries_are_taken_in_reasonable_time();
	a_junction_at_the_destination_is_not_entered();
	repeated_runs_do_not_grow_the_heap();
	a_thrown_away_file_that_stays_stops_the_publication();
	the_destination_and_the_staged_folders_are_held();
	a_cancel_while_putting_in_place_keeps_only_whole_items();
	the_cost_of_extracting_is_measured_against_a_plain_copy();
	purge(dst_dir);
	for (size_t i = 0; i < n_zip; i++) {
		DeleteFileW(made_zip[i]);
	}
	CHECK(0 == p0_lab_close(&lab, 0 != wf_test_failures));
	return TEST_RESULT();
}
