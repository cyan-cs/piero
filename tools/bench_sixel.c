/* Trusted generated fixtures only: decoder microbenchmark, not a product
 * sandbox substitute and not a terminal latency measurement. */
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include "preview/wf_decode_win.h"
#include "render/wf_sixel.h"
#include "render/wf_pixcell.h"
#include "render/wf_present.h"

static double now(void)
{
	LARGE_INTEGER t, f;
	QueryPerformanceCounter(&t);
	QueryPerformanceFrequency(&f);
	return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}
static double cpu_ms(void)
{
	FILETIME created, ended, kernel, user;
	ULARGE_INTEGER k, u;
	if (!GetProcessTimes(
	            GetCurrentProcess(), &created, &ended, &kernel, &user)) {
		return 0;
	}
	k.LowPart = kernel.dwLowDateTime;
	k.HighPart = kernel.dwHighDateTime;
	u.LowPart = user.dwLowDateTime;
	u.HighPart = user.dwHighDateTime;
	return (double)(k.QuadPart + u.QuadPart) / 10000.0;
}

static void
save(const wchar_t *prefix, const wchar_t *suffix, const void *data, size_t n)
{
	wchar_t path[2048];
	FILE *f = NULL;
	swprintf(path, 2048, L"%ls%ls", prefix, suffix);
	if (!_wfopen_s(&f, path, L"wb") && f) {
		fwrite(data, 1, n, f);
		fclose(f);
	}
}
static void ppm(
        const wchar_t *prefix, const WfPixmap *pm, const wchar_t *suffix)
{
	WfBuf b = {0};
	char header[100];
	snprintf(header, sizeof header, "P6\n%u %u\n255\n", pm->w, pm->h);
	(void)wf_buf_puts(&b, header);
	for (size_t i = 0; i < (size_t)pm->w * pm->h; i++) {
		uint32_t rgb = wf_px_over(pm->px[i], 0);
		unsigned char p[3] = {(unsigned char)(rgb >> 16),
		        (unsigned char)(rgb >> 8), (unsigned char)rgb};
		(void)wf_buf_append(&b, p, 3);
	}
	save(prefix, suffix, b.data, b.len);
	wf_buf_free(&b);
}
int wmain(int argc, wchar_t **argv)
{
	FILE *f = NULL;
	long len;
	void *bytes;
	bool pdf;
	setvbuf(stdout, NULL, _IONBF, 0);
	if ((argc != 4 && argc != 5) || _wfopen_s(&f, argv[1], L"rb") || !f) {
		return 2;
	}
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	rewind(f);
	if (len <= 0 || len > 16 * 1024 * 1024) {
		fclose(f);
		return 2;
	}
	bytes = malloc((size_t)len);
	if (!bytes || fread(bytes, 1, (size_t)len, f) != (size_t)len) {
		fclose(f);
		free(bytes);
		return 2;
	}
	fclose(f);
	pdf = !wcscmp(argv[3], L"pdf");
	unsigned first = argc == 5 ? (unsigned)wcstoul(argv[4], NULL, 10) : 0;
	if (first > 3) {
		free(bytes);
		return 2;
	}
	for (unsigned mode = first; mode < (argc == 5 ? first + 1 : 4);
	        mode++) {
		WfPixmap *pm = NULL;
		WfDecodeInfo info = {0};
		WfBuf out = {0};
		uint32_t width = mode ? 400 : 40, height = mode ? 400 : 40;
		double t = now(), decoded, encoded;
		double cpu_start = cpu_ms(), decode_cpu, encode_cpu;
		PROCESS_MEMORY_COUNTERS memory = {0};
		WfDecodeStatus status;
		printf("begin mode=%u pdf=%d\n", mode, pdf);
		status = pdf ? wf_decode_pdf(bytes, (size_t)len, 0, width,
		                       height, NULL, &pm, &info)
		             : wf_decode_image(bytes, (size_t)len, width,
		                       height, &pm, &info);
		decoded = now() - t;
		decode_cpu = cpu_ms() - cpu_start;
		if (status != WF_DSTAT_OK || !pm) {
			printf("decode failed=%d\n", (int)status);
			free(bytes);
			return 1;
		}
		t = now();
		cpu_start = cpu_ms();
		if (mode) {
			WfStatus st = wf_sixel_encode_ex(pm, 0,
			        mode == 1           ? 256u
			                : mode == 2 ? 128u
			                            : 64u,
			        mode == 1, NULL, NULL, &out);
			if (st != WF_OK) {
				printf("encode failed=%d\n", (int)st);
				return 1;
			}
		} else {
			WfStyle base = {0xffffff, 0, 0};
			WfFrame *frame = wf_frame_new(40, 20, &base);
			wf_pixcell_draw_view(frame, 0, 0, 40, 20, pm, 0, 0, 0,
			        WF_PIXSTYLE_HALFBLOCK, NULL, NULL);
			(void)wf_present_diff(NULL, frame, &out);
			wf_frame_free(frame);
		}
		encoded = now() - t;
		encode_cpu = cpu_ms() - cpu_start;
		(void)GetProcessMemoryInfo(
		        GetCurrentProcess(), &memory, sizeof memory);
		printf("%u,%u,%u,%.3f,%.3f,%zu,%zu,%.3f,%.3f,%zu\n", mode,
		        pm->w, pm->h, decoded, encoded, out.len,
		        wf_pixmap_bytes(pm->w, pm->h), decode_cpu, encode_cpu,
		        (size_t)memory.PeakWorkingSetSize);
		if (mode == 1) {
			save(argv[2], L".sixel", out.data, out.len);
			ppm(argv[2], pm, L".ppm");
		}
		if (!mode) {
			save(argv[2], L".text.vt", out.data, out.len);
			ppm(argv[2], pm, L".text.ppm");
		}
		if (mode == 2) {
			save(argv[2], L".balanced.sixel", out.data, out.len);
		}
		if (mode == 3) {
			save(argv[2], L".fast.sixel", out.data, out.len);
		}
		wf_buf_free(&out);
		wf_pixmap_free(pm);
	}
	free(bytes);
	return 0;
}
