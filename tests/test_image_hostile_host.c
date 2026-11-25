#include <windows.h>
#include <tlhelp32.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "preview/wf_dechost_client_win.h"
#include "wf_imgfix.h"
#include "wf_test.h"

/*
 * Hostile pictures against the real isolated host (DR-DRAFT-039): the answer is
 * a refusal in bounded time, the host survives it or is replaced, the memory
 * wall of the job holds, and starting and stopping hosts leaves nothing behind
 * in the application (handles, threads). AppContainer where the machine has one
 * (reported as SKIP when it does not).
 */

#define SKIP_CODE 77
#define TIMEOUT_MS 12000u

static wchar_t g_dir[MAX_PATH];
static wchar_t g_host_exe[MAX_PATH * 2], g_staged[MAX_PATH * 2];
static int g_sandbox;

static HANDLE fixture(const wchar_t *name, const void *p, size_t n)
{
	wchar_t path[MAX_PATH * 2];
	HANDLE h;
	DWORD put;
	const uint8_t *q = p;

	swprintf_s(path, MAX_PATH * 2, L"%ls\\%ls", g_dir, name);
	h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
	        NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	while (n) {
		DWORD k = n > (1u << 28) ? (1u << 28) : (DWORD)n;

		CHECK(WriteFile(h, q, k, &put, NULL) && put == k);
		q += k;
		n -= k;
	}
	SetFilePointer(h, 0, NULL, FILE_BEGIN);
	return h;
}

static WfDecHost *start(uint32_t memory_mib)
{
	WfDecHostOptions opt;
	WfDecHost *h = NULL;
	DWORD err = 0;

	memset(&opt, 0, sizeof opt);
	opt.exe = g_sandbox ? g_staged : g_host_exe;
	opt.sandbox = g_sandbox;
	opt.memory_mib = memory_mib;
	CHECK(WF_OK == wf_dechost_start(&opt, &h, &err));
	return h;
}

static WfDecodeStatus decode_file(
        WfDecHost *h, HANDLE f, ULONGLONG *ms, WfPixmap **pm)
{
	WfDecodeInfo info;
	ULONGLONG t0 = GetTickCount64();
	WfDecodeStatus st = wf_dechost_decode(h, WF_DKIND_IMAGE, f, 160, 90, 0,
	        NULL, TIMEOUT_MS, NULL, 0, pm, &info);

	*ms = GetTickCount64() - t0;
	return st;
}

/* A good picture the host must still be able to show. */
static void the_host_still_works(WfDecHost *h)
{
	size_t n;
	uint8_t *p = fix_png_zeros(320, 180, 2, 0, &n);
	HANDLE f = fixture(L"good.png", p, n);
	WfPixmap *pm = NULL;
	ULONGLONG ms;

	CHECK(WF_DSTAT_OK == decode_file(h, f, &ms, &pm));
	CHECK(pm && 160 == pm->w && 90 == pm->h);
	wf_pixmap_free(pm);
	CloseHandle(f);
	free(p);
}

static void hostile_files_are_refused_and_the_host_goes_on(void)
{
	static const struct
	{
		const char *what;
		uint32_t w, h;
		int color, il;
	} c[] = {
	        {"png 16384 square", 16384, 16384, 0, 0},
	        {"png 65535 square", 65535, 65535, 2, 0},
	        {"png 2^31-1 square", 0x7FFFFFFFu, 0x7FFFFFFFu, 0, 0},
	        {"png 2^32-1 square", 0xFFFFFFFFu, 0xFFFFFFFFu, 2, 1},
	        {"png 0 x 0", 0, 0, 2, 0},
	        {"png interlaced 30000 square", 30000, 30000, 2, 1},
	};
	WfDecHost *h = start(0);

	for (size_t i = 0; h && i < sizeof c / sizeof *c; i++) {
		size_t n;
		uint8_t *p =
		        fix_png_zeros(c[i].w, c[i].h, c[i].color, c[i].il, &n);
		HANDLE f = fixture(L"hostile.png", p, n);
		WfPixmap *pm = NULL;
		ULONGLONG ms;
		WfDecodeStatus st = decode_file(h, f, &ms, &pm);

		if (WF_DSTAT_CORRUPT != st && WF_DSTAT_UNSUPPORTED != st &&
		        WF_DSTAT_TOO_LARGE != st) {
			printf("%s: status %d after %llu ms\n", c[i].what,
			        (int)st, (unsigned long long)ms);
			CHECK_FAIL("a hostile picture did not get a plain "
			           "refusal");
		}
		CHECK(NULL == pm);
		CHECK(ms < TIMEOUT_MS / 2);
		CHECK(wf_dechost_alive(
		        h)); /* a refusal is an answer, not a death */
		CloseHandle(f);
		free(p);
	}
	the_host_still_works(h);
	wf_dechost_stop(h);
}

/* A file bigger than the host takes is refused as a size, without reading it. */
static void a_file_over_the_byte_limit_is_refused_unread(void)
{
	wchar_t path[MAX_PATH * 2];
	HANDLE f;
	LARGE_INTEGER sz;
	WfDecHost *h = start(0);
	WfPixmap *pm = NULL;
	ULONGLONG ms;

	swprintf_s(path, MAX_PATH * 2, L"%ls\\big.bin", g_dir);
	f = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
	        NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(INVALID_HANDLE_VALUE != f);
	sz.QuadPart = (LONGLONG)WF_DWIRE_INPUT_MAX +
	        4096; /* a hole: no bytes written */
	if (SetFilePointerEx(f, sz, NULL, FILE_BEGIN) && SetEndOfFile(f)) {
		SetFilePointer(f, 0, NULL, FILE_BEGIN);
		CHECK(WF_DSTAT_TOO_LARGE == decode_file(h, f, &ms, &pm));
		CHECK(NULL == pm && ms < 2000);
	} else {
		CHECK_FAIL("could not make a file over the limit");
	}
	CloseHandle(f);
	the_host_still_works(h);
	wf_dechost_stop(h);
}

/* The memory wall: a host held to far less than the picture needs fails with a
 * status, takes nothing else down, and the next host is fine. */
static void a_host_over_its_memory_limit_is_stopped_and_replaced(void)
{
	size_t n;
	uint8_t *p =
	        fix_png_zeros(15360, 8640, 2, 1, &n); /* ~195 MiB to decode */
	HANDLE f = fixture(L"heavy.png", p, n);
	WfDecHost *h = start(40);
	WfPixmap *pm = NULL;
	ULONGLONG ms;
	WfDecodeStatus st = decode_file(h, f, &ms, &pm);

	CHECK(WF_DSTAT_OK != st && NULL == pm);
	CHECK(ms < TIMEOUT_MS);
	wf_pixmap_free(pm);
	wf_dechost_stop(h);
	h = start(0); /* the default wall: the same picture is shown */
	pm = NULL;
	CHECK(WF_DSTAT_OK == decode_file(h, f, &ms, &pm));
	CHECK(pm && pm->w <= 160 && pm->h <= 90);
	wf_pixmap_free(pm);
	wf_dechost_stop(h);
	CloseHandle(f);
	free(p);
}

static unsigned threads_of_this_process(void)
{
	HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	THREADENTRY32 t;
	unsigned n = 0;

	memset(&t, 0, sizeof t);
	t.dwSize = sizeof t;
	if (INVALID_HANDLE_VALUE == s) {
		return 0;
	}
	for (BOOL ok = Thread32First(s, &t); ok; ok = Thread32Next(s, &t)) {
		n += t.th32OwnerProcessID == GetCurrentProcessId();
	}
	CloseHandle(s);
	return n;
}

static void starting_and_stopping_hosts_leaves_no_handle_or_thread(void)
{
	DWORD h0, h1;
	unsigned t0, t1;
	size_t n;
	uint8_t *p = fix_png_zeros(640, 360, 2, 0, &n);
	HANDLE f = fixture(L"cycle.png", p, n);

	/* one warm-up: the first start may open things that stay (the staged copy) */
	{
		WfDecHost *h = start(0);
		WfPixmap *pm = NULL;
		ULONGLONG ms;

		(void)decode_file(h, f, &ms, &pm);
		wf_pixmap_free(pm);
		wf_dechost_stop(h);
	}
	GetProcessHandleCount(GetCurrentProcess(), &h0);
	t0 = threads_of_this_process();
	for (int i = 0; i < 40; i++) {
		WfDecHost *h = start(0);
		WfPixmap *pm = NULL;
		ULONGLONG ms;

		CHECK(WF_DSTAT_OK == decode_file(h, f, &ms, &pm));
		wf_pixmap_free(pm);
		if (i % 4 == 3) { /* and a host that is killed mid-way */
			HANDLE heavy_abandon =
			        CreateEventW(NULL, TRUE, TRUE, NULL);
			WfDecodeInfo info;

			(void)wf_dechost_decode(h, WF_DKIND_IMAGE, f, 160, 90,
			        0, NULL, TIMEOUT_MS, heavy_abandon, 0, &pm,
			        &info);
			wf_pixmap_free(pm);
			CloseHandle(heavy_abandon);
		}
		wf_dechost_stop(h);
	}
	GetProcessHandleCount(GetCurrentProcess(), &h1);
	t1 = threads_of_this_process();
	printf("handles %lu -> %lu, threads %u -> %u after 40 host lifetimes\n",
	        (unsigned long)h0, (unsigned long)h1, t0, t1);
	CHECK(h1 <= h0 + 2);
	CHECK(t1 <= t0 + 1);
	CloseHandle(f);
	free(p);
}

int main(void)
{
	wchar_t tmp[MAX_PATH], *slash;
	DWORD err = 0;
	WfDecHostOptions opt;
	WfDecHost *probe = NULL;

	setvbuf(stdout, NULL, _IONBF, 0);
	GetModuleFileNameW(NULL, g_host_exe, MAX_PATH * 2);
	slash = wcsrchr(g_host_exe, L'\\');
	wcscpy_s(slash + 1, 32, L"wf_dechost.exe");
	GetTempPathW(MAX_PATH, tmp);
	swprintf_s(g_dir, MAX_PATH, L"%lspiero_hostile_%lu", tmp,
	        GetCurrentProcessId());
	CreateDirectoryW(g_dir, NULL);
	if (WF_OK ==
	        wf_dechost_stage(g_host_exe, g_staged, MAX_PATH * 2, &err)) {
		memset(&opt, 0, sizeof opt);
		opt.exe = g_staged;
		opt.sandbox = 1;
		if (WF_OK == wf_dechost_start(&opt, &probe, &err)) {
			g_sandbox = 1;
			wf_dechost_stop(probe);
		}
	}
	if (!g_sandbox) {
		puts("NOTE: no AppContainer here; the host runs as an ordinary "
		     "process");
	}
	hostile_files_are_refused_and_the_host_goes_on();
	a_file_over_the_byte_limit_is_refused_unread();
	a_host_over_its_memory_limit_is_stopped_and_replaced();
	starting_and_stopping_hosts_leaves_no_handle_or_thread();
	{
		wchar_t p[MAX_PATH * 2];
		const wchar_t *names[] = {L"hostile.png", L"big.bin",
		        L"heavy.png", L"cycle.png", L"good.png"};

		for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
			swprintf_s(
			        p, MAX_PATH * 2, L"%ls\\%ls", g_dir, names[i]);
			DeleteFileW(p);
		}
		RemoveDirectoryW(g_dir);
	}
	if (wf_test_failures) {
		return 1;
	}
	if (!g_sandbox) {
		puts("ok (AppContainer part SKIPPED)");
		return SKIP_CODE;
	}
	puts("ok");
	return 0;
}
