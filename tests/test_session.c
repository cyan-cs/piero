/*
 * The saved session (DR-DRAFT-018): the text form round-trips Unicode and
 * refuses every kind of damage whole; the store keeps two slots, one writer
 * (the lock), leaves a newer version's data alone, and forgets by emptying;
 * the writer thread writes the latest state and a normal stop writes what is
 * pending. Real files in a folder of their own under the temporary folder.
 */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "session/wf_session.h"
#include "session/wf_session_dedupe.h"
#include "session/wf_session_store.h"
#include "session/wf_session_writer.h"
#include "wf_test.h"

static wchar_t g_dir[MAX_PATH + 32];

static WfSession sample(void)
{
	static wchar_t *picked[3];
	WfSession s;

	memset(&s, 0, sizeof s);
	s.dir = _wcsdup(L"C:\\dev\\\x30c4\x30fc\x30eb\\tool name");
	s.entry = _wcsdup(L"\x65e5\x672c\x8a9e #1 (copy).txt");
	s.cursor = 12;
	s.top = 7;
	picked[0] = _wcsdup(L"a.txt");
	picked[1] = _wcsdup(L"emoji \xD83D\xDE00.txt"); /* a surrogate pair */
	picked[2] = _wcsdup(L"line\nbreak"); /* a name that is not a name */
	s.selected = malloc(sizeof picked);
	memcpy(s.selected, picked, sizeof picked);
	s.n_selected = 3;
	s.pane = WF_SESSION_PANE_PREVIEW;
	s.pv_top = 40;
	s.pv_left = 3;
	return s;
}

static void unicode_and_odd_names_round_trip(void)
{
	WfSession s = sample(), back;
	char *text;
	size_t n;

	CHECK(WF_OK == wf_session_encode(&s, &text, &n));
	CHECK(text && n > 0 && '\n' == text[n - 1]);
	CHECK(WF_OK == wf_session_decode(text, n, &back));
	CHECK(back.dir && back.entry);
	CHECK(!wcscmp(back.dir, s.dir) && !wcscmp(back.entry, s.entry));
	CHECK(12 == back.cursor && 7 == back.top && 40 == back.pv_top &&
	        3 == back.pv_left);
	CHECK(WF_SESSION_PANE_PREVIEW == back.pane);
	/* the name with a line break is not a name inside a folder: left out */
	CHECK(2 == back.n_selected);
	CHECK(!wcscmp(back.selected[0], L"a.txt") &&
	        !wcscmp(back.selected[1], L"emoji \xD83D\xDE00.txt"));
	/* nothing in the text can end a record early */
	{
		size_t lines = 0;

		for (size_t i = 0; i < n; i++) {
			lines += '\n' == text[i];
		}
		CHECK(7 == lines); /* dir entry cursor pane pv sel sel */
	}
	free(text);
	wf_session_free(&back);
	wf_session_free(&s);
}

static void encode_refuses_what_it_cannot_say(void)
{
	WfSession s;
	char *text = NULL;
	size_t n;

	memset(&s, 0, sizeof s);
	CHECK(WF_E_INVAL == wf_session_encode(&s, &text, &n)); /* no folder */
	s.dir = _wcsdup(L"relative\\path");
	CHECK(WF_E_INVAL == wf_session_encode(&s, &text, &n));
	free(s.dir);
	s.dir = _wcsdup(L"C:\\a\\..\\b");
	CHECK(WF_E_INVAL == wf_session_encode(&s, &text, &n));
	free(s.dir);
	s.dir = _wcsdup(L"C:\\ok\xD800"); /* a lone surrogate */
	CHECK(WF_E_INVAL == wf_session_encode(&s, &text, &n));
	free(s.dir);
	s.dir = NULL;
	CHECK(!text);
}

static WfStatus decode_text(const char *text)
{
	WfSession s;
	WfStatus st = wf_session_decode(text, strlen(text), &s);

	if (WF_OK == st) {
		wf_session_free(&s);
	} else {
		CHECK(!s.dir && !s.entry && !s.selected && 0 == s.n_selected);
	}
	return st;
}

static void every_kind_of_damage_is_refused_whole(void)
{
	/* in the text a backslash of a path is written twice (\\), so a path
	 * "C:\ok" is `C:\\ok` there and "C:\\\\ok" in these C strings */
	char big[WF_SESSION_MAX_BYTES + 64], many[40000];
	size_t used = 0;

	CHECK(WF_OK == decode_text("dir C:\\\\ok\n"));
	CHECK(WF_OK == decode_text("dir C:\\\\ok\nentry a b\nsel x\nsel y\n"));
	CHECK(WF_E_CORRUPT == decode_text("")); /* nothing: no folder */
	/* a NUL, raw or written as an escape, is refused wherever it stands */
	{
		static const char raw[] = "dir C:\\\\ok\0ignored\n";
		WfSession s;

		CHECK(WF_E_CORRUPT ==
		        wf_session_decode(raw, sizeof raw - 1, &s));
	}
	CHECK(WF_E_CORRUPT == decode_text("dir C:\\\\ok\\x00ignored\n"));
	CHECK(WF_E_CORRUPT == decode_text("dir C:\\\\ok\nentry a\\x00b c\n"));
	CHECK(WF_E_CORRUPT ==
	        decode_text("dir C:\\\\ok\nentry a b\nsel x\\x00y\n"));
	CHECK(WF_OK ==
	        decode_text(
	                "dir C:\\\\ok\\x41\n")); /* other escapes still read */
	CHECK(WF_E_CORRUPT == decode_text("entry a\n"));    /* no folder */
	CHECK(WF_E_CORRUPT == decode_text("dir C:\\\\ok")); /* cut short */
	CHECK(WF_E_CORRUPT == decode_text("dir C:\\\\ok\nsurprise 1\n"));
	CHECK(WF_E_CORRUPT == decode_text("dir C:\\\\ok\ndir C:\\\\other\n"));
	CHECK(WF_E_CORRUPT ==
	        decode_text("dir C:\\\\ok\ncursor 1 2\ncursor 1 2\n"));
	CHECK(WF_E_CORRUPT == decode_text("dir C:\\\\ok\nentry a\nentry b\n"));
	CHECK(WF_E_CORRUPT ==
	        decode_text("dir C:\\\\ok\npane list\npane full\n"));
	CHECK(WF_E_CORRUPT == decode_text("dir C:\\\\ok\npane sideways\n"));
	CHECK(WF_E_CORRUPT == decode_text("dir C:\\\\ok\ncursor 1\n"));
	CHECK(WF_E_CORRUPT == decode_text("dir C:\\\\ok\ncursor 1 2 3\n"));
	CHECK(WF_E_CORRUPT == decode_text("dir C:\\\\ok\ncursor -1 2\n"));
	CHECK(WF_E_CORRUPT == decode_text("dir C:\\\\ok\ncursor a 2\n"));
	CHECK(WF_E_CORRUPT ==
	        decode_text("dir C:\\\\ok\ncursor 99999999999 2\n"));
	CHECK(WF_E_CORRUPT == decode_text("dir C:\\\\ok\npv 1 x\n"));
	CHECK(WF_E_CORRUPT ==
	        decode_text("dir C:\\\\ok\nsel\n")); /* no space */
	/* the folder must be a place: absolute, no . or .., not extended */
	CHECK(WF_E_CORRUPT == decode_text("dir relative\n"));
	CHECK(WF_E_CORRUPT == decode_text("dir C:\\\\a\\\\..\\\\b\n"));
	CHECK(WF_E_CORRUPT == decode_text("dir C:\\\\a\\\\.\\\\b\n"));
	CHECK(WF_E_CORRUPT == decode_text("dir \\\\\\\\?\\\\C:\\\\a\n"));
	CHECK(WF_E_CORRUPT == decode_text("dir C:/a/b\n"));
	/* escapes: an unknown one, a short one, and bytes that are not UTF-8 */
	CHECK(WF_E_CORRUPT == decode_text("dir C:\\\\a\\q\n"));
	CHECK(WF_E_CORRUPT == decode_text("dir C:\\\\a\\x4\n"));
	CHECK(WF_E_CORRUPT == decode_text("dir C:\\\\a\\xZZ\n"));
	CHECK(WF_E_CORRUPT == decode_text("dir C:\\\\a\\xC3\n"));
	CHECK(WF_E_CORRUPT == decode_text("dir C:\\\\a\\xFF\\xFE\n"));
	CHECK(WF_E_CORRUPT ==
	        decode_text("dir C:\\\\a\\x0A\n")); /* a line break */
	/* names are one name: no separator, no control character */
	CHECK(WF_E_CORRUPT == decode_text("dir C:\\\\ok\nentry a\\\\b\n"));
	CHECK(WF_E_CORRUPT == decode_text("dir C:\\\\ok\nentry a/b\n"));
	CHECK(WF_E_CORRUPT == decode_text("dir C:\\\\ok\nsel a\\x01b\n"));
	CHECK(WF_E_CORRUPT ==
	        decode_text("dir C:\\\\ok\nentry \n")); /* empty */
	/* what the text may say in a valid escape is read back as it was */
	{
		WfSession s;

		CHECK(WF_OK ==
		        wf_session_decode("dir C:\\\\a\\x20b\nentry t\\x41\n",
		                strlen("dir C:\\\\a\\x20b\nentry t\\x41\n"),
		                &s));
		CHECK(!wcscmp(s.dir, L"C:\\a b") && !wcscmp(s.entry, L"tA"));
		wf_session_free(&s);
	}
	/* limits: one path too long, too many names, the whole too large */
	memset(big, 'x', sizeof big);
	memcpy(big, "dir C:\\\\", 8);
	big[WF_SESSION_MAX_PATH + 20] = '\n';
	big[WF_SESSION_MAX_PATH + 21] = '\0';
	CHECK(WF_E_CORRUPT == decode_text(big));
	used += (size_t)snprintf(many, sizeof many, "dir C:\\\\ok\n");
	for (int i = 0; i <= (int)WF_SESSION_MAX_SELECTED; i++) {
		used += (size_t)snprintf(
		        many + used, sizeof many - used, "sel n%d\n", i);
	}
	CHECK(WF_E_CORRUPT == decode_text(many));
	{
		WfSession s;

		memset(big, 'y', sizeof big);
		memcpy(big, "dir C:\\\\ok\n", 11);
		big[WF_SESSION_MAX_BYTES + 32] = '\n';
		CHECK(WF_E_CORRUPT ==
		        wf_session_decode(big, WF_SESSION_MAX_BYTES + 33, &s));
	}
	{
		/* a NUL inside the text: damage wherever it is, in a name or a key */
		static const char in_name[] = "dir C:\\\\o\0k\n";
		static const char in_key[] = "dir C:\\\\ok\nen\0try a\n";
		WfSession s;

		CHECK(WF_E_CORRUPT ==
		        wf_session_decode(in_name, sizeof in_name - 1, &s));
		CHECK(WF_E_CORRUPT ==
		        wf_session_decode(in_key, sizeof in_key - 1, &s));
		/* a NUL where nothing else about the line is wrong: only the check for
		 * it can refuse these (a name with a NUL in it reads as one name) */
		static const char in_entry[] = "entry a\0b\n";
		static const char in_value_end[] = "dir C:\\\\ok\0\n";
		static const char in_selected[] = "dir C:\\\\ok\nsel a\0\n";

		CHECK(WF_E_CORRUPT ==
		        wf_session_decode(in_entry, sizeof in_entry - 1, &s));
		CHECK(WF_E_CORRUPT ==
		        wf_session_decode(in_value_end, sizeof in_value_end - 1,
		                &s));
		CHECK(WF_E_CORRUPT ==
		        wf_session_decode(in_selected, sizeof in_selected - 1,
		                &s));
		CHECK(NULL == s.dir &&
		        0 == s.n_selected); /* nothing is left to free */
	}
}

/* Freeing what was never made is allowed, and freeing twice is too. */
static void freeing_nothing_is_safe(void)
{
	WfSession s;

	wf_session_free(NULL);
	memset(&s, 0, sizeof s);
	wf_session_free(&s);
	wf_session_free(&s);
	CHECK(NULL == s.dir);
}

static void a_hostile_session_cannot_name_anything_but_a_place(void)
{
	/* Command records and editor records without position fields are invalid.
	 * A structured editor-position record is tested separately. */
	static const char *const words[] = {"open", "write", "run", "exec",
	        "save", "delete", "copy", "move", "edit", "cmd", "shell"};

	for (size_t i = 0; i < sizeof words / sizeof *words; i++) {
		char text[96];

		snprintf(text, sizeof text, "dir C:\\\\ok\n%s C:\\\\x\n",
		        words[i]);
		CHECK(WF_E_CORRUPT == decode_text(text));
	}
}

static void paths_that_are_places(void)
{
	CHECK(wf_session_path_ok(L"C:\\"));
	CHECK(wf_session_path_ok(L"C:\\a b\\c"));
	CHECK(wf_session_path_ok(L"\\\\server\\share"));
	CHECK(wf_session_path_ok(L"\\\\server\\share\\x"));
	CHECK(wf_session_path_ok(L"\\\\wsl.localhost\\Ubuntu\\home"));
	CHECK(!wf_session_path_ok(NULL));
	CHECK(!wf_session_path_ok(L""));
	CHECK(!wf_session_path_ok(L"C:"));
	CHECK(!wf_session_path_ok(L"C:a"));
	CHECK(!wf_session_path_ok(L"\\\\"));
	CHECK(!wf_session_path_ok(L"\\\\server"));
	CHECK(!wf_session_path_ok(L"\\\\server\\"));
	CHECK(!wf_session_path_ok(L"\\\\server\\\\file"));
	CHECK(WF_E_CORRUPT == decode_text("dir \\\\\\\\server\n"));
	CHECK(!wf_session_path_ok(L"\\\\?\\C:\\a"));
	CHECK(!wf_session_path_ok(L"\\\\.\\COM1"));
	CHECK(!wf_session_path_ok(L"C:\\a\\..\\b"));
	CHECK(!wf_session_path_ok(L"C:\\a\\.."));
	CHECK(!wf_session_path_ok(L"C:\\a\x01"));
	CHECK(!wf_session_path_ok(L"1:\\a"));
	CHECK(!wf_session_path_ok(L"\x30c4:\\a"));
}

static void put_file(const wchar_t *name, const char *bytes)
{
	wchar_t path[MAX_PATH + 64];
	HANDLE h;
	DWORD n;

	swprintf(path, MAX_PATH + 64, L"%ls\\%ls", g_dir, name);
	h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	if (INVALID_HANDLE_VALUE != h) {
		WriteFile(h, bytes, (DWORD)strlen(bytes), &n, NULL);
		CloseHandle(h);
	}
}

static long long file_size(const wchar_t *name)
{
	wchar_t path[MAX_PATH + 64];
	WIN32_FILE_ATTRIBUTE_DATA d;

	swprintf(path, MAX_PATH + 64, L"%ls\\%ls", g_dir, name);
	if (!GetFileAttributesExW(path, GetFileExInfoStandard, &d)) {
		return -1;
	}
	return ((long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}

static char *file_text(const wchar_t *name)
{
	wchar_t path[MAX_PATH + 64];
	HANDLE h;
	char *buf = calloc(1, 4096);
	DWORD n = 0;

	swprintf(path, MAX_PATH + 64, L"%ls\\%ls", g_dir, name);
	h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
	        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE != h) {
		ReadFile(h, buf, 4095, &n, NULL);
		CloseHandle(h);
	}
	return buf;
}

static WfSession tiny(const wchar_t *dir)
{
	WfSession s;

	memset(&s, 0, sizeof s);
	s.dir = _wcsdup(dir);
	return s;
}

static void write_session(WfSessionStore *st, const wchar_t *dir)
{
	WfSession s = tiny(dir);
	char *text;
	size_t n;

	CHECK(WF_OK == wf_session_encode(&s, &text, &n));
	CHECK(WF_OK == wf_session_store_write(st, text, n));
	free(text);
	wf_session_free(&s);
}

static bool reads_dir(WfSessionStore *st, const wchar_t *dir)
{
	WfSession got;
	bool same = WF_SESSION_FOUND == wf_session_store_read(st, &got, NULL) &&
	        !wcscmp(got.dir, dir);

	wf_session_free(&got);
	return same;
}

static void nothing_is_saved_until_something_is_written(void)
{
	WfSessionStore *st;
	WfSession got;

	CHECK(WF_OK == wf_session_store_open(g_dir, &st));
	CHECK(WF_SESSION_NONE == wf_session_store_read(st, &got, NULL));
	CHECK(-1 == file_size(L"session.a") && -1 == file_size(L"session.b"));
	/* reading never creates a file; writing needs the right to write */
	CHECK(WF_E_ACCESS == wf_session_store_write(st, "dir C:\\a\n", 9));
	CHECK(WF_E_ACCESS == wf_session_store_forget(st));
	wf_session_store_close(st);
	CHECK(-1 == file_size(L"session.a"));
}

static void the_newest_slot_wins_and_a_cut_short_one_is_ignored(void)
{
	WfSessionStore *st;
	WfSession got;

	CHECK(WF_OK == wf_session_store_open(g_dir, &st));
	CHECK(wf_session_store_lock(st));
	write_session(st, L"C:\\one");
	CHECK(reads_dir(st, L"C:\\one"));
	write_session(st, L"C:\\two");
	CHECK(reads_dir(st, L"C:\\two"));
	write_session(st, L"C:\\three");
	CHECK(reads_dir(st, L"C:\\three"));
	/* the newest slot is cut short (a crash while it was written): the
	 * other one, whole, is what a reader gets */
	{
		char *a = file_text(L"session.a"), *b = file_text(L"session.b");
		long long seq_a = strstr(a, "\nseq ")
		        ? atoll(strstr(a, "\nseq ") + 5)
		        : 0,
		          seq_b = strstr(b, "\nseq ")
		        ? atoll(strstr(b, "\nseq ") + 5)
		        : 0;

		put_file(seq_a > seq_b ? L"session.a" : L"session.b",
		        "piero-session 1\nseq 99\nsize 50\n--\ndir C:\\par");
		free(a);
		free(b);
	}
	CHECK(WF_SESSION_FOUND == wf_session_store_read(st, &got, NULL));
	CHECK(!wcscmp(got.dir, L"C:\\two"));
	wf_session_free(&got);
	/* the next write goes over the cut-short slot and wins again */
	write_session(st, L"C:\\four");
	CHECK(reads_dir(st, L"C:\\four"));
	wf_session_store_close(st);
}

static void damage_in_both_slots_reads_as_damaged_not_as_nothing(void)
{
	WfSessionStore *st;
	WfSession got;

	CHECK(WF_OK == wf_session_store_open(g_dir, &st));
	put_file(
	        L"session.a", "piero-session 1\nseq 1\nsize 99\n--\ndir C:\\x");
	put_file(L"session.b", "garbage that is not a record at all");
	CHECK(WF_SESSION_DAMAGED == wf_session_store_read(st, &got, NULL));
	CHECK(!got.dir);
	/* a whole record that says something unreadable is damaged too */
	put_file(L"session.a",
	        "piero-session 1\nseq 1\nsize 11\n--\nentry only\n");
	put_file(L"session.b", "");
	CHECK(WF_SESSION_DAMAGED == wf_session_store_read(st, &got, NULL));
	CHECK(wf_session_store_lock(st));
	CHECK(WF_OK == wf_session_store_forget(st));
	wf_session_store_close(st);
}

static void a_newer_version_is_left_exactly_as_it_is(void)
{
	WfSessionStore *st;
	WfSession got;
	int version = 0;
	char *before, *after;

	CHECK(WF_OK == wf_session_store_open(g_dir, &st));
	CHECK(wf_session_store_lock(st));
	write_session(st, L"C:\\mine");
	put_file(L"session.a", "piero-session 7\nfuture data\n");
	before = file_text(L"session.a");
	CHECK(WF_SESSION_UNSUPPORTED ==
	        wf_session_store_read(st, &got, &version));
	CHECK(7 == version);
	/* neither read nor replaced, not even through the other slot */
	CHECK(WF_E_FORMAT == wf_session_store_write(st, "dir C:\\x\n", 9));
	after = file_text(L"session.a");
	CHECK(!strcmp(before, after));
	free(before);
	free(after);
	put_file(L"session.a", "piero-session 9999999999999999\n");
	CHECK(WF_SESSION_DAMAGED == wf_session_store_read(st, &got, NULL));
	CHECK(WF_E_CORRUPT == wf_session_store_write(st, "", 0));
	put_file(L"session.a", "piero-session 01\n");
	CHECK(WF_SESSION_DAMAGED == wf_session_store_read(st, &got, NULL));
	wf_session_store_close(st);
	put_file(L"session.a", "");
	put_file(L"session.b", "");
}

static void a_failed_forget_does_not_allow_overwriting_damage(void)
{
	static const char damaged[] = "broken session header\n";
	WfSessionStore *store;
	wchar_t path[MAX_PATH + 64];
	char *after;
	HANDLE reader;

	put_file(L"session.a", damaged);
	put_file(L"session.b", "");
	CHECK(WF_OK == wf_session_store_open(g_dir, &store));
	CHECK(wf_session_store_lock(store));
	swprintf(path, MAX_PATH + 64, L"%ls\\session.a", g_dir);
	reader = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(reader != INVALID_HANDLE_VALUE);
	CHECK(WF_OK != wf_session_store_forget(store));
	if (reader != INVALID_HANDLE_VALUE) {
		CloseHandle(reader);
	}
	CHECK(WF_E_CORRUPT == wf_session_store_write(store, "", 0));
	after = file_text(L"session.a");
	CHECK(after && !strcmp(after, damaged));
	free(after);
	CHECK(WF_OK == wf_session_store_forget(store));
	write_session(store, L"C:\\after-forget");
	CHECK(reads_dir(store, L"C:\\after-forget"));
	wf_session_store_close(store);
}

static void forgetting_empties_the_files_and_keeps_them(void)
{
	WfSessionStore *st;
	WfSession got;

	CHECK(WF_OK == wf_session_store_open(g_dir, &st));
	CHECK(wf_session_store_lock(st));
	write_session(st, L"C:\\a");
	write_session(st, L"C:\\b");
	CHECK(WF_OK == wf_session_store_forget(st));
	CHECK(0 == file_size(L"session.a") && 0 == file_size(L"session.b"));
	CHECK(WF_SESSION_NONE == wf_session_store_read(st, &got, NULL));
	/* forgetting again, with nothing there, is not an error */
	CHECK(WF_OK == wf_session_store_forget(st));
	/* and a session can be saved again afterwards */
	write_session(st, L"C:\\c");
	CHECK(reads_dir(st, L"C:\\c"));
	wf_session_store_close(st);
}

/* A slot that is a second name of another file is neither written nor emptied:
 * both go through one check, and the other file keeps what it had. */
static void a_slot_that_is_another_files_second_name_is_never_changed(void)
{
	WfSessionStore *st;
	wchar_t slot[MAX_PATH + 64];
	char *text;

	put_file(L"precious.txt", "PRECIOUS");
	swprintf(slot, MAX_PATH + 64, L"%ls\\session.a", g_dir);
	DeleteFileW(slot);
	{
		wchar_t target[MAX_PATH + 64];

		swprintf(target, MAX_PATH + 64, L"%ls\\precious.txt", g_dir);
		CHECK(CreateHardLinkW(slot, target, NULL));
	}
	CHECK(WF_OK == wf_session_store_open(g_dir, &st));
	CHECK(wf_session_store_lock(st));
	/* forgetting: refused, and the other file is not emptied */
	CHECK(WF_OK != wf_session_store_forget(st));
	text = file_text(L"precious.txt");
	CHECK(text && !strcmp(text, "PRECIOUS"));
	free(text);
	CHECK(8 == file_size(L"precious.txt"));
	/* saving: the same, whichever slot is chosen first; the one that is the
	 * second name is refused, the other may be written */
	{
		WfSession one = tiny(L"C:\\a");
		char *body;
		size_t n;

		CHECK(WF_OK == wf_session_encode(&one, &body, &n));
		(void)wf_session_store_write(st, body, n);
		(void)wf_session_store_write(st, body, n);
		free(body);
		wf_session_free(&one);
	}
	text = file_text(L"precious.txt");
	CHECK(text && !strcmp(text, "PRECIOUS"));
	free(text);
	wf_session_store_close(st);
	DeleteFileW(slot);
	swprintf(slot, MAX_PATH + 64, L"%ls\\precious.txt", g_dir);
	DeleteFileW(slot);
}

static void only_one_process_holds_the_right_to_write(void)
{
	WfSessionStore *first, *second;

	CHECK(WF_OK == wf_session_store_open(g_dir, &first));
	CHECK(WF_OK == wf_session_store_open(g_dir, &second));
	CHECK(wf_session_store_lock(first));
	CHECK(wf_session_store_lock(first)); /* again: still the holder */
	CHECK(!wf_session_store_lock(second));
	CHECK(!wf_session_store_locked(second));
	CHECK(WF_E_ACCESS == wf_session_store_write(second, "dir C:\\a\n", 9));
	CHECK(WF_E_ACCESS == wf_session_store_forget(second));
	/* the other one can still read what the holder wrote */
	write_session(first, L"C:\\shared");
	CHECK(reads_dir(second, L"C:\\shared"));
	/* when the holder is gone (here: closed; the system does the same when
	 * a process ends in any way) the right is free again */
	wf_session_store_close(first);
	CHECK(wf_session_store_lock(second));
	write_session(second, L"C:\\second");
	CHECK(reads_dir(second, L"C:\\second"));
	wf_session_store_close(second);
}

static void a_lock_name_that_is_not_a_file_is_refused(void)
{
	WfSessionStore *st;
	wchar_t path[MAX_PATH + 64];

	swprintf(path, MAX_PATH + 64, L"%ls\\lockdir", g_dir);
	CHECK(CreateDirectoryW(path, NULL));
	CHECK(WF_OK == wf_session_store_open(path, &st));
	/* the folder holds no session.lock: it is made; but when the name is
	 * taken by a folder, the lock is refused */
	wcscat_s(path, MAX_PATH + 64, L"\\session.lock");
	CHECK(CreateDirectoryW(path, NULL));
	CHECK(!wf_session_store_lock(st));
	wf_session_store_close(st);
	RemoveDirectoryW(path);
	swprintf(path, MAX_PATH + 64, L"%ls\\lockdir", g_dir);
	RemoveDirectoryW(path);
}

static void an_unusable_folder_is_an_error_not_a_guess(void)
{
	WfSessionStore *st;
	WfSession got;

	CHECK(WF_E_INVAL == wf_session_store_open(NULL, &st));
	CHECK(WF_E_INVAL == wf_session_store_open(L"", &st));
	CHECK(WF_OK == wf_session_store_open(L"C:\\no\\such\\folder", &st));
	CHECK(WF_SESSION_NONE == wf_session_store_read(st, &got, NULL));
	CHECK(!wf_session_store_lock(st));
	wf_session_store_close(st);
}

static char *record(const wchar_t *dir, size_t *n)
{
	WfSession s = tiny(dir);
	char *text;

	CHECK(WF_OK == wf_session_encode(&s, &text, n));
	wf_session_free(&s);
	return text;
}

static void the_writer_writes_the_latest_state_and_flushes_on_stop(void)
{
	WfSessionStore *st;
	WfSessionWriter *w;
	size_t n;

	CHECK(WF_OK == wf_session_store_open(g_dir, &st));
	CHECK(WF_OK == wf_session_writer_start(st, &w));
	for (int i = 0; i < 40; i++) {
		wchar_t dir[40];
		char *text;

		swprintf(dir, 40, L"C:\\burst\\%d", i);
		text = record(dir, &n);
		CHECK(WF_OK == wf_session_writer_submit(w, text, n));
	}
	/* a normal stop writes what is pending: the last one is on disk */
	wf_session_writer_stop(w, 5000);
	CHECK(reads_dir(st, L"C:\\burst\\39"));
	wf_session_store_close(st);
}

/* The record of what was handed over skips a repeat of the state, and is dropped
 * when the state did not reach the disk: "handed over" is not "saved". The same
 * state is then offered again, and written once the writer can. */
static void a_state_that_was_not_saved_is_handed_over_again(void)
{
	WfSessionStore *st;
	WfSessionWriter *w;
	WfSessionDedupe seen = {NULL, 0};
	size_t n;
	char *text;
	ULONGLONG until;

	/* the plain rules of the record */
	CHECK(!wf_dedupe_is_repeat(&seen, "a", 1));
	CHECK(wf_dedupe_remember(&seen, "a", 1));
	CHECK(wf_dedupe_is_repeat(&seen, "a", 1) &&
	        !wf_dedupe_is_repeat(&seen, "b", 1));
	CHECK(!wf_dedupe_is_repeat(&seen, "a", 0));
	wf_dedupe_forget(&seen);
	CHECK(!wf_dedupe_is_repeat(&seen, "a", 1));

	CHECK(WF_OK == wf_session_store_open(g_dir, &st));
	CHECK(WF_OK == wf_session_writer_start(st, &w));
	/* the files hold a newer version: the write of state A is refused, and said */
	put_file(L"session.a", "piero-session 9\nnewer\n");
	{
		WfSession got;

		CHECK(WF_SESSION_UNSUPPORTED ==
		        wf_session_store_read(st, &got, NULL));
	}
	text = record(L"C:\\saved-later", &n);
	CHECK(!wf_dedupe_is_repeat(&seen, text, n));
	(void)wf_dedupe_remember(&seen, text, n); /* handed over: remembered */
	CHECK(WF_OK == wf_session_writer_submit(w, text, n));
	until = GetTickCount64() + 5000;
	while (!wf_session_writer_take_failure(w) && GetTickCount64() < until) {
		Sleep(10);
	}
	CHECK(GetTickCount64() < until);
	/* the same state, as it was: still "handed over" until the failure is known */
	text = record(L"C:\\saved-later", &n);
	CHECK(wf_dedupe_is_repeat(&seen, text, n));
	wf_dedupe_forget(&seen); /* what the loop does on the failure */
	CHECK(!wf_dedupe_is_repeat(&seen, text, n)); /* offered again */
	/* Explicitly forgetting the incompatible data permits the retry. */
	CHECK(WF_OK == wf_session_store_forget(st));
	{
		WfSession got;

		/* the right to write is held: the files are looked at again */
		CHECK(WF_SESSION_NONE == wf_session_store_read(st, &got, NULL));
	}
	CHECK(WF_OK == wf_session_writer_submit(w, text, n));
	wf_session_writer_stop(w, 5000);
	CHECK(reads_dir(st, L"C:\\saved-later"));
	wf_session_store_close(st);
	wf_dedupe_forget(&seen);
}

static void the_writer_needs_the_right_to_write_and_tells_a_failure_once(void)
{
	WfSessionStore *first, *second;
	WfSessionWriter *w, *none;
	size_t n;
	char *text;
	ULONGLONG until;

	CHECK(WF_OK == wf_session_store_open(g_dir, &first));
	CHECK(WF_OK == wf_session_store_open(g_dir, &second));
	CHECK(WF_OK == wf_session_writer_start(first, &w));
	CHECK(WF_E_ACCESS == wf_session_writer_start(second, &none) && !none);
	CHECK(!wf_session_writer_take_failure(w));
	/* the files hold a newer version: every write is refused, and said */
	put_file(L"session.a", "piero-session 9\nnewer\n");
	{
		WfSession got;

		CHECK(WF_SESSION_UNSUPPORTED ==
		        wf_session_store_read(first, &got, NULL));
	}
	text = record(L"C:\\x", &n);
	CHECK(WF_OK == wf_session_writer_submit(w, text, n));
	until = GetTickCount64() + 5000;
	while (!wf_session_writer_take_failure(w) && GetTickCount64() < until) {
		Sleep(10);
	}
	CHECK(GetTickCount64() < until);           /* a failure was reported */
	CHECK(!wf_session_writer_take_failure(w)); /* ... once */
	CHECK(0 == wf_session_writer_written(w));
	wf_session_writer_stop(w, 5000);
	wf_session_store_close(first);
	wf_session_store_close(second);
	put_file(L"session.a", "");
	put_file(L"session.b", "");
}

/* A run that only saves never read the files: a newer version's data is found
 * when the right to write is taken, and no write replaces it. */
static void a_saving_only_run_leaves_a_newer_version_alone(void)
{
	WfSessionStore *st;
	WfSessionWriter *w;
	size_t n;
	char *text;
	ULONGLONG until;
	static const char newer[] = "piero-session 9\nnewer data\n";

	put_file(L"session.a", newer);
	put_file(L"session.b", "");
	CHECK(WF_OK == wf_session_store_open(g_dir, &st));
	CHECK(WF_OK == wf_session_writer_start(st, &w));
	text = record(L"C:\\x", &n);
	CHECK(WF_OK == wf_session_writer_submit(w, text, n));
	until = GetTickCount64() + 5000;
	while (!wf_session_writer_take_failure(w) && GetTickCount64() < until) {
		Sleep(10);
	}
	CHECK(GetTickCount64() < until); /* the refusal is told */
	CHECK(0 == wf_session_writer_written(w));
	wf_session_writer_stop(w, 5000);
	CHECK((long long)(sizeof newer - 1) == file_size(L"session.a"));
	CHECK(0 == file_size(L"session.b"));
	wf_session_store_close(st);
	put_file(L"session.a", "");
}

static void a_stopped_writer_takes_nothing_and_stopping_nothing_is_fine(void)
{
	WfWorkerStopReport r = wf_session_writer_stop(NULL, 10);

	CHECK(WF_OK == r.status);
	CHECK(WF_E_INVAL == wf_session_writer_submit(NULL, _strdup("x"), 1));
}

static void the_editor_record_round_trips_and_is_checked(void)
{
	WfSession s = tiny(L"C:\\dev\\x"), back;
	char *text;
	size_t n;

	s.has_edit = true;
	s.edit.path = _wcsdup(L"C:\\dev\\x\\\x65e5\x672c #1.txt");
	s.edit.line = 41;
	s.edit.col = 7;
	s.edit.top = 30;
	s.edit.left = 2;
	s.edit.modified = true;
	CHECK(WF_OK == wf_session_encode(&s, &text, &n));
	CHECK(WF_OK == wf_session_decode(text, n, &back));
	CHECK(back.has_edit && !wcscmp(back.edit.path, s.edit.path));
	CHECK(41 == back.edit.line && 7 == back.edit.col &&
	        30 == back.edit.top && 2 == back.edit.left &&
	        back.edit.modified);
	free(text);
	wf_session_free(&back);
	/* an editor path that is not a place leaves the editor out, not the record */
	free(s.edit.path);
	s.edit.path = _wcsdup(L"relative.txt");
	CHECK(WF_OK == wf_session_encode(&s, &text, &n));
	CHECK(WF_OK == wf_session_decode(text, n, &back) && !back.has_edit);
	free(text);
	wf_session_free(&back);
	wf_session_free(&s);
	/* damage in the record is damage of the whole */
	CHECK(WF_E_CORRUPT ==
	        decode_text(
	                "dir C:\\\\ok\nedit 1 2 3 4 2 C:\\\\ok\\\\a\n")); /* modified 2 */
	CHECK(WF_E_CORRUPT == decode_text("dir C:\\\\ok\nedit 1 2 3 4 0\n"));
	CHECK(WF_E_CORRUPT ==
	        decode_text("dir C:\\\\ok\nedit 1 2 3 0 C:\\\\a\n"));
	CHECK(WF_E_CORRUPT ==
	        decode_text("dir C:\\\\ok\nedit 1 2 3 4 0 relative\n"));
	CHECK(WF_E_CORRUPT ==
	        decode_text(
	                "dir C:\\\\ok\nedit 1 2 3 4 0 C:\\\\a\\\\..\\\\b\n"));
	CHECK(WF_E_CORRUPT ==
	        decode_text("dir C:\\\\ok\nedit -1 2 3 4 0 C:\\\\a\n"));
	CHECK(WF_E_CORRUPT ==
	        decode_text("dir C:\\\\ok\nedit 1 2 3 4 0 C:\\\\a\\x\n"));
	CHECK(WF_E_CORRUPT ==
	        decode_text("dir C:\\\\ok\nedit 1 2 3 4 0 C:\\\\a\n"
	                    "edit 1 2 3 4 0 C:\\\\b\n")); /* twice */
	CHECK(WF_OK == decode_text("dir C:\\\\ok\nedit 1 2 3 4 0 C:\\\\a\n"));
	CHECK(WF_OK ==
	        decode_text(
	                "dir C:\\\\ok\nedit 0 0 0 0 1 C:\\\\a b\\\\c d.txt\n"));
}

/* The test program started again, with the folder to hold in the environment:
 * it takes the right to write, leaves a marker file, and waits to be killed. */
static int hold_the_lock_until_killed(const wchar_t *dir)
{
	WfSessionStore *st;
	wchar_t marker[MAX_PATH + 64];
	HANDLE h;

	if (WF_OK != wf_session_store_open(dir, &st) ||
	        !wf_session_store_lock(st)) {
		return 2;
	}
	swprintf(marker, MAX_PATH + 64, L"%ls\\held.txt", dir);
	h = CreateFileW(marker, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return 3;
	}
	CloseHandle(h);
	Sleep(60000); /* the parent ends this process; nothing is closed here */
	return 0;
}

/* The right to write is a handle, so it is the system that gives it back when
 * the process that holds it ends in any way, including being killed. */
static void the_right_to_write_is_given_back_when_its_holder_is_killed(void)
{
	wchar_t self[MAX_PATH], marker[MAX_PATH + 64], command[MAX_PATH + 8];
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	WfSessionStore *st;
	ULONGLONG until;
	bool held = false, free_again = false;

	GetModuleFileNameW(NULL, self, MAX_PATH);
	swprintf(command, MAX_PATH + 8, L"\"%ls\"", self);
	swprintf(marker, MAX_PATH + 64, L"%ls\\held.txt", g_dir);
	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	SetEnvironmentVariableW(L"WF_SESSION_TEST_HOLD", g_dir);
	CHECK(CreateProcessW(self, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
	        NULL, NULL, &si, &pi));
	SetEnvironmentVariableW(L"WF_SESSION_TEST_HOLD", NULL);
	if (!pi.hProcess) {
		return;
	}
	until = GetTickCount64() + 10000;
	while (INVALID_FILE_ATTRIBUTES == GetFileAttributesW(marker) &&
	        GetTickCount64() < until) {
		Sleep(20);
	}
	CHECK(INVALID_FILE_ATTRIBUTES != GetFileAttributesW(marker));
	CHECK(WF_OK == wf_session_store_open(g_dir, &st));
	held = !wf_session_store_lock(st); /* while it lives, it is not ours */
	CHECK(held);
	TerminateProcess(pi.hProcess, 9);
	CHECK(WAIT_OBJECT_0 == WaitForSingleObject(pi.hProcess, 5000));
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	until = GetTickCount64() + 5000;
	while (!(free_again = wf_session_store_lock(st)) &&
	        GetTickCount64() < until) {
		Sleep(20);
	}
	CHECK(free_again); /* ... and once it is gone, it is */
	wf_session_store_close(st);
	DeleteFileW(marker);
}

int main(void)
{
	wchar_t tmp[MAX_PATH], hold[MAX_PATH];

	if (GetEnvironmentVariableW(L"WF_SESSION_TEST_HOLD", hold, MAX_PATH)) {
		return hold_the_lock_until_killed(hold);
	}
	GetTempPathW(MAX_PATH, tmp);
	swprintf(g_dir, MAX_PATH + 32, L"%lswf-session-%lu", tmp,
	        GetCurrentProcessId());
	if (!CreateDirectoryW(g_dir, NULL)) {
		CHECK(0 && "the test folder could not be made");
		return TEST_RESULT();
	}
	unicode_and_odd_names_round_trip();
	encode_refuses_what_it_cannot_say();
	every_kind_of_damage_is_refused_whole();
	freeing_nothing_is_safe();
	a_hostile_session_cannot_name_anything_but_a_place();
	paths_that_are_places();
	nothing_is_saved_until_something_is_written();
	the_newest_slot_wins_and_a_cut_short_one_is_ignored();
	damage_in_both_slots_reads_as_damaged_not_as_nothing();
	a_newer_version_is_left_exactly_as_it_is();
	a_failed_forget_does_not_allow_overwriting_damage();
	forgetting_empties_the_files_and_keeps_them();
	a_slot_that_is_another_files_second_name_is_never_changed();
	only_one_process_holds_the_right_to_write();
	a_lock_name_that_is_not_a_file_is_refused();
	an_unusable_folder_is_an_error_not_a_guess();
	the_writer_writes_the_latest_state_and_flushes_on_stop();
	the_writer_needs_the_right_to_write_and_tells_a_failure_once();
	a_state_that_was_not_saved_is_handed_over_again();
	a_saving_only_run_leaves_a_newer_version_alone();
	a_stopped_writer_takes_nothing_and_stopping_nothing_is_fine();
	the_editor_record_round_trips_and_is_checked();
	the_right_to_write_is_given_back_when_its_holder_is_killed();
	/* the folder's own files, then the folder */
	{
		static const wchar_t *const files[] = {
		        L"session.a", L"session.b", L"session.lock"};
		wchar_t p[MAX_PATH + 64];

		for (size_t i = 0; i < 3; i++) {
			swprintf(
			        p, MAX_PATH + 64, L"%ls\\%ls", g_dir, files[i]);
			DeleteFileW(p);
		}
		RemoveDirectoryW(g_dir);
	}
	return TEST_RESULT();
}
