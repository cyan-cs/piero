#include <string.h>

#include "../tools/p0/p0_lab.h"
#include "wf_test.h"
#include "xfer/wf_integrity_win.h"

static HANDLE file(P0Lab *lab,
        const wchar_t *leaf,
        const char *data,
        DWORD bytes,
        FILE_ID_INFO *id)
{
	wchar_t path[1024];
	DWORD written;
	HANDLE h;
	CHECK(p0_lab_path(lab, leaf, path, 1024));
	h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        0, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	CHECK(WriteFile(h, data, bytes, &written, NULL) && written == bytes);
	CHECK(GetFileInformationByHandleEx(h, FileIdInfo, id, sizeof *id));
	CloseHandle(h);
	CHECK(p0_area_track(lab->area, leaf, 0));
	return CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, 0, NULL);
}
static bool cancel(void *context)
{
	return *(int *)context != 0;
}
typedef struct
{
	WfIntegrityJob *job;
	HANDLE start, source, destination;
	FILE_ID_INFO a, b;
	int ok;
} Concurrent;
static DWORD WINAPI compare_thread(void *context)
{
	Concurrent *c = context;
	WfIntegrityReport report;
	if (WAIT_OBJECT_0 != WaitForSingleObject(c->start, 5000)) {
		return 1;
	}
	c->ok = 1;
	for (unsigned i = 0; i < 64; i++) {
		if (wf_integrity_compare(c->job, c->source, &c->a,
		            c->destination, &c->b, 3, NULL, NULL,
		            &report) != WF_INTEGRITY_MATCH) {
			c->ok = 0;
			break;
		}
	}
	return 0;
}
int main(void)
{
	P0Lab lab;
	WfIntegrityJob *job = NULL;
	WfIntegrityReport report;
	FILE_ID_INFO a, b, c, z;
	HANDLE ha, hb, hc, hz;
	int stop = 1;
	CHECK(p0_lab_open(&lab, "integrity", 0));
	if (!lab.area) {
		return 1;
	}
	ha = file(&lab, L"source.txt", "abc", 3, &a);
	hb = file(&lab, L"copy.txt", "abc", 3, &b);
	hc = file(&lab, L"tampered.txt", "abd", 3, &c);
	hz = file(&lab, L"empty.txt", "", 0, &z);
	CHECK(wf_integrity_begin(&job, &report));
	if (job) {
		CHECK(wf_integrity_compare(job, ha, &a, hb, &b, 3, NULL, NULL,
		              &report) == WF_INTEGRITY_MATCH);
		CHECK(3 == report.source_bytes &&
		        3 == report.destination_bytes);
		CHECK(wf_integrity_compare(job, ha, &a, hc, &c, 3, NULL, NULL,
		              &report) == WF_INTEGRITY_MISMATCH);
		CHECK(wf_integrity_compare(job, ha, &a, hb, &b, 2, NULL, NULL,
		              &report) == WF_INTEGRITY_CHANGED);
		CHECK(wf_integrity_compare(job, ha, &b, hb, &b, 3, NULL, NULL,
		              &report) == WF_INTEGRITY_CHANGED);
		{
			FILE_ID_INFO wrong = a;
			wrong.FileId.Identifier[0] ^= 1;
			CHECK(wf_integrity_compare(job, ha, &wrong, hb, &b, 3,
			              NULL, NULL,
			              &report) == WF_INTEGRITY_CHANGED);
			wrong = b;
			wrong.VolumeSerialNumber ^= 1;
			CHECK(wf_integrity_compare(job, ha, &a, hb, &wrong, 3,
			              NULL, NULL,
			              &report) == WF_INTEGRITY_CHANGED);
		}
		CHECK(wf_integrity_compare(job, ha, &a, hb, &b, 3, cancel,
		              &stop, &report) == WF_INTEGRITY_CANCELLED);
		CHECK(wf_integrity_compare(job, hz, &z, hb, &b, 0, NULL, NULL,
		              &report) == WF_INTEGRITY_CHANGED);
		{
			Concurrent contexts[2] = {{0}};
			HANDLE threads[2],
			        start = CreateEventW(NULL, TRUE, FALSE, NULL);
			CHECK(NULL != start);
			for (unsigned i = 0; i < 2; i++) {
				contexts[i].a = a;
				contexts[i].b = b;
				contexts[i].start = start;
				contexts[i].source = ReOpenFile(
				        ha, GENERIC_READ, FILE_SHARE_READ, 0);
				contexts[i].destination = ReOpenFile(
				        hb, GENERIC_READ, FILE_SHARE_READ, 0);
				CHECK(wf_integrity_begin(
				        &contexts[i].job, &report));
				threads[i] = CreateThread(NULL, 0,
				        compare_thread, &contexts[i], 0, NULL);
				CHECK(NULL != threads[i]);
			}
			CHECK(SetEvent(start));
			if (WAIT_OBJECT_0 !=
			        WaitForMultipleObjects(
			                2, threads, TRUE, 10000)) {
				return 1;
			}
			for (unsigned i = 0; i < 2; i++) {
				CHECK(contexts[i].ok);
				CHECK(wf_integrity_end(&contexts[i].job));
				CloseHandle(contexts[i].source);
				CloseHandle(contexts[i].destination);
				CloseHandle(threads[i]);
			}
			CloseHandle(start);
		}
		{
			wchar_t original[1024], alias[1024];
			CHECK(p0_lab_path(&lab, L"copy.txt", original, 1024));
			CHECK(p0_lab_path(
			        &lab, L"copy-alias.txt", alias, 1024));
			CloseHandle(hb);
			CHECK(CreateHardLinkW(alias, original, NULL));
			CHECK(p0_area_track(lab.area, L"copy-alias.txt", 0));
			hb = CreateFileW(original, GENERIC_READ,
			        FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
			CHECK(wf_integrity_compare(job, ha, &a, hb, &b, 3, NULL,
			              NULL, &report) == WF_INTEGRITY_CHANGED);
		}
	}
	wf_integrity_end(&job);
	CHECK(NULL == job);
	CloseHandle(ha);
	CloseHandle(hb);
	CloseHandle(hc);
	CloseHandle(hz);
	CHECK(0 == p0_lab_close(&lab, 0));
	return TEST_RESULT();
}
