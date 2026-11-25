/* Reproducible CPU-side measurements, not a terminal paint-time claim.
 * Creates only a NEW results directory. Optional enumeration is read-only. */
#include <windows.h>
#include <psapi.h>
#include <winternl.h>
#include <fcntl.h>
#include <io.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "ui/wf_app_priv.h"
#include "fs/wf_enum_win.h"
#include "render/wf_present.h"
#include "render/wf_sixel.h"

#define MAX_SAMPLES 501u
#define WARMUP 3u

typedef struct
{
	const char *name;
	size_t items;
	double us[MAX_SAMPLES];
	size_t bytes;
} Measurement;

static Measurement results[24];
static size_t used;
static unsigned samples = 31;
static LARGE_INTEGER frequency;
static WfApp app;
static WfFrame *frame, *changed;
static WfPixmap *picture, *small;
static WfBuf output;
static size_t entry_count;
static const wchar_t *enumeration;
static size_t enumerated_count;

static double now_us(void)
{
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return (double)t.QuadPart * 1000000.0 / (double)frequency.QuadPart;
}

static int compare_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

static double percentile(const double *sorted, size_t n, unsigned p)
{
	size_t rank = (n * p + 99u) / 100u;
	return sorted[rank ? rank - 1 : 0];
}

static WfDir *directory(size_t n)
{
	WfDir *d = wf_dir_new();
	if (!d) {
		return NULL;
	}
	for (size_t i = 0; i < n; i++) {
		wchar_t name[48];
		swprintf(name, 48, L"file%06zu.txt", n - i);
		if (wf_dir_add(d, name, wcslen(name), 0, (uint64_t)i * 17u,
		            (uint64_t)i) != WF_OK) {
			wf_dir_free(d);
			return NULL;
		}
	}
	return d;
}

typedef WfStatus (*Operation)(double *time, size_t *bytes);

static WfStatus build_directory(double *time, size_t *bytes)
{
	double start = now_us();
	WfDir *d = directory(entry_count);
	*time = now_us() - start;
	if (!d) {
		return WF_E_NOMEM;
	}
	*bytes = 0;
	wf_dir_free(d);
	return WF_OK;
}

static WfStatus sort_directory(double *time, size_t *bytes)
{
	WfDir *d = directory(entry_count); /* fixture setup is not timed */
	if (!d) {
		return WF_E_NOMEM;
	}
	double start = now_us();
	WfStatus st = wf_dir_sort(d, WF_SORT_NAME, false);
	*time = now_us() - start;
	*bytes = 0;
	wf_dir_free(d);
	return st;
}

static WfStatus sort_again(double *time, size_t *bytes)
{
	WfDir *d = directory(entry_count);
	if (!d) {
		return WF_E_NOMEM;
	}
	WfStatus st = wf_dir_sort(d, WF_SORT_NAME, false);
	if (st == WF_OK) {
		double start = now_us();
		st = wf_dir_sort(d, WF_SORT_NAME, false);
		*time = now_us() - start;
	}
	*bytes = 0;
	wf_dir_free(d);
	return st;
}

static WfStatus enumerate_directory(double *time, size_t *bytes)
{
	WfDir *d = NULL;
	double start = now_us();
	WfStatus st = wf_enum_dir_win(enumeration, &d);
	*time = now_us() - start;
	if (st == WF_OK && (!d || d->truncated)) {
		st = WF_E_RANGE;
	}
	if (st == WF_OK) {
		enumerated_count = d->n;
	}
	*bytes = 0;
	wf_dir_free(d);
	return st;
}

static WfStatus draw(double *time, size_t *bytes)
{
	double start = now_us();
	wf_app_draw(&app, frame);
	*time = now_us() - start;
	*bytes = 0;
	return WF_OK;
}

static WfStatus input_frame(double *time, size_t *bytes)
{
	WfEvent down = {.type = WF_EV_KEY, .key = WF_KEY_DOWN};
	WfEvent up = {.type = WF_EV_KEY, .key = WF_KEY_UP};
	double start = now_us();
	(void)app.mode->on_event(&app, &down);
	wf_app_draw(&app, changed);
	(void)app.mode->on_event(&app, &up);
	wf_app_draw(&app, frame);
	*time = (now_us() - start) / 2.0;
	*bytes = 0;
	return WF_OK;
}

static WfStatus present(const WfFrame *previous, double *time, size_t *bytes)
{
	wf_buf_clear(&output);
	double start = now_us();
	WfStatus st = wf_present_diff(previous, frame, &output);
	*time = now_us() - start;
	*bytes = output.len;
	return st;
}
static WfStatus full(double *t, size_t *b)
{
	return present(NULL, t, b);
}
static WfStatus idle(double *t, size_t *b)
{
	return present(frame, t, b);
}
static WfStatus row(double *t, size_t *b)
{
	return present(changed, t, b);
}

static WfStatus scale(double *time, size_t *bytes)
{
	WfPixmap *p = NULL;
	double start = now_us();
	WfStatus st = wf_pixmap_scale(picture, 320, 180, &p);
	*time = now_us() - start;
	*bytes = p ? wf_pixmap_bytes(p->w, p->h) : 0;
	wf_pixmap_free(p);
	return st;
}

static WfStatus sixel(double *time, size_t *bytes)
{
	wf_buf_clear(&output);
	double start = now_us();
	WfStatus st = wf_sixel_encode(small, 0x101010u, 64, &output);
	*time = now_us() - start;
	*bytes = output.len;
	return st;
}

static bool measure(const char *name, size_t items, Operation operation)
{
	if (used == sizeof results / sizeof results[0]) {
		return false;
	}
	Measurement *r = &results[used];
	r->name = name;
	r->items = items;
	for (unsigned i = 0; i < samples + WARMUP; i++) {
		double time;
		size_t bytes;
		WfStatus st = operation(&time, &bytes);
		if (st != WF_OK || !isfinite(time) || time < 0) {
			fprintf(stderr, "benchmark failed: %s status=%d\n",
			        name, (int)st);
			return false;
		}
		if (i >= WARMUP) {
			r->us[i - WARMUP] = time;
		}
		r->bytes = bytes;
	}
	if (operation == enumerate_directory) {
		r->items = enumerated_count;
	}
	used++;
	return true;
}

static WfStatus no_listing(
        void *ctx, uint32_t gen, const wchar_t *path, const wchar_t *parent)
{
	(void)ctx;
	(void)gen;
	(void)path;
	(void)parent;
	return WF_OK;
}

static FILE *new_file(const wchar_t *root, const wchar_t *name)
{
	wchar_t path[2048];
	if (swprintf(path, 2048, L"%ls\\%ls", root, name) < 0) {
		return NULL;
	}
	HANDLE h =
	        CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		return NULL;
	}
	int fd = _open_osfhandle((intptr_t)h, _O_WRONLY | _O_BINARY);
	if (fd < 0) {
		CloseHandle(h);
		return NULL;
	}
	FILE *f = _fdopen(fd, "wb");
	if (!f) {
		_close(fd);
	}
	return f;
}

static DWORD os_build(void)
{
	typedef LONG(WINAPI * Version)(PRTL_OSVERSIONINFOW);
	FARPROC raw =
	        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
	Version version = NULL;
	_Static_assert(sizeof raw == sizeof version, "function pointer size");
	memcpy(&version, &raw, sizeof version);
	RTL_OSVERSIONINFOW info = {0};
	info.dwOSVersionInfoSize = sizeof info;
	return version && version(&info) == 0 ? info.dwBuildNumber : 0;
}

static bool save(const wchar_t *root,
        const PROCESS_MEMORY_COUNTERS_EX *before,
        DWORD handles_before)
{
	PROCESS_MEMORY_COUNTERS_EX after = {0};
	after.cb = sizeof after;
	DWORD handles_after = 0;
	SYSTEM_INFO system;
	GetNativeSystemInfo(&system);
	if (!GetProcessMemoryInfo(GetCurrentProcess(),
	            (PROCESS_MEMORY_COUNTERS *)&after, sizeof after) ||
	        !GetProcessHandleCount(GetCurrentProcess(), &handles_after)) {
		return false;
	}
	FILE *csv = new_file(root, L"samples.csv"),
	     *json = new_file(root, L"summary.json");
	if (!csv || !json) {
		if (csv) {
			fclose(csv);
		}
		if (json) {
			fclose(json);
		}
		return false;
	}
	fprintf(csv, "metric,items,sample,microseconds,output_bytes\n");
	fprintf(json,
	        "{\n\"schema\":1,\"compiler\":\"%s\",\"configuration\":\"%s\","
	        "\"os_build\":%lu,\"qpc_hz\":%lld,\"samples\":%u,\"warmup\":%u,"
	        "\"processor_architecture\":%u,\"logical_processors\":%lu,"
	        "\"scope\":\"CPU-side; no terminal paint or WriteConsoleW "
	        "calls measured\","
	        "\"handles_before\":%lu,\"handles_after\":%lu,\"private_"
	        "before\":%zu,"
	        "\"private_after\":%zu,\"peak_working_set\":%zu,\"results\":["
	        "\n",
	        WF_BENCH_COMPILER, WF_BENCH_CONFIGURATION, os_build(),
	        frequency.QuadPart, samples, WARMUP,
	        (unsigned)system.wProcessorArchitecture,
	        system.dwNumberOfProcessors, handles_before, handles_after,
	        (size_t)before->PrivateUsage, (size_t)after.PrivateUsage,
	        (size_t)after.PeakWorkingSetSize);
	for (size_t i = 0; i < used; i++) {
		Measurement *r = &results[i];
		double sorted[MAX_SAMPLES];
		memcpy(sorted, r->us, samples * sizeof *sorted);
		qsort(sorted, samples, sizeof *sorted, compare_double);
		for (unsigned s = 0; s < samples; s++) {
			fprintf(csv, "%s,%zu,%u,%.6f,%zu\n", r->name, r->items,
			        s, r->us[s], r->bytes);
		}
		fprintf(json,
		        "%s{\"metric\":\"%s\",\"items\":%zu,\"p50_us\":%.6f,"
		        "\"p95_us\":%.6f,"
		        "\"p99_us\":%.6f,\"max_us\":%.6f,\"output_bytes\":%zu}",
		        i ? ",\n" : "", r->name, r->items,
		        percentile(sorted, samples, 50),
		        percentile(sorted, samples, 95),
		        percentile(sorted, samples, 99), sorted[samples - 1],
		        r->bytes);
	}
	fprintf(json,
	        "\n],\"not_run\":[\"Windows Terminal paint latency\",\"actual "
	        "terminal write calls/backpressure\","
	        "\"AppContainer PDF/image decode\",\"startup\",\"real "
	        "Navigation Memory latency\",\"Copy throughput (P0 gate)\","
	        "\"watcher re-enumeration counts\"]}\n");
	bool ok = !ferror(csv) && !ferror(json);
	if (fclose(csv)) {
		ok = false;
	}
	if (fclose(json)) {
		ok = false;
	}
	return ok;
}

int wmain(int argc, wchar_t **argv)
{
	const wchar_t *destination = NULL;
	for (int i = 1; i < argc; i++) {
		if (!wcscmp(argv[i], L"--output") && i + 1 < argc) {
			destination = argv[++i];
		} else if (!wcscmp(argv[i], L"--enumerate") && i + 1 < argc) {
			enumeration = argv[++i];
		} else if (!wcscmp(argv[i], L"--samples") && i + 1 < argc) {
			wchar_t *end;
			unsigned long n = wcstoul(argv[++i], &end, 10);
			if (*end || n < 3 || n > MAX_SAMPLES) {
				return 2;
			}
			samples = (unsigned)n;
		} else {
			return 2;
		}
	}
	if (!destination || !QueryPerformanceFrequency(&frequency) ||
	        frequency.QuadPart <= 0 ||
	        !CreateDirectoryW(destination, NULL)) {
		return 2;
	}
	PROCESS_MEMORY_COUNTERS_EX before = {0};
	before.cb = sizeof before;
	DWORD handles_before = 0;
	if (!GetProcessMemoryInfo(GetCurrentProcess(),
	            (PROCESS_MEMORY_COUNTERS *)&before, sizeof before) ||
	        !GetProcessHandleCount(GetCurrentProcess(), &handles_before)) {
		return 1;
	}
	int rc = 1;
	bool initialized = false;
	/* Fixed catalog input: user overlays cannot change the workload. */
	wf_i18n_initialize(L"", L"");
	wf_i18n_set_language(WF_UILANG_EN);
	if (wf_app_init(&app, 160, 40, (WfFsPort){NULL, no_listing},
	            L"C:\\benchmark-model") != WF_OK) {
		goto done;
	}
	initialized = true;
	WfDir *listing = directory(4000);
	if (!listing) {
		goto done;
	}
	wf_app_on_listing(&app, app.nav_gen, WF_OK, listing, NULL);
	frame = wf_frame_new(160, 40, &WF_STYLE_BASE);
	changed = wf_frame_new(160, 40, &WF_STYLE_BASE);
	if (!frame || !changed ||
	        wf_pixmap_new(1920, 1080, &picture) != WF_OK) {
		goto done;
	}
	for (size_t i = 0; i < (size_t)picture->w * picture->h; i++) {
		picture->px[i] =
		        0xff000000u | ((uint32_t)i * 2654435761u & 0xffffffu);
	}
	if (wf_pixmap_scale(picture, 320, 180, &small) != WF_OK) {
		goto done;
	}
	if (!measure("frame_160x40", 6400, draw) ||
	        !measure("input_to_frame_cpu", 4000, input_frame) ||
	        !measure("vt_full_generation", 6400, full) ||
	        !measure("vt_idle_generation", 6400, idle) ||
	        !measure("vt_cursor_generation", 6400, row) ||
	        !measure("scale_1920x1080_to_320x180", 2073600, scale) ||
	        !measure("sixel_320x180_64colors", 57600, sixel)) {
		goto done;
	}
	const size_t sizes[] = {1000, 10000, 100000};
	for (size_t i = 0; i < 3; i++) {
		entry_count = sizes[i];
		if (!measure("directory_model_build", entry_count,
		            build_directory) ||
		        !measure("sort_name", entry_count, sort_directory) ||
		        !measure("sort_name_already_sorted", entry_count,
		                sort_again)) {
			goto done;
		}
	}
	if (enumeration &&
	        !measure("enum_win32_read_only", 0, enumerate_directory)) {
		goto done;
	}
	rc = 0;
done:
	wf_pixmap_free(small);
	wf_pixmap_free(picture);
	wf_buf_free(&output);
	wf_frame_free(changed);
	wf_frame_free(frame);
	if (initialized) {
		wf_app_destroy(&app);
	}
	if (!rc && !save(destination, &before, handles_before)) {
		rc = 1;
	}
	return rc;
}
