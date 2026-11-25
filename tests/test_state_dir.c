/*
 * Which state folder is used under %APPDATA% / %LOCALAPPDATA% (DR-DRAFT-017):
 * piero, else an existing wfile, else piero to be created; both existing means
 * piero is used and the older one is looked at for records that must not be
 * overlooked. Every folder here is made under a scratch folder of this test
 * and removed again; nothing outside it is touched. LOCALAPPDATA is pointed at
 * the scratch folder for the draft and decoder-stage cases and restored.
 */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/wf_state_dir.h"
#include "config/wf_config_win.h"
#include "edit/wf_edraft_win.h"
#include "preview/wf_dechost_client_win.h"
#include "recovery/wf_old_state.h"
#include "recovery/wf_recovery_store.h"
#include "wf_test.h"

static wchar_t scratch[MAX_PATH];

static void at(const wchar_t *base, const wchar_t *rel, wchar_t *out)
{
	swprintf(out, 1100, L"%ls\\%ls", base, rel);
}

static bool mk(const wchar_t *base, const wchar_t *rel)
{
	wchar_t p[1100];

	at(base, rel, p);
	return CreateDirectoryW(p, NULL) ||
	        ERROR_ALREADY_EXISTS == GetLastError();
}

static bool put(const wchar_t *base, const wchar_t *rel, const char *text)
{
	wchar_t p[1100];
	DWORD wrote;
	HANDLE h;
	bool ok;

	at(base, rel, p);
	h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	ok = WriteFile(h, text, (DWORD)strlen(text), &wrote, NULL) &&
	        wrote == strlen(text);
	CloseHandle(h);
	return ok;
}

static bool has(const wchar_t *base, const wchar_t *rel)
{
	wchar_t p[1100];

	at(base, rel, p);
	return INVALID_FILE_ATTRIBUTES != GetFileAttributesW(p);
}

static bool holds(const wchar_t *base, const wchar_t *rel, const char *text)
{
	wchar_t p[1100];
	char buf[256];
	DWORD got = 0;
	HANDLE h;
	bool ok;

	at(base, rel, p);
	h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	ok = ReadFile(h, buf, sizeof buf, &got, NULL) && got == strlen(text) &&
	        !memcmp(buf, text, got);
	CloseHandle(h);
	return ok;
}

/* A fresh folder inside the scratch folder. */
static void fresh(const wchar_t *name, wchar_t *out)
{
	at(scratch, name, out);
	CHECK(CreateDirectoryW(out, NULL));
}

/* Removes `dir` and what is in it: this test's own folder, never a link. */
static void remove_tree(const wchar_t *dir)
{
	wchar_t pattern[1100], child[1100];
	WIN32_FIND_DATAW fd;
	HANDLE h;

	swprintf(pattern, 1100, L"%ls\\*", dir);
	h = FindFirstFileW(pattern, &fd);
	if (INVALID_HANDLE_VALUE != h) {
		do {
			if (!wcscmp(fd.cFileName, L".") ||
			        !wcscmp(fd.cFileName, L"..")) {
				continue;
			}
			swprintf(child, 1100, L"%ls\\%ls", dir, fd.cFileName);
			if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
			        !(fd.dwFileAttributes &
			                FILE_ATTRIBUTE_REPARSE_POINT)) {
				remove_tree(child);
			} else if (fd.dwFileAttributes &
			        FILE_ATTRIBUTE_DIRECTORY) {
				RemoveDirectoryW(
				        child); /* a link: itself only */
			} else {
				SetFileAttributesW(
				        child, FILE_ATTRIBUTE_NORMAL);
				DeleteFileW(child);
			}
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	RemoveDirectoryW(dir);
}

static void the_folder_is_chosen_by_what_exists(void)
{
	wchar_t root[MAX_PATH];
	WfStateKind kind = WF_STATE_BOTH;
	DWORD error = 99;

	fresh(L"classify", root);
	CHECK(WF_OK == wf_state_classify(root, &kind, &error));
	CHECK(WF_STATE_FRESH == kind && !wcscmp(wf_state_name(kind), L"piero"));
	CHECK(mk(root, L"wfile"));
	CHECK(WF_OK == wf_state_classify(root, &kind, &error));
	CHECK(WF_STATE_LEGACY == kind &&
	        !wcscmp(wf_state_name(kind), L"wfile"));
	CHECK(mk(root, L"piero"));
	CHECK(WF_OK == wf_state_classify(root, &kind, &error));
	CHECK(WF_STATE_BOTH == kind && !wcscmp(wf_state_name(kind), L"piero"));
	/* the same without the older one */
	{
		wchar_t only[MAX_PATH];

		fresh(L"classify-new", only);
		CHECK(mk(only, L"piero"));
		CHECK(WF_OK == wf_state_classify(only, &kind, &error));
		CHECK(WF_STATE_PIERO == kind &&
		        !wcscmp(wf_state_name(kind), L"piero"));
	}
	/* a name that exists but is not a folder is never guessed around */
	{
		wchar_t odd[MAX_PATH];

		fresh(L"classify-file", odd);
		CHECK(put(odd, L"piero", "not a folder"));
		CHECK(WF_E_OS == wf_state_classify(odd, &kind, &error));
		CHECK(ERROR_DIRECTORY == error);
		CHECK(mk(odd, L"wfile"));
		CHECK(WF_E_OS == wf_state_classify(odd, &kind, &error));
	}
	/* a link in its place is named as a link, and is not followed */
	{
		wchar_t odd[MAX_PATH], cmd[2300], target[1100], link[1100];
		STARTUPINFOW si = {0};
		PROCESS_INFORMATION pi = {0};
		DWORD code = 99;

		fresh(L"classify-link", odd);
		CHECK(mk(odd, L"link-target"));
		at(odd, L"link-target", target);
		at(odd, L"piero", link);
		swprintf(cmd, 2300, L"cmd.exe /c mklink /J \"%ls\" \"%ls\"",
		        link, target);
		si.cb = sizeof si;
		if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
		            CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
			WaitForSingleObject(pi.hProcess, 10000);
			GetExitCodeProcess(pi.hProcess, &code);
			CloseHandle(pi.hThread);
			CloseHandle(pi.hProcess);
		}
		CHECK(0 == code);
		CHECK(WF_E_OS == wf_state_classify(odd, &kind, &error));
		CHECK(ERROR_REPARSE_POINT_ENCOUNTERED == error);
		CHECK(!has(
		        odd, L"link-target\\x")); /* nothing was made in it */
	}
	CHECK(WF_E_INVAL == wf_state_classify(NULL, &kind, &error));
	CHECK(WF_E_INVAL == wf_state_classify(root, NULL, &error));
	CHECK(WF_E_INVAL == wf_state_classify(L"", &kind, &error));
}

static void the_path_follows_the_choice_and_creates_nothing(void)
{
	wchar_t root[MAX_PATH], want[1100], sentinel_dir[MAX_PATH];
	wchar_t *path = NULL;
	WfStateKind kind = WF_STATE_BOTH;
	DWORD error = 0;

	fresh(L"paths", root);
	CHECK(SetEnvironmentVariableW(L"PIERO_TEST_STATE_ROOT", root));
	/* neither: piero, and the call makes nothing */
	CHECK(WF_OK ==
	        wf_startup_state_path(L"PIERO_TEST_STATE_ROOT", L"journal",
	                &path, &kind, &error));
	swprintf(want, 1100, L"%ls\\piero\\journal", root);
	CHECK(path && !_wcsicmp(path, want) && WF_STATE_FRESH == kind);
	free(path);
	CHECK(!has(root, L"piero") && !has(root, L"wfile"));
	/* the folder itself, no trailing separator */
	CHECK(WF_OK ==
	        wf_startup_state_path(
	                L"PIERO_TEST_STATE_ROOT", L"", &path, NULL, &error));
	swprintf(want, 1100, L"%ls\\piero", root);
	CHECK(path && !_wcsicmp(path, want));
	free(path);
	/* only the older one: it stays in use, with its content untouched */
	CHECK(mk(root, L"wfile"));
	swprintf(sentinel_dir, MAX_PATH, L"wfile\\journal");
	CHECK(mk(root, sentinel_dir));
	CHECK(put(root, L"wfile\\journal\\keep.txt", "old record"));
	CHECK(WF_OK ==
	        wf_startup_state_path(L"PIERO_TEST_STATE_ROOT", L"journal",
	                &path, &kind, &error));
	swprintf(want, 1100, L"%ls\\wfile\\journal", root);
	CHECK(path && !_wcsicmp(path, want) && WF_STATE_LEGACY == kind);
	free(path);
	CHECK(!has(root, L"piero"));
	/* both: piero, the older one named on request and left as it was */
	CHECK(mk(root, L"piero"));
	CHECK(WF_OK ==
	        wf_startup_state_path(L"PIERO_TEST_STATE_ROOT",
	                L"copy-journal.txt", &path, &kind, &error));
	swprintf(want, 1100, L"%ls\\piero\\copy-journal.txt", root);
	CHECK(path && !_wcsicmp(path, want) && WF_STATE_BOTH == kind);
	free(path);
	CHECK(WF_OK ==
	        wf_startup_legacy_path(
	                L"PIERO_TEST_STATE_ROOT", L"journal", &path, &error));
	swprintf(want, 1100, L"%ls\\wfile\\journal", root);
	CHECK(path && !_wcsicmp(path, want));
	free(path);
	CHECK(holds(root, L"wfile\\journal\\keep.txt", "old record"));
	/* an unset variable is an error, not a default */
	CHECK(SetEnvironmentVariableW(L"PIERO_TEST_STATE_ROOT", NULL));
	CHECK(WF_OK !=
	        wf_startup_state_path(L"PIERO_TEST_STATE_ROOT", L"journal",
	                &path, &kind, &error));
	CHECK(NULL == path);
}

static void records_left_in_the_older_folder_are_found(void)
{
	static const wchar_t *id = L"33333333-3333-3333-3333-333333333333";
	wchar_t root[MAX_PATH], journal[1100];
	WfRecoveryStore *store = NULL;
	WfRecoveryAbandonReport report;
	WfStateReport r;
	DWORD error = 0;

	fresh(L"old-records", root);
	CHECK(mk(root, L"wfile"));
	at(root, L"wfile\\journal", journal);
	/* nothing there: nothing found, nothing blocks */
	memset(&r, 0, sizeof r);
	wf_state_look_store(&r, journal);
	CHECK(!wf_state_report_blocks(&r) && !wf_state_report_any(&r));
	CHECK(wf_recovery_store_open(journal, true, &store, &error));
	if (!store) {
		return;
	}
	CHECK(wf_recovery_store_put(
	        store, id, L"REMOVED-TEST-VOLUME", "journal", 7, &error));
	memset(&r, 0, sizeof r);
	wf_state_look_store(&r, journal);
	CHECK(1 == r.unresolved && wf_state_report_blocks(&r));
	/* resolved by hand: the gate lifts */
	CHECK(wf_recovery_store_abandon(store, id, id, &report));
	wf_recovery_store_close(store);
	memset(&r, 0, sizeof r);
	wf_state_look_store(&r, journal);
	CHECK(0 == r.unresolved && !wf_state_report_blocks(&r));
}

static void anything_unreadable_blocks_and_notices_alone_do_not(void)
{
	wchar_t root[MAX_PATH], p[1100];
	WfStateReport r;

	fresh(L"old-unreadable", root);
	CHECK(mk(root, L"wfile"));
	/* a file where the journal folder should be: unknown means stopped */
	CHECK(put(root, L"wfile\\journal", "not a folder"));
	at(root, L"wfile\\journal", p);
	memset(&r, 0, sizeof r);
	wf_state_look_store(&r, p);
	CHECK(r.store_unknown && wf_state_report_blocks(&r));
	/* a receipt inventory that is not one: stopped as well */
	CHECK(put(
	        root, L"wfile\\copy-journal.txt", "garbage, not a journal\n"));
	at(root, L"wfile\\copy-journal.txt", p);
	memset(&r, 0, sizeof r);
	wf_state_look_copy_journal(&r, p);
	CHECK(wf_state_report_blocks(&r) && (r.copy_legacy || r.copy_invalid));
	/* a folder where the inventory file belongs cannot be read as one */
	CHECK(mk(root, L"wfile\\copy-journal.d"));
	at(root, L"wfile\\copy-journal.d", p);
	memset(&r, 0, sizeof r);
	wf_state_look_copy_journal(&r, p);
	CHECK(r.copy_invalid && wf_state_report_blocks(&r));
	/* drafts left behind: a notice, the files stay, nothing is stopped */
	CHECK(mk(root, L"wfile\\drafts"));
	at(root, L"wfile\\drafts", p);
	memset(&r, 0, sizeof r);
	wf_state_look_drafts(&r, p);
	CHECK(!r.drafts && !wf_state_report_any(&r)); /* empty folder */
	CHECK(put(root, L"wfile\\drafts\\a.draft", "unsaved"));
	memset(&r, 0, sizeof r);
	wf_state_look_drafts(&r, p);
	CHECK(r.drafts && wf_state_report_any(&r) &&
	        !wf_state_report_blocks(&r));
	CHECK(holds(root, L"wfile\\drafts\\a.draft", "unsaved"));
	/* a missing inventory and a missing drafts folder are nothing */
	at(root, L"wfile\\copy-journal.missing", p);
	memset(&r, 0, sizeof r);
	wf_state_look_copy_journal(&r, p);
	CHECK(!wf_state_report_any(&r));
	at(root, L"wfile\\drafts-missing", p);
	wf_state_look_drafts(&r, p);
	CHECK(!wf_state_report_any(&r));
}

static bool draft_lands_in(const wchar_t *root, const wchar_t *folder)
{
	wchar_t p[1100];
	WIN32_FIND_DATAW fd;
	HANDLE h;
	bool found = false;

	swprintf(p, 1100, L"%ls\\%ls\\drafts\\*", root, folder);
	h = FindFirstFileW(p, &fd);
	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	do {
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
			found = true;
		}
	} while (!found && FindNextFileW(h, &fd));
	FindClose(h);
	return found;
}

static void drafts_follow_the_same_choice(void)
{
	static const wchar_t *const cases[] = {
	        L"drafts-fresh", L"drafts-old", L"drafts-both"};
	wchar_t saved[MAX_PATH * 2], root[MAX_PATH];
	WfEncoding enc;
	char *text = NULL;
	size_t n = 0;
	DWORD got =
	        GetEnvironmentVariableW(L"LOCALAPPDATA", saved, MAX_PATH * 2);

	CHECK(got > 0 && got < MAX_PATH * 2);
	memset(&enc, 0, sizeof enc);
	enc.kind = WF_ENC_UTF8;
	wf_draft_set_dir(NULL);
	for (size_t i = 0; i < 3; i++) {
		fresh(cases[i], root);
		if (i) {
			CHECK(mk(root, L"wfile"));
		}
		if (0 == i || 2 == i) {
			CHECK(mk(root, L"piero"));
			if (0 == i) {
				/* "fresh" means piero only here: the other
				 * name must never be made */
			}
		}
		CHECK(SetEnvironmentVariableW(L"LOCALAPPDATA", root));
		CHECK(WF_OK ==
		        wf_draft_write(
		                L"C:\\Users\\me\\note.txt", enc, "unsaved", 7));
		CHECK(WF_OK ==
		        wf_draft_read(
		                L"C:\\Users\\me\\note.txt", &enc, &text, &n));
		CHECK(7 == n && text && !memcmp(text, "unsaved", 7));
		free(text);
		text = NULL;
		if (1 == i) {
			CHECK(draft_lands_in(root, L"wfile") &&
			        !has(root, L"piero"));
		} else {
			CHECK(draft_lands_in(root, L"piero"));
			CHECK(0 != i || !has(root, L"wfile"));
		}
		wf_draft_remove(L"C:\\Users\\me\\note.txt");
	}
	CHECK(SetEnvironmentVariableW(L"LOCALAPPDATA", saved));
}

static void the_decoder_host_is_staged_under_the_folder_in_use(void)
{
	wchar_t saved[MAX_PATH * 2], root[MAX_PATH], out[1100], want[64];
	const wchar_t *dirs[2] = {L"piero", L"wfile"};
	DWORD error = 0,
	      got = GetEnvironmentVariableW(
	              L"LOCALAPPDATA", saved, MAX_PATH * 2);

	CHECK(got > 0 && got < MAX_PATH * 2);
	for (size_t i = 0; i < 2; i++) {
		wchar_t source[1100];

		fresh(i ? L"stage-old" : L"stage-new", root);
		if (i) {
			CHECK(mk(root, L"wfile"));
		}
		CHECK(put(root, L"source.bin", "a program"));
		at(root, L"source.bin", source);
		CHECK(SetEnvironmentVariableW(L"LOCALAPPDATA", root));
		CHECK(WF_OK == wf_dechost_stage(source, out, 1100, &error));
		swprintf(want, 64, L"\\%ls\\worker\\", dirs[i]);
		CHECK(wcsstr(out, want));
		/* the staged copy is named piero.exe whichever folder holds it */
		CHECK(wcslen(out) > 10 &&
		        !_wcsicmp(out + wcslen(out) - 10, L"\\piero.exe"));
		CHECK(INVALID_FILE_ATTRIBUTES != GetFileAttributesW(out));
		CHECK(i ? !has(root, L"piero") : !has(root, L"wfile"));
	}
	CHECK(SetEnvironmentVariableW(L"LOCALAPPDATA", saved));
}

int main(void)
{
	wchar_t temp[MAX_PATH];
	DWORD n = GetTempPathW(MAX_PATH, temp);

	if (!n || n >= MAX_PATH) {
		return 1;
	}
	swprintf(scratch, MAX_PATH, L"%lspiero-state-test-%lu-%llu", temp,
	        GetCurrentProcessId(), (unsigned long long)GetTickCount64());
	if (!CreateDirectoryW(scratch, NULL)) {
		return 1;
	}
	the_folder_is_chosen_by_what_exists();
	the_path_follows_the_choice_and_creates_nothing();
	records_left_in_the_older_folder_are_found();
	anything_unreadable_blocks_and_notices_alone_do_not();
	drafts_follow_the_same_choice();
	the_decoder_host_is_staged_under_the_folder_in_use();
	remove_tree(scratch);
	CHECK(INVALID_FILE_ATTRIBUTES == GetFileAttributesW(scratch));
	return TEST_RESULT();
}
