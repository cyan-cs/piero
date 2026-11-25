/*
 * Smart Go's candidate worker against a real folder: five at a time, from
 * where the last batch stopped; case-insensitive Unicode prefixes; hidden
 * entries; stale searches never delivered; no handle left open; the end of a
 * folder is seen. Real files in an isolated area.
 */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../tools/p0/p0_lab.h"
#include "fs/wf_candworker.h"
#include "wf_test.h"
#include "base/wf_macro.h"

static P0Lab lab;
static wchar_t root[1024];

static double now_ms(void)
{
	LARGE_INTEGER f, c;

	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c);
	return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static void path_in(const wchar_t *rel, wchar_t *out)
{
	swprintf(out, 1100, L"%ls\\%ls", root, rel);
}

static void make_file(const wchar_t *rel, DWORD attributes)
{
	wchar_t p[1100];
	HANDLE h;

	path_in(rel, p);
	h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	        FILE_ATTRIBUTE_NORMAL | attributes, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	if (INVALID_HANDLE_VALUE != h) {
		CloseHandle(h);
	}
}

static void make_dir(const wchar_t *rel)
{
	wchar_t p[1100];

	path_in(rel, p);
	CHECK(CreateDirectoryW(p, NULL) ||
	        ERROR_ALREADY_EXISTS == GetLastError());
}

/* The next batch of the live search, or false after `ms`. */
static bool next_batch(WfCandWorker *w, WfCandBatch *b, DWORD ms)
{
	ULONGLONG until = GetTickCount64() + ms;

	for (;;) {
		if (wf_candworker_poll(w, b)) {
			return true;
		}
		if (GetTickCount64() >= until) {
			return false;
		}
		WaitForSingleObject(wf_candworker_event(w), 20);
	}
}

/* Reads a whole search: first batch, then more until the end. */
typedef struct
{
	wchar_t names[40][WF_CAND_NAME];
	size_t n, batches;
	bool ended, ok;
} Collected;

static Collected read_all(WfCandWorker *w,
        WfCandPort port,
        uint32_t gen,
        const wchar_t *dir,
        const wchar_t *prefix,
        bool hidden,
        bool dirs_only)
{
	Collected c;
	WfCandBatch b;

	memset(&c, 0, sizeof c);
	CHECK(WF_OK ==
	        port.start(port.ctx, gen, dir, prefix, hidden, dirs_only));
	for (;;) {
		if (!next_batch(w, &b, 10000)) {
			return c;
		}
		CHECK(b.gen == gen);
		CHECK(b.n <= WF_CAND_BATCH);
		c.batches++;
		for (size_t i = 0; i < b.n && c.n < 40; i++) {
			wcscpy_s(c.names[c.n++], WF_CAND_NAME, b.items[i].name);
		}
		if (WF_OK != b.st) {
			return c;
		}
		if (!b.more) {
			c.ended = c.ok = true;
			return c;
		}
		CHECK(WF_OK == port.more(port.ctx, gen));
	}
}

static bool collected_has(const Collected *c, const wchar_t *name)
{
	for (size_t i = 0; i < c->n; i++) {
		if (!wcscmp(c->names[i], name)) {
			return true;
		}
	}
	return false;
}

static bool all_distinct(const Collected *c)
{
	for (size_t i = 0; i < c->n; i++) {
		for (size_t j = i + 1; j < c->n; j++) {
			if (!wcscmp(c->names[i], c->names[j])) {
				return false;
			}
		}
	}
	return true;
}

static void make_fixture(void)
{
	static const wchar_t *const dirs[] = {L"d\\Documents", L"d\\Downloads",
	        L"d\\Desktop", L"d\\dotfiles", L"d\\Docker",
	        L"d\\Documents Backup",
	        L"d\\\x6F22\x5B57\x30D5\x30A9\x30EB\x30C0", L"d\\Music"};
	static const wchar_t *const files[] = {L"d\\docs.txt", L"d\\Zeta.txt",
	        L"d\\\xE4pfel.txt", L"d\\LongFileNameForShortTest.txt"};

	make_dir(L"d");
	for (size_t i = 0; i < WF_ARRAY_LEN(dirs); i++) {
		make_dir(dirs[i]);
	}
	for (size_t i = 0; i < WF_ARRAY_LEN(files); i++) {
		make_file(files[i], 0);
	}
	make_file(L"d\\secret.txt", FILE_ATTRIBUTE_HIDDEN);
	make_dir(L"big");
	for (int i = 0; i < 5000; i++) {
		wchar_t rel[64];

		swprintf(rel, 64, L"big\\item%05d.dat", i);
		make_file(rel, 0);
	}
	make_file(L"big\\needle.txt", 0);
	make_dir(L"empty");
}

static void the_first_batch_then_the_next(void)
{
	WfCandWorker *w;
	WfCandPort port;
	WfCandBatch b, b2;
	wchar_t dir[1100];

	CHECK(WF_OK == wf_candworker_start(&w));
	port = wf_candworker_port(w);
	path_in(L"big", dir);
	CHECK(WF_OK == port.start(port.ctx, 1, dir, L"", false, false));
	CHECK(next_batch(w, &b, 5000));
	CHECK(1 == b.gen && b.first && WF_OK == b.st && WF_CAND_BATCH == b.n &&
	        b.more);
	for (size_t i = 0; i < b.n; i++) {
		CHECK(wcscmp(b.items[i].name, L".") &&
		        wcscmp(b.items[i].name, L".."));
	}
	/* nothing arrives by itself: no read-ahead */
	Sleep(150);
	CHECK(!wf_candworker_poll(w, &b2));
	CHECK(1 == wf_candworker_open_handles(w)); /* the search stays open */
	/* the next batch continues where this one stopped */
	CHECK(WF_OK == port.more(port.ctx, 1));
	CHECK(next_batch(w, &b2, 5000));
	CHECK(1 == b2.gen && !b2.first && WF_CAND_BATCH == b2.n && b2.more);
	for (size_t i = 0; i < b.n; i++) {
		for (size_t j = 0; j < b2.n; j++) {
			CHECK(0 != wcscmp(b.items[i].name, b2.items[j].name));
		}
	}
	port.cancel(port.ctx);
	/* a small folder comes whole, hidden entries left out */
	path_in(L"d", dir);
	Collected c = read_all(w, port, 2, dir, L"", false, false);
	CHECK(c.ok && c.ended && 12 == c.n);
	CHECK(all_distinct(&c));
	CHECK(collected_has(&c, L"Documents") && collected_has(&c, L"Music") &&
	        collected_has(&c, L"docs.txt") &&
	        collected_has(&c, L"\x6F22\x5B57\x30D5\x30A9\x30EB\x30C0"));
	CHECK(!collected_has(&c, L"secret.txt"));
	CHECK(0 == wf_candworker_open_handles(w)); /* closed at the end */
	CHECK(WF_OK == wf_candworker_stop(w).status);
}

/* Asked for folders alone, the worker sends folders alone: no file is made into a
 * batch to be thrown away, and the end of a folder with none says so at once. */
static void only_folders_when_asked(void)
{
	WfCandWorker *w;
	WfCandPort port;
	wchar_t dir[1100];
	Collected c;

	CHECK(WF_OK == wf_candworker_start(&w));
	port = wf_candworker_port(w);
	path_in(L"d", dir);
	c = read_all(w, port, 1, dir, L"", false, true);
	CHECK(c.ok && c.ended &&
	        8 == c.n); /* its eight folders, none of its four files */
	CHECK(collected_has(&c, L"Documents") && collected_has(&c, L"Music") &&
	        collected_has(&c, L"\x6F22\x5B57\x30D5\x30A9\x30EB\x30C0"));
	CHECK(!collected_has(&c, L"docs.txt"));
	/* with a prefix: the folders that start with it, and not the files that do */
	c = read_all(w, port, 2, dir, L"do", false, true);
	CHECK(c.ok && c.ended && 5 == c.n && collected_has(&c, L"Documents") &&
	        !collected_has(&c, L"docs.txt"));
	/* a folder of files only: nothing, and it is the end */
	path_in(L"big", dir);
	c = read_all(w, port, 3, dir, L"", false, true);
	CHECK(c.ok && c.ended && 0 == c.n);
	CHECK(1 == c.batches); /* one answer: the end */
	{
		Collected all = read_all(w, port, 4, dir, L"", false, false);

		/* what the screen would have had to ask for, a batch of five at a time,
		 * only to throw the files away */
		printf("info: 5001 files, no folder: %zu batches of "
		       "everything, %zu when only folders are asked\n",
		        (size_t)all.batches, (size_t)c.batches);
		CHECK(all.batches > 100);
	}
	CHECK(0 == wf_candworker_open_handles(w));
	CHECK(WF_OK == wf_candworker_stop(w).status);
}

static void prefixes_match_ordinal_ignoring_case_and_not_short_names(void)
{
	WfCandWorker *w;
	WfCandPort port;
	wchar_t dir[1100];
	Collected c;

	CHECK(WF_OK == wf_candworker_start(&w));
	port = wf_candworker_port(w);
	path_in(L"d", dir);

	c = read_all(w, port, 1, dir, L"do", false, false);
	CHECK(c.ok && 6 == c.n && 1 == c.batches && all_distinct(&c));
	CHECK(collected_has(&c, L"Documents") &&
	        collected_has(&c, L"Downloads") &&
	        collected_has(&c, L"docs.txt") &&
	        collected_has(&c, L"dotfiles") &&
	        collected_has(&c, L"Docker") &&
	        collected_has(&c, L"Documents Backup"));
	c = read_all(w, port, 2, dir, L"DOC", false, false);
	CHECK(c.ok &&
	        4 == c.n); /* Documents, docs.txt, Docker, Documents Backup */
	c = read_all(w, port, 3, dir, L"Documents ", false, false);
	CHECK(c.ok && 1 == c.n && collected_has(&c, L"Documents Backup"));
	c = read_all(w, port, 4, dir, L"nothing", false, false);
	CHECK(c.ok && 0 == c.n && 1 == c.batches);
	/* Unicode: a CJK prefix, and a letter whose case differs */
	c = read_all(w, port, 5, dir, L"\x6F22", false, false);
	CHECK(c.ok && 1 == c.n);
	c = read_all(w, port, 6, dir, L"\xC4", false,
	        false); /* A-umlaut finds a-umlaut */
	CHECK(c.ok && 1 == c.n && collected_has(&c, L"\xE4pfel.txt"));
	/* the 8.3 name LONGFI~1.TXT is not a match for what the long name says */
	c = read_all(w, port, 7, dir, L"LONGFI~", false, false);
	CHECK(c.ok && 0 == c.n);
	c = read_all(w, port, 8, dir, L"LongFileName", false, false);
	CHECK(c.ok && 1 == c.n);
	/* a prefix longer than any name */
	c = read_all(
	        w, port, 9, dir, L"Documents Backup and more", false, false);
	CHECK(c.ok && 0 == c.n);
	CHECK(0 == wf_candworker_open_handles(w));
	CHECK(WF_OK == wf_candworker_stop(w).status);
}

static void hidden_entries_only_when_asked(void)
{
	WfCandWorker *w;
	WfCandPort port;
	wchar_t dir[1100];
	Collected c;

	CHECK(WF_OK == wf_candworker_start(&w));
	port = wf_candworker_port(w);
	path_in(L"d", dir);
	c = read_all(w, port, 1, dir, L"sec", false, false);
	CHECK(c.ok && 0 == c.n);
	c = read_all(w, port, 2, dir, L"sec", true, false);
	CHECK(c.ok && 1 == c.n && collected_has(&c, L"secret.txt"));
	CHECK(WF_OK == wf_candworker_stop(w).status);
}

static void flags_tell_folders_from_files(void)
{
	WfCandWorker *w;
	WfCandPort port;
	WfCandBatch b;
	wchar_t dir[1100];

	CHECK(WF_OK == wf_candworker_start(&w));
	port = wf_candworker_port(w);
	path_in(L"d", dir);
	CHECK(WF_OK == port.start(port.ctx, 1, dir, L"Zeta", false, false));
	CHECK(next_batch(w, &b, 5000) && 1 == b.n &&
	        !(b.items[0].flags & 1u)); /* a file */
	CHECK(WF_OK == port.start(port.ctx, 2, dir, L"Music", false, false));
	CHECK(next_batch(w, &b, 5000) && 1 == b.n && (b.items[0].flags & 1u));
	CHECK(WF_OK == port.start(port.ctx, 3, dir, L"sec", true, false));
	CHECK(next_batch(w, &b, 5000) && 1 == b.n && (b.items[0].flags & 2u));
	CHECK(WF_OK == wf_candworker_stop(w).status);
}

static void errors_are_told_not_hidden(void)
{
	WfCandWorker *w;
	WfCandPort port;
	WfCandBatch b;
	wchar_t p[1100];

	CHECK(WF_OK == wf_candworker_start(&w));
	port = wf_candworker_port(w);
	path_in(L"does\\not\\exist", p);
	CHECK(WF_OK == port.start(port.ctx, 1, p, L"", false, false));
	CHECK(next_batch(w, &b, 5000));
	CHECK(1 == b.gen && WF_E_NOENT == b.st && 0 == b.n && !b.more &&
	        0 != b.error);
	path_in(L"d\\docs.txt", p); /* a file is not a folder */
	CHECK(WF_OK == port.start(port.ctx, 2, p, L"", false, false));
	CHECK(next_batch(w, &b, 5000));
	CHECK(WF_OK != b.st && 0 == b.n);
	path_in(L"empty", p); /* an empty folder is not an error */
	CHECK(WF_OK == port.start(port.ctx, 3, p, L"", false, false));
	CHECK(next_batch(w, &b, 5000));
	CHECK(WF_OK == b.st && 0 == b.n && !b.more);
	CHECK(0 == wf_candworker_open_handles(w));
	/* bad arguments */
	CHECK(WF_E_INVAL == port.start(port.ctx, 0, p, L"", false, false));
	CHECK(WF_E_INVAL == port.start(port.ctx, 4, NULL, L"", false, false));
	CHECK(WF_OK == wf_candworker_stop(w).status);
}

static void a_newer_search_ends_the_older_one(void)
{
	WfCandWorker *w;
	WfCandPort port;
	WfCandBatch b;
	wchar_t big[1100], small[1100];
	int stale = 0, newest = 0;

	CHECK(WF_OK == wf_candworker_start(&w));
	port = wf_candworker_port(w);
	path_in(L"big", big);
	path_in(L"d", small);
	for (uint32_t g = 1; g <= 40; g++) {
		/* a rarely matching prefix keeps the worker scanning */
		CHECK(WF_OK ==
		        port.start(port.ctx, g, big, L"zzz", false, false));
	}
	CHECK(WF_OK == port.start(port.ctx, 41, small, L"Mu", false, false));
	while (next_batch(w, &b, 3000)) {
		if (41 == b.gen) {
			newest++;
		} else {
			stale++;
		}
	}
	CHECK(0 == stale && 1 == newest); /* only the live search answers */
	/* more() for a search that is not live is refused */
	CHECK(WF_E_INVAL == port.more(port.ctx, 7));
	Sleep(100);
	CHECK(0 == wf_candworker_open_handles(w));
	CHECK(WF_OK == wf_candworker_stop(w).status);
}

static void cancel_closes_the_handle(void)
{
	WfCandWorker *w;
	WfCandPort port;
	WfCandBatch b;
	wchar_t dir[1100];
	int waited = 0;

	CHECK(WF_OK == wf_candworker_start(&w));
	port = wf_candworker_port(w);
	path_in(L"big", dir);
	CHECK(WF_OK == port.start(port.ctx, 1, dir, L"", false, false));
	CHECK(next_batch(w, &b, 5000) && b.more);
	CHECK(1 == wf_candworker_open_handles(w));
	port.cancel(port.ctx);
	while (wf_candworker_open_handles(w) && waited++ < 100) {
		Sleep(20);
	}
	CHECK(0 == wf_candworker_open_handles(w));
	CHECK(WF_E_INVAL == port.more(port.ctx, 1)); /* nothing to continue */
	CHECK(!next_batch(w, &b, 200));
	CHECK(WF_OK == wf_candworker_stop(w).status);
}

static void no_handles_leak_over_many_searches(void)
{
	WfCandWorker *w;
	WfCandPort port;
	WfCandBatch b;
	wchar_t dir[1100];
	DWORD before, after;

	CHECK(WF_OK == wf_candworker_start(&w));
	port = wf_candworker_port(w);
	path_in(L"d", dir);
	GetProcessHandleCount(GetCurrentProcess(), &before);
	for (uint32_t g = 1; g <= 400; g++) {
		CHECK(port.start(port.ctx, g, dir, g % 3 ? L"do" : L"", false,
		              false) == WF_OK);
		if (g % 2) {
			CHECK(next_batch(w, &b, 5000) && b.gen == g);
		} else {
			port.cancel(port.ctx);
		}
	}
	Sleep(200);
	GetProcessHandleCount(GetCurrentProcess(), &after);
	CHECK(wf_candworker_open_handles(w) <= 1);
	CHECK(after <= before + 4);
	CHECK(WF_OK == wf_candworker_stop(w).status);
}

static void the_fifth_hit_costs_a_short_scan_and_a_rare_one_a_long_scan(void)
{
	WfCandWorker *w;
	WfCandPort port;
	WfCandBatch b;
	wchar_t dir[1100];
	double t0, first, more, rare;

	CHECK(WF_OK == wf_candworker_start(&w));
	port = wf_candworker_port(w);
	path_in(L"big", dir);
	t0 = now_ms();
	CHECK(WF_OK == port.start(port.ctx, 1, dir, L"item", false, false));
	CHECK(next_batch(w, &b, 5000) && WF_CAND_BATCH == b.n && b.more);
	first = now_ms() - t0;
	t0 = now_ms();
	CHECK(WF_OK == port.more(port.ctx, 1));
	CHECK(next_batch(w, &b, 5000) && WF_CAND_BATCH == b.n && b.more);
	more = now_ms() - t0;
	t0 = now_ms();
	CHECK(WF_OK == port.start(port.ctx, 2, dir, L"needle", false, false));
	CHECK(next_batch(w, &b, 10000) && 1 == b.n && !b.more);
	rare = now_ms() - t0;
	printf("  5000-entry folder: first five %.2f ms, next five %.2f ms, "
	       "a rare name %.2f ms\n",
	        first, more, rare);
	CHECK(first < 500 && more < 500 && rare < 5000);
	CHECK(WF_OK == wf_candworker_stop(w).status);
}

static void stopping_while_scanning_returns_promptly(void)
{
	WfCandWorker *w;
	WfCandPort port;
	wchar_t dir[1100];
	double t0;
	WfWorkerStopReport r;

	CHECK(WF_OK == wf_candworker_start(&w));
	port = wf_candworker_port(w);
	path_in(L"big", dir);
	CHECK(WF_OK == port.start(port.ctx, 1, dir, L"zzz", false, false));
	t0 = now_ms();
	r = wf_candworker_stop(w);
	CHECK(WF_OK == r.status && !r.deferred && now_ms() - t0 < 1500);
}

int main(void)
{
	size_t i;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (!p0_lab_open(&lab, "candworker", 0) || !lab.area) {
		return TEST_RESULT();
	}
	wcscpy_s(root, 1024, lab.root);
	while (wcslen(root) > 3 && L'\\' == root[wcslen(root) - 1]) {
		root[wcslen(root) - 1] = 0;
	}
	make_fixture();
	the_first_batch_then_the_next();
	only_folders_when_asked();
	prefixes_match_ordinal_ignoring_case_and_not_short_names();
	hidden_entries_only_when_asked();
	flags_tell_folders_from_files();
	errors_are_told_not_hidden();
	a_newer_search_ends_the_older_one();
	cancel_closes_the_handle();
	no_handles_leak_over_many_searches();
	the_fifth_hit_costs_a_short_scan_and_a_rare_one_a_long_scan();
	stopping_while_scanning_returns_promptly();
	/* the fixture's own cleanup: files first, then folders */
	{
		WIN32_FIND_DATAW fd;
		HANDLE f;
		wchar_t pat[1100], p[1100];
		static const wchar_t *const folders[] = {
		        L"d", L"big", L"empty"};

		for (i = 0; i < 3; i++) {
			path_in(folders[i], pat);
			wcscat_s(pat, 1100, L"\\*");
			f = FindFirstFileW(pat, &fd);
			if (INVALID_HANDLE_VALUE != f) {
				do {
					if (!wcscmp(fd.cFileName, L".") ||
					        !wcscmp(fd.cFileName, L"..")) {
						continue;
					}
					swprintf(p, 1100, L"%ls\\%ls\\%ls",
					        root, folders[i], fd.cFileName);
					SetFileAttributesW(
					        p, FILE_ATTRIBUTE_NORMAL);
					if (fd.dwFileAttributes &
					        FILE_ATTRIBUTE_DIRECTORY) {
						RemoveDirectoryW(p);
					} else {
						DeleteFileW(p);
					}
				} while (FindNextFileW(f, &fd));
				FindClose(f);
			}
			path_in(folders[i], p);
			RemoveDirectoryW(p);
		}
	}
	CHECK(0 == p0_lab_close(&lab, 0 != wf_test_failures));
	return TEST_RESULT();
}
