/*
 * The copy engine on the real file system (Windows only), in a scratch area.
 * What this pins down: copies are exact (every file compared by hash), keep
 * what a copy can keep and say what it cannot; nothing at the destination is
 * lost or merged; cancelling and every kind of failure leave no temporary;
 * and a killed process leaves only temporaries that the journal lets the next
 * start remove, and only when they are provably its own.
 */
#include <windows.h>

#include <shellapi.h>
#include <stdio.h>
#include <string.h>

#include "../tools/p0/p0_common.h"
#include "../tools/p0/p0_lab.h"
#include "fs/owned_delete.h"
#include "wf_journal_test.h"
#include "wf_test.h"
#include "fs/wf_weakid_win.h"
#include "xfer/wf_copyengine_win.h"
#include "xfer/wf_probe_win.h"
#include "xfer/wf_recycle_win.h"
#include "base/wf_macro.h"

static P0Lab lab;
static wchar_t root[1024], src_dir[1024], dst_dir[1024], journal_path[1100];
static WfProbeOps ops;
static wchar_t g_exe_w[1024];

static bool put(const wchar_t *path, const void *data, size_t n)
{
	HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD w = 0;
	bool ok;

	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	ok = !n || (WriteFile(h, data, (DWORD)n, &w, NULL) && w == n);
	CloseHandle(h);
	return ok;
}

static void make_dir(const wchar_t *rel)
{
	wchar_t path[1100];

	CHECK(p0_area_mkdir(lab.area, rel));
	(void)path;
}

static void path_in(const wchar_t *base, const wchar_t *rel, wchar_t *out)
{
	swprintf(out, 1100, L"%ls\\%ls", base, rel);
}

/* A file of `n` bytes of a repeatable pattern. */
static bool make_pattern(const wchar_t *path, size_t n, unsigned seed)
{
	unsigned char *buf = malloc(n ? n : 1);
	size_t i;
	bool ok;

	for (i = 0; i < n; i++) {
		buf[i] = (unsigned char)((i * 131 + seed + (i >> 8)) & 0xFF);
	}
	ok = put(path, buf, n);
	free(buf);
	return ok;
}

static bool hash_of(const wchar_t *path, char hex[P0_HASH_HEX])
{
	return 0 != p0_sha256_file(path, hex);
}

static bool same_file_content(const wchar_t *a, const wchar_t *b)
{
	char ha[P0_HASH_HEX], hb[P0_HASH_HEX];

	return hash_of(a, ha) && hash_of(b, hb) && !strcmp(ha, hb);
}

static bool exists(const wchar_t *path)
{
	return INVALID_FILE_ATTRIBUTES != GetFileAttributesW(path);
}

/* Temporaries the engine would have made, in `dir`. */
static size_t temporaries(const wchar_t *dir)
{
	wchar_t pattern[1100];
	WIN32_FIND_DATAW fd;
	HANDLE f;
	size_t n = 0;

	swprintf(pattern, 1100, L"%ls\\.wfcopy-*", dir);
	f = FindFirstFileW(pattern, &fd);
	if (INVALID_HANDLE_VALUE == f) {
		return 0;
	}
	do {
		n++;
	} while (FindNextFileW(f, &fd));
	FindClose(f);
	return n;
}

/* The test's own clean-up of what it left on purpose: the temporaries in `dir`. */
static void remove_temporaries(const wchar_t *dir)
{
	wchar_t pattern[1100], file[1100];
	WIN32_FIND_DATAW fd;
	HANDLE f;

	swprintf(pattern, 1100, L"%ls\\.wfcopy-*", dir);
	f = FindFirstFileW(pattern, &fd);
	if (INVALID_HANDLE_VALUE == f) {
		return;
	}
	do {
		swprintf(file, 1100, L"%ls\\%ls", dir, fd.cFileName);
		SetFileAttributesW(file, FILE_ATTRIBUTE_NORMAL);
		DeleteFileW(file);
	} while (FindNextFileW(f, &fd));
	FindClose(f);
}

static WfCopySpec spec_for(const wchar_t *const *names,
        size_t n,
        WfCollide collide,
        WfCopyVerify verify,
        WfCopyKind kind)
{
	WfCopySpec s;
	WfNodeInfo d;

	memset(&s, 0, sizeof s);
	wf_slot_init(&s.slot);
	CHECK(wf_xfer_capture(&ops,
	              WF_CJ_COPY == kind ? WF_XFER_COPY : WF_XFER_MOVE, src_dir,
	              names, n, 0, &s.slot) == WF_OK);
	s.dest = _wcsdup(dst_dir);
	CHECK(WF_OK == ops.node(NULL, dst_dir, &d));
	s.dest_id = d.id;
	s.kind = kind;
	s.verify = verify;
	s.collide = collide;
	return s;
}

static WfCopyResult run(
        WfCopySpec *s, const WfCopyControl *ctl, WfCopyJournal *j)
{
	WfCopyResult r;

	size_t i;

	CHECK(WF_OK == wf_copyengine_run(s, ctl, j, &r));
	for (i = 0; i < r.failure_total && i < WF_CJ_FAILURES; i++) {
		printf("  failure: %ls: %s (%lu)\n", r.failures[i].name,
		        wf_copyfail_text(r.failures[i].kind),
		        r.failures[i].error);
	}
	return r;
}

static void clean_up(void);

static void fresh_dirs(void)
{
	/* the source and destination are emptied by using new subfolders per test */
	static unsigned n;
	wchar_t rel[64];

	clean_up(); /* the previous scenario's folders */
	n++;
	swprintf(rel, 64, L"s%u", n);
	make_dir(rel);
	swprintf(src_dir, 1024, L"%ls\\%ls", root, rel);
	swprintf(rel, 64, L"d%u", n);
	make_dir(rel);
	swprintf(dst_dir, 1024, L"%ls\\%ls", root, rel);
}

/* Removes everything below `dir` (not `dir` itself) for a scratch folder of
 * this test, never leaving `root` and never entering a link. */
static void purge_contents(const wchar_t *dir)
{
	wchar_t pattern[1100], child[1100];
	WIN32_FIND_DATAW fd;
	HANDLE f;
	size_t rl = wcslen(root);

	if (0 != wcsncmp(dir, root, rl) || L'\\' != dir[rl]) {
		return; /* not ours: touch nothing */
	}
	swprintf(pattern, 1100, L"%ls\\*", dir);
	f = FindFirstFileW(pattern, &fd);
	if (INVALID_HANDLE_VALUE == f) {
		return;
	}
	do {
		if (!wcscmp(fd.cFileName, L".") ||
		        !wcscmp(fd.cFileName, L"..")) {
			continue;
		}
		swprintf(child, 1100, L"%ls\\%ls", dir, fd.cFileName);
		SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			if (!(fd.dwFileAttributes &
			            FILE_ATTRIBUTE_REPARSE_POINT)) {
				purge_contents(child);
			}
			RemoveDirectoryW(child);
		} else {
			DeleteFileW(child);
		}
	} while (FindNextFileW(f, &fd));
	FindClose(f);
}

/* Empties the scratch folders the last fresh_dirs() made. */
static void clean_up(void)
{
	purge_contents(src_dir);
	purge_contents(dst_dir);
}

static void a_file_is_copied_exactly_with_what_a_copy_can_keep(void)
{
	wchar_t a[1100], b[1100], ads[1200], ads_b[1200];
	const wchar_t *names[] = {L"doc.bin"};
	WfCopySpec spec;
	WfCopyResult r;
	FILETIME ta, tb;
	HANDLE h;
	char text[64];
	DWORD got;

	fresh_dirs();
	path_in(src_dir, L"doc.bin", a);
	path_in(dst_dir, L"doc.bin", b);
	CHECK(make_pattern(a, 3u * 1024 * 1024 + 17, 7));
	CHECK(SetFileAttributesW(
	        a, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE));
	swprintf(ads, 1200, L"%ls:Zone.Identifier", a);
	CHECK(put(ads, "[ZoneTransfer]\r\nZoneId=3\r\n", 26));
	{
		FILETIME old = {
		        0xD53E8000, 0x01D7A000}; /* some time in the past */

		h = CreateFileW(a, FILE_WRITE_ATTRIBUTES,
		        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
		        0, NULL);
		CHECK(SetFileTime(h, &old, NULL, &old));
		CloseHandle(h);
	}
	spec = spec_for(
	        names, 1, WF_COLLIDE_STOP, WF_VERIFY_SHA256, WF_CJ_COPY);
	r = run(&spec, NULL, NULL);
	CHECK(WF_CS_DONE == r.state && 1 == r.files_copied &&
	        0 == r.failure_total);
	CHECK(r.bytes == 3u * 1024 * 1024 + 17 && 1 == r.streams_kept);
	CHECK(same_file_content(a, b));
	CHECK(0 != (GetFileAttributesW(b) & FILE_ATTRIBUTE_HIDDEN));
	swprintf(ads_b, 1200, L"%ls:Zone.Identifier", b);
	h = CreateFileW(ads_b, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, 0, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	got = 0;
	if (INVALID_HANDLE_VALUE != h) {
		ReadFile(h, text, sizeof text - 1, &got, NULL);
		CloseHandle(h);
	}
	CHECK(26 == got && !memcmp(text, "[ZoneTransfer]", 14));
	h = CreateFileW(a, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0,
	        NULL);
	CHECK(GetFileTime(h, NULL, NULL, &ta));
	CloseHandle(h);
	h = CreateFileW(b, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0,
	        NULL);
	CHECK(GetFileTime(h, NULL, NULL, &tb));
	CloseHandle(h);
	CHECK(0 ==
	        CompareFileTime(&ta, &tb)); /* the modification time is kept */
	CHECK(0 == temporaries(dst_dir));
	CHECK(1 == r.created_n && !r.created[0].dir &&
	        (r.created[0].id.kind == WF_ID_STRONG));
	wf_copyresult_free(&r);
	wf_copyspec_free(&spec);
}

/* A weak identity (a FAT volume) may take part in a plain copy that leaves what
 * exists as it is. Not in a move, not in a replacement on it, not in a folder
 * copied onto it (built under a hidden name and renamed, which a weak identity
 * does not survive): each is refused before anything is touched. A weak
 * identity that the object does not have is the object having changed. */
static void weak_identities_take_part_in_a_plain_copy_only(void)
{
	wchar_t a[1100], b[1100], tree_rel[200], inner[1100];
	const wchar_t *file[] = {L"w.bin"}, *tree[] = {L"wtree"};
	WfCopySpec spec;
	WfCopyResult r;

	fresh_dirs();
	path_in(src_dir, L"w.bin", a);
	path_in(dst_dir, L"w.bin", b);
	CHECK(make_pattern(a, 1000, 3));
	swprintf(tree_rel, 200, L"%ls\\wtree", src_dir + wcslen(root) + 1);
	make_dir(tree_rel);
	path_in(src_dir, L"wtree\\in.bin", inner);
	CHECK(make_pattern(inner, 10, 4));

	/* a move whose source has only a weak identity */
	spec = spec_for(
	        file, 1, WF_COLLIDE_STOP, WF_VERIFY_SHA256, WF_CJ_MOVE_ACROSS);
	wf_weak_fill(&spec.slot.src[0].snap.id, spec.dest_id.volume, 77);
	CHECK(WF_E_ACCESS == wf_copyengine_run(&spec, NULL, NULL, &r));
	CHECK(0 == r.files_copied &&
	        GetFileAttributesW(a) != INVALID_FILE_ATTRIBUTES);
	CHECK(GetFileAttributesW(b) == INVALID_FILE_ATTRIBUTES);
	wf_copyresult_free(&r);
	wf_copyspec_free(&spec);

	/* a replacement on a weak destination */
	spec = spec_for(
	        file, 1, WF_COLLIDE_OVERWRITE, WF_VERIFY_SHA256, WF_CJ_COPY);
	wf_weak_fill(&spec.dest_id, spec.dest_id.volume, 78);
	CHECK(WF_E_ACCESS == wf_copyengine_run(&spec, NULL, NULL, &r));
	CHECK(0 == r.files_copied &&
	        GetFileAttributesW(b) == INVALID_FILE_ATTRIBUTES);
	wf_copyresult_free(&r);
	wf_copyspec_free(&spec);

	/* a folder onto a weak destination: refused, and no hidden staging left */
	spec = spec_for(tree, 1, WF_COLLIDE_STOP, WF_VERIFY_SHA256, WF_CJ_COPY);
	wf_weak_fill(&spec.dest_id, spec.dest_id.volume, 78);
	CHECK(WF_E_ACCESS == wf_copyengine_run(&spec, NULL, NULL, &r));
	CHECK(0 == r.files_copied && 0 == temporaries(dst_dir));
	path_in(dst_dir, L"wtree", b);
	CHECK(GetFileAttributesW(b) == INVALID_FILE_ATTRIBUTES);
	wf_copyresult_free(&r);
	wf_copyspec_free(&spec);

	/* a plain copy is not refused by that rule, but a weak identity that the
	 * (proven) folder does not have is a folder that is not the one listed */
	path_in(dst_dir, L"w.bin", b);
	spec = spec_for(file, 1, WF_COLLIDE_STOP, WF_VERIFY_SHA256, WF_CJ_COPY);
	wf_weak_fill(&spec.dest_id, spec.dest_id.volume, 78);
	CHECK(WF_OK == wf_copyengine_run(&spec, NULL, NULL, &r));
	CHECK(0 == r.files_copied && r.files_failed >= 1);
	CHECK(GetFileAttributesW(b) == INVALID_FILE_ATTRIBUTES);
	wf_copyresult_free(&r);
	wf_copyspec_free(&spec);

	/* and a source whose weak identity is not the file's is not copied */
	spec = spec_for(file, 1, WF_COLLIDE_STOP, WF_VERIFY_SHA256, WF_CJ_COPY);
	wf_weak_fill(&spec.slot.src[0].snap.id, spec.dest_id.volume, 77);
	CHECK(WF_OK == wf_copyengine_run(&spec, NULL, NULL, &r));
	CHECK(0 == r.files_copied && r.files_failed >= 1);
	CHECK(GetFileAttributesW(b) == INVALID_FILE_ATTRIBUTES &&
	        0 == temporaries(dst_dir));
	wf_copyresult_free(&r);
	wf_copyspec_free(&spec);
}

static void a_tree_is_copied_with_every_file_compared(void)
{
	wchar_t p[1100], q[1100];
	const wchar_t *names[] = {L"tree"};
	static const wchar_t uni[] = {
	        0x65E5, 0x672C, 0xD83D, 0xDE00, L'.', L't', L'x', L't', 0};
	WfCopySpec spec;
	WfCopyResult r;
	size_t i;
	const wchar_t *files[] = {
	        L"a.txt", L"sub\\b.bin", L"sub\\deeper\\c.bin"};

	fresh_dirs();
	path_in(src_dir, L"tree", p);
	CHECK(CreateDirectoryW(p, NULL));
	path_in(src_dir, L"tree\\sub", p);
	CHECK(CreateDirectoryW(p, NULL));
	path_in(src_dir, L"tree\\sub\\deeper", p);
	CHECK(CreateDirectoryW(p, NULL));
	path_in(src_dir, L"tree\\empty", p);
	CHECK(CreateDirectoryW(p, NULL));
	for (i = 0; i < 3; i++) {
		swprintf(q, 1100, L"%ls\\tree\\%ls", src_dir, files[i]);
		CHECK(make_pattern(q, 1000 + i * 70000, (unsigned)i));
	}
	swprintf(q, 1100, L"%ls\\tree\\%ls", src_dir, uni);
	CHECK(put(q, "unicode", 7));
	spec = spec_for(
	        names, 1, WF_COLLIDE_STOP, WF_VERIFY_SHA256, WF_CJ_COPY);
	r = run(&spec, NULL, NULL);
	CHECK(WF_CS_DONE == r.state && 4 == r.files_copied &&
	        4 == r.dirs_created && 0 == r.failure_total);
	for (i = 0; i < 3; i++) {
		wchar_t a[1100], b[1100];

		swprintf(a, 1100, L"%ls\\tree\\%ls", src_dir, files[i]);
		swprintf(b, 1100, L"%ls\\tree\\%ls", dst_dir, files[i]);
		CHECK(same_file_content(a, b));
	}
	swprintf(q, 1100, L"%ls\\tree\\empty", dst_dir);
	CHECK(exists(q)); /* empty folders come across */
	swprintf(q, 1100, L"%ls\\tree\\%ls", dst_dir, uni);
	CHECK(exists(q));
	CHECK(1 == r.created_n && r.created[0].dir);
	CHECK(0 == temporaries(dst_dir));
	wf_copyresult_free(&r);
	wf_copyspec_free(&spec);
}

static void nothing_at_the_destination_is_lost_or_merged(void)
{
	wchar_t a[1100], b[1100], dir_a[1100], dir_b[1100], inner[1100];
	const wchar_t *names[] = {L"f.txt", L"folder"};
	WfCopySpec spec;
	WfCopyResult r;

	fresh_dirs();
	path_in(src_dir, L"f.txt", a);
	path_in(dst_dir, L"f.txt", b);
	CHECK(put(a, "new content", 11));
	CHECK(put(b, "OLD", 3));
	path_in(src_dir, L"folder", dir_a);
	path_in(dst_dir, L"folder", dir_b);
	CHECK(CreateDirectoryW(dir_a, NULL) && CreateDirectoryW(dir_b, NULL));
	swprintf(inner, 1100, L"%ls\\x.txt", dir_a);
	CHECK(put(inner, "src", 3));
	swprintf(inner, 1100, L"%ls\\keep.txt", dir_b);
	CHECK(put(inner, "dest", 4));

	/* STOP: both names are taken; both destinations are untouched */
	spec = spec_for(
	        names, 2, WF_COLLIDE_STOP, WF_VERIFY_SHA256, WF_CJ_COPY);
	r = run(&spec, NULL, NULL);
	CHECK(0 == r.files_copied && 2 == r.failure_total &&
	        WF_CF_EXISTS == r.failures[0].kind);
	{
		char text[16] = "";
		HANDLE h = CreateFileW(b, GENERIC_READ, FILE_SHARE_READ, NULL,
		        OPEN_EXISTING, 0, NULL);
		DWORD got = 0;

		ReadFile(h, text, 15, &got, NULL);
		CloseHandle(h);
		CHECK(3 == got && !memcmp(text, "OLD", 3));
	}
	wf_copyresult_free(&r);

	/* SKIP: left alone, counted */
	spec.collide = WF_COLLIDE_SKIP;
	r = run(&spec, NULL, NULL);
	CHECK(0 == r.files_copied && 2 == r.files_skipped &&
	        0 == r.failure_total);
	wf_copyresult_free(&r);

	/* OVERWRITE: the file is replaced after a verified copy; the folder is
	 * still never merged */
	spec.collide = WF_COLLIDE_OVERWRITE;
	r = run(&spec, NULL, NULL);
	CHECK(1 == r.files_copied && 1 == r.overwritten &&
	        1 == r.failure_total);
	CHECK(WF_CF_EXISTS == r.failures[0].kind);
	/* a file that replaced another is not offered for Undo: the old one is
	 * gone, so Undo could not put it back */
	CHECK(0 == r.created_n);
	CHECK(same_file_content(a, b));
	swprintf(inner, 1100, L"%ls\\keep.txt", dir_b);
	CHECK(exists(inner));
	swprintf(inner, 1100, L"%ls\\x.txt", dir_b);
	CHECK(!exists(inner));
	CHECK(0 == temporaries(dst_dir));
	wf_copyresult_free(&r);

	/* a read-only file is not replaced */
	CHECK(put(a, "third", 5));
	CHECK(SetFileAttributesW(b, FILE_ATTRIBUTE_READONLY));
	wf_copyspec_free(&spec);
	spec = spec_for(
	        names, 1, WF_COLLIDE_OVERWRITE, WF_VERIFY_SHA256, WF_CJ_COPY);
	r = run(&spec, NULL, NULL);
	CHECK(0 == r.files_copied && WF_CF_DEST_UNSAFE == r.failures[0].kind);
	CHECK(!same_file_content(a, b));
	CHECK(SetFileAttributesW(b, FILE_ATTRIBUTE_NORMAL));
	wf_copyresult_free(&r);
	wf_copyspec_free(&spec);
}

/* A folder that is already at the destination is entered, not refused, skipped
 * or renamed, when the job says so (a copy, under skip, keep both or
 * overwrite): the files in it follow the policy name by name, what was there is
 * kept, and only what the job made is registered for Undo. */
static void build_merge_case(wchar_t *dst_assets)
{
	wchar_t p[1100];

	fresh_dirs();
	path_in(src_dir, L"assets", p);
	CHECK(CreateDirectoryW(p, NULL));
	path_in(src_dir, L"assets\\sub", p);
	CHECK(CreateDirectoryW(p, NULL));
	path_in(src_dir, L"assets\\a.png", p);
	CHECK(put(p, "new a", 5));
	path_in(src_dir, L"assets\\b.png", p);
	CHECK(put(p, "new b", 5));
	path_in(src_dir, L"assets\\sub\\x.txt", p);
	CHECK(put(p, "x", 1));
	path_in(dst_dir, L"assets", dst_assets);
	CHECK(CreateDirectoryW(dst_assets, NULL));
	path_in(dst_dir, L"assets\\sub", p);
	CHECK(CreateDirectoryW(p, NULL));
	path_in(dst_dir, L"assets\\a.png", p);
	CHECK(put(p, "OLD a", 5));
	path_in(dst_dir, L"assets\\c.png", p);
	CHECK(put(p, "keep c", 6));
	path_in(dst_dir, L"assets\\sub\\y.txt", p);
	CHECK(put(p, "keep y", 6));
}

static bool holds(const wchar_t *path, const char *text)
{
	char got[32] = "";
	DWORD n = 0;
	HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, 0, NULL);

	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	ReadFile(h, got, 31, &n, NULL);
	CloseHandle(h);
	return n == strlen(text) && !memcmp(got, text, n);
}

static void a_folder_that_is_there_is_merged_and_files_follow_the_policy(void)
{
	const wchar_t *names[] = {L"assets"};
	wchar_t dst_assets[1100], p[1100];
	WfCopySpec spec;
	WfCopyResult r;

	/* skip: the same-named file stays, the rest comes in, nothing is lost */
	build_merge_case(dst_assets);
	spec = spec_for(
	        names, 1, WF_COLLIDE_SKIP, WF_VERIFY_SHA256, WF_CJ_COPY);
	spec.merge_folders = true;
	r = run(&spec, NULL, NULL);
	CHECK(0 == r.failure_total && 2 == r.files_copied &&
	        1 == r.files_skipped && 0 == r.dirs_created);
	path_in(dst_dir, L"assets\\a.png", p);
	CHECK(holds(p, "OLD a"));
	path_in(dst_dir, L"assets\\b.png", p);
	CHECK(holds(p, "new b"));
	path_in(dst_dir, L"assets\\c.png", p);
	CHECK(holds(p, "keep c"));
	path_in(dst_dir, L"assets\\sub\\x.txt", p);
	CHECK(holds(p, "x"));
	path_in(dst_dir, L"assets\\sub\\y.txt", p);
	CHECK(holds(p, "keep y"));
	/* only what was made is registered: b.png and sub\x.txt, not the folders
	 * that were there */
	CHECK(2 == r.created_n);
	for (size_t k = 0; k < r.created_n; k++) {
		CHECK((r.created[k].id.kind == WF_ID_STRONG) &&
		        !r.created[k].dir);
	}
	CHECK(0 == temporaries(dst_assets));
	wf_copyresult_free(&r);

	/* keep both: the same-named file gets "a (2).png" */
	build_merge_case(dst_assets);
	wf_copyspec_free(&spec);
	spec = spec_for(
	        names, 1, WF_COLLIDE_RENAME, WF_VERIFY_SHA256, WF_CJ_COPY);
	spec.merge_folders = true;
	r = run(&spec, NULL, NULL);
	CHECK(0 == r.failure_total && 3 == r.files_copied);
	path_in(dst_dir, L"assets\\a.png", p);
	CHECK(holds(p, "OLD a"));
	path_in(dst_dir, L"assets\\a (2).png", p);
	CHECK(holds(p, "new a"));
	CHECK(3 == r.created_n);
	wf_copyresult_free(&r);

	/* overwrite: the file is replaced after a verified copy, the rest merges */
	build_merge_case(dst_assets);
	wf_copyspec_free(&spec);
	spec = spec_for(
	        names, 1, WF_COLLIDE_OVERWRITE, WF_VERIFY_SHA256, WF_CJ_COPY);
	spec.merge_folders = true;
	r = run(&spec, NULL, NULL);
	CHECK(0 == r.failure_total && 3 == r.files_copied &&
	        1 == r.overwritten);
	path_in(dst_dir, L"assets\\a.png", p);
	CHECK(holds(p, "new a"));
	path_in(dst_dir, L"assets\\c.png", p);
	CHECK(holds(p, "keep c"));
	wf_copyresult_free(&r);

	/* stop: a name that is taken is a failure, folders are not entered */
	build_merge_case(dst_assets);
	wf_copyspec_free(&spec);
	spec = spec_for(
	        names, 1, WF_COLLIDE_STOP, WF_VERIFY_SHA256, WF_CJ_COPY);
	spec.merge_folders = true;
	r = run(&spec, NULL, NULL);
	CHECK(0 == r.files_copied && WF_CF_EXISTS == r.failures[0].kind);
	path_in(dst_dir, L"assets\\b.png", p);
	CHECK(!exists(p));
	wf_copyresult_free(&r);

	/* off: as before, a folder that is there is skipped whole */
	build_merge_case(dst_assets);
	wf_copyspec_free(&spec);
	spec = spec_for(
	        names, 1, WF_COLLIDE_SKIP, WF_VERIFY_SHA256, WF_CJ_COPY);
	r = run(&spec, NULL, NULL);
	CHECK(0 == r.files_copied && 1 == r.files_skipped);
	path_in(dst_dir, L"assets\\b.png", p);
	CHECK(!exists(p));
	wf_copyresult_free(&r);
	wf_copyspec_free(&spec);
}

/* The list of what a job made grows with the job: seventy items cross several
 * growths of it, and each is registered once with its own path and identity. */
static void every_item_a_job_made_is_registered_once(void)
{
	enum
	{
		COUNT = 70
	};
	wchar_t text[COUNT][16];
	const wchar_t *names[COUNT];
	WfCopySpec spec;
	WfCopyResult r;

	fresh_dirs();
	for (int i = 0; i < COUNT; i++) {
		wchar_t a[1100];

		swprintf(text[i], 16, L"m%02d.txt", i);
		names[i] = text[i];
		path_in(src_dir, text[i], a);
		CHECK(put(a, "x", 1));
	}
	spec = spec_for(
	        names, COUNT, WF_COLLIDE_RENAME, WF_VERIFY_SHA256, WF_CJ_COPY);
	r = run(&spec, NULL, NULL);
	CHECK(0 == r.failure_total && COUNT == r.created_n);
	for (size_t k = 0; k < r.created_n; k++) {
		CHECK((r.created[k].id.kind == WF_ID_STRONG) &&
		        r.created[k].path);
		for (size_t j = 0; j < k; j++) {
			CHECK(wcscmp(r.created[k].path, r.created[j].path));
		}
	}
	wf_copyresult_free(&r);
	wf_copyspec_free(&spec);
}

/* Keep both: a name that is taken gives the new one the next free name
 * ("a (2).txt"), for files and folders, and touches nothing that was there. */
static void keep_both_gives_the_new_ones_the_next_free_name(void)
{
	wchar_t a[1100], b[1100], c[1100], dir_a[1100], dir_b[1100], inner[1100];
	const wchar_t *names[] = {L"f.txt", L"folder", L".hidden", L"noext"};
	WfCopySpec spec;
	WfCopyResult r;

	fresh_dirs();
	path_in(src_dir, L"f.txt", a);
	path_in(dst_dir, L"f.txt", b);
	CHECK(put(a, "new content", 11));
	CHECK(put(b, "OLD", 3));
	path_in(src_dir, L".hidden", a);
	CHECK(put(a, "h-new", 5));
	path_in(dst_dir, L".hidden", b);
	CHECK(put(b, "h-old", 5));
	path_in(src_dir, L"noext", a);
	CHECK(put(a, "n-new", 5));
	path_in(dst_dir, L"noext", b);
	CHECK(put(b, "n-old", 5));
	path_in(src_dir, L"folder", dir_a);
	path_in(dst_dir, L"folder", dir_b);
	CHECK(CreateDirectoryW(dir_a, NULL) && CreateDirectoryW(dir_b, NULL));
	swprintf(inner, 1100, L"%ls\\x.txt", dir_a);
	CHECK(put(inner, "src", 3));
	swprintf(inner, 1100, L"%ls\\keep.txt", dir_b);
	CHECK(put(inner, "dest", 4));

	spec = spec_for(
	        names, 4, WF_COLLIDE_RENAME, WF_VERIFY_SHA256, WF_CJ_COPY);
	r = run(&spec, NULL, NULL);
	CHECK(0 == r.failure_total);
	/* each is registered with the identity it had when it was made */
	CHECK(4 == r.created_n);
	for (size_t k = 0; k < r.created_n; k++) {
		CHECK((r.created[k].id.kind == WF_ID_STRONG));
	}
	/* the new ones are beside the old, each under its next free name */
	path_in(dst_dir, L"f (2).txt", c);
	CHECK(exists(c));
	path_in(src_dir, L"f.txt", a);
	CHECK(same_file_content(a, c));
	path_in(dst_dir, L".hidden (2)", c);
	CHECK(exists(c)); /* a leading dot is part of the stem */
	path_in(dst_dir, L"noext (2)", c);
	CHECK(exists(c));
	swprintf(inner, 1100, L"%ls (2)\\x.txt", dir_b);
	CHECK(exists(inner));
	/* the old ones are exactly as they were */
	path_in(dst_dir, L"f.txt", b);
	{
		char text[16] = "";
		HANDLE h = CreateFileW(b, GENERIC_READ, FILE_SHARE_READ, NULL,
		        OPEN_EXISTING, 0, NULL);
		DWORD got = 0;

		ReadFile(h, text, 15, &got, NULL);
		CloseHandle(h);
		CHECK(3 == got && !memcmp(text, "OLD", 3));
	}
	swprintf(inner, 1100, L"%ls\\keep.txt", dir_b);
	CHECK(exists(inner));
	swprintf(inner, 1100, L"%ls\\x.txt", dir_b);
	CHECK(!exists(inner));
	CHECK(0 == temporaries(dst_dir));
	wf_copyresult_free(&r);

	/* again: (2) is taken now, so the next ones are (3) */
	r = run(&spec, NULL, NULL);
	CHECK(0 == r.failure_total);
	path_in(dst_dir, L"f (3).txt", c);
	CHECK(exists(c));
	path_in(dst_dir, L"f (2).txt", c);
	CHECK(exists(c));
	swprintf(inner, 1100, L"%ls (3)\\x.txt", dir_b);
	CHECK(exists(inner));
	wf_copyresult_free(&r);
	wf_copyspec_free(&spec);
}

/* Moves the last-write time of a file by `seconds`, as a program that rewrites
 * a file without changing its size does. */
static bool shift_mtime(const wchar_t *path, long seconds)
{
	HANDLE h =
	        CreateFileW(path, FILE_WRITE_ATTRIBUTES | FILE_READ_ATTRIBUTES,
	                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	FILETIME ft;
	ULARGE_INTEGER t;
	bool ok = false;

	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	if (GetFileTime(h, NULL, NULL, &ft)) {
		t.LowPart = ft.dwLowDateTime;
		t.HighPart = ft.dwHighDateTime;
		t.QuadPart += (ULONGLONG)seconds * 10000000ull;
		ft.dwLowDateTime = t.LowPart;
		ft.dwHighDateTime = t.HighPart;
		ok = SetFileTime(h, NULL, NULL, &ft);
	}
	CloseHandle(h);
	return ok;
}

/* The chosen file was written again after it was chosen, with the same size:
 * the size alone does not show it, the write time does. */
static void a_source_rewritten_to_the_same_size_is_not_copied(void)
{
	wchar_t a[1100], b[1100];
	const wchar_t *names[] = {L"s.txt"};
	WfCopySpec spec;
	WfCopyResult r;

	fresh_dirs();
	path_in(src_dir, L"s.txt", a);
	path_in(dst_dir, L"s.txt", b);
	CHECK(put(a, "1234567890", 10));
	spec = spec_for(
	        names, 1, WF_COLLIDE_STOP, WF_VERIFY_SHA256, WF_CJ_COPY);
	CHECK(put(a, "abcdefghij", 10)); /* ten bytes again */
	CHECK(shift_mtime(a, 20));
	r = run(&spec, NULL, NULL);
	CHECK(0 == r.files_copied &&
	        WF_CF_SOURCE_CHANGED == r.failures[0].kind);
	CHECK(!exists(b) && 0 == temporaries(dst_dir));
	wf_copyresult_free(&r);
	wf_copyspec_free(&spec);
}

static void a_source_that_is_not_what_was_chosen_is_not_copied(void)
{
	wchar_t a[1100], b[1100];
	const wchar_t *names[] = {L"s.txt"};
	WfCopySpec spec;
	WfCopyResult r;

	fresh_dirs();
	path_in(src_dir, L"s.txt", a);
	path_in(dst_dir, L"s.txt", b);
	CHECK(put(a, "1234567890", 10));
	spec = spec_for(
	        names, 1, WF_COLLIDE_STOP, WF_VERIFY_SHA256, WF_CJ_COPY);
	CHECK(put(a, "12345", 5)); /* the file changed after it was captured */
	r = run(&spec, NULL, NULL);
	CHECK(0 == r.files_copied &&
	        WF_CF_SOURCE_CHANGED == r.failures[0].kind);
	CHECK(!exists(b) && 0 == temporaries(dst_dir));
	wf_copyresult_free(&r);
	wf_copyspec_free(&spec);

	/* held open for writing by someone else: cannot be read consistently */
	CHECK(put(a, "abcdef", 6));
	spec = spec_for(
	        names, 1, WF_COLLIDE_STOP, WF_VERIFY_SHA256, WF_CJ_COPY);
	{
		HANDLE writer = CreateFileW(a, GENERIC_WRITE,
		        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
		        0, NULL);

		CHECK(INVALID_HANDLE_VALUE != writer);
		r = run(&spec, NULL, NULL);
		CHECK(0 == r.files_copied &&
		        WF_CF_SOURCE_UNREADABLE == r.failures[0].kind);
		CHECK(!exists(b) && 0 == temporaries(dst_dir));
		CloseHandle(writer);
	}
	wf_copyresult_free(&r);
	wf_copyspec_free(&spec);
}

/* `base` + "\d" + `levels` folders of `width` letters each, as an extended
 * path ("\\?\...") so that it can be longer than MAX_PATH. */
static void deep_path(const wchar_t *base,
        unsigned levels,
        unsigned width,
        wchar_t *out,
        size_t cap)
{
	size_t at = (size_t)swprintf(out, cap, L"\\\\?\\%ls\\d", base);

	for (unsigned i = 0; i < levels; i++) {
		out[at++] = L'\\';
		for (unsigned k = 0; k < width; k++) {
			out[at++] = (wchar_t)(L'a' + i);
		}
	}
	out[at] = L'\0';
}

/* Makes the folders of deep_path(), and removes them again, one level at a
 * time: the scratch purge of the test works with paths of 1100 units. */
static bool deep_make(const wchar_t *base, unsigned levels, unsigned width)
{
	wchar_t path[8192];
	size_t at;

	deep_path(base, 0, width, path, WF_ARRAY_LEN(path));
	if (!CreateDirectoryW(path, NULL)) {
		return false;
	}
	for (unsigned i = 0; i < levels; i++) {
		at = wcslen(path);
		path[at++] = L'\\';
		for (unsigned k = 0; k < width; k++) {
			path[at++] = (wchar_t)(L'a' + i);
		}
		path[at] = L'\0';
		if (!CreateDirectoryW(path, NULL)) {
			return false;
		}
	}
	return true;
}

static void deep_remove(const wchar_t *base, unsigned levels, unsigned width)
{
	wchar_t path[8192], file[8300];

	for (unsigned i = levels + 1; i-- > 0;) {
		deep_path(base, i, width, path, WF_ARRAY_LEN(path));
		if (i == levels) {
			swprintf(file, WF_ARRAY_LEN(file), L"%ls\\x.txt", path);
			(void)DeleteFileW(file);
		}
		(void)RemoveDirectoryW(path);
	}
}

/* A folder deeper than the old fixed buffers held (a path of 1100 units and
 * more) is copied whole: the preflight lets paths of 32766 units through, and
 * the engine builds them as long. */
static void a_folder_with_a_very_long_path_is_copied(void)
{
	wchar_t src_file[8300], dst_file[8300];
	const wchar_t *names[] = {L"d"};
	WfCopySpec spec;
	WfCopyResult r;

	fresh_dirs();
	CHECK(deep_make(src_dir, 6, 200));
	deep_path(src_dir, 6, 200, src_file, WF_ARRAY_LEN(src_file));
	CHECK(wcslen(src_file) > 1100);
	wcscat_s(src_file, WF_ARRAY_LEN(src_file), L"\\x.txt");
	CHECK(put(src_file, "deep", 4));
	spec = spec_for(
	        names, 1, WF_COLLIDE_STOP, WF_VERIFY_SHA256, WF_CJ_COPY);
	r = run(&spec, NULL, NULL);
	CHECK(0 == r.failure_total && 1 == r.files_copied);
	deep_path(dst_dir, 6, 200, dst_file, WF_ARRAY_LEN(dst_file));
	wcscat_s(dst_file, WF_ARRAY_LEN(dst_file), L"\\x.txt");
	CHECK(GetFileAttributesW(dst_file) != INVALID_FILE_ATTRIBUTES);
	wf_copyresult_free(&r);
	wf_copyspec_free(&spec);
	deep_remove(src_dir, 6, 200);
	deep_remove(dst_dir, 6, 200);
}

/* A file that fails inside a sub folder is listed with its whole path, so that
 * a retry takes the item that failed and not the folder it is in. */
static void a_failure_below_a_sub_folder_keeps_its_full_path(void)
{
	wchar_t sub[1100], file[1100], want[1100];
	const wchar_t *names[] = {L"top"};
	bool found = false;
	WfCopySpec spec;
	WfCopyResult r;
	HANDLE writer;

	fresh_dirs();
	path_in(src_dir, L"top", sub);
	CHECK(CreateDirectoryW(sub, NULL));
	swprintf(sub, 1100, L"%ls\\top\\inner", src_dir);
	CHECK(CreateDirectoryW(sub, NULL));
	swprintf(file, 1100, L"%ls\\x.txt", sub);
	CHECK(put(file, "held", 4));
	spec = spec_for(
	        names, 1, WF_COLLIDE_STOP, WF_VERIFY_SHA256, WF_CJ_COPY);
	writer = CreateFileW(file, GENERIC_WRITE,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0,
	        NULL);
	CHECK(INVALID_HANDLE_VALUE != writer);
	r = run(&spec, NULL, NULL);
	CloseHandle(writer);
	swprintf(want, 1100, L"%ls\\top\\inner\\x.txt", src_dir);
	for (size_t i = 0; i < r.fail_all_n; i++) {
		found |= 0 == wcscmp(r.fail_all[i].path, want);
	}
	CHECK(found);
	wf_copyresult_free(&r);
	wf_copyspec_free(&spec);
}

typedef struct
{
	int chunks;
	int cancel_after;
} Hooks;

static bool checkpoint(void *ctx)
{
	Hooks *h = ctx;

	h->chunks++;
	return !(h->cancel_after && h->chunks > h->cancel_after);
}

static void cancelling_leaves_nothing_behind(void)
{
	wchar_t a[1100], b[1100];
	const wchar_t *names[] = {L"big.bin", L"second.bin"};
	WfCopySpec spec;
	WfCopyResult r;
	Hooks h = {0, 5};
	WfCopyControl ctl = {&h, checkpoint, NULL};

	fresh_dirs();
	path_in(src_dir, L"big.bin", a);
	CHECK(make_pattern(a, 20u << 20, 3));
	path_in(src_dir, L"second.bin", a);
	CHECK(make_pattern(a, 1000, 4));
	spec = spec_for(
	        names, 2, WF_COLLIDE_STOP, WF_VERIFY_SHA256, WF_CJ_COPY);
	r = run(&spec, &ctl, NULL);
	CHECK(WF_CS_CANCELLED == r.state && 0 == r.files_copied);
	path_in(dst_dir, L"big.bin", b);
	CHECK(!exists(b) && 0 == temporaries(dst_dir));
	path_in(dst_dir, L"second.bin", b);
	CHECK(!exists(b)); /* the job stopped; the second was never started */
	wf_copyresult_free(&r);
	wf_copyspec_free(&spec);
}

static bool id_unreadable(void)
{
	return true;
}

/* A temporary that was made and whose identity cannot be read cannot be
 * journalled as created, and nothing is deleted without an identity: the copy
 * is reported as failed, the destination has no file of that name, the
 * temporary stays under its announced name, and the journal holds the record of
 * the announcement for the next start to treat as unresolved (never removed on
 * a guess). Another copy afterwards is not affected. */
static void a_temporary_without_an_identity_is_kept_and_recorded(void)
{
	wchar_t a[1100], b[1100];
	const wchar_t *names[] = {L"one.bin"};
	WfCopySpec spec;
	WfCopyResult r;
	WfCopyJournal *j;
	WfCopyJournalInfo rec;

	fresh_dirs();
	swprintf(journal_path, 1100, L"%ls\\journal-noid.txt", root);
	j = wf_cj_open(journal_path);
	CHECK(NULL != j);
	path_in(src_dir, L"one.bin", a);
	CHECK(make_pattern(a, 5000, 7));
	spec = spec_for(
	        names, 1, WF_COLLIDE_STOP, WF_VERIFY_SHA256, WF_CJ_COPY);
	wf_copyengine_test_tmp_id_unreadable = id_unreadable;
	r = run(&spec, NULL, j);
	wf_copyengine_test_tmp_id_unreadable = NULL;
	CHECK(0 == r.files_copied && 1 == r.files_failed);
	CHECK(r.failure_total >= 1 && WF_CF_WRITE_FAILED == r.failures[0].kind);
	path_in(dst_dir, L"one.bin", b);
	CHECK(!exists(b)); /* the destination was not touched */
	CHECK(1 ==
	        temporaries(
	                dst_dir)); /* and the temporary was not guessed away */
	remove_temporaries(dst_dir);
	wf_copyresult_free(&r);
	wf_cj_close(j);
	CHECK(wf_cj_inspect(journal_path, &rec));
	CHECK(WF_CJ_RECEIPTS == rec.format && rec.records >= 1);
	/* the source is as it was, and the next copy works */
	CHECK(exists(a));
	r = run(&spec, NULL, NULL);
	CHECK(WF_CS_DONE == r.state && 1 == r.files_copied && exists(b));
	wf_copyresult_free(&r);
	wf_copyspec_free(&spec);
	DeleteFileW(journal_path); /* this test's own journal */
}

/* A folder whose copy cannot be finished leaves nothing under its name, and
 * what was made while trying is taken apart again. */
static void an_unfinished_folder_is_not_left_behind(void)
{
	wchar_t p[1100], q[1100], held[1100];
	const wchar_t *names[] = {L"T"};
	WfCopySpec spec;
	WfCopyResult r;
	HANDLE writer;

	fresh_dirs();
	path_in(src_dir, L"T", p);
	CHECK(CreateDirectoryW(p, NULL));
	path_in(src_dir, L"T\\sub", p);
	CHECK(CreateDirectoryW(p, NULL));
	path_in(src_dir, L"T\\a.bin", p);
	CHECK(make_pattern(p, 3000, 1));
	path_in(src_dir, L"T\\sub\\b.bin", p);
	CHECK(make_pattern(p, 3000, 2));
	path_in(src_dir, L"T\\sub\\held.bin", held);
	CHECK(make_pattern(held, 3000, 3));
	path_in(dst_dir, L"T", q);

	/* one file inside cannot be read: the folder is not made at all */
	writer = CreateFileW(held, GENERIC_WRITE,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0,
	        NULL);
	CHECK(INVALID_HANDLE_VALUE != writer);
	spec = spec_for(
	        names, 1, WF_COLLIDE_STOP, WF_VERIFY_SHA256, WF_CJ_COPY);
	r = run(&spec, NULL, NULL);
	CHECK(r.failure_total >= 1 &&
	        WF_CF_SOURCE_UNREADABLE == r.failures[0].kind);
	CHECK(0 == r.files_copied && 0 == r.dirs_created && 0 == r.bytes);
	CHECK(!exists(q) && 0 == temporaries(dst_dir) && 0 == r.created_n);
	wf_copyresult_free(&r);
	wf_copyspec_free(&spec);
	CloseHandle(writer);

	/* cancelled in the middle of the folder */
	{
		Hooks h = {0, 3};
		WfCopyControl ctl = {&h, checkpoint, NULL};

		path_in(src_dir, L"T\\big.bin", p);
		CHECK(make_pattern(p, 8u << 20, 4));
		spec = spec_for(names, 1, WF_COLLIDE_STOP, WF_VERIFY_SHA256,
		        WF_CJ_COPY);
		r = run(&spec, &ctl, NULL);
		CHECK(WF_CS_CANCELLED == r.state && 0 == r.files_copied);
		CHECK(!exists(q) && 0 == temporaries(dst_dir));
		wf_copyresult_free(&r);
		wf_copyspec_free(&spec);
	}

	/* and whole when nothing is wrong */
	CHECK(DeleteFileW(held)); /* the held one is gone from the source */
	spec = spec_for(
	        names, 1, WF_COLLIDE_STOP, WF_VERIFY_SHA256, WF_CJ_COPY);
	r = run(&spec, NULL, NULL);
	CHECK(WF_CS_DONE == r.state && 3 == r.files_copied &&
	        0 == r.failure_total && exists(q) && 0 == temporaries(dst_dir));
	wf_copyresult_free(&r);
	wf_copyspec_free(&spec);
}

/* wf_owned_delete_empty_dir: only the folder that was made, only when empty. */
static void an_empty_folder_is_removed_only_on_its_own_identity(void)
{
	wchar_t d[1100], f[1100];
	FILE_ID_INFO id, other;
	HANDLE h;
	WfOwnedResult res;

	fresh_dirs();
	path_in(dst_dir, L"made", d);
	CHECK(CreateDirectoryW(d, NULL));
	h = CreateFileW(d, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	CHECK(GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof id));
	CloseHandle(h);
	other = id;
	other.FileId.Identifier[0] ^= 0xFF;

	res = wf_owned_delete_empty_dir(d, &other, NULL);
	CHECK(WF_OWNED_MANUAL == res && exists(d)); /* not the folder made */

	path_in(d, L"inside.txt", f);
	CHECK(put(f, "x", 1));
	res = wf_owned_delete_empty_dir(d, &id, NULL);
	CHECK(WF_OWNED_DELETED != res && exists(d) &&
	        exists(f)); /* not empty */
	CHECK(DeleteFileW(f));

	res = wf_owned_delete_empty_dir(d, &id, NULL);
	CHECK(WF_OWNED_DELETED == res && !exists(d));
	res = wf_owned_delete_empty_dir(d, &id, NULL);
	CHECK(WF_OWNED_ABSENT == res);

	/* a file is never taken for a folder */
	path_in(dst_dir, L"plain.txt", f);
	CHECK(put(f, "y", 1));
	h = CreateFileW(f, FILE_READ_ATTRIBUTES, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, 0, NULL);
	CHECK(GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof id));
	CloseHandle(h);
	res = wf_owned_delete_empty_dir(f, &id, NULL);
	CHECK(WF_OWNED_MANUAL == res && exists(f));
	CHECK(DeleteFileW(f));
}

static void verification_choices_follow_the_plan(void)
{
	wchar_t a[1100], b[1100];
	const wchar_t *names[] = {L"v.bin"};
	WfCopySpec spec;
	WfCopyResult r;
	WfCopyVerify modes[] = {
	        WF_VERIFY_NONE, WF_VERIFY_SHA256, WF_VERIFY_HMAC_SHA256};
	size_t i;

	for (i = 0; i < 3; i++) {
		fresh_dirs();
		path_in(src_dir, L"v.bin", a);
		path_in(dst_dir, L"v.bin", b);
		CHECK(make_pattern(a, 70000, (unsigned)i));
		spec = spec_for(
		        names, 1, WF_COLLIDE_STOP, modes[i], WF_CJ_COPY);
		r = run(&spec, NULL, NULL);
		CHECK(1 == r.files_copied && 0 == r.failure_total &&
		        same_file_content(a, b));
		wf_copyresult_free(&r);
		wf_copyspec_free(&spec);
	}
}

static void links_are_not_followed_and_say_so(void)
{
	wchar_t target[1100], junction[1100], cmd[2400], inside[1100];
	const wchar_t *names[] = {L"tree"};
	WfCopySpec spec;
	WfCopyResult r;
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	DWORD code = 1;

	fresh_dirs();
	path_in(src_dir, L"tree", inside);
	CHECK(CreateDirectoryW(inside, NULL));
	path_in(src_dir, L"outside", target);
	CHECK(CreateDirectoryW(target, NULL));
	swprintf(inside, 1100, L"%ls\\secret.txt", target);
	CHECK(put(inside, "not to be copied", 16));
	swprintf(junction, 1100, L"%ls\\tree\\jump", src_dir);
	swprintf(cmd, 2400, L"cmd.exe /c mklink /J \"%ls\" \"%ls\" >nul",
	        junction, target);
	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL,
	            NULL, &si, &pi)) {
		WaitForSingleObject(pi.hProcess, 10000);
		GetExitCodeProcess(pi.hProcess, &code);
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
	}
	if (0 != code || !exists(junction)) {
		printf("  NOT TESTED: a junction could not be made here\n");
		return;
	}
	spec = spec_for(
	        names, 1, WF_COLLIDE_STOP, WF_VERIFY_SHA256, WF_CJ_COPY);
	r = run(&spec, NULL, NULL);
	CHECK(1 == r.links_skipped && 1 == r.failure_total &&
	        WF_CF_LINK_SKIPPED == r.failures[0].kind);
	swprintf(inside, 1100, L"%ls\\tree\\jump", dst_dir);
	CHECK(!exists(inside)); /* neither the link nor its target's content */
	wf_copyresult_free(&r);
	wf_copyspec_free(&spec);
	/* the junction is removed by the area's own cleanup; make sure the area
	 * does not follow it */
	RemoveDirectoryW(junction);
}

static void a_move_removes_the_source_only_after_a_verified_copy(void)
{
	wchar_t a[1100], b[1100], c[1100], d[1100];
	const wchar_t *names[] = {L"moved.bin", L"kept.bin"};
	WfCopySpec spec;
	WfCopyResult r;

	fresh_dirs();
	path_in(src_dir, L"moved.bin", a);
	path_in(dst_dir, L"moved.bin", b);
	path_in(src_dir, L"kept.bin", c);
	path_in(dst_dir, L"kept.bin", d);
	CHECK(make_pattern(a, 5000, 1));
	CHECK(make_pattern(c, 5000, 2));
	CHECK(put(d, "occupied",
	        8)); /* kept.bin cannot be copied: its source stays */
	spec = spec_for(
	        names, 2, WF_COLLIDE_STOP, WF_VERIFY_NONE, WF_CJ_MOVE_ACROSS);
	r = run(&spec, NULL, NULL);
	CHECK(1 == r.files_copied && 1 == r.sources_recycled &&
	        1 == r.sources_kept);
	CHECK(exists(b) &&
	        !exists(a)); /* copied, verified, source to the bin */
	CHECK(exists(c));    /* the one that failed is still where it was */
	CHECK(WF_CF_EXISTS == r.failures[0].kind);
	wf_copyresult_free(&r);
	wf_copyspec_free(&spec);
}

typedef struct
{
	wchar_t late[1100];
	bool append; /* touch a file that was already copied, else add one */
	bool done;
} LateChange;

static void late_progress(
        void *ctx, size_t files, uint64_t bytes, const wchar_t *current)
{
	LateChange *c = ctx;

	(void)files;
	(void)bytes;
	(void)current;
	if (c->done) {
		return;
	}
	c->done = true;
	if (c->append) {
		HANDLE h = CreateFileW(c->late, FILE_APPEND_DATA,
		        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
		        0, NULL);
		DWORD w;

		CHECK(INVALID_HANDLE_VALUE != h);
		if (INVALID_HANDLE_VALUE != h) {
			WriteFile(h, "more", 4, &w, NULL);
			CloseHandle(h);
		}
	} else {
		CHECK(put(c->late, "added meanwhile", 15));
	}
}

/* A moved folder goes to the bin only if it still holds exactly what was
 * copied: a file changed or added after it was listed is not in the copy. */
static void a_moved_folder_that_changed_meanwhile_is_kept(void)
{
	const wchar_t *names[] = {L"tree"};
	int variant;

	for (variant = 0; variant < 2; variant++) {
		wchar_t tree[1100], f1[1100], f2[1100], f3[1100], c1[1100];
		WfCopySpec spec;
		WfCopyResult r;
		WfCopyControl ctl;
		LateChange late;

		fresh_dirs();
		path_in(src_dir, L"tree", tree);
		CHECK(CreateDirectoryW(tree, NULL));
		path_in(tree, L"a.txt", f1);
		path_in(tree, L"b.txt", f2);
		path_in(tree, L"c.txt", f3);
		CHECK(put(f1, "aaa", 3) && put(f2, "bbb", 3) &&
		        put(f3, "ccc", 3));
		memset(&late, 0, sizeof late);
		late.append = 0 == variant;
		wcscpy_s(late.late, 1100, 0 == variant ? f1 : tree);
		if (1 == variant) {
			path_in(tree, L"zz-new.txt", late.late);
		}
		memset(&ctl, 0, sizeof ctl);
		ctl.ctx = &late;
		ctl.progress = late_progress;
		spec = spec_for(names, 1, WF_COLLIDE_STOP, WF_VERIFY_NONE,
		        WF_CJ_MOVE_ACROSS);
		r = run(&spec, &ctl, NULL);
		CHECK(late.done);
		/* the copy itself is whole ... */
		path_in(dst_dir, L"tree\\a.txt", c1);
		CHECK(exists(c1));
		/* ... but the source is not what was copied: it stays, whole */
		CHECK(0 == r.sources_recycled && 1 == r.sources_kept);
		CHECK(exists(f1) && exists(f2) && exists(f3));
		CHECK(r.failure_total >= 1 &&
		        WF_CF_SOURCE_CHANGED == r.failures[0].kind);
		wf_copyresult_free(&r);
		wf_copyspec_free(&spec);
	}
}

/* Recycling: only the object that was listed, and only to the Recycle Bin. */
static void recycling_checks_what_it_is_asked_to_remove(void)
{
	wchar_t a[1100], b[1100];
	const wchar_t *names[] = {L"gone.txt", L"swapped.txt"};
	WfCopySpec spec;
	WfCopyResult r;

	fresh_dirs();
	path_in(src_dir, L"gone.txt", a);
	path_in(src_dir, L"swapped.txt", b);
	CHECK(put(a, "to the bin", 10));
	CHECK(put(b, "original", 8));
	spec = spec_for(
	        names, 2, WF_COLLIDE_STOP, WF_VERIFY_NONE, WF_CJ_RECYCLE);
	free(spec.dest);
	spec.dest = NULL;
	/* swapped.txt is replaced by a different file after it was listed */
	CHECK(DeleteFileW(b));
	CHECK(put(b, "impostor", 8));
	r = run(&spec, NULL, NULL);
	CHECK(1 == r.sources_recycled && 1 == r.sources_kept);
	CHECK(!exists(a));
	/* and the job says which source went, by position */
	CHECK(2 == r.item_n && r.item_done && 1 == r.item_done[0] &&
	        0 == r.item_done[1]);
	CHECK(exists(b)); /* the one that is not the listed object stays */
	CHECK(WF_CF_NOT_REMOVED == r.failures[0].kind);
	wf_copyresult_free(&r);
	wf_copyspec_free(&spec);
}

/* A folder is measured before the bin is asked, and the bin's own limits are
 * a refusal, not a question put to the user. */
static void recycling_a_folder_and_the_bin_limits(void)
{
	wchar_t dir[1100], sub[1100], f1[1100], f2[1100];
	const wchar_t *names[] = {L"tree"};
	WfCopySpec spec;
	WfCopyResult r;

	CHECK(wf_recycle_fits(false, 1000, 1000));
	CHECK(!wf_recycle_fits(false, 1000, 1001));
	CHECK(!wf_recycle_fits(true, 1000, 1)); /* "delete immediately" */
	CHECK(!wf_recycle_fits(false, 0, 0));   /* capacity unknown */

	fresh_dirs();
	path_in(src_dir, L"tree", dir);
	path_in(dir, L"sub", sub);
	path_in(dir, L"a.txt", f1);
	path_in(sub, L"b.txt", f2);
	CHECK(CreateDirectoryW(dir, NULL) && CreateDirectoryW(sub, NULL));
	CHECK(put(f1, "aaaa", 4) && put(f2, "bbbbbb", 6));
	spec = spec_for(
	        names, 1, WF_COLLIDE_STOP, WF_VERIFY_NONE, WF_CJ_RECYCLE);
	free(spec.dest);
	spec.dest = NULL;
	r = run(&spec, NULL, NULL);
	CHECK(1 == r.sources_recycled && 0 == r.sources_kept);
	CHECK(!exists(dir)); /* the whole folder is in the bin */
	wf_copyresult_free(&r);
	wf_copyspec_free(&spec);
}

static void the_journal_resolves_what_a_dead_writer_left(void)
{
	WfCopyJournal *j;
	WfCopyJournalInfo rec;
	wchar_t tmp[1100], other[1100];
	HANDLE h;
	FILE_ID_INFO id;
	DWORD w;

	fresh_dirs();
	swprintf(journal_path, 1100, L"%ls\\journal-a.txt", root);
	j = wf_cj_open(journal_path);
	CHECK(NULL != j);
	/* a temporary this "process" made */
	swprintf(tmp, 1100, L"%ls\\.wfcopy-0000000000000001.tmp", dst_dir);
	CHECK(wf_cj_intent(j, tmp));
	h = CreateFileW(tmp, GENERIC_WRITE | READ_CONTROL, 0, NULL, CREATE_NEW,
	        0, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	WriteFile(h, "partial", 7, &w, NULL);
	CHECK(GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof id));
	CloseHandle(h);
	CHECK(wf_cj_created(j, tmp, &id));
	/* someone else's file under a name that was only announced */
	swprintf(other, 1100, L"%ls\\.wfcopy-0000000000000002.tmp", dst_dir);
	CHECK(wf_cj_intent(j, other));
	CHECK(put(other, "user data", 9));
	wf_cj_close(j);

	/* this process is alive and owns the records: nothing is touched */
	CHECK(wf_cj_inspect(journal_path, &rec));
	CHECK(rec.records >= 2);
	CHECK(exists(tmp) && exists(other));
}

typedef struct
{
	int chunks;
	wchar_t marker[1100];
} ChildHooks;

static bool child_checkpoint(void *ctx)
{
	(void)ctx;
	Sleep(60); /* slow enough that the parent can kill it in the middle */
	return true;
}

static void child_progress(
        void *ctx, size_t files, uint64_t bytes, const wchar_t *current)
{
	ChildHooks *h = ctx;

	(void)files;
	(void)current;
	if (++h->chunks == 3 && bytes > 0) {
		put(h->marker, "go", 2);
	}
}

/* The test exe run as the process that gets killed. */
static int child_main(const wchar_t *src,
        const wchar_t *dst,
        const wchar_t *journal,
        const wchar_t *marker)
{
	const wchar_t *names[] = {L"large.bin"};
	WfCopySpec spec;
	WfCopyResult r;
	ChildHooks h = {0, {0}};
	WfCopyControl ctl;
	WfCopyJournal *j = wf_cj_open(journal);
	WfNodeInfo d;

	wcscpy_s(h.marker, 1100, marker);
	ops = wf_probe_win();
	memset(&spec, 0, sizeof spec);
	wf_slot_init(&spec.slot);
	if (WF_OK !=
	        wf_xfer_capture(
	                &ops, WF_XFER_COPY, src, names, 1, 0, &spec.slot)) {
		return 2;
	}
	spec.dest = _wcsdup(dst);
	if (WF_OK != ops.node(NULL, dst, &d)) {
		return 3;
	}
	spec.dest_id = d.id;
	spec.kind = WF_CJ_COPY;
	spec.verify = WF_VERIFY_SHA256;
	spec.collide = WF_COLLIDE_STOP;
	ctl.ctx = &h;
	ctl.checkpoint = child_checkpoint;
	ctl.progress = child_progress;
	/* progress needs the hooks too */
	if (WF_OK != wf_copyengine_run(&spec, &ctl, j, &r)) {
		return 4;
	}
	return 0;
}

static void a_killed_copy_leaves_only_what_the_journal_can_remove(void)
{
	wchar_t big[1100], cmd[4800], marker[1100], j2[1100], keep[1100];
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	WfCopyJournalInfo rec;
	DWORD i;
	char srchash[P0_HASH_HEX], after[P0_HASH_HEX];

	fresh_dirs();
	path_in(src_dir, L"large.bin", big);
	CHECK(make_pattern(big, 48u << 20, 9));
	CHECK(hash_of(big, srchash));
	swprintf(j2, 1100, L"%ls\\journal-b.txt", root);
	swprintf(marker, 1100, L"%ls\\marker-b.txt", root);
	swprintf(cmd, 4800, L"\"%ls\" --child \"%ls\" \"%ls\" \"%ls\" \"%ls\"",
	        g_exe_w, src_dir, dst_dir, j2, marker);
	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	CHECK(CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
	        NULL, NULL, &si, &pi));
	for (i = 0; i < 200 && !exists(marker); i++) {
		Sleep(50);
	}
	CHECK(exists(marker));
	CHECK(1 == temporaries(dst_dir)); /* the copy is half-way */
	/* while the writer lives, recovery leaves its temporary alone */
	CHECK(wf_cj_inspect(j2, &rec) && rec.records >= 1);
	CHECK(1 == temporaries(dst_dir));
	TerminateProcess(pi.hProcess, 99); /* a crash, not a clean exit */
	WaitForSingleObject(pi.hProcess, 10000);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	CHECK(1 == temporaries(dst_dir)); /* what a crash leaves */
	path_in(dst_dir, L"large.bin", keep);
	CHECK(!exists(keep)); /* never a partial file under the final name */
	CHECK(wf_cj_inspect(j2, &rec));
	CHECK(rec.records > 0);
	CHECK(1 == temporaries(dst_dir)); /* no unauthenticated cleanup */
	CHECK(hash_of(big, after) &&
	        !strcmp(srchash, after)); /* the source is intact */
	/* Repeated inspection is non-mutating and preserves the inventory. */
	CHECK(wf_cj_inspect(j2, &rec) && rec.records > 0);
}

static bool id_of(const wchar_t *path, FILE_ID_INFO *id)
{
	HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	bool ok;

	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	ok = 0 != GetFileInformationByHandleEx(h, FileIdInfo, id, sizeof *id);
	CloseHandle(h);
	return ok;
}

/* A merge makes items at several levels. Each is registered with the folder it
 * is in, as the job held it (so Undo can prove that folder), and a folder that
 * is new inside a merged one is built hidden and published whole: a cancel in
 * the middle leaves no half-made folder under its name. */
static bool made_in(
        const WfCopyResult *r, const wchar_t *path, const wchar_t *dir)
{
	FILE_ID_INFO want;

	if (!id_of(dir, &want)) {
		return false;
	}
	for (size_t k = 0; k < r->created_n; k++) {
		const WfCopyCreated *c = &r->created[k];

		if (!wcscmp(c->path, path)) {
			return (c->parent.kind == WF_ID_STRONG) &&
			        c->parent.volume == want.VolumeSerialNumber &&
			        !memcmp(c->parent.id, want.FileId.Identifier,
			                sizeof c->parent.id);
		}
	}
	return false;
}

static void a_merge_names_each_parent_and_stages_new_folders(void)
{
	const wchar_t *names[] = {L"assets"};
	wchar_t dst_assets[1100], p[1100], sub[1100], fresh[1100];
	WfCopySpec spec;
	WfCopyResult r;

	build_merge_case(dst_assets);
	path_in(src_dir, L"assets\\fresh", p);
	CHECK(CreateDirectoryW(p, NULL));
	path_in(src_dir, L"assets\\fresh\\f.txt", p);
	CHECK(put(p, "f", 1));
	spec = spec_for(
	        names, 1, WF_COLLIDE_SKIP, WF_VERIFY_SHA256, WF_CJ_COPY);
	spec.merge_folders = true;
	r = run(&spec, NULL, NULL);
	CHECK(0 == r.failure_total && 0 == temporaries(dst_assets));
	path_in(dst_dir, L"assets\\sub", sub);
	path_in(dst_dir, L"assets\\fresh", fresh);
	path_in(dst_dir, L"assets\\b.png", p);
	CHECK(made_in(&r, p, dst_assets)); /* in the folder entered */
	path_in(dst_dir, L"assets\\sub\\x.txt", p);
	CHECK(made_in(&r, p, sub));            /* one level down */
	CHECK(made_in(&r, fresh, dst_assets)); /* the new folder, whole */
	CHECK(3 == r.created_n);
	wf_copyresult_free(&r);
	wf_copyspec_free(&spec);

	/* cancelled inside a folder that is new: nothing of it stays, what was
	 * there is untouched, and nothing is registered for it */
	build_merge_case(dst_assets);
	path_in(src_dir, L"assets\\fresh", p);
	CHECK(CreateDirectoryW(p, NULL));
	path_in(src_dir, L"assets\\fresh\\a.bin", p);
	CHECK(put(p, "a", 1));
	path_in(src_dir, L"assets\\fresh\\b.bin", p);
	CHECK(make_pattern(p, 8u << 20, 5));
	{
		Hooks h = {0, 0};
		WfCopyControl ctl = {&h, checkpoint, NULL};

		/* a whole run counts the chunks; the next one stops half way, which
		 * is inside the big file of the new folder (the small ones are few) */
		spec = spec_for(names, 1, WF_COLLIDE_SKIP, WF_VERIFY_SHA256,
		        WF_CJ_COPY);
		spec.merge_folders = true;
		r = run(&spec, &ctl, NULL);
		CHECK(WF_CS_DONE == r.state && h.chunks > 8);
		wf_copyresult_free(&r);
		wf_copyspec_free(&spec);
		build_merge_case(dst_assets);
		path_in(src_dir, L"assets\\fresh", p);
		CHECK(CreateDirectoryW(p, NULL));
		path_in(src_dir, L"assets\\fresh\\a.bin", p);
		CHECK(put(p, "a", 1));
		path_in(src_dir, L"assets\\fresh\\b.bin", p);
		CHECK(make_pattern(p, 8u << 20, 5));
		h.cancel_after = h.chunks / 2;
		h.chunks = 0;
		spec = spec_for(names, 1, WF_COLLIDE_SKIP, WF_VERIFY_SHA256,
		        WF_CJ_COPY);
		spec.merge_folders = true;
		r = run(&spec, &ctl, NULL);
		CHECK(WF_CS_CANCELLED == r.state);
		path_in(dst_dir, L"assets\\fresh", p);
		CHECK(!exists(p) && 0 == temporaries(dst_assets));
		path_in(dst_dir, L"assets\\a.png", p);
		CHECK(holds(p, "OLD a"));
		path_in(dst_dir, L"assets\\c.png", p);
		CHECK(holds(p, "keep c"));
		path_in(dst_dir, L"assets\\sub\\y.txt", p);
		CHECK(holds(p, "keep y"));
		for (size_t k = 0; k < r.created_n; k++) {
			CHECK(wcscmp(r.created[k].path, fresh));
		}
		wf_copyresult_free(&r);
	}
	wf_copyspec_free(&spec);
}

/* A folder to be entered must be a plain folder that is not the one being
 * copied: a junction at the destination is never entered (it is a taken name
 * of another kind, and nothing is written through it), and the very same
 * folder, found by its FileId, is refused. */
static bool junction_to(const wchar_t *link, const wchar_t *target)
{
	wchar_t cmd[2400];
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	DWORD code = 1;

	swprintf(cmd, 2400, L"cmd.exe /c mklink /J \"%ls\" \"%ls\" >nul", link,
	        target);
	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL,
	            NULL, &si, &pi)) {
		WaitForSingleObject(pi.hProcess, 10000);
		GetExitCodeProcess(pi.hProcess, &code);
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
	}
	return 0 == code && exists(link);
}

static void a_junction_or_the_same_folder_is_not_entered_by_a_merge(void)
{
	const wchar_t *names[] = {L"assets"};
	wchar_t p[1100], target[1100], link[1100];
	WfCopySpec spec;
	WfCopyResult r;
	static const WfCollide pol[] = {
	        WF_COLLIDE_SKIP, WF_COLLIDE_RENAME, WF_COLLIDE_OVERWRITE};

	for (int k = 0; k < 3; k++) {
		/* the destination's "assets" is a junction to somewhere else */
		fresh_dirs();
		path_in(src_dir, L"assets", p);
		CHECK(CreateDirectoryW(p, NULL));
		path_in(src_dir, L"assets\\f.txt", p);
		CHECK(put(p, "payload", 7));
		path_in(dst_dir, L"elsewhere", target);
		CHECK(CreateDirectoryW(target, NULL));
		path_in(dst_dir, L"assets", link);
		if (!junction_to(link, target)) {
			printf("  NOT TESTED: a junction could not be made "
			       "here\n");
			return;
		}
		spec = spec_for(names, 1, pol[k], WF_VERIFY_SHA256, WF_CJ_COPY);
		spec.merge_folders = true;
		r = run(&spec, NULL, NULL);
		path_in(dst_dir, L"elsewhere\\f.txt", p);
		CHECK(!exists(p));
		/* a junction is a name taken by something that is not a plain folder:
		 * the policy decides for that name as for any other object, and it is
		 * never entered */
		if (WF_COLLIDE_SKIP == pol[k]) {
			CHECK(1 == r.files_skipped && 0 == r.files_copied);
		} else if (WF_COLLIDE_RENAME == pol[k]) {
			path_in(dst_dir, L"assets (2)\\f.txt", p);
			CHECK(holds(p, "payload"));
		} else {
			CHECK(r.failure_total >= 1 &&
			        WF_CF_EXISTS == r.failures[0].kind);
		}
		CHECK(0 == temporaries(dst_dir));
		RemoveDirectoryW(link); /* the link, never its target */
		wf_copyresult_free(&r);
		wf_copyspec_free(&spec);
	}
	/* the folder is its own destination: copying "assets" into the folder it
	 * is in finds "assets" there, and it is the same folder (by FileId) */
	for (int k = 0; k < 3; k++) {
		fresh_dirs();
		path_in(src_dir, L"assets", p);
		CHECK(CreateDirectoryW(p, NULL));
		path_in(src_dir, L"assets\\f.txt", p);
		CHECK(put(p, "payload", 7));
		spec = spec_for(names, 1, pol[k], WF_VERIFY_SHA256, WF_CJ_COPY);
		free(spec.dest);
		spec.dest = _wcsdup(src_dir);
		{
			WfNodeInfo d;

			CHECK(WF_OK == ops.node(NULL, src_dir, &d));
			spec.dest_id = d.id;
		}
		spec.merge_folders = true;
		r = run(&spec, NULL, NULL);
		path_in(src_dir, L"assets\\f.txt", p);
		CHECK(holds(p, "payload")); /* untouched */
		path_in(src_dir, L"assets\\f (2).txt", p);
		CHECK(!exists(p));
		CHECK(0 == r.files_copied);
		wf_copyresult_free(&r);
		wf_copyspec_free(&spec);
	}
}

static bool file_is(const wchar_t *path, const char *text)
{
	char buf[64] = "";
	DWORD got = 0;
	HANDLE h = CreateFileW(path, GENERIC_READ,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0,
	        NULL);

	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	ReadFile(h, buf, sizeof buf - 1, &got, NULL);
	CloseHandle(h);
	buf[got] = 0;
	return !strcmp(buf, text);
}

/* Receipts never authorize changes, including a claimed discard of originals. */
static void a_dead_writers_old_version_is_put_back_never_deleted(void)
{
	wchar_t dest[1100], aside[1100], jp[1100];
	WfCopyJournal *j;
	WfCopyJournalInfo rec;
	FILE_ID_INFO id;

	fresh_dirs();
	swprintf(jp, 1100, L"%ls\\journal-c.txt", root);
	path_in(dst_dir, L"f.txt", dest);
	path_in(dst_dir, L".wfcopy-00000000000000aa.old", aside);

	/* 1. The original is preserved; only the fixture restores its own file. */
	DeleteFileW(jp);
	CHECK(put(aside, "OLD", 3) && id_of(aside, &id));
	j = wf_cj_open(jp);
	CHECK(wf_cj_aside(j, dest, aside, &id));
	wf_cj_close(j);
	wf_test_disown(jp);
	CHECK(wf_cj_inspect(jp, &rec));
	CHECK(rec.records > 0);
	CHECK(!exists(dest) && file_is(aside, "OLD"));
	CHECK(MoveFileW(
	        aside, dest)); /* explicit recovery of this fixture only */
	DeleteFileW(dest);

	/* 2. the replacement took the name but the discard was never recorded:
	 * both are left, and it says so */
	DeleteFileW(jp);
	CHECK(put(aside, "OLD", 3) && id_of(aside, &id) && put(dest, "NEW", 3));
	j = wf_cj_open(jp);
	CHECK(wf_cj_aside(j, dest, aside, &id));
	wf_cj_close(j);
	wf_test_disown(jp);
	CHECK(wf_cj_inspect(jp, &rec));
	CHECK(rec.records > 0);
	CHECK(file_is(dest, "NEW") && file_is(aside, "OLD"));

	/* 3. Even a discard receipt cannot authorize removal. */
	DeleteFileW(jp);
	j = wf_cj_open(jp);
	CHECK(wf_cj_aside(j, dest, aside, &id));
	CHECK(wf_cj_discard(j, &id));
	wf_cj_close(j);
	wf_test_disown(jp);
	CHECK(wf_cj_inspect(jp, &rec));
	CHECK(rec.records > 0);
	CHECK(file_is(dest, "NEW") && file_is(aside, "OLD"));
	DeleteFileW(dest);
	DeleteFileW(aside);

	/* 4. the name holds something that is not the recorded file: neither
	 * restored nor removed */
	DeleteFileW(jp);
	CHECK(put(aside, "OLD", 3) && id_of(aside, &id));
	j = wf_cj_open(jp);
	CHECK(wf_cj_aside(j, dest, aside, &id));
	CHECK(wf_cj_discard(j, &id));
	wf_cj_close(j);
	DeleteFileW(aside);
	CHECK(put(aside, "SOMEONE ELSE", 12));
	wf_test_disown(jp);
	CHECK(wf_cj_inspect(jp, &rec));
	CHECK(rec.records > 0);
	CHECK(file_is(aside, "SOMEONE ELSE") && !exists(dest));
	DeleteFileW(aside);
	DeleteFileW(jp);
}

static wchar_t g_hold_marker[1100];

/* Stands in for the writer dying here: says so, and never goes on. */
static void say_and_wait(const wchar_t *dest, const wchar_t *aside)
{
	(void)dest;
	(void)aside;
	put(g_hold_marker, "go", 2);
	Sleep(INFINITE);
}

/* The test exe as the process that dies between the two renames of an
 * overwrite. */
static int child_overwrite_main(const wchar_t *src,
        const wchar_t *dst,
        const wchar_t *journal,
        const wchar_t *marker)
{
	const wchar_t *names[] = {L"o.txt"};
	WfCopySpec spec;
	WfCopyResult r;
	WfCopyJournal *j = wf_cj_open(journal);
	WfNodeInfo d;

	wcscpy_s(g_hold_marker, 1100, marker);
	wf_copyengine_test_after_aside = say_and_wait;
	ops = wf_probe_win();
	memset(&spec, 0, sizeof spec);
	wf_slot_init(&spec.slot);
	if (WF_OK !=
	        wf_xfer_capture(
	                &ops, WF_XFER_COPY, src, names, 1, 0, &spec.slot)) {
		return 2;
	}
	spec.dest = _wcsdup(dst);
	if (WF_OK != ops.node(NULL, dst, &d)) {
		return 3;
	}
	spec.dest_id = d.id;
	spec.kind = WF_CJ_COPY;
	spec.verify = WF_VERIFY_SHA256;
	spec.collide = WF_COLLIDE_OVERWRITE;
	if (WF_OK != wf_copyengine_run(&spec, NULL, j, &r)) {
		return 4;
	}
	return 0;
}

/* The process is killed after the old file was moved aside and before the new
 * one took its name. The next start must leave the destination holding the
 * old file, not nothing. */
static void a_copy_killed_between_the_two_renames_keeps_the_old_file(void)
{
	wchar_t sp[1100], dp[1100], cmd[4800], marker[1100], jp[1100];
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	WfCopyJournalInfo rec;
	DWORD i;

	fresh_dirs();
	path_in(src_dir, L"o.txt", sp);
	path_in(dst_dir, L"o.txt", dp);
	CHECK(put(sp, "NEW", 3) && put(dp, "OLD", 3));
	swprintf(jp, 1100, L"%ls\\journal-d.txt", root);
	swprintf(marker, 1100, L"%ls\\marker-d.txt", root);
	DeleteFileW(jp);
	DeleteFileW(marker);
	swprintf(cmd, 4800,
	        L"\"%ls\" --child-over \"%ls\" \"%ls\" \"%ls\" \"%ls\"",
	        g_exe_w, src_dir, dst_dir, jp, marker);
	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	CHECK(CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
	        NULL, NULL, &si, &pi));
	for (i = 0; i < 200 && !exists(marker); i++) {
		Sleep(50);
	}
	CHECK(exists(marker));
	CHECK(!exists(dp)); /* the window: no file under the name */
	TerminateProcess(pi.hProcess, 99);
	WaitForSingleObject(pi.hProcess, 10000);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	CHECK(!exists(dp));
	CHECK(wf_cj_inspect(jp, &rec));
	CHECK(rec.records > 0);
	CHECK(!exists(dp)); /* no path from receipts is executed */
	CHECK(temporaries(dst_dir) >= 1);
	CHECK(wf_cj_inspect(jp, &rec) && rec.records > 0);
	DeleteFileW(jp);
	DeleteFileW(marker);
}

/* The destination was proven as one folder; by the time the job starts, the
 * name leads to another. Nothing is written there. */
static void the_destination_must_be_the_folder_that_was_checked(void)
{
	wchar_t a[1100], moved[1100], inside[1100];
	const wchar_t *names[] = {L"a.txt"};
	WfCopySpec spec;
	WfCopyResult r;

	fresh_dirs();
	path_in(src_dir, L"a.txt", a);
	CHECK(put(a, "data", 4));
	spec = spec_for(
	        names, 1, WF_COLLIDE_STOP, WF_VERIFY_SHA256, WF_CJ_COPY);
	/* the checked folder is moved away and a different one takes its name */
	swprintf(moved, 1100, L"%ls-checked", dst_dir);
	CHECK(MoveFileW(dst_dir, moved));
	CHECK(CreateDirectoryW(dst_dir, NULL));
	r = run(&spec, NULL, NULL);
	path_in(dst_dir, L"a.txt", inside);
	CHECK(0 == r.files_copied && 1 == r.files_failed);
	CHECK(r.failure_total >= 1 && WF_CF_DEST_CHANGED == r.failures[0].kind);
	CHECK(!exists(inside) && 0 == temporaries(dst_dir));
	path_in(moved, L"a.txt", inside);
	CHECK(!exists(inside));
	wf_copyresult_free(&r);
	/* the same for a destination that became a link to somewhere else */
	RemoveDirectoryW(dst_dir);
	{
		wchar_t cmd[2400];
		int code;

		swprintf(cmd, 2400,
		        L"cmd /c mklink /J \"%ls\" \"%ls\" >nul 2>nul", dst_dir,
		        moved);
		code = _wsystem(cmd);
		if (0 == code && exists(dst_dir)) {
			WfCopyResult r2 = run(&spec, NULL, NULL);

			path_in(moved, L"a.txt", inside);
			CHECK(WF_CS_FAILED == r2.state && !exists(inside));
			wf_copyresult_free(&r2);
			RemoveDirectoryW(dst_dir); /* the link itself */
		} else {
			printf("  NOT TESTED: a junction could not be made "
			       "here\n");
		}
	}
	CHECK(MoveFileW(moved, dst_dir));
	wf_copyspec_free(&spec);
}

typedef struct
{
	wchar_t from[1100], to[1100], inner_from[1100], inner_to[1100];
	int tried;
	bool moved_top, moved_inner;
} Swapper;

static void try_to_swap(void *ctx,
        size_t files_done,
        uint64_t bytes_done,
        const wchar_t *current)
{
	Swapper *w = ctx;

	(void)files_done;
	(void)bytes_done;
	(void)current;
	if (w->tried++) {
		return;
	}
	/* what an attacker would do between the check and the read */
	w->moved_inner = 0 != MoveFileW(w->inner_from, w->inner_to);
	w->moved_top = 0 != MoveFileW(w->from, w->to);
	if (w->moved_inner) {
		MoveFileW(w->inner_to, w->inner_from);
	}
	if (w->moved_top) {
		MoveFileW(w->to, w->from);
	}
}

/* While a folder is being copied, neither it nor a folder inside it can be
 * renamed or replaced by a link: the paths under it keep meaning it. */
static void a_source_folder_cannot_be_swapped_while_it_is_read(void)
{
	wchar_t tree[1100], sub[1100], f1[1100], f2[1100], f3[1100];
	const wchar_t *names[] = {L"tree"};
	WfCopySpec spec;
	WfCopyResult r;
	WfCopyControl ctl;
	Swapper w;

	fresh_dirs();
	path_in(src_dir, L"tree", tree);
	path_in(tree, L"sub", sub);
	path_in(tree, L"one.bin", f1);
	path_in(sub, L"two.bin", f2);
	path_in(sub, L"three.bin", f3);
	CHECK(CreateDirectoryW(tree, NULL) && CreateDirectoryW(sub, NULL));
	CHECK(make_pattern(f1, 3 << 20, 1) && make_pattern(f2, 3 << 20, 2) &&
	        make_pattern(f3, 3 << 20, 3));
	memset(&w, 0, sizeof w);
	wcscpy_s(w.from, 1100, tree);
	swprintf(w.to, 1100, L"%ls-away", tree);
	wcscpy_s(w.inner_from, 1100, sub);
	swprintf(w.inner_to, 1100, L"%ls-away", sub);
	memset(&ctl, 0, sizeof ctl);
	ctl.ctx = &w;
	ctl.progress = try_to_swap;
	spec = spec_for(
	        names, 1, WF_COLLIDE_STOP, WF_VERIFY_SHA256, WF_CJ_COPY);
	r = run(&spec, &ctl, NULL);
	CHECK(w.tried > 0 && !w.moved_top);
	CHECK(3 == r.files_copied && 0 == r.failure_total);
	wf_copyresult_free(&r);
	wf_copyspec_free(&spec);
}

static wchar_t g_swap_path[1100], g_swap_aside[1100];

static void swap_it(const wchar_t *path)
{
	if (wcscmp(path, g_swap_path)) {
		return;
	}
	/* between the check and the shell: the listed file goes away and another
	 * takes its name */
	MoveFileW(g_swap_path, g_swap_aside);
	put(g_swap_path, "impostor", 8);
}

static void the_object_the_shell_takes_must_be_the_one_that_was_checked(void)
{
	wchar_t a[1100];
	const wchar_t *names[] = {L"wfile-test-impostor.txt"};
	WfCopySpec spec;
	WfCopyResult r;
	WfRecycleReport rr;
	WfStatus st;
	WfNodeInfo n;

	fresh_dirs();
	path_in(src_dir, L"wfile-test-impostor.txt", a);
	CHECK(put(a, "the real one", 12));
	wcscpy_s(g_swap_path, 1100, a);
	swprintf(g_swap_aside, 1100, L"%ls.real", a);
	spec = spec_for(
	        names, 1, WF_COLLIDE_STOP, WF_VERIFY_NONE, WF_CJ_RECYCLE);
	free(spec.dest);
	spec.dest = NULL;
	wf_recycle_test_before_shell = swap_it;
	r = run(&spec, NULL, NULL);
	wf_recycle_test_before_shell = NULL;
	/* the real file was never touched; what was under the name is in the bin
	 * (restorable), and the job says it did not do what was asked */
	CHECK(exists(g_swap_aside) && !exists(a));
	CHECK(0 == r.sources_recycled && 1 == r.sources_kept);
	CHECK(WF_CF_REPLACED_IN_BIN == r.failures[0].kind);
	wf_copyresult_free(&r);
	wf_copyspec_free(&spec);

	/* the same through the call itself, with the report */
	CHECK(put(a, "again the real one", 18));
	DeleteFileW(g_swap_aside);
	CHECK(WF_OK == ops.node(NULL, a, &n));
	{
		WfNodeInfo dn;

		CHECK(WF_OK == ops.node(NULL, src_dir, &dn));
		CHECK(wf_recycle_thread_init());
		wf_recycle_test_before_shell = swap_it;
		st = wf_recycle(a, &n.id, &dn.id, &rr);
		wf_recycle_test_before_shell = NULL;
		wf_recycle_thread_done();
	}
	CHECK(WF_E_INVAL == st && rr.swapped);
	CHECK(exists(g_swap_aside));

	/* a folder that is not the one the listing proved is refused outright */
	{
		WfNodeInfo wrong = n;

		wrong.id.id[0] ^= 0x55;
		CHECK(wf_recycle_thread_init());
		st = wf_recycle(g_swap_aside, &n.id, &wrong.id, &rr);
		wf_recycle_thread_done();
		CHECK(WF_E_INVAL == st && exists(g_swap_aside));
	}
}

int main(int argc, char **argv)
{
	wchar_t **wargv;
	int wargc;
	bool recycle_available = true;

	wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
	wcscpy_s(g_exe_w, 1024, wargv[0]);
	if (6 == argc && !strcmp(argv[1], "--child-over")) {
		return child_overwrite_main(
		        wargv[2], wargv[3], wargv[4], wargv[5]);
	}
	if (6 == argc && !strcmp(argv[1], "--child")) {
		return child_main(wargv[2], wargv[3], wargv[4], wargv[5]);
	}
	ops = wf_probe_win();
	CHECK(p0_lab_open(&lab, "copyengine", 0));
	if (!lab.area) {
		return TEST_RESULT();
	}
	wcscpy_s(root, 1024, lab.root);
	while (wcslen(root) > 3 && L'\\' == root[wcslen(root) - 1]) {
		root[wcslen(root) - 1] = 0;
	}
	{
		WfNodeInfo node;
		WfRecycleReport capability;
		CHECK(WF_OK == ops.node(NULL, root, &node));
		WfStatus check =
		        wf_recycle_preflight(root, &node.id, &capability);
		if (WF_E_ACCESS == check &&
		        (WF_RP_BIN_QUERY == capability.phase ||
		                WF_RP_BIN_POLICY == capability.phase)) {
			recycle_available = false;
			printf("BLOCKED: real Recycle Bin prerequisite: phase "
			       "%d HRESULT %08lx Win32 %lu; copy-only cases "
			       "still run\n",
			        (int)capability.phase,
			        (unsigned long)capability.hresult,
			        capability.error);
		} else {
			CHECK(WF_OK == check);
		}
	}

	a_file_is_copied_exactly_with_what_a_copy_can_keep();
	a_tree_is_copied_with_every_file_compared();
	weak_identities_take_part_in_a_plain_copy_only();
	nothing_at_the_destination_is_lost_or_merged();
	a_folder_that_is_there_is_merged_and_files_follow_the_policy();
	a_merge_names_each_parent_and_stages_new_folders();
	a_junction_or_the_same_folder_is_not_entered_by_a_merge();
	keep_both_gives_the_new_ones_the_next_free_name();
	every_item_a_job_made_is_registered_once();
	a_source_that_is_not_what_was_chosen_is_not_copied();
	a_source_rewritten_to_the_same_size_is_not_copied();
	a_folder_with_a_very_long_path_is_copied();
	a_failure_below_a_sub_folder_keeps_its_full_path();
	cancelling_leaves_nothing_behind();
	a_temporary_without_an_identity_is_kept_and_recorded();
	an_unfinished_folder_is_not_left_behind();
	an_empty_folder_is_removed_only_on_its_own_identity();
	verification_choices_follow_the_plan();
	links_are_not_followed_and_say_so();
	if (recycle_available) {
		a_move_removes_the_source_only_after_a_verified_copy();
		recycling_checks_what_it_is_asked_to_remove();
		recycling_a_folder_and_the_bin_limits();
	}
	the_destination_must_be_the_folder_that_was_checked();
	a_source_folder_cannot_be_swapped_while_it_is_read();
	if (recycle_available) {
		the_object_the_shell_takes_must_be_the_one_that_was_checked();
	}
	the_journal_resolves_what_a_dead_writer_left();
	a_killed_copy_leaves_only_what_the_journal_can_remove();
	a_moved_folder_that_changed_meanwhile_is_kept();
	a_dead_writers_old_version_is_put_back_never_deleted();
	a_copy_killed_between_the_two_renames_keeps_the_old_file();
	clean_up();
	{
		static const wchar_t *const extra[] = {L"journal-a.txt",
		        L"journal-b.txt", L"marker-b.txt", L"journal-c.txt",
		        L"journal-d.txt", L"marker-d.txt"};
		size_t i;

		for (i = 0; i < WF_ARRAY_LEN(extra); i++) {
			wchar_t path[1100];

			swprintf(path, 1100, L"%ls\\%ls", root, extra[i]);
			DeleteFileW(path);
		}
	}
	CHECK(0 == p0_lab_close(&lab, 0 != wf_test_failures));
	if (!wf_test_failures && !recycle_available) {
		puts("BLOCKED: non-Recycle integration cases passed; real "
		     "Move/Recycle cases NOT RUN");
		return 77;
	}
	return TEST_RESULT();
}
