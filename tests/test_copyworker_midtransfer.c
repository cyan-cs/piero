/*
 * Pause, Resume and Cancel in the MIDDLE of a transfer, through the real copy
 * worker and engine on a scratch area.
 *
 * Synchronisation is the worker's own progress: the test pauses the moment
 * bytes_done passes a floor and then waits for the worker to report
 * WF_CS_PAUSED, so nothing here depends on a Sleep landing at the right time.
 * A copy that ends before the pause lands fails the test (enlarge the file);
 * it is never counted as a pass.
 */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tools/p0/p0_common.h"
#include "../tools/p0/p0_lab.h"
#include "base/wf_macro.h"
#include "wf_test.h"
#include "xfer/wf_copyworker.h"
#include "xfer/wf_probe_win.h"

#define SOURCE_MIB 192u
#define PAUSE_FLOOR_BYTES (8ull << 20)

static P0Lab lab;
static wchar_t root[1024], src_dir[1024], dst_dir[1024], journal[1100];
static wchar_t src_file[1100], dst_file[1100];
static WfProbeOps ops;

/* Distinct content in every MiB, so a misplaced or repeated block is seen. */
static bool put_big(const wchar_t *path, unsigned mib)
{
	HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	unsigned char *block = malloc(1u << 20);
	bool ok = INVALID_HANDLE_VALUE != h && block;

	for (unsigned m = 0; ok && m < mib; m++) {
		DWORD put = 0;

		for (size_t i = 0; i < (1u << 20); i++) {
			block[i] = (unsigned char)(i * 7 + (i >> 9) + m * 31);
		}
		ok = WriteFile(h, block, 1u << 20, &put, NULL) &&
		        put == 1u << 20;
	}
	if (INVALID_HANDLE_VALUE != h) {
		CloseHandle(h);
	}
	free(block);
	return ok;
}

static bool exists(const wchar_t *p)
{
	return INVALID_FILE_ATTRIBUTES != GetFileAttributesW(p);
}

/* Size of the engine's temporary in the destination, or UINT64_MAX if none.
 * Read through a handle: the size in a directory entry is not updated until
 * the writer closes the file. */
static uint64_t temporary_size(size_t *count)
{
	wchar_t pattern[1100], path[1100];
	WIN32_FIND_DATAW fd;
	HANDLE f;
	uint64_t size = UINT64_MAX;

	*count = 0;
	swprintf(pattern, 1100, L"%ls\\.wfcopy-*.tmp", dst_dir);
	f = FindFirstFileW(pattern, &fd);
	if (INVALID_HANDLE_VALUE == f) {
		return size;
	}
	do {
		LARGE_INTEGER n;
		HANDLE h;

		(*count)++;
		swprintf(path, 1100, L"%ls\\%ls", dst_dir, fd.cFileName);
		h = CreateFileW(path, FILE_READ_ATTRIBUTES,
		        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (INVALID_HANDLE_VALUE != h) {
			if (GetFileSizeEx(h, &n)) {
				size = (uint64_t)n.QuadPart;
			}
			CloseHandle(h);
		}
	} while (FindNextFileW(f, &fd));
	FindClose(f);
	return size;
}

static WfCopySpec job_for(const wchar_t *name)
{
	const wchar_t *names[1];
	WfCopySpec s;
	WfNodeInfo d;

	names[0] = name;
	memset(&s, 0, sizeof s);
	wf_slot_init(&s.slot);
	CHECK(wf_xfer_capture(&ops, WF_XFER_COPY, src_dir, names, 1, 0,
	              &s.slot) == WF_OK);
	s.dest = _wcsdup(dst_dir);
	CHECK(WF_OK == ops.node(NULL, dst_dir, &d));
	s.dest_id = d.id;
	s.kind = WF_CJ_COPY;
	s.verify = WF_VERIFY_SHA256;
	s.collide = WF_COLLIDE_STOP;
	s.total_files = 1;
	s.total_bytes = s.slot.src[0].snap.size;
	return s;
}

static size_t collect(WfCopyWorker *w, WfCopyResult *out, unsigned ms)
{
	ULONGLONG until = GetTickCount64() + ms;

	while (GetTickCount64() < until) {
		if (wf_copyworker_poll(w, out)) {
			return 1;
		}
		WaitForSingleObject(wf_copyworker_event(w), 50);
	}
	return 0;
}

/*
 * Pauses as soon as the copy has passed the floor, then waits until the
 * worker itself says PAUSED. Returns false (a test failure) when the job
 * ended first or never reached the floor.
 */
static bool pause_mid_transfer(WfCopyWorker *w, WfCopyProgress *at)
{
	ULONGLONG until = GetTickCount64() + 30000;
	int old_priority = GetThreadPriority(GetCurrentThread());
	bool landed = false;

	/*
	 * The pause is a request the worker honours at its next checkpoint, one
	 * per MiB. On a loaded machine this thread, spinning on the counter, can
	 * be kept off the CPU for longer than the worker needs to write the rest
	 * of the file; the worker then parks at its last checkpoint with every
	 * byte written, which is a correct pause but not a mid-transfer one.
	 * A higher priority while it spins keeps the request ahead of the copy.
	 */
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
	for (;;) {
		if (GetTickCount64() > until ||
		        !wf_copyworker_progress(w, at)) {
			goto out; /* finished, or stuck, before reaching it */
		}
		if (at->bytes_done >= PAUSE_FLOOR_BYTES) {
			break;
		}
	}
	wf_copyworker_set_paused(w, true);
	until = GetTickCount64() + 5000;
	while (GetTickCount64() < until) {
		if (wf_copyworker_progress(w, at) &&
		        WF_CS_PAUSED == at->state) {
			landed = true;
			break;
		}
		Sleep(1);
	}
out:
	SetThreadPriority(GetCurrentThread(), old_priority);
	return landed;
}

static void remove_destination(void)
{
	SetFileAttributesW(dst_file, FILE_ATTRIBUTE_NORMAL);
	DeleteFileW(dst_file);
}

static void pause_resume_in_the_middle(const char *source_hash)
{
	WfCopyWorker *w;
	WfCopySpec a;
	WfCopyResult r;
	WfCopyProgress held, later, after;
	uint64_t temp_held;
	size_t temps;
	char dest_hash[P0_HASH_HEX], source_after[P0_HASH_HEX];
	bool grew = false;
	ULONGLONG until;

	remove_destination();
	CHECK(WF_OK == wf_copyworker_start(&w, journal));
	a = job_for(L"big.bin");
	CHECK(WF_OK == wf_copyworker_submit(w, &a, NULL));

	CHECK(pause_mid_transfer(w, &held));
	/* a pause in the middle: something was copied, not everything */
	CHECK(held.bytes_done >= PAUSE_FLOOR_BYTES &&
	        held.bytes_done < held.bytes_total);
	temp_held = temporary_size(&temps);
	CHECK(1 == temps && temp_held > 0 && temp_held < held.bytes_total);
	CHECK(!exists(dst_file)); /* the final name is not given while paused */

	/* while paused nothing moves: counter, temporary size, final name */
	Sleep(700);
	CHECK(wf_copyworker_progress(w, &later) && WF_CS_PAUSED == later.state);
	printf("midtransfer: held=%llu later=%llu total=%llu temp=%llu "
	       "temps=%zu\n",
	        (unsigned long long)held.bytes_done,
	        (unsigned long long)later.bytes_done,
	        (unsigned long long)held.bytes_total,
	        (unsigned long long)temp_held, temps);
	CHECK(later.bytes_done == held.bytes_done);
	CHECK(temporary_size(&temps) == temp_held && 1 == temps);
	CHECK(!exists(dst_file));

	/* resume: the counter moves again and the job ends complete */
	wf_copyworker_set_paused(w, false);
	until = GetTickCount64() + 20000;
	while (GetTickCount64() < until) {
		if (!wf_copyworker_progress(w, &after)) {
			break; /* already finished */
		}
		if (after.bytes_done > held.bytes_done) {
			grew = true;
			CHECK(WF_CS_PAUSED != after.state);
			break;
		}
	}
	CHECK(grew || exists(dst_file));
	CHECK(1 == collect(w, &r, 30000));
	CHECK(WF_CS_DONE == r.state && 1 == r.files_copied);
	CHECK(exists(dst_file));
	temporary_size(&temps);
	CHECK(0 == temps);

	/* the copy and the untouched source, hashed here and not by the engine */
	CHECK(p0_sha256_file(dst_file, dest_hash));
	CHECK(0 == strcmp(source_hash, dest_hash));
	CHECK(p0_sha256_file(src_file, source_after));
	CHECK(0 == strcmp(source_hash, source_after));

	wf_copyresult_free(&r);
	wf_copyspec_free(&a);
	CHECK(WF_OK == wf_copyworker_stop(w).status);
}

static void pause_then_cancel_in_the_middle(const char *source_hash)
{
	WfCopyWorker *w;
	WfCopySpec a;
	WfCopyResult r;
	WfCopyProgress held, idle;
	size_t temps;
	char source_after[P0_HASH_HEX];

	remove_destination();
	CHECK(WF_OK == wf_copyworker_start(&w, journal));
	a = job_for(L"big.bin");
	CHECK(WF_OK == wf_copyworker_submit(w, &a, NULL));

	CHECK(pause_mid_transfer(w, &held));
	CHECK(held.bytes_done >= PAUSE_FLOOR_BYTES &&
	        held.bytes_done < held.bytes_total);
	temporary_size(&temps);
	CHECK(1 == temps); /* there is something to clean up */

	wf_copyworker_cancel(w, true);
	CHECK(1 == collect(w, &r, 20000));
	CHECK(WF_CS_CANCELLED == r.state);
	CHECK(!exists(dst_file)); /* an unfinished file is never published */
	temporary_size(&temps);
	CHECK(0 == temps); /* and its temporary is gone */
	CHECK(!wf_copyworker_progress(w, &idle)); /* nothing left running */

	CHECK(p0_sha256_file(src_file, source_after));
	CHECK(0 == strcmp(source_hash, source_after));

	wf_copyresult_free(&r);
	wf_copyspec_free(&a);
	CHECK(WF_OK == wf_copyworker_stop(w).status);
}

int main(void)
{
	char source_hash[P0_HASH_HEX];

	ops = wf_probe_win();
	CHECK(p0_lab_open(&lab, "copyworker-mid", 0));
	if (!lab.area) {
		return TEST_RESULT();
	}
	wcscpy_s(root, 1024, lab.root);
	while (wcslen(root) > 3 && L'\\' == root[wcslen(root) - 1]) {
		root[wcslen(root) - 1] = 0;
	}
	CHECK(p0_area_mkdir(lab.area, L"s"));
	CHECK(p0_area_mkdir(lab.area, L"d"));
	swprintf(src_dir, 1024, L"%ls\\s", root);
	swprintf(dst_dir, 1024, L"%ls\\d", root);
	swprintf(journal, 1100, L"%ls\\journal.txt", root);
	swprintf(src_file, 1100, L"%ls\\big.bin", src_dir);
	swprintf(dst_file, 1100, L"%ls\\big.bin", dst_dir);

	/* the source, its copy and the staging file; refuse before writing a byte */
	CHECK(p0_free_space_for(root, 3ull * SOURCE_MIB << 20));
	if (wf_test_failures) {
		return TEST_RESULT();
	}
	CHECK(put_big(src_file, SOURCE_MIB));
	CHECK(p0_sha256_file(src_file, source_hash));
	pause_resume_in_the_middle(source_hash);
	pause_then_cancel_in_the_middle(source_hash);

	/* the fixture's own cleanup */
	remove_destination();
	SetFileAttributesW(src_file, FILE_ATTRIBUTE_NORMAL);
	DeleteFileW(src_file);
	{
		wchar_t j[1100];

		swprintf(j, 1100, L"%ls\\journal.txt", root);
		SetFileAttributesW(j, FILE_ATTRIBUTE_NORMAL);
		DeleteFileW(j);
	}
	CHECK(0 == p0_lab_close(&lab, 0 != wf_test_failures));
	return TEST_RESULT();
}
