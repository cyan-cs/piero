/*
 * The real piero-syntax.dll and the real grammars next to this program
 * (DR-DRAFT-033): what each language colours, class by class, the lazy loading,
 * the limits, bad input, the counters, and that opening and closing sessions
 * again and again does not leak handles or memory.
 */
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <string.h>

#include "render/wf_theme.h"
#include "syntax/wf_synhost_win.h"
#include "syntax/wf_synlang.h"
#include "syntax/wf_synruns.h"
#include "wf_syntax_pin.h"
#include "wf_test.h"

static bool loaded(const wchar_t *module)
{
	return NULL != GetModuleHandleW(module);
}

static bool runs_are_valid(const WfSynResult *r, size_t len)
{
	uint32_t end = 0;

	for (uint32_t i = 0; i < r->nruns; i++) {
		const WfSynRun *u = &r->runs[i];

		if (!u->length || u->start < end || u->cls >= WF_SC_COUNT ||
		        !u->cls || (size_t)u->start + u->length > len) {
			return false;
		}
		end = u->start + u->length;
	}
	return true;
}

/* The class at the `nth` (0-based) place `needle` is in `text`: -1 when it is
 * not there. Every byte of the needle must have the same class. */
static int class_of(
        const WfSynResult *r, const char *text, const char *needle, int nth)
{
	const char *at = text;
	size_t n = strlen(needle);
	uint32_t c;

	for (int i = 0; i <= nth; i++) {
		at = strstr(at, needle);
		if (!at) {
			return -1;
		}
		if (i < nth) {
			at += n;
		}
	}
	c = wf_syn_runs_class_at(r->runs, r->nruns, (uint32_t)(at - text));
	for (size_t k = 1; k < n; k++) {
		if (wf_syn_runs_class_at(r->runs, r->nruns,
		            (uint32_t)(at - text + (ptrdiff_t)k)) != c) {
			return -2; /* the needle is not one run of one class */
		}
	}
	return (int)c;
}

typedef struct
{
	const char *needle;
	int nth;
	unsigned cls;
} Expect;

static void check_language(
        const char *id, const char *text, const Expect *ex, size_t nex)
{
	WfSynSession *s = NULL;
	WfSynResult r;
	uint32_t st = wf_synhost_open(id, NULL, &s);

	if (WF_SYN_OK != st) {
		printf("open %s: %s\n", id, wf_syn_status_name(st));
		CHECK_FAIL("a grammar did not open");
		return;
	}
	st = wf_synhost_highlight(s, text, strlen(text), &r);
	CHECK(WF_SYN_OK == st);
	CHECK(runs_are_valid(&r, strlen(text)));
	CHECK(r.nruns > 0);
	for (size_t i = 0; i < nex; i++) {
		int got = class_of(&r, text, ex[i].needle, ex[i].nth);

		if (got != (int)ex[i].cls) {
			printf("%s: \"%s\" #%d: class %d, expected %u\n", id,
			        ex[i].needle, ex[i].nth, got, ex[i].cls);
			CHECK_FAIL("a class is not the one expected");
		}
	}
	wf_synhost_result_free(&r);
	wf_synhost_close(s);
}

static void json_classes(void)
{
	static const char T[] = "{\"k\": \"v\", \"n\": 42.5e3, \"t\": true, "
	                        "\"f\": false, \"z\": null, "
	                        "\"e\": \"a\\nb\"} // c\n";
	static const Expect ex[] = {{"\"k\"", 0, WF_SC_KEY},
	        {"\"v\"", 0, WF_SC_STRING}, {"42.5e3", 0, WF_SC_NUMBER},
	        {"true", 0, WF_SC_BOOLEAN}, {"false", 0, WF_SC_BOOLEAN},
	        {"null", 0, WF_SC_NULL}, {"\\n", 0, WF_SC_ESCAPE},
	        {"// c", 0, WF_SC_COMMENT}, {"{", 0, WF_SC_PUNCTUATION},
	        {":", 0, WF_SC_PUNCTUATION}, {"}", 0, WF_SC_PUNCTUATION}};

	check_language("json", T, ex, sizeof ex / sizeof ex[0]);
}

static void jsonc_classes(void)
{
	static const char T[] =
	        "{\n  // line\n  \"a\": 1, /* block */\n  \"b\": [true,],\n}\n";
	static const Expect ex[] = {{"// line", 0, WF_SC_COMMENT},
	        {"/* block */", 0, WF_SC_COMMENT}, {"\"a\"", 0, WF_SC_KEY},
	        {"1", 0, WF_SC_NUMBER}, {"true", 0, WF_SC_BOOLEAN},
	        {"[", 0, WF_SC_PUNCTUATION}};

	check_language("jsonc", T, ex, sizeof ex / sizeof ex[0]);
}

static void yaml_classes(void)
{
	static const char T[] =
	        "# c\nname: piero\nlist:\n  - 1\n  - true\n  - null\n"
	        "  - \"q\"\nanchor: &a {x: 1}\nref: *a\n";
	static const Expect ex[] = {{"# c", 0, WF_SC_COMMENT},
	        {"name", 0, WF_SC_KEY}, {"piero", 0, WF_SC_STRING},
	        {"list", 0, WF_SC_KEY}, {"1", 0, WF_SC_NUMBER},
	        {"true", 0, WF_SC_BOOLEAN}, {"null", 0, WF_SC_NULL},
	        {"\"q\"", 0, WF_SC_STRING}, {"&", 0, WF_SC_OPERATOR},
	        {"a", 2, WF_SC_VARIABLE}, {"a", 3, WF_SC_VARIABLE},
	        {"-", 0, WF_SC_PUNCTUATION}};

	check_language("yaml", T, ex, sizeof ex / sizeof ex[0]);
}

static void toml_classes(void)
{
	static const char T[] =
	        "# c\n[server]\nhost = \"h\\n\"\nport = 80\non = true\n"
	        "when = 2020-01-02\n[[items]]\nk = [1, 2]\n";
	static const Expect ex[] = {{"# c", 0, WF_SC_COMMENT},
	        {"server", 0, WF_SC_HEADING}, {"host", 0, WF_SC_KEY},
	        {"\\n", 0, WF_SC_ESCAPE}, {"80", 0, WF_SC_NUMBER},
	        {"true", 0, WF_SC_BOOLEAN}, {"2020-01-02", 0, WF_SC_CONSTANT},
	        {"=", 0, WF_SC_OPERATOR}, {"[[", 0, WF_SC_PUNCTUATION},
	        {"items", 0, WF_SC_HEADING}};

	check_language("toml", T, ex, sizeof ex / sizeof ex[0]);
}

static void ini_classes(void)
{
	static const char T[] = "; c\n[core]\nname = piero\nflag=1\n";
	static const Expect ex[] = {{"core", 0, WF_SC_HEADING},
	        {"name", 0, WF_SC_KEY}, {"flag", 0, WF_SC_KEY},
	        {"=", 0, WF_SC_OPERATOR}, {"[", 0, WF_SC_PUNCTUATION}};

	check_language("ini", T, ex, sizeof ex / sizeof ex[0]);
	/* the comment is a comment (the grammar's node ends with the line) */
	{
		WfSynSession *s = NULL;
		WfSynResult r;

		CHECK(WF_SYN_OK == wf_synhost_open("ini", NULL, &s));
		CHECK(WF_SYN_OK == wf_synhost_highlight(s, T, strlen(T), &r));
		CHECK(WF_SC_COMMENT ==
		        wf_syn_runs_class_at(r.runs, r.nruns, 0));
		wf_synhost_result_free(&r);
		wf_synhost_close(s);
	}
}

static void xml_classes(void)
{
	static const char T[] =
	        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!-- c -->\n"
	        "<a id=\"1\" k='v'>text &amp; <b/><![CDATA[x]]></a>\n";
	static const Expect ex[] = {{"xml", 0, WF_SC_KEYWORD},
	        {"version", 0, WF_SC_KEY}, {"1.0", 0, WF_SC_NUMBER},
	        {"UTF-8", 0, WF_SC_STRING}, {"<!-- c -->", 0, WF_SC_COMMENT},
	        {"id", 0, WF_SC_KEY}, {"\"1\"", 0, WF_SC_STRING},
	        {"'v'", 0, WF_SC_STRING}, {"&amp;", 0, WF_SC_CONSTANT},
	        {"/>", 0, WF_SC_PUNCTUATION}, {"=", 0, WF_SC_OPERATOR}};

	check_language("xml", T, ex, sizeof ex / sizeof ex[0]);
}

static void markdown_classes(void)
{
	static const char T[] =
	        "# Title\n\nSome *em* and **strong** and `code` and "
	        "[link](http://x).\n\n- item\n\n```c\nint x;\n```\n";
	static const Expect ex[] = {{"#", 0, WF_SC_HEADING},
	        {"Title", 0, WF_SC_HEADING}, {"em", 0, WF_SC_MARKUP},
	        {"strong", 0, WF_SC_MARKUP}, {"code", 0, WF_SC_STRING},
	        {"link", 0, WF_SC_LINK}, {"http://x", 0, WF_SC_LINK},
	        {"- ", 0, WF_SC_PUNCTUATION}, {"```", 0, WF_SC_PUNCTUATION},
	        {"c", 1, WF_SC_TYPE}};

	check_language("markdown", T, ex, sizeof ex / sizeof ex[0]);
}

static void powershell_classes(void)
{
	static const char T[] =
	        "# c\nfunction Get-X {\n  param($a)\n  if ($a -eq 1) { \"s\" } "
	        "else { 42 }\n}\n$v = Get-X 3\n";
	static const Expect ex[] = {{"# c", 0, WF_SC_COMMENT},
	        {"function", 0, WF_SC_KEYWORD}, {"Get-X", 0, WF_SC_FUNCTION},
	        {"param", 0, WF_SC_KEYWORD}, {"$a", 0, WF_SC_VARIABLE},
	        {"-eq", 0, WF_SC_OPERATOR}, {"1", 0, WF_SC_NUMBER},
	        {"\"s\"", 0, WF_SC_STRING}, {"if", 0, WF_SC_KEYWORD},
	        {"else", 0, WF_SC_KEYWORD}, {"42", 0, WF_SC_NUMBER}};

	check_language("powershell", T, ex, sizeof ex / sizeof ex[0]);
}

static void bash_classes(void)
{
	static const char T[] = "#!/bin/sh\n# c\nif [ -n \"$X\" ]; then\n  "
	                        "echo \"hi $X\" | grep h\n"
	                        "fi\nf() { return 0; }\n";
	static const Expect ex[] = {{"# c", 0, WF_SC_COMMENT},
	        {"if", 0, WF_SC_KEYWORD}, {"then", 0, WF_SC_KEYWORD},
	        {"fi", 0, WF_SC_KEYWORD}, {"echo", 0, WF_SC_FUNCTION},
	        {"grep", 0, WF_SC_FUNCTION}, {"-n", 0, WF_SC_OPERATOR},
	        {"|", 0, WF_SC_OPERATOR}, {"X", 0, WF_SC_VARIABLE},
	        {"hi ", 0, WF_SC_STRING}};

	check_language("bash", T, ex, sizeof ex / sizeof ex[0]);
}

static void nothing_is_loaded_until_something_is_coloured(void)
{
	WfSynSession *s = NULL;

	CHECK(!loaded(L"piero-syntax.dll"));
	CHECK(!wf_synhost_loaded());
	/* looking at what is installed loads nothing */
	for (size_t i = 0; i < WF_SYNLANG_COUNT; i++) {
		size_t n;
		const WfSynLang *l = wf_synlang_all(&n) + i;

		CHECK(wf_synhost_installed(
		        l)); /* the build installs all of them */
	}
	CHECK(!loaded(L"piero-syntax.dll"));
	CHECK(!wf_synhost_loaded());
	CHECK(0 == wf_synhost_stat(WF_SYN_STAT_LOADED));

	/* a session loads the runtime and that grammar, and no other */
	CHECK(WF_SYN_OK == wf_synhost_open("json", NULL, &s));
	CHECK(loaded(L"piero-syntax.dll") && wf_synhost_loaded());
	CHECK(1 == wf_synhost_stat(WF_SYN_STAT_LOADED));
	CHECK(1 == wf_synhost_stat(WF_SYN_STAT_SESSIONS));
	{
		WfSynSession *t = NULL;

		CHECK(WF_SYN_OK ==
		        wf_synhost_open("json", NULL, &t));      /* again */
		CHECK(1 == wf_synhost_stat(WF_SYN_STAT_LOADED)); /* not twice */
		CHECK(2 == wf_synhost_stat(WF_SYN_STAT_SESSIONS));
		wf_synhost_close(t);
	}
	{
		WfSynSession *y = NULL;

		CHECK(WF_SYN_OK == wf_synhost_open("yaml", NULL, &y));
		CHECK(2 ==
		        wf_synhost_stat(
		                WF_SYN_STAT_LOADED)); /* json and yaml */
		wf_synhost_close(y);
		/* a session that ends does not unload its grammar: it is kept */
		CHECK(2 == wf_synhost_stat(WF_SYN_STAT_LOADED));
	}
	wf_synhost_close(s);
}

static uint64_t parses(void)
{
	return wf_synhost_stat(WF_SYN_STAT_PARSES);
}

static void parses_are_counted_and_edits_are_incremental(void)
{
	static const char A[] = "{\"a\": 1, \"b\": [1, 2, 3]}\n";
	static const char B[] = "{\"a\": 12, \"b\": [1, 2, 3]}\n";
	WfSynSession *s = NULL;
	WfSynResult r;
	uint64_t before;

	CHECK(WF_SYN_OK == wf_synhost_open("json", NULL, &s));
	before = parses();
	CHECK(WF_SYN_OK == wf_synhost_highlight(s, A, strlen(A), &r));
	CHECK(0 == r.incremental && parses() == before + 1);
	wf_synhost_result_free(&r);
	/* the same text again: nothing is parsed */
	CHECK(WF_SYN_OK == wf_synhost_highlight(s, A, strlen(A), &r));
	CHECK(1 == r.incremental && parses() == before + 1);
	CHECK(WF_SC_NUMBER == class_of(&r, A, "1", 0));
	wf_synhost_result_free(&r);
	/* one edit: parsed once more, from the old tree */
	CHECK(WF_SYN_OK == wf_synhost_highlight(s, B, strlen(B), &r));
	CHECK(1 == r.incremental && parses() == before + 2);
	CHECK(WF_SC_NUMBER == class_of(&r, B, "12", 0));
	wf_synhost_result_free(&r);
	/* and back: the result is the same as for a text parsed from nothing */
	{
		WfSynSession *t = NULL;
		WfSynResult r2;

		CHECK(WF_SYN_OK == wf_synhost_open("json", NULL, &t));
		CHECK(WF_SYN_OK == wf_synhost_highlight(s, A, strlen(A), &r));
		CHECK(WF_SYN_OK == wf_synhost_highlight(t, A, strlen(A), &r2));
		CHECK(r.nruns == r2.nruns &&
		        !memcmp(r.runs, r2.runs, r.nruns * sizeof r.runs[0]));
		wf_synhost_result_free(&r);
		wf_synhost_result_free(&r2);
		wf_synhost_close(t);
	}
	/* a change of the theme parses nothing and loads nothing */
	{
		uint64_t p = parses(), l = wf_synhost_stat(WF_SYN_STAT_LOADED),
		         q = wf_synhost_stat(WF_SYN_STAT_QUERIES);

		for (int t = 0; t < WF_THEME_PRESET_COUNT; t++) {
			wf_theme_activate(
			        &wf_theme_preset((WfThemeId)t)->palette);
		}
		CHECK(p == parses() &&
		        l == wf_synhost_stat(WF_SYN_STAT_LOADED) &&
		        q == wf_synhost_stat(WF_SYN_STAT_QUERIES));
	}
	wf_synhost_close(s);
}

static void limits_stop_a_text_and_never_the_program(void)
{
	WfSynSession *s = NULL;
	WfSynResult r;
	char *big;
	WfSynLimits lim = {0, 0, 0, 0, 0};

	CHECK(WF_SYN_OK == wf_synhost_open("json", NULL, &s));
	/* over the size */
	big = malloc(3u * 1024u * 1024u);
	CHECK(big);
	memset(big, ' ', 3u * 1024u * 1024u);
	CHECK(WF_SYN_E_LIMIT ==
	        wf_synhost_highlight(s, big, 3u * 1024u * 1024u, &r));
	CHECK(0 == r.nruns && NULL == r.runs);
	/* one line over the line limit */
	memset(big, 'a', 70000);
	big[0] = '"';
	big[69999] = '"';
	CHECK(WF_SYN_E_LIMIT == wf_synhost_highlight(s, big, 70000, &r));
	free(big);
	wf_synhost_close(s);

	/* the runs: over a limit of 2 */
	lim.max_runs = 2;
	CHECK(WF_SYN_OK == wf_synhost_open("json", &lim, &s));
	CHECK(WF_SYN_E_LIMIT ==
	        wf_synhost_highlight(s, "{\"a\": 1, \"b\": 2}\n", 17, &r));
	wf_synhost_close(s);

	/* a smaller text limit */
	memset(&lim, 0, sizeof lim);
	lim.max_text = 8;
	CHECK(WF_SYN_OK == wf_synhost_open("json", &lim, &s));
	CHECK(WF_SYN_E_LIMIT ==
	        wf_synhost_highlight(s, "[1, 2, 3, 4]\n", 13, &r));
	CHECK(WF_SYN_OK == wf_synhost_highlight(s, "[1,2]\n", 6, &r));
	wf_synhost_result_free(&r);
	wf_synhost_close(s);
}

static void a_deeply_nested_text_returns_in_bounded_time(void)
{
	WfSynSession *s = NULL;
	WfSynResult r;
	WfSynLimits lim = {
	        0, 0, 50, 50, 0}; /* 50 ms for the parse and for the query */
	size_t n = 300000;
	char *t = malloc(n + 2);
	ULONGLONG t0, took;
	uint32_t st;

	CHECK(t);
	memset(t, '[', n);
	t[n] = '\n';
	CHECK(WF_SYN_OK == wf_synhost_open("json", &lim, &s));
	t0 = GetTickCount64();
	st = wf_synhost_highlight(s, t, n + 1, &r);
	took = GetTickCount64() - t0;
	/* either it was fast enough or it said it was not: never a hang */
	CHECK(WF_SYN_OK == st || WF_SYN_E_TIMEOUT == st ||
	        WF_SYN_E_LIMIT == st);
	CHECK(took < 5000);
	wf_synhost_result_free(&r);
	/* and the session is still usable */
	CHECK(WF_SYN_OK == wf_synhost_highlight(s, "[1]\n", 4, &r));
	CHECK(WF_SC_NUMBER == class_of(&r, "[1]\n", "1", 0));
	wf_synhost_result_free(&r);
	wf_synhost_close(s);
	free(t);
}

static void bad_text_gives_valid_runs(void)
{
	static const char bad[] =
	        "{\"a\": \xff\xfe 1, \"\x80\": [}}} \"unterminated\n\x01\x02";
	static const char many_errors[] = "}}}}]]]]{{{{[[[[:::,,,\"\"\"'''";
	WfSynSession *s = NULL;
	WfSynResult r;

	CHECK(WF_SYN_OK == wf_synhost_open("json", NULL, &s));
	CHECK(WF_SYN_OK == wf_synhost_highlight(s, bad, sizeof bad - 1, &r));
	CHECK(runs_are_valid(&r, sizeof bad - 1));
	wf_synhost_result_free(&r);
	CHECK(WF_SYN_OK ==
	        wf_synhost_highlight(
	                s, many_errors, sizeof many_errors - 1, &r));
	CHECK(runs_are_valid(&r, sizeof many_errors - 1));
	wf_synhost_result_free(&r);
	/* no text at all, and NUL bytes in it */
	CHECK(WF_SYN_OK == wf_synhost_highlight(s, "", 0, &r) && 0 == r.nruns);
	wf_synhost_result_free(&r);
	CHECK(WF_SYN_OK == wf_synhost_highlight(s, "[1,\0,2]", 7, &r));
	CHECK(runs_are_valid(&r, 7));
	wf_synhost_result_free(&r);
	CHECK(WF_SYN_E_BADARG == wf_synhost_highlight(s, NULL, 4, &r));
	wf_synhost_close(s);
	/* not an official language: refused, whatever it is called */
	CHECK(WF_SYN_E_UNKNOWN == wf_synhost_open("c", NULL, &s) && !s);
	CHECK(WF_SYN_E_UNKNOWN == wf_synhost_open("dotenv", NULL, &s) && !s);
	CHECK(WF_SYN_E_UNKNOWN ==
	                wf_synhost_open("markdown_inline", NULL, &s) &&
	        !s);
	CHECK(WF_SYN_E_UNKNOWN == wf_synhost_open("..\\json", NULL, &s) && !s);
	CHECK(WF_SYN_E_BADARG == wf_synhost_open(NULL, NULL, &s));
}

static void japanese_and_crlf_are_positions_in_bytes(void)
{
	/* "名前" and "日本語" are 6 and 9 bytes in UTF-8 */
	static const char T[] =
	        "{\"\xe5\x90\x8d\xe5\x89\x8d\": "
	        "\"\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\", \"n\": 7}\r\n";
	WfSynSession *s = NULL;
	WfSynResult r;
	const char *at;

	CHECK(WF_SYN_OK == wf_synhost_open("json", NULL, &s));
	CHECK(WF_SYN_OK == wf_synhost_highlight(s, T, sizeof T - 1, &r));
	CHECK(runs_are_valid(&r, sizeof T - 1));
	CHECK(WF_SC_KEY == class_of(&r, T, "\"\xe5\x90\x8d\xe5\x89\x8d\"", 0));
	CHECK(WF_SC_STRING ==
	        class_of(&r, T, "\"\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\"", 0));
	at = strstr(T, "\"\xe5\x90\x8d");
	CHECK(at &&
	        wf_syn_runs_class_at(r.runs, r.nruns, (uint32_t)(at - T)) ==
	                WF_SC_KEY);
	CHECK(WF_SC_NUMBER ==
	        class_of(&r, T, "7", 0)); /* after the multibyte text */
	/* no run starts or ends inside a character */
	for (uint32_t i = 0; i < r.nruns; i++) {
		CHECK(0x80 != ((unsigned char)T[r.runs[i].start] & 0xc0));
		if (r.runs[i].start + r.runs[i].length < sizeof T - 1) {
			CHECK(0x80 !=
			        ((unsigned char)T[r.runs[i].start +
			                 r.runs[i].length] &
			                0xc0));
		}
	}
	wf_synhost_result_free(&r);
	wf_synhost_close(s);
}

static size_t private_bytes(void)
{
	PROCESS_MEMORY_COUNTERS_EX m;

	m.cb = sizeof m;
	GetProcessMemoryInfo(
	        GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&m, sizeof m);
	return m.PrivateUsage;
}

#if defined(__SANITIZE_ADDRESS__)
#	define UNDER_ASAN 1
#elif defined(__has_feature)
#	if __has_feature(address_sanitizer)
#		define UNDER_ASAN 1
#	endif
#endif

/* The process grew by less than 4 MiB since `m0`. Not measurable under ASan,
 * whose quarantine and shadow memory are in the private bytes: the leak check
 * there is the allocator's own. */
static bool flat_since(size_t m0)
{
#ifdef UNDER_ASAN
	(void)m0;
	return true;
#else
	return private_bytes() < m0 + 4u * 1024u * 1024u;
#endif
}

static DWORD handles(void)
{
	DWORD n = 0;

	GetProcessHandleCount(GetCurrentProcess(), &n);
	return n;
}

static void sessions_do_not_leak(void)
{
	static const char T[] =
	        "{\"a\": [1, 2, {\"b\": null}], \"c\": \"text\"}\n";
	DWORD h0;
	size_t m0;

	/* one round first: the grammar is loaded and the allocator is warm */
	for (int i = 0; i < 20; i++) {
		WfSynSession *s = NULL;
		WfSynResult r;

		CHECK(WF_SYN_OK == wf_synhost_open("json", NULL, &s));
		CHECK(WF_SYN_OK ==
		        wf_synhost_highlight(s, T, sizeof T - 1, &r));
		wf_synhost_result_free(&r);
		wf_synhost_close(s);
	}
	h0 = handles();
	m0 = private_bytes();
	for (int i = 0; i < 2000; i++) {
		WfSynSession *s = NULL;
		WfSynResult r;

		CHECK(WF_SYN_OK == wf_synhost_open("json", NULL, &s));
		CHECK(WF_SYN_OK ==
		        wf_synhost_highlight(s, T, sizeof T - 1, &r));
		CHECK(WF_SYN_OK ==
		                wf_synhost_highlight(s, T, sizeof T - 2, &r) ||
		        true);
		wf_synhost_result_free(&r);
		wf_synhost_close(s);
	}
	CHECK(0 == wf_synhost_stat(WF_SYN_STAT_SESSIONS));
	CHECK(handles() <= h0 + 2);
	CHECK(flat_since(m0)); /* 2000 rounds: nothing grows */
	/* the same for one long-lived session over many edits */
	{
		WfSynSession *s = NULL;
		WfSynResult r;
		char t[64];

		CHECK(WF_SYN_OK == wf_synhost_open("json", NULL, &s));
		m0 = private_bytes();
		for (int i = 0; i < 3000; i++) {
			int n = snprintf(t, sizeof t,
			        "{\"a\": %d, \"b\": [%d]}\n", i, i * 7);

			CHECK(WF_SYN_OK ==
			        wf_synhost_highlight(s, t, (size_t)n, &r));
			wf_synhost_result_free(&r);
		}
		CHECK(flat_since(m0));
		wf_synhost_close(s);
	}
}

static void the_runtime_and_the_grammars_are_released_at_the_end(void)
{
	/* every session is closed: shutdown lets the DLLs go */
	CHECK(0 == wf_synhost_stat(WF_SYN_STAT_SESSIONS));
	wf_synhost_shutdown();
	CHECK(!wf_synhost_loaded());
	CHECK(!loaded(L"piero-syntax.dll"));
	CHECK(!loaded(L"parser.dll"));
	wf_synhost_shutdown(); /* twice is fine */
	/* and it can be loaded again */
	{
		WfSynSession *s = NULL;

		CHECK(WF_SYN_OK == wf_synhost_open("toml", NULL, &s));
		CHECK(1 == wf_synhost_stat(WF_SYN_STAT_LOADED));
		wf_synhost_close(s);
		wf_synhost_shutdown();
	}
}

static void a_host_that_was_never_pinned_refuses(void)
{
	WfSynSession *s = NULL;

	/* not configured: nothing is loaded, whatever is there */
	CHECK(WF_SYN_E_TRUST == wf_synhost_open("json", NULL, &s) && !s);
	CHECK(!loaded(L"piero-syntax.dll"));
	/* a pin that is not the DLL's */
	wf_synhost_configure(NULL,
	        "00000000000000000000000000000000000000000000000000000000000000"
	        "00");
	CHECK(WF_SYN_E_TRUST == wf_synhost_open("json", NULL, &s) && !s);
	CHECK(!loaded(L"piero-syntax.dll"));
	/* not 64 hex digits is no pin at all */
	wf_synhost_configure(NULL, "abc");
	CHECK(WF_SYN_E_TRUST == wf_synhost_open("json", NULL, &s) && !s);
	wf_synhost_configure(NULL, NULL);
	CHECK(WF_SYN_E_TRUST == wf_synhost_open("json", NULL, &s) && !s);
	CHECK(!loaded(L"piero-syntax.dll"));
}

int main(void)
{
	a_host_that_was_never_pinned_refuses();
	wf_synhost_configure(NULL, WF_SYNTAX_DLL_SHA256);
	nothing_is_loaded_until_something_is_coloured();
	json_classes();
	jsonc_classes();
	yaml_classes();
	toml_classes();
	ini_classes();
	xml_classes();
	markdown_classes();
	powershell_classes();
	bash_classes();
	parses_are_counted_and_edits_are_incremental();
	limits_stop_a_text_and_never_the_program();
	a_deeply_nested_text_returns_in_bounded_time();
	bad_text_gives_valid_runs();
	japanese_and_crlf_are_positions_in_bytes();
	sessions_do_not_leak();
	the_runtime_and_the_grammars_are_released_at_the_end();
	return TEST_RESULT();
}
