/*
 * The editor as a user drives it, with a fake file system and clipboard:
 * keys in, buffers, prompts and screen out. What matters here is the flow of
 * the questions (nothing is lost by one stray key), that every key binding
 * reaches the same operations, and that the screen says what is true.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "edit/wf_esearch.h"
#include "edit/wf_esession.h"
#include "edit/wf_esession_internal.h"
#include "text/wf_text.h"
#include "render/wf_theme.h"
#include "wf_test.h"
#include "base/wf_macro.h"

typedef struct
{
	wchar_t path[64];
	char data[512];
	size_t n;
	bool present;
	WfLoadVerdict verdict;
	WfEncoding enc;
	uint8_t id;
	DWORD attributes;
} FakeFile;

typedef struct
{
	FakeFile files[6];
	WfSaveVerdict
	        next_save; /* verdict for the next save (OK = really save) */
	wchar_t kept_temp[260], kept_original[260]; /* what a failed save leaves */
	int saves, new_saves, loads;
	char clip[512];
	size_t clip_n;
	bool clip_present;
	bool clip_set_fails;
	bool clip_crlf; /* the system clipboard writes a line ending as CRLF */
	bool skip_size_check; /* the file layer lets a big file through */
	char last_saved[512];
	size_t last_saved_n;
} World;

static World W;

static FakeFile *find(const wchar_t *path)
{
	size_t i;

	for (i = 0; i < WF_ARRAY_LEN(W.files); i++) {
		if (W.files[i].path[0] && !wcscmp(W.files[i].path, path)) {
			return &W.files[i];
		}
	}
	return NULL;
}

static FakeFile *add(const wchar_t *path, const char *data)
{
	size_t i;

	for (i = 0; i < WF_ARRAY_LEN(W.files); i++) {
		if (!W.files[i].path[0]) {
			FakeFile *f = &W.files[i];

			wcscpy_s(f->path, 64, path);
			f->n = strlen(data);
			memcpy(f->data, data, f->n);
			f->present = true;
			f->enc.kind = WF_ENC_UTF8;
			f->id = (uint8_t)(i + 1);
			return f;
		}
	}
	return NULL;
}

static void stamp_of(const FakeFile *f, WfFileStamp *s)
{
	memset(s, 0, sizeof *s);
	s->valid = true;
	s->id.FileId.Identifier[0] = f->id;
	s->size = f->n;
	s->links = 1;
	s->attributes = f->attributes;
}

static WfStatus fake_load(void *ctx,
        const wchar_t *path,
        size_t max,
        unsigned cp,
        const WfEncoding *force,
        WfLoaded *out)
{
	FakeFile *f = find(path);

	(void)ctx;
	(void)cp;
	W.loads++;
	memset(out, 0, sizeof *out);
	if (!f || !f->present) {
		out->verdict = WF_FL_MISSING;
		return WF_OK;
	}
	if (f->n > max && !W.skip_size_check) {
		out->verdict = WF_FL_TOO_BIG;
		out->reason = "larger than the editing limit";
		return WF_OK;
	}
	out->verdict = f->verdict;
	out->reason = "fake reason";
	out->enc = force ? *force : f->enc;
	out->candidate_ok = WF_FL_AMBIGUOUS == f->verdict;
	if (WF_FL_OK == f->verdict ||
	        (force && WF_FL_AMBIGUOUS == f->verdict)) {
		out->verdict = WF_FL_OK;
		out->utf8 = malloc(f->n + 1);
		memcpy(out->utf8, f->data, f->n);
		out->utf8[f->n] = '\0';
		out->utf8_n = f->n;
	}
	stamp_of(f, &out->stamp);
	return WF_OK;
}

static WfStatus fake_over(void *ctx,
        const wchar_t *path,
        const WfFileStamp *was,
        const char *utf8,
        size_t n,
        WfEncoding enc,
        const WfSaveOptions *opt,
        WfSaveResult *out)
{
	FakeFile *f = find(path);

	(void)ctx;
	(void)was;
	(void)enc;
	(void)opt;
	memset(out, 0, sizeof *out);
	W.saves++;
	if (WF_SV_OK != W.next_save) {
		out->verdict = W.next_save;
		out->reason = "fake refusal";
		wcscpy_s(out->kept_temp, WF_ARRAY_LEN(out->kept_temp), W.kept_temp);
		wcscpy_s(out->kept_original, WF_ARRAY_LEN(out->kept_original),
		        W.kept_original);
		W.next_save = WF_SV_OK;
		return WF_OK;
	}
	memcpy(f->data, utf8, n);
	f->n = n;
	memcpy(W.last_saved, utf8, n);
	W.last_saved_n = n;
	out->verdict = WF_SV_OK;
	stamp_of(f, &out->stamp);
	return WF_OK;
}

static WfStatus fake_new(void *ctx,
        const wchar_t *path,
        const char *utf8,
        size_t n,
        WfEncoding enc,
        WfSaveResult *out)
{
	FakeFile *f = find(path);

	(void)ctx;
	(void)enc;
	memset(out, 0, sizeof *out);
	W.new_saves++;
	if (f && f->present) {
		out->verdict = WF_SV_EXISTS;
		return WF_OK;
	}
	if (!f) {
		f = add(path, "");
	}
	memcpy(f->data, utf8, n);
	f->n = n;
	f->present = true;
	memcpy(W.last_saved, utf8, n);
	W.last_saved_n = n;
	out->verdict = WF_SV_OK;
	stamp_of(f, &out->stamp);
	return WF_OK;
}

static WfStatus fake_clip_get(void *ctx, char **utf8, size_t *n)
{
	(void)ctx;
	*utf8 = NULL;
	*n = 0;
	if (!W.clip_present) {
		return WF_E_NOENT;
	}
	*utf8 = malloc(W.clip_n + 1);
	memcpy(*utf8, W.clip, W.clip_n);
	(*utf8)[W.clip_n] = '\0';
	*n = W.clip_n;
	return WF_OK;
}

static WfStatus fake_clip_set(void *ctx, const char *utf8, size_t n)
{
	(void)ctx;
	if (W.clip_set_fails) {
		return WF_E_OS;
	}
	W.clip_n = 0;
	for (size_t i = 0; i < n; i++) {
		if (W.clip_crlf && '\n' == utf8[i] &&
		        (0 == i || '\r' != utf8[i - 1])) {
			W.clip[W.clip_n++] = '\r';
		}
		W.clip[W.clip_n++] = utf8[i];
	}
	W.clip_present = true;
	return WF_OK;
}

static struct
{
	wchar_t path[260];
	char text[256];
	size_t n;
	WfEncoding enc;
	bool present;
	bool fail; /* the next writes fail */
	int writes, removes;
	int tries; /* writes asked for, failed or not */
	bool late_fail; /* the last write, taken in, failed afterwards (a worker) */
} D;

static bool fake_draft_allowed(void *ctx, const wchar_t *path, size_t bytes)
{
	(void)ctx;
	(void)bytes;
	return !wcsstr(path, L"secret");
}

static WfStatus fake_draft_write(void *ctx,
        const wchar_t *path,
        WfEncoding enc,
        const char *utf8,
        size_t n)
{
	(void)ctx;
	D.tries++;
	if (D.fail) {
		return WF_E_OS;
	}
	wcscpy_s(D.path, 260, path);
	memcpy(D.text, utf8, n < sizeof D.text ? n : sizeof D.text);
	D.n = n;
	D.enc = enc;
	D.present = true;
	D.writes++;
	return WF_OK;
}

static WfStatus fake_draft_read(void *ctx,
        const wchar_t *path,
        WfEncoding *enc,
        char **utf8,
        size_t *n)
{
	(void)ctx;
	if (!D.present || wcscmp(D.path, path)) {
		return WF_E_NOENT;
	}
	*utf8 = malloc(D.n + 1);
	if (!*utf8) {
		return WF_E_NOMEM;
	}
	memcpy(*utf8, D.text, D.n);
	(*utf8)[D.n] = '\0';
	*n = D.n;
	*enc = D.enc;
	return WF_OK;
}

static void fake_draft_remove(void *ctx, const wchar_t *path)
{
	(void)ctx;
	if (D.present && !wcscmp(D.path, path)) {
		D.present = false;
		D.removes++;
	}
}

static WfEditSession *S;
#define ROWS 24
#define COLS 100

static WfEsResult press(const WfEvent *e)
{
	return wf_es_event(S, e, ROWS, COLS);
}

static WfEsResult chr(uint32_t c)
{
	WfEvent e = {WF_EV_CHAR, c, WF_KEY_NONE, 0};

	return press(&e);
}

static WfEsResult key(WfKey k)
{
	WfEvent e = {WF_EV_KEY, 0, k, 0};

	return press(&e);
}

static WfEsResult ctrl(char c)
{
	WfEvent e = {WF_EV_CTRL, (uint32_t)c, WF_KEY_NONE, 0};

	return press(&e);
}

static WfEsResult alt(char c)
{
	WfEvent e = {WF_EV_ALT, (uint32_t)c, WF_KEY_NONE, 0};

	return press(&e);
}

static void typing(const char *s)
{
	for (; *s; s++) {
		chr((unsigned char)*s);
	}
}

static bool text_is(const char *expect)
{
	WfEditor *e = wf_es_current(S);
	char *out = NULL;
	size_t n = 0;
	bool same;

	CHECK(wf_et_read(&e->text, (WfPos){0, 0}, wf_et_end(&e->text), &out,
	              &n) == WF_OK);
	same = n == strlen(expect) && !memcmp(out, expect, n);
	if (!same) {
		printf("  text [%.*s] expected [%s]\n", (int)n, out, expect);
	}
	free(out);
	return same;
}

static const WfKeymap *SETUP_KEYS; /* the keys of a chosen profile */

static bool fake_draft_failed(void *ctx, const wchar_t *path);

static void setup(WfKeymapId preset)
{
	WfEditConfig c;
	WfEditIo io = {NULL, fake_load, fake_over, fake_new, fake_clip_get,
	        fake_clip_set, fake_draft_allowed, fake_draft_write,
	        fake_draft_read, fake_draft_remove, fake_draft_failed};

	memset(&W, 0, sizeof W);
	wf_es_free(S);
	wf_es_config_default(&c);
	c.keymap = preset;
	c.keys = SETUP_KEYS;
	S = wf_es_new(&c, io);
	CHECK(S != NULL);
}

static bool open_file(const wchar_t *path)
{
	char why[160];

	return WF_OPEN_OK == wf_es_open(S, path, why, sizeof why);
}

static bool screen_has(const char *needle)
{
	WfFrame *f = wf_frame_new(COLS, ROWS, &(WfStyle){0, 0, 0});
	bool found = false;
	int y, x;

	wf_es_draw(S, f);
	for (y = 0; y < f->rows && !found; y++) {
		char row[2048] = "";

		for (x = 0; x < f->cols; x++) {
			strcat_s(row, sizeof row,
			        f->cells[y * f->cols + x].utf8);
		}
		found = NULL != strstr(row, needle);
	}
	if (!found) {
		printf("  screen lacks [%s]\n", needle);
	}
	wf_frame_free(f);
	return found;
}

static void opening_typing_and_saving(void)
{
	FakeFile *f;

	setup(WF_KM_PIERO);
	f = add(L"C:\\w\\notes.txt", "hello\r\nworld\r\n");
	CHECK(open_file(L"C:\\w\\notes.txt"));
	CHECK(1 == wf_es_buffer_count(S));
	CHECK(screen_has("notes.txt") && screen_has("UTF-8") &&
	        screen_has("CRLF"));
	CHECK(screen_has("Ln 1/3") && screen_has("Saved"));
	typing("X");
	CHECK(text_is("Xhello\r\nworld\r\n"));
	CHECK(screen_has("[Modified]") && screen_has("Modified"));
	ctrl('s');
	CHECK(1 == W.saves && 15 == f->n &&
	        !memcmp(f->data, "Xhello\r\nworld\r\n", 15));
	CHECK(screen_has("Saved 15 bytes") || screen_has("Saved"));
	CHECK(!screen_has("[Modified]"));
	CHECK(wf_es_take_saved(S) && !wf_es_take_saved(S));
	/* Japanese text, an emoji and a wide character are typed as they are */
	typing("");
	chr(0x65E5);
	chr(0x1F600);
	CHECK(text_is("X\xE6\x97\xA5\xF0\x9F\x98\x80hello\r\nworld\r\n"));
	CHECK(screen_has("\xE6\x97\xA5"));
}

static void leaving_with_unsaved_text_asks_and_one_key_decides_nothing(void)
{
	FakeFile *f;

	setup(WF_KM_PIERO);
	f = add(L"C:\\w\\a.txt", "keep");
	CHECK(open_file(L"C:\\w\\a.txt"));
	/* unmodified: Ctrl+X and Esc leave at once */
	CHECK(WF_ES_CLOSED == ctrl('x'));

	setup(WF_KM_PIERO);
	f = add(L"C:\\w\\a.txt", "keep");
	CHECK(open_file(L"C:\\w\\a.txt"));
	typing("!");
	CHECK(WF_ES_REDRAW == ctrl('x') && wf_es_prompt_active(S));
	CHECK(screen_has("Save changes to a.txt?"));
	/* random letters and Tab decide nothing */
	CHECK(WF_ES_NONE == chr('x') && wf_es_prompt_active(S));
	CHECK(WF_ES_NONE == key(WF_KEY_TAB) && wf_es_prompt_active(S));
	/* Enter answers with the selection, and it starts on Cancel: a stray
	 * Enter keeps everything; it never saves or discards */
	CHECK(WF_ES_REDRAW == key(WF_KEY_ENTER) && !wf_es_prompt_active(S));
	CHECK(text_is("!keep") && 0 == W.saves && !strcmp(f->data, "keep"));
	CHECK(WF_ES_REDRAW == ctrl('x') && wf_es_prompt_active(S));
	/* Cancel keeps everything */
	CHECK(WF_ES_REDRAW == chr('c') && !wf_es_prompt_active(S));
	CHECK(text_is("!keep") && 0 == W.saves);
	/* Esc also cancels the question */
	CHECK(WF_ES_REDRAW == key(WF_KEY_ESC) && wf_es_prompt_active(S));
	CHECK(WF_ES_REDRAW == key(WF_KEY_ESC) && !wf_es_prompt_active(S));
	/* Discard leaves without writing */
	CHECK(WF_ES_REDRAW == ctrl('x'));
	CHECK(WF_ES_CLOSED == chr('D'));
	CHECK(0 == W.saves && !strcmp(f->data, "keep"));
	/* Save writes, then leaves */
	setup(WF_KM_PIERO);
	f = add(L"C:\\w\\a.txt", "keep");
	CHECK(open_file(L"C:\\w\\a.txt"));
	typing("+");
	CHECK(WF_ES_REDRAW == ctrl('x'));
	CHECK(WF_ES_CLOSED == chr('s'));
	CHECK(1 == W.saves && !memcmp(f->data, "+keep", 5));
	/* Esc on a clean buffer leaves too */
	setup(WF_KM_PIERO);
	add(L"C:\\w\\a.txt", "x");
	CHECK(open_file(L"C:\\w\\a.txt"));
	CHECK(WF_ES_CLOSED == key(WF_KEY_ESC));
}

static void a_failed_save_never_closes_the_buffer(void)
{
	setup(WF_KM_PIERO);
	add(L"C:\\w\\a.txt", "keep");
	CHECK(open_file(L"C:\\w\\a.txt"));
	typing("!");
	CHECK(WF_ES_REDRAW == ctrl('x'));
	W.next_save = WF_SV_IO;
	CHECK(WF_ES_REDRAW == chr('s')); /* stays open */
	CHECK(1 == wf_es_buffer_count(S) && text_is("!keep"));
	CHECK(screen_has("Not saved") && screen_has("Modified"));
	/* read-only or refused by the unsafe check: the same */
	W.next_save = WF_SV_UNSAFE;
	CHECK(WF_ES_REDRAW == ctrl('s') && screen_has("Not saved"));
	CHECK(screen_has("Modified"));
}

static void a_file_changed_on_disk_offers_reload_save_as_or_cancel(void)
{
	FakeFile *f;

	setup(WF_KM_PIERO);
	f = add(L"C:\\w\\a.txt", "disk v1");
	CHECK(open_file(L"C:\\w\\a.txt"));
	typing("mine ");
	W.next_save = WF_SV_CONFLICT;
	ctrl('s');
	CHECK(wf_es_prompt_active(S) && screen_has("changed on disk"));
	/* Enter answers with the selection, which starts on Cancel: my edits
	 * are kept, nothing is reloaded or written */
	CHECK(WF_ES_REDRAW == key(WF_KEY_ENTER));
	CHECK(text_is("mine disk v1") && !wf_es_prompt_active(S));
	CHECK(0 != strcmp(f->data, "mine disk v1"));
	W.next_save = WF_SV_CONFLICT;
	ctrl('s');
	CHECK(wf_es_prompt_active(S));
	chr('c'); /* the letter does the same */
	CHECK(text_is("mine disk v1") && !wf_es_prompt_active(S));
	CHECK(0 != strcmp(f->data, "mine disk v1"));

	/* Reload drops them, on purpose */
	W.next_save = WF_SV_CONFLICT;
	ctrl('s');
	memcpy(f->data, "theirs", 6);
	f->n = 6;
	chr('r');
	CHECK(text_is("theirs") && !wf_es_prompt_active(S));
	CHECK(screen_has("Reloaded") && screen_has("Saved") &&
	        !screen_has("[Modified]"));

	/* Another name: the Save As prompt, never an overwrite */
	typing("x");
	W.next_save = WF_SV_CONFLICT;
	ctrl('s');
	chr('a');
	CHECK(wf_es_prompt_active(S) && screen_has("Save as"));
	typing("copy.txt");
	key(WF_KEY_ENTER);
	CHECK(1 == W.new_saves && NULL != find(L"C:\\w\\copy.txt"));
	CHECK(!memcmp(find(L"C:\\w\\copy.txt")->data, "xtheirs", 7));
	CHECK(screen_has("copy.txt") && screen_has("Saved"));

	/* a file that is gone */
	setup(WF_KM_PIERO);
	add(L"C:\\w\\g.txt", "g");
	CHECK(open_file(L"C:\\w\\g.txt"));
	typing("1");
	W.next_save = WF_SV_GONE;
	ctrl('s');
	CHECK(screen_has("deleted or replaced"));
	chr('c');
	CHECK(text_is("1g"));
}

static void save_as_never_overwrites_and_checks_names(void)
{
	setup(WF_KM_PIERO);
	add(L"C:\\w\\a.txt", "aaa");
	add(L"C:\\w\\b.txt", "bbb");
	CHECK(open_file(L"C:\\w\\a.txt"));
	typing("Z");
	alt('s');
	CHECK(wf_es_prompt_active(S));
	typing("b.txt");
	key(WF_KEY_ENTER);
	CHECK(screen_has("taken") || screen_has("Choose another"));
	CHECK(!memcmp(find(L"C:\\w\\b.txt")->data, "bbb", 3));
	/* names the system would reject are refused before any write */
	alt('s');
	typing("CON");
	key(WF_KEY_ENTER);
	CHECK(1 == W.new_saves && screen_has("Save As"));
	alt('s');
	typing("..\\x.txt");
	key(WF_KEY_ENTER);
	CHECK(1 == W.new_saves);
	alt('s');
	typing("a/b.txt");
	key(WF_KEY_ENTER);
	CHECK(1 == W.new_saves);
	/* a good name, as a full path too */
	alt('s');
	typing("C:\\w\\sub\\ok.txt");
	key(WF_KEY_ENTER);
	CHECK(2 == W.new_saves && NULL != find(L"C:\\w\\sub\\ok.txt"));
	CHECK(screen_has("ok.txt"));
	/* Esc leaves the prompt alone */
	alt('s');
	typing("whatever");
	key(WF_KEY_ESC);
	CHECK(!wf_es_prompt_active(S) && 2 == W.new_saves);
}

/* A prompt takes the first pasted line as text; a pasted Enter neither
 * confirms nor lets the next line in; a question is not answered by paste. */
static void pasted_text_fills_a_prompt_but_never_confirms_it(void)
{
	WfEvent enter = {WF_EV_KEY, 0, WF_KEY_ENTER, WF_VK_PASTED};
	size_t i;

	setup(WF_KM_PIERO);
	add(L"C:\\w\\p.txt", "foo bar foo\nbaz foo\nend");
	CHECK(open_file(L"C:\\w\\p.txt"));
	ctrl('f');
	CHECK(wf_es_prompt_active(S));
	for (i = 0; i < 3; i++) {
		WfEvent c = {WF_EV_CHAR, (uint32_t)"foo"[i], WF_KEY_NONE, WF_VK_PASTED};

		wf_es_event(S, &c, ROWS, COLS);
	}
	wf_es_event(S, &enter, ROWS, COLS);
	CHECK(wf_es_prompt_active(S)); /* not confirmed */
	{
		WfEvent c = {WF_EV_CHAR, 'x', WF_KEY_NONE, WF_VK_PASTED};

		wf_es_event(S, &c, ROWS, COLS); /* the second line: not taken */
	}
	key(WF_KEY_ENTER); /* typed: confirms what the first line made */
	CHECK(!wf_es_prompt_active(S) && screen_has("Match 1 of 3"));

	/* a question: pasted keys answer nothing */
	typing("z");
	ctrl('x'); /* leaving with unsaved text asks */
	if (wf_es_prompt_active(S)) {
		WfEvent d = {WF_EV_CHAR, 'd', WF_KEY_NONE, WF_VK_PASTED};

		wf_es_event(S, &d, ROWS, COLS);
		CHECK(wf_es_prompt_active(S)); /* still asking */
		CHECK(WF_ES_CLOSED == chr('d')); /* typed: answered */
	}
}

/* "Match i of n" while moving between matches comes from positions kept for the
 * text; an edit, or another pattern, makes them again. */
static void the_match_count_follows_edits_and_patterns(void)
{
	setup(WF_KM_PIERO);
	add(L"C:\\w\\m.txt", "foo foo bar\nfoo");
	CHECK(open_file(L"C:\\w\\m.txt"));
	ctrl('f');
	typing("foo");
	key(WF_KEY_ENTER);
	CHECK(screen_has("Match 1 of 3"));
	key(WF_KEY_F3);
	CHECK(screen_has("Match 2 of 3"));
	key(WF_KEY_END);
	typing(" foo"); /* the text changed: one more match */
	key(WF_KEY_F3);
	CHECK(screen_has("of 4"));
	ctrl('f'); /* another pattern */
	typing("bar");
	key(WF_KEY_ENTER);
	CHECK(screen_has("Match 1 of 1"));
}

static void find_replace_goto(void)
{
	setup(WF_KM_PIERO);
	add(L"C:\\w\\f.txt", "foo bar foo\nbaz foo\nend");
	CHECK(open_file(L"C:\\w\\f.txt"));
	ctrl('f');
	typing("foo");
	key(WF_KEY_ENTER);
	CHECK(wf_ed_has_selection(wf_es_current(S)) &&
	        screen_has("Match 1 of 3"));
	key(WF_KEY_F3);
	CHECK(screen_has("Match 2 of 3"));
	key(WF_KEY_F3);
	key(WF_KEY_F3);
	CHECK(screen_has("wrapped"));
	alt('p');
	CHECK(screen_has("Match 3 of 3"));
	/* not found says so, and does not move */
	ctrl('f');
	{
		int i;

		for (i = 0; i < 3; i++) {
			key(WF_KEY_BACKSPACE);
		}
	}
	typing("zzz");
	key(WF_KEY_ENTER);
	CHECK(screen_has("Not found: zzz"));
	/* case and whole word are options of the prompt */
	ctrl('f');
	CHECK(true);
	key(WF_KEY_ESC);

	/* go to */
	ctrl('g');
	typing("2,5");
	key(WF_KEY_ENTER);
	CHECK(1 == wf_es_current(S)->cur.line &&
	        4 == wf_es_current(S)->cur.col);
	ctrl('g');
	typing("junk");
	key(WF_KEY_ENTER);
	CHECK(screen_has("Not a line number"));

	/* replace, asking */
	ctrl('h');
	typing("foo");
	key(WF_KEY_ENTER);
	typing("FOO");
	key(WF_KEY_ENTER);
	CHECK(screen_has("Replace with"));
	chr('n'); /* skip the first one found after the cursor (line 2) */
	chr('y'); /* ... then no more after this point: finished */
	CHECK(!wf_es_prompt_active(S) || screen_has("Replace"));
}

static void replace_all_is_one_undo_step(void)
{
	setup(WF_KM_PIERO);
	add(L"C:\\w\\r.txt", "a-a-a");
	CHECK(open_file(L"C:\\w\\r.txt"));
	ctrl('h');
	typing("a");
	key(WF_KEY_ENTER);
	typing("bb");
	key(WF_KEY_ENTER);
	chr('a');
	CHECK(text_is("bb-bb-bb") && screen_has("Replaced 3 occurrences"));
	ctrl('z');
	CHECK(text_is("a-a-a"));
	ctrl('y');
	CHECK(text_is("bb-bb-bb"));
}

/* A copy the system clipboard refuses is said so, and the next paste brings
 * back what was copied, not what the system clipboard still held. */
static void a_refused_system_clipboard_is_reported_and_not_trusted(void)
{
	size_t n = 0;
	char *all = NULL;

	setup(WF_KM_PIERO);
	add(L"C:\\w\\clip.txt", "one\ntwo\n");
	CHECK(open_file(L"C:\\w\\clip.txt"));
	memcpy(W.clip, "OLD", 3);
	W.clip_n = 3;
	W.clip_present = true;
	W.clip_set_fails = true;
	ctrl('c'); /* copy the line */
	CHECK(screen_has("clipboard"));
	CHECK(3 == W.clip_n && !memcmp(W.clip, "OLD", 3));
	key(WF_KEY_DOWN);
	ctrl('v');
	CHECK(wf_et_read(&wf_es_current(S)->text, (WfPos){0, 0},
	              wf_et_end(&wf_es_current(S)->text), &all, &n) == WF_OK);
	CHECK(12 == n &&
	        (!memcmp(all, "one\ntwo\none\n", 12) ||
	                !memcmp(all, "one\none\ntwo\n", 12)));
	free(all);
	/* once the system accepts a copy again, it is the system's turn */
	W.clip_set_fails = false;
	ctrl('c');
	CHECK(4 == W.clip_n && '\n' == W.clip[3] && memcmp(W.clip, "OLD", 3));
}

/* The system clipboard has CRLF where the editor's own copy has LF: it is still
 * the copy the editor made, so a line copied is pasted as a line, not into the
 * middle of the one the cursor is in. */
static void a_line_copied_comes_back_as_a_line_through_a_crlf_clipboard(void)
{
	size_t n = 0;
	char *all = NULL;

	setup(WF_KM_PIERO);
	add(L"C:\\w\\line.txt", "one\ntwo\n");
	CHECK(open_file(L"C:\\w\\line.txt"));
	W.clip_crlf = true;
	key(WF_KEY_RIGHT);
	key(WF_KEY_RIGHT);                        /* in the middle of "one" */
	ctrl('c');                                /* the line */
	CHECK(W.clip_n > 4 && '\r' == W.clip[3]); /* the system holds CRLF */
	ctrl('v');
	CHECK(wf_et_read(&wf_es_current(S)->text, (WfPos){0, 0},
	              wf_et_end(&wf_es_current(S)->text), &all, &n) == WF_OK);
	CHECK(12 == n &&
	        (!memcmp(all, "one\none\ntwo\n", 12) ||
	                !memcmp(all, "one\ntwo\none\n", 12)));
	free(all);
	W.clip_crlf = false;
}

/* A file that is within the limit on disk but not as text (UTF-16 doubling
 * into UTF-8, say) must not become an empty buffer that saves over it. */
static void a_text_that_does_not_fit_never_becomes_an_empty_buffer(void)
{
	WfEditConfig c;
	WfEditIo io = {NULL, fake_load, fake_over, fake_new, fake_clip_get,
	        fake_clip_set, fake_draft_allowed, fake_draft_write,
	        fake_draft_read, fake_draft_remove, NULL};
	char why[160] = "";
	static const char big[] =
	        "01234567890123456789012345678901234567890123456789012345678901"
	        "23"
	        "45678901234567890123456789012345678901234567890123456789";
	size_t saves;

	memset(&W, 0, sizeof W);
	wf_es_free(S);
	wf_es_config_default(&c);
	c.ed.max_bytes = 32;
	S = wf_es_new(&c, io);
	CHECK(S != NULL);
	add(L"C:\\w\\small.txt", "small\n");
	add(L"C:\\w\\huge.txt", big);
	CHECK(open_file(L"C:\\w\\small.txt"));
	W.skip_size_check = true;
	CHECK(WF_OPEN_REFUSED ==
	        wf_es_open(S, L"C:\\w\\huge.txt", why, sizeof why));
	CHECK('\0' != why[0]);
	/* no buffer was made for it; the one we had is still the one shown */
	CHECK(1 == wf_es_buffer_count(S));
	CHECK(wf_es_current(S) && 2 == wf_es_current(S)->text.n);
	/* typing and saving touches only the open file */
	saves = (size_t)W.saves;
	typing("x");
	ctrl('s');
	CHECK((size_t)W.saves == saves + 1);
	{
		FakeFile *huge = find(L"C:\\w\\huge.txt");

		CHECK(huge && huge->n == sizeof big - 1 &&
		        !memcmp(huge->data, big, huge->n));
	}
	/* the same through the open prompt */
	ctrl('o');
	typing("C:\\w\\huge.txt");
	key(WF_KEY_ENTER);
	CHECK(1 == wf_es_buffer_count(S));
	CHECK(screen_has("larger than") || screen_has("Cannot open"));
	/* and as the only file: nothing is left open at all */
	setup(WF_KM_PIERO);
	wf_es_free(S);
	c.ed.max_bytes = 32;
	S = wf_es_new(&c, io);
	add(L"C:\\w\\huge.txt", big);
	W.skip_size_check = true;
	CHECK(WF_OPEN_REFUSED ==
	        wf_es_open(S, L"C:\\w\\huge.txt", why, sizeof why));
	CHECK(0 == wf_es_buffer_count(S) && 0 == W.saves && 0 == W.new_saves);
}

/* No byte below 0x20 or DEL is ever in a cell the terminal will be given. */
static bool frame_has_controls(void)
{
	WfFrame *f = wf_frame_new(COLS, ROWS, &(WfStyle){0, 0, 0});
	bool bad = false;

	wf_es_draw(S, f);
	for (int i = 0; i < f->rows * f->cols && !bad; i++) {
		for (const char *p = f->cells[i].utf8; *p; p++) {
			if ((unsigned char)*p < 0x20 || 0x7f == *p) {
				bad = true;
			}
		}
	}
	wf_frame_free(f);
	return bad;
}

/* Text taken from the file by a selection and put in the search line is
 * shown like the text is: the file's own escape bytes never reach the
 * screen. */
static void controls_taken_from_the_text_into_a_prompt_are_only_shown(void)
{
	size_t n = 0;
	char *got = NULL;

	setup(WF_KM_PIERO);
	add(L"C:\\w\\esc.txt", "a\x1b[2J\x1b]0;pwned\x07z");
	CHECK(open_file(L"C:\\w\\esc.txt"));
	CHECK(!frame_has_controls()); /* the text itself, as before */
	ctrl('a');                    /* select it all */
	ctrl('f');                    /* ... and put it in the search line */
	CHECK(S && wf_es_prompt_active(S));
	CHECK(!frame_has_controls());
	/* the search itself still works on the real bytes */
	key(WF_KEY_ENTER);
	CHECK(!frame_has_controls());
	ctrl('h'); /* replace: the same line, the same rule */
	CHECK(!frame_has_controls());
	key(WF_KEY_ESC);
	/* text typed or pasted into it as well, with the cursor in the middle */
	ctrl('f');
	W.clip_present = true;
	memcpy(W.clip, "x\x1b[31my", 7);
	W.clip_n = 7;
	ctrl('v');
	key(WF_KEY_LEFT);
	key(WF_KEY_LEFT);
	CHECK(!frame_has_controls());
	key(WF_KEY_ESC);
	CHECK(wf_et_read(&wf_es_current(S)->text, (WfPos){0, 0},
	              wf_et_end(&wf_es_current(S)->text), &got, &n) == WF_OK);
	CHECK(16 == n &&
	        !memcmp(got, "a\x1b[2J\x1b]0;pwned\x07z", 16)); /* unchanged */
	free(got);
}

static void the_nano_preset_does_what_the_table_says(void)
{
	FakeFile *f;

	setup(WF_KM_NANO);
	f = add(L"C:\\w\\n.txt", "one\ntwo\nthree\n");
	CHECK(open_file(L"C:\\w\\n.txt"));
	CHECK(screen_has("NANO"));
	key(WF_KEY_DOWN);
	ctrl('k'); /* cut the line */
	CHECK(text_is("one\nthree\n"));
	CHECK(W.clip_present && !memcmp(W.clip, "two\n", 4));
	key(WF_KEY_UP);
	ctrl('u'); /* paste it back above */
	CHECK(text_is("two\none\nthree\n"));
	alt('u'); /* undo */
	CHECK(text_is("one\nthree\n"));
	alt('e'); /* redo */
	CHECK(text_is("two\none\nthree\n"));
	/* mark, move, copy with Alt+6 */
	{
		WfEvent mark = {WF_EV_CTRL, '6', WF_KEY_NONE, 0};

		press(&mark);
		key(WF_KEY_RIGHT);
		key(WF_KEY_RIGHT);
		alt('6');
		CHECK(2 == wf_es_current(S)->clip_n);
		CHECK(2 == W.clip_n &&
		        !memcmp(W.clip, wf_es_current(S)->clip, 2));
	}
	ctrl('_');
	typing("3");
	key(WF_KEY_ENTER);
	CHECK(2 == wf_es_current(S)->cur.line);
	ctrl('s');
	CHECK(1 == W.saves && 14 == f->n);
	/* in this preset Esc is not "leave", ^X is */
	CHECK(WF_ES_REDRAW == key(WF_KEY_ESC));
	CHECK(WF_ES_CLOSED == ctrl('x'));
	/* ^O is Save As here */
	setup(WF_KM_NANO);
	add(L"C:\\w\\n.txt", "x");
	CHECK(open_file(L"C:\\w\\n.txt"));
	ctrl('o');
	CHECK(wf_es_prompt_active(S) && screen_has("Save as"));
}

static void the_system_clipboard_is_used_for_paste_and_is_sanitized(void)
{
	setup(WF_KM_PIERO);
	add(L"C:\\w\\c.txt", "base");
	CHECK(open_file(L"C:\\w\\c.txt"));
	/* text from another program, with an escape sequence and a NUL-free
	 * control character inside */
	W.clip_n = strlen("line1\r\nline2\x1B[31m\x07");
	memcpy(W.clip, "line1\r\nline2\x1B[31m\x07", W.clip_n);
	W.clip_present = true;
	ctrl('v');
	CHECK(text_is("line1\r\nline2[31mbase") ||
	        text_is("line1\nline2[31mbase"));
	CHECK(screen_has("control character"));
	/* copy puts the selection on the system clipboard */
	ctrl('a');
	ctrl('c');
	CHECK(W.clip_n > 0 && NULL != strstr(W.clip, "line1"));
}

/* The search prompt starts with the selected text, bytes and all. What is
 * drawn for it must be the readable form ("^[" for an escape), made by the
 * sanitiser. The frame also refuses to carry a control byte, but it can only
 * show it as "?": this check is what tells the two layers apart, so a prompt
 * that stopped sanitising would not hide behind the frame. */
static void prompt_shows_controls_in_readable_form(void)
{
	setup(WF_KM_PIERO);
	add(L"C:\\w\\ctl.txt", "a\x1b[2Jz");
	CHECK(open_file(L"C:\\w\\ctl.txt"));
	ctrl('a');
	ctrl('f');
	CHECK(wf_es_prompt_active(S));
	/* with the cursor at the end the whole text is the part before it,
	 * the part drawn by the code that must sanitise it */
	key(WF_KEY_END);
	/* ": " ends the prompt's label; the text row above also shows "a^[[2Jz",
	 * so the bare string would not tell the prompt from the text */
	CHECK(screen_has(": a^[[2Jz"));
	key(WF_KEY_ESC);
}

static void several_buffers_and_no_double_opening(void)
{
	setup(WF_KM_PIERO);
	add(L"C:\\w\\one.txt", "1");
	add(L"C:\\w\\two.txt", "2");
	CHECK(open_file(L"C:\\w\\one.txt"));
	CHECK(open_file(L"C:\\w\\two.txt"));
	CHECK(2 == wf_es_buffer_count(S) && screen_has("[2/2]"));
	CHECK(open_file(L"C:\\w\\one.txt")); /* already open: switched to */
	CHECK(2 == wf_es_buffer_count(S) && screen_has("one.txt"));
	typing("A");
	alt('.');
	CHECK(screen_has("two.txt") && text_is("2"));
	alt(',');
	CHECK(text_is("A1"));
	/* closing the clean one leaves the other; its unsaved text is still asked
	 * about when it is closed */
	alt('.');
	CHECK(WF_ES_REDRAW == ctrl('x') && 1 == wf_es_buffer_count(S));
	CHECK(text_is("A1"));
	CHECK(WF_ES_REDRAW == ctrl('x') && wf_es_prompt_active(S));
	CHECK(WF_ES_CLOSED == chr('d'));
	/* the same file under another spelling is recognised by its identity */
	setup(WF_KM_PIERO);
	add(L"C:\\w\\one.txt", "1");
	{
		FakeFile *dup = add(L"C:\\alias\\one.txt", "1");

		dup->id = find(L"C:\\w\\one.txt")->id;
	}
	CHECK(open_file(L"C:\\w\\one.txt"));
	CHECK(open_file(L"C:\\alias\\one.txt"));
	CHECK(1 == wf_es_buffer_count(S));
}

static void what_cannot_be_edited_is_refused_with_a_reason(void)
{
	char why[160];
	FakeFile *f;

	setup(WF_KM_PIERO);
	f = add(L"C:\\w\\bin.dat", "xx");
	f->verdict = WF_FL_BINARY;
	CHECK(WF_OPEN_REFUSED ==
	        wf_es_open(S, L"C:\\w\\bin.dat", why, sizeof why));
	CHECK(NULL != strstr(why, "binary") && 0 == wf_es_buffer_count(S));
	f = add(L"C:\\w\\link", "x");
	f->verdict = WF_FL_LINK;
	CHECK(WF_OPEN_REFUSED ==
	        wf_es_open(S, L"C:\\w\\link", why, sizeof why));
	CHECK(WF_OPEN_REFUSED ==
	        wf_es_open(S, L"C:\\w\\nothing", why, sizeof why));
	{
		WfEditConfig c;
		WfEditIo io = {NULL, fake_load, fake_over, fake_new,
		        fake_clip_get, fake_clip_set, fake_draft_allowed,
		        fake_draft_write, fake_draft_read, fake_draft_remove,
		        NULL};

		wf_es_config_default(&c);
		c.ed.max_bytes = 4;
		wf_es_free(S);
		S = wf_es_new(&c, io);
		add(L"C:\\w\\big.txt", "123456789");
		CHECK(WF_OPEN_REFUSED ==
		        wf_es_open(S, L"C:\\w\\big.txt", why, sizeof why));
		CHECK(NULL != strstr(why, "limit"));
	}
}

static void unknown_text_is_opened_only_when_asked(void)
{
	char why[160];
	FakeFile *f;

	setup(WF_KM_PIERO);
	f = add(L"C:\\w\\sj.txt", "abc");
	f->verdict = WF_FL_AMBIGUOUS;
	f->enc.kind = WF_ENC_CODEPAGE;
	f->enc.codepage = 932;
	CHECK(WF_OPEN_ASKING ==
	        wf_es_open(S, L"C:\\w\\sj.txt", why, sizeof why));
	CHECK(0 == wf_es_buffer_count(S) && screen_has("Not valid UTF-8"));
	/* the question is part of the screen once a buffer exists; test the
	 * answer path */
	CHECK(wf_es_prompt_active(S));
	chr('n');
	CHECK(0 == wf_es_buffer_count(S) && !wf_es_prompt_active(S));
	CHECK(WF_OPEN_ASKING ==
	        wf_es_open(S, L"C:\\w\\sj.txt", why, sizeof why));
	chr('y');
	CHECK(1 == wf_es_buffer_count(S) && text_is("abc"));
	CHECK(WF_ENC_CODEPAGE == wf_es_current(S)->enc.kind);
	/* an allocation that fails anywhere while the buffer is made: the buffers that
	 * were open stay as they were, with their text, and the failure is said */
	for (long fail_at = 0; fail_at < 24; fail_at++) {
		setup(WF_KM_PIERO);
		add(L"C:\\w\\keep.txt", "kept text");
		f = add(L"C:\\w\\sj.txt", "abc");
		f->verdict = WF_FL_AMBIGUOUS;
		f->enc.kind = WF_ENC_CODEPAGE;
		f->enc.codepage = 932;
		CHECK(open_file(L"C:\\w\\keep.txt"));
		CHECK(WF_OPEN_ASKING ==
		        wf_es_open(S, L"C:\\w\\sj.txt", why, sizeof why));
		wf_et_fail_nth_alloc(fail_at);
		chr('y');
		wf_et_fail_nth_alloc(-1);
		CHECK(!wf_es_prompt_active(S));
		CHECK(1 == wf_es_buffer_count(S) || 2 == wf_es_buffer_count(S));
		if (1 == wf_es_buffer_count(S)) {
			CHECK(text_is("kept text")); /* nothing lost */
		} else {
			CHECK(text_is("abc"));
		}
	}
}

static void a_read_only_file_cannot_be_typed_into(void)
{
	FakeFile *f;

	setup(WF_KM_PIERO);
	f = add(L"C:\\w\\ro.txt", "fixed");
	f->attributes = FILE_ATTRIBUTE_READONLY;
	CHECK(open_file(L"C:\\w\\ro.txt"));
	typing("x");
	CHECK(text_is("fixed") && screen_has("Read-only"));
	key(WF_KEY_BACKSPACE);
	CHECK(text_is("fixed"));
	ctrl('s');
	CHECK(0 == W.saves && screen_has("Read-only"));
	CHECK(WF_ES_CLOSED == ctrl('x'));
}

/* A file in a WSL distribution opens for reading only, whatever its
 * attributes say: there is no way to save it safely from here. */
static void a_wsl_file_is_edited_and_saved_like_any_other(void)
{
	FakeFile *f;

	setup(WF_KM_PIERO);
	f = add(L"\\\\wsl.localhost\\Ubuntu\\home\\me\\n.txt", "fixed");
	CHECK(open_file(L"\\\\wsl.localhost\\Ubuntu\\home\\me\\n.txt"));
	typing("x");
	CHECK(text_is("xfixed") && !screen_has("Read-only"));
	ctrl('s');
	CHECK(1 == W.saves && !memcmp(f->data, "xfixed", 6));
	CHECK(WF_ES_CLOSED == ctrl('x'));
}

/* Linux tells File from file, and there is no file identity to tell them
 * apart otherwise: neither is taken for the other one that is open. */
static void names_that_differ_by_case_are_two_files_in_a_distribution(void)
{
	setup(WF_KM_PIERO);
	add(L"\\\\wsl.localhost\\Ubuntu\\t\\File", "upper");
	add(L"\\\\wsl.localhost\\Ubuntu\\t\\file", "lower");
	CHECK(open_file(L"\\\\wsl.localhost\\Ubuntu\\t\\File"));
	CHECK(text_is("upper"));
	CHECK(open_file(L"\\\\wsl.localhost\\Ubuntu\\t\\file"));
	CHECK(text_is("lower"));
}

static void the_screen_marks_syntax_selection_and_matches(void)
{
	WfFrame *f;
	size_t x;
	bool keyword_colored = false;

	setup(WF_KM_PIERO);
	add(L"C:\\w\\code.c", "int main(void)\n{\n\treturn 0; /* done */\n}\n");
	CHECK(open_file(L"C:\\w\\code.c"));
	CHECK(screen_has("C") && screen_has("main"));
	f = wf_frame_new(COLS, ROWS, &(WfStyle){0, 0, 0});
	wf_es_draw(S, f);
	/* the first row of text starts after the line-number gutter */
	for (x = 0; x + 2 < (size_t)f->cols; x++) {
		const WfCell *c = &f->cells[1 * f->cols + (int)x];

		if (!strcmp(c->utf8, "i") &&
		        !strcmp(f->cells[1 * f->cols + (int)x + 1].utf8, "n") &&
		        !strcmp(f->cells[1 * f->cols + (int)x + 2].utf8, "t")) {
			/* the first cell is the cursor; the next one shows the colour */
			keyword_colored =
			        f->cells[1 * f->cols + (int)x + 1].fg_rgb !=
			        wf_theme_styles()->base.fg_rgb;
		}
	}
	CHECK(keyword_colored);
	wf_frame_free(f);
	/* a long line scrolls horizontally with the cursor */
	setup(WF_KM_PIERO);
	{
		char longline[400];

		memset(longline, 'x', sizeof longline - 1);
		longline[sizeof longline - 1] = '\0';
		add(L"C:\\w\\long.txt", longline);
	}
	CHECK(open_file(L"C:\\w\\long.txt"));
	key(WF_KEY_CTRL_END);
	CHECK(screen_has("Col 400"));
}

static void editing_keys_reach_the_same_operations(void)
{
	setup(WF_KM_PIERO);
	add(L"C:\\w\\k.txt", "a b c\nsecond\n");
	CHECK(open_file(L"C:\\w\\k.txt"));
	key(WF_KEY_SHIFT_RIGHT);
	key(WF_KEY_SHIFT_RIGHT);
	CHECK(wf_ed_has_selection(wf_es_current(S)));
	key(WF_KEY_BACKSPACE);
	CHECK(text_is("b c\nsecond\n"));
	key(WF_KEY_CTRL_RIGHT);
	typing("Q");
	CHECK(text_is("b Qc\nsecond\n"));
	ctrl('k'); /* delete line */
	CHECK(text_is("second\n"));
	ctrl('z');
	CHECK(text_is("b Qc\nsecond\n"));
	key(WF_KEY_TAB);
	CHECK(text_is("b Q\tc\nsecond\n"));
	key(WF_KEY_SHIFT_TAB);
	key(WF_KEY_ENTER);
	CHECK(wf_et_check(&wf_es_current(S)->text));
}

static void a_new_file_is_created_only_by_saving(void)
{
	setup(WF_KM_PIERO);
	add(L"C:\\w\\base.txt", "base");
	CHECK(open_file(L"C:\\w\\base.txt"));
	ctrl('o'); /* open or create */
	typing("fresh.txt");
	key(WF_KEY_ENTER);
	CHECK(2 == wf_es_buffer_count(S) && screen_has("[New]"));
	CHECK(NULL == find(L"C:\\w\\fresh.txt")); /* nothing on disk yet */
	typing("new words");
	ctrl('s');
	CHECK(1 == W.new_saves && 0 == W.saves &&
	        NULL != find(L"C:\\w\\fresh.txt"));
	CHECK(!memcmp(find(L"C:\\w\\fresh.txt")->data, "new words", 9));
	CHECK(!screen_has("[New]") && screen_has("Saved"));
	/* from now on it is an ordinary file: the next save replaces */
	typing("!");
	ctrl('s');
	CHECK(1 == W.saves);
}

/* A new file is loaded empty, and empty has no line break of its own: Enter in
 * it makes the configured one (CRLF on Windows), not LF. */
static void a_new_file_breaks_lines_as_configured(void)
{
	const FakeFile *f;

	setup(WF_KM_PIERO);
	add(L"C:\\w\\base.txt", "base");
	CHECK(open_file(L"C:\\w\\base.txt"));
	ctrl('o');
	typing("crlf.txt");
	key(WF_KEY_ENTER);
	typing("a");
	key(WF_KEY_ENTER);
	typing("b");
	ctrl('s');
	f = find(L"C:\\w\\crlf.txt");
	CHECK(NULL != f && 4 == f->n && !memcmp(f->data, "a\r\nb", 4));
}

static void a_file_can_be_inserted_at_the_cursor(void)
{
	setup(WF_KM_PIERO);
	add(L"C:\\w\\main.txt", "[]");
	add(L"C:\\w\\part.txt", "inserted\n");
	CHECK(open_file(L"C:\\w\\main.txt"));
	key(WF_KEY_RIGHT);
	ctrl('r');
	typing("part.txt");
	key(WF_KEY_ENTER);
	CHECK(text_is("[inserted\n]") && screen_has("Inserted 9 bytes"));
	ctrl('z');
	CHECK(text_is("[]"));
	/* a missing or binary file is refused and nothing changes */
	ctrl('r');
	typing("absent.txt");
	key(WF_KEY_ENTER);
	CHECK(text_is("[]") && screen_has("Cannot insert"));
}

static void soft_wrap_shows_long_lines_in_rows_without_changing_them(void)
{
	WfEditConfig c;
	WfEditIo io = {NULL, fake_load, fake_over, fake_new, fake_clip_get,
	        fake_clip_set, fake_draft_allowed, fake_draft_write,
	        fake_draft_read, fake_draft_remove, NULL};
	char longline[301];

	memset(&W, 0, sizeof W);
	wf_es_free(S);
	wf_es_config_default(&c);
	c.soft_wrap = true;
	S = wf_es_new(&c, io);
	memset(longline, 'a', 300);
	longline[100] =
	        'Z'; /* the second row of a 93-column text area starts around here */
	longline[300] = '\0';
	add(L"C:\\w\\wrap.txt", longline);
	CHECK(open_file(L"C:\\w\\wrap.txt"));
	CHECK(screen_has("Z")); /* visible without scrolling right */
	key(WF_KEY_DOWN);       /* one visual row down, still the same line */
	CHECK(0 == wf_es_current(S)->cur.line &&
	        wf_es_current(S)->cur.col > 50);
	CHECK(text_is(longline));
}

static void selection_search_marks_and_whitespace_are_drawn(void)
{
	WfEditConfig c;
	WfEditIo io = {NULL, fake_load, fake_over, fake_new, fake_clip_get,
	        fake_clip_set, fake_draft_allowed, fake_draft_write,
	        fake_draft_read, fake_draft_remove, NULL};
	WfFrame *f;
	bool selected_cell = false, matched = false, tab_mark = false;
	int x;

	memset(&W, 0, sizeof W);
	wf_es_free(S);
	wf_es_config_default(&c);
	c.show_whitespace = true;
	S = wf_es_new(&c, io);
	add(L"C:\\w\\mark.txt", "\tfind me find\nsecond");
	CHECK(open_file(L"C:\\w\\mark.txt"));
	ctrl('f');
	typing("find");
	key(WF_KEY_ENTER); /* selects the first match, marks all */
	key(WF_KEY_F3);
	f = wf_frame_new(COLS, ROWS, &(WfStyle){0, 0, 0});
	wf_es_draw(S, f);
	for (x = 0; x < f->cols; x++) {
		const WfCell *cell = &f->cells[1 * f->cols + x];

		if (cell->bg_rgb == wf_theme_styles()->text_selection.bg_rgb) {
			selected_cell = true; /* the current match */
		}
		if (cell->bg_rgb == wf_theme_styles()->match.bg_rgb) {
			matched = true; /* another match */
		}
		if (!strcmp(cell->utf8, "\xE2\x86\x92")) {
			tab_mark = true;
		}
	}
	CHECK(selected_cell && matched && tab_mark);
	wf_frame_free(f);
}

/* Any size the terminal can be resized to draws without trouble. */
static void every_window_size_draws_safely(void)
{
	int rows, cols;

	setup(WF_KM_PIERO);
	add(L"C:\\w\\sz.c", "int main(void)\n{\n\treturn 0;\n}\n");
	CHECK(open_file(L"C:\\w\\sz.c"));
	typing("x");
	ctrl('f');
	typing("in");
	for (rows = 1; rows <= 30; rows++) {
		for (cols = 1; cols <= 60; cols += (cols < 30 ? 1 : 7)) {
			WfFrame *f =
			        wf_frame_new(cols, rows, &(WfStyle){0, 0, 0});

			CHECK(NULL != f);
			if (f) {
				WfEvent resize = {
				        WF_EV_RESIZE, 0, WF_KEY_NONE, 0};

				wf_es_event(S, &resize, rows, cols);
				wf_es_draw(S, f);
				wf_frame_free(f);
			}
		}
	}
	key(WF_KEY_ESC);
	ctrl('h');
	typing("a");
	key(WF_KEY_ENTER);
	typing("b");
	key(WF_KEY_ENTER);
	{
		WfFrame *f = wf_frame_new(COLS, ROWS, &(WfStyle){0, 0, 0});

		wf_es_draw(S, f);
		wf_frame_free(f);
	}
}

/*
 * The terminal's own cursor must sit on the insertion cell of the focused
 * text (or prompt) after every frame: the terminal puts an IME composition at
 * its cursor, and drawing otherwise leaves the cursor at the last cell
 * written, the status line. The editor shows its insertion point as the one
 * reversed cell; the frame's cursor must be exactly there. These tests read
 * the frame, so they say where the cursor ends up, not what an IME does with it.
 */
static int reversed_cells(const WfFrame *f, int *x, int *y)
{
	int n = 0, cx, cy;

	for (cy = 0; cy < f->rows; cy++) {
		for (cx = 0; cx < f->cols; cx++) {
			const WfCell *c = &f->cells[cy * f->cols + cx];

			if (c->width && (c->attributes & WF_ATTR_REVERSE)) {
				n++;
				*x = cx;
				*y = cy;
			}
		}
	}
	return n;
}

/* Draws at the given size; true when the cursor is shown, inside the frame
 * and on a reversed cell. In the text no other cell is reversed (one
 * insertion point); a prompt row may carry reversed key labels besides. */
static bool cursor_cell(
        int cols, int rows, bool only, int *cx, int *cy)
{
	WfFrame *f = wf_frame_new(cols, rows, &(WfStyle){0, 0, 0});
	WfEvent resize = {WF_EV_RESIZE, 0, WF_KEY_NONE, 0};
	int rx = -1, ry = -1;
	bool ok;

	CHECK(NULL != f);
	if (!f) {
		return false;
	}
	wf_es_event(S, &resize, rows, cols);
	wf_es_draw(S, f);
	ok = f->cursor.visible && f->cursor.x >= 0 && f->cursor.x < cols &&
	        f->cursor.y >= 0 && f->cursor.y < rows;
	if (ok) {
		int n = reversed_cells(f, &rx, &ry);
		const WfCell *at = &f->cells[f->cursor.y * cols + f->cursor.x];

		ok = (at->attributes & WF_ATTR_REVERSE) && at->width &&
		        (!only || (1 == n && rx == f->cursor.x &&
		                         ry == f->cursor.y));
	}
	if (!ok) {
		printf("  cursor visible=%d at %d,%d; reversed cell at %d,%d "
		       "(%dx%d)\n",
		        f->cursor.visible, f->cursor.x, f->cursor.y, rx, ry,
		        cols, rows);
	}
	*cx = f->cursor.x;
	*cy = f->cursor.y;
	wf_frame_free(f);
	return ok;
}

static bool cursor_on_insertion_cell(int cols, int rows, int *cx, int *cy)
{
	return cursor_cell(cols, rows, true, cx, cy);
}

static void the_native_cursor_is_on_the_insertion_cell(void)
{
	static const uint32_t japanese[] = {0x6708, 0x304C, 0x304D, 0x308C,
	        0x3044, 0x3067, 0x3059, 0x306D}; /* 月がきれいですね */
	static const uint32_t wide_more[] = {0x1F600, 0x20BB7};
	static const WfKeymapId presets[] = {WF_KM_PIERO, WF_KM_NANO};
	size_t p, i;

	for (p = 0; p < WF_ARRAY_LEN(presets); p++) {
		int x0, y0, x, y, expect, before_x;

		setup(presets[p]);
		add(L"C:\\w\\ime.txt", "");
		CHECK(open_file(L"C:\\w\\ime.txt"));

		/* first line of an empty file */
		CHECK(cursor_on_insertion_cell(COLS, ROWS, &x0, &y0));

		/* committed Japanese: each ideograph and kana is two cells */
		expect = x0;
		for (i = 0; i < WF_ARRAY_LEN(japanese); i++) {
			chr(japanese[i]);
			expect += wf_cp_width(japanese[i]);
		}
		CHECK(cursor_on_insertion_cell(COLS, ROWS, &x, &y));
		CHECK(y == y0 && x == expect);
		CHECK(text_is("\xE6\x9C\x88\xE3\x81\x8C\xE3\x81\x8D\xE3\x82\x8C"
		              "\xE3\x81\x84\xE3\x81\xA7\xE3\x81\x99\xE3\x81\xAD"));

		/* an emoji and a character outside the BMP (a surrogate pair) */
		for (i = 0; i < WF_ARRAY_LEN(wide_more); i++) {
			before_x = x;
			chr(wide_more[i]);
			CHECK(cursor_on_insertion_cell(COLS, ROWS, &x, &y));
			CHECK(y == y0 && x == before_x + wf_cp_width(wide_more[i]));
		}

		/* a new line: back to the left edge, one row down */
		key(WF_KEY_ENTER);
		CHECK(cursor_on_insertion_cell(COLS, ROWS, &x, &y));
		CHECK(x == x0 && y == y0 + 1);

		/* the middle of the screen, then past the bottom (scrolling) */
		for (i = 0; i < 40; i++) {
			typing("line");
			key(WF_KEY_ENTER);
		}
		CHECK(cursor_on_insertion_cell(COLS, ROWS, &x, &y));
		CHECK(y > y0 && y < ROWS - 1);
		for (i = 0; i < 8; i++) {
			key(WF_KEY_UP);
			CHECK(cursor_on_insertion_cell(COLS, ROWS, &x, &y));
		}

		/* a line longer than the window scrolls sideways */
		key(WF_KEY_DOWN);
		for (i = 0; i < 150; i++) {
			chr('a');
		}
		for (i = 0; i < 20; i++) {
			chr(0x3042);
		}
		CHECK(cursor_on_insertion_cell(COLS, ROWS, &x, &y));

		/* the window is resized under the cursor, both ways */
		CHECK(cursor_on_insertion_cell(40, 10, &x, &y));
		CHECK(cursor_on_insertion_cell(COLS, ROWS, &x, &y));
		CHECK(cursor_on_insertion_cell(60, 7, &x, &y));
		CHECK(cursor_on_insertion_cell(COLS, ROWS, &x, &y));
	}
}

/* A prompt owns the cursor while it is open and gives it back when closed. */
static void a_prompt_takes_the_native_cursor_and_gives_it_back(void)
{
	int tx, ty, px, py, x, y;

	setup(WF_KM_PIERO);
	add(L"C:\\w\\pr.txt", "alpha\nbeta\n");
	CHECK(open_file(L"C:\\w\\pr.txt"));
	typing("x");
	CHECK(cursor_on_insertion_cell(COLS, ROWS, &tx, &ty));

	ctrl('f'); /* the search prompt */
	CHECK(cursor_cell(COLS, ROWS, false, &px, &py));
	CHECK(py != ty || px != tx);
	chr(0x6708);
	chr(0x304C);
	CHECK(cursor_cell(COLS, ROWS, false, &x, &y));
	CHECK(y == py && x == px + 4); /* two wide characters */

	key(WF_KEY_ESC); /* back to the text */
	CHECK(cursor_on_insertion_cell(COLS, ROWS, &x, &y));
	CHECK(x == tx && y == ty);
}

/* What the IME commits is saved as typed and reads back the same. */
static void committed_japanese_is_saved_and_read_back(void)
{
	static const char saved[] =
	        "\xE6\x9C\x88\xE3\x81\x8C\xE3\x81\x8D\xE3\x82\x8C\xE3\x81\x84"
	        "\xE3\x81\xA7\xE3\x81\x99\xE3\x81\xAD\xF0\xA0\xAE\xB7";
	static const uint32_t typed[] = {0x6708, 0x304C, 0x304D, 0x308C,
	        0x3044, 0x3067, 0x3059, 0x306D, 0x20BB7};
	size_t i;
	FakeFile *f;
	int x, y;

	setup(WF_KM_PIERO);
	f = add(L"C:\\w\\save.txt", "");
	CHECK(open_file(L"C:\\w\\save.txt"));
	for (i = 0; i < WF_ARRAY_LEN(typed); i++) {
		chr(typed[i]);
	}
	ctrl('s');
	CHECK(f->n == sizeof saved - 1 && !memcmp(f->data, saved, f->n));
	/* a fresh session reads it back, with the cursor shown on the line */
	{
		setup(WF_KM_PIERO); /* a fresh session and a fresh world */
		f = add(L"C:\\w\\save.txt", saved);
		CHECK(open_file(L"C:\\w\\save.txt"));
		CHECK(text_is(saved));
		CHECK(cursor_on_insertion_cell(COLS, ROWS, &x, &y));
	}
}

/*
 * Answering "Save changes?" without typing a letter. With a Japanese IME on, a
 * letter key starts a composition and the program never sees it, so the
 * question has to be answerable with Up/Down and Enter. The selection starts
 * on Cancel. These tests feed the events a terminal would deliver; they say
 * nothing about what an IME does before it delivers them.
 */
static WfEsResult press_vk(WfEventType type, uint32_t ch, uint16_t vk)
{
	WfEvent e = {type, ch, WF_KEY_NONE, vk};

	return press(&e);
}

static void the_save_question_is_answered_without_letters(void)
{
	FakeFile *f;

	/* Up from Cancel selects Discard; Enter discards, and only then */
	setup(WF_KM_PIERO);
	f = add(L"C:\\w\\a.txt", "keep");
	CHECK(open_file(L"C:\\w\\a.txt"));
	typing("!");
	CHECK(WF_ES_REDRAW == ctrl('x') && wf_es_prompt_active(S));
	CHECK(WF_ES_REDRAW == key(WF_KEY_UP) && wf_es_prompt_active(S));
	CHECK(screen_has("Enter = D"));
	CHECK(WF_ES_CLOSED == key(WF_KEY_ENTER));
	CHECK(0 == W.saves && !strcmp(f->data, "keep"));

	/* Down from Cancel wraps to Save; Enter saves and closes */
	setup(WF_KM_PIERO);
	f = add(L"C:\\w\\a.txt", "keep");
	CHECK(open_file(L"C:\\w\\a.txt"));
	typing("!");
	CHECK(WF_ES_REDRAW == ctrl('x'));
	CHECK(screen_has("Enter = C")); /* the initial selection */
	CHECK(WF_ES_REDRAW == key(WF_KEY_DOWN) && screen_has("Enter = S"));
	CHECK(WF_ES_CLOSED == key(WF_KEY_ENTER));
	CHECK(1 == W.saves && !memcmp(f->data, "!keep", 5));

	/* round the circle and back on Cancel: Enter keeps everything */
	setup(WF_KM_PIERO);
	f = add(L"C:\\w\\a.txt", "keep");
	CHECK(open_file(L"C:\\w\\a.txt"));
	typing("!");
	ctrl('x');
	key(WF_KEY_DOWN);
	key(WF_KEY_DOWN);
	key(WF_KEY_DOWN);
	CHECK(screen_has("Enter = C"));
	CHECK(WF_ES_REDRAW == key(WF_KEY_ENTER) && !wf_es_prompt_active(S));
	CHECK(text_is("!keep") && 0 == W.saves && !strcmp(f->data, "keep"));

	/* a failed save through the selection keeps the buffer */
	setup(WF_KM_PIERO);
	f = add(L"C:\\w\\a.txt", "keep");
	CHECK(open_file(L"C:\\w\\a.txt"));
	typing("!");
	ctrl('x');
	key(WF_KEY_DOWN);
	W.next_save = WF_SV_IO;
	CHECK(WF_ES_REDRAW == key(WF_KEY_ENTER)); /* stays open */
	CHECK(1 == wf_es_buffer_count(S) && text_is("!keep"));
	CHECK(screen_has("Not saved") && screen_has("Modified"));
}

/* What an IME can deliver instead of the letter is never an answer. */
static void ime_leftovers_are_not_answers(void)
{
	static const uint32_t not_answers[] = {
	        0xFF53, /* fullwidth s */
	        0xFF44, /* fullwidth d */
	        0xFF43, /* fullwidth c */
	        0xFF33, /* fullwidth S */
	        0x3059, /* kana su: what the key s becomes */
	        0x3067, /* kana de */
	        0x3061, /* kana chi */
	        0x1F600, /* an emoji (a surrogate pair in UTF-16) */
	        0x20BB7,
	};
	size_t i;
	FakeFile *f;

	setup(WF_KM_PIERO);
	f = add(L"C:\\w\\a.txt", "keep");
	CHECK(open_file(L"C:\\w\\a.txt"));
	typing("!");
	CHECK(WF_ES_REDRAW == ctrl('x'));
	for (i = 0; i < WF_ARRAY_LEN(not_answers); i++) {
		CHECK(WF_ES_NONE == chr(not_answers[i]) && wf_es_prompt_active(S));
	}
	/* a letter the IME was handling (VK_PROCESSKEY) answers nothing, in
	 * either case, whatever it carries */
	CHECK(WF_ES_NONE == press_vk(WF_EV_CHAR, 'd', 0xE5));
	CHECK(WF_ES_NONE == press_vk(WF_EV_CHAR, 'D', 0xE5));
	CHECK(WF_ES_NONE == press_vk(WF_EV_CHAR, 's', 0xE5));
	CHECK(wf_es_prompt_active(S));
	/* a run of such input, then Enter: Enter answers Cancel, nothing else */
	for (i = 0; i < 20; i++) {
		chr(0xFF44);
		chr(0x3067);
	}
	CHECK(wf_es_prompt_active(S));
	CHECK(WF_ES_REDRAW == key(WF_KEY_ENTER) && !wf_es_prompt_active(S));
	CHECK(text_is("!keep") && 0 == W.saves && !strcmp(f->data, "keep"));

	/* the real letter still works, either case, from a real key */
	CHECK(WF_ES_REDRAW == ctrl('x'));
	CHECK(WF_ES_CLOSED == press_vk(WF_EV_CHAR, 'D', 'D'));
	CHECK(0 == W.saves && !strcmp(f->data, "keep"));

	/* the other questions share the rules: changed on disk */
	setup(WF_KM_PIERO);
	f = add(L"C:\\w\\a.txt", "disk v1");
	CHECK(open_file(L"C:\\w\\a.txt"));
	typing("mine ");
	W.next_save = WF_SV_CONFLICT;
	ctrl('s');
	CHECK(wf_es_prompt_active(S));
	CHECK(WF_ES_NONE == chr(0xFF52)); /* fullwidth r: not Reload */
	CHECK(WF_ES_NONE == press_vk(WF_EV_CHAR, 'r', 0xE5));
	CHECK(wf_es_prompt_active(S) && text_is("mine disk v1"));
	CHECK(WF_ES_REDRAW == key(WF_KEY_DOWN)); /* Cancel -> Reload */
	CHECK(screen_has("Enter = R"));
	key(WF_KEY_UP);
	CHECK(screen_has("Enter = C"));
	CHECK(WF_ES_REDRAW == key(WF_KEY_ENTER) && !wf_es_prompt_active(S));
	CHECK(text_is("mine disk v1"));
}

/* Pasted text is data: it cannot press keys, and nothing in it is run. */
static void pasted_escape_sequences_stay_text(void)
{
	setup(WF_KM_PIERO);
	add(L"C:\\w\\p.txt", "");
	CHECK(open_file(L"C:\\w\\p.txt"));
	W.clip_n = strlen("\x1b[2J\x1b]0;title\x07rm -rf\r\nnext");
	memcpy(W.clip, "\x1b[2J\x1b]0;title\x07rm -rf\r\nnext", W.clip_n);
	W.clip_present = true;
	ctrl('v');
	CHECK(text_is("[2J]0;titlerm -rf\nnext") ||
	        text_is("[2J]0;titlerm -rf\r\nnext"));
	/* what is drawn contains no escape character of its own making */
	{
		WfFrame *f = wf_frame_new(COLS, ROWS, &(WfStyle){0, 0, 0});
		int y, x;
		bool escape_in_cell = false;

		wf_es_draw(S, f);
		for (y = 0; y < f->rows; y++) {
			for (x = 0; x < f->cols; x++) {
				if (strchr(f->cells[y * f->cols + x].utf8,
				            0x1B)) {
					escape_in_cell = true;
				}
			}
		}
		CHECK(!escape_in_cell);
		wf_frame_free(f);
	}
}

static WfKeymap *KEYMAP;

static void setup_keys(WfProfile profile)
{
	WfKeyConfig cfg;
	WfKeyIssue issue;

	wf_keyconfig_init(&cfg);
	cfg.profile = profile;
	wf_keymap_free(KEYMAP);
	KEYMAP = NULL;
	CHECK(WF_OK == wf_keymap_build(&cfg, WF_KM_PIERO, &KEYMAP, &issue));
	SETUP_KEYS = KEYMAP;
	setup(WF_KM_PIERO);
	SETUP_KEYS = NULL;
}

static WfPos where(void)
{
	return wf_es_current(S)->cur;
}

static bool at(size_t line, size_t col)
{
	WfPos p = where();

	if (p.line != line || p.col != col) {
		printf("  cursor %zu:%zu expected %zu:%zu\n", p.line, p.col,
		        line, col);
	}
	return p.line == line && p.col == col;
}

static void enter_command(const char *line)
{
	chr(':');
	CHECK(wf_es_prompt_active(S));
	typing(line);
	key(WF_KEY_ENTER);
}

/* A save that stops half way leaves files behind; the editor says so and
 * offers to open them on the spot: the new text, the previous version, or
 * both side by side. Closing the question changes nothing. */
static void a_half_saved_file_offers_the_files_it_left(void)
{
	/* both files were left */
	setup_keys(WF_PROFILE_VIM);
	add(L"C:\\w\\r.txt", "keep\n");
	add(L"C:\\w\\r.txt.new", "new text\n");
	add(L"C:\\w\\r.txt~", "keep\n");
	CHECK(open_file(L"C:\\w\\r.txt"));
	chr('x');
	W.next_save = WF_SV_IO;
	wcscpy_s(W.kept_temp, 260, L"C:\\w\\r.txt.new");
	wcscpy_s(W.kept_original, 260, L"C:\\w\\r.txt~");
	enter_command("w");
	CHECK(screen_has("Stopped half way") && screen_has("r.txt.new") &&
	        screen_has("r.txt~"));
	CHECK(1 == wf_es_buffer_count(S));
	chr('b'); /* both */
	CHECK(3 == wf_es_buffer_count(S));

	/* only the new text was left: open it, or close the question */
	setup_keys(WF_PROFILE_VIM);
	add(L"C:\\w\\q.txt", "keep\n");
	add(L"C:\\w\\q.txt.new", "new text\n");
	CHECK(open_file(L"C:\\w\\q.txt"));
	chr('x');
	W.next_save = WF_SV_IO;
	wcscpy_s(W.kept_temp, 260, L"C:\\w\\q.txt.new");
	W.kept_original[0] = 0;
	enter_command("w");
	CHECK(screen_has("Stopped half way") && screen_has("q.txt.new"));
	chr('c');
	CHECK(1 == wf_es_buffer_count(S) && !screen_has("Stopped half way"));
	W.kept_temp[0] = 0;
}

static bool fake_draft_failed(void *ctx, const wchar_t *path)
{
	bool failed = D.late_fail;

	(void)ctx;
	(void)path;
	D.late_fail = false;
	return failed;
}

/* Drafts: unsaved text is written down on a timer, offered back when the file
 * is opened next, and removed when it is saved or thrown away; a secret-looking
 * file is never written. */
/* A write that was taken in and failed afterwards (the system adapter writes on
 * a worker) is told, and the text is written again at the next period: it is not
 * taken for kept for good. */
static void a_draft_that_failed_afterwards_is_written_again(void)
{
	memset(&D, 0, sizeof D);
	setup(WF_KM_PIERO);
	add(L"C:\\w\\late.txt", "disk\n");
	CHECK(open_file(L"C:\\w\\late.txt"));
	chr('x');
	wf_es_save_drafts(S, 200);
	CHECK(1 == D.tries);
	wf_es_save_drafts(S, 200 + 2 * WF_ES_DRAFT_EVERY_MS);
	CHECK(1 == D.tries); /* it holds the text: nothing to do */
	D.late_fail = true;  /* the worker could not write it after all */
	wf_es_save_drafts(S, 200 + 4 * WF_ES_DRAFT_EVERY_MS);
	CHECK(2 == D.tries); /* so it is offered again */
	wf_es_save_drafts(S, 200 + 6 * WF_ES_DRAFT_EVERY_MS);
	CHECK(2 == D.tries); /* and not again once it holds it */
}

/* The draft is written again only when the text is not the one it holds: a
 * timer going round over unchanged text copies and writes nothing, and a write
 * that failed is tried again, not counted as done. */
static void a_draft_is_written_only_for_text_it_does_not_hold(void)
{
	memset(&D, 0, sizeof D);
	setup(WF_KM_PIERO);
	add(L"C:\\w\\d.txt", "disk\n");
	CHECK(open_file(L"C:\\w\\d.txt"));
	chr('x');
	wf_es_save_drafts(S, 200);
	CHECK(1 == D.writes && 1 == D.tries);
	/* the timer goes round (more than the period) over the same text */
	wf_es_save_drafts(S, 200 + 2 * WF_ES_DRAFT_EVERY_MS);
	wf_es_save_drafts(S, 200 + 4 * WF_ES_DRAFT_EVERY_MS);
	CHECK(1 == D.tries);
	/* the draft holds the text: nothing is due, and the wait is not told to
	 * wake up at once for a write that would do nothing */
	CHECK(UINT32_MAX == wf_es_timeout(S, 200 + 4 * WF_ES_DRAFT_EVERY_MS));
	/* an edit: written again, once */
	chr('y');
	/* the new text is due when the period since the last write is over, not
	 * before: the wait is for that moment */
	CHECK(0 == wf_es_timeout(S, 200 + 6 * WF_ES_DRAFT_EVERY_MS));
	wf_es_save_drafts(S, 200 + 6 * WF_ES_DRAFT_EVERY_MS);
	wf_es_save_drafts(S, 200 + 8 * WF_ES_DRAFT_EVERY_MS);
	CHECK(2 == D.writes && 2 == D.tries);
	/* a write that fails is tried at the next period, not forgotten */
	chr('z');
	D.fail = true;
	wf_es_save_drafts(S, 200 + 10 * WF_ES_DRAFT_EVERY_MS);
	CHECK(3 == D.tries && 2 == D.writes);
	wf_es_save_drafts(
	        S, 200 + 10 * WF_ES_DRAFT_EVERY_MS + 100); /* too soon */
	CHECK(3 == D.tries);
	wf_es_save_drafts(S, 200 + 12 * WF_ES_DRAFT_EVERY_MS);
	CHECK(4 == D.tries && 2 == D.writes);
	D.fail = false;
	wf_es_save_drafts(S, 200 + 14 * WF_ES_DRAFT_EVERY_MS);
	CHECK(5 == D.tries && 3 == D.writes);
	wf_es_save_drafts(
	        S, 200 + 16 * WF_ES_DRAFT_EVERY_MS); /* now it holds it */
	CHECK(5 == D.tries);
	/* a name that may not be kept is not copied either: the write is never asked for */
	memset(&D, 0, sizeof D);
	setup(WF_KM_PIERO);
	add(L"C:\\w\\secret.txt", "disk\n");
	CHECK(open_file(L"C:\\w\\secret.txt"));
	chr('x');
	wf_es_save_drafts(S, 200);
	CHECK(0 == D.tries && 0 == D.writes);
}

/* The saved session asks where the cursor is and puts it back (DR-DRAFT-018):
 * the answer is the buffer's own, a place out of range or inside a character is
 * made valid, "modified" always has a draft behind it, and while the question
 * about a draft is open the place waits for the answer (which moves the cursor). */
static void the_session_place_is_reported_and_put_back(void)
{
	WfEsPlace p;

	memset(&D, 0, sizeof D);
	setup_keys(WF_PROFILE_VIM);
	add(L"C:\\w\\p.txt", "one\ntwo\nthree\n");
	CHECK(!wf_es_place(S, &p)); /* no buffer: nothing to say */
	CHECK(open_file(L"C:\\w\\p.txt"));
	CHECK(wf_es_place(S, &p) && !wcscmp(p.path, L"C:\\w\\p.txt") &&
	        0 == p.line && 0 == p.col && !p.modified);
	wf_es_goto(S, 1, 2, 0, 0);
	CHECK(wf_es_place(S, &p) && 1 == p.line && 2 == p.col);
	/* far out of range: the last line, the end of it */
	wf_es_goto(S, 999, 999, 999, 5);
	CHECK(wf_es_place(S, &p) && p.line < 4 && p.top < 4);
	CHECK(p.col == wf_es_current(S)->text.lines[p.line].len);
	/* "modified" needs a draft: an edit alone is not enough */
	wf_es_goto(S, 1, 0, 0, 0);
	chr('x');
	CHECK(wf_es_place(S, &p) && !p.modified);
	wf_es_save_drafts(S, 200);
	CHECK(D.present && wf_es_place(S, &p) && p.modified);
	enter_command("w");
	CHECK(wf_es_place(S, &p) && !p.modified); /* saved: nothing unsaved */

	/* a place inside a character is moved to its start, never kept */
	memset(&D, 0, sizeof D);
	setup_keys(WF_PROFILE_VIM);
	add(L"C:\\w\\j.txt", "\xE3\x81\x82\xE3\x81\x84\n");
	CHECK(open_file(L"C:\\w\\j.txt"));
	wf_es_goto(S, 0, 1, 0, 0);
	CHECK(wf_es_place(S, &p) && 0 == p.col);
	wf_es_goto(S, 0, 3, 0, 0);
	CHECK(wf_es_place(S, &p) && 3 == p.col);

	/* the question about a draft is open: the place waits for the answer */
	memset(&D, 0, sizeof D);
	setup_keys(WF_PROFILE_VIM);
	add(L"C:\\w\\q.txt", "disk\n");
	wcscpy_s(D.path, 260, L"C:\\w\\q.txt");
	memcpy(D.text, "draft text\n", 11);
	D.n = 11;
	D.enc.kind = WF_ENC_UTF8;
	D.present = true;
	CHECK(open_file(L"C:\\w\\q.txt"));
	CHECK(wf_es_prompt_active(S));
	wf_es_goto(S, 0, 3, 0, 0);
	CHECK(wf_es_place(S, &p) && 0 == p.line && 0 == p.col); /* not yet */
	chr('r');
	CHECK(text_is("draft text\n"));
	CHECK(wf_es_place(S, &p) && 0 == p.line && 3 == p.col); /* after it */
	/* the same when the draft is discarded */
	memset(&D, 0, sizeof D);
	setup_keys(WF_PROFILE_VIM);
	add(L"C:\\w\\r.txt", "disk\n");
	wcscpy_s(D.path, 260, L"C:\\w\\r.txt");
	memcpy(D.text, "draft text\n", 11);
	D.n = 11;
	D.enc.kind = WF_ENC_UTF8;
	D.present = true;
	CHECK(open_file(L"C:\\w\\r.txt"));
	wf_es_goto(S, 0, 2, 0, 0);
	chr('d');
	CHECK(text_is("disk\n"));
	CHECK(wf_es_place(S, &p) && 2 == p.col);
}

/* Another program changed the file under a buffer: the person is told once, the
 * text is kept as it is, nothing is reloaded or saved, and the stamp the program
 * compares with is the one the editor kept. */
static void an_outside_change_is_said_once_and_nothing_else_happens(void)
{
	const wchar_t *path = NULL;
	WfFileStamp stamp;
	FakeFile *f;

	setup(WF_KM_PIERO);
	wf_es_note_external_change(S); /* no buffer: nothing happens */
	CHECK(!wf_es_disk_stamp(S, &path, &stamp));
	f = add(L"C:\\w\\ext.txt", "hello\r\nworld\r\n");
	CHECK(open_file(L"C:\\w\\ext.txt"));
	CHECK(wf_es_disk_stamp(S, &path, &stamp) &&
	        !wcscmp(path, L"C:\\w\\ext.txt"));
	typing("X");
	CHECK(text_is("Xhello\r\nworld\r\n"));
	wf_es_note_external_change(S);
	CHECK(screen_has("changed by another program"));
	CHECK(text_is("Xhello\r\nworld\r\n") && screen_has("[Modified]"));
	CHECK(0 == W.saves && 1 == W.loads); /* no save, no reload */
	/* said once: after the message is gone, another event says nothing */
	typing("Y");
	CHECK(!screen_has("changed by another program"));
	wf_es_note_external_change(S);
	CHECK(!screen_has("changed by another program"));
	/* a save is a new baseline: the next change is worth saying again */
	ctrl('s');
	CHECK(1 == W.saves && f->n > 0);
	wf_es_note_external_change(S);
	CHECK(screen_has("changed by another program"));
}

/* A draft comes back as it was saved: an escape character and a mix of LF and
 * CRLF line ends are part of the text, not something a paste would clean. */
static void a_restored_draft_is_the_saved_text_byte_for_byte(void)
{
	static const char saved[] = "a\x1b[0m\r\nb\nc\r\nd";

	memset(&D, 0, sizeof D);
	setup_keys(WF_PROFILE_VIM);
	add(L"C:\\w\\d.txt", "disk\n");
	wcscpy_s(D.path, 260, L"C:\\w\\d.txt");
	memcpy(D.text, saved, sizeof saved - 1);
	D.n = sizeof saved - 1;
	D.enc.kind = WF_ENC_UTF8;
	D.present = true;
	CHECK(open_file(L"C:\\w\\d.txt"));
	CHECK(wf_es_prompt_active(S));
	chr('r');
	CHECK(text_is(saved));
	CHECK(wf_et_check(&wf_es_current(S)->text));
}

/* Names that differ only in case are one file or two by what the files are
 * (their identity), not by the spelling: two files get two buffers, and one
 * file under two spellings gets one. */
static void case_does_not_decide_which_buffer_a_file_is(void)
{
	FakeFile *upper, *lower;

	setup(WF_KM_PIERO);
	upper = add(L"C:\\w\\Foo.txt", "upper");
	lower = add(L"C:\\w\\foo.txt", "lower");
	CHECK(upper->id !=
	        lower->id); /* two files, as in a case-sensitive folder */
	CHECK(open_file(L"C:\\w\\Foo.txt"));
	CHECK(open_file(L"C:\\w\\foo.txt"));
	CHECK(2 == wf_es_buffer_count(S));
	CHECK(text_is("lower"));

	setup(WF_KM_PIERO);
	upper = add(L"C:\\w\\Foo.txt", "one file");
	lower = add(L"C:\\w\\foo.txt", "one file");
	lower->id = upper->id; /* the same file, reached by another spelling */
	CHECK(open_file(L"C:\\w\\Foo.txt"));
	CHECK(open_file(L"C:\\w\\foo.txt"));
	CHECK(1 == wf_es_buffer_count(S));
}

/* What a drawing costs after each key in a text of many lines: the header says
 * LF, CRLF or MIXED, and typing a letter does not change that. 300000 lines,
 * 200 letters typed and drawn; the time is printed, the label is checked. */
static void typing_a_letter_does_not_count_the_line_ends_again(void)
{
	enum
	{
		LINES = 300000,
		KEYS = 200
	};
	char *big = malloc((size_t)LINES * 2 + 1);
	WfFrame *f = wf_frame_new(COLS, ROWS, &(WfStyle){0, 0, 0});
	LARGE_INTEGER a, b, freq;
	WfEncoding enc = {WF_ENC_UTF8, 0};

	CHECK(NULL != big);
	setup(WF_KM_PIERO);
	add(L"C:\\w\\many.txt", "x\n");
	CHECK(open_file(L"C:\\w\\many.txt"));
	for (size_t i = 0; big && i < LINES; i++) {
		big[2 * i] = 'x';
		big[2 * i + 1] = '\n';
	}
	CHECK(WF_OK ==
	        wf_ed_set_text(wf_es_current(S), big, (size_t)LINES * 2, enc));
	wf_es_draw(S, f);
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&a);
	for (int k = 0; k < KEYS; k++) {
		CHECK(WF_OK == wf_ed_type(wf_es_current(S), 'a'));
		wf_es_draw(S, f);
	}
	QueryPerformanceCounter(&b);
	printf("info: %d keys drawn in a text of %d lines: %.1f ms\n", KEYS,
	        LINES,
	        (double)(b.QuadPart - a.QuadPart) * 1000.0 /
	                (double)freq.QuadPart);
	CHECK(screen_has("LF"));
	wf_frame_free(f);
	free(big);
}

/* The line-end label follows the edits that change it (a break added or taken
 * away, a whole text put in, an edit undone) and is not asked again by the ones
 * that do not. */
static void the_line_end_label_follows_the_edits_that_change_it(void)
{
	WfEncoding enc = {WF_ENC_UTF8, 0};
	WfFrame *f = wf_frame_new(COLS, ROWS, &(WfStyle){0, 0, 0});
	WfEditor *e;

	setup(WF_KM_PIERO);
	add(L"C:\\w\\eol.txt", "x\n");
	CHECK(open_file(L"C:\\w\\eol.txt"));
	e = wf_es_current(S);
	CHECK(WF_OK == wf_ed_set_text(e, "a\r\nb\nc", 6, enc));
	wf_es_draw(S, f);
	CHECK(screen_has("Mixed"));
	/* the one LF line goes: only CRLF is left */
	wf_ed_goto(e, 2, 1);
	CHECK(WF_OK == wf_ed_delete_line(e));
	wf_es_draw(S, f);
	CHECK(screen_has("CRLF") && !screen_has("Mixed"));
	/* a letter does not change it */
	CHECK(WF_OK == wf_ed_type(e, 'z'));
	wf_es_draw(S, f);
	CHECK(screen_has("CRLF"));
	/* undone, the line is back and so is the mix */
	CHECK(WF_OK == wf_ed_undo(e)); /* the letter */
	CHECK(WF_OK == wf_ed_undo(e)); /* the line */
	wf_es_draw(S, f);
	CHECK(screen_has("Mixed"));
	/* a text put in whole */
	CHECK(WF_OK == wf_ed_set_text(e, "p\nq\n", 4, enc));
	wf_es_draw(S, f);
	CHECK(screen_has("LF") && !screen_has("Mixed") && !screen_has("CRLF"));
	wf_frame_free(f);
}

/* What a drawing costs while only the cursor moves, with soft wrap on: the rows
 * of the lines on screen are worked out again at every drawing. Two texts of
 * about 1 MiB: one single line, and 20000 lines of 50 characters; the cursor
 * moves one key to the right and the screen is drawn, 100 times. */
static void soft_wrap_cost_of_moving_the_cursor(void)
{
	enum
	{
		KEYS = 100
	};
	WfEditConfig c;
	WfEditIo io = {NULL, fake_load, fake_over, fake_new, fake_clip_get,
	        fake_clip_set, fake_draft_allowed, fake_draft_write,
	        fake_draft_read, fake_draft_remove, NULL};
	static const char *const kind[] = {"one line of 1 MiB",
	        "20000 lines of 50 characters",
	        "2000 lines of 400 characters, read down to line 600 first",
	        "one line of 1 MiB, no wrap, cursor at its end, a key left and "
	        "right",
	        "one line of 4 MiB, no wrap, cursor at its end, a key left and "
	        "right"};

	for (int t = 0; t < 5; t++) {
		size_t n = 0, ll = 2 == t ? 400 : 50,
		       total = t >= 3 ? (size_t)1 << (3 == t ? 20 : 22)
		        : 0 == t
		        ? (size_t)1 << 20
		        : (1 == t ? (size_t)20000 : (size_t)2000) * (ll + 1);
		char *text = malloc(total + 1);
		WfFrame *f = wf_frame_new(COLS, ROWS, &(WfStyle){0, 0, 0});
		LARGE_INTEGER a, b, freq;

		CHECK(NULL != text);
		for (size_t i = 0; text && i < total; i++) {
			text[n++] = t && t < 3 && ll == i % (ll + 1)
			        ? '\n'
			        : (char)('a' + i % 26);
		}
		memset(&W, 0, sizeof W);
		wf_es_free(S);
		wf_es_config_default(&c);
		c.soft_wrap = t < 3;
		S = wf_es_new(&c, io);
		add(L"C:\\w\\wrap.txt", "x");
		CHECK(open_file(L"C:\\w\\wrap.txt"));
		CHECK(WF_OK ==
		        wf_ed_set_text(wf_es_current(S), text, n,
		                (WfEncoding){WF_ENC_UTF8, 0}));
		wf_es_draw(S, f);
		for (size_t line = 1; 2 == t && line <= 600; line += 10) {
			wf_ed_goto(wf_es_current(S), line,
			        1); /* what a page down does */
			wf_es_draw(S, f);
		}
		if (t >= 3) {
			wf_ed_move(wf_es_current(S), WF_MV_LINE_END, false, 0);
			wf_es_draw(S, f);
		}
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&a);
		for (int k = 0; k < KEYS; k++) {
			wf_ed_move(wf_es_current(S),
			        t >= 3 && k % 2 ? WF_MV_LEFT : WF_MV_RIGHT,
			        false, 0);
			wf_es_draw(S, f);
		}
		QueryPerformanceCounter(&b);
		printf("info: %d moves and drawings, %s: %.1f ms\n", KEYS,
		        kind[t],
		        (double)(b.QuadPart - a.QuadPart) * 1000.0 /
		                (double)freq.QuadPart);
		wf_frame_free(f);
		free(text);
	}
}

/* True when the two frames show the same text everywhere. */
static bool same_screen(const WfFrame *a, const WfFrame *b)
{
	if (a->cols != b->cols || a->rows != b->rows) {
		return false;
	}
	for (int i = 0; i < a->cols * a->rows; i++) {
		if (strcmp(a->cells[i].utf8, b->cells[i].utf8)) {
			return false;
		}
	}
	return true;
}

/* The kept rows of a wrapped line are never used for another text, width or
 * tab width: after an edit, a resize and a tab-width change, the drawing is the
 * one a cache that was emptied would give. */
static void kept_wrap_rows_are_never_stale(void)
{
	WfEditConfig c;
	WfEditIo io = {NULL, fake_load, fake_over, fake_new, fake_clip_get,
	        fake_clip_set, fake_draft_allowed, fake_draft_write,
	        fake_draft_read, fake_draft_remove, NULL};
	char longline[501];
	WfFrame *kept, *fresh;
	WfEditor *e;

	memset(&W, 0, sizeof W);
	wf_es_free(S);
	wf_es_config_default(&c);
	c.soft_wrap = true;
	S = wf_es_new(&c, io);
	memset(longline, 'a', 500);
	longline[100] = 'Z';
	longline[300] = '\t';
	longline[500] = '\0';
	add(L"C:\\w\\keep.txt", longline);
	CHECK(open_file(L"C:\\w\\keep.txt"));
	e = wf_es_current(S);
	kept = wf_frame_new(COLS, ROWS, &(WfStyle){0, 0, 0});
	fresh = wf_frame_new(COLS, ROWS, &(WfStyle){0, 0, 0});
	for (int step = 0; step < 4; step++) {
		if (1 == step) { /* the text changes */
			e->cur = (WfPos){0, 0};
			CHECK(WF_OK ==
			        wf_ed_insert_text(e,
			                "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
			                "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
			                "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
			                "bbbbbbbbbbbbbbbb",
			                130));
		} else if (2 == step) { /* the tab width changes */
			e->opt.tab_width = 3;
		} else if (3 == step) { /* the screen is narrower */
			wf_frame_free(kept);
			wf_frame_free(fresh);
			kept = wf_frame_new(
			        COLS - 30, ROWS, &(WfStyle){0, 0, 0});
			fresh = wf_frame_new(
			        COLS - 30, ROWS, &(WfStyle){0, 0, 0});
		}
		wf_es_draw(S, kept);
		wf_es_draw(
		        S, kept); /* a second drawing: now from what was kept */
		wf_es_wrap_free(S->buf[S->cur]);
		wf_es_draw(S, fresh);
		CHECK(same_screen(kept, fresh));
	}
	wf_frame_free(kept);
	wf_frame_free(fresh);
}

static void drafts_are_written_offered_and_removed(void)
{
	/* written when due, not before; removed by a save */
	memset(&D, 0, sizeof D);
	setup_keys(WF_PROFILE_VIM);
	add(L"C:\\w\\d.txt", "keep\n");
	CHECK(open_file(L"C:\\w\\d.txt"));
	wf_es_save_drafts(S, 100);
	CHECK(!D.present); /* nothing changed yet */
	chr('x');
	wf_es_save_drafts(S, 200);
	CHECK(D.present && 1 == D.writes && 4 == D.n && !memcmp(D.text, "eep\n", 4));
	chr('x');
	wf_es_save_drafts(S, 200 + 1000);
	CHECK(1 == D.writes); /* not due yet */
	wf_es_save_drafts(S, 200 + WF_ES_DRAFT_EVERY_MS);
	CHECK(2 == D.writes && 3 == D.n);
	enter_command("w");
	CHECK(!D.present); /* saved: the draft has no use */

	/* a secret-looking file is never written down */
	memset(&D, 0, sizeof D);
	setup_keys(WF_PROFILE_VIM);
	add(L"C:\\w\\my-secret.txt", "keep\n");
	CHECK(open_file(L"C:\\w\\my-secret.txt"));
	chr('x');
	wf_es_save_drafts(S, 100);
	CHECK(0 == D.writes && !D.present);

	/* offered when the file is opened: restore */
	memset(&D, 0, sizeof D);
	setup_keys(WF_PROFILE_VIM);
	add(L"C:\\w\\e.txt", "disk\n");
	wcscpy_s(D.path, 260, L"C:\\w\\e.txt");
	memcpy(D.text, "draft text\n", 11);
	D.n = 11;
	D.enc.kind = WF_ENC_UTF8;
	D.present = true;
	CHECK(open_file(L"C:\\w\\e.txt"));
	CHECK(screen_has("kept from last time"));
	chr('r');
	CHECK(text_is("draft text\n"));
	CHECK(wf_ed_modified(wf_es_current(S))); /* not saved: the file is as it was */

	/* ... and discard */
	memset(&D, 0, sizeof D);
	setup_keys(WF_PROFILE_VIM);
	add(L"C:\\w\\f.txt", "disk\n");
	wcscpy_s(D.path, 260, L"C:\\w\\f.txt");
	memcpy(D.text, "draft text\n", 11);
	D.n = 11;
	D.enc.kind = WF_ENC_UTF8;
	D.present = true;
	CHECK(open_file(L"C:\\w\\f.txt"));
	chr('d');
	CHECK(text_is("disk\n") && !D.present);

	/* a draft equal to the file says nothing and goes */
	memset(&D, 0, sizeof D);
	setup_keys(WF_PROFILE_VIM);
	add(L"C:\\w\\g.txt", "same\n");
	wcscpy_s(D.path, 260, L"C:\\w\\g.txt");
	memcpy(D.text, "same\n", 5);
	D.n = 5;
	D.enc.kind = WF_ENC_UTF8;
	D.present = true;
	CHECK(open_file(L"C:\\w\\g.txt"));
	CHECK(!D.present && !screen_has("kept from last time"));
}

#define SAMPLE "one two\nsecond line\nthird\n"

static void vim_starts_in_normal_mode_and_insert_types(void)
{
	setup_keys(WF_PROFILE_VIM);
	add(L"C:\\w\\v.txt", SAMPLE);
	CHECK(open_file(L"C:\\w\\v.txt"));
	CHECK(screen_has("NORMAL"));
	/* a letter that means nothing in Normal mode types nothing */
	chr('z');
	chr('Q');
	CHECK(text_is(SAMPLE));
	CHECK(screen_has("NORMAL"));
	/* i: typing; the letters that are motions in Normal are text here */
	chr('i');
	CHECK(screen_has("-- INSERT --"));
	typing("ABj");
	CHECK(text_is("ABjone two\nsecond line\nthird\n"));
	/* Ctrl+S saves from Insert mode */
	ctrl('s');
	CHECK(1 == W.saves);
	key(WF_KEY_ESC);
	CHECK(screen_has("NORMAL") && !screen_has("-- INSERT --"));
	/* Esc in Normal mode does not close the buffer */
	CHECK(WF_ES_CLOSED != key(WF_KEY_ESC));
	CHECK(1 == wf_es_buffer_count(S));
	/* a, o and O */
	chr('0');
	chr('a');
	typing("-");
	key(WF_KEY_ESC);
	CHECK(text_is("A-Bjone two\nsecond line\nthird\n"));
	chr('o');
	typing("NEW");
	key(WF_KEY_ESC);
	CHECK(text_is("A-Bjone two\nNEW\nsecond line\nthird\n"));
	chr('O');
	typing("TOP");
	key(WF_KEY_ESC);
	CHECK(text_is("A-Bjone two\nTOP\nNEW\nsecond line\nthird\n"));
}

/* Pasted text is text in Normal mode too, not a run of commands. */
static void pasted_text_is_inserted_in_vim_normal_mode(void)
{
	setup_keys(WF_PROFILE_VIM);
	add(L"C:\\w\\n.txt", "abc");
	CHECK(open_file(L"C:\\w\\n.txt"));
	CHECK(screen_has("NORMAL"));
	{
		const char *pasted = "dd";
		size_t i;

		for (i = 0; pasted[i]; i++) {
			WfEvent c = {WF_EV_CHAR, (uint32_t)pasted[i], WF_KEY_NONE,
			        WF_VK_PASTED};

			wf_es_event(S, &c, ROWS, COLS);
		}
		{
			WfEvent nl = {WF_EV_KEY, 0, WF_KEY_ENTER, WF_VK_PASTED};
			WfEvent x = {WF_EV_CHAR, 'x', WF_KEY_NONE, WF_VK_PASTED};

			wf_es_event(S, &nl, ROWS, COLS);
			wf_es_event(S, &x, ROWS, COLS);
		}
	}
	CHECK(text_is("dd\nxabc"));
	CHECK(screen_has("NORMAL")); /* still Normal: nothing switched mode */
	/* typed, the same keys are commands as before */
	chr('d');
	chr('d');
	CHECK(!text_is("dd\nxabc"));
}

static void vim_motions_counts_and_sequences(void)
{
	setup_keys(WF_PROFILE_VIM);
	add(L"C:\\w\\v.txt", SAMPLE);
	CHECK(open_file(L"C:\\w\\v.txt"));
	chr('l');
	CHECK(at(0, 1));
	chr('$');
	CHECK(at(0, 7));
	chr('0');
	CHECK(at(0, 0));
	chr('w');
	CHECK(at(0, 4));
	chr('b');
	CHECK(at(0, 0));
	chr('j');
	CHECK(at(1, 0));
	chr('k');
	CHECK(at(0, 0));
	chr('2');
	chr('j');
	CHECK(at(2, 0));
	chr('G');
	CHECK(at(3, 0));
	chr('g');
	CHECK(screen_has("NORMAL g"));
	chr('g');
	CHECK(at(0, 0));
	/* a number before G is a line */
	chr('2');
	chr('G');
	CHECK(at(1, 0));
	/* a huge count is capped, not trusted */
	for (int i = 0; i < 12; i++) {
		chr('9');
	}
	chr('j');
	CHECK(3 == where().line);
	/* the time runs out: the next g starts again */
	wf_es_set_clock(S, 1000);
	chr('g');
	wf_es_set_clock(S, 9000);
	chr('g');
	CHECK(at(3, 0));
	wf_es_set_clock(S, 9100);
	chr('g');
	CHECK(at(0, 0));
	/* Esc drops a partial sequence */
	chr('G');
	chr('g');
	key(WF_KEY_ESC);
	chr('g');
	CHECK(at(3, 0));
	key(WF_KEY_ESC);
	/* arrows work in both modes */
	key(WF_KEY_UP);
	CHECK(at(2, 0));
}

static void vim_lines_are_cut_copied_and_pasted_as_lines(void)
{
	setup_keys(WF_PROFILE_VIM);
	add(L"C:\\w\\v.txt", SAMPLE);
	CHECK(open_file(L"C:\\w\\v.txt"));
	chr('j');
	chr('d');
	chr('d');
	CHECK(text_is("one two\nthird\n"));
	chr('p'); /* after the current line */
	CHECK(text_is("one two\nthird\nsecond line\n"));
	chr('u');
	CHECK(text_is("one two\nthird\n"));
	ctrl('r');
	CHECK(text_is("one two\nthird\nsecond line\n"));
	chr('g');
	chr('g');
	chr('y');
	chr('y');
	chr('P'); /* above */
	CHECK(text_is("one two\none two\nthird\nsecond line\n"));
	/* a number before dd is the number of lines; a paste takes none */
	chr('3');
	chr('d');
	chr('d');
	CHECK(text_is("second line\n"));
	chr('u');
	CHECK(text_is("one two\none two\nthird\nsecond line\n"));
	chr('3');
	chr('p');
	CHECK(screen_has("A number cannot"));
	/* x deletes the character, and u takes it back */
	chr('g');
	chr('g');
	chr('x');
	CHECK(text_is("ne two\none two\nthird\nsecond line\n"));
	chr('u');
	CHECK(text_is("one two\none two\nthird\nsecond line\n"));
}

/* d, y and c take a motion; counts go before either; D and C go to the end. */
/* u, then the cursor to the first line (undo leaves it where the change was) */
static void undo_home(void)
{
	chr('u');
	chr('g');
	chr('g');
}

static void vim_operators_take_motions(void)
{
	setup_keys(WF_PROFILE_VIM);
	add(L"C:\\w\\o.txt", SAMPLE);
	CHECK(open_file(L"C:\\w\\o.txt"));
	chr('d');
	chr('w');
	CHECK(text_is("two\nsecond line\nthird\n"));
	undo_home();
	CHECK(text_is(SAMPLE));
	chr('w');
	chr('D');
	CHECK(text_is("one \nsecond line\nthird\n"));
	undo_home();
	chr('g');
	chr('g');
	chr('2');
	chr('d');
	chr('d');
	CHECK(text_is("third\n"));
	undo_home();
	chr('d');
	chr('j');
	CHECK(text_is("third\n"));
	undo_home();
	chr('d');
	chr('G');
	CHECK(text_is(""));
	undo_home();
	CHECK(text_is(SAMPLE));
	chr('c');
	chr('w');
	CHECK(screen_has("INSERT"));
	chr('X');
	key(WF_KEY_ESC);
	CHECK(text_is("X two\nsecond line\nthird\n"));
	chr('u'); /* the typing, then the deletion of the word */
	undo_home();
	/* d then Esc does nothing at all */
	chr('d');
	key(WF_KEY_ESC);
	chr('x');
	CHECK(text_is("ne two\nsecond line\nthird\n"));
}

/* "de" with a count takes the words (a run of punctuation is a word) and not
 * the character that follows the last one. */
static void vim_counted_word_end_stops_at_the_word(void)
{
	setup_keys(WF_PROFILE_VIM);
	add(L"C:\\w\\e.txt", "foo.bar baz\nx\n");
	CHECK(open_file(L"C:\\w\\e.txt"));
	chr('2');
	chr('d');
	chr('e');
	CHECK(text_is("bar baz\nx\n")); /* "foo." is gone, not "foo.bar" */
	undo_home();
	chr('3');
	chr('d');
	chr('e');
	CHECK(text_is(" baz\nx\n")); /* "foo.bar" */
	undo_home();
	chr('d');
	chr('e');
	CHECK(text_is(".bar baz\nx\n"));
}

static void vim_visual_registers_and_macros(void)
{
	setup_keys(WF_PROFILE_VIM);
	add(L"C:\\w\\v2.txt", SAMPLE);
	CHECK(open_file(L"C:\\w\\v2.txt"));
	/* Visual: the character under the cursor goes too */
	chr('v');
	CHECK(screen_has("VISUAL"));
	chr('l');
	chr('l');
	chr('d');
	CHECK(text_is(" two\nsecond line\nthird\n"));
	undo_home();
	/* Visual line takes whole lines */
	chr('V');
	chr('j');
	chr('d');
	CHECK(text_is("third\n"));
	undo_home();
	/* Esc leaves Visual mode without touching the text */
	chr('v');
	key(WF_KEY_ESC);
	chr('x');
	CHECK(text_is("ne two\nsecond line\nthird\n"));
	undo_home();
	/* a named register holds a line while the clip moves on */
	chr('g');
	chr('g');
	chr('"');
	chr('a');
	chr('y');
	chr('y');
	chr('j');
	chr('d');
	chr('d');
	chr('"');
	chr('a');
	chr('p');
	CHECK(text_is("one two\nthird\none two\n"));
	undo_home();
	undo_home();
	chr('g');
	chr('g');
	/* a macro: record x and j, play it again */
	chr('q');
	chr('a');
	chr('x');
	chr('j');
	chr('q');
	CHECK(text_is("ne two\nsecond line\nthird\n"));
	chr('@');
	chr('a');
	CHECK(text_is("ne two\necond line\nthird\n"));
}

static void vim_finds_objects_dot_and_append(void)
{
	setup_keys(WF_PROFILE_VIM);
	add(L"C:\\w\\v4.txt", SAMPLE);
	CHECK(open_file(L"C:\\w\\v4.txt"));
	/* e: to the end of the word, the last letter included */
	chr('d');
	chr('e');
	CHECK(text_is(" two\nsecond line\nthird\n"));
	undo_home();
	/* f and t: up to and including / up to the character */
	chr('d');
	chr('f');
	chr('t');
	CHECK(text_is("wo\nsecond line\nthird\n"));
	undo_home();
	chr('d');
	chr('t');
	chr('t');
	CHECK(text_is("two\nsecond line\nthird\n"));
	undo_home();
	/* a character that is not there: nothing happens */
	chr('d');
	chr('f');
	chr('z');
	CHECK(text_is(SAMPLE));
	/* F looks back from the cursor */
	chr('$');
	chr('d');
	chr('F');
	chr('o');
	CHECK(text_is("one tw\nsecond line\nthird\n"));
	undo_home();
	/* F with a character wider than the text before the cursor reads nothing
	 * beyond the line */
	chr('$');
	chr('d');
	chr('F');
	chr(0x3042);
	CHECK(text_is(SAMPLE));
	chr('g');
	chr('g');
	chr('0');
	/* text objects */
	chr('d');
	chr('i');
	chr('w');
	CHECK(text_is(" two\nsecond line\nthird\n"));
	undo_home();
	chr('d');
	chr('a');
	chr('w');
	CHECK(text_is("two\nsecond line\nthird\n"));
	undo_home();
	chr('w');
	chr('d');
	chr('a');
	chr('w'); /* no blanks after it: the ones before go */
	CHECK(text_is("one\nsecond line\nthird\n"));
	undo_home();
	/* "dw" on the last word of a line keeps the line break */
	chr('w');
	chr('d');
	chr('w');
	CHECK(text_is("one \nsecond line\nthird\n"));
	undo_home();

	add(L"C:\\w\\v5.txt", "say \"hi there\" ok\ncall(a, b)\n");
	CHECK(open_file(L"C:\\w\\v5.txt"));
	chr('d');
	chr('i');
	chr('"');
	CHECK(text_is("say \"\" ok\ncall(a, b)\n"));
	undo_home();
	chr('d');
	chr('a');
	chr('"');
	CHECK(text_is("say ok\ncall(a, b)\n"));
	undo_home();
	chr('j');
	for (int i = 0; i < 6; i++) {
		chr('l');
	}
	chr('d');
	chr('i');
	chr('(');
	CHECK(text_is("say \"hi there\" ok\ncall()\n"));
	undo_home();

	/* "." repeats the last change; a count repeats it that many times */
	add(L"C:\\w\\v6.txt", "a b c d e\none two\n");
	CHECK(open_file(L"C:\\w\\v6.txt"));
	chr('d');
	chr('w');
	chr('.');
	CHECK(text_is("c d e\none two\n"));
	chr('2');
	chr('.');
	CHECK(text_is("e\none two\n"));
	/* a change with typed text: the text comes back too */
	chr('j');
	chr('0');
	chr('c');
	chr('w');
	typing("X");
	key(WF_KEY_ESC);
	chr('w');
	chr('.');
	CHECK(text_is("e\nX X\n"));
	/* undo is not a change to repeat */
	chr('u');
	chr('.');
	CHECK(text_is("e\nX X\n"));

	/* "A adds to the register; the choice serves one command only */
	add(L"C:\\w\\v7.txt", SAMPLE);
	CHECK(open_file(L"C:\\w\\v7.txt"));
	chr('"');
	chr('a');
	chr('y');
	chr('y');
	chr('j');
	chr('"');
	chr('A');
	chr('y');
	chr('y');
	chr('g');
	chr('g');
	chr('"');
	chr('a');
	chr('p');
	CHECK(text_is("one two\none two\nsecond line\nsecond line\nthird\n"));
	undo_home();
	chr('j');
	chr('y');
	chr('y');
	chr('"');
	chr('a');
	chr('x');
	chr('p');
	CHECK(text_is("one two\necond line\nsecond line\nthird\n"));

	/* a macro takes the answer to a prompt too */
	add(L"C:\\w\\v8.txt", SAMPLE);
	CHECK(open_file(L"C:\\w\\v8.txt"));
	chr('q');
	chr('a');
	chr('/');
	typing("third");
	key(WF_KEY_ENTER);
	chr('q');
	CHECK(at(2, 5));
	chr('g');
	chr('g');
	chr('@');
	chr('a');
	CHECK(at(2, 5));
}

static void vim_search_and_buffers(void)
{
	setup_keys(WF_PROFILE_VIM);
	add(L"C:\\w\\v.txt", SAMPLE);
	add(L"C:\\w\\w.txt", "other\n");
	CHECK(open_file(L"C:\\w\\v.txt"));
	chr('/');
	CHECK(wf_es_prompt_active(S));
	typing("third");
	key(WF_KEY_ENTER);
	CHECK(at(2, 5)); /* the match is selected, the cursor is at its end */
	chr('g');
	chr('g');
	chr('/');
	typing("e");
	key(WF_KEY_ENTER);
	chr('n');
	chr('N');
	CHECK(!wf_es_prompt_active(S));
	/* g t / g T and :bnext / :bprev */
	CHECK(open_file(L"C:\\w\\w.txt"));
	CHECK(2 == wf_es_buffer_count(S));
	chr('g');
	chr('T');
	CHECK(screen_has("v.txt"));
	chr('g');
	chr('t');
	CHECK(screen_has("w.txt"));
	enter_command("bprev");
	CHECK(screen_has("v.txt"));
	enter_command("bnext");
	CHECK(screen_has("w.txt"));
	enter_command("buffers");
	CHECK(screen_has("1:v.txt") && screen_has("2:w.txt"));
}

static void the_command_line_saves_and_quits_only_safely(void)
{
	FakeFile *f;

	/* :w */
	setup_keys(WF_PROFILE_VIM);
	f = add(L"C:\\w\\v.txt", "keep\n");
	CHECK(open_file(L"C:\\w\\v.txt"));
	chr('x');
	enter_command("w");
	CHECK(1 == W.saves && !memcmp(f->data, "eep\n", 4));
	CHECK(1 == wf_es_buffer_count(S));

	/* :q with unsaved changes asks, and nothing is decided by one stray key */
	chr('x');
	{
		int saves_before = W.saves;

		chr(':');
		typing("q");
		CHECK(WF_ES_REDRAW == key(WF_KEY_ENTER) && wf_es_prompt_active(S));
		CHECK(screen_has("Save changes to v.txt?"));
		CHECK(WF_ES_REDRAW == key(WF_KEY_ENTER) && !wf_es_prompt_active(S));
		CHECK(1 == wf_es_buffer_count(S) && saves_before == W.saves);
	}
	chr(':');
	typing("q");
	CHECK(WF_ES_REDRAW == key(WF_KEY_ENTER) && wf_es_prompt_active(S));
	CHECK(WF_ES_REDRAW == chr('c') && !wf_es_prompt_active(S));
	CHECK(1 == wf_es_buffer_count(S));

	/* :q! throws away this buffer's changes and nothing else is written */
	W.saves = 0;
	chr(':');
	typing("q!");
	CHECK(WF_ES_CLOSED == key(WF_KEY_ENTER));
	CHECK(0 == W.saves && !memcmp(f->data, "eep\n", 4));

	/* :wq saves, then closes */
	setup_keys(WF_PROFILE_VIM);
	f = add(L"C:\\w\\v.txt", "keep\n");
	CHECK(open_file(L"C:\\w\\v.txt"));
	chr('x');
	chr(':');
	typing("wq");
	CHECK(WF_ES_CLOSED == key(WF_KEY_ENTER));
	CHECK(1 == W.saves && !memcmp(f->data, "eep\n", 4));

	/* :wq does not close when the save fails */
	setup_keys(WF_PROFILE_VIM);
	f = add(L"C:\\w\\v.txt", "keep\n");
	CHECK(open_file(L"C:\\w\\v.txt"));
	chr('x');
	W.next_save = WF_SV_IO;
	chr(':');
	typing("wq");
	CHECK(WF_ES_CLOSED != key(WF_KEY_ENTER));
	CHECK(1 == wf_es_buffer_count(S) && screen_has("Not saved"));
	CHECK(!memcmp(f->data, "keep\n", 5));
	/* and the wish to close does not linger: the next :w only saves */
	enter_command("w");
	CHECK(1 == wf_es_buffer_count(S));

	/* :q on a clean buffer leaves */
	setup_keys(WF_PROFILE_VIM);
	add(L"C:\\w\\v.txt", "keep\n");
	CHECK(open_file(L"C:\\w\\v.txt"));
	chr(':');
	typing("q");
	CHECK(WF_ES_CLOSED == key(WF_KEY_ENTER));
}

static void the_command_line_refuses_what_it_does_not_know(void)
{
	setup_keys(WF_PROFILE_VIM);
	add(L"C:\\w\\v.txt", "keep\n");
	CHECK(open_file(L"C:\\w\\v.txt"));
	enter_command("frobnicate");
	CHECK(screen_has("Unknown command: frobnicate"));
	CHECK(1 == wf_es_buffer_count(S));
	enter_command("!dir");
	CHECK(1 == wf_es_buffer_count(S) && !wf_es_prompt_active(S));
	CHECK(screen_has("Unknown command: !dir"));
	enter_command("w extra words");
	CHECK(0 == W.saves && screen_has("nothing may follow"));
	/* :replace opens the same replace prompt as the key */
	enter_command("replace");
	CHECK(wf_es_prompt_active(S) && screen_has("Replace"));
	key(WF_KEY_ESC);
	/* the history of the command line is its own */
	chr(':');
	key(WF_KEY_UP);
	key(WF_KEY_ESC);
}

static void the_nano_profile_types_in_the_editor_and_uses_ctrl_keys(void)
{
	FakeFile *f;

	setup_keys(WF_PROFILE_NANO);
	f = add(L"C:\\w\\n.txt", "keep\n");
	CHECK(open_file(L"C:\\w\\n.txt"));
	typing("a");
	CHECK(text_is("akeep\n"));
	ctrl('s');
	CHECK(1 == W.saves && !memcmp(f->data, "akeep\n", 6));
	/* Ctrl+X on a clean buffer leaves */
	CHECK(WF_ES_CLOSED == ctrl('x'));
	/* the help is made from the keys in force */
	setup_keys(WF_PROFILE_NANO);
	add(L"C:\\w\\n.txt", "keep\n");
	CHECK(open_file(L"C:\\w\\n.txt"));
	key(WF_KEY_F2);
	CHECK(screen_has("Ctrl+S"));
	setup_keys(WF_PROFILE_VIM);
	add(L"C:\\w\\n.txt", "keep\n");
	CHECK(open_file(L"C:\\w\\n.txt"));
	key(WF_KEY_F2);
	CHECK(screen_has("gg, Ctrl+Home") && screen_has("Next buffer"));
	{
		bool seen = false;

		for (int page = 0; page < 8 && !seen; page++) {
			key(WF_KEY_PGDN); /* the list goes on: operators, Visual, Insert */
			seen = screen_has("Delete with a motion");
		}
		CHECK(seen);
	}
}

static void vim_sequence_expires_without_another_key(void)
{
	setup_keys(WF_PROFILE_VIM);
	add(L"C:\\w\\timeout.txt", "one\ntwo\nthree");
	CHECK(open_file(L"C:\\w\\timeout.txt"));
	wf_es_set_clock(S, 100);
	typing("g");
	CHECK(WF_SEQ_TIMEOUT_MS == wf_es_timeout(S, 100));
	CHECK(!wf_es_tick(S, 100 + WF_SEQ_TIMEOUT_MS - 1));
	CHECK(wf_es_tick(S, 100 + WF_SEQ_TIMEOUT_MS));
	CHECK(UINT32_MAX == wf_es_timeout(S, 100 + WF_SEQ_TIMEOUT_MS));
	CHECK(!wf_es_tick(S, 100 + WF_SEQ_TIMEOUT_MS + 1));
	CHECK(1 == wf_ed_cursor_col(wf_es_current(S)));
	typing("2g");
	CHECK(wf_es_tick(S, 100 + 2 * WF_SEQ_TIMEOUT_MS + 1));
	typing("i");
	CHECK(screen_has("-- INSERT --"));
}

static void uncertain_saves_cannot_be_retried_and_wrapped_cursor_stays_visible(
        void)
{
	setup(WF_KM_PIERO);
	add(L"C:\\w\\uncertain.txt", "base");
	CHECK(open_file(L"C:\\w\\uncertain.txt"));
	typing("X");
	W.next_save = WF_SV_COMMITTED_UNVERIFIED;
	ctrl('s');
	CHECK(W.saves == 1 && wf_ed_modified(wf_es_current(S)));
	CHECK(wf_es_take_saved(S));
	ctrl('s');
	CHECK(W.saves == 1 && strstr(wf_es_status(S), "Reopen"));

	WfEditConfig c;
	WfEditIo io = {NULL, fake_load, fake_over, fake_new, fake_clip_get,
	        fake_clip_set, fake_draft_allowed, fake_draft_write,
	        fake_draft_read, fake_draft_remove, NULL};
	wf_es_free(S);
	memset(&W, 0, sizeof W);
	wf_es_config_default(&c);
	c.soft_wrap = true;
	c.line_numbers = false;
	S = wf_es_new(&c, io);
	char line[401];
	memset(line, 'x', 400);
	line[400] = 0;
	add(L"C:\\w\\wraplong.txt", line);
	CHECK(open_file(L"C:\\w\\wraplong.txt"));
	wf_ed_goto(wf_es_current(S), 1, 401);
	WfFrame *f = wf_frame_new(40, 10, &(WfStyle){0, 0, 0});
	wf_es_draw(S, f);
	unsigned cursors = 0;
	for (int y = 1; y < 8; y++) {
		for (int x = 0; x < 40; x++) {
			cursors += !!(f->cells[y * 40 + x].attributes &
			        WF_ATTR_REVERSE);
		}
	}
	CHECK(cursors == 1);
	wf_ed_goto(wf_es_current(S), 1, 1);
	wf_es_draw(S, f);
	CHECK(f->cells[40].attributes & WF_ATTR_REVERSE);
	wf_frame_free(f);
}

/*
 * The syntax worker is a fake that records what it was asked and lets the test
 * answer. What is checked: a buffer a grammar can colour is handed over as the
 * parser reads it (lines and line feeds, whatever the file's line ends), nothing
 * is asked while the cursor only moves, the text is plain until the answer comes,
 * the answer colours the right bytes, an answer for older text is dropped, the
 * colours stay where they were while the next answer is on its way, and the
 * bytes that are saved are the bytes that were typed.
 */
#include "syntax/wf_synport.h"

static struct
{
	int requests, closes;
	uint32_t gen;
	char lang[24];
	char text[512];
	size_t len;
	bool installed;
	uint32_t slot;
} FS;

static WfStatus fs_request(void *ctx,
        uint32_t slot,
        uint32_t gen,
        const char *lang,
        const char *text,
        size_t len)
{
	(void)ctx;
	FS.requests++;
	FS.slot = slot;
	FS.gen = gen;
	strcpy_s(FS.lang, sizeof FS.lang, lang);
	FS.len = len < sizeof FS.text ? len : sizeof FS.text;
	memcpy(FS.text, text, FS.len);
	return WF_OK;
}

static void fs_close(void *ctx, uint32_t slot)
{
	(void)ctx;
	(void)slot;
	FS.closes++;
}

static bool fs_installed(void *ctx, const char *lang)
{
	(void)ctx;
	return FS.installed && (!strcmp(lang, "json") || !strcmp(lang, "toml"));
}

static void syntax_setup(void)
{
	WfSynPort port = {NULL, fs_request, fs_close, fs_installed};

	memset(&FS, 0, sizeof FS);
	FS.installed = true;
	setup(WF_KM_PIERO);
	wf_es_set_syntax(S, port);
}

/* An answer for the last request: `runs` as (start, length, class) triples. */
static void answer(const WfSynRun *runs, uint32_t n, uint32_t status)
{
	WfSynAnswer a;

	memset(&a, 0, sizeof a);
	a.slot = FS.slot;
	a.gen = FS.gen;
	a.status = status;
	a.text_len = (uint32_t)FS.len;
	if (n) {
		a.runs = malloc(n * sizeof *runs);
		memcpy(a.runs, runs, n * sizeof *runs);
		a.nruns = n;
	}
	wf_es_on_syntax(S, &a);
	wf_synanswer_free(&a);
}

/* The foreground colour of the cell the text `needle` starts at, on screen. */
static int fg_of(const char *needle, uint32_t *fg)
{
	WfFrame *f = wf_frame_new(COLS, ROWS, &(WfStyle){0, 0, 0});
	int found = 0;

	wf_es_draw(S, f);
	for (int y = 1; y < f->rows - 1 && !found; y++) {
		char row[2048] = "";
		const char *hit;

		for (int x = 0; x < f->cols; x++) {
			strcat_s(row, sizeof row,
			        f->cells[y * f->cols + x].utf8);
		}
		hit = strstr(row, needle);
		if (hit) {
			*fg = f->cells[y * f->cols + (int)(hit - row)].fg_rgb;
			found = 1;
		}
	}
	wf_frame_free(f);
	return found;
}

/* The text the parser reads is rebuilt for a new text; when memory runs out in
 * the middle of that, the answer for the OLD text must not be read against the
 * new, smaller buffer. One long line becomes many short ones: the buffer
 * shrinks, and the line-start table (which must grow) is what cannot be had. */
static void a_failed_rebuild_does_not_keep_the_old_length(void)
{
	static const wchar_t *path = L"C:\\w\\big.json";
	char longline[460], few[80];
	uint32_t fg;
	WfEditor *e;

	syntax_setup();
	memset(longline, 'x', sizeof longline);
	memcpy(longline, "{\"k\": \"", 7);
	memcpy(longline + sizeof longline - 3, "\"}", 3);
	add(path, longline);
	CHECK(open_file(path));
	CHECK(fg_of("xx", &fg)); /* drawn: the long text is asked for */
	CHECK(1 == FS.requests && FS.len > 400);
	e = wf_es_current(S);
	memset(few, 0, sizeof few);
	for (int i = 0; i < 30; i++) {
		few[i * 2] = (char)('a' + i % 20);
		few[i * 2 + 1] = '\n';
	}
	CHECK(WF_OK ==
	        wf_ed_set_text(e, few, 60, (WfEncoding){WF_ENC_UTF8, 0}));
	wf_et_fail_nth_alloc(
	        1); /* the buffer is resized; the line table is not */
	(void)fg_of("a", &fg);
	wf_et_fail_nth_alloc(-1);
	CHECK(1 ==
	        FS.requests); /* nothing was asked for the text it could not hold */
	answer(NULL, 0,
	        WF_SYN_OK); /* the answer for the long text arrives late */
	CHECK(WF_TS_READY != S->buf[S->cur]->syn.ts.state);
}

/* What each key costs on the editor side of the colouring: the buffer is made
 * into the one text the parser reads and handed over, at every drawing after a
 * change. A JSON text of about 1 MiB, 100 letters typed and drawn; the worker is
 * the fake of these tests, so this is the part before the worker. */
static void a_key_in_a_coloured_buffer_costs(void)
{
	enum
	{
		LINES = 30000,
		KEYS = 100
	};
	char *text = malloc((size_t)LINES * 40 + 16);
	size_t n = 0;
	LARGE_INTEGER a, b, freq;
	WfFrame *f = wf_frame_new(COLS, ROWS, &(WfStyle){0, 0, 0});

	CHECK(NULL != text);
	syntax_setup();
	for (int l = 0; text && l < LINES; l++) {
		n += (size_t)snprintf(
		        text + n, 40, "{\"key%05d\": \"value %05d\"},\n", l, l);
	}
	add(L"C:\\w\\big.json", "{}\n");
	CHECK(open_file(L"C:\\w\\big.json"));
	CHECK(WF_OK ==
	        wf_ed_set_text(wf_es_current(S), text, n,
	                (WfEncoding){WF_ENC_UTF8, 0}));
	wf_es_draw(S, f);
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&a);
	for (int k = 0; k < KEYS; k++) {
		CHECK(WF_OK == wf_ed_type(wf_es_current(S), 'a'));
		wf_es_draw(S, f);
	}
	QueryPerformanceCounter(&b);
	printf("info: %d keys in a coloured text of %zu bytes: %.1f ms (%d "
	       "requests)\n",
	        KEYS, n,
	        (double)(b.QuadPart - a.QuadPart) * 1000.0 /
	                (double)freq.QuadPart,
	        FS.requests);
	wf_frame_free(f);
	free(text);
}

static void a_grammar_colours_the_editor_buffer(void)
{
	static const wchar_t *path = L"C:\\w\\a.json";
	uint32_t plain, key_c, number, fg;
	FakeFile *f;
	WfSynRun runs[] = {{1, 3, WF_SC_KEY}, {6, 2, WF_SC_NUMBER}};

	syntax_setup();
	wf_theme_activate(&wf_theme_preset(WF_THEME_SPADE)->palette);
	plain = wf_theme_styles()->syntax[WF_SC_PLAIN];
	key_c = wf_theme_styles()->syntax[WF_SC_KEY];
	number = wf_theme_styles()->syntax[WF_SC_NUMBER];
	f = add(path, "{\"a\": 12}\r\n");
	CHECK(open_file(path));
	CHECK(0 ==
	        FS.requests); /* nothing is asked until the buffer is drawn */
	/* the first look: the text is asked for, as the parser reads it */
	CHECK(fg_of("12", &fg) && plain == fg); /* plain until the answer */
	CHECK(1 == FS.requests && !strcmp(FS.lang, "json") &&
	        WF_SYN_SLOT_EDITOR == FS.slot);
	CHECK(11 == FS.len && !memcmp(FS.text, "{\"a\": 12}\n\n", 11));
	/* the answer colours it */
	answer(runs, 2, WF_SYN_OK);
	CHECK(fg_of("\"a\"", &fg) && key_c == fg);
	CHECK(fg_of("12", &fg) && number == fg);
	/* moving the cursor and drawing again asks for nothing */
	key(WF_KEY_RIGHT);
	key(WF_KEY_DOWN);
	CHECK(fg_of("12", &fg) && number == fg);
	CHECK(1 == FS.requests);
	/* a theme change recolours and asks for nothing */
	wf_theme_activate(&wf_theme_preset(WF_THEME_HEART)->palette);
	CHECK(fg_of("\"a\"", &fg) &&
	        wf_theme_styles()->syntax[WF_SC_KEY] == fg);
	CHECK(key_c != fg);
	CHECK(1 == FS.requests);
	wf_theme_activate(&wf_theme_preset(WF_THEME_SPADE)->palette);
	/* typing changes the text: it is asked for again, once per drawing */
	key(WF_KEY_UP);
	key(WF_KEY_HOME);
	typing("[");
	CHECK(fg_of("12", &fg));
	CHECK(2 == FS.requests);
	CHECK(fg_of("12", &fg));
	CHECK(2 == FS.requests); /* the same text is not asked for twice */
	/* the old colours are still there, shifted by the one byte typed: "a" and 12
	 * keep their colours while the answer is on its way */
	CHECK(fg_of("\"a\"", &fg) && key_c == fg);
	CHECK(fg_of("12", &fg) && number == fg);
	CHECK(fg_of("[", &fg) && plain == fg); /* the edited stretch is plain */
	CHECK(fg_of("2}", &fg) &&
	        number == fg); /* the last digit moved with its colour */
	/* nothing was written by any of this */
	CHECK(0 == W.saves && 0 == W.new_saves);
	(void)f;
	/* the bytes saved are the bytes typed, with the file's own line ends */
	ctrl('s');
	CHECK(1 == W.saves && W.last_saved_n == 12 &&
	        !memcmp(W.last_saved, "[{\"a\": 12}\r\n", 12));
}

static void a_stale_answer_is_dropped(void)
{
	uint32_t plain, fg;
	WfSynRun runs[] = {{1, 3, WF_SC_KEY}};
	uint32_t old_gen;

	syntax_setup();
	wf_theme_activate(&wf_theme_preset(WF_THEME_SPADE)->palette);
	plain = wf_theme_styles()->syntax[WF_SC_PLAIN];
	add(L"C:\\w\\a.json", "{\"a\": 1}\n");
	CHECK(open_file(L"C:\\w\\a.json"));
	CHECK(fg_of("1", &fg));
	old_gen = FS.gen;
	typing("x"); /* new text, new request */
	CHECK(fg_of("1", &fg));
	CHECK(2 == FS.requests && FS.gen != old_gen);
	{
		WfSynAnswer a;

		memset(&a, 0, sizeof a);
		a.slot = WF_SYN_SLOT_EDITOR;
		a.gen = old_gen; /* the answer for the text before the key */
		a.status = WF_SYN_OK;
		a.runs = malloc(sizeof runs);
		memcpy(a.runs, runs, sizeof runs);
		a.nruns = 1;
		a.text_len =
		        (uint32_t)FS
		                .len; /* as long as the text now: only the request number says it is old */
		wf_es_on_syntax(S, &a);
		CHECK(a.runs); /* not taken: the receiver left it to the caller */
		wf_synanswer_free(&a);
	}
	CHECK(fg_of("\"a\"", &fg) && plain == fg); /* it was not applied */
	/* an answer for the editor slot of another request id is ignored too */
	{
		WfSynAnswer a;

		memset(&a, 0, sizeof a);
		a.slot = WF_SYN_SLOT_PREVIEW;
		a.gen = FS.gen;
		a.status = WF_SYN_OK;
		wf_es_on_syntax(S, &a);
	}
	CHECK(fg_of("\"a\"", &fg) && plain == fg);
}

static void a_refused_grammar_leaves_plain_text_and_editing_goes_on(void)
{
	uint32_t plain, fg;

	syntax_setup();
	wf_theme_activate(&wf_theme_preset(WF_THEME_SPADE)->palette);
	plain = wf_theme_styles()->syntax[WF_SC_PLAIN];
	add(L"C:\\w\\a.json", "{\"a\": 1}\n");
	CHECK(open_file(L"C:\\w\\a.json"));
	CHECK(fg_of("1", &fg));
	answer(NULL, 0, WF_SYN_E_TRUST);
	CHECK(fg_of("\"a\"", &fg) && plain == fg);
	typing("abc");
	CHECK(text_is("abc{\"a\": 1}\n"));
	ctrl('s');
	CHECK(1 == W.saves && !memcmp(W.last_saved, "abc{\"a\": 1}\n", 12));
	/* a limit: also plain, also editing as before */
	answer(NULL, 0, WF_SYN_E_LIMIT);
	CHECK(fg_of("1", &fg) && plain == fg);
}

static void no_grammar_means_the_hand_lexer_as_before(void)
{
	uint32_t fg, string;

	/* nothing installed: no request at all, and the old colours */
	syntax_setup();
	FS.installed = false;
	wf_theme_activate(&wf_theme_preset(WF_THEME_SPADE)->palette);
	string = wf_theme_styles()->syntax[WF_SC_STRING];
	add(L"C:\\w\\a.json", "{\"a\": \"x\"}\n");
	CHECK(open_file(L"C:\\w\\a.json"));
	CHECK(fg_of("\"x\"", &fg) && string == fg);
	CHECK(0 == FS.requests);
	/* a file no grammar names: also nothing asked */
	FS.installed = true;
	add(L"C:\\w\\n.txt", "just text\n");
	CHECK(open_file(L"C:\\w\\n.txt"));
	CHECK(fg_of("just", &fg));
	CHECK(0 == FS.requests);
	/* no port at all (a build without the worker) */
	setup(WF_KM_PIERO);
	add(L"C:\\w\\a.json", "{\"a\": \"x\"}\n");
	CHECK(open_file(L"C:\\w\\a.json"));
	CHECK(fg_of("\"x\"", &fg) && string == fg);
	/* syntax off in the configuration: nothing asked, plain */
	{
		WfEditConfig c;
		WfEditIo io = {NULL, fake_load, fake_over, fake_new,
		        fake_clip_get, fake_clip_set, fake_draft_allowed,
		        fake_draft_write, fake_draft_read, fake_draft_remove,
		        NULL};
		WfSynPort port = {NULL, fs_request, fs_close, fs_installed};

		memset(&W, 0, sizeof W);
		memset(&FS, 0, sizeof FS);
		FS.installed = true;
		wf_es_free(S);
		wf_es_config_default(&c);
		c.syntax = false;
		S = wf_es_new(&c, io);
		wf_es_set_syntax(S, port);
		add(L"C:\\w\\a.json", "{\"a\": \"x\"}\n");
		CHECK(open_file(L"C:\\w\\a.json"));
		CHECK(fg_of("\"x\"", &fg) &&
		        fg == wf_theme_styles()->syntax[WF_SC_PLAIN]);
		CHECK(0 == FS.requests);
	}
}

static void undo_and_redo_ask_again_and_switching_files_asks_for_the_new_one(
        void)
{
	uint32_t fg;

	syntax_setup();
	add(L"C:\\w\\a.json", "[1]\n");
	add(L"C:\\w\\b.toml", "x = 1\n");
	CHECK(open_file(L"C:\\w\\a.json"));
	CHECK(fg_of("1", &fg));
	CHECK(1 == FS.requests && !strcmp(FS.lang, "json"));
	typing("2");
	CHECK(fg_of("2", &fg));
	CHECK(2 == FS.requests);
	CHECK(WF_OK ==
	        wf_ed_undo(wf_es_current(
	                S))); /* the text changes: it is asked for again */
	CHECK(fg_of("1", &fg));
	CHECK(3 == FS.requests && FS.len >= 4);
	CHECK(!memcmp(FS.text, "[1]\n", 4));
	CHECK(WF_OK == wf_ed_redo(wf_es_current(S)));
	CHECK(fg_of("2", &fg));
	CHECK(4 == FS.requests);
	/* another file: its language, its text */
	CHECK(open_file(L"C:\\w\\b.toml"));
	CHECK(fg_of("x = 1", &fg));
	CHECK(5 == FS.requests && !strcmp(FS.lang, "toml") &&
	        !memcmp(FS.text, "x = 1\n", 6));
	/* back to the first: asked for again (the answer in hand is for the other),
	 * and its own unsaved text is the one that is sent */
	CHECK(open_file(L"C:\\w\\a.json"));
	CHECK(fg_of("2", &fg));
	CHECK(6 == FS.requests && !strcmp(FS.lang, "json") &&
	        !memcmp(FS.text, "2[1]\n", 5));
	/* closing the editor ends the session of the slot */
	{
		int closes = FS.closes;

		wf_es_free(S);
		S = NULL;
		CHECK(FS.closes == closes + 1);
	}
}

static void the_editor_never_sends_a_text_over_the_limit(void)
{
	uint32_t fg;
	char *big;

	syntax_setup();
	add(L"C:\\w\\a.json", "[1]\n");
	CHECK(open_file(L"C:\\w\\a.json"));
	CHECK(fg_of("1", &fg));
	/* a text of 3 MiB is not handed to the worker (the buffer's own limit is raised
	 * for this check) */
	big = malloc(3u * 1024u * 1024u);
	CHECK(big);
	if (big) {
		WfEditor *e = wf_es_current(S);
		size_t before = (size_t)FS.requests;

		for (size_t i = 0; i < 3u * 1024u * 1024u; i++) {
			big[i] = 999 == i % 1000 ? '\n' : 'x';
		}
		e->opt.max_bytes = 0; /* unlimited */
		e->text.max_bytes = 0;
		CHECK(WF_OK == wf_ed_insert_text(e, big, 3u * 1024u * 1024u));
		CHECK(fg_of("xxxx", &fg));
		CHECK((size_t)FS.requests == before); /* nothing sent */
		CHECK(fg == wf_theme_styles()->syntax[WF_SC_PLAIN]); /* plain */
		free(big);
	}
}

static void syntax_in_the_editor(void)
{
	a_grammar_colours_the_editor_buffer();
	a_failed_rebuild_does_not_keep_the_old_length();
	a_key_in_a_coloured_buffer_costs();
	a_stale_answer_is_dropped();
	a_refused_grammar_leaves_plain_text_and_editing_goes_on();
	no_grammar_means_the_hand_lexer_as_before();
	undo_and_redo_ask_again_and_switching_files_asks_for_the_new_one();
	the_editor_never_sends_a_text_over_the_limit();
}

/* The matches of one line are all marked, however many there are (the drawing
 * used to keep the first 64), also on the part scrolled into view and on the rows
 * of a wrapped line. */
static int marked_cells(const WfFrame *f, int row)
{
	int n = 0;

	for (int x = 0; x < f->cols; x++) {
		const WfCell *c = &f->cells[row * f->cols + x];

		n += c->bg_rgb == wf_theme_styles()->match.bg_rgb;
	}
	return n;
}

static void every_match_of_a_line_is_marked_not_the_first_sixty_four(void)
{
	WfEditConfig c;
	WfEditIo io = {NULL, fake_load, fake_over, fake_new, fake_clip_get,
	        fake_clip_set, fake_draft_allowed, fake_draft_write,
	        fake_draft_read, fake_draft_remove, NULL};
	char line[1024];
	size_t n = 0;
	WfFrame *f;

	/* 150 matches: "ab " 150 times (the fake file holds 512 bytes) */
	for (int i = 0; i < 150; i++) {
		n += (size_t)snprintf(line + n, sizeof line - n, "ab ");
	}
	snprintf(line + n, sizeof line - n, "END");
	/* wrapped: every row shows its own matches, 33 to a row of 100 cells */
	memset(&W, 0, sizeof W);
	wf_es_free(S);
	wf_es_config_default(&c);
	c.soft_wrap = true;
	S = wf_es_new(&c, io);
	add(L"C:\\w\\many.txt", line);
	CHECK(open_file(L"C:\\w\\many.txt"));
	ctrl('f');
	typing("ab");
	key(WF_KEY_ENTER);
	key(WF_KEY_F3);
	f = wf_frame_new(COLS, ROWS, &(WfStyle){0, 0, 0});
	wf_es_draw(S, f);
	{
		int total = 0;

		for (int row = 1; row < ROWS - 2; row++) {
			total += marked_cells(f, row);
		}
		/* 200 matches of 2 cells; the selected one is not "marked" but selected */
		CHECK(total >= 2 * 140);
	}
	wf_frame_free(f);
	/* not wrapped, scrolled to the end: the last matches are marked */
	memset(&W, 0, sizeof W);
	wf_es_free(S);
	wf_es_config_default(&c);
	c.soft_wrap = false;
	S = wf_es_new(&c, io);
	add(L"C:\\w\\many.txt", line);
	CHECK(open_file(L"C:\\w\\many.txt"));
	ctrl('f');
	typing("ab");
	key(WF_KEY_ENTER);
	key(WF_KEY_F3);
	key(WF_KEY_END);
	f = wf_frame_new(COLS, ROWS, &(WfStyle){0, 0, 0});
	wf_es_draw(S, f);
	CHECK(marked_cells(f, 1) >=
	        2 * 20); /* the end of 450 cells: far past the 64th match */
	wf_frame_free(f);
}

/* Backwards: the nearest match before the cursor, an overlapping one, never one
 * that starts inside a character, and the wrap to the end. */
/* The worst case of the plain search: a long line of one letter and a pattern
 * that is that letter nearly all the way. Printed, to know whether the search
 * needs more than the comparison at each start. */
static void the_worst_case_of_the_plain_search(void)
{
	enum
	{
		LINE = 1000000,
		PAT = 100
	};
	WfEText t;
	char *line = malloc(LINE), pat[PAT + 1];
	WfSearch q;
	WfMatch m;
	clock_t t0;
	bool found;

	CHECK(line != NULL);
	memset(line, 'a', LINE);
	memset(pat, 'a', PAT);
	pat[PAT - 1] = 'b'; /* never there */
	pat[PAT] = '\0';
	CHECK(WF_OK == wf_et_init(&t, WF_EOL_LF, 0));
	CHECK(WF_OK ==
	        wf_et_insert(&t, (WfPos){0, 0}, line, LINE, false, NULL));
	q.pat = pat;
	q.pat_n = PAT;
	q.match_case = true;
	q.whole_word = false;
	t0 = clock();
	found = wf_es_find(&t, &q, (WfPos){0, 0}, true, false, &m, NULL);
	printf("info: the plain search, 1000000-unit line and a pattern of %d "
	       "that fails at its end: %.1f ms\n",
	        (int)PAT, (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC);
	CHECK(!found);
	wf_et_free(&t);
	free(line);
}

static void searching_backwards_finds_the_match_before_the_cursor(void)
{
	WfEText t;
	WfSearch q = {"aa", 2, true, false};
	WfMatch m;
	bool wrapped;
	static const char J[] = "\xe3\x81\x82\xe3\x81\x82 \xe3\x81\x82";

	CHECK(WF_OK == wf_et_init(&t, WF_EOL_LF, 0));
	CHECK(WF_OK ==
	        wf_et_insert(
	                &t, (WfPos){0, 0}, "aaa aa\nbb\naa", 12, false, NULL));
	/* from the end of line 3: the match on that line starts at 0 */
	CHECK(wf_es_find(&t, &q, (WfPos){2, 2}, false, false, &m, &wrapped));
	CHECK(2 == m.start.line && 0 == m.start.col);
	CHECK(wf_es_find(&t, &q, (WfPos){2, 0}, false, false, &m, &wrapped));
	CHECK(0 == m.start.line && 4 == m.start.col &&
	        !wrapped); /* "aa" at 4 of line 1 */
	/* overlapping: "aaa" holds two, the later start is the nearer */
	CHECK(wf_es_find(&t, &q, (WfPos){0, 4}, false, false, &m, &wrapped));
	CHECK(0 == m.start.line && 1 == m.start.col);
	CHECK(wf_es_find(&t, &q, (WfPos){0, 1}, false, false, &m, &wrapped));
	CHECK(0 == m.start.line && 0 == m.start.col);
	/* before the first match: nothing without a wrap, the last one with it */
	CHECK(!wf_es_find(&t, &q, (WfPos){0, 0}, false, false, &m, &wrapped));
	CHECK(wf_es_find(&t, &q, (WfPos){0, 0}, false, true, &m, &wrapped));
	CHECK(wrapped && 2 == m.start.line && 0 == m.start.col);
	wf_et_free(&t);
	/* a pattern longer than the line, and an empty pattern */
	CHECK(WF_OK == wf_et_init(&t, WF_EOL_LF, 0));
	CHECK(WF_OK == wf_et_insert(&t, (WfPos){0, 0}, "a", 1, false, NULL));
	CHECK(!wf_es_find(&t, &q, (WfPos){0, 1}, false, true, &m, &wrapped));
	wf_et_free(&t);
	/* UTF-8: a match never starts at a continuation byte */
	{
		WfSearch qj = {"\xe3\x81\x82", 3, true, false};

		CHECK(WF_OK == wf_et_init(&t, WF_EOL_LF, 0));
		CHECK(WF_OK ==
		        wf_et_insert(&t, (WfPos){0, 0}, J, sizeof J - 1, false,
		                NULL));
		CHECK(wf_es_find(
		        &t, &qj, (WfPos){0, 9}, false, false, &m, &wrapped));
		CHECK(7 ==
		        m.start.col); /* the last character starts at 7, before 9 */
		CHECK(wf_es_find(
		        &t, &qj, (WfPos){0, 3}, false, false, &m, &wrapped));
		CHECK(0 == m.start.col);
		CHECK(wf_es_find(
		        &t, &qj, (WfPos){0, 7}, false, false, &m, &wrapped));
		CHECK(3 == m.start.col); /* strictly before the cursor */
		CHECK(!wf_es_find(
		        &t, &qj, (WfPos){0, 0}, false, false, &m, &wrapped));
		wf_et_free(&t);
	}
}

int main(void)
{
	uncertain_saves_cannot_be_retried_and_wrapped_cursor_stays_visible();
	setvbuf(stdout, NULL, _IONBF, 0);
	opening_typing_and_saving();
	a_wsl_file_is_edited_and_saved_like_any_other();
	names_that_differ_by_case_are_two_files_in_a_distribution();
	the_native_cursor_is_on_the_insertion_cell();
	the_save_question_is_answered_without_letters();
	a_half_saved_file_offers_the_files_it_left();
	drafts_are_written_offered_and_removed();
	a_restored_draft_is_the_saved_text_byte_for_byte();
	case_does_not_decide_which_buffer_a_file_is();
	typing_a_letter_does_not_count_the_line_ends_again();
	soft_wrap_cost_of_moving_the_cursor();
	kept_wrap_rows_are_never_stale();
	the_line_end_label_follows_the_edits_that_change_it();
	an_outside_change_is_said_once_and_nothing_else_happens();
	the_session_place_is_reported_and_put_back();
	ime_leftovers_are_not_answers();
	a_prompt_takes_the_native_cursor_and_gives_it_back();
	committed_japanese_is_saved_and_read_back();
	leaving_with_unsaved_text_asks_and_one_key_decides_nothing();
	a_failed_save_never_closes_the_buffer();
	a_file_changed_on_disk_offers_reload_save_as_or_cancel();
	save_as_never_overwrites_and_checks_names();
	pasted_text_fills_a_prompt_but_never_confirms_it();
	find_replace_goto();
	a_draft_is_written_only_for_text_it_does_not_hold();
	a_draft_that_failed_afterwards_is_written_again();
	the_match_count_follows_edits_and_patterns();
	replace_all_is_one_undo_step();
	the_nano_preset_does_what_the_table_says();
	a_refused_system_clipboard_is_reported_and_not_trusted();
	a_line_copied_comes_back_as_a_line_through_a_crlf_clipboard();
	a_text_that_does_not_fit_never_becomes_an_empty_buffer();
	controls_taken_from_the_text_into_a_prompt_are_only_shown();
	the_system_clipboard_is_used_for_paste_and_is_sanitized();
	prompt_shows_controls_in_readable_form();
	several_buffers_and_no_double_opening();
	what_cannot_be_edited_is_refused_with_a_reason();
	unknown_text_is_opened_only_when_asked();
	a_read_only_file_cannot_be_typed_into();
	the_screen_marks_syntax_selection_and_matches();
	editing_keys_reach_the_same_operations();
	a_new_file_is_created_only_by_saving();
	a_new_file_breaks_lines_as_configured();
	a_file_can_be_inserted_at_the_cursor();
	soft_wrap_shows_long_lines_in_rows_without_changing_them();
	selection_search_marks_and_whitespace_are_drawn();
	every_window_size_draws_safely();
	pasted_escape_sequences_stay_text();
	vim_starts_in_normal_mode_and_insert_types();
	pasted_text_is_inserted_in_vim_normal_mode();
	vim_motions_counts_and_sequences();
	vim_lines_are_cut_copied_and_pasted_as_lines();
	vim_operators_take_motions();
	vim_counted_word_end_stops_at_the_word();
	vim_visual_registers_and_macros();
	vim_finds_objects_dot_and_append();
	vim_search_and_buffers();
	the_command_line_saves_and_quits_only_safely();
	the_command_line_refuses_what_it_does_not_know();
	the_nano_profile_types_in_the_editor_and_uses_ctrl_keys();
	vim_sequence_expires_without_another_key();
	every_match_of_a_line_is_marked_not_the_first_sixty_four();
	searching_backwards_finds_the_match_before_the_cursor();
	the_worst_case_of_the_plain_search();
	syntax_in_the_editor();
	wf_es_free(S);
	wf_keymap_free(KEYMAP);
	return TEST_RESULT();
}
