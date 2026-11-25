/*
 * Integration test against the real filesystem (Windows only).
 *
 * Everything it creates lives in its own scratch area (tools/p0/p0_area.h):
 * only objects it recorded are removed, by exact name, after the ownership
 * proofs. No enumerate-and-delete.
 */
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "../tools/p0/p0_area.h"
#include "fs/wf_enum_win.h"
#include "wf_test.h"

static P0Area *g_area;

/* Test-only Win32 boundary: the real implementation runs on real fixtures,
 * while unavailable filesystem capabilities can be exercised deterministically.
 */
enum
{
	INFO_NORMAL,
	INFO_UNSUPPORTED,
	INFO_NO_ID,
	INFO_NO_GUID,
	INFO_ZERO_ENTRY_ID,
	INFO_REPARSE,
	INFO_FAT
};
static int info_mode, native_calls;
static const wchar_t *rename_from, *rename_to;
static int rename_attempted, rename_blocked;

static BOOL test_file_info(
        HANDLE h, FILE_INFO_BY_HANDLE_CLASS cls, LPVOID buffer, DWORD bytes)
{
	BOOL ok;
	if (cls == FileIdExtdDirectoryInfo) {
		native_calls++;
		if (rename_from && !rename_attempted) {
			rename_attempted = 1;
			rename_blocked =
			        !MoveFileExW(rename_from, rename_to, 0) &&
			        ERROR_SHARING_VIOLATION == GetLastError();
		}
		if (INFO_UNSUPPORTED == info_mode) {
			SetLastError(ERROR_NOT_SUPPORTED);
			return FALSE;
		}
	}
	/* FAT and exFAT refuse all three classes with an invalid parameter. */
	if (INFO_FAT == info_mode &&
	        (cls == FileAttributeTagInfo || cls == FileIdInfo ||
	                cls == FileIdExtdDirectoryInfo)) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (cls == FileIdInfo && INFO_NO_ID == info_mode) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	ok = GetFileInformationByHandleEx(h, cls, buffer, bytes);
	if (ok && cls == FileAttributeTagInfo && INFO_REPARSE == info_mode) {
		((FILE_ATTRIBUTE_TAG_INFO *)buffer)->FileAttributes |=
		        FILE_ATTRIBUTE_REPARSE_POINT;
	}
	if (ok && cls == FileIdExtdDirectoryInfo &&
	        INFO_ZERO_ENTRY_ID == info_mode) {
		FILE_ID_EXTD_DIR_INFO *entry = buffer;
		for (;;) {
			memset(entry->FileId.Identifier, 0, 16);
			if (!entry->NextEntryOffset) {
				break;
			}
			entry = (FILE_ID_EXTD_DIR_INFO *)((BYTE *)entry +
			        entry->NextEntryOffset);
		}
	}
	return ok;
}

static DWORD test_final_path(HANDLE h, LPWSTR path, DWORD count, DWORD flags)
{
	if (INFO_NO_GUID == info_mode) {
		SetLastError(ERROR_NOT_SUPPORTED);
		return 0;
	}
	return GetFinalPathNameByHandleW(h, path, count, flags);
}

/* In FAT mode the volume says it is FAT32 (the folder itself is not). */
static BOOL test_volume_info(HANDLE h,
        LPWSTR name,
        DWORD name_n,
        LPDWORD serial,
        LPDWORD component,
        LPDWORD flags,
        LPWSTR fs,
        DWORD fs_n)
{
	BOOL ok = GetVolumeInformationByHandleW(
	        h, name, name_n, serial, component, flags, fs, fs_n);

	if (ok && INFO_FAT == info_mode && fs && fs_n >= 6) {
		wcscpy_s(fs, fs_n, L"FAT32");
	}
	return ok;
}

#define GetFileInformationByHandleEx test_file_info
#define GetVolumeInformationByHandleW test_volume_info
#define GetFinalPathNameByHandleW test_final_path
#define wf_enum_dir_win test_enum_dir_win
#define wf_enum_dir_win_limited test_enum_dir_win_limited
#define wf_enum_dir_win_budget test_enum_dir_win_budget
#define wf_enum_dir_win_controlled test_enum_dir_win_controlled
#define wf_enum_names_win test_enum_names_win
#define wf_enum_dir_win_ordered test_enum_dir_win_ordered
WfStatus test_enum_dir_win_ordered(const wchar_t *path,
        size_t limit,
        size_t bytes,
        WfSortKey key,
        bool descending,
        bool dirs_first,
        bool (*cancel)(void *),
        void *ctx,
        WfDir **out);
WfStatus test_enum_names_win(
        const wchar_t *path, bool (*cancel)(void *), void *ctx, WfDir **out);
WfStatus test_enum_dir_win_limited(
        const wchar_t *path, size_t limit, WfDir **out);
WfStatus test_enum_dir_win_budget(
        const wchar_t *path, size_t limit, size_t bytes, WfDir **out);
WfStatus test_enum_dir_win_controlled(const wchar_t *path,
        size_t limit,
        size_t bytes,
        bool (*cancel)(void *),
        void *ctx,
        WfDir **out);
#include "../src/fs/wf_enum_win.c"
#undef wf_enum_dir_win_budget
#undef wf_enum_dir_win_controlled
#undef wf_enum_names_win
#undef wf_enum_dir_win_ordered
#undef wf_enum_dir_win_limited
#undef wf_enum_dir_win
#undef GetFinalPathNameByHandleW
#undef GetVolumeInformationByHandleW
#undef GetFileInformationByHandleEx

/* Independently opened fixture handles are the oracle, not an enumerator
 * helper. Dropping IDs or attaching an ID to the wrong name must fail. */
static void check_identity(const wchar_t *path, const WfFileId *identity)
{
	HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	FILE_ID_INFO id;

	CHECK(INVALID_HANDLE_VALUE != h);
	if (INVALID_HANDLE_VALUE == h) {
		return;
	}
	CHECK(GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof id));
	CHECK((identity->kind == WF_ID_STRONG));
	CHECK(identity->volume == id.VolumeSerialNumber);
	CHECK(0 == memcmp(identity->id, id.FileId.Identifier, 16));
	CloseHandle(h);
}

static void touch(const wchar_t *rel, int is_dir)
{
	wchar_t x[4096];
	HANDLE h;

	if (is_dir) {
		CHECK(p0_area_mkdir(g_area, rel));
		return;
	}
	CHECK(p0_area_xpath(g_area, rel, x, 4096));
	h = CreateFileW(x, GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	if (INVALID_HANDLE_VALUE != h) {
		CloseHandle(h);
	}
	CHECK(p0_area_track(g_area, rel, 0));
}

/* An empty directory is success with zero entries, not an error. */
static void empty_directory(void)
{
	wchar_t p[4096];
	WfDir *d = NULL;

	CHECK(p0_area_mkdir(g_area, L"empty"));
	CHECK(p0_area_path(g_area, L"empty", p, 4096));
	CHECK(WF_OK == wf_enum_dir_win(p, &d) && 0 == d->n);
	wf_dir_free(d);
}

/* Every identity in a fresh listing is the one the system reports for that
 * object; the details (dates, attributes) come from the same listing, and
 * names are sanitized for display. */
static void listing_identities_and_details(const wchar_t *p, const WfDir *d)
{
	wchar_t child[4096];
	size_t i;
	int saw_rlo = 0;

	check_identity(p, &d->identity);
	CHECK(0 == wcscmp(d->e[0].name, L"sub") &&
	        (d->e[0].flags & WF_ENT_DIR));
	CHECK(0 == wcscmp(d->e[1].name, L"file2.txt"));
	CHECK(0 == wcscmp(d->e[2].name, L"file10.txt"));
	for (i = 0; i < d->n; i++) {
		swprintf(child, 4096, L"%ls\\%ls", p, d->e[i].name);
		check_identity(child, &d->e[i].identity);
		CHECK(d->e[i].meta_known && 0 != d->e[i].ctime &&
		        0 != d->e[i].mtime);
		CHECK(!!(d->e[i].attributes & FILE_ATTRIBUTE_DIRECTORY) ==
		        !!(d->e[i].flags & WF_ENT_DIR));
		CHECK(NULL == strchr(d->e[i].display, '\x1b'));
		saw_rlo |= strstr(d->e[i].display, "<RLO>") != NULL;
	}
	CHECK(saw_rlo);
	CHECK(0 != d->e[1].mtime);
}

/* An unsupported class, missing parent ID, or unproved canonical volume must
 * still browse, but must never supply authority, and must not leak handles. */
static void missing_information_grants_no_authority(const wchar_t *p)
{
	int mode;
	DWORD before, after;
	WfDir *d = NULL;
	size_t i;

	CHECK(GetProcessHandleCount(GetCurrentProcess(), &before));
	for (mode = INFO_UNSUPPORTED; mode <= INFO_FAT; mode++) {
		int repeat;

		info_mode = mode;
		for (repeat = 0; repeat < 8; repeat++) {
			CHECK(WF_OK == test_enum_dir_win(p, &d) && d &&
			        4 == d->n);
			if (d) {
				if (INFO_ZERO_ENTRY_ID != mode) {
					CHECK(!(d->identity.kind ==
					        WF_ID_STRONG));
				}
				for (i = 0; i < d->n; i++) {
					CHECK(!(d->e[i].identity.kind ==
					        WF_ID_STRONG));
				}
			}
			wf_dir_free(d);
			d = NULL;
		}
	}
	/* A preview lists under a byte budget. A volume that cannot report the
	 * attribute-tag class is not a reparse point: it must still be listed. */
	info_mode = INFO_FAT;
	CHECK(WF_OK == test_enum_dir_win_budget(p, 100, 1u << 20, &d) && d &&
	        4 == d->n);
	wf_dir_free(d);
	d = NULL;
	/* A real reparse point still refuses a budgeted listing. */
	info_mode = INFO_REPARSE;
	CHECK(WF_E_ACCESS == test_enum_dir_win_budget(p, 100, 1u << 20, &d));
	/* a FAT volume (no FileId, no extended listing) gives weak identities: the
	 * volume serial and the entry index, marked, and never a proven one */
	info_mode = INFO_FAT;
	CHECK(WF_OK == test_enum_dir_win(p, &d) && d && 4 == d->n);
	if (d) {
		CHECK(wf_fileid_is_weak(&d->identity) &&
		        !(d->identity.kind == WF_ID_STRONG));
		for (i = 0; i < d->n; i++) {
			const WfFileId *id = &d->e[i].identity;

			CHECK(wf_fileid_is_weak(id) &&
			        !(id->kind == WF_ID_STRONG) && 0 == id->id[8] &&
			        0 == id->id[15] &&
			        id->volume == d->identity.volume);
		}
		/* each is the entry's own index: two entries never share one */
		CHECK(0 !=
		        memcmp(d->e[0].identity.id, d->e[1].identity.id, 16));
	}
	wf_dir_free(d);
	d = NULL;
	/* and a volume that is not FAT never does */
	info_mode = INFO_UNSUPPORTED;
	CHECK(WF_OK == test_enum_dir_win(p, &d) && d);
	CHECK(d && !wf_fileid_is_weak(&d->identity) &&
	        !(d->identity.kind == WF_ID_STRONG));
	for (i = 0; d && i < d->n; i++) {
		CHECK(!wf_fileid_is_weak(&d->e[i].identity));
	}
	wf_dir_free(d);
	d = NULL;
	info_mode = INFO_NORMAL;
	CHECK(GetProcessHandleCount(GetCurrentProcess(), &after));
	CHECK(before == after);
}

/* A normal listing, the degraded ones, and a missing directory. */
/* The listing a walk reads (the search): the same entries in the same order with
 * the same details and identities, and no display text made for any. */
static void names_only_listing(const wchar_t *p, const WfDir *full)
{
	WfDir *d = NULL;

	CHECK(WF_OK == test_enum_names_win(p, NULL, NULL, &d) && d);
	if (!d || d->n != full->n) {
		wf_dir_free(d);
		CHECK(0 && "the same number of entries");
		return;
	}
	CHECK(d->no_display &&
	        (d->identity.kind == WF_ID_STRONG) ==
	                (full->identity.kind == WF_ID_STRONG));
	for (size_t i = 0; i < d->n; i++) {
		CHECK(0 == wcscmp(d->e[i].name, full->e[i].name));
		CHECK(NULL == d->e[i].display && NULL != full->e[i].display);
		CHECK(d->e[i].flags == full->e[i].flags &&
		        d->e[i].size == full->e[i].size &&
		        d->e[i].mtime == full->e[i].mtime);
		CHECK((d->e[i].identity.kind == WF_ID_STRONG) ==
		                (full->e[i].identity.kind == WF_ID_STRONG) &&
		        0 ==
		                memcmp(d->e[i].identity.id,
		                        full->e[i].identity.id,
		                        sizeof d->e[i].identity.id));
	}
	wf_dir_free(d);
}

/* A folder listed in the order asked for is in the order of the listing by name
 * sorted again as asked: one sort, the same result. */
static void ordered_listing(const wchar_t *p)
{
	static const struct
	{
		WfSortKey key;
		bool desc, dirs;
	} spec[] = {{WF_SORT_SIZE, true, true}, {WF_SORT_DATE, false, false},
	        {WF_SORT_EXT, false, true}, {WF_SORT_NAME, true, false}};

	for (size_t s = 0; s < sizeof spec / sizeof *spec; s++) {
		WfDir *once = NULL, *twice = NULL;

		CHECK(WF_OK ==
		        test_enum_dir_win_ordered(p, 0, 0, spec[s].key,
		                spec[s].desc, spec[s].dirs, NULL, NULL, &once));
		CHECK(WF_OK == wf_enum_dir_win(p, &twice));
		if (once && twice && once->n == twice->n) {
			CHECK(WF_OK ==
			        wf_dir_sort_with(twice, spec[s].key,
			                spec[s].desc, spec[s].dirs));
			for (size_t i = 0; i < once->n; i++) {
				CHECK(0 ==
				        wcscmp(once->e[i].name,
				                twice->e[i].name));
				CHECK(NULL !=
				        once->e[i]
				                .display); /* a listing to draw has its text */
			}
		} else {
			CHECK(0 && "the same entries");
		}
		wf_dir_free(once);
		wf_dir_free(twice);
	}
}

static void directory_listing(void)
{
	wchar_t p[4096], missing[4096];
	WfDir *d = NULL;

	CHECK(p0_area_mkdir(g_area, L"list"));
	touch(L"list\\file10.txt", 0);
	touch(L"list\\file2.txt", 0);
	touch(L"list\\sub", 1);
	touch(L"list\\x\x202Ey.txt", 0);
	CHECK(p0_area_path(g_area, L"list", p, 4096));
	CHECK(WF_OK == wf_enum_dir_win(p, &d) && 4 == d->n);
	if (d && 4 == d->n) {
		listing_identities_and_details(p, d);
		names_only_listing(p, d);
		ordered_listing(p);
	}
	wf_dir_free(d);
	d = NULL;
	missing_information_grants_no_authority(p);
	swprintf(missing, 4096, L"%ls\\nope", p);
	CHECK(WF_E_NOENT == wf_enum_dir_win(missing, &d) && NULL == d);
}

static void unusable_paths_are_refused(void)
{
	WfDir *d = NULL;

	CHECK(WF_E_INVAL == wf_enum_dir_win(L"relative", &d));
	CHECK(WF_E_INVAL == wf_enum_dir_win(L"C:\\a\\..\\b", &d));
}

/* More records than one native buffer; every identity is independently checked
 * after sorting, and the directory cannot move during listing. */
static void more_records_than_one_buffer(void)
{
	wchar_t rel[128], p[4096], child[4096], moved[4096];
	WfDir *d = NULL;
	size_t i;
	int j;

	CHECK(p0_area_mkdir(g_area, L"many"));
	for (j = 0; j < 1200; j++) {
		swprintf(rel, 128,
		        L"many\\entry-%04d-long-enough-for-multiple-native-"
		        L"buffers.txt",
		        j);
		touch(rel, 0);
	}
	CHECK(p0_area_path(g_area, L"many", p, 4096));
	CHECK(p0_area_path(g_area, L"moved", moved, 4096));
	rename_from = p;
	rename_to = moved;
	native_calls = 0;
	CHECK(WF_OK == test_enum_dir_win(p, &d) && d && 1200 == d->n);
	CHECK(native_calls >= 3);
	CHECK(rename_attempted && rename_blocked);
	rename_from = rename_to = NULL;
	if (d && 1200 == d->n) {
		check_identity(p, &d->identity);
		for (i = 0; i < d->n; i++) {
			swprintf(child, 4096, L"%ls\\%ls", p, d->e[i].name);
			check_identity(child, &d->e[i].identity);
		}
	}
	wf_dir_free(d);
}

/* A path far beyond MAX_PATH must work through the extended form. */
static void path_beyond_max_path(void)
{
	wchar_t rel[1024] = L"deep", step[128], p[4096];
	WfDir *d = NULL;
	int lv;

	CHECK(p0_area_mkdir(g_area, rel));
	for (lv = 1; lv <= 6; lv++) {
		swprintf(step, 128, L"\\%070d", lv);
		wcscat_s(rel, 1024, step);
		CHECK(p0_area_mkdir(g_area, rel));
	}
	CHECK(p0_area_path(g_area, rel, p, 4096));
	CHECK(wcslen(p) > MAX_PATH);
	wcscat_s(rel, 1024, L"\\leaf.txt");
	touch(rel, 0);

	rel[wcslen(rel) - wcslen(L"\\leaf.txt")] = L'\0';
	CHECK(p0_area_path(g_area, rel, p, 4096));
	CHECK(WF_OK == wf_enum_dir_win(p, &d) && d && 1 == d->n &&
	        0 == wcscmp(d->e[0].name, L"leaf.txt"));
	wf_dir_free(d);
}

int main(void)
{
	wchar_t tmp[MAX_PATH], rep[512];
	int left;

	GetTempPathW(MAX_PATH, tmp);
	tmp[wcslen(tmp) - 1] = L'\0'; /* the trailing backslash */
	g_area = p0_area_create(tmp, "enum");
	CHECK(NULL != g_area);
	if (!g_area) {
		return 1;
	}
	empty_directory();
	directory_listing();
	unusable_paths_are_refused();
	more_records_than_one_buffer();
	path_beyond_max_path();
	left = p0_area_destroy(g_area, rep, 512);
	CHECK(0 == left);
	return TEST_RESULT();
}
