/*
 * The WSL operations and the editor's save against a real, running WSL
 * distribution, in a scratch folder of its own under /tmp. Nothing is started
 * or installed: if no distribution answers, the test says NOT RUN and exits
 * 77, which CTest reports as Skipped (never as Passed).
 * Set PIERO_NO_REAL_WSL=1 to skip it on purpose.
 *
 * Linux-side facts (mode bits, links) are prepared and read with wsl.exe as
 * the ordinary user, only inside the scratch folder.
 */
#include <windows.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/wf_efile_win.h"
#include "fs/wf_editworker.h"
#include "fs/wf_wsl_win.h"
#include "fs/wf_wslops_win.h"
#include "wf_test.h"

/* CTest reports this exit code as Skipped (SKIP_RETURN_CODE), never as Passed. */
#define SKIP_EXIT 77

static wchar_t root[512]; /* \\wsl.localhost\<distro>\tmp\piero-test-... */
static char distro[WF_WSL_NAME_MAX];
static char linux_root[256]; /* /tmp/piero-test-... */
static bool have_shell;
static bool part_skipped; /* some checks could not run: not a full PASS */

static void at(const wchar_t *name, wchar_t *out)
{
	swprintf(out, 1100, L"%ls\\%ls", root, name);
}

static bool put(const wchar_t *name, const void *data, size_t n)
{
	wchar_t p[1100];
	HANDLE h;
	DWORD done = 0;
	bool ok;

	at(name, p);
	h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	ok = !n || (WriteFile(h, data, (DWORD)n, &done, NULL) && done == n);
	CloseHandle(h);
	return ok;
}

static size_t slurp(const wchar_t *name, void *buf, size_t cap)
{
	wchar_t p[1100];
	HANDLE h;
	DWORD got = 0;

	at(name, p);
	h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return (size_t)-1;
	}
	if (!ReadFile(h, buf, (DWORD)cap, &got, NULL)) {
		got = (DWORD)-1;
	}
	CloseHandle(h);
	return got;
}

static bool has(const wchar_t *name, const char *data)
{
	char buf[4096];
	size_t n = slurp(name, buf, sizeof buf);

	return n == strlen(data) && !memcmp(buf, data, n);
}

static bool exists(const wchar_t *name)
{
	wchar_t p[1100];

	at(name, p);
	return INVALID_FILE_ATTRIBUTES != GetFileAttributesW(p);
}

/* Runs `sh -c <script>` in the distribution as the ordinary user; the output
 * (trimmed) goes to `out`. */
static bool shell(const char *script, char *out, size_t cap)
{
	char cmd[1024];
	FILE *f;
	size_t n;

	snprintf(cmd, sizeof cmd, "wsl.exe -d %s -- sh -c \"%s\" 2>nul", distro,
	        script);
	f = _popen(cmd, "r");
	if (!f) {
		return false;
	}
	n = fread(out, 1, cap - 1, f);
	out[n] = 0;
	while (n && ('\n' == out[n - 1] || '\r' == out[n - 1])) {
		out[--n] = 0;
	}
	return 0 == _pclose(f);
}

static bool find_distribution(void)
{
	WfWslDistro d[16];
	size_t n = wf_wsl_distros(d, 16);

	for (size_t i = 0; i < n; i++) {
		wchar_t probe[256], name[WF_WSL_NAME_MAX];
		size_t k = 0;

		for (; d[i].name[k]; k++) {
			name[k] = (wchar_t)d[i].name[k];
		}
		name[k] = 0;
		swprintf(probe, 256, L"\\\\wsl.localhost\\%ls\\tmp", name);
		if (INVALID_FILE_ATTRIBUTES != GetFileAttributesW(probe)) {
			size_t j = 0;

			for (; d[i].name[j] && j + 1 < sizeof distro; j++) {
				distro[j] = (char)d[i].name[j];
			}
			distro[j] = 0;
			swprintf(root, 512,
			        L"\\\\wsl.localhost\\%ls\\tmp\\piero-test-%lu-%"
			        L"lu",
			        name, (unsigned long)GetCurrentProcessId(),
			        (unsigned long)GetTickCount());
			snprintf(linux_root, sizeof linux_root,
			        "/tmp/piero-test-%lu-%lu",
			        (unsigned long)GetCurrentProcessId(),
			        (unsigned long)GetTickCount());
			return true;
		}
	}
	return false;
}

static void names_are_kept_exactly(void)
{
	WfWslReport r;

	CHECK(WF_OK == wf_wslops_create(root, L"File", false, &r));
	CHECK(WF_OK == wf_wslops_create(root, L"file", false, &r));
	CHECK(WF_OK == wf_wslops_create(root, L"FILE", false, &r));
	CHECK(WF_OK != wf_wslops_create(root, L"file", false, &r)); /* taken */
	CHECK(exists(L"File") && exists(L"file") && exists(L"FILE"));
	CHECK(put(L"File", "1", 1) && put(L"file", "22", 2) &&
	        put(L"FILE", "333", 3));
	CHECK(has(L"File", "1") && has(L"file", "22") && has(L"FILE", "333"));
	CHECK(WF_OK == wf_wslops_create(root, L"with space", false, &r));
	CHECK(WF_OK ==
	        wf_wslops_create(root, L"\u65e5\u672c\u8a9e.txt", false, &r));
	CHECK(exists(L"\u65e5\u672c\u8a9e.txt"));
	/* a name with a separator is never created */
	CHECK(WF_OK != wf_wslops_create(root, L"back\\slash", false, &r));
}

static void rename_delete_and_copy_work_on_the_real_thing(void)
{
	WfWslReport r;
	wchar_t dest[1100], src[1100];

	CHECK(put(L"a.txt", "alpha", 5));
	CHECK(WF_OK != wf_wslops_rename(root, L"a.txt", root, L"file", &r));
	CHECK(has(L"file", "22") && has(L"a.txt", "alpha"));
	CHECK(WF_OK == wf_wslops_rename(root, L"a.txt", root, L"b.txt", &r));
	CHECK(!exists(L"a.txt") && has(L"b.txt", "alpha"));

	CHECK(WF_OK == wf_wslops_create(root, L"dir", true, &r));
	at(L"dir", dest);
	at(L"b.txt", src);
	CHECK(WF_OK == wf_wslops_copy(src, dest, L"copy.txt", &r));
	CHECK(has(L"dir\\copy.txt", "alpha") && has(L"b.txt", "alpha"));
	CHECK(WF_OK !=
	        wf_wslops_copy(src, dest, L"copy.txt", &r)); /* no overwrite */
	CHECK(has(L"dir\\copy.txt", "alpha"));

	/* a folder out of the distribution and back */
	{
		wchar_t back[1100];

		at(L"dir", src);
		at(L"back", back);
		CHECK(WF_OK == wf_wslops_create(root, L"back", true, &r));
		CHECK(WF_OK == wf_wslops_copy(src, back, L"dir", &r));
		CHECK(has(L"back\\dir\\copy.txt", "alpha"));
	}

	at(L"back", src);
	CHECK(WF_OK != wf_wslops_delete(src, false, &r)); /* not empty */
	CHECK(exists(L"back\\dir\\copy.txt"));
	CHECK(WF_OK == wf_wslops_delete(src, true, &r) && !exists(L"back"));
	at(L"b.txt", src);
	CHECK(WF_OK == wf_wslops_delete(src, false, &r) && !exists(L"b.txt"));
}

static void a_save_keeps_the_mode_and_a_link_is_never_followed(void)
{
	WfEncoding utf8 = {WF_ENC_UTF8, 0};
	WfLoaded l;
	WfSaveResult sr;
	WfFileStamp was;
	wchar_t p[1100], back[1100], lnk[1100];
	char out[256], script[512];
	WfWslReport r;

	at(L"run.sh", p);
	at(L"run.sh~", back);
	CHECK(put(L"run.sh", "#!/bin/sh\necho old\n", 19));
	CHECK(WF_OK == wf_efile_load(p, 0, 1252, NULL, &l) &&
	        WF_FL_OK == l.verdict && l.stamp.valid);
	CHECK(0 ==
	        l.stamp.id.VolumeSerialNumber); /* no identity on this path */
	was = l.stamp;
	wf_efile_loaded_free(&l);

	if (have_shell) {
		snprintf(script, sizeof script,
		        "chmod 751 %s/run.sh && stat -c %%a %s/run.sh",
		        linux_root, linux_root);
		CHECK(shell(script, out, sizeof out) && !strcmp(out, "751"));
	}
	CHECK(WF_OK ==
	                wf_efile_save_over(p, &was, "#!/bin/sh\necho new\n", 19,
	                        utf8, NULL, &sr) &&
	        WF_SV_OK == sr.verdict);
	CHECK(has(L"run.sh", "#!/bin/sh\necho new\n") &&
	        has(L"run.sh~", "#!/bin/sh\necho old\n"));
	if (have_shell) {
		snprintf(script, sizeof script, "stat -c %%a %s/run.sh",
		        linux_root);
		CHECK(shell(script, out, sizeof out) && !strcmp(out, "751"));
	}

	/* the file changed on the Linux side since it was read */
	DeleteFileW(back);
	CHECK(put(L"run.sh", "#!/bin/sh\necho TWO\n", 19));
	CHECK(WF_OK ==
	                wf_efile_save_over(
	                        p, &sr.stamp, "x", 1, utf8, NULL, &sr) &&
	        WF_SV_CONFLICT == sr.verdict);
	CHECK(has(L"run.sh", "#!/bin/sh\necho TWO\n"));

	/* a new file, and the name taken */
	at(L"fresh.txt", p);
	CHECK(WF_OK == wf_efile_save_new(p, "fresh\n", 6, utf8, &sr) &&
	        WF_SV_OK == sr.verdict && has(L"fresh.txt", "fresh\n"));
	if (WF_SV_OK != sr.verdict) {
		printf("  save_new: verdict %d error %lu: %s\n",
		        (int)sr.verdict, (unsigned long)sr.error,
		        sr.reason ? sr.reason : "-");
	}
	CHECK(WF_OK == wf_efile_save_new(p, "again", 5, utf8, &sr) &&
	        WF_SV_EXISTS == sr.verdict && has(L"fresh.txt", "fresh\n"));

	if (!have_shell) {
		printf("  NOT RUN: wsl.exe cannot run commands (mode and "
		       "links)\n");
		part_skipped = true;
		return;
	}
	/* a symbolic link: not opened, not copied, deleted as a link */
	snprintf(script, sizeof script, "ln -s fresh.txt %s/lnk && echo ok",
	        linux_root);
	CHECK(shell(script, out, sizeof out) && !strcmp(out, "ok"));
	at(L"lnk", lnk);
	CHECK(WF_OK == wf_efile_load(lnk, 0, 1252, NULL, &l) &&
	        WF_FL_LINK == l.verdict);
	wf_efile_loaded_free(&l);
	CHECK(WF_OK == wf_efile_save_over(lnk, &was, "x", 1, utf8, NULL, &sr) &&
	        WF_SV_OK != sr.verdict);
	CHECK(has(L"fresh.txt", "fresh\n"));
	{
		wchar_t into[1100];

		at(L"dir", into);
		CHECK(WF_OK != wf_wslops_copy(lnk, into, L"lnk", &r));
		CHECK(!exists(L"dir\\lnk"));
	}
	CHECK(WF_OK == wf_wslops_delete(lnk, false, &r));
	if (r.reason) {
		printf("  delete link: error %lu: %s\n", (unsigned long)r.error,
		        r.reason);
	}
	CHECK(!exists(L"lnk") && has(L"fresh.txt", "fresh\n"));
}

static bool mtime_of(const wchar_t *name, FILETIME *t)
{
	wchar_t p[1100];
	HANDLE h;
	bool ok;

	at(name, p);
	h = CreateFileW(p, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	ok = 0 != GetFileTime(h, NULL, NULL, t);
	CloseHandle(h);
	return ok;
}

static void a_copy_keeps_the_time_and_an_empty_create_can_be_undone(void)
{
	WfWslReport r;
	wchar_t p[1100], d[1100];
	FILETIME old, a = {0}, b = {0};
	ULARGE_INTEGER u, x, y;
	HANDLE h;

	/* 2001-02-03 04:05:06 UTC */
	u.QuadPart = (11644473600ull + 981173106ull) * 10000000ull;
	old.dwLowDateTime = u.LowPart;
	old.dwHighDateTime = u.HighPart;
	CHECK(put(L"tm.txt", "x", 1));
	at(L"tm.txt", p);
	h = CreateFileW(p, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == h || !SetFileTime(h, NULL, NULL, &old)) {
		printf("  NOT RUN: the time of a WSL file cannot be set "
		       "(%lu)\n",
		        GetLastError());
		part_skipped = true;
	} else {
		CHECK(WF_OK == wf_wslops_create(root, L"tmd", true, &r));
		at(L"tmd", d);
		CHECK(WF_OK == wf_wslops_copy(p, d, L"tm.txt", &r));
		CHECK(mtime_of(L"tmd\\tm.txt", &a) && mtime_of(L"tm.txt", &b));
		x.LowPart = a.dwLowDateTime;
		x.HighPart = a.dwHighDateTime;
		y.LowPart = b.dwLowDateTime;
		y.HighPart = b.dwHighDateTime;
		/* within one second: the share may keep less than 100 ns */
		CHECK((x.QuadPart > y.QuadPart
		                      ? x.QuadPart - y.QuadPart
		                      : y.QuadPart - x.QuadPart) < 10000000ull);
		CHECK(x.QuadPart / 10000000ull < u.QuadPart / 10000000ull + 5);
	}
	if (INVALID_HANDLE_VALUE != h) {
		CloseHandle(h);
	}

	CHECK(WF_OK == wf_wslops_create(root, L"em.txt", false, &r));
	CHECK(put(L"full.txt", "x", 1));
	CHECK(WF_OK != wf_wslops_remove_if_empty(root, L"full.txt", &r) &&
	        exists(L"full.txt"));
	CHECK(WF_OK == wf_wslops_remove_if_empty(root, L"em.txt", &r) &&
	        !exists(L"em.txt"));
	CHECK(WF_OK == wf_wslops_create(root, L"emd", true, &r));
	CHECK(WF_OK == wf_wslops_remove_if_empty(root, L"emd", &r) &&
	        !exists(L"emd"));
}

/* Runs one request through the real edit worker, as the file manager does. */
static bool run(WfEditWorker *w,
        WfEditKind kind,
        const wchar_t *parent,
        const wchar_t *old_name,
        const wchar_t *dest,
        const wchar_t *new_name,
        WfEditResult *res)
{
	WfEditRequest req;
	WfEditPort port = wf_editworker_port(w);
	static uint32_t gen;

	memset(&req, 0, sizeof req);
	req.kind = kind;
	req.parent = parent;
	req.old_name = old_name;
	req.dest_parent = dest;
	req.new_name = new_name;
	if (WF_OK != port.submit(port.ctx, ++gen, &req)) {
		return false;
	}
	if (WAIT_OBJECT_0 !=
	        WaitForSingleObject(wf_editworker_event(w), 30000)) {
		return false;
	}
	return wf_editworker_poll(w, res) && res->gen == gen;
}

static void the_edit_worker_does_the_same_on_the_real_thing(void)
{
	WfEditWorker *w = NULL;
	WfEditResult res;
	wchar_t sub[1100];

	CHECK(WF_OK == wf_editworker_start(&w));
	if (!w) {
		return;
	}
	at(L"w", sub);
	CHECK(run(w, WF_EDIT_CREATE_DIR, root, NULL, NULL, L"w", &res) &&
	        WF_OK == res.outcome.st);
	CHECK(run(w, WF_EDIT_CREATE_FILE, sub, NULL, NULL, L"n.txt", &res) &&
	        WF_OK == res.outcome.st && exists(L"w\\n.txt"));
	CHECK(run(w, WF_EDIT_CREATE_FILE, sub, NULL, NULL, L"n.txt", &res) &&
	        WF_OK != res.outcome.st && res.outcome.detail);
	CHECK(run(w, WF_EDIT_RENAME, sub, L"n.txt", NULL, L"m.txt", &res) &&
	        WF_OK == res.outcome.st && exists(L"w\\m.txt") &&
	        !exists(L"w\\n.txt"));
	CHECK(run(w, WF_EDIT_WSL_COPY, sub, L"m.txt", sub, L"copy.txt", &res) &&
	        WF_OK == res.outcome.st && exists(L"w\\copy.txt"));
	CHECK(run(w, WF_EDIT_WSL_COPY, sub, L"m.txt", sub, L"copy.txt", &res) &&
	        WF_OK != res.outcome.st); /* never overwritten */
	CHECK(run(w, WF_EDIT_CREATE_DIR, sub, NULL, NULL, L"d", &res) &&
	        WF_OK == res.outcome.st);
	{
		wchar_t d[1100];

		at(L"w\\d", d);
		CHECK(run(w, WF_EDIT_MOVE, sub, L"copy.txt", d, L"copy.txt",
		              &res) &&
		        WF_OK == res.outcome.st && exists(L"w\\d\\copy.txt") &&
		        !exists(L"w\\copy.txt"));
	}
	CHECK(run(w, WF_EDIT_WSL_DELETE, root, L"w", NULL, L"w", &res) &&
	        WF_OK == res.outcome.st && !exists(L"w"));
	(void)wf_editworker_stop(w);
}

int main(void)
{
	WfWslReport r;
	char out[64];

	if (GetEnvironmentVariableA("PIERO_NO_REAL_WSL", out, sizeof out)) {
		printf("NOT RUN: PIERO_NO_REAL_WSL is set\n");
		return SKIP_EXIT;
	}
	if (!find_distribution()) {
		printf("NOT RUN: no running WSL distribution answered\n");
		return SKIP_EXIT;
	}
	if (!CreateDirectoryW(root, NULL)) {
		printf("NOT RUN: the scratch folder cannot be created (%lu)\n",
		        GetLastError());
		return SKIP_EXIT;
	}
	have_shell = shell("echo ok", out, sizeof out) && !strcmp(out, "ok");
	/* A distribution that was idle may still be waking up: wait (bounded)
	 * until it takes a write, or say NOT RUN rather than fail. */
	{
		bool ready = false;

		for (int attempt = 0; attempt < 20 && !ready; attempt++) {
			WfWslReport w;

			ready = WF_OK ==
			                wf_wslops_create(
			                        root, L".warm", false, &w) &&
			        WF_OK ==
			                wf_wslops_delete_in(
			                        root, L".warm", false, &w);
			if (!ready) {
				Sleep(500);
			}
		}
		if (!ready) {
			(void)wf_wslops_delete(root, true, &r);
			printf("NOT RUN: the distribution did not take a "
			       "write\n");
			return SKIP_EXIT;
		}
	}
	names_are_kept_exactly();
	rename_delete_and_copy_work_on_the_real_thing();
	a_save_keeps_the_mode_and_a_link_is_never_followed();
	a_copy_keeps_the_time_and_an_empty_create_can_be_undone();
	the_edit_worker_does_the_same_on_the_real_thing();
	/* the scratch folder was made by this run; it takes it away again */
	CHECK(WF_OK == wf_wslops_delete(root, true, &r));
	CHECK(INVALID_FILE_ATTRIBUTES == GetFileAttributesW(root));
	if (!wf_test_failures && part_skipped) {
		puts("SKIP: some WSL checks could not run (see NOT RUN above)");
		return SKIP_EXIT;
	}
	return TEST_RESULT();
}
