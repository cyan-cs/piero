#include <windows.h>

#include "p0_common.h"
#include "../../src/base/wf_macro.h"

#include <bcrypt.h>
#include <share.h>
#include <string.h>

void p0_utc_iso(char out[24])
{
	SYSTEMTIME t;

	GetSystemTime(&t);
	snprintf(out, 24, "%04u-%02u-%02uT%02u:%02u:%02uZ", t.wYear, t.wMonth,
	        t.wDay, t.wHour, t.wMinute, t.wSecond);
}

void p0_utc_compact(char out[20])
{
	SYSTEMTIME t;

	GetSystemTime(&t);
	snprintf(out, 20, "%04u%02u%02uT%02u%02u%02uZ", t.wYear, t.wMonth,
	        t.wDay, t.wHour, t.wMinute, t.wSecond);
}

int p0_mkdirs(const wchar_t *path)
{
	wchar_t buf[MAX_PATH * 2];
	size_t i, n = wcslen(path);

	if (n >= WF_ARRAY_LEN(buf)) {
		return 0;
	}
	memcpy(buf, path, (n + 1) * sizeof *buf);
	for (i = 3; i <= n; i++) { /* skip "C:\" */
		if (L'\\' == buf[i] || L'\0' == buf[i]) {
			wchar_t c = buf[i];

			buf[i] = L'\0';
			if (!CreateDirectoryW(buf, NULL) &&
			        ERROR_ALREADY_EXISTS != GetLastError()) {
				return 0;
			}
			buf[i] = c;
		}
	}
	return 1;
}

struct P0Hash
{
	BCRYPT_ALG_HANDLE alg;
	BCRYPT_HASH_HANDLE h;
	UCHAR *obj;
};

P0Hash *p0_hash_begin(void)
{
	P0Hash *h = calloc(1, sizeof *h);
	DWORD objlen = 0, got = 0;

	if (!h) {
		return NULL;
	}
	if (BCryptOpenAlgorithmProvider(
	            &h->alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0) {
		goto fail;
	}
	BCryptGetProperty(h->alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objlen,
	        sizeof objlen, &got, 0);
	h->obj = malloc(objlen);
	if (!h->obj ||
	        BCryptCreateHash(h->alg, &h->h, h->obj, objlen, NULL, 0, 0) <
	                0) {
		goto fail;
	}
	return h;
fail:
	if (h->alg) {
		BCryptCloseAlgorithmProvider(h->alg, 0);
	}
	free(h->obj);
	free(h);
	return NULL;
}

void p0_hash_update(P0Hash *h, const void *p, size_t n)
{
	const UCHAR *b = p;

	while (n > 0) { /* BCryptHashData takes a ULONG length */
		ULONG k = n > 0x40000000u ? 0x40000000u : (ULONG)n;

		BCryptHashData(h->h, (PUCHAR)b, k, 0);
		b += k;
		n -= k;
	}
}

void p0_hash_end(P0Hash *h, char hex[P0_HASH_HEX])
{
	UCHAR d[32];
	int i;

	BCryptFinishHash(h->h, d, sizeof d, 0);
	for (i = 0; i < 32; i++) {
		snprintf(hex + i * 2, 3, "%02x", d[i]);
	}
	BCryptDestroyHash(h->h);
	BCryptCloseAlgorithmProvider(h->alg, 0);
	free(h->obj);
	free(h);
}

int p0_free_space_for(const wchar_t *dir, unsigned long long fixtures)
{
	static const unsigned long long RESERVE = 4ull << 30;
	ULARGE_INTEGER avail;

	if (!GetDiskFreeSpaceExW(dir, &avail, NULL, NULL)) {
		fprintf(stderr,
		        "cannot read the free space of the test volume "
		        "(error %lu)\n",
		        GetLastError());
		return 0;
	}
	if (avail.QuadPart < fixtures + RESERVE) {
		fprintf(stderr,
		        "not enough free disk space: %llu MiB free, the test "
		        "needs %llu MiB plus a %llu MiB reserve\n",
		        avail.QuadPart >> 20, fixtures >> 20, RESERVE >> 20);
		return 0;
	}
	return 1;
}

static int sha256_file(const wchar_t *path, char hex[P0_HASH_HEX], DWORD share)
{
	HANDLE f = CreateFileW(path, GENERIC_READ, share, NULL, OPEN_EXISTING,
	        FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	P0Hash *h;
	static UCHAR buf[1 << 20];
	DWORD got;
	BOOL read_ok;

	hex[0] = '\0';
	if (INVALID_HANDLE_VALUE == f) {
		return 0;
	}
	h = p0_hash_begin();
	if (!h) {
		CloseHandle(f);
		return 0;
	}
	while ((read_ok = ReadFile(f, buf, sizeof buf, &got, NULL)) &&
	        got > 0) {
		p0_hash_update(h, buf, got);
	}
	CloseHandle(f);
	p0_hash_end(h, hex);
	if (!read_ok) {
		hex[0] = '\0';
		return 0;
	}
	return 1;
}

int p0_sha256_file(const wchar_t *path, char hex[P0_HASH_HEX])
{
	return sha256_file(path, hex, FILE_SHARE_READ | FILE_SHARE_WRITE);
}

int p0_sha256_observe(const wchar_t *path, char hex[P0_HASH_HEX])
{
	return sha256_file(path, hex,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
}

void p0_json_str(FILE *f, const char *s)
{
	fputc('"', f);
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;

		if ('"' == c || '\\' == c) {
			fprintf(f, "\\%c", c);
		} else if (c < 0x20) {
			fprintf(f, "\\u%04x", c);
		} else {
			fputc(c, f);
		}
	}
	fputc('"', f);
}

void p0_json_wstr(FILE *f, const wchar_t *s)
{
	char u8[2048];
	int n = WideCharToMultiByte(
	        CP_UTF8, 0, s, -1, u8, sizeof u8, NULL, NULL);

	p0_json_str(f, n > 0 ? u8 : "");
}

FILE *p0_fopen_w(const wchar_t *path)
{
	/* _wfopen_s would open the file exclusively; the host process reads the
	 * log while the child is still writing it, so share it explicitly. */
	return _wfsopen(path, L"wb", _SH_DENYWR);
}

static int reg_str(HKEY root,
        const wchar_t *key,
        const wchar_t *name,
        wchar_t *out,
        DWORD cap_units)
{
	DWORD cb = cap_units * sizeof *out;

	out[0] = L'\0';
	return ERROR_SUCCESS ==
	        RegGetValueW(root, key, name, RRF_RT_REG_SZ, NULL, out, &cb);
}

static DWORD reg_dword(HKEY root, const wchar_t *key, const wchar_t *name)
{
	DWORD v = 0, cb = sizeof v;

	RegGetValueW(root, key, name, RRF_RT_REG_DWORD, NULL, &v, &cb);
	return v;
}

typedef LONG(WINAPI *RtlGetVersionFn)(OSVERSIONINFOW *);

static void os_version(DWORD *major, DWORD *minor, DWORD *build)
{
	OSVERSIONINFOW vi = {.dwOSVersionInfoSize = sizeof vi};
	HMODULE nt = GetModuleHandleW(L"ntdll.dll");
	RtlGetVersionFn fn = nt
	        ? (RtlGetVersionFn)(void *)GetProcAddress(nt, "RtlGetVersion")
	        : NULL;

	if (fn) {
		fn(&vi);
	}
	*major = vi.dwMajorVersion;
	*minor = vi.dwMinorVersion;
	*build = vi.dwBuildNumber;
}

/* The ProductName registry value still says "Windows 10" on Windows 11, so
 * the family is taken from the build number (Windows 11 starts at 22000). */
static const char *os_family(DWORD build)
{
	return build >= 22000 ? "Windows 11" : "Windows 10";
}

void p0_os_summary(char *out, size_t cap)
{
	static const wchar_t *k =
	        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
	wchar_t edition[64], disp[32];
	DWORD maj, min, build, ubr = reg_dword(HKEY_LOCAL_MACHINE, k, L"UBR");
	char e8[128], d8[64];

	os_version(&maj, &min, &build);
	reg_str(HKEY_LOCAL_MACHINE, k, L"EditionID", edition, 64);
	reg_str(HKEY_LOCAL_MACHINE, k, L"DisplayVersion", disp, 32);
	WideCharToMultiByte(CP_UTF8, 0, edition, -1, e8, sizeof e8, NULL, NULL);
	WideCharToMultiByte(CP_UTF8, 0, disp, -1, d8, sizeof d8, NULL, NULL);
	snprintf(out, cap, "%s %s (%s) build %lu.%lu.%lu.%lu", os_family(build),
	        d8, e8, maj, min, build, ubr);
}

static int is_elevated(void)
{
	HANDLE tok;
	TOKEN_ELEVATION e = {0};
	DWORD got = 0;
	int r = 0;

	if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
		if (GetTokenInformation(
		            tok, TokenElevation, &e, sizeof e, &got)) {
			r = 0 != e.TokenIsElevated;
		}
		CloseHandle(tok);
	}
	return r;
}

int p0_write_environment(const wchar_t *file,
        const wchar_t *target_path,
        const char *harness_name,
        const char *harness_version)
{
	static const wchar_t *k =
	        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
	FILE *f = p0_fopen_w(file);
	wchar_t exe[MAX_PATH], tmp[256], root[1024], fsname[32] = L"";
	char hash[P0_HASH_HEX];
	DWORD maj, min, build, serial = 0, maxc = 0, flags = 0;
	ULARGE_INTEGER avail = {0}, total = {0}, freeb = {0};
	MEMORYSTATUSEX ms = {.dwLength = sizeof ms};
	SYSTEM_INFO si;
	UINT dtype;

	if (!f) {
		return 0;
	}
	GetModuleFileNameW(NULL, exe, MAX_PATH);
	if (!p0_sha256_file(exe, hash)) {
		strcpy_s(hash, sizeof hash, "unavailable");
	}
	os_version(&maj, &min, &build);
	GlobalMemoryStatusEx(&ms);
	GetNativeSystemInfo(&si);

	fputs("{\n  \"harness\": {\"name\": ", f);
	p0_json_str(f, harness_name);
	fputs(", \"version\": ", f);
	p0_json_str(f, harness_version);
	fprintf(f, ", \"exe_sha256\": \"%s\", \"compiler\": ", hash);
#if defined(__clang__)
	p0_json_str(f, "clang-cl " __clang_version__);
#elif defined(_MSC_VER)
	{
		char c[48];

		snprintf(c, sizeof c, "MSVC %d", _MSC_VER);
		p0_json_str(f, c);
	}
#else
	p0_json_str(f, "unknown");
#endif
	fprintf(f, "},\n  \"os\": {\"family\": \"%s\", \"product_registry\": ",
	        os_family(build));
	reg_str(HKEY_LOCAL_MACHINE, k, L"ProductName", tmp, 256);
	p0_json_wstr(f, tmp);
	fputs(", \"display_version\": ", f);
	reg_str(HKEY_LOCAL_MACHINE, k, L"DisplayVersion", tmp, 256);
	p0_json_wstr(f, tmp);
	fputs(", \"edition\": ", f);
	reg_str(HKEY_LOCAL_MACHINE, k, L"EditionID", tmp, 256);
	p0_json_wstr(f, tmp);
	fprintf(f,
	        ", \"version\": \"%lu.%lu.%lu\", \"ubr\": %lu, \"arch\": "
	        "\"%s\"},\n",
	        maj, min, build, reg_dword(HKEY_LOCAL_MACHINE, k, L"UBR"),
	        PROCESSOR_ARCHITECTURE_AMD64 == si.wProcessorArchitecture
	                ? "x64"
	                : PROCESSOR_ARCHITECTURE_ARM64 ==
	                        si.wProcessorArchitecture
	                ? "arm64"
	                : "other");

	fputs("  \"cpu\": {\"name\": ", f);
	reg_str(HKEY_LOCAL_MACHINE,
	        L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
	        L"ProcessorNameString", tmp, 256);
	p0_json_wstr(f, tmp);
	fprintf(f, ", \"logical_processors\": %lu},\n",
	        si.dwNumberOfProcessors);
	fprintf(f, "  \"memory_mb\": %llu,\n  \"elevated\": %s,\n",
	        (unsigned long long)(ms.ullTotalPhys >> 20),
	        is_elevated() ? "true" : "false");

	fputs("  \"volume\": {\"target\": ", f);
	p0_json_wstr(f, target_path);
	if (GetVolumePathNameW(target_path, root, 1024) &&
	        GetVolumeInformationW(
	                root, NULL, 0, &serial, &maxc, &flags, fsname, 32)) {
		dtype = GetDriveTypeW(root);
		GetDiskFreeSpaceExW(root, &avail, &total, &freeb);
		fputs(", \"root\": ", f);
		p0_json_wstr(f, root);
		fputs(", \"fs_name\": ", f);
		p0_json_wstr(f, fsname);
		fprintf(f,
		        ", \"serial\": \"%08lx\", \"max_component\": %lu, "
		        "\"flags\": \"0x%08lx\","
		        " \"supports_open_by_file_id\": %s, "
		        "\"supports_reparse_points\": %s,"
		        " \"supports_named_streams\": %s, \"supports_sparse\": "
		        "%s,"
		        " \"drive_type\": %u, \"free_bytes\": %llu, "
		        "\"total_bytes\": %llu",
		        serial, maxc, flags,
		        (flags & FILE_SUPPORTS_OPEN_BY_FILE_ID) ? "true"
		                                                : "false",
		        (flags & FILE_SUPPORTS_REPARSE_POINTS) ? "true"
		                                               : "false",
		        (flags & FILE_NAMED_STREAMS) ? "true" : "false",
		        (flags & FILE_SUPPORTS_SPARSE_FILES) ? "true" : "false",
		        dtype, (unsigned long long)avail.QuadPart,
		        (unsigned long long)total.QuadPart);
	} else {
		fprintf(f, ", \"error\": \"volume query failed (%lu)\"",
		        GetLastError());
	}
	fputs("}\n}\n", f);
	fclose(f);
	return 1;
}

int p0_write_run_json(const wchar_t *file,
        const char *test_id,
        const char *run_id,
        const char *build_id,
        const char *filesystem,
        const char *const *hypotheses,
        const char *result,
        const char *const *decisions,
        const char *notes)
{
	FILE *f = p0_fopen_w(file);
	char ts[24], os[256], git[128] = "unknown";
	size_t i;

	if (!f) {
		return 0;
	}
	p0_utc_iso(ts);
	p0_os_summary(os, sizeof os);
	GetEnvironmentVariableA("P0_GIT_COMMIT", git, sizeof git);

	fputs("{\n  \"test_id\": ", f);
	p0_json_str(f, test_id);
	fputs(",\n  \"run_id\": ", f);
	p0_json_str(f, run_id);
	fprintf(f, ",\n  \"timestamp_utc\": \"%s\",\n  \"git_commit\": ", ts);
	p0_json_str(f, git);
	fputs(",\n  \"build_id\": ", f);
	p0_json_str(f, build_id);
	fputs(",\n  \"os_build\": ", f);
	p0_json_str(f, os);
	fputs(",\n  \"filesystem\": ", f);
	p0_json_str(f, filesystem);
	fputs(",\n  \"hypotheses\": [", f);
	for (i = 0; hypotheses[i]; i++) {
		fputs(i ? ", " : "", f);
		p0_json_str(f, hypotheses[i]);
	}
	fputs("],\n  \"result\": ", f);
	p0_json_str(f, result);
	fputs(",\n  \"decision_ids\": [", f);
	for (i = 0; decisions[i]; i++) {
		fputs(i ? ", " : "", f);
		p0_json_str(f, decisions[i]);
	}
	fputs("],\n  \"notes\": ", f);
	p0_json_str(f, notes);
	fputs("\n}\n", f);
	fclose(f);
	return 1;
}
