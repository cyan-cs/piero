/* Opt-in diagnostic. Only fresh owned fixture data is copied; no aliases,
 * ACL changes, arbitrary target paths, overwrites or automatic cleanup. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "p0/p0_lab.h"
#include "p0/p0_common.h"
#include "path/wf_path.h"
#include "xfer/wf_probe_win.h"
#include "xfer/wf_copyengine_win.h"

static WfProbeOps real_ops;
static wchar_t failed_path[1024];

static WfStatus observed_node(void *ctx, const wchar_t *path, WfNodeInfo *out)
{
	(void)ctx;
	WfStatus st = real_ops.node(real_ops.ctx, path, out);
	if (st != WF_OK && !failed_path[0] && wcslen(path) < 1024) {
		wcscpy_s(failed_path, 1024, path);
	}
	return st;
}

static void failed_open(void)
{
	wchar_t *extended = NULL;
	if (!failed_path[0] ||
	        WF_OK != wf_path_to_extended(failed_path, &extended)) {
		return;
	}
	wchar_t environment_profile[1024];
	DWORD units = GetEnvironmentVariableW(
	        L"USERPROFILE", environment_profile, 1024);
	printf("ancestor_matches_environment_profile=%d\n",
	        units && units < 1024 &&
	                CompareStringOrdinal(failed_path, -1,
	                        environment_profile, -1, TRUE) == CSTR_EQUAL);
	for (unsigned form = 0; form < 2; form++) {
		for (unsigned mask = 0; mask < 2; mask++) {
			DWORD access = mask ? FILE_READ_ATTRIBUTES : 0;
			HANDLE h = CreateFileW(form ? extended : failed_path,
			        access,
			        FILE_SHARE_READ | FILE_SHARE_WRITE |
			                FILE_SHARE_DELETE,
			        NULL, OPEN_EXISTING,
			        FILE_FLAG_BACKUP_SEMANTICS |
			                FILE_FLAG_OPEN_REPARSE_POINT,
			        NULL);
			DWORD error =
			        h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
			printf("ancestor_open extended=%u access=0x%lx share=7 "
			       "disposition=3 flags=0x02200000 error=%lu\n",
			        form, access, error);
			if (h != INVALID_HANDLE_VALUE) {
				CloseHandle(h);
			}
		}
	}
	DWORD attrs = GetFileAttributesW(failed_path);
	printf("ancestor_attributes readable=%d directory=%d reparse=%d\n",
	        attrs != INVALID_FILE_ATTRIBUTES,
	        attrs != INVALID_FILE_ATTRIBUTES &&
	                (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0,
	        attrs != INVALID_FILE_ATTRIBUTES &&
	                (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0);
	free(extended);
}

static bool create_source(P0Lab *lab, const wchar_t *relative)
{
	wchar_t path[1024];
	unsigned char data[4096];
	if (!p0_lab_path(lab, relative, path, 1024)) {
		return false;
	}
	for (size_t i = 0; i < sizeof data; i++) {
		data[i] = (unsigned char)(i * 131u);
	}
	HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		return false;
	}
	bool ok = p0_area_track(lab->area, relative, 0) != 0;
	for (unsigned i = 0; ok && i < 64; i++) {
		DWORD written = 0;
		ok = WriteFile(h, data, sizeof data, &written, NULL) &&
		        written == sizeof data;
	}
	ok = ok && FlushFileBuffers(h);
	CloseHandle(h);
	return ok;
}

static bool hashes_match(const wchar_t *a, const wchar_t *b)
{
	char left[P0_HASH_HEX], right[P0_HASH_HEX];
	return p0_sha256_file(a, left) && p0_sha256_file(b, right) &&
	        !strcmp(left, right);
}

static bool copy_case(P0Lab *lab,
        const wchar_t *name,
        const wchar_t *payload,
        const char *label)
{
	wchar_t destination[1024], source[1024], copied[1024], journal[1024];
	wchar_t source_relative[64], copied_relative[64];
	const wchar_t *names[] = {name};
	swprintf(source_relative, 64, L"source\\%ls", payload);
	swprintf(copied_relative, 64, L"piero\\%ls", payload);
	if (!p0_lab_path(lab, L"source", source, 1024) ||
	        !p0_lab_path(lab, L"piero", destination, 1024) ||
	        !p0_lab_path(lab, L"receipts", journal, 1024)) {
		return false;
	}
	WfCopySpec spec = {0};
	WfProbeOps observed = real_ops;
	observed.node = observed_node;
	failed_path[0] = 0;
	WfStatus st = wf_xfer_capture(
	        &observed, WF_XFER_COPY, source, names, 1, 0, &spec.slot);
	WfPreflightReport report = {0};
	if (st == WF_OK) {
		st = wf_preflight_run(&observed, &spec.slot, destination,
		        &WF_PREFLIGHT_DEFAULT, NULL, &report);
	}
	bool admitted = wf_preflight_can_start(st, &report, 0);
	printf("piero_%s capture_or_preflight_status=%d reason=%d "
	       "admitted=%d\n",
	        label, (int)st, (int)report.reason, admitted);
	if (!admitted) {
		failed_open();
		wf_copyspec_free(&spec);
		return false;
	}
	WfNodeInfo node;
	if (real_ops.node(NULL, destination, &node) != WF_OK) {
		wf_copyspec_free(&spec);
		return false;
	}
	spec.dest = _wcsdup(destination);
	spec.dest_id = node.id;
	spec.kind = WF_CJ_COPY;
	spec.collide = WF_COLLIDE_STOP;
	spec.verify = WF_VERIFY_SHA256;
	spec.total_files = report.tree_files;
	spec.total_bytes = report.total_bytes;
	WfCopyJournal *writer = spec.dest ? wf_cj_open(journal) : NULL;
	WfCopyResult result = {0};
	st = writer ? wf_copyengine_run(&spec, NULL, writer, &result) : WF_E_OS;
	bool completed = st == WF_OK && result.state == WF_CS_DONE &&
	        !result.failure_total && result.files_copied == 1;
	bool hash_match = false;
	if (p0_lab_path(lab, source_relative, source, 1024) &&
	        p0_lab_path(lab, copied_relative, copied, 1024)) {
		hash_match = hashes_match(source, copied);
	}
	printf("piero_%s engine_status=%d state=%d failures=%zu "
	       "completed=%d source_and_destination_hash_match=%d\n",
	        label, (int)st, (int)result.state, result.failure_total,
	        completed, hash_match);
	for (size_t i = 0; i < result.failure_total && i < WF_CJ_FAILURES;
	        i++) {
		printf("engine_failure index=%zu kind=%d error=%lu\n", i,
		        (int)result.failures[i].kind, result.failures[i].error);
	}
	wf_copyresult_free(&result);
	wf_cj_close(writer);
	wf_copyspec_free(&spec);
	return completed && hash_match;
}

int wmain(int argc, wchar_t **argv)
{
	if (argc != 2 || wcscmp(argv[1], L"--exercise")) {
		fputs("usage: probe_copy_environment --exercise\n"
		      "Creates and retains only a new owned fixture beside the "
		      "probe.\n",
		        stderr);
		return 2;
	}
	P0Lab lab = {0};
	if (!p0_lab_open(&lab, "copy-environment", 0)) {
		printf("fixture_error=%lu\n", lab.error);
		return 2;
	}
	bool setup = p0_area_mkdir(lab.area, L"source") &&
	        p0_area_mkdir(lab.area, L"source\\tree") &&
	        p0_area_mkdir(lab.area, L"standard") &&
	        p0_area_mkdir(lab.area, L"piero") &&
	        create_source(&lab, L"source\\item.bin") &&
	        create_source(&lab, L"source\\tree\\item.bin");
	wchar_t source[1024], target[1024];
	setup = setup && p0_lab_path(&lab, L"source\\item.bin", source, 1024) &&
	        p0_lab_path(&lab, L"standard\\item.bin", target, 1024);
	if (!setup) {
		p0_lab_close(&lab, 1);
		fputs("fixture_setup=FAIL\n", stderr);
		return 2;
	}
	BOOL copied = CopyFileW(source, target, TRUE);
	DWORD error = copied ? 0 : GetLastError();
	bool native_ok = copied &&
	        p0_area_track(lab.area, L"standard\\item.bin", 0) &&
	        hashes_match(source, target);
	printf("CopyFileW fail_if_exists=1 error=%lu hash_match=%d\n", error,
	        native_ok);
	BOOL collision = CopyFileW(source, target, TRUE);
	error = collision ? 0 : GetLastError();
	bool collision_ok = !collision &&
	        (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) &&
	        hashes_match(source, target);
	printf("CopyFileW collision_error=%lu "
	       "original_and_destination_kept=%d\n",
	        error, collision_ok);
	real_ops = wf_probe_win();
	WfVolInfo volume;
	WfStatus volume_status = real_ops.volume(NULL, lab.root, &volume);
	printf("fixture_volume status=%d ntfs=%d local=%d readonly=%d\n",
	        (int)volume_status,
	        volume_status == WF_OK && !wcscmp(volume.fs_name, L"NTFS"),
	        volume_status == WF_OK && volume.is_local,
	        volume_status == WF_OK && volume.read_only);
	bool file_ok = copy_case(&lab, L"item.bin", L"item.bin", "file");
	bool folder_ok = copy_case(&lab, L"tree", L"tree\\item.bin", "folder");
	wchar_t tree_source[1024];
	bool originals_kept = hashes_match(source, target) &&
	        p0_lab_path(
	                &lab, L"source\\tree\\item.bin", tree_source, 1024) &&
	        hashes_match(tree_source, target);
	printf("file_and_folder_source_hashes_kept=%d\n", originals_kept);
	p0_lab_close(&lab, 1); /* Never sweep fixtures or recorded targets. */
	puts("fixture_retained=1 terminal_visual_verification=NOT_RUN");
	return native_ok && collision_ok && file_ok && folder_ok &&
	                originals_kept
	        ? 0
	        : 1;
}
