/* Independent storage PoC. All mutations target this run's manifested area.
 * It contains no production engine, rollback or recovery deletion capability.
 */
#include <windows.h>

#include <sddl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/fs/wf_enum_win.h"
#include "../../src/path/wf_path.h"
#include "../../src/xfer/wf_probe_win.h"
#include "p0_common.h"
#include "p0_lab.h"
#include "p0_source_guard.h"

#define CAP 32768
static P0Lab lab;
static WfProbeOps ops;
static wchar_t exe[1024], evidence[1024], run_name[128], cloud_path[1024];
static FILE *csv[3];
static unsigned passed[3], failed[3], sequence;
static int failures;
#define REQUIRE(c)                                                             \
	do {                                                                   \
		if (!(c)) {                                                    \
			fprintf(stderr, "FAIL %s:%d: %s (error=%lu)\n",        \
			        __FILE__, __LINE__, #c, GetLastError());       \
			p0_lab_close(&lab, 1);                                 \
			ExitProcess(1);                                        \
		}                                                              \
	} while (0)

typedef struct
{
	wchar_t src[1024], dst[1024], a[1024], b[1024], sr[64], dr[64];
} Fixture;

static int new_file(
        const wchar_t *rel, const char *text, SECURITY_ATTRIBUTES *security)
{
	wchar_t *path = malloc(CAP * sizeof *path);
	wchar_t *extended = NULL;
	HANDLE h = INVALID_HANDLE_VALUE;
	DWORD written;
	int ok = 0;
	if (!path || !p0_lab_path(&lab, rel, path, CAP)) {
		goto done;
	}
	if (WF_OK != wf_path_to_extended(path, &extended)) {
		goto done;
	}
	h = CreateFileW(extended, GENERIC_WRITE, 0, security, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		goto done;
	}
	if (!WriteFile(h, text, (DWORD)strlen(text), &written, NULL) ||
	        written != strlen(text)) {
		goto done;
	}
	CloseHandle(h);
	h = INVALID_HANDLE_VALUE;
	ok = p0_area_track(lab.area, rel, 0);
done:
	if (INVALID_HANDLE_VALUE != h) {
		CloseHandle(h);
	}
	free(extended);
	free(path);
	return ok;
}

static int prepare(Fixture *f)
{
	wchar_t rel[128];
	swprintf(f->sr, 64, L"src%u", ++sequence);
	swprintf(f->dr, 64, L"dst%u", sequence);
	if (!p0_area_mkdir(lab.area, f->sr) ||
	        !p0_area_mkdir(lab.area, f->dr)) {
		return 0;
	}
	if (!p0_lab_path(&lab, f->sr, f->src, 1024) ||
	        !p0_lab_path(&lab, f->dr, f->dst, 1024)) {
		return 0;
	}
	swprintf(rel, 128, L"%ls\\a", f->sr);
	if (!new_file(rel, "fixture", NULL) ||
	        !p0_lab_path(&lab, rel, f->a, 1024)) {
		return 0;
	}
	swprintf(rel, 128, L"%ls\\b", f->sr);
	return new_file(rel, "fixture", NULL) &&
	        p0_lab_path(&lab, rel, f->b, 1024);
}

static size_t entries(const wchar_t *dir)
{
	WfDir *d = NULL;
	size_t n = SIZE_MAX;
	if (WF_OK == wf_enum_dir_win(dir, &d)) {
		n = d->n;
	}
	wf_dir_free(d);
	return n;
}

static void result(unsigned group,
        const char *name,
        int ok,
        DWORD error,
        int expected,
        int actual,
        const WfPreflightReport *report,
        const WfFileId *id,
        const char *details)
{
	size_t i;
	if (ok) {
		passed[group]++;
	} else {
		failed[group]++;
		failures++;
	}
	printf("%s S%02u %s expected=%d actual=%d error=%lu %s\n",
	        ok ? "PASS" : "FAIL", group + 8, name, expected, actual, error,
	        details);
	if (0 == group) {
		DWORD handles;
		GetProcessHandleCount(GetCurrentProcess(), &handles);
		printf("  process_handles=%lu\n", handles);
	}
	if (!csv[group]) {
		return;
	}
	fprintf(csv[group], "%s,%s,%d,%d,%lu,%llu,", name, ok ? "PASS" : "FAIL",
	        expected, actual, error,
	        (unsigned long long)(id ? id->volume : 0));
	if (id) {
		for (i = 0; i < 16; i++) {
			fprintf(csv[group], "%02x", id->id[i]);
		}
	}
	fprintf(csv[group], ",%zu,%zu,%zu,%zu,%llu,%llu,%llu,%llu,",
	        report ? report->longest_final : 0,
	        report ? report->longest_txn : 0,
	        report ? report->max_path_units : 0,
	        report ? report->max_component : 0,
	        (unsigned long long)WF_PREFLIGHT_DEFAULT.reserve_bytes,
	        (unsigned long long)(report ? report->reserve_bytes : 0),
	        (unsigned long long)(report ? report->free_bytes : 0),
	        (unsigned long long)(report ? report->need_bytes : 0));
	p0_json_str(csv[group], details);
	fputc('\n', csv[group]);
	fflush(csv[group]);
}

static int hold_child(int argc, wchar_t **argv)
{
	HANDLE ready, done, file;
	DWORD error;
	if (6 != argc) {
		return 2;
	}
	ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[4]);
	done = OpenEventW(SYNCHRONIZE, FALSE, argv[5]);
	if (!ready || !done) {
		return 3;
	}
	file = CreateFileW(argv[2],
	        !wcscmp(argv[3], L"office") ? GENERIC_READ | GENERIC_WRITE
	                                    : FILE_APPEND_DATA,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING, 0, NULL);
	error = INVALID_HANDLE_VALUE == file ? GetLastError() : 0;
	SetEvent(ready);
	if (!error && WAIT_OBJECT_0 != WaitForSingleObject(done, 10000)) {
		error = ERROR_TIMEOUT;
	}
	if (INVALID_HANDLE_VALUE != file) {
		CloseHandle(file);
	}
	CloseHandle(done);
	CloseHandle(ready);
	return (int)error;
}

static void writer_case(int append)
{
	Fixture f;
	wchar_t ready_name[128], done_name[128], command[4096];
	HANDLE ready, done;
	STARTUPINFOW si = {0};
	PROCESS_INFORMATION pi = {0};
	WfNodeInfo snapshot;
	P0SourceGuard guard;
	P0GuardResult got = P0_SOURCE_BUSY;
	DWORD error = 0, code = 1;
	int ok = prepare(&f);
	REQUIRE(ok);
	if (!ok) {
		return;
	}
	REQUIRE(WF_OK == ops.node(NULL, f.a, &snapshot));
	swprintf(ready_name, 128, L"Local\\wfile-ready-%lu-%u",
	        GetCurrentProcessId(), sequence);
	swprintf(done_name, 128, L"Local\\wfile-done-%lu-%u",
	        GetCurrentProcessId(), sequence);
	ready = CreateEventW(NULL, TRUE, FALSE, ready_name);
	done = CreateEventW(NULL, TRUE, FALSE, done_name);
	REQUIRE(ready && done);
	swprintf(command, 4096, L"\"%ls\" --hold \"%ls\" %ls \"%ls\" \"%ls\"",
	        exe, f.a, append ? L"append" : L"office", ready_name,
	        done_name);
	si.cb = sizeof si;
	ok = ready && done &&
	        CreateProcessW(exe, command, NULL, NULL, FALSE,
	                CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
	REQUIRE(ok);
	if (ok) {
		REQUIRE(WAIT_OBJECT_0 == WaitForSingleObject(ready, 5000));
		got = p0_source_guard_acquire(f.a, &snapshot, &guard, &error);
		p0_source_guard_release(&guard);
		SetEvent(done);
		if (WAIT_OBJECT_0 != WaitForSingleObject(pi.hProcess, 5000)) {
			TerminateProcess(pi.hProcess, 99);
		}
		REQUIRE(GetExitCodeProcess(pi.hProcess, &code) && 0 == code);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
	}
	if (done) {
		CloseHandle(done);
	}
	if (ready) {
		CloseHandle(ready);
	}
	result(0, append ? "append_log_process" : "office_writer_process",
	        ok && 0 == code && P0_SOURCE_BUSY == got &&
	                ERROR_SHARING_VIOLATION == error,
	        error, P0_SOURCE_BUSY, got, NULL, &snapshot.id,
	        "real separate writer process; failed guard exposes "
	        "SOURCE_BUSY");
}

static void guard_cases(void)
{
	Fixture f;
	P0SourceGuard guard;
	WfNodeInfo snapshot;
	P0GuardResult got;
	DWORD error, written;
	HANDLE h;
	wchar_t rel[128], target[1024];
	char before_hash[65], after_hash[65];
	int ok = prepare(&f), acquired;
	REQUIRE(ok);
	if (!ok) {
		return;
	}
	REQUIRE(WF_OK == ops.node(NULL, f.a, &snapshot));
	got = p0_source_guard_acquire(f.a, &snapshot, &guard, &error);
	acquired = P0_GUARD_OK == got;
	if (acquired) {
		swprintf(rel, 128, L"%ls\\a", f.dr);
		REQUIRE(p0_lab_path(&lab, rel, target, 1024));
		ok = CopyFileExW(f.a, target, NULL, NULL, NULL,
		             COPY_FILE_FAIL_IF_EXISTS) != 0;
		error = ok ? 0 : GetLastError();
		if (ok) {
			REQUIRE(p0_area_track(lab.area, rel, 0));
		}
		REQUIRE(p0_sha256_file(f.a, before_hash) &&
		        p0_sha256_file(target, after_hash));
		ok = ok && !strcmp(before_hash, after_hash);
	}
	result(0, "guarded_copy", acquired && ok, error, P0_GUARD_OK, got, NULL,
	        &snapshot.id,
	        "guard held across actual CopyFileExW, FAIL_IF_EXISTS, "
	        "matching SHA256; no cancellation");
	if (acquired) {
		DWORD access[] = {GENERIC_WRITE, FILE_APPEND_DATA, DELETE};
		unsigned i;
		const char *names[] = {"write_while_guarded",
		        "append_while_guarded", "delete_access_while_guarded"};
		for (i = 0; i < 3; i++) {
			h = CreateFileW(f.a, access[i],
			        FILE_SHARE_READ | FILE_SHARE_WRITE |
			                FILE_SHARE_DELETE,
			        NULL, OPEN_EXISTING, 0, NULL);
			error = INVALID_HANDLE_VALUE == h ? GetLastError() : 0;
			if (INVALID_HANDLE_VALUE != h) {
				CloseHandle(h);
			}
			result(0, names[i],
			        INVALID_HANDLE_VALUE == h && 32 == error, error,
			        32, (int)error, NULL, &snapshot.id,
			        "access denied by FILE_SHARE_READ; no deletion "
			        "requested");
		}
		swprintf(target, 1024, L"%ls\\renamed", f.src);
		ok = 0 != MoveFileExW(f.a, target, 0);
		error = ok ? 0 : GetLastError();
		result(0, "rename_while_guarded", !ok && 32 == error, error, 32,
		        (int)error, NULL, &snapshot.id, "no replacement flags");
		h = CreateFileW(
		        f.a, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
		error = INVALID_HANDLE_VALUE == h ? GetLastError() : 0;
		if (INVALID_HANDLE_VALUE != h) {
			CloseHandle(h);
		}
		result(0, "recreate_while_guarded",
		        INVALID_HANDLE_VALUE == h && 32 == error, error, 32,
		        (int)error, NULL, &snapshot.id,
		        "test-generated source only; CREATE_ALWAYS must be "
		        "blocked");
	}
	p0_source_guard_release(&guard);
	p0_source_guard_release(&guard);
	h = CreateFileW(f.a, GENERIC_WRITE,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING, 0, NULL);
	result(0, "writer_after_release", INVALID_HANDLE_VALUE != h,
	        INVALID_HANDLE_VALUE == h ? GetLastError() : 0, 1,
	        INVALID_HANDLE_VALUE != h, NULL, &snapshot.id,
	        "positive control: guard release permits writer");
	if (INVALID_HANDLE_VALUE != h) {
		CloseHandle(h);
	}
	writer_case(0);
	writer_case(1);
	REQUIRE(prepare(&f));
	REQUIRE(WF_OK == ops.node(NULL, f.a, &snapshot));
	swprintf(target, 1024, L"%ls\\adopted", f.src);
	REQUIRE(MoveFileExW(f.a, target, 0));
	swprintf(rel, 128, L"%ls\\adopted", f.sr);
	REQUIRE(p0_area_track(lab.area, rel, 0));
	swprintf(rel, 128, L"%ls\\a", f.sr);
	REQUIRE(new_file(rel, "fixture", NULL));
	got = p0_source_guard_acquire(f.a, &snapshot, &guard, &error);
	result(0, "replacement_after_slot",
	        P0_SOURCE_CHANGED == got &&
	                INVALID_HANDLE_VALUE == guard.handle,
	        error, P0_SOURCE_CHANGED, got, NULL, &snapshot.id,
	        "old object retained while same bytes/new FileId occupy source "
	        "name");
	p0_source_guard_release(&guard);
	REQUIRE(WF_OK == ops.node(NULL, f.a, &snapshot));
	h = CreateFileW(f.a, FILE_APPEND_DATA, 0, NULL, OPEN_EXISTING, 0, NULL);
	REQUIRE(INVALID_HANDLE_VALUE != h);
	REQUIRE(WriteFile(h, "x", 1, &written, NULL));
	CloseHandle(h);
	got = p0_source_guard_acquire(f.a, &snapshot, &guard, &error);
	result(0, "growth_after_slot", P0_SOURCE_CHANGED == got, error,
	        P0_SOURCE_CHANGED, got, NULL, &snapshot.id,
	        "size/mtime change refuses");
	p0_source_guard_release(&guard);
	{
		PSECURITY_DESCRIPTOR sd = NULL;
		SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, FALSE};
		REQUIRE(ConvertStringSecurityDescriptorToSecurityDescriptorW(
		        L"D:P(D;;0x1;;;WD)(A;;FA;;;WD)", SDDL_REVISION_1, &sd,
		        NULL));
		sa.lpSecurityDescriptor = sd;
		swprintf(rel, 128, L"%ls\\no-read", f.sr);
		REQUIRE(new_file(rel, "fixture", &sa));
		LocalFree(sd);
		REQUIRE(p0_lab_path(&lab, rel, target, 1024));
		REQUIRE(WF_OK == ops.node(NULL, target, &snapshot));
		got = p0_source_guard_acquire(
		        target, &snapshot, &guard, &error);
		result(0, "source_acl_denied",
		        P0_SOURCE_BUSY == got && 5 == error, error,
		        P0_SOURCE_BUSY, got, NULL, &snapshot.id,
		        "DACL supplied only when creating this run's source "
		        "file");
		p0_source_guard_release(&guard);
	}
}

static WfStatus cloud_node(void *ctx, const wchar_t *path, WfNodeInfo *out)
{
	WfStatus st = ops.node(ctx, path, out);
	if (WF_OK == st && !wcscmp(path, cloud_path)) {
		out->flags |= WF_ENT_CLOUD;
	}
	return st;
}

static int junction(const wchar_t *relative, const wchar_t *target)
{
	wchar_t link[1024], cmd[4096], system_cmd[1024];
	STARTUPINFOW si = {0};
	PROCESS_INFORMATION pi = {0};
	DWORD code = 99;
	if (!p0_lab_path(&lab, relative, link, 1024) ||
	        !GetSystemDirectoryW(system_cmd, 1024)) {
		return 0;
	}
	wcscat_s(system_cmd, 1024, L"\\cmd.exe");
	swprintf(cmd, 4096, L"\"%ls\" /c mklink /J \"%ls\" \"%ls\" >nul 2>nul",
	        system_cmd, link, target);
	si.cb = sizeof si;
	if (!CreateProcessW(system_cmd, cmd, NULL, NULL, FALSE,
	            CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
		return 0;
	}
	if (WAIT_OBJECT_0 != WaitForSingleObject(pi.hProcess, 5000)) {
		TerminateProcess(pi.hProcess, 99);
	}
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return 0 == code && p0_area_track(lab.area, relative, 1);
}

/* State the batch scenarios share. */
typedef struct
{
	Fixture f;
	WfTransferSlot slot;
	WfPreflightPolicy policy;
	wchar_t target[1024], rel[128];
} BatchLab;

/* A complete batch starts nothing and writes nothing; a colliding second target stops it unchanged. */
static void batch_complete_and_collision(BatchLab *bl)
{
	const wchar_t *names[] = {L"a", L"b"};
	WfPreflightReport r;
	WfStatus st;
	char before[65], after[65];

	REQUIRE(prepare(&bl->f));
	REQUIRE(wf_xfer_capture(&ops, WF_XFER_COPY, bl->f.src, names, 2, 0,
	                &bl->slot) == WF_OK);
	st = wf_preflight_run(
	        &ops, &bl->slot, bl->f.dst, &bl->policy, NULL, &r);
	result(1, "complete_batch",
	        wf_preflight_can_start(st, &r, 0) && 2 == r.top_files &&
	                0 == entries(bl->f.dst),
	        0, WF_PF_OK, r.reason, &r, &bl->slot.src[0].snap.id,
	        "all top-level checks completed; preflight wrote zero entries; "
	        "no engine connected");
	swprintf(bl->rel, 128, L"%ls\\b", bl->f.dr);
	REQUIRE(new_file(bl->rel, "unrelated-destination", NULL));
	REQUIRE(p0_lab_path(&lab, bl->rel, bl->target, 1024));
	REQUIRE(p0_sha256_file(bl->target, before));
	st = wf_preflight_run(
	        &ops, &bl->slot, bl->f.dst, &bl->policy, NULL, &r);
	REQUIRE(p0_sha256_file(bl->target, after));
	result(1, "second_target_collision",
	        WF_PF_COLLISION == r.reason && 1 == r.bad_index &&
	                !wf_preflight_can_start(st, &r, 1) &&
	                1 == entries(bl->f.dst) && !strcmp(before, after),
	        0, WF_PF_COLLISION, r.reason, &r, &bl->slot.src[1].snap.id,
	        "zero copied items; collision bytes SHA256 unchanged; no a "
	        "target created");
	wf_slot_free(&bl->slot);
}

/* A configured reserve larger than the free space is refused. */
static void batch_reserve_exceeds_free(BatchLab *bl)
{
	const wchar_t *names[] = {L"a", L"b"};
	WfPreflightReport r;
	WfStatus st;

	REQUIRE(prepare(&bl->f));
	REQUIRE(wf_xfer_capture(&ops, WF_XFER_COPY, bl->f.src, names, 2, 0,
	                &bl->slot) == WF_OK);
	bl->policy.reserve_bytes = UINT64_MAX - 100;
	st = wf_preflight_run(
	        &ops, &bl->slot, bl->f.dst, &bl->policy, NULL, &r);
	result(1, "configured_reserve_exceeds_free",
	        WF_PF_NO_SPACE == r.reason &&
	                !wf_preflight_can_start(st, &r, 1) &&
	                0 == entries(bl->f.dst),
	        0, WF_PF_NO_SPACE, r.reason, &r, &bl->slot.src[0].snap.id,
	        "configured reserve UINT64_MAX-100; real GetDiskFreeSpaceExW; "
	        "not disk exhaustion or a measured recommended margin");
	bl->policy = WF_PREFLIGHT_DEFAULT;
}

/* A destination whose ACL denies adding entries is refused. */
static void batch_acl_denied(BatchLab *bl)
{
	WfPreflightReport r;
	WfStatus st;

	{
		PSECURITY_DESCRIPTOR sd = NULL;
		SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, FALSE};
		REQUIRE(ConvertStringSecurityDescriptorToSecurityDescriptorW(
		        L"D:P(D;;0x6;;;WD)(A;;FA;;;WD)", SDDL_REVISION_1, &sd,
		        NULL));
		sa.lpSecurityDescriptor = sd;
		swprintf(bl->rel, 128, L"denied%u", sequence);
		REQUIRE(p0_lab_path(&lab, bl->rel, bl->target, 1024));
		REQUIRE(CreateDirectoryW(bl->target, &sa));
		LocalFree(sd);
		REQUIRE(p0_area_track(lab.area, bl->rel, 1));
		{
			HANDLE probe = CreateFileW(bl->target,
			        FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY |
			                FILE_READ_ATTRIBUTES,
			        FILE_SHARE_READ | FILE_SHARE_WRITE |
			                FILE_SHARE_DELETE,
			        NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
			        NULL);
			DWORD observed_error = INVALID_HANDLE_VALUE == probe
			        ? GetLastError()
			        : 0;
			if (INVALID_HANDLE_VALUE != probe) {
				CloseHandle(probe);
			}
			st = wf_preflight_run(&ops, &bl->slot, bl->target,
			        &bl->policy, NULL, &r);
			result(1, "destination_acl_denied",
			        WF_PF_DEST_ACCESS == r.reason &&
			                !wf_preflight_can_start(st, &r, 1) &&
			                0 == entries(bl->target),
			        observed_error, WF_PF_DEST_ACCESS, r.reason, &r,
			        NULL,
			        "new fixture directory denies FILE_ADD_FILE "
			        "and FILE_ADD_SUBDIRECTORY; user ACLs "
			        "unchanged");
		}
	}
}

/* A successful preflight is not an execution guarantee: the source guard still refuses a changed source. */
static void batch_changed_after_preflight(BatchLab *bl)
{
	WfPreflightReport r;
	WfStatus st;

	{
		P0SourceGuard guard;
		P0GuardResult got;
		DWORD error;
		st = wf_preflight_run(
		        &ops, &bl->slot, bl->f.dst, &bl->policy, NULL, &r);
		REQUIRE(wf_preflight_can_start(st, &r, 0));
		swprintf(bl->target, 1024, L"%ls\\old-b", bl->f.src);
		REQUIRE(MoveFileExW(bl->f.b, bl->target, 0));
		swprintf(bl->rel, 128, L"%ls\\old-b", bl->f.sr);
		REQUIRE(p0_area_track(lab.area, bl->rel, 0));
		swprintf(bl->rel, 128, L"%ls\\b", bl->f.sr);
		REQUIRE(new_file(bl->rel, "fixture", NULL));
		got = p0_source_guard_acquire(
		        bl->f.b, &bl->slot.src[1].snap, &guard, &error);
		result(1, "changed_after_successful_preflight",
		        P0_SOURCE_CHANGED == got && 0 == entries(bl->f.dst),
		        error, P0_SOURCE_CHANGED, got, &r,
		        &bl->slot.src[1].snap.id,
		        "successful preflight is not an execution guarantee; "
		        "separate guard refuses before any copy");
		p0_source_guard_release(&guard);
	}
	wf_slot_free(&bl->slot);
}

/* Cloud consent is policy only; an injected transition after capture cannot bypass it. */
static void batch_injected_cloud(BatchLab *bl)
{
	const wchar_t *names[] = {L"a", L"b"};
	WfPreflightReport r;
	WfStatus st;

	REQUIRE(prepare(&bl->f));
	{
		WfProbeOps injected = ops;
		injected.node = cloud_node;
		wcscpy_s(cloud_path, 1024, bl->f.b);
		REQUIRE(wf_xfer_capture(&injected, WF_XFER_COPY, bl->f.src,
		                names, 2, 0, &bl->slot) == WF_OK);
		st = wf_preflight_run(
		        &injected, &bl->slot, bl->f.dst, &bl->policy, NULL, &r);
		result(1, "injected_cloud_consent",
		        1 == r.cloud_files && 7 == r.cloud_bytes &&
		                !wf_preflight_can_start(st, &r, 0) &&
		                wf_preflight_can_start(st, &r, 1) &&
		                0 == entries(bl->f.dst),
		        0, 1, (int)r.cloud_files, &r, &bl->slot.src[1].snap.id,
		        "injected CLOUD attribute on a real ordinary fixture; "
		        "policy only; real cloud provider NOT_TESTED; no "
		        "hydration attempted");
		wf_slot_free(&bl->slot);
		REQUIRE(wf_xfer_capture(&ops, WF_XFER_COPY, bl->f.src, names, 2,
		                0, &bl->slot) == WF_OK);
		st = wf_preflight_run(
		        &injected, &bl->slot, bl->f.dst, &bl->policy, NULL, &r);
		result(1, "injected_cloud_after_slot",
		        WF_PF_SOURCE_CHANGED == r.reason &&
		                !wf_preflight_can_start(st, &r, 0) &&
		                0 == entries(bl->f.dst),
		        0, WF_PF_SOURCE_CHANGED, r.reason, &r,
		        &bl->slot.src[1].snap.id,
		        "injected CLOUD transition after registration; stale "
		        "non-cloud snapshot cannot bypass consent; real "
		        "provider NOT_TESTED");
		wf_slot_free(&bl->slot);
		cloud_path[0] = 0;
	}
}

/* A source junction and a destination junction are both refused. */
static void batch_junctions(BatchLab *bl)
{
	const wchar_t *names[] = {L"a", L"b"};
	WfPreflightReport r;
	WfStatus st;

	{
		const wchar_t *link_name[] = {L"junction"};
		REQUIRE(prepare(&bl->f));
		swprintf(bl->rel, 128, L"%ls\\junction", bl->f.sr);
		REQUIRE(junction(bl->rel, bl->f.dst));
		REQUIRE(p0_lab_path(&lab, bl->rel, bl->target, 1024));
		REQUIRE(wf_xfer_capture(&ops, WF_XFER_COPY, bl->f.src,
		                link_name, 1, 0, &bl->slot) == WF_OK);
		st = wf_preflight_run(
		        &ops, &bl->slot, bl->f.dst, &bl->policy, NULL, &r);
		result(1, "source_junction_refused",
		        WF_PF_REPARSE == r.reason &&
		                !wf_preflight_can_start(st, &r, 1) &&
		                0 == entries(bl->f.dst),
		        0, WF_PF_REPARSE, r.reason, &r,
		        &bl->slot.src[0].snap.id,
		        "real directory junction; no traversal or hydration");
		wf_slot_free(&bl->slot);
		REQUIRE(wf_xfer_capture(&ops, WF_XFER_COPY, bl->f.src, names, 2,
		                0, &bl->slot) == WF_OK);
		st = wf_preflight_run(
		        &ops, &bl->slot, bl->target, &bl->policy, NULL, &r);
		result(1, "destination_junction_refused",
		        WF_PF_DEST_REPARSE == r.reason &&
		                !wf_preflight_can_start(st, &r, 1) &&
		                0 == entries(bl->f.dst),
		        0, WF_PF_DEST_REPARSE, r.reason, &r, NULL,
		        "real destination reparse point refused; all ancestor "
		        "checks enabled");
		wf_slot_free(&bl->slot);
	}
}

static void batch_cases(void)
{
	BatchLab bl;

	memset(&bl, 0, sizeof bl);
	bl.policy = WF_PREFLIGHT_DEFAULT;
	batch_complete_and_collision(&bl);
	batch_reserve_exceeds_free(&bl);
	batch_acl_denied(&bl);
	batch_changed_after_preflight(&bl);
	batch_injected_cloud(&bl);
	batch_junctions(&bl);
}

/* State the path scenarios hand to each other: the fixture, the deepest
 * directory created so far, and the transfer slot. */
typedef struct
{
	wchar_t *rel, *path, *target;
	Fixture f;
	WfTransferSlot slot;
	WfPreflightPolicy policy;
} PathLab;

/* Unicode names count UTF-16 units; a configured limit the transaction path exceeds is refused. */
static void path_unicode_and_tight_limit(PathLab *pl)
{
	const wchar_t *names[] = {L"漢😀"};
	WfPreflightReport r;
	WfVolInfo volume;
	WfStatus st;

	swprintf(pl->rel, CAP, L"%ls\\漢😀", pl->f.sr);
	REQUIRE(new_file(pl->rel, "fixture", NULL));
	REQUIRE(wf_xfer_capture(&ops, WF_XFER_COPY, pl->f.src, names, 1, 0,
	                &pl->slot) == WF_OK);
	REQUIRE(WF_OK == ops.volume(NULL, pl->f.dst, &volume));
	st = wf_preflight_run(
	        &ops, &pl->slot, pl->f.dst, &pl->policy, NULL, &r);
	result(2, "unicode_utf16",
	        wf_preflight_can_start(st, &r, 0) &&
	                r.max_component == volume.max_component &&
	                0 == entries(pl->f.dst),
	        0, 1, wf_preflight_can_start(st, &r, 0), &r,
	        &pl->slot.src[0].snap.id,
	        "CJK plus surrogate pair: top name is 3 UTF16 units, not 2 "
	        "glyphs");
	pl->policy.max_path_units = r.longest_txn - 1;
	st = wf_preflight_run(
	        &ops, &pl->slot, pl->f.dst, &pl->policy, NULL, &r);
	result(2, "final_fits_transaction_rejected",
	        WF_PF_PATH_TOO_LONG == r.reason &&
	                r.longest_final <= pl->policy.max_path_units &&
	                0 == entries(pl->f.dst),
	        0, WF_PF_PATH_TOO_LONG, r.reason, &r, &pl->slot.src[0].snap.id,
	        "tight configured limit; final path fits; exact transaction "
	        "path does not; no target mutation");
}

/* Real deep NTFS directories (extended paths), the sampled API range and the manifest round trip. */
static void path_deep_directories(PathLab *pl)
{
	wchar_t component[201];
	WfPreflightReport r;
	WfStatus st;
	unsigned depth;
	size_t maximum = 0;
	DWORD error = 0;

	pl->policy = WF_PREFLIGHT_DEFAULT;
	for (depth = 0; depth < 200; depth++) {
		component[depth] = L'界';
	}
	component[200] = 0;
	wcscpy_s(pl->rel, CAP, pl->f.dr);
	for (depth = 1; depth <= 160; depth++) {
		wcscat_s(pl->rel, CAP, L"\\");
		wcscat_s(pl->rel, CAP, component);
		if (!p0_lab_path(&lab, pl->rel, pl->path, CAP)) {
			error = ERROR_FILENAME_EXCED_RANGE;
			break;
		}
		/* Extended form is mandatory; use the P0 area's validated
		 * physical path for actual creation and the logical alias for
		 * preflight. */
		if (!p0_area_xpath(lab.area, pl->rel, pl->target, CAP)) {
			error = ERROR_FILENAME_EXCED_RANGE;
			break;
		}
		if (!CreateDirectoryW(pl->target, NULL)) {
			error = GetLastError();
			break;
		}
		REQUIRE(p0_area_track(lab.area, pl->rel, 1));
		maximum = wcslen(pl->target);
		if (2 == depth || 20 == depth || 80 == depth || 160 == depth) {
			st = wf_preflight_run(&ops, &pl->slot, pl->path,
			        &pl->policy, NULL, &r);
			result(2,
			        2 == depth            ? "deep_2"
			                : 20 == depth ? "deep_20"
			                : 80 == depth ? "deep_80"
			                              : "deep_160",
			        wf_preflight_can_start(st, &r, 0) &&
			                0 == entries(pl->path),
			        0, WF_PF_OK, r.reason, &r,
			        &pl->slot.src[0].snap.id,
			        "real NTFS directories; all ancestors checked; "
			        "UTF16; success is sampled API range, not a "
			        "universal maximum");
		}
	}
	{
		char details[256];
		snprintf(details, sizeof details,
		        "largest successful physical extended directory "
		        "path=%zu UTF16 units, depth=%u; nominal policy "
		        "ceiling=32766; prefix expansion/API limits may be "
		        "smaller",
		        maximum, depth - 1);
		result(2, "sampled_api_range", maximum > 32000 && 0 == error,
		        error, 1, maximum > 32000, NULL, NULL, details);
	}
	{
		P0Area *reopened = p0_area_open(p0_area_root(lab.area));
		int ok = reopened &&
		        p0_area_xpath(reopened, pl->rel, pl->target, CAP) &&
		        INVALID_FILE_ATTRIBUTES !=
		                GetFileAttributesW(pl->target);
		result(2, "long_utf8_manifest_roundtrip", ok,
		        ok ? 0 : GetLastError(), 1, ok, NULL, NULL,
		        "reopen the durable manifest containing CJK long "
		        "paths; no reap or deletion through the reopened area");
		if (reopened) {
			p0_area_forget(reopened);
		}
	}
}

/* A 255-unit component: final and transaction lengths must both fit. */
static void path_wide_component(PathLab *pl)
{
	WfPreflightReport r;
	DWORD error = 0;

	/* At the deepest created destination, a 255-unit top-level final name
	 * exceeds a tighter policy although its fixed payload name is shorter.
	 */
	{
		wchar_t wide_name[256];
		const wchar_t *wide[] = {wide_name};
		unsigned i;
		for (i = 0; i < 255; i++) {
			wide_name[i] = L'漢';
		}
		wide_name[255] = 0;
		swprintf(pl->rel, CAP, L"%ls\\%ls", pl->f.sr, wide_name);
		REQUIRE(new_file(pl->rel, "fixture", NULL));
		{
			WfNodeInfo snapshot;
			P0SourceGuard guard;
			P0GuardResult got;
			REQUIRE(p0_lab_path(&lab, pl->rel, pl->target, CAP));
			REQUIRE(WF_OK == ops.node(NULL, pl->target, &snapshot));
			got = p0_source_guard_acquire(
			        pl->target, &snapshot, &guard, &error);
			result(0, "unicode_long_guard", P0_GUARD_OK == got,
			        error, P0_GUARD_OK, got, NULL, &snapshot.id,
			        "actual 255-unit CJK component and path beyond "
			        "MAX_PATH; extended W API guard");
			p0_source_guard_release(&guard);
		}
		wf_slot_free(&pl->slot);
		REQUIRE(wf_xfer_capture(&ops, WF_XFER_COPY, pl->f.src, wide, 1,
		                0, &pl->slot) == WF_OK);
		pl->policy.max_path_units = wcslen(pl->path) + 4 + 100;
		wf_preflight_run(
		        &ops, &pl->slot, pl->path, &pl->policy, NULL, &r);
		result(2, "long_final_shorter_payload",
		        WF_PF_PATH_TOO_LONG == r.reason &&
		                r.longest_final > r.longest_txn &&
		                0 == entries(pl->path),
		        0, WF_PF_PATH_TOO_LONG, r.reason, &r,
		        &pl->slot.src[0].snap.id,
		        "255-unit component legal on measured volume; both "
		        "final and transaction lengths must fit");
	}
	wf_slot_free(&pl->slot);
}

/* A 256-unit component is refused by the API itself. */
static void path_oversized_component(PathLab *pl)
{
	DWORD error = 0;

	{
		wchar_t oversized[257], *extended = NULL;
		HANDLE h;
		unsigned i;
		for (i = 0; i < 256; i++) {
			oversized[i] = L'漢';
		}
		oversized[256] = 0;
		swprintf(pl->rel, CAP, L"%ls\\%ls", pl->f.sr, oversized);
		REQUIRE(p0_lab_path(&lab, pl->rel, pl->target, CAP));
		REQUIRE(WF_OK == wf_path_to_extended(pl->target, &extended));
		h = CreateFileW(
		        extended, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
		error = INVALID_HANDLE_VALUE == h ? GetLastError() : 0;
		if (INVALID_HANDLE_VALUE != h) {
			CloseHandle(h);
			REQUIRE(p0_area_track(lab.area, pl->rel, 0));
		}
		result(2, "api_component_256",
		        INVALID_HANDLE_VALUE == h &&
		                (ERROR_FILENAME_EXCED_RANGE == error ||
		                        ERROR_INVALID_NAME == error),
		        error, ERROR_FILENAME_EXCED_RANGE, (int)error, NULL,
		        NULL,
		        "actual extended CreateFileW probe; volume reports max "
		        "component 255; no successful object expected");
		free(extended);
	}
}

/* Both real namespaces probed at the deepest destination. */
static void path_actual_namespaces(PathLab *pl)
{
	/* After all no-mutation refusal assertions, probe both actual
	 * namespaces at the deepest destination. These are manifested test
	 * fixtures only. */
	{
		const wchar_t *deep_rel = pl->path + wcslen(lab.root);
		const wchar_t *txn =
		        L".wfile-txn-00000000-0000-0000-0000-000000000001";
		WfTransferPaths lengths;
		WfNodeInfo node;
		wchar_t *extended = NULL;
		char details[256];
		unsigned probe;
		REQUIRE(WF_TXN_NAME_UNITS == wcslen(txn));
		REQUIRE(wf_xfer_path_lengths(
		                wcslen(pl->path) + 4, 1, 1, &lengths) == WF_OK);
		swprintf(pl->rel, CAP, L"%ls\\%ls", deep_rel, txn);
		REQUIRE(p0_area_mkdir(lab.area, pl->rel));
		for (probe = 0; probe < 2; probe++) {
			size_t expected = probe ? lengths.transaction_units
			                        : lengths.final_units;
			swprintf(pl->rel, CAP,
			        probe ? L"%ls\\%ls\\%ls" : L"%ls\\%ls",
			        deep_rel, probe ? txn : L"a",
			        WF_TXN_PAYLOAD_NAME);
			REQUIRE(new_file(pl->rel, "fixture", NULL));
			REQUIRE(p0_lab_path(&lab, pl->rel, pl->target, CAP));
			REQUIRE(WF_OK == ops.node(NULL, pl->target, &node) &&
			        WF_OK ==
			                wf_path_to_extended(
			                        pl->target, &extended));
			snprintf(details, sizeof details,
			        "actual extended CreateFileW at depth 160; "
			        "expected=%zu actual=%zu UTF16 units; fixture "
			        "created after refusal checks; not a universal "
			        "API maximum",
			        expected, wcslen(extended));
			result(2,
			        probe ? "actual_transaction_namespace"
			              : "actual_final_namespace",
			        wcslen(extended) == expected && 7 == node.size,
			        0, 1, wcslen(extended) == expected, NULL,
			        &node.id, details);
			free(extended);
			extended = NULL;
		}
	}
}

/* A 70-level source tree: the explicit stack grows past its first frames. */
static void path_deep_source_stack(PathLab *pl)
{
	WfPreflightReport r;
	WfStatus st;

	{
		const wchar_t *tree[] = {L"tree"};
		unsigned level;
		swprintf(pl->rel, CAP, L"%ls\\tree", pl->f.sr);
		REQUIRE(p0_area_mkdir(lab.area, pl->rel));
		for (level = 0; level < 70; level++) {
			wcscat_s(pl->rel, CAP, L"\\層");
			REQUIRE(p0_area_mkdir(lab.area, pl->rel));
		}
		wcscat_s(pl->rel, CAP, L"\\end");
		REQUIRE(new_file(pl->rel, "fixture", NULL));
		REQUIRE(wf_xfer_capture(&ops, WF_XFER_COPY, pl->f.src, tree, 1,
		                0, &pl->slot) == WF_OK);
		pl->policy = WF_PREFLIGHT_DEFAULT;
		st = wf_preflight_run(
		        &ops, &pl->slot, pl->f.dst, &pl->policy, NULL, &r);
		result(2, "deep_source_stack_growth",
		        wf_preflight_can_start(st, &r, 0) &&
		                71 == r.tree_dirs && 1 == r.tree_files &&
		                r.longest_txn == r.longest_final + 55,
		        0, WF_PF_OK, r.reason, &r, &pl->slot.src[0].snap.id,
		        "real 70-level source tree; explicit stack grows past "
		        "16/32/64 frames; cached relative length survives "
		        "realloc; no copy");
		wf_slot_free(&pl->slot);
	}
}

static void path_cases(void)
{
	PathLab pl;

	memset(&pl, 0, sizeof pl);
	pl.policy = WF_PREFLIGHT_DEFAULT;
	pl.rel = calloc(CAP, sizeof *pl.rel);
	pl.path = malloc(CAP * sizeof *pl.path);
	pl.target = malloc(CAP * sizeof *pl.target);
	REQUIRE(pl.rel && pl.path && pl.target);
	REQUIRE(prepare(&pl.f));
	path_unicode_and_tight_limit(&pl);
	path_deep_directories(&pl);
	path_wide_component(&pl);
	path_oversized_component(&pl);
	path_actual_namespaces(&pl);
	path_deep_source_stack(&pl);
	free(pl.target);
	free(pl.path);
	free(pl.rel);
}

static int start_evidence(const wchar_t *root, const wchar_t *run)
{
	unsigned i;
	for (i = 0; i < 3; i++) {
		wchar_t path[1024];
		const wchar_t *names[] = {L"cases.csv", L"environment.json",
		        L"run.json", L"alias.json"};
		unsigned j;
		for (j = 0; j < 4; j++) {
			swprintf(path, 1024, L"%ls\\P0-S%02u\\%ls\\%ls", root,
			        i + 8, run, names[j]);
			if (INVALID_FILE_ATTRIBUTES !=
			        GetFileAttributesW(path)) {
				return 0;
			}
		}
	}
	wcscpy_s(evidence, 1024, root);
	wcscpy_s(run_name, 128, run);
	for (i = 0; i < 3; i++) {
		wchar_t dir[1024], path[1024];
		swprintf(dir, 1024, L"%ls\\P0-S%02u\\%ls", root, i + 8, run);
		if (!p0_mkdirs(dir)) {
			return 0;
		}
		swprintf(path, 1024, L"%ls\\environment.json", dir);
		if (!p0_write_environment(
		            path, p0_area_root(lab.area), "p0_s08s10", "1.0")) {
			return 0;
		}
		{
			FILE_ID_INFO identity;
			FILE *json;
			unsigned j;
			if (!GetFileInformationByHandleEx(lab.pin, FileIdInfo,
			            &identity, sizeof identity)) {
				return 0;
			}
			swprintf(path, 1024, L"%ls\\alias.json", dir);
			json = p0_fopen_w(path);
			if (!json) {
				return 0;
			}
			fputs("{\"logical_root\":", json);
			p0_json_wstr(json, lab.root);
			fputs(",\"physical_root\":", json);
			p0_json_wstr(json, p0_area_root(lab.area));
			fputs(",\"dos_device_target\":", json);
			p0_json_wstr(json, lab.target);
			fprintf(json,
			        ",\"verified_volume\":%llu,\"verified_root_"
			        "file_id\":\"",
			        (unsigned long long)
			                identity.VolumeSerialNumber);
			for (j = 0; j < 16; j++) {
				fprintf(json, "%02x",
				        identity.FileId.Identifier[j]);
			}
			fputs("\",\"acl_changes_to_existing_directories\":"
			      "false,\"preflight_checks_disabled\":false}\n",
			        json);
			fclose(json);
		}
		swprintf(path, 1024, L"%ls\\cases.csv", dir);
		csv[i] = p0_fopen_w(path);
		if (!csv[i]) {
			return 0;
		}
		fputs("case,result,expected,actual,win32_error,source_volume,"
		      "source_id,final_units,transaction_units,policy_limit,"
		      "max_component,default_reserve_setting,active_reserve_"
		      "setting,observed_available_bytes,need_bytes,details\n",
		        csv[i]);
	}
	return 1;
}

int wmain(int argc, wchar_t **argv)
{
	DWORD before, after;
	unsigned i;
	char filesystem[32] = "unknown";
	if (argc > 1 && !wcscmp(argv[1], L"--hold")) {
		return hold_child(argc, argv);
	}
	if (2 == argc && !wcscmp(argv[1], L"--warm-child")) {
		return 0;
	}
	REQUIRE(GetModuleFileNameW(NULL, exe, 1024));
	if (!p0_lab_open(&lab, "s08s10", 1)) {
		fprintf(stderr, "BLOCKED isolated alias: error=%lu\n",
		        lab.error);
		return 77;
	}
	ops = wf_probe_win();
	{
		WfVolInfo volume;
		REQUIRE(WF_OK == ops.volume(NULL, lab.root, &volume));
		WideCharToMultiByte(CP_UTF8, 0, volume.fs_name, -1, filesystem,
		        sizeof filesystem, NULL, NULL);
	}
	printf("logical=%ls physical=%ls DOS device=%ls -> %ls\n", lab.root,
	        p0_area_root(lab.area), lab.drive, lab.target);
	if (4 == argc && !wcscmp(argv[1], L"--evidence") &&
	        !start_evidence(argv[2], argv[3])) {
		p0_lab_close(&lab, 1);
		return 2;
	}
	/* Warm up hashing/CRT before checking retained application resources.
	 */
	{
		P0Hash *hash = p0_hash_begin();
		char hex[65];
		REQUIRE(NULL != hash);
		if (hash) {
			p0_hash_end(hash, hex);
		}
	}
	/* CopyFileEx lazily initializes OS copy-provider resources. Measure our
	 * lifetimes after that initialization, rather than exempting a handle
	 * delta. */
	{
		wchar_t source[1024], dest[1024];
		WfNodeInfo snapshot;
		P0SourceGuard guard;
		DWORD error;
		PSECURITY_DESCRIPTOR sd = NULL;
		REQUIRE(new_file(L"warm-source", "fixture", NULL));
		REQUIRE(p0_lab_path(&lab, L"warm-source", source, 1024) &&
		        p0_lab_path(&lab, L"warm-copy", dest, 1024));
		REQUIRE(WF_OK == ops.node(NULL, source, &snapshot));
		REQUIRE(p0_source_guard_acquire(source, &snapshot, &guard,
		                &error) == P0_GUARD_OK);
		REQUIRE(CopyFileExW(source, dest, NULL, NULL, NULL,
		        COPY_FILE_FAIL_IF_EXISTS));
		p0_source_guard_release(&guard);
		REQUIRE(p0_area_track(lab.area, L"warm-copy", 0));
		REQUIRE(ConvertStringSecurityDescriptorToSecurityDescriptorW(
		        L"D:P(A;;FA;;;WD)", SDDL_REVISION_1, &sd, NULL));
		LocalFree(sd);
	}
	{
		wchar_t command[1200];
		STARTUPINFOW si = {0};
		PROCESS_INFORMATION pi = {0};
		si.cb = sizeof si;
		swprintf(command, 1200, L"\"%ls\" --warm-child", exe);
		REQUIRE(CreateProcessW(exe, command, NULL, NULL, FALSE,
		        CREATE_NO_WINDOW, NULL, NULL, &si, &pi));
		REQUIRE(WAIT_OBJECT_0 ==
		        WaitForSingleObject(pi.hProcess, 5000));
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
	}
	REQUIRE(GetProcessHandleCount(GetCurrentProcess(), &before));
	guard_cases();
	REQUIRE(GetProcessHandleCount(GetCurrentProcess(), &after));
	printf("handles after guard=%lu baseline=%lu\n", after, before);
	batch_cases();
	REQUIRE(GetProcessHandleCount(GetCurrentProcess(), &after));
	printf("handles after batch=%lu baseline=%lu\n", after, before);
	if (!(2 == argc && !wcscmp(argv[1], L"--guard-only"))) {
		path_cases();
	}
	REQUIRE(GetProcessHandleCount(GetCurrentProcess(), &after));
	printf("handles before=%lu after=%lu\n", before, after);
	REQUIRE(before == after);
	for (i = 0; i < 3; i++) {
		if (csv[i]) {
			fclose(csv[i]);
			csv[i] = NULL;
		}
	}
	if (0 != p0_lab_close(&lab, 0 != failures)) {
		fprintf(stderr,
		        "FAIL scratch cleanup or exact alias removal\n");
		failures++;
	}
	for (i = 0; evidence[0] && i < 3; i++) {
		const char *const hyp[] = {NULL}, *const decisions[] = {NULL};
		wchar_t path[1024];
		char test[16], run[128];
		swprintf(path, 1024, L"%ls\\P0-S%02u\\%ls\\run.json", evidence,
		        i + 8, run_name);
		snprintf(test, sizeof test, "P0-S%02u", i + 8);
		WideCharToMultiByte(
		        CP_UTF8, 0, run_name, -1, run, sizeof run, NULL, NULL);
		REQUIRE(p0_write_run_json(path, test, run, "native-poc",
		        filesystem, hyp,
		        failed[i] || failures ? "FAIL" : "PASS", decisions,
		        "Actual environment only. Independent PoC. 256MiB "
		        "reserve is configured/provisional, not empirically "
		        "endorsed. TOCTOU and PROGRESS_STOP decisions remain "
		        "open. Production copy STOP retained."));
	}
	return failures ? 1 : 0;
}
