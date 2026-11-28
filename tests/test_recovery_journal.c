#define main recovery_cases_unused
#include "test_recovery_win.c"
#undef main
#include "../tools/p0/p0_journal.h"
#include <stddef.h>
static void packed_id(uint8_t out[24], const FILE_ID_INFO *id)
{
	memcpy(out, &id->VolumeSerialNumber, 8);
	memcpy(out + 8, id->FileId.Identifier, 16);
}
int main(void)
{
	Fixture f;
	P0Journal journal;
	P0Intent intent = {0};
	P0TempId temp = {0};
	uint8_t bytes[4096];
	DWORD got = 0;
	wchar_t path[CAP];
	HANDLE h;
	P0Scan scan;
	CHECK(fixture(&f, 0));
	memcpy(intent.txn, f.record.txn, 16);
	intent.src_size = f.record.source_size;
	wcscpy_s(intent.dest_parent, 260, f.record.parent_path);
	wcscpy_s(intent.name, 128, L"final.bin");
	memcpy(temp.txn, f.record.txn, 16);
	memcpy(temp.nonce, f.record.nonce, 32);
	temp.src_size = f.record.source_size;
	temp.volume = f.record.parent.VolumeSerialNumber;
	wcscpy_s(temp.rel_payload, 128, f.record.rel_payload);
	packed_id(temp.parent_id, &f.record.parent);
	packed_id(temp.txn_dir_id, &f.record.directory);
	packed_id(temp.owner_id, &f.record.owner);
	packed_id(temp.payload_id, &f.record.payload);
	CHECK(p0_lab_path(&f.lab, L"journal.wjr", path, CAP));
	CHECK(p0j_open(&journal, path));
	CHECK(p0j_append(&journal, P0J_INTENT, &intent, sizeof intent) &&
	        p0j_flush(&journal));
	CHECK(p0j_append(&journal, P0J_TEMP_ID, &temp, sizeof temp) &&
	        p0j_flush(&journal));
	{
		uint64_t written = journal.written;
		CHECK(!p0j_append(&journal, P0J_INTENT, NULL, 1));
		CHECK(!p0j_append(&journal, P0J_INTENT, &intent, UINT32_MAX));
		CHECK(written == journal.written);
	}
	p0j_close(&journal);
	h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, 0, NULL);
	CHECK(INVALID_HANDLE_VALUE != h &&
	        ReadFile(h, bytes, sizeof bytes, &got, NULL));
	CloseHandle(h);
	const size_t journal_size = 8 + 16 + sizeof intent + 16 + sizeof temp;
	if (got != journal_size || journal_size > sizeof bytes) {
		CHECK_FAIL("journal fixture is incomplete");
		keep(&f);
		return TEST_RESULT();
	}
	{
		P0Rec records[2];
		uint8_t changed[sizeof bytes];
		size_t n, offset;
		uint32_t crc, huge = UINT32_MAX;
		if (2 != p0j_parse(bytes, got, records, 2)) {
			CHECK_FAIL("journal fixture records are invalid");
			keep(&f);
			return TEST_RESULT();
		}
		CHECK(0 == p0j_parse(NULL, got, records, 2));
		CHECK(0 == p0j_parse(bytes, got, NULL, 2));
		CHECK(0 == p0j_parse(bytes, got, NULL, 0));
		for (n = 0; n < got; n++) {
			CHECK(p0j_parse(bytes, n, records, 2) < 2);
		}
		CHECK(2 == p0j_parse(bytes, got, records, 2));
		offset = (size_t)(records[1].data - bytes);
		memcpy(changed, bytes, got);
		memset(changed + offset + offsetof(P0TempId, rel_payload), 'x',
		        sizeof temp.rel_payload);
		crc = p0j_crc32(changed + offset - 4, 4 + sizeof temp);
		memcpy(changed + offset + sizeof temp, &crc, sizeof crc);
		p0r_scan(changed, got, f.lab.root, &scan);
		CHECK(P0S_MANUAL == scan.cls);
		CHECK(INVALID_FILE_ATTRIBUTES != GetFileAttributesW(f.payload));
		memcpy(changed, bytes, got);
		memcpy(changed + offset - 8, &huge, sizeof huge);
		CHECK(1 == p0j_parse(changed, got, records, 2));
		memcpy(changed, bytes, got);
		changed[journal_size - 1] ^= 1;
		CHECK(1 == p0j_parse(changed, got, records, 2));
	}
	p0r_scan(bytes, got, f.lab.root, &scan);
	CHECK(P0S_AUTO_CANDIDATE == scan.cls);
	CHECK(MoveFileW(f.payload, f.final));
	p0r_scan(bytes, got, f.lab.root, &scan);
	CHECK(P0S_COMMITTED == scan.cls);
	CHECK(INVALID_FILE_ATTRIBUTES != GetFileAttributesW(f.final));
	keep(&f);
	return TEST_RESULT();
}
