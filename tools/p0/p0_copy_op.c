/*
 * The "isolated copy operation" of P0-S02/S03: just enough of the design 7.2
 * ordering to be killed at exact points. It is a measuring device, NOT the copy
 * engine: source-handle acceptance and HMAC are integrated, but commit remains
 * blocked by Storage gates. No cleanup/recovery mutation. It never deletes.
 *
 *   p0_copy_op --journal J --src S --dest-parent D --txn <uuid> --trace T
 *              [--crash-at POINT]
 *
 * Order (design 7.2): INTENT append -> flush -> create transaction (dir +
 * owner.bin) -> CopyFileExW; inside the first progress callback: query the
 * payload FileId -> TEMP_ID append -> flush.
 *
 * "Crash" is TerminateProcess on itself: the process dies at once, without
 * running any exit handler, exactly at the named point. Every step first
 * appends a line to the trace (flushed to the OS), so the harness knows what
 * had happened. The trace is the harness's oracle, not part of the system
 * under test.
 */
#include <windows.h>

#include <bcrypt.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "fs/owned_integrity.h"
#include "p0_common.h"
#include "p0_journal.h"
#include "p0_source_guard.h"
#include "path/wf_canonical_win.h"
#include "xfer/wf_copy_source_win.h"
#include "xfer/wf_probe_win.h"

#define CRASH_EXIT_CODE 0xC0DE0001u

static FILE *g_trace;
static const char *g_crash;
static LARGE_INTEGER g_freq;
static unsigned g_seq;
static P0Journal g_j;

static double now_ms(void)
{
	LARGE_INTEGER t;

	QueryPerformanceCounter(&t);
	return (double)t.QuadPart * 1000.0 / (double)g_freq.QuadPart;
}

static void trace(const char *fmt, ...)
{
	va_list ap;

	fprintf(g_trace, "%u,%.3f,", g_seq++, now_ms());
	va_start(ap, fmt);
	vfprintf(g_trace, fmt, ap);
	va_end(ap);
	fputc('\n', g_trace);
	fflush(g_trace);
}

/* A named point: logged first, then the process dies here if it is the chosen
 * one. */
static void point(const char *name)
{
	trace("POINT,%s", name);
	if (g_crash && !strcmp(g_crash, name)) {
		TerminateProcess(GetCurrentProcess(), CRASH_EXIT_CODE);
	}
}

static int parse_uuid(const char *s, uint8_t out[16])
{
	int i = 0;

	while (*s && i < 16) {
		unsigned v;

		if ('-' == *s) {
			s++;
			continue;
		}
		if (1 != sscanf_s(s, "%2x", &v)) {
			return 0;
		}
		out[i++] = (uint8_t)v;
		s += 2;
	}
	return 16 == i;
}

typedef struct
{
	wchar_t txn_dir[MAX_PATH * 2];
	uint8_t txn[16];
	uint8_t txn_dir_id[P0J_ID_BYTES], owner_id[P0J_ID_BYTES],
	        parent_id[P0J_ID_BYTES];
	uint8_t nonce[32];
	uint64_t src_size;
	wchar_t rel_payload[128];
	int first;
	int temp_id_durable;
	uint8_t payload_id[P0J_ID_BYTES];
	P0SourceGuard source;
	WfCopySourceObserver *observer;
	HANDLE parent_pin, directory_pin, owner_pin;
} Ctx;

static DWORD CALLBACK progress(LARGE_INTEGER total,
        LARGE_INTEGER xfer,
        LARGE_INTEGER ssize,
        LARGE_INTEGER sxfer,
        DWORD sno,
        DWORD reason,
        HANDLE hs,
        HANDLE hd,
        LPVOID data)
{
	Ctx *c = data;
	P0TempId t;

	WfCopySourceReport source_report;
	(void)total;
	(void)xfer;
	(void)ssize;
	(void)sxfer;
	(void)reason;
	if (!wf_copy_source_observe(c->observer, hs, sno, &source_report)) {
		trace("SOURCE_REJECTED,error=%lu,checks=%u",
		        source_report.error, source_report.callback_checks);
		return PROGRESS_STOP;
	}
	if (c->first) {
		return c->temp_id_durable ? PROGRESS_CONTINUE : PROGRESS_STOP;
	}
	c->first = 1;

	point("AFTER_COPY_START");
	if (!hd || INVALID_HANDLE_VALUE == hd) {
		trace("NO_DEST_HANDLE");
		return PROGRESS_STOP;
	}
	point("ON_FIRST_DEST_HANDLE");

	memset(&t, 0, sizeof t);
	if (!p0j_id_of_handle(hd, t.payload_id)) {
		trace("ID_QUERY_FAILED,%lu", GetLastError());
		return PROGRESS_STOP;
	}
	point("AFTER_ID_QUERY");

	memcpy(t.txn, c->txn, 16);
	memcpy(t.txn_dir_id, c->txn_dir_id, P0J_ID_BYTES);
	memcpy(t.owner_id, c->owner_id, P0J_ID_BYTES);
	memcpy(t.parent_id, c->parent_id, P0J_ID_BYTES);
	memcpy(t.nonce, c->nonce, 32);
	memcpy(&t.volume, c->parent_id, 8);
	t.src_size = c->src_size;
	wcscpy_s(t.rel_payload, 128, c->rel_payload);
	if (!p0j_append(&g_j, P0J_TEMP_ID, &t, sizeof t)) {
		trace("TEMP_ID_APPEND_FAILED");
		return PROGRESS_STOP; /* stop before any authority-dependent
		                         transition (design 7.7) */
	}
	trace("TEMP_ID_APPENDED,written=%llu", (unsigned long long)g_j.written);
	point("AFTER_TEMP_ID_WRITE");

	if (!p0j_flush(&g_j)) {
		trace("TEMP_ID_FLUSH_FAILED");
		return PROGRESS_STOP;
	}
	trace("JOURNAL_FLUSHED,flushed=%llu", (unsigned long long)g_j.flushed);
	memcpy(c->payload_id, t.payload_id, sizeof c->payload_id);
	c->temp_id_durable = 1;
	point("AFTER_TEMP_ID_FLUSH");
	return PROGRESS_CONTINUE;
}

static void unpack(FILE_ID_INFO *out, const uint8_t bytes[P0J_ID_BYTES])
{
	memcpy(&out->VolumeSerialNumber, bytes, 8);
	memcpy(out->FileId.Identifier, bytes + 8, 16);
}

static int verify_copy(Ctx *c, const P0Intent *intent, WfIntegrityJob *job)
{
	WfOwnershipRecord record = {0};
	WfVerifiedOwned *token = NULL;
	WfOwnedReport owned;
	WfIntegrityReport integrity;
	FILE_ID_INFO source;
	WfIntegrityResult result;
	record.intent_durable = 1;
	record.temp_id_durable = c->temp_id_durable;
	memcpy(record.intent_txn, intent->txn, 16);
	memcpy(record.txn, c->txn, 16);
	memcpy(record.nonce, c->nonce, 32);
	unpack(&record.parent, c->parent_id);
	unpack(&record.directory, c->txn_dir_id);
	unpack(&record.owner, c->owner_id);
	unpack(&record.payload, c->payload_id);
	record.intent_source_size = intent->src_size;
	record.source_size = c->src_size;
	record.parent_path = intent->dest_parent;
	record.rel_payload = c->rel_payload;
	if (WF_OWNED_VERIFIED !=
	        wf_owned_verify(
	                WF_OWNED_COPY_PAYLOAD, &record, &token, &owned)) {
		trace("OWNERSHIP_MANUAL,error=%lu", owned.error);
		return 0;
	}
	source.VolumeSerialNumber = c->source.identity.volume;
	memcpy(source.FileId.Identifier, c->source.identity.id, 16);
	point("BEFORE_HMAC_VERIFY");
	result = wf_owned_integrity_compare(job, c->source.handle, &source,
	        token, NULL, NULL, &integrity, &owned);
	trace("HMAC_RESULT,status=%u,error=%lu,cng=%ld,source_bytes=%llu,"
	      "destination_bytes=%llu",
	        (unsigned)result, integrity.error, integrity.cng_status,
	        (unsigned long long)integrity.source_bytes,
	        (unsigned long long)integrity.destination_bytes);
	wf_owned_release(token);
	if (WF_INTEGRITY_MATCH != result) {
		return 0;
	}
	point("AFTER_HMAC_VERIFY");
	/* Equality grants no durability/commit authority. */
	trace("COMMIT_BLOCKED,storage_recovery_powerloss_gates_incomplete");
	return 1;
}

/* ---- the operation, phase by phase -------------------------------------- *
 * The order is the safety property under test: the destination parent is
 * pinned, the INTENT is made durable, only then the transaction is created,
 * and only inside the copy does TEMP_ID get written. Every phase returns 0 or
 * the exit code that names the phase that refused. */

typedef struct
{
	wchar_t journal[MAX_PATH * 2], src[MAX_PATH * 2], parent[MAX_PATH * 2],
	        trace_path[MAX_PATH * 2], uuidw[64];
	char crash8[64], uuid8[64];
	Ctx c;
	P0Intent in;
	WfCanonicalPath recorded_parent;
	WfNodeInfo snapshot;
	WfIntegrityJob *job;
	WfCopySourceReport source_report;
} Op;

/* 0 when the command line is complete; else the exit code. */
static int parse_arguments(int argc, wchar_t **argv, Op *op)
{
	int i;

	for (i = 1; i + 1 < argc; i += 2) {
		const wchar_t *k = argv[i], *v = argv[i + 1];

		if (!wcscmp(k, L"--journal")) {
			wcscpy_s(op->journal, MAX_PATH * 2, v);
		} else if (!wcscmp(k, L"--src")) {
			wcscpy_s(op->src, MAX_PATH * 2, v);
		} else if (!wcscmp(k, L"--dest-parent")) {
			wcscpy_s(op->parent, MAX_PATH * 2, v);
		} else if (!wcscmp(k, L"--trace")) {
			wcscpy_s(op->trace_path, MAX_PATH * 2, v);
		} else if (!wcscmp(k, L"--txn")) {
			wcscpy_s(op->uuidw, 64, v);
			WideCharToMultiByte(CP_UTF8, 0, v, -1, op->uuid8,
			        sizeof op->uuid8, NULL, NULL);
		} else if (!wcscmp(k, L"--crash-at")) {
			WideCharToMultiByte(CP_UTF8, 0, v, -1, op->crash8,
			        sizeof op->crash8, NULL, NULL);
			g_crash = op->crash8;
		}
	}
	if (!op->journal[0] || !op->src[0] || !op->parent[0] ||
	        !op->trace_path[0] || !op->uuid8[0] ||
	        !parse_uuid(op->uuid8, op->c.txn)) {
		return 2;
	}
	g_trace = p0_fopen_w(op->trace_path);
	if (!g_trace) {
		return 3;
	}
	fputs("seq,t_ms,event\n", g_trace);
	trace("START,crash_at=%s", g_crash ? g_crash : "none");
	return 0;
}

/* The source is held by a read guard, observed by the copy callbacks, and the
 * read-back job is ready before anything is written. */
static int acquire_source(Op *op)
{
	WfProbeOps ops = wf_probe_win();
	WfIntegrityReport integrity;
	DWORD error;

	if (WF_OK != ops.node(NULL, op->src, &op->snapshot) ||
	        p0_source_guard_acquire(op->src, &op->snapshot, &op->c.source,
	                &error) != P0_GUARD_OK ||
	        !wf_copy_source_begin(op->c.source.handle, &op->snapshot,
	                &op->c.observer, &op->source_report)) {
		trace("SOURCE_GUARD_FAILED");
		return 4;
	}
	if (!wf_integrity_begin(&op->job, &integrity)) {
		trace("HMAC_JOB_FAILED,error=%lu,cng=%ld", integrity.error,
		        integrity.cng_status);
		return 4;
	}
	op->c.src_size = op->snapshot.size;
	return 0;
}

/* Opens the destination parent, proves it is a plain directory on a local
 * NTFS/ReFS volume that can open by ID, records its identity and canonical
 * path, and keeps the handle as the parent pin. */
static int pin_destination_parent(Op *op)
{
	HANDLE h = CreateFileW(op->parent, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ, NULL, OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	FILE_ATTRIBUTE_TAG_INFO tag;
	wchar_t fs[32];
	DWORD flags, error;

	if (INVALID_HANDLE_VALUE == h) {
		return 5;
	}
	if (!GetFileInformationByHandleEx(
	            h, FileAttributeTagInfo, &tag, sizeof tag)) {
		trace("DEST_PARENT_QUERY_FAILED,error=%lu", GetLastError());
		goto refuse;
	}
	if (!(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
	        (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
		goto refuse;
	}
	if (!GetVolumeInformationByHandleW(
	            h, NULL, 0, NULL, NULL, &flags, fs, 32)) {
		trace("DEST_VOLUME_QUERY_FAILED,error=%lu", GetLastError());
		goto refuse;
	}
	if ((wcscmp(fs, L"NTFS") && wcscmp(fs, L"ReFS")) ||
	        !(flags & FILE_SUPPORTS_OPEN_BY_FILE_ID) ||
	        (flags & FILE_READ_ONLY_VOLUME)) {
		trace("DEST_VOLUME_UNSUPPORTED");
		goto refuse;
	}
	if (!p0j_id_of_handle(h, op->c.parent_id) ||
	        !wf_canonical_from_handle(h, &op->recorded_parent, &error)) {
		goto refuse;
	}
	op->c.parent_pin = h;
	return 0;
refuse:
	CloseHandle(h);
	return 5;
}

/* INTENT is appended and flushed BEFORE anything exists at the destination;
 * if it cannot be made durable the destination is never touched. */
static int record_intent(Op *op)
{
	P0Intent *in = &op->in;

	memset(in, 0, sizeof *in);
	if (BCryptGenRandom(
	            NULL, in->job, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 ||
	        BCryptGenRandom(NULL, op->c.nonce, sizeof op->c.nonce,
	                BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
		trace("RNG_FAILED");
		return 7;
	}
	memcpy(in->txn, op->c.txn, 16);
	in->src_size = op->c.src_size;
	if (wcslen(op->recorded_parent.path) >= 260) {
		trace("DEST_PARENT_TOO_LONG,error=%lu,utf16_units=%zu,capacity="
		      "260",
		        (DWORD)ERROR_FILENAME_EXCED_RANGE,
		        wcslen(op->recorded_parent.path));
		return 5;
	}
	wcscpy_s(in->dest_parent, 260, op->recorded_parent.path);
	wf_canonical_free(&op->recorded_parent);
	wcscpy_s(in->name, 128, L"result.bin");

	point("BEFORE_INTENT_WRITE");
	if (!p0j_append(&g_j, P0J_INTENT, in, sizeof *in)) {
		return 7;
	}
	trace("INTENT_APPENDED,written=%llu", (unsigned long long)g_j.written);
	point("AFTER_INTENT_WRITE_BEFORE_FLUSH");
	if (!p0j_flush(&g_j)) {
		trace("INTENT_FLUSH_FAILED"); /* design: stop before touching
		                                 the destination */
		return 8;
	}
	trace("JOURNAL_FLUSHED,flushed=%llu", (unsigned long long)g_j.flushed);
	point("AFTER_INTENT_FLUSH");
	return 0;
}

/* Reads the owner file back through a pinned handle: same object, same
 * nonce. A conflicting writer or rename handle refuses before the copy. */
static bool owner_matches(HANDLE h, const Ctx *c)
{
	uint8_t observed_id[P0J_ID_BYTES], nonce[32];
	DWORD got;

	return p0j_id_of_handle(h, observed_id) &&
	        !memcmp(observed_id, c->owner_id, sizeof observed_id) &&
	        ReadFile(h, nonce, sizeof nonce, &got, NULL) &&
	        got == sizeof nonce && !memcmp(nonce, c->nonce, sizeof nonce);
}

/* Creates the transaction directory and its owner file (nonce, flushed), then
 * pins both through verification. No pathname is adopted afterwards. */
static int create_transaction(Op *op)
{
	Ctx *c = &op->c;
	wchar_t owner[MAX_PATH * 2];
	HANDLE h;
	DWORD wr;

	point("BEFORE_TXN_CREATE");
	trace("TXN_CREATE_BEGIN");
	swprintf(c->txn_dir, MAX_PATH * 2, L"%ls\\.wfile-txn-%ls",
	        op->in.dest_parent, op->uuidw);
	if (!CreateDirectoryW(c->txn_dir, NULL)) {
		return 9;
	}
	swprintf(owner, MAX_PATH * 2, L"%ls\\owner.bin", c->txn_dir);
	h = CreateFileW(owner, GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return 10;
	}
	if (!WriteFile(h, c->nonce, sizeof c->nonce, &wr, NULL) ||
	        wr != sizeof c->nonce || !FlushFileBuffers(h) ||
	        !p0j_id_of_handle(h, c->owner_id)) {
		CloseHandle(h);
		return 10;
	}
	CloseHandle(h);
	h = CreateFileW(owner, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return 10;
	}
	if (!owner_matches(h, c)) {
		CloseHandle(h);
		return 10;
	}
	c->owner_pin = h;
	h = CreateFileW(c->txn_dir, FILE_READ_ATTRIBUTES, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	if (INVALID_HANDLE_VALUE == h || !p0j_id_of_handle(h, c->txn_dir_id)) {
		if (INVALID_HANDLE_VALUE != h) {
			CloseHandle(h);
		}
		return 10;
	}
	c->directory_pin = h;
	trace("TXN_CREATE_END");
	point("AFTER_TXN_CREATE");
	return 0;
}

/* The copy itself (TEMP_ID is written from inside the first callback), then
 * source acceptance and the measured verification. Not a commit. */
static int copy_and_verify(Op *op)
{
	Ctx *c = &op->c;
	wchar_t payload[MAX_PATH * 2];
	BOOL ok;
	DWORD error;

	swprintf(
	        c->rel_payload, 128, L".wfile-txn-%ls\\payload.bin", op->uuidw);
	swprintf(payload, MAX_PATH * 2, L"%ls\\payload.bin", c->txn_dir);
	trace("COPY_BEGIN");
	ok = CopyFileExW(op->src, payload, progress, c, NULL,
	        COPY_FILE_FAIL_IF_EXISTS | COPY_FILE_COPY_SYMLINK);
	error = ok ? 0 : GetLastError();
	trace("COPY_END,ok=%d,err=%lu", 0 != ok, error);
	if (!wf_copy_source_finish(
	            c->observer, 0 != ok, error, &op->source_report)) {
		trace("COPY_NOT_ACCEPTED,error=%lu,state=%u",
		        op->source_report.error,
		        (unsigned)op->source_report.state);
		return 11;
	}
	point("AFTER_SOURCE_ACCEPT");
	if (!verify_copy(c, &op->in, op->job)) {
		return 11;
	}
	return 0; /* measured source/ownership/content verification, NOT commit */
}

static int release_operation(Op *op, int result)
{
	Ctx *c = &op->c;

	if (!wf_integrity_end(&op->job)) {
		result = 11;
		trace("KEY_CLEANUP_REQUIRED");
		wf_integrity_end(
		        &op->job); /* one bounded retry; never compare again */
	}
	wf_copy_source_release(c->observer);
	p0_source_guard_release(&c->source);
	if (INVALID_HANDLE_VALUE != c->owner_pin) {
		CloseHandle(c->owner_pin);
	}
	if (INVALID_HANDLE_VALUE != c->directory_pin) {
		CloseHandle(c->directory_pin);
	}
	if (INVALID_HANDLE_VALUE != c->parent_pin) {
		CloseHandle(c->parent_pin);
	}
	wf_canonical_free(&op->recorded_parent);
	p0j_close(&g_j);
	fclose(g_trace);
	return result;
}

int wmain(int argc, wchar_t **argv)
{
	Op op;
	int result;

	QueryPerformanceFrequency(&g_freq);
	memset(&op, 0, sizeof op);
	op.c.source.handle = INVALID_HANDLE_VALUE;
	op.c.parent_pin = op.c.directory_pin = op.c.owner_pin =
	        INVALID_HANDLE_VALUE;
	if (0 != (result = parse_arguments(argc, argv, &op))) {
		return result;
	}

	/* The order below is the property under test: parent pinned, journal
	 * open, INTENT durable, transaction created, copy, verification. The
	 * first refusal ends the run; release_operation() always runs. */
	if (0 != (result = acquire_source(&op))) {
		goto done;
	}
	if (0 != (result = pin_destination_parent(&op))) {
		goto done;
	}
	if (!p0j_open(&g_j, op.journal)) {
		result = 6;
		goto done;
	}
	if (0 != (result = record_intent(&op))) {
		goto done;
	}
	if (0 != (result = create_transaction(&op))) {
		goto done;
	}
	result = copy_and_verify(&op);
done:
	return release_operation(&op, result);
}
