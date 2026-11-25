/*
 * NTFS facts of an entry (DR-DRAFT-026): the formatter with facts made by hand
 * (pure), and the reader on real files, folders, links and streams made in a
 * folder of the temporary directory. What the volume or the account cannot make
 * (a symbolic link without the privilege, compression, sparse files) is a SKIP,
 * never a pass.
 */
#include <windows.h>
#include <winioctl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs/wf_ntfsfacts.h"
#include "ui/wf_app.h"
#include "ui/wf_info.h"
#include "wf_test.h"

static wchar_t root[MAX_PATH + 40];
static int skips;
static bool hard_link_made;

static void skip(const char *what)
{
	printf("SKIP %s\n", what);
	skips++;
}

static bool has_line(const WfInfo *in, const char *part)
{
	for (int i = 0; i < in->n; i++) {
		if (strstr(in->text[i], part)) {
			return true;
		}
	}
	return false;
}

static void build(WfInfo *in, const WfNtfsFacts *f)
{
	WfEntry e;

	memset(&e, 0, sizeof e);
	e.name = L"a.txt";
	e.display = "a.txt";
	e.size = 3;
	e.meta_known = true;
	wf_info_build_facts(in, &e, NULL, NULL, NULL, NULL, f);
}

static void lines_without_facts(void)
{
	WfInfo none, not_asked;
	WfNtfsFacts f;

	build(&none, NULL);
	memset(&f, 0, sizeof f);
	build(&not_asked, &f); /* WF_NF_NOT_ASKED */
	CHECK(none.n == not_asked.n);
	CHECK(none.n > 4);
	CHECK(!has_line(&none, "Volume:"));
	CHECK(!has_line(&not_asked, "Streams:"));
}

static void lines_remote_and_failed(void)
{
	WfInfo in;
	WfNtfsFacts f;

	memset(&f, 0, sizeof f);
	f.state = WF_NF_REMOTE;
	build(&in, &f);
	CHECK(has_line(&in, "not read"));
	CHECK(!has_line(&in, "Volume:"));
	f.state = WF_NF_FAILED;
	f.error = 5;
	build(&in, &f);
	CHECK(has_line(&in, "could not be read (error 5)"));
	CHECK(!has_line(&in, "Streams:"));
}

static void lines_full(void)
{
	WfInfo in;
	WfNtfsFacts f;

	memset(&f, 0, sizeof f);
	f.state = WF_NF_OK;
	f.links_known = true;
	f.links = 3;
	f.streams_known = true;
	f.n_streams = 8;
	for (int i = 0; i < WF_NF_STREAMS; i++) {
		snprintf(f.stream[i].name, sizeof f.stream[i].name, "s%d", i);
		f.stream[i].size = (uint64_t)i;
	}
	f.volume_known = true;
	snprintf(f.root, sizeof f.root, "C:\\");
	snprintf(f.fs_name, sizeof f.fs_name, "NTFS");
	f.serial = 0xA1B2C3D4u;
	f.attributes = 0x0800u | 0x4000u | 0x0200u | 0x1000u;
	build(&in, &f);
	CHECK(has_line(&in, "3 (this one and 2 other hard links)"));
	CHECK(has_line(&in, "s0  0 bytes"));
	CHECK(has_line(&in, "s5  5 bytes"));
	CHECK(!has_line(&in, "s6"));
	CHECK(has_line(&in, "and 2 more"));
	CHECK(has_line(&in, "C:\\  NTFS  serial A1B2-C3D4"));
	CHECK(has_line(&in, "compressed, encrypted, sparse, offline"));
	CHECK(!has_line(&in, "Target:"));

	/* nothing special: one name, no streams, ordinary */
	memset(&f, 0, sizeof f);
	f.state = WF_NF_OK;
	f.links_known = true;
	f.links = 1;
	f.streams_known = true;
	build(&in, &f);
	CHECK(has_line(&in, "1 (no other hard link)"));
	CHECK(has_line(&in, "Streams:   none"));
	CHECK(has_line(&in, "Volume:    unknown"));
	CHECK(has_line(&in, "Stored:    ordinary"));

	/* parts that were not read are not numbers */
	f.links_known = false;
	f.streams_known = false;
	build(&in, &f);
	CHECK(has_line(&in, "Names:     unknown"));
	CHECK(has_line(&in, "too many to list"));

	/* a link with and without its target */
	f.attributes = FILE_ATTRIBUTE_REPARSE_POINT;
	f.target_known = true;
	snprintf(f.target, sizeof f.target, "D:\\somewhere");
	build(&in, &f);
	CHECK(has_line(&in, "Target:    D:\\somewhere"));
	f.target_known = false;
	build(&in, &f);
	CHECK(has_line(&in, "Target:    unreadable"));
}

static void put_file(const wchar_t *path, const char *text)
{
	HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD n;

	CHECK(INVALID_HANDLE_VALUE != h);
	if (INVALID_HANDLE_VALUE != h) {
		WriteFile(h, text, (DWORD)strlen(text), &n, NULL);
		CloseHandle(h);
	}
}

static const WfNtfsStream *stream_named(const WfNtfsFacts *f, const char *name)
{
	for (size_t i = 0; i < WF_NF_STREAMS && i < f->n_streams; i++) {
		if (!strcmp(f->stream[i].name, name)) {
			return &f->stream[i];
		}
	}
	return NULL;
}

static void plain_file_and_folder(void)
{
	wchar_t p[MAX_PATH + 80];
	WfNtfsFacts f;

	swprintf(p, MAX_PATH + 80, L"%ls\\plain.txt", root);
	put_file(p, "hello");
	wf_ntfsfacts_read(p, &f);
	CHECK(WF_NF_OK == f.state);
	CHECK(f.links_known && 1 == f.links);
	CHECK(f.streams_known && 0 == f.n_streams);
	CHECK(f.volume_known && f.fs_name[0] && f.root[0]);
	CHECK(0 == (f.attributes & FILE_ATTRIBUTE_REPARSE_POINT));
	CHECK(!f.target_known);

	wf_ntfsfacts_read(root, &f);
	CHECK(WF_NF_OK == f.state);
	CHECK(f.attributes & FILE_ATTRIBUTE_DIRECTORY);
	CHECK(f.links_known && 1 == f.links);
}

static void alternate_streams(void)
{
	wchar_t p[MAX_PATH + 80], s[MAX_PATH + 100];
	WfNtfsFacts f;
	const WfNtfsStream *a, *b;

	swprintf(p, MAX_PATH + 80, L"%ls\\ads.txt", root);
	put_file(p, "main");
	swprintf(s, MAX_PATH + 100, L"%ls:alpha", p);
	put_file(s, "12345");
	swprintf(s, MAX_PATH + 100, L"%ls:beta", p);
	put_file(s, "12345678901");
	wf_ntfsfacts_read(p, &f);
	CHECK(WF_NF_OK == f.state);
	if (0 == strcmp(f.fs_name, "NTFS")) {
		CHECK(f.streams_known);
		CHECK(2 == f.n_streams);
		a = stream_named(&f, "alpha");
		b = stream_named(&f, "beta");
		CHECK(a && 5 == a->size);
		CHECK(b && 11 == b->size);
		CHECK(!stream_named(&f, ":$DATA") && !stream_named(&f, ""));
	} else {
		skip("alternate streams: the temporary folder is not on NTFS");
	}

	/* more than are kept: all counted, the first ones kept */
	swprintf(p, MAX_PATH + 80, L"%ls\\many.txt", root);
	put_file(p, "m");
	for (int i = 0; i < 9; i++) {
		swprintf(s, MAX_PATH + 100, L"%ls:s%d", p, i);
		put_file(s, "x");
	}
	wf_ntfsfacts_read(p, &f);
	if (0 == strcmp(f.fs_name, "NTFS")) {
		WfInfo in;

		CHECK(9 == f.n_streams);
		build(&in, &f);
		CHECK(has_line(&in, "and 3 more"));
	}

	/* a stream name that is not a plain name is sanitized before it is drawn */
	swprintf(p, MAX_PATH + 80, L"%ls\\jp.txt", root);
	put_file(p, "j");
	swprintf(s, MAX_PATH + 100, L"%ls:\u65e5\u672c\u8a9e", p);
	put_file(s, "x");
	wf_ntfsfacts_read(p, &f);
	if (0 == strcmp(f.fs_name, "NTFS")) {
		CHECK(1 == f.n_streams);
		CHECK(stream_named(&f, "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"));
	}
}

static void hard_link(void)
{
	wchar_t a[MAX_PATH + 80], b[MAX_PATH + 80];
	WfNtfsFacts f;

	swprintf(a, MAX_PATH + 80, L"%ls\\one.txt", root);
	swprintf(b, MAX_PATH + 80, L"%ls\\two.txt", root);
	put_file(a, "hl");
	if (!CreateHardLinkW(b, a, NULL)) {
		skip("hard link: the volume cannot make one");
		return;
	}
	hard_link_made = true;
	wf_ntfsfacts_read(a, &f);
	CHECK(f.links_known && 2 == f.links);
	wf_ntfsfacts_read(b, &f);
	CHECK(f.links_known && 2 == f.links);
}

static void to_utf8(const wchar_t *w, char *out, size_t cap)
{
	int n = WideCharToMultiByte(
	        CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL);

	if (n <= 0) {
		out[0] = 0;
	}
}

static bool make_junction(const wchar_t *link, const wchar_t *target)
{
	BYTE buf[4096];
	wchar_t sub[MAX_PATH + 8];
	HANDLE h;
	DWORD got;
	USHORT sub_bytes, prn_bytes, data;
	size_t at = 16;

	swprintf(sub, MAX_PATH + 8, L"\\??\\%ls", target);
	sub_bytes = (USHORT)(wcslen(sub) * 2);
	prn_bytes = (USHORT)(wcslen(target) * 2);
	if (!CreateDirectoryW(link, NULL)) {
		return false;
	}
	h = CreateFileW(link, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	memset(buf, 0, sizeof buf);
	data = (USHORT)(8 + sub_bytes + 2 + prn_bytes + 2);
	*(ULONG *)buf = IO_REPARSE_TAG_MOUNT_POINT;
	*(USHORT *)(buf + 4) = data;
	*(USHORT *)(buf + 8) = 0;
	*(USHORT *)(buf + 10) = sub_bytes;
	*(USHORT *)(buf + 12) = (USHORT)(sub_bytes + 2);
	*(USHORT *)(buf + 14) = prn_bytes;
	memcpy(buf + at, sub, sub_bytes);
	memcpy(buf + at + sub_bytes + 2, target, prn_bytes);
	got = 0;
	if (!DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf,
	            (DWORD)(8u + data), NULL, 0, &got, NULL)) {
		CloseHandle(h);
		return false;
	}
	CloseHandle(h);
	return true;
}

static void links(void)
{
	wchar_t target[MAX_PATH + 80], junc[MAX_PATH + 80], sym[MAX_PATH + 80],
	        file[MAX_PATH + 80], fsym[MAX_PATH + 80];
	char want[MAX_PATH * 3];
	WfNtfsFacts f;

	swprintf(target, MAX_PATH + 80, L"%ls\\jtarget", root);
	CreateDirectoryW(target, NULL);
	swprintf(junc, MAX_PATH + 80, L"%ls\\junc", root);
	if (make_junction(junc, target)) {
		wf_ntfsfacts_read(junc, &f);
		CHECK(WF_NF_OK == f.state);
		CHECK(f.attributes & FILE_ATTRIBUTE_REPARSE_POINT);
		CHECK(f.target_known);
		to_utf8(target, want, sizeof want);
		CHECK(!strcmp(f.target, want));
	} else {
		skip("junction: could not be made");
	}

	swprintf(file, MAX_PATH + 80, L"%ls\\sfile.txt", root);
	put_file(file, "s");
	swprintf(sym, MAX_PATH + 80, L"%ls\\dsym", root);
	swprintf(fsym, MAX_PATH + 80, L"%ls\\fsym.txt", root);
	if (CreateSymbolicLinkW(fsym, L"sfile.txt",
	            SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
		wf_ntfsfacts_read(fsym, &f);
		CHECK(WF_NF_OK == f.state);
		CHECK(f.attributes & FILE_ATTRIBUTE_REPARSE_POINT);
		CHECK(f.target_known);
		CHECK(!strcmp(f.target, "sfile.txt")); /* relative, as made */
		/* the link is looked at, not followed: its own name count is 1 */
		CHECK(f.links_known && 1 == f.links);
	} else {
		skip("symbolic link: the account may not create one");
	}
	(void)sym;
}

static void storage_kinds(void)
{
	wchar_t p[MAX_PATH + 80];
	HANDLE h;
	DWORD got;
	USHORT comp = COMPRESSION_FORMAT_DEFAULT;
	WfNtfsFacts f;

	swprintf(p, MAX_PATH + 80, L"%ls\\comp.txt", root);
	put_file(p, "compress me compress me compress me");
	h = CreateFileW(p, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE != h &&
	        DeviceIoControl(h, FSCTL_SET_COMPRESSION, &comp, sizeof comp,
	                NULL, 0, &got, NULL)) {
		CloseHandle(h);
		wf_ntfsfacts_read(p, &f);
		CHECK(f.attributes & FILE_ATTRIBUTE_COMPRESSED);
	} else {
		if (INVALID_HANDLE_VALUE != h) {
			CloseHandle(h);
		}
		skip("compression: the volume cannot compress");
	}

	swprintf(p, MAX_PATH + 80, L"%ls\\sparse.bin", root);
	put_file(p, "s");
	h = CreateFileW(p, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE != h &&
	        DeviceIoControl(
	                h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &got, NULL)) {
		CloseHandle(h);
		wf_ntfsfacts_read(p, &f);
		CHECK(f.attributes & FILE_ATTRIBUTE_SPARSE_FILE);
	} else {
		if (INVALID_HANDLE_VALUE != h) {
			CloseHandle(h);
		}
		skip("sparse: the volume cannot make a sparse file");
	}
}

static void refusals_and_errors(void)
{
	WfNtfsFacts f;
	wchar_t p[MAX_PATH + 80];

	swprintf(p, MAX_PATH + 80, L"%ls\\missing.txt", root);
	wf_ntfsfacts_read(p, &f);
	CHECK(WF_NF_FAILED == f.state);
	CHECK(ERROR_FILE_NOT_FOUND == f.error);
	CHECK(!f.links_known && !f.streams_known && !f.volume_known);

	wf_ntfsfacts_read(L"\\\\server\\share\\x.txt", &f);
	CHECK(WF_NF_REMOTE == f.state);
	wf_ntfsfacts_read(L"\\\\?\\UNC\\server\\share\\x.txt", &f);
	CHECK(WF_NF_REMOTE == f.state);
	wf_ntfsfacts_read(L"\\\\wsl$\\Ubuntu\\home", &f);
	CHECK(WF_NF_REMOTE == f.state);
	wf_ntfsfacts_read(L"relative\\x.txt", &f);
	CHECK(WF_NF_REMOTE == f.state); /* not an absolute path: fail closed */
	wf_ntfsfacts_read(NULL, &f);
	CHECK(WF_NF_REMOTE == f.state);
	CHECK(wf_ntfsfacts_remote(L"\\\\server\\share"));
	CHECK(!wf_ntfsfacts_remote(root));
	{
		wchar_t lp[MAX_PATH + 90];

		swprintf(lp, MAX_PATH + 90, L"\\\\?\\%ls", root);
		CHECK(!wf_ntfsfacts_remote(lp));
	}
}

/* A file another program holds open with no sharing at all is still read: only
 * attributes are asked for, and they never conflict with a sharing mode. */
static void a_locked_file_is_read(void)
{
	wchar_t p[MAX_PATH + 80];
	HANDLE h;
	WfNtfsFacts f;

	swprintf(p, MAX_PATH + 80, L"%ls\\locked.txt", root);
	put_file(p, "locked");
	h = CreateFileW(p, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	wf_ntfsfacts_read(p, &f);
	CHECK(WF_NF_OK == f.state);
	CHECK(f.links_known && 1 == f.links);
	if (INVALID_HANDLE_VALUE != h) {
		CloseHandle(h);
	}
}

typedef struct
{
	WfApp *app;
} RealPort;

/* A listing of the real folder, answered at once. */
static WfStatus real_submit(
        void *ctx, uint32_t gen, const wchar_t *path, const wchar_t *parent)
{
	RealPort *rp = ctx;
	WfDir *d = wf_dir_new();
	WIN32_FIND_DATAW fd;
	wchar_t pat[MAX_PATH + 50];
	HANDLE h;

	(void)parent;
	swprintf(pat, MAX_PATH + 50, L"%ls\\*", path);
	h = FindFirstFileW(pat, &fd);
	if (!d || INVALID_HANDLE_VALUE == h) {
		wf_dir_free(d);
		wf_app_on_listing(rp->app, gen, WF_E_NOENT, NULL, NULL);
		return WF_OK;
	}
	do {
		if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) {
			(void)wf_dir_add(d, fd.cFileName, wcslen(fd.cFileName),
			        (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			                ? WF_ENT_DIR
			                : 0u,
			        fd.nFileSizeLow, 0);
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	(void)wf_dir_sort(d, WF_SORT_NAME, 0);
	wf_app_on_listing(rp->app, gen, WF_OK, d, NULL);
	return WF_OK;
}

/* Puts the cursor on `name` (j presses) and opens the details window. */
static bool open_info_on(WfApp *a, const wchar_t *name)
{
	WfEvent j = {WF_EV_CHAR, 'j', WF_KEY_NONE, 0};
	WfEvent i = {WF_EV_CHAR, 'i', WF_KEY_NONE, 0};

	for (int k = 0; k < 64; k++) {
		const WfEntry *e = wf_view_current(&a->cur);

		if (e && !wcscmp(e->name, name)) {
			(void)a->mode->on_event(a, &i);
			return &WF_MODE_INFO == a->mode;
		}
		(void)a->mode->on_event(a, &j);
	}
	return false;
}

static void info_in_the_application(void)
{
	WfApp a;
	RealPort rp = {&a};
	WfFsPort port = {&rp, real_submit};
	WfEvent esc = {WF_EV_KEY, 0, WF_KEY_ESC, 0};
	bool ntfs;

	CHECK(WF_OK == wf_app_init(&a, 100, 40, port, root));
	CHECK(&WF_MODE_NORMAL == a.mode);

	CHECK(open_info_on(&a, L"ads.txt"));
	ntfs = has_line(&a.info, "NTFS  serial");
	if (ntfs) {
		CHECK(has_line(&a.info, "alpha  5 bytes"));
		CHECK(has_line(&a.info, "beta  11 bytes"));
	} else {
		skip("application: the temporary folder is not on NTFS");
	}
	CHECK(has_line(&a.info, "Names:     1 (no other hard link)"));
	CHECK(has_line(&a.info, "Stored:    ordinary"));
	(void)a.mode->on_event(&a, &esc);

	/* a hard link shows the count; a missing file is not reached */
	CHECK(open_info_on(&a, L"one.txt"));
	if (hard_link_made) {
		CHECK(has_line(&a.info, "2 (this one and 1 other hard links)"));
	}
	(void)a.mode->on_event(&a, &esc);
	wf_app_destroy(&a);
}

/* Removes what the run made (the folder is flat except two empty folders). */
static void cleanup(void)
{
	WIN32_FIND_DATAW fd;
	wchar_t pat[MAX_PATH + 50], p[MAX_PATH + 300];
	HANDLE h;

	swprintf(pat, MAX_PATH + 50, L"%ls\\*", root);
	h = FindFirstFileW(pat, &fd);
	if (INVALID_HANDLE_VALUE != h) {
		do {
			if (!wcscmp(fd.cFileName, L".") ||
			        !wcscmp(fd.cFileName, L"..")) {
				continue;
			}
			swprintf(p, MAX_PATH + 300, L"%ls\\%ls", root,
			        fd.cFileName);
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
				RemoveDirectoryW(
				        p); /* a junction too: the link only */
			} else {
				DeleteFileW(p);
			}
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	RemoveDirectoryW(root);
}

/* A process id is reused soon, and a run that was killed leaves its folder
 * behind. A folder named by the id alone then already holds "junc" and
 * "one.txt", the junction and the hard link fail, and the test skips them: a
 * mutation in the code they test goes unnoticed. So the folder is made fresh,
 * and never taken over. */
static wchar_t stale[MAX_PATH + 40];

static void stale_root(const wchar_t *tmp)
{
	wchar_t p[MAX_PATH + 80];

	swprintf(stale, MAX_PATH + 40, L"%lsntfsfacts_%lu", tmp,
	        (unsigned long)GetCurrentProcessId());
	CreateDirectoryW(stale, NULL);
	swprintf(p, MAX_PATH + 80, L"%ls\\junc", stale);
	CreateDirectoryW(p, NULL);
	swprintf(p, MAX_PATH + 80, L"%ls\\one.txt", stale);
	put_file(p, "stale");
}

static void make_root(const wchar_t *tmp)
{
	for (unsigned n = 0; n < 1000; n++) {
		swprintf(root, MAX_PATH + 40, L"%lsntfsfacts_%lu_%llu_%u", tmp,
		        (unsigned long)GetCurrentProcessId(),
		        (unsigned long long)GetTickCount64(), n);
		if (CreateDirectoryW(root, NULL)) {
			break;
		}
	}
	CHECK(0 != wcscmp(root, stale));
}

static void drop_stale(void)
{
	wchar_t p[MAX_PATH + 80];

	swprintf(p, MAX_PATH + 80, L"%ls\\junc", stale);
	RemoveDirectoryW(p);
	swprintf(p, MAX_PATH + 80, L"%ls\\one.txt", stale);
	DeleteFileW(p);
	RemoveDirectoryW(stale);
}

int main(void)
{
	wchar_t tmp[MAX_PATH];

	lines_without_facts();
	lines_remote_and_failed();
	lines_full();

	GetTempPathW(MAX_PATH, tmp);
	stale_root(tmp);
	make_root(tmp);
	plain_file_and_folder();
	alternate_streams();
	hard_link();
	links();
	storage_kinds();
	refusals_and_errors();
	a_locked_file_is_read();
	info_in_the_application();
	cleanup();
	drop_stale();

	if (wf_test_failures) {
		printf("test_ntfsfacts: %d failure(s)\n", wf_test_failures);
		return 1;
	}
	printf("test_ntfsfacts: all checks passed (%d skipped)\n", skips);
	return 0;
}
