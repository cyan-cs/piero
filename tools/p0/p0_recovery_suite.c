/* S12/S13: deterministic process termination with the real shared classifier.
 * OS caches survive these runs. This collector makes no power-loss claim. */
#include <stddef.h>
#include <string.h>

#include "p0_common.h"
#include "p0_journal.h"
#include "p0_lab.h"
#include "path/wf_canonical_win.h"
#define CAP 4096
#define CRASH_EXIT 0xC0DE0013u
#define TXN L".wfile-txn-01000000-0000-0000-0000-000000000000"
static const char *file_points[] = {"AFTER_INTENT_FLUSH", "AFTER_TEMP_ID_FLUSH",
        "MID_COPY", "AFTER_COPY_FINISH", "AFTER_STRUCTURAL_VERIFY",
        "AFTER_DATA_FLUSH", "BEFORE_RENAME", "AFTER_RENAME",
        "BEFORE_COMMITTED_FLUSH", "AFTER_COMMITTED_FLUSH"};
static const char *directory_points[] = {"AFTER_TXN_CREATE", "MID_CHILD_COPY",
        "AFTER_ALL_CHILD_COPIES", "AFTER_DURABILITY_BARRIER",
        "AFTER_COMMIT_BEFORE_COMPLETION"};
static void crash_at(int selected, int current)
{
	if (selected == current) {
		TerminateProcess(GetCurrentProcess(), CRASH_EXIT);
	}
}
static void data_pattern(uint8_t bytes[65536])
{
	size_t i;
	for (i = 0; i < 65536; i++) {
		bytes[i] = (uint8_t)('a' + i % 23);
	}
}
static int id_at(const wchar_t *path, uint8_t id[24])
{
	HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	int ok = INVALID_HANDLE_VALUE != h && p0j_id_of_handle(h, id);
	if (INVALID_HANDLE_VALUE != h) {
		CloseHandle(h);
	}
	return ok;
}
static int child(const wchar_t *root, int directory, int point)
{
	wchar_t journal_path[CAP], txn[CAP], owner[CAP], payload[CAP],
	        final[CAP], child_path[CAP];
	P0Journal journal;
	P0Intent intent = {0};
	P0TempId temp = {0};
	WfCanonicalPath canonical = {0};
	HANDLE parent, h, data;
	DWORD error, wrote;
	uint8_t bytes[65536];
	FILE_STANDARD_INFO standard;
	FILE_RENAME_INFO *rename;
	size_t rename_bytes;
	parent = CreateFileW(root, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	if (INVALID_HANDLE_VALUE == parent ||
	        !wf_canonical_from_handle(parent, &canonical, &error) ||
	        !p0j_id_of_handle(parent, temp.parent_id) ||
	        wcslen(canonical.path) >= 260) {
		return 11;
	}
	intent.txn[0] = temp.txn[0] = 1;
	intent.job[0] = 1;
	intent.src_size = temp.src_size = 65536;
	temp.volume = canonical.volume;
	wcscpy_s(intent.dest_parent, 260, canonical.path);
	wf_canonical_free(&canonical);
	wcscpy_s(intent.name, 128, L"final.bin");
	wcscpy_s(temp.rel_payload, 128, TXN L"\\payload.bin");
	temp.nonce[0] = 83;
	swprintf(journal_path, CAP, L"%ls\\journal.wjr", root);
	swprintf(txn, CAP, L"%ls\\%ls", root, TXN);
	swprintf(owner, CAP, L"%ls\\owner.bin", txn);
	swprintf(payload, CAP, L"%ls\\payload.bin", txn);
	swprintf(final, CAP, L"%ls\\final.bin", root);
	if (!p0j_open(&journal, journal_path) ||
	        !p0j_append(&journal,
	                directory ? P0J_DIRECTORY_INTENT : P0J_INTENT, &intent,
	                sizeof intent) ||
	        !p0j_flush(&journal)) {
		return 12;
	}
	if (!directory) {
		crash_at(point, 0);
	}
	if (!CreateDirectoryW(txn, NULL)) {
		return 13;
	}
	h = CreateFileW(owner, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == h ||
	        !WriteFile(h, temp.nonce, 32, &wrote, NULL) || 32 != wrote ||
	        !FlushFileBuffers(h)) {
		return 14;
	}
	CloseHandle(h);
	if (!id_at(txn, temp.txn_dir_id) || !id_at(owner, temp.owner_id)) {
		return 15;
	}
	if (directory) {
		crash_at(point, 0);
	}
	if (directory && !CreateDirectoryW(payload, NULL)) {
		return 16;
	}
	h = CreateFileW(payload,
	        DELETE | FILE_READ_ATTRIBUTES |
	                (directory ? FILE_LIST_DIRECTORY : GENERIC_WRITE),
	        FILE_SHARE_READ, NULL, directory ? OPEN_EXISTING : CREATE_NEW,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	if (INVALID_HANDLE_VALUE == h ||
	        !p0j_id_of_handle(h, temp.payload_id) ||
	        !p0j_append(&journal, P0J_TEMP_ID, &temp, sizeof temp) ||
	        !p0j_flush(&journal)) {
		return 17;
	}
	if (!directory) {
		crash_at(point, 1);
	}
	data = h;
	if (directory) {
		swprintf(child_path, CAP, L"%ls\\child.bin", payload);
		data = CreateFileW(child_path, GENERIC_WRITE, FILE_SHARE_READ,
		        NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
		if (INVALID_HANDLE_VALUE == data) {
			return 18;
		}
	}
	data_pattern(bytes);
	if (!WriteFile(data, bytes, 32768, &wrote, NULL) || 32768 != wrote) {
		return 19;
	}
	crash_at(point, directory ? 1 : 2);
	if (!WriteFile(data, bytes + 32768, 32768, &wrote, NULL) ||
	        32768 != wrote) {
		return 20;
	}
	crash_at(point, directory ? 2 : 3);
	if (!GetFileInformationByHandleEx(
	            data, FileStandardInfo, &standard, sizeof standard) ||
	        65536 != standard.EndOfFile.QuadPart) {
		return 21;
	}
	if (!directory) {
		crash_at(point, 4);
	}
	if (!FlushFileBuffers(data)) {
		return 22;
	}
	if (directory) {
		CloseHandle(data);
	}
	crash_at(point, directory ? 3 : 5);
	if (!directory) {
		crash_at(point, 6);
	}
	rename_bytes =
	        sizeof(FILE_RENAME_INFO) + wcslen(final) * sizeof(wchar_t);
	rename = calloc(1, rename_bytes);
	if (!rename) {
		return 23;
	}
	rename->ReplaceIfExists = FALSE;
	rename->FileNameLength = (DWORD)(wcslen(final) * sizeof(wchar_t));
	memcpy(rename->FileName, final, rename->FileNameLength);
	if (!SetFileInformationByHandle(
	            h, FileRenameInfo, rename, (DWORD)rename_bytes)) {
		return 24;
	}
	free(rename);
	crash_at(point, directory ? 4 : 7);
	if (!p0j_append(&journal, P0J_COMMITTED, temp.txn, 16)) {
		return 25;
	}
	crash_at(point, 8);
	if (!p0j_flush(&journal)) {
		return 26;
	}
	crash_at(point, 9);
	return 27;
}
static int one_case(const wchar_t *exe, int directory, int point, FILE *csv)
{
	P0Lab lab;
	wchar_t command[CAP * 3], journal[CAP], final[CAP];
	STARTUPINFOW startup = {0};
	PROCESS_INFORMATION process = {0};
	HANDLE h;
	DWORD exit_code = 0, got = 0;
	uint8_t bytes[8192], expected_bytes[65536];
	P0Scan scan;
	P0ScanClass expected;
	char expected_hash[65], final_hash[65] = "absent";
	P0Hash *hash;
	int ok = 0;
	uint8_t before[24], after[24];
	int final_exists;
	if (!p0_lab_open(&lab, directory ? "s12-process" : "s13-process", 0)) {
		return 0;
	}
	if (!p0_lab_path(&lab, L"journal.wjr", journal, CAP) ||
	        !p0_lab_path(&lab, L"final.bin", final, CAP)) {
		goto done;
	}
	swprintf(command, CAP * 3, L"\"%ls\" --child \"%ls\" %d %d", exe,
	        lab.root, directory, point);
	startup.cb = sizeof startup;
	if (!CreateProcessW(exe, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
	            NULL, NULL, &startup, &process)) {
		goto done;
	}
	if (WAIT_OBJECT_0 != WaitForSingleObject(process.hProcess, 15000)) {
		TerminateProcess(process.hProcess, 99);
	}
	GetExitCodeProcess(process.hProcess, &exit_code);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	h = CreateFileW(journal, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		goto done;
	}
	if (!ReadFile(h, bytes, sizeof bytes, &got, NULL)) {
		CloseHandle(h);
		goto done;
	}
	CloseHandle(h);
	final_exists = id_at(final, before);
	p0r_scan(bytes, got, lab.root, &scan);
	expected = directory ? (4 == point ? P0S_COMMITTED : P0S_MANUAL)
	        : 0 == point ? P0S_END
	        : point >= 7 ? P0S_COMMITTED
	                     : P0S_AUTO_CANDIDATE;
	data_pattern(expected_bytes);
	hash = p0_hash_begin();
	if (!hash) {
		goto done;
	}
	p0_hash_update(hash, expected_bytes, sizeof expected_bytes);
	p0_hash_end(hash, expected_hash);
	if (directory && final_exists) {
		wcscat_s(final, CAP, L"\\child.bin");
	}
	if (final_exists && !p0_sha256_file(final, final_hash)) {
		goto done;
	}
	ok = CRASH_EXIT == exit_code && scan.cls == expected &&
	        (P0S_COMMITTED != expected ||
	                (final_exists && !strcmp(final_hash, expected_hash)));
	if (directory && final_exists) {
		*wcsrchr(final, L'\\') = 0;
	}
	if (final_exists &&
	        (!id_at(final, after) || memcmp(before, after, 24))) {
		ok = 0;
	}
	fprintf(csv, "%s,%s,%s,%s,%lu,%lu,%s,%s,%s\n",
	        directory ? "P0-S12" : "P0-S13",
	        directory ? directory_points[point] : file_points[point],
	        p0r_class_name(expected), p0r_class_name(scan.cls), exit_code,
	        got, expected_hash, final_hash, ok ? "PASS" : "FAIL");
	fflush(csv);
	printf("%s %s: %s (%s)\n", directory ? "S12" : "S13",
	        directory ? directory_points[point] : file_points[point],
	        ok ? "PASS" : "FAIL", scan.reason);
done:
	p0_lab_close(&lab, 1);
	return ok;
}
int wmain(int argc, wchar_t **argv)
{
	wchar_t exe[CAP], run_dir[CAP], path[CAP];
	const wchar_t *root, *run;
	const char *const empty[] = {NULL};
	char run8[128], filesystem[64];
	wchar_t volume[CAP], fs[32];
	int directory, point, failures = 0;
	if (5 == argc && !wcscmp(argv[1], L"--child")) {
		return child(argv[2], _wtoi(argv[3]), _wtoi(argv[4]));
	}
	if (3 != argc) {
		fputs("usage: p0_recovery_suite <results-root> "
		      "<unique-run-id>\n",
		        stderr);
		return 2;
	}
	root = argv[1];
	run = argv[2];
	if (!*run || wcslen(run) > 96 ||
	        wcsspn(run,
	                L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0"
	                L"123456789-_") != wcslen(run)) {
		return 2;
	}
	if (!GetModuleFileNameW(NULL, exe, CAP) ||
	        !WideCharToMultiByte(
	                CP_UTF8, 0, run, -1, run8, sizeof run8, NULL, NULL)) {
		return 2;
	}
	if (!GetVolumePathNameW(exe, volume, CAP) ||
	        !GetVolumeInformationW(
	                volume, NULL, 0, NULL, NULL, NULL, fs, 32) ||
	        !WideCharToMultiByte(CP_UTF8, 0, fs, -1, filesystem,
	                sizeof filesystem, NULL, NULL)) {
		return 2;
	}
	for (directory = 0; directory < 2; directory++) {
		wchar_t group[CAP];
		FILE *csv;
		int group_failures = 0;
		const char *test = directory ? "P0-S12" : "P0-S13";
		swprintf(group, CAP, L"%ls\\%hs", root, test);
		if (!p0_mkdirs(group)) {
			return 2;
		}
		swprintf(run_dir, CAP, L"%ls\\%ls", group, run);
		if (!CreateDirectoryW(run_dir, NULL)) {
			fputs("refusing existing evidence run\n", stderr);
			return 2;
		}
		swprintf(path, CAP, L"%ls\\environment.json", run_dir);
		if (!p0_write_environment(
		            path, exe, "p0_recovery_suite", "1")) {
			return 2;
		}
		swprintf(path, CAP, L"%ls\\cases.csv", run_dir);
		csv = p0_fopen_w(path);
		if (!csv) {
			return 2;
		}
		fputs("test_id,crash_point,expected,actual,child_exit,journal_"
		      "bytes,expected_sha256,final_sha256,result\n",
		        csv);
		for (point = 0; point < (directory ? 5 : 10); point++) {
			if (!one_case(exe, directory, point, csv)) {
				group_failures++;
			}
		}
		fclose(csv);
		failures += group_failures;
		swprintf(path, CAP, L"%ls\\run.json", run_dir);
		if (!p0_write_run_json(path, test, run8, "C17-/W4-/WX",
		            filesystem, empty, group_failures ? "FAIL" : "PASS",
		            empty,
		            "Deterministic TerminateProcess only; OS caches "
		            "survive. Not VM or power-loss evidence. Fresh "
		            "local scratch areas retained; shared read-only "
		            "classifier, committed SHA-256 verified.")) {
			return 2;
		}
	}
	return failures ? 1 : 0;
}
