/*
 * How piero-syntax.dll's loader treats files that are not what it expects
 * (DR-DRAFT-033, section 4): the table of official hashes decides, the manifest
 * only describes, every refusal is a status and never a loaded module, the paths
 * and the DLL search are the ones the design says. The loader is linked straight
 * into this program with a trust table of its own, over copies of the real grammar
 * and over stand-in DLLs built by this test build (tests/syntax_fixtures).
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <winioctl.h>

#include "syntax/wf_dllload_win.h"
#include "syntax/wf_manifest.h"
#include "syntax/wf_synhost_win.h"
#include "syntax/wf_syncore.h"
#include "syntax/wf_synlang.h"
#include "syntax/wf_synruns.h"
#include "wf_test.h"

#ifndef REPARSE_DATA_BUFFER_HEADER_SIZE
#	define REPARSE_DATA_BUFFER_HEADER_SIZE 8
#endif

static wchar_t root[MAX_PATH + 80];  /* the fixture root */
static wchar_t built[MAX_PATH + 80]; /* the build's own syntax\ folder */
static wchar_t fixtures[MAX_PATH + 80];
static int skips;

static void skip(const char *what)
{
	printf("SKIP %s\n", what);
	skips++;
}

static void join(wchar_t *out, size_t cap, const wchar_t *a, const wchar_t *b)
{
	swprintf(out, cap, L"%ls\\%ls", a, b);
}

static bool mkdirs(const wchar_t *path)
{
	wchar_t tmp[MAX_PATH + 80];
	size_t n = wcslen(path);

	wcscpy_s(tmp, MAX_PATH + 80, path);
	for (size_t i = 3; i <= n; i++) {
		if (L'\\' == tmp[i] || L'\0' == tmp[i]) {
			wchar_t c = tmp[i];

			tmp[i] = L'\0';
			CreateDirectoryW(tmp, NULL);
			tmp[i] = c;
		}
	}
	return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static bool write_file(const wchar_t *path, const void *data, size_t n)
{
	HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD got = 0;
	bool ok;

	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	ok = (0 == n || WriteFile(h, data, (DWORD)n, &got, NULL)) && got == n;
	CloseHandle(h);
	return ok;
}

static char *read_file(const wchar_t *path, size_t *n)
{
	HANDLE h = CreateFileW(path, GENERIC_READ,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	LARGE_INTEGER sz;
	char *buf = NULL;
	DWORD got = 0;

	if (INVALID_HANDLE_VALUE == h) {
		return NULL;
	}
	if (GetFileSizeEx(h, &sz) && sz.QuadPart < 16 * 1024 * 1024) {
		buf = malloc((size_t)sz.QuadPart + 1);
		if (buf &&
		        (!ReadFile(h, buf, (DWORD)sz.QuadPart, &got, NULL) ||
		                got != sz.QuadPart)) {
			free(buf);
			buf = NULL;
		}
		if (buf) {
			buf[got] = '\0';
			*n = got;
		}
	}
	CloseHandle(h);
	return buf;
}

static bool copy_file(const wchar_t *from, const wchar_t *to)
{
	return 0 != CopyFileW(from, to, FALSE);
}

static void remove_tree(const wchar_t *dir)
{
	wchar_t cmd[2 * (MAX_PATH + 80)];
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;

	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	swprintf(cmd, 2 * (MAX_PATH + 80), L"cmd.exe /c rmdir /s /q \"%ls\"",
	        dir);
	if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL,
	            NULL, &si, &pi)) {
		WaitForSingleObject(pi.hProcess, 30000);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
	}
}

static void hex_of(const wchar_t *path, char out[65])
{
	size_t n = 0;
	char *b = read_file(path, &n);
	uint8_t d[32];

	out[0] = '\0';
	if (b && wf_dll_sha256(b, n, d)) {
		for (int i = 0; i < 32; i++) {
			snprintf(out + i * 2, 3, "%02x", d[i]);
		}
	}
	free(b);
}

typedef struct
{
	char dll[65], query[65], manifest[65];
	WfSynTrust t;
} Trust;

static const char GOOD_QUERY[] = "(string) @string\n(number) @number\n";

static void make_manifest(
        const wchar_t *path, unsigned abi, const char *id, const char *extra)
{
	char text[1024];
	int n = snprintf(text, sizeof text,
	        "id = \"%s\"\nversion = \"1\"\nabi = %u\nlibrary = "
	        "\"parser.dll\"\n"
	        "entrypoint = \"tree_sitter_%s\"\nextensions = [\".json\", "
	        "\".geojson\", "
	        "\".webmanifest\"]\nquery = \"highlights.scm\"\nrelease = "
	        "\"1.0.0\"\n%s",
	        id, abi, id, extra ? extra : "");

	CHECK(write_file(path, text, (size_t)n));
}

/* grammars\json\{parser.dll, highlights.scm, manifest.toml} under `where`, the
 * DLL copied from `dll_src`; the table says what these files are. */
static void make_grammar(Trust *tr,
        const wchar_t *where,
        const wchar_t *dll_src,
        const char *query,
        unsigned abi)
{
	wchar_t dir[MAX_PATH + 80], p[MAX_PATH + 80];

	join(dir, MAX_PATH + 80, where, L"grammars");
	join(p, MAX_PATH + 80, dir, L"json");
	wcscpy_s(dir, MAX_PATH + 80, p);
	CHECK(mkdirs(dir));
	join(p, MAX_PATH + 80, dir, L"parser.dll");
	CHECK(copy_file(dll_src, p));
	hex_of(p, tr->dll);
	join(p, MAX_PATH + 80, dir, L"highlights.scm");
	CHECK(write_file(p, query, strlen(query)));
	hex_of(p, tr->query);
	join(p, MAX_PATH + 80, dir, L"manifest.toml");
	make_manifest(p, abi, "json", NULL);
	hex_of(p, tr->manifest);
	tr->t.id = "json";
	tr->t.dll = tr->dll;
	tr->t.query = tr->query;
	tr->t.manifest = tr->manifest;
}

static void real_dll(wchar_t *out, const wchar_t *id)
{
	swprintf(out, MAX_PATH + 80, L"%ls\\grammars\\%ls\\parser.dll", built,
	        id);
}

static void fixture_dll(wchar_t *out, const wchar_t *name)
{
	swprintf(out, MAX_PATH + 80, L"%ls\\%ls", fixtures, name);
}

static uint32_t open_json(const WfSyntaxApi *api, WfSynSession **s)
{
	*s = NULL;
	return api->open("json", NULL, s);
}

static const char JSON_TEXT[] = "{\"a\": 1, \"b\": \"x\"}\n";

static bool colours_json(const WfSyntaxApi *api, WfSynSession *s)
{
	WfSynResult r;
	bool ok = WF_SYN_OK ==
	                api->highlight(
	                        s, JSON_TEXT, sizeof JSON_TEXT - 1, &r) &&
	        WF_SC_NUMBER == wf_syn_runs_class_at(r.runs, r.nruns, 6) &&
	        WF_SC_STRING == wf_syn_runs_class_at(r.runs, r.nruns, 14);

	api->result_free(&r);
	return ok;
}

/* A fresh folder for one case; the loader is told about it and its table. */
static void begin(const wchar_t *name, wchar_t *dir)
{
	join(dir, MAX_PATH + 80, root, name);
	remove_tree(dir);
	CHECK(mkdirs(dir));
}

static void use(const wchar_t *dir, Trust *tr)
{
	wf_syncore_api()->shutdown();
	wf_syncore_configure(dir, tr ? &tr->t : NULL, tr ? 1 : 0);
}

static void a_grammar_that_is_not_there(void)
{
	const WfSyntaxApi *api = wf_syncore_api();
	wchar_t d[MAX_PATH + 80], jreal[MAX_PATH + 80];
	WfSynSession *s;
	Trust tr;

	begin(L"none", d);
	real_dll(jreal, L"json");
	make_grammar(&tr, d, jreal, GOOD_QUERY, 14);
	remove_tree(d);
	CHECK(mkdirs(d));
	use(d, &tr);
	CHECK(WF_SYN_E_NOGRAMMAR == open_json(api, &s) && !s);
	CHECK(0 == api->stat(WF_SYN_STAT_LOADED));
	/* a language the table does not list is not a thing that can be named */
	CHECK(WF_SYN_E_TRUST == api->open("yaml", NULL, &s) && !s);
	/* and when the files appear, the next open finds them: "not installed" is
	 * not remembered */
	make_grammar(&tr, d, jreal, GOOD_QUERY,
	        14); /* the same files, so the same table */
	CHECK(WF_SYN_OK == open_json(api, &s));
	CHECK(colours_json(api, s));
	api->close(s);
	api->shutdown();
}

static void one_grammar_several_times(void)
{
	const WfSyntaxApi *api = wf_syncore_api();
	wchar_t d[MAX_PATH + 80], jreal[MAX_PATH + 80];
	WfSynSession *a, *b, *c;
	Trust tr;

	begin(L"one", d);
	real_dll(jreal, L"json");
	make_grammar(&tr, d, jreal, GOOD_QUERY, 14);
	use(d, &tr);
	CHECK(WF_SYN_OK == open_json(api, &a));
	CHECK(WF_SYN_OK == open_json(api, &b));
	CHECK(WF_SYN_OK == open_json(api, &c));
	CHECK(1 ==
	        api->stat(WF_SYN_STAT_LOADED)); /* one DLL for three sessions */
	CHECK(3 == api->stat(WF_SYN_STAT_SESSIONS));
	CHECK(colours_json(api, a) && colours_json(api, b) &&
	        colours_json(api, c));
	api->close(a);
	api->close(b);
	api->close(c);
	CHECK(0 == api->stat(WF_SYN_STAT_SESSIONS));
	CHECK(1 == api->stat(WF_SYN_STAT_LOADED)); /* kept until shutdown */
	api->shutdown();
	CHECK(0 == api->stat(WF_SYN_STAT_LOADED));
}

/* The same text twice parses once; a change parses again from the old tree. */
static void a_text_is_parsed_when_it_changes_and_not_otherwise(void)
{
	const WfSyntaxApi *api = wf_syncore_api();
	wchar_t d[MAX_PATH + 80], jreal[MAX_PATH + 80];
	WfSynSession *s;
	WfSynResult r;
	Trust tr;
	uint64_t before;
	static const char B[] = "{\"a\": 12, \"b\": \"x\"}\n";

	begin(L"parse", d);
	real_dll(jreal, L"json");
	make_grammar(&tr, d, jreal, GOOD_QUERY, 14);
	use(d, &tr);
	CHECK(WF_SYN_OK == open_json(api, &s));
	before = api->stat(WF_SYN_STAT_PARSES);
	CHECK(WF_SYN_OK ==
	        api->highlight(s, JSON_TEXT, sizeof JSON_TEXT - 1, &r));
	CHECK(0 == r.incremental &&
	        before + 1 == api->stat(WF_SYN_STAT_PARSES));
	api->result_free(&r);
	CHECK(WF_SYN_OK ==
	        api->highlight(s, JSON_TEXT, sizeof JSON_TEXT - 1, &r));
	CHECK(1 == r.incremental &&
	        before + 1 == api->stat(WF_SYN_STAT_PARSES));
	api->result_free(&r);
	CHECK(WF_SYN_OK == api->highlight(s, B, sizeof B - 1, &r));
	CHECK(1 == r.incremental &&
	        before + 2 == api->stat(WF_SYN_STAT_PARSES));
	CHECK(WF_SC_NUMBER == wf_syn_runs_class_at(r.runs, r.nruns, 7));
	api->result_free(&r);
	api->close(s);
	api->shutdown();
}

/* A text over a limit is an answer; the next text works again. */
static void a_text_over_a_limit_is_refused_and_the_session_goes_on(void)
{
	const WfSyntaxApi *api = wf_syncore_api();
	wchar_t d[MAX_PATH + 80], jreal[MAX_PATH + 80];
	WfSynSession *s;
	WfSynResult r;
	Trust tr;
	char *big = malloc(3u * 1024u * 1024u);

	CHECK(big);
	begin(L"limit", d);
	real_dll(jreal, L"json");
	make_grammar(&tr, d, jreal, GOOD_QUERY, 14);
	use(d, &tr);
	CHECK(WF_SYN_OK == open_json(api, &s));
	if (big) {
		for (size_t i = 0; i < 3u * 1024u * 1024u; i++) {
			big[i] = 99 == i % 100
			        ? '\n'
			        : ' '; /* short lines: only the size is over */
		}
		CHECK(WF_SYN_E_LIMIT ==
		        api->highlight(s, big, 3u * 1024u * 1024u, &r));
		memset(big, 'a', 70000);
		CHECK(WF_SYN_E_LIMIT ==
		        api->highlight(s, big, 70000, &r)); /* one long line */
		free(big);
	}
	CHECK(colours_json(api, s));
	api->close(s);
	api->shutdown();
}

static void files_that_are_not_the_official_ones(void)
{
	const WfSyntaxApi *api = wf_syncore_api();
	wchar_t d[MAX_PATH + 80], jreal[MAX_PATH + 80], p[MAX_PATH + 80];
	WfSynSession *s;
	Trust tr;
	static const wchar_t *const names[] = {
	        L"parser.dll", L"highlights.scm", L"manifest.toml"};

	real_dll(jreal, L"json");
	for (int k = 0; k < 3; k++) {
		char *b;
		size_t n;
		wchar_t jd[MAX_PATH + 80];

		begin(L"tamper", d);
		make_grammar(&tr, d, jreal, GOOD_QUERY, 14);
		join(jd, MAX_PATH + 80, d, L"grammars\\json");
		join(p, MAX_PATH + 80, jd, names[k]);
		b = read_file(p, &n);
		CHECK(b && n > 20);
		if (b) {
			b[n / 2] = (char)(b[n / 2] ^ 0x20); /* one byte */
			CHECK(write_file(p, b, n));
			free(b);
		}
		use(d, &tr);
		CHECK(WF_SYN_E_TRUST == open_json(api, &s) && !s);
		CHECK(0 == api->stat(WF_SYN_STAT_LOADED)); /* never loaded */
		/* the refusal is kept for the life of the process: putting the
		 * official file back is for the next start */
		make_grammar(&tr, d, jreal, GOOD_QUERY, 14);
		CHECK(WF_SYN_E_TRUST == open_json(api, &s));
		use(d, &tr); /* "the next start" */
		CHECK(WF_SYN_OK == open_json(api, &s));
		api->close(s);
		api->shutdown();
	}
	/* a table that does not list the grammar at all */
	begin(L"tamper", d);
	make_grammar(&tr, d, jreal, GOOD_QUERY, 14);
	use(d, NULL);
	CHECK(WF_SYN_E_TRUST == open_json(api, &s));
	/* a table with a hash that is not a hash */
	tr.t.dll = "zz";
	use(d, &tr);
	CHECK(WF_SYN_E_TRUST == open_json(api, &s));
	api->shutdown();
}

static void a_dll_that_is_not_a_dll_or_not_a_grammar(void)
{
	const WfSyntaxApi *api = wf_syncore_api();
	wchar_t d[MAX_PATH + 80], p[MAX_PATH + 80], junk[MAX_PATH + 80],
	        yreal[MAX_PATH + 80], jreal[MAX_PATH + 80];
	WfSynSession *s;
	Trust tr;
	char text[4096];

	real_dll(jreal, L"json");
	real_dll(yreal, L"yaml");
	/* a text file called parser.dll (its hash is in the table) */
	begin(L"junk", d);
	join(junk, MAX_PATH + 80, d, L"junk.bin");
	memset(text, 'x', sizeof text);
	CHECK(write_file(junk, text, sizeof text));
	make_grammar(&tr, d, junk, GOOD_QUERY, 14);
	use(d, &tr);
	CHECK(WF_SYN_E_LOAD == open_json(api, &s) && !s);
	CHECK(0 == api->stat(WF_SYN_STAT_LOADED));
	/* a real DLL cut short */
	{
		size_t n;
		char *b = read_file(jreal, &n);

		CHECK(b);
		join(junk, MAX_PATH + 80, d, L"cut.bin");
		CHECK(b && write_file(junk, b, 1000));
		free(b);
	}
	make_grammar(&tr, d, junk, GOOD_QUERY, 14);
	use(d, &tr);
	CHECK(WF_SYN_E_LOAD == open_json(api, &s));
	/* another grammar's DLL where JSON's belongs: no tree_sitter_json in it */
	make_grammar(&tr, d, yreal, GOOD_QUERY, 14);
	use(d, &tr);
	CHECK(WF_SYN_E_SYMBOL == open_json(api, &s) && !s);
	CHECK(0 == api->stat(WF_SYN_STAT_LOADED)); /* and it was let go again */
	(void)p;
	api->shutdown();
}

static void fixtures_with_the_wrong_abi_or_no_language(void)
{
	static const struct
	{
		const wchar_t *dll;
		uint32_t want;
		unsigned man_abi; /* what the manifest says */
	} cases[] = {{L"fixture_abi_new.dll", WF_SYN_E_ABI, 14},
	        {L"fixture_abi_new.dll", WF_SYN_E_ABI,
	                99}, /* it agrees: still too new */
	        {L"fixture_abi_old.dll", WF_SYN_E_ABI, 14},
	        {L"fixture_abi_old.dll", WF_SYN_E_ABI,
	                12}, /* it agrees: still too old */
	        {L"fixture_null.dll", WF_SYN_E_SYMBOL, 14}};
	const WfSyntaxApi *api = wf_syncore_api();

	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		wchar_t d[MAX_PATH + 80], dll[MAX_PATH + 80];
		WfSynSession *s;
		Trust tr;

		begin(L"abi", d);
		fixture_dll(dll, cases[i].dll);
		make_grammar(&tr, d, dll, GOOD_QUERY, cases[i].man_abi);
		use(d, &tr);
		CHECK(cases[i].want == open_json(api, &s) && !s);
		CHECK(0 == api->stat(WF_SYN_STAT_LOADED));
		CHECK(0 == api->stat(WF_SYN_STAT_SESSIONS));
		api->shutdown();
	}
	/* the real grammar, but the manifest says another ABI than the DLL has */
	{
		wchar_t d[MAX_PATH + 80], jreal[MAX_PATH + 80],
		        p[MAX_PATH + 80];
		WfSynSession *s;
		Trust tr;

		begin(L"abi", d);
		real_dll(jreal, L"json");
		make_grammar(&tr, d, jreal, GOOD_QUERY, 15); /* it is 14 */
		use(d, &tr);
		CHECK(WF_SYN_E_ABI == open_json(api, &s));
		make_grammar(
		        &tr, d, jreal, GOOD_QUERY, 12); /* below what is read */
		use(d, &tr);
		CHECK(WF_SYN_E_ABI == open_json(api, &s));
		(void)p;
		api->shutdown();
	}
}

static void bad_manifests_and_queries(void)
{
	const WfSyntaxApi *api = wf_syncore_api();
	wchar_t d[MAX_PATH + 80], jreal[MAX_PATH + 80], p[MAX_PATH + 80];
	WfSynSession *s;
	Trust tr;
	static const char *const bad_manifest[] = {
	        "id = \"json\"\n",    /* missing keys */
	        "this is not toml\n", /* syntax */
	        "id = "
	        "\"yaml\"\nversion=\"1\"\nabi="
	        "14\nlibrary=\"parser.dll\"\n"
	        "entrypoint=\"tree_sitter_"
	        "yaml\"\nextensions=[\".yml\"]\n"
	        "query=\"highlights.scm\"\nrelease="
	        "\"1\"\n",                        /* another language */
	        "id = \"json\"\nid = \"json\"\n", /* twice */
	        "id=\"json\"\nversion=\"1\"\nabi="
	        "14\nlibrary=\"..\\\\parser.dll\"\n"
	        "entrypoint=\"tree_sitter_"
	        "json\"\nextensions=[\".json\"]\n"
	        "query=\"highlights.scm\"\nrelease="
	        "\"1\"\n", /* a path as a name */
	        "id=\"json\"\nversion=\"1\"\nabi="
	        "14\nlibrary=\"evil.dll\"\n"
	        "entrypoint=\"tree_sitter_"
	        "json\"\nextensions=[\".json\"]\n"
	        "query=\"highlights.scm\"\nrelease="
	        "\"1\"\n", /* another DLL name */
	        "id=\"json\"\nversion=\"1\"\nabi="
	        "14\nlibrary=\"parser.dll\"\n"
	        "entrypoint=\"system\"\nextensions=[\"."
	        "json\"]\n"
	        "query=\"highlights.scm\"\nrelease="
	        "\"1\"\n", /* another export */
	        "id=\"json\"\nversion=\"1\"\nabi="
	        "14\nlibrary=\"parser.dll\"\n"
	        "entrypoint=\"tree_sitter_"
	        "json\"\nextensions=[\".exe\"]\n"
	        "query=\"highlights.scm\"\nrelease="
	        "\"1\"\n", /* an extension the
		              table does not give */
	};
	static const char *const bad_query[] = {
	        "(string @string\n",        /* not balanced */
	        "(no_such_node) @string\n", /* a node the grammar has not */
	        "(string) @string @@\n",    /* nonsense */
	        "(string) @string (#eq? @string", /* cut off */
	};

	real_dll(jreal, L"json");
	for (size_t i = 0; i < sizeof bad_manifest / sizeof bad_manifest[0];
	        i++) {
		begin(L"manifest", d);
		make_grammar(&tr, d, jreal, GOOD_QUERY, 14);
		join(p, MAX_PATH + 80, d, L"grammars\\json\\manifest.toml");
		CHECK(write_file(p, bad_manifest[i], strlen(bad_manifest[i])));
		hex_of(p,
		        tr.manifest); /* the table vouches for it: it is the text that is bad */
		use(d, &tr);
		CHECK(WF_SYN_E_MANIFEST == open_json(api, &s) && !s);
		CHECK(0 == api->stat(WF_SYN_STAT_LOADED));
		api->shutdown();
	}
	for (size_t i = 0; i < sizeof bad_query / sizeof bad_query[0]; i++) {
		begin(L"query", d);
		make_grammar(&tr, d, jreal, bad_query[i], 14);
		use(d, &tr);
		CHECK(WF_SYN_E_QUERY == open_json(api, &s) && !s);
		CHECK(0 ==
		        api->stat(WF_SYN_STAT_LOADED)); /* the DLL was let go */
		api->shutdown();
	}
	/* an empty query is a query: nothing is coloured, nothing breaks */
	begin(L"query", d);
	make_grammar(&tr, d, jreal, "; nothing\n", 14);
	use(d, &tr);
	CHECK(WF_SYN_OK == open_json(api, &s));
	{
		WfSynResult r;

		CHECK(WF_SYN_OK ==
		        api->highlight(s, JSON_TEXT, sizeof JSON_TEXT - 1, &r));
		CHECK(0 == r.nruns);
		api->result_free(&r);
	}
	api->close(s);
	/* a capture the class table does not know paints nothing */
	api->shutdown();
	begin(L"query", d);
	make_grammar(&tr, d, jreal,
	        "(string) @nothing.at.all\n(number) @number\n", 14);
	use(d, &tr);
	CHECK(WF_SYN_OK == open_json(api, &s));
	{
		WfSynResult r;

		CHECK(WF_SYN_OK ==
		        api->highlight(s, JSON_TEXT, sizeof JSON_TEXT - 1, &r));
		CHECK(1 == r.nruns && WF_SC_NUMBER == r.runs[0].cls);
		api->result_free(&r);
	}
	api->close(s);
	api->shutdown();
}

static void dependencies_are_looked_for_in_the_right_places(void)
{
	const WfSyntaxApi *api = wf_syncore_api();
	wchar_t d[MAX_PATH + 80], dll[MAX_PATH + 80], dep[MAX_PATH + 80],
	        elsewhere[MAX_PATH + 80], p[MAX_PATH + 80], cwd[MAX_PATH + 80];
	WfSynSession *s;
	Trust tr;

	fixture_dll(dll, L"fixture_dep_abi_new.dll");
	fixture_dll(dep, L"fixture_dependency.dll");
	if (INVALID_FILE_ATTRIBUTES == GetFileAttributesW(dll) ||
	        INVALID_FILE_ATTRIBUTES == GetFileAttributesW(dep)) {
		CHECK_FAIL("the dependency fixtures were not built");
		return;
	}
	GetCurrentDirectoryW(MAX_PATH + 80, cwd);

	/* the library it needs is nowhere: the loader refuses */
	begin(L"dep", d);
	make_grammar(&tr, d, dll, GOOD_QUERY, 14);
	use(d, &tr);
	CHECK(WF_SYN_E_LOAD == open_json(api, &s) && !s);

	/* in the current directory: not searched */
	join(elsewhere, MAX_PATH + 80, d, L"cwd");
	CHECK(mkdirs(elsewhere));
	join(p, MAX_PATH + 80, elsewhere, L"fixture_dependency.dll");
	CHECK(copy_file(dep, p));
	CHECK(SetCurrentDirectoryW(elsewhere));
	use(d, &tr);
	CHECK(WF_SYN_E_LOAD == open_json(api, &s));
	SetCurrentDirectoryW(cwd);

	/* in a folder on PATH: not searched */
	{
		wchar_t old[8192], now[8192];

		GetEnvironmentVariableW(L"PATH", old, 8192);
		swprintf(now, 8192, L"%ls;%ls", elsewhere, old);
		SetEnvironmentVariableW(L"PATH", now);
		use(d, &tr);
		CHECK(WF_SYN_E_LOAD == open_json(api, &s));
		SetEnvironmentVariableW(L"PATH", old);
	}

	/* beside the grammar DLL itself: found (the DLL then says its language is of
	 * a newer ABI, so it is refused for that, not for the load) */
	join(p, MAX_PATH + 80, d, L"grammars\\json\\fixture_dependency.dll");
	CHECK(copy_file(dep, p));
	use(d, &tr);
	CHECK(WF_SYN_E_ABI == open_json(api, &s));
	api->shutdown();
}

static void reparse_points_are_not_followed(void)
{
	const WfSyntaxApi *api = wf_syncore_api();
	wchar_t d[MAX_PATH + 80], real[MAX_PATH + 80], link[MAX_PATH + 80],
	        jreal[MAX_PATH + 80], tmp[MAX_PATH + 80];
	WfSynSession *s;
	Trust tr;
	HANDLE h;
	BYTE buf[2048];
	wchar_t sub[MAX_PATH + 12];
	USHORT sb, pb, data;
	DWORD got;

	real_dll(jreal, L"json");
	/* the grammars folder is a junction to a real one: refused */
	begin(L"reparse", d);
	join(real, MAX_PATH + 80, d, L"elsewhere");
	CHECK(mkdirs(real));
	make_grammar(&tr, real, jreal, GOOD_QUERY, 14);
	join(tmp, MAX_PATH + 80, d, L"install");
	CHECK(mkdirs(tmp));
	join(link, MAX_PATH + 80, tmp, L"grammars");
	CreateDirectoryW(link, NULL);
	h = CreateFileW(link, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	if (INVALID_HANDLE_VALUE == h) {
		skip("junction: could not open the folder");
		return;
	}
	{
		wchar_t target[MAX_PATH + 80];

		join(target, MAX_PATH + 80, real, L"grammars");
		swprintf(sub, MAX_PATH + 12, L"\\??\\%ls", target);
		sb = (USHORT)(wcslen(sub) * 2);
		pb = (USHORT)(wcslen(target) * 2);
		memset(buf, 0, sizeof buf);
		data = (USHORT)(8 + sb + 2 + pb + 2);
		*(ULONG *)buf = IO_REPARSE_TAG_MOUNT_POINT;
		*(USHORT *)(buf + 4) = data;
		*(USHORT *)(buf + 10) = sb;
		*(USHORT *)(buf + 12) = (USHORT)(sb + 2);
		*(USHORT *)(buf + 14) = pb;
		memcpy(buf + 16, sub, sb);
		memcpy(buf + 16 + sb + 2, target, pb);
		if (!DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf,
		            (DWORD)(8u + data), NULL, 0, &got, NULL)) {
			CloseHandle(h);
			skip("junction: could not be made");
			return;
		}
		CloseHandle(h);
	}
	{
		wchar_t via[MAX_PATH + 80];

		join(via, MAX_PATH + 80, link, L"json\\parser.dll");
		CHECK(wf_dll_present(
		        via)); /* it is there, through the junction */
	}
	use(tmp, &tr);
	CHECK(WF_SYN_E_PATH == open_json(api, &s) && !s);
	CHECK(0 == api->stat(WF_SYN_STAT_LOADED));
	/* the same files, not behind a junction: fine */
	use(real, &tr);
	CHECK(WF_SYN_OK == open_json(api, &s));
	api->close(s);
	api->shutdown();
	RemoveDirectoryW(link); /* the junction itself, not what it points to */
}

static void a_file_that_is_loaded_cannot_be_changed(void)
{
	const WfSyntaxApi *api = wf_syncore_api();
	wchar_t d[MAX_PATH + 80], jreal[MAX_PATH + 80], p[MAX_PATH + 80],
	        moved[MAX_PATH + 80];
	WfSynSession *s;
	Trust tr;

	begin(L"held", d);
	real_dll(jreal, L"json");
	make_grammar(&tr, d, jreal, GOOD_QUERY, 14);
	use(d, &tr);
	CHECK(WF_SYN_OK == open_json(api, &s));
	join(p, MAX_PATH + 80, d, L"grammars\\json\\parser.dll");
	join(moved, MAX_PATH + 80, d, L"grammars\\json\\moved.dll");
	/* loaded: it cannot be replaced, deleted or renamed under the process */
	CHECK(INVALID_HANDLE_VALUE ==
	        CreateFileW(p, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
	                FILE_ATTRIBUTE_NORMAL, NULL));
	CHECK(!DeleteFileW(p));
	CHECK(!copy_file(jreal, p));
	/* Windows lets a mapped image be renamed (it cannot be deleted or written);
	 * the loaded copy is the one that was hashed, so a rename changes nothing */
	CHECK(MoveFileW(p, moved));
	CHECK(colours_json(api, s));
	CHECK(MoveFileW(moved, p));
	api->close(s);
	api->shutdown();
	/* released: now it can */
	CHECK(DeleteFileW(p));
}

static void while_it_is_held_nobody_can_swap_it(void)
{
	wchar_t d[MAX_PATH + 80], p[MAX_PATH + 80], moved[MAX_PATH + 80];
	WfDllHold hold;
	uint8_t other[32] = {0};
	char text[] = "abc";

	begin(L"hold", d);
	join(p, MAX_PATH + 80, d, L"f.bin");
	join(moved, MAX_PATH + 80, d, L"g.bin");
	CHECK(write_file(p, text, 3));
	/* the digest is wrong: nothing is held */
	CHECK(WF_SYN_E_TRUST == wf_dll_hold(p, 1024, other, &hold));
	CHECK(INVALID_HANDLE_VALUE == hold.h);
	CHECK(write_file(p, text, 3)); /* so it was released */
	/* a file over the limit, a folder, a missing file */
	CHECK(WF_SYN_E_PATH == wf_dll_hold(p, 2, NULL, &hold));
	CHECK(WF_SYN_E_PATH == wf_dll_hold(d, 1024, NULL, &hold));
	{
		wchar_t none[MAX_PATH + 80];

		join(none, MAX_PATH + 80, d, L"none.bin");
		CHECK(WF_SYN_E_NOGRAMMAR ==
		        wf_dll_hold(none, 1024, NULL, &hold));
	}
	/* held: the bytes cannot change, the file cannot go or be renamed */
	{
		uint8_t want[32];

		CHECK(wf_dll_sha256("abc", 3, want));
		CHECK(WF_SYN_OK == wf_dll_hold(p, 1024, want, &hold));
		CHECK(INVALID_HANDLE_VALUE ==
		        CreateFileW(p, GENERIC_WRITE,
		                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
		                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL));
		CHECK(!write_file(p, "xyz", 3));
		CHECK(!DeleteFileW(p));
		CHECK(!MoveFileW(p, moved));
		{
			char *back = NULL;
			size_t n = 0;

			CHECK(WF_SYN_OK == wf_dll_hold_read(&hold, &back, &n));
			CHECK(3 == n && !memcmp(back, "abc", 3));
			free(back);
		}
		wf_dll_release(&hold);
		wf_dll_release(&hold);      /* twice is fine */
		CHECK(MoveFileW(p, moved)); /* and now it can be moved */
	}
}

static void paths_are_checked(void)
{
	wchar_t out[MAX_PATH + 80];
	const wchar_t *one[] = {L"syntax"};
	const wchar_t *dots[] = {L".."};
	const wchar_t *slash[] = {L"a\\b"};
	const wchar_t *fwd[] = {L"a/b"};
	const wchar_t *colon[] = {L"c:x"};
	const wchar_t *empty[] = {L""};
	const wchar_t *hidden[] = {L".git"};
	const wchar_t *dd[] = {L"a..b"};
	const wchar_t *space[] = {L"a b"};
	wchar_t longpart[100];
	const wchar_t *lp[1];

	for (int i = 0; i < 99; i++) {
		longpart[i] = L'a';
	}
	longpart[99] = L'\0';
	lp[0] = longpart;
	/* a good root and part */
	CHECK(WF_SYN_OK == wf_dll_resolve(root, one, 1, out, MAX_PATH + 80));
	/* a root that is not a plain drive path */
	CHECK(WF_SYN_E_PATH ==
	        wf_dll_resolve(L"syntax", one, 1, out, MAX_PATH + 80));
	CHECK(WF_SYN_E_PATH ==
	        wf_dll_resolve(
	                L"\\\\server\\share\\x", one, 1, out, MAX_PATH + 80));
	CHECK(WF_SYN_E_PATH ==
	        wf_dll_resolve(L"\\\\?\\C:\\x", one, 1, out, MAX_PATH + 80));
	CHECK(WF_SYN_E_PATH ==
	        wf_dll_resolve(L"\\\\?\\UNC\\server\\share", one, 1, out,
	                MAX_PATH + 80));
	CHECK(WF_SYN_E_PATH ==
	        wf_dll_resolve(L"C:", one, 1, out, MAX_PATH + 80));
	CHECK(WF_SYN_E_PATH ==
	        wf_dll_resolve(L"C:x\\y", one, 1, out, MAX_PATH + 80));
	CHECK(WF_SYN_E_PATH == wf_dll_resolve(L"", one, 1, out, MAX_PATH + 80));
	CHECK(WF_SYN_E_PATH ==
	        wf_dll_resolve(L".\\x", one, 1, out, MAX_PATH + 80));
	/* parts that are not plain names */
	CHECK(WF_SYN_E_PATH ==
	        wf_dll_resolve(root, dots, 1, out, MAX_PATH + 80));
	CHECK(WF_SYN_E_PATH ==
	        wf_dll_resolve(root, slash, 1, out, MAX_PATH + 80));
	CHECK(WF_SYN_E_PATH ==
	        wf_dll_resolve(root, fwd, 1, out, MAX_PATH + 80));
	CHECK(WF_SYN_E_PATH ==
	        wf_dll_resolve(root, colon, 1, out, MAX_PATH + 80));
	CHECK(WF_SYN_E_PATH ==
	        wf_dll_resolve(root, empty, 1, out, MAX_PATH + 80));
	CHECK(WF_SYN_E_PATH ==
	        wf_dll_resolve(root, hidden, 1, out, MAX_PATH + 80));
	CHECK(WF_SYN_E_PATH == wf_dll_resolve(root, dd, 1, out, MAX_PATH + 80));
	CHECK(WF_SYN_E_PATH ==
	        wf_dll_resolve(root, space, 1, out, MAX_PATH + 80));
	CHECK(WF_SYN_E_PATH == wf_dll_resolve(root, lp, 1, out, MAX_PATH + 80));
	/* arguments */
	CHECK(WF_SYN_E_BADARG ==
	        wf_dll_resolve(NULL, one, 1, out, MAX_PATH + 80));
	CHECK(WF_SYN_E_BADARG ==
	        wf_dll_resolve(root, NULL, 1, out, MAX_PATH + 80));
	CHECK(WF_SYN_E_BADARG ==
	        wf_dll_resolve(root, one, 0, out, MAX_PATH + 80));
	/* an output that is too small */
	CHECK(WF_SYN_E_PATH == wf_dll_resolve(root, one, 1, out, 10));
	/* the loader refuses a relative path, whatever the file is */
	{
		HMODULE m = NULL;

		CHECK(WF_SYN_E_BADARG == wf_dll_load(L"parser.dll", &m) && !m);
		CHECK(WF_SYN_E_BADARG == wf_dll_load(L".\\parser.dll", &m));
		CHECK(WF_SYN_E_BADARG == wf_dll_load(L"\\parser.dll", &m));
		CHECK(WF_SYN_E_BADARG == wf_dll_load(NULL, &m));
	}
	/* the directory of this program is a real path on a fixed drive */
	{
		wchar_t exe[MAX_PATH + 80];

		CHECK(WF_SYN_OK == wf_dll_exe_dir(exe, MAX_PATH + 80));
		CHECK(exe[1] == L':' && exe[2] == L'\\');
		CHECK(WF_SYN_OK ==
		        wf_dll_module_dir((const void *)paths_are_checked, exe,
		                MAX_PATH + 80));
	}
	CHECK(0 == wf_dll_hex32("zz", (uint8_t[32]){0}));
	CHECK(0 == wf_dll_hex32(NULL, (uint8_t[32]){0}));
	CHECK(0 ==
	        wf_dll_hex32("0123456789abcdef0123456789abcdef0123456789abcdef0"
	                     "123456789abcdeg",
	                (uint8_t[32]){0}));
	CHECK(1 ==
	        wf_dll_hex32("0123456789ABCDEF0123456789abcdef0123456789abcdef0"
	                     "123456789abcdef",
	                (uint8_t[32]){0}));
}

static void the_host_looks_only_where_it_was_told(void)
{
	wchar_t d[MAX_PATH + 80], sd[MAX_PATH + 80], p[MAX_PATH + 80],
	        real[MAX_PATH + 80], cwd[MAX_PATH + 80], decoy[MAX_PATH + 80];
	char pin[65];
	WfSynSession *s = NULL;
	const WfSynLang *json = wf_synlang_by_id("json");

	begin(L"host", d);
	swprintf(real, MAX_PATH + 80, L"%ls\\piero-syntax.dll", built);
	hex_of(real, pin);
	/* nothing installed under the root */
	wf_synhost_configure(d, pin);
	CHECK(!wf_synhost_installed(json));
	CHECK(WF_SYN_E_NOGRAMMAR == wf_synhost_open("json", NULL, &s) && !s);
	/* a copy of the DLL in the current directory is not what is meant */
	GetCurrentDirectoryW(MAX_PATH + 80, cwd);
	join(decoy, MAX_PATH + 80, d, L"decoy");
	CHECK(mkdirs(decoy));
	join(p, MAX_PATH + 80, decoy, L"piero-syntax.dll");
	CHECK(copy_file(real, p));
	CHECK(SetCurrentDirectoryW(decoy));
	CHECK(WF_SYN_E_NOGRAMMAR == wf_synhost_open("json", NULL, &s));
	CHECK(NULL == GetModuleHandleW(L"piero-syntax.dll"));
	SetCurrentDirectoryW(cwd);
	/* installed as it should be: syntax\piero-syntax.dll + the grammar */
	join(sd, MAX_PATH + 80, d, L"syntax");
	CHECK(mkdirs(sd));
	join(p, MAX_PATH + 80, sd, L"piero-syntax.dll");
	CHECK(copy_file(real, p));
	CHECK(!wf_synhost_installed(
	        json)); /* the grammar folder is not there yet */
	{
		wchar_t g[MAX_PATH + 80], from[MAX_PATH + 80];
		static const wchar_t *const files[] = {
		        L"parser.dll", L"highlights.scm", L"manifest.toml"};

		join(g, MAX_PATH + 80, sd, L"grammars\\json");
		CHECK(mkdirs(g));
		for (int i = 0; i < 3; i++) {
			wchar_t to[MAX_PATH + 80];

			swprintf(from, MAX_PATH + 80,
			        L"%ls\\grammars\\json\\%ls", built, files[i]);
			join(to, MAX_PATH + 80, g, files[i]);
			CHECK(copy_file(from, to));
		}
	}
	CHECK(wf_synhost_installed(json));
	CHECK(!wf_synhost_installed(wf_synlang_by_id("yaml")));
	CHECK(wf_synhost_installed(
	        wf_synlang_by_id("dotenv"))); /* needs no DLL */
	CHECK(NULL ==
	        GetModuleHandleW(
	                L"piero-syntax.dll")); /* looking loaded nothing */
	CHECK(WF_SYN_OK == wf_synhost_open("json", NULL, &s));
	{
		wchar_t at[MAX_PATH + 80];
		HMODULE m = GetModuleHandleW(L"piero-syntax.dll");

		CHECK(m && GetModuleFileNameW(m, at, MAX_PATH + 80));
		CHECK(NULL != wcsstr(at, L"\\host\\syntax\\piero-syntax.dll"));
	}
	wf_synhost_close(s);
	wf_synhost_shutdown();
	CHECK(NULL == GetModuleHandleW(L"piero-syntax.dll"));
	/* the DLL changed by one byte: refused by the pin, never loaded */
	{
		char *b;
		size_t n;

		b = read_file(p, &n);
		CHECK(b);
		b[n / 3] = (char)(b[n / 3] ^ 0x40);
		CHECK(write_file(p, b, n));
		free(b);
	}
	wf_synhost_configure(d, pin);
	{
		uint32_t dbg = wf_synhost_open("json", NULL, &s);

		printf("DBG1 %s\n", wf_syn_status_name(dbg));
		CHECK(WF_SYN_E_TRUST == dbg && !s);
	}
	CHECK(NULL == GetModuleHandleW(L"piero-syntax.dll"));
	/* the refusal is kept (not hashed again each time) until it is configured again */
	CHECK(copy_file(real, p));
	CHECK(WF_SYN_E_TRUST == wf_synhost_open("json", NULL, &s));
	wf_synhost_configure(d, pin);
	CHECK(WF_SYN_OK == wf_synhost_open("json", NULL, &s));
	wf_synhost_close(s);
	wf_synhost_shutdown();
	/* a DLL that is not a DLL, vouched for by the pin */
	{
		char junk[2048];
		char pin2[65];

		memset(junk, 'x', sizeof junk);
		CHECK(write_file(p, junk, sizeof junk));
		hex_of(p, pin2);
		wf_synhost_configure(d, pin2);
		CHECK(WF_SYN_E_LOAD == wf_synhost_open("json", NULL, &s) && !s);
	}
	wf_synhost_configure(NULL, NULL);
}

int main(void)
{
	wchar_t tmp[MAX_PATH], exe[MAX_PATH + 80];

	GetTempPathW(MAX_PATH, tmp);
	swprintf(root, MAX_PATH + 80, L"%lswfsyn_%lu", tmp,
	        (unsigned long)GetCurrentProcessId());
	CHECK(mkdirs(root));
	CHECK(WF_SYN_OK == wf_dll_exe_dir(exe, MAX_PATH + 80));
	swprintf(built, MAX_PATH + 80, L"%ls\\syntax", exe);
	swprintf(fixtures, MAX_PATH + 80, L"%ls\\syntax_fixtures", exe);

	paths_are_checked();
	while_it_is_held_nobody_can_swap_it();
	a_grammar_that_is_not_there();
	one_grammar_several_times();
	a_text_is_parsed_when_it_changes_and_not_otherwise();
	a_text_over_a_limit_is_refused_and_the_session_goes_on();
	files_that_are_not_the_official_ones();
	a_dll_that_is_not_a_dll_or_not_a_grammar();
	fixtures_with_the_wrong_abi_or_no_language();
	bad_manifests_and_queries();
	dependencies_are_looked_for_in_the_right_places();
	reparse_points_are_not_followed();
	a_file_that_is_loaded_cannot_be_changed();
	the_host_looks_only_where_it_was_told();

	wf_syncore_api()->shutdown();
	remove_tree(root);
	if (wf_test_failures) {
		printf("test_syntax_loader: %d failure(s)\n", wf_test_failures);
		return 1;
	}
	printf("test_syntax_loader: all checks passed (%d skipped)\n", skips);
	return skips ? 0 : 0;
}
