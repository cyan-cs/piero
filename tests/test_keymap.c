/*
 * Keys as data: strokes (text, terminal events), the command registry, the
 * built-in profiles, sequences and counts, Custom overrides and the clashes
 * they can make.
 */
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "edit/wf_ekeymap.h"
#include "input/wf_keymap.h"
#include "wf_test.h"
#include "base/wf_macro.h"

static WfStroke st(const char *text)
{
	WfStroke s = {0, 0, false};

	CHECK(wf_stroke_parse(text, strlen(text), &s));
	return s;
}

static WfSeq seq(const char *text)
{
	WfSeq q;

	memset(&q, 0, sizeof q);
	CHECK(wf_seq_parse(text, strlen(text), &q));
	return q;
}

static WfEvent ev_char(uint32_t c)
{
	WfEvent e = {WF_EV_CHAR, c, WF_KEY_NONE, 0};

	return e;
}

static WfEvent ev_type(WfEventType t, uint32_t c)
{
	WfEvent e = {t, c, WF_KEY_NONE, 0};

	return e;
}

static WfEvent ev_key(WfKey k)
{
	WfEvent e = {WF_EV_KEY, 0, k, 0};

	return e;
}

static bool round_trip(const char *in, const char *expect)
{
	WfStroke s;
	char out[32];

	if (!wf_stroke_parse(in, strlen(in), &s)) {
		return false;
	}
	wf_stroke_format(s, out, sizeof out);
	if (strcmp(out, expect)) {
		printf("  %s -> %s (wanted %s)\n", in, out, expect);
		return false;
	}
	return true;
}

static void strokes_read_and_write_text(void)
{
	WfStroke s;
	const char *why = NULL;

	CHECK(round_trip("g", "g") && round_trip("G", "G"));
	CHECK(round_trip("ctrl+s", "Ctrl+S"));
	CHECK(round_trip("alt+x", "Alt+X") &&
	        round_trip("Ctrl+Shift+s", "Ctrl+Shift+S"));
	CHECK(round_trip("Shift+g", "G"));
	CHECK(round_trip("Space", "Space") && round_trip("Comma", "Comma") &&
	        round_trip("Plus", "Plus"));
	CHECK(round_trip("pageup", "PageUp") &&
	        round_trip("PgDn", "PageDown") && round_trip("Escape", "Esc") &&
	        round_trip("Del", "Delete"));
	CHECK(round_trip("Ctrl+Up", "Ctrl+Up") &&
	        round_trip("Shift+Tab", "Shift+Tab"));
	CHECK(round_trip("F2", "F2") && round_trip("f3", "F3"));
	CHECK(round_trip(":", ":") && round_trip("/", "/") &&
	        round_trip("$", "$"));
	CHECK(round_trip(
	        "\xe6\x97\xa5", "\xe6\x97\xa5")); /* a kanji is a key too */
	CHECK(!wf_stroke_parse("", 0, &s) && !wf_stroke_parse("Ctrl+", 5, &s) &&
	        !wf_stroke_parse("gg", 2, &s) &&
	        !wf_stroke_parse("Nonsense", 8, &s));
	CHECK(!wf_stroke_parse(
	        "Shift+1", 7, &s)); /* Shift is in the character */
	/* what the terminal can say, and what it cannot */
	CHECK(wf_stroke_bindable(st("Ctrl+S"), true, &why));
	CHECK(wf_stroke_bindable(st("Alt+."), true, &why));
	CHECK(wf_stroke_bindable(st("Ctrl+Left"), true, &why));
	CHECK(!wf_stroke_bindable(st("Ctrl+C"), true, &why) && why);
	CHECK(wf_stroke_bindable(
	        st("Ctrl+C"), false, &why)); /* the editor takes it */
	CHECK(!wf_stroke_bindable(st("Ctrl+I"), true, &why));
	CHECK(!wf_stroke_bindable(st("Ctrl+M"), true, &why));
	CHECK(!wf_stroke_bindable(st("Ctrl+."), true, &why));
	CHECK(wf_stroke_bindable(st("Shift+Left"), true, NULL)); /* reported */
	CHECK(!wf_stroke_bindable(st("Alt+Up"), true, &why) &&
	        why); /* not reported */
	CHECK(!wf_stroke_bindable(st("Shift+F2"), true, &why));
}

/* yazi's <C-s> notation is read as the same keys */
static void angle_notation_is_read(void)
{
	static const char *const pairs[][2] = {{"<C-s>", "Ctrl+S"},
	        {"<A-x>", "Alt+x"}, {"<S-Tab>", "Shift+Tab"},
	        {"<Enter>", "Enter"}, {"<Esc>", "Esc"}, {"<F2>", "F2"},
	        {"<C-S-a>", "Ctrl+Shift+A"}, {"<Space>", "Space"},
	        {"<Up>", "Up"}, {"<C-Left>", "Ctrl+Left"}};
	WfStroke a, b;
	size_t i;

	for (i = 0; i < WF_ARRAY_LEN(pairs); i++) {
		CHECK(wf_stroke_parse(pairs[i][0], strlen(pairs[i][0]), &a));
		CHECK(wf_stroke_parse(pairs[i][1], strlen(pairs[i][1]), &b));
		CHECK(a.code == b.code && a.mods == b.mods &&
		        a.named == b.named);
	}
	/* not keys */
	CHECK(!wf_stroke_parse("<>", 2, &a));
	CHECK(!wf_stroke_parse("<C->", 4, &a));
	CHECK(!wf_stroke_parse("<<a>>", 5, &a));
	CHECK(!wf_stroke_parse("<Hyper-x>", 9, &a));
	CHECK(!wf_stroke_parse("<C-Bogus>", 9, &a));
}

static void sequences_and_lists(void)
{
	WfSeq q;
	WfKeyOverride o;
	char out[64];

	q = seq("gg");
	CHECK(2 == q.n && 'g' == q.s[0].code && 'g' == q.s[1].code);
	q = seq("g g");
	CHECK(2 == q.n);
	q = seq("Ctrl+S");
	CHECK(1 == q.n && WF_MOD_CTRL == q.s[0].mods);
	q = seq("Esc");
	CHECK(1 == q.n && q.s[0].named);
	CHECK(!wf_seq_parse("a b c d", 7, &q)); /* three strokes at most */
	wf_seq_format(&(WfSeq){{st("g"), st("g")}, 2}, out, sizeof out);
	CHECK(!strcmp(out, "gg"));
	wf_seq_format(&(WfSeq){{st("g"), st("Esc")}, 2}, out, sizeof out);
	CHECK(!strcmp(out, "g Esc"));
	CHECK(wf_keys_parse_list("k, Up", &o) && 2 == o.n);
	wf_keys_format_list(&o, out, sizeof out);
	CHECK(!strcmp(out, "k, Up"));
	CHECK(wf_keys_parse_list("none", &o) && 0 == o.n);
	CHECK(wf_keys_parse_list("none   ", &o) && 0 == o.n);
	CHECK(!wf_keys_parse_list("none garbage", &o));
	CHECK(wf_keys_parse_list("Comma", &o) && 1 == o.n &&
	        ',' == o.seq[0].s[0].code);
	CHECK(!wf_keys_parse_list("a, b, c", &o) &&
	        !wf_keys_parse_list("", &o));
	CHECK(!wf_keys_parse_list("a,", &o));
}

static void formatting_respects_capacity(void)
{
	WfStroke s = st("Ctrl+Shift+S");
	WfSeq q = seq("g Esc");
	char out[32];
	size_t cap;

	CHECK(0 == wf_stroke_format(s, NULL, 0));
	CHECK(0 == wf_seq_format(&q, NULL, 0));
	for (cap = 1; cap <= sizeof out; cap++) {
		size_t n, expected;

		memset(out, '?', sizeof out);
		n = wf_stroke_format(s, out, cap);
		expected = cap <= strlen("Ctrl+Shift+S")
		        ? cap - 1
		        : strlen("Ctrl+Shift+S");
		CHECK(n == expected && '\0' == out[n] &&
		        !memcmp(out, "Ctrl+Shift+S", n));
		CHECK(cap == sizeof out || '?' == out[cap]);
		memset(out, '?', sizeof out);
		n = wf_seq_format(&q, out, cap);
		expected = cap <= strlen("g Esc") ? cap - 1 : strlen("g Esc");
		CHECK(n == expected && '\0' == out[n] &&
		        !memcmp(out, "g Esc", n));
		CHECK(cap == sizeof out || '?' == out[cap]);
	}
}

static void events_become_strokes(void)
{
	WfStroke s;
	WfEvent e;

	e = ev_char('G');
	CHECK(wf_stroke_from_event(&e, &s) && 'G' == s.code && !s.mods &&
	        !s.named);
	e = ev_char(':');
	CHECK(wf_stroke_from_event(&e, &s) && ':' == s.code);
	e = ev_type(WF_EV_CTRL, 's');
	CHECK(wf_stroke_from_event(&e, &s) && wf_stroke_equal(s, st("Ctrl+S")));
	e = ev_type(WF_EV_CTRL_SHIFT, 's');
	CHECK(wf_stroke_from_event(&e, &s) &&
	        wf_stroke_equal(s, st("Ctrl+Shift+S")));
	e = ev_type(WF_EV_ALT, 'x');
	CHECK(wf_stroke_from_event(&e, &s) && wf_stroke_equal(s, st("Alt+X")));
	e = ev_key(WF_KEY_CTRL_L);
	CHECK(wf_stroke_from_event(&e, &s) && wf_stroke_equal(s, st("Ctrl+L")));
	e = ev_key(WF_KEY_CTRL_UP);
	CHECK(wf_stroke_from_event(&e, &s) &&
	        wf_stroke_equal(s, st("Ctrl+Up")));
	e = ev_key(WF_KEY_SHIFT_TAB);
	CHECK(wf_stroke_from_event(&e, &s) &&
	        wf_stroke_equal(s, st("Shift+Tab")));
	e = ev_key(WF_KEY_ESC);
	CHECK(wf_stroke_from_event(&e, &s) && wf_stroke_equal(s, st("Esc")));
	e = ev_type(WF_EV_RELEASE, 'a');
	CHECK(!wf_stroke_from_event(&e, &s));
	e = ev_type(WF_EV_RESIZE, 0);
	CHECK(!wf_stroke_from_event(&e, &s));
	e = ev_char(0x1B);
	CHECK(!wf_stroke_from_event(&e, &s));
	/* every key the terminal layer names has a stroke */
	{
		int k;

		for (k = WF_KEY_UP; k <= WF_KEY_SHIFT_DELETE; k++) {
			WfEvent ek = ev_key((WfKey)k);

			CHECK(wf_stroke_from_event(&ek, &s));
		}
	}
}

static void the_registry_is_consistent(void)
{
	size_t i, j, n = wf_cmd_total();

	CHECK(n > 90);
	for (i = 0; i < n; i++) {
		const WfCommandInfo *a = wf_cmd_at(i);

		CHECK(a && a->name && a->name[0] && a->ctx_mask);
		CHECK(wf_cmd_info(a->id) == a && wf_cmd_by_name(a->name) == a);
		CHECK(*wf_msg_in(WF_UILANG_EN, a->desc) &&
		        *wf_msg_in(WF_UILANG_JA, a->desc));
		for (j = i + 1; j < n; j++) {
			const WfCommandInfo *b = wf_cmd_at(j);

			CHECK(a->id != b->id && strcmp(a->name, b->name));
		}
	}
	CHECK(WF_FM_NAV_UP == wf_cmd_by_name("fm.nav.up")->id);
	CHECK(WF_CMD_SAVE == wf_cmd_by_name("ed.save")->id);
	CHECK(!wf_cmd_by_name("fm.nothing"));
	{
		WfKeyContext c;

		CHECK(wf_context_from_name("vim_normal", &c) &&
		        WF_CTX_VIM_NORMAL == c);
		CHECK(!wf_context_from_name(
		        "confirm", &c)); /* fixed: no bindings */
	}
}

static WfKeymap *build(int profile, int base, int legacy_editor)
{
	WfKeyConfig cfg;
	WfKeymap *m = NULL;
	WfKeyIssue why;

	wf_keyconfig_init(&cfg);
	cfg.profile = profile;
	cfg.base = base;
	if (WF_OK != wf_keymap_build(&cfg, legacy_editor, &m, &why)) {
		printf("  build failed: profile %d kind %d ctx %d cmd %d other "
		       "%d (%s)\n",
		        profile, (int)why.kind, (int)why.ctx, (int)why.cmd,
		        (int)why.other, why.why ? why.why : "");
	}
	CHECK(NULL != m);
	CHECK(WF_KI_NONE == why.kind);
	return m;
}

static void key_lists_respect_capacity(void)
{
	WfKeymap *m = build(WF_PROFILE_LEGACY, 0, 0);
	WfKeyOverride o;
	char out[32];
	size_t cap;

	CHECK(wf_keys_parse_list("k, Up", &o));
	CHECK(0 == wf_keys_format_list(&o, NULL, 0));
	CHECK(0 == wf_keymap_keys_text(m, WF_CTX_FM, WF_FM_NAV_UP, NULL, 0));
	CHECK(0 == wf_keymap_first_key(m, WF_CTX_FM, WF_FM_NAV_UP, NULL, 0));
	for (cap = 1; cap <= sizeof out; cap++) {
		size_t n, expected = cap <= 5 ? cap - 1 : 5;

		memset(out, '?', sizeof out);
		n = wf_keys_format_list(&o, out, cap);
		CHECK(n == expected && '\0' == out[n] &&
		        !memcmp(out, "k, Up", n));
		CHECK(cap == sizeof out || '?' == out[cap]);
		memset(out, '?', sizeof out);
		n = wf_keymap_keys_text(m, WF_CTX_FM, WF_FM_NAV_UP, out, cap);
		CHECK(n == expected && '\0' == out[n] &&
		        !memcmp(out, "k, Up", n));
		CHECK(cap == sizeof out || '?' == out[cap]);
	}
	memset(&o, 0, sizeof o);
	CHECK(0 == wf_keys_format_list(&o, NULL, 0));
	CHECK(0 == wf_keys_format_list(&o, out, 1) && !out[0]);
	CHECK(4 == wf_keys_format_list(&o, out, sizeof out) &&
	        !strcmp(out, "none"));
	wf_keymap_free(m);
}

static const char *run(const WfKeymap *m,
        WfKeyContext c,
        WfKeyState *s,
        const char *keys,
        uint16_t *cmd,
        uint32_t *count)
{
	WfSeq q = seq(keys);
	WfKeyResult r = {0};
	uint8_t i;
	static char name[16];

	for (i = 0; i < q.n; i++) {
		r = wf_keymap_feed(m, c, s, q.s[i], 1000);
	}
	if (cmd) {
		*cmd = r.cmd;
	}
	if (count) {
		*count = r.count;
	}
	switch (r.verdict) {
	case WF_KR_NONE:      return "none";
	case WF_KR_PENDING:   return "pending";
	case WF_KR_COMMAND:   return "command";
	case WF_KR_CANCELLED: return "cancelled";
	case WF_KR_REFUSED:   return "refused";
	}
	snprintf(name, sizeof name, "?");
	return name;
}

static void every_profile_is_valid_and_complete(void)
{
	static const int profiles[][3] = {{WF_PROFILE_LEGACY, 0, WF_KM_PIERO},
	        {WF_PROFILE_LEGACY, 0, WF_KM_NANO}, {WF_PROFILE_NANO, 0, 0},
	        {WF_PROFILE_VIM, 0, 0}, {WF_PROFILE_CUSTOM, WF_BASE_NANO, 0},
	        {WF_PROFILE_CUSTOM, WF_BASE_VIM, 0}};
	size_t p, i;

	for (p = 0; p < WF_ARRAY_LEN(profiles); p++) {
		WfKeymap *m =
		        build(profiles[p][0], profiles[p][1], profiles[p][2]);
		unsigned c;

		if (!m) {
			continue;
		}
		for (c = 0; c < WF_CTX_BINDABLE; c++) {
			size_t n, total;
			const WfBinding *b =
			        wf_keymap_bindings(m, (WfKeyContext)c, &n);
			const WfCommandInfo *ci[160];

			/* every binding is a command that exists in its context */
			for (i = 0; i < n; i++) {
				const WfCommandInfo *info =
				        wf_cmd_info(b[i].cmd);

				CHECK(info && (info->ctx_mask & WF_CTX_BIT(c)));
			}
			/* a context in use has every required command bound */
			total = wf_cmd_list((WfKeyContext)c, ci, 160);
			for (i = 0; n && i < total; i++) {
				if (ci[i]->flags & WF_CF_REQUIRED) {
					char keys[64];

					wf_keymap_keys_text(m, (WfKeyContext)c,
					        ci[i]->id, keys, sizeof keys);
					CHECK(keys[0]);
				}
			}
		}
		wf_keymap_free(m);
	}
}

static void nano_keys_use_arrows_and_ctrl_chords(void)
{
	WfKeymap *m = build(WF_PROFILE_NANO, 0, 0);
	WfKeyState s;
	uint16_t cmd;
	uint32_t n;

	wf_keystate_reset(&s);
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "Ctrl+P", &cmd, &n), "command") &&
	        WF_FM_NAV_UP == cmd);
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "Down", &cmd, &n), "command") &&
	        WF_FM_NAV_DOWN == cmd);
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "g", &cmd, &n), "command") &&
	        WF_FM_SMART_GO == cmd);
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "Ctrl+L", &cmd, &n), "command") &&
	        WF_FM_SMART_GO_PATH == cmd);
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "Alt+6", &cmd, &n), "command") &&
	        WF_FM_COPY_STAGE == cmd);
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "Ctrl+K", &cmd, &n), "command") &&
	        WF_FM_MOVE_STAGE == cmd);
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "Delete", &cmd, &n), "command") &&
	        WF_FM_RECYCLE == cmd);
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "S", &cmd, &n), "command") &&
	        WF_FM_SORT == cmd);
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "Comma", &cmd, &n), "command") &&
	        WF_FM_SETTINGS == cmd);
	/* no counts in Nano: a digit is just a key that does nothing */
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "5", &cmd, &n), "none"));
	CHECK(!wf_keymap_counts(m, WF_CTX_FM) && !wf_keymap_modal(m));
	/* ':' is not bound in Nano's file manager */
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, ":", &cmd, &n), "none"));
	/* the editor: Nano's preset and the extras */
	CHECK(!strcmp(run(m, WF_CTX_EDIT, &s, "Ctrl+W", &cmd, &n), "command") &&
	        WF_CMD_FIND == cmd);
	CHECK(!strcmp(run(m, WF_CTX_EDIT, &s, "Ctrl+K", &cmd, &n), "command") &&
	        WF_CMD_CUT == cmd);
	CHECK(!strcmp(run(m, WF_CTX_EDIT, &s, "Ctrl+X", &cmd, &n), "command") &&
	        WF_CMD_EXIT == cmd);
	CHECK(!strcmp(run(m, WF_CTX_EDIT, &s, "Ctrl+G", &cmd, &n), "command") &&
	        WF_CMD_HELP == cmd);
	CHECK(!strcmp(run(m, WF_CTX_EDIT, &s, "Alt+6", &cmd, &n), "command") &&
	        WF_CMD_COPY == cmd);
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "j", &cmd, &n), "none"));
	CHECK(!strcmp(run(m, WF_CTX_PREVIEW, &s, "h", &cmd, &n), "none"));
	wf_keymap_free(m);
}

static void vim_sequences_counts_and_prefixes(void)
{
	WfKeymap *m = build(WF_PROFILE_VIM, 0, 0);
	WfKeyState s;
	uint16_t cmd;
	uint32_t n;
	WfKeyResult r;

	wf_keystate_reset(&s);
	CHECK(wf_keymap_counts(m, WF_CTX_FM) && wf_keymap_modal(m));
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "j", &cmd, &n), "command") &&
	        WF_FM_NAV_DOWN == cmd && 1 == n);
	/* g alone is not Smart Go in Vim: it waits */
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "g", &cmd, &n), "pending"));
	CHECK(wf_keystate_active(&s));
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "g", &cmd, &n), "command") &&
	        WF_FM_NAV_FIRST == cmd);
	CHECK(!wf_keystate_active(&s));
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "G", &cmd, &n), "command") &&
	        WF_FM_NAV_LAST == cmd);
	/* yy and dd are the two-key forms; one y or d does nothing yet */
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "y", &cmd, &n), "pending"));
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "y", &cmd, &n), "command") &&
	        WF_FM_COPY_STAGE == cmd);
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "d", &cmd, &n), "pending"));
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "d", &cmd, &n), "command") &&
	        WF_FM_MOVE_STAGE == cmd);
	/* a half-typed dd followed by something else runs nothing for the d */
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "d", &cmd, &n), "pending"));
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "j", &cmd, &n), "command") &&
	        WF_FM_NAV_DOWN == cmd);
	CHECK(!wf_keystate_active(&s));
	/* x is bound to nothing in Vim's file manager */
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "x", &cmd, &n), "none"));
	/* Esc drops a pending prefix */
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "d", &cmd, &n), "pending"));
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "Esc", &cmd, &n), "cancelled"));
	CHECK(!wf_keystate_active(&s));
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "d", &cmd, &n), "pending"));
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "d", &cmd, &n), "command") &&
	        WF_FM_MOVE_STAGE == cmd);
	/* counts: 5j, 12j; 10G is "go to 10" */
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "5", &cmd, &n), "pending"));
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "j", &cmd, &n), "command") &&
	        WF_FM_NAV_DOWN == cmd && 5 == n);
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "12", &cmd, &n), "pending"));
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "k", &cmd, &n), "command") &&
	        12 == n);
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "10", &cmd, &n), "pending"));
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "G", &cmd, &n), "command") &&
	        WF_FM_NAV_LAST == cmd && 10 == n);
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "3", &cmd, &n), "pending"));
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "gg", &cmd, &n), "command") &&
	        WF_FM_NAV_FIRST == cmd && 3 == n);
	/* a count never reaches a command that is not a move: it is refused */
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "5", &cmd, &n), "pending"));
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "dd", &cmd, &n), "refused"));
	CHECK(!wf_keystate_active(&s));
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "3", &cmd, &n), "pending"));
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "Delete", &cmd, &n), "refused"));
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "2", &cmd, &n), "pending"));
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "p", &cmd, &n), "refused"));
	/* a huge count is capped, not wrapped */
	{
		int i;

		for (i = 0; i < 12; i++) {
			(void)run(m, WF_CTX_FM, &s, "9", &cmd, &n);
		}
		CHECK(!strcmp(run(m, WF_CTX_FM, &s, "j", &cmd, &n),
		              "command") &&
		        WF_COUNT_MAX == n);
	}
	/* time out: the prefix is forgotten, the next key starts fresh */
	r = wf_keymap_feed(m, WF_CTX_FM, &s, st("g"), 1000);
	CHECK(WF_KR_PENDING == r.verdict);
	r = wf_keymap_feed(
	        m, WF_CTX_FM, &s, st("g"), 1000 + WF_SEQ_TIMEOUT_MS + 1);
	CHECK(WF_KR_PENDING == r.verdict); /* a new first g, not the second */
	r = wf_keymap_feed(
	        m, WF_CTX_FM, &s, st("g"), 1000 + WF_SEQ_TIMEOUT_MS + 5);
	CHECK(WF_KR_COMMAND == r.verdict && WF_FM_NAV_FIRST == r.cmd);
	/* 0 is a count digit only after another digit */
	CHECK(!strcmp(run(m, WF_CTX_VIM_NORMAL, &s, "0", &cmd, &n),
	              "command") &&
	        WF_CMD_HOME == cmd);
	CHECK(!strcmp(
	        run(m, WF_CTX_VIM_NORMAL, &s, "20", &cmd, &n), "pending"));
	CHECK(!strcmp(run(m, WF_CTX_VIM_NORMAL, &s, "j", &cmd, &n),
	              "command") &&
	        20 == n);
	/* the same key means different things in different contexts */
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "i", &cmd, &n), "command") &&
	        WF_FM_INFO == cmd);
	CHECK(!strcmp(run(m, WF_CTX_VIM_NORMAL, &s, "i", &cmd, &n),
	              "command") &&
	        WF_CMD_VIM_INSERT == cmd);
	CHECK(!strcmp(run(m, WF_CTX_VIM_INSERT, &s, "i", &cmd, &n),
	        "none")); /* typing */
	CHECK(!strcmp(run(m, WF_CTX_VIM_INSERT, &s, "Esc", &cmd, &n),
	              "command") &&
	        WF_CMD_VIM_NORMAL == cmd);
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, ":", &cmd, &n), "command") &&
	        WF_FM_CMDLINE == cmd);
	CHECK(!strcmp(run(m, WF_CTX_VIM_NORMAL, &s, "gt", &cmd, &n),
	              "command") &&
	        WF_CMD_NEXT_BUFFER == cmd);
	CHECK(!strcmp(run(m, WF_CTX_VIM_NORMAL, &s, "gT", &cmd, &n),
	              "command") &&
	        WF_CMD_PREV_BUFFER == cmd);
	/* nothing leaks between contexts: a prefix in one does not carry */
	CHECK(!strcmp(run(m, WF_CTX_VIM_NORMAL, &s, "g", &cmd, &n), "pending"));
	wf_keystate_reset(&s);
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "d", &cmd, &n), "pending"));
	wf_keymap_free(m);
}

static WfKeyIssue try_override(
        int profile, int base, WfKeyContext ctx, uint16_t cmd, const char *keys)
{
	WfKeyConfig cfg;
	WfKeyOverride o;
	WfKeymap *m = NULL;
	WfKeyIssue why;

	wf_keyconfig_init(&cfg);
	cfg.profile = profile;
	cfg.base = base;
	memset(&o, 0, sizeof o);
	o.ctx = (uint8_t)ctx;
	o.cmd = cmd;
	CHECK(wf_keys_parse_list(keys, &o));
	CHECK(wf_keyconfig_set(&cfg, &o));
	if (WF_OK == wf_keymap_build(&cfg, 0, &m, &why)) {
		CHECK(WF_KI_NONE == why.kind);
		wf_keymap_free(m);
	}
	return why;
}

static void custom_overrides_and_their_clashes(void)
{
	WfKeyIssue w;
	WfKeyConfig cfg;
	WfKeyOverride o;
	WfKeymap *m = NULL;
	WfKeyState s;
	uint16_t cmd;
	uint32_t n;

	/* a plain rebinding works, and frees the old key */
	wf_keyconfig_init(&cfg);
	cfg.profile = WF_PROFILE_CUSTOM;
	cfg.base = WF_BASE_NANO;
	memset(&o, 0, sizeof o);
	o.ctx = WF_CTX_FM;
	o.cmd = WF_FM_COPY_STAGE;
	CHECK(wf_keys_parse_list("c, Ctrl+O", &o) &&
	        wf_keyconfig_set(&cfg, &o));
	CHECK(WF_OK == wf_keymap_build(&cfg, 0, &m, &w));
	wf_keystate_reset(&s);
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "c", &cmd, &n), "command") &&
	        WF_FM_COPY_STAGE == cmd);
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "Ctrl+O", &cmd, &n), "command") &&
	        WF_FM_COPY_STAGE == cmd);
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "y", &cmd, &n), "none"));
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "Ctrl+K", &cmd, &n), "command") &&
	        WF_FM_MOVE_STAGE == cmd);
	{
		char keys[64], def[64];

		wf_keymap_keys_text(
		        m, WF_CTX_FM, WF_FM_COPY_STAGE, keys, sizeof keys);
		CHECK(!strcmp(keys, "c, Ctrl+O"));
		wf_keymap_default_text(WF_BASE_NANO, WF_CTX_FM,
		        WF_FM_COPY_STAGE, def, sizeof def);
		CHECK(!strcmp(def, "Alt+6"));
	}
	/* the check used by the key-capture screen */
	{
		WfSeq q = seq("Ctrl+K");

		w = wf_keymap_check(m, WF_CTX_FM, WF_FM_PASTE, &q);
		CHECK(WF_KI_SAME_KEYS == w.kind && WF_FM_MOVE_STAGE == w.other);
		q = seq("Ctrl+C");
		w = wf_keymap_check(m, WF_CTX_FM, WF_FM_PASTE, &q);
		CHECK(WF_KI_BAD_KEY == w.kind && w.why);
		q = seq("y"); /* free now */
		w = wf_keymap_check(m, WF_CTX_FM, WF_FM_PASTE, &q);
		CHECK(WF_KI_NONE == w.kind);
		q = seq("c"); /* its own current key: not a clash with itself */
		w = wf_keymap_check(m, WF_CTX_FM, WF_FM_COPY_STAGE, &q);
		CHECK(WF_KI_NONE == w.kind);
	}
	wf_keymap_free(m);
	/* clashes are found when the keymap is built */
	w = try_override(WF_PROFILE_CUSTOM, WF_BASE_NANO, WF_CTX_FM,
	        WF_FM_PASTE, "Ctrl+K");
	CHECK(WF_KI_SAME_KEYS == w.kind);
	w = try_override(
	        WF_PROFILE_CUSTOM, WF_BASE_NANO, WF_CTX_FM, WF_FM_PASTE, "g g");
	CHECK(WF_KI_PREFIX == w.kind); /* g is Smart Go */
	w = try_override(
	        WF_PROFILE_CUSTOM, WF_BASE_VIM, WF_CTX_FM, WF_FM_PASTE, "d");
	CHECK(WF_KI_PREFIX == w.kind); /* d d is move */
	w = try_override(
	        WF_PROFILE_CUSTOM, WF_BASE_VIM, WF_CTX_FM, WF_FM_PASTE, "5");
	CHECK(WF_KI_COUNT_DIGIT == w.kind);
	w = try_override(WF_PROFILE_CUSTOM, WF_BASE_NANO, WF_CTX_FM,
	        WF_FM_PASTE, "Ctrl+C");
	CHECK(WF_KI_BAD_KEY == w.kind);
	w = try_override(
	        WF_PROFILE_CUSTOM, WF_BASE_NANO, WF_CTX_FM, WF_FM_QUIT, "none");
	CHECK(WF_KI_REQUIRED == w.kind && WF_FM_QUIT == w.cmd);
	w = try_override(WF_PROFILE_CUSTOM, WF_BASE_NANO, WF_CTX_FM,
	        WF_FM_SETTINGS, "none");
	CHECK(WF_KI_REQUIRED == w.kind);
	w = try_override(WF_PROFILE_CUSTOM, WF_BASE_NANO, WF_CTX_PREVIEW,
	        WF_PV_CLOSE, "none");
	CHECK(WF_KI_REQUIRED == w.kind);
	w = try_override(
	        WF_PROFILE_CUSTOM, WF_BASE_NANO, WF_CTX_FM, WF_PV_UP, "u");
	CHECK(WF_KI_UNKNOWN ==
	        w.kind); /* a preview command in the file manager */
	/* the same key in another context is fine */
	w = try_override(WF_PROFILE_CUSTOM, WF_BASE_NANO, WF_CTX_PREVIEW,
	        WF_PV_LOAD, "y");
	CHECK(WF_KI_NONE == w.kind);
	/* a non-custom profile ignores overrides altogether */
	wf_keyconfig_init(&cfg);
	cfg.profile = WF_PROFILE_NANO;
	memset(&o, 0, sizeof o);
	o.ctx = WF_CTX_FM;
	o.cmd = WF_FM_COPY_STAGE;
	CHECK(wf_keys_parse_list("c", &o) && wf_keyconfig_set(&cfg, &o));
	CHECK(WF_OK == wf_keymap_build(&cfg, 0, &m, &w));
	wf_keystate_reset(&s);
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "Alt+6", &cmd, &n), "command"));
	wf_keymap_free(m);
	/* a broken Custom still has defaults to fall back to */
	wf_keyconfig_init(&cfg);
	cfg.profile = WF_PROFILE_CUSTOM;
	cfg.base = WF_BASE_VIM;
	memset(&o, 0, sizeof o);
	o.ctx = WF_CTX_FM;
	o.cmd = WF_FM_PASTE;
	CHECK(wf_keys_parse_list("d d", &o) && wf_keyconfig_set(&cfg, &o));
	CHECK(WF_E_INVAL == wf_keymap_build(&cfg, 0, &m, &w) && !m);
	CHECK(WF_OK == wf_keymap_build_defaults(&cfg, 0, &m) && m);
	wf_keystate_reset(&s);
	CHECK(!strcmp(run(m, WF_CTX_FM, &s, "p", &cmd, &n), "command") &&
	        WF_FM_PASTE == cmd);
	wf_keymap_free(m);
}

static void keyconfig_bookkeeping(void)
{
	WfKeyConfig a, b;
	WfKeyOverride o;

	wf_keyconfig_init(&a);
	wf_keyconfig_init(&b);
	CHECK(wf_keyconfig_equal(&a, &b));
	memset(&o, 0, sizeof o);
	o.ctx = WF_CTX_FM;
	o.cmd = WF_FM_INFO;
	CHECK(wf_keys_parse_list("I", &o));
	CHECK(wf_keyconfig_set(&a, &o) && !wf_keyconfig_equal(&a, &b));
	CHECK(wf_keyconfig_find(&a, WF_CTX_FM, WF_FM_INFO));
	CHECK(!wf_keyconfig_find(&a, WF_CTX_PREVIEW, WF_FM_INFO));
	CHECK(wf_keyconfig_set(&b, &o) && wf_keyconfig_equal(&a, &b));
	CHECK(wf_keyconfig_remove(&a, WF_CTX_FM, WF_FM_INFO));
	CHECK(!wf_keyconfig_remove(&a, WF_CTX_FM, WF_FM_INFO));
	/* full */
	{
		size_t i;

		for (i = 0; i < WF_KEY_OVERRIDES; i++) {
			o.cmd = (uint16_t)(1 + i);
			CHECK(wf_keyconfig_set(&a, &o));
		}
		o.cmd = 60000;
		CHECK(!wf_keyconfig_set(&a, &o));
	}
}

static void dispatch_is_fast(void)
{
	WfKeymap *m = build(WF_PROFILE_VIM, 0, 0);
	WfKeyState s;
	LARGE_INTEGER f, t0, t1;
	WfStroke strokes[4];
	long i, n = 2000000, hits = 0;

	strokes[0] = st("j");
	strokes[1] = st("g");
	strokes[2] = st("g");
	strokes[3] = st("Ctrl+Down");
	wf_keystate_reset(&s);
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t0);
	for (i = 0; i < n; i++) {
		hits += wf_keymap_feed(m, WF_CTX_FM, &s, strokes[i & 3], 1000)
		                .verdict == WF_KR_COMMAND;
	}
	QueryPerformanceCounter(&t1);
	printf("  key dispatch: %.3f us per key (%ld commands)\n",
	        (double)(t1.QuadPart - t0.QuadPart) * 1e6 / (double)f.QuadPart /
	                (double)n,
	        hits);
	CHECK(hits > n / 2);
	wf_keymap_free(m);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	strokes_read_and_write_text();
	angle_notation_is_read();
	sequences_and_lists();
	formatting_respects_capacity();
	key_lists_respect_capacity();
	events_become_strokes();
	the_registry_is_consistent();
	every_profile_is_valid_and_complete();
	nano_keys_use_arrows_and_ctrl_chords();
	vim_sequences_counts_and_prefixes();
	custom_overrides_and_their_clashes();
	keyconfig_bookkeeping();
	dispatch_is_fast();
	return TEST_RESULT();
}
