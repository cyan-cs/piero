#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "p0/p0_lab.h"
#include "edit/wf_efile_win.h"
#include "ui/wf_app_priv.h"
#include "xfer/wf_copyengine_win.h"
#include "xfer/wf_probe_win.h"

static double milliseconds(void)
{
	LARGE_INTEGER t, f;
	QueryPerformanceCounter(&t);
	QueryPerformanceFrequency(&f);
	return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

static double cpu(void)
{
	FILETIME a, b, k, u;
	ULARGE_INTEGER x, y;
	GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u);
	x.LowPart = k.dwLowDateTime;
	x.HighPart = k.dwHighDateTime;
	y.LowPart = u.dwLowDateTime;
	y.HighPart = u.dwHighDateTime;
	return (double)(x.QuadPart + y.QuadPart) / 10000.0;
}

int main(void)
{
	P0Lab lab = {0};
	wchar_t src[2048], dst[2048], file[2048], made[2048], ads[2048],
	        journal[2048], backup[2048];
	const wchar_t *names[] = {L"sample.txt"};
	char *data = malloc(8u << 20);
	WfProbeOps ops = wf_probe_win();
	PROCESS_MEMORY_COUNTERS memory = {0};
	int result = 1;
	if (!data || !p0_lab_open(&lab, "review-bench", 0)) {
		goto done;
	}
	memset(data, 'x', 8u << 20);
	if (!p0_area_mkdir(lab.area, L"s") || !p0_area_mkdir(lab.area, L"d") ||
	        !p0_lab_path(&lab, L"s", src, 2048) ||
	        !p0_lab_path(&lab, L"d", dst, 2048) ||
	        !p0_lab_path(&lab, L"s\\sample.txt", file, 2048) ||
	        !p0_lab_path(&lab, L"d\\sample.txt", made, 2048) ||
	        !p0_lab_path(&lab, L"journal", journal, 2048)) {
		goto done;
	}
	for (int stream = 0; stream < 2; stream++) {
		HANDLE h;
		DWORD put, count = stream ? 65536 : 8u << 20;
		swprintf(ads, 2048, L"%ls%ls", file, stream ? L":review" : L"");
		h = CreateFileW(
		        ads, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
		if (h == INVALID_HANDLE_VALUE) {
			goto done;
		}
		BOOL ok = WriteFile(h, data, count, &put, NULL);
		CloseHandle(h);
		if (!ok || put != count) {
			goto done;
		}
	}
	for (int i = 0; i < 6; i++) {
		WfCopySpec spec = {0};
		WfCopyResult r = {0};
		WfNodeInfo node;
		WfCopyJournal *j = wf_cj_open(journal);
		wf_slot_init(&spec.slot);
		if (!j ||
		        wf_xfer_capture(&ops, WF_XFER_COPY, src, names, 1, 0,
		                &spec.slot) != WF_OK ||
		        ops.node(NULL, dst, &node) != WF_OK) {
			if (j) {
				wf_cj_close(j);
			}
			wf_copyspec_free(&spec);
			goto done;
		}
		spec.dest = _wcsdup(dst);
		spec.dest_id = node.id;
		spec.kind = WF_CJ_COPY;
		spec.verify = WF_VERIFY_SHA256;
		spec.collide = WF_COLLIDE_STOP;
		double wall = milliseconds(), used = cpu();
		WfStatus st = wf_copyengine_run(&spec, NULL, j, &r);
		printf("copy_8MiB_ads64KiB,%d,%.3f,%.3f\n", i,
		        milliseconds() - wall, cpu() - used);
		bool ok =
		        st == WF_OK && r.files_copied == 1 && !r.failure_total;
		wf_copyresult_free(&r);
		wf_copyspec_free(&spec);
		wf_cj_close(j);
		if (!ok || !DeleteFileW(made)) {
			goto done;
		}
	}
	wf_efile_set_journal(journal);
	if (!p0_area_xpath(lab.area, L"s\\sample.txt~", backup, 2048)) {
		goto done;
	}
	for (int i = 0; i < 6; i++) {
		WfLoaded loaded = {0};
		WfSaveResult saved;
		if (wf_efile_load(file, 16u << 20, 0, NULL, &loaded) != WF_OK ||
		        loaded.verdict != WF_FL_OK) {
			wf_efile_loaded_free(&loaded);
			goto done;
		}
		double wall = milliseconds(), used = cpu();
		WfStatus st = wf_efile_save_over(file, &loaded.stamp,
		        loaded.utf8, loaded.utf8_n, loaded.enc, NULL, &saved);
		printf("save_8MiB_ads64KiB,%d,%.3f,%.3f\n", i,
		        milliseconds() - wall, cpu() - used);
		wf_efile_loaded_free(&loaded);
		if (st != WF_OK || saved.verdict != WF_SV_OK) {
			goto done;
		}
		/* Each sample owns a disposable original. Retention cleanup is outside
		 * the measured save; no application setting disables retention. */
		if (saved.backup_made &&
		        (wcscmp(saved.kept_original, backup) ||
		                !DeleteFileW(backup))) {
			goto done;
		}
	}
	{
		WfApp *app = calloc(1, sizeof *app);
		HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
		unsigned loops = 0;
		if (!app || !event) {
			free(app);
			if (event) {
				CloseHandle(event);
			}
			goto done;
		}
		app->mode = &WF_MODE_SETTINGS;
		app->image.due = 1;
		app->now_ms = 2;
		double wall = milliseconds(), used = cpu();
		while (milliseconds() - wall < 250) {
			DWORD wait = wf_app_image_timeout(app);
			WaitForSingleObject(event, wait > 250 ? 250 : wait);
			loops++;
		}
		printf("modal_idle_250ms,%u,%.3f,%.3f\n", loops,
		        milliseconds() - wall, cpu() - used);
		CloseHandle(event);
		free(app);
	}
	memory.cb = sizeof memory;
	GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof memory);
	printf("peak_working_set_bytes,%zu\n",
	        (size_t)memory.PeakWorkingSetSize);
	if (!DeleteFileW(file) || !DeleteFileW(journal) ||
	        !RemoveDirectoryW(src) || !RemoveDirectoryW(dst)) {
		goto done;
	}
	result = 0;
done:
	free(data);
	if (lab.area && p0_lab_close(&lab, result != 0)) {
		result = 1;
	}
	return result;
}
