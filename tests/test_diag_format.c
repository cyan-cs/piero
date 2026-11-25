#include "diagnostics/wf_diag_format.h"
#include "wf_test.h"
int main(void)
{
	unsigned char bytes[16];
	WfDiagDiskRecord record = {4, 0x4788814eu}; /* CRC32("abc\n") */
	WfDiagDiskHeader header = {0};
	CHECK(0xcbf43926u == wf_diag_crc32("123456789", 9));
	/* Literal CRC from independent IEEE CRC32 calculator. */
	record.crc32 = 0x4788814eu;
	memcpy(bytes, &record, sizeof record);
	memcpy(bytes + sizeof record, "abc\n", 4);
	CHECK(wf_diag_records_valid(bytes, 12));
	bytes[8] = 'x';
	CHECK(!wf_diag_records_valid(bytes, 12));
	CHECK(!wf_diag_records_valid(bytes, 11));
	CHECK(wf_diag_records_valid(NULL, 0));
	header.path_units = 3;
	header.crc32 = wf_diag_header_crc(&header, L"abc");
	CHECK(header.crc32 != wf_diag_header_crc(&header, L"abd"));
	return TEST_RESULT();
}
