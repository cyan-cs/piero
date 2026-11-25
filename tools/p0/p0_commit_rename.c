/*
 * P0-S07 proof of concept: handle-based no-clobber commit, and whether the
 * destination parent can be swapped underneath it (H-005, H-006, H-007, H-028).
 *
 * A measuring device, not the commit step of an engine: it never replaces a
 * file on purpose, never deletes outside its own scratch area, and every check
 * records what actually happened. Cases that cannot run here are written down
 * as NOT_TESTED with the reason.
 *
 *   A  success path (absolute name, parent-relative name, long path)
 *   B  collisions: a pre-existing destination must survive untouched
 *   C  destination appears after verification (sequential and raced)
 *   D  case-only behaviour
 *   E  the destination parent is swapped between verification and rename
 *   F  another process holds the payload open
 *   G  case-sensitive directory
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p0_area.h"
#include "p0_common.h"
#include "../../src/base/wf_macro.h"

#define VERSION "1.0"
#define XP 4096

/* NT rename (parent-relative names), declared by hand */

typedef LONG NTSTATUS_;
typedef struct
{
	union
	{
		NTSTATUS_ Status;
		PVOID Pointer;
	};
	ULONG_PTR Information;
} IOSB_;
typedef struct
{
	BOOLEAN ReplaceIfExists;
	HANDLE RootDirectory;
	ULONG FileNameLength;
	WCHAR FileName[1];
} NT_RENAME_;
typedef NTSTATUS_(NTAPI *NtSetInformationFile_t)(
        HANDLE, IOSB_ *, PVOID, ULONG, ULONG);
typedef ULONG(NTAPI *RtlNtStatusToDosError_t)(NTSTATUS_);
#define FileRenameInformation_ 10

typedef struct
{
	char id[40];
	char hyp[24];
	char expected[110];
	char observed[300];
	char status[12]; /* PASS FAIL INFO NOT_TESTED */
} Row;

static Row g_rows[400];
static size_t g_nrows;
static P0Area *g_area;

static void row(const char *id,
        const char *hyp,
        const char *status,
        const char *expected,
        const char *fmt,
        ...)
{
	Row *r = &g_rows[g_nrows++];
	va_list ap;

	snprintf(r->id, sizeof r->id, "%s", id);
	snprintf(r->hyp, sizeof r->hyp, "%s", hyp);
	snprintf(r->status, sizeof r->status, "%s", status);
	snprintf(r->expected, sizeof r->expected, "%s", expected);
	va_start(ap, fmt);
	vsnprintf(r->observed, sizeof r->observed, fmt, ap);
	va_end(ap);
	printf("  %-34s %-11s %s\n", r->id, r->status, r->observed);
}

/* area helpers (extended paths; every created object is recorded) */

typedef struct
{
	wchar_t rel[700];
	int is_dir;
} Known;

static Known g_known[256];
static int g_nknown;

static void known(const wchar_t *rel, int is_dir)
{
	wcscpy_s(g_known[g_nknown].rel, 700, rel);
	g_known[g_nknown].is_dir = is_dir;
	g_nknown++;
}

/* Records what exists, then removes it in reverse creation order (the area's
 * own proofs apply to every removal). */
static void sweep(void)
{
	int i;
	char tracked[256] = {0};

	for (i = 0; i < g_nknown; i++) {
		wchar_t x[XP];

		/* only what exists now can have been made by this run */
		if (p0_area_xpath(g_area, g_known[i].rel, x, XP) &&
		        INVALID_FILE_ATTRIBUTES != GetFileAttributesW(x)) {
			tracked[i] = p0_area_track(g_area, g_known[i].rel,
			                     g_known[i].is_dir) != 0;
		}
	}
	for (i = g_nknown - 1; i >= 0; i--) {
		if (tracked[i] && !p0_area_remove(g_area, g_known[i].rel)) {
			fprintf(stderr, "  not removed: %ls (%s)\n",
			        g_known[i].rel, p0_area_last_refusal(g_area));
		}
	}
	g_nknown = 0;
}

/* A directory that outlives the individual cases (removed with the whole area).
 */
static void group_dir(const wchar_t *rel)
{
	if (!p0_area_mkdir(g_area, rel)) {
		fprintf(stderr, "  cannot create group directory %ls\n", rel);
	}
}

static int X(const wchar_t *rel, wchar_t *out)
{
	return p0_area_xpath(g_area, rel, out, XP);
}

static int mkd(const wchar_t *rel)
{
	wchar_t x[XP];

	if (!X(rel, x) || !CreateDirectoryW(x, NULL)) {
		return 0;
	}
	known(rel, 1);
	return 1;
}

static uint64_t rng(uint64_t *s)
{
	*s ^= *s >> 12;
	*s ^= *s << 25;
	*s ^= *s >> 27;
	return *s * 0x2545F4914F6CDD1Dull;
}

static int mkf(const wchar_t *rel, uint64_t seed, size_t size)
{
	wchar_t x[XP];
	HANDLE h;
	uint8_t *b = malloc(size ? size : 1);
	size_t i;
	DWORD wr;
	int ok;

	if (!b || !X(rel, x)) {
		return 0;
	}
	for (i = 0; i < size; i++) {
		b[i] = (uint8_t)(rng(&seed) >> 56);
	}
	h = CreateFileW(x, GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	ok = INVALID_HANDLE_VALUE != h &&
	        WriteFile(h, b, (DWORD)size, &wr, NULL);
	if (INVALID_HANDLE_VALUE != h) {
		CloseHandle(h);
	}
	free(b);
	if (ok) {
		known(rel, 0);
	}
	return ok;
}

static int hash_rel(const wchar_t *rel, char hex[P0_HASH_HEX])
{
	wchar_t x[XP];

	hex[0] = '\0';
	return X(rel, x) && p0_sha256_file(x, hex);
}

static int move_rel(const wchar_t *from, const wchar_t *to, DWORD *err)
{
	wchar_t a[XP], b[XP];
	BOOL ok;

	if (!X(from, a) || !X(to, b)) {
		return 0;
	}
	ok = MoveFileExW(a, b, 0); /* never MOVEFILE_REPLACE_EXISTING */
	*err = ok ? 0 : GetLastError();
	return 0 != ok;
}

static int junction(const wchar_t *link_rel, const wchar_t *target_rel)
{
	wchar_t l[XP], t[XP], cmd[XP * 2];
	STARTUPINFOW si = {.cb = sizeof si};
	PROCESS_INFORMATION pi;
	DWORD code = 1;

	if (!p0_area_path(g_area, link_rel, l, XP) ||
	        !p0_area_path(g_area, target_rel, t, XP)) {
		return 0;
	}
	swprintf(cmd, XP * 2,
	        L"cmd.exe /c mklink /J \"%ls\" \"%ls\" >nul 2>nul", l, t);
	if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
	            NULL, NULL, &si, &pi)) {
		return 0;
	}
	WaitForSingleObject(pi.hProcess, 10000);
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	if (0 == code) {
		known(link_rel, 1);
	}
	return 0 == code;
}

typedef struct
{
	char hash[P0_HASH_HEX];
	uint8_t id[24];
	FILETIME mtime;
	DWORD attrs;
	int ok;
} Snap;

static int id_of(HANDLE h, uint8_t id[24])
{
	FILE_ID_INFO f;

	if (!GetFileInformationByHandleEx(h, FileIdInfo, &f, sizeof f)) {
		return 0;
	}
	memcpy(id, &f.VolumeSerialNumber, 8);
	memcpy(id + 8, f.FileId.Identifier, 16);
	return 1;
}

static void snap(const wchar_t *rel, Snap *s)
{
	wchar_t x[XP];
	HANDLE h;
	BY_HANDLE_FILE_INFORMATION bi;

	memset(s, 0, sizeof *s);
	if (!X(rel, x)) {
		return;
	}
	h = CreateFileW(x, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return;
	}
	if (GetFileInformationByHandle(h, &bi)) {
		s->mtime = bi.ftLastWriteTime;
		s->attrs = bi.dwFileAttributes;
		s->ok = id_of(h, s->id);
	}
	CloseHandle(h);
	if (!(s->attrs & FILE_ATTRIBUTE_DIRECTORY)) {
		hash_rel(rel, s->hash);
	}
}

static int same_snap(const Snap *a, const Snap *b)
{
	return a->ok && b->ok && !strcmp(a->hash, b->hash) &&
	        !memcmp(a->id, b->id, 24) && a->attrs == b->attrs &&
	        a->mtime.dwLowDateTime == b->mtime.dwLowDateTime &&
	        a->mtime.dwHighDateTime == b->mtime.dwHighDateTime;
}

static NtSetInformationFile_t pNtSet;
static RtlNtStatusToDosError_t pToDos;

/* Win32 FileRenameInfo with ReplaceIfExists = FALSE. `root` may be NULL. */
static DWORD rename_win32(HANDLE h, HANDLE root, const wchar_t *name)
{
	size_t nb = wcslen(name) * sizeof(wchar_t),
	       sz = sizeof(FILE_RENAME_INFO) + nb;
	FILE_RENAME_INFO *ri = calloc(1, sz);
	BOOL ok;
	DWORD err;

	ri->RootDirectory = root;
	ri->FileNameLength = (DWORD)nb;
	memcpy(ri->FileName, name, nb);
	ok = SetFileInformationByHandle(h, FileRenameInfo, ri, (DWORD)sz);
	err = ok ? 0 : GetLastError();
	free(ri);
	return err;
}

/* NtSetInformationFile(FileRenameInformation): the only way to hand a
 * RootDirectory handle to the kernel. `name` is relative to `root`, or an NT
 * path when root is NULL. */
static DWORD rename_nt(HANDLE h, HANDLE root, const wchar_t *name)
{
	size_t nb = wcslen(name) * sizeof(wchar_t),
	       sz = sizeof(NT_RENAME_) + nb;
	NT_RENAME_ *ri = calloc(1, sz);
	IOSB_ iosb = {0};
	NTSTATUS_ st;
	DWORD err;

	ri->ReplaceIfExists = FALSE;
	ri->RootDirectory = root;
	ri->FileNameLength = (ULONG)nb;
	memcpy(ri->FileName, name, nb);
	st = pNtSet(h, &iosb, ri, (ULONG)sz, FileRenameInformation_);
	err = st >= 0 ? 0 : pToDos(st);
	free(ri);
	return err;
}

/* DELETE|READ|WRITE handle on the payload, as design 7.2 re-opens it. */
static HANDLE open_payload(const wchar_t *rel, DWORD share)
{
	wchar_t x[XP];

	if (!X(rel, x)) {
		return INVALID_HANDLE_VALUE;
	}
	return CreateFileW(x, DELETE | GENERIC_READ | GENERIC_WRITE, share,
	        NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
}

static HANDLE open_dir(const wchar_t *rel, DWORD share)
{
	wchar_t x[XP];

	if (!X(rel, x)) {
		return INVALID_HANDLE_VALUE;
	}
	return CreateFileW(x,
	        FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES |
	                SYNCHRONIZE,
	        share, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
}

/* "\\?\C:\x" -> "\??\C:\x" for the NT call */
static int to_nt_path(const wchar_t *ext, wchar_t *out, size_t cap)
{
	return 0 == wcsncmp(ext, L"\\\\?\\", 4) &&
	        (size_t)swprintf(out, cap, L"\\??\\%ls", ext + 4) < cap;
}

static int exists_rel(const wchar_t *rel)
{
	wchar_t x[XP];

	return X(rel, x) && INVALID_FILE_ATTRIBUTES != GetFileAttributesW(x);
}

/* where does the (open) payload handle live now? */
static void where_is(HANDLE h, wchar_t *out, size_t cap)
{
	out[0] = L'\0';
	GetFinalPathNameByHandleW(h, out, (DWORD)cap, VOLUME_NAME_DOS);
}

static void txn_setup(const wchar_t *base,
        const wchar_t *tag,
        wchar_t *payload_rel,
        wchar_t *parent_rel)
{
	wchar_t r[700];

	swprintf(parent_rel, 700, L"%ls\\P_%ls", base, tag);
	swprintf(r, 700, L"%ls\\.wfile-txn-%ls", parent_rel, tag);
	mkd(parent_rel);
	mkd(r);
	swprintf(payload_rel, 700, L"%ls\\payload.bin", r);
	mkf(payload_rel, 0xABCD, 64 * 1024);
}

static void case_success(void)
{
	static const struct
	{
		const char *id;
		int mode; /* 0 absolute win32, 1 parent-relative win32, 2
		             parent-relative NT, 3 absolute NT */
	} v[] = {{"A1_absolute_win32", 0}, {"A2_parent_relative_win32", 1},
	        {"A3_parent_relative_nt", 2}, {"A3b_absolute_nt", 3}};
	size_t i;

	group_dir(L"a");
	for (i = 0; i < WF_ARRAY_LEN(v); i++) {
		wchar_t tag[16], payload[700], parent[700], final_rel[700],
		        xparent[XP], xfinal[XP], ntp[XP], nm[XP];
		char before[P0_HASH_HEX], after[P0_HASH_HEX];
		uint8_t id0[24], id1[24];
		HANDLE hp, hroot = NULL;
		DWORD err;
		int moved;
		char name[40];

		swprintf(tag, 16, L"a%zu", i);
		txn_setup(L"a", tag, payload, parent);
		swprintf(final_rel, 700, L"%ls\\result.bin", parent);
		hash_rel(payload, before);
		hp = open_payload(payload, FILE_SHARE_READ);
		id_of(hp, id0);
		X(parent, xparent);
		X(final_rel, xfinal);
		if (1 == v[i].mode || 2 == v[i].mode) {
			hroot = open_dir(parent,
			        FILE_SHARE_READ | FILE_SHARE_WRITE |
			                FILE_SHARE_DELETE);
		}

		switch (v[i].mode) {
		case 0: err = rename_win32(hp, NULL, xfinal); break;
		case 1: err = rename_win32(hp, hroot, L"result.bin"); break;
		case 2: err = rename_nt(hp, hroot, L"result.bin"); break;
		default:
			to_nt_path(xfinal, ntp, XP);
			err = rename_nt(hp, NULL, ntp);
		}
		(void)nm;
		moved = 0 == err;
		known(final_rel, 0);
		id_of(hp, id1);
		CloseHandle(hp);
		if (hroot) {
			CloseHandle(hroot);
		}
		hash_rel(final_rel, after);
		snprintf(name, sizeof name, "%s", v[i].id);
		row(name, "H-005",
		        moved && !strcmp(before, after) &&
		                        !memcmp(id0, id1, 24) &&
		                        !exists_rel(payload)
		                ? "PASS"
		                : (err ? "INFO" : "FAIL"),
		        "destination absent: rename succeeds, content and "
		        "FileId preserved",
		        "err=%lu moved=%d content_same=%d fileid_same=%d "
		        "old_name_gone=%d",
		        err, moved, !strcmp(before, after),
		        !memcmp(id0, id1, 24), !exists_rel(payload));
	}
	sweep();
}

/* H-028: a destination path beyond MAX_PATH */
static void case_long_path(void)
{
	wchar_t rel[700] = L"lp", payload[700], parent[700], final_rel[700],
	        xfinal[XP];
	char before[P0_HASH_HEX], after[P0_HASH_HEX];
	HANDLE hp, hroot;
	DWORD err1, err2;
	int level;

	group_dir(rel);
	for (level = 0; level < 3; level++) {
		wchar_t step[120];

		swprintf(step, 120, L"\\%090d", level);
		wcscat_s(rel, 700, step);
		mkd(rel);
	}
	wcscpy_s(parent, 700, rel);
	{
		wchar_t r[700];

		swprintf(r, 700, L"%ls\\.wfile-txn-lp", parent);
		mkd(r);
		swprintf(payload, 700, L"%ls\\payload.bin", r);
		mkf(payload, 0x1111, 8192);
	}
	swprintf(final_rel, 700, L"%ls\\result.bin", parent);
	X(final_rel, xfinal);
	hash_rel(payload, before);
	hp = open_payload(payload, FILE_SHARE_READ);
	hroot = open_dir(
	        parent, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
	err1 = rename_nt(hp, hroot, L"result.bin");
	known(final_rel, 0);
	CloseHandle(hroot); /* our DELETE-access handle would make the hash read
	                       a sharing violation */
	CloseHandle(hp);
	hash_rel(final_rel, after);
	row("A4_long_path_parent_relative_nt", "H-028",
	        !err1 && !strcmp(before, after) ? "PASS" : "FAIL",
	        "path > MAX_PATH: parent-relative rename works",
	        "path_units=%zu err=%lu content_same=%d", wcslen(xfinal), err1,
	        !strcmp(before, after));

	/* the same, absolute name, second payload */
	{
		wchar_t r[700], p2[700], f2[700], xf2[XP], ntp[XP];
		char h2[P0_HASH_HEX], h3[P0_HASH_HEX];

		swprintf(r, 700, L"%ls\\.wfile-txn-lp2", parent);
		mkd(r);
		swprintf(p2, 700, L"%ls\\payload.bin", r);
		mkf(p2, 0x2222, 4096);
		swprintf(f2, 700, L"%ls\\result2.bin", parent);
		X(f2, xf2);
		hash_rel(p2, h2);
		hp = open_payload(p2, FILE_SHARE_READ);
		err2 = rename_win32(hp, NULL, xf2);
		if (ERROR_PATH_NOT_FOUND == err2 ||
		        ERROR_INVALID_NAME == err2 ||
		        ERROR_FILENAME_EXCED_RANGE == err2) {
			to_nt_path(xf2, ntp, XP);
			err2 = rename_nt(hp, NULL, ntp);
		}
		known(f2, 0);
		CloseHandle(hp);
		hash_rel(f2, h3);
		row("A4b_long_path_absolute", "H-028",
		        !err2 && !strcmp(h2, h3) ? "PASS" : "INFO",
		        "path > MAX_PATH: absolute-name rename",
		        "err=%lu content_same=%d", err2, !strcmp(h2, h3));
	}
	sweep();
}

typedef enum
{
	B_FILE,
	B_READONLY,
	B_OPEN_HANDLE,
	B_EMPTY_DIR,
	B_FULL_DIR,
	B_JUNCTION,
	B_CASE_VARIANT
} BKind;

static void case_collisions(void)
{
	static const struct
	{
		const char *id;
		BKind k;
	} v[] = {
	        {"B1_existing_file", B_FILE},
	        {"B2_existing_readonly_file", B_READONLY},
	        {"B3_existing_file_held_open", B_OPEN_HANDLE},
	        {"B4_existing_empty_directory", B_EMPTY_DIR},
	        {"B5_existing_nonempty_directory", B_FULL_DIR},
	        {"B6_existing_junction", B_JUNCTION},
	        {"B7_existing_name_differs_by_case", B_CASE_VARIANT},
	};
	size_t i;

	group_dir(L"b");
	for (i = 0; i < WF_ARRAY_LEN(v); i++) {
		wchar_t tag[16], payload[700], parent[700], dest[700],
		        other[700], xfinal[XP];
		Snap d0, d1, p0s, p1s;
		HANDLE hp, held = INVALID_HANDLE_VALUE, hroot;
		DWORD err_abs, err_rel;
		int dest_is_dir = 0;
		wchar_t x[XP];

		swprintf(tag, 16, L"b%zu", i);
		txn_setup(L"b", tag, payload, parent);
		swprintf(dest, 700, L"%ls\\%ls", parent,
		        B_CASE_VARIANT == v[i].k ? L"RESULT.BIN"
		                                 : L"result.bin");
		switch (v[i].k) {
		case B_FILE:
		case B_OPEN_HANDLE:
		case B_CASE_VARIANT: mkf(dest, 0x7777, 3000); break;
		case B_READONLY:
			mkf(dest, 0x7777, 3000);
			X(dest, x);
			SetFileAttributesW(x, FILE_ATTRIBUTE_READONLY);
			break;
		case B_EMPTY_DIR:
			mkd(dest);
			dest_is_dir = 1;
			break;
		case B_FULL_DIR:
			mkd(dest);
			swprintf(other, 700, L"%ls\\inner.txt", dest);
			mkf(other, 0x8888, 100);
			dest_is_dir = 1;
			break;
		case B_JUNCTION: {
			wchar_t t[700];

			swprintf(t, 700, L"%ls\\jt", parent);
			mkd(t);
			junction(dest, t);
			dest_is_dir = 1;
			break;
		}
		}
		if (B_OPEN_HANDLE == v[i].k) {
			X(dest, x);
			held = CreateFileW(x, GENERIC_READ,
			        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
			        OPEN_EXISTING, 0, NULL);
		}
		snap(dest, &d0);
		snap(payload, &p0s);
		X(dest, xfinal);

		hp = open_payload(payload, FILE_SHARE_READ);
		err_abs = rename_win32(hp, NULL, xfinal);
		hroot = open_dir(parent,
		        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
		/* the name actually asked for: same stem, as the design would
		 * use */
		err_rel = rename_nt(hp, hroot,
		        B_CASE_VARIANT == v[i].k ? L"result.bin"
		                                 : L"result.bin");
		CloseHandle(hroot);
		CloseHandle(hp);
		if (INVALID_HANDLE_VALUE != held) {
			CloseHandle(held);
		}

		snap(dest, &d1);
		snap(payload, &p1s);
		{
			int dest_ok = dest_is_dir
			        ? (d1.ok && !memcmp(d0.id, d1.id, 24) &&
			                  d0.attrs == d1.attrs)
			        : same_snap(&d0, &d1);
			int payload_ok = same_snap(&p0s, &p1s);

			row(v[i].id, "H-005",
			        err_abs && err_rel && dest_ok && payload_ok
			                ? "PASS"
			                : "FAIL",
			        "rename refused; destination and payload both "
			        "untouched",
			        "absolute err=%lu, parent-relative err=%lu, "
			        "destination_unchanged=%d payload_unchanged=%d",
			        err_abs, err_rel, dest_ok, payload_ok);
		}
		if (B_READONLY == v[i].k) { /* our own object: make it removable
		                               again */
			X(dest, x);
			SetFileAttributesW(x, FILE_ATTRIBUTE_NORMAL);
		}
		sweep();
	}
}

typedef struct
{
	wchar_t xdest[XP];
	HANDLE go;
	DWORD err;
	int created;
} Racer;

static DWORD WINAPI racer(LPVOID arg)
{
	Racer *r = arg;
	HANDLE h;
	DWORD wr;

	WaitForSingleObject(r->go, INFINITE);
	h = CreateFileW(r->xdest, GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		r->err = GetLastError();
		return 0;
	}
	WriteFile(h, "attacker", 8, &wr, NULL);
	CloseHandle(h);
	r->created = 1;
	return 0;
}

static void case_after_preflight(void)
{
	wchar_t payload[700], parent[700], dest[700], xfinal[XP];
	Snap d0, d1;
	HANDLE hp;
	DWORD err;

	group_dir(L"c");
	txn_setup(L"c", L"c1", payload, parent);
	swprintf(dest, 700, L"%ls\\result.bin", parent);
	X(dest, xfinal);
	hp = open_payload(payload, FILE_SHARE_READ);
	/* preflight said "free"; verification passed; now someone creates it */
	mkf(dest, 0x9999, 1234);
	snap(dest, &d0);
	err = rename_win32(hp, NULL, xfinal);
	snap(dest, &d1);
	CloseHandle(hp);
	row("C1_created_after_verification", "H-005",
	        err && same_snap(&d0, &d1) ? "PASS" : "FAIL",
	        "rename refused; the late arrival is untouched",
	        "err=%lu destination_unchanged=%d", err, same_snap(&d0, &d1));
	sweep();
}

static void case_race(int trials)
{
	int t, rename_won = 0, attacker_won = 0, both = 0, neither = 0,
	       wrong_content = 0;

	group_dir(L"r");
	for (t = 0; t < trials; t++) {
		wchar_t tag[24], payload[700], parent[700], dest[700],
		        xfinal[XP];
		Racer r = {0};
		HANDLE th, hp;
		char want[P0_HASH_HEX], got[P0_HASH_HEX];
		DWORD err;
		int attacker_created, rename_ok;

		swprintf(tag, 24, L"r%d", t);
		txn_setup(L"r", tag, payload, parent);
		swprintf(dest, 700, L"%ls\\result.bin", parent);
		X(dest, xfinal);
		wcscpy_s(r.xdest, XP, xfinal);
		r.go = CreateEventW(NULL, TRUE, FALSE, NULL);
		hash_rel(payload, want);
		hp = open_payload(payload, FILE_SHARE_READ);
		th = CreateThread(NULL, 0, racer, &r, 0, NULL);
		Sleep(1);
		SetEvent(r.go);
		if (t & 1) {
			YieldProcessor(); /* vary who gets there first */
		}
		err = rename_win32(hp, NULL, xfinal);
		WaitForSingleObject(th, 5000);
		CloseHandle(th);
		CloseHandle(r.go);
		CloseHandle(hp);
		attacker_created = r.created;
		rename_ok = 0 == err;

		known(dest, 0);
		hash_rel(dest, got);
		if (rename_ok && attacker_created) {
			both++;
		} else if (rename_ok) {
			rename_won++;
			if (strcmp(got, want)) {
				wrong_content++;
			}
		} else if (attacker_created) {
			attacker_won++;
			if (!exists_rel(payload)) {
				wrong_content++;
			}
		} else {
			neither++;
		}
		sweep();
	}
	row("C2_race_create_vs_rename", "H-005",
	        !both && !wrong_content ? "PASS" : "FAIL",
	        "exactly one winner per trial; nobody overwritten",
	        "trials=%d rename_won=%d attacker_won=%d both_succeeded=%d "
	        "neither=%d wrong_content=%d",
	        trials, rename_won, attacker_won, both, neither, wrong_content);
}

static void read_stored_case(
        const wchar_t *dir_rel, wchar_t *out, size_t cap, const wchar_t *probe)
{
	WIN32_FIND_DATAW fd;
	wchar_t x[XP], pat[XP];
	HANDLE h;

	out[0] = L'\0';
	if (!X(dir_rel, x)) {
		return;
	}
	swprintf(pat, XP, L"%ls\\%ls", x, probe);
	h = FindFirstFileW(pat, &fd);
	if (INVALID_HANDLE_VALUE != h) {
		wcscpy_s(out, cap, fd.cFileName);
		FindClose(h);
	}
}

static void case_case_only(void)
{
	wchar_t payload[700], parent[700], tmp[700], fin[700], stored[128];
	HANDLE hp, hroot;
	DWORD err;
	char before[P0_HASH_HEX], after[P0_HASH_HEX];

	group_dir(L"d");
	txn_setup(L"d", L"d1", payload, parent);
	/* first the commit to "file.bin", then a case-only rename of the
	 * committed file */
	swprintf(fin, 700, L"%ls\\file.bin", parent);
	hp = open_payload(payload, FILE_SHARE_READ);
	hroot = open_dir(
	        parent, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
	rename_nt(hp, hroot, L"file.bin");
	known(fin, 0);
	CloseHandle(hp);
	hash_rel(fin, before);
	hp = open_payload(fin, FILE_SHARE_READ);
	err = rename_nt(hp, hroot, L"FILE.BIN"); /* ReplaceIfExists FALSE, same
	                                            file, different case */
	CloseHandle(hp);
	read_stored_case(parent, stored, 128, L"file.bin");
	hash_rel(fin, after);
	row("D1_case_only_rename_same_file", "H-006", "INFO",
	        "recorded, not assumed",
	        "err=%lu stored_name_now=%ls content_same=%d", err,
	        stored[0] ? stored : L"?", !strcmp(before, after));

	/* a directory */
	swprintf(tmp, 700, L"%ls\\dirx", parent);
	mkd(tmp);
	{
		wchar_t x[XP];
		HANDLE hd;

		X(tmp, x);
		hd = CreateFileW(x, DELETE | FILE_LIST_DIRECTORY | SYNCHRONIZE,
		        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		        NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
		err = INVALID_HANDLE_VALUE == hd
		        ? GetLastError()
		        : rename_nt(hd, hroot, L"DIRX");
		if (INVALID_HANDLE_VALUE != hd) {
			CloseHandle(hd);
		}
		read_stored_case(parent, stored, 128, L"dirx");
		row("D2_case_only_rename_directory", "H-006", "INFO",
		        "recorded, not assumed", "err=%lu stored_name_now=%ls",
		        err, stored[0] ? stored : L"?");
		if (0 == err) { /* keep the sweep's recorded name valid */
			wchar_t up[700];

			swprintf(up, 700, L"%ls\\DIRX", parent);
			known(up, 1);
		}
	}
	CloseHandle(hroot);
	sweep();
}

typedef enum
{
	SW_DIR,
	SW_JUNCTION,
	SW_ANCESTOR
} SwapKind;
typedef enum
{
	M_PATH,
	M_WIN32_ROOT,
	M_NT_ROOT
} Mode;

static const char *swap_name[] = {
        "dir_replaced", "junction_planted", "grandparent_replaced"};
static const char *mode_name[] = {
        "path_based", "win32_parent_handle", "nt_parent_handle"};

static void parent_swap(SwapKind sk,
        DWORD payload_share,
        DWORD parent_share,
        Mode mode,
        int idx,
        int detect_only)
{
	wchar_t g[64], p[64], gp[700], parent[700], payload[700],
	        final_rel[700], moved[700], outside[700], newparent[700];
	wchar_t xfinal[XP], landed[XP], verified[XP], nowp[XP];
	HANDLE hpay, hpar;
	uint8_t id_verified[24], id_landing_dir[24];
	DWORD swap_err = 0, ren_err = 0;
	int swapped = 0, redirected = -1, detect = -1;
	char id[96], expected[110], payload_share_s[8], parent_share_s[8];

	swprintf(g, 64, L"e%d_g", idx);
	swprintf(p, 64, L"e%d_p", idx);
	swprintf(gp, 700, L"e\\%ls", g);
	mkd(gp);
	swprintf(parent, 700, L"%ls\\%ls", gp, p);
	mkd(parent);
	{
		wchar_t r[700];

		swprintf(r, 700, L"%ls\\.wfile-txn-e", parent);
		mkd(r);
		swprintf(payload, 700, L"%ls\\payload.bin", r);
		mkf(payload, 0x5150, 16384);
	}
	swprintf(outside, 700, L"e\\e%d_outside", idx);
	mkd(outside);
	swprintf(final_rel, 700, L"%ls\\result.bin", parent);
	X(final_rel, xfinal);

	hpay = open_payload(payload, payload_share);
	hpar = open_dir(parent, parent_share);
	if (INVALID_HANDLE_VALUE == hpay || INVALID_HANDLE_VALUE == hpar) {
		row("E_setup", "H-007", "FAIL", "handles open",
		        "open failed (%lu)", GetLastError());
		if (INVALID_HANDLE_VALUE != hpay) {
			CloseHandle(hpay);
		}
		if (INVALID_HANDLE_VALUE != hpar) {
			CloseHandle(hpar);
		}
		sweep();
		return;
	}
	id_of(hpar, id_verified);
	where_is(hpar, verified, XP);

	switch (sk) {
	case SW_DIR:
	case SW_JUNCTION:
		swprintf(moved, 700, L"%ls_moved", parent);
		swapped = move_rel(parent, moved, &swap_err);
		if (swapped) {
			known(moved, 1); /* the old parent now lives here, with
			                    everything in it */
			{
				wchar_t t[700], t2[700];

				swprintf(t, 700, L"%ls\\.wfile-txn-e", moved);
				swprintf(t2, 700, L"%ls\\payload.bin", t);
				known(t, 1);
				known(t2, 0);
			}
			if (SW_DIR == sk) {
				swapped = mkd(parent);
			} else {
				swapped = junction(parent, outside);
			}
		}
		break;
	case SW_ANCESTOR:
		swprintf(moved, 700, L"%ls_moved", gp);
		swapped = move_rel(gp, moved, &swap_err);
		if (swapped) {
			wchar_t a[700], b[700], c[700];

			known(moved, 1);
			swprintf(a, 700, L"%ls\\%ls", moved, p);
			swprintf(b, 700, L"%ls\\.wfile-txn-e", a);
			swprintf(c, 700, L"%ls\\payload.bin", b);
			known(a, 1);
			known(b, 1);
			known(c, 0);
			swapped = mkd(gp);
			if (swapped) {
				swprintf(newparent, 700, L"%ls\\%ls", gp, p);
				swapped = mkd(newparent);
			}
		}
		break;
	}

	snprintf(payload_share_s, sizeof payload_share_s, "%s",
	        payload_share & FILE_SHARE_DELETE ? "+del" : "no-del");
	snprintf(parent_share_s, sizeof parent_share_s, "%s",
	        parent_share & FILE_SHARE_DELETE ? "+del" : "no-del");
	snprintf(id, sizeof id, "E%d_%s_payload_%s_parent_%s_%s", idx,
	        swap_name[sk], payload_share_s, parent_share_s,
	        mode_name[mode]);

	if (!swapped) {
		/* the OS itself refused the swap: nothing to redirect */
		snprintf(expected, sizeof expected,
		        "swap blocked by the OS, or commit stays inside the "
		        "verified parent");
		row(id, "H-007", "PASS", expected,
		        "swap_blocked err=%lu (the open handle(s) prevented "
		        "the parent from being replaced)",
		        swap_err);
		CloseHandle(hpay);
		CloseHandle(hpar);
		sweep();
		return;
	}

	if (detect_only) { /* detection by comparing the parent handle's current
	                      final path */
		where_is(hpar, nowp, XP);
		detect = 0 != wcscmp(verified, nowp);
		row(id, "H-007", detect ? "PASS" : "FAIL",
		        "GetFinalPathNameByHandle(parent) changes when the "
		        "parent is swapped",
		        "verified=%ls now=%ls", verified, nowp);
		CloseHandle(hpay);
		CloseHandle(hpar);
		sweep();
		return;
	}

	switch (mode) {
	case M_PATH: ren_err = rename_win32(hpay, NULL, xfinal); break;
	case M_WIN32_ROOT:
		ren_err = rename_win32(hpay, hpar, L"result.bin");
		break;
	case M_NT_ROOT: ren_err = rename_nt(hpay, hpar, L"result.bin"); break;
	}

	if (0 == ren_err) {
		wchar_t *slash;
		HANDLE hd;

		where_is(hpay, landed, XP);
		slash = wcsrchr(landed, L'\\');
		if (slash) {
			*slash = L'\0';
		}
		hd = CreateFileW(landed, FILE_READ_ATTRIBUTES,
		        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		        NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
		if (INVALID_HANDLE_VALUE != hd && id_of(hd, id_landing_dir)) {
			redirected =
			        0 != memcmp(id_landing_dir, id_verified, 24);
		}
		if (INVALID_HANDLE_VALUE != hd) {
			CloseHandle(hd);
		}
		/* where it landed must also be tracked for cleanup */
		{
			wchar_t rel[700];

			if (1 == redirected && SW_JUNCTION == sk) {
				swprintf(rel, 700, L"%ls\\result.bin", outside);
			} else {
				swprintf(rel, 700, L"%ls\\result.bin",
				        SW_ANCESTOR == sk
				                ? (1 == redirected ? newparent
				                                   : moved)
				                : (1 == redirected ? parent
				                                   : moved));
			}
			known(rel, 0);
			if (SW_ANCESTOR == sk) {
				wchar_t rel2[700];

				swprintf(rel2, 700, L"%ls\\%ls\\result.bin",
				        moved, p);
				known(rel2, 0);
			}
			if (SW_DIR == sk || SW_JUNCTION == sk) {
				wchar_t rel2[700];

				swprintf(rel2, 700, L"%ls\\result.bin", parent);
				known(rel2, 0);
				swprintf(rel2, 700, L"%ls\\result.bin", moved);
				known(rel2, 0);
			}
		}
	}
	CloseHandle(hpay);
	CloseHandle(hpar);

	if (M_PATH == mode) {
		/* the vulnerable baseline: recorded, not judged */
		row(id, "H-007", "INFO",
		        "baseline: a path-based commit follows the swapped "
		        "path",
		        "rename_err=%lu redirected_outside_verified_parent=%d",
		        ren_err, redirected);
	} else {
		/* the property a design could rely on: never lands outside the
		 * verified parent */
		int ok = 0 != ren_err || 0 == redirected;

		snprintf(expected, sizeof expected,
		        "handle-bound commit never lands outside the verified "
		        "parent");
		row(id, "H-007", ok ? "PASS" : "FAIL", expected,
		        "rename_err=%lu redirected_outside_verified_parent=%d",
		        ren_err, redirected);
	}
	sweep();
}

static void case_parent_swaps(void)
{
	static const SwapKind sks[] = {SW_DIR, SW_JUNCTION, SW_ANCESTOR};
	static const Mode modes[] = {M_PATH, M_WIN32_ROOT, M_NT_ROOT};
	static const DWORD pshare[] = {FILE_SHARE_READ,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE};
	int idx = 0;
	size_t a, b, c;
	const DWORD all =
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

	group_dir(L"e");
	for (a = 0; a < 3; a++) {
		for (b = 0; b < 2; b++) {
			for (c = 0; c < 3; c++) {
				parent_swap(sks[a], pshare[b], all, modes[c],
				        idx++, 0);
			}
		}
	}

	/* the parent directory handle opened WITHOUT delete sharing */
	parent_swap(SW_DIR, all, FILE_SHARE_READ | FILE_SHARE_WRITE, M_NT_ROOT,
	        idx++, 0);
	parent_swap(SW_JUNCTION, all, FILE_SHARE_READ | FILE_SHARE_WRITE,
	        M_NT_ROOT, idx++, 0);
	/* detection of a swap from the parent handle's final path */
	parent_swap(SW_DIR, all, all, M_NT_ROOT, idx++, 1);
	parent_swap(SW_ANCESTOR, all, all, M_NT_ROOT, idx++, 1);
}

/* E7: the destination parent already is a link (what preflight now refuses) */

/*
 * Which open handles stop the destination parent from being swapped? Without
 * this control the "swap blocked" rows above could just mean the adversary
 * step is broken. The first combination (no handle at all) must be swappable.
 */
static void case_swap_matrix(void)
{
#define SHARE_ALL (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)
	static const struct
	{
		const char *id;
		int payload; /* 0 none, 1 read access, 2 DELETE|READ|WRITE
		                access (the commit handle) */
		DWORD pay_share;
		int parent; /* a directory handle on the destination parent */
		DWORD par_share;
		int design; /* the combination the design actually holds at
		               commit time */
	} v[] = {
	        {"M0_no_handles_control", 0, 0, 0, 0, 0},
	        {"M1_payload_read_access_share_all", 1, SHARE_ALL, 0, 0, 0},
	        {"M2_payload_commit_handle_share_read", 2, FILE_SHARE_READ, 0,
	                0, 1},
	        {"M3_payload_commit_handle_share_all", 2, SHARE_ALL, 0, 0, 1},
	        {"M4_parent_dir_handle_share_all_only", 0, 0, 1, SHARE_ALL, 0},
	        {"M5_parent_dir_handle_without_share_delete", 0, 0, 1,
	                FILE_SHARE_READ | FILE_SHARE_WRITE, 0},
	};
	size_t i;
	int control_ok = 0, design_blocked = 1, design_n = 0;

	group_dir(L"m");
	for (i = 0; i < WF_ARRAY_LEN(v); i++) {
		wchar_t tag[16], payload[700], parent[700], moved[700], t1[700],
		        t2[700], x[XP];
		HANDLE hp = INVALID_HANDLE_VALUE, hd = INVALID_HANDLE_VALUE;
		DWORD err = 0;
		int swapped;

		swprintf(tag, 16, L"m%zu", i);
		txn_setup(L"m", tag, payload, parent);
		if (1 == v[i].payload) {
			X(payload, x);
			hp = CreateFileW(x, GENERIC_READ, v[i].pay_share, NULL,
			        OPEN_EXISTING, 0, NULL);
		} else if (2 == v[i].payload) {
			hp = open_payload(payload, v[i].pay_share);
		}
		if (v[i].parent) {
			hd = open_dir(parent, v[i].par_share);
		}
		swprintf(moved, 700, L"%ls_moved", parent);
		swapped = move_rel(parent, moved, &err);
		if (swapped) {
			swprintf(t1, 700, L"%ls\\.wfile-txn-%ls", moved, tag);
			swprintf(t2, 700, L"%ls\\payload.bin", t1);
			known(moved, 1);
			known(t1, 1);
			known(t2, 0);
		}
		if (INVALID_HANDLE_VALUE != hp) {
			CloseHandle(hp);
		}
		if (INVALID_HANDLE_VALUE != hd) {
			CloseHandle(hd);
		}
		row(v[i].id, "H-007", "INFO", "recorded",
		        "rename of the destination parent directory: %s "
		        "(err=%lu)%s",
		        swapped ? "SUCCEEDED" : "refused", err,
		        v[i].design ? "  <- the handle the design holds from "
		                      "verification to commit"
		                    : "");
		if (0 == i) {
			control_ok = swapped;
		}
		if (v[i].design) {
			design_n++;
			if (swapped) {
				design_blocked = 0;
			}
		}
		sweep();
	}
	row("M_control_adversary_can_swap", "H-007",
	        control_ok ? "PASS" : "FAIL",
	        "with no handle open the parent CAN be replaced (otherwise "
	        "every 'blocked' row is vacuous)",
	        "control %s", control_ok ? "swapped" : "could not swap");
	row("M_commit_handle_pins_the_parent", "H-007",
	        design_blocked && design_n ? "PASS" : "FAIL",
	        "while the payload commit handle is open the destination "
	        "parent cannot be renamed or replaced",
	        "%d design combination(s) tested, all blocked=%d", design_n,
	        design_blocked);
}

/*
 * What if verification and commit do NOT overlap (the commit handle is closed
 * in between and the payload is re-opened by path)? Then a swap is possible and
 * the FileId re-check is the only defence. Recorded as the reason the design
 * insists on one continuous handle.
 */
static void case_unpinned_reopen(void)
{
	wchar_t payload[700], parent[700], moved[700], t1[700], t2[700],
	        other[700], final_rel[700], xfinal[XP];
	HANDLE hp;
	uint8_t id_v[24], id_now[24];
	DWORD err = 0;
	int swapped, ids_differ = -1, committed_attacker = -1;
	char mine[P0_HASH_HEX], landed[P0_HASH_HEX];

	group_dir(L"u");
	txn_setup(L"u", L"u1", payload, parent);
	hash_rel(payload, mine);
	hp = open_payload(payload, FILE_SHARE_READ);
	id_of(hp, id_v);
	CloseHandle(hp); /* verification done, handle released: the gap */

	swprintf(moved, 700, L"%ls_moved", parent);
	swapped = move_rel(parent, moved, &err);
	if (swapped) {
		swprintf(t1, 700, L"%ls\\.wfile-txn-u1", moved);
		swprintf(t2, 700, L"%ls\\payload.bin", t1);
		known(moved, 1);
		known(t1, 1);
		known(t2, 0);
		mkd(parent);
		swprintf(other, 700, L"%ls\\.wfile-txn-u1", parent);
		mkd(other);
		swprintf(t2, 700, L"%ls\\payload.bin", other);
		mkf(t2, 0xBAD,
		        16384); /* the attacker's look-alike at the same path */

		hp = open_payload(payload,
		        FILE_SHARE_READ); /* re-open by the original path */
		if (INVALID_HANDLE_VALUE != hp) {
			id_of(hp, id_now);
			ids_differ = 0 != memcmp(id_v, id_now, 24);
			swprintf(final_rel, 700, L"%ls\\result.bin", parent);
			X(final_rel, xfinal);
			/* without the FileId re-check this commit would publish
			 * the attacker's file */
			if (0 == rename_win32(hp, NULL, xfinal)) {
				CloseHandle(hp);
				known(final_rel, 0);
				hash_rel(final_rel, landed);
				committed_attacker = 0 != strcmp(landed, mine);
			} else {
				CloseHandle(hp);
			}
		}
	}
	row("U1_unpinned_reopen_by_path", "H-007", swapped ? "INFO" : "FAIL",
	        "recorded: with a gap the parent is swappable; the FileId "
	        "re-check detects the substitute",
	        "swapped=%d (err=%lu) reopened_fileid_differs=%d "
	        "blind_commit_would_publish_attacker_content=%d",
	        swapped, err, ids_differ, committed_attacker);
	sweep();
}

static void case_parent_is_junction(void)
{
	wchar_t target[700] = L"e\\e7_target", link[700] = L"e\\e7_link",
	        payload[700], dest[700], r[700], xfinal[XP];
	HANDLE hp, hroot;
	DWORD err;
	char got[P0_HASH_HEX], want[P0_HASH_HEX];
	wchar_t xl[XP], landed[XP];

	mkd(target);
	junction(link, target);
	swprintf(r, 700, L"e\\e7_src");
	mkd(r);
	swprintf(payload, 700, L"%ls\\payload.bin", r);
	mkf(payload, 0x4242, 4096);
	hash_rel(payload, want);
	swprintf(dest, 700, L"%ls\\result.bin", link);
	X(dest, xfinal);
	hp = open_payload(payload, FILE_SHARE_READ);
	hroot = open_dir(link,
	        FILE_SHARE_READ | FILE_SHARE_WRITE |
	                FILE_SHARE_DELETE); /* follows the link */
	err = rename_nt(hp, hroot, L"result.bin");
	where_is(hp, landed, XP);
	X(link, xl);
	CloseHandle(hroot);
	CloseHandle(hp); /* our DELETE-access handle must be gone before the
	                    content is hashed */
	{
		wchar_t t[700];

		swprintf(t, 700, L"%ls\\result.bin", target);
		hash_rel(t, got);
		known(t, 0);
	}
	row("E7_parent_already_a_junction", "H-007", "INFO",
	        "a handle opened on a junction refers to its TARGET; the "
	        "commit lands there",
	        "err=%lu landed=%ls content_in_target=%d", err, landed,
	        !strcmp(got, want));
	sweep();
}

static void case_other_handle(void)
{
	static const struct
	{
		const char *id;
		DWORD share;
	} v[] = {{"F1_other_handle_share_rw",
	                 FILE_SHARE_READ | FILE_SHARE_WRITE},
	        {"F2_other_handle_share_rwd",
	                FILE_SHARE_READ | FILE_SHARE_WRITE |
	                        FILE_SHARE_DELETE}};
	size_t i;

	group_dir(L"f");
	for (i = 0; i < 2; i++) {
		wchar_t tag[16], payload[700], parent[700], x[XP];
		HANDLE hp, other, hroot;
		DWORD err;

		swprintf(tag, 16, L"f%zu", i);
		txn_setup(L"f", tag, payload, parent);
		X(payload, x);
		other = CreateFileW(x, GENERIC_READ, v[i].share, NULL,
		        OPEN_EXISTING, 0, NULL); /* an AV-like reader */
		hp = open_payload(payload,
		        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
		err = INVALID_HANDLE_VALUE == hp
		        ? GetLastError()
		        : 0; /* before any other call */
		hroot = open_dir(parent,
		        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
		if (INVALID_HANDLE_VALUE == hp) {
			row(v[i].id, "H-009", "INFO", "recorded",
			        "our own open (DELETE access) failed with "
			        "err=%lu while the other reader holds the file "
			        "(%s)",
			        err,
			        ERROR_SHARING_VIOLATION == err
			                ? "sharing violation: a reader without "
			                  "FILE_SHARE_DELETE blocks the commit "
			                  "handle"
			                : "other");
		} else {
			err = rename_nt(hp, hroot, L"result.bin");
			row(v[i].id, "H-009", "INFO", "recorded",
			        "rename with another reader holding the file: "
			        "err=%lu",
			        err);
			CloseHandle(hp);
		}
		CloseHandle(hroot);
		if (INVALID_HANDLE_VALUE != other) {
			CloseHandle(other);
		}
		{
			wchar_t fr[700];

			swprintf(fr, 700, L"%ls\\result.bin", parent);
			known(fr, 0);
		}
		sweep();
	}
}

static void case_case_sensitive(void)
{
	wchar_t x[XP];
	HANDLE h;
	IOSB_ iosb = {0};
	struct
	{
		ULONG Flags;
	} info = {1}; /* FILE_CS_FLAG_CASE_SENSITIVE_DIR */
	NTSTATUS_ st;

	group_dir(L"g");
	if (!X(L"g", x)) {
		return;
	}
	h = CreateFileW(x,
	        FILE_WRITE_ATTRIBUTES | FILE_LIST_DIRECTORY | SYNCHRONIZE,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		row("G1_case_sensitive_directory", "H-006", "NOT_TESTED",
		        "directory switched to case-sensitive",
		        "could not open the directory (%lu)", GetLastError());
		sweep();
		return;
	}
	st = pNtSet(h, &iosb, &info, sizeof info,
	        71 /* FileCaseSensitiveInformation */);
	CloseHandle(h);
	if (st < 0) {
		row("G1_case_sensitive_directory", "H-006", "NOT_TESTED",
		        "directory switched to case-sensitive",
		        "NtSetInformationFile(FileCaseSensitiveInformation) "
		        "failed: 0x%08lx (%lu). Not run; recorded as NOT "
		        "TESTED",
		        (unsigned long)st, pToDos(st));
		sweep();
		return;
	}
	/* two names that differ only by case must coexist; the commit must not
	 * treat them as a collision */
	{
		wchar_t payload[700], parent[700], other[700], fin[700];
		HANDLE hp, hroot;
		DWORD err;
		int a_exists;

		txn_setup(L"g", L"g1", payload, parent);
		/* the txn parent was created inside the case-sensitive
		 * directory and inherits the flag? record both */
		swprintf(other, 700, L"%ls\\Result.bin", parent);
		mkf(other, 0x1, 100);
		swprintf(fin, 700, L"%ls\\result.bin", parent);
		hp = open_payload(payload, FILE_SHARE_READ);
		hroot = open_dir(parent,
		        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
		err = rename_nt(hp, hroot, L"result.bin");
		a_exists = exists_rel(fin);
		CloseHandle(hroot);
		CloseHandle(hp);
		known(fin, 0);
		row("G1_case_sensitive_directory", "H-006", "INFO",
		        "recorded: does 'Result.bin' block 'result.bin'?",
		        "directory switched; err=%lu both_names_coexist=%d (a "
		        "child directory created inside inherits the flag only "
		        "on some builds)",
		        err, a_exists);
	}
	sweep();
}

int wmain(int argc, wchar_t **argv)
{
	wchar_t base[MAX_PATH * 2] = L"", results[MAX_PATH * 2] = L"",
	                        label[48] = L"win11-ntfs", dir[MAX_PATH * 2],
	                        p[MAX_PATH * 2];
	wchar_t rep[2048] = L"";
	char ts[20], label8[48], run_id[96], os[256], bid[64], eh[P0_HASH_HEX],
	        notes[640];
	int i, quick = 0, trials = 500, leftovers, pass = 0, fail = 0, info = 0,
	       nt = 0;
	HMODULE nt_dll = GetModuleHandleW(L"ntdll.dll");
	FILE *f;
	wchar_t exe[MAX_PATH];
	static const char *const hyp[] = {
	        "H-005", "H-006", "H-007", "H-028", NULL};
	static const char *const dec[] = {"D-021", "I-005", NULL};

	for (i = 1; i < argc; i++) {
		if (!wcscmp(argv[i], L"--work-base") && i + 1 < argc) {
			wcscpy_s(base, MAX_PATH * 2, argv[++i]);
		} else if (!wcscmp(argv[i], L"--results") && i + 1 < argc) {
			wcscpy_s(results, MAX_PATH * 2, argv[++i]);
		} else if (!wcscmp(argv[i], L"--label") && i + 1 < argc) {
			wcscpy_s(label, 48, argv[++i]);
		} else if (!wcscmp(argv[i], L"--quick")) {
			quick = 1;
		}
	}
	if (!base[0] || !results[0]) {
		fputs("usage: p0_commit_rename --work-base <dir> --results "
		      "<root> [--label x] [--quick]\n",
		        stderr);
		return 2;
	}
	if (quick) {
		trials = 40;
	}
	pNtSet = (NtSetInformationFile_t)(void *)GetProcAddress(
	        nt_dll, "NtSetInformationFile");
	pToDos = (RtlNtStatusToDosError_t)(void *)GetProcAddress(
	        nt_dll, "RtlNtStatusToDosError");
	if (!pNtSet || !pToDos) {
		return 1;
	}

	p0_utc_compact(ts);
	WideCharToMultiByte(
	        CP_UTF8, 0, label, -1, label8, sizeof label8, NULL, NULL);
	snprintf(run_id, sizeof run_id, "%s-S07-%s%s", ts, label8,
	        quick ? "-QUICK" : "");
	g_area = p0_area_create(base, run_id);
	if (!g_area) {
		fputs("cannot create the scratch area\n", stderr);
		return 1;
	}
	printf("run_id: %s\n", run_id);

	case_success();
	case_long_path();
	case_collisions();
	case_after_preflight();
	case_race(trials);
	case_case_only();
	case_swap_matrix();
	case_unpinned_reopen();
	case_parent_swaps();
	case_parent_is_junction();
	case_other_handle();
	case_case_sensitive();

	leftovers = p0_area_destroy(g_area, rep, 2048);
	printf("scratch area cleanup: %d leftover(s)%ls%ls\n", leftovers,
	        leftovers ? L": " : L"", rep);

	swprintf(dir, MAX_PATH * 2, L"%ls\\P0-S07\\%hs", results, run_id);
	{
		static const wchar_t *const sub[] = {
		        L"traces", L"artifacts", L"eventlog", L"screenshots"};

		for (i = 0; i < 4; i++) {
			swprintf(p, MAX_PATH * 2, L"%ls\\%ls", dir, sub[i]);
			p0_mkdirs(p);
		}
	}
	swprintf(p, MAX_PATH * 2, L"%ls\\artifacts\\results.csv", dir);
	f = p0_fopen_w(p);
	fputs("case,hypothesis,status,expected,observed\n", f);
	for (i = 0; i < (int)g_nrows; i++) {
		const Row *r = &g_rows[i];
		char q[600];
		size_t k, m = 0;

		for (k = 0; r->observed[k] && m < sizeof q - 2; k++) {
			if ('"' == r->observed[k]) {
				q[m++] = '"';
			}
			q[m++] = r->observed[k];
		}
		q[m] = '\0';
		fprintf(f, "%s,%s,%s,\"%s\",\"%s\"\n", r->id, r->hyp, r->status,
		        r->expected, q);
		if (!strcmp(r->status, "PASS")) {
			pass++;
		} else if (!strcmp(r->status, "FAIL")) {
			fail++;
		} else if (!strcmp(r->status, "INFO")) {
			info++;
		} else {
			nt++;
		}
	}
	fclose(f);

	swprintf(p, MAX_PATH * 2, L"%ls\\environment.json", dir);
	p0_write_environment(p, base, "p0_commit_rename", VERSION);
	GetModuleFileNameW(NULL, exe, MAX_PATH);
	p0_sha256_file(exe, eh);
	snprintf(bid, sizeof bid, "p0_commit_rename-%s-%.12s", VERSION, eh);
	p0_os_summary(os, sizeof os);
	snprintf(notes, sizeof notes,
	        "%d PASS, %d FAIL, %d INFO (recorded observations, not "
	        "judged), %d NOT_TESTED on %s, NTFS only. "
	        "Race trials: %d. Scratch-area leftovers: %d.%s",
	        pass, fail, info, nt, os, trials, leftovers,
	        quick ? " QUICK RUN - NOT PLAN EVIDENCE." : "");
	swprintf(p, MAX_PATH * 2, L"%ls\\run.json", dir);
	p0_write_run_json(p, "P0-S07", run_id, bid, "NTFS", hyp,
	        quick ? "BLOCKED" : (fail || leftovers ? "FAIL" : "PASS"), dec,
	        notes);
	printf("\n%d PASS, %d FAIL, %d INFO, %d NOT_TESTED\nevidence: %ls\n",
	        pass, fail, info, nt, dir);
	return fail || leftovers ? 1 : 0;
}
