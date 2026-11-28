#include <windows.h>

#include "p0_area.h"
#include "p0_common.h"

#include <bcrypt.h>
#include <io.h>
#include <processsnapshot.h>
#include <share.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define P0A_PATH 32768 /* UTF-16 units, extended paths including NUL */
#define MARKER L".p0-owner"
#define MANIFEST L".p0-manifest"
#define NONCE_BYTES 32

typedef struct
{
	wchar_t *rel; /* owned */
	int is_dir;
} Entry;

struct P0Area
{
	wchar_t root[P0A_PATH];
	wchar_t final_root[P0A_PATH]; /* GetFinalPathNameByHandle of root at
	                                 creation */
	char nonce[NONCE_BYTES * 2 + 1];
	BYTE id[24]; /* volume serial (8) + FileId (16) of the root */
	Entry *e;
	size_t n, cap;
	char refusal[160];
};

static void refuse(P0Area *a, const char *why)
{
	snprintf(a->refusal, sizeof a->refusal, "%s", why);
}

const char *p0_area_last_refusal(const P0Area *a)
{
	return a->refusal;
}

const wchar_t *p0_area_root(const P0Area *a)
{
	return a->root;
}

DWORD p0_area_open_handles(const P0Area *a, size_t *count)
{
	if (!a || !count) {
		return ERROR_INVALID_PARAMETER;
	}
	HPSS snapshot = NULL;
	HPSSWALK marker = NULL;
	PSS_HANDLE_ENTRY entry;
	const wchar_t *label = wcsrchr(a->root, L'\\');
	label = label ? label + 1 : a->root;
	size_t length = wcslen(label);
	*count = 0;
	DWORD error = PssCaptureSnapshot(
	        GetCurrentProcess(), PSS_CAPTURE_HANDLES, 0, &snapshot);
	if (error != ERROR_SUCCESS) {
		return error;
	}
	error = PssWalkMarkerCreate(NULL, &marker);
	if (error == ERROR_SUCCESS) {
		while ((error = PssWalkSnapshot(snapshot, PSS_WALK_HANDLES,
		                marker, &entry, sizeof entry)) ==
		        ERROR_SUCCESS) {
			/* File names can be absent from PSS entries. Query only disk
			 * handles in our process, never pipes or console handles. */
			if (GetFileType(entry.Handle) != FILE_TYPE_DISK) {
				continue;
			}
			wchar_t name[P0A_PATH];
			DWORD units = GetFinalPathNameByHandleW(
			        entry.Handle, name, P0A_PATH, VOLUME_NAME_NT);
			if (!units || units >= P0A_PATH) {
				error = ERROR_PARTIAL_COPY;
				break;
			}
			for (size_t i = 1; i + length <= units; i++) {
				if (name[i - 1] == L'\\' &&
				        !memcmp(name + i, label,
				                length * sizeof(wchar_t)) &&
				        (i + length == units ||
				                name[i + length] == L'\\')) {
					(*count)++;
					break;
				}
			}
		}
		PssWalkMarkerFree(marker);
		if (error == ERROR_NO_MORE_ITEMS) {
			error = ERROR_SUCCESS;
		}
	}
	PssFreeSnapshot(GetCurrentProcess(), snapshot);
	return error;
}

static int dir_identity(
        const wchar_t *path, BYTE id[24], wchar_t *final_path, size_t cap)
{
	FILE_ID_INFO fid;
	HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	int ok = 0;

	if (INVALID_HANDLE_VALUE == h) {
		return 0;
	}
	if (GetFileInformationByHandleEx(h, FileIdInfo, &fid, sizeof fid)) {
		memcpy(id, &fid.VolumeSerialNumber, 8);
		memcpy(id + 8, fid.FileId.Identifier, 16);
		ok = 1;
	}
	if (ok && final_path) {
		ok = GetFinalPathNameByHandleW(
		             h, final_path, (DWORD)cap, VOLUME_NAME_DOS) > 0;
	}
	CloseHandle(h);
	return ok;
}

static int starts_with_ci(const wchar_t *s, const wchar_t *prefix)
{
	return 0 == _wcsnicmp(s, prefix, wcslen(prefix));
}

int p0_area_path(const P0Area *a, const wchar_t *rel, wchar_t *out, size_t cap)
{
	size_t i, comp_start = 0, n = wcslen(rel);

	if (0 == n || L'\\' == rel[0] || L'/' == rel[0] ||
	        (n >= 2 && L':' == rel[1])) {
		return 0;
	}
	for (i = 0; i <= n; i++) {
		if (i == n || L'\\' == rel[i]) {
			size_t len = i - comp_start;

			if (0 == len) {
				return 0; /* empty component */
			}
			if ((1 == len && L'.' == rel[comp_start]) ||
			        (2 == len && L'.' == rel[comp_start] &&
			                L'.' == rel[comp_start + 1])) {
				return 0;
			}
			comp_start = i + 1;
		} else if (NULL != wcschr(L"*?<>|\":/", rel[i])) {
			return 0;
		}
	}
	return (size_t)swprintf(out, cap, L"%ls\\%ls", a->root, rel) < cap;
}

/* The parent chain of `path` must resolve inside the area's own root. */
static int parent_inside_root(const P0Area *a, const wchar_t *path)
{
	wchar_t parent[P0A_PATH], fin[P0A_PATH];
	wchar_t *slash;
	HANDLE h;
	int ok;

	wcscpy_s(parent, P0A_PATH, path);
	slash = wcsrchr(parent, L'\\');
	if (!slash) {
		return 0;
	}
	*slash = L'\0';
	h = CreateFileW(parent, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return 0;
	}
	ok = GetFinalPathNameByHandleW(h, fin, P0A_PATH, VOLUME_NAME_DOS) > 0;
	CloseHandle(h);
	if (!ok) {
		return 0;
	}
	/* the parent must be the root itself or below it, compared on final
	 * paths */
	return starts_with_ci(fin, a->final_root) &&
	        (L'\0' == fin[wcslen(a->final_root)] ||
	                L'\\' == fin[wcslen(a->final_root)]);
}

static void hex_of(const BYTE *b, size_t n, char *out)
{
	size_t i;

	for (i = 0; i < n; i++) {
		snprintf(out + i * 2, 3, "%02x", b[i]);
	}
}

static int write_marker(P0Area *a, const char *run_id)
{
	wchar_t p[P0A_PATH];
	char idhex[49];
	FILE *f;

	swprintf(p, P0A_PATH, L"%ls\\%ls", a->root, MARKER);
	f = p0_fopen_w(p);
	if (!f) {
		return 0;
	}
	hex_of(a->id, 24, idhex);
	fprintf(f, "wfile-p0-owner v1\nnonce=%s\nrun=%s\nroot_id=%s\n",
	        a->nonce, run_id, idhex);
	fflush(f);
	_commit(_fileno(f));
	fclose(f);
	return 1;
}

/* Reads the nonce back from disk; returns 1 if it equals ours. */
static int marker_matches(const P0Area *a)
{
	wchar_t p[P0A_PATH];
	char line[256];
	FILE *f;
	int ok = 0;

	swprintf(p, P0A_PATH, L"%ls\\%ls", a->root, MARKER);
	f = _wfsopen(p, L"rb", _SH_DENYNO);
	if (!f) {
		return 0;
	}
	while (fgets(line, sizeof line, f)) {
		if (!strncmp(line, "nonce=", 6)) {
			line[strcspn(line, "\r\n")] = '\0';
			ok = 0 == strcmp(line + 6, a->nonce);
		}
	}
	fclose(f);
	return ok;
}

static int prove_ownership(P0Area *a)
{
	BYTE id[24];

	if (!dir_identity(a->root, id, NULL, 0)) {
		refuse(a, "root directory not found");
		return 0;
	}
	if (0 != memcmp(id, a->id, sizeof id)) {
		refuse(a,
		        "root directory is not the one this run created "
		        "(FileId differs)");
		return 0;
	}
	if (!marker_matches(a)) {
		refuse(a, "owner marker missing or nonce differs");
		return 0;
	}
	return 1;
}

static int manifest_append(P0Area *a, const wchar_t *rel, int is_dir)
{
	wchar_t p[P0A_PATH];
	FILE *f;
	char *u8;
	int bytes =
	        WideCharToMultiByte(CP_UTF8, 0, rel, -1, NULL, 0, NULL, NULL);
	int ok;
	if (bytes <= 0) {
		return 0;
	}
	u8 = malloc((size_t)bytes);
	if (!u8) {
		return 0;
	}
	if (!WideCharToMultiByte(CP_UTF8, 0, rel, -1, u8, bytes, NULL, NULL)) {
		free(u8);
		return 0;
	}

	swprintf(p, P0A_PATH, L"%ls\\%ls", a->root, MANIFEST);
	f = p0_fappend_w(p);
	if (!f) {
		free(u8);
		return 0;
	}
	ok = fprintf(f, "%c %s\n", is_dir ? 'D' : 'F', u8) > 0 &&
	        0 == fflush(f) && 0 == _commit(_fileno(f));
	fclose(f);
	free(u8);
	return ok;
}

static int push(P0Area *a, const wchar_t *rel, int is_dir)
{
	if (a->n == a->cap) {
		size_t cap = a->cap ? a->cap * 2 : 64;
		Entry *ne = realloc(a->e, cap * sizeof *ne);

		if (!ne) {
			return 0;
		}
		a->e = ne;
		a->cap = cap;
	}
	a->e[a->n].rel = _wcsdup(rel);
	if (!a->e[a->n].rel) {
		return 0;
	}
	a->e[a->n].is_dir = is_dir;
	a->n++;
	return 1;
}

P0Area *p0_area_create(const wchar_t *base, const char *run_id)
{
	P0Area *a = calloc(1, sizeof *a);
	BYTE nonce[NONCE_BYTES];
	wchar_t name[96];
	PSECURITY_DESCRIPTOR sd = NULL;
	SECURITY_ATTRIBUTES security = {sizeof security, NULL, FALSE};

	if (!a) {
		return NULL;
	}
	if (BCryptGenRandom(NULL, nonce, sizeof nonce,
	            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
		goto fail;
	}
	hex_of(nonce, sizeof nonce, a->nonce);
	if (0 == MultiByteToWideChar(CP_UTF8, 0, run_id, -1, name, 80)) {
		goto fail;
	}
	if ((size_t)swprintf(a->root, P0A_PATH, L"%ls\\wfile-p0-%ls", base,
	            name) >= P0A_PATH) {
		goto fail;
	}
	/* CreateDirectoryW fails on an existing directory: an area is always
	 * new */
	if (!p0_private_security(1, &sd)) {
		goto fail;
	}
	security.lpSecurityDescriptor = sd;
	if (!CreateDirectoryW(a->root, &security)) {
		goto fail;
	}
	if (!dir_identity(a->root, a->id, a->final_root, P0A_PATH) ||
	        !write_marker(a, run_id)) {
		goto fail;
	}
	LocalFree(sd);
	return a;
fail:
	LocalFree(sd);
	free(a);
	return NULL;
}

P0Area *p0_area_open(const wchar_t *root)
{
	P0Area *a = calloc(1, sizeof *a);
	wchar_t p[P0A_PATH];
	char line[P0A_PATH * 3 + 4], idhex[64] = "";
	FILE *f = NULL;
	BYTE live[24];

	if (!a) {
		return NULL;
	}
	wcscpy_s(a->root, P0A_PATH, root);
	swprintf(p, P0A_PATH, L"%ls\\%ls", root, MARKER);
	f = _wfsopen(p, L"rb", _SH_DENYNO);
	if (!f) {
		goto fail;
	}
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (!strncmp(line, "nonce=", 6)) {
			snprintf(a->nonce, sizeof a->nonce, "%s", line + 6);
		} else if (!strncmp(line, "root_id=", 8)) {
			snprintf(idhex, sizeof idhex, "%s", line + 8);
		}
	}
	if (ferror(f) || strlen(a->nonce) != NONCE_BYTES * 2 ||
	        48 != strlen(idhex)) {
		goto fail;
	}
	{
		size_t i;

		for (i = 0; i < 24; i++) {
			unsigned v = 0;

			if (1 != sscanf_s(idhex + i * 2, "%2x", &v)) {
				goto fail;
			}
			a->id[i] = (BYTE)v;
		}
	}
	/* the directory on disk must be the one the marker was written for */
	if (!dir_identity(a->root, live, a->final_root, P0A_PATH) ||
	        0 != memcmp(live, a->id, sizeof live)) {
		goto fail;
	}
	swprintf(p, P0A_PATH, L"%ls\\%ls", root, MANIFEST);
	/* The marker remains owned by fail until validation has succeeded. */
	fclose(f);
	f = _wfsopen(p, L"rb", _SH_DENYNO);
	if (f) {
		while (fgets(line, sizeof line, f)) {
			wchar_t rel[P0A_PATH];

			line[strcspn(line, "\r\n")] = '\0';
			if (('F' != line[0] && 'D' != line[0]) ||
			        ' ' != line[1]) {
				continue;
			}
			if (MultiByteToWideChar(CP_UTF8, 0, line + 2, -1, rel,
			            P0A_PATH) == 0) {
				continue;
			}
			push(a, rel, 'D' == line[0]);
		}
		fclose(f);
	}
	return a;
fail:
	if (f) {
		fclose(f);
	}
	free(a);
	return NULL;
}

/* "C:\x" -> "\\?\C:\x": paths beyond MAX_PATH work with no manifest or registry
 * setting. */
int p0_area_xpath(const P0Area *a, const wchar_t *rel, wchar_t *out, size_t cap)
{
	wchar_t p[P0A_PATH];

	return p0_area_path(a, rel, p, P0A_PATH) &&
	        (size_t)swprintf(out, cap, L"\\\\?\\%ls", p) < cap;
}

int p0_area_track(P0Area *a, const wchar_t *rel, int is_dir)
{
	wchar_t x[P0A_PATH];

	if (!p0_area_xpath(a, rel, x, P0A_PATH) ||
	        INVALID_FILE_ATTRIBUTES == GetFileAttributesW(x)) {
		return 0; /* only objects that exist, at a legal name, can be
		             recorded */
	}
	return manifest_append(a, rel, is_dir) && push(a, rel, is_dir);
}

int p0_area_mkdir(P0Area *a, const wchar_t *rel)
{
	wchar_t x[P0A_PATH];

	if (!p0_area_xpath(a, rel, x, P0A_PATH) || !CreateDirectoryW(x, NULL)) {
		return 0;
	}
	return p0_area_track(a, rel, 1);
}

static int recorded(const P0Area *a, const wchar_t *rel, int *is_dir)
{
	size_t i;

	for (i = 0; i < a->n; i++) {
		if (0 == wcscmp(a->e[i].rel, rel)) { /* exact, case-sensitive */
			*is_dir = a->e[i].is_dir;
			return 1;
		}
	}
	return 0;
}

int p0_area_remove(P0Area *a, const wchar_t *rel)
{
	wchar_t p[P0A_PATH];
	int is_dir;
	DWORD attr;

	a->refusal[0] = '\0';
	if (!p0_area_xpath(a, rel, p, P0A_PATH)) {
		refuse(a, "illegal relative name");
		return 0;
	}
	if (!recorded(a, rel, &is_dir)) {
		refuse(a, "object was not recorded by this run");
		return 0;
	}
	if (!prove_ownership(a)) {
		return 0;
	}
	/* Nothing there: nothing to delete, so no proof about its parents is
	 * needed. (A path that reaches a real object THROUGH a link is not
	 * 'nothing': the attribute query follows the link, finds it, and the
	 * parent proof below refuses.) */
	attr = GetFileAttributesW(p);
	if (INVALID_FILE_ATTRIBUTES == attr) {
		return 1; /* already gone */
	}
	if (!parent_inside_root(a, p)) {
		refuse(a,
		        "parent chain does not resolve inside the area (link "
		        "on the way?)");
		return 0;
	}
	if (0 != (attr & FILE_ATTRIBUTE_DIRECTORY)) {
		if (!is_dir) {
			refuse(a, "recorded as a file but is a directory now");
			return 0;
		}
		return 0 !=
		        RemoveDirectoryW(
		                p); /* only succeeds when empty; also removes a link
		 itself */
	}
	if (is_dir) {
		refuse(a, "recorded as a directory but is a file now");
		return 0;
	}
	if (attr & FILE_ATTRIBUTE_READONLY) {
		SetFileAttributesW(p,
		        attr & ~(DWORD)FILE_ATTRIBUTE_READONLY); /* a recorded
			                                            object of
			                                            ours */
	}
	return 0 != DeleteFileW(p);
}

/*
 * Names in the root other than our two bookkeeping files: things this run did
 * not record and therefore does not own. Read-only listing; returns the count
 * and, if `report` is given, appends the names.
 */
static int list_strays(const P0Area *a, wchar_t *report, size_t cap)
{
	WIN32_FIND_DATAW fd;
	wchar_t pat[P0A_PATH];
	HANDLE h;
	int n = 0;

	swprintf(pat, P0A_PATH, L"%ls\\*", a->root);
	h = FindFirstFileW(pat, &fd);
	if (INVALID_HANDLE_VALUE == h) {
		return 0;
	}
	do {
		if (!wcscmp(fd.cFileName, L".") ||
		        !wcscmp(fd.cFileName, L"..") ||
		        !wcscmp(fd.cFileName, MARKER) ||
		        !wcscmp(fd.cFileName, MANIFEST)) {
			continue;
		}
		n++;
		if (report && wcslen(report) + wcslen(fd.cFileName) + 3 < cap) {
			wcscat_s(report, cap, fd.cFileName);
			wcscat_s(report, cap, L"; ");
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	return n;
}

/* Report only owned relative names, never an absolute user path. Capture the
 * error before ownership checks or subsequent cleanup can replace it. */
static void cleanup_failure(wchar_t *report,
        size_t cap,
        const wchar_t *operation,
        const wchar_t *relative,
        DWORD error)
{
	wchar_t detail[256];
	if (!report || !cap) {
		return;
	}
	int n = swprintf(detail, sizeof detail / sizeof detail[0],
	        L"%ls(%ls): win32=%lu; ", operation, relative, error);
	if (n > 0 && wcslen(report) + (size_t)n < cap) {
		wcscat_s(report, cap, detail);
	}
}

int p0_area_destroy(P0Area *a, wchar_t *report, size_t cap)
{
	size_t i;
	int left = 0;
	wchar_t p[P0A_PATH];
	HANDLE root_guard = INVALID_HANDLE_VALUE;

	if (report && cap) {
		report[0] = L'\0';
	}
	if (!prove_ownership(a)) {
		if (report) {
			swprintf(report, cap, L"REFUSED: %hs", a->refusal);
		}
		left = (int)a->n + 1;
		goto done;
	}
	for (i = a->n; i > 0; i--) {
		if (!p0_area_remove(a, a->e[i - 1].rel)) {
			DWORD error = GetLastError();
			left++;
			cleanup_failure(report, cap, L"remove recorded",
			        a->e[i - 1].rel, error);
		}
	}
	/*
	 * Bookkeeping files go only when nothing else is left. While anything
	 * remains - a recorded object that would not go, or content we did not
	 * create - the marker and manifest STAY, so the area can still be
	 * proven ours and reaped later (p0_area_reap). Deleting them first
	 * would turn a leftover into an orphan that nobody can safely clean.
	 */
	{
		wchar_t strays[1024] = L"";
		int nstray = list_strays(a, strays, 1024);

		if (0 == left && 0 == nstray) {
			/* Discover a root reader that denies delete sharing BEFORE
			 * removing the only records that make this area reapable.
			 * Holding DELETE also prevents a new incompatible reader
			 * from opening between this check and RemoveDirectoryW. */
			root_guard = CreateFileW(a->root,
			        DELETE | FILE_READ_ATTRIBUTES,
			        FILE_SHARE_READ | FILE_SHARE_WRITE |
			                FILE_SHARE_DELETE,
			        NULL, OPEN_EXISTING,
			        FILE_FLAG_BACKUP_SEMANTICS |
			                FILE_FLAG_OPEN_REPARSE_POINT,
			        NULL);
			if (root_guard == INVALID_HANDLE_VALUE) {
				DWORD error = GetLastError();
				cleanup_failure(report, cap, L"CreateFileW",
				        L"root delete guard", error);
				left++;
				goto done;
			}
			FILE_ID_INFO id;
			if (!GetFileInformationByHandleEx(
			            root_guard, FileIdInfo, &id, sizeof id)) {
				DWORD error = GetLastError();
				cleanup_failure(report, cap,
				        L"GetFileInformationByHandleEx",
				        L"root delete guard", error);
				left++;
				goto done;
			}
			if (memcmp(&id.VolumeSerialNumber, a->id, 8) ||
			        memcmp(id.FileId.Identifier, a->id + 8, 16)) {
				cleanup_failure(report, cap, L"identity",
				        L"root delete guard",
				        ERROR_INVALID_DATA);
				left++;
				goto done;
			}
			swprintf(p, P0A_PATH, L"%ls\\%ls", a->root, MANIFEST);
			if (!DeleteFileW(p)) {
				DWORD error = GetLastError();
				/* No manifest exists until an area's first record. */
				if (error != ERROR_FILE_NOT_FOUND ||
				        a->n != 0) {
					cleanup_failure(report, cap,
					        L"DeleteFileW", MANIFEST,
					        error);
					left++;
					goto done;
				}
			}
			swprintf(p, P0A_PATH, L"%ls\\%ls", a->root, MARKER);
			if (!DeleteFileW(p)) {
				cleanup_failure(report, cap, L"DeleteFileW",
				        MARKER, GetLastError());
				left++;
				goto done;
			}
			if (!RemoveDirectoryW(a->root)) {
				cleanup_failure(report, cap,
				        L"RemoveDirectoryW", L"root",
				        GetLastError());
				left++;
			}
		} else if (nstray) {
			left++;
			if (report &&
			        wcslen(report) + wcslen(strays) + 64 < cap) {
				wcscat_s(report, cap,
				        L"[unexpected content in root, marker "
				        L"kept: ");
				wcscat_s(report, cap, strays);
				wcscat_s(report, cap, L"]");
			}
		}
	}
done:
	if (root_guard != INVALID_HANDLE_VALUE) {
		CloseHandle(root_guard);
	}
	if (left) {
		wchar_t enabled[2];
		if (GetEnvironmentVariableW(
		            L"WF_P0_HANDLE_DIAGNOSTICS", enabled, 2) == 1 &&
		        enabled[0] == L'1') {
			size_t count;
			DWORD error = p0_area_open_handles(a, &count);
			fprintf(stderr,
			        "P0 owned handle snapshot: error=%lu "
			        "count=%zu\n",
			        error, count);
		}
	}
	p0_area_forget(a);
	return left;
}

void p0_area_forget(P0Area *a)
{
	size_t i;

	if (!a) {
		return;
	}
	for (i = 0; i < a->n; i++) {
		free(a->e[i].rel);
	}
	free(a->e);
	free(a);
}
