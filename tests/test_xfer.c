#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "xfer/wf_preflight_internal.h"
#include "wf_fake_probe.h"
#include "wf_test.h"

static FakeProbe P; /* too large for the stack */
static WfProbeOps OPS;

static void fixture(void)
{
	fp_init(&P);
	OPS = fp_ops(&P);
	fp_dir(&P, L"C:\\src", 10);
	fp_file(&P, L"C:\\src\\a.txt", 100, 11);
	fp_file(&P, L"C:\\src\\b.txt", 200, 12);
	fp_dir(&P, L"C:\\src\\tree", 13);
	fp_file(&P, L"C:\\src\\tree\\x.bin", 1000, 14);
	fp_dir(&P, L"C:\\src\\tree\\sub", 15);
	fp_file(&P, L"C:\\src\\tree\\sub\\y.bin", 50, 16);
	fp_dir(&P, L"C:\\dst", 20);
}

static WfTransferSlot capture(
        WfTransferMode m, const wchar_t *const *names, size_t n)
{
	WfTransferSlot s;

	CHECK(WF_OK == wf_xfer_capture(&OPS, m, L"C:\\src", names, n, 0, &s));
	return s;
}

static WfPreflightReport run(const WfTransferSlot *s, const wchar_t *dest)
{
	WfPreflightReport r;

	CHECK(wf_preflight_run(
	              &OPS, s, dest, &WF_PREFLIGHT_DEFAULT, NULL, &r) == WF_OK);
	return r;
}

static const wchar_t *const N_A[] = {L"a.txt"};
static const wchar_t *const N_AB[] = {L"a.txt", L"b.txt"};
static const wchar_t *const N_TREE[] = {L"tree"};
static const wchar_t *const N_ALL[] = {L"a.txt", L"tree"};

static void test_capture(void)
{
	WfTransferSlot s, c;

	fixture();
	s = capture(WF_XFER_COPY, N_ALL, 2);
	CHECK(2 == s.count && WF_XFER_COPY == s.mode &&
	        0 == wcscmp(s.dir, L"C:\\src"));
	CHECK(100 == s.src[0].snap.size &&
	        (s.src[0].snap.id.kind == WF_ID_STRONG));
	CHECK(s.src[1].snap.flags & WF_ENT_DIR);
	CHECK((s.dir_id.kind == WF_ID_STRONG));

	CHECK(WF_OK == wf_slot_clone(&c, &s));
	CHECK(2 == c.count && c.src[0].name != s.src[0].name &&
	        0 == wcscmp(c.src[1].name, L"tree"));
	wf_slot_free(&c);
	wf_slot_free(&s);
	CHECK(0 == s.count && NULL == s.dir);
	wf_slot_free(&s); /* idempotent */

	/* any unreadable source fails the whole capture and leaves nothing */
	{
		const wchar_t *const bad[] = {L"a.txt", L"ghost"};

		CHECK(wf_xfer_capture(&OPS, WF_XFER_COPY, L"C:\\src", bad, 2, 0,
		              &s) == WF_E_NOENT);
		CHECK(0 == s.count && NULL == s.src && NULL == s.dir);
	}
	CHECK(WF_E_INVAL ==
	        wf_xfer_capture(&OPS, WF_XFER_COPY, L"C:\\src", N_A, 0, 0, &s));
	CHECK(WF_E_INVAL ==
	        wf_xfer_capture(&OPS, WF_XFER_NONE, L"C:\\src", N_A, 1, 0, &s));
}

static void test_ok_copy(void)
{
	WfTransferSlot s;
	WfPreflightReport r;

	fixture();
	s = capture(WF_XFER_COPY, N_ALL, 2);
	r = run(&s, L"C:\\dst");
	CHECK(WF_PF_OK == r.reason);
	CHECK(1 == r.top_files && 1 == r.top_dirs);
	CHECK(3 == r.tree_files &&
	        2 == r.tree_dirs); /* a.txt, x.bin, y.bin / tree, sub */
	CHECK(1150 == r.total_bytes);
	CHECK(r.auto_recovery && 0 == wcscmp(r.fs_name, L"NTFS"));
	CHECK(r.need_bytes == 1150 + WF_PREFLIGHT_DEFAULT.reserve_bytes);
	/* "C:\\dst" ext = 4+6 = 10, + 1 + "tree\\sub\\y.bin"(15) */
	CHECK(r.longest_final == 10 + 1 + 14);
	CHECK(80 ==
	        r.longest_txn); /* dest(10) + txn/payload(60) + descendant tail(10) */
	wf_slot_free(&s);
}

static void test_collision(void)
{
	WfTransferSlot s;
	WfPreflightReport r;

	fixture();
	fp_file(&P, L"C:\\dst\\b.txt", 5,
	        30); /* only the second one collides */
	s = capture(WF_XFER_COPY, N_AB, 2);
	r = run(&s, L"C:\\dst");
	CHECK(WF_PF_COLLISION == r.reason && 1 == r.bad_index);
	CHECK(0 == wcscmp(r.bad_name, L"b.txt"));
	wf_slot_free(&s);

	/* case-insensitive filesystem: B.TXT is the same name */
	fixture();
	fp_file(&P, L"C:\\dst\\B.TXT", 5, 30);
	s = capture(WF_XFER_COPY, N_AB, 2);
	CHECK(WF_PF_COLLISION == run(&s, L"C:\\dst").reason);
	wf_slot_free(&s);

	/* a dangling link is still an existing name */
	fixture();
	{
		FpNode *n = fp_add(&P, L"C:\\dst\\a.txt", WF_ENT_LINK, 0, 31);

		n->tag = 0xA000000C;
	}
	s = capture(WF_XFER_COPY, N_A, 1);
	CHECK(WF_PF_COLLISION == run(&s, L"C:\\dst").reason);
	wf_slot_free(&s);

	/* an I/O error while probing the name is not "absent" */
	fixture();
	fp_add(&P, L"C:\\dst\\a.txt", 0, 1, 32)->io_error = 1;
	s = capture(WF_XFER_COPY, N_A, 1);
	CHECK(WF_PF_IO_ERROR == run(&s, L"C:\\dst").reason);
	wf_slot_free(&s);

	/* same folder is called out, for both modes */
	fixture();
	s = capture(WF_XFER_COPY, N_A, 1);
	CHECK(WF_PF_SAME_FOLDER == run(&s, L"C:\\src").reason);
	CHECK(WF_PF_SAME_FOLDER == run(&s, L"C:\\SRC\\").reason);
	s.mode = WF_XFER_MOVE;
	CHECK(WF_PF_SAME_FOLDER == run(&s, L"C:\\src").reason);
	wf_slot_free(&s);
}

static void test_destination_inside_source(void)
{
	WfTransferSlot s;

	fixture();
	s = capture(WF_XFER_COPY, N_TREE, 1);
	CHECK(WF_PF_DEST_INSIDE_SOURCE == run(&s, L"C:\\src\\tree").reason);
	CHECK(WF_PF_DEST_INSIDE_SOURCE ==
	        run(&s, L"C:\\src\\tree\\sub").reason);
	wf_slot_free(&s);

	/* reached through a SUBST-style alias (not a reparse point, so it is
	 * not refused as a link): the resolved file ID still gives it away */
	fixture();
	s = capture(WF_XFER_COPY, N_TREE, 1);
	{
		/* it has an ID of its own, but resolves to tree's */
		FpNode *j = fp_add(&P, L"C:\\alias", WF_ENT_DIR, 0, 99);

		j->resolve_to = 13;
	}
	CHECK(WF_PF_DEST_INSIDE_SOURCE == run(&s, L"C:\\alias").reason);
	/* ...and a folder below the alias is inside too */
	fp_dir(&P, L"C:\\alias\\deeper", 98);
	CHECK(WF_PF_DEST_INSIDE_SOURCE == run(&s, L"C:\\alias\\deeper").reason);
	/* an alias of the source folder itself is the same folder */
	{
		const wchar_t *const na[] = {L"a.txt"};
		FpNode *j;

		wf_slot_free(&s);
		j = fp_add(&P, L"C:\\srclink", WF_ENT_DIR, 0, 97);
		j->resolve_to = 10; /* C:\src */
		s = capture(WF_XFER_COPY, na, 1);
		CHECK(WF_PF_SAME_FOLDER == run(&s, L"C:\\srclink").reason);
	}
	wf_slot_free(&s);

	/* a sibling whose name merely starts the same is fine */
	fixture();
	fp_dir(&P, L"C:\\src\\tree2", 40);
	s = capture(WF_XFER_COPY, N_TREE, 1);
	CHECK(WF_PF_OK == run(&s, L"C:\\src\\tree2").reason);
	wf_slot_free(&s);
}

static void test_source_identity(void)
{
	WfTransferSlot s;

	fixture();
	s = capture(WF_XFER_COPY, N_AB, 2);

	P.n[2].size = 101; /* a.txt grew after `y` */
	CHECK(WF_PF_SOURCE_CHANGED == run(&s, L"C:\\dst").reason);
	P.n[2].size = 100;
	CHECK(WF_PF_OK == run(&s, L"C:\\dst").reason);

	P.n[2].mtime = 2000; /* touched */
	CHECK(WF_PF_SOURCE_CHANGED == run(&s, L"C:\\dst").reason);
	P.n[2].mtime = 1000;

	P.n[2].id = 99; /* replaced by a different file of the same name */
	CHECK(WF_PF_SOURCE_CHANGED == run(&s, L"C:\\dst").reason);
	P.n[2].id = 11;

	P.n[2].flags = WF_ENT_DIR; /* file became a directory */
	CHECK(WF_PF_SOURCE_CHANGED == run(&s, L"C:\\dst").reason);
	P.n[2].flags = 0;

	P.n[3].io_error = 1;
	CHECK(WF_PF_IO_ERROR == run(&s, L"C:\\dst").reason);
	P.n[3].io_error = 0;

	/* deleted */
	P.n[3].path[0] = L'Z';
	{
		WfPreflightReport r = run(&s, L"C:\\dst");

		CHECK(WF_PF_SOURCE_MISSING == r.reason && 1 == r.bad_index);
	}
	wf_slot_free(&s);

	/* without file IDs (FAT-like) attributes alone must still catch changes
	 */
	fixture();
	fp_add(&P, L"C:\\src\\f.txt", 0, 7, 0);
	{
		const wchar_t *const nf[] = {L"f.txt"};

		s = capture(WF_XFER_COPY, nf, 1);
		CHECK(!(s.src[0].snap.id.kind == WF_ID_STRONG));
		CHECK(WF_PF_OK == run(&s, L"C:\\dst").reason);
		fp_find(&P, L"C:\\src\\f.txt")->size = 8;
		CHECK(WF_PF_SOURCE_CHANGED == run(&s, L"C:\\dst").reason);
	}
	wf_slot_free(&s);
}

static void test_reparse(void)
{
	WfTransferSlot s;
	FpNode *n;

	/* a symlink somewhere in the tree refuses the whole copy */
	fixture();
	n = fp_add(&P, L"C:\\src\\tree\\sub\\lnk", WF_ENT_LINK, 0, 50);
	n->tag = 0xA000000C;
	s = capture(WF_XFER_COPY, N_TREE, 1);
	{
		WfPreflightReport r = run(&s, L"C:\\dst");

		CHECK(WF_PF_REPARSE == r.reason &&
		        0 == wcscmp(r.bad_name, L"lnk"));
	}
	wf_slot_free(&s);

	/* so does a junction, and an unknown tag */
	fixture();
	n = fp_add(&P, L"C:\\src\\tree\\j", WF_ENT_LINK | WF_ENT_DIR, 0, 51);
	n->tag = 0xA0000003;
	s = capture(WF_XFER_COPY, N_TREE, 1);
	CHECK(WF_PF_REPARSE == run(&s, L"C:\\dst").reason);
	n->tag = 0x80000023; /* AF_UNIX: not on the allowlist */
	CHECK(WF_PF_REPARSE == run(&s, L"C:\\dst").reason);
	wf_slot_free(&s);

	/* transparent kinds pass and are counted */
	fixture();
	n = fp_add(&P, L"C:\\src\\tree\\w.dat", WF_ENT_LINK, 10, 52);
	n->tag = 0x80000017; /* WOF */
	n = fp_add(&P, L"C:\\src\\tree\\d.dat", WF_ENT_LINK, 10, 53);
	n->tag = 0x80000013; /* dedup */
	n = fp_add(&P, L"C:\\src\\tree\\c.dat", WF_ENT_LINK, 10, 54);
	n->tag = 0x9000701A; /* CLOUD_7 */
	s = capture(WF_XFER_COPY, N_TREE, 1);
	{
		WfPreflightReport r = run(&s, L"C:\\dst");

		CHECK(WF_PF_OK == r.reason && 3 == r.tree_links);
	}
	wf_slot_free(&s);

	/* a top-level symlink: MOVE renames the link itself, COPY refuses */
	fixture();
	n = fp_add(&P, L"C:\\src\\ln", WF_ENT_LINK, 0, 55);
	n->tag = 0xA000000C;
	{
		const wchar_t *const nl[] = {L"ln"};

		s = capture(WF_XFER_COPY, nl, 1);
		CHECK(WF_PF_REPARSE == run(&s, L"C:\\dst").reason);
		s.mode = WF_XFER_MOVE;
		CHECK(WF_PF_OK == run(&s, L"C:\\dst").reason);
		n->tag = 0x80000023; /* unknown: refused even for MOVE */
		wf_slot_free(&s);
		s = capture(WF_XFER_MOVE, nl, 1);
		CHECK(WF_PF_REPARSE == run(&s, L"C:\\dst").reason);
	}
	wf_slot_free(&s);

	/* classification table */
	CHECK(WF_REPARSE_NONE == wf_reparse_classify(0, 0xA000000C));
	CHECK(WF_REPARSE_TRANSPARENT ==
	        wf_reparse_classify(WF_ENT_LINK, 0x9000001A));
	CHECK(WF_REPARSE_TRANSPARENT ==
	        wf_reparse_classify(WF_ENT_LINK, 0x9000F01A));
	CHECK(WF_REPARSE_UNKNOWN ==
	        wf_reparse_classify(WF_ENT_LINK, 0x9000001B));
	CHECK(WF_REPARSE_REDIRECT ==
	        wf_reparse_classify(WF_ENT_LINK, 0xA0000003));
}

static void test_cloud_space_limits(void)
{
	WfTransferSlot s;
	WfPreflightReport r;
	WfPreflightPolicy pol = WF_PREFLIGHT_DEFAULT;

	fixture();
	fp_add(&P, L"C:\\src\\tree\\cloud.bin", WF_ENT_CLOUD, 5000, 60);
	s = capture(WF_XFER_COPY, N_TREE, 1);
	r = run(&s, L"C:\\dst");
	CHECK(WF_PF_OK == r.reason && 1 == r.cloud_files &&
	        5000 == r.cloud_bytes);
	CHECK(r.total_bytes == 1000 + 50 + 5000);
	wf_slot_free(&s);

	/* space: free must cover data + reserve */
	fixture();
	s = capture(WF_XFER_COPY, N_AB, 2);
	P.vols[0].info.free_bytes = 300 + pol.reserve_bytes - 1;
	CHECK(WF_PF_NO_SPACE == run(&s, L"C:\\dst").reason);
	P.vols[0].info.free_bytes = 300 + pol.reserve_bytes;
	CHECK(WF_PF_OK == run(&s, L"C:\\dst").reason);
	wf_slot_free(&s);

	/* path overhead: legal final path, but the transaction path would not
	 * fit */
	fixture();
	s = capture(WF_XFER_COPY, N_TREE, 1);
	pol.max_path_units = 80; /* dest(10) + txn/payload(60) + tail(10) */
	CHECK(WF_OK == wf_preflight_run(&OPS, &s, L"C:\\dst", &pol, NULL, &r));
	CHECK(WF_PF_OK == r.reason);
	pol.max_path_units--;
	CHECK(WF_OK == wf_preflight_run(&OPS, &s, L"C:\\dst", &pol, NULL, &r));
	CHECK(WF_PF_PATH_TOO_LONG == r.reason &&
	        0 == wcscmp(r.bad_name, L"y.bin"));
	/* the final path alone would have fit, which is the point of S10 */
	CHECK(r.longest_final <= pol.max_path_units &&
	        r.longest_txn > pol.max_path_units);
	wf_slot_free(&s);

	/* component longer than the destination volume allows */
	fixture();
	s = capture(WF_XFER_COPY, N_TREE, 1);
	P.vols[0].info.max_component = 4;
	CHECK(WF_PF_COMPONENT_TOO_LONG ==
	        run(&s, L"C:\\dst").reason); /* "tree" ok, "x.bin" no */
	wf_slot_free(&s);

	/* UNC destinations count the longer extended prefix */
	fixture();
	fp_dir(&P, L"C:\\src\\u", 70);
	fp_add(&P, L"C:\\src\\u\\f", 0, 1, 71);
	{
		const wchar_t *const nu[] = {L"u"};
		WfPreflightPolicy tight = WF_PREFLIGHT_DEFAULT;

		s = capture(WF_XFER_COPY, nu, 1);
		fp_dir(&P, L"C:\\dst\\x", 72);
		/* C:\dst\x extended: 12; txn/payload: 60; tail "\f": 2. */
		tight.max_path_units = 74;
		CHECK(wf_preflight_run(&OPS, &s, L"C:\\dst\\x", &tight, NULL,
		              &r) == WF_OK &&
		        WF_PF_OK == r.reason);
	}
	wf_slot_free(&s);
}

static void test_destination_checks(void)
{
	WfTransferSlot s;

	fixture();
	s = capture(WF_XFER_COPY, N_A, 1);

	CHECK(WF_PF_DEST_NOT_DIR == run(&s, L"C:\\nowhere").reason);
	CHECK(WF_PF_DEST_NOT_DIR == run(&s, L"C:\\src\\a.txt").reason);

	P.access_status = WF_E_ACCESS;
	CHECK(WF_PF_DEST_ACCESS == run(&s, L"C:\\dst").reason);
	P.access_status = WF_OK;

	P.vols[0].info.read_only = 1;
	CHECK(WF_PF_VOLUME_UNSUPPORTED == run(&s, L"C:\\dst").reason);
	P.vols[0].info.read_only = 0;

	/* FAT-like destinations are allowed but flagged: no automatic recovery
	 */
	wcscpy_s(P.vols[0].info.fs_name, 16, L"exFAT");
	{
		WfPreflightReport r = run(&s, L"C:\\dst");

		CHECK(WF_PF_OK == r.reason && !r.auto_recovery);
	}
	wcscpy_s(P.vols[0].info.fs_name, 16, L"NTFS");
	P.vols[0].info.is_local = 0; /* SMB */
	CHECK(!run(&s, L"C:\\dst").auto_recovery);
	P.vols[0].info.is_local = 1;
	P.vols[0].info.supports_open_by_id = 0;
	CHECK(!run(&s, L"C:\\dst").auto_recovery);
	P.vols[0].info.supports_open_by_id = 1;

	wf_slot_free(&s);

	/* empty slot */
	{
		WfTransferSlot e;
		WfPreflightReport r;

		wf_slot_init(&e);
		CHECK(wf_preflight_run(&OPS, &e, L"C:\\dst",
		              &WF_PREFLIGHT_DEFAULT, NULL, &r) == WF_OK);
		CHECK(WF_PF_EMPTY_SLOT == r.reason);
	}
}

static void test_destination_links_refused(void)
{
	WfTransferSlot s;
	WfPreflightReport r;
	FpNode *n;

	/* the destination folder itself */
	fixture();
	s = capture(WF_XFER_COPY, N_A, 1);
	n = fp_add(&P, L"C:\\jdst", WF_ENT_DIR | WF_ENT_LINK, 0, 80);
	n->tag = 0xA0000003; /* mount point */
	r = run(&s, L"C:\\jdst");
	CHECK(WF_PF_DEST_REPARSE == r.reason &&
	        0 == wcscmp(r.bad_name, L"jdst"));

	/* ...a symlink... */
	n->tag = 0xA000000C;
	CHECK(WF_PF_DEST_REPARSE == run(&s, L"C:\\jdst").reason);

	/* ...an ancestor of it, however deep */
	fp_dir(&P, L"C:\\jdst\\sub", 81);
	fp_dir(&P, L"C:\\jdst\\sub\\deep", 82);
	r = run(&s, L"C:\\jdst\\sub\\deep");
	CHECK(WF_PF_DEST_REPARSE == r.reason &&
	        0 == wcscmp(r.bad_name, L"jdst"));

	/* kinds that are fine as *sources* are not fine as destinations */
	n->tag = 0x9000001A; /* cloud */
	CHECK(WF_PF_DEST_REPARSE == run(&s, L"C:\\jdst").reason);
	n->tag = 0x80000023; /* unknown */
	CHECK(WF_PF_DEST_REPARSE == run(&s, L"C:\\jdst").reason);

	/* an ancestor that cannot be examined is not assumed clean */
	n->flags = WF_ENT_DIR; /* no longer a link */
	n->io_error = 1;
	CHECK(WF_PF_IO_ERROR == run(&s, L"C:\\jdst\\sub").reason);
	n->io_error = 0;
	CHECK(WF_PF_OK == run(&s, L"C:\\jdst\\sub").reason);

	/* a link elsewhere (the source side) is a different rule */
	wf_slot_free(&s);
}

static void test_cross_volume(void)
{
	WfTransferSlot s;
	WfPreflightReport r;

	fixture();
	P.nvols = 2;
	wcscpy_s(P.vols[1].prefix, 64, L"D:");
	P.vols[1].info = P.vols[0].info;
	wcscpy_s(P.vols[1].info.root, 260, L"D:\\");
	P.vols[1].info.serial = 0xBBBB;
	fp_add(&P, L"D:\\", WF_ENT_DIR, 0, 1)->vol = 2;
	fp_add(&P, L"D:\\dst", WF_ENT_DIR, 0, 90)->vol = 2;

	s = capture(WF_XFER_MOVE, N_AB, 2);
	r = run(&s, L"D:\\dst");
	CHECK(WF_PF_CROSS_VOLUME == r.reason);

	/* the same request as a COPY is fine */
	s.mode = WF_XFER_COPY;
	CHECK(WF_PF_OK == run(&s, L"D:\\dst").reason);
	s.mode = WF_XFER_MOVE;
	CHECK(WF_PF_OK == run(&s, L"C:\\dst").reason);

	/* no file IDs: fall back to volume serial + root */
	P.n[2].id = 0;
	P.n[3].id = 0;
	wf_slot_free(&s);
	s = capture(WF_XFER_MOVE, N_AB, 2);
	CHECK(WF_PF_CROSS_VOLUME == run(&s, L"D:\\dst").reason);
	CHECK(WF_PF_OK == run(&s, L"C:\\dst").reason);
	wf_slot_free(&s);
}

static void test_tree_edges(void)
{
	WfTransferSlot s;
	WfPreflightReport r;

	/* a directory too big to list completely: refuse, do not guess */
	fixture();
	fp_find(&P, L"C:\\src\\tree\\sub")->list_truncated = 1;
	s = capture(WF_XFER_COPY, N_TREE, 1);
	CHECK(WF_PF_TREE_TOO_LARGE == run(&s, L"C:\\dst").reason);
	wf_slot_free(&s);

	/* an unreadable subdirectory */
	fixture();
	fp_find(&P, L"C:\\src\\tree\\sub")->list_error = 1;
	s = capture(WF_XFER_COPY, N_TREE, 1);
	r = run(&s, L"C:\\dst");
	CHECK(WF_PF_IO_ERROR == r.reason && 0 == wcscmp(r.bad_name, L"sub"));
	wf_slot_free(&s);

	/* empty directory copies fine */
	fixture();
	fp_dir(&P, L"C:\\src\\empty", 95);
	{
		const wchar_t *const ne[] = {L"empty"};

		s = capture(WF_XFER_COPY, ne, 1);
		r = run(&s, L"C:\\dst");
		CHECK(WF_PF_OK == r.reason && 1 == r.tree_dirs &&
		        0 == r.total_bytes);
	}
	wf_slot_free(&s);

	/* two sources that fold to one destination name */
	fixture();
	fp_file(&P, L"C:\\src\\A.TXT", 1,
	        96); /* distinct on a case-sensitive source */
	{
		const wchar_t *const nn[] = {L"a.txt", L"A.TXT"};

		s = capture(WF_XFER_COPY, nn, 2);
		r = run(&s, L"C:\\dst");
		CHECK(WF_PF_COLLISION == r.reason && 1 == r.bad_index);
	}
	wf_slot_free(&s);
}

static void test_deep_tree_does_not_recurse(void)
{
	WfTransferSlot s;
	WfPreflightReport r;
	wchar_t path[300] = L"C:\\src\\deep";
	int i;

	fixture();
	fp_dir(&P, path, 100);
	/* the fake caps path length at 299; use short names, depth well past
	 * anything a recursive implementation could survive on a small stack */
	for (i = 0; i < 140; i++) {
		wcscat_s(path, 300, L"\\d");
		fp_dir(&P, path, 200 + (uint64_t)i);
	}
	{
		const wchar_t *const nd[] = {L"deep"};

		s = capture(WF_XFER_COPY, nd, 1);
		r = run(&s, L"C:\\dst");
		CHECK(WF_PF_OK == r.reason && 141 == r.tree_dirs);
	}
	wf_slot_free(&s);
}

static bool always_cancelled(void *ctx)
{
	(void)ctx;
	return 1;
}

static void test_cancel(void)
{
	WfTransferSlot s;
	WfPreflightReport r;
	WfCancel c = {NULL, always_cancelled};

	fixture();
	s = capture(WF_XFER_COPY, N_TREE, 1);
	CHECK(wf_preflight_run(&OPS, &s, L"C:\\dst", &WF_PREFLIGHT_DEFAULT, &c,
	              &r) == WF_OK);
	CHECK(WF_PF_CANCELLED == r.reason);
	wf_slot_free(&s);
}

static void test_reason_texts(void)
{
	int r;

	for (r = 0; r <= WF_PF_CANCELLED; r++) {
		CHECK(0 != strcmp(wf_pf_reason_text((WfPfReason)r), "unknown"));
	}
	CHECK(0 == strcmp(wf_pf_reason_text((WfPfReason)999), "unknown"));
}

static WfStatus no_such_node(void *ctx, const wchar_t *path, WfNodeInfo *out)
{
	(void)ctx;
	(void)path;
	(void)out;
	return WF_E_NOENT; /* nothing in the destination */
}

/* A slot of `n` sources named "f<number>.txt", with the name of source `dup_at`
 * written again in the other case at the end; the check that two sources never
 * land on one name must find it without comparing every name with every other. */
static void names_collide_among_many(
        size_t n, size_t dup_at, size_t want_index, double *ms_out)
{
	WfTransferSlot s;
	WfProbeOps ops;
	WfPreflightReport rep;
	WfPfWalk w;
	clock_t t0;
	WfStatus st;

	memset(&s, 0, sizeof s);
	memset(&ops, 0, sizeof ops);
	ops.node = no_such_node;
	s.count = n;
	s.src = calloc(n, sizeof *s.src);
	CHECK(s.src != NULL);
	for (size_t i = 0; i < n; i++) {
		wchar_t name[32];

		swprintf(name, 32, L"f%07zu.txt", i);
		s.src[i].name = _wcsdup(name);
	}
	if (dup_at < n) {
		wchar_t name[32];

		swprintf(name, 32, L"F%07zu.TXT", dup_at);
		free(s.src[n - 1].name);
		s.src[n - 1].name = _wcsdup(name); /* the last, in other case */
	}
	memset(&rep, 0, sizeof rep);
	memset(&w, 0, sizeof w);
	rep.reason = WF_PF_OK;
	w.ops = &ops;
	w.rep = &rep;
	w.max_component = 255;
	t0 = clock();
	st = wf_pf_check_collisions(&s, L"C:\\dst", false, &w);
	*ms_out = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
	CHECK(WF_OK == st);
	if (want_index < n) {
		CHECK(WF_PF_COLLISION == rep.reason &&
		        want_index == rep.bad_index);
	} else {
		CHECK(WF_PF_OK == rep.reason);
	}
	for (size_t i = 0; i < n; i++) {
		free(s.src[i].name);
	}
	free(s.src);
}

static void test_many_sources(void)
{
	enum
	{
		N = 30000
	};
	double ms, ignore;

	/* the duplicate is the last: the slot's first source to have a name an earlier
	 * one has is that one */
	names_collide_among_many(N, 12345, N - 1, &ms);
	printf("info: %d sources, the last one a duplicate: %.1f ms\n", (int)N,
	        ms);
	CHECK(ms < 1000.0);
	names_collide_among_many(N, N, N, &ignore); /* none: all distinct */
	/* small cases: the verdict and the place do not change */
	names_collide_among_many(2, 0, 1, &ignore);
	names_collide_among_many(3, 1, 2, &ignore);
	names_collide_among_many(1, 0, 1, &ignore);
}

int main(void)
{
	test_capture();
	test_ok_copy();
	test_collision();
	test_many_sources();
	test_destination_inside_source();
	test_source_identity();
	test_reparse();
	test_cloud_space_limits();
	test_destination_checks();
	test_destination_links_refused();
	test_cross_volume();
	test_tree_edges();
	test_deep_tree_does_not_recurse();
	test_cancel();
	test_reason_texts();
	return TEST_RESULT();
}
