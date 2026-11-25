/* Integration test: the real probe, the real preflight, the real worker. */
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "../tools/p0/p0_lab.h"
#include "fs/wf_enum_win.h"
#include "path/wf_path.h"
#include "wf_test.h"
#include "xfer/wf_probe_win.h"
#include "xfer/wf_xferworker.h"

static wchar_t root[MAX_PATH * 2];
static WfProbeOps ops;
static P0Lab lab;

static void P(wchar_t *out, const wchar_t *rel)
{
	swprintf(out, MAX_PATH * 2, L"%ls%ls%ls", root,
	        L'\\' == root[wcslen(root) - 1] ? L"" : L"\\", rel);
}

static void mkdir_(const wchar_t *rel)
{
	wchar_t p[MAX_PATH * 2];

	P(p, rel);
	CHECK(p0_area_mkdir(lab.area, rel));
}

static void mkfile(const wchar_t *rel, DWORD bytes)
{
	wchar_t p[MAX_PATH * 2];
	HANDLE h;
	char buf[1024];
	DWORD wrote;

	P(p, rel);
	h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	if (INVALID_HANDLE_VALUE == h) {
		return;
	}
	memset(buf, 'x', sizeof buf);
	while (bytes) {
		DWORD n = bytes < sizeof buf ? bytes : (DWORD)sizeof buf;

		WriteFile(h, buf, n, &wrote, NULL);
		bytes -= n;
	}
	CloseHandle(h);
	CHECK(p0_area_track(lab.area, rel, 0));
}

static void append_byte(const wchar_t *rel)
{
	wchar_t p[MAX_PATH * 2];
	HANDLE h;
	DWORD wrote;

	P(p, rel);
	h = CreateFileW(p, FILE_APPEND_DATA, 0, NULL, OPEN_EXISTING,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	if (INVALID_HANDLE_VALUE != h) {
		WriteFile(h, "y", 1, &wrote, NULL);
		CloseHandle(h);
	}
}

static int junction(const wchar_t *rel_link, const wchar_t *rel_target)
{
	wchar_t cmd[MAX_PATH * 6], link[MAX_PATH * 2], tgt[MAX_PATH * 2];
	STARTUPINFOW si = {.cb = sizeof si};
	PROCESS_INFORMATION pi;
	DWORD code = 1;

	P(link, rel_link);
	P(tgt, rel_target);
	swprintf(cmd, MAX_PATH * 6,
	        L"cmd.exe /c mklink /J \"%ls\" \"%ls\" >nul", link, tgt);
	si.dwFlags = STARTF_USESHOWWINDOW;
	if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
	            NULL, NULL, &si, &pi)) {
		return 0;
	}
	WaitForSingleObject(pi.hProcess, 10000);
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	if (0 == code) {
		CHECK(p0_area_track(lab.area, rel_link, 1));
	}
	return 0 == code;
}

static WfPreflightReport run(WfTransferMode m,
        const wchar_t *const *names,
        size_t n,
        const wchar_t *dest_rel,
        const WfPreflightPolicy *pol)
{
	wchar_t src[MAX_PATH * 2], dst[MAX_PATH * 2];
	WfTransferSlot s;
	WfPreflightReport r;

	P(src, L"src");
	P(dst, dest_rel);
	CHECK(WF_OK == wf_xfer_capture(&ops, m, src, names, n, 0, &s));
	CHECK(wf_preflight_run(&ops, &s, dst, pol ? pol : &WF_PREFLIGHT_DEFAULT,
	              NULL, &r) == WF_OK);
	wf_slot_free(&s);
	return r;
}

/* Names in a directory, joined: a cheap "did anything change" oracle. */
static void listing(const wchar_t *rel, wchar_t *out, size_t cap)
{
	wchar_t p[MAX_PATH * 2];
	WfDir *d = NULL;
	size_t i;

	P(p, rel);
	out[0] = L'\0';
	if (WF_OK != wf_enum_dir_win(p, &d)) {
		return;
	}
	for (i = 0; i < d->n; i++) {
		wcscat_s(out, cap, d->e[i].name);
		wcscat_s(out, cap, L"|");
	}
	wf_dir_free(d);
}

static void test_probe(void)
{
	wchar_t p[MAX_PATH * 2];
	WfNodeInfo a, a2, d, none;
	WfVolInfo v;

	P(p, L"src\\a.txt");
	CHECK(WF_OK == ops.node(NULL, p, &a));
	CHECK(!(a.flags & WF_ENT_DIR) && 10 == a.size && 0 != a.mtime &&
	        1 == a.nlinks);
	CHECK((a.id.kind == WF_ID_STRONG)); /* NTFS/ReFS test machine */
	CHECK(WF_OK == ops.node(NULL, p, &a2) &&
	        wf_fileid_equal(&a.id, &a2.id));

	P(p, L"src");
	CHECK(WF_OK == ops.node(NULL, p, &d) && (d.flags & WF_ENT_DIR));
	CHECK(!wf_fileid_equal(&a.id, &d.id));

	P(p, L"src\\missing");
	CHECK(WF_E_NOENT == ops.node(NULL, p, &none));
	CHECK(WF_E_INVAL == ops.node(NULL, L"relative", &none));

	P(p, L"src");
	CHECK(WF_OK == ops.volume(NULL, p, &v));
	CHECK(v.is_local && !v.read_only && 255 == v.max_component &&
	        v.free_bytes > 0);
	CHECK(L'\0' != v.root[0] && L'\0' != v.fs_name[0]);
	printf("  volume: fs=%ls root=%ls free=%llu open_by_id=%d\n", v.fs_name,
	        v.root, (unsigned long long)v.free_bytes,
	        v.supports_open_by_id);

	P(p, L"dst");
	CHECK(WF_OK == ops.dest_access(NULL, p));
	P(p, L"nowhere");
	CHECK(WF_E_NOENT == ops.dest_access(NULL, p));
	/* attribute-only probing must not have created anything */
	P(p, L"dst");
	{
		wchar_t l[512];

		listing(L"dst", l, 512);
		CHECK(L'\0' == l[0]);
	}
}

static void test_identity_changes(void)
{
	static const wchar_t *const n[] = {L"a.txt"};
	wchar_t p[MAX_PATH * 2];
	WfPreflightReport r;

	r = run(WF_XFER_COPY, n, 1, L"dst", NULL);
	CHECK(WF_PF_OK == r.reason);

	{ /* capture, then grow the file before preflight */
		wchar_t src[MAX_PATH * 2], dst[MAX_PATH * 2];
		WfTransferSlot s;

		P(src, L"src");
		P(dst, L"dst");
		CHECK(WF_OK ==
		        wf_xfer_capture(&ops, WF_XFER_COPY, src, n, 1, 0, &s));
		append_byte(L"src\\a.txt");
		CHECK(wf_preflight_run(&ops, &s, dst, &WF_PREFLIGHT_DEFAULT,
		              NULL, &r) == WF_OK);
		CHECK(WF_PF_SOURCE_CHANGED == r.reason);
		wf_slot_free(&s);
	}
	{ /* replaced by a different file of the same name and size */
		wchar_t src[MAX_PATH * 2], dst[MAX_PATH * 2], tmp[MAX_PATH * 2];
		WfTransferSlot s;

		P(src, L"src");
		P(dst, L"dst");
		CHECK(WF_OK ==
		        wf_xfer_capture(&ops, WF_XFER_COPY, src, n, 1, 0, &s));
		P(p, L"src\\a.txt");
		P(tmp, L"src\\a.new");
		mkfile(L"src\\a.new", (DWORD)s.src[0].snap.size);
		CHECK(p0_area_remove(lab.area, L"src\\a.txt"));
		CHECK(MoveFileW(tmp, p));
		CHECK(p0_area_track(lab.area, L"src\\a.txt", 0));
		CHECK(wf_preflight_run(&ops, &s, dst, &WF_PREFLIGHT_DEFAULT,
		              NULL, &r) == WF_OK);
		CHECK(WF_PF_SOURCE_CHANGED ==
		        r.reason); /* new FileId / mtime */
		wf_slot_free(&s);
	}
	{ /* gone */
		static const wchar_t *const g[] = {L"b.txt"};
		wchar_t src[MAX_PATH * 2], dst[MAX_PATH * 2];
		WfTransferSlot s;

		P(src, L"src");
		P(dst, L"dst");
		CHECK(WF_OK ==
		        wf_xfer_capture(&ops, WF_XFER_COPY, src, g, 1, 0, &s));
		P(p, L"src\\b.txt");
		CHECK(p0_area_remove(lab.area, L"src\\b.txt"));
		CHECK(wf_preflight_run(&ops, &s, dst, &WF_PREFLIGHT_DEFAULT,
		              NULL, &r) == WF_OK);
		CHECK(WF_PF_SOURCE_MISSING == r.reason);
		wf_slot_free(&s);
		mkfile(L"src\\b.txt", 20); /* restore for later tests */
	}
}

static void test_refusals_do_not_touch_the_destination(void)
{
	static const wchar_t *const all[] = {L"a.txt", L"b.txt", L"tree"};
	wchar_t before[512], after[512];
	WfPreflightReport r;
	WfPreflightPolicy tiny = WF_PREFLIGHT_DEFAULT;
	WfPreflightPolicy hungry = WF_PREFLIGHT_DEFAULT;

	/* one existing name blocks the lot, and nothing appears in dst */
	mkfile(L"dst\\b.txt", 1);
	listing(L"dst", before, 512);
	r = run(WF_XFER_COPY, all, 3, L"dst", NULL);
	CHECK(WF_PF_COLLISION == r.reason && 0 == wcscmp(r.bad_name, L"b.txt"));
	listing(L"dst", after, 512);
	CHECK(0 == wcscmp(before, after));
	{
		wchar_t p[MAX_PATH * 2];

		P(p, L"dst\\b.txt");
		CHECK(p0_area_remove(lab.area, L"dst\\b.txt"));
	}

	/* same folder */
	r = run(WF_XFER_COPY, all, 3, L"src", NULL);
	CHECK(WF_PF_SAME_FOLDER == r.reason);

	/* destination inside a source: by path, then through a junction alias
	 */
	{
		static const wchar_t *const t[] = {L"tree"};

		r = run(WF_XFER_COPY, t, 1, L"src\\tree\\sub", NULL);
		CHECK(WF_PF_DEST_INSIDE_SOURCE == r.reason);
		if (junction(L"alias", L"src\\tree")) {
			/* a real junction onto the source: refused as a link,
			 * and the source tree behind it stays intact */
			r = run(WF_XFER_COPY, t, 1, L"alias", NULL);
			CHECK(WF_PF_DEST_REPARSE == r.reason);
			/* a link to somewhere unrelated is refused the same way
			 */
			r = run(WF_XFER_COPY, t, 1, L"alias\\sub", NULL);
			CHECK(WF_PF_DEST_REPARSE == r.reason ||
			        WF_PF_DEST_INSIDE_SOURCE == r.reason);
			{
				wchar_t p[MAX_PATH * 2];

				P(p, L"alias");
				CHECK(p0_area_remove(lab.area,
				        L"alias")); /* junction only */
				P(p, L"src\\tree\\x.bin");
				CHECK(INVALID_FILE_ATTRIBUTES !=
				        GetFileAttributesW(p));
			}
		} else {
			printf("  SKIP: mklink /J unavailable\n");
		}
	}

	/* transaction-path overhead is judged before anything is created */
	r = run(WF_XFER_COPY, all, 3, L"dst", NULL);
	CHECK(WF_PF_OK == r.reason && r.tree_files >= 3);
	tiny.max_path_units = r.longest_txn - 1;
	r = run(WF_XFER_COPY, all, 3, L"dst", &tiny);
	CHECK(WF_PF_PATH_TOO_LONG == r.reason);
	CHECK(r.longest_final <=
	        tiny.max_path_units); /* final path alone would fit */

	/* a reserve larger than the disk can never be satisfied */
	hungry.reserve_bytes = UINT64_MAX / 2;
	r = run(WF_XFER_COPY, all, 3, L"dst", &hungry);
	CHECK(WF_PF_NO_SPACE == r.reason);

	listing(L"dst", after, 512);
	CHECK(L'\0' == after[0]); /* still empty after every refusal above */
}

static void test_reparse_in_tree(void)
{
	static const wchar_t *const t[] = {L"tree"};
	WfPreflightReport r;
	wchar_t p[MAX_PATH * 2];

	if (!junction(L"src\\tree\\jct", L"dst")) {
		printf("  SKIP: mklink /J unavailable\n");
		return;
	}
	{
		WfNodeInfo n;

		P(p, L"src\\tree\\jct");
		CHECK(WF_OK == ops.node(NULL, p, &n));
		CHECK((n.flags & WF_ENT_LINK) && (n.flags & WF_ENT_DIR));
		CHECK(WF_REPARSE_REDIRECT ==
		        wf_reparse_classify(n.flags, n.reparse_tag));
		CHECK(0xA0000003u == n.reparse_tag);
	}
	r = run(WF_XFER_COPY, t, 1, L"dst", NULL);
	CHECK(WF_PF_REPARSE == r.reason && 0 == wcscmp(r.bad_name, L"jct"));

	/* moving the folder renames it; a junction inside is not touched */
	r = run(WF_XFER_MOVE, t, 1, L"dst", NULL);
	CHECK(WF_PF_OK == r.reason);

	P(p, L"src\\tree\\jct");
	CHECK(p0_area_remove(lab.area, L"src\\tree\\jct"));
}

static void test_hardlink_count(void)
{
	wchar_t a[MAX_PATH * 2], b[MAX_PATH * 2];
	WfNodeInfo n;

	P(a, L"src\\a.txt");
	P(b, L"src\\a.link");
	if (!CreateHardLinkW(b, a, NULL)) {
		printf("  SKIP: hard links unavailable\n");
		return;
	}
	CHECK(p0_area_track(lab.area, L"src\\a.link", 0));
	CHECK(WF_OK == ops.node(NULL, a, &n) && 2 == n.nlinks);
	CHECK(p0_area_remove(lab.area, L"src\\a.link"));
	CHECK(WF_OK == ops.node(NULL, a, &n) && 1 == n.nlinks);
}

static void test_worker(void)
{
	static const wchar_t *const names[] = {L"a.txt", L"tree"};
	WfXferWorker *w = NULL;
	WfXferPort port;
	WfXferResult r;
	wchar_t src[MAX_PATH * 2], dst[MAX_PATH * 2];

	P(src, L"src");
	P(dst, L"dst");
	CHECK(WF_OK == wf_xferworker_start(&w, ops, WF_PREFLIGHT_DEFAULT));
	port = wf_xferworker_port(w);

	CHECK(!wf_xferworker_poll(w, &r));
	CHECK(WF_OK ==
	        port.capture(port.ctx, 1, WF_XFER_COPY, src, names, 2, 0));
	CHECK(WAIT_OBJECT_0 ==
	        WaitForSingleObject(wf_xferworker_event(w), 5000));
	CHECK(wf_xferworker_poll(w, &r) && WF_XR_CAPTURE == r.kind &&
	        1 == r.gen);
	CHECK(WF_OK == r.st && 2 == r.slot.count &&
	        WF_XFER_COPY == r.slot.mode);

	CHECK(WF_OK == port.preflight(port.ctx, 2, &r.slot, dst));
	wf_slot_free(&r.slot);
	CHECK(WAIT_OBJECT_0 ==
	        WaitForSingleObject(wf_xferworker_event(w), 5000));
	CHECK(wf_xferworker_poll(w, &r) && WF_XR_PREFLIGHT == r.kind &&
	        2 == r.gen);
	CHECK(WF_OK == r.st && WF_PF_OK == r.report.reason);
	CHECK(WAIT_TIMEOUT == WaitForSingleObject(wf_xferworker_event(w), 0));

	/* a capture of something that is not there fails cleanly */
	{
		static const wchar_t *const ghost[] = {L"ghost"};

		CHECK(port.capture(port.ctx, 3, WF_XFER_COPY, src, ghost, 1,
		              0) == WF_OK);
		CHECK(WAIT_OBJECT_0 ==
		        WaitForSingleObject(wf_xferworker_event(w), 5000));
		CHECK(wf_xferworker_poll(w, &r) && WF_E_NOENT == r.st &&
		        0 == r.slot.count);
	}

	/* a burst: the newest is always answered, nothing leaks or hangs */
	{
		int i, got = 0;

		for (i = 10; i < 60; i++) {
			CHECK(port.capture(port.ctx, (uint32_t)i, WF_XFER_COPY,
			              src, names, 2, 0) == WF_OK);
		}
		while (WAIT_OBJECT_0 ==
		                WaitForSingleObject(
		                        wf_xferworker_event(w), 3000) &&
		        wf_xferworker_poll(w, &r)) {
			got |= r.gen == 59;
			wf_slot_free(&r.slot);
			if (got) {
				break;
			}
		}
		CHECK(got);
	}

	/* stopping with work queued must be clean */
	CHECK(WF_OK ==
	        port.capture(port.ctx, 99, WF_XFER_COPY, src, names, 2, 0));
	{
		WfWorkerStopReport report = wf_xferworker_stop(w);
		CHECK(WF_OK == report.status && !report.deferred);
		if (report.completion) {
			CloseHandle(report.completion);
		}
	}
	CHECK(WF_OK == wf_xferworker_stop(NULL).status);
}

static void cleanup(void)
{
	CHECK(0 == p0_lab_close(&lab, 0 != wf_test_failures));
}

/* A restricted runner may allow its scratch subtree while denying a profile
 * ancestor. Production must still refuse that destination. Exercise that
 * refusal, then report the unavailable integration environment as SKIP;
 * never weaken preflight or claim the remaining scenarios passed. */
static int ancestors_available(void)
{
	wchar_t path[MAX_PATH * 2];
	wcscpy_s(path, MAX_PATH * 2, root);
	for (;;) {
		WfNodeInfo node;
		WfStatus status = ops.node(ops.ctx, path, &node);
		size_t parent;
		if (WF_OK != status) {
			static const wchar_t *const names[] = {L"a.txt"};
			WfPreflightReport report =
			        run(WF_XFER_COPY, names, 1, L"dst", NULL);
			CHECK(WF_PF_IO_ERROR == report.reason);
			printf("BLOCKED: ancestor %ls cannot be probed "
			       "(status=%d); preflight safely refuses.\n",
			        path, (int)status);
			return 0;
		}
		parent = wf_path_parent_len(path);
		if (!parent) {
			return 1;
		}
		path[parent] = 0;
	}
}

int main(int argc, char **argv)
{
	int alias = !(2 == argc && !strcmp(argv[1], "--literal"));
	if (!p0_lab_open(&lab, "xfer", alias)) {
		printf("BLOCKED dedicated test environment: error=%lu\n",
		        lab.error);
		return 77;
	}
	wcscpy_s(root, MAX_PATH * 2, lab.root);
	printf("scratch=%ls logical_root=%ls mapped=%d\n",
	        p0_area_root(lab.area), root, lab.mapped);
	ops = wf_probe_win();

	mkdir_(L"src");
	mkdir_(L"dst");
	mkfile(L"src\\a.txt", 10);
	mkfile(L"src\\b.txt", 20);
	mkdir_(L"src\\tree");
	mkfile(L"src\\tree\\x.bin", 3000);
	mkdir_(L"src\\tree\\sub");
	mkfile(L"src\\tree\\sub\\y.bin", 50);
	mkdir_(L"src\\empty");
	if (!ancestors_available()) {
		cleanup();
		return wf_test_failures ? 1 : 77;
	}

	test_probe();
	test_hardlink_count();
	test_refusals_do_not_touch_the_destination();
	test_reparse_in_tree();
	test_worker();
	test_identity_changes(); /* last: it replaces and deletes sources */

	cleanup();
	return TEST_RESULT();
}
