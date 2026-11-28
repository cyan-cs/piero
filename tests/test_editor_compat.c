/* Frozen results from Vim 9.1 patches 1-2081, -Nu NONE -n -es.
 * Reuse the session's fake IO so save and clipboard checks use real commands. */
#define main original_esession_main
#include "test_esession.c"
#undef main

static void begin_compat(WfProfile profile, const char *text)
{
	setup_keys(profile);
	CHECK(add(L"C:\\w\\compat.txt", text));
	CHECK(open_file(L"C:\\w\\compat.txt"));
}

static void frozen_vim_cases(void)
{
	/* vim_escape */
	begin_compat(WF_PROFILE_VIM, "abc\n");
	typing("iX");
	key(WF_KEY_ESC);
	CHECK(text_is("Xabc\n"));
	CHECK(at(0, 0));
	/* vim_l_boundary */
	begin_compat(WF_PROFILE_VIM, "ab\ncd\n");
	typing("llll");
	CHECK(text_is("ab\ncd\n"));
	CHECK(at(0, 1));
	/* vim_zero */
	begin_compat(WF_PROFILE_VIM, "  abcdef\n");
	typing("llllll0");
	CHECK(text_is("  abcdef\n"));
	CHECK(at(0, 0));
	/* vim_dollar */
	begin_compat(WF_PROFILE_VIM, "abc\n");
	typing("$");
	CHECK(text_is("abc\n"));
	CHECK(at(0, 2));
	/* vim_G */
	begin_compat(WF_PROFILE_VIM, "abc\n  def\n");
	typing("G");
	CHECK(text_is("abc\n  def\n"));
	CHECK(at(1, 2));
	/* vim_w_newline */
	begin_compat(WF_PROFILE_VIM, "one\n  two\n");
	typing("w");
	CHECK(text_is("one\n  two\n"));
	CHECK(at(1, 2));
	/* vim_b_newline */
	begin_compat(WF_PROFILE_VIM, "one\ntwo\n");
	typing("jb");
	CHECK(text_is("one\ntwo\n"));
	CHECK(at(0, 0));
	/* vim_cw */
	begin_compat(WF_PROFILE_VIM, "one two\n");
	typing("cwX");
	key(WF_KEY_ESC);
	CHECK(text_is("X two\n"));
	CHECK(at(0, 0));
	/* vim_yyp */
	begin_compat(WF_PROFILE_VIM, "one\ntwo\n");
	typing("yyp");
	CHECK(text_is("one\none\ntwo\n"));
	CHECK(at(1, 0));
	/* vim_undo_insert */
	begin_compat(WF_PROFILE_VIM, "abc\n");
	typing("iA");
	key(WF_KEY_ESC);
	typing("aB");
	key(WF_KEY_ESC);
	typing("u");
	CHECK(text_is("Aabc\n"));
	CHECK(at(0, 1));
	/* vim_2D */
	begin_compat(WF_PROFILE_VIM, "abc\ndef\nghi\n");
	typing("2D");
	CHECK(text_is("ghi\n"));
	CHECK(at(0, 0));
	/* vim_d2dollar */
	begin_compat(WF_PROFILE_VIM, "abc\ndef\nghi\n");
	typing("d2$");
	CHECK(text_is("ghi\n"));
	CHECK(at(0, 0));
	/* vim_count_object */
	begin_compat(WF_PROFILE_VIM, "one two three\n");
	typing("d2iw");
	CHECK(text_is("two three\n"));
	CHECK(at(0, 0));
	/* vim_visual_object */
	begin_compat(WF_PROFILE_VIM, "one two\n");
	typing("viwd");
	CHECK(text_is(" two\n"));
	CHECK(at(0, 0));
	/* vim_search_case */
	begin_compat(WF_PROFILE_VIM, "a A\n");
	typing("/A");
	key(WF_KEY_ENTER);
	CHECK(text_is("a A\n"));
	CHECK(at(0, 2));
	/* vim_dd_control */
	begin_compat(WF_PROFILE_VIM, "one\ntwo\nthree\n");
	typing("dd");
	CHECK(text_is("two\nthree\n"));
	CHECK(at(0, 0));
	/* vim_multiplied_control */
	begin_compat(WF_PROFILE_VIM, "one two three four five six seven\n");
	typing("2d3w");
	CHECK(text_is("seven\n"));
	CHECK(at(0, 0));
	/* vim_diw_control */
	begin_compat(WF_PROFILE_VIM, "one two\n");
	typing("diw");
	CHECK(text_is(" two\n"));
	CHECK(at(0, 0));
	/* vim_find_control */
	begin_compat(WF_PROFILE_VIM, "one:two\n");
	typing("df:");
	CHECK(text_is("two\n"));
	CHECK(at(0, 0));
	/* vim_dot_control */
	begin_compat(WF_PROFILE_VIM, "one\ntwo\nthree\n");
	typing("dd.");
	CHECK(text_is("three\n"));
	CHECK(at(0, 0));
	/* vim_visual_control */
	begin_compat(WF_PROFILE_VIM, "one two\n");
	typing("vd");
	CHECK(text_is("ne two\n"));
	CHECK(at(0, 0));
	/* vim_empty_register */
	begin_compat(WF_PROFILE_VIM, "one\ntwo\n");
	typing("yy\\\"ap");
	CHECK(text_is("one\ntwo\n"));
	CHECK(at(0, 0));
}

static void drain(void)
{
	unsigned ticks;
	for (ticks = 0; ticks < 2000 && wf_es_motion_pending(S); ticks++) {
		(void)wf_es_tick(S, ticks);
	}
	CHECK(!wf_es_motion_pending(S));
}

static void nano_cutbuffer_and_save(void)
{
	begin_compat(WF_PROFILE_NANO, "A\nB\nC\nD\n");
	ctrl('k');
	ctrl('k');
	CHECK(text_is("C\nD\n"));
	CHECK(S->nano_cut.n == 4 && !memcmp(S->nano_cut.text, "A\nB\n", 4));
	key(WF_KEY_DOWN);
	ctrl('k');
	CHECK(S->nano_cut.n == 2 && !memcmp(S->nano_cut.text, "D\n", 2));
	CHECK(!W.clip_present);
	ctrl('u');
	CHECK(text_is("C\nD\n"));
	begin_compat(WF_PROFILE_NANO, "abc\nxyz\n");
	alt('6');
	key(WF_KEY_DOWN);
	key(WF_KEY_RIGHT);
	ctrl('u');
	CHECK(text_is("abc\nxabc\nyz\n"));
	begin_compat(WF_PROFILE_NANO, "abc def\n");
	key(WF_KEY_CTRL_DELETE);
	CHECK(text_is("def\n"));
	alt('u');
	CHECK(text_is("abc def\n"));
	begin_compat(WF_PROFILE_NANO, "  abc\n");
	key(WF_KEY_RIGHT);
	key(WF_KEY_RIGHT);
	key(WF_KEY_RIGHT);
	ctrl('a');
	CHECK(at(0, 0));
	/* Write-out saves the complete buffer, including when text is marked. */
	ctrl('6');
	key(WF_KEY_RIGHT);
	key(WF_KEY_RIGHT);
	WfEditor *ed = wf_es_current(S);
	WfPos cursor = ed->cur, anchor = ed->anchor;
	ctrl('o');
	CHECK(!strcmp(S->in, "C:\\w\\compat.txt"));
	key(WF_KEY_ENTER);
	CHECK(W.saves == 1 && W.last_saved_n == 6);
	CHECK(ed->has_anchor && !wf_pos_cmp(cursor, ed->cur) &&
	        !wf_pos_cmp(anchor, ed->anchor));
	ctrl('o');
	typing("other.txt");
	key(WF_KEY_ENTER);
	CHECK(W.new_saves == 1 &&
	        !wcscmp(wf_es_buffer(S)->path, L"C:\\w\\other.txt"));
	CHECK(add(L"C:\\w\\taken.txt", "untouched"));
	ctrl('o');
	typing("taken.txt");
	key(WF_KEY_ENTER);
	CHECK(S->msg_error &&
	        !strcmp(find(L"C:\\w\\taken.txt")->data, "untouched"));
	CHECK(!wcscmp(wf_es_buffer(S)->path, L"C:\\w\\other.txt"));
	ed->read_only = true;
	int saves = W.saves;
	ctrl('o');
	key(WF_KEY_ENTER);
	CHECK(S->msg_error && W.saves == saves);
	ctrl('o');
	typing("readonly-copy.txt");
	key(WF_KEY_ENTER);
	CHECK(!S->msg_error && !wf_es_current(S)->read_only);
	CHECK(!wcscmp(wf_es_buffer(S)->path, L"C:\\w\\readonly-copy.txt"));
}

static void vim_registers_and_history(void)
{
	begin_compat(WF_PROFILE_VIM, "one\ntwo\n");
	typing("\"ayiw");
	CHECK(!S->vim.regs[0].lines && S->vim.regs[0].n == 3);
	typing("\"byy");
	CHECK(S->vim.regs[1].lines && S->vim.regs[1].n == 4);
	memcpy(W.clip, "EXTERNAL", 8);
	W.clip_n = 8;
	W.clip_present = true;
	typing("\"aP");
	CHECK(text_is("oneone\ntwo\n"));
	chr('u');
	typing("\"bp");
	CHECK(text_is("one\none\ntwo\n"));
	CHECK(!memcmp(W.clip, "EXTERNAL", 8));
	begin_compat(WF_PROFILE_VIM, "abc\n");
	typing("iONE");
	key(WF_KEY_ENTER);
	typing("TWO");
	key(WF_KEY_ESC);
	chr('u');
	CHECK(text_is("abc\n"));
	CHECK(at(0, 0));
	ctrl('r');
	CHECK(text_is("ONE\nTWOabc\n"));
	CHECK(at(1, 2));
	begin_compat(WF_PROFILE_VIM, "one two\n");
	typing("cwX");
	key(WF_KEY_ESC);
	chr('u');
	CHECK(text_is("one two\n"));
	ctrl('r');
	CHECK(text_is("X two\n"));
	begin_compat(WF_PROFILE_VIM, "a A a\n");
	typing("/a");
	key(WF_KEY_ENTER);
	CHECK(at(0, 4));
	chr('n');
	CHECK(at(0, 0));
	chr('n');
	CHECK(at(0, 4));
	begin_compat(WF_PROFILE_VIM, "aa\nbb\n");
	typing("vjl\"ay");
	CHECK(!S->vim.regs[0].lines && S->vim.regs[0].n == 5);
	typing("\"ap");
	CHECK(text_is("aaa\nbba\nbb\n"));
	begin_compat(WF_PROFILE_VIM, "abc\n");
	WfEditor *ed = wf_es_current(S);
	ed->undo.max_bytes = 2 * sizeof(WfURec) + 8;
	typing("iAB");
	size_t bytes = ed->undo.bytes, pos = ed->undo.pos;
	chr('C');
	CHECK(S->msg_error && text_is("ABabc\n"));
	CHECK(ed->undo.bytes == bytes && ed->undo.pos == pos);
	key(WF_KEY_ESC);
	chr('u');
	CHECK(text_is("abc\n"));
	ctrl('r');
	CHECK(text_is("ABabc\n"));
	unsigned redo_failures = 0;
	for (int nth = 0; nth < 16; nth++) {
		begin_compat(WF_PROFILE_VIM, "abc\n");
		typing("iAB");
		key(WF_KEY_ESC);
		chr('u');
		chr('i');
		ed = wf_es_current(S);
		pos = ed->undo.n;
		bytes = ed->undo.bytes;
		wf_et_fail_nth_alloc(nth);
		chr('Z');
		wf_et_fail_nth_alloc(-1);
		/* Zero refuses the first allocation; larger values permit that
		 * many allocations before refusing all subsequent ones. */
		if (S->msg_error) {
			redo_failures++;
			CHECK(text_is("abc\n"));
			CHECK(ed->undo.n == pos && !ed->undo.pos &&
			        ed->undo.bytes == bytes);
			key(WF_KEY_ESC);
			ctrl('r');
			CHECK(text_is("ABabc\n"));
		}
	}
	CHECK(redo_failures > 0);
	begin_compat(WF_PROFILE_VIM, "abc\n");
	typing("iX");
	key(WF_KEY_CTRL_RIGHT);
	chr('Y');
	key(WF_KEY_ESC);
	chr('u');
	CHECK(text_is("Xabc\n"));
	chr('u');
	CHECK(text_is("abc\n"));
}

static void failed_capture_keeps_everything(void)
{
	unsigned vim_failures = 0, nano_failures = 0;
	for (int nth = 0; nth < 40; nth++) {
		begin_compat(WF_PROFILE_VIM, "old new\n");
		typing("\"ayiw");
		typing("wvll");
		WfEditor *ed = wf_es_current(S);
		WfPos cursor = ed->cur, anchor = ed->anchor;
		size_t history = ed->undo.pos;
		typing("\"a");
		wf_et_fail_nth_alloc(nth);
		chr('d');
		wf_et_fail_nth_alloc(-1);
		if (S->msg_error) {
			vim_failures++;
			CHECK(text_is("old new\n"));
			CHECK(ed->has_anchor && !wf_pos_cmp(cursor, ed->cur) &&
			        !wf_pos_cmp(anchor, ed->anchor));
			CHECK(history == ed->undo.pos);
			CHECK(S->vim.regs[0].n == 3 &&
			        !memcmp(S->vim.regs[0].text, "old", 3));
			CHECK(S->vim.unnamed.n == 3 &&
			        !memcmp(S->vim.unnamed.text, "old", 3));
		}
		begin_compat(WF_PROFILE_NANO, "A\nB\nC\n");
		ctrl('k');
		ed = wf_es_current(S);
		history = ed->undo.pos;
		wf_et_fail_nth_alloc(nth);
		ctrl('k');
		wf_et_fail_nth_alloc(-1);
		if (S->msg_error) {
			nano_failures++;
			CHECK(text_is("B\nC\n"));
			CHECK(at(0, 0));
			CHECK(history == ed->undo.pos && !ed->has_anchor);
			CHECK(S->nano_cut.n == 2 &&
			        !memcmp(S->nano_cut.text, "A\n", 2));
			CHECK(ed->clip_n == 2 && !memcmp(ed->clip, "A\n", 2));
		}
	}
	printf("capture fault points: Vim %u, Nano %u\n", vim_failures,
	        nano_failures);
	CHECK(vim_failures >= 5 && nano_failures >= 5);
}

static void queued_input_is_preserved(void)
{
	const size_t n = 4u << 20;
	char *text = malloc(n);
	CHECK(text);
	if (!text) {
		return;
	}
	memset(text, 'a', n);
	begin_compat(WF_PROFILE_VIM, "placeholder");
	WfEditor *ed = wf_es_current(S);
	CHECK(WF_OK ==
	        wf_ed_set_text(ed, text, n, (WfEncoding){WF_ENC_UTF8, 0}));
	chr('w');
	CHECK(wf_es_motion_pending(S));
	typing("iXYZ");
	key(WF_KEY_ESC);
	drain();
	/* Esc cancelled w. Every queued key is then applied in order at column 0. */
	CHECK(ed->text.lines[0].len == n + 3 &&
	        !memcmp(ed->text.lines[0].s, "XYZaaa", 6));
	chr('u');
	CHECK(ed->text.lines[0].len == n);
	chr('w');
	WfEvent ch = {WF_EV_CHAR, 'h', WF_KEY_NONE, 0};
	for (size_t i = 0; i < WF_ARRAY_LEN(S->input); i++) {
		CHECK(wf_es_event(S, &ch, ROWS, COLS) != WF_ES_BUSY);
	}
	CHECK(WF_ES_BUSY == wf_es_event(S, &ch, ROWS, COLS));
	CHECK(S->input_n == WF_ARRAY_LEN(S->input));
	drain();
	CHECK(at(0, n - 65));
	CHECK(wf_es_event(S, &ch, ROWS, COLS) != WF_ES_BUSY);
	CHECK(at(0, n - 66));
	chr('0');
	chr('w');
	typing("iZ");
	CHECK(WF_OK ==
	        wf_ed_type(ed, 'Q')); /* invalidates the pending generation */
	drain();
	CHECK(!memcmp(ed->text.lines[0].s, "QZaa", 4));
	begin_compat(WF_PROFILE_VIM, "placeholder");
	ed = wf_es_current(S);
	CHECK(WF_OK ==
	        wf_ed_set_text(ed, text, n, (WfEncoding){WF_ENC_UTF8, 0}));
	typing("qadwq");
	drain();
	CHECK(ed->text.bytes == 0 && S->vim.macro[0].n == 2);
	CHECK(WF_OK ==
	        wf_ed_set_text(ed, text, n, (WfEncoding){WF_ENC_UTF8, 0}));
	typing("@a");
	CHECK(wf_es_motion_pending(S));
	typing("iZ");
	drain();
	CHECK(text_is("Z"));
	key(WF_KEY_ESC);
	/* Closing via retained input returns later events to the host intact. */
	begin_compat(WF_PROFILE_NANO, "placeholder");
	ed = wf_es_current(S);
	CHECK(WF_OK ==
	        wf_ed_set_text(ed, text, n, (WfEncoding){WF_ENC_UTF8, 0}));
	key(WF_KEY_CTRL_RIGHT);
	ctrl('x');
	chr('z');
	for (unsigned ticks = 0; wf_es_buffer_count(S) && ticks < 2000;
	        ticks++) {
		(void)wf_es_tick(S, ticks);
	}
	CHECK(!wf_es_buffer_count(S));
	WfEvent retained[2];
	CHECK(1 ==
	        wf_es_take_closed_input(S, retained, WF_ARRAY_LEN(retained)));
	CHECK(retained[0].type == WF_EV_CHAR && retained[0].ch == 'z');
	free(text);
}

int main(void)
{
	frozen_vim_cases();
	nano_cutbuffer_and_save();
	vim_registers_and_history();
	failed_capture_keeps_everything();
	queued_input_is_preserved();
	wf_es_free(S);
	S = NULL;
	wf_keymap_free(KEYMAP);
	KEYMAP = NULL;
	return TEST_RESULT();
}
