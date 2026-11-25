/*
 * Drafts: what is never kept, and that a draft written is the draft read
 * back, replaced by the next one, and gone when removed. The folder is a
 * scratch one under the temp directory, made and removed by this test.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "base/wf_macro.h"
#include "edit/wf_edraft_win.h"
#include "wf_test.h"
#include "wf_draft_case_probe.h"
#undef GetFileInformationByHandleEx

typedef enum
{
	CASE_NATIVE,
	CASE_SENSITIVE,
	CASE_INSENSITIVE,
	CASE_UNKNOWN
} CaseProbe;
static CaseProbe case_probe;

BOOL WINAPI wf_test_draft_case_info(
        HANDLE file, FILE_INFO_BY_HANDLE_CLASS kind, LPVOID info, DWORD size)
{
	if (FileCaseSensitiveInfo != kind || CASE_NATIVE == case_probe) {
		return GetFileInformationByHandleEx(file, kind, info, size);
	}
	if (CASE_UNKNOWN == case_probe) {
		SetLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	if (size < sizeof(FILE_CASE_SENSITIVE_INFO)) {
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	((FILE_CASE_SENSITIVE_INFO *)info)->Flags = CASE_SENSITIVE == case_probe
	        ? FILE_CS_FLAG_CASE_SENSITIVE_DIR
	        : 0;
	return TRUE;
}

static void secret_name_exclusions(void)
{
	CHECK(wf_draft_allowed(L"C:\\Users\\me\\notes.txt", 100));
	CHECK(wf_draft_allowed(L"C:\\src\\main.c", 100));
	/* names that say they hold secrets */
	CHECK(!wf_draft_allowed(L"C:\\Users\\me\\.env", 10));
	CHECK(!wf_draft_allowed(L"C:\\Users\\me\\.env.local", 10));
	CHECK(!wf_draft_allowed(L"C:\\x\\server.key", 10));
	CHECK(!wf_draft_allowed(L"C:\\x\\CERT.PEM", 10));
	CHECK(!wf_draft_allowed(L"C:\\x\\id_rsa", 10));
	CHECK(!wf_draft_allowed(L"C:\\x\\my-Password-list.txt", 10));
	CHECK(!wf_draft_allowed(L"C:\\x\\api_token.json", 10));
	/* folders that are for secrets */
	CHECK(!wf_draft_allowed(L"C:\\Users\\me\\.ssh\\config", 10));
	CHECK(!wf_draft_allowed(L"C:\\Users\\me\\.aws\\credentials", 10));
	/* a network path leaves the machine it is kept on */
	CHECK(!wf_draft_allowed(L"\\\\server\\share\\a.txt", 10));
	CHECK(!wf_draft_allowed(L"\\\\?\\UNC\\server\\share\\a.txt", 10));
	CHECK(wf_draft_allowed(L"\\\\?\\C:\\Users\\me\\a.txt", 10));
	/* too large to be a draft, and no name at all */
	CHECK(!wf_draft_allowed(L"C:\\x\\big.txt", (size_t)3 << 20));
	CHECK(!wf_draft_allowed(L"C:\\x\\", 10));
	CHECK(!wf_draft_allowed(L"", 10));
	CHECK(!wf_draft_allowed(NULL, 10));
}

static void invalid_paths_are_rejected_before_access(void)
{
	WfEncoding enc = {WF_ENC_UTF8, 0};
	char *text = NULL;
	size_t n = 1;

	CHECK(WF_E_INVAL == wf_draft_write(NULL, enc, "text", 4));
	CHECK(WF_E_INVAL == wf_draft_write(L"", enc, "text", 4));
	CHECK(WF_E_INVAL == wf_draft_read(NULL, &enc, &text, &n));
	CHECK(!text && !n);
	CHECK(WF_E_INVAL == wf_draft_read(L"", &enc, &text, &n));
	wf_draft_remove(NULL);
	wf_draft_remove(L"");
}

static bool make_scratch(wchar_t *dir, size_t cap)
{
	wchar_t temp[MAX_PATH];
	static unsigned serial;
	DWORD length = GetTempPathW(MAX_PATH, temp);
	int written;

	if (!length || length >= MAX_PATH) {
		return false;
	}
	written = swprintf(dir, cap, L"%lswf-draft-%lu-%llu-%u", temp,
	        (unsigned long)GetCurrentProcessId(),
	        (unsigned long long)GetTickCount64(), ++serial);
	return written > 0 && (size_t)written < cap &&
	        CreateDirectoryW(dir, NULL);
}

static void a_draft_round_trips_and_is_replaced_and_removed(void)
{
	wchar_t dir[MAX_PATH + 40], file[MAX_PATH + 80];
	const wchar_t *path = L"C:\\Users\\me\\notes.txt";
	WfEncoding enc = {WF_ENC_UTF8, 0}, back;
	char *text = NULL;
	size_t n = 0;
	WIN32_FIND_DATAW fd;
	HANDLE h;

	if (!make_scratch(dir, WF_ARRAY_LEN(dir))) {
		CHECK_FAIL("cannot create an owned draft fixture");
		return;
	}
	wf_draft_set_dir(dir);

	CHECK(WF_E_NOENT == wf_draft_read(path, &back, &text, &n));

	CHECK(WF_OK == wf_draft_write(path, enc, "first\n\xE3\x81\x82", 9));
	CHECK(WF_OK == wf_draft_read(path, &back, &text, &n));
	CHECK(9 == n && !memcmp(text, "first\n\xE3\x81\x82", 9) &&
	        WF_ENC_UTF8 == back.kind);
	free(text);

	/* the next write replaces it; text with a header-like line is just text */
	CHECK(WF_OK == wf_draft_write(path, enc, "second\n--\nsize 1\n", 17));
	CHECK(WF_OK == wf_draft_read(path, &back, &text, &n));
	CHECK(17 == n && !memcmp(text, "second\n--\nsize 1\n", 17));
	free(text);

	/* another file's draft is another draft */
	CHECK(WF_E_NOENT == wf_draft_read(L"C:\\Users\\me\\other.txt", &back,
	                            &text, &n));

	/* a secret-looking file is refused at the door, whoever asks */
	CHECK(WF_OK != wf_draft_write(L"C:\\Users\\me\\.env", enc, "k=v", 3));
	CHECK(WF_E_NOENT == wf_draft_read(L"C:\\Users\\me\\.env", &back, &text,
	                            &n));

	wf_draft_remove(path);
	CHECK(WF_E_NOENT == wf_draft_read(path, &back, &text, &n));
	wf_draft_remove(path); /* none is not an error */

	/* clean up what this test made: only files in its own folder */
	swprintf(file, WF_ARRAY_LEN(file), L"%ls\\*", dir);
	h = FindFirstFileW(file, &fd);
	if (INVALID_HANDLE_VALUE != h) {
		do {
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
				swprintf(file, WF_ARRAY_LEN(file), L"%ls\\%ls", dir,
				        fd.cFileName);
				DeleteFileW(file);
			}
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	RemoveDirectoryW(dir);
	wf_draft_set_dir(NULL);
}

static void remove_tree_files(const wchar_t *dir)
{
	wchar_t file[MAX_PATH + 80];
	WIN32_FIND_DATAW fd;
	HANDLE h;

	swprintf(file, WF_ARRAY_LEN(file), L"%ls\\*", dir);
	h = FindFirstFileW(file, &fd);
	if (INVALID_HANDLE_VALUE != h) {
		do {
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
				swprintf(file, WF_ARRAY_LEN(file), L"%ls\\%ls",
				        dir, fd.cFileName);
				DeleteFileW(file);
			}
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
}

/* Two names that differ only in case are two files unless the folder says they
 * are one: where that is not known, each has a draft of its own, and writing,
 * reading or removing one never touches the other. */
static void names_that_differ_in_case_keep_their_own_drafts(void)
{
	wchar_t dir[MAX_PATH + 40];
	WfEncoding enc = {WF_ENC_UTF8, 0}, back;
	char *text = NULL;
	size_t n = 0;

	if (!make_scratch(dir, WF_ARRAY_LEN(dir))) {
		CHECK_FAIL("cannot create an owned draft fixture");
		return;
	}
	wf_draft_set_dir(dir);

	/* the folder is not there: nothing says the two are one */
	CHECK(WF_OK ==
	        wf_draft_write(L"C:\\no\\such\\Foo.txt", enc, "upper", 5));
	CHECK(WF_OK ==
	        wf_draft_write(L"C:\\no\\such\\foo.txt", enc, "lower", 5));
	CHECK(WF_OK ==
	        wf_draft_read(L"C:\\no\\such\\Foo.txt", &back, &text, &n));
	CHECK(5 == n && !memcmp(text, "upper", 5));
	free(text);
	CHECK(WF_OK ==
	        wf_draft_read(L"C:\\no\\such\\foo.txt", &back, &text, &n));
	CHECK(5 == n && !memcmp(text, "lower", 5));
	free(text);
	wf_draft_remove(L"C:\\no\\such\\Foo.txt");
	CHECK(WF_E_NOENT ==
	        wf_draft_read(L"C:\\no\\such\\Foo.txt", &back, &text, &n));
	CHECK(WF_OK ==
	        wf_draft_read(L"C:\\no\\such\\foo.txt", &back, &text, &n));
	CHECK(5 == n && !memcmp(text, "lower", 5)); /* still there */
	free(text);
	wf_draft_remove(L"C:\\no\\such\\foo.txt");

	/* an ordinary (case-insensitive) folder that exists: the same file */
	{
		wchar_t where[MAX_PATH + 80], spelled[MAX_PATH + 80];

		swprintf(where, WF_ARRAY_LEN(where), L"%ls\\here", dir);
		CHECK(CreateDirectoryW(dir, NULL) ||
		        ERROR_ALREADY_EXISTS == GetLastError());
		CHECK(CreateDirectoryW(where, NULL));
		swprintf(spelled, WF_ARRAY_LEN(spelled), L"%ls\\Note.txt",
		        where);
		CHECK(WF_OK == wf_draft_write(spelled, enc, "same", 4));
		swprintf(spelled, WF_ARRAY_LEN(spelled), L"%ls\\NOTE.TXT",
		        where);
		CHECK(WF_OK == wf_draft_read(spelled, &back, &text, &n));
		CHECK(4 == n && !memcmp(text, "same", 4));
		free(text);
		wf_draft_remove(spelled);
		CHECK(WF_E_NOENT == wf_draft_read(spelled, &back, &text, &n));
		RemoveDirectoryW(where);
	}
	remove_tree_files(dir);
	RemoveDirectoryW(dir);
	wf_draft_set_dir(NULL);
}

/* Changes `from` to `to` (as long) inside every file of `dir`, as if a draft of
 * another file had come to rest in this one's place. */
static void rename_inside_files(
        const wchar_t *dir, const char *from, const char *to)
{
	wchar_t file[MAX_PATH + 80];
	WIN32_FIND_DATAW fd;
	HANDLE h;

	swprintf(file, WF_ARRAY_LEN(file), L"%ls\\*", dir);
	h = FindFirstFileW(file, &fd);
	if (INVALID_HANDLE_VALUE == h) {
		return;
	}
	do {
		HANDLE f;
		char data[4096], *at;
		DWORD got = 0, put = 0;

		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			continue;
		}
		swprintf(file, WF_ARRAY_LEN(file), L"%ls\\%ls", dir,
		        fd.cFileName);
		f = CreateFileW(file, GENERIC_READ | GENERIC_WRITE, 0, NULL,
		        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (INVALID_HANDLE_VALUE == f) {
			continue;
		}
		if (ReadFile(f, data, sizeof data - 1, &got, NULL) && got) {
			data[got] = '\0';
			at = strstr(data, from);
			if (at) {
				memcpy(at, to, strlen(to));
				SetFilePointer(f, 0, NULL, FILE_BEGIN);
				WriteFile(f, data, got, &put, NULL);
			}
		}
		CloseHandle(f);
	} while (FindNextFileW(h, &fd));
	FindClose(h);
}

/* A draft that says it belongs to another path is not offered for this one,
 * whatever place it was found in. */
static void a_draft_of_another_path_is_not_offered(void)
{
	wchar_t dir[MAX_PATH + 40], where[MAX_PATH + 80], file[MAX_PATH + 80];
	WfEncoding enc = {WF_ENC_UTF8, 0}, back;
	char *text = NULL;
	size_t n = 0;

	if (!make_scratch(dir, WF_ARRAY_LEN(dir))) {
		CHECK_FAIL("cannot create an owned draft fixture");
		return;
	}
	wf_draft_set_dir(dir);
	CHECK(CreateDirectoryW(dir, NULL) ||
	        ERROR_ALREADY_EXISTS == GetLastError());
	swprintf(where, WF_ARRAY_LEN(where), L"%ls\\here", dir);
	CHECK(CreateDirectoryW(where, NULL));
	swprintf(file, WF_ARRAY_LEN(file), L"%ls\\Note.txt", where);
	CHECK(WF_OK == wf_draft_write(file, enc, "mine", 4));
	rename_inside_files(dir, "Note.txt", "Mote.txt");
	CHECK(WF_E_NOENT == wf_draft_read(file, &back, &text, &n));
	wf_draft_remove(file);
	remove_tree_files(dir);
	RemoveDirectoryW(where);
	RemoveDirectoryW(dir);
	wf_draft_set_dir(NULL);
}

/* Makes the empty folder `dir` case-sensitive (NTFS, Windows 10 and later; it
 * needs the right to change it and an NTFS volume). False where that cannot be
 * done: the test that needs it is then not run, and says so. */
static bool make_case_sensitive(const wchar_t *dir)
{
	FILE_CASE_SENSITIVE_INFO info = {FILE_CS_FLAG_CASE_SENSITIVE_DIR};
	HANDLE h =
	        CreateFileW(dir, FILE_WRITE_ATTRIBUTES | FILE_READ_ATTRIBUTES,
	                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	                NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	bool ok;
	DWORD error;

	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	ok = SetFileInformationByHandle(
	        h, FileCaseSensitiveInfo, &info, sizeof info);
	error = GetLastError();
	CloseHandle(h);
	SetLastError(error);
	return ok;
}

/* Exercise metadata failures even on volumes where enabling case sensitivity
 * is unavailable. Draft storage and deletion still use real files. */
static void case_metadata_controls_draft_identity(void)
{
	wchar_t dir[MAX_PATH + 40], upper[MAX_PATH + 90], lower[MAX_PATH + 90];
	WfEncoding enc = {WF_ENC_UTF8, 0}, back;
	const CaseProbe probes[] = {
	        CASE_SENSITIVE, CASE_UNKNOWN, CASE_INSENSITIVE};
	char *text = NULL;
	size_t n = 0;

	if (!make_scratch(dir, WF_ARRAY_LEN(dir))) {
		CHECK_FAIL("cannot create an owned draft fixture");
		return;
	}
	wf_draft_set_dir(dir);
	swprintf(upper, WF_ARRAY_LEN(upper), L"%ls\\Foo.txt", dir);
	swprintf(lower, WF_ARRAY_LEN(lower), L"%ls\\foo.txt", dir);
	for (size_t i = 0; i < WF_ARRAY_LEN(probes); ++i) {
		case_probe = probes[i];
		CHECK(WF_OK == wf_draft_write(upper, enc, "upper", 5));
		CHECK(WF_OK == wf_draft_write(lower, enc, "lower", 5));
		CHECK(WF_OK == wf_draft_read(upper, &back, &text, &n));
		CHECK(text && 5 == n &&
		        !memcmp(text,
		                CASE_INSENSITIVE == case_probe ? "lower"
		                                               : "upper",
		                5));
		free(text);
		text = NULL;
		wf_draft_remove(upper);
		if (CASE_INSENSITIVE == case_probe) {
			CHECK(WF_E_NOENT ==
			        wf_draft_read(lower, &back, &text, &n));
		} else {
			CHECK(WF_OK == wf_draft_read(lower, &back, &text, &n));
			CHECK(text && 5 == n && !memcmp(text, "lower", 5));
		}
		free(text);
		text = NULL;
		wf_draft_remove(lower);
		remove_tree_files(dir);
	}
	case_probe = CASE_NATIVE;
	wf_draft_set_dir(NULL);
	RemoveDirectoryW(dir);
}

/* In a folder that is case-sensitive, Foo.txt and foo.txt are two files, and
 * each keeps its own draft. */
static void in_a_case_sensitive_folder_each_spelling_has_its_own_draft(void)
{
	wchar_t dir[MAX_PATH + 40], where[MAX_PATH + 80], upper[MAX_PATH + 90],
	        lower[MAX_PATH + 90];
	WfEncoding enc = {WF_ENC_UTF8, 0}, back;
	char *text = NULL;
	size_t n = 0;

	if (!make_scratch(dir, WF_ARRAY_LEN(dir))) {
		CHECK_FAIL("cannot create an owned draft fixture");
		return;
	}
	wf_draft_set_dir(dir);
	CHECK(CreateDirectoryW(dir, NULL) ||
	        ERROR_ALREADY_EXISTS == GetLastError());
	swprintf(where, WF_ARRAY_LEN(where), L"%ls\\cs", dir);
	CHECK(CreateDirectoryW(where, NULL));
	if (!make_case_sensitive(where)) {
		printf("info: a case-sensitive folder cannot be made here "
		       "(Win32 %lu): "
		       "not run\n",
		        GetLastError());
	} else {
		swprintf(upper, WF_ARRAY_LEN(upper), L"%ls\\Foo.txt", where);
		swprintf(lower, WF_ARRAY_LEN(lower), L"%ls\\foo.txt", where);
		CHECK(WF_OK == wf_draft_write(upper, enc, "upper", 5));
		CHECK(WF_OK == wf_draft_write(lower, enc, "lower", 5));
		CHECK(WF_OK == wf_draft_read(upper, &back, &text, &n));
		CHECK(5 == n && !memcmp(text, "upper", 5));
		free(text);
		wf_draft_remove(lower);
		CHECK(WF_OK == wf_draft_read(upper, &back, &text, &n));
		CHECK(5 == n && !memcmp(text, "upper", 5));
		free(text);
		wf_draft_remove(upper);
	}
	remove_tree_files(dir);
	RemoveDirectoryW(where);
	RemoveDirectoryW(dir);
	wf_draft_set_dir(NULL);
}

int main(void)
{
	secret_name_exclusions();
	invalid_paths_are_rejected_before_access();
	a_draft_round_trips_and_is_replaced_and_removed();
	names_that_differ_in_case_keep_their_own_drafts();
	a_draft_of_another_path_is_not_offered();
	case_metadata_controls_draft_identity();
	in_a_case_sensitive_folder_each_spelling_has_its_own_draft();
	return TEST_RESULT();
}
