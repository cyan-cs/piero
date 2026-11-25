/* Test-only API interception. Keys/digests are inspected in memory only. */
#include <windows.h>

#include <bcrypt.h>
#include <stdlib.h>
#include <string.h>

#include "../tools/p0/p0_lab.h"
#include "wf_test.h"
static int fault, reads, creations, destroys, opens, closes, erase_checks, ids;
static void *erased_job;
static size_t erased_bytes;
static UCHAR observed_key[32];
static NTSTATUS fail_status(void)
{
	return (NTSTATUS)0xC0000001L;
}
static NTSTATUS WINAPI random_call(
        BCRYPT_ALG_HANDLE h, PUCHAR p, ULONG n, ULONG f)
{
	return 1 == fault ? fail_status() : BCryptGenRandom(h, p, n, f);
}
static NTSTATUS WINAPI open_call(
        BCRYPT_ALG_HANDLE *h, LPCWSTR a, LPCWSTR p, ULONG f)
{
	NTSTATUS s;
	if (2 == fault) {
		return fail_status();
	}
	s = BCryptOpenAlgorithmProvider(h, a, p, f);
	if (s >= 0) {
		opens++;
	}
	CHECK(BCRYPT_ALG_HANDLE_HMAC_FLAG == f);
	return s;
}
static NTSTATUS WINAPI property_call(
        BCRYPT_HANDLE h, LPCWSTR p, PUCHAR b, ULONG n, ULONG *got, ULONG f)
{
	return 3 == fault ? fail_status()
	                  : BCryptGetProperty(h, p, b, n, got, f);
}
static NTSTATUS WINAPI create_call(BCRYPT_ALG_HANDLE a,
        BCRYPT_HASH_HANDLE *h,
        PUCHAR b,
        ULONG n,
        PUCHAR key,
        ULONG key_n,
        ULONG f)
{
	NTSTATUS s;
	CHECK(32 == key_n && !memcmp(key, observed_key, 32));
	if (4 == fault) {
		return fail_status();
	}
	s = BCryptCreateHash(a, h, b, n, key, key_n, f);
	if (s >= 0) {
		creations++;
	}
	return s;
}
static NTSTATUS WINAPI hash_call(
        BCRYPT_HASH_HANDLE h, PUCHAR b, ULONG n, ULONG f)
{
	return 5 == fault ? fail_status() : BCryptHashData(h, b, n, f);
}
static NTSTATUS WINAPI finish_call(
        BCRYPT_HASH_HANDLE h, PUCHAR b, ULONG n, ULONG f)
{
	return 6 == fault ? fail_status() : BCryptFinishHash(h, b, n, f);
}
static NTSTATUS WINAPI destroy_call(BCRYPT_HASH_HANDLE h)
{
	if (11 == fault) {
		return fail_status();
	}
	destroys++;
	return BCryptDestroyHash(h);
}
static NTSTATUS WINAPI close_call(BCRYPT_ALG_HANDLE h, ULONG f)
{
	if (12 == fault) {
		return fail_status();
	}
	closes++;
	return BCryptCloseAlgorithmProvider(h, f);
}
static BOOL WINAPI information_call(
        HANDLE h, FILE_INFO_BY_HANDLE_CLASS kind, LPVOID out, DWORD size)
{
	BOOL ok = GetFileInformationByHandleEx(h, kind, out, size);
	if (ok && kind == FileIdInfo && 10 == fault && ++ids >= 3) {
		((FILE_ID_INFO *)out)->FileId.Identifier[0] ^= 1;
	}
	return ok;
}
static BOOL WINAPI read_call(
        HANDLE h, LPVOID b, DWORD n, LPDWORD got, LPOVERLAPPED o)
{
	reads++;
	if (7 == fault && 2 == reads) {
		SetLastError(ERROR_READ_FAULT);
		return FALSE;
	}
	if (8 == fault && 2 == reads) {
		*got = 0;
		return TRUE;
	}
	if (9 == fault && n > 7) {
		n = 7;
	}
	return ReadFile(h, b, n, got, o);
}
static void checked_free(void *p)
{
	if (p == erased_job && p) {
		/* job object size is supplied below once its definition is
		 * visible. */
		const UCHAR *bytes = p;
		size_t i;
		for (i = 0; i < erased_bytes; i++) {
			CHECK(0 == bytes[i]);
		}
		erase_checks++;
		erased_job = NULL;
	}
	free(p);
}
#define BCryptGenRandom random_call
#define BCryptOpenAlgorithmProvider open_call
#define BCryptGetProperty property_call
#define BCryptCreateHash create_call
#define BCryptHashData hash_call
#define BCryptFinishHash finish_call
#define BCryptDestroyHash destroy_call
#define BCryptCloseAlgorithmProvider close_call
#define ReadFile read_call
#define GetFileInformationByHandleEx information_call
#define free checked_free
#include "../src/xfer/wf_integrity_win.c"
#undef ReadFile
#undef GetFileInformationByHandleEx
#undef free
#undef BCryptGenRandom
#undef BCryptOpenAlgorithmProvider
#undef BCryptGetProperty
#undef BCryptCreateHash
#undef BCryptHashData
#undef BCryptFinishHash
#undef BCryptDestroyHash
#undef BCryptCloseAlgorithmProvider

static HANDLE make(P0Lab *lab, const wchar_t *name, FILE_ID_INFO *id)
{
	wchar_t path[1024];
	DWORD i, written;
	UCHAR block[65536];
	HANDLE h;
	memset(block, 0xA7, sizeof block);
	CHECK(p0_lab_path(lab, name, path, 1024));
	h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	for (i = 0; i < 4; i++) {
		CHECK(WriteFile(h, block, sizeof block, &written, NULL) &&
		        written == sizeof block);
	}
	CloseHandle(h);
	CHECK(p0_area_track(lab->area, name, 0));
	h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, 0, NULL);
	CHECK(GetFileInformationByHandleEx(h, FileIdInfo, id, sizeof *id));
	return h;
}
static bool cancel_read(void *ctx)
{
	(void)ctx;
	return reads >= 2;
}
int main(void)
{
	P0Lab lab;
	WfIntegrityJob *j = NULL, *other = NULL;
	WfIntegrityReport r;
	FILE_ID_INFO a, b;
	HANDLE ha, hb;
	int i;
	CHECK(p0_lab_open(&lab, "integrity-fault", 0));
	if (!lab.area) {
		return 1;
	}
	ha = make(&lab, L"source", &a);
	hb = make(&lab, L"copy", &b);
	for (fault = 1; fault <= 3; fault++) {
		CHECK(!wf_integrity_begin(&j, &r) && NULL == j &&
		        r.cng_status < 0);
	}
	fault = 0;
	CHECK(wf_integrity_begin(&j, &r));
	CHECK(wf_integrity_begin(&other, &r));
	if (!j || !other) {
		return 1;
	}
	CHECK(0 != memcmp(j->key, other->key, 32));
	memcpy(observed_key, j->key, 32);
	for (fault = 4; fault <= 8; fault++) {
		reads = 0;
		CHECK(wf_integrity_compare(j, ha, &a, hb, &b, 262144, NULL,
		              NULL, &r) != WF_INTEGRITY_MATCH);
		if (fault <= 6) {
			CHECK(r.cng_status < 0);
		}
		if (7 == fault) {
			CHECK(ERROR_READ_FAULT == r.error);
		}
	}
	fault = 9;
	reads = 0;
	CHECK(WF_INTEGRITY_MATCH ==
	        wf_integrity_compare(
	                j, ha, &a, hb, &b, 262144, NULL, NULL, &r));
	fault = 0;
	reads = 0;
	CHECK(wf_integrity_compare(j, ha, &a, hb, &b, 262144, cancel_read, NULL,
	              &r) == WF_INTEGRITY_CANCELLED);
	CHECK(wf_integrity_compare(j, ha, &a, hb, &b, UINT64_MAX, NULL, NULL,
	              &r) == WF_INTEGRITY_ERROR);
	fault = 10;
	ids = 0;
	CHECK(WF_INTEGRITY_CHANGED ==
	        wf_integrity_compare(
	                j, ha, &a, hb, &b, 262144, NULL, NULL, &r));
	fault = 0;
	/* RFC 4231 test case 1: fixed key is strictly test-local. */
	{
		static const UCHAR expected_digest[32] = {0xb0, 0x34, 0x4c,
		        0x61, 0xd8, 0xdb, 0x38, 0x53, 0x5c, 0xa8, 0xaf, 0xce,
		        0xaf, 0x0b, 0xf1, 0x2b, 0x88, 0x1d, 0xc2, 0x00, 0xc9,
		        0x83, 0x3d, 0xa7, 0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32,
		        0xcf, 0xf7};
		BCRYPT_HASH_HANDLE hash;
		UCHAR digest[32], key[20];
		memset(key, 0x0b, sizeof key);
		CHECK(BCryptCreateHash(j->algorithm, &hash, NULL, 0, key,
		              sizeof key, 0) >= 0);
		CHECK(BCryptHashData(hash, (PUCHAR) "Hi There", 8, 0) >= 0);
		CHECK(BCryptFinishHash(hash, digest, 32, 0) >= 0);
		CHECK(!memcmp(digest, expected_digest, 32));
		CHECK(BCryptDestroyHash(hash) >= 0);
		SecureZeroMemory(key, sizeof key);
		SecureZeroMemory(digest, sizeof digest);
	}
	/* Repeat jobs exercise cleanup and fresh key generation. */
	wf_integrity_end(&other);
	/* Cleanup failures retain ownership, erase the key immediately and
	 * poison the job. A later finite retry releases the CNG object safely.
	 */
	CHECK(wf_integrity_begin(&other, &r));
	memcpy(observed_key, other->key, 32);
	fault = 11;
	CHECK(wf_integrity_compare(other, ha, &a, hb, &b, 262144, NULL, NULL,
	              &r) == WF_INTEGRITY_ERROR);
	CHECK(!wf_integrity_end(&other) && NULL != other);
	CHECK(wf_integrity_compare(other, ha, &a, hb, &b, 262144, NULL, NULL,
	              &r) == WF_INTEGRITY_ERROR);
	for (i = 0; i < 32; i++) {
		CHECK(0 == other->key[i]);
	}
	fault = 0;
	CHECK(wf_integrity_end(&other) && NULL == other);
	CHECK(wf_integrity_begin(&other, &r));
	fault = 12;
	CHECK(!wf_integrity_end(&other) && NULL != other);
	fault = 0;
	CHECK(wf_integrity_end(&other) && NULL == other);
	erased_bytes = sizeof *j;
	for (i = 0; i < 32; i++) {
		CHECK(wf_integrity_begin(&other, &r));
		erased_job = other;
		wf_integrity_end(&other);
	}
	erased_job = j;
	wf_integrity_end(&j);
	wf_integrity_end(&j);
	CHECK(33 == erase_checks && opens == closes && creations == destroys);
	SecureZeroMemory(observed_key, sizeof observed_key);
	CloseHandle(ha);
	CloseHandle(hb);
	CHECK(0 == p0_lab_close(&lab, 0));
	return TEST_RESULT();
}
