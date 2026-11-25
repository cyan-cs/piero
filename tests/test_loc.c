/*
 * The place/name model shared by Windows, WSL and SFTP: what each kind may do
 * by default, how names are kept and shown, and how a place becomes a key.
 */
#include <stdlib.h>
#include <string.h>

#include "fs/wf_loc.h"
#include "wf_test.h"

#define S(x) ((const uint8_t *)(x))

static void read_only_is_the_default_and_a_partial_proof_grants_nothing(void)
{
	uint32_t wsl = wf_caps_default(WF_BK_WSL);
	uint32_t sftp = wf_caps_default(WF_BK_SFTP);
	WfSaveProof all = {true, true, true, true, true, true};
	WfSaveProof p;

	CHECK(wf_cap_has(wsl, WF_CAP_LIST | WF_CAP_READ | WF_CAP_READ_RANGE));
	CHECK(!(wsl &
	        (WF_CAP_NEW_FILE | WF_CAP_SAVE_OVER | WF_CAP_RENAME |
	                WF_CAP_FSYNC)));
	CHECK(!(sftp & (WF_CAP_NEW_FILE | WF_CAP_SAVE_OVER | WF_CAP_RENAME)));
	/* nothing claims case sensitivity or identity before it is probed */
	CHECK(!(wsl & (WF_CAP_CASE_SENSITIVE | WF_CAP_IDENTITY)));
	CHECK(wf_cap_has(wf_caps_default(WF_BK_LOCAL), WF_CAP_SAVE_OVER));
	CHECK(0 == wf_caps_default((WfBackendKind)99));

	CHECK(wsl == wf_caps_grant_save(WF_BK_WSL, wsl, NULL));
	CHECK(wf_cap_has(wf_caps_grant_save(WF_BK_WSL, wsl, &all),
	        WF_CAP_SAVE_OVER | WF_CAP_NEW_FILE));

	/* every single missing piece of proof withholds replacing */
	for (int i = 0; i < 6; i++) {
		bool *f[6];
		uint32_t got;

		p = all;
		f[0] = &p.staging_in_same_directory;
		f[1] = &p.noclobber_publish;
		f[2] = &p.atomic_replace;
		f[3] = &p.identity_checked_at_commit;
		f[4] = &p.data_durable_before_commit;
		f[5] = &p.readback_verified;
		*f[i] = false;
		got = wf_caps_grant_save(WF_BK_SFTP, sftp, &p);
		CHECK(!(got & WF_CAP_SAVE_OVER));
		/* a new file does not need the replace-only proofs */
		CHECK(!!(got & WF_CAP_NEW_FILE) == (2 == i || 3 == i));
	}
}

static void the_same_name_in_two_cases_is_two_places(void)
{
	WfLocId a, b, c, d;
	uint8_t k1[256], k2[256];
	size_t n1, n2;

	CHECK(WF_OK == wf_locid_wsl(&a, "Ubuntu"));
	CHECK(WF_OK == wf_locid_wsl(&b, "ubuntu"));
	CHECK(!wf_locid_equal(&a, &b));
	CHECK(WF_OK == wf_locid_sftp(&c, "box", 0));
	CHECK(WF_OK == wf_locid_sftp(&d, "box", 22));
	CHECK(wf_locid_equal(&c, &d)); /* 0 means the default port */
	CHECK(WF_OK == wf_locid_sftp(&d, "box", 2222));
	CHECK(!wf_locid_equal(&c, &d));

	/* same path, different place: different memory keys */
	CHECK(WF_OK ==
	        wf_loc_memory_key(&a, S("/home/u"), 7, k1, sizeof k1, &n1));
	CHECK(WF_OK ==
	        wf_loc_memory_key(&c, S("/home/u"), 7, k2, sizeof k2, &n2));
	CHECK(n1 != n2 || memcmp(k1, k2, n1));
	/* same place, paths differing only by case: different keys */
	CHECK(WF_OK ==
	        wf_loc_memory_key(&a, S("/home/U"), 7, k2, sizeof k2, &n2));
	CHECK(n1 == n2 && memcmp(k1, k2, n1));
	CHECK(WF_E_RANGE == wf_loc_memory_key(&a, S("/home/u"), 7, k1, 8, &n1));

	/* what a distro or alias may not look like */
	CHECK(WF_E_INVAL == wf_locid_wsl(&a, ""));
	CHECK(WF_E_INVAL == wf_locid_wsl(&a, "-o"));
	CHECK(WF_E_INVAL == wf_locid_wsl(&a, "a b"));
	CHECK(WF_E_INVAL == wf_locid_wsl(&a, "a\\b"));
	CHECK(WF_E_INVAL == wf_locid_sftp(&a, "h:22", 22));
	CHECK(WF_E_INVAL == wf_locid_sftp(&a, "h", 70000));
}

static void posix_names_and_paths_are_taken_as_they_are(void)
{
	uint8_t out[64];
	size_t n;

	CHECK(wf_posix_name_ok(S("a\\b"), 3));     /* a backslash is a letter */
	CHECK(wf_posix_name_ok(S("\xFF\xFE"), 2)); /* not UTF-8: still a name */
	CHECK(!wf_posix_name_ok(S("a/b"), 3));
	CHECK(!wf_posix_name_ok(S("a\0b"), 3));
	CHECK(!wf_posix_name_ok(S("."), 1) && !wf_posix_name_ok(S(".."), 2));
	CHECK(wf_posix_name_ok(S("..."), 3));
	CHECK(!wf_posix_name_ok(S(""), 0));
	{
		uint8_t big[300];

		memset(big, 'x', sizeof big);
		CHECK(wf_posix_name_ok(big, 255) &&
		        !wf_posix_name_ok(big, 256));
	}

	CHECK(wf_posix_path_ok(S("/"), 1));
	CHECK(wf_posix_path_ok(S("/home/u/x"), 9));
	CHECK(!wf_posix_path_ok(S("home"), 4));
	CHECK(!wf_posix_path_ok(S("/a/"), 3));
	CHECK(!wf_posix_path_ok(S("/a//b"), 5));
	CHECK(!wf_posix_path_ok(S("/a/../b"), 7));
	CHECK(!wf_posix_path_ok(S("/a/./b"), 6));
	CHECK(!wf_posix_path_ok(S("/a\0b"), 4));
	CHECK(!wf_posix_path_ok(S("C:\\x"), 4));

	CHECK(WF_OK ==
	        wf_posix_join(S("/"), 1, S("a"), 1, out, sizeof out, &n));
	CHECK(2 == n && !memcmp(out, "/a", 3));
	CHECK(WF_OK ==
	        wf_posix_join(S("/a"), 2, S("B"), 1, out, sizeof out, &n));
	CHECK(4 == n && !memcmp(out, "/a/B", 5));
	CHECK(WF_E_INVAL ==
	        wf_posix_join(S("/a"), 2, S(".."), 2, out, sizeof out, &n));
	CHECK(WF_E_INVAL ==
	        wf_posix_join(S("/a"), 2, S("b/c"), 3, out, sizeof out, &n));
	CHECK(WF_E_RANGE == wf_posix_join(S("/a"), 2, S("b"), 1, out, 4, &n));
}

static void a_name_is_shown_so_that_it_cannot_act_or_be_confused(void)
{
	char out[128];
	bool cut;

	/* plain text, Japanese and an emoji pass through */
	wf_name_display(S("a.txt"), 5, out, sizeof out, &cut);
	CHECK(!strcmp(out, "a.txt") && !cut);
	wf_name_display(
	        S("\xE6\x9C\x88\xF0\x9F\x98\x80"), 7, out, sizeof out, &cut);
	CHECK(!strcmp(out, "\xE6\x9C\x88\xF0\x9F\x98\x80"));

	/* the terminal is not driven by a name */
	wf_name_display(S("a\x1b[31mb"), 7, out, sizeof out, &cut);
	CHECK(!strchr(out, 0x1b) && !strcmp(out, "a\\x1B[31mb"));
	wf_name_display(S("x\x7F\n\r\t"), 5, out, sizeof out, &cut);
	CHECK(!strcmp(out, "x\\x7F\\x0A\\x0D\\x09"));
	wf_name_display(S("\xC2\x9B"), 2, out, sizeof out, &cut); /* C1 CSI */
	CHECK(!strcmp(out, "\\u{009B}"));
	/* a right-to-left override does not reorder the line */
	wf_name_display(S("a\xE2\x80\xAEgpj.exe"), 11, out, sizeof out, &cut);
	CHECK(!strcmp(out, "a\\u{202E}gpj.exe"));
	wf_name_display(S("\xE2\x80\xA8"), 3, out, sizeof out, &cut);
	CHECK(!strcmp(out, "\\u{2028}"));

	/* bytes that are not UTF-8 are written, not dropped or replaced */
	wf_name_display(S("\xFF\xFE"), 2, out, sizeof out, &cut);
	CHECK(!strcmp(out, "\\xFF\\xFE"));
	wf_name_display(
	        S("\xC0\x80"), 2, out, sizeof out, &cut); /* overlong NUL */
	CHECK(!strcmp(out, "\\xC0\\x80"));
	wf_name_display(
	        S("\xED\xA0\x80"), 3, out, sizeof out, &cut); /* surrogate */
	CHECK(!strcmp(out, "\\xED\\xA0\\x80"));
	wf_name_display(
	        S("\xE6\x9C"), 2, out, sizeof out, &cut); /* cut short */
	CHECK(!strcmp(out, "\\xE6\\x9C"));

	/* two different names never look alike: the escape itself is escaped */
	{
		char a[64], b[64];

		wf_name_display(S("\\x41"), 4, a, sizeof a, &cut);
		wf_name_display(S("\x01"
		                  "A"),
		        2, b, sizeof b, &cut);
		CHECK(!strcmp(a, "\\\\x41") && strcmp(a, b));
		wf_name_display(S("\xC3"), 1, a, sizeof a, &cut);
		wf_name_display(S("\\xC3"), 4, b, sizeof b, &cut);
		CHECK(strcmp(a, b));
	}

	/* cut at a boundary, never in the middle of an escape */
	wf_name_display(S("ab\x01"
	                  "cd"),
	        5, out, 5, &cut);
	CHECK(cut && !strcmp(out, "ab"));
	CHECK(0 == wf_name_display(S("a"), 1, NULL, 0, &cut));
}

static int cmp_shown(const void *x, const void *y)
{
	return strcmp((const char *)x, (const char *)y);
}

static void injective_over_every_single_byte_and_pair(void)
{
	static char seen[256 * 256][24];
	size_t kept = 0;

	/* every one- and two-byte string (bar NUL) shows differently */
	for (unsigned a = 1; a < 256; a++) {
		for (unsigned b = 0; b < 256; b++) {
			uint8_t raw[2] = {(uint8_t)a, (uint8_t)b};
			char out[24];
			bool cut;

			wf_name_display(raw, b ? 2 : 1, out, sizeof out, &cut);
			CHECK(!cut);
			strcpy_s(seen[kept++], 24, out);
		}
	}
	qsort(seen, kept, sizeof seen[0], cmp_shown);
	for (size_t i = 1; i < kept; i++) {
		if (!strcmp(seen[i - 1], seen[i])) {
			CHECK_FAIL("two names display the same");
			return;
		}
	}
}

static void wsl_paths_are_recognised_by_their_server_name(void)
{
	CHECK(wf_wsl_unc_is(L"\\\\wsl.localhost\\Ubuntu"));
	CHECK(wf_wsl_unc_is(L"\\\\wsl.localhost\\Ubuntu\\home\\u"));
	CHECK(wf_wsl_unc_is(L"\\\\WSL.Localhost\\Ubuntu"));
	CHECK(wf_wsl_unc_is(L"\\\\wsl$\\Ubuntu\\x"));
	CHECK(wf_wsl_unc_is(L"\\\\?\\UNC\\wsl.localhost\\Ubuntu\\x"));
	CHECK(wf_wsl_unc_is(L"\\\\wsl.localhost"));
	CHECK(wf_wsl_unc_is(L"\\\\wsl.localhost/Ubuntu"));
	/* near misses are other servers, not WSL */
	CHECK(!wf_wsl_unc_is(L"\\\\wsl.localhostx\\share\\a"));
	CHECK(!wf_wsl_unc_is(L"\\\\wslx\\share"));
	CHECK(!wf_wsl_unc_is(L"\\\\server\\wsl.localhost\\a"));
	CHECK(!wf_wsl_unc_is(L"C:\\wsl.localhost\\a"));
	CHECK(!wf_wsl_unc_is(L""));
	CHECK(!wf_wsl_unc_is(NULL));
}

static void new_files_start_with_the_line_ending_of_their_place(void)
{
	CHECK(WF_NEW_EOL_CRLF == wf_new_file_eol(WF_BK_LOCAL));
	CHECK(WF_NEW_EOL_LF == wf_new_file_eol(WF_BK_WSL));
	CHECK(WF_NEW_EOL_LF == wf_new_file_eol(WF_BK_SFTP));
}

int main(void)
{
	read_only_is_the_default_and_a_partial_proof_grants_nothing();
	the_same_name_in_two_cases_is_two_places();
	posix_names_and_paths_are_taken_as_they_are();
	a_name_is_shown_so_that_it_cannot_act_or_be_confused();
	injective_over_every_single_byte_and_pair();
	wsl_paths_are_recognised_by_their_server_name();
	new_files_start_with_the_line_ending_of_their_place();
	return TEST_RESULT();
}
