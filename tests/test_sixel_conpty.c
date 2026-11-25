/* Real console boundary under ConPTY; the parent emulates terminal replies.
 * This validates transport, not Windows Terminal's visual rendering. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "term/wf_term.h"
#include "render/wf_sixel.h"
#include "wf_test.h"

static int child(void)
{
	WfTerm *term = NULL;
	WfEvent ev[16];
	WfPixmap *pm = NULL;
	WfBuf data = {0};
	bool key = false;
	uint64_t end = GetTickCount64() + 1400;
	if (WF_OK != wf_term_open(&term)) {
		return 2;
	}
	while (GetTickCount64() < end) {
		size_t n = wf_term_read_events(term, ev, 16);
		for (size_t i = 0; i < n; i++) {
			if (ev[i].type == WF_EV_CHAR && ev[i].ch == 'K') {
				key = true;
			}
		}
		Sleep(5);
	}
	if (wf_term_caps_ready(wf_term_capabilities(term))) {
		const WfTermCaps *c = wf_term_capabilities(term);
		char line[64];
		snprintf(line, sizeof line, "CAPS=%u,%u KEY=%d", c->cell_w,
		        c->cell_h, key);
		(void)wf_term_write(term, line, strlen(line));
		if (WF_OK == wf_pixmap_new(400, 400, &pm)) {
			for (size_t i = 0; i < 160000; i++) {
				pm->px[i] = 0xffff0000;
			}
			if (WF_OK == wf_sixel_encode(pm, 0, 256, &data)) {
				(void)wf_term_write(term, data.data, data.len);
			}
		}
	} else {
		const char *line = key ? "FALLBACK KEY=1" : "FALLBACK KEY=0";
		(void)wf_term_write(term, line, strlen(line));
	}
	wf_pixmap_free(pm);
	wf_buf_free(&data);
	wf_term_close(term);
	return 0;
}

typedef struct
{
	HANDLE pipe;
	char raw[65536];
	size_t n;
} Capture;
static DWORD WINAPI read_output(LPVOID ctx)
{
	Capture *c = ctx;
	DWORD got;
	while (c->n + 1 < sizeof c->raw &&
	        ReadFile(c->pipe, c->raw + c->n,
	                (DWORD)(sizeof c->raw - c->n - 1), &got, NULL) &&
	        got) {
		c->n += got;
	}
	c->raw[c->n] = 0;
	return 0;
}

static void run_case(
        const wchar_t *exe, const char *reply, DWORD delay, bool expected)
{
	HANDLE in_r = NULL, in_w = NULL, out_r = NULL, out_w = NULL, reader;
	HPCON pc = NULL;
	STARTUPINFOEXW si = {0};
	PROCESS_INFORMATION pi = {0};
	SIZE_T bytes = 0;
	COORD size = {120, 40};
	wchar_t cmd[2048];
	Capture *cap = calloc(1, sizeof *cap);
	DWORD put, code = 999;
	CHECK(cap != NULL);
	CHECK(CreatePipe(&in_r, &in_w, NULL, 0));
	CHECK(CreatePipe(&out_r, &out_w, NULL, 0));
	CHECK(SUCCEEDED(CreatePseudoConsole(size, in_r, out_w, 0, &pc)));
	CloseHandle(in_r);
	CloseHandle(out_w);
	si.StartupInfo.cb = sizeof si;
	(void)InitializeProcThreadAttributeList(NULL, 1, 0, &bytes);
	si.lpAttributeList = malloc(bytes);
	CHECK(InitializeProcThreadAttributeList(
	        si.lpAttributeList, 1, 0, &bytes));
	CHECK(UpdateProcThreadAttribute(si.lpAttributeList, 0,
	        PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, pc, sizeof pc, NULL,
	        NULL));
	swprintf(cmd, 2048, L"\"%ls\" --child", exe);
	CHECK(CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
	        EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &si.StartupInfo,
	        &pi));
	DeleteProcThreadAttributeList(si.lpAttributeList);
	free(si.lpAttributeList);
	cap->pipe = out_r;
	reader = CreateThread(NULL, 0, read_output, cap, 0, NULL);
	Sleep(delay);
	if (*reply) {
		CHECK(WriteFile(in_w, reply, (DWORD)strlen(reply), &put, NULL));
	}
	CHECK(WriteFile(in_w, "K", 1, &put, NULL));
	CHECK(WAIT_OBJECT_0 == WaitForSingleObject(pi.hProcess, 6000));
	CHECK(GetExitCodeProcess(pi.hProcess, &code) && code == 0);
	ClosePseudoConsole(pc);
	CHECK(WAIT_OBJECT_0 == WaitForSingleObject(reader, 3000));
	printf("ConPTY expected=%d bytes=%zu caps=%d sixel=%d fallback=%d\n",
	        expected, cap->n, strstr(cap->raw, "CAPS=") != NULL,
	        strstr(cap->raw, "\x1bP") != NULL,
	        strstr(cap->raw, "FALLBACK") != NULL);
	if (expected) {

		CHECK(strstr(cap->raw, "CAPS=10,20 KEY=1") != NULL);
		CHECK(strstr(cap->raw, "\x1bP") != NULL);
		CHECK(strstr(cap->raw, "400;400") != NULL);
	} else {
		CHECK(strstr(cap->raw, "FALLBACK KEY=1") != NULL);
	}
	CloseHandle(reader);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	CloseHandle(in_w);
	CloseHandle(out_r);
	free(cap);
}

int wmain(int argc, wchar_t **argv)
{
	wchar_t exe[1024];
	if (argc > 1 && !wcscmp(argv[1], L"--child")) {
		return child();
	}
	CHECK(GetModuleFileNameW(NULL, exe, 1024));
	/* Inbox ConPTY on this host answers DA1 itself without attribute 4,
	 * consumes parent responses, and forwards 16t/14t. Even a synthetic
	 * supporting frontend reply must not override the negative relay. */
	run_case(exe, "\x1b[?61;4;28c\x1b[6;20;10t", 150, false);
	run_case(exe, "\x1b[?1;2c\x1b[6;20;10t", 150, false);
	run_case(exe, "", 150, false);
	run_case(exe, "\x1b[?61;;4;28c\x1b[6;999999;10t", 150, false);
	run_case(exe, "\x1b[?61;4;28c\x1b[6;20;10t", 1000, false);
	return TEST_RESULT();
}
