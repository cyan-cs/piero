/*
 * An SFTP place listed and read through a real child process (the stand-in program fake_sftpd instead of ssh.exe), the
 * reasons a connection is refused, and that nothing can be changed there.
 *
 *   test_sftpplace <fake_sftpd.exe>
 */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/wf_efile_win.h"
#include "fs/wf_dir.h"
#include "fs/wf_sftpplace_win.h"
#include "wf_test.h"

static wchar_t exe[1024];

static void use_mode(const wchar_t *mode)
{
	wchar_t line[1200];

	swprintf(line, 1200, L"\"%ls\" %ls", exe, mode);
	wf_sftp_place_test_command(exe, line);
	wf_sftp_place_close();
}

static const WfEntry *find(const WfDir *d, const wchar_t *name)
{
	for (size_t i = 0; d && i < d->n; i++) {
		if (!wcscmp(d->e[i].name, name)) {
			return &d->e[i];
		}
	}
	return NULL;
}

static void a_listing_comes_through_a_real_session(void)
{
	WfDir *d = NULL;
	const WfEntry *e;

	use_mode(L"serve");
	CHECK(wf_sftp_place_is(L"\\\\sftp.invalid\\box"));
	CHECK(wf_sftp_place_is(L"\\\\SFTP.INVALID\\box\\a"));
	CHECK(!wf_sftp_place_is(L"\\\\sftp.invalidx\\box") &&
	        !wf_sftp_place_is(L"C:\\x") && !wf_sftp_place_is(NULL));

	CHECK(WF_OK ==
	        wf_sftp_place_list(
	                L"\\\\sftp.invalid\\box", 1000, NULL, NULL, &d));
	CHECK(d && 6 == d->n);
	/* Linux case is kept: both are there */
	CHECK(find(d, L"a.txt") && find(d, L"A.txt"));
	e = find(d, L"dir");
	CHECK(e && (e->flags & WF_ENT_DIR));
	e = find(d, L"link");
	CHECK(e && (e->flags & WF_ENT_LINK) && !(e->flags & WF_ENT_DIR));
	e = find(d, L"a.txt");
	CHECK(e && 100 == e->size && !(e->flags & WF_ENT_DIR));
	wf_dir_free(d);

	/* a sub folder, with the session kept for it */
	d = NULL;
	CHECK(WF_OK ==
	        wf_sftp_place_list(
	                L"\\\\sftp.invalid\\box\\dir", 1000, NULL, NULL, &d));
	CHECK(d && 1 == d->n && find(d, L"x") && 70000 == find(d, L"x")->size);
	wf_dir_free(d);

	/* the server's "no such file" is an answer, not a broken session */
	d = NULL;
	CHECK(WF_E_NOENT ==
	        wf_sftp_place_list(L"\\\\sftp.invalid\\box\\nosuch", 1000, NULL,
	                NULL, &d));
	CHECK(!d);
	d = NULL;
	CHECK(WF_OK ==
	        wf_sftp_place_list(
	                L"\\\\sftp.invalid\\box", 1000, NULL, NULL, &d));
	wf_dir_free(d);

	/* after close a new session is made */
	wf_sftp_place_close();
	d = NULL;
	CHECK(WF_OK ==
	        wf_sftp_place_list(
	                L"\\\\sftp.invalid\\box", 1000, NULL, NULL, &d));
	wf_dir_free(d);

	/* a name that could be read as an option never reaches ssh */
	d = NULL;
	CHECK(WF_OK !=
	        wf_sftp_place_list(
	                L"\\\\sftp.invalid\\-oProxy", 10, NULL, NULL, &d));
	CHECK(WF_OK !=
	        wf_sftp_place_list(
	                L"\\\\sftp.invalid\\a b", 10, NULL, NULL, &d));
	CHECK(WF_OK !=
	        wf_sftp_place_list(L"\\\\sftp.invalid\\", 10, NULL, NULL, &d));
	CHECK(WF_OK != wf_sftp_place_list(L"C:\\", 10, NULL, NULL, &d));
	CHECK(!d);
	wf_sftp_place_close();
}

static void a_refused_connection_says_why(void)
{
	WfDir *d = NULL;
	char note[256];

	use_mode(L"refuse");
	CHECK(WF_OK !=
	        wf_sftp_place_list(
	                L"\\\\sftp.invalid\\box", 10, NULL, NULL, &d));
	wf_sftp_place_note(note, sizeof note);
	CHECK(strstr(note, "not known") && strstr(note, "never accepts"));

	use_mode(L"changed");
	CHECK(WF_OK !=
	        wf_sftp_place_list(
	                L"\\\\sftp.invalid\\box", 10, NULL, NULL, &d));
	wf_sftp_place_note(note, sizeof note);
	CHECK(strstr(note, "CHANGED"));
	CHECK(!d);

	/* a server that never answers: the wait ends, nothing hangs */
	use_mode(L"silent");
	wf_sftp_place_test_wait(400);
	{
		DWORD t0 = GetTickCount();

		CHECK(WF_OK !=
		        wf_sftp_place_list(
		                L"\\\\sftp.invalid\\box", 10, NULL, NULL, &d));
		CHECK(GetTickCount() - t0 < 8000);
	}
	wf_sftp_place_test_wait(0);
	wf_sftp_place_close();
	wf_sftp_place_test_command(NULL, NULL);
}

static void a_remote_file_is_read_whole_and_never_written_back(void)
{
	uint8_t *buf = NULL;
	size_t n = 0;
	uint32_t mtime = 1;
	WfLoaded l;
	WfSaveResult sr;
	WfEncoding utf8 = {WF_ENC_UTF8, 0};
	WfFileStamp was;

	use_mode(L"serve");
	CHECK(WF_OK ==
	        wf_sftp_place_read(
	                L"\\\\sftp.invalid\\box\\a.txt", 0, &buf, &n, &mtime));
	CHECK(100 == n && buf && buf[5] == (uint8_t)(5 * 7 + 3));
	free(buf);
	buf = NULL;
	CHECK(WF_E_RANGE ==
	        wf_sftp_place_read(
	                L"\\\\sftp.invalid\\box\\a.txt", 50, &buf, &n, &mtime));
	CHECK(WF_E_INVAL ==
	        wf_sftp_place_read(
	                L"\\\\sftp.invalid\\box\\dir", 0, &buf, &n, &mtime));
	CHECK(WF_E_INVAL ==
	        wf_sftp_place_read(
	                L"\\\\sftp.invalid\\box\\link", 0, &buf, &n, &mtime));
	CHECK(WF_E_INVAL ==
	        wf_sftp_place_read(
	                L"\\\\sftp.invalid\\box\\dev", 0, &buf, &n, &mtime));
	CHECK(WF_E_NOENT ==
	        wf_sftp_place_read(
	                L"\\\\sftp.invalid\\box\\none", 0, &buf, &n, &mtime));
	CHECK(WF_E_INVAL ==
	        wf_sftp_place_read(
	                L"\\\\sftp.invalid\\box", 0, &buf, &n, &mtime));
	CHECK(!buf);

	/* the editor's loader reads it too (this one is binary), with a stamp
	 * that says read-only */
	CHECK(WF_OK ==
	        wf_efile_load(
	                L"\\\\sftp.invalid\\box\\a.txt", 0, 1252, NULL, &l));
	CHECK(WF_FL_BINARY == l.verdict && l.stamp.valid &&
	        (l.stamp.attributes & FILE_ATTRIBUTE_READONLY) &&
	        100 == l.stamp.size);
	wf_efile_loaded_free(&l);
	CHECK(WF_OK ==
	        wf_efile_load(
	                L"\\\\sftp.invalid\\box\\none", 0, 1252, NULL, &l));
	CHECK(WF_FL_MISSING == l.verdict);
	wf_efile_loaded_free(&l);

	/* and nothing is ever saved there */
	memset(&was, 0, sizeof was);
	was.valid = true;
	CHECK(WF_OK ==
	                wf_efile_save_new(L"\\\\sftp.invalid\\box\\n.txt", "x",
	                        1, utf8, &sr) &&
	        WF_SV_UNSAFE == sr.verdict);
	CHECK(WF_OK ==
	                wf_efile_save_over(L"\\\\sftp.invalid\\box\\a.txt",
	                        &was, "x", 1, utf8, NULL, &sr) &&
	        WF_SV_UNSAFE == sr.verdict);
	wf_sftp_place_close();
	wf_sftp_place_test_command(NULL, NULL);
}

int wmain(int argc, wchar_t **argv)
{
	if (argc >= 2) {
		wcscpy_s(exe, 1024, argv[1]);
	} else { /* the mutation harness copies it next to the test */
		wchar_t *slash;

		GetModuleFileNameW(NULL, exe, 1024);
		slash = wcsrchr(exe, L'\\');
		if (!slash) {
			return 2;
		}
		wcscpy_s(slash + 1, 1024 - (size_t)(slash + 1 - exe),
		        L"fake_sftpd.exe");
	}
	a_listing_comes_through_a_real_session();
	a_refused_connection_says_why();
	a_remote_file_is_read_whole_and_never_written_back();
	return TEST_RESULT();
}
