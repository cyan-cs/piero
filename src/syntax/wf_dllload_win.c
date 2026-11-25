#include "wf_dllload_win.h"

#include <bcrypt.h>
#include <stdlib.h>
#include <string.h>

#include "wf_syntax_abi.h"

#define PATH_MAX_WIDE 1024u

/* "\\?\C:\x" -> "C:\x"; a UNC or any other device path is refused. */
static uint32_t strip_dos_prefix(wchar_t *p)
{
	size_t n = wcslen(p);

	if (n >= 4 && !wcsncmp(p, L"\\\\?\\", 4)) {
		if (n >= 8 && !_wcsnicmp(p + 4, L"UNC\\", 4)) {
			return WF_SYN_E_PATH;
		}
		memmove(p, p + 4, (n - 4 + 1) * sizeof *p);
	}
	return WF_SYN_OK;
}

static uint32_t fixed_or_removable_drive(const wchar_t *p)
{
	wchar_t root[4];
	UINT t;

	if (!((p[0] >= L'A' && p[0] <= L'Z') ||
	            (p[0] >= L'a' && p[0] <= L'z')) ||
	        L':' != p[1] || L'\\' != p[2]) {
		return WF_SYN_E_PATH;
	}
	root[0] = p[0];
	root[1] = L':';
	root[2] = L'\\';
	root[3] = L'\0';
	t = GetDriveTypeW(root);
	return DRIVE_FIXED == t || DRIVE_REMOVABLE == t ? WF_SYN_OK
	                                                : WF_SYN_E_PATH;
}

/* The real directory `dir` is, with every link resolved, and the checks of
 * step 1. out has cap wide characters. */
static uint32_t settle_dir(const wchar_t *dir, wchar_t *out, size_t cap)
{
	HANDLE h = CreateFileW(dir, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	DWORD n;
	uint32_t st;

	if (INVALID_HANDLE_VALUE == h) {
		return WF_SYN_E_PATH;
	}
	n = GetFinalPathNameByHandleW(h, out, (DWORD)cap, VOLUME_NAME_DOS);
	CloseHandle(h);
	if (!n || n >= cap) {
		return WF_SYN_E_PATH;
	}
	st = strip_dos_prefix(out);
	if (WF_SYN_OK != st) {
		return st;
	}
	n = (DWORD)wcslen(out);
	while (n > 3 && L'\\' == out[n - 1]) {
		out[--n] = L'\0';
	}
	return fixed_or_removable_drive(out);
}

uint32_t wf_dll_exe_dir(wchar_t *out, size_t cap)
{
	wchar_t exe[PATH_MAX_WIDE];
	DWORD n = GetModuleFileNameW(NULL, exe, PATH_MAX_WIDE);
	wchar_t *slash;

	if (!n || n >= PATH_MAX_WIDE || !out || cap < 8) {
		return WF_SYN_E_PATH;
	}
	slash = wcsrchr(exe, L'\\');
	if (!slash) {
		return WF_SYN_E_PATH;
	}
	*slash = L'\0';
	return settle_dir(exe, out, cap);
}

uint32_t wf_dll_module_dir(const void *addr, wchar_t *out, size_t cap)
{
	HMODULE m = NULL;
	wchar_t file[PATH_MAX_WIDE];
	DWORD n;
	wchar_t *slash;

	if (!out || cap < 8 ||
	        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                (LPCWSTR)addr, &m)) {
		return WF_SYN_E_PATH;
	}
	n = GetModuleFileNameW(m, file, PATH_MAX_WIDE);
	if (!n || n >= PATH_MAX_WIDE) {
		return WF_SYN_E_PATH;
	}
	slash = wcsrchr(file, L'\\');
	if (!slash) {
		return WF_SYN_E_PATH;
	}
	*slash = L'\0';
	return settle_dir(file, out, cap);
}

static int plain_part(const wchar_t *s)
{
	size_t n = wcslen(s);

	if (!n || n > 64 || L'.' == s[0]) {
		return 0;
	}
	for (size_t i = 0; i < n; i++) {
		wchar_t c = s[i];

		if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
		            (c >= L'0' && c <= L'9') || L'.' == c ||
		            L'_' == c || L'-' == c)) {
			return 0;
		}
		if (L'.' == c && i + 1 < n && L'.' == s[i + 1]) {
			return 0;
		}
	}
	return 1;
}

uint32_t wf_dll_resolve(const wchar_t *root,
        const wchar_t *const *parts,
        size_t nparts,
        wchar_t *out,
        size_t cap)
{
	size_t len;
	int exists = 1;

	if (!root || !out || !parts || !nparts) {
		return WF_SYN_E_BADARG;
	}
	len = wcslen(root);
	if (len < 3 || len >= cap ||
	        WF_SYN_OK != fixed_or_removable_drive(root)) {
		return WF_SYN_E_PATH;
	}
	memcpy(out, root, (len + 1) * sizeof *out);
	for (size_t i = 0; i < nparts; i++) {
		size_t pn;
		DWORD a;

		if (!plain_part(parts[i])) {
			return WF_SYN_E_PATH;
		}
		pn = wcslen(parts[i]);
		if (len + 1 + pn + 1 > cap) {
			return WF_SYN_E_PATH;
		}
		if (len > 3 || L'\\' != out[len - 1]) {
			out[len++] = L'\\';
		}
		memcpy(out + len, parts[i], pn * sizeof *out);
		len += pn;
		out[len] = L'\0';
		if (exists) {
			a = GetFileAttributesW(out);
			if (INVALID_FILE_ATTRIBUTES == a) {
				exists =
				        0; /* later parts cannot exist either */
			} else if (a & FILE_ATTRIBUTE_REPARSE_POINT) {
				return WF_SYN_E_PATH;
			} else if (i + 1 < nparts &&
			        !(a & FILE_ATTRIBUTE_DIRECTORY)) {
				return WF_SYN_E_PATH;
			}
		}
	}
	return WF_SYN_OK;
}

int wf_dll_present(const wchar_t *path)
{
	DWORD a = GetFileAttributesW(path);

	return INVALID_FILE_ATTRIBUTES != a &&
	        !(a &
	                (FILE_ATTRIBUTE_DIRECTORY |
	                        FILE_ATTRIBUTE_REPARSE_POINT));
}

typedef struct
{
	BCRYPT_ALG_HANDLE alg;
	BCRYPT_HASH_HANDLE h;
} Sha;

static int sha_begin(Sha *s)
{
	memset(s, 0, sizeof *s);
	if (BCryptOpenAlgorithmProvider(
	            &s->alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0) {
		return 0;
	}
	if (BCryptCreateHash(s->alg, &s->h, NULL, 0, NULL, 0, 0) < 0) {
		BCryptCloseAlgorithmProvider(s->alg, 0);
		return 0;
	}
	return 1;
}

static void sha_end(Sha *s)
{
	if (s->h) {
		BCryptDestroyHash(s->h);
	}
	if (s->alg) {
		BCryptCloseAlgorithmProvider(s->alg, 0);
	}
}

int wf_dll_sha256(const void *data, size_t n, uint8_t out[32])
{
	Sha s;
	int ok;

	if (!sha_begin(&s)) {
		return 0;
	}
	ok = BCryptHashData(s.h, (PUCHAR)(uintptr_t)data, (ULONG)n, 0) >= 0 &&
	        BCryptFinishHash(s.h, out, 32, 0) >= 0;
	sha_end(&s);
	return ok;
}

int wf_dll_hex32(const char *hex, uint8_t out[32])
{
	if (!hex || 64 != strlen(hex)) {
		return 0;
	}
	for (size_t i = 0; i < 32; i++) {
		unsigned v = 0;

		for (size_t k = 0; k < 2; k++) {
			char c = hex[i * 2 + k];

			v <<= 4;
			if (c >= '0' && c <= '9') {
				v |= (unsigned)(c - '0');
			} else if (c >= 'a' && c <= 'f') {
				v |= (unsigned)(c - 'a' + 10);
			} else if (c >= 'A' && c <= 'F') {
				v |= (unsigned)(c - 'A' + 10);
			} else {
				return 0;
			}
		}
		out[i] = (uint8_t)v;
	}
	return 1;
}

uint32_t wf_dll_hold(const wchar_t *path,
        uint32_t max_bytes,
        const uint8_t *expect,
        WfDllHold *hold)
{
	HANDLE h;
	BY_HANDLE_FILE_INFORMATION info;
	LARGE_INTEGER size;
	Sha s;
	uint8_t *buf;
	uint64_t left;
	uint32_t st = WF_SYN_E_TRUST;

	if (!path || !hold) {
		return WF_SYN_E_BADARG;
	}
	hold->h = INVALID_HANDLE_VALUE;
	hold->size = 0;
	memset(hold->sha256, 0, sizeof hold->sha256);
	/* share READ only: nobody can write, delete or rename it while we hold it;
	 * the leaf is opened as itself, so a link is seen as a link */
	h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING,
	        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return ERROR_FILE_NOT_FOUND == GetLastError() ||
		                ERROR_PATH_NOT_FOUND == GetLastError()
		        ? WF_SYN_E_NOGRAMMAR
		        : WF_SYN_E_PATH;
	}
	if (!GetFileInformationByHandle(h, &info) ||
	        (info.dwFileAttributes &
	                (FILE_ATTRIBUTE_DIRECTORY |
	                        FILE_ATTRIBUTE_REPARSE_POINT)) ||
	        !GetFileSizeEx(h, &size) || size.QuadPart <= 0 ||
	        (uint64_t)size.QuadPart > max_bytes) {
		CloseHandle(h);
		return WF_SYN_E_PATH;
	}
	buf = malloc(65536);
	if (!buf || !sha_begin(&s)) {
		free(buf);
		CloseHandle(h);
		return WF_SYN_E_NOMEM;
	}
	left = (uint64_t)size.QuadPart;
	while (left) {
		DWORD want = left > 65536 ? 65536u : (DWORD)left, got = 0;

		if (!ReadFile(h, buf, want, &got, NULL) || !got ||
		        BCryptHashData(s.h, buf, got, 0) < 0) {
			st = WF_SYN_E_PATH;
			goto out;
		}
		left -= got;
	}
	if (BCryptFinishHash(s.h, hold->sha256, 32, 0) < 0) {
		st = WF_SYN_E_INTERNAL;
		goto out;
	}
	if (expect && memcmp(expect, hold->sha256, 32)) {
		st = WF_SYN_E_TRUST;
		goto out;
	}
	hold->h = h;
	hold->size = (uint32_t)size.QuadPart;
	st = WF_SYN_OK;
out:
	sha_end(&s);
	free(buf);
	if (WF_SYN_OK != st) {
		CloseHandle(h);
	}
	return st;
}

uint32_t wf_dll_hold_read(const WfDllHold *hold, char **out, size_t *n)
{
	char *buf;
	DWORD got = 0;
	LARGE_INTEGER zero;

	if (!hold || INVALID_HANDLE_VALUE == hold->h || !out || !n) {
		return WF_SYN_E_BADARG;
	}
	zero.QuadPart = 0;
	buf = malloc((size_t)hold->size + 1);
	if (!buf) {
		return WF_SYN_E_NOMEM;
	}
	if (!SetFilePointerEx(hold->h, zero, NULL, FILE_BEGIN) ||
	        !ReadFile(hold->h, buf, hold->size, &got, NULL) ||
	        got != hold->size) {
		free(buf);
		return WF_SYN_E_PATH;
	}
	buf[got] = '\0';
	*out = buf;
	*n = got;
	return WF_SYN_OK;
}

void wf_dll_release(WfDllHold *hold)
{
	if (hold && INVALID_HANDLE_VALUE != hold->h) {
		CloseHandle(hold->h);
		hold->h = INVALID_HANDLE_VALUE;
	}
}

static int same_path(const wchar_t *a, const wchar_t *b)
{
	wchar_t x[PATH_MAX_WIDE], y[PATH_MAX_WIDE];

	if (wcslen(a) >= PATH_MAX_WIDE || wcslen(b) >= PATH_MAX_WIDE) {
		return 0;
	}
	wcscpy_s(x, PATH_MAX_WIDE, a);
	wcscpy_s(y, PATH_MAX_WIDE, b);
	if (WF_SYN_OK != strip_dos_prefix(x) ||
	        WF_SYN_OK != strip_dos_prefix(y)) {
		return 0;
	}
	return 0 == _wcsicmp(x, y);
}

uint32_t wf_dll_load(const wchar_t *path, HMODULE *out)
{
	HMODULE m;
	wchar_t at[PATH_MAX_WIDE];
	DWORD n, mode = 0;

	if (!path || !out || L'\\' == path[0] || L'\0' == path[0] ||
	        L':' != path[1]) {
		return WF_SYN_E_BADARG; /* a relative path is never given to the loader */
	}
	*out = NULL;
	/* A file that is not a loadable image must be an error returned here, not the
	 * "Bad Image" box of the system: that box waits for a click, and the program
	 * (or a test with no desktop) waits with it. */
	(void)SetThreadErrorMode(SEM_FAILCRITICALERRORS, &mode);
	m = LoadLibraryExW(path, NULL,
	        LOAD_LIBRARY_SEARCH_SYSTEM32 |
	                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
	(void)SetThreadErrorMode(mode, NULL);
	if (!m) {
		return WF_SYN_E_LOAD;
	}
	n = GetModuleFileNameW(m, at, PATH_MAX_WIDE);
	if (!n || n >= PATH_MAX_WIDE || !same_path(at, path)) {
		FreeLibrary(m);
		return WF_SYN_E_LOAD;
	}
	*out = m;
	return WF_SYN_OK;
}
