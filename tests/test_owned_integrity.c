#include <windows.h>

#include <bcrypt.h>
#include <string.h>

#include "../tools/p0/p0_journal.h"
#include "../tools/p0/p0_lab.h"
#include "fs/owned_integrity.h"
#include "path/wf_canonical_win.h"
#include "wf_test.h"

static int identity(const wchar_t *path, FILE_ID_INFO *id)
{
	HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	int ok;
	if (INVALID_HANDLE_VALUE == h) {
		return 0;
	}
	ok = GetFileInformationByHandleEx(h, FileIdInfo, id, sizeof *id);
	CloseHandle(h);
	return ok;
}
static int write_new(const wchar_t *path, const void *data, DWORD size)
{
	DWORD written;
	HANDLE h =
	        CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
	int ok;
	if (INVALID_HANDLE_VALUE == h) {
		return 0;
	}
	ok = WriteFile(h, data, size, &written, NULL) && written == size &&
	        FlushFileBuffers(h);
	CloseHandle(h);
	return ok;
}
static void pack(uint8_t out[24], const FILE_ID_INFO *id)
{
	memcpy(out, &id->VolumeSerialNumber, 8);
	memcpy(out + 8, id->FileId.Identifier, 16);
}
int main(void)
{
	P0Lab lab;
	WfOwnershipRecord record = {0};
	WfOwnedReport ownership;
	WfIntegrityReport integrity;
	WfIntegrityJob *job = NULL;
	WfVerifiedOwned *token = NULL;
	WfCanonicalPath canonical = {0};
	P0Journal journal;
	P0Intent intent = {0};
	P0TempId temp = {0};
	wchar_t source[1024], transaction[1024], owner[1024], payload[1024],
	        log[1024];
	const wchar_t *dir = L".wfile-txn-01010101-0101-0101-0101-010101010101";
	const wchar_t *relative =
	        L".wfile-txn-01010101-0101-0101-0101-010101010101\\payload.bin";
	FILE_ID_INFO source_id;
	HANDLE source_guard, parent;
	DWORD error, handles_before, handles_after;
	CHECK(p0_lab_open(&lab, "owned-integrity", 1));
	if (!lab.area) {
		return 1;
	}
	/* CNG lazily initializes process-wide provider state on first use. */
	CHECK(wf_integrity_begin(&job, &integrity));
	wf_integrity_end(&job);
	CHECK(p0_lab_path(&lab, L"source.txt", source, 1024));
	CHECK(write_new(source, "body", 4));
	CHECK(p0_area_track(lab.area, L"source.txt", 0));
	source_guard = CreateFileW(source, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	CHECK(GetFileInformationByHandleEx(
	        source_guard, FileIdInfo, &source_id, sizeof source_id));
	parent = CreateFileW(lab.root, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS, NULL);
	CHECK(wf_canonical_from_handle(parent, &canonical, &error));
	CloseHandle(parent);
	CHECK(identity(lab.root, &record.parent));
	memset(record.intent_txn, 1, 16);
	memcpy(record.txn, record.intent_txn, 16);
	CHECK(BCryptGenRandom(NULL, record.nonce, 32,
	              BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0);
	record.parent_path = canonical.path;
	record.rel_payload = relative;
	record.source_size = record.intent_source_size = 4;
	CHECK(p0_lab_path(&lab, L"intent.wjr", log, 1024));
	CHECK(p0j_open(&journal, log));
	CHECK(p0_area_track(lab.area, L"intent.wjr", 0));
	memcpy(intent.txn, record.txn, 16);
	intent.src_size = 4;
	wcscpy_s(intent.dest_parent, 260, canonical.path);
	wcscpy_s(intent.name, 128, L"final.txt");
	CHECK(p0j_append(&journal, P0J_INTENT, &intent, sizeof intent) &&
	        p0j_flush(&journal));
	record.intent_durable = 1;
	CHECK(p0_area_mkdir(lab.area, dir));
	CHECK(p0_lab_path(&lab, dir, transaction, 1024));
	swprintf(owner, 1024, L"%ls\\owner.bin", transaction);
	CHECK(write_new(owner, record.nonce, 32));
	CHECK(p0_area_track(lab.area,
	        L".wfile-txn-01010101-0101-0101-0101-010101010101\\owner.bin",
	        0));
	CHECK(p0_lab_path(&lab, relative, payload, 1024));
	/* Isolated fixture only: source pathname TOCTOU is NOT solved here. */
	CHECK(CopyFileExW(
	        source, payload, NULL, NULL, NULL, COPY_FILE_FAIL_IF_EXISTS));
	CHECK(p0_area_track(lab.area, relative, 0));
	CHECK(identity(transaction, &record.directory) &&
	        identity(owner, &record.owner) &&
	        identity(payload, &record.payload));
	printf("source_volume=%016llx payload_volume=%016llx source_id=",
	        (unsigned long long)source_id.VolumeSerialNumber,
	        (unsigned long long)record.payload.VolumeSerialNumber);
	for (unsigned i = 0; i < 16; i++) {
		printf("%02x", source_id.FileId.Identifier[i]);
	}
	printf(" payload_id=");
	for (unsigned i = 0; i < 16; i++) {
		printf("%02x", record.payload.FileId.Identifier[i]);
	}
	puts("");
	memcpy(temp.txn, record.txn, 16);
	memcpy(temp.nonce, record.nonce, 32);
	pack(temp.parent_id, &record.parent);
	pack(temp.txn_dir_id, &record.directory);
	pack(temp.owner_id, &record.owner);
	pack(temp.payload_id, &record.payload);
	temp.volume = record.parent.VolumeSerialNumber;
	temp.src_size = 4;
	wcscpy_s(temp.rel_payload, 128, relative);
	CHECK(p0j_append(&journal, P0J_TEMP_ID, &temp, sizeof temp) &&
	        p0j_flush(&journal));
	record.temp_id_durable = 1;
	/* Measure retained verification resources after CopyFileExW has
	 * initialized its process-wide worker machinery; source/journal stay
	 * open at both ends. */
	CHECK(GetProcessHandleCount(GetCurrentProcess(), &handles_before));
	CHECK(wf_owned_verify(WF_OWNED_COPY_PAYLOAD, &record, &token,
	              &ownership) == WF_OWNED_VERIFIED);
	CHECK(wf_integrity_begin(&job, &integrity));
	CHECK(wf_owned_integrity_compare(job, source_guard, &source_id, token,
	              NULL, NULL, &integrity,
	              &ownership) == WF_INTEGRITY_MATCH);
	{
		HANDLE writer = CreateFileW(payload, GENERIC_WRITE,
		        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		        NULL, OPEN_EXISTING, 0, NULL);
		CHECK(INVALID_HANDLE_VALUE == writer &&
		        ERROR_SHARING_VIOLATION == GetLastError());
		if (INVALID_HANDLE_VALUE != writer) {
			CloseHandle(writer);
		}
	}
	source_id.FileId.Identifier[0] ^= 1;
	CHECK(wf_owned_integrity_compare(job, source_guard, &source_id, token,
	              NULL, NULL, &integrity,
	              &ownership) == WF_INTEGRITY_CHANGED);
	CHECK(wf_owned_integrity_compare(job, source_guard, &source_id, NULL,
	              NULL, NULL, &integrity,
	              &ownership) == WF_INTEGRITY_CHANGED);
	wf_integrity_end(&job);
	wf_owned_release(token);
	/* Recovery verification creates a fresh key; never restores the old
	 * key. */
	source_id.FileId.Identifier[0] ^= 1;
	CHECK(wf_integrity_begin(&job, &integrity));
	CHECK(wf_owned_verify(WF_OWNED_COPY_PAYLOAD, &record, &token,
	              &ownership) == WF_OWNED_VERIFIED);
	CHECK(wf_owned_integrity_compare(job, source_guard, &source_id, token,
	              NULL, NULL, &integrity,
	              &ownership) == WF_INTEGRITY_MATCH);
	wf_integrity_end(&job);
	wf_owned_release(token);
	CHECK(GetProcessHandleCount(GetCurrentProcess(), &handles_after));
	printf("handles_before=%lu handles_after=%lu\n", handles_before,
	        handles_after);
	CHECK(handles_after == handles_before);
	CloseHandle(source_guard);
	p0j_close(&journal);
	wf_canonical_free(&canonical);
	CHECK(0 == p0_lab_close(&lab, 0));
	return TEST_RESULT();
}
