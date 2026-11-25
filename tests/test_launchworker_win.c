/*
 * Open against the real shell (Windows only). A private, uniquely named file
 * type is registered under HKCU\Software\Classes for the duration of the run
 * and points at open_target.exe, which records where it was started and what
 * it was given. Nothing outside the scratch area and that one registry key
 * is touched, and the key is removed at the end.
 */
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "../tools/p0/p0_lab.h"
#include "fs/wf_launchworker.h"
#include "wf_test.h"
#include "wf_test_assoc.h"
#include "xfer/wf_probe_win.h"

static WfLaunchWorker *g_worker;
static uint32_t g_gen;
static WfTestAssoc association;
#define g_ext association.ext

static LSTATUS register_type(void)
{
	wchar_t exe[1024], *slash;
	DWORD n = GetModuleFileNameW(NULL, exe, 1024);
	if (!n || n >= 1024 || !(slash = wcsrchr(exe, L'\\'))) {
		return ERROR_BAD_PATHNAME;
	}
	wcscpy_s(slash + 1, (size_t)(exe + 1024 - slash - 1),
	        L"open_target.exe");
	if (ERROR_SUCCESS != wf_test_assoc_name(&association)) {
		return ERROR_GEN_FAILURE;
	}
	return wf_test_assoc_register(&association, exe);
}

static void unregister_type(void)
{
	CHECK(ERROR_SUCCESS == wf_test_assoc_cleanup(&association));
}

typedef struct
{
	P0Lab lab;
	WfProbeOps ops;
	wchar_t dir[1024], neutral[1024];
	WfNodeInfo dir_node;
} Env;

static bool exists(const wchar_t *path)
{
	return INVALID_FILE_ATTRIBUTES != GetFileAttributesW(path);
}

/* Creates a file of the registered type and returns its identity. */
static bool make_file(Env *v, const wchar_t *stem, wchar_t *path, WfNodeInfo *n)
{
	wchar_t rel[200];
	HANDLE h;

	swprintf(rel, 200, L"w\\%ls%ls", stem, g_ext);
	CHECK(p0_lab_path(&v->lab, rel, path, 1100));
	h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	CloseHandle(h);
	CHECK(p0_area_track(v->lab.area, rel, 0));
	CHECK(WF_OK == v->ops.node(NULL, path, n) &&
	        (n->id.kind == WF_ID_STRONG));
	return true;
}

static WfLaunchResult launch(const wchar_t *path,
        const WfFileId *id,
        const wchar_t *directory,
        WfStatus expect_submit)
{
	WfLaunchResult r;
	WfLaunchRequest req = {path, *id, directory, false};
	WfLaunchPort port = wf_launchworker_port(g_worker);
	WfStatus st = port.submit(port.ctx, ++g_gen, &req);

	memset(&r, 0, sizeof r);
	CHECK(st == expect_submit);
	if (WF_OK != st) {
		return r;
	}
	CHECK(WAIT_OBJECT_0 ==
	        WaitForSingleObject(wf_launchworker_event(g_worker), 20000));
	CHECK(wf_launchworker_poll(g_worker, &r));
	CHECK(r.gen == g_gen);
	CHECK(!wf_launchworker_poll(g_worker, &r)); /* exactly one answer */
	return r;
}

/* Waits for the started program to leave its note, then reads it. */
static bool read_seen(Env *v, const wchar_t *path, wchar_t *out, size_t cap)
{
	wchar_t seen[1200], rel[300];
	DWORD i, got = 0;
	HANDLE h = INVALID_HANDLE_VALUE;
	const wchar_t *leaf = wcsrchr(path, L'\\') + 1;

	swprintf(seen, 1200, L"%ls.seen", path);
	for (i = 0; i < 200 && INVALID_HANDLE_VALUE == h; i++) {
		Sleep(50);
		if (!exists(seen)) {
			continue;
		}
		h = CreateFileW(seen, GENERIC_READ,
		        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
		        FILE_ATTRIBUTE_NORMAL, NULL);
	}
	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	for (i = 0; i < 100; i++) { /* the writer may still be at it */
		got = 0;
		if (ReadFile(h, out, (DWORD)((cap - 1) * sizeof *out), &got,
		            NULL) &&
		        got > 0 && L'\n' == out[got / sizeof *out - 1]) {
			break;
		}
		SetFilePointer(h, 0, NULL, FILE_BEGIN);
		Sleep(50);
	}
	CloseHandle(h);
	out[got / sizeof *out] = 0;
	swprintf(rel, 300, L"w\\%ls.seen", leaf);
	CHECK(p0_area_track(v->lab.area, rel, 0));
	return got > 0;
}

static bool seen_starts_with(const wchar_t *text, const wchar_t *dir)
{
	size_t n = wcslen(dir);

	return !wcsncmp(text, dir, n) && L'\n' == text[n];
}

static void opens_the_file_from_the_neutral_directory(Env *v)
{
	wchar_t path[1100], text[2200], cwd_before[1024], cwd_after[1024];
	WfNodeInfo n;
	WfLaunchResult r;

	CHECK(make_file(v, L"plain", path, &n));
	GetCurrentDirectoryW(1024, cwd_before);
	r = launch(path, &n.id, v->neutral, WF_OK);
	GetCurrentDirectoryW(1024, cwd_after);
	CHECK(WF_OK == r.outcome.st);
	CHECK(!wcsncmp(r.name, L"plain", 5));
	CHECK(read_seen(v, path, text, 2200));
	CHECK(seen_starts_with(text, v->neutral));
	CHECK(NULL != wcsstr(text, path)); /* the absolute path was passed */
	CHECK(!wcscmp(
	        cwd_before, cwd_after)); /* wfile's own CWD is untouched */
}

static void file_dir_starts_it_in_the_folder_of_the_file(Env *v)
{
	wchar_t path[1100], text[2200];
	WfNodeInfo n;
	WfLaunchResult r;

	CHECK(make_file(v, L"infolder", path, &n));
	r = launch(path, &n.id, v->dir, WF_OK);
	CHECK(WF_OK == r.outcome.st);
	CHECK(read_seen(v, path, text, 2200));
	CHECK(seen_starts_with(text, v->dir));
}

static void unicode_names_reach_the_program_intact(Env *v)
{
	static const wchar_t stem[] = {0x65E5, 0x672C, 0xD842, 0xDFB7, 0};
	wchar_t path[1100], text[2200];
	WfNodeInfo n;
	WfLaunchResult r;

	CHECK(make_file(v, stem, path, &n));
	r = launch(path, &n.id, v->neutral, WF_OK);
	CHECK(WF_OK == r.outcome.st);
	CHECK(read_seen(v, path, text, 2200));
	CHECK(NULL != wcsstr(text, path));
}

/* Nothing is started for an object that is not the one that was listed. */
static void refuses_what_is_not_the_listed_object(Env *v)
{
	wchar_t path[1100], other[1100], seen[1200];
	WfNodeInfo n, o;
	WfLaunchResult r;
	WfFileId none;

	CHECK(make_file(v, L"stale", path, &n));
	CHECK(make_file(v, L"another", other, &o));
	swprintf(seen, 1200, L"%ls.seen", path);

	r = launch(
	        path, &o.id, v->neutral, WF_OK); /* someone else's identity */
	CHECK(WF_E_INVAL == r.outcome.st);
	memset(&none, 0, sizeof none);
	r = launch(path, &none, v->neutral, WF_E_INVAL); /* unproven: refused */
	(void)r;
	r = launch(v->dir, &v->dir_node.id, v->neutral, WF_OK); /* a folder */
	CHECK(WF_OK != r.outcome.st);
	swprintf(seen, 1200, L"%ls\\nothing%ls", v->dir, g_ext);
	n.id.id[0] ^= 0xFF; /* a file that was replaced since the listing */
	r = launch(other, &n.id, v->neutral, WF_OK);
	CHECK(WF_E_INVAL == r.outcome.st);
	swprintf(seen, 1200, L"%ls\\ghost%ls", v->dir, g_ext);
	r = launch(seen, &o.id, v->neutral, WF_OK); /* gone */
	CHECK(WF_E_NOENT == r.outcome.st);

	Sleep(500); /* a wrongly started program would have written by now */
	swprintf(seen, 1200, L"%ls.seen", path);
	CHECK(!exists(seen));
	swprintf(seen, 1200, L"%ls.seen", other);
	CHECK(!exists(seen));
}

/* One request at a time: a second one is refused until the answer is read. */
static void a_second_request_waits_for_the_answer(Env *v)
{
	wchar_t path[1100];
	WfNodeInfo n;
	WfLaunchRequest req = {0};
	WfLaunchPort port = wf_launchworker_port(g_worker);
	WfLaunchResult r;

	CHECK(make_file(v, L"busy", path, &n));
	req.path = path;
	req.id = n.id;
	req.directory = v->neutral;
	CHECK(WF_OK == port.submit(port.ctx, ++g_gen, &req));
	CHECK(WF_E_INVAL == port.submit(port.ctx, ++g_gen, &req));
	CHECK(WAIT_OBJECT_0 ==
	        WaitForSingleObject(wf_launchworker_event(g_worker), 20000));
	CHECK(WF_E_INVAL == port.submit(port.ctx, ++g_gen, &req)); /* unread */
	CHECK(wf_launchworker_poll(g_worker, &r) && WF_OK == r.outcome.st);
	{
		wchar_t text[2200];

		CHECK(read_seen(v, path, text, 2200));
	}
	g_gen--; /* the refused submits did not consume a generation */
}

int main(void)
{
	Env v;

	LSTATUS prerequisite;
	setvbuf(stdout, NULL, _IONBF, 0);
	prerequisite = register_type();
	if (ERROR_SUCCESS != prerequisite) {
		printf("BLOCKED: isolated HKCU association unavailable: Win32 "
		       "%ld, cleanup %ld\n",
		        prerequisite, association.cleanup_error);
		return ERROR_ACCESS_DENIED == prerequisite &&
		                !association.cleanup_error
		        ? 77
		        : 1;
	}
	memset(&v, 0, sizeof v);
	v.ops = wf_probe_win();
	CHECK(p0_lab_open(&v.lab, "launchworker", 0));
	if (!v.lab.area) {
		unregister_type();
		return TEST_RESULT();
	}
	CHECK(p0_area_mkdir(v.lab.area, L"w"));
	CHECK(p0_area_mkdir(v.lab.area, L"neutral"));
	CHECK(p0_lab_path(&v.lab, L"w", v.dir, 1024));
	CHECK(p0_lab_path(&v.lab, L"neutral", v.neutral, 1024));
	CHECK(WF_OK == v.ops.node(NULL, v.dir, &v.dir_node));
	CHECK(WF_OK == wf_launchworker_start(&g_worker));

	opens_the_file_from_the_neutral_directory(&v);
	file_dir_starts_it_in_the_folder_of_the_file(&v);
	unicode_names_reach_the_program_intact(&v);
	refuses_what_is_not_the_listed_object(&v);
	a_second_request_waits_for_the_answer(&v);

	unregister_type();
	{
		WfWorkerStopReport stop = wf_launchworker_stop(g_worker);

		CHECK(WF_OK == stop.status);
	}
	CHECK(0 == p0_lab_close(&v.lab, 0 != wf_test_failures));
	return TEST_RESULT();
}
