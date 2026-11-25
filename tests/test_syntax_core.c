/*
 * The parts of syntax colouring that need no DLL (DR-DRAFT-033): the table of
 * official languages and the files that name them, the manifest reader, runs
 * of classes, the overlay that keeps colours in place while a text changes, the
 * .env lexer, and the colours the theme gives the classes of every preset.
 */
#include <stdio.h>
#include <string.h>

#include "render/wf_theme.h"
#include "syntax/wf_dotenv.h"
#include "syntax/wf_manifest.h"
#include "syntax/wf_synlang.h"
#include "syntax/wf_synoverlay.h"
#include "syntax/wf_synruns.h"
#include "wf_test.h"

static bool in_list(const char *list, const char *word)
{
	size_t n = strlen(word);

	while (list && *list) {
		const char *end = strchr(list, ';');
		size_t len = end ? (size_t)(end - list) : strlen(list);

		if (len == n && !memcmp(list, word, n)) {
			return true;
		}
		list = end ? end + 1 : NULL;
	}
	return false;
}

static void the_table_is_consistent(void)
{
	size_t n = 0;
	const WfSynLang *t = wf_synlang_all(&n);

	CHECK(WF_SYNLANG_COUNT == n);
	for (size_t i = 0; i < n; i++) {
		CHECK(t[i].id && t[i].id[0]);
		CHECK(wf_synlang_by_id(t[i].id) == &t[i]);
		CHECK(t[i].builtin == (NULL == t[i].entry));
		CHECK(t[i].builtin || !strncmp(t[i].entry, "tree_sitter_", 12));
		for (size_t j = i + 1; j < n; j++) {
			CHECK(strcmp(t[i].id, t[j].id));
		}
		/* an extension or a name belongs to one language only */
		for (size_t j = 0; j < n; j++) {
			const char *p = t[i].exts;

			while (p && *p && j != i) {
				const char *e = strchr(p, ';');
				char w[32];
				size_t len = e ? (size_t)(e - p) : strlen(p);

				memcpy(w, p, len);
				w[len] = '\0';
				CHECK(!in_list(t[j].exts, w));
				p = e ? e + 1 : NULL;
			}
		}
		/* lower case, and a dot first for an extension */
		for (const char *p = t[i].exts; p && *p; p++) {
			CHECK(!(*p >= 'A' && *p <= 'Z'));
		}
		for (const char *p = t[i].names; p && *p; p++) {
			CHECK(!(*p >= 'A' && *p <= 'Z'));
		}
	}
	CHECK(NULL == wf_synlang_by_id("c"));
	CHECK(NULL == wf_synlang_by_id(NULL));
	CHECK(NULL == wf_synlang_by_id("json "));
	CHECK(wf_synlang_by_id("dotenv")->builtin);
	CHECK(wf_synlang_by_id("markdown_inline")->companion);
}

static const char *lang_of(const wchar_t *name)
{
	const WfSynLang *l = wf_synlang_for_name(name);

	return l ? l->id : "-";
}

static void files_name_their_language(void)
{
	CHECK(!strcmp("json", lang_of(L"a.json")));
	CHECK(!strcmp("json", lang_of(L"A.JSON")));
	CHECK(!strcmp("json", lang_of(L"x.geojson")));
	/* a whole name beats the extension */
	CHECK(!strcmp("jsonc", lang_of(L"tsconfig.json")));
	CHECK(!strcmp("jsonc", lang_of(L"TSCONFIG.JSON")));
	CHECK(!strcmp("jsonc", lang_of(L"a.jsonc")));
	CHECK(!strcmp("yaml", lang_of(L"ci.yml")));
	CHECK(!strcmp("yaml", lang_of(L"ci.YAML")));
	CHECK(!strcmp("toml", lang_of(L"Cargo.toml")));
	CHECK(!strcmp("toml", lang_of(L"Cargo.lock")));
	CHECK(!strcmp("ini", lang_of(L"a.ini")));
	CHECK(!strcmp("ini", lang_of(L"a.cfg")));
	CHECK(!strcmp("ini", lang_of(L".gitconfig")));
	CHECK(!strcmp("xml", lang_of(L"a.csproj")));
	CHECK(!strcmp("xml", lang_of(L"logo.svg")));
	CHECK(!strcmp("markdown", lang_of(L"README.md")));
	CHECK(!strcmp("powershell", lang_of(L"build.ps1")));
	CHECK(!strcmp("bash", lang_of(L"run.sh")));
	CHECK(!strcmp("bash", lang_of(L".bashrc")));
	CHECK(!strcmp("dotenv", lang_of(L".env")));
	CHECK(!strcmp("dotenv", lang_of(L".env.local")));
	CHECK(!strcmp("dotenv", lang_of(L"prod.env")));
	/* nothing else */
	CHECK(!strcmp("-", lang_of(L"a.txt")));
	CHECK(!strcmp("-", lang_of(L"json")));
	CHECK(!strcmp("-", lang_of(L"a.")));
	CHECK(!strcmp("-", lang_of(L"")));
	CHECK(!strcmp("-", lang_of(L"a.jsonx")));
	CHECK(!strcmp("-", lang_of(L"a.json.bak")));
	CHECK(!strcmp("-", lang_of(L"\x3042.json")));
	CHECK(NULL == wf_synlang_for_name(NULL));
	{
		wchar_t long_name[200];

		for (int i = 0; i < 190; i++) {
			long_name[i] = L'a';
		}
		wcscpy_s(long_name + 190, 10, L".json");
		CHECK(NULL == wf_synlang_for_name(long_name));
	}
	/* the helper language is never chosen by a name */
	CHECK(NULL == wf_synlang_for_name(L"a.markdown_inline"));
}

static const char GOOD[] = "# a comment\n"
                           "id = \"toml\"\n"
                           "version = \"0.7.0\"\n"
                           "abi = 14\n"
                           "library = \"parser.dll\"\n"
                           "entrypoint = \"tree_sitter_toml\"\n"
                           "extensions = [\".toml\"]\n"
                           "query = \"highlights.scm\"\n"
                           "release = \"1.0.0\"  # the pack\n";

static WfMfError parse(const char *s, WfManifest *m, unsigned *line)
{
	return wf_manifest_parse(s, strlen(s), m, line);
}

static void a_manifest_is_read_strictly(void)
{
	WfManifest m;
	unsigned line = 99;
	char big[WF_MANIFEST_MAX_BYTES + 8];
	char text[1024];

	CHECK(WF_MF_OK == parse(GOOD, &m, &line));
	CHECK(!strcmp(m.id, "toml") && !strcmp(m.version, "0.7.0") &&
	        14 == m.abi && !strcmp(m.library, "parser.dll") &&
	        !strcmp(m.entrypoint, "tree_sitter_toml") && 1 == m.nexts &&
	        !strcmp(m.exts[0], ".toml") &&
	        !strcmp(m.query, "highlights.scm") &&
	        !strcmp(m.release, "1.0.0"));
	CHECK(WF_MF_OK == wf_manifest_agrees(&m, wf_synlang_by_id("toml")));
	CHECK(WF_MF_E_DISAGREES ==
	        wf_manifest_agrees(&m, wf_synlang_by_id("json")));
	CHECK(WF_MF_E_DISAGREES ==
	        wf_manifest_agrees(&m, wf_synlang_by_id("dotenv")));
	CHECK(WF_MF_E_DISAGREES ==
	        wf_manifest_agrees(NULL, wf_synlang_by_id("toml")));

	/* CRLF and tabs are only white space */
	{
		char crlf[sizeof GOOD * 2];
		size_t o = 0;

		for (const char *p = GOOD; *p; p++) {
			if ('\n' == *p) {
				crlf[o++] = '\r';
			}
			crlf[o++] = *p;
		}
		crlf[o] = '\0';
		CHECK(WF_MF_OK == parse(crlf, &m, NULL));
	}
	/* size */
	CHECK(WF_MF_E_SIZE == wf_manifest_parse("", 0, &m, NULL));
	memset(big, ' ', sizeof big);
	CHECK(WF_MF_E_SIZE ==
	        wf_manifest_parse(big, WF_MANIFEST_MAX_BYTES + 1, &m, NULL));
	/* bytes that are not text */
	{
		char bad[sizeof GOOD];

		memcpy(bad, GOOD, sizeof GOOD);
		bad[20] = '\0';
		CHECK(WF_MF_E_ENCODING ==
		        wf_manifest_parse(bad, sizeof GOOD - 1, &m, &line));
		memcpy(bad, GOOD, sizeof GOOD);
		bad[20] = (char)-128;
		CHECK(WF_MF_E_ENCODING ==
		        wf_manifest_parse(bad, sizeof GOOD - 1, &m, &line));
		CHECK(line >= 1);
		memcpy(bad, GOOD, sizeof GOOD);
		bad[20] = 0x07;
		CHECK(WF_MF_E_ENCODING ==
		        wf_manifest_parse(bad, sizeof GOOD - 1, &m, NULL));
	}
	/* shape */
	CHECK(WF_MF_E_SYNTAX == parse("id \"toml\"\n", &m, &line) && 1 == line);
	CHECK(WF_MF_E_SYNTAX == parse("id = toml\n", &m, NULL));
	CHECK(WF_MF_E_SYNTAX == parse("id = \"toml\n", &m, NULL));
	CHECK(WF_MF_E_SYNTAX == parse("id = \"to\\ml\"\n", &m, NULL));
	CHECK(WF_MF_E_SYNTAX == parse("id = \"toml\" junk\n", &m, NULL));
	CHECK(WF_MF_E_SYNTAX == parse("abi = x\n", &m, NULL));
	CHECK(WF_MF_E_SYNTAX == parse("extensions = \".a\"\n", &m, NULL));
	CHECK(WF_MF_E_SYNTAX ==
	        parse("extensions = [\".a\" \".b\"]\n", &m, NULL));
	CHECK(WF_MF_E_SYNTAX == parse("= 3\n", &m, NULL));
	CHECK(WF_MF_E_UNKNOWN_KEY == parse("colour = \"x\"\n", &m, NULL));
	CHECK(WF_MF_E_SYNTAX ==
	        parse("Id = \"x\"\n", &m, NULL)); /* keys are lower case */
	snprintf(text, sizeof text, "%sid = \"json\"\n", GOOD);
	CHECK(WF_MF_E_DUPLICATE == parse(text, &m, &line) && line > 1);
	/* missing keys: every one is needed */
	{
		static const char *const keys[] = {"id", "version", "abi",
		        "library", "entrypoint", "extensions", "query",
		        "release"};

		for (size_t k = 0; k < 8; k++) {
			char cut[sizeof GOOD];
			size_t o = 0;
			const char *p = GOOD;

			while (*p) {
				const char *e = strchr(p, '\n');
				size_t n = (size_t)(e - p) + 1;

				if (strncmp(p, keys[k], strlen(keys[k])) ||
				        ' ' != p[strlen(keys[k])]) {
					memcpy(cut + o, p, n);
					o += n;
				}
				p += n;
			}
			cut[o] = '\0';
			CHECK(WF_MF_E_MISSING == parse(cut, &m, NULL));
		}
	}
	/* lengths, numbers, names */
	{
		char long_id[200];

		snprintf(long_id, sizeof long_id, "id = \"%0*d\"\n", 70, 1);
		CHECK(WF_MF_E_LENGTH == parse(long_id, &m, NULL));
	}
	CHECK(WF_MF_E_RANGE == parse("abi = 1234567\n", &m, NULL));
	CHECK(WF_MF_E_RANGE == parse("abi = 99999999999999999999\n", &m, NULL));
	CHECK(WF_MF_E_FILENAME ==
	                parse("library = \"..\\\\x.dll\"\n", &m, NULL) ||
	        WF_MF_E_SYNTAX ==
	                parse("library = \"..\\\\x.dll\"\n", &m, NULL));
	CHECK(WF_MF_E_FILENAME == parse("library = \"../x.dll\"\n", &m, NULL));
	CHECK(WF_MF_E_FILENAME == parse("library = \"a/b.dll\"\n", &m, NULL));
	CHECK(WF_MF_E_FILENAME == parse("library = \"..\"\n", &m, NULL));
	CHECK(WF_MF_E_FILENAME == parse("library = \"a..b.dll\"\n", &m, NULL));
	CHECK(WF_MF_E_FILENAME == parse("library = \".hidden\"\n", &m, NULL));
	CHECK(WF_MF_E_FILENAME == parse("library = \"C:x.dll\"\n", &m, NULL));
	CHECK(WF_MF_E_FILENAME == parse("library = \"\"\n", &m, NULL));
	CHECK(WF_MF_E_FILENAME == parse("query = \"a b.scm\"\n", &m, NULL));
	CHECK(WF_MF_E_FILENAME == parse("extensions = [\"toml\"]\n", &m, NULL));
	CHECK(WF_MF_E_FILENAME ==
	        parse("extensions = [\".TOML\"]\n", &m, NULL));
	CHECK(WF_MF_E_FILENAME == parse("extensions = [\".\"]\n", &m, NULL));
	{
		char many[1024] = "extensions = [";

		for (int i = 0; i < 33; i++) {
			strcat_s(many, sizeof many, i ? ", \".a\"" : "\".a\"");
		}
		strcat_s(many, sizeof many, "]\n");
		CHECK(WF_MF_E_RANGE == parse(many, &m, NULL));
	}
}

static void a_manifest_must_agree_with_the_table(void)
{
	static const char *const swaps[][2] = {
	        {"id = \"toml\"", "id = \"json\""},
	        {"tree_sitter_toml", "tree_sitter_json"},
	        {"parser.dll", "other.dll"},
	        {"highlights.scm", "other.scm"},
	        {"[\".toml\"]", "[\".toml\", \".tml\"]"},
	        {"[\".toml\"]", "[\".tml\"]"},
	        {"[\".toml\"]", "[]"},
	};
	WfManifest m;

	for (size_t k = 0; k < sizeof swaps / sizeof swaps[0]; k++) {
		char text[1024];
		const char *at = strstr(GOOD, swaps[k][0]);
		size_t head = (size_t)(at - GOOD);

		memcpy(text, GOOD, head);
		strcpy_s(text + head, sizeof text - head, swaps[k][1]);
		strcat_s(text, sizeof text, at + strlen(swaps[k][0]));
		CHECK(WF_MF_OK == parse(text, &m, NULL));
		CHECK(WF_MF_E_DISAGREES ==
		        wf_manifest_agrees(&m, wf_synlang_by_id("toml")));
	}
	/* the same extension twice is not the same set */
	{
		char text[1024];
		const char *at = strstr(GOOD, "[\".toml\"]");

		memcpy(text, GOOD, (size_t)(at - GOOD));
		strcpy_s(text + (at - GOOD), 100, "[\".toml\", \".toml\"]");
		strcat_s(text, sizeof text, at + 9);
		CHECK(WF_MF_OK == parse(text, &m, NULL));
		CHECK(WF_MF_E_DISAGREES ==
		        wf_manifest_agrees(&m, wf_synlang_by_id("toml")));
	}
}

/* The folder this test's source is in the tests\ folder of: the source tree. */
static const char *source_root(void)
{
	static char root[512];
	char *a, *b, *cut;

	snprintf(root, sizeof root, "%s", __FILE__);
	for (int k = 0; k < 2; k++) { /* the file name, then "tests" */
		a = strrchr(root, '\\');
		b = strrchr(root, '/');
		cut = a > b ? a : b;
		if (cut) {
			*cut = '\0';
		}
	}
	return root;
}

/* The manifests that ship agree with the table: read from the source tree. */
static void the_shipped_manifests_agree(void)
{
	size_t n = 0;
	const WfSynLang *t = wf_synlang_all(&n);

	for (size_t i = 0; i < n; i++) {
		char path[512], buf[WF_MANIFEST_MAX_BYTES + 1];
		FILE *f = NULL;
		size_t got;
		WfManifest m;

		if (t[i].builtin) {
			continue;
		}
		snprintf(path, sizeof path, "%s/grammar-pack/%s/manifest.toml",
		        source_root(), t[i].id);
		if (0 != fopen_s(&f, path, "rb") || !f) {
			CHECK_FAIL("manifest of a language is missing");
			continue;
		}
		got = fread(buf, 1, sizeof buf - 1, f);
		fclose(f);
		CHECK(WF_MF_OK == wf_manifest_parse(buf, got, &m, NULL));
		CHECK(WF_MF_OK == wf_manifest_agrees(&m, &t[i]));
		CHECK(m.abi >= 13 && m.abi <= 15);
	}
}

static void runs_are_built_and_read(void)
{
	uint8_t cls[12] = {0, 0, 2, 2, 2, 0, 5, 5, 0, 0, 2, 2};
	WfSynRun *r = NULL;
	uint32_t n = 0;
	uint8_t out[12];

	CHECK(WF_SYN_OK == wf_syn_runs_build(cls, 12, 100, &r, &n));
	CHECK(3 == n && 2 == r[0].start && 3 == r[0].length && 2 == r[0].cls &&
	        6 == r[1].start && 2 == r[1].length && 5 == r[1].cls &&
	        10 == r[2].start && 2 == r[2].length);
	CHECK(2 == wf_syn_runs_class_at(r, n, 3));
	CHECK(0 == wf_syn_runs_class_at(r, n, 5));
	CHECK(5 == wf_syn_runs_class_at(r, n, 7));
	CHECK(0 == wf_syn_runs_class_at(r, n, 8));
	CHECK(0 == wf_syn_runs_class_at(r, n, 500));
	wf_syn_runs_fill(r, n, 0, 12, out);
	CHECK(!memcmp(out, cls, 12));
	wf_syn_runs_fill(
	        r, n, 3, 5, out); /* a window that begins inside a run */
	CHECK(!memcmp(out, cls + 3, 5));
	wf_syn_runs_fill(r, n, 11, 6, out); /* and one that runs past the end */
	CHECK(2 == out[0] && 0 == out[1] && 0 == out[5]);
	free(r);
	CHECK(WF_SYN_E_LIMIT == wf_syn_runs_build(cls, 12, 2, &r, &n) && !r &&
	        !n);
	CHECK(WF_SYN_OK == wf_syn_runs_build(cls, 0, 1, &r, &n) && !r && !n);
	{
		uint8_t none[4] = {0};

		CHECK(WF_SYN_OK == wf_syn_runs_build(none, 4, 1, &r, &n) &&
		        !r && !n);
	}
}

static WfSynOverlay overlay_of(const char *text, const uint8_t *cls)
{
	WfSynOverlay ov;
	WfSynRun *r = NULL;
	uint32_t n = 0;
	size_t len = strlen(text);
	char *base = malloc(len + 1);

	CHECK(WF_SYN_OK == wf_syn_runs_build(cls, (uint32_t)len, 1000, &r, &n));
	memcpy(base, text, len + 1);
	memset(&ov, 0, sizeof ov);
	wf_synov_set(&ov, r, n, base, len, len);
	return ov;
}

static void colours_stay_in_place_while_the_text_changes(void)
{
	/* "ab cd ef": a=1 b=1 . c=2 d=2 . e=3 f=3 */
	const char *old = "ab cd ef";
	const uint8_t cls[8] = {1, 1, 0, 2, 2, 0, 3, 3};
	WfSynOverlay ov = overlay_of(old, cls);
	uint8_t out[32];

	/* the text it is of */
	wf_synov_retarget(&ov, old, 8);
	wf_synov_fill(&ov, 0, 8, out);
	CHECK(!memcmp(out, cls, 8));

	/* a letter typed in the middle word: the words around do not move */
	wf_synov_retarget(&ov, "ab cXd ef", 9);
	wf_synov_fill(&ov, 0, 9, out);
	CHECK(1 == out[0] && 1 == out[1] && 0 == out[2]);
	CHECK(0 == out[4]);                /* the edited stretch is plain */
	CHECK(3 == out[7] && 3 == out[8]); /* shifted by one */
	CHECK(0 == out[6]);

	/* text removed from the middle */
	wf_synov_retarget(&ov, "ab  ef", 6);
	wf_synov_fill(&ov, 0, 6, out);
	CHECK(1 == out[0] && 1 == out[1] && 3 == out[4] && 3 == out[5]);

	/* appended, and a window that is only the appended part */
	wf_synov_retarget(&ov, "ab cd ef gh", 11);
	wf_synov_fill(&ov, 0, 8, out);
	CHECK(!memcmp(out, cls, 8));
	wf_synov_fill(&ov, 8, 3, out);
	CHECK(0 == out[0] && 0 == out[1] && 0 == out[2]);

	/* prepended */
	wf_synov_retarget(&ov, "ZZab cd ef", 10);
	wf_synov_fill(&ov, 0, 10, out);
	CHECK(0 == out[0] && 0 == out[1] && 1 == out[2] && 1 == out[3] &&
	        2 == out[5] && 3 == out[8]);

	/* everything replaced: nothing carries over */
	wf_synov_retarget(&ov, "xyz", 3);
	wf_synov_fill(&ov, 0, 3, out);
	CHECK(0 == out[0] && 0 == out[1] && 0 == out[2]);

	/* the same text again */
	wf_synov_retarget(&ov, old, 8);
	wf_synov_fill(&ov, 0, 8, out);
	CHECK(!memcmp(out, cls, 8));
	/* a window in the middle */
	wf_synov_fill(&ov, 3, 2, out);
	CHECK(2 == out[0] && 2 == out[1]);
	wf_synov_clear(&ov);
	wf_synov_fill(&ov, 0, 4, out);
	CHECK(0 == out[0]);
	wf_synov_clear(&ov); /* twice is fine */
}

static void an_overlay_without_a_base_is_the_text_itself(void)
{
	WfSynOverlay ov;
	WfSynRun *r = malloc(sizeof *r);
	uint8_t out[8];

	r->start = 2;
	r->length = 3;
	r->cls = 4;
	memset(&ov, 0, sizeof ov);
	wf_synov_set(&ov, r, 1, NULL, 0, 8);
	wf_synov_retarget(&ov, "anything", 8); /* ignored */
	wf_synov_fill(&ov, 0, 8, out);
	CHECK(0 == out[1] && 4 == out[2] && 4 == out[4] && 0 == out[5]);
	wf_synov_clear(&ov);
}

static WfSynRun *dup_runs(const WfSynRun *r, uint32_t n)
{
	WfSynRun *c = malloc(n * sizeof *c);

	memcpy(c, r, n * sizeof *c);
	return c;
}

static void runs_that_are_not_what_a_worker_promises_are_not_kept(void)
{
	static const WfSynRun ok[] = {{0, 2, 1}, {2, 3, 2}, {10, 2, 3}};
	static const WfSynRun unsorted[] = {{5, 2, 1}, {0, 2, 2}};
	static const WfSynRun overlap[] = {{0, 4, 1}, {3, 2, 2}};
	static const WfSynRun beyond[] = {{10, 8, 1}};
	static const WfSynRun wraps[] = {{4294967290u, 100, 1}};
	static const WfSynRun empty[] = {{3, 0, 1}};
	static const WfSynRun bad_class[] = {{0, 1, WF_SC_COUNT}};
	static const struct
	{
		const WfSynRun *r;
		uint32_t n;
	} bad[] = {{unsorted, 2}, {overlap, 2}, {beyond, 1}, {wraps, 1},
	        {empty, 1}, {bad_class, 1}};
	WfSynOverlay ov;
	uint8_t out[16];

	memset(&ov, 0, sizeof ov);
	CHECK(wf_synov_set(&ov, dup_runs(ok, 3), 3, NULL, 0, 12));
	CHECK(ov.active);
	wf_synov_fill(&ov, 0, 12, out);
	CHECK(1 == out[0] && 2 == out[2] && 0 == out[5] && 3 == out[11]);
	/* exactly up to the end of the text is fine; one past it is not */
	CHECK(wf_synov_set(&ov, dup_runs(ok, 3), 3, NULL, 0, 12));
	CHECK(!wf_synov_set(&ov, dup_runs(ok, 3), 3, NULL, 0, 11));
	for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
		CHECK(!wf_synov_set(&ov, dup_runs(bad[i].r, bad[i].n), bad[i].n,
		        NULL, 0, 16));
		CHECK(!ov.active); /* nothing is kept, so nothing is drawn */
		wf_synov_fill(&ov, 0, 16, out);
		CHECK(0 == out[0] && 0 == out[8]);
	}
	/* no runs at all is fine: the text is plain */
	CHECK(wf_synov_set(&ov, NULL, 0, NULL, 0, 4));
	wf_synov_clear(&ov);
}

static uint8_t env_class(const WfSynRun *r, uint32_t n, uint32_t at)
{
	return (uint8_t)wf_syn_runs_class_at(r, n, at);
}

static void dotenv_lines_are_coloured(void)
{
	static const char T[] =
	        "# c\nexport A=1\nB=\"x $A\\n\" # t\nC=plain # tail\n"
	        "D='raw $X'\nnot a line\r\nE=${F}\n";
	WfSynRun *r;
	uint32_t n;

	CHECK(WF_SYN_OK == wf_dotenv_highlight(T, sizeof T - 1, NULL, &r, &n));
	CHECK(WF_SC_COMMENT == env_class(r, n, 0));
	CHECK(WF_SC_KEYWORD == env_class(r, n, 4));   /* export */
	CHECK(WF_SC_KEY == env_class(r, n, 11));      /* A */
	CHECK(WF_SC_OPERATOR == env_class(r, n, 12)); /* = */
	CHECK(0 == env_class(r, n, 13));              /* 1: plain */
	{
		const char *b = strstr(T, "B=");
		uint32_t at = (uint32_t)(b - T);

		CHECK(WF_SC_KEY == env_class(r, n, at));
		CHECK(WF_SC_STRING == env_class(r, n, at + 2));
		CHECK(WF_SC_VARIABLE == env_class(r, n, at + 5)); /* $A */
		CHECK(WF_SC_ESCAPE == env_class(r, n, at + 7));   /* \n */
		CHECK(WF_SC_COMMENT == env_class(r, n, at + 11)); /* # t */
	}
	{
		uint32_t at = (uint32_t)(strstr(T, "C=") - T);

		CHECK(WF_SC_KEY == env_class(r, n, at));
		CHECK(0 == env_class(r, n, at + 2)); /* plain */
		CHECK(WF_SC_COMMENT == env_class(r, n, at + 8));
	}
	{
		uint32_t at = (uint32_t)(strstr(T, "D=") - T);

		CHECK(WF_SC_STRING == env_class(r, n, at + 3));
		CHECK(WF_SC_STRING ==
		        env_class(r, n, at + 8)); /* $X is not one in '' */
	}
	{
		uint32_t at = (uint32_t)(strstr(T, "not a") - T);

		CHECK(0 == env_class(r, n, at)); /* not NAME=value */
	}
	{
		uint32_t at = (uint32_t)(strstr(T, "E=") - T);

		CHECK(WF_SC_KEY == env_class(r, n, at));
		CHECK(WF_SC_VARIABLE == env_class(r, n, at + 3));
	}
	free(r);

	/* limits, and no text */
	{
		WfSynLimits lim = {10, 0, 0, 0, 0};

		CHECK(WF_SYN_E_LIMIT ==
		        wf_dotenv_highlight(T, sizeof T - 1, &lim, &r, &n));
		lim.max_text = 0;
		lim.max_line = 4;
		CHECK(WF_SYN_E_LIMIT ==
		        wf_dotenv_highlight(T, sizeof T - 1, &lim, &r, &n));
	}
	CHECK(WF_SYN_OK == wf_dotenv_highlight("", 0, NULL, &r, &n) && !n);
	CHECK(WF_SYN_E_BADARG == wf_dotenv_highlight("a", 1, NULL, NULL, &n));
}

static void every_theme_colours_every_class_readably(void)
{
	for (int t = 0; t < WF_THEME_PRESET_COUNT; t++) {
		const WfThemeDefinition *d = wf_theme_preset((WfThemeId)t);
		const WfThemeStyles *s;
		uint32_t before;

		CHECK(d);
		if (!d) {
			continue;
		}
		wf_theme_activate(&d->palette);
		s = wf_theme_styles();
		before = wf_theme_generation();
		/* the classes of the grammars are readable on the background */
		for (int c = 11; c < WF_SYNTAX_CLASSES; c++) {
			CHECK(wf_color_contrast(s->syntax[c],
			              d->palette.background) >= 4.5);
		}
		/* each class has a style: the class colour on the base background */
		for (int c = 0; c < WF_SYNTAX_CLASSES; c++) {
			WfStyle st = wf_theme_syntax_style((uint8_t)c);

			CHECK(st.fg_rgb == s->syntax[c]);
			CHECK(st.bg_rgb == s->base.bg_rgb);
		}
		CHECK(wf_theme_syntax_style(WF_SC_COMMENT).attributes &
		        WF_ATTR_DIM);
		CHECK(wf_theme_syntax_style(WF_SC_KEYWORD).attributes &
		        WF_ATTR_BOLD);
		CHECK(wf_theme_syntax_style(WF_SC_HEADING).attributes &
		        WF_ATTR_BOLD);
		CHECK(wf_theme_syntax_style(WF_SC_LINK).attributes &
		        WF_ATTR_UNDERLINE);
		CHECK(wf_theme_syntax_style(WF_SC_ERROR).attributes &
		        WF_ATTR_UNDERLINE);
		CHECK(0 == wf_theme_syntax_style(WF_SC_STRING).attributes);
		/* a class outside the table is plain text */
		CHECK(wf_theme_syntax_style(200).fg_rgb == s->base.fg_rgb);
		/* reading styles changes nothing */
		CHECK(before == wf_theme_generation());
	}
	/* a palette whose accents sit close to the background (a custom theme can be
	 * made so): the classes are moved until they read, both on dark and on light */
	for (int light = 0; light < 2; light++) {
		WfThemePalette p = wf_theme_preset(WF_THEME_SPADE)->palette;
		uint32_t bg = light ? 0xF0F0F0u : 0x101010u;
		uint32_t near = light ? 0xE0E0E0u : 0x202020u;

		p.background = bg;
		p.foreground = light ? 0x000000u : 0xFFFFFFu;
		p.accent = p.success_foreground = p.warning_foreground = near;
		p.error_foreground = p.directory_foreground =
		        p.muted_foreground = near;
		wf_theme_activate(&p);
		for (int c = 11; c < WF_SYNTAX_CLASSES; c++) {
			CHECK(wf_color_contrast(
			              wf_theme_styles()->syntax[c], bg) >= 4.5);
		}
	}
	/* the themes differ: two presets do not give the string the same colour */
	{
		uint32_t a, b;

		wf_theme_activate(&wf_theme_preset(WF_THEME_SPADE)->palette);
		a = wf_theme_styles()->syntax[WF_SC_KEYWORD];
		wf_theme_activate(&wf_theme_preset(WF_THEME_HEART)->palette);
		b = wf_theme_styles()->syntax[WF_SC_KEYWORD];
		CHECK(a != b);
	}
	/* a theme change is a change of the table and nothing else */
	{
		uint32_t g = wf_theme_generation();

		wf_theme_activate(&wf_theme_preset(WF_THEME_KING)->palette);
		CHECK(wf_theme_generation() == g + 1);
	}
	/* the number of classes is the number of colours */
	CHECK(WF_SYNTAX_CLASSES == WF_SC_COUNT);
}

int main(void)
{
	the_table_is_consistent();
	files_name_their_language();
	a_manifest_is_read_strictly();
	a_manifest_must_agree_with_the_table();
	the_shipped_manifests_agree();
	runs_are_built_and_read();
	colours_stay_in_place_while_the_text_changes();
	an_overlay_without_a_base_is_the_text_itself();
	runs_that_are_not_what_a_worker_promises_are_not_kept();
	dotenv_lines_are_coloured();
	every_theme_colours_every_class_readably();
	return TEST_RESULT();
}
