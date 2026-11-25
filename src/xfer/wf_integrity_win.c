#include <windows.h>

#include "wf_integrity_win.h"

#include <bcrypt.h>
#include <stdlib.h>
#include <string.h>

#include "../fs/wf_weakid_win.h"

#define BUFFER_BYTES 65536u
#define DIGEST_BYTES 32u
struct WfIntegrityJob
{
	BCRYPT_ALG_HANDLE algorithm;
	ULONG object_bytes;
	UCHAR key[32];
	BCRYPT_HASH_HANDLE pending_hash;
	UCHAR *pending_object;
	bool closing;
};
typedef struct
{
	FILE_ID_INFO id;
	FILE_STANDARD_INFO standard;
	FILE_BASIC_INFO basic;
	FILE_ATTRIBUTE_TAG_INFO tag;
} Snapshot;

static bool cng_ok(NTSTATUS status, WfIntegrityReport *report)
{
	if (status >= 0) {
		return 1;
	}
	report->cng_status = status;
	return 0;
}

bool wf_integrity_end(WfIntegrityJob **job)
{
	WfIntegrityJob *j;
	if (!job || !*job) {
		return 1;
	}
	j = *job;
	j->closing = true;
	SecureZeroMemory(j->key, sizeof j->key);
	if (j->pending_hash) {
		if (BCryptDestroyHash(j->pending_hash) < 0) {
			return 0;
		}
		j->pending_hash = NULL;
		SecureZeroMemory(j->pending_object, j->object_bytes);
		free(j->pending_object);
		j->pending_object = NULL;
	}
	if (j->algorithm && BCryptCloseAlgorithmProvider(j->algorithm, 0) < 0) {
		return 0;
	}
	*job = NULL;
	SecureZeroMemory(j, sizeof *j);
	free(j);
	return 1;
}

bool wf_integrity_begin(WfIntegrityJob **out, WfIntegrityReport *report)
{
	WfIntegrityJob *j;
	ULONG got = 0, length = 0;
	if (!out || !report) {
		return 0;
	}
	memset(report, 0, sizeof *report);
	if (*out) {
		report->error = ERROR_INVALID_PARAMETER;
		return 0;
	}
	j = calloc(1, sizeof *j);
	if (!j) {
		report->error = ERROR_NOT_ENOUGH_MEMORY;
		return 0;
	}
	if (!cng_ok(BCryptGenRandom(NULL, j->key, sizeof j->key,
	                    BCRYPT_USE_SYSTEM_PREFERRED_RNG),
	            report)) {
		goto fail;
	}
	if (!cng_ok(BCryptOpenAlgorithmProvider(&j->algorithm,
	                    BCRYPT_SHA256_ALGORITHM, NULL,
	                    BCRYPT_ALG_HANDLE_HMAC_FLAG),
	            report)) {
		goto fail;
	}
	if (!cng_ok(BCryptGetProperty(j->algorithm, BCRYPT_OBJECT_LENGTH,
	                    (PUCHAR)&j->object_bytes, sizeof j->object_bytes,
	                    &got, 0),
	            report)) {
		goto fail;
	}
	if (got != sizeof j->object_bytes || !j->object_bytes) {
		goto fail;
	}
	if (!cng_ok(BCryptGetProperty(j->algorithm, BCRYPT_HASH_LENGTH,
	                    (PUCHAR)&length, sizeof length, &got, 0),
	            report)) {
		goto fail;
	}
	if (got != sizeof length || DIGEST_BYTES != length) {
		goto fail;
	}
	*out = j;
	return 1;
fail:
	if (!wf_integrity_end(&j)) {
		*out = j;
	}
	return 0;
}

static bool snapshot(HANDLE h, Snapshot *s, WfIntegrityReport *report)
{
	if (!wf_handle_file_id_info(h, &s->id)) {
		goto fail;
	}
	if (!GetFileInformationByHandleEx(
	            h, FileStandardInfo, &s->standard, sizeof s->standard)) {
		goto fail;
	}
	if (!GetFileInformationByHandleEx(
	            h, FileBasicInfo, &s->basic, sizeof s->basic)) {
		goto fail;
	}
	/* FAT has no FileAttributeTagInfo: the attributes are read the older way,
	 * and a reparse tag is not asked for (only the flags are used here) */
	if (!wf_handle_attributes(h, &s->tag.FileAttributes, &report->error)) {
		return 0;
	}
	return 1;
fail:
	report->error = GetLastError();
	return 0;
}

static bool identity_equal(const FILE_ID_INFO *a, const FILE_ID_INFO *b)
{
	return a->VolumeSerialNumber == b->VolumeSerialNumber &&
	        !memcmp(a->FileId.Identifier, b->FileId.Identifier,
	                sizeof a->FileId.Identifier);
}

static bool expected(const Snapshot *s, const FILE_ID_INFO *id, uint64_t size)
{
	static const UCHAR zero[16] = {0};
	return memcmp(id->FileId.Identifier, zero, sizeof zero) &&
	        identity_equal(&s->id, id) && !s->standard.Directory &&
	        !s->standard.DeletePending && 1 == s->standard.NumberOfLinks &&
	        s->standard.EndOfFile.QuadPart >= 0 &&
	        (uint64_t)s->standard.EndOfFile.QuadPart == size &&
	        !(s->tag.FileAttributes &
	                (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE |
	                        FILE_ATTRIBUTE_RECALL_ON_OPEN |
	                        FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS));
}

static bool stable(const Snapshot *a, const Snapshot *b)
{
	return identity_equal(&a->id, &b->id) &&
	        a->standard.EndOfFile.QuadPart ==
	        b->standard.EndOfFile.QuadPart &&
	        a->basic.CreationTime.QuadPart ==
	        b->basic.CreationTime.QuadPart &&
	        a->basic.LastWriteTime.QuadPart ==
	        b->basic.LastWriteTime.QuadPart &&
	        a->basic.ChangeTime.QuadPart == b->basic.ChangeTime.QuadPart &&
	        a->basic.FileAttributes == b->basic.FileAttributes;
}

static WfIntegrityResult hash_file(WfIntegrityJob *job,
        HANDLE file,
        uint64_t size,
        WfIntegrityCancel cancelled,
        void *context,
        UCHAR digest[DIGEST_BYTES],
        uint64_t *total,
        WfIntegrityReport *report)
{
	UCHAR *object = NULL, *buffer = NULL;
	BCRYPT_HASH_HANDLE hash = NULL;
	LARGE_INTEGER zero = {0};
	WfIntegrityResult result = WF_INTEGRITY_ERROR;
	DWORD got;
	*total = 0;
	object = calloc(1, job->object_bytes);
	buffer = malloc(BUFFER_BYTES);
	if (!object || !buffer) {
		report->error = ERROR_NOT_ENOUGH_MEMORY;
		goto done;
	}
	if (!cng_ok(BCryptCreateHash(job->algorithm, &hash, object,
	                    job->object_bytes, job->key, sizeof job->key, 0),
	            report)) {
		goto done;
	}
	if (!SetFilePointerEx(file, zero, NULL, FILE_BEGIN)) {
		report->error = GetLastError();
		goto done;
	}
	for (;;) {
		if (cancelled && cancelled(context)) {
			result = WF_INTEGRITY_CANCELLED;
			goto done;
		}
		if (!ReadFile(file, buffer, BUFFER_BYTES, &got, NULL)) {
			report->error = GetLastError();
			goto done;
		}
		if (cancelled && cancelled(context)) {
			result = WF_INTEGRITY_CANCELLED;
			goto done;
		}
		if (!got) {
			break;
		}
		if (*total > size || (uint64_t)got > size - *total) {
			result = WF_INTEGRITY_CHANGED;
			goto done;
		}
		if (!cng_ok(BCryptHashData(hash, buffer, got, 0), report)) {
			goto done;
		}
		*total += got;
	}
	if (*total != size) {
		result = WF_INTEGRITY_CHANGED;
		goto done;
	}
	if (!cng_ok(BCryptFinishHash(hash, digest, DIGEST_BYTES, 0), report)) {
		goto done;
	}
	result = WF_INTEGRITY_MATCH;
done:
	if (hash) {
		/* A valid exclusively owned hash must be destroyed before its
		 * backing storage is erased/freed (BCryptCreateHash contract).
		 */
		if (!cng_ok(BCryptDestroyHash(hash), report)) {
			/* Retain cleanup ownership; end() can retry without
			 * freeing backing memory underneath CNG. The poisoned
			 * job cannot verify again. */
			job->pending_hash = hash;
			job->pending_object = object;
			job->closing = true;
			SecureZeroMemory(job->key, sizeof job->key);
			object = NULL;
			result = WF_INTEGRITY_ERROR;
		}
	}
	if (object) {
		SecureZeroMemory(object, job->object_bytes);
		free(object);
	}
	if (buffer) {
		SecureZeroMemory(buffer, BUFFER_BYTES);
		free(buffer);
	}
	return result;
}

WfIntegrityResult wf_integrity_compare(WfIntegrityJob *job,
        HANDLE source,
        const FILE_ID_INFO *source_id,
        HANDLE destination,
        const FILE_ID_INFO *destination_id,
        uint64_t size,
        WfIntegrityCancel cancelled,
        void *context,
        WfIntegrityReport *report)
{
	Snapshot before[2], after[2];
	UCHAR digests[2][DIGEST_BYTES] = {{0}};
	UCHAR difference = 0;
	WfIntegrityResult result = WF_INTEGRITY_ERROR;
	size_t i;
	if (!report) {
		return result;
	}
	memset(report, 0, sizeof *report);
	if (!job || job->closing || !source_id || !destination_id ||
	        size > INT64_MAX || INVALID_HANDLE_VALUE == source ||
	        INVALID_HANDLE_VALUE == destination || !source ||
	        !destination) {
		report->error = ERROR_INVALID_PARAMETER;
		return result;
	}
	if (!snapshot(source, &before[0], report) ||
	        !snapshot(destination, &before[1], report)) {
		goto done;
	}
	result = WF_INTEGRITY_CHANGED;
	if (identity_equal(source_id, destination_id) ||
	        !expected(&before[0], source_id, size) ||
	        !expected(&before[1], destination_id, size)) {
		goto done;
	}
	result = hash_file(job, source, size, cancelled, context, digests[0],
	        &report->source_bytes, report);
	if (WF_INTEGRITY_MATCH != result) {
		goto done;
	}
	result = hash_file(job, destination, size, cancelled, context,
	        digests[1], &report->destination_bytes, report);
	if (WF_INTEGRITY_MATCH != result) {
		goto done;
	}
	result = WF_INTEGRITY_ERROR;
	if (!snapshot(source, &after[0], report) ||
	        !snapshot(destination, &after[1], report)) {
		goto done;
	}
	result = WF_INTEGRITY_CHANGED;
	if (!expected(&after[0], source_id, size) ||
	        !expected(&after[1], destination_id, size) ||
	        !stable(&before[0], &after[0]) ||
	        !stable(&before[1], &after[1])) {
		goto done;
	}
	for (i = 0; i < DIGEST_BYTES; i++) {
		difference |= digests[0][i] ^ digests[1][i];
	}
	result = difference ? WF_INTEGRITY_MISMATCH : WF_INTEGRITY_MATCH;
done:
	SecureZeroMemory(digests, sizeof digests);
	return result;
}
