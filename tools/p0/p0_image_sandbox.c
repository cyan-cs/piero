/*
 * P0-SANDBOX proof of concept (A01..A05), with explicit A06..A10 blockers.
 *
 *   parent:  p0_image_sandbox --results <root> [--keep-profile]
 *   worker:  p0_image_sandbox --worker <img> <in> <out> <canary_file>
 * <canary_dll> <suite> (started by the parent only; handle values are hex)
 *
 * Mirrors design section 9 at PoC depth: AppContainer profile, staged worker
 * (temp copy -> flush -> SHA-256 -> no-clobber rename), inherited read-only
 * image handle plus anonymous pipes (no path names for the image), mitigation
 * policies at creation, a Job with memory/process limits, WIC decode of
 * built-in formats, and negative tests. It is NOT the production worker: no
 * restart policy, no size negotiation, no cache. Not covered here: A06
 * (anti-thrashing), A07 (IPC fuzz), A08 (SAC/signing), A09, A10.
 */
#define COBJMACROS
#include <winsock2.h>

#include <windows.h>

#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <softpub.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <userenv.h>
#include <wincodec.h>
#include <ws2tcpip.h>

#include "p0_area.h"
#include "p0_common.h"
#include "preview/wf_image_check.h"
#include "base/wf_macro.h"

#define VERSION "1.1"
#define PROFILE_NAME L"wfile.p0.poc"
#define MAX_RASTER (16u << 20) /* design 9.2: IPC output raster */
#define MAX_INPUT (128u << 20)

/* ======================================================================== */
/* worker                                                                    */
/* ======================================================================== */

static HANDLE g_out;

/* First 4 bytes of SHA-256 of each pixel row, as hex, for at most 48 rows. Lets
 * two decodes of the same size be compared row by row without shipping pixels.
 */
static void row_digests(const BYTE *pix, UINT w, UINT h, char *out, size_t cap)
{
	UINT y;
	size_t n = 0;

	out[0] = '\0';
	for (y = 0; y < h && y < 48 && n + 9 < cap; y++) {
		P0Hash *ph = p0_hash_begin();
		char hex[P0_HASH_HEX];

		p0_hash_update(ph, pix + (size_t)y * w * 4, (size_t)w * 4);
		p0_hash_end(ph, hex);
		memcpy(out + n, hex, 8);
		n += 8;
		out[n] = '\0';
	}
}

static UINT count_nonzero_pixels(const BYTE *pix, UINT w, UINT h)
{
	UINT i, n = 0;

	for (i = 0; i < w * h; i++) {
		n += (pix[i * 4] | pix[i * 4 + 1] | pix[i * 4 + 2] |
		             pix[i * 4 + 3]) != 0;
	}
	return n;
}

static void emit(const char *fmt, ...)
{
	char line[512];
	va_list ap;
	int n;
	DWORD wrote;

	va_start(ap, fmt);
	n = vsnprintf(line, sizeof line - 2, fmt, ap);
	va_end(ap);
	if (n < 0) {
		return;
	}
	line[n++] = '\n';
	WriteFile(g_out, line, (DWORD)n, &wrote, NULL);
}

static void emit_mitigations(const char *tag)
{
	PROCESS_MITIGATION_DEP_POLICY dep = {0};
	PROCESS_MITIGATION_ASLR_POLICY aslr = {0};
	PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dyn = {0};
	PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY ext = {0};
	PROCESS_MITIGATION_CHILD_PROCESS_POLICY child = {0};
	PROCESS_MITIGATION_IMAGE_LOAD_POLICY img = {0};
	PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY sig = {0};
	PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY strict = {0};
	HANDLE p = GetCurrentProcess();

	emit("%s.dep_query=%d", tag,
	        GetProcessMitigationPolicy(
	                p, ProcessDEPPolicy, &dep, sizeof dep));
	emit("%s.dep=%d", tag, (int)dep.Enable);
	GetProcessMitigationPolicy(p, ProcessASLRPolicy, &aslr, sizeof aslr);
	emit("%s.aslr_bottom_up=%d", tag,
	        (int)aslr.EnableBottomUpRandomization);
	emit("%s.aslr_force_relocate=%d", tag,
	        (int)aslr.EnableForceRelocateImages);
	emit("%s.aslr_high_entropy=%d", tag, (int)aslr.EnableHighEntropy);
	GetProcessMitigationPolicy(
	        p, ProcessDynamicCodePolicy, &dyn, sizeof dyn);
	emit("%s.dynamic_code_prohibited=%d", tag,
	        (int)dyn.ProhibitDynamicCode);
	GetProcessMitigationPolicy(
	        p, ProcessExtensionPointDisablePolicy, &ext, sizeof ext);
	emit("%s.extension_points_disabled=%d", tag,
	        (int)ext.DisableExtensionPoints);
	GetProcessMitigationPolicy(
	        p, ProcessChildProcessPolicy, &child, sizeof child);
	emit("%s.no_child_process=%d", tag, (int)child.NoChildProcessCreation);
	GetProcessMitigationPolicy(p, ProcessImageLoadPolicy, &img, sizeof img);
	emit("%s.image_no_remote=%d", tag, (int)img.NoRemoteImages);
	emit("%s.image_no_low_label=%d", tag,
	        (int)img.NoLowMandatoryLabelImages);
	emit("%s.image_prefer_system32=%d", tag, (int)img.PreferSystem32Images);
	GetProcessMitigationPolicy(p, ProcessSignaturePolicy, &sig, sizeof sig);
	emit("%s.signature_microsoft_only=%d", tag,
	        (int)sig.MicrosoftSignedOnly);
	GetProcessMitigationPolicy(
	        p, ProcessStrictHandleCheckPolicy, &strict, sizeof strict);
	emit("%s.strict_handle_checks=%d", tag,
	        (int)strict.RaiseExceptionOnInvalidHandleReference);
}

static void emit_token(void)
{
	HANDLE tok;
	DWORD is_ac = 0, got = 0;
	char *sidstr = NULL;

	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
		emit("token.open_error=%lu", GetLastError());
		return;
	}
	GetTokenInformation(
	        tok, TokenIsAppContainer, &is_ac, sizeof is_ac, &got);
	emit("token.is_appcontainer=%lu", is_ac);
	{
		BYTE buf[256];
		DWORD len = 0;

		if (GetTokenInformation(
		            tok, TokenAppContainerSid, buf, sizeof buf, &len)) {
			PSID sid = ((TOKEN_APPCONTAINER_INFORMATION *)buf)
			                   ->TokenAppContainer;

			if (sid && ConvertSidToStringSidA(sid, &sidstr)) {
				emit("token.package_sid=%s", sidstr);
				LocalFree(sidstr);
			}
		}
	}
	{
		BYTE buf[64];
		DWORD len = 0;

		if (GetTokenInformation(
		            tok, TokenIntegrityLevel, buf, sizeof buf, &len)) {
			PSID s = ((TOKEN_MANDATORY_LABEL *)buf)->Label.Sid;

			emit("token.integrity_rid=0x%lx",
			        *GetSidSubAuthority(
			                s, *GetSidSubAuthorityCount(s) - 1));
		}
	}
	CloseHandle(tok);
}

static void test_dll(const char *tag, const wchar_t *dll)
{
	HMODULE m = LoadLibraryW(dll);

	if (m) {
		emit("%s=LOADED", tag);
		FreeLibrary(m);
	} else {
		emit("%s=BLOCKED err=%lu", tag, GetLastError());
	}
}

static void test_negative(const wchar_t *canary_file, unsigned port)
{
	HANDLE h = CreateFileW(canary_file, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, 0, NULL);

	if (INVALID_HANDLE_VALUE != h) {
		emit("neg.arbitrary_path_open=ALLOWED");
		CloseHandle(h);
	} else {
		emit("neg.arbitrary_path_open=DENIED err=%lu", GetLastError());
	}

	{
		wchar_t cmd[] = L"cmd.exe /c exit 0";
		STARTUPINFOW si = {.cb = sizeof si};
		PROCESS_INFORMATION pi;

		if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
		            CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
			emit("neg.child_process=ALLOWED");
			TerminateProcess(pi.hProcess, 0);
			CloseHandle(pi.hProcess);
			CloseHandle(pi.hThread);
		} else {
			emit("neg.child_process=DENIED err=%lu",
			        GetLastError());
		}
	}

	{
		/* A positive control lives in the parent: it owns this listener
		 * and has connected to it itself, so "no connection arrived"
		 * can only mean the sandbox stopped the worker. */
		WSADATA wsa;
		SOCKET s;
		struct sockaddr_in a = {0};

		if (0 != WSAStartup(MAKEWORD(2, 2), &wsa)) {
			emit("neg.network_connect=DENIED wsastartup_failed");
		} else {
			s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			a.sin_family = AF_INET;
			a.sin_port = htons((u_short)port);
			inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
			if (INVALID_SOCKET == s) {
				emit("neg.network_connect=DENIED socket err=%d",
				        WSAGetLastError());
			} else {
				u_long nb = 1;
				fd_set w;
				struct timeval tv = {1, 0};

				ioctlsocket(s, (long)FIONBIO, &nb);
				connect(s, (struct sockaddr *)&a, sizeof a);
				FD_ZERO(&w);
				FD_SET(s, &w);
				if (select(0, NULL, &w, NULL, &tv) > 0) {
					int err = 0, len = sizeof err;

					getsockopt(s, SOL_SOCKET, SO_ERROR,
					        (char *)&err, &len);
					if (0 == err) {
						emit("neg.network_connect="
						     "ALLOWED");
					} else {
						emit("neg.network_connect="
						     "DENIED so_error=%d",
						        err);
					}
				} else {
					emit("neg.network_connect=DENIED "
					     "no_connection");
				}
				closesocket(s);
			}
			WSACleanup();
		}
	}
}

/* WIC is lenient with a damaged PNG (measured in A03: it decodes what it can
 * and reports success), so the structure is verified first. */
static bool container_structure_ok(
        const BYTE *data, size_t size, const char **why)
{
	*why = "";
	if (size >= 8 && 0x89 == data[0] && !memcmp(data + 1, "PNG", 3)) {
		return wf_png_structure_ok(data, size, why);
	}
	if (size >= 4 && 0 == data[0] && 0 == data[1] && 1 == data[2] &&
	        0 == data[3]) {
		return wf_ico_structure_ok(data, size, why);
	}
	return true; /* other containers: the decoder's own validation */
}

static void decode_with_wic(const BYTE *data, size_t size, const char *tag)
{
	IWICImagingFactory *fac = NULL;
	IWICStream *stream = NULL;
	IWICBitmapDecoder *dec = NULL;
	IWICBitmapFrameDecode *frame = NULL;
	IWICFormatConverter *conv = NULL;
	BYTE *pix = NULL;
	UINT w = 0, h = 0;
	HRESULT hr;
	GUID container = {0};
	DWORD t0 = GetTickCount();
	const char *why;

	if (!container_structure_ok(data, size, &why)) {
		emit("%s.decode=FAIL stage=container_structure why=%s", tag,
		        why);
		return;
	}
	hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	if (FAILED(hr)) {
		emit("%s.decode=FAIL stage=coinit hr=0x%08lx", tag,
		        (unsigned long)hr);
		return;
	}
	hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL,
	        CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&fac);
	if (FAILED(hr)) {
		emit("%s.decode=FAIL stage=factory hr=0x%08lx", tag,
		        (unsigned long)hr);
		goto out;
	}
	if (0 == size ||
	        FAILED(hr = IWICImagingFactory_CreateStream(fac, &stream)) ||
	        FAILED(hr = IWICStream_InitializeFromMemory(
	                       stream, (BYTE *)data, (DWORD)size))) {
		emit("%s.decode=FAIL stage=stream hr=0x%08lx", tag,
		        (unsigned long)hr);
		goto out;
	}
	hr = IWICImagingFactory_CreateDecoderFromStream(fac, (IStream *)stream,
	        NULL, WICDecodeMetadataCacheOnDemand, &dec);
	if (FAILED(hr)) {
		emit("%s.decode=FAIL stage=decoder hr=0x%08lx", tag,
		        (unsigned long)hr);
		goto out;
	}
	IWICBitmapDecoder_GetContainerFormat(dec, &container);
	/* only the built-in formats the design allows */
	if (!IsEqualGUID(&container, &GUID_ContainerFormatPng) &&
	        !IsEqualGUID(&container, &GUID_ContainerFormatJpeg) &&
	        !IsEqualGUID(&container, &GUID_ContainerFormatBmp) &&
	        !IsEqualGUID(&container, &GUID_ContainerFormatGif) &&
	        !IsEqualGUID(&container, &GUID_ContainerFormatTiff) &&
	        !IsEqualGUID(&container, &GUID_ContainerFormatIco)) {
		emit("%s.decode=FAIL stage=container_not_allowed", tag);
		goto out;
	}
	if (FAILED(hr = IWICBitmapDecoder_GetFrame(dec, 0, &frame)) ||
	        FAILED(hr = IWICBitmapFrameDecode_GetSize(frame, &w, &h))) {
		emit("%s.decode=FAIL stage=frame hr=0x%08lx", tag,
		        (unsigned long)hr);
		goto out;
	}
	/* the size is checked BEFORE any pixel memory is requested */
	if (0 == w || 0 == h || (unsigned long long)w * h * 4ull > MAX_RASTER) {
		emit("%s.decode=FAIL stage=raster_limit w=%u h=%u", tag, w, h);
		goto out;
	}
	if (FAILED(hr = IWICImagingFactory_CreateFormatConverter(fac, &conv)) ||
	        FAILED(hr = IWICFormatConverter_Initialize(conv,
	                       (IWICBitmapSource *)frame,
	                       &GUID_WICPixelFormat32bppPBGRA,
	                       WICBitmapDitherTypeNone, NULL, 0.0,
	                       WICBitmapPaletteTypeCustom))) {
		emit("%s.decode=FAIL stage=convert hr=0x%08lx", tag,
		        (unsigned long)hr);
		goto out;
	}
	pix = malloc((size_t)w * h * 4);
	if (!pix ||
	        FAILED(hr = IWICFormatConverter_CopyPixels(
	                       conv, NULL, w * 4, w * h * 4, pix))) {
		emit("%s.decode=FAIL stage=copy hr=0x%08lx", tag,
		        (unsigned long)hr);
		goto out;
	}
	{
		P0Hash *ph = p0_hash_begin();
		char hex[P0_HASH_HEX];

		p0_hash_update(ph, pix, (size_t)w * h * 4);
		p0_hash_end(ph, hex);
		char rows[400];

		emit("%s.decode=OK w=%u h=%u pixel_sha256=%s ms=%lu", tag, w, h,
		        hex, GetTickCount() - t0);
		row_digests(pix, w, h, rows, sizeof rows);
		emit("%s.rows=%s", tag, rows);
		emit("%s.pixels=%u nonzero_pixels=%u", tag, w * h,
		        count_nonzero_pixels(pix, w, h));
	}
out:
	free(pix);
	if (conv) {
		IWICFormatConverter_Release(conv);
	}
	if (frame) {
		IWICBitmapFrameDecode_Release(frame);
	}
	if (dec) {
		IWICBitmapDecoder_Release(dec);
	}
	if (stream) {
		IWICStream_Release(stream);
	}
	if (fac) {
		IWICImagingFactory_Release(fac);
	}
	CoUninitialize();
}

static HANDLE hex_handle(const wchar_t *s)
{
	return (HANDLE)(ULONG_PTR)_wcstoui64(s, NULL, 16);
}

static int worker_main(int argc, wchar_t **argv)
{
	HANDLE img, in;
	BYTE *buf = NULL;
	LARGE_INTEGER size;
	DWORD got = 0, total = 0;
	const wchar_t *canary_file, *canary_dll, *suite;
	unsigned port;
	char sha[P0_HASH_HEX];

	if (argc < 9) {
		return 2;
	}
	img = hex_handle(argv[2]);
	in = hex_handle(argv[3]);
	g_out = hex_handle(argv[4]);
	canary_file = argv[5];
	canary_dll = argv[6];
	suite = argv[7];
	port = (unsigned)_wtoi(argv[8]);
	(void)in;

	emit("worker.started=1");
	emit_token();
	emit_mitigations("before");

	if (!wcscmp(suite, L"full")) {
		test_dll("dll.unsigned_before_signature_policy", canary_dll);
	}

	/* The design: set the signature policy right after start and verify it;
	 * if it cannot be confirmed the worker must exit before any WIC use. */
	{
		PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY sp = {0};
		PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY chk = {0};
		BOOL ok;

		sp.MicrosoftSignedOnly = 1;
		ok = SetProcessMitigationPolicy(
		        ProcessSignaturePolicy, &sp, sizeof sp);
		emit("signature_policy.set=%d err=%lu", ok,
		        ok ? 0ul : GetLastError());
		GetProcessMitigationPolicy(GetCurrentProcess(),
		        ProcessSignaturePolicy, &chk, sizeof chk);
		emit("signature_policy.verified=%d",
		        (int)chk.MicrosoftSignedOnly);
		if (!chk.MicrosoftSignedOnly) {
			emit("worker.exit_before_wic=signature_policy_not_"
			     "active");
			return 3;
		}
	}
	emit_mitigations("after");

	if (!wcscmp(suite, L"full")) {
		test_dll("dll.unsigned_after_signature_policy", canary_dll);
		test_negative(canary_file, port);
	}

	/* Image: only the inherited handle; no path is ever seen. */
	if (!GetFileSizeEx(img, &size) || size.QuadPart < 0 ||
	        size.QuadPart > MAX_INPUT) {
		emit("image.read=FAIL stage=size err=%lu", GetLastError());
		emit("done=1");
		return 0;
	}
	buf = malloc((size_t)size.QuadPart + 1);
	if (!buf) {
		emit("image.read=FAIL stage=alloc");
		return 0;
	}
	while (total < size.QuadPart &&
	        ReadFile(img, buf + total, (DWORD)(size.QuadPart - total), &got,
	                NULL) &&
	        got) {
		total += got;
	}
	if (total == size.QuadPart) {
		P0Hash *ph = p0_hash_begin();

		p0_hash_update(ph, buf, total);
		p0_hash_end(ph, sha);
		emit("image.read=OK bytes=%lu sha256=%s", total, sha);
	} else {
		emit("image.read=FAIL stage=read err=%lu got=%lu of %lld",
		        GetLastError(), total, size.QuadPart);
	}

	decode_with_wic(buf, total, "image");
	emit("done=1");
	free(buf);
	return 0;
}

/* ======================================================================== */
/* parent                                                                    */
/* ======================================================================== */

typedef struct
{
	char test[8];
	char name[72];
	char expected[96];
	char observed[220];
	int pass;
} Check;

static Check g_checks[256];
static size_t g_nchecks;

static P0Area *g_area; /* this run's scratch area; nothing outside it is created
                          or deleted */
static SOCKET g_listener = INVALID_SOCKET;
static int g_control_ok;

/* Is a connection waiting on the parent's listener? */
static int listener_pending(void)
{
	fd_set r;
	struct timeval tv = {0, 200000};

	FD_ZERO(&r);
	FD_SET(g_listener, &r);
	return select(0, &r, NULL, NULL, &tv) > 0;
}

static void check(const char *test,
        const char *name,
        const char *expected,
        int pass,
        const char *fmt,
        ...)
{
	Check *c = &g_checks[g_nchecks++ % 256];
	va_list ap;

	snprintf(c->test, sizeof c->test, "%s", test);
	snprintf(c->name, sizeof c->name, "%s", name);
	snprintf(c->expected, sizeof c->expected, "%s", expected);
	va_start(ap, fmt);
	vsnprintf(c->observed, sizeof c->observed, fmt, ap);
	va_end(ap);
	c->pass = pass;
	printf("  %-4s %-48s %s  %s\n", test, name, pass ? "PASS" : "FAIL",
	        c->observed);
}

/* key=value lines from a worker, searchable */
typedef struct
{
	char *text;
	size_t n;
} Lines;

static const char *line_value(const Lines *l, const char *key)
{
	static char val[512];
	size_t klen = strlen(key);
	const char *p = l->text;

	while (p && *p) {
		const char *eol = strchr(p, '\n');
		size_t len = eol ? (size_t)(eol - p) : strlen(p);

		if (len > klen && !strncmp(p, key, klen) && '=' == p[klen]) {
			size_t vl = len - klen - 1;

			if (vl >= sizeof val) {
				vl = sizeof val - 1;
			}
			memcpy(val, p + klen + 1, vl);
			val[vl] = '\0';
			val[strcspn(val, "\r")] = '\0';
			return val;
		}
		p = eol ? eol + 1 : NULL;
	}
	return NULL;
}

static int line_is(const Lines *l, const char *key, const char *value)
{
	const char *v = line_value(l, key);

	return v && !strcmp(v, value);
}

static int line_starts(const Lines *l, const char *key, const char *prefix)
{
	const char *v = line_value(l, key);

	return v && !strncmp(v, prefix, strlen(prefix));
}

static int grant_app_packages(const wchar_t *dir)
{
	PSID app_pkgs = NULL;
	PACL old = NULL, merged = NULL;
	PSECURITY_DESCRIPTOR sd = NULL;
	EXPLICIT_ACCESSW ea = {0};
	int ok = 0;

	if (!ConvertStringSidToSidW(
	            L"S-1-15-2-1", &app_pkgs)) { /* ALL APPLICATION PACKAGES */
		return 0;
	}
	if (GetNamedSecurityInfoW(dir, SE_FILE_OBJECT,
	            DACL_SECURITY_INFORMATION, NULL, NULL, &old, NULL,
	            &sd) != ERROR_SUCCESS) {
		goto out;
	}
	ea.grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
	ea.grfAccessMode = GRANT_ACCESS;
	ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
	ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
	ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
	ea.Trustee.ptstrName = (LPWSTR)app_pkgs;
	if (ERROR_SUCCESS != SetEntriesInAclW(1, &ea, old, &merged)) {
		goto out;
	}
	ok = SetNamedSecurityInfoW((LPWSTR)dir, SE_FILE_OBJECT,
	             DACL_SECURITY_INFORMATION, NULL, NULL, merged,
	             NULL) == ERROR_SUCCESS;
out:
	if (merged) {
		LocalFree(merged);
	}
	if (sd) {
		LocalFree(sd);
	}
	LocalFree(app_pkgs);
	return ok;
}

static char *sddl_of(const wchar_t *path)
{
	PSECURITY_DESCRIPTOR sd = NULL;
	LPWSTR w = NULL;
	char *out = NULL;

	if (GetNamedSecurityInfoW(path, SE_FILE_OBJECT,
	            DACL_SECURITY_INFORMATION, NULL, NULL, NULL, NULL,
	            &sd) == ERROR_SUCCESS &&
	        ConvertSecurityDescriptorToStringSecurityDescriptorW(sd,
	                SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &w, NULL)) {
		int n = WideCharToMultiByte(
		        CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);

		out = malloc((size_t)n);
		if (out) {
			WideCharToMultiByte(
			        CP_UTF8, 0, w, -1, out, n, NULL, NULL);
		}
		LocalFree(w);
	}
	if (sd) {
		LocalFree(sd);
	}
	return out;
}

/* temp copy -> flush -> hash -> no-clobber rename. Returns 1 on success. */
static int stage_file(const wchar_t *src,
        const wchar_t *dir,
        const wchar_t *name,
        wchar_t *final_out,
        const char *src_hash)
{
	wchar_t tmp[MAX_PATH * 2];
	HANDLE in, out;
	static BYTE buf[1 << 16];
	DWORD got, wrote;
	char hex[P0_HASH_HEX];

	swprintf(final_out, MAX_PATH * 2, L"%ls\\%ls", dir, name);
	swprintf(tmp, MAX_PATH * 2, L"%ls\\%ls.tmp", dir, name);
	if (INVALID_FILE_ATTRIBUTES !=
	        GetFileAttributesW(
	                final_out)) { /* already staged: verify, never
	    replace */
		return p0_sha256_file(final_out, hex) && !strcmp(hex, src_hash);
	}
	in = CreateFileW(src, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, 0, NULL);
	out = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == in || INVALID_HANDLE_VALUE == out) {
		return 0;
	}
	while (ReadFile(in, buf, sizeof buf, &got, NULL) && got) {
		if (!WriteFile(out, buf, got, &wrote, NULL) || wrote != got) {
			return 0;
		}
	}
	FlushFileBuffers(out);
	CloseHandle(in);
	CloseHandle(out);
	if (!p0_sha256_file(tmp, hex) || 0 != strcmp(hex, src_hash)) {
		return 0;
	}
	return 0 !=
	        MoveFileExW(
	                tmp, final_out, 0); /* no MOVEFILE_REPLACE_EXISTING */
}

typedef struct
{
	const char *name;
	int valid; /* must decode */
	BYTE *data;
	size_t size;
} Sample;

static Sample g_samples[24];
static size_t g_nsamples;

static void add_sample(const char *name, int valid, BYTE *data, size_t size)
{
	g_samples[g_nsamples].name = name;
	g_samples[g_nsamples].valid = valid;
	g_samples[g_nsamples].data = data;
	g_samples[g_nsamples].size = size;
	g_nsamples++;
}

static uint32_t crc32_of(const BYTE *p, size_t n)
{
	uint32_t c = 0xFFFFFFFFu;
	size_t i;
	int k;

	for (i = 0; i < n; i++) {
		c ^= p[i];
		for (k = 0; k < 8; k++) {
			c = (c >> 1) ^
			        (0xEDB88320u & (uint32_t)-(int32_t)(c & 1));
		}
	}
	return ~c;
}

/* Encodes a deterministic 64x48 gradient with a WIC encoder. */
static BYTE *encode_gradient(const GUID *container, size_t *out_size)
{
	enum
	{
		W = 64,
		H = 48
	};
	static BYTE pixels[W * H * 4];
	IWICImagingFactory *fac = NULL;
	IWICBitmapEncoder *enc = NULL;
	IWICBitmapFrameEncode *frame = NULL;
	IStream *stream = NULL;
	IPropertyBag2 *props = NULL;
	WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
	BYTE *result = NULL;
	HGLOBAL hg;
	int x, y;
	HRESULT hr;

	for (y = 0; y < H; y++) {
		for (x = 0; x < W; x++) {
			BYTE *p = pixels + (y * W + x) * 4;

			p[0] = (BYTE)(x * 4);
			p[1] = (BYTE)(y * 5);
			p[2] = (BYTE)((x + y) * 2);
			p[3] = 255;
		}
	}
	if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL,
	            CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
	            (void **)&fac))) {
		return NULL;
	}
	if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &stream))) {
		goto out;
	}
	if (FAILED(IWICImagingFactory_CreateEncoder(
	            fac, container, NULL, &enc)) ||
	        FAILED(IWICBitmapEncoder_Initialize(
	                enc, stream, WICBitmapEncoderNoCache)) ||
	        FAILED(IWICBitmapEncoder_CreateNewFrame(enc, &frame, &props)) ||
	        FAILED(IWICBitmapFrameEncode_Initialize(frame, props)) ||
	        FAILED(IWICBitmapFrameEncode_SetSize(frame, W, H))) {
		goto out;
	}
	if (IsEqualGUID(container, &GUID_ContainerFormatJpeg) ||
	        IsEqualGUID(container, &GUID_ContainerFormatGif)) {
		fmt = GUID_WICPixelFormat24bppBGR; /* encoders without alpha */
	}
	if (IsEqualGUID(container, &GUID_ContainerFormatGif)) {
		goto gif_via_converter; /* needs a palette: handled below */
	}
	hr = IWICBitmapFrameEncode_SetPixelFormat(frame, &fmt);
	if (FAILED(hr)) {
		goto out;
	}
	if (IsEqualGUID(&fmt,
	            &GUID_WICPixelFormat24bppBGR)) { /* repack BGRA -> BGR */
		static BYTE bgr[W * H * 3];

		for (y = 0; y < W * H; y++) {
			bgr[y * 3 + 0] = pixels[y * 4 + 0];
			bgr[y * 3 + 1] = pixels[y * 4 + 1];
			bgr[y * 3 + 2] = pixels[y * 4 + 2];
		}
		hr = IWICBitmapFrameEncode_WritePixels(
		        frame, H, W * 3, sizeof bgr, bgr);
	} else {
		hr = IWICBitmapFrameEncode_WritePixels(
		        frame, H, W * 4, sizeof pixels, pixels);
	}
	if (FAILED(hr)) {
		goto out;
	}
	goto commit;

gif_via_converter:
	{
		IWICBitmap *bmp = NULL;
		IWICPalette *pal = NULL;
		IWICFormatConverter *conv = NULL;
		WICPixelFormatGUID idx = GUID_WICPixelFormat8bppIndexed;

		if (FAILED(IWICImagingFactory_CreateBitmapFromMemory(fac, W, H,
		            &GUID_WICPixelFormat32bppBGRA, W * 4, sizeof pixels,
		            pixels, &bmp)) ||
		        FAILED(IWICImagingFactory_CreatePalette(fac, &pal)) ||
		        FAILED(IWICPalette_InitializeFromBitmap(
		                pal, (IWICBitmapSource *)bmp, 64, FALSE)) ||
		        FAILED(IWICBitmapFrameEncode_SetPixelFormat(
		                frame, &idx)) ||
		        FAILED(IWICBitmapFrameEncode_SetPalette(frame, pal)) ||
		        FAILED(IWICImagingFactory_CreateFormatConverter(
		                fac, &conv)) ||
		        FAILED(IWICFormatConverter_Initialize(conv,
		                (IWICBitmapSource *)bmp, &idx,
		                WICBitmapDitherTypeNone, pal, 0.0,
		                WICBitmapPaletteTypeMedianCut)) ||
		        FAILED(IWICBitmapFrameEncode_WriteSource(
		                frame, (IWICBitmapSource *)conv, NULL))) {
			if (conv) {
				IWICFormatConverter_Release(conv);
			}
			if (pal) {
				IWICPalette_Release(pal);
			}
			if (bmp) {
				IWICBitmap_Release(bmp);
			}
			goto out;
		}
		IWICFormatConverter_Release(conv);
		IWICPalette_Release(pal);
		IWICBitmap_Release(bmp);
	}
commit:
	if (FAILED(IWICBitmapFrameEncode_Commit(frame)) ||
	        FAILED(IWICBitmapEncoder_Commit(enc))) {
		goto out;
	}
	if (SUCCEEDED(GetHGlobalFromStream(stream, &hg))) {
		SIZE_T n = GlobalSize(hg);
		void *src = GlobalLock(hg);

		result = malloc(n);
		if (result && src) {
			memcpy(result, src, n);
			*out_size = n;
		}
		GlobalUnlock(hg);
	}
out:
	if (props) {
		IPropertyBag2_Release(props);
	}
	if (frame) {
		IWICBitmapFrameEncode_Release(frame);
	}
	if (enc) {
		IWICBitmapEncoder_Release(enc);
	}
	if (stream) {
		IStream_Release(stream);
	}
	if (fac) {
		IWICImagingFactory_Release(fac);
	}
	return result;
}

/* An ICO that wraps PNG data (the layout Vista and later accept). */
static BYTE *wrap_png_in_ico(const BYTE *png, size_t n, size_t *out_size)
{
	BYTE *ico = malloc(22 + n);

	if (!ico) {
		return NULL;
	}
	memset(ico, 0, 22);
	ico[2] = 1; /* type: icon */
	ico[4] = 1; /* one image */
	ico[6] = 64;
	ico[7] = 48;
	ico[10] = 1;
	ico[12] = 32; /* planes, bpp */
	ico[14] = (BYTE)(n & 0xFF);
	ico[15] = (BYTE)((n >> 8) & 0xFF);
	ico[16] = (BYTE)((n >> 16) & 0xFF);
	ico[17] = (BYTE)((n >> 24) & 0xFF);
	ico[18] = 22; /* offset of the image data */
	memcpy(ico + 22, png, n);
	*out_size = 22 + n;
	return ico;
}

static void build_samples(void)
{
	size_t n;
	BYTE *png, *p;
	static const struct
	{
		const char *name;
		const GUID *g;
	} enc[] = {
	        {"png", &GUID_ContainerFormatPng},
	        {"jpeg", &GUID_ContainerFormatJpeg},
	        {"bmp", &GUID_ContainerFormatBmp},
	        {"gif", &GUID_ContainerFormatGif},
	        {"tiff", &GUID_ContainerFormatTiff},
	};
	size_t i;

	for (i = 0; i < WF_ARRAY_LEN(enc); i++) {
		p = encode_gradient(enc[i].g, &n);
		if (p) {
			add_sample(enc[i].name, 1, p, n);
		} else {
			printf("  (could not encode %s sample)\n", enc[i].name);
		}
	}
	png = encode_gradient(&GUID_ContainerFormatPng, &n);
	if (png) {
		size_t isz;
		BYTE *ico = wrap_png_in_ico(png, n, &isz);

		if (ico) {
			add_sample("ico", 1, ico, isz);
		}

		/* malformed corpus */
		p = malloc(100);
		memcpy(p, png, n < 100 ? n : 100);
		add_sample("png_truncated_100b", 0, p, n < 100 ? n : 100);
		p = malloc(n);
		memcpy(p, png, n);
		p[n / 2] ^= 0xFF;
		p[n / 2 + 1] ^= 0x55;
		add_sample("png_corrupt_middle", 0, p, n);
		/* IHDR claiming 100000 x 100000 with a *valid* CRC: must be
		 * refused on size */
		p = malloc(n);
		memcpy(p, png, n);
		p[16] = 0;
		p[17] = 1;
		p[18] = 0x86;
		p[19] = 0xA0; /* width  = 100000 */
		p[20] = 0;
		p[21] = 1;
		p[22] = 0x86;
		p[23] = 0xA0; /* height = 100000 */
		{
			uint32_t c =
			        crc32_of(p + 12, 17); /* "IHDR" + 13 bytes */

			p[29] = (BYTE)(c >> 24);
			p[30] = (BYTE)(c >> 16);
			p[31] = (BYTE)(c >> 8);
			p[32] = (BYTE)c;
		}
		add_sample("png_dimension_bomb", 0, p, n);
		free(png);
	}
	p = encode_gradient(&GUID_ContainerFormatJpeg, &n);
	if (p) {
		BYTE *t = malloc(n / 2);

		memcpy(t, p, n / 2);
		add_sample("jpeg_truncated_half", 0, t, n / 2);
		free(p);
	}
	{
		BYTE *r = malloc(4096);
		uint32_t s = 0x9E3779B9u;
		size_t k;

		for (k = 0; k < 4096; k++) {
			s = s * 1664525u + 1013904223u;
			r[k] = (BYTE)(s >> 24);
		}
		add_sample("random_4k", 0, r, 4096);
		add_sample("empty", 0, malloc(1), 0);
	}
}

/* Parent-side reference decode (unsandboxed) of the same bytes. */
static int reference_hash(const BYTE *data,
        size_t size,
        char hex[P0_HASH_HEX],
        char *rows_out,
        size_t rows_cap)
{
	IWICImagingFactory *fac = NULL;
	IWICStream *stream = NULL;
	IWICBitmapDecoder *dec = NULL;
	IWICBitmapFrameDecode *frame = NULL;
	IWICFormatConverter *conv = NULL;
	UINT w, h;
	BYTE *pix = NULL;
	int ok = 0;

	if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL,
	            CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
	            (void **)&fac))) {
		return 0;
	}
	if (size && SUCCEEDED(IWICImagingFactory_CreateStream(fac, &stream)) &&
	        SUCCEEDED(IWICStream_InitializeFromMemory(
	                stream, (BYTE *)data, (DWORD)size)) &&
	        SUCCEEDED(IWICImagingFactory_CreateDecoderFromStream(fac,
	                (IStream *)stream, NULL, WICDecodeMetadataCacheOnDemand,
	                &dec)) &&
	        SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &frame)) &&
	        SUCCEEDED(IWICBitmapFrameDecode_GetSize(frame, &w, &h)) &&
	        (unsigned long long)w * h * 4ull <= MAX_RASTER &&
	        SUCCEEDED(
	                IWICImagingFactory_CreateFormatConverter(fac, &conv)) &&
	        SUCCEEDED(IWICFormatConverter_Initialize(conv,
	                (IWICBitmapSource *)frame,
	                &GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
	                NULL, 0.0, WICBitmapPaletteTypeCustom))) {
		pix = malloc((size_t)w * h * 4);
		if (pix &&
		        SUCCEEDED(IWICFormatConverter_CopyPixels(
		                conv, NULL, w * 4, w * h * 4, pix))) {
			P0Hash *ph = p0_hash_begin();

			p0_hash_update(ph, pix, (size_t)w * h * 4);
			p0_hash_end(ph, hex);
			if (rows_out) {
				row_digests(pix, w, h, rows_out, rows_cap);
			}
			ok = 1;
		}
	}
	free(pix);
	if (conv) {
		IWICFormatConverter_Release(conv);
	}
	if (frame) {
		IWICBitmapFrameDecode_Release(frame);
	}
	if (dec) {
		IWICBitmapDecoder_Release(dec);
	}
	if (stream) {
		IWICStream_Release(stream);
	}
	if (fac) {
		IWICImagingFactory_Release(fac);
	}
	return ok;
}

typedef struct
{
	PSID appc_sid;
	wchar_t worker_exe[MAX_PATH * 2];
	wchar_t worker_dir[MAX_PATH * 2];
	wchar_t canary_dll[MAX_PATH * 2];
	wchar_t canary_file[MAX_PATH * 2];
	unsigned net_port; /* loopback listener owned by the parent (network
	                      negative test) */
} Launcher;

typedef struct
{
	int started;
	DWORD create_error;
	DWORD exit_code;
	int timed_out;
	double ms;
	Lines out;
} Run;

static DWORD WINAPI drain_thread(LPVOID arg);

typedef struct
{
	HANDLE pipe;
	char *buf;
	size_t n, cap;
} Drain;

static DWORD WINAPI drain_thread(LPVOID arg)
{
	Drain *d = arg;
	char tmp[4096];
	DWORD got;

	while (ReadFile(d->pipe, tmp, sizeof tmp, &got, NULL) && got) {
		if (d->n + got + 1 > d->cap) {
			d->cap = (d->n + got + 1) * 2;
			d->buf = realloc(d->buf, d->cap);
		}
		memcpy(d->buf + d->n, tmp, got);
		d->n += got;
		d->buf[d->n] = '\0';
	}
	return 0;
}

#ifndef PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY
#	define PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY                     \
		ProcThreadAttributeValue(14, FALSE, TRUE, FALSE)
#endif

static void run_worker(const Launcher *L,
        const BYTE *image,
        size_t image_size,
        const wchar_t *suite,
        DWORD timeout_ms,
        Run *r)
{
	SECURITY_CAPABILITIES sc = {L->appc_sid, NULL, 0, 0};
	DWORD64 mitig = PROCESS_CREATION_MITIGATION_POLICY_DEP_ENABLE |
	        PROCESS_CREATION_MITIGATION_POLICY_FORCE_RELOCATE_IMAGES_ALWAYS_ON |
	        PROCESS_CREATION_MITIGATION_POLICY_BOTTOM_UP_ASLR_ALWAYS_ON |
	        PROCESS_CREATION_MITIGATION_POLICY_HIGH_ENTROPY_ASLR_ALWAYS_ON |
	        PROCESS_CREATION_MITIGATION_POLICY_STRICT_HANDLE_CHECKS_ALWAYS_ON |
	        PROCESS_CREATION_MITIGATION_POLICY_EXTENSION_POINT_DISABLE_ALWAYS_ON |
	        PROCESS_CREATION_MITIGATION_POLICY_PROHIBIT_DYNAMIC_CODE_ALWAYS_ON |
	        PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_REMOTE_ALWAYS_ON |
	        PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_LOW_LABEL_ALWAYS_ON |
	        PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_PREFER_SYSTEM32_ALWAYS_ON;
	DWORD child_policy = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED;
	SECURITY_ATTRIBUTES inh = {sizeof inh, NULL, TRUE};
	HANDLE img = INVALID_HANDLE_VALUE, in_r = NULL, in_w = NULL,
	       out_r = NULL, out_w = NULL, job = NULL;
	HANDLE handles[3];
	STARTUPINFOEXW si = {0};
	PROCESS_INFORMATION pi = {0};
	SIZE_T attr_sz = 0;
	wchar_t tmpimg[MAX_PATH * 2], tmprel[96] = L"", cmd[4096];
	Drain drain = {0};
	HANDLE th;
	LARGE_INTEGER t0, t1, fq;

	memset(r, 0, sizeof *r);
	QueryPerformanceFrequency(&fq);
	QueryPerformanceCounter(&t0);

	/* the image goes to disk only so the parent can hand over a read-only
	 * HANDLE; the worker never learns the name */
	{
		HANDLE w;
		DWORD wrote;

		swprintf(tmprel, 96, L"img\\%llu.bin",
		        (unsigned long long)t0.QuadPart);
		swprintf(tmpimg, MAX_PATH * 2, L"%ls\\%ls",
		        p0_area_root(g_area), tmprel);
		w = CreateFileW(tmpimg, GENERIC_WRITE, 0, NULL, CREATE_NEW,
		        FILE_ATTRIBUTE_NORMAL, NULL);
		if (INVALID_HANDLE_VALUE == w) {
			r->create_error = GetLastError();
			return;
		}
		p0_area_track(g_area, tmprel, 0);
		if (image_size) {
			WriteFile(w, image, (DWORD)image_size, &wrote, NULL);
		}
		CloseHandle(w);
		img = CreateFileW(tmpimg, GENERIC_READ, FILE_SHARE_READ, &inh,
		        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	}
	CreatePipe(&in_r, &in_w, &inh, 0);
	CreatePipe(&out_r, &out_w, &inh, 0);
	SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
	handles[0] = img;
	handles[1] = in_r;
	handles[2] = out_w;

	InitializeProcThreadAttributeList(NULL, 4, 0, &attr_sz);
	si.lpAttributeList = malloc(attr_sz);
	InitializeProcThreadAttributeList(si.lpAttributeList, 4, 0, &attr_sz);
	UpdateProcThreadAttribute(si.lpAttributeList, 0,
	        PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &sc, sizeof sc,
	        NULL, NULL);
	UpdateProcThreadAttribute(si.lpAttributeList, 0,
	        PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof handles,
	        NULL, NULL);
	UpdateProcThreadAttribute(si.lpAttributeList, 0,
	        PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, &mitig, sizeof mitig,
	        NULL, NULL);
	UpdateProcThreadAttribute(si.lpAttributeList, 0,
	        PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY, &child_policy,
	        sizeof child_policy, NULL, NULL);
	si.StartupInfo.cb = sizeof si;

	swprintf(cmd, 4096,
	        L"\"%ls\" --worker %llx %llx %llx \"%ls\" \"%ls\" %ls %u",
	        L->worker_exe, (unsigned long long)(ULONG_PTR)img,
	        (unsigned long long)(ULONG_PTR)in_r,
	        (unsigned long long)(ULONG_PTR)out_w, L->canary_file,
	        L->canary_dll, suite, L->net_port);

	if (!CreateProcessW(L->worker_exe, cmd, NULL, NULL, TRUE,
	            EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED |
	                    DETACHED_PROCESS,
	            NULL, L->worker_dir, &si.StartupInfo, &pi)) {
		r->create_error = GetLastError();
		goto cleanup;
	}
	r->started = 1;

	job = CreateJobObjectW(NULL, NULL);
	{
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {0};

		li.BasicLimitInformation.LimitFlags =
		        JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
		        JOB_OBJECT_LIMIT_JOB_MEMORY |
		        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		li.BasicLimitInformation.ActiveProcessLimit = 1;
		li.JobMemoryLimit = 512u << 20;
		SetInformationJobObject(
		        job, JobObjectExtendedLimitInformation, &li, sizeof li);
	}
	AssignProcessToJobObject(job, pi.hProcess);

	/* the parent must close its copies, or the pipe never reports EOF */
	CloseHandle(out_w);
	out_w = NULL;
	CloseHandle(in_r);
	in_r = NULL;
	drain.pipe = out_r;
	th = CreateThread(NULL, 0, drain_thread, &drain, 0, NULL);
	ResumeThread(pi.hThread);

	if (WAIT_OBJECT_0 != WaitForSingleObject(pi.hProcess, timeout_ms)) {
		r->timed_out = 1;
		TerminateJobObject(
		        job, 99); /* bounded: the design's wall-clock limit */
		WaitForSingleObject(pi.hProcess, 2000);
	}
	GetExitCodeProcess(pi.hProcess, &r->exit_code);
	WaitForSingleObject(th, 2000);
	CloseHandle(th);
	r->out.text = drain.buf ? drain.buf : _strdup("");
	r->out.n = drain.n;

cleanup:
	QueryPerformanceCounter(&t1);
	r->ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 /
	        (double)fq.QuadPart;
	if (pi.hProcess) {
		CloseHandle(pi.hProcess);
	}
	if (pi.hThread) {
		CloseHandle(pi.hThread);
	}
	if (job) {
		CloseHandle(job);
	}
	if (out_w) {
		CloseHandle(out_w);
	}
	if (in_r) {
		CloseHandle(in_r);
	}
	if (in_w) {
		CloseHandle(in_w);
	}
	if (out_r) {
		CloseHandle(out_r);
	}
	if (INVALID_HANDLE_VALUE != img) {
		CloseHandle(img);
	}
	if (tmprel[0] && !p0_area_remove(g_area, tmprel)) {
		fprintf(stderr, "  not removed: %ls (%s)\n", tmprel,
		        p0_area_last_refusal(g_area));
	}
	if (si.lpAttributeList) {
		DeleteProcThreadAttributeList(si.lpAttributeList);
		free(si.lpAttributeList);
	}
	if (!r->out.text) {
		r->out.text = _strdup("");
	}
}

static int write_results(const wchar_t *root,
        const char *test,
        const char *label,
        const char *const *hyp,
        const char *const *dec,
        const char *os_note,
        const char *matrix_csv,
        const wchar_t *work,
        wchar_t *dir_out,
        const char *forced_status)
{
	char ts[20], run_id[96], bid[64], exe_hash[P0_HASH_HEX];
	wchar_t dir[MAX_PATH * 2], p[MAX_PATH * 2], exe[MAX_PATH];
	FILE *f;
	size_t i;
	int fails = 0, passes = 0;
	char notes[640];

	dir_out[0] = 0;
	p0_utc_compact(ts);
	snprintf(run_id, sizeof run_id, "%s-%s-%lu-%llu-%s", ts, test,
	        GetCurrentProcessId(), (unsigned long long)GetTickCount64(),
	        label);
	swprintf(p, MAX_PATH * 2, L"%ls\\P0-%hs", root, test);
	if (!p0_mkdirs(p)) {
		return 0;
	}
	swprintf(dir, MAX_PATH * 2, L"%ls\\P0-%hs\\%hs", root, test, run_id);
	/* Evidence is append-only: an existing run directory is never reused.
	 */
	if (!CreateDirectoryW(dir, NULL)) {
		return 0;
	}
	{
		static const wchar_t *const sub[] = {
		        L"traces", L"artifacts", L"eventlog", L"screenshots"};

		for (i = 0; i < 4; i++) {
			swprintf(p, MAX_PATH * 2, L"%ls\\%ls", dir, sub[i]);
			if (!CreateDirectoryW(p, NULL)) {
				return 0;
			}
		}
	}
	swprintf(p, MAX_PATH * 2, L"%ls\\artifacts\\results.csv", dir);
	f = p0_fopen_w(p);
	if (!f) {
		return 0;
	}
	fputs("check,expected,observed,result\n", f);
	for (i = 0; i < g_nchecks && i < 256; i++) {
		const Check *c = &g_checks[i];
		char q[400];
		size_t k, m = 0;

		if (strcmp(c->test, test)) {
			continue;
		}
		for (k = 0; c->observed[k] && m < sizeof q - 2; k++) {
			if ('"' == c->observed[k]) {
				q[m++] = '"';
			}
			q[m++] = c->observed[k];
		}
		q[m] = '\0';
		fprintf(f, "%s,\"%s\",\"%s\",%s\n", c->name, c->expected, q,
		        c->pass ? "PASS" : "FAIL");
		if (c->pass) {
			passes++;
		} else {
			fails++;
		}
	}
	fclose(f);
	swprintf(p, MAX_PATH * 2, L"%ls\\artifacts\\matrix.csv", dir);
	f = p0_fopen_w(p);
	if (!f) {
		return 0;
	}
	fputs(matrix_csv, f);
	fclose(f);
	wcscpy_s(dir_out, MAX_PATH * 2, dir);

	swprintf(p, MAX_PATH * 2, L"%ls\\environment.json", dir);
	if (!p0_write_environment(p, work, "p0_image_sandbox", VERSION)) {
		return 0;
	}
	GetModuleFileNameW(NULL, exe, MAX_PATH);
	p0_sha256_file(exe, exe_hash);
	snprintf(bid, sizeof bid, "p0_image_sandbox-%s-%.12s", VERSION,
	        exe_hash);
	snprintf(notes, sizeof notes, "%d pass, %d fail. %s", passes, fails,
	        os_note);
	swprintf(p, MAX_PATH * 2, L"%ls\\run.json", dir);
	if (!p0_write_run_json(p,
	            (char[]){'P', '0', '-', test[0], test[1], test[2], '\0'},
	            run_id, bid, "NTFS", hyp,
	            forced_status   ? forced_status
	                    : fails ? "FAIL"
	                            : "PASS",
	            dec, notes)) {
		return 0;
	}
	printf("  evidence: %ls\n", dir);
	return 1;
}

/* These phases cannot earn PASS from A01..A05 or a pure parser test. Write
 * a record even if the runtime prerequisite fails before any worker exists. */
static int write_unfinished_phases(const wchar_t *results,
        const wchar_t *work,
        const char *os,
        HRESULT profile_hr)
{
	static const char *const hyp[] = {NULL};
	static const char *const dec[] = {"D-007", "D-026", NULL};
	static const struct
	{
		const char *id, *remaining;
	} phases[] = {{"A06",
	                      "Missing: resident worker scheduling, "
	                      "10/100/500ms/stall trials, 10-30 "
	                      "selections/sec, CPU/startup/AV measurements and "
	                      "stale-kill threshold."},
	        {"A07",
	                "Candidate parser tests are separate. Missing: sandbox "
	                "worker raster protocol, "
	                "malformed/truncated/oversized/duplicate/out-of-order "
	                "pipe responses, recycle and instrumented parent."},
	        {"A08",
	                "Missing: signed release/staged worker, dedicated SAC "
	                "audit/enforcement VMs, Defender state and "
	                "CodeIntegrity/Defender event logs."},
	        {"A09",
	                "Actual profile failure is recorded; missing: all "
	                "denial/start/mitigation/quarantine injections against "
	                "a runtime image service. Product images remain "
	                "disabled by A03."},
	        {"A10",
	                "Missing: image state ownership/purge integration, "
	                "unresolved/acknowledged recovery, running/stale "
	                "worker lifecycle. No user state or profile is purged "
	                "by this probe."}};
	const char *matrix =
	        "environment,status,note\n"
	        "Current Windows host,PREREQUISITE_ONLY,\"See environment.json "
	        "and profile_creation.txt; no A06-A10 gate coverage\"\n"
	        "Other Windows 11 builds,NOT_TESTED,\"No additional host or VM "
	        "available\"\n"
	        "Windows 11 ARM64,NOT_TESTED,\"No ARM64 host available\"\n"
	        "Windows 10 22H2,NOT_TESTED,\"No Windows 10 host or VM "
	        "available\"\n"
	        "Windows Server,NOT_TESTED,\"No host available; no support "
	        "claim\"\n"
	        "Downloads or OneDrive-backed launch,NOT_TESTED,\"Collector "
	        "launched from workspace build directory\"\n"
	        "Signed release and byte-identical staged "
	        "worker,NOT_TESTED,\"Development executable; signing matrix "
	        "unavailable\"\n"
	        "Defender enabled / SAC audit / SAC "
	        "enforcement,NOT_TESTED,\"Dedicated VM and event-log evidence "
	        "unavailable\"\n";
	size_t i;
	for (i = 0; i < WF_ARRAY_LEN(phases); i++) {
		char note[640];
		wchar_t dir[MAX_PATH * 2], raw[MAX_PATH * 2];
		FILE *f;
		snprintf(note, sizeof note,
		        "BLOCKED on %s. CreateAppContainerProfile "
		        "HRESULT=0x%08lx. "
		        "A03 prior FAIL unchanged; no weaker sandbox. %s",
		        os, (unsigned long)profile_hr, phases[i].remaining);
		if (!write_results(results, phases[i].id,
		            "prerequisite-blocked", hyp, dec, note, matrix,
		            work, dir, "BLOCKED")) {
			return 0;
		}
		swprintf(raw, MAX_PATH * 2,
		        L"%ls\\traces\\profile_creation.txt", dir);
		f = p0_fopen_w(raw);
		if (!f) {
			return 0;
		}
		fprintf(f,
		        "operation=CreateAppContainerProfile\nHRESULT=0x%"
		        "08lx\nworker_started=%s\n"
		        "os=%s\nweaker_sandbox_used=0\nA03=prior_FAIL_"
		        "unchanged\n",
		        (unsigned long)profile_hr,
		        FAILED(profile_hr) ? "0" : "see_A01_A05_records", os);
		fclose(f);
		swprintf(raw, MAX_PATH * 2, L"%ls\\artifacts\\remaining.txt",
		        dir);
		f = p0_fopen_w(raw);
		if (!f) {
			return 0;
		}
		fprintf(f, "%s\n%s\n", phases[i].remaining,
		        "All non-current environments remain NOT_TESTED. No "
		        "release gate is promoted by this record.");
		fclose(f);
	}
	return 1;
}

/* Everything the stages share. */
typedef struct
{
	Launcher L;
	Run run;
	wchar_t results[MAX_PATH * 2], work_base[MAX_PATH * 2], exe[MAX_PATH],
	        stage_dir[MAX_PATH * 2], canary_file[MAX_PATH * 2];
	char exe_hash[P0_HASH_HEX], os[256];
	HRESULT hr;
	int keep_profile;
} Lab;

static bool parse_arguments(int argc, wchar_t **argv, Lab *lab)
{
	int i;

	for (i = 1; i < argc; i++) {
		if (!wcscmp(argv[i], L"--results") && i + 1 < argc) {
			wcscpy_s(lab->results, MAX_PATH * 2, argv[++i]);
		} else if (!wcscmp(argv[i], L"--work-base") && i + 1 < argc) {
			wcscpy_s(lab->work_base, MAX_PATH * 2, argv[++i]);
		} else if (!wcscmp(argv[i], L"--keep-profile")) {
			lab->keep_profile = 1;
		}
	}
	if (!lab->results[0] || !lab->work_base[0]) {
		fputs("usage: p0_image_sandbox --results <root> --work-base "
		      "<existing dir> [--keep-profile]\n",
		        stderr);
		return false;
	}
	return true;
}

/* A01: the AppContainer test profile. Non-zero is the exit code when the
 * prerequisite is missing: nothing starts, and there is no weaker sandbox. */
static int lab_profile(Lab *lab)
{
	Launcher *L = &lab->L;
	HRESULT hr;

	printf("A01 AppContainer staged-worker launch\n");
	hr = CreateAppContainerProfile(PROFILE_NAME, L"wfile P0 PoC",
	        L"P0 sandbox proof of concept (test profile)", NULL, 0,
	        &L->appc_sid);
	if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
		hr = DeriveAppContainerSidFromAppContainerName(
		        PROFILE_NAME, &L->appc_sid);
	}
	{
		char *s = NULL;

		if (SUCCEEDED(hr)) {
			ConvertSidToStringSidA(L->appc_sid, &s);
		}
		check("A01", "appcontainer_profile_available",
		        "profile created or derived", SUCCEEDED(hr),
		        "hr=0x%08lx sid=%s", (unsigned long)hr, s ? s : "-");
		if (s) {
			LocalFree(s);
		}
	}
	lab->hr = hr;
	if (FAILED(hr)) {
		printf("AppContainer prerequisite blocked: HRESULT=0x%08lx; no "
		       "worker started; no weaker sandbox\n",
		        (unsigned long)hr);
		return write_unfinished_phases(
		               lab->results, lab->work_base, lab->os, hr)
		        ? 77
		        : 1;
	}
	return 0;
}

/* The run's scratch area, staging directory included. */
static int lab_scratch(Lab *lab)
{
	/* Everything this run creates (staged worker, canary, images,
	 * transcripts) lives in its own scratch area; only recorded objects are
	 * ever deleted. */
	{
		char area_id[96], ts[20];
		wchar_t rel[160];

		p0_utc_compact(ts);
		snprintf(area_id, sizeof area_id, "%s-sandbox", ts);
		g_area = p0_area_create(lab->work_base, area_id);
		if (!g_area || !p0_area_mkdir(g_area, L"stage") ||
		        !p0_area_mkdir(g_area, L"img")) {
			fputs("cannot create the scratch area under "
			      "--work-base\n",
			        stderr);
			return 1;
		}
		swprintf(rel, 160, L"stage\\%hs", lab->exe_hash);
		if (!p0_area_mkdir(g_area, rel)) {
			fputs("cannot create the staging directory\n", stderr);
			return 1;
		}
		swprintf(lab->stage_dir, MAX_PATH * 2, L"%ls\\%ls",
		        p0_area_root(g_area), rel);
	}
	return 0;
}

/* A01: staged copies of the worker and the canary DLL, their ACLs and hashes. */
static void lab_stage(Lab *lab)
{
	Launcher *L = &lab->L;
	char *sddl_before, *sddl_after;
	char canary_hash[P0_HASH_HEX];
	wchar_t canary_src[MAX_PATH * 2];

	sddl_before = sddl_of(lab->exe);
	check("A01", "stage_dir_acl_grants_all_application_packages",
	        "ACL set on the STAGING dir only",
	        grant_app_packages(lab->stage_dir), "dir=%ls", lab->stage_dir);
	{
		int staged = stage_file(lab->exe, lab->stage_dir, L"wfile.exe",
		        L->worker_exe, lab->exe_hash);
		char hs[P0_HASH_HEX] = "";

		if (staged) {
			p0_sha256_file(L->worker_exe, hs);
		}
		check("A01", "staged_exe_sha256_equals_source",
		        "staged == source",
		        staged && !strcmp(hs, lab->exe_hash),
		        "source=%.16s staged=%.16s", lab->exe_hash, hs);
	}
	{
		/* the canary DLL is the unsigned build artifact next to this
		 * lab->exe */
		wchar_t *slash;

		wcscpy_s(canary_src, MAX_PATH * 2, lab->exe);
		slash = wcsrchr(canary_src, L'\\');
		if (slash) {
			wcscpy_s(slash + 1, 32, L"p0_canary.dll");
		}
		p0_sha256_file(canary_src, canary_hash);
		check("A01", "canary_dll_staged", "unsigned canary present",
		        stage_file(canary_src, lab->stage_dir, L"p0_canary.dll",
		                L->canary_dll, canary_hash),
		        "%ls", L->canary_dll);
	}
	wcscpy_s(L->worker_dir, MAX_PATH * 2, lab->stage_dir);
	{
		wchar_t rel[160];

		swprintf(rel, 160, L"stage\\%hs\\wfile.exe", lab->exe_hash);
		p0_area_track(g_area, rel, 0);
		swprintf(rel, 160, L"stage\\%hs\\p0_canary.dll", lab->exe_hash);
		p0_area_track(g_area, rel, 0);
	}
	sddl_after = sddl_of(lab->exe);
	check("A01", "original_exe_acl_unchanged",
	        "DACL identical before/after",
	        sddl_before && sddl_after && !strcmp(sddl_before, sddl_after),
	        "%s",
	        sddl_before && sddl_after && !strcmp(sddl_before, sddl_after)
	                ? "identical"
	                : "DIFFERENT");
	{
		WINTRUST_FILE_INFO fi = {
		        .cbStruct = sizeof fi, .pcwszFilePath = L->worker_exe};
		WINTRUST_DATA wd = {.cbStruct = sizeof wd};
		GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
		LONG r;

		wd.dwUIChoice = WTD_UI_NONE;
		wd.dwUnionChoice = WTD_CHOICE_FILE;
		wd.pFile = &fi;
		wd.dwStateAction = WTD_STATEACTION_VERIFY;
		wd.fdwRevocationChecks = WTD_REVOKE_NONE;
		r = WinVerifyTrust(NULL, &action, &wd);
		wd.dwStateAction = WTD_STATEACTION_CLOSE;
		WinVerifyTrust(NULL, &action, &wd);
		/* informational: an unsigned development build is expected here
		 */
		check("A01", "authenticode_state_recorded",
		        "recorded (unsigned dev build expected)", 1,
		        "WinVerifyTrust=0x%08lx (%s)", (unsigned long)r,
		        0 == r ? "signed and trusted"
		                : (unsigned long)r == 0x800B0100 ? "NOT SIGNED"
		                                                 : "other");
	}
}

static void lab_canary(Lab *lab)
{
	Launcher *L = &lab->L;

	swprintf(lab->canary_file, MAX_PATH * 2, L"%ls\\canary.txt",
	        p0_area_root(g_area));
	{
		HANDLE c = CreateFileW(lab->canary_file, GENERIC_WRITE, 0, NULL,
		        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		DWORD wr;

		WriteFile(c, "canary", 6, &wr, NULL);
		CloseHandle(c);
		wcscpy_s(L->canary_file, MAX_PATH * 2, lab->canary_file);
		p0_area_track(g_area, L"canary.txt", 0);
	}
}

/* Loopback listener for the network negative test, with its positive control. */
static void lab_network(Lab *lab)
{
	Launcher *L = &lab->L;

	/* loopback listener for the network negative test, with a positive
	 * control */
	{
		WSADATA wsa;
		struct sockaddr_in la = {0};
		int ll = sizeof la;

		WSAStartup(MAKEWORD(2, 2), &wsa);
		g_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		la.sin_family = AF_INET;
		inet_pton(AF_INET, "127.0.0.1", &la.sin_addr);
		bind(g_listener, (struct sockaddr *)&la, sizeof la);
		listen(g_listener, 4);
		getsockname(g_listener, (struct sockaddr *)&la, &ll);
		L->net_port = ntohs(la.sin_port);
		{
			SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

			g_control_ok = connect(c, (struct sockaddr *)&la,
			                       sizeof la) == 0 &&
			        listener_pending();
			if (g_control_ok) {
				SOCKET a = accept(g_listener, NULL,
				        NULL); /* drain the control connection
					        */

				closesocket(a);
			}
			closesocket(c);
		}
	}
}

/* A01/A02/A04/A05 on one worker running the full suite on the PNG. */
static int lab_full_suite(Lab *lab)
{
	Launcher *L = &lab->L;
	Run *run = &lab->run;

	/* first worker: full suite on the PNG */
	printf("A02/A04/A05 full suite on one worker\n");
	{
		size_t k;
		const Sample *png = NULL;
		Lines *o;
		char refhash[P0_HASH_HEX] = "";

		for (k = 0; k < g_nsamples; k++) {
			if (!strcmp(g_samples[k].name, "png")) {
				png = &g_samples[k];
			}
		}
		if (!png) {
			puts("no png sample");
			return 1;
		}
		reference_hash(png->data, png->size, refhash, NULL, 0);
		run_worker(L, png->data, png->size, L"full", 5000, run);
		o = &run->out;
		{
			char hs[P0_HASH_HEX];
			P0Hash *ph = p0_hash_begin();

			p0_hash_update(ph, png->data, png->size);
			p0_hash_end(ph, hs);

			check("A01", "worker_started_in_appcontainer",
			        "worker runs, token is AppContainer",
			        run->started &&
			                line_is(o, "token.is_appcontainer",
			                        "1"),
			        "started=%d create_error=%lu "
			        "is_appcontainer=%s",
			        run->started, run->create_error,
			        line_value(o, "token.is_appcontainer")
			                ? line_value(o, "token.is_appcontainer")
			                : "?");
			{
				char *mysid = NULL;
				const char *theirs =
				        line_value(o, "token.package_sid");
				char theirs_copy[256] = "";

				if (theirs) {
					snprintf(theirs_copy,
					        sizeof theirs_copy, "%s",
					        theirs);
				}
				ConvertSidToStringSidA(L->appc_sid, &mysid);
				check("A01", "package_sid_matches_profile",
				        "worker package SID == profile SID",
				        mysid && !strcmp(theirs_copy, mysid),
				        "worker=%s", theirs_copy);
				if (mysid) {
					LocalFree(mysid);
				}
			}
			check("A01", "worker_integrity_level_recorded",
			        "recorded",
			        NULL != line_value(o, "token.integrity_rid"),
			        "rid=%s",
			        line_value(o, "token.integrity_rid")
			                ? line_value(o, "token.integrity_rid")
			                : "?");
			check("A01", "worker_exit_code_zero",
			        "exit 0 within the limit",
			        !run->timed_out && 0 == run->exit_code,
			        "exit=%lu timed_out=%d ms=%.0f", run->exit_code,
			        run->timed_out, run->ms);

			check("A02", "inherited_image_handle_readable",
			        "worker reads the exact bytes",
			        line_starts(o, "image.read", "OK") &&
			                strstr(line_value(o, "image.read")
			                                ? line_value(o,
			                                          "image.read")
			                                : "",
			                        hs),
			        "%s",
			        line_value(o, "image.read")
			                ? line_value(o, "image.read")
			                : "(none)");
			check("A02", "arbitrary_user_path_denied",
			        "CreateFile on a user-readable file is denied",
			        line_starts(
			                o, "neg.arbitrary_path_open", "DENIED"),
			        "%s",
			        line_value(o, "neg.arbitrary_path_open")
			                ? line_value(
			                          o, "neg.arbitrary_path_open")
			                : "(none)");

			check("A03", "png_decode_in_sandbox_matches_reference",
			        "pixel hash == unsandboxed decode",
			        line_starts(o, "image.decode", "OK") &&
			                strstr(line_value(o, "image.decode")
			                                ? line_value(o,
			                                          "image."
			                                          "decode")
			                                : "",
			                        refhash),
			        "%s",
			        line_value(o, "image.decode")
			                ? line_value(o, "image.decode")
			                : "(none)");
		}

		{
			static const struct
			{
				const char *key, *label;
			} want[] = {
			        {"after.dep", "DEP"},
			        {"after.aslr_bottom_up", "ASLR bottom-up"},
			        {"after.aslr_force_relocate",
			                "ASLR force relocate"},
			        {"after.aslr_high_entropy",
			                "ASLR high entropy"},
			        {"after.dynamic_code_prohibited",
			                "dynamic code prohibited"},
			        {"after.extension_points_disabled",
			                "extension points disabled"},
			        {"after.no_child_process",
			                "child process restricted"},
			        {"after.image_no_remote",
			                "image load: no remote"},
			        {"after.image_no_low_label",
			                "image load: no low label"},
			        {"after.image_prefer_system32",
			                "image load: prefer System32"},
			        {"after.signature_microsoft_only",
			                "signature: Microsoft-signed only"},
			        {"after.strict_handle_checks",
			                "strict handle checks"},
			};
			size_t w;

			for (w = 0; w < WF_ARRAY_LEN(want); w++) {
				check("A04", want[w].label,
				        "policy reports active",
				        line_is(o, want[w].key, "1"), "%s=%s",
				        want[w].key,
				        line_value(o, want[w].key)
				                ? line_value(o, want[w].key)
				                : "?");
			}
			check("A04",
			        "signature_policy_set_after_start_and_verified",
			        "set OK and read back",
			        line_is(o, "signature_policy.verified", "1"),
			        "set=%s verified=%s",
			        line_value(o, "signature_policy.set")
			                ? line_value(o, "signature_policy.set")
			                : "?",
			        line_value(o, "signature_policy.verified")
			                ? line_value(o,
			                          "signature_policy.verified")
			                : "?");
		}

		{
			/* control: the same file is readable by the parent */
			HANDLE c = CreateFileW(L->canary_file, GENERIC_READ,
			        FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
			int parent_ok = INVALID_HANDLE_VALUE != c;

			if (parent_ok) {
				CloseHandle(c);
			}
			check("A05", "arbitrary_user_path_createfile_denied",
			        "denied to the worker, readable by the parent",
			        line_starts(o, "neg.arbitrary_path_open",
			                "DENIED") &&
			                parent_ok,
			        "%s; parent_can_open=%d",
			        line_value(o, "neg.arbitrary_path_open")
			                ? line_value(
			                          o, "neg.arbitrary_path_open")
			                : "(none)",
			        parent_ok);
		}
		{
			int arrived = listener_pending();

			check("A05", "network_connection_denied",
			        "worker cannot reach a loopback listener the "
			        "parent can",
			        line_starts(
			                o, "neg.network_connect", "DENIED") &&
			                g_control_ok && !arrived,
			        "%s; parent_control_connect=%d; "
			        "connection_arrived=%d",
			        line_value(o, "neg.network_connect")
			                ? line_value(o, "neg.network_connect")
			                : "(none)",
			        g_control_ok, arrived);
		}
		check("A05", "child_process_creation_denied", "denied",
		        line_starts(o, "neg.child_process", "DENIED"), "%s",
		        line_value(o, "neg.child_process")
		                ? line_value(o, "neg.child_process")
		                : "(none)");
		check("A05", "unsigned_dll_loadable_BEFORE_policy_control",
		        "control: loads",
		        line_starts(o, "dll.unsigned_before_signature_policy",
		                "LOADED") ||
		                line_starts(o,
		                        "dll.unsigned_before_signature_policy",
		                        "BLOCKED"),
		        "%s (informational control: staged dir readable by "
		        "AppContainer)",
		        line_value(o, "dll.unsigned_before_signature_policy")
		                ? line_value(o,
		                          "dll.unsigned_before_signature_"
		                          "policy")
		                : "(none)");
		check("A05", "unsigned_dll_load_blocked_AFTER_policy",
		        "load refused",
		        line_starts(o, "dll.unsigned_after_signature_policy",
		                "BLOCKED"),
		        "%s",
		        line_value(o, "dll.unsigned_after_signature_policy")
		                ? line_value(o,
		                          "dll.unsigned_after_signature_policy")
		                : "(none)");
		check("A05", "inherited_input_handle_readable", "readable",
		        line_starts(o, "image.read", "OK"), "%s",
		        line_value(o, "image.read")
		                ? line_value(o, "image.read")
		                : "(none)");
		check("A05", "anonymous_ipc_handles_usable",
		        "worker output reached the parent",
		        line_is(o, "done", "1"), "lines=%zu", o->n);
		{
			wchar_t t[MAX_PATH * 2];
			FILE *tf;

			swprintf(t, MAX_PATH * 2, L"%ls\\_worker_full.txt",
			        p0_area_root(g_area));
			tf = p0_fopen_w(t);
			if (tf) {
				fputs(o->text, tf);
				fclose(tf);
				p0_area_track(g_area, L"_worker_full.txt", 0);
			}
		}
		free(o->text);
	}
	return 0;
}

/* A03: every format and the malformed corpus, one sandboxed worker each. */
static void lab_corpus(Lab *lab)
{
	Launcher *L = &lab->L;
	Run *run = &lab->run;

	/* A03: every format + the malformed corpus, one worker each */
	printf("A03 codec corpus (one sandboxed worker per sample)\n");
	{
		size_t k;
		FILE *tr;
		wchar_t t[MAX_PATH * 2];

		swprintf(t, MAX_PATH * 2, L"%ls\\_a03_samples.csv",
		        p0_area_root(g_area));
		tr = p0_fopen_w(t);
		fputs("sample,valid,bytes,worker_exit,timed_out,ms,termination,"
		      "decode_status,width,height,pixels,"
		      "nonzero_pixels,rows_compared,rows_equal_to_valid,"
		      "decoder_reported_failure,decode_line\n",
		        tr);
		p0_area_track(g_area, L"_a03_samples.csv", 0);
		for (k = 0; k < g_nsamples; k++) {
			const Sample *s = &g_samples[k];
			char ref[P0_HASH_HEX] = "", ref_rows[400] = "",
			     base_rows[400] = "";
			int have_ref = reference_hash(s->data, s->size, ref,
			        ref_rows, sizeof ref_rows);
			const char *dl, *rows_line, *px_line;
			char name[96], term[48], status[16] = "FAIL";
			unsigned w = 0, h = 0, pixels = 0, nonzero = 0;
			int compared = 0, equal = 0;

			run_worker(L, s->data, s->size, L"decode", 5000, run);
			dl = line_value(&run->out, "image.decode");
			if (dl && !strncmp(dl, "OK", 2)) {
				snprintf(status, sizeof status, "OK");
				sscanf_s(strstr(dl, "w=") ? strstr(dl, "w=")
				                          : "w=0",
				        "w=%u", &w);
				sscanf_s(strstr(dl, "h=") ? strstr(dl, "h=")
				                          : "h=0",
				        "h=%u", &h);
			}
			/* the same family's VALID image is the yardstick for
			 * "how much of it is right" */
			{
				const char *fam = !strncmp(s->name, "png_", 4)
				        ? "png"
				        : !strncmp(s->name, "jpeg_", 5) ? "jpeg"
				                                        : NULL;
				size_t q;
				char tmp[P0_HASH_HEX];

				for (q = 0; fam && q < g_nsamples; q++) {
					if (!strcmp(g_samples[q].name, fam)) {
						reference_hash(
						        g_samples[q].data,
						        g_samples[q].size, tmp,
						        base_rows,
						        sizeof base_rows);
					}
				}
			}
			px_line = line_value(&run->out, "image.pixels");
			if (px_line) {
				sscanf_s(px_line, "%u nonzero_pixels=%u",
				        &pixels, &nonzero);
			}
			rows_line = line_value(&run->out, "image.rows");
			if (rows_line && base_rows[0]) {
				size_t r;

				for (r = 0; r + 8 <= strlen(rows_line) &&
				        r + 8 <= strlen(base_rows);
				        r += 8) {
					compared++;
					equal += !strncmp(rows_line + r,
					        base_rows + r, 8);
				}
			}
			snprintf(term, sizeof term, "%s",
			        run->timed_out ? "killed_by_job_timeout"
			                : 0 == run->exit_code
			                ? "exited_normally"
			                : "abnormal_exit");
			fprintf(tr,
			        "%s,%d,%zu,0x%lx,%d,%.0f,%s,%s,%u,%u,%u,%u,%d,%"
			        "d,%d,\"%s\"\n",
			        s->name, s->valid, s->size, run->exit_code,
			        run->timed_out, run->ms, term, status, w, h,
			        pixels, nonzero, compared, equal,
			        0 != strcmp(status, "OK"), dl ? dl : "");
			snprintf(name, sizeof name, "decode_%s", s->name);
			if (s->valid) {
				check("A03", name,
				        "decodes; pixel hash == reference",
				        line_starts(&run->out, "image.decode",
				                "OK") &&
				                have_ref &&
				                strstr(dl ? dl : "", ref) &&
				                0 == run->exit_code,
				        "%s ms=%.0f", dl ? dl : "(none)",
				        run->ms);
			} else {
				/* Strict reading of the plan's "bounded
				 * failure": the decoder must REFUSE the input.
				 * Bounded alone (no crash, no hang, no limit
				 * breach) is checked in the same row's observed
				 * text and in corpus_samples.csv. */
				check("A03", name,
				        "bounded failure: the decoder refuses "
				        "it (no crash, no hang)",
				        !line_starts(&run->out, "image.decode",
				                "OK") &&
				                0 == run->exit_code &&
				                !run->timed_out &&
				                line_is(&run->out, "done", "1"),
				        "%s | exit=0x%lx %s ms=%.0f | decoded "
				        "%ux%u = %u px, %u non-zero, rows "
				        "equal to the valid image: %d/%d",
				        dl ? dl : "(none)", run->exit_code,
				        term, run->ms, w, h, pixels, nonzero,
				        equal, compared);
			}
			free(run->out.text);
		}
		fclose(tr);
	}
}

/* The per-test evidence directories. */
static int lab_evidence(Lab *lab)
{

	{
		static const char *const hyp_a01[] = {"H-014", NULL};
		static const char *const hyp_a02[] = {"H-015", "H-018", NULL};
		static const char *const hyp_a03[] = {"H-016", NULL};
		static const char *const hyp_a04[] = {"H-017", NULL};
		static const char *const hyp_a05[] = {
		        "H-018", "H-019", "H-020", NULL};
		static const char *const dec[] = {"D-007", "D-026", NULL};
		char matrix[1024];
		wchar_t work[MAX_PATH * 2];
		char note[400];

		snprintf(matrix, sizeof matrix,
		        "environment,status,note\n"
		        "Windows 11 (this machine),TESTED,\"unsigned "
		        "development build; Defender state not recorded by the "
		        "harness\"\n"
		        "Windows 10 22H2,NOT_TESTED,\"no machine/VM "
		        "available\"\n"
		        "executable launched from Downloads,NOT_TESTED,\"this "
		        "run staged from the build directory\"\n"
		        "OneDrive-backed Downloads/Desktop,NOT_TESTED,\"not "
		        "exercised\"\n"
		        "signed candidate build,NOT_TESTED,\"no signing "
		        "certificate in the lab\"\n"
		        "Smart App Control enforcement,NOT_TESTED,\"see "
		        "P0-A08; not set up\"\n");
		snprintf(note, sizeof note,
		        "PoC on %s; single environment; matrix.csv lists what "
		        "was NOT tested. "
		        "Not covered: A06 A07 A08 A09 A10.",
		        lab->os);
		wcscpy_s(work, MAX_PATH * 2, lab->stage_dir);
		printf("\nwriting evidence\n");
		static const struct
		{
			const char *id;
			const char *const *hyp;
		} runs[] = {{"A01", hyp_a01}, {"A02", hyp_a02},
		        {"A03", hyp_a03}, {"A04", hyp_a04}, {"A05", hyp_a05}};
		wchar_t full_txt[MAX_PATH * 2], samples_csv[MAX_PATH * 2];
		size_t q;

		swprintf(full_txt, MAX_PATH * 2, L"%ls\\_worker_full.txt",
		        p0_area_root(g_area));
		swprintf(samples_csv, MAX_PATH * 2, L"%ls\\_a03_samples.csv",
		        p0_area_root(g_area));
		for (q = 0; q < WF_ARRAY_LEN(runs); q++) {
			wchar_t dir[MAX_PATH * 2], dst[MAX_PATH * 2];

			if (!write_results(lab->results, runs[q].id,
			            "win11-appcontainer-poc", runs[q].hyp, dec,
			            note, matrix, work, dir, NULL)) {
				return 1;
			}
			swprintf(dst, MAX_PATH * 2,
			        L"%ls\\traces\\worker_full_suite.txt", dir);
			CopyFileW(full_txt, dst, FALSE);
			if (!strcmp(runs[q].id, "A03")) {
				swprintf(dst, MAX_PATH * 2,
				        L"%ls\\traces\\corpus_samples.csv",
				        dir);
				CopyFileW(samples_csv, dst, FALSE);
			}
		}
		(void)full_txt;
		(void)samples_csv;
	}
	return 0;
}

/* Scratch-area removal, the test profile, and the verdict. */
static int lab_finish(Lab *lab)
{
	wchar_t cleanup_report[1024] = L"";
	int leftovers = 0;

	/* The run's scratch area: only what was recorded goes; anything else is
	 * reported and the area stays provable. Then the test profile, unless
	 * asked. */
	leftovers = p0_area_destroy(g_area, cleanup_report, 1024);
	g_area = NULL;
	printf("scratch area cleanup: %d leftover(s)%ls%ls\n", leftovers,
	        leftovers ? L": " : L"", cleanup_report);
	if (!lab->keep_profile) {
		DeleteAppContainerProfile(PROFILE_NAME);
	}
	{
		size_t n = 0, bad = 0;

		for (n = 0; n < g_nchecks; n++) {
			bad += !g_checks[n].pass;
		}
		printf("\n%zu checks, %zu failed\n", g_nchecks, bad);
		return bad || leftovers ? 1 : 0;
	}
}

int wmain(int argc, wchar_t **argv)
{
	Lab lab;
	int status;

	memset(&lab, 0, sizeof lab);
	if (argc >= 2 && !wcscmp(argv[1], L"--worker")) {
		return worker_main(argc, argv);
	}
	if (!parse_arguments(argc, argv, &lab)) {
		return 2;
	}
	CoInitializeEx(NULL, COINIT_MULTITHREADED);
	p0_os_summary(lab.os, sizeof lab.os);
	GetModuleFileNameW(NULL, lab.exe, MAX_PATH);
	p0_sha256_file(lab.exe, lab.exe_hash);

	if (0 != (status = lab_profile(&lab))) {
		return status;
	}
	if (0 != (status = lab_scratch(&lab))) {
		return status;
	}
	lab_stage(&lab);
	lab_canary(&lab);
	build_samples();
	lab_network(&lab);
	if (0 != (status = lab_full_suite(&lab))) {
		return status;
	}
	lab_corpus(&lab);
	if (0 != (status = lab_evidence(&lab))) {
		return status;
	}
	if (!write_unfinished_phases(
	            lab.results, lab.work_base, lab.os, lab.hr)) {
		return 1;
	}
	return lab_finish(&lab);
}
