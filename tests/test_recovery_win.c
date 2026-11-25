/* Real objects in a new P0Lab per case. Altered/adopted fixtures are kept. */
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "../tools/p0/p0_journal.h"
#include "../tools/p0/p0_lab.h"
#include "path/wf_canonical_win.h"
#include "recovery/wf_recovery.h"
#include "wf_test.h"
#define CAP 2048
#define TXN L".wfile-txn-01000000-0000-0000-0000-000000000000"
typedef struct
{
	P0Lab lab;
	P0Journal journal;
	WfOwnershipRecord record;
	WfCanonicalPath canonical;
	wchar_t txn[CAP], owner[CAP], payload[CAP], final[CAP];
} Fixture;
static int identity(const wchar_t *path, FILE_ID_INFO *id)
{
	HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	int ok = INVALID_HANDLE_VALUE != h &&
	        GetFileInformationByHandleEx(h, FileIdInfo, id, sizeof *id);
	if (INVALID_HANDLE_VALUE != h) {
		CloseHandle(h);
	}
	return ok;
}
static int write_new(const wchar_t *path, const void *data, DWORD bytes)
{
	HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD written;
	int ok;
	if (INVALID_HANDLE_VALUE == h) {
		return 0;
	}
	ok = WriteFile(h, data, bytes, &written, NULL) && written == bytes &&
	        FlushFileBuffers(h);
	CloseHandle(h);
	return ok;
}
static int fixture(Fixture *f, int directory)
{
	HANDLE h;
	DWORD error;
	memset(f, 0, sizeof *f);
	if (!p0_lab_open(&f->lab, "recovery", 0)) {
		return 0;
	}
	if (!p0_lab_path(&f->lab, TXN, f->txn, CAP) ||
	        !p0_lab_path(&f->lab, TXN L"\\owner.bin", f->owner, CAP) ||
	        !p0_lab_path(&f->lab, TXN L"\\payload.bin", f->payload, CAP) ||
	        !p0_lab_path(&f->lab, L"final.bin", f->final, CAP)) {
		return 0;
	}
	f->record.intent_durable = 1;
	f->record.temp_id_durable = 2 == directory ? 0 : 1;
	f->record.intent_txn[0] = f->record.txn[0] = 1;
	f->record.nonce[0] = 77;
	f->record.intent_source_size = f->record.source_size = 4;
	f->record.rel_payload = TXN L"\\payload.bin";
	h = CreateFileW(f->lab.root, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return 0;
	}
	if (!wf_canonical_from_handle(h, &f->canonical, &error)) {
		CloseHandle(h);
		return 0;
	}
	CloseHandle(h);
	f->record.parent_path = f->canonical.path;
	if (2 == directory) {
		P0Intent intent = {0};
		wchar_t journal[CAP];
		memcpy(intent.txn, f->record.txn, 16);
		intent.src_size = f->record.source_size;
		wcscpy_s(intent.dest_parent, 260, f->canonical.path);
		wcscpy_s(intent.name, 128, L"final.bin");
		if (!p0_lab_path(&f->lab, L"live-journal.wjr", journal, CAP) ||
		        !p0j_open(&f->journal, journal) ||
		        !p0j_append(&f->journal, P0J_DIRECTORY_INTENT, &intent,
		                sizeof intent) ||
		        !p0j_flush(&f->journal)) {
			return 0;
		}
	}
	if (!CreateDirectoryW(f->txn, NULL) ||
	        !write_new(f->owner, f->record.nonce, 32) ||
	        (2 != directory &&
	                !(directory ? CreateDirectoryW(f->payload, NULL)
	                            : write_new(f->payload, "data", 4))) ||
	        !identity(f->lab.root, &f->record.parent) ||
	        !identity(f->txn, &f->record.directory) ||
	        !identity(f->owner, &f->record.owner) ||
	        (2 != directory && !identity(f->payload, &f->record.payload))) {
		return 0;
	}
	return 1;
}
static void keep(Fixture *f)
{
	p0j_close(&f->journal);
	wf_canonical_free(&f->canonical);
	CHECK(0 == p0_lab_close(&f->lab, 1));
}
static void residue_cases(void)
{
	Fixture f;
	WfOwnedReport report;
	WfRecoveryReport recovery;
	WfVerifiedOwned *token = NULL;
	wchar_t stranger[CAP];
	FILE_ID_INFO final_before, final_after;
	CHECK(fixture(&f, 0));
	CHECK(MoveFileW(f.payload, f.final));
	CHECK(identity(f.final, &final_before));
	CHECK(wf_owned_verify_committed(WF_OWNED_TXN_OWNER, &f.record,
	              L"missing.bin", 0, &token, &report) == WF_OWNED_MANUAL);
	CHECK(ERROR_FILE_NOT_FOUND == report.error && !token);
	CHECK(wf_owned_verify_committed(WF_OWNED_TXN_OWNER, &f.record,
	              L"final.bin", 0, &token, &report) == WF_OWNED_VERIFIED);
	CHECK(WF_OWNED_MANUAL == wf_owned_finish_committed(token, &report));
	CHECK(INVALID_FILE_ATTRIBUTES != GetFileAttributesW(f.owner));
	CHECK(WF_OWNED_DELETED == wf_owned_delete(token, &report));
	CHECK(INVALID_FILE_ATTRIBUTES == GetFileAttributesW(f.owner));
	{
		WfVerifiedOwned *reconstructed = NULL;
		f.record.nonce[0] ^= 1;
		CHECK(wf_owned_verify_committed(WF_OWNED_TXN_DIRECTORY,
		              &f.record, L"final.bin", 0, &reconstructed,
		              &report) == WF_OWNED_MANUAL);
		wf_owned_release(reconstructed);
		f.record.nonce[0] ^= 1;
	}
	CHECK(identity(f.final, &final_after));
	CHECK(!memcmp(&final_before, &final_after, sizeof final_before));
	CHECK(WF_RECOVERY_MANUAL ==
	        wf_recovery_inspect(&f.record, L"final.bin", 0, &recovery));
	CHECK(WF_OWNED_DELETED == wf_owned_finish_committed(token, &report));
	wf_owned_release(token);
	token = NULL;
	CHECK(INVALID_FILE_ATTRIBUTES == GetFileAttributesW(f.txn));
	CHECK(identity(f.final, &final_after));
	CHECK(!memcmp(&final_before, &final_after, sizeof final_before));
	keep(&f);
	CHECK(fixture(&f, 0));
	CHECK(MoveFileW(f.payload, f.final));
	CHECK(p0_lab_path(&f.lab, TXN L"\\unrelated.txt", stranger, CAP));
	CHECK(write_new(stranger, "USER", 4));
	CHECK(WF_RECOVERY_MANUAL ==
	        wf_recovery_inspect(&f.record, L"final.bin", 0, &recovery));
	CHECK(wf_owned_verify_committed(WF_OWNED_TXN_OWNER, &f.record,
	              L"final.bin", 0, &token, &report) == WF_OWNED_MANUAL);
	CHECK(!token &&
	        INVALID_FILE_ATTRIBUTES != GetFileAttributesW(stranger));
	CHECK(INVALID_FILE_ATTRIBUTES != GetFileAttributesW(f.owner));
	keep(&f);
	CHECK(fixture(&f, 0));
	CHECK(MoveFileW(f.payload, f.final));
	f.record.nonce[0] ^= 1;
	CHECK(WF_RECOVERY_MANUAL ==
	        wf_recovery_inspect(&f.record, L"final.bin", 0, &recovery));
	CHECK(INVALID_FILE_ATTRIBUTES != GetFileAttributesW(f.owner));
	keep(&f);
	CHECK(fixture(&f, 0));
	CHECK(write_new(f.final, "USER", 4));
	CHECK(wf_owned_verify_committed(WF_OWNED_TXN_OWNER, &f.record,
	              L"final.bin", 0, &token, &report) == WF_OWNED_MANUAL);
	CHECK(INVALID_FILE_ATTRIBUTES != GetFileAttributesW(f.payload) &&
	        INVALID_FILE_ATTRIBUTES != GetFileAttributesW(f.final));
	keep(&f);
	CHECK(fixture(&f, 0));
	CHECK(MoveFileW(f.payload, f.final));
	f.record.intent_source_size = f.record.source_size = 5;
	CHECK(WF_RECOVERY_MANUAL ==
	        wf_recovery_inspect(&f.record, L"final.bin", 0, &recovery));
	keep(&f);
	CHECK(fixture(&f, 0));
	CHECK(MoveFileW(f.payload, f.final));
	CHECK(wf_owned_verify_committed(WF_OWNED_TXN_OWNER, &f.record,
	              L"final.bin", 0, &token, &report) == WF_OWNED_VERIFIED);
	CHECK(WF_OWNED_DELETED == wf_owned_delete(token, &report));
	wf_owned_release(token);
	token = NULL;
	CHECK(wf_owned_verify_committed(WF_OWNED_TXN_DIRECTORY, &f.record,
	              L"final.bin", 0, &token, &report) == WF_OWNED_MANUAL);
	CHECK(WF_RECOVERY_MANUAL ==
	        wf_recovery_inspect(&f.record, L"final.bin", 0, &recovery));
	CHECK(GetFileAttributesW(f.txn) & FILE_ATTRIBUTE_DIRECTORY);
	keep(&f);
}
static void live_pack_id(uint8_t bytes[24], const FILE_ID_INFO *id)
{
	memcpy(bytes, &id->VolumeSerialNumber, 8);
	memcpy(bytes + 8, id->FileId.Identifier, 16);
}
static bool live_durable(void *context, const WfOwnershipRecord *candidate)
{
	Fixture *f = context;
	P0TempId temp = {0};
	FILE_ID_128 zero = {0};
	CHECK(!candidate->temp_id_durable &&
	        memcmp(&candidate->payload.FileId, &zero, sizeof zero));
	memcpy(temp.txn, candidate->txn, 16);
	memcpy(temp.nonce, candidate->nonce, 32);
	temp.src_size = candidate->source_size;
	temp.volume = candidate->parent.VolumeSerialNumber;
	wcscpy_s(temp.rel_payload, 128, candidate->rel_payload);
	live_pack_id(temp.parent_id, &candidate->parent);
	live_pack_id(temp.txn_dir_id, &candidate->directory);
	live_pack_id(temp.owner_id, &candidate->owner);
	live_pack_id(temp.payload_id, &candidate->payload);
	if (!p0j_append(&f->journal, P0J_TEMP_ID, &temp, sizeof temp) ||
	        !p0j_flush(&f->journal)) {
		return 0;
	}
	f->record.payload = candidate->payload;
	f->record.temp_id_durable = 1;
	return 1;
}
static bool live_refuse_temp_id(
        void *context, const WfOwnershipRecord *candidate)
{
	(void)context;
	(void)candidate;
	return 0;
}
static void live_cases(void)
{
	Fixture f;
	WfLiveManifest *manifest = NULL;
	WfOwnedReport report;
	size_t directory = 0, file = 0, deleted = 0;
	wchar_t stranger[CAP], known[CAP];
	CHECK(fixture(&f, 1));
	CHECK(wf_owned_live_begin(&f.record, live_durable, &f, &manifest,
	              &report) == WF_OWNED_MANUAL);
	CHECK(!manifest &&
	        INVALID_FILE_ATTRIBUTES != GetFileAttributesW(f.payload));
	keep(&f);
	CHECK(fixture(&f, 2));
	CHECK(wf_owned_live_begin(&f.record, live_durable, &f, &manifest,
	              &report) == WF_OWNED_VERIFIED);
	if (manifest) {
		CHECK(wf_owned_live_create(manifest, 0, L"nested", 1, NULL, 0,
		              0, &directory, &report) == WF_OWNED_VERIFIED);
		CHECK(wf_owned_live_create(manifest, directory, L"known.txt", 0,
		              "owned", 5, 5, &file,
		              &report) == WF_OWNED_VERIFIED);
		CHECK(wf_owned_live_create(manifest, 0, L"second.txt", 0,
		              "owned", 5, 5, &file,
		              &report) == WF_OWNED_VERIFIED);
		CHECK(wf_owned_live_create(manifest, 0, L"..\\bad", 0, "x", 1,
		              1, &file, &report) == WF_OWNED_MANUAL);
		CHECK(p0_lab_path(
		        &f.lab, TXN L"\\payload.bin\\USER.txt", stranger, CAP));
		CHECK(write_new(stranger, "USER", 4));
		CHECK(wf_owned_live_create(manifest, 0, L"USER.txt", 0, NULL, 0,
		              4, &file, &report) == WF_OWNED_MANUAL);
		CHECK(WF_OWNED_RECOVERY_REQUIRED ==
		        wf_owned_live_cleanup(manifest, &deleted, &report));
		CHECK(3 == deleted);
		CHECK(INVALID_FILE_ATTRIBUTES != GetFileAttributesW(stranger));
		CHECK(p0_lab_path(
		        &f.lab, TXN L"\\payload.bin\\nested", known, CAP));
		CHECK(INVALID_FILE_ATTRIBUTES == GetFileAttributesW(known));
		CHECK(GetFileAttributesW(f.payload) & FILE_ATTRIBUTE_DIRECTORY);
		wf_owned_live_release(manifest);
		manifest = NULL;
	}
	keep(&f);
	CHECK(fixture(&f, 1));
	CHECK(wf_owned_live_begin(&f.record, live_durable, &f, &manifest,
	              &report) == WF_OWNED_MANUAL);
	CHECK(!manifest &&
	        INVALID_FILE_ATTRIBUTES != GetFileAttributesW(f.payload));
	keep(&f);
	CHECK(fixture(&f, 2));
	CHECK(wf_owned_live_begin(&f.record, live_durable, &f, &manifest,
	              &report) == WF_OWNED_VERIFIED);
	if (manifest) {
		CHECK(wf_owned_live_create(manifest, 0, L"known.txt", 0,
		              "owned", 5, 5, &file,
		              &report) == WF_OWNED_VERIFIED);
		CHECK(WF_OWNED_DELETED ==
		        wf_owned_live_cleanup(manifest, &deleted, &report));
		CHECK(2 == deleted &&
		        INVALID_FILE_ATTRIBUTES ==
		                GetFileAttributesW(f.payload));
		CHECK(INVALID_FILE_ATTRIBUTES != GetFileAttributesW(f.owner));
		wf_owned_live_release(manifest);
	}
	keep(&f);
	CHECK(fixture(&f, 2));
	CHECK(wf_owned_live_begin(&f.record, live_refuse_temp_id, NULL,
	              &manifest, &report) == WF_OWNED_MANUAL);
	CHECK(!manifest &&
	        INVALID_FILE_ATTRIBUTES != GetFileAttributesW(f.payload));
	keep(&f);
}
int main(int argc, char **argv)
{
	Fixture f;
	WfRecoveryReport report;
	if (2 == argc && !strcmp(argv[1], "--live")) {
		live_cases();
		return TEST_RESULT();
	}
	if (2 == argc && !strcmp(argv[1], "--residue")) {
		residue_cases();
		return TEST_RESULT();
	}
	residue_cases();
	live_cases();
	CHECK(fixture(&f, 0));
	CHECK(WF_RECOVERY_CLEANUP_CANDIDATE ==
	        wf_recovery_inspect(&f.record, L"final.bin", 0, &report));
	CHECK(INVALID_FILE_ATTRIBUTES != GetFileAttributesW(f.payload));
	f.record.temp_id_durable = 0;
	CHECK(WF_RECOVERY_MANUAL ==
	        wf_recovery_inspect(&f.record, L"final.bin", 0, &report));
	f.record.temp_id_durable = 1;
	CHECK(MoveFileW(f.payload, f.final));
	CHECK(WF_RECOVERY_COMMITTED ==
	        wf_recovery_inspect(&f.record, L"final.bin", 0, &report));
	CHECK(INVALID_FILE_ATTRIBUTES != GetFileAttributesW(f.final));
	CHECK(WF_RECOVERY_MANUAL ==
	        wf_recovery_inspect(&f.record, L"..\\final.bin", 0, &report));
	keep(&f);
	CHECK(fixture(&f, 1));
	CHECK(WF_RECOVERY_MANUAL ==
	        wf_recovery_inspect(&f.record, L"final.bin", 1, &report));
	CHECK(MoveFileW(f.payload, f.final));
	CHECK(WF_RECOVERY_COMMITTED ==
	        wf_recovery_inspect(&f.record, L"final.bin", 1, &report));
	keep(&f);
	return TEST_RESULT();
}
