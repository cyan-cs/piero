/*
 * A stand-in for "ssh.exe -s sftp" in tests: speaks SFTP v3 on its standard
 * input and output from the in-memory tree of wf_fake_sftp.h.
 *
 *   fake_sftpd            serve
 *   fake_sftpd silent     read requests, never answer
 *   fake_sftpd refuse     print what OpenSSH prints for an unknown host key
 *                         to stderr and exit 255 without a byte on stdout
 *   fake_sftpd changed    the same for a changed host key
 *   fake_sftpd stall      answer the handshake, then go silent
 *   fake_sftpd noisy      serve, and write a great deal to stderr first and
 *                         with every answer (far more than a pipe holds)
 *
 * The command line the caller built (options, host, "sftp") is ignored; only
 * the first word chooses the mode, and the test passes it as the program's
 * own argument, not as part of the ssh options.
 */
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "wf_fake_sftp.h"

static Srv srv;

static bool read_all(HANDLE h, uint8_t *b, size_t n)
{
	size_t have = 0;

	while (have < n) {
		DWORD got = 0;

		if (!ReadFile(h, b + have, (DWORD)(n - have), &got, NULL) ||
		        0 == got) {
			return false;
		}
		have += got;
	}
	return true;
}

static bool write_all(HANDLE h, const uint8_t *b, size_t n)
{
	size_t at = 0;

	while (at < n) {
		DWORD put = 0;

		if (!WriteFile(h, b + at, (DWORD)(n - at), &put, NULL) ||
		        !put) {
			return false;
		}
		at += put;
	}
	return true;
}

int main(int argc, char **argv)
{
	HANDLE in = GetStdHandle(STD_INPUT_HANDLE),
	       out = GetStdHandle(STD_OUTPUT_HANDLE);
	const char *mode = argc > 1 ? argv[1] : "serve";
	static uint8_t pkt[WF_SFTP_MAX_PACKET + 4];
	bool stall = !strcmp(mode, "stall"), first = true;
	bool noisy = !strcmp(mode, "noisy");

	if (!strcmp(mode, "refuse")) {
		fputs("No ED25519 host key is known for fake and you have "
		      "requested strict checking.\r\nHost key verification "
		      "failed.\r\n",
		        stderr);
		return 255;
	}
	if (!strcmp(mode, "changed")) {
		fputs("@@@ WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED! "
		      "@@@\r\nHost key verification failed.\r\n",
		        stderr);
		return 255;
	}
	if (noisy) {
		static char line[1024];
		DWORD put;
		HANDLE err = GetStdHandle(STD_ERROR_HANDLE);

		fputs("NOISE-START: the first lines are the ones kept\r\n",
		        stderr);
		fflush(stderr);
		memset(line, 'x', sizeof line);
		line[sizeof line - 2] = '\r';
		line[sizeof line - 1] = '\n';
		for (int i = 0; i < 400; i++) { /* 400 KiB: six pipes full */
			WriteFile(err, line, sizeof line, &put, NULL);
		}
	}
	for (;;) {
		size_t total;
		WfStatus s;

		if (!read_all(in, pkt, 4)) {
			return 0;
		}
		s = wf_sftp_frame(pkt, 4, &total);
		if (WF_E_RANGE != s || !read_all(in, pkt + 4, total - 4)) {
			return 2;
		}
		if (!strcmp(mode, "silent") || (stall && !first)) {
			continue; /* never answer */
		}
		first = false;
		srv.out_n = srv.out_at = 0;
		srv.ext = true;
		on_request(&srv, pkt + 4, total - 4);
		if (!write_all(out, srv.out, srv.out_n)) {
			return 3;
		}
		if (noisy) {
			static char more[8192];
			DWORD put;

			memset(more, 'y', sizeof more);
			WriteFile(GetStdHandle(STD_ERROR_HANDLE), more,
			        sizeof more, &put, NULL);
		}
	}
}
