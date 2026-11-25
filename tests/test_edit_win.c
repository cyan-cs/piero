#include <stdio.h>

#include "../tools/p0/p0_common.h"
#include "../tools/p0/p0_lab.h"
#include "fs/wf_edit_win.h"
#include "fs/wf_weakid_win.h"
#include "wf_test.h"
#include "xfer/wf_probe_win.h"
#include "base/wf_macro.h"

static FILE *case_results;

static void case_row(const char *label,
        const char *operation,
        WfStatus status,
        const WfEditReport *report,
        const WfFileId *before,
        const WfFileId *after,
        int unchanged)
{
	unsigned i;
	if (!case_results) {
		return;
	}
	fprintf(case_results, "%s,%s,%d,%d,%lu,%llu,", label, operation, status,
	        report->name, report->error,
	        before ? (unsigned long long)before->volume : 0);
	if (before) {
		for (i = 0; i < 16; i++) {
			fprintf(case_results, "%02x", before->id[i]);
		}
	}
	fprintf(case_results, ",%llu,",
	        after ? (unsigned long long)after->volume : 0);
	if (after) {
		for (i = 0; i < 16; i++) {
			fprintf(case_results, "%02x", after->id[i]);
		}
	}
	fprintf(case_results, ",%d\n", unchanged);
}

static unsigned child_count(const wchar_t *parent)
{
	wchar_t pattern[1200];
	WIN32_FIND_DATAW data;
	HANDLE find;
	unsigned count = 0;
	swprintf(pattern, 1200, L"%ls\\*", parent);
	find = FindFirstFileW(pattern, &data);
	CHECK(INVALID_HANDLE_VALUE != find);
	if (INVALID_HANDLE_VALUE == find) {
		return ~0u;
	}
	do {
		if (wcscmp(data.cFileName, L".") &&
		        wcscmp(data.cFileName, L"..")) {
			count++;
		}
	} while (FindNextFileW(find, &data));
	CHECK(ERROR_NO_MORE_FILES == GetLastError());
	FindClose(find);
	return count;
}

/* Removing destination validation or any reserved-stem/control check must fail
 * this matrix: every rejection preserves both directory populations and the
 * source's independently measured identity and payload. */
static void rejected_name(
        P0Lab *lab, const char *label, const wchar_t *name, WfNameResult reason)
{
	wchar_t from[1024], to[1024], source[1024];
	WfEditReport report;
	WfProbeOps ops = wf_probe_win();
	WfNodeInfo a, b, item, after;
	WfStatus status;
	char hash[65], observed[65];
	unsigned mode, from_count, to_count;
	int unchanged;
	const char *operations[] = {"create-file", "create-directory", "rename",
	        "move", "rename-source", "move-source"};
	CHECK(p0_lab_path(lab, L"qualification-from", from, 1024));
	CHECK(p0_lab_path(lab, L"qualification-to", to, 1024));
	CHECK(p0_lab_path(lab, L"qualification-from\\source", source, 1024));
	CHECK(WF_OK == ops.node(NULL, from, &a));
	CHECK(WF_OK == ops.node(NULL, to, &b));
	CHECK(WF_OK == ops.node(NULL, source, &item));
	CHECK(p0_sha256_file(source, hash));
	from_count = child_count(from);
	to_count = child_count(to);
	for (mode = 0; mode < 6; mode++) {
		if (mode < 2) {
			status = wf_edit_create_checked(
			        to, &b.id, name, (int)mode, &report);
		} else if (2 == mode) {
			status = wf_edit_rename_checked(from, &a.id, L"source",
			        &item.id, name, &report);
		} else if (3 == mode) {
			status = wf_edit_move(from, &a.id, L"source", &item.id,
			        to, &b.id, name, &report);
		} else if (4 == mode) {
			status = wf_edit_rename_checked(from, &a.id, name,
			        &item.id, L"unexpected", &report);
		} else {
			status = wf_edit_move(from, &a.id, name, &item.id, to,
			        &b.id, L"unexpected", &report);
		}
		CHECK(WF_E_INVAL == status);
		CHECK(report.name == reason);
		CHECK(0 == report.error);
		memset(&after, 0, sizeof after);
		unchanged = WF_OK == ops.node(NULL, source, &after) &&
		        wf_fileid_equal(&item.id, &after.id) &&
		        p0_sha256_file(source, observed) &&
		        !strcmp(hash, observed) &&
		        child_count(from) == from_count &&
		        child_count(to) == to_count;
		CHECK(unchanged);
		case_row(label, operations[mode], status, &report, &item.id,
		        &after.id, unchanged);
	}
}

static void test_name_matrix(P0Lab *lab)
{
	static const wchar_t *stems[] = {
	        L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"};
	static const wchar_t *invalid[] = {L"", L".", L"..", L"trailing.",
	        L"trailing ", L"a:b", L"a::$DATA", L"a<b", L"a>b", L"a\"b",
	        L"a/b", L"a\\b", L"a|b", L"a?b", L"a*b",
	        L"a\x061c"
	        L"b",
	        L"a\x180e"
	        L"b",
	        L"a\x200b"
	        L"b",
	        L"a\x200c"
	        L"b",
	        L"a\x200d"
	        L"b",
	        L"a\x200e"
	        L"b",
	        L"a\x200f"
	        L"b",
	        L"a\x202a"
	        L"b",
	        L"a\x202b"
	        L"b",
	        L"a\x202c"
	        L"b",
	        L"a\x202d"
	        L"b",
	        L"a\x202e"
	        L"b",
	        L"a\x2060"
	        L"b",
	        L"a\x2066"
	        L"b",
	        L"a\x2069"
	        L"b",
	        L"a\xfeff"
	        L"b",
	        L"a\xd800", L"a\xdc00",
	        L"a\xd800"
	        L"b"};
	wchar_t parent[1024], name[300];
	WfEditReport report;
	unsigned i, j, k;
	char label[80];
	CHECK(p0_area_mkdir(lab->area, L"qualification-from"));
	CHECK(p0_area_mkdir(lab->area, L"qualification-to"));
	CHECK(p0_lab_path(lab, L"qualification-from", parent, 1024));
	CHECK(WF_OK == wf_edit_create(parent, L"source", 0, &report));
	CHECK(p0_area_track(lab->area, L"qualification-from\\source", 0));
	for (i = 0; i < WF_ARRAY_LEN(stems); i++) {
		for (j = 0; j < 3; j++) {
			swprintf(name, 300, L"%ls%ls", stems[i],
			        0 == j ? L""
			                : 1 == j ? L".txt"
			                         : L" .txt");
			for (k = 0; name[k]; k++) {
				if (name[k] >= L'A' && name[k] <= L'Z') {
					name[k] += L'a' - L'A';
				}
			}
			snprintf(label, sizeof label, "reserved-%u-%u", i, j);
			rejected_name(lab, label, name, WF_NAME_RESERVED);
		}
	}
	for (i = 0; i < 2; i++) {
		for (j = 1; j <= 12; j++) {
			for (k = 0; k < 2; k++) {
				wchar_t digit = (wchar_t)(j <= 9 ? L'0' + j
				                : 10 == j        ? 0xb9
				                : 11 == j        ? 0xb2
				                                 : 0xb3);
				swprintf(name, 300, L"%ls%lc%ls",
				        i ? L"lPt" : L"cOm", digit,
				        k ? L".log" : L"");
				snprintf(label, sizeof label, "device-%u-%u-%u",
				        i, j, k);
				rejected_name(
				        lab, label, name, WF_NAME_RESERVED);
			}
		}
	}
	for (i = 0; i < WF_ARRAY_LEN(invalid); i++) {
		snprintf(label, sizeof label, "invalid-%u", i);
		rejected_name(lab, label, invalid[i],
		        i ? WF_NAME_INVALID : WF_NAME_EMPTY);
	}
	/* NUL is the API terminator, so nonempty embedded-NUL inputs cannot be
	 * represented by this API. Empty above exercises C0 U+0000. */
	for (i = 1; i <= 159; i++) {
		if (i < 32 || i >= 127) {
			name[0] = L'a';
			name[1] = (wchar_t)i;
			name[2] = L'b';
			name[3] = 0;
			snprintf(label, sizeof label, "control-U%04X", i);
			rejected_name(lab, label, name, WF_NAME_INVALID);
		}
	}
	for (i = 0x2060; i <= 0x206f; i++) {
		name[0] = L'a';
		name[1] = (wchar_t)i;
		name[2] = L'b';
		name[3] = 0;
		snprintf(label, sizeof label, "invisible-U%04X", i);
		rejected_name(lab, label, name, WF_NAME_INVALID);
	}
	for (i = 0; i < 256; i++) {
		name[i] = L'x';
	}
	name[256] = 0;
	rejected_name(lab, "component-256", name, WF_NAME_TOO_LONG);
}

static void test_allowed_matrix(P0Lab *lab)
{
	static const wchar_t *names[] = {L"COM0", L"LPT0", L"COM10.txt",
	        L"LPT10.txt", L".hidden", L"\x6f22\x5b57\xd83d\xde00.txt",
	        L"\x00e9", L"e\x0301"};
	wchar_t from[1024], to[1024], source[1024], target[1024], rel[1200],
	        name[257];
	WfProbeOps ops = wf_probe_win();
	WfNodeInfo a, b, item, after;
	WfEditReport report;
	WfStatus status;
	unsigned i, j;
	char label[80];
	int identity;
	CHECK(p0_area_mkdir(lab->area, L"allowed-from"));
	CHECK(p0_area_mkdir(lab->area, L"allowed-to"));
	CHECK(p0_lab_path(lab, L"allowed-from", from, 1024));
	CHECK(p0_lab_path(lab, L"allowed-to", to, 1024));
	CHECK(WF_OK == ops.node(NULL, from, &a));
	CHECK(WF_OK == ops.node(NULL, to, &b));
	for (i = 0; i <= WF_ARRAY_LEN(names); i++) {
		if (i == WF_ARRAY_LEN(names)) {
			for (j = 0; j < 255; j++) {
				name[j] = L'\x6f22';
			}
			name[255] = 0;
		} else {
			wcscpy_s(name, 257, names[i]);
		}
		snprintf(label, sizeof label, "allowed-%u", i);
		status = wf_edit_create_checked(from, &a.id, name, 0, &report);
		CHECK(WF_OK == status);
		swprintf(rel, 1200, L"allowed-from\\%ls", name);
		CHECK(p0_area_track(lab->area, rel, 0));
		CHECK(p0_lab_path(lab, rel, source, 1024));
		CHECK(WF_OK == ops.node(NULL, source, &item));
		case_row(label, "create-file", status, &report, NULL, &item.id,
		        1);
		status = wf_edit_rename_checked(
		        from, &a.id, name, &item.id, L"temporary", &report);
		CHECK(WF_OK == status);
		CHECK(p0_area_track(lab->area, L"allowed-from\\temporary", 0));
		CHECK(p0_lab_path(
		        lab, L"allowed-from\\temporary", source, 1024));
		CHECK(WF_OK == ops.node(NULL, source, &after));
		identity = wf_fileid_equal(&item.id, &after.id);
		CHECK(identity);
		case_row(label, "rename", status, &report, &item.id, &after.id,
		        identity);
		status = wf_edit_move(from, &a.id, L"temporary", &item.id, to,
		        &b.id, name, &report);
		CHECK(WF_OK == status);
		swprintf(rel, 1200, L"allowed-to\\%ls", name);
		CHECK(p0_area_track(lab->area, rel, 0));
		CHECK(p0_lab_path(lab, rel, target, 1024));
		CHECK(WF_OK == ops.node(NULL, target, &after));
		identity = wf_fileid_equal(&item.id, &after.id);
		CHECK(identity);
		case_row(label, "move", status, &report, &item.id, &after.id,
		        identity);
	}
	/* Both spellings coexist, proving that the API did not NFC-normalize.
	 */
	CHECK(child_count(to) == WF_ARRAY_LEN(names) + 1);
}

static void test_collision_matrix(P0Lab *lab)
{
	wchar_t from[1024], to[1024], source[1024], target[1024], rel[300];
	WfProbeOps ops = wf_probe_win();
	WfNodeInfo a, b, item, destination, after, other;
	WfEditReport report;
	WfStatus status;
	unsigned kind, mode;
	char label[80];
	int unchanged;
	for (kind = 0; kind < 4; kind++) {
		swprintf(rel, 300, L"collision-%u-from", kind);
		CHECK(p0_area_mkdir(lab->area, rel));
		CHECK(p0_lab_path(lab, rel, from, 1024));
		swprintf(rel, 300, L"collision-%u-to", kind);
		CHECK(p0_area_mkdir(lab->area, rel));
		CHECK(p0_lab_path(lab, rel, to, 1024));
		CHECK(WF_OK ==
		        wf_edit_create(from, L"source", !!(kind & 1), &report));
		swprintf(rel, 300, L"collision-%u-from\\source", kind);
		CHECK(p0_area_track(lab->area, rel, !!(kind & 1)));
		CHECK(p0_lab_path(lab, rel, source, 1024));
		CHECK(wf_edit_create(to, L"destination", !!(kind & 2),
		              &report) == WF_OK);
		swprintf(rel, 300, L"collision-%u-to\\destination", kind);
		CHECK(p0_area_track(lab->area, rel, !!(kind & 2)));
		CHECK(p0_lab_path(lab, rel, target, 1024));
		if (kind & 1) {
			CHECK(wf_edit_create(source, L"source-child", 0,
			              &report) == WF_OK);
			swprintf(rel, 300,
			        L"collision-%u-from\\source\\source-child",
			        kind);
			CHECK(p0_area_track(lab->area, rel, 0));
		}
		if (kind & 2) {
			CHECK(wf_edit_create(target, L"destination-child", 0,
			              &report) == WF_OK);
			swprintf(rel, 300,
			        L"collision-%u-to\\destination\\destination-"
			        L"child",
			        kind);
			CHECK(p0_area_track(lab->area, rel, 0));
		}
		CHECK(WF_OK == ops.node(NULL, from, &a));
		CHECK(WF_OK == ops.node(NULL, to, &b));
		CHECK(WF_OK == ops.node(NULL, source, &item));
		CHECK(WF_OK == ops.node(NULL, target, &destination));
		for (mode = 0; mode < 3; mode++) {
			if (mode < 2) {
				status = wf_edit_create(
				        to, L"destination", (int)mode, &report);
			} else {
				status = wf_edit_move(from, &a.id, L"source",
				        &item.id, to, &b.id, L"destination",
				        &report);
			}
			CHECK(WF_OK != status);
			CHECK(0 != report.error);
			CHECK(WF_OK == ops.node(NULL, source, &after));
			CHECK(WF_OK == ops.node(NULL, target, &other));
			unchanged = wf_fileid_equal(&item.id, &after.id) &&
			        wf_fileid_equal(&destination.id, &other.id) &&
			        1 == child_count(from) &&
			        1 == child_count(to) &&
			        (!(kind & 1) || 1 == child_count(source)) &&
			        (!(kind & 2) || 1 == child_count(target));
			CHECK(unchanged);
			snprintf(label, sizeof label,
			        "collision-source%u-destination%u",
			        !!(kind & 1), !!(kind & 2));
			case_row(label,
			        0 == mode           ? "create-file"
			                : 1 == mode ? "create-directory"
			                            : "move",
			        status, &report, &destination.id, &other.id,
			        unchanged);
		}
	}
}

static void test_expected_parent(P0Lab *lab)
{
	wchar_t parent[1024], source[1024], target[1024], rel[300];
	WfProbeOps ops = wf_probe_win();
	WfNodeInfo directory, item, after;
	WfEditReport report;
	WfFileId wrong, invalid = {0};
	const WfFileId *expected;
	WfStatus status;
	unsigned mode, kind;
	char label[80];
	int unchanged;
	for (mode = 0; mode < 3; mode++) {
		for (kind = 0; kind < 4; kind++) {
			swprintf(
			        rel, 300, L"expected-parent-%u-%u", mode, kind);
			CHECK(p0_area_mkdir(lab->area, rel));
			CHECK(p0_lab_path(lab, rel, parent, 1024));
			CHECK(WF_OK ==
			        wf_edit_create(parent, L"source", 0, &report));
			swprintf(rel, 300, L"expected-parent-%u-%u\\source",
			        mode, kind);
			CHECK(p0_area_track(lab->area, rel, 0));
			CHECK(p0_lab_path(lab, rel, source, 1024));
			CHECK(WF_OK == ops.node(NULL, parent, &directory));
			CHECK(WF_OK == ops.node(NULL, source, &item));
			wrong = directory.id;
			wrong.id[0] ^= 255;
			if (3 == kind) {
				wrong = directory.id;
				wrong.volume ^= 1;
			}
			expected = 0 == kind || 3 == kind ? &wrong
			        : 1 == kind               ? &invalid
			                                  : NULL;
			if (mode < 2) {
				status = wf_edit_create_checked(parent,
				        expected, L"unexpected", (int)mode,
				        &report);
			} else {
				status = wf_edit_rename_checked(parent,
				        expected, L"source", &item.id,
				        L"unexpected", &report);
			}
			if (WF_OK == status) {
				swprintf(rel, 300,
				        L"expected-parent-%u-%u\\unexpected",
				        mode, kind);
				CHECK(p0_area_track(lab->area, rel, 1 == mode));
			}
			CHECK(WF_E_INVAL == status);
			CHECK(0 == report.error);
			CHECK(WF_OK == ops.node(NULL, parent, &after));
			swprintf(target, 1024, L"%ls\\unexpected", parent);
			unchanged = wf_fileid_equal(&directory.id, &after.id) &&
			        1 == child_count(parent) &&
			        INVALID_FILE_ATTRIBUTES !=
			                GetFileAttributesW(source) &&
			        INVALID_FILE_ATTRIBUTES ==
			                GetFileAttributesW(target);
			CHECK(unchanged);
			snprintf(label, sizeof label, "expected-parent-%s",
			        0 == kind           ? "mismatch"
			                : 1 == kind ? "invalid"
			                : 2 == kind ? "missing"
			                            : "wrong-volume");
			case_row(label,
			        0 == mode           ? "create-file-checked"
			                : 1 == mode ? "create-directory-checked"
			                            : "rename-checked",
			        status, &report, &directory.id, &after.id,
			        unchanged);
		}
	}
}

static int junction(P0Lab *lab, const wchar_t *relative, const wchar_t *target)
{
	wchar_t link[1024], command[4096], system_cmd[1024];
	STARTUPINFOW si = {0};
	PROCESS_INFORMATION pi = {0};
	DWORD code = 99, wait;
	if (!p0_lab_path(lab, relative, link, 1024) ||
	        !GetSystemDirectoryW(system_cmd, 1024)) {
		return 0;
	}
	wcscat_s(system_cmd, 1024, L"\\cmd.exe");
	swprintf(command, 4096,
	        L"\"%ls\" /c mklink /J \"%ls\" \"%ls\" >nul 2>nul", system_cmd,
	        link, target);
	si.cb = sizeof si;
	if (!CreateProcessW(system_cmd, command, NULL, NULL, FALSE,
	            CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
		return 0;
	}
	wait = WaitForSingleObject(pi.hProcess, 5000);
	if (WAIT_OBJECT_0 == wait) {
		GetExitCodeProcess(pi.hProcess, &code);
	}
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return 0 == code && p0_area_track(lab->area, relative, 1);
}

static void test_reparse_refused(P0Lab *lab)
{
	wchar_t parent[1024], target[1024], link[1024];
	WfEditReport report;
	WfProbeOps ops = wf_probe_win();
	WfNodeInfo item, directory, after;
	WfStatus status;
	CHECK(p0_area_mkdir(lab->area, L"reparse-target"));
	CHECK(p0_lab_path(lab, L"reparse-target", target, 1024));
	wcscpy_s(parent, 1024, lab->root);
	CHECK(junction(lab, L"edit-junction", target));
	CHECK(p0_lab_path(lab, L"edit-junction", link, 1024));
	CHECK(WF_OK == ops.node(NULL, link, &item));
	CHECK(item.flags & WF_ENT_LINK);
	CHECK(WF_OK == ops.node(NULL, parent, &directory));
	CHECK(wf_edit_rename(parent, L"edit-junction", &item.id,
	              L"renamed-junction", &report) == WF_E_INVAL);
	CHECK(wf_edit_move(parent, &directory.id, L"edit-junction", &item.id,
	              parent, &directory.id, L"moved-junction",
	              &report) == WF_E_INVAL);
	CHECK(WF_E_INVAL == wf_edit_create(link, L"unexpected", 0, &report));
	CHECK(INVALID_FILE_ATTRIBUTES != GetFileAttributesW(link));
	/* A checked leaf alone would miss a junction in an earlier component.
	 */
	CHECK(p0_area_mkdir(lab->area, L"reparse-target\\inner"));
	CHECK(p0_lab_path(lab, L"reparse-target\\inner", target, 1024));
	CHECK(WF_OK == wf_edit_create(target, L"source", 0, &report));
	CHECK(p0_area_track(lab->area, L"reparse-target\\inner\\source", 0));
	CHECK(p0_lab_path(lab, L"reparse-target\\inner\\source", target, 1024));
	CHECK(WF_OK == ops.node(NULL, target, &item));
	wcscat_s(link, 1024, L"\\inner");
	status = wf_edit_create(link, L"unexpected", 0, &report);
	CHECK(WF_E_INVAL == status);
	CHECK(WF_OK == ops.node(NULL, target, &after));
	case_row("ancestor-junction", "create-file", status, &report, &item.id,
	        &after.id,
	        wf_fileid_equal(&item.id, &after.id) && 1 == child_count(link));
	status = wf_edit_rename(link, L"source", &item.id, L"renamed", &report);
	CHECK(WF_E_INVAL == status);
	CHECK(WF_OK == ops.node(NULL, target, &after));
	case_row("ancestor-junction", "rename", status, &report, &item.id,
	        &after.id,
	        wf_fileid_equal(&item.id, &after.id) && 1 == child_count(link));
	CHECK(WF_OK == ops.node(NULL, link, &directory));
	status = wf_edit_move(link, &directory.id, L"source", &item.id, parent,
	        &directory.id, L"moved", &report);
	CHECK(WF_E_INVAL == status);
	CHECK(WF_OK == ops.node(NULL, target, &after));
	case_row("source-ancestor-junction", "move", status, &report, &item.id,
	        &after.id,
	        wf_fileid_equal(&item.id, &after.id) && 1 == child_count(link));
}

/* 0 missing, 1 folder, 2 file (a link counts as what it is, not followed). */
static int entry_kind(P0Lab *lab, const wchar_t *rel)
{
	wchar_t path[1024];
	DWORD attrs;

	if (!p0_lab_path(lab, rel, path, 1024)) {
		return -1;
	}
	attrs = GetFileAttributesW(path);
	if (INVALID_FILE_ATTRIBUTES == attrs) {
		return 0;
	}
	return attrs & FILE_ATTRIBUTE_DIRECTORY ? 1 : 2;
}

/* A weak identity (a FAT volume) is only ever met by a weak one. On a volume
 * that has proven identities it is never the folder that was listed, so a new
 * name is not made, and no folder on the way is made either. */
static void test_weak_identity_is_not_a_proven_folder(P0Lab *lab)
{
	wchar_t base[1024];
	WfEditReport report;
	WfProbeOps ops = wf_probe_win();
	WfNodeInfo dir;
	WfFileId weak;

	CHECK(p0_area_mkdir(lab->area, L"wk"));
	CHECK(p0_lab_path(lab, L"wk", base, 1024));
	CHECK(WF_OK == ops.node(NULL, base, &dir));
	CHECK((dir.id.kind == WF_ID_STRONG) && !wf_fileid_is_weak(&dir.id));
	/* the same bytes, claimed as a weak identity */
	wf_weak_fill(&weak, dir.id.volume, 4242);
	CHECK(WF_OK !=
	        wf_edit_create_checked(base, &weak, L"x.txt", 0, &report));
	CHECK(WF_OK !=
	        wf_edit_create_path_checked(
	                base, &weak, L"p\\q\\x.txt", 0, &report));
	CHECK(0 == entry_kind(lab, L"wk\\x.txt") &&
	        0 == entry_kind(lab, L"wk\\p"));
	/* neither proven nor weak: nothing to stand on */
	memset(&weak, 0, sizeof weak);
	CHECK(WF_E_INVAL ==
	        wf_edit_create_checked(base, &weak, L"x.txt", 0, &report));
	/* a rename never takes a weak identity */
	CHECK(WF_OK ==
	        wf_edit_create_checked(base, &dir.id, L"r.txt", 0, &report));
	CHECK(p0_area_track(lab->area, L"wk\\r.txt", 0));
	wf_weak_fill(&weak, dir.id.volume, 4242);
	CHECK(WF_OK !=
	        wf_edit_rename_checked(
	                base, &weak, L"r.txt", &weak, L"s.txt", &report));
	CHECK(2 == entry_kind(lab, L"wk\\r.txt") &&
	        0 == entry_kind(lab, L"wk\\s.txt"));
}

/* A new name "a/b/c" makes the folders on the way, enters the ones that exist,
 * and never goes through a file, a link, "..", or a parent that has changed. */
static void test_create_path(P0Lab *lab)
{
	wchar_t base[1024], outside[1024], linked[1024];
	WfEditReport report;
	WfProbeOps ops = wf_probe_win();
	WfNodeInfo dir, other;
	WfFileId stale;
	WfStatus status;

	CHECK(p0_area_mkdir(lab->area, L"cp"));
	CHECK(p0_lab_path(lab, L"cp", base, 1024));
	CHECK(WF_OK == ops.node(NULL, base, &dir));

	/* both folders on the way are made, the file is inside the last */
	status = wf_edit_create_path_checked(
	        base, &dir.id, L"a/b/hello", 0, &report);
	CHECK(WF_OK == status);
	CHECK(p0_area_track(lab->area, L"cp\\a", 1));
	CHECK(p0_area_track(lab->area, L"cp\\a\\b", 1));
	CHECK(p0_area_track(lab->area, L"cp\\a\\b\\hello", 0));
	CHECK(1 == entry_kind(lab, L"cp\\a") &&
	        1 == entry_kind(lab, L"cp\\a\\b") &&
	        2 == entry_kind(lab, L"cp\\a\\b\\hello"));

	/* existing folders are entered, a folder is made as the last part */
	status = wf_edit_create_path_checked(
	        base, &dir.id, L"a/b/sub", 1, &report);
	CHECK(WF_OK == status);
	CHECK(p0_area_track(lab->area, L"cp\\a\\b\\sub", 1));
	CHECK(1 == entry_kind(lab, L"cp\\a\\b\\sub") &&
	        2 == entry_kind(lab, L"cp\\a\\b\\hello"));

	/* a backslash separates too */
	status =
	        wf_edit_create_path_checked(base, &dir.id, L"a\\c", 0, &report);
	CHECK(WF_OK == status);
	CHECK(p0_area_track(lab->area, L"cp\\a\\c", 0));
	CHECK(2 == entry_kind(lab, L"cp\\a\\c"));

	/* the name at the end is taken: nothing is replaced */
	status = wf_edit_create_path_checked(
	        base, &dir.id, L"a/b/hello", 0, &report);
	CHECK(WF_OK != status && ERROR_FILE_EXISTS == report.error);
	CHECK(2 == entry_kind(lab, L"cp\\a\\b\\hello"));

	/* a file on the way is not entered */
	status = wf_edit_create_path_checked(
	        base, &dir.id, L"a/c/x", 0, &report);
	CHECK(WF_OK != status && ERROR_ALREADY_EXISTS == report.error);
	CHECK(2 == entry_kind(lab, L"cp\\a\\c"));

	/* ".." does not leave the folder; what comes before it may stay */
	status = wf_edit_create_path_checked(
	        base, &dir.id, L"a/../outside", 0, &report);
	CHECK(WF_E_INVAL == status && WF_NAME_OK != report.name);
	CHECK(0 == entry_kind(lab, L"outside"));
	CHECK(0 == entry_kind(lab, L"cp\\outside"));

	/* a link on the way is not entered: nothing appears behind it */
	CHECK(p0_area_mkdir(lab->area, L"cp-target"));
	CHECK(p0_lab_path(lab, L"cp-target", outside, 1024));
	CHECK(junction(lab, L"cp\\link", outside));
	status = wf_edit_create_path_checked(
	        base, &dir.id, L"link/x", 0, &report);
	CHECK(WF_OK != status);
	CHECK(0 == entry_kind(lab, L"cp-target\\x"));
	CHECK(p0_lab_path(lab, L"cp\\link", linked, 1024));
	CHECK(WF_OK == ops.node(NULL, linked, &other));
	CHECK(other.flags & WF_ENT_LINK);

	/* the folder is not the one that was seen: nothing is made */
	CHECK(WF_OK == ops.node(NULL, base, &other));
	stale = other.id;
	stale.id[0] ^= 255;
	status = wf_edit_create_path_checked(base, &stale, L"z/y", 0, &report);
	CHECK(WF_E_INVAL == status);
	CHECK(0 == entry_kind(lab, L"cp\\z"));
}

/* Qualify the NTFS kernel property used by parent_check_chain: holding a
 * descendant without share-delete prevents renaming an ancestor. Removing
 * this sharing restriction must make this assertion fail on this volume. */
static void test_ancestor_pin(P0Lab *lab)
{
	wchar_t ancestor[1024], descendant[1024];
	WfProbeOps ops = wf_probe_win();
	WfNodeInfo before, after;
	WfEditReport report;
	HANDLE pin;
	WfStatus status;
	int unchanged;
	CHECK(p0_area_mkdir(lab->area, L"pin-ancestor"));
	CHECK(p0_area_mkdir(lab->area, L"pin-ancestor\\descendant"));
	CHECK(p0_lab_path(lab, L"pin-ancestor", ancestor, 1024));
	CHECK(p0_lab_path(lab, L"pin-ancestor\\descendant", descendant, 1024));
	CHECK(WF_OK == ops.node(NULL, ancestor, &before));
	pin = CreateFileW(descendant, FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
	        NULL, OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	CHECK(INVALID_HANDLE_VALUE != pin);
	status = wf_edit_rename(lab->root, L"pin-ancestor", &before.id,
	        L"pin-renamed", &report);
	if (WF_OK == status) {
		CHECK(p0_area_track(lab->area, L"pin-renamed", 1));
		CHECK(p0_area_track(lab->area, L"pin-renamed\\descendant", 1));
	}
	CHECK(WF_OK != status);
	CHECK(0 != report.error);
	memset(&after, 0, sizeof after);
	unchanged = WF_OK == ops.node(NULL, ancestor, &after) &&
	        wf_fileid_equal(&before.id, &after.id);
	CHECK(unchanged);
	case_row("descendant-pins-ancestor", "rename", status, &report,
	        &before.id, &after.id, unchanged);
	if (INVALID_HANDLE_VALUE != pin) {
		CloseHandle(pin);
	}
}

static void test_writer_excludes_rename(P0Lab *lab)
{
	wchar_t parent[1024], source[1024];
	WfEditReport report;
	WfNodeInfo item;
	WfProbeOps ops = wf_probe_win();
	HANDLE writer;
	WfStatus status;
	CHECK(p0_area_mkdir(lab->area, L"writer"));
	CHECK(p0_lab_path(lab, L"writer", parent, 1024));
	CHECK(WF_OK == wf_edit_create(parent, L"item", 0, &report));
	CHECK(p0_area_track(lab->area, L"writer\\item", 0));
	CHECK(p0_lab_path(lab, L"writer\\item", source, 1024));
	CHECK(WF_OK == ops.node(NULL, source, &item));
	writer = CreateFileW(source, GENERIC_WRITE,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING, 0, NULL);
	CHECK(INVALID_HANDLE_VALUE != writer);
	status = wf_edit_rename(parent, L"item", &item.id, L"renamed", &report);
	if (WF_OK == status) {
		CHECK(p0_area_track(lab->area, L"writer\\renamed", 0));
	}
	CHECK(WF_E_ACCESS == status);
	CHECK(ERROR_SHARING_VIOLATION == report.error);
	CHECK(INVALID_FILE_ATTRIBUTES != GetFileAttributesW(source));
	case_row("active-writer", "rename", status, &report, &item.id, NULL,
	        INVALID_FILE_ATTRIBUTES != GetFileAttributesW(source));
	if (INVALID_HANDLE_VALUE != writer) {
		CloseHandle(writer);
	}
}

static void test_move(P0Lab *lab)
{
	wchar_t from[1024], to[1024], source[1024], target[1024];
	WfProbeOps ops = wf_probe_win();
	WfNodeInfo a, b, item, after;
	WfEditReport report;
	WfFileId wrong;
	char hash[65], observed[65], source_hash[65];
	HANDLE file;
	DWORD written;
	WfStatus status;
	CHECK(p0_area_mkdir(lab->area, L"move-from"));
	CHECK(p0_area_mkdir(lab->area, L"move-to"));
	CHECK(p0_lab_path(lab, L"move-from", from, 1024));
	CHECK(p0_lab_path(lab, L"move-to", to, 1024));
	CHECK(WF_OK == wf_edit_create(from, L"item", 0, &report));
	CHECK(p0_area_track(lab->area, L"move-from\\item", 0));
	CHECK(WF_OK == wf_edit_create(to, L"collision", 0, &report));
	CHECK(p0_area_track(lab->area, L"move-to\\collision", 0));
	CHECK(p0_lab_path(lab, L"move-from\\item", source, 1024));
	file = CreateFileW(
	        source, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	CHECK(INVALID_HANDLE_VALUE != file);
	CHECK(WriteFile(file, "move-payload", 12, &written, NULL));
	CHECK(12 == written);
	if (INVALID_HANDLE_VALUE != file) {
		CloseHandle(file);
	}
	CHECK(p0_sha256_file(source, source_hash));
	CHECK(p0_lab_path(lab, L"move-to\\collision", target, 1024));
	CHECK(WF_OK == ops.node(NULL, from, &a));
	CHECK(WF_OK == ops.node(NULL, to, &b));
	CHECK(WF_OK == ops.node(NULL, source, &item));
	CHECK(p0_sha256_file(target, hash));
	status = wf_edit_move(from, &a.id, L"item", &item.id, to, &b.id,
	        L"collision", &report);
	CHECK(WF_OK != status);
	CHECK(0 != report.error);
	CHECK(p0_sha256_file(target, observed) && !strcmp(hash, observed));
	CHECK(p0_sha256_file(source, observed) &&
	        !strcmp(source_hash, observed));
	CHECK(WF_OK == ops.node(NULL, source, &after));
	case_row("no-clobber-file", "move", status, &report, &item.id,
	        &after.id,
	        wf_fileid_equal(&item.id, &after.id) &&
	                p0_sha256_file(target, observed) &&
	                !strcmp(hash, observed) &&
	                p0_sha256_file(source, observed) &&
	                !strcmp(source_hash, observed));
	wrong = a.id;
	wrong.id[0] ^= 255;
	status = wf_edit_move(
	        from, &wrong, L"item", &item.id, to, &b.id, L"moved", &report);
	CHECK(WF_E_INVAL == status);
	CHECK(WF_OK == ops.node(NULL, source, &after));
	case_row("wrong-source-parent-id", "move", status, &report, &item.id,
	        &after.id, wf_fileid_equal(&item.id, &after.id));
	wrong = b.id;
	wrong.id[0] ^= 255;
	status = wf_edit_move(
	        from, &a.id, L"item", &item.id, to, &wrong, L"moved", &report);
	CHECK(WF_E_INVAL == status);
	CHECK(WF_OK == ops.node(NULL, source, &after));
	case_row("wrong-destination-parent-id", "move", status, &report,
	        &item.id, &after.id, wf_fileid_equal(&item.id, &after.id));
	wrong = item.id;
	wrong.id[0] ^= 255;
	status = wf_edit_move(
	        from, &a.id, L"item", &wrong, to, &b.id, L"moved", &report);
	CHECK(WF_E_INVAL == status);
	CHECK(WF_OK == ops.node(NULL, source, &after));
	case_row("wrong-source-id", "move", status, &report, &item.id,
	        &after.id, wf_fileid_equal(&item.id, &after.id));
	CHECK(INVALID_FILE_ATTRIBUTES != GetFileAttributesW(source));
	CHECK(wf_edit_move(from, &a.id, L"item", &item.id, to, &b.id, L"moved",
	              &report) == WF_OK);
	if (INVALID_FILE_ATTRIBUTES == GetFileAttributesW(source)) {
		CHECK(p0_area_track(lab->area, L"move-to\\moved", 0));
		CHECK(p0_lab_path(lab, L"move-to\\moved", target, 1024));
		CHECK(WF_OK == ops.node(NULL, target, &after));
		CHECK(wf_fileid_equal(&item.id, &after.id));
		CHECK(p0_sha256_file(target, observed) &&
		        !strcmp(source_hash, observed));
	}
	CHECK(WF_OK == wf_edit_create(from, L"empty-dir", 1, &report));
	CHECK(p0_area_track(lab->area, L"move-from\\empty-dir", 1));
	CHECK(p0_lab_path(lab, L"move-from\\empty-dir", source, 1024));
	CHECK(WF_OK == ops.node(NULL, source, &item));
	CHECK(wf_edit_move(from, &a.id, L"empty-dir", &item.id, to, &b.id,
	              L"dir-moved", &report) == WF_OK);
	CHECK(p0_area_track(lab->area, L"move-to\\dir-moved", 1));
	CHECK(p0_lab_path(lab, L"move-to\\dir-moved", target, 1024));
	CHECK(WF_OK == ops.node(NULL, target, &after));
	CHECK(wf_fileid_equal(&item.id, &after.id));
	CHECK(wf_edit_move(to, &b.id, L"dir-moved", &item.id, to, &b.id,
	              L"DIR-MOVED", &report) == WF_OK);
	CHECK(p0_area_track(lab->area, L"move-to\\DIR-MOVED", 1));
}

int main(int argc, char **argv)
{
	P0Lab lab;
	WfEditReport report;
	WfProbeOps ops = wf_probe_win();
	WfNodeInfo before, after;
	wchar_t parent[1024], source[1024], destination[1024], name[256],
	        rel[300];
	char original[65], collision[65], observed[65];
	HANDLE file;
	DWORD written, h0, h1;
	unsigned i;
	WfStatus status;
	if (1 != argc && (3 != argc || strcmp(argv[1], "--results"))) {
		return 2;
	}
	if (3 == argc) {
		if (fopen_s(&case_results, argv[2], "wb") || !case_results) {
			return 2;
		}
		fputs("case,operation,status,name_result,win32_error,before_"
		      "volume,before_fileid,after_volume,after_fileid,"
		      "invariant_preserved\n",
		        case_results);
	}
	CHECK(p0_lab_open(&lab, "names", 1));
	if (!lab.area) {
		return 1;
	}
	CHECK(p0_area_mkdir(lab.area, L"edit"));
	CHECK(p0_lab_path(&lab, L"edit", parent, 1024));
	/* Warm process creation before measuring edit-backend handles. */
	test_reparse_refused(&lab);
	test_create_path(&lab);
	test_weak_identity_is_not_a_proven_folder(&lab);
	CHECK(GetProcessHandleCount(GetCurrentProcess(), &h0));
	CHECK(WF_E_INVAL == wf_edit_create(parent, L"NUL.txt", 0, &report));
	CHECK(WF_OK == wf_edit_create(parent, L"COM0", 0, &report));
	CHECK(p0_area_track(lab.area, L"edit\\COM0", 0));
	status = wf_edit_create(parent, L"directory", 1, &report);
	CHECK(WF_OK == status);
	CHECK(p0_area_track(lab.area, L"edit\\directory", 1));
	CHECK(p0_lab_path(&lab, L"edit\\directory", source, 1024));
	CHECK(WF_OK == ops.node(NULL, source, &after));
	case_row("allowed-directory", "create-directory", status, &report, NULL,
	        &after.id, !!(after.flags & WF_ENT_DIR));
	CHECK(WF_OK == wf_edit_create(parent, L"漢字😀.txt", 0, &report));
	CHECK(p0_area_track(lab.area, L"edit\\漢字😀.txt", 0));
	CHECK(p0_lab_path(&lab, L"edit\\COM0", source, 1024));
	file = CreateFileW(
	        source, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	CHECK(INVALID_HANDLE_VALUE != file);
	CHECK(WriteFile(file, "original", 8, &written, NULL));
	CloseHandle(file);
	CHECK(WF_OK == ops.node(NULL, source, &before));
	CHECK(p0_sha256_file(source, original));
	CHECK(WF_OK == wf_edit_create(parent, L"collision", 0, &report));
	CHECK(p0_area_track(lab.area, L"edit\\collision", 0));
	CHECK(p0_lab_path(&lab, L"edit\\collision", destination, 1024));
	CHECK(p0_sha256_file(destination, collision));
	status = wf_edit_create(parent, L"COM0", 0, &report);
	CHECK(WF_OK != status);
	CHECK(p0_sha256_file(source, observed) && !strcmp(original, observed));
	CHECK(WF_OK == ops.node(NULL, source, &after));
	case_row("no-clobber-file", "create-file", status, &report, &before.id,
	        &after.id,
	        wf_fileid_equal(&before.id, &after.id) &&
	                !strcmp(original, observed));
	status = wf_edit_rename(
	        parent, L"COM0", &before.id, L"collision", &report);
	CHECK(WF_OK != status);
	CHECK(p0_sha256_file(source, observed) && !strcmp(original, observed));
	CHECK(p0_sha256_file(destination, observed) &&
	        !strcmp(collision, observed));
	CHECK(WF_OK == ops.node(NULL, source, &after));
	case_row("no-clobber-file", "rename", status, &report, &before.id,
	        &after.id,
	        wf_fileid_equal(&before.id, &after.id) &&
	                p0_sha256_file(destination, observed) &&
	                !strcmp(collision, observed) &&
	                p0_sha256_file(source, observed) &&
	                !strcmp(original, observed));
	before.id.id[0] ^= 255;
	CHECK(wf_edit_rename(parent, L"COM0", &before.id, L"changed",
	              &report) != WF_OK);
	CHECK(INVALID_FILE_ATTRIBUTES != GetFileAttributesW(source));
	before.id.id[0] ^= 255;
	status = wf_edit_rename(parent, L"COM0", &before.id, L"com0", &report);
	CHECK(WF_OK == status);
	CHECK(p0_area_track(lab.area, L"edit\\com0", 0));
	CHECK(WF_OK == ops.node(NULL, source, &after));
	CHECK(wf_fileid_equal(&before.id, &after.id));
	{
		WIN32_FIND_DATAW data;
		HANDLE find = FindFirstFileW(source, &data);
		CHECK(INVALID_HANDLE_VALUE != find);
		if (INVALID_HANDLE_VALUE != find) {
			CHECK(!wcscmp(data.cFileName, L"com0"));
			case_row("case-only-spelling-observed", "rename",
			        status, &report, &before.id, &after.id,
			        wf_fileid_equal(&before.id, &after.id) &&
			                !wcscmp(data.cFileName, L"com0"));
			FindClose(find);
		}
	}
	for (i = 0; i < 255; i++) {
		name[i] = L'漢';
	}
	name[255] = 0;
	CHECK(WF_OK == wf_edit_create(parent, name, 0, &report));
	swprintf(rel, 300, L"edit\\%ls", name);
	CHECK(p0_area_track(lab.area, rel, 0));
	test_move(&lab);
	test_writer_excludes_rename(&lab);
	test_ancestor_pin(&lab);
	test_name_matrix(&lab);
	test_allowed_matrix(&lab);
	test_collision_matrix(&lab);
	test_expected_parent(&lab);
	CHECK(GetProcessHandleCount(GetCurrentProcess(), &h1));
	CHECK(h0 == h1);
	printf("N01 case-only rename, Unicode, 255-unit name, collision "
	       "hashes, identity rejection; error=%lu handles=%lu/%lu\n",
	        report.error, h0, h1);
	if (3 == argc) {
		wchar_t environment[1200];
		CHECK(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		              argv[2], -1, environment, 1100) > 0);
		wcscat_s(environment, 1200, L".environment.json");
		CHECK(p0_write_environment(environment, p0_area_root(lab.area),
		        "test_edit_win", "namegate-1"));
		CHECK(0 == fclose(case_results));
		case_results = NULL;
	}
	CHECK(0 == p0_lab_close(&lab, 0 != wf_test_failures));
	return TEST_RESULT();
}
