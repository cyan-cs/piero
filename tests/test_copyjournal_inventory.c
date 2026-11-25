#include <windows.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../src/xfer/wf_copyjournal_win.h"
#include "../tools/p0/p0_lab.h"
#include "wf_test.h"

static bool write_bytes(const wchar_t *path, const char *bytes, DWORD size)
{
	HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD put = 0;
	if (file == INVALID_HANDLE_VALUE) {
		return false;
	}
	bool ok = WriteFile(file, bytes, size, &put, NULL) && put == size;
	CloseHandle(file);
	return ok;
}

static bool same_bytes(const wchar_t *path, const char *bytes, DWORD size)
{
	char buffer[1024];
	DWORD got = 0;
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		return false;
	}
	bool ok = ReadFile(file, buffer, sizeof buffer, &got, NULL) &&
	        got == size && !memcmp(buffer, bytes, size);
	CloseHandle(file);
	return ok;
}

/* Every path is inside a newly manifested scratch area. Receipts may describe
 * that victim, but neither legacy nor current records may authorize mutation. */
int main(void)
{
	P0Lab lab = {0};
	wchar_t log[1024], victim[1024], link[1024];
	char legacy[4096], utf8[3072], idtext[64];
	FILE_ID_INFO id = {0};
	WfCopyJournalInfo info;
	const char *records[] = {legacy, "PIERO-COPY-RECEIPTS 3\n",
	        "PIERO-COPY-RECEIPTS 2\nI 1 1 - x\n",
	        "PIERO-COPY-RECEIPTS 2\nI 1 1 - x",
	        "PIERO-COPY-RECEIPTS 2\nI 1 1 - bad%XZ\n",
	        "PIERO-COPY-RECEIPTS 2\nI 1 1 - \xff\n",
	        "PIERO-COPY-RECEIPTS 2\nX 1 1 - x\n",
	        ("PIERO-COPY-RECEIPTS 2\nA 1 1 "
	         "0000000000000000:00000000000000000000000000000000 x\n")};
	const bool valid[] = {
	        true, true, true, false, false, false, false, false};
	CHECK(p0_lab_open(&lab, "receipt-inventory", 1));
	if (!lab.area) {
		return 1;
	}
	CHECK(p0_lab_path(&lab, L"receipts.txt", log, 1024));
	CHECK(p0_lab_path(&lab, L"victim.txt", victim, 1024));
	CHECK(p0_lab_path(&lab, L"alias.txt", link, 1024));
	if (wf_test_failures) {
		p0_lab_close(&lab, 1);
		return 1;
	}
	CHECK(wf_cj_inspect(log, &info) && info.format == WF_CJ_MISSING);
	CHECK(write_bytes(victim, "PRESERVE", 8));
	HANDLE owned = CreateFileW(victim, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, 0, NULL);
	CHECK(owned != INVALID_HANDLE_VALUE);
	if (owned != INVALID_HANDLE_VALUE) {
		CHECK(GetFileInformationByHandleEx(
		        owned, FileIdInfo, &id, sizeof id));
		CloseHandle(owned);
	}
	CHECK(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, victim, -1,
	              utf8, sizeof utf8, NULL, NULL) > 0);
	if (wf_test_failures) {
		p0_lab_close(&lab, 1);
		return 1;
	}
	int n = snprintf(idtext, sizeof idtext,
	        "%016llx:", (unsigned long long)id.VolumeSerialNumber);
	for (size_t i = 0; i < 16; i++) {
		n += snprintf(idtext + n, sizeof idtext - (size_t)n, "%02x",
		        id.FileId.Identifier[i]);
	}
	CHECK(snprintf(legacy, sizeof legacy, "C 0 0 %s %s\n", idtext, utf8) >
	        0);
	for (size_t i = 0; i < sizeof records / sizeof records[0]; i++) {
		DWORD size = (DWORD)strlen(records[i]);
		CHECK(write_bytes(log, records[i], size));
		CHECK(wf_cj_inspect(log, &info) == valid[i]);
		CHECK(same_bytes(log, records[i], size));
		CHECK(same_bytes(victim, "PRESERVE", 8));
		WfCopyJournal *journal = wf_cj_open(log);
		CHECK((journal != NULL) == (i == 2));
		wf_cj_close(journal);
		CHECK(same_bytes(log, records[i], size));
	}
	CHECK(write_bytes(log, "", 0));
	WfCopyJournal *journal = wf_cj_open(log);
	CHECK(journal != NULL);
	if (journal) {
		CHECK(wf_cj_intent(journal, victim));
		CHECK(wf_cj_created(journal, victim, &id));
		CHECK(wf_cj_aside(journal, victim, victim, &id));
		CHECK(wf_cj_discard(journal, &id));
		CHECK(wf_cj_verified(journal, &id));
		CHECK(wf_cj_gone(journal, &id));
		wf_cj_close(journal);
		CHECK(wf_cj_inspect(log, &info));
		CHECK(info.format == WF_CJ_RECEIPTS && info.records == 6);
		CHECK(same_bytes(victim, "PRESERVE", 8));
	}
	/* A second name for the log is rejected without changing either name. */
	CHECK(CreateHardLinkW(link, log, NULL));
	CHECK(!wf_cj_inspect(log, &info));
	CHECK(info.error == ERROR_ACCESS_DENIED);
	journal = wf_cj_open(log);
	CHECK(journal == NULL);
	wf_cj_close(journal);
	CHECK(DeleteFileW(link));
	HANDLE file = CreateFileW(log, GENERIC_READ | GENERIC_WRITE,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0,
	        NULL);
	CHECK(file != INVALID_HANDLE_VALUE);
	if (file != INVALID_HANDLE_VALUE) {
		OVERLAPPED lock = {0};
		CHECK(LockFileEx(
		        file, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, 0, &lock));
		CHECK(!wf_cj_inspect(log, &info));
		CHECK(info.error == ERROR_LOCK_VIOLATION);
		CHECK(UnlockFileEx(file, 0, MAXDWORD, 0, &lock));
		LARGE_INTEGER end;
		end.QuadPart = (16LL << 20) + 1;
		CHECK(SetFilePointerEx(file, end, NULL, FILE_BEGIN));
		CHECK(SetEndOfFile(file));
		CloseHandle(file);
		CHECK(!wf_cj_inspect(log, &info));
		CHECK(info.error == ERROR_FILE_TOO_LARGE);
	}
	CHECK(same_bytes(victim, "PRESERVE", 8));
	CHECK(0 == p0_lab_close(&lab, 1));
	return TEST_RESULT();
}
