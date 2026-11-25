#include "p0_journal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "path/wf_canonical_win.h"
#include "recovery/wf_recovery.h"

#define MAGIC 0x43455257u /* "WREC" little-endian */
#define FILE_HDR "WJRN\1\0\0\0"
#define FILE_HDR_LEN 8

uint32_t p0j_crc32(const void *p, size_t n)
{
	static uint32_t table[256];
	const uint8_t *b = p;
	uint32_t c = 0xFFFFFFFFu;
	size_t i;

	if (!table[1]) {
		uint32_t k;
		int j;

		for (k = 0; k < 256; k++) {
			uint32_t v = k;

			for (j = 0; j < 8; j++) {
				v = (v >> 1) ^
				        (0xEDB88320u &
				                (uint32_t)-(int32_t)(v & 1));
			}
			table[k] = v;
		}
	}
	for (i = 0; i < n; i++) {
		c = table[(c ^ b[i]) & 0xFF] ^ (c >> 8);
	}
	return ~c;
}

int p0j_open(P0Journal *j, const wchar_t *path)
{
	DWORD wr;

	memset(j, 0, sizeof *j);
	/* READ sharing only: nobody else may write or delete it while we hold
	 * it */
	j->h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
	        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == j->h) {
		return 0;
	}
	if (!WriteFile(j->h, FILE_HDR, FILE_HDR_LEN, &wr, NULL) ||
	        FILE_HDR_LEN != wr) {
		return 0;
	}
	j->written = FILE_HDR_LEN;
	return 1;
}

int p0j_append(P0Journal *j, P0JType type, const void *payload, uint32_t len)
{
	size_t total = 4 + 4 + 4 + (size_t)len + 4;
	uint8_t *rec = malloc(total);
	uint32_t magic = MAGIC, crc;
	DWORD wr;
	int ok;

	if (!rec) {
		return 0;
	}
	memcpy(rec, &magic, 4);
	memcpy(rec + 4, &len, 4);
	rec[8] = (uint8_t)type;
	rec[9] = rec[10] = rec[11] = 0;
	memcpy(rec + 12, payload, len);
	crc = p0j_crc32(rec + 8, 4 + (size_t)len);
	memcpy(rec + 12 + len, &crc, 4);
	ok = WriteFile(j->h, rec, (DWORD)total, &wr, NULL) && wr == total;
	free(rec);
	if (ok) {
		j->written += total;
	}
	return ok;
}

int p0j_flush(P0Journal *j)
{
	if (!FlushFileBuffers(j->h)) {
		return 0;
	}
	j->flushed = j->written; /* only after the call succeeded */
	return 1;
}

void p0j_close(P0Journal *j)
{
	if (j->h && INVALID_HANDLE_VALUE != j->h) {
		CloseHandle(j->h);
	}
	j->h = NULL;
}

size_t p0j_parse(const uint8_t *buf, size_t n, P0Rec *out, size_t max)
{
	size_t pos = FILE_HDR_LEN, count = 0;

	if (n < FILE_HDR_LEN || 0 != memcmp(buf, FILE_HDR, FILE_HDR_LEN)) {
		return 0;
	}
	while (count < max && pos + 12 <= n) {
		uint32_t magic, len, crc;

		memcpy(&magic, buf + pos, 4);
		memcpy(&len, buf + pos + 4, 4);
		if (MAGIC != magic || len > (1u << 20) ||
		        pos + 12 + (size_t)len + 4 > n) {
			break; /* torn or absent tail */
		}
		memcpy(&crc, buf + pos + 12 + len, 4);
		if (p0j_crc32(buf + pos + 8, 4 + (size_t)len) != crc) {
			break;
		}
		out[count].type = buf[pos + 8];
		out[count].len = len;
		out[count].data = buf + pos + 12;
		count++;
		pos += 12 + (size_t)len + 4;
	}
	return count;
}

int p0j_id_of_handle(HANDLE h, uint8_t out[P0J_ID_BYTES])
{
	FILE_ID_INFO f;

	if (!GetFileInformationByHandleEx(h, FileIdInfo, &f, sizeof f)) {
		return 0;
	}
	memcpy(out, &f.VolumeSerialNumber, 8);
	memcpy(out + 8, f.FileId.Identifier, 16);
	return 1;
}

const char *p0r_class_name(P0ScanClass c)
{
	switch (c) {
	case P0S_END:            return "END";
	case P0S_MANUAL:         return "MANUAL";
	case P0S_AUTO_CANDIDATE: return "AUTO_CANDIDATE";
	case P0S_ABORTED:        return "ABORTED";
	case P0S_SUSPECT:        return "SUSPECT";
	case P0S_COMMITTED:      return "COMMITTED";
	}
	return "?";
}

/* Compatibility adapter only. Every target ownership/commit decision goes
 * through the same product classifier and wfstorage handle proof. */
static int count_txn_names(const wchar_t *dir)
{
	WIN32_FIND_DATAW entry;
	HANDLE find;
	wchar_t pattern[32768];
	int count = 0;
	int n = swprintf(pattern, 32768, L"%ls\\.wfile-txn-*", dir);
	if (n < 0 || n >= 32768) {
		return -1;
	}
	find = FindFirstFileW(pattern, &entry);
	if (INVALID_HANDLE_VALUE == find) {
		return ERROR_FILE_NOT_FOUND == GetLastError() ? 0 : -1;
	}
	do {
		if (INT_MAX == count) {
			count = -1;
			break;
		}
		count++;
	} while (FindNextFileW(find, &entry));
	if (count >= 0 && ERROR_NO_MORE_FILES != GetLastError()) {
		count = -1;
	}
	FindClose(find);
	return count;
}
static void unpack_id(FILE_ID_INFO *id, const uint8_t bytes[24])
{
	memcpy(&id->VolumeSerialNumber, bytes, 8);
	memcpy(id->FileId.Identifier, bytes + 8, 16);
}
/* The journal's content as the state machine reads it. */
typedef struct
{
	P0Intent intent;
	P0TempId temp;
	int directory;
} JournalRecords;

/* Reads the records in order and checks the transitions (INTENT first and only
 * once, TEMP_ID only after INTENT and before COMMITTED, COMMITTED naming the
 * same transaction). Pure: no file is touched. False for anything malformed or
 * ambiguous; *out says which records were seen. */
static bool read_journal_records(
        const uint8_t *buf, size_t n, P0Scan *out, JournalRecords *j)
{
	P0Rec records[64];
	size_t count, i;
	int committed = 0;
	P0Intent *intent = &j->intent;
	P0TempId *temp = &j->temp;

	memset(j, 0, sizeof *j);
	count = p0j_parse(buf, n, records, 64);
	if (64 == count ||
	        (n &&
	                (n < FILE_HDR_LEN ||
	                        memcmp(buf, FILE_HDR, FILE_HDR_LEN)))) {
		return false;
	}
	for (i = 0; i < count; i++) {
		P0Rec *r = &records[i];
		if (P0J_INTENT == r->type || P0J_DIRECTORY_INTENT == r->type) {
			if (out->has_intent || i || r->len != sizeof *intent) {
				return false;
			}
			memcpy(intent, r->data, sizeof *intent);
			out->has_intent = 1;
			j->directory = P0J_DIRECTORY_INTENT == r->type;
		} else if (P0J_TEMP_ID == r->type) {
			if (!out->has_intent || out->has_temp_id || committed ||
			        r->len != sizeof *temp) {
				return false;
			}
			memcpy(temp, r->data, sizeof *temp);
			out->has_temp_id = 1;
		} else if (P0J_COMMITTED == r->type) {
			if (!out->has_temp_id || committed || 16 != r->len ||
			        memcmp(r->data, intent->txn, 16)) {
				return false;
			}
			committed = 1;
		} else {
			return false;
		}
	}
	return true;
}

/* Compares the journal with what is on disk at `parent` and classifies it with
 * the shared recovery classifier. Read-only. */
static void classify_against_disk(
        const JournalRecords *j, const wchar_t *parent, P0Scan *out)
{
	const P0Intent intent = j->intent;
	const P0TempId temp = j->temp;
	const int directory = j->directory;
	WfOwnershipRecord record = {0};
	WfRecoveryReport recovery;
	wchar_t path[32768], txn_leaf[80];
	DWORD attr, error;
	WfCanonicalPath actual = {0}, expected = {0};
	HANDLE h = INVALID_HANDLE_VALUE;

	if (!wmemchr(intent.dest_parent, 0, 260) ||
	        !wmemchr(intent.name, 0, 128) || !intent.name[0] ||
	        !wf_canonical_normalize(intent.dest_parent, &expected.path)) {
		return;
	}
	h = CreateFileW(parent, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	if (INVALID_HANDLE_VALUE == h ||
	        !wf_canonical_from_handle(h, &actual, &error)) {
		goto done;
	}
	expected.volume = actual.volume;
	if (!wf_canonical_equal(&actual, &expected)) {
		goto done;
	}
	swprintf(txn_leaf, 80,
	        L".wfile-txn-%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%"
	        L"02x%02x%02x%02x%02x",
	        intent.txn[0], intent.txn[1], intent.txn[2], intent.txn[3],
	        intent.txn[4], intent.txn[5], intent.txn[6], intent.txn[7],
	        intent.txn[8], intent.txn[9], intent.txn[10], intent.txn[11],
	        intent.txn[12], intent.txn[13], intent.txn[14], intent.txn[15]);
	if (swprintf(path, 32768, L"%ls\\%ls", expected.path, txn_leaf) < 0) {
		goto done;
	}
	attr = GetFileAttributesW(path);
	error = INVALID_FILE_ATTRIBUTES == attr ? GetLastError() : 0;
	out->txn_exists = INVALID_FILE_ATTRIBUTES != attr;
	if (INVALID_FILE_ATTRIBUTES == attr && ERROR_FILE_NOT_FOUND != error) {
		goto done;
	}
	if (!out->has_temp_id) {
		out->cls = out->txn_exists ? P0S_MANUAL : P0S_END;
		strcpy_s(out->reason, sizeof out->reason,
		        out->txn_exists
		                ? "transaction exists without durable TEMP_ID"
		                : "durable INTENT; no transaction created");
		goto done;
	}
	if (!wmemchr(temp.rel_payload, 0, 128)) {
		goto done;
	}
	memcpy(out->temp_payload_id, temp.payload_id, 24);
	record.intent_durable = record.temp_id_durable = 1;
	memcpy(record.intent_txn, intent.txn, 16);
	memcpy(record.txn, temp.txn, 16);
	memcpy(record.nonce, temp.nonce, 32);
	record.intent_source_size = intent.src_size;
	record.source_size = temp.src_size;
	unpack_id(&record.parent, temp.parent_id);
	unpack_id(&record.directory, temp.txn_dir_id);
	unpack_id(&record.owner, temp.owner_id);
	unpack_id(&record.payload, temp.payload_id);
	record.parent_path = expected.path;
	record.rel_payload = temp.rel_payload;
	if (temp.volume != record.parent.VolumeSerialNumber ||
	        !wf_owned_record_valid(&record)) {
		goto done;
	}
	if (swprintf(path, 32768, L"%ls\\%ls", expected.path,
	            temp.rel_payload) < 0) {
		goto done;
	}
	out->payload_exists =
	        INVALID_FILE_ATTRIBUTES != GetFileAttributesW(path);
	wf_recovery_inspect(&record, intent.name, directory, &recovery);
	switch (recovery.classification) {
	case WF_RECOVERY_END:    out->cls = P0S_END; break;
	case WF_RECOVERY_MANUAL: out->cls = P0S_MANUAL; break;
	case WF_RECOVERY_CLEANUP_CANDIDATE:
		out->cls = P0S_AUTO_CANDIDATE;
		break;
	case WF_RECOVERY_ABORTED:   out->cls = P0S_ABORTED; break;
	case WF_RECOVERY_COMMITTED: out->cls = P0S_COMMITTED; break;
	}
	out->ownership_checks_total = 1;
	out->ownership_checks_passed =
	        P0S_AUTO_CANDIDATE == out->cls || P0S_COMMITTED == out->cls;
	snprintf(out->reason, sizeof out->reason, "%s",
	        recovery.reason ? recovery.reason
	                        : "shared ownership classifier");
done:
	if (INVALID_HANDLE_VALUE != h) {
		CloseHandle(h);
	}
	wf_canonical_free(&actual);
	wf_canonical_free(&expected);
}

void p0r_scan(const uint8_t *buf, size_t n, const wchar_t *parent, P0Scan *out)
{
	JournalRecords j;

	if (!out) {
		return;
	}
	memset(out, 0, sizeof *out);
	out->cls = P0S_MANUAL;
	strcpy_s(out->reason, sizeof out->reason,
	        "malformed, unavailable or ambiguous journal state");
	if (!parent || (!buf && n)) {
		return;
	}
	out->txn_names_found = count_txn_names(parent);
	if (out->txn_names_found < 0) {
		return;
	}
	if (!read_journal_records(buf, n, out, &j)) {
		return;
	}
	if (!out->has_intent) {
		out->cls = out->txn_names_found ? P0S_SUSPECT : P0S_END;
		strcpy_s(out->reason, sizeof out->reason,
		        out->txn_names_found
		                ? "unexplained transaction names"
		                : "no journal intent or transaction names");
		return;
	}
	classify_against_disk(&j, parent, out);
}
