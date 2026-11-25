#include <stdlib.h>
#include <string.h>

#include "fs/owned_delete.h"
#include "path/wf_canonical_win.h"
#include "wf_test.h"

static const wchar_t parent[] =
        L"\\\\?\\Volume{01234567-89ab-cdef-0123-456789abcdef}\\日本語";
static const wchar_t rel[] =
        L".wfile-txn-01010101-0101-0101-0101-010101010101\\payload.bin";

static WfOwnershipRecord valid_record(void)
{
	WfOwnershipRecord r = {0};
	r.intent_durable = r.temp_id_durable = 1;
	memset(r.intent_txn, 1, 16);
	memset(r.txn, 1, 16);
	memset(r.nonce, 2, 32);
	r.parent.VolumeSerialNumber = r.directory.VolumeSerialNumber =
	        r.owner.VolumeSerialNumber = r.payload.VolumeSerialNumber = 7;
	r.parent.FileId.Identifier[0] = 1;
	r.directory.FileId.Identifier[0] = 2;
	r.owner.FileId.Identifier[0] = 3;
	r.payload.FileId.Identifier[0] = 4;
	r.source_size = r.intent_source_size = 4096;
	r.parent_path = parent;
	r.rel_payload = rel;
	return r;
}

int main(void)
{
	WfOwnershipRecord r = valid_record(), bad;
	wchar_t *p = NULL;
	WfCanonicalPath a = {7, (wchar_t *)parent}, b = {8, (wchar_t *)parent};
	WfVerifiedOwned *token = NULL;
	WfOwnedReport report;
	CHECK(wf_owned_record_valid(&r));
	CHECK(!wf_owned_record_valid(NULL));
#define BAD(change)                                                            \
	do {                                                                   \
		bad = r;                                                       \
		change;                                                        \
		CHECK(!wf_owned_record_valid(&bad));                           \
	} while (0)
	BAD(bad.intent_durable = 0);
	BAD(bad.temp_id_durable = 0);
	BAD(bad.intent_txn[0] ^= 1);
	BAD(bad.source_size++);
	BAD(memset(bad.payload.FileId.Identifier, 0, 16));
	BAD(bad.payload.VolumeSerialNumber++);
	BAD(bad.parent_path = L"C:\\untrusted");
	BAD(bad.rel_payload = L"..\\payload.bin");
	BAD(bad.rel_payload = L".wfile-txn-01010101-0101-0101-0101-"
	                      L"010101010101\\Payload.bin");
	BAD(bad.owner = bad.payload);
	CHECK(!wf_canonical_equal(
	        &a, &b)); /* Equal spelling cannot override volume. */
	b.volume = 7;
	CHECK(wf_canonical_equal(&a, &b));
	b.path = L"\\\\?\\Volume{01234567-89ab-cdef-0123-456789abcdef}"
	         L"\\日本語\\Payload.bin";
	a.path = L"\\\\?\\Volume{01234567-89ab-cdef-0123-456789abcdef}"
	         L"\\日本語\\payload.bin";
	CHECK(!wf_canonical_equal(&a, &b));
	b.path = L"\\\\?\\Volume{01234567-89ab-cdef-0123-456789abcdef}"
	         L"\\日本語\\adopted.bin";
	CHECK(!wf_canonical_equal(&a, &b));
	/* different and not comparable are told apart, and nothing is allocated */
	{
		bool same = true;

		b.path = a.path;
		CHECK(WF_OK == wf_canonical_compare(&a, &b, &same) && same);
		b.volume = 99;
		CHECK(WF_OK == wf_canonical_compare(&a, &b, &same) && !same);
		b.volume = a.volume;
		b.path = L"C:\\not-a-volume-path";
		same = true;
		CHECK(WF_E_INVAL == wf_canonical_compare(&a, &b, &same) &&
		        !same);
		b.path = NULL;
		CHECK(WF_E_INVAL == wf_canonical_compare(&a, &b, &same));
		CHECK(WF_E_INVAL == wf_canonical_compare(&a, &b, NULL));
		CHECK(!wf_canonical_equal(&a, &b));
		/* the GUID folds case, the rest of the path does not */
		a.path = L"\\\\?\\Volume{01234567-89AB-CDEF-0123-456789ABCDEF}"
		         L"\\x";
		b.path = L"\\\\?\\VOLUME{01234567-89ab-cdef-0123-456789abcdef}"
		         L"\\x";
		CHECK(WF_OK == wf_canonical_compare(&a, &b, &same) && same);
		b.path = L"\\\\?\\VOLUME{01234567-89ab-cdef-0123-456789abcdef}"
		         L"\\X";
		CHECK(WF_OK == wf_canonical_compare(&a, &b, &same) && !same);
		a.path = (wchar_t *)parent;
		b.path = (wchar_t *)parent;
	}
	CHECK(wf_canonical_normalize(
	        L"\\\\?\\VOLUME{01234567-89AB-CDEF-0123-456789ABCDEF}\\日本語",
	        &p));
	CHECK(p && !wcscmp(p, parent));
	free(p);
	p = NULL;
	CHECK(!wf_canonical_normalize(L"\\\\?\\Volume{01234567-89ab-cdef-0123-"
	                              L"456789abcdef}\\x\\..\\y",
	        &p));
	CHECK(!wf_canonical_normalize(
	        L"\\\\?\\Volume{01234567-89ab-cdef-0123-456789abcdef}\\x:ads",
	        &p));
	CHECK(!wf_canonical_normalize(
	        L"\\\\?\\Volume{01234567-89ab-cdef-0123-456789abcdef}\\x.\\y",
	        &p));
	CHECK(!wf_canonical_normalize(L"C:\\x", &p));
	CHECK(WF_OWNED_MANUAL ==
	        wf_owned_verify((WfOwnedKind)99, &r, &token, &report));
	CHECK(NULL == token);
	CHECK(WF_OWNED_MANUAL == wf_owned_delete(NULL, &report));
	wf_owned_release(NULL);
	return TEST_RESULT();
}
