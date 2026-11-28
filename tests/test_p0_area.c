/*
 * The scratch-area rules are the thing standing between a P0 run and the
 * user's files, so they are tested like safety code: every refusal must leave
 * the protected object untouched.
 */
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "../tools/p0/p0_area.h"
#include "../tools/p0/p0_common.h"
#include "wf_test.h"
#include "../src/base/wf_macro.h"

static wchar_t g_base[MAX_PATH];

static int exists(const wchar_t *p)
{
	return INVALID_FILE_ATTRIBUTES != GetFileAttributesW(p);
}

static void make_file(const wchar_t *path, const char *text)
{
	HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD wr;

	CHECK(INVALID_HANDLE_VALUE != h);
	if (INVALID_HANDLE_VALUE != h) {
		WriteFile(h, text, (DWORD)strlen(text), &wr, NULL);
		CloseHandle(h);
	}
}

/* Each test works under its own base, itself made through a first-level area,
 * so even the test scaffolding never deletes by pattern. */
static P0Area *fresh(const char *id)
{
	P0Area *a = p0_area_create(g_base, id);

	CHECK(NULL != a);
	return a;
}

static void test_roundtrip(void)
{
	P0Area *a = fresh("rt");
	wchar_t p[MAX_PATH * 2], rep[512];

	CHECK(p0_area_mkdir(a, L"sub"));
	CHECK(p0_area_path(a, L"sub\\f.txt", p, MAX_PATH * 2));
	make_file(p, "x");
	CHECK(p0_area_track(a, L"sub\\f.txt", 0));
	CHECK(exists(p));
	CHECK(0 == p0_area_destroy(a, rep, 512));
	CHECK(!exists(p));
}

/* The editor pins its original with DELETE access before saving by rename.
 * A test observing that save must be able to hash the pinned file without
 * interfering; a stable-source integrity reader deliberately cannot. */
static void test_hash_observation_shares_delete(void)
{
	P0Area *a = fresh("hash-observe");
	wchar_t path[MAX_PATH * 2];
	char expected[P0_HASH_HEX], observed[P0_HASH_HEX];
	HANDLE pin;

	CHECK(a != NULL);
	if (!a) {
		return;
	}
	CHECK(p0_area_path(a, L"original.txt", path, MAX_PATH * 2));
	make_file(path, "abc");
	CHECK(p0_area_track(a, L"original.txt", 0));
	CHECK(p0_sha256_file(path, expected));
	pin = CreateFileW(path, GENERIC_READ | DELETE,
	        FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(pin != INVALID_HANDLE_VALUE);
	if (pin != INVALID_HANDLE_VALUE) {
		CHECK(!p0_sha256_file(path, observed));
		CHECK(p0_sha256_observe(path, observed));
		CHECK(!strcmp(expected, observed));
		CloseHandle(pin);
	}
	CHECK(0 == p0_area_destroy(a, NULL, 0));
}

/* An object that is already gone is not a leftover, even when its parent is
 * gone too. */
static void test_already_gone_is_not_a_leftover(void)
{
	P0Area *a = fresh("gone");
	wchar_t p[MAX_PATH * 2], rep[512];

	CHECK(p0_area_mkdir(a, L"d"));
	CHECK(p0_area_path(a, L"d\\f.txt", p, MAX_PATH * 2));
	make_file(p, "x");
	CHECK(p0_area_track(a, L"d\\f.txt", 0));
	CHECK(p0_area_remove(a, L"d\\f.txt"));
	CHECK(p0_area_remove(a, L"d"));
	CHECK(p0_area_remove(
	        a, L"d\\f.txt")); /* again, with its parent gone as well */
	CHECK(0 == p0_area_destroy(a, rep, 512));
}

/* Beyond MAX_PATH: created, recorded and removed through the extended form. */
static void test_long_paths(void)
{
	P0Area *a = fresh("long");
	wchar_t rel[1024] = L"", step[128], x[4096], rep[512];
	int level;

	for (level = 0; level < 4; level++) {
		swprintf(step, 128, L"%ls%0100d", level ? L"\\" : L"", level);
		wcscat_s(rel, 1024, step);
		CHECK(p0_area_mkdir(a, rel));
	}
	wcscat_s(rel, 1024, L"\\leaf.txt");
	CHECK(p0_area_xpath(a, rel, x, 4096));
	CHECK(wcslen(x) > 260 + 10);
	{
		HANDLE h = CreateFileW(x, GENERIC_WRITE, 0, NULL, CREATE_NEW,
		        FILE_ATTRIBUTE_NORMAL, NULL);

		CHECK(INVALID_HANDLE_VALUE != h);
		if (INVALID_HANDLE_VALUE != h) {
			CloseHandle(h);
		}
	}
	CHECK(p0_area_track(a, rel, 0));
	CHECK(0 == p0_area_destroy(a, rep, 512));
}

static void test_name_rules(void)
{
	P0Area *a = fresh("names");
	wchar_t p[MAX_PATH * 2];
	static const wchar_t *const bad[] = {L"", L"..\\x", L"a\\..\\..\\b",
	        L"C:\\x", L"\\x", L"/x", L".", L"a\\.\\b", L"a*b", L"a?b",
	        L"a:b", L"a\\\\b", L"a|b", L"a\"b", L"a<b", L"a>b", L"x\\",
	        L"c:foo"};
	size_t i;

	for (i = 0; i < WF_ARRAY_LEN(bad); i++) {
		if (p0_area_path(a, bad[i], p, MAX_PATH * 2)) {
			printf("  accepted bad name: %ls\n", bad[i]);
			CHECK_FAIL("bad name accepted");
		}
	}
	CHECK(p0_area_path(a, L"ok\\name.txt", p, MAX_PATH * 2));
	CHECK(0 == p0_area_destroy(a, NULL, 0));
}

static void test_unrecorded_object_is_never_removed(void)
{
	P0Area *a = fresh("unrec");
	wchar_t mine[MAX_PATH * 2], stray[MAX_PATH * 2], rep[512];

	CHECK(p0_area_path(a, L"mine.txt", mine, MAX_PATH * 2));
	CHECK(p0_area_path(a, L"stray.txt", stray, MAX_PATH * 2));
	make_file(mine, "m");
	make_file(stray, "s");
	CHECK(p0_area_track(a, L"mine.txt", 0));

	CHECK(!p0_area_remove(a, L"stray.txt")); /* never recorded */
	CHECK(NULL != strstr(p0_area_last_refusal(a), "not recorded"));
	CHECK(exists(stray));

	/* destroy removes what it recorded and leaves the stray, saying so */
	CHECK(p0_area_destroy(a, rep, 512) >= 1);
	CHECK(!exists(mine) && exists(stray));
	CHECK(NULL != wcsstr(rep, L"stray.txt"));

	/* the test removes its own stray by exact path (it created it), then
	 * reaps */
	DeleteFileW(stray);
	{
		wchar_t root[MAX_PATH * 2];
		P0Area *again;

		swprintf(root, MAX_PATH * 2, L"%ls\\wfile-p0-unrec", g_base);
		again = p0_area_open(root);
		CHECK(NULL != again);
		if (again) {
			CHECK(0 == p0_area_destroy(again, NULL, 0));
		}
		CHECK(!exists(root));
	}
}

static void test_foreign_replacement_of_root(void)
{
	/* The root is deleted and recreated by someone else with the same name:
	 * the FileId differs, so nothing in it may be touched. */
	P0Area *a = fresh("swap");
	wchar_t root[MAX_PATH * 2], victim[MAX_PATH * 2], marker[MAX_PATH * 2],
	        p[MAX_PATH * 2], rep[512];

	wcscpy_s(root, MAX_PATH * 2, p0_area_root(a));
	CHECK(p0_area_path(a, L"f.txt", p, MAX_PATH * 2));
	make_file(p, "mine");
	CHECK(p0_area_track(a, L"f.txt", 0));

	/* an impostor directory: same name, different object, a file called
	 * f.txt inside */
	swprintf(marker, MAX_PATH * 2, L"%ls\\.p0-owner", root);
	DeleteFileW(marker);
	{
		wchar_t man[MAX_PATH * 2];

		swprintf(man, MAX_PATH * 2, L"%ls\\.p0-manifest", root);
		DeleteFileW(man);
	}
	DeleteFileW(p);
	RemoveDirectoryW(root);
	CHECK(CreateDirectoryW(root, NULL));
	swprintf(victim, MAX_PATH * 2, L"%ls\\f.txt", root);
	make_file(victim, "someone else's data");

	CHECK(p0_area_destroy(a, rep, 512) >= 1);
	CHECK(exists(victim)); /* untouched */
	CHECK(NULL != wcsstr(rep, L"REFUSED"));

	DeleteFileW(victim);
	RemoveDirectoryW(root);
}

static void test_tampered_marker(void)
{
	P0Area *a = fresh("nonce");
	wchar_t root[MAX_PATH * 2], p[MAX_PATH * 2], marker[MAX_PATH * 2],
	        rep[512];
	FILE *f = NULL;

	wcscpy_s(root, MAX_PATH * 2, p0_area_root(a));
	CHECK(p0_area_path(a, L"f.txt", p, MAX_PATH * 2));
	make_file(p, "keep me");
	CHECK(p0_area_track(a, L"f.txt", 0));

	swprintf(marker, MAX_PATH * 2, L"%ls\\.p0-owner", root);
	f = _wfsopen(marker, L"wb", 0x10);
	if (f) {
		fputs("wfile-p0-owner v1\nnonce=0000\n", f);
		fclose(f);
	}
	CHECK(p0_area_destroy(a, rep, 512) >= 1);
	CHECK(exists(p)); /* the nonce did not match: nothing deleted */
	CHECK(NULL != wcsstr(rep, L"REFUSED"));

	DeleteFileW(p);
	DeleteFileW(marker);
	{
		wchar_t man[MAX_PATH * 2];

		swprintf(man, MAX_PATH * 2, L"%ls\\.p0-manifest", root);
		DeleteFileW(man);
	}
	RemoveDirectoryW(root);
}

static int make_junction(const wchar_t *link, const wchar_t *target)
{
	wchar_t cmd[MAX_PATH * 6];
	STARTUPINFOW si = {.cb = sizeof si};
	PROCESS_INFORMATION pi;
	DWORD code = 1;

	swprintf(cmd, MAX_PATH * 6,
	        L"cmd.exe /c mklink /J \"%ls\" \"%ls\" >nul", link, target);
	if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
	            NULL, NULL, &si, &pi)) {
		return 0;
	}
	WaitForSingleObject(pi.hProcess, 10000);
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return 0 == code;
}

static void test_junction_cannot_redirect_a_delete(void)
{
	/* "outside" is a directory made by a second area: the first area has no
	 * right to anything in it, even through a link it created itself. */
	P0Area *a = fresh("jin");
	P0Area *outside = fresh("jout");
	wchar_t link[MAX_PATH * 2], victim[MAX_PATH * 2], tgt[MAX_PATH * 2],
	        rep[512];

	CHECK(p0_area_path(outside, L"victim.txt", victim, MAX_PATH * 2));
	make_file(victim, "outside data");
	CHECK(p0_area_track(outside, L"victim.txt", 0));

	CHECK(p0_area_path(a, L"jct", link, MAX_PATH * 2));
	wcscpy_s(tgt, MAX_PATH * 2, p0_area_root(outside));
	if (!make_junction(link, tgt)) {
		printf("  SKIP: mklink /J unavailable\n");
		p0_area_destroy(a, NULL, 0);
		p0_area_destroy(outside, NULL, 0);
		return;
	}
	CHECK(p0_area_track(a, L"jct", 1));
	/* record a path THROUGH the link, as a buggy caller might */
	CHECK(p0_area_track(a, L"jct\\victim.txt", 0));

	CHECK(!p0_area_remove(a, L"jct\\victim.txt"));
	CHECK(NULL != strstr(p0_area_last_refusal(a), "parent chain"));
	CHECK(exists(victim)); /* the file behind the link survives */

	CHECK(p0_area_destroy(a, rep, 512) >=
	        1); /* the through-link entry is refused */
	CHECK(exists(victim));
	CHECK(!exists(link)); /* the link itself (ours) is gone */
	CHECK(0 == p0_area_destroy(outside, NULL, 0));
	{
		/* The area stays provable (marker kept) because one recorded
		 * entry could not be removed. That entry was a deliberately bad
		 * record made by this test, so the test, which created the
		 * area, removes its bookkeeping by exact path. */
		wchar_t root[MAX_PATH * 2], f[MAX_PATH * 2];

		swprintf(root, MAX_PATH * 2, L"%ls\\wfile-p0-jin", g_base);
		CHECK(exists(root));
		swprintf(f, MAX_PATH * 2, L"%ls\\.p0-manifest", root);
		DeleteFileW(f);
		swprintf(f, MAX_PATH * 2, L"%ls\\.p0-owner", root);
		DeleteFileW(f);
		RemoveDirectoryW(root);
	}
}

static void test_reap_after_a_crash(void)
{
	/* a run "crashes" (handle forgotten, nothing deleted); a later process
	 * re-opens the area from disk and cleans it with the same proofs */
	P0Area *a = fresh("reap");
	P0Area *b;
	wchar_t root[MAX_PATH * 2], p[MAX_PATH * 2];

	wcscpy_s(root, MAX_PATH * 2, p0_area_root(a));
	CHECK(p0_area_mkdir(a, L"d"));
	CHECK(p0_area_path(a, L"d\\x.bin", p, MAX_PATH * 2));
	make_file(p, "x");
	CHECK(p0_area_track(a, L"d\\x.bin", 0));
	p0_area_forget(a);

	b = p0_area_open(root);
	CHECK(NULL != b);
	if (b) {
		CHECK(0 == p0_area_destroy(b, NULL, 0));
	}
	CHECK(!exists(p) && !exists(root));

	/* a directory without our marker cannot be opened as an area at all */
	CHECK(CreateDirectoryW(root, NULL));
	CHECK(NULL == p0_area_open(root));
	RemoveDirectoryW(root);
}

/* A leftover must stay provable: the marker survives while strays remain, so a
 * later reap works once the stray has been dealt with by a human. */
static void test_leftover_stays_reapable(void)
{
	P0Area *a = fresh("keep"), *b;
	wchar_t root[MAX_PATH * 2], mine[MAX_PATH * 2], stray[MAX_PATH * 2],
	        marker[MAX_PATH * 2], rep[512];

	wcscpy_s(root, MAX_PATH * 2, p0_area_root(a));
	CHECK(p0_area_path(a, L"mine.txt", mine, MAX_PATH * 2));
	CHECK(p0_area_path(a, L"stray.txt", stray, MAX_PATH * 2));
	make_file(mine, "m");
	make_file(stray, "s");
	CHECK(p0_area_track(a, L"mine.txt", 0));
	CHECK(p0_area_destroy(a, rep, 512) >= 1);
	swprintf(marker, MAX_PATH * 2, L"%ls\\.p0-owner", root);
	CHECK(exists(marker)); /* still provably ours */
	CHECK(!exists(mine) && exists(stray));

	b = p0_area_open(root); /* a later process can re-open it */
	CHECK(NULL != b);
	DeleteFileW(stray); /* the human (this test) deals with the stray */
	if (b) {
		CHECK(0 == p0_area_destroy(b, rep, 512));
	}
	CHECK(!exists(root));
}

static void test_cleanup_reports_sharing_failure(void)
{
	P0Area *a = fresh("locked-cleanup");
	wchar_t root[1024], child[1100], report[512];
	wcscpy_s(root, 1024, p0_area_root(a));
	CHECK(p0_area_path(a, L"child", child, 1100));
	make_file(child, "preserved");
	CHECK(p0_area_track(a, L"child", 0));
	HANDLE pin = CreateFileW(child, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(pin != INVALID_HANDLE_VALUE);
	if (pin == INVALID_HANDLE_VALUE) {
		p0_area_forget(a);
		return;
	}
	size_t handles = 0;
	CHECK(p0_area_open_handles(a, &handles) == ERROR_SUCCESS);
	CHECK(handles >= 1);
	CHECK(p0_area_destroy(a, report, 512) > 0);
	CHECK(wcsstr(report, L"child") && wcsstr(report, L"win32=32"));
	CHECK(exists(child));
	CloseHandle(pin);
	a = p0_area_open(root);
	CHECK(a != NULL); /* failed cleanup retained ownership evidence */
	if (a) {
		CHECK(p0_area_destroy(a, report, 512) == 0);
	}
}

/* A root-directory reader without delete sharing must not orphan the lab by
 * letting cleanup remove its ownership records before discovering the lock. */
static void test_locked_root_keeps_ownership(void)
{
	DWORD before_handles = 0, after_handles = 0;
	CHECK(GetProcessHandleCount(GetCurrentProcess(), &before_handles));
	P0Area *a = fresh("locked-root");
	wchar_t root[1024], marker[1100], report[512];
	wcscpy_s(root, 1024, p0_area_root(a));
	swprintf(marker, 1100, L"%ls\\.p0-owner", root);
	HANDLE pin = CreateFileW(root,
	        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	CHECK(pin != INVALID_HANDLE_VALUE);
	if (pin == INVALID_HANDLE_VALUE) {
		p0_area_forget(a);
		return;
	}
	CHECK(p0_area_destroy(a, report, 512) > 0);
	CHECK(wcsstr(report, L"win32=32") != NULL);
	CHECK(exists(marker));
	CloseHandle(pin);
	a = p0_area_open(root);
	CHECK(a != NULL);
	if (a) {
		CHECK(p0_area_destroy(a, report, 512) == 0);
	} else {
		/* Pre-fix regression: only an empty root may remain. */
		CHECK(RemoveDirectoryW(root));
	}
	CHECK(GetProcessHandleCount(GetCurrentProcess(), &after_handles));
	CHECK(before_handles == after_handles);
}

static void test_delete_sharing_root_reader_is_compatible(void)
{
	P0Area *a = fresh("shared-root");
	wchar_t root[1024], report[512];
	wcscpy_s(root, 1024, p0_area_root(a));
	/* The same access/sharing/overlapped flags as the directory watcher.
	 * A compatible reader is not evidence of a sharing violation. */
	HANDLE pin = CreateFileW(root, FILE_LIST_DIRECTORY,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
	CHECK(pin != INVALID_HANDLE_VALUE);
	CHECK(p0_area_destroy(a, report, 512) == 0);
	if (pin != INVALID_HANDLE_VALUE) {
		CloseHandle(pin);
	}
	CHECK(!exists(root));
}

static void test_create_refuses_existing(void)
{
	P0Area *a = fresh("dup");
	CHECK(NULL ==
	        p0_area_create(g_base,
	                "dup")); /* never adopts an existing directory */
	CHECK(0 == p0_area_destroy(a, NULL, 0));
	CHECK(NULL == p0_area_create(L"C:\\definitely\\not\\here", "x"));
}

int main(void)
{
	wchar_t tmp[MAX_PATH];

	GetTempPathW(MAX_PATH, tmp);
	swprintf(g_base, MAX_PATH, L"%lswfile-p0-areatest-%lu", tmp,
	        GetCurrentProcessId());
	CHECK(CreateDirectoryW(g_base, NULL));

	test_roundtrip();
	test_hash_observation_shares_delete();
	test_already_gone_is_not_a_leftover();
	test_long_paths();
	test_name_rules();
	test_unrecorded_object_is_never_removed();
	test_foreign_replacement_of_root();
	test_tampered_marker();
	test_junction_cannot_redirect_a_delete();
	test_reap_after_a_crash();
	test_leftover_stays_reapable();
	test_cleanup_reports_sharing_failure();
	test_locked_root_keeps_ownership();
	test_delete_sharing_root_reader_is_compatible();
	test_create_refuses_existing();

	RemoveDirectoryW(g_base); /* exact path, created above, empty if
	                             everything cleaned up */
	return TEST_RESULT();
}
