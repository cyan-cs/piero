#include "wf_edraft_win.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../base/wf_macro.h"
#include "../base/wf_slotfile.h"
#include "../base/wf_state_dir.h"
#include "../fs/owned_delete.h"

#define DRAFT_MAX_BYTES (2u << 20)
#define DIR_MAX 520
#define FILE_MAX_CHARS (DIR_MAX + 40)

static wchar_t g_dir[DIR_MAX];

void wf_draft_set_dir(const wchar_t *dir)
{
	if (dir) {
		wcsncpy_s(g_dir, DIR_MAX, dir, _TRUNCATE);
	} else {
		g_dir[0] = L'\0';
	}
}

static bool has_text(const wchar_t *hay, const wchar_t *needle)
{
	size_t n = wcslen(needle), h = wcslen(hay);

	for (size_t i = 0; i + n <= h; i++) {
		if (0 == _wcsnicmp(hay + i, needle, n)) {
			return true;
		}
	}
	return false;
}

static bool ends_with(const wchar_t *s, const wchar_t *tail)
{
	size_t n = wcslen(s), t = wcslen(tail);

	return n >= t && 0 == _wcsicmp(s + n - t, tail);
}

static bool starts_with(const wchar_t *s, const wchar_t *head)
{
	return 0 == _wcsnicmp(s, head, wcslen(head));
}

bool wf_draft_allowed(const wchar_t *path, size_t bytes)
{
	static const wchar_t *const tails[] = {L".key", L".pem", L".pfx",
	        L".p12", L".kdbx", L".ppk", L".gpg", L".asc", L".keystore",
	        L".jks", L".crt"};
	static const wchar_t *const heads[] = {L".env", L"id_rsa",
	        L"id_ed25519", L"id_ecdsa", L"id_dsa", L".netrc", L".npmrc",
	        L".pgpass"};
	static const wchar_t *const words[] = {L"password", L"passwd",
	        L"secret", L"token", L"credential", L"private", L"apikey",
	        L"api_key"};
	const wchar_t *leaf = path;

	if (!path || !path[0] || bytes > DRAFT_MAX_BYTES) {
		return false;
	}
	/* a network path: the text would leave the place it is kept in */
	if (starts_with(path, L"\\\\") && !starts_with(path, L"\\\\?\\")) {
		return false;
	}
	if (starts_with(path, L"\\\\?\\UNC\\")) {
		return false;
	}
	for (const wchar_t *p = path; *p; p++) {
		if (L'\\' == *p || L'/' == *p) {
			leaf = p + 1;
		}
	}
	if (!leaf[0]) {
		return false;
	}
	if (has_text(path, L"\\.ssh\\") || has_text(path, L"/.ssh/") ||
	        has_text(path, L"\\.gnupg\\") || has_text(path, L"\\.aws\\")) {
		return false;
	}
	for (size_t i = 0; i < WF_ARRAY_LEN(tails); i++) {
		if (ends_with(leaf, tails[i])) {
			return false;
		}
	}
	for (size_t i = 0; i < WF_ARRAY_LEN(heads); i++) {
		if (starts_with(leaf, heads[i])) {
			return false;
		}
	}
	for (size_t i = 0; i < WF_ARRAY_LEN(words); i++) {
		if (has_text(leaf, words[i])) {
			return false;
		}
	}
	return true;
}

static bool base_dir(wchar_t *out, size_t cap)
{
	wchar_t root[DIR_MAX];
	WfStateKind kind;
	DWORD n;

	if (g_dir[0]) {
		return 0 == wcscpy_s(out, cap, g_dir);
	}
	n = GetEnvironmentVariableW(L"LOCALAPPDATA", root, DIR_MAX);
	if (0 == n || n >= DIR_MAX) {
		return false;
	}
	/* the state folder in use (DR-DRAFT-017); if it cannot be told, no
	 * draft is written or looked for */
	if (WF_OK != wf_state_classify(root, &kind, NULL)) {
		return false;
	}
	return swprintf(out, cap, L"%ls\\%ls\\drafts", root,
	               wf_state_name(kind)) > 0;
}

static bool make_dir(const wchar_t *dir)
{
	wchar_t part[DIR_MAX];
	size_t n = wcslen(dir);

	if (n >= DIR_MAX) {
		return false;
	}
	for (size_t i = 3; i <= n; i++) { /* skip "C:\" */
		if (L'\\' == dir[i] || L'\0' == dir[i]) {
			memcpy(part, dir, i * sizeof *part);
			part[i] = L'\0';
			if (!CreateDirectoryW(part, NULL) &&
			        ERROR_ALREADY_EXISTS != GetLastError()) {
				return false;
			}
		}
	}
	SetFileAttributesW(dir,
	        FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED);
	return true;
}

/* Whether two names that differ only in case are the same file where `path`
 * lives. Only a folder that says it is case-insensitive is taken for one: where
 * that cannot be told (the folder is not there, a network or an old file
 * system), "Foo.txt" and "foo.txt" are two files and keep two drafts. */
static bool names_fold_case(const wchar_t *path)
{
	wchar_t parent[FILE_MAX_CHARS];
	const wchar_t *cut = wcsrchr(path, L'\\');
	FILE_CASE_SENSITIVE_INFO info;
	HANDLE h;
	bool known;

	if (!cut || (size_t)(cut - path) >= WF_ARRAY_LEN(parent)) {
		return false;
	}
	memcpy(parent, path, (size_t)(cut - path) * sizeof *parent);
	parent[cut - path] = L'\0';
	h = CreateFileW(parent, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	known = GetFileInformationByHandleEx(
	        h, FileCaseSensitiveInfo, &info, sizeof info);
	CloseHandle(h);
	return known && !(info.Flags & FILE_CS_FLAG_CASE_SENSITIVE_DIR);
}

/* `fold`: the name is hashed lower-cased (see names_fold_case). */
static bool draft_path(const wchar_t *path,
        bool fold,
        wchar_t *out,
        size_t cap,
        const wchar_t *suffix)
{
	wchar_t dir[DIR_MAX];
	/* FNV-1a-style hash; preserve this seed for existing draft filenames. */
	uint64_t h = 1469598103934665603ull;

	if (!base_dir(dir, DIR_MAX)) {
		return false;
	}
	for (const wchar_t *p = path; *p; p++) {
		wchar_t c = fold ? (wchar_t)towlower(*p) : *p;

		h = (h ^ (uint64_t)(c & 0xFF)) * 1099511628211ull;
		h = (h ^ (uint64_t)(c >> 8)) * 1099511628211ull;
	}
	return swprintf(out, cap, L"%ls\\%016llx%ls", dir,
	               (unsigned long long)h, suffix) > 0;
}

/*
 * A draft lives in one of two slot files (.a, .b), written and read by
 * wf_slotfile.c (the saved session uses the same scheme): a write goes to the
 * slot that holds the older draft, a slot cut short is ignored, nothing is
 * renamed over an existing name. A draft is removed only through the
 * owned-delete check that the object found is the very file that was looked
 * at (remove_slot below).
 */

#define DRAFT_MAGIC "piero-draft 2"

static const wchar_t *const SUFFIX[2] = {L".a", L".b"};

WfStatus wf_draft_write(
        const wchar_t *path, WfEncoding enc, const char *utf8, size_t n)
{
	wchar_t dir[DIR_MAX], file[2][FILE_MAX_CHARS];
	const wchar_t *files[2] = {file[0], file[1]};
	char where[2200], before[2300], after[64];
	bool fold;

	if (!wf_draft_allowed(path, n)) {
		return WF_E_INVAL;
	}
	fold = names_fold_case(path);
	if (!base_dir(dir, DIR_MAX) || !make_dir(dir) ||
	        !draft_path(path, fold, file[0], WF_ARRAY_LEN(file[0]),
	                SUFFIX[0]) ||
	        !draft_path(path, fold, file[1], WF_ARRAY_LEN(file[1]),
	                SUFFIX[1])) {
		return WF_E_INVAL;
	}
	if (!WideCharToMultiByte(CP_UTF8, 0, path, -1, where, (int)sizeof where,
	            NULL, NULL)) {
		return WF_E_INVAL;
	}
	snprintf(before, sizeof before, "path %s\n", where);
	snprintf(after, sizeof after, "enc %d %u\n", (int)enc.kind,
	        enc.codepage);
	return wf_slotfile_write(
	        files, DRAFT_MAGIC, before, after, utf8, n, DRAFT_MAX_BYTES);
}

/* The path the draft says it belongs to is the one asked for: a file name is
 * hashed to a place, and two paths may meet in one (a hash is not a name). */
static bool header_names(const char *header, const wchar_t *path, bool fold)
{
	wchar_t stored[FILE_MAX_CHARS];
	const char *at = !strncmp(header, "path ", 5) ? header
	        : strstr(header, "\npath ") ? strstr(header, "\npath ") + 1
	                                    : NULL;
	const char *end;
	char line[2200];
	size_t len;

	if (!at) {
		return false;
	}
	at += 5;
	end = strchr(at, '\n');
	len = end ? (size_t)(end - at) : strlen(at);
	if (len >= sizeof line) {
		return false;
	}
	memcpy(line, at, len);
	line[len] = '\0';
	if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line, -1,
	            stored, (int)WF_ARRAY_LEN(stored))) {
		return false;
	}
	return 0 == (fold ? _wcsicmp(stored, path) : wcscmp(stored, path));
}

WfStatus wf_draft_read(
        const wchar_t *path, WfEncoding *enc, char **utf8, size_t *n)
{
	wchar_t file[2][FILE_MAX_CHARS];
	WfSlotFile sl[2];
	int pick;
	const char *at;
	int kind = 0;
	unsigned cp = 0;

	bool fold;

	if (!utf8 || !n || !enc) {
		return WF_E_INVAL;
	}
	*utf8 = NULL;
	*n = 0;
	if (!path || !path[0]) {
		return WF_E_INVAL;
	}
	fold = names_fold_case(path);
	if (!draft_path(
	            path, fold, file[0], WF_ARRAY_LEN(file[0]), SUFFIX[0]) ||
	        !draft_path(path, fold, file[1], WF_ARRAY_LEN(file[1]),
	                SUFFIX[1])) {
		return WF_E_INVAL;
	}
	wf_slotfile_read(file[0], DRAFT_MAGIC, DRAFT_MAX_BYTES, &sl[0]);
	wf_slotfile_read(file[1], DRAFT_MAGIC, DRAFT_MAX_BYTES, &sl[1]);
	pick = wf_slotfile_pick(&sl[0], &sl[1]);
	/* a draft without the encoding line is not one this program wrote */
	at = pick < 0 ? NULL : strstr(sl[pick].header, "\nenc ");
	if (!at || 2 != sscanf_s(at + 5, "%d %u", &kind, &cp) ||
	        !header_names(sl[pick].header, path, fold)) {
		wf_slotfile_free(&sl[0]);
		wf_slotfile_free(&sl[1]);
		return WF_E_NOENT;
	}
	enc->kind = (WfEncKind)kind;
	enc->codepage = cp;
	*utf8 = sl[pick].body; /* the caller frees it */
	*n = sl[pick].body_n;
	sl[pick].body = NULL;
	wf_slotfile_free(&sl[0]);
	wf_slotfile_free(&sl[1]);
	return WF_OK;
}

/* The slot's own file, found again by what it is, then removed only if it is
 * still that object (owned_delete's check; the draft folder is ours). */
static void remove_slot(const wchar_t *file)
{
	FILE_ID_INFO id;
	WfOwnedReport rep;
	HANDLE h = CreateFileW(file, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	bool have;

	if (INVALID_HANDLE_VALUE == h) {
		return;
	}
	have = GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof id);
	CloseHandle(h);
	if (have) {
		(void)wf_owned_delete_save_temp(file, &id, &rep);
	}
}

void wf_draft_remove(const wchar_t *path)
{
	wchar_t file[FILE_MAX_CHARS];
	bool fold;

	if (!path || !path[0]) {
		return;
	}
	fold = names_fold_case(path);
	for (int i = 0; i < 2; i++) {
		if (draft_path(
		            path, fold, file, WF_ARRAY_LEN(file), SUFFIX[i])) {
			remove_slot(file);
		}
	}
}
