/*
 * The folder size worker (D) against real folders: the totals are exact, a
 * link is counted and never followed, an unreadable or missing folder is told
 * rather than counted as empty, a newer request ends the older, cancelling
 * delivers nothing, no search handle stays open, and stopping returns
 * promptly. Real files in an isolated area.
 */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../tools/p0/p0_lab.h"
#include "fs/wf_sizeworker.h"
#include "wf_test.h"
#include "base/wf_macro.h"

static P0Lab lab;
static wchar_t root[1024];

static void path_in(const wchar_t *rel, wchar_t *out)
{
	swprintf(out, 1100, L"%ls\\%ls", root, rel);
}

static void make_dir(const wchar_t *rel)
{
	wchar_t p[1100];

	path_in(rel, p);
	CHECK(CreateDirectoryW(p, NULL) ||
	        ERROR_ALREADY_EXISTS == GetLastError());
}

/* A file of exactly `bytes` bytes. */
static void make_file(const wchar_t *rel, DWORD bytes)
{
	wchar_t p[1100];
	HANDLE h;
	char block[4096];

	memset(block, 'x', sizeof block);
	path_in(rel, p);
	h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	if (INVALID_HANDLE_VALUE == h) {
		return;
	}
	while (bytes) {
		DWORD n = bytes < sizeof block ? bytes : (DWORD)sizeof block;
		DWORD got;

		CHECK(WriteFile(h, block, n, &got, NULL) && got == n);
		bytes -= n;
	}
	CloseHandle(h);
}

/* The finished update of each folder, kept so that asking for folder 1 does
 * not lose the answer of folder 2 that arrived first. */
static WfSizeUpdate seen[8];
static bool seen_has[8];

static void forget_seen(void)
{
	memset(seen_has, 0, sizeof seen_has);
}

/* Waits for the update of `index` that says it is finished (or `ms`). */
static bool finished(WfSizeWorker *w, size_t index, WfSizeUpdate *out, DWORD ms)
{
	ULONGLONG until = GetTickCount64() + ms;

	for (;;) {
		WfSizeUpdate u;

		if (seen_has[index]) {
			*out = seen[index];
			return true;
		}
		while (wf_sizeworker_poll(w, &u)) {
			if (u.finished && u.index < 8) {
				seen[u.index] = u;
				seen_has[u.index] = true;
			}
		}
		if (seen_has[index]) {
			continue;
		}
		if (GetTickCount64() >= until) {
			return false;
		}
		WaitForSingleObject(wf_sizeworker_event(w), 20);
	}
}

/* Removes what the fixture made: files, folders, and a junction itself (never
 * what it points at). Only ever called on a folder inside the lab. */
static void wipe(const wchar_t *dir)
{
	WIN32_FIND_DATAW fd;
	wchar_t pat[1100], p[1100];
	HANDLE f;

	swprintf(pat, 1100, L"%ls\\*", dir);
	f = FindFirstFileW(pat, &fd);
	if (INVALID_HANDLE_VALUE == f) {
		return;
	}
	do {
		if (!wcscmp(fd.cFileName, L".") ||
		        !wcscmp(fd.cFileName, L"..")) {
			continue;
		}
		swprintf(p, 1100, L"%ls\\%ls", dir, fd.cFileName);
		SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL);
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			if (!(fd.dwFileAttributes &
			            FILE_ATTRIBUTE_REPARSE_POINT)) {
				wipe(p);
			}
			RemoveDirectoryW(p);
		} else {
			DeleteFileW(p);
		}
	} while (FindNextFileW(f, &fd));
	FindClose(f);
}

static void make_fixture(void)
{
	make_dir(L"a");
	make_dir(L"a\\b");
	make_dir(L"a\\b\\c");
	make_dir(L"empty");
	make_dir(L"flat");
	make_file(L"a\\one.bin", 1000);
	make_file(L"a\\b\\two.bin", 5000);
	make_file(L"a\\b\\c\\three.bin", 70000);
	make_file(L"flat\\f1", 10);
	make_file(L"flat\\f2", 20);
	make_file(L"flat\\f3", 0);
}

static void totals_are_exact(void)
{
	WfSizeWorker *w;
	WfSizePort port;
	const wchar_t *names[] = {L"a", L"empty", L"flat"};
	WfSizeUpdate u;

	forget_seen();
	CHECK(WF_OK == wf_sizeworker_start(&w));
	port = wf_sizeworker_port(w);
	CHECK(WF_OK == port.start(port.ctx, 1, root, names, 3));
	CHECK(finished(w, 0, &u, 10000));
	CHECK(76000 == u.bytes && 3 == u.files && 2 == u.folders);
	CHECK(!u.partial && WF_OK == u.st);
	CHECK(finished(w, 1, &u, 10000));
	CHECK(0 == u.bytes && 0 == u.files && 0 == u.folders && !u.partial);
	CHECK(finished(w, 2, &u, 10000));
	CHECK(30 == u.bytes && 3 == u.files && 0 == u.folders);
	CHECK(0 == wf_sizeworker_open_handles(w));
	wf_sizeworker_stop(w);
}

static void a_missing_folder_is_told_not_counted_as_empty(void)
{
	WfSizeWorker *w;
	WfSizePort port;
	const wchar_t *names[] = {L"no-such-folder", L"one.bin", L"b\\c"};
	wchar_t inside[1100];
	WfSizeUpdate u;

	forget_seen();
	path_in(L"a", inside);
	CHECK(WF_OK == wf_sizeworker_start(&w));
	port = wf_sizeworker_port(w);
	CHECK(WF_OK == port.start(port.ctx, 2, inside, names, 3));
	CHECK(finished(w, 0, &u, 10000));
	CHECK(WF_E_NOENT == u.st && u.partial && 0 == u.bytes);
	/* a file is not a folder */
	CHECK(finished(w, 1, &u, 10000));
	CHECK(WF_E_INVAL == u.st && u.partial && 0 == u.bytes);
	/* a name is one component; a path with a separator is not a name */
	CHECK(finished(w, 2, &u, 10000));
	CHECK(WF_OK != u.st && u.partial && 0 == u.bytes);
	wf_sizeworker_stop(w);
}

/* A junction to a big folder is not entered: it counts as a folder and adds
 * no bytes. Needs permission to make junctions; `mklink /J` does not. */
static void a_junction_is_counted_never_followed(void)
{
	WfSizeWorker *w;
	WfSizePort port;
	const wchar_t *names[] = {L"withlink"};
	wchar_t cmd[2600], link[1100], target[1100];
	WfSizeUpdate u;
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	DWORD code = 1;

	memset(&si, 0, sizeof si);
	si.cb = sizeof si;

	make_dir(L"withlink");
	make_file(L"withlink\\own.bin", 100);
	path_in(L"withlink\\to-a", link);
	path_in(L"a", target);
	swprintf(cmd, 2600, L"cmd.exe /c mklink /J \"%ls\" \"%ls\" >nul", link,
	        target);
	if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
	            NULL, NULL, &si, &pi)) {
		printf("SKIP junction: cmd.exe could not start\n");
		return;
	}
	WaitForSingleObject(pi.hProcess, 20000);
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	if (code) {
		printf("SKIP junction: mklink /J failed (%lu)\n", code);
		return;
	}
	forget_seen();
	CHECK(WF_OK == wf_sizeworker_start(&w));
	port = wf_sizeworker_port(w);
	CHECK(WF_OK == port.start(port.ctx, 3, root, names, 1));
	CHECK(finished(w, 0, &u, 10000));
	CHECK(100 == u.bytes && 1 == u.files && 1 == u.folders);
	CHECK(!u.partial);
	wf_sizeworker_stop(w);
	/* the junction itself, as a folder to count: refused, not followed */
	{
		const wchar_t *link_name[] = {L"to-a"};
		wchar_t inside[1100];

		forget_seen();
		path_in(L"withlink", inside);
		CHECK(WF_OK == wf_sizeworker_start(&w));
		port = wf_sizeworker_port(w);
		CHECK(WF_OK == port.start(port.ctx, 4, inside, link_name, 1));
		CHECK(finished(w, 0, &u, 10000));
		CHECK(WF_E_INVAL == u.st && 0 == u.bytes);
		wf_sizeworker_stop(w);
	}
}

static void a_newer_request_ends_the_older(void)
{
	WfSizeWorker *w;
	WfSizePort port;
	const wchar_t *first[] = {L"a"}, *second[] = {L"flat"};
	WfSizeUpdate u;
	ULONGLONG until;

	forget_seen();
	CHECK(WF_OK == wf_sizeworker_start(&w));
	port = wf_sizeworker_port(w);
	CHECK(WF_OK == port.start(port.ctx, 10, root, first, 1));
	CHECK(WF_OK == port.start(port.ctx, 11, root, second, 1));
	CHECK(finished(w, 0, &u, 10000));
	CHECK(11 == u.gen && 30 == u.bytes);
	/* nothing of generation 10 may arrive afterwards */
	until = GetTickCount64() + 300;
	while (GetTickCount64() < until) {
		while (wf_sizeworker_poll(w, &u)) {
			CHECK(10 != u.gen);
		}
		Sleep(20);
	}
	wf_sizeworker_stop(w);
}

static void cancel_delivers_nothing_and_closes_the_handle(void)
{
	WfSizeWorker *w;
	WfSizePort port;
	const wchar_t *names[] = {L"a"};
	WfSizeUpdate u;

	forget_seen();
	CHECK(WF_OK == wf_sizeworker_start(&w));
	port = wf_sizeworker_port(w);
	CHECK(WF_OK == port.start(port.ctx, 20, root, names, 1));
	port.cancel(port.ctx);
	Sleep(200);
	while (wf_sizeworker_poll(w, &u)) {
		CHECK(0 && "an ended request delivered");
	}
	CHECK(0 == wf_sizeworker_open_handles(w));
	/* the worker is still usable */
	CHECK(WF_OK == port.start(port.ctx, 21, root, names, 1));
	CHECK(finished(w, 0, &u, 10000) && 76000 == u.bytes);
	wf_sizeworker_stop(w);
}

/* With nothing to deliver, a poll is not a walk over every folder of the request. */
static void an_empty_poll_costs_nothing(void)
{
	enum
	{
		FOLDERS = 30000,
		POLLS = 5000
	};
	WfSizeWorker *w;
	WfSizePort port;
	const wchar_t **names = malloc(FOLDERS * sizeof *names);
	WfSizeUpdate u;
	LARGE_INTEGER t0, t1, freq;
	double ms;
	size_t finished_n = 0;
	ULONGLONG until = GetTickCount64() + 60000;

	CHECK(names != NULL);
	for (int i = 0; i < FOLDERS; i++) {
		names[i] = L"missing"; /* each is told and is done at once */
	}
	forget_seen();
	CHECK(WF_OK == wf_sizeworker_start(&w));
	port = wf_sizeworker_port(w);
	CHECK(WF_OK == port.start(port.ctx, 300, root, names, FOLDERS));
	while (finished_n < FOLDERS && GetTickCount64() < until) {
		while (wf_sizeworker_poll(w, &u)) {
			finished_n += u.finished;
		}
		Sleep(10);
	}
	CHECK(FOLDERS ==
	        finished_n); /* everything is delivered: now there is nothing */
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&t0);
	for (int i = 0; i < POLLS; i++) {
		CHECK(!wf_sizeworker_poll(w, &u));
	}
	QueryPerformanceCounter(&t1);
	ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 /
	        (double)freq.QuadPart;
	printf("info: %d empty polls of a request of %d folders: %.3f ms\n",
	        (int)POLLS, (int)FOLDERS, ms);
	CHECK(ms < 50.0);
	wf_sizeworker_stop(w);
	free(names);
}

static void bad_requests_are_refused(void)
{
	WfSizeWorker *w;
	WfSizePort port;
	const wchar_t *names[] = {L"a"};

	forget_seen();
	CHECK(WF_OK == wf_sizeworker_start(&w));
	port = wf_sizeworker_port(w);
	CHECK(WF_E_INVAL == port.start(port.ctx, 0, root, names, 1));
	CHECK(WF_E_INVAL == port.start(port.ctx, 1, root, names, 0));
	CHECK(WF_E_INVAL == port.start(port.ctx, 1, NULL, names, 1));
	CHECK(WF_E_INVAL ==
	        port.start(port.ctx, 1, root, names, WF_SIZE_MAX_FOLDERS + 1));
	wf_sizeworker_stop(w);
}

static void no_handles_leak_over_many_requests(void)
{
	WfSizeWorker *w;
	WfSizePort port;
	const wchar_t *names[] = {L"a", L"flat"};
	WfSizeUpdate u;

	forget_seen();
	CHECK(WF_OK == wf_sizeworker_start(&w));
	port = wf_sizeworker_port(w);
	for (uint32_t gen = 100; gen < 140; gen++) {
		CHECK(WF_OK == port.start(port.ctx, gen, root, names, 2));
		if (0 == gen % 4) {
			port.cancel(port.ctx);
		}
	}
	CHECK(WF_OK == port.start(port.ctx, 200, root, names, 2));
	CHECK(finished(w, 0, &u, 10000) && 200 == u.gen);
	CHECK(finished(w, 1, &u, 10000));
	CHECK(0 == wf_sizeworker_open_handles(w));
	wf_sizeworker_stop(w);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	if (!p0_lab_open(&lab, "sizeworker", 0) || !lab.area) {
		return TEST_RESULT();
	}
	wcscpy_s(root, 1024, lab.root);
	while (wcslen(root) > 3 && L'\\' == root[wcslen(root) - 1]) {
		root[wcslen(root) - 1] = 0;
	}
	make_fixture();
	totals_are_exact();
	a_missing_folder_is_told_not_counted_as_empty();
	a_junction_is_counted_never_followed();
	a_newer_request_ends_the_older();
	cancel_delivers_nothing_and_closes_the_handle();
	bad_requests_are_refused();
	an_empty_poll_costs_nothing();
	no_handles_leak_over_many_requests();
	/* only the folders this test made: the lab's own marker stays */
	{
		static const wchar_t *const made[] = {
		        L"a", L"empty", L"flat", L"withlink"};
		wchar_t p[1100];

		for (size_t i = 0; i < sizeof made / sizeof *made; i++) {
			path_in(made[i], p);
			wipe(p);
			RemoveDirectoryW(p);
		}
	}
	CHECK(0 == p0_lab_close(&lab, 0 != wf_test_failures));
	return TEST_RESULT();
}
