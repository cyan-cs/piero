/*
 * The editor's file access against the real filesystem (Windows only), in a
 * scratch area. What this pins down: files open only when they can be saved
 * back byte for byte; a save replaces the file without losing its permissions,
 * attributes or alternate data streams; and every case where a replacement
 * would lose something, or would overwrite someone else's change, is refused
 * with the original untouched and no temporary left behind.
 */
#include <windows.h>

#include <aclapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tools/p0/p0_lab.h"
#include "edit/wf_efile_win.h"
#include "async/wf_blocking.h"
#include "wf_test.h"
#include "xfer/wf_copyjournal_win.h"

static P0Lab lab;
static wchar_t dir[1024];

__declspec(noinline) static void path_of(const wchar_t *name, wchar_t *out)
{
	swprintf(out, 1100, L"%ls\\%ls", dir, name);
}

static bool write_raw(const wchar_t *path, const void *data, size_t n)
{
	HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD put = 0;
	bool ok;

	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	ok = !n || (WriteFile(h, data, (DWORD)n, &put, NULL) && put == n);
	CloseHandle(h);
	return ok;
}

static size_t read_raw(const wchar_t *path, char *buf, size_t cap)
{
	HANDLE h = CreateFileW(path, GENERIC_READ,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD got = 0;

	if (INVALID_HANDLE_VALUE == h) {
		return (size_t)-1;
	}
	if (!ReadFile(h, buf, (DWORD)cap, &got, NULL)) {
		got = 0;
	}
	CloseHandle(h);
	return got;
}

static bool has_content(const wchar_t *path, const char *expect, size_t n)
{
	char buf[4096];
	size_t got = read_raw(path, buf, sizeof buf);

	return got == n && !memcmp(buf, expect, n);
}

/* Registers a file the test created, so the area removes it at the end. */
__declspec(noinline) static void track(const wchar_t *name)
{
	wchar_t rel[260];

	swprintf(rel, 260, L"w\\%ls", name);
	CHECK(p0_area_track(lab.area, rel, 0));
}

static size_t temporaries(void)
{
	wchar_t pattern[1100];
	WIN32_FIND_DATAW fd;
	HANDLE f;
	size_t count = 0;

	swprintf(pattern, 1100, L"%ls\\.*.tmp", dir);
	f = FindFirstFileW(pattern, &fd);
	if (INVALID_HANDLE_VALUE == f) {
		return 0;
	}
	do {
		count++;
	} while (FindNextFileW(f, &fd));
	FindClose(f);
	return count;
}

static WfLoaded load(const wchar_t *name, const WfEncoding *force)
{
	wchar_t path[1100];
	WfLoaded l;

	path_of(name, path);
	CHECK(WF_OK == wf_efile_load(path, 1u << 20, 932, force, &l));
	return l;
}

__declspec(noinline) static void loading_decides_by_content(void)
{
	wchar_t path[1100];
	WfLoaded l;
	static const uint8_t u16[] = {
	        0xFF, 0xFE, 'h', 0, 'i', 0, 0x3D, 0xD8, 0, 0xDE};
	static const uint8_t bin[] = {0, 0, 0, 0, 'a', 1, 2, 3, 4, 0, 0, 0};
	static const uint8_t sjis[] = {0x82, 0xA0, 0x82, 0xA2}; /* あい */

	path_of(L"utf8.txt", path);
	CHECK(write_raw(path, "caf\xC3\xA9\r\nline\n", 12));
	track(L"utf8.txt");
	l = load(L"utf8.txt", NULL);
	CHECK(WF_FL_OK == l.verdict && WF_ENC_UTF8 == l.enc.kind &&
	        12 == l.utf8_n);
	CHECK(l.stamp.valid && 12 == l.stamp.size && 1 == l.stamp.links);
	wf_efile_loaded_free(&l);

	path_of(L"u16.txt", path);
	CHECK(write_raw(path, u16, sizeof u16));
	track(L"u16.txt");
	l = load(L"u16.txt", NULL);
	CHECK(WF_FL_OK == l.verdict && WF_ENC_UTF16LE == l.enc.kind &&
	        6 == l.utf8_n && !memcmp(l.utf8, "hi\xF0\x9F\x98\x80", 6));
	wf_efile_loaded_free(&l);

	path_of(L"bin.dat", path);
	CHECK(write_raw(path, bin, sizeof bin));
	track(L"bin.dat");
	l = load(L"bin.dat", NULL);
	CHECK(WF_FL_BINARY == l.verdict);
	wf_efile_loaded_free(&l);

	/* not UTF-8: the candidate is offered, and used only when chosen */
	path_of(L"sjis.txt", path);
	CHECK(write_raw(path, sjis, sizeof sjis));
	track(L"sjis.txt");
	l = load(L"sjis.txt", NULL);
	CHECK(WF_FL_AMBIGUOUS == l.verdict && l.candidate_ok &&
	        WF_ENC_CODEPAGE == l.enc.kind && 932 == l.enc.codepage &&
	        !l.utf8);
	{
		WfEncoding chosen = l.enc;

		wf_efile_loaded_free(&l);
		l = load(L"sjis.txt", &chosen);
		CHECK(WF_FL_OK == l.verdict && 6 == l.utf8_n);
		wf_efile_loaded_free(&l);
	}
	/* forced onto the wrong encoding it is refused, not mangled */
	{
		WfEncoding u16le = {WF_ENC_UTF16LE_RAW, 0};

		path_of(L"odd.txt", path);
		CHECK(write_raw(path, "abc", 3));
		track(L"odd.txt");
		l = load(L"odd.txt", &u16le);
		CHECK(WF_FL_BAD_ENCODING == l.verdict);
		wf_efile_loaded_free(&l);
	}
	path_of(L"empty.txt", path);
	CHECK(write_raw(path, "", 0));
	track(L"empty.txt");
	l = load(L"empty.txt", NULL);
	CHECK(WF_FL_OK == l.verdict && 0 == l.utf8_n);
	wf_efile_loaded_free(&l);
}

__declspec(noinline) static void loading_refuses_what_it_cannot_hold(void)
{
	wchar_t path[1100], sub[1100];
	WfLoaded l;
	char big[2048];

	l = load(L"nothing.txt", NULL);
	CHECK(WF_FL_MISSING == l.verdict);
	wf_efile_loaded_free(&l);

	swprintf(sub, 1100, L"%ls\\folder", dir);
	CHECK(CreateDirectoryW(sub, NULL));
	CHECK(p0_area_track(lab.area, L"w\\folder", 1));
	l = load(L"folder", NULL);
	CHECK(WF_FL_NOT_FILE == l.verdict);
	wf_efile_loaded_free(&l);

	memset(big, 'x', sizeof big);
	path_of(L"big.txt", path);
	CHECK(write_raw(path, big, sizeof big));
	track(L"big.txt");
	{
		WfLoaded cut;

		CHECK(WF_OK == wf_efile_load(path, 1000, 0, NULL, &cut));
		CHECK(WF_FL_TOO_BIG == cut.verdict && !cut.utf8);
		wf_efile_loaded_free(&cut);
	}
}

/*
 * wf_efile_load opens the file without delete sharing, so a handle that is
 * still open makes a rename fail. After every kind of answer, good or bad,
 * the file must be free: renamed away and back here. This is what the single
 * cleanup of the function promises on each of its exits.
 */
static bool file_is_free(const wchar_t *name)
{
	wchar_t from[1100], to[1100];
	bool ok;

	path_of(name, from);
	swprintf(to, 1100, L"%ls.moved", from);
	ok = MoveFileW(from, to) && MoveFileW(to, from);
	return ok;
}

__declspec(noinline) static void every_answer_lets_go_of_the_file(void)
{
	static const uint8_t bin[] = {0, 0, 0, 0, 'a', 1, 2, 3, 4, 0, 0, 0};
	static const uint8_t sjis[] = {0x82, 0xA0, 0x82, 0xA2};
	static const uint8_t bad_utf8[] = {'o', 'k', 0xC3, 0x28, 'x'};
	wchar_t path[1100];
	WfLoaded l;
	WfEncoding utf8 = {WF_ENC_UTF8, 0};

	path_of(L"free-ok.txt", path);
	CHECK(write_raw(path, "plain\n", 6));
	track(L"free-ok.txt");
	l = load(L"free-ok.txt", NULL);
	CHECK(WF_FL_OK == l.verdict && file_is_free(L"free-ok.txt"));
	wf_efile_loaded_free(&l);

	path_of(L"free-bin.dat", path);
	CHECK(write_raw(path, bin, sizeof bin));
	track(L"free-bin.dat");
	l = load(L"free-bin.dat", NULL);
	CHECK(WF_FL_BINARY == l.verdict && !l.utf8 &&
	        file_is_free(L"free-bin.dat"));
	wf_efile_loaded_free(&l);

	path_of(L"free-sjis.txt", path);
	CHECK(write_raw(path, sjis, sizeof sjis));
	track(L"free-sjis.txt");
	l = load(L"free-sjis.txt", NULL);
	CHECK(WF_FL_AMBIGUOUS == l.verdict && !l.utf8 &&
	        file_is_free(L"free-sjis.txt"));
	wf_efile_loaded_free(&l);

	path_of(L"free-bad.txt", path);
	CHECK(write_raw(path, bad_utf8, sizeof bad_utf8));
	track(L"free-bad.txt");
	l = load(L"free-bad.txt", &utf8); /* forced: decoding must refuse it */
	CHECK(WF_FL_BAD_ENCODING == l.verdict && !l.utf8 &&
	        file_is_free(L"free-bad.txt"));
	wf_efile_loaded_free(&l);

	path_of(L"free-big.txt", path);
	CHECK(write_raw(path, "0123456789", 10));
	track(L"free-big.txt");
	CHECK(WF_OK == wf_efile_load(path, 4, 0, NULL, &l));
	CHECK(WF_FL_TOO_BIG == l.verdict && !l.utf8 &&
	        file_is_free(L"free-big.txt"));
	wf_efile_loaded_free(&l);

	/* a missing file leaves nothing open, and the answer carries the error */
	l = load(L"free-missing.txt", NULL);
	CHECK(WF_FL_MISSING == l.verdict && 0 != l.error);
	wf_efile_loaded_free(&l);
}

static WfSaveResult save_over(const wchar_t *name,
        const WfFileStamp *was,
        const char *text,
        WfEncoding enc,
        bool backup)
{
	wchar_t path[1100];
	WfSaveResult r;
	WfSaveOptions opt = {backup};

	path_of(name, path);
	CHECK(wf_efile_save_over(
	              path, was, text, strlen(text), enc, &opt, &r) == WF_OK);
	if (r.backup_made && r.kept_original[0]) {
		wchar_t backup_name[1100];

		swprintf(backup_name, 1100, L"%ls~", name);
		track(backup_name);
	}
	return r;
}

__declspec(noinline) static void
a_save_replaces_the_file_and_keeps_what_belongs_to_it(void)
{
	wchar_t path[1100], ads[1200];
	WfEncoding utf8 = {WF_ENC_UTF8, 0};
	WfLoaded l;
	WfSaveResult r;
	PSECURITY_DESCRIPTOR sd1 = NULL, sd2 = NULL;
	PACL dacl1 = NULL, dacl2 = NULL;
	FILETIME created1, created2;
	HANDLE h;
	char buf[64];

	path_of(L"doc.txt", path);
	CHECK(write_raw(path, "old\r\ntext\r\n", 11));
	track(L"doc.txt");
	/* a hidden file with a downloaded-from stream */
	CHECK(SetFileAttributesW(
	        path, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE));
	swprintf(ads, 1200, L"%ls:Zone.Identifier", path);
	CHECK(write_raw(ads, "[ZoneTransfer]\r\nZoneId=3\r\n", 26));
	l = load(L"doc.txt", NULL);
	CHECK(WF_FL_OK == l.verdict);
	h = CreateFileW(path, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0,
	        NULL);
	CHECK(GetFileTime(h, &created1, NULL, NULL));
	CloseHandle(h);
	CHECK(GetNamedSecurityInfoW(path, SE_FILE_OBJECT,
	              DACL_SECURITY_INFORMATION, NULL, NULL, &dacl1, NULL,
	              &sd1) == ERROR_SUCCESS);

	r = save_over(
	        L"doc.txt", &l.stamp, "new\r\ntext\r\nmore\r\n", utf8, false);
	CHECK(WF_SV_OK == r.verdict && r.stamp.valid && !r.kept_temp[0]);
	CHECK(has_content(path, "new\r\ntext\r\nmore\r\n", 17));
	CHECK(0 == temporaries());

	/* attributes, stream, creation time and permissions all survived */
	{
		DWORD attrs = GetFileAttributesW(path);

		CHECK(0 != (attrs & FILE_ATTRIBUTE_HIDDEN));
	}
	CHECK(26 == read_raw(ads, buf, sizeof buf) &&
	        !memcmp(buf, "[ZoneTransfer]", 14));
	h = CreateFileW(path, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0,
	        NULL);
	CHECK(GetFileTime(h, &created2, NULL, NULL));
	CloseHandle(h);
	CHECK(0 == CompareFileTime(&created1, &created2));
	CHECK(GetNamedSecurityInfoW(path, SE_FILE_OBJECT,
	              DACL_SECURITY_INFORMATION, NULL, NULL, &dacl2, NULL,
	              &sd2) == ERROR_SUCCESS);
	CHECK(dacl1 && dacl2);
	{
		DWORD n1 = dacl1 ? dacl1->AceCount : 0,
		      n2 = dacl2 ? dacl2->AceCount : 0;

		CHECK(n1 == n2);
	}
	LocalFree(sd1);
	LocalFree(sd2);

	/* A retained original does not stop the next save: it goes to ~2, and
	 * the first backup is left exactly as it was. */
	{
		WfSaveResult again =
		        save_over(L"doc.txt", &r.stamp, "third\n", utf8, false);
		wchar_t backup[1100], second[1100];

		swprintf(backup, 1100, L"%ls~", path);
		swprintf(second, 1100, L"%ls~2", path);
		CHECK(WF_SV_OK == again.verdict && again.backup_made);
		CHECK(has_content(path, "third\n", 6));
		CHECK(has_content(second, "new\r\ntext\r\nmore\r\n", 17));
		CHECK(INVALID_FILE_ATTRIBUTES != GetFileAttributesW(backup));
		track(L"doc.txt~2");
		CHECK(DeleteFileW(backup)); /* this fixture's already checked original */
		CHECK(DeleteFileW(second));
	}
	wf_efile_loaded_free(&l);
}

/* The save temporary is born with the original's permissions, not the
 * folder's: a file only its owner may read has a temporary only its owner
 * may read, from the first byte written. */
__declspec(noinline) static void
the_save_temporary_is_never_more_open_than_the_original(void)
{
	wchar_t model[1100], tmp[1100], plain[1100];
	HANDLE token = NULL, h = INVALID_HANDLE_VALUE, hp;
	DWORD need = 0, error = 0;
	TOKEN_USER *user = NULL;
	EXPLICIT_ACCESSW grant;
	PACL only_me = NULL, dacl = NULL, dacl_plain = NULL;
	PSECURITY_DESCRIPTOR sd = NULL, sd_plain = NULL;
	SECURITY_DESCRIPTOR_CONTROL ctl = 0, ctl_plain = 0;
	DWORD rev;
	WfEncoding utf8 = {WF_ENC_UTF8, 0};
	WfLoaded l;
	WfSaveResult r;

	path_of(L"sec-model.txt", model);
	path_of(L"sec-model.txt.tmp", tmp);
	path_of(L"sec-plain.tmp", plain);
	CHECK(write_raw(model, "secret", 6));
	track(L"sec-model.txt");
	CHECK(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
	GetTokenInformation(token, TokenUser, NULL, 0, &need);
	user = malloc(need);
	CHECK(user && GetTokenInformation(token, TokenUser, user, need, &need));
	CloseHandle(token);
	if (!user) {
		return;
	}
	memset(&grant, 0, sizeof grant);
	grant.grfAccessPermissions = GENERIC_ALL;
	grant.grfAccessMode = SET_ACCESS;
	grant.grfInheritance = NO_INHERITANCE;
	grant.Trustee.TrusteeForm = TRUSTEE_IS_SID;
	grant.Trustee.TrusteeType = TRUSTEE_IS_USER;
	grant.Trustee.ptstrName = (LPWSTR)user->User.Sid;
	CHECK(ERROR_SUCCESS == SetEntriesInAclW(1, &grant, NULL, &only_me));
	CHECK(SetNamedSecurityInfoW(model, SE_FILE_OBJECT,
	              DACL_SECURITY_INFORMATION |
	                      PROTECTED_DACL_SECURITY_INFORMATION,
	              NULL, NULL, only_me, NULL) == ERROR_SUCCESS);

	/* a file made the ordinary way inherits the folder's entries: this is
	 * what the temporary used to get */
	hp = CreateFileW(plain, GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(INVALID_HANDLE_VALUE != hp);
	if (INVALID_HANDLE_VALUE != hp) {
		CloseHandle(hp);
		track(L"sec-plain.tmp");
	}
	CHECK(GetNamedSecurityInfoW(plain, SE_FILE_OBJECT,
	              DACL_SECURITY_INFORMATION, NULL, NULL, &dacl_plain, NULL,
	              &sd_plain) == ERROR_SUCCESS);
	CHECK(GetSecurityDescriptorControl(sd_plain, &ctl_plain, &rev));
	CHECK(!(ctl_plain & SE_DACL_PROTECTED));

	CHECK(wf_efile_create_temp_like(model, tmp, &h, &error));
	if (INVALID_HANDLE_VALUE != h) {
		CHECK(GetSecurityInfo(h, SE_FILE_OBJECT,
		              DACL_SECURITY_INFORMATION, NULL, NULL, &dacl,
		              NULL, &sd) == ERROR_SUCCESS);
		CHECK(GetSecurityDescriptorControl(sd, &ctl, &rev));
		CHECK(ctl & SE_DACL_PROTECTED);
		CHECK(dacl && 1 == dacl->AceCount);
		if (dacl && 1 == dacl->AceCount) {
			ACCESS_ALLOWED_ACE *ace = NULL;

			CHECK(GetAce(dacl, 0, (LPVOID *)&ace) && ace &&
			        EqualSid((PSID)&ace->SidStart, user->User.Sid));
		}
		CloseHandle(h);
		track(L"sec-model.txt.tmp");
	}
	/* a model whose permissions cannot be read stops the save */
	{
		HANDLE hh;
		wchar_t missing[1100];

		path_of(L"sec-no-such.txt", missing);
		hh = INVALID_HANDLE_VALUE;
		CHECK(!wf_efile_create_temp_like(missing, tmp, &hh, &error));
		CHECK(INVALID_HANDLE_VALUE == hh);
	}
	LocalFree(sd);
	LocalFree(sd_plain);

	/* end to end: after a save the file is still only the owner's */
	l = load(L"sec-model.txt", NULL);
	CHECK(WF_FL_OK == l.verdict);
	r = save_over(L"sec-model.txt", &l.stamp, "new secret", utf8, false);
	CHECK(WF_SV_OK == r.verdict);
	sd = NULL;
	CHECK(GetNamedSecurityInfoW(model, SE_FILE_OBJECT,
	              DACL_SECURITY_INFORMATION, NULL, NULL, &dacl, NULL,
	              &sd) == ERROR_SUCCESS);
	CHECK(dacl && 1 == dacl->AceCount);
	CHECK(has_content(model, "new secret", 10));
	LocalFree(sd);
	LocalFree(only_me);
	free(user);
	wf_efile_loaded_free(&l);
}

__declspec(noinline) static void every_replacement_retains_one_original(void)
{
	wchar_t path[1100], bak[1100];
	WfEncoding utf8 = {WF_ENC_UTF8, 0};
	WfLoaded l;
	WfSaveResult r;

	path_of(L"b.txt", path);
	path_of(L"b.txt~", bak);
	CHECK(write_raw(path, "v1", 2));
	track(L"b.txt");
	l = load(L"b.txt", NULL);
	r = save_over(L"b.txt", &l.stamp, "v2", utf8, true);
	CHECK(WF_SV_OK == r.verdict && r.backup_made);
	CHECK(has_content(bak, "v1", 2) && has_content(path, "v2", 2));
	/* the backup name is taken now: it is never overwritten, the next save
	 * keeps its original under the next free name */
	{
		wchar_t bak2[1100], bak3[1100];

		path_of(L"b.txt~2", bak2);
		path_of(L"b.txt~3", bak3);
		r = save_over(L"b.txt", &r.stamp, "v3", utf8, true);
		CHECK(WF_SV_OK == r.verdict && r.backup_made);
		track(L"b.txt~2");
		CHECK(has_content(bak, "v1", 2) && has_content(bak2, "v2", 2) &&
		        has_content(path, "v3", 2));
		/* Disabling the legacy preference cannot bypass preservation. */
		wf_efile_loaded_free(&l);
		l = load(L"b.txt", NULL);
		r = save_over(L"b.txt", &l.stamp, "v4", utf8, false);
		track(L"b.txt~3");
		CHECK(WF_SV_OK == r.verdict && r.backup_made &&
		        has_content(path, "v4", 2) && has_content(bak3, "v3", 2));
		CHECK(has_content(bak, "v1", 2) && has_content(bak2, "v2", 2));
	}
	wf_efile_loaded_free(&l);
}

__declspec(noinline) static void changes_made_elsewhere_are_never_overwritten(
        void)
{
	wchar_t path[1100];
	WfEncoding utf8 = {WF_ENC_UTF8, 0};
	WfLoaded l;
	WfSaveResult r;

	path_of(L"c.txt", path);
	CHECK(write_raw(path, "mine", 4));
	track(L"c.txt");
	l = load(L"c.txt", NULL);

	/* someone else rewrites it */
	Sleep(20);
	CHECK(write_raw(path, "theirs!", 7));
	r = save_over(L"c.txt", &l.stamp, "my edit", utf8, false);
	CHECK(WF_SV_CONFLICT == r.verdict);
	CHECK(has_content(path, "theirs!", 7) && 0 == temporaries());

	/* merely touched (same content, new time): not a conflict */
	CHECK(write_raw(path, "mine", 4));
	wf_efile_loaded_free(&l);
	l = load(L"c.txt", NULL);
	Sleep(20);
	CHECK(write_raw(path, "mine", 4));
	r = save_over(L"c.txt", &l.stamp, "edited", utf8, false);
	CHECK(WF_SV_OK == r.verdict && has_content(path, "edited", 6));
	wf_efile_loaded_free(&l);

	/* same length, same modification time, other content: still a conflict
	 * (the time and the size prove nothing) */
	CHECK(write_raw(path, "mine", 4));
	l = load(L"c.txt", NULL);
	{
		HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0,
		        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		FILETIME ct, at, wt;
		DWORD got = 0;
		bool ok = INVALID_HANDLE_VALUE != h &&
		        GetFileTime(h, &ct, &at, &wt) &&
		        WriteFile(h, "MINE", 4, &got, NULL) && 4 == got &&
		        SetFileTime(h, &ct, &at, &wt);

		CHECK(ok);
		if (INVALID_HANDLE_VALUE != h) {
			CloseHandle(h);
		}
	}
	r = save_over(L"c.txt", &l.stamp, "my edit", utf8, false);
	CHECK(WF_SV_CONFLICT == r.verdict);
	CHECK(has_content(path, "MINE", 4) && 0 == temporaries());
	wf_efile_loaded_free(&l);
}

__declspec(noinline) static void a_vanished_or_swapped_file_is_reported(void)
{
	wchar_t path[1100], other[1100];
	WfEncoding utf8 = {WF_ENC_UTF8, 0};
	WfLoaded l;
	WfSaveResult r;

	path_of(L"d.txt", path);
	path_of(L"d-other.txt", other);
	CHECK(write_raw(path, "data", 4));
	track(L"d.txt");
	l = load(L"d.txt", NULL);

	/* another file takes the name (a rename, as editors that save by
	 * replacing would do) */
	CHECK(write_raw(other, "different file", 14));
	track(L"d-other.txt");
	{
		wchar_t aside[1100];

		path_of(L"d-aside.txt", aside);
		CHECK(MoveFileW(path, aside));
		track(L"d-aside.txt");
		CHECK(MoveFileW(other, path));
	}
	r = save_over(L"d.txt", &l.stamp, "mine", utf8, false);
	CHECK(WF_SV_GONE == r.verdict);
	CHECK(has_content(path, "different file", 14) && 0 == temporaries());

	/* removed altogether */
	{
		wchar_t aside[1100];

		path_of(L"d-aside2.txt", aside);
		CHECK(MoveFileW(path, aside));
		track(L"d-aside2.txt");
	}
	r = save_over(L"d.txt", &l.stamp, "mine", utf8, false);
	CHECK(WF_SV_GONE == r.verdict && 0 == temporaries());
	wf_efile_loaded_free(&l);
}

__declspec(noinline) static void
a_replacement_that_would_lose_something_is_refused(void)
{
	wchar_t path[1100], link[1100];
	WfEncoding utf8 = {WF_ENC_UTF8, 0};
	WfLoaded l;
	WfSaveResult r;

	/* read-only */
	path_of(L"ro.txt", path);
	CHECK(write_raw(path, "ro", 2));
	track(L"ro.txt");
	l = load(L"ro.txt", NULL);
	CHECK(SetFileAttributesW(path, FILE_ATTRIBUTE_READONLY));
	r = save_over(L"ro.txt", &l.stamp, "x", utf8, false);
	CHECK(WF_SV_UNSAFE == r.verdict && has_content(path, "ro", 2));
	CHECK(SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL));
	wf_efile_loaded_free(&l);

	/* a hard link: both names must keep sharing one file */
	path_of(L"hl.txt", path);
	path_of(L"hl-link.txt", link);
	CHECK(write_raw(path, "hl", 2));
	track(L"hl.txt");
	CHECK(CreateHardLinkW(link, path, NULL));
	track(L"hl-link.txt");
	l = load(L"hl.txt", NULL);
	CHECK(WF_FL_OK == l.verdict && 2 == l.stamp.links);
	r = save_over(L"hl.txt", &l.stamp, "changed", utf8, false);
	CHECK(WF_SV_UNSAFE == r.verdict);
	CHECK(has_content(path, "hl", 2) && has_content(link, "hl", 2));
	wf_efile_loaded_free(&l);
	CHECK(0 == temporaries());
}

__declspec(noinline) static void streams_that_would_not_be_kept_stop_the_save(
        void)
{
	wchar_t path[1100], ads[1200];
	WfEncoding utf8 = {WF_ENC_UTF8, 0};
	WfLoaded l;
	WfSaveResult r;
	static char large[(1u << 20) + 16];

	path_of(L"s.txt", path);
	CHECK(write_raw(path, "s", 1));
	track(L"s.txt");
	swprintf(ads, 1200, L"%ls:huge", path);
	memset(large, 'z', sizeof large);
	CHECK(write_raw(ads, large, sizeof large));
	l = load(L"s.txt", NULL);
	r = save_over(L"s.txt", &l.stamp, "t", utf8, false);
	CHECK(WF_SV_UNSAFE == r.verdict && has_content(path, "s", 1));
	wf_efile_loaded_free(&l);
}

__declspec(noinline) static void
text_that_does_not_fit_the_encoding_is_refused_before_writing(void)
{
	wchar_t path[1100];
	WfEncoding cp932 = {WF_ENC_CODEPAGE, 932};
	WfLoaded l;
	WfSaveResult r;
	static const uint8_t sjis[] = {0x82, 0xA0};

	path_of(L"e.txt", path);
	CHECK(write_raw(path, sjis, sizeof sjis));
	track(L"e.txt");
	l = load(L"e.txt", &cp932);
	CHECK(WF_FL_OK == l.verdict);
	r = save_over(L"e.txt", &l.stamp, "\xE3\x81\x82\xF0\x9F\x98\x80", cp932,
	        false);
	CHECK(WF_SV_ENCODE == r.verdict && 3 == r.bad_offset);
	CHECK(has_content(path, "\x82\xA0", 2) && 0 == temporaries());
	/* what does fit is written in that code page */
	r = save_over(
	        L"e.txt", &l.stamp, "\xE3\x81\x82\xE3\x81\x84", cp932, false);
	CHECK(WF_SV_OK == r.verdict &&
	        has_content(path, "\x82\xA0\x82\xA2", 4));
	wf_efile_loaded_free(&l);
}

__declspec(noinline) static void bom_and_utf16_round_trip_through_a_save(void)
{
	wchar_t path[1100];
	WfLoaded l;
	WfSaveResult r;
	static const uint8_t bom8[] = {0xEF, 0xBB, 0xBF, 'a'};
	char buf[64];

	path_of(L"bom.txt", path);
	CHECK(write_raw(path, bom8, sizeof bom8));
	track(L"bom.txt");
	l = load(L"bom.txt", NULL);
	CHECK(WF_FL_OK == l.verdict && WF_ENC_UTF8_BOM == l.enc.kind);
	r = save_over(L"bom.txt", &l.stamp, "ab", l.enc, false);
	CHECK(WF_SV_OK == r.verdict);
	CHECK(5 == read_raw(path, buf, sizeof buf) &&
	        !memcmp(buf,
	                "\xEF\xBB\xBF"
	                "ab",
	                5));
	wf_efile_loaded_free(&l);

	{
		static const uint8_t be[] = {0xFE, 0xFF, 0, 'x'};

		wchar_t backup[1100];
		swprintf(backup, 1100, L"%ls~", path);
		CHECK(DeleteFileW(
		        backup)); /* finish the independent BOM fixture case */
		CHECK(write_raw(path, be, sizeof be));
		l = load(L"bom.txt", NULL);
		CHECK(WF_FL_OK == l.verdict && WF_ENC_UTF16BE == l.enc.kind);
		r = save_over(L"bom.txt", &l.stamp, "xy", l.enc, false);
		CHECK(WF_SV_OK == r.verdict);
		CHECK(6 == read_raw(path, buf, sizeof buf) &&
		        !memcmp(buf, "\xFE\xFF\0x\0y", 6));
		wf_efile_loaded_free(&l);
	}
}

__declspec(noinline) static void new_files_never_replace_anything(void)
{
	wchar_t path[1100];
	WfEncoding utf8 = {WF_ENC_UTF8, 0};
	WfSaveResult r;

	path_of(L"fresh.txt", path);
	CHECK(WF_OK == wf_efile_save_new(path, "hello\n", 6, utf8, &r));
	CHECK(WF_SV_OK == r.verdict && has_content(path, "hello\n", 6));
	track(L"fresh.txt");
	/* the name is taken now: refused, content intact */
	CHECK(WF_OK == wf_efile_save_new(path, "other", 5, utf8, &r));
	CHECK(WF_SV_EXISTS == r.verdict && has_content(path, "hello\n", 6));
	/* a bad character set refuses before the file is created */
	path_of(L"nope.txt", path);
	{
		WfEncoding cp932 = {WF_ENC_CODEPAGE, 932};

		CHECK(wf_efile_save_new(
		              path, "\xF0\x9F\x98\x80", 4, cp932, &r) == WF_OK);
		CHECK(WF_SV_ENCODE == r.verdict);
		CHECK(INVALID_FILE_ATTRIBUTES == GetFileAttributesW(path));
	}
	/* a folder that does not exist */
	path_of(L"missing\\x.txt", path);
	CHECK(WF_OK == wf_efile_save_new(path, "x", 1, utf8, &r));
	CHECK(WF_SV_IO == r.verdict);
}

__declspec(noinline) static void unicode_names_work(void)
{
	static const wchar_t name[] = {
	        0x65E5, 0x672C, 0xD83D, 0xDE00, L'.', L't', L'x', L't', 0};
	wchar_t path[1100];
	WfEncoding utf8 = {WF_ENC_UTF8, 0};
	WfSaveResult r;
	WfLoaded l;

	path_of(name, path);
	CHECK(WF_OK == wf_efile_save_new(path, "x", 1, utf8, &r) &&
	        WF_SV_OK == r.verdict);
	track(name);
	l = load(name, NULL);
	CHECK(WF_FL_OK == l.verdict);
	r = save_over(name, &l.stamp, "yy", utf8, false);
	CHECK(WF_SV_OK == r.verdict && has_content(path, "yy", 2));
	wf_efile_loaded_free(&l);
}

/* Another program has the file open without allowing it to be replaced: the
 * swap fails, the original stays, and no temporary is left. */
__declspec(noinline) static void a_locked_file_is_left_alone(void)
{
	wchar_t path[1100];
	WfEncoding utf8 = {WF_ENC_UTF8, 0};
	WfLoaded l;
	WfSaveResult r;
	HANDLE holder;

	path_of(L"locked.txt", path);
	CHECK(write_raw(path, "locked", 6));
	track(L"locked.txt");
	l = load(L"locked.txt", NULL);
	holder = CreateFileW(path, GENERIC_READ,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(INVALID_HANDLE_VALUE != holder);
	r = save_over(L"locked.txt", &l.stamp, "new", utf8, false);
	CHECK(WF_SV_IO == r.verdict && 0 != r.error && !r.kept_temp[0]);
	CHECK(has_content(path, "locked", 6) && 0 == temporaries());
	CloseHandle(holder);
	/* once released, the same save goes through */
	r = save_over(L"locked.txt", &l.stamp, "new", utf8, false);
	CHECK(WF_SV_OK == r.verdict && has_content(path, "new", 3));
	wf_efile_loaded_free(&l);
}

/* A symbolic link is not a file: where one can be made (it needs rights or
 * developer mode) it must be refused; otherwise this is reported, not hidden. */
__declspec(noinline) static void a_symbolic_link_is_not_opened(void)
{
	wchar_t path[1100], link[1100];
	WfLoaded l;

	path_of(L"target.txt", path);
	path_of(L"symlink.txt", link);
	CHECK(write_raw(path, "t", 1));
	track(L"target.txt");
	if (!CreateSymbolicLinkW(
	            link, path, SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
		printf("  NOT TESTED: symbolic links cannot be created here "
		       "(error %lu)\n",
		        GetLastError());
		return;
	}
	track(L"symlink.txt");
	l = load(L"symlink.txt", NULL);
	CHECK(WF_FL_LINK == l.verdict && !l.utf8);
	wf_efile_loaded_free(&l);
}

static wchar_t journal[1100];
static wchar_t g_aside[1100];

static bool die_between_the_renames(const wchar_t *dest, const wchar_t *aside)
{
	(void)dest;
	wcscpy_s(g_aside, 1100, aside);
	return true;
}

/* The original's name does not exist between the two renames of a save. The
 * move is recorded first as the user's file for manual recovery after a crash;
 * with no journal to record it in, no save replaces anything. */
__declspec(noinline) static void
a_save_that_dies_between_the_renames_does_not_lose_the_original(void)
{
	wchar_t path[1100];
	WfEncoding utf8 = {WF_ENC_UTF8, 0};
	WfLoaded l;
	WfSaveResult r;
	WfCopyJournalInfo info;
	char buf[64];
	size_t got;

	path_of(L"crash.txt", path);
	CHECK(write_raw(path, "original", 8));
	track(L"crash.txt");
	l = load(L"crash.txt", NULL);

	/* no journal: nothing is moved, the original is as it was */
	wf_efile_set_journal(NULL);
	r = save_over(L"crash.txt", &l.stamp, "new text", utf8, false);
	CHECK(WF_SV_IO == r.verdict && has_content(path, "original", 8));
	CHECK(r.reason && strstr(r.reason, "Settings > About"));
	CHECK(0 == temporaries());
	wf_efile_set_journal(journal);

	/* the process dies with the original moved aside */
	DeleteFileW(journal);
	wf_efile_test_after_aside = die_between_the_renames;
	r = save_over(L"crash.txt", &l.stamp, "new text", utf8, false);
	wf_efile_test_after_aside = NULL;
	CHECK(WF_SV_IO == r.verdict);
	CHECK(INVALID_FILE_ATTRIBUTES ==
	        GetFileAttributesW(path)); /* the window */
	got = read_raw(g_aside, buf, sizeof buf);
	CHECK(8 == got && !memcmp(buf, "original", 8)); /* nothing lost */
	/* Legacy journal text cannot authorize filesystem mutation. */
	CHECK(wf_cj_inspect(journal, &info) && info.records > 0);
	CHECK(has_content(g_aside, "original", 8));
	CHECK(INVALID_FILE_ATTRIBUTES == GetFileAttributesW(path));
	/* Explicit manual recovery of this fixture's own original. */
	CHECK(MoveFileW(g_aside, path));
	CHECK(has_content(path, "original", 8));
	CHECK(INVALID_FILE_ATTRIBUTES == GetFileAttributesW(g_aside));
	/* the new text was never anywhere it could be mistaken for the file */
	CHECK(1 == temporaries());
	{
		WIN32_FIND_DATAW fd;
		wchar_t pattern[1100], tmp[1100];
		HANDLE f;

		swprintf(pattern, 1100, L"%ls\\.*.tmp", dir);
		f = FindFirstFileW(pattern, &fd);
		CHECK(INVALID_HANDLE_VALUE != f);
		if (INVALID_HANDLE_VALUE != f) {
			FindClose(f);
			swprintf(tmp, 1100, L"%ls\\%ls", dir, fd.cFileName);
			CHECK(has_content(tmp, "new text", 8));
			DeleteFileW(tmp);
		}
	}

	/* An ordinary save preserves the old version; inspection never replays. */
	wf_efile_loaded_free(&l);
	l = load(L"crash.txt", NULL);
	DeleteFileW(journal);
	r = save_over(L"crash.txt", &l.stamp, "after", utf8, false);
	CHECK(WF_SV_OK == r.verdict && has_content(path, "after", 5));
	CHECK(0 == temporaries());
	CHECK(wf_cj_inspect(journal, &info) && info.records > 0);
	wf_efile_loaded_free(&l);
	DeleteFileW(journal);
}

/*
 * A replacing save that cannot be recorded in the journal must say what
 * refused it. One sentence with "error 0" for everything hid whether the
 * gate of an old record was working, or the system had failed. A gate is a
 * refusal and invents no error; a failure carries the Win32 error taken
 * where it happened. In every case the original is unchanged and no
 * temporary is left. Only this fixture's own journals are touched.
 */
static void a_refused_save_says_what_refused_it(void)
{
	static const char legacy_record[] =
	        "C 0 0 0000000000000000:00000000000000000000000000000000 x\n";
	WfEncoding utf8 = {WF_ENC_UTF8, 0};
	wchar_t path[1100], jp[1100];
	WfLoaded l;
	WfSaveResult r;
	HANDLE hold;

	path_of(L"why.txt", path);
	CHECK(write_raw(path, "original", 8));
	track(L"why.txt");
	l = load(L"why.txt", NULL);

	/* the gate: an old or damaged record is kept, replacing saves are locked */
	wf_efile_set_journal_unavailable(WF_EFJ_FORMAT, 0);
	r = save_over(L"why.txt", &l.stamp, "new text", utf8, false);
	CHECK(WF_SV_UNSAFE == r.verdict && 0 == r.error);
	CHECK(r.reason && strstr(r.reason, "old or unknown operation history"));
	CHECK(has_content(path, "original", 8) && 0 == temporaries());

	/* failures keep their own error */
	wf_efile_set_journal_unavailable(WF_EFJ_NO_STATE, ERROR_PATH_NOT_FOUND);
	r = save_over(L"why.txt", &l.stamp, "new text", utf8, false);
	CHECK(WF_SV_IO == r.verdict && ERROR_PATH_NOT_FOUND == r.error);
	CHECK(r.reason && strstr(r.reason, "state folder"));
	CHECK(has_content(path, "original", 8) && 0 == temporaries());
	wf_efile_set_journal_unavailable(WF_EFJ_INSPECT, ERROR_ACCESS_DENIED);
	r = save_over(L"why.txt", &l.stamp, "new text", utf8, false);
	CHECK(WF_SV_IO == r.verdict && ERROR_ACCESS_DENIED == r.error);
	CHECK(r.reason && strstr(r.reason, "could not be inspected"));
	CHECK(has_content(path, "original", 8) && 0 == temporaries());

	/* a cause that is not known still points at Settings > About */
	wf_efile_set_journal(NULL);
	r = save_over(L"why.txt", &l.stamp, "new text", utf8, false);
	CHECK(WF_SV_IO == r.verdict && r.reason &&
	        strstr(r.reason, "Settings > About"));
	CHECK(has_content(path, "original", 8) && 0 == temporaries());

	/* the real thing: an old-format record on disk is neither touched nor
	 * appended to, and the refusal is the gate's */
	path_of(L"why-legacy.txt", jp);
	CHECK(write_raw(jp, legacy_record, sizeof legacy_record - 1));
	track(L"why-legacy.txt");
	wf_efile_set_journal(jp);
	r = save_over(L"why.txt", &l.stamp, "new text", utf8, false);
	CHECK(WF_SV_UNSAFE == r.verdict && 0 == r.error);
	CHECK(r.reason && strstr(r.reason, "old or unknown operation history"));
	CHECK(has_content(jp, legacy_record, sizeof legacy_record - 1));
	CHECK(has_content(path, "original", 8) && 0 == temporaries());

	/* a journal someone else holds exclusively: a sharing violation, not 0 */
	path_of(L"why-held.txt", jp);
	CHECK(write_raw(jp, "", 0));
	track(L"why-held.txt");
	hold = CreateFileW(jp, GENERIC_READ | GENERIC_WRITE, 0, NULL,
	        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(INVALID_HANDLE_VALUE != hold);
	wf_efile_set_journal(jp);
	r = save_over(L"why.txt", &l.stamp, "new text", utf8, false);
	CHECK(WF_SV_IO == r.verdict && ERROR_SHARING_VIOLATION == r.error);
	CHECK(r.reason && strstr(r.reason, "could not be opened for recording"));
	CHECK(has_content(path, "original", 8) && 0 == temporaries());
	if (INVALID_HANDLE_VALUE != hold) {
		CloseHandle(hold);
	}

	/* a journal that cannot be appended to */
	path_of(L"why-readonly.txt", jp);
	CHECK(write_raw(jp, "", 0));
	track(L"why-readonly.txt");
	CHECK(SetFileAttributesW(jp, FILE_ATTRIBUTE_READONLY));
	wf_efile_set_journal(jp);
	r = save_over(L"why.txt", &l.stamp, "new text", utf8, false);
	CHECK(WF_SV_IO == r.verdict && ERROR_ACCESS_DENIED == r.error);
	CHECK(has_content(path, "original", 8) && 0 == temporaries());
	SetFileAttributesW(jp, FILE_ATTRIBUTE_NORMAL);

	/* with a usable journal the same save goes through: the refusals above
	 * were about the journal, not about this file */
	wf_efile_set_journal(journal);
	DeleteFileW(journal);
	r = save_over(L"why.txt", &l.stamp, "new text", utf8, false);
	CHECK(WF_SV_OK == r.verdict && has_content(path, "new text", 8));
	wf_efile_loaded_free(&l);
	DeleteFileW(journal);
}

/* A path into a WSL distribution is not written: nothing about it can prove
 * what a save needs. Refused by name, before any file is touched. */
static void set_mtime(const wchar_t *path, int year_offset)
{
	HANDLE h = CreateFileW(path, FILE_WRITE_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	ULARGE_INTEGER u;
	FILETIME ft;

	/* 2000-01-01 plus whole years of 365 days */
	u.QuadPart = (11644473600ull + 946684800ull +
	                     (unsigned long long)year_offset * 365 * 86400) *
	        10000000ull;
	ft.dwLowDateTime = u.LowPart;
	ft.dwHighDateTime = u.HighPart;
	if (INVALID_HANDLE_VALUE != h) {
		SetFileTime(h, NULL, NULL, &ft);
		CloseHandle(h);
	}
}

/* The hint that another program changed the file under a buffer (a notice, never
 * a decision): it compares the id, the size and the write time that the editor
 * kept with what the disk says now, reading attributes only. */
static WfFileStamp stamp_now(const wchar_t *path)
{
	WfLoaded l;
	WfFileStamp s;

	memset(&s, 0, sizeof s);
	if (WF_OK == wf_efile_load(path, 1u << 20, 0, NULL, &l)) {
		if (WF_FL_OK == l.verdict) {
			s = l.stamp;
		}
		wf_efile_loaded_free(&l);
	}
	return s;
}

static void a_hint_of_an_outside_change_reads_attributes_only(void)
{
	wchar_t p[1100], other[1100];
	WfFileStamp st, none;
	HANDLE lock;

	path_of(L"hint.txt", p);
	path_of(L"hint-other.txt", other);
	memset(&none, 0, sizeof none);
	CHECK(write_raw(p, "one\r\n", 5));
	st = stamp_now(p);
	CHECK(st.valid);
	CHECK(!wf_efile_probably_changed(p, &st)); /* nothing happened */
	/* no stamp, no claim: the hint says nothing it does not know */
	CHECK(!wf_efile_probably_changed(p, &none));
	CHECK(!wf_efile_probably_changed(p, NULL));
	/* a rewrite of another length, with the write time put back to what it was:
	 * only the size says so */
	set_mtime(p, 2);
	st = stamp_now(p);
	CHECK(!wf_efile_probably_changed(p, &st));
	CHECK(write_raw(p, "three\r\n", 7));
	set_mtime(p, 2);
	CHECK(wf_efile_probably_changed(p, &st));
	/* a rewrite of the same length, at another time */
	CHECK(write_raw(p, "one\r\n", 5));
	st = stamp_now(p);
	CHECK(write_raw(p, "two\r\n", 5));
	set_mtime(p, 3);
	CHECK(wf_efile_probably_changed(p, &st));
	/* the name now belongs to another file of the same size and time */
	st = stamp_now(p);
	CHECK(write_raw(other, "two\r\n", 5));
	set_mtime(other, 3);
	CHECK(st.valid);
	{
		WfFileStamp o = stamp_now(other);

		CHECK(o.size == st.size && o.mtime == st.mtime);
	}
	CHECK(MoveFileExW(other, p, MOVEFILE_REPLACE_EXISTING));
	CHECK(wf_efile_probably_changed(p, &st));
	/* held open by another program with no sharing: only attributes are read,
	 * so a file that did not change says it did not */
	st = stamp_now(p);
	lock = CreateFileW(p, GENERIC_READ | GENERIC_WRITE, 0, NULL,
	        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(INVALID_HANDLE_VALUE != lock);
	CHECK(!wf_efile_probably_changed(p, &st));
	if (INVALID_HANDLE_VALUE != lock) {
		CloseHandle(lock);
	}
	/* gone */
	CHECK(DeleteFileW(p));
	CHECK(wf_efile_probably_changed(p, &st));
}

/* With all nine backup names taken the save is refused and nothing is
 * removed: the product never deletes a backup on its own. */
static void nine_backups_stop_the_save(void)
{
	WfEncoding utf8 = {WF_ENC_UTF8, 0};
	wchar_t path[1100], name[1100];
	WfLoaded l;
	WfSaveResult r;

	path_of(L"nine.txt", path);
	CHECK(write_raw(path, "v1", 2));
	track(L"nine.txt");
	for (unsigned n = 1; n <= 9; n++) {
		if (1 == n) {
			swprintf(name, 1100, L"%ls~", path);
		} else {
			swprintf(name, 1100, L"%ls~%u", path, n);
		}
		CHECK(write_raw(name, "old", 3));
		set_mtime(name, 5 == n ? 1 : 10 + (int)n);
	}
	l = load(L"nine.txt", NULL);
	r = save_over(L"nine.txt", &l.stamp, "v2", utf8, true);
	CHECK(WF_SV_UNSAFE == r.verdict && !r.backup_made);
	CHECK(has_content(path, "v1", 2)); /* the file is as it was */
	for (unsigned n = 1; n <= 9; n++) { /* and every backup is still there */
		if (1 == n) {
			swprintf(name, 1100, L"%ls~", path);
		} else {
			swprintf(name, 1100, L"%ls~%u", path, n);
		}
		CHECK(has_content(name, "old", 3));
	}
	wf_efile_loaded_free(&l);
	for (unsigned n = 1; n <= 9; n++) {
		if (1 == n) {
			swprintf(name, 1100, L"%ls~", path);
		} else {
			swprintf(name, 1100, L"%ls~%u", path, n);
		}
		DeleteFileW(name);
	}
}

/* A job can be stopped at the two safe places: before the file is read, and
 * while the new content is still going to its temporary file. Stopping a save
 * leaves the original as it was, with no backup and no temporary file; a
 * request made before a job starts is cleared by the caller. */
static void a_job_stops_at_a_safe_point(void)
{
	WfEncoding utf8 = {WF_ENC_UTF8, 0};
	wchar_t path[1100];
	WfLoaded l;
	WfSaveResult r;
	WfCancel stop = {0};

	path_of(L"stop.txt", path);
	CHECK(write_raw(path, "keep me", 7));
	track(L"stop.txt");

	l = load(L"stop.txt", NULL);
	CHECK(WF_FL_OK == l.verdict);

	wf_cancel_ask(&stop);
	wf_cancel_bind(&stop);
	r = save_over(L"stop.txt", &l.stamp, "new text", utf8, true);
	wf_cancel_bind(NULL);
	CHECK(WF_SV_IO == r.verdict && !r.backup_made);
	CHECK(has_content(path, "keep me", 7)); /* the file is as it was */
	CHECK(0 == temporaries());              /* and nothing is left over */
	wf_efile_loaded_free(&l);

	/* a load that is asked to stop says so, and reads nothing */
	wf_cancel_bind(&stop);
	l = load(L"stop.txt", NULL);
	wf_cancel_bind(NULL);
	CHECK(WF_FL_OK != l.verdict);
	wf_efile_loaded_free(&l);

	/* with nothing asked, both go through */
	l = load(L"stop.txt", NULL);
	CHECK(WF_FL_OK == l.verdict);
	r = save_over(L"stop.txt", &l.stamp, "new text", utf8, true);
	CHECK(WF_SV_OK == r.verdict && has_content(path, "new text", 8));
	wf_efile_loaded_free(&l);
}

/* A WSL path has no file identity, so a save overwrites the bytes in place
 * (a rename would give the file a new owner and mode) after copying the
 * original to <name>~, and the file must still hold the bytes that were read.
 * The path rules are exercised here through the test switch, on a local
 * scratch folder; a real distribution is covered by test_wsl_real. */
static void wsl_files_are_saved_in_place(void)
{
	WfEncoding utf8 = {WF_ENC_UTF8, 0};
	wchar_t p[1100], b[1100];
	WfLoaded l;
	WfSaveResult r;
	WfFileStamp first;

	wf_efile_test_force_wsl = true;
	path_of(L"wsl1.txt", p);
	path_of(L"wsl1.txt~", b);
	CHECK(write_raw(p, "old", 3));
	track(L"wsl1.txt");
	CHECK(WF_OK == wf_efile_load(p, 0, 1252, NULL, &l) &&
	        WF_FL_OK == l.verdict && l.stamp.valid);
	first = l.stamp;
	wf_efile_loaded_free(&l);

	CHECK(WF_OK == wf_efile_save_over(p, &first, "new text", 8, utf8, NULL, &r) &&
	        WF_SV_OK == r.verdict);
	CHECK(has_content(p, "new text", 8) && has_content(b, "old", 3));
	CHECK(r.backup_made && r.stamp.valid && 8 == r.stamp.size);
	track(L"wsl1.txt~");

	/* the backup name is taken: the next save keeps its original under ~2 */
	first = r.stamp;
	CHECK(WF_OK ==
	                wf_efile_save_over(
	                        p, &first, "again", 5, utf8, NULL, &r) &&
	        WF_SV_OK == r.verdict);
	{
		wchar_t b2[1100];

		path_of(L"wsl1.txt~2", b2);
		track(L"wsl1.txt~2");
		CHECK(has_content(p, "again", 5) && has_content(b, "old", 3) &&
		        has_content(b2, "new text", 8));
		DeleteFileW(b2);
	}
	/* nine in the way: refused, nothing written */
	{
		wchar_t n[1100];

		for (unsigned i = 2; i <= 9; i++) {
			swprintf(n, 1100, L"%ls~%u", p, i);
			CHECK(write_raw(n, "x", 1));
		}
		first = r.stamp;
		CHECK(WF_OK ==
		                wf_efile_save_over(
		                        p, &first, "tenth", 5, utf8, NULL, &r) &&
		        WF_SV_UNSAFE == r.verdict && has_content(p, "again", 5));
		for (unsigned i = 2; i <= 9; i++) {
			swprintf(n, 1100, L"%ls~%u", p, i);
			DeleteFileW(n);
		}
		/* with room again it goes through */
		CHECK(WF_OK ==
		                wf_efile_save_over(
		                        p, &first, "tenth", 5, utf8, NULL, &r) &&
		        WF_SV_OK == r.verdict && has_content(p, "tenth", 5));
		swprintf(n, 1100, L"%ls~2", p);
		DeleteFileW(n);
	}
	r.stamp = first;

	/* changed elsewhere since it was read: refused, backup not made */
	DeleteFileW(b);
	CHECK(write_raw(p, "ELSEWHERE", 9));
	CHECK(WF_OK == wf_efile_save_over(
	                       p, &first, "mine", 4, utf8, NULL, &r) &&
	        WF_SV_CONFLICT == r.verdict);
	CHECK(has_content(p, "ELSEWHERE", 9));
	CHECK(INVALID_FILE_ATTRIBUTES == GetFileAttributesW(b));

	/* the same length with other bytes is a change too (the hash decides) */
	CHECK(write_raw(p, "xyz", 3));
	CHECK(WF_OK == wf_efile_load(p, 0, 1252, NULL, &l));
	first = l.stamp;
	wf_efile_loaded_free(&l);
	CHECK(write_raw(p, "xzy", 3));
	CHECK(WF_OK == wf_efile_save_over(
	                       p, &first, "mine", 4, utf8, NULL, &r) &&
	        WF_SV_CONFLICT == r.verdict);
	CHECK(has_content(p, "xzy", 3));

	/* gone, or read-only: nothing is written */
	CHECK(WF_OK == wf_efile_load(p, 0, 1252, NULL, &l));
	first = l.stamp;
	wf_efile_loaded_free(&l);
	SetFileAttributesW(p, FILE_ATTRIBUTE_READONLY);
	CHECK(WF_OK == wf_efile_save_over(
	                       p, &first, "mine", 4, utf8, NULL, &r) &&
	        WF_SV_UNSAFE == r.verdict);
	SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL);
	CHECK(has_content(p, "xzy", 3));
	DeleteFileW(p);
	CHECK(WF_OK == wf_efile_save_over(
	                       p, &first, "mine", 4, utf8, NULL, &r) &&
	        WF_SV_GONE == r.verdict);
	CHECK(INVALID_FILE_ATTRIBUTES == GetFileAttributesW(p));

	/* a new file: written and read back; an existing name is refused */
	CHECK(WF_OK == wf_efile_save_new(p, "fresh", 5, utf8, &r) &&
	        WF_SV_OK == r.verdict && has_content(p, "fresh", 5));
	CHECK(WF_OK == wf_efile_save_new(p, "other", 5, utf8, &r) &&
	        WF_SV_EXISTS == r.verdict && has_content(p, "fresh", 5));
	wf_efile_test_force_wsl = false;
}

int main(void)
{
	CHECK(p0_lab_open(&lab, "efile", 0));
	if (!lab.area) {
		return TEST_RESULT();
	}
	CHECK(p0_area_mkdir(lab.area, L"w"));
	CHECK(p0_lab_path(&lab, L"w", dir, 1024));
	p0_lab_path(&lab, L"efile-journal.txt", journal, 1100);
	wf_efile_set_journal(journal);
	loading_decides_by_content();
	every_answer_lets_go_of_the_file();
	a_refused_save_says_what_refused_it();
	nine_backups_stop_the_save();
	a_job_stops_at_a_safe_point();
	wsl_files_are_saved_in_place();
	loading_refuses_what_it_cannot_hold();
	a_save_replaces_the_file_and_keeps_what_belongs_to_it();
	the_save_temporary_is_never_more_open_than_the_original();
	every_replacement_retains_one_original();
	changes_made_elsewhere_are_never_overwritten();
	a_vanished_or_swapped_file_is_reported();
	a_replacement_that_would_lose_something_is_refused();
	streams_that_would_not_be_kept_stop_the_save();
	text_that_does_not_fit_the_encoding_is_refused_before_writing();
	bom_and_utf16_round_trip_through_a_save();
	new_files_never_replace_anything();
	unicode_names_work();
	a_locked_file_is_left_alone();
	a_symbolic_link_is_not_opened();
	a_save_that_dies_between_the_renames_does_not_lose_the_original();
	a_hint_of_an_outside_change_reads_attributes_only();
	CHECK(0 == p0_lab_close(&lab, 0 != wf_test_failures));
	return TEST_RESULT();
}
