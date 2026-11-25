/*
 * The child-process side of an SFTP session, with a stand-in program for
 * ssh.exe (fake_sftpd): real pipes, a real process, real time limits.
 *
 *   test_sshproc <fake_sftpd.exe>
 */
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "fs/wf_sftpc.h"
#include "fs/wf_sshcmd.h"
#include "fs/wf_sshproc_win.h"
#include "wf_test.h"

static wchar_t exe[1024];

static WfStatus start(const wchar_t *mode, WfSshProc **p)
{
	wchar_t line[1200];

	swprintf(line, 1200, L"\"%ls\" %ls", exe, mode);
	return wf_sshproc_start(exe, line, p);
}

#define P(x) ((const uint8_t *)(x))

static void a_real_session_lists_and_reads(void)
{
	WfSshProc *p = NULL;
	WfSftpClient *c = NULL;
	WfSftpListing l;
	uint8_t buf[300];
	size_t got = 0;
	WfSftpStream st;

	CHECK(WF_OK == start(L"serve", &p));
	if (!p) {
		return;
	}
	st = wf_sshproc_stream(p);
	CHECK(WF_OK == wf_sftpc_open(&st, 5000, &c));
	CHECK(c && wf_sftpc_has_posix_rename(c) && wf_sftpc_has_fsync(c));
	CHECK(WF_OK == wf_sftpc_list(c, P("/"), 1, 100, &l));
	CHECK(6 == l.n);
	wf_sftp_listing_free(&l);
	CHECK(WF_OK ==
	        wf_sftpc_read_range(c, P("/a.txt"), 6, 5, 200, buf, &got));
	CHECK(95 == got && buf[0] == (uint8_t)(5 * 7 + 3));
	CHECK(!wf_sftpc_broken(c));
	wf_sftpc_close(c);
	/* the child is still alive until the session is freed; freeing ends it */
	CHECK(!wf_sshproc_wait(p, 0, NULL));
	wf_sshproc_free(p);
}

/* A child that writes far more to stderr than a pipe holds, before it answers
 * and with every answer: its stderr is read all along, so it is not left
 * waiting, the session works, and only the start of what it wrote is kept. */
static void a_child_that_fills_stderr_does_not_stall_the_session(void)
{
	WfSshProc *p = NULL;
	WfSftpClient *c = NULL;
	WfSftpListing l;
	WfSftpStream st;
	uint8_t buf[300];
	size_t got = 0;
	char err[8192];

	CHECK(WF_OK == start(L"noisy", &p));
	if (!p) {
		return;
	}
	st = wf_sshproc_stream(p);
	CHECK(WF_OK == wf_sftpc_open(&st, 5000, &c));
	if (c) {
		for (int i = 0; i < 30;
		        i++) { /* 240 KiB more, with the answers */
			CHECK(WF_OK == wf_sftpc_list(c, P("/"), 1, 100, &l));
			wf_sftp_listing_free(&l);
		}
		CHECK(WF_OK ==
		        wf_sftpc_read_range(
		                c, P("/a.txt"), 6, 5, 200, buf, &got));
		CHECK(95 == got && !wf_sftpc_broken(c));
		wf_sftpc_close(c);
	}
	wf_sshproc_stderr(p, err, sizeof err);
	CHECK(0 ==
	        strncmp(err, "NOISE-START",
	                11)); /* the start is what is kept */
	CHECK(strlen(err) <= 4096);
	wf_sshproc_free(p);
}

static void a_silent_server_times_out_and_the_session_is_broken(void)
{
	WfSshProc *p = NULL;
	WfSftpClient *c = NULL;
	WfSftpStream st;
	DWORD t0 = GetTickCount(), took;

	CHECK(WF_OK == start(L"silent", &p));
	if (!p) {
		return;
	}
	st = wf_sshproc_stream(p);
	CHECK(WF_E_TIMEOUT == wf_sftpc_open(&st, 300, &c) && !c);
	took = GetTickCount() - t0;
	CHECK(took >= 250 && took < 3000);
	wf_sshproc_free(p);

	/* the handshake works, the first request is never answered */
	CHECK(WF_OK == start(L"stall", &p));
	if (!p) {
		return;
	}
	st = wf_sshproc_stream(p);
	CHECK(WF_OK == wf_sftpc_open(&st, 300, &c));
	{
		WfSftpListing l;

		CHECK(WF_E_TIMEOUT == wf_sftpc_list(c, P("/"), 1, 10, &l));
		CHECK(wf_sftpc_broken(c));
	}
	wf_sftpc_close(c);
	wf_sshproc_free(p);
}

static DWORD WINAPI cancel_later(void *arg)
{
	Sleep(200);
	wf_sshproc_cancel((WfSshProc *)arg);
	return 0;
}

static void a_cancel_from_another_thread_ends_a_long_wait(void)
{
	WfSshProc *p = NULL;
	WfSftpClient *c = NULL;
	WfSftpStream st;
	HANDLE t;
	DWORD t0 = GetTickCount(), took;

	CHECK(WF_OK == start(L"silent", &p));
	if (!p) {
		return;
	}
	st = wf_sshproc_stream(p);
	t = CreateThread(NULL, 0, cancel_later, p, 0, NULL);
	CHECK(NULL != t);
	/* a limit of a minute: only the cancel can end this in time */
	CHECK(WF_E_CANCELLED == wf_sftpc_open(&st, 60000, &c));
	took = GetTickCount() - t0;
	CHECK(took >= 150 && took < 5000);
	if (t) {
		WaitForSingleObject(t, 5000);
		CloseHandle(t);
	}
	/* later calls are cancelled at once too */
	CHECK(WF_E_CANCELLED == wf_sftpc_open(&st, 60000, &c));
	wf_sshproc_free(p);
}

static void a_refusal_is_read_from_stderr_and_the_exit_code(void)
{
	static const struct
	{
		const wchar_t *mode;
		WfSshFail want;
	} cases[] = {{L"refuse", WF_SSH_FAIL_HOSTKEY_UNKNOWN},
	        {L"changed", WF_SSH_FAIL_HOSTKEY_CHANGED}};

	for (size_t i = 0; i < 2; i++) {
		WfSshProc *p = NULL;
		WfSftpClient *c = NULL;
		WfSftpStream st;
		char err[512];
		int code = 0;

		CHECK(WF_OK == start(cases[i].mode, &p));
		if (!p) {
			continue;
		}
		st = wf_sshproc_stream(p);
		/* the stream ends at once: no handshake, so no session */
		CHECK(WF_OK != wf_sftpc_open(&st, 3000, &c) && !c);
		CHECK(wf_sshproc_wait(p, 3000, &code) && 255 == code);
		wf_sshproc_stderr(p, err, sizeof err);
		CHECK(cases[i].want == wf_ssh_classify(code, err));
		wf_sshproc_free(p);
	}
}

static void bad_arguments_and_a_missing_program(void)
{
	WfSshProc *p = NULL;

	CHECK(WF_E_INVAL == wf_sshproc_start(NULL, L"x", &p));
	CHECK(WF_E_INVAL == wf_sshproc_start(L"", L"x", &p));
	CHECK(WF_E_INVAL == wf_sshproc_start(exe, L"", &p));
	CHECK(WF_E_OS ==
	                wf_sshproc_start(
	                        L"C:\\no\\such\\ssh.exe", L"ssh.exe", &p) &&
	        !p);
	wf_sshproc_free(NULL);
	wf_sshproc_cancel(NULL);
}

int wmain(int argc, wchar_t **argv)
{
	if (argc != 2) {
		fputs("usage: test_sshproc <fake_sftpd.exe>\n", stderr);
		return 2;
	}
	wcscpy_s(exe, 1024, argv[1]);
	a_real_session_lists_and_reads();
	a_child_that_fills_stderr_does_not_stall_the_session();
	a_silent_server_times_out_and_the_session_is_broken();
	a_cancel_from_another_thread_ends_a_long_wait();
	a_refusal_is_read_from_stderr_and_the_exit_code();
	bad_arguments_and_a_missing_program();
	return TEST_RESULT();
}
