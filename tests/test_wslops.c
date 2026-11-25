/*
 * Changes to files in a WSL distribution (create, rename, delete, copy) run
 * here against a scratch folder: the operations use only calls that behave
 * the same on a local disk and on \\wsl.localhost, so the rules can be pinned
 * down without a distribution. A real distribution is exercised separately
 * by test_wsl_real (and is NOT RUN where none is running).
 */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tools/p0/p0_lab.h"
#include "fs/wf_wslops_win.h"
#include "wf_test.h"

static P0Lab lab;
static wchar_t dir[1024];

static void at(const wchar_t *name, wchar_t *out)
{
	swprintf(out, 1100, L"%ls\\%ls", dir, name);
}

static bool put(const wchar_t *name, const void *data, size_t n)
{
	wchar_t p[1100];
	HANDLE h;
	DWORD done = 0;
	bool ok;

	at(name, p);
	h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	ok = !n || (WriteFile(h, data, (DWORD)n, &done, NULL) && done == n);
	CloseHandle(h);
	return ok;
}

static bool exists(const wchar_t *name)
{
	wchar_t p[1100];

	at(name, p);
	return INVALID_FILE_ATTRIBUTES != GetFileAttributesW(p);
}

static size_t slurp(const wchar_t *name, void *buf, size_t cap)
{
	wchar_t p[1100];
	HANDLE h;
	DWORD got = 0;

	at(name, p);
	h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return (size_t)-1;
	}
	if (!ReadFile(h, buf, (DWORD)cap, &got, NULL)) {
		got = (DWORD)-1;
	}
	CloseHandle(h);
	return got;
}

static void names_that_windows_would_rewrite_are_refused(void)
{
	static const wchar_t *const bad[] = {L"", L".", L"..", L"a/b", L"a\\b",
	        L"a:b", L"a*", L"a?", L"a\"", L"a<", L"a>", L"a|", L"a.", L"a ",
	        L"a\tb"};

	for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
		CHECK(!wf_wslops_name_ok(bad[i]));
	}
	CHECK(!wf_wslops_name_ok(NULL));
	CHECK(wf_wslops_name_ok(L"a.txt") && wf_wslops_name_ok(L".hidden") &&
	        wf_wslops_name_ok(L"File") &&
	        wf_wslops_name_ok(L"\u65e5\u672c"));
	{
		wchar_t long_name[300];

		for (int i = 0; i < 257; i++) {
			long_name[i] = L'x';
		}
		long_name[256] = 0;
		CHECK(!wf_wslops_name_ok(long_name)); /* 256 characters */
		long_name[255] = 0;
		CHECK(wf_wslops_name_ok(long_name)); /* 255 */
	}
}

static void create_never_replaces(void)
{
	WfWslReport r;

	CHECK(WF_OK == wf_wslops_create(dir, L"new.txt", false, &r));
	CHECK(exists(L"new.txt"));
	CHECK(put(L"new.txt", "keep", 4));
	CHECK(WF_OK != wf_wslops_create(dir, L"new.txt", false, &r) &&
	        r.reason);
	{
		char b[8] = "";

		CHECK(4 == slurp(L"new.txt", b, sizeof b) &&
		        !memcmp(b, "keep", 4));
	}
	CHECK(WF_OK == wf_wslops_create(dir, L"sub", true, &r));
	CHECK(WF_OK != wf_wslops_create(dir, L"sub", true, &r));
	CHECK(WF_OK !=
	        wf_wslops_create(dir, L"sub", false, &r)); /* a folder has it */
	CHECK(WF_OK != wf_wslops_create(dir, L"a:b", false, &r));
	CHECK(WF_OK !=
	        wf_wslops_create(L"C:\\no\\such\\folder", L"x", false, &r));
	CHECK(WF_E_INVAL == wf_wslops_create(dir, L"x", false, NULL));
}

/* "a/b/c": folders on the way are made or entered; a file on the way is not
 * entered, and neither is a link. */
static void a_name_with_folders_on_the_way(void)
{
	WfWslReport r;
	wchar_t p[1100];

	CHECK(WF_OK == wf_wslops_create_path(dir, L"p/q/hello", false, &r));
	CHECK(exists(L"p\\q\\hello"));
	CHECK(WF_OK == wf_wslops_create_path(dir, L"p/q/sub", true, &r));
	CHECK(exists(L"p\\q\\sub") && exists(L"p\\q\\hello"));
	/* the last name is taken: nothing is replaced */
	CHECK(WF_OK != wf_wslops_create_path(dir, L"p/q/hello", false, &r) &&
	        r.reason);
	/* a file on the way */
	CHECK(WF_OK != wf_wslops_create_path(dir, L"p/q/hello/x", false, &r) &&
	        r.reason);
	CHECK(!exists(L"p\\q\\hello\\x"));
	/* nothing leaves the folder, and no part is empty */
	CHECK(WF_OK != wf_wslops_create_path(dir, L"p/../x", false, &r));
	CHECK(WF_OK != wf_wslops_create_path(dir, L"p//x", false, &r));
	CHECK(WF_OK != wf_wslops_create_path(dir, L"/x", false, &r));
	CHECK(!exists(L"x"));
	/* a link is not entered (a link needs the privilege: without it the
	 * part is skipped, not passed) */
	at(L"lnk", p);
	if (CreateSymbolicLinkW(p, dir,
	            SYMBOLIC_LINK_FLAG_DIRECTORY |
	                    SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
		CHECK(WF_OK !=
		        wf_wslops_create_path(dir, L"lnk/inside", false, &r));
		CHECK(!exists(L"inside"));
		RemoveDirectoryW(p);
	} else {
		printf("  NOT RUN: a symbolic link cannot be made here\n");
	}
}

static void rename_needs_a_free_name(void)
{
	WfWslReport r;
	char b[8] = "";

	CHECK(put(L"r1.txt", "one", 3) && put(L"r2.txt", "two", 3));
	CHECK(WF_OK != wf_wslops_rename(dir, L"r1.txt", dir, L"r2.txt", &r));
	CHECK(3 == slurp(L"r2.txt", b, sizeof b) && !memcmp(b, "two", 3));
	CHECK(exists(L"r1.txt"));
	CHECK(WF_OK == wf_wslops_rename(dir, L"r1.txt", dir, L"r3.txt", &r));
	CHECK(!exists(L"r1.txt") && exists(L"r3.txt"));
	CHECK(WF_OK != wf_wslops_rename(dir, L"gone", dir, L"x", &r));
	CHECK(WF_OK != wf_wslops_rename(dir, L"r3.txt", dir, L"a/b", &r));
	/* into another folder, name kept */
	CHECK(WF_OK == wf_wslops_create(dir, L"dest", true, &r));
	{
		wchar_t d2[1100];

		at(L"dest", d2);
		CHECK(WF_OK ==
		        wf_wslops_rename(dir, L"r3.txt", d2, L"r3.txt", &r));
		CHECK(!exists(L"r3.txt") && exists(L"dest\\r3.txt"));
	}
}

static void delete_goes_no_further_than_asked(void)
{
	WfWslReport r;
	wchar_t p[1100];

	CHECK(put(L"d1.txt", "x", 1));
	at(L"d1.txt", p);
	CHECK(WF_OK == wf_wslops_delete(p, false, &r) && !exists(L"d1.txt"));
	CHECK(WF_OK != wf_wslops_delete(p, false, &r)); /* already gone */

	/* a read-only file is not removed behind the user's back */
	CHECK(put(L"d2.txt", "x", 1));
	at(L"d2.txt", p);
	SetFileAttributesW(p, FILE_ATTRIBUTE_READONLY);
	CHECK(WF_OK != wf_wslops_delete(p, false, &r) && exists(L"d2.txt"));
	SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL);
	CHECK(WF_OK == wf_wslops_delete(p, false, &r));

	/* a folder with content needs `tree`; without it nothing is touched */
	CHECK(WF_OK == wf_wslops_create(dir, L"t", true, &r));
	CHECK(put(L"t\\a.txt", "1", 1));
	at(L"t", p);
	CHECK(WF_OK == wf_wslops_create(p, L"in", true, &r));
	CHECK(put(L"t\\in\\b.txt", "2", 1));
	CHECK(WF_OK != wf_wslops_delete(p, false, &r));
	CHECK(exists(L"t\\a.txt") && exists(L"t\\in\\b.txt"));
	CHECK(WF_OK == wf_wslops_delete(p, true, &r) && !exists(L"t"));
	CHECK(r.done >= 4);

	/* a read-only file inside stops the tree and leaves the rest */
	CHECK(WF_OK == wf_wslops_create(dir, L"t2", true, &r));
	CHECK(put(L"t2\\ro.txt", "1", 1));
	{
		wchar_t f[1100];

		at(L"t2\\ro.txt", f);
		SetFileAttributesW(f, FILE_ATTRIBUTE_READONLY);
		at(L"t2", p);
		CHECK(WF_OK != wf_wslops_delete(p, true, &r) &&
		        exists(L"t2\\ro.txt"));
		SetFileAttributesW(f, FILE_ATTRIBUTE_NORMAL);
		CHECK(WF_OK == wf_wslops_delete(p, true, &r) && !exists(L"t2"));
	}
	CHECK(WF_E_INVAL == wf_wslops_delete(NULL, true, &r));
}

static bool same_bytes(const wchar_t *a, const wchar_t *b)
{
	static char x[200000], y[200000];
	size_t nx = slurp(a, x, sizeof x), ny = slurp(b, y, sizeof y);

	return nx == ny && (size_t)-1 != nx && !memcmp(x, y, nx);
}

static void a_copy_is_whole_or_absent(void)
{
	static char big[150000];
	WfWslReport r;
	wchar_t src[1100], dst_dir[1100];

	for (size_t i = 0; i < sizeof big; i++) {
		big[i] = (char)(i * 31 + 7);
	}
	CHECK(put(L"c1.bin", big, sizeof big));
	CHECK(put(L"empty.txt", "", 0));
	CHECK(WF_OK == wf_wslops_create(dir, L"to", true, &r));
	at(L"c1.bin", src);
	at(L"to", dst_dir);
	CHECK(WF_OK == wf_wslops_copy(src, dst_dir, L"c1.bin", &r));
	CHECK(same_bytes(L"c1.bin", L"to\\c1.bin"));
	at(L"empty.txt", src);
	CHECK(WF_OK == wf_wslops_copy(src, dst_dir, L"empty.txt", &r));
	CHECK(exists(L"to\\empty.txt"));

	/* an existing name is never overwritten, and no temporary is left */
	CHECK(put(L"to\\c1.bin", "mine", 4));
	at(L"c1.bin", src);
	CHECK(WF_OK != wf_wslops_copy(src, dst_dir, L"c1.bin", &r));
	{
		char b[8] = "";

		CHECK(4 == slurp(L"to\\c1.bin", b, sizeof b) &&
		        !memcmp(b, "mine", 4));
	}
	{
		WIN32_FIND_DATAW fd;
		wchar_t pat[1100];
		HANDLE f;
		int leftovers = 0;

		swprintf(pat, 1100, L"%ls\\.piero-*", dst_dir);
		f = FindFirstFileW(pat, &fd);
		if (INVALID_HANDLE_VALUE != f) {
			leftovers++;
			FindClose(f);
		}
		CHECK(0 == leftovers);
	}
	CHECK(WF_OK !=
	        wf_wslops_copy(L"C:\\no\\such\\file", dst_dir, L"x", &r));
	CHECK(WF_OK != wf_wslops_copy(src, dst_dir, L"a/b", &r));
}

static void a_folder_is_copied_whole_and_never_into_itself(void)
{
	WfWslReport r;
	wchar_t src[1100], into[1100];

	CHECK(WF_OK == wf_wslops_create(dir, L"tree", true, &r));
	CHECK(put(L"tree\\top.txt", "top", 3));
	at(L"tree", src);
	CHECK(WF_OK == wf_wslops_create(src, L"deep", true, &r));
	CHECK(put(L"tree\\deep\\leaf.txt", "leaf", 4));
	CHECK(WF_OK == wf_wslops_create(src, L"empty", true, &r));
	CHECK(WF_OK == wf_wslops_create(dir, L"out", true, &r));
	at(L"out", into);
	CHECK(WF_OK == wf_wslops_copy(src, into, L"tree", &r));
	CHECK(same_bytes(L"tree\\top.txt", L"out\\tree\\top.txt"));
	CHECK(same_bytes(
	        L"tree\\deep\\leaf.txt", L"out\\tree\\deep\\leaf.txt"));
	CHECK(exists(L"out\\tree\\empty"));
	/* a second copy to the same name stops at the folder */
	CHECK(WF_OK != wf_wslops_copy(src, into, L"tree", &r));

	/* into itself, or below itself, would never end */
	CHECK(WF_OK != wf_wslops_copy(src, src, L"again", &r));
	{
		wchar_t below[1100];

		at(L"tree\\deep", below);
		CHECK(WF_OK != wf_wslops_copy(src, below, L"again", &r));
		CHECK(!exists(L"tree\\again") && !exists(L"tree\\deep\\again"));
	}
}

static void a_link_is_never_followed(void)
{
	WfWslReport r;
	wchar_t target[1100], link[1100], into[1100];

	CHECK(put(L"real.txt", "real", 4));
	at(L"real.txt", target);
	at(L"lnk.txt", link);
	if (!CreateSymbolicLinkW(link, target,
	            SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
		printf("  symbolic links cannot be made here (%lu): NOT RUN\n",
		        GetLastError());
		return;
	}
	CHECK(WF_OK == wf_wslops_create(dir, L"lout", true, &r));
	at(L"lout", into);
	CHECK(WF_OK != wf_wslops_copy(link, into, L"lnk.txt", &r));
	CHECK(!exists(L"lout\\lnk.txt"));
	/* deleting the link removes the link, not what it points at */
	CHECK(WF_OK == wf_wslops_delete(link, false, &r));
	CHECK(!exists(L"lnk.txt") && exists(L"real.txt"));
}

static bool time_of(const wchar_t *name, FILETIME *t)
{
	wchar_t p[1100];
	HANDLE h;
	bool ok;

	at(name, p);
	h = CreateFileW(p, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	ok = 0 != GetFileTime(h, NULL, NULL, t);
	CloseHandle(h);
	return ok;
}

static void a_copy_keeps_the_modification_time(void)
{
	WfWslReport r;
	wchar_t p[1100], d[1100];
	FILETIME old = {0}, a = {0}, b = {0};
	HANDLE h;
	ULARGE_INTEGER u;

	/* 2001-02-03 04:05:06 UTC, well away from "now" */
	u.QuadPart = (11644473600ull + 981173106ull) * 10000000ull;
	old.dwLowDateTime = u.LowPart;
	old.dwHighDateTime = u.HighPart;
	CHECK(put(L"old.txt", "x", 1));
	at(L"old.txt", p);
	h = CreateFileW(p, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	if (INVALID_HANDLE_VALUE != h) {
		CHECK(SetFileTime(h, NULL, NULL, &old));
		CloseHandle(h);
	}
	CHECK(WF_OK == wf_wslops_create(dir, L"tm", true, &r));
	at(L"tm", d);
	CHECK(WF_OK == wf_wslops_copy(p, d, L"old.txt", &r));
	CHECK(time_of(L"tm\\old.txt", &a) && time_of(L"old.txt", &b));
	CHECK(a.dwLowDateTime == b.dwLowDateTime &&
	        a.dwHighDateTime == b.dwHighDateTime &&
	        a.dwHighDateTime == old.dwHighDateTime);

	/* a folder keeps its time too, after its content was put in */
	CHECK(WF_OK == wf_wslops_create(dir, L"fold", true, &r));
	CHECK(put(L"fold\\f.txt", "1", 1));
	{
		wchar_t f[1100];

		at(L"fold", f);
		h = CreateFileW(f, FILE_WRITE_ATTRIBUTES,
		        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
		        FILE_FLAG_BACKUP_SEMANTICS, NULL);
		if (INVALID_HANDLE_VALUE != h) {
			CHECK(SetFileTime(h, NULL, NULL, &old));
			CloseHandle(h);
		}
		CHECK(WF_OK == wf_wslops_copy(f, d, L"fold", &r));
	}
	CHECK(time_of(L"tm\\fold", &a) &&
	        a.dwHighDateTime == old.dwHighDateTime &&
	        a.dwLowDateTime == old.dwLowDateTime);
}

static void undo_of_a_create_takes_only_what_is_still_empty(void)
{
	WfWslReport r;

	CHECK(put(L"e1.txt", "", 0));
	CHECK(WF_OK == wf_wslops_remove_if_empty(dir, L"e1.txt", &r) &&
	        !exists(L"e1.txt"));
	CHECK(put(L"e2.txt", "x", 1));
	CHECK(WF_OK != wf_wslops_remove_if_empty(dir, L"e2.txt", &r) &&
	        exists(L"e2.txt"));
	CHECK(WF_OK == wf_wslops_create(dir, L"ed", true, &r));
	CHECK(WF_OK == wf_wslops_remove_if_empty(dir, L"ed", &r) &&
	        !exists(L"ed"));
	CHECK(WF_OK == wf_wslops_create(dir, L"ed2", true, &r));
	CHECK(put(L"ed2\\in.txt", "x", 1));
	CHECK(WF_OK != wf_wslops_remove_if_empty(dir, L"ed2", &r) &&
	        exists(L"ed2\\in.txt"));
	CHECK(WF_OK != wf_wslops_remove_if_empty(dir, L"gone", &r));
	CHECK(WF_OK != wf_wslops_remove_if_empty(dir, L"a/b", &r));
	CHECK(0 == wf_wslops_progress() || wf_wslops_progress() > 0);
}

static void copied_bytes_are_counted(void)
{
	WfWslReport r;
	wchar_t p[1100], d[1100];

	wf_wslops_progress_reset();
	CHECK(0 == wf_wslops_progress());
	CHECK(put(L"cnt.bin", "0123456789", 10));
	CHECK(WF_OK == wf_wslops_create(dir, L"cn", true, &r));
	at(L"cnt.bin", p);
	at(L"cn", d);
	CHECK(WF_OK == wf_wslops_copy(p, d, L"cnt.bin", &r));
	CHECK(10 == wf_wslops_progress());
}

/* "Into itself" is told by the path: in the Linux part of a WSL path foo and Foo
 * are two folders, in the distribution name and on a Windows disk they are not. */
static void inside_is_told_with_the_case_of_linux_paths(void)
{
	const wchar_t *foo = L"\\\\wsl.localhost\\Ubuntu\\home\\foo";
	size_t n = wcslen(foo);

	CHECK(wf_wslops_is_inside(foo, foo, n));
	CHECK(wf_wslops_is_inside(
	        L"\\\\wsl.localhost\\Ubuntu\\home\\foo\\sub", foo, n));
	CHECK(wf_wslops_is_inside(
	        L"\\\\WSL.LOCALHOST\\ubuntu\\home\\foo", foo, n));
	CHECK(!wf_wslops_is_inside(
	        L"\\\\wsl.localhost\\Ubuntu\\home\\Foo", foo, n));
	CHECK(!wf_wslops_is_inside(
	        L"\\\\wsl.localhost\\Ubuntu\\home\\Foo\\sub", foo, n));
	CHECK(!wf_wslops_is_inside(
	        L"\\\\wsl.localhost\\Ubuntu\\home\\foobar", foo, n));
	CHECK(!wf_wslops_is_inside(L"\\\\wsl.localhost\\Ubuntu\\home", foo, n));
	/* the distribution alone, and a Windows path */
	CHECK(wf_wslops_is_inside(foo, L"\\\\wsl.localhost\\ubuntu", 22));
	CHECK(wf_wslops_is_inside(L"C:\\Data\\FOO\\x", L"C:\\data\\foo", 11));
	CHECK(!wf_wslops_is_inside(L"C:\\data\\foo2", L"C:\\data\\foo", 11));
}

int main(void)
{
	CHECK(p0_lab_open(&lab, "wslops", 0));
	if (!lab.area) {
		return TEST_RESULT();
	}
	CHECK(p0_area_mkdir(lab.area, L"w"));
	CHECK(p0_lab_path(&lab, L"w", dir, 1024));
	names_that_windows_would_rewrite_are_refused();
	create_never_replaces();
	a_name_with_folders_on_the_way();
	rename_needs_a_free_name();
	delete_goes_no_further_than_asked();
	a_copy_is_whole_or_absent();
	a_folder_is_copied_whole_and_never_into_itself();
	a_link_is_never_followed();
	a_copy_keeps_the_modification_time();
	undo_of_a_create_takes_only_what_is_still_empty();
	copied_bytes_are_counted();
	inside_is_told_with_the_case_of_linux_paths();
	{
		/* everything in `w` was made by this test: it takes its own
		 * tree away (the area only removes what it recorded) */
		WfWslReport rr;

		CHECK(WF_OK == wf_wslops_delete(dir, true, &rr));
	}
	CHECK(0 == p0_lab_close(&lab, 0 != wf_test_failures));
	return TEST_RESULT();
}
