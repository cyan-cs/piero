/*
 * syntax_probe FILE [LANG] [--bench]: colours FILE with the installed grammar next to
 * this program and prints every run (class, offsets, text). A lab tool for
 * writing and checking highlights.scm; it uses the same host, trust checks and
 * worker-less path as piero.exe. With --bench nothing is printed but the
 * times: loading the grammar, the first colouring, and the median of 20 more
 * after a one-byte edit at the end. It is not part of the product.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "wf_syntax_pin.h"
#include "wf_synhost_win.h"
#include "wf_synlang.h"

static const char *const class_names[WF_SC_COUNT] = {"plain", "comment",
        "string", "escape", "number", "keyword", "type", "preproc", "heading",
        "key", "markup", "boolean", "null", "function", "variable", "constant",
        "operator", "punctuation", "link", "error"};

int main(int argc, char **argv)
{
	FILE *f;
	long size;
	char *text;
	wchar_t wname[260];
	bool bench = argc > 3 && !strcmp(argv[3], "--bench");
	LARGE_INTEGER freq, t0, t1;
	const WfSynLang *lang;
	WfSynSession *s = NULL;
	WfSynResult r;
	uint32_t st;
	const wchar_t *leaf;

	if (argc < 2) {
		fputs("usage: syntax_probe FILE [LANG]\n", stderr);
		return 2;
	}
	if (0 != fopen_s(&f, argv[1], "rb") || !f) {
		perror("open");
		return 2;
	}
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	text = malloc((size_t)size + 1);
	if (!text || fread(text, 1, (size_t)size, f) != (size_t)size) {
		return 2;
	}
	fclose(f);
	text[size] = '\0';
	MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, wname, 260);
	leaf = wcsrchr(wname, L'\\');
	leaf = leaf ? leaf + 1 : wname;
	leaf = wcsrchr(wname, L'/') && wcsrchr(wname, L'/') + 1 > leaf
	        ? wcsrchr(wname, L'/') + 1
	        : leaf;
	lang = argc > 2 && strcmp(argv[2], "-") ? wf_synlang_by_id(argv[2])
	                                        : wf_synlang_for_name(leaf);
	if (!lang) {
		fputs("no official language for this file\n", stderr);
		return 3;
	}
	wf_synhost_configure(NULL, WF_SYNTAX_DLL_SHA256);
	if (!wf_synhost_installed(lang)) {
		fprintf(stderr, "%s is not installed\n", lang->id);
		return 4;
	}
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&t0);
	st = wf_synhost_open(lang->id, NULL, &s);
	if (WF_SYN_OK != st) {
		fprintf(stderr, "open %s: %s\n", lang->id,
		        wf_syn_status_name(st));
		return 5;
	}
	QueryPerformanceCounter(&t1);
	if (bench) {
		printf("open_ms=%.2f ",
		        (double)(t1.QuadPart - t0.QuadPart) * 1000.0 /
		                (double)freq.QuadPart);
	}
	QueryPerformanceCounter(&t0);
	st = wf_synhost_highlight(s, text, (size_t)size, &r);
	QueryPerformanceCounter(&t1);
	if (WF_SYN_OK != st) {
		fprintf(stderr, "highlight: %s\n", wf_syn_status_name(st));
		return 6;
	}
	if (bench) {
		double edit_ms[20];

		printf("first_ms=%.2f (parse=%uus query=%uus) runs=%u ",
		        (double)(t1.QuadPart - t0.QuadPart) * 1000.0 /
		                (double)freq.QuadPart,
		        r.parse_us, r.query_us, r.nruns);
		wf_synhost_result_free(&r);
		for (int i = 0; i < 20; i++) {
			WfSynResult e;

			text[size - 1] =
			        i % 2 ? '\n' : ' '; /* the last byte flips */
			QueryPerformanceCounter(&t0);
			st = wf_synhost_highlight(s, text, (size_t)size, &e);
			QueryPerformanceCounter(&t1);
			if (WF_SYN_OK != st) {
				return 6;
			}
			edit_ms[i] = (double)(t1.QuadPart - t0.QuadPart) *
			        1000.0 / (double)freq.QuadPart;
			wf_synhost_result_free(&e);
		}
		for (int i = 1; i < 20;
		        i++) { /* insertion sort, then the median */
			double v = edit_ms[i];
			int k = i;

			while (k > 0 && edit_ms[k - 1] > v) {
				edit_ms[k] = edit_ms[k - 1];
				k--;
			}
			edit_ms[k] = v;
		}
		printf("edit_median_ms=%.2f edit_max_ms=%.2f "
		       "loaded_dlls=%llu\n",
		        edit_ms[10], edit_ms[19],
		        (unsigned long long)wf_synhost_stat(
		                WF_SYN_STAT_LOADED));
		wf_synhost_close(s);
		wf_synhost_shutdown();
		return 0;
	}
	for (uint32_t i = 0; i < r.nruns; i++) {
		printf("%-11s %6u+%-4u ", class_names[r.runs[i].cls],
		        r.runs[i].start, r.runs[i].length);
		fputc('"', stdout);
		for (uint32_t k = 0; k < r.runs[i].length && k < 40; k++) {
			unsigned char c =
			        (unsigned char)text[r.runs[i].start + k];

			fputc('\n' == c ? '~' : c, stdout);
		}
		fputs("\"\n", stdout);
	}
	printf("runs=%u parse=%uus query=%uus loaded=%llu\n", r.nruns,
	        r.parse_us, r.query_us,
	        (unsigned long long)wf_synhost_stat(WF_SYN_STAT_LOADED));
	wf_synhost_result_free(&r);
	wf_synhost_close(s);
	wf_synhost_shutdown();
	return 0;
}
