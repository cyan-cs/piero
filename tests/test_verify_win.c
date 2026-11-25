/*
 * Read-back verification against real files (Windows only): the stage machine
 * must only reach "the source may be removed" through a MATCH that the existing
 * keyed SHA-256 measurement produced, and every other outcome (mismatch,
 * identity change, concurrent modification, cancellation, I/O failure) must
 * stop it. Everything lives in a p0 scratch area.
 */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tools/p0/p0_lab.h"
#include "wf_test.h"
#include "xfer/wf_verify_win.h"

#define BODY_BYTES (3u * 1024u * 1024u + 17u) /* several read slices */

static P0Lab lab;

static bool write_body(const wchar_t *path, unsigned char flip_at, bool flip)
{
	HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	unsigned char *buf = malloc(BODY_BYTES);
	DWORD wrote;
	bool ok = false;

	if (INVALID_HANDLE_VALUE == h || !buf) {
		goto done;
	}
	for (size_t i = 0; i < BODY_BYTES; i++) {
		buf[i] = (unsigned char)(i * 31u + (i >> 8));
	}
	if (flip) {
		buf[BODY_BYTES - 1 - flip_at] ^= 1u;
	}
	ok = WriteFile(h, buf, BODY_BYTES, &wrote, NULL) && BODY_BYTES == wrote;
done:
	if (INVALID_HANDLE_VALUE != h) {
		CloseHandle(h);
	}
	free(buf);
	return ok;
}

typedef struct
{
	const wchar_t *path;
	HANDLE handle;
	FILE_ID_INFO id;
} File;

static bool open_file(File *f, const wchar_t *rel, bool share_write)
{
	wchar_t path[1024];

	CHECK(p0_lab_path(&lab, rel, path, 1024));
	f->handle = CreateFileW(path, GENERIC_READ,
	        FILE_SHARE_READ | (share_write ? FILE_SHARE_WRITE : 0), NULL,
	        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	if (INVALID_HANDLE_VALUE == f->handle) {
		return false;
	}
	return GetFileInformationByHandleEx(
	        f->handle, FileIdInfo, &f->id, sizeof f->id);
}

static bool make_pair(const wchar_t *src_rel,
        const wchar_t *dst_rel,
        bool corrupt_destination)
{
	wchar_t a[1024], b[1024];

	CHECK(p0_lab_path(&lab, src_rel, a, 1024));
	CHECK(p0_lab_path(&lab, dst_rel, b, 1024));
	if (!write_body(a, 0, false) ||
	        !write_body(b, 5, corrupt_destination)) {
		return false;
	}
	return p0_area_track(lab.area, src_rel, 0) &&
	        p0_area_track(lab.area, dst_rel, 0);
}

/* A machine that has run every stage a real transfer runs before verifying. */
static WfStageMachine ready(WfTransferKind kind)
{
	WfVerifyPlan plan;
	WfStageMachine m;

	CHECK(WF_OK == wf_verify_plan(kind, WF_VERIFY_SHA256, &plan));
	wf_stage_init(&m, &plan);
	CHECK(wf_stage_copied(&m, true));
	CHECK(wf_stage_source_accepted(&m, true));
	CHECK(wf_stage_destination_durable(&m, true));
	return m;
}

typedef struct
{
	unsigned calls;
	unsigned act_at; /* 1-based call on which to act; 0 = never */
	bool cancel;     /* return true (cancel) at act_at */
	HANDLE writer;   /* else: write through this handle at act_at */
} Hook;

static bool hook(void *ctx)
{
	Hook *h = ctx;
	h->calls++;
	if (h->act_at && h->calls == h->act_at) {
		if (h->cancel) {
			return true;
		}
		if (INVALID_HANDLE_VALUE != h->writer) {
			DWORD wrote;
			LARGE_INTEGER at = {.QuadPart = 4096};
			unsigned char x = 0x5A;

			SetFilePointerEx(h->writer, at, NULL, FILE_BEGIN);
			WriteFile(h->writer, &x, 1, &wrote, NULL);
		}
	}
	return false;
}

static void match_authorizes_removal_only_for_a_cross_volume_move(void)
{
	File s, d;
	WfStageMachine m = ready(WF_KIND_MOVE_CROSS_VOLUME);
	WfIntegrityReport report;
	Hook h = {0};

	CHECK(make_pair(L"s1.bin", L"d1.bin", false));
	CHECK(open_file(&s, L"s1.bin", false) &&
	        open_file(&d, L"d1.bin", true));
	h.writer = INVALID_HANDLE_VALUE;
	CHECK(wf_verify_handles(&m, s.handle, &s.id, d.handle, &d.id,
	        BODY_BYTES, hook, &h, &report));
	CHECK(WF_STAGE_VERIFIED == m.stage &&
	        BODY_BYTES == report.source_bytes &&
	        BODY_BYTES == report.destination_bytes && h.calls > 1);
	CHECK(!wf_stage_may_remove_source(&m)); /* not yet recorded */
	CHECK(wf_stage_verified_recorded(&m, true) && wf_stage_complete(&m));
	CHECK(wf_stage_may_remove_source(&m));
	CloseHandle(s.handle);
	CloseHandle(d.handle);

	/* the same MATCH on a plain copy never makes the source removable */
	m = ready(WF_KIND_COPY);
	CHECK(open_file(&s, L"s1.bin", false) &&
	        open_file(&d, L"d1.bin", true));
	memset(&h, 0, sizeof h);
	h.writer = INVALID_HANDLE_VALUE;
	CHECK(wf_verify_handles(&m, s.handle, &s.id, d.handle, &d.id,
	        BODY_BYTES, hook, &h, &report));
	CHECK(wf_stage_complete(&m) && !wf_stage_may_remove_source(&m));
	CloseHandle(s.handle);
	CloseHandle(d.handle);
}

static void a_single_flipped_bit_stops_the_move(void)
{
	File s, d;
	WfStageMachine m = ready(WF_KIND_MOVE_CROSS_VOLUME);
	WfIntegrityReport report;
	Hook h = {0};

	CHECK(make_pair(L"s2.bin", L"d2.bin", true)); /* last bytes differ */
	CHECK(open_file(&s, L"s2.bin", false) &&
	        open_file(&d, L"d2.bin", true));
	h.writer = INVALID_HANDLE_VALUE;
	CHECK(!wf_verify_handles(&m, s.handle, &s.id, d.handle, &d.id,
	        BODY_BYTES, hook, &h, &report));
	CHECK(WF_STAGE_STOPPED == m.stage && WF_STOP_MISMATCH == m.stop);
	CHECK(!wf_stage_may_remove_source(&m));
	CHECK(!wf_stage_verified_recorded(&m, true) && !wf_stage_complete(&m));
	CloseHandle(s.handle);
	CloseHandle(d.handle);
}

static void identity_and_size_changes_stop_the_move(void)
{
	File s, d, other;
	WfStageMachine m;
	WfIntegrityReport report;
	Hook h = {0};

	CHECK(make_pair(L"s3.bin", L"d3.bin", false));
	CHECK(open_file(&s, L"s3.bin", false) &&
	        open_file(&d, L"d3.bin", true));
	CHECK(make_pair(L"s3b.bin", L"d3b.bin", false));
	CHECK(open_file(&other, L"s3b.bin", false));
	h.writer = INVALID_HANDLE_VALUE;

	/* the source is no longer the object that was accepted */
	m = ready(WF_KIND_MOVE_CROSS_VOLUME);
	CHECK(!wf_verify_handles(&m, s.handle, &other.id, d.handle, &d.id,
	        BODY_BYTES, hook, &h, &report));
	CHECK(WF_STOP_CHANGED == m.stop && !wf_stage_may_remove_source(&m));

	/* the destination is not the object that was copied to */
	m = ready(WF_KIND_MOVE_CROSS_VOLUME);
	CHECK(!wf_verify_handles(&m, s.handle, &s.id, d.handle, &other.id,
	        BODY_BYTES, hook, &h, &report));
	CHECK(WF_STOP_CHANGED == m.stop);

	/* a size that is not the recorded size is never "verified" */
	m = ready(WF_KIND_MOVE_CROSS_VOLUME);
	CHECK(!wf_verify_handles(&m, s.handle, &s.id, d.handle, &d.id,
	        BODY_BYTES - 1, hook, &h, &report));
	CHECK(WF_STAGE_STOPPED == m.stage && !wf_stage_may_remove_source(&m));
	CloseHandle(s.handle);
	CloseHandle(d.handle);
	CloseHandle(other.handle);
}

static void concurrent_modification_during_read_stops_the_move(void)
{
	File s, d;
	WfStageMachine m = ready(WF_KIND_MOVE_CROSS_VOLUME);
	WfIntegrityReport report;
	Hook h = {0};
	wchar_t path[1024];
	HANDLE writer;

	CHECK(make_pair(L"s4.bin", L"d4.bin", false));
	CHECK(open_file(&s, L"s4.bin", false) &&
	        open_file(&d, L"d4.bin", true));
	CHECK(p0_lab_path(&lab, L"d4.bin", path, 1024));
	writer = CreateFileW(path, GENERIC_WRITE,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(INVALID_HANDLE_VALUE != writer);
	/* change an already-read part of the destination while it is read */
	h.act_at = 3;
	h.writer = writer;
	CHECK(!wf_verify_handles(&m, s.handle, &s.id, d.handle, &d.id,
	        BODY_BYTES, hook, &h, &report));
	CHECK(WF_STAGE_STOPPED == m.stage &&
	        (WF_STOP_MISMATCH == m.stop || WF_STOP_CHANGED == m.stop));
	CHECK(!wf_stage_may_remove_source(&m));
	CloseHandle(writer);
	CloseHandle(s.handle);
	CloseHandle(d.handle);
}

static void cancellation_and_io_failure_stop_the_move(void)
{
	File s, d;
	WfStageMachine m;
	WfIntegrityReport report;
	Hook h = {0};

	CHECK(make_pair(L"s5.bin", L"d5.bin", false));
	CHECK(open_file(&s, L"s5.bin", false) &&
	        open_file(&d, L"d5.bin", true));

	m = ready(WF_KIND_MOVE_CROSS_VOLUME);
	h.act_at = 2;
	h.cancel = true;
	h.writer = INVALID_HANDLE_VALUE;
	CHECK(!wf_verify_handles(&m, s.handle, &s.id, d.handle, &d.id,
	        BODY_BYTES, hook, &h, &report));
	CHECK(WF_STOP_CANCELLED == m.stop && !wf_stage_may_remove_source(&m));

	/* a handle that cannot be read is an error, never a match */
	m = ready(WF_KIND_MOVE_CROSS_VOLUME);
	memset(&h, 0, sizeof h);
	h.writer = INVALID_HANDLE_VALUE;
	CloseHandle(d.handle);
	CHECK(!wf_verify_handles(&m, s.handle, &s.id, d.handle, &d.id,
	        BODY_BYTES, hook, &h, &report));
	CHECK(WF_STAGE_STOPPED == m.stage && WF_STOP_NONE != m.stop &&
	        !wf_stage_may_remove_source(&m));
	CloseHandle(s.handle);
}

/* Reporting a verification before the destination was flushed is refused even
 * if the bytes are equal: durability and read-back are separate stages. */
static void verification_cannot_precede_durability(void)
{
	File s, d;
	WfVerifyPlan plan;
	WfStageMachine m;
	WfIntegrityReport report;
	Hook h = {0};

	CHECK(wf_verify_plan(WF_KIND_MOVE_CROSS_VOLUME, WF_VERIFY_SHA256,
	              &plan) == WF_OK);
	wf_stage_init(&m, &plan);
	CHECK(wf_stage_copied(&m, true) && wf_stage_source_accepted(&m, true));
	CHECK(make_pair(L"s6.bin", L"d6.bin", false));
	CHECK(open_file(&s, L"s6.bin", false) &&
	        open_file(&d, L"d6.bin", true));
	h.writer = INVALID_HANDLE_VALUE;
	CHECK(!wf_verify_handles(&m, s.handle, &s.id, d.handle, &d.id,
	        BODY_BYTES, hook, &h, &report));
	CHECK(WF_STOP_OUT_OF_ORDER == m.stop &&
	        !wf_stage_may_remove_source(&m));
	CloseHandle(s.handle);
	CloseHandle(d.handle);
}

static void result_mapping_is_total(void)
{
	CHECK(WF_VERDICT_MATCH == wf_verify_verdict(WF_INTEGRITY_MATCH));
	CHECK(WF_VERDICT_MISMATCH == wf_verify_verdict(WF_INTEGRITY_MISMATCH));
	CHECK(WF_VERDICT_CHANGED == wf_verify_verdict(WF_INTEGRITY_CHANGED));
	CHECK(WF_VERDICT_CANCELLED ==
	        wf_verify_verdict(WF_INTEGRITY_CANCELLED));
	CHECK(WF_VERDICT_ERROR == wf_verify_verdict(WF_INTEGRITY_ERROR));
	CHECK(WF_VERDICT_ERROR == wf_verify_verdict((WfIntegrityResult)42));
}

int main(void)
{
	CHECK(p0_lab_open(&lab, "verify", 0));
	if (!lab.area) {
		return 1;
	}
	result_mapping_is_total();
	match_authorizes_removal_only_for_a_cross_volume_move();
	a_single_flipped_bit_stops_the_move();
	identity_and_size_changes_stop_the_move();
	concurrent_modification_during_read_stops_the_move();
	cancellation_and_io_failure_stop_the_move();
	verification_cannot_precede_durability();
	CHECK(0 == p0_lab_close(&lab, 0 != wf_test_failures));
	return TEST_RESULT();
}
