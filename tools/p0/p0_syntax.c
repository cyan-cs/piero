/*
 * Syntax colours through the real product (DR-DRAFT-033). The program is copied
 * into the lab with different sets of grammars beside it, and the files below
 * are looked at in the preview pane:
 *
 *   A  piero.exe alone: nothing is coloured, and no syntax DLL is in the process
 *   B  the JSON grammar only: JSON is coloured, YAML is not, and the YAML and
 *      TOML grammars are never loaded
 *   C  every grammar: what is shown is loaded, what is not shown is not
 *   D  a grammar with a changed byte, E a grammar replaced by another DLL:
 *      refused, never loaded, the text still shown, the program still answering
 *   F  the same install after a theme change: the colours follow the theme
 *
 * Colours are read from what the program wrote (the SGR before a word) and
 * compared with the colour the theme gives the class. Modules are read from the
 * running process. Included by p0_conpty_host.c after p0_state_gate.c.
 */
#include "wf_sgr_rgb.h"

#define SYN_CONFIG                                                             \
	"[meta]\r\nschema_version = 1\r\nonboarding_completed = true\r\n"      \
	"[updates]\r\nenabled = false\r\nconsent_recorded = true\r\n"          \
	"[appearance]\r\ncolor_mode = truecolor\r\n"

#define SYN_JSON "{\"key\": \"v\", \"n\": 42, \"t\": true, \"z\": null}\n"
#define SYN_YAML "name: piero\ncount: 42\nok: true\n"
#define SYN_TOML "[server]\nhost = \"h\"\nport = 80\n"

typedef struct
{
	bool syntax_dll;      /* piero-syntax.dll beside the program */
	const char *grammars; /* ';'-separated ids, NULL: all of them */
	const char *corrupt; /* one byte of this grammar's parser.dll changed */
	const char *swap; /* this grammar's parser.dll is another grammar's */
	bool big;         /* a JSON file of 2.6 MiB first in the folder */
} SynInstall;

static const char *const syn_all[] = {"json", "jsonc", "yaml", "toml", "ini",
        "xml", "markdown", "markdown_inline", "powershell", "bash"};

static bool syn_wants(const SynInstall *in, const char *id)
{
	size_t n = strlen(id);
	const char *p = in->grammars;

	if (!p) {
		return true;
	}
	while (*p) {
		const char *e = strchr(p, ';');
		size_t len = e ? (size_t)(e - p) : strlen(p);

		if (len == n && !memcmp(p, id, n)) {
			return true;
		}
		p = e ? e + 1 : p + len;
	}
	return false;
}

static bool syn_copy(const wchar_t *src, const wchar_t *dst)
{
	return 0 != CopyFileW(src, dst, FALSE);
}

static void syn_dir_of(const wchar_t *path, wchar_t *out, size_t cap)
{
	wchar_t *slash;

	wcscpy_s(out, cap, path);
	for (wchar_t *c = out; *c; c++) {
		if (L'/' == *c) {
			*c = L'\\';
		}
	}
	slash = wcsrchr(out, L'\\');
	if (slash) {
		*slash = L'\0';
	}
}

static bool syn_flip_byte(const wchar_t *path)
{
	HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
	        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	LARGE_INTEGER mid;
	unsigned char b;
	DWORD n;
	bool ok = false;

	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	mid.QuadPart = 1000;
	if (SetFilePointerEx(h, mid, NULL, FILE_BEGIN) &&
	        ReadFile(h, &b, 1, &n, NULL) && 1 == n) {
		b = (unsigned char)(b ^ 0x55);
		ok = SetFilePointerEx(h, mid, NULL, FILE_BEGIN) &&
		        WriteFile(h, &b, 1, &n, NULL) && 1 == n;
	}
	CloseHandle(h);
	return ok;
}

/* Builds <lab>\install: piero.exe, and syntax\ as `in` says. Returns the path of
 * the copied program in `exe`. */
static bool syn_install(
        Gate *g, const SynInstall *in, wchar_t *exe, size_t exe_cap)
{
	wchar_t from[1100], src_syntax[1100], rel[1100], dst[1100], srcp[1100];

	/* the built syntax folder is beside this host, whichever copy of the program
	 * (a mutant, for one) is the one being run */
	{
		wchar_t host[1100];

		GetModuleFileNameW(NULL, host, 1100);
		syn_dir_of(host, from, 1100);
	}
	if (!gate_mkdir(g, L"install") ||
	        !gate_path(g, L"install\\piero.exe", exe) ||
	        !syn_copy(g_child, exe)) {
		return false;
	}
	(void)exe_cap;
	if (!in->syntax_dll) {
		return true;
	}
	swprintf(src_syntax, 1100, L"%ls\\syntax", from);
	if (!gate_mkdir(g, L"install\\syntax") ||
	        !gate_mkdir(g, L"install\\syntax\\grammars")) {
		return false;
	}
	swprintf(srcp, 1100, L"%ls\\piero-syntax.dll", src_syntax);
	if (!gate_path(g, L"install\\syntax\\piero-syntax.dll", dst) ||
	        !syn_copy(srcp, dst)) {
		return false;
	}
	for (size_t i = 0; i < sizeof syn_all / sizeof syn_all[0]; i++) {
		static const wchar_t *const files[] = {
		        L"parser.dll", L"highlights.scm", L"manifest.toml"};
		wchar_t id[32];

		if (!syn_wants(in, syn_all[i])) {
			continue;
		}
		MultiByteToWideChar(CP_UTF8, 0, syn_all[i], -1, id, 32);
		swprintf(rel, 1100, L"install\\syntax\\grammars\\%ls", id);
		if (!gate_mkdir(g, rel)) {
			return false;
		}
		for (size_t k = 0; k < 3; k++) {
			wchar_t relf[1100];

			swprintf(srcp, 1100, L"%ls\\grammars\\%ls\\%ls",
			        src_syntax, id, files[k]);
			swprintf(relf, 1100, L"%ls\\%ls", rel, files[k]);
			if (!gate_path(g, relf, dst) || !syn_copy(srcp, dst)) {
				return false;
			}
			if (0 == k && in->swap &&
			        !strcmp(in->swap, syn_all[i])) {
				/* the parser.dll of the YAML grammar where JSON's belongs */
				swprintf(srcp, 1100,
				        L"%ls\\grammars\\yaml\\parser.dll",
				        src_syntax);
				if (!syn_copy(srcp, dst)) {
					return false;
				}
			}
			if (0 == k && in->corrupt &&
			        !strcmp(in->corrupt, syn_all[i]) &&
			        !syn_flip_byte(dst)) {
				return false;
			}
		}
	}
	return true;
}

/* The modules of the program's process, as full paths in one lower-case string
 * separated by '|'. */
static bool syn_modules(Gate *g, char *out, size_t cap)
{
	HMODULE mods[512];
	DWORD need = 0;
	size_t n = 0;

	out[0] = '\0';
	if (!EnumProcessModulesEx(g->pr.p.pi.hProcess, mods, sizeof mods, &need,
	            LIST_MODULES_ALL) ||
	        need > sizeof mods) {
		return false;
	}
	for (DWORD i = 0; i < need / sizeof mods[0] && i < 512; i++) {
		wchar_t w[1100];
		char u[2200];
		DWORD length;
		int bytes;

		length = GetModuleFileNameExW(
		        g->pr.p.pi.hProcess, mods[i], w, WF_ARRAY_LEN(w));
		if (!length || length >= WF_ARRAY_LEN(w)) {
			goto failed;
		}
		bytes = WideCharToMultiByte(
		        CP_UTF8, 0, w, -1, u, sizeof u, NULL, NULL);
		if (bytes <= 0 || (size_t)bytes >= cap - n) {
			goto failed;
		}
		for (char *c = u; *c; c++) {
			if (*c >= 'A' && *c <= 'Z') {
				*c = (char)(*c + 32);
			}
		}
		n += (size_t)snprintf(out + n, cap - n, "%s|", u);
	}
	return true;
failed:
	out[0] = '\0';
	return false;
}

static bool syn_has_module(const char *mods, const char *part)
{
	return NULL != strstr(mods, part);
}

#define GRAM_DIR "\\grammars\\"

/* How many grammar DLLs are among the modules ("...\grammars\<id>\parser.dll"). */
static int syn_count_grammars(const char *mods)
{
	int n = 0;
	const char *p = mods;

	while (*p) {
		const char *end = strchr(p, '|');
		size_t len = end ? (size_t)(end - p) : strlen(p);
		char one[1200];

		if (len >= sizeof one) {
			len = sizeof one - 1;
		}
		memcpy(one, p, len);
		one[len] = 0;
		if (strstr(one, GRAM_DIR) && len >= 10 &&
		        !strcmp(one + len - 10, "parser.dll")) {
			n++;
		}
		p = end ? end + 1 : p + len;
	}
	return n;
}

/* The foreground RGB of the last 38;2; before the last write of text. */
static bool syn_fg_before(Pty *p, const char *text, char *out, size_t cap)
{
	size_t tn = strlen(text);
	bool found = false;

	out[0] = '\0';
	EnterCriticalSection(&p->lock);
	if (p->raw_n >= tn) {
		for (size_t i = p->raw_n - tn + 1; i-- > 0 && !found;) {
			if (!memcmp(p->raw + i, text, tn)) {
				size_t lo = i > 240 ? i - 240 : 0;

				for (size_t k = i; k > lo + 5; k--) {
					if (!memcmp(p->raw + k - 5, "38;2;",
					            5)) {
						found = p0_sgr_rgb(p->raw + k,
						        i - k, out, cap);
						break;
					}
				}
				break; /* the last place only */
			}
		}
	}
	LeaveCriticalSection(&p->lock);
	return found;
}

/* "r;g;b" the theme gives class `cls`. */
static void syn_color_of(WfThemeId id, unsigned cls, char *out, size_t cap)
{
	uint32_t c;

	wf_theme_activate(&wf_theme_preset(id)->palette);
	c = wf_theme_styles()->syntax[cls];
	snprintf(out, cap, "%u;%u;%u", (c >> 16) & 255u, (c >> 8) & 255u,
	        c & 255u);
}

/* Waits until `text` was last written in the colour of class `cls`. */
static bool syn_wait_colour(
        Gate *g, const char *text, WfThemeId theme, unsigned cls, DWORD ms)
{
	char want[32], got[32];
	double t0 = now_ms();

	syn_color_of(theme, cls, want, sizeof want);
	do {
		if (syn_fg_before(&g->pr.p, text, got, sizeof got) &&
		        !strcmp(got, want)) {
			return true;
		}
		Sleep(50);
	} while (now_ms() - t0 < (double)ms);
	{
		unsigned char tail[16384];
		size_t n;
		wchar_t file[80];
		static unsigned sequence;

		fprintf(stderr,
		        "syntax colour: text=%s expected=%s observed=%s\n",
		        text, want, got[0] ? got : "<no RGB foreground>");
		EnterCriticalSection(&g->pr.p.lock);
		n = g->pr.p.raw_n < sizeof tail ? g->pr.p.raw_n : sizeof tail;
		if (n) {
			memcpy(tail, g->pr.p.raw + g->pr.p.raw_n - n, n);
		}
		LeaveCriticalSection(&g->pr.p.lock);
		swprintf(file, WF_ARRAY_LEN(file),
		        L"traces\\syntax-colour-%u.vt", ++sequence);
		ev_write_raw(g->pr.e, file, tail, n);
	}
	return false;
}

/* True when the program never wrote the colour of class `cls` of the theme. */
static bool syn_never_colour(Gate *g, WfThemeId theme, unsigned cls)
{
	char want[48];

	syn_color_of(theme, cls, want + 5, sizeof want - 5);
	memcpy(want, "38;2;", 5);
	return !raw_has(&g->pr.p, want);
}

/* A JSON file of about 2.6 MiB made of short lines, first in the folder. */
static bool syn_make_big(Gate *g)
{
	wchar_t p[1100];
	size_t n = 2600u * 1024u, at = 0;
	char *b = malloc(n + 1);
	HANDLE h;
	DWORD got = 0;
	bool ok;

	if (!b || !gate_path(g, L"browse\\0big.json", p)) {
		free(b);
		return false;
	}
	at += (size_t)snprintf(b + at, n + 1 - at, "{\n");
	while (at + 16 < n) {
		at += (size_t)snprintf(
		        b + at, n + 1 - at, "  \"k%zu\": 1,\n", at);
	}
	at += (size_t)snprintf(b + at, n + 1 - at, "  \"end\": 0\n}\n");
	h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	ok = INVALID_HANDLE_VALUE != h &&
	        WriteFile(h, b, (DWORD)at, &got, NULL) && got == at;
	if (INVALID_HANDLE_VALUE != h) {
		CloseHandle(h);
	}
	free(b);
	return ok;
}

/* How much the program has written so far. */
static size_t syn_mark(Gate *g)
{
	size_t n;

	EnterCriticalSection(&g->pr.p.lock);
	n = g->pr.p.raw_n;
	LeaveCriticalSection(&g->pr.p.lock);
	return n;
}

/* True when the colour of class `cls` was written after `from`. */
static bool syn_colour_since(
        Gate *g, size_t from, WfThemeId theme, unsigned cls)
{
	char want[48];
	size_t wn;
	bool found = false;

	syn_color_of(theme, cls, want + 5, sizeof want - 5);
	memcpy(want, "38;2;", 5);
	wn = strlen(want);
	EnterCriticalSection(&g->pr.p.lock);
	for (size_t i = from; i + wn <= g->pr.p.raw_n && !found; i++) {
		found = 0 == memcmp(g->pr.p.raw + i, want, wn);
	}
	LeaveCriticalSection(&g->pr.p.lock);
	return found;
}

static bool syn_case(Ev *e,
        const char *tag,
        const SynInstall *in,
        int (*steps)(Ev *, Gate *))
{
	Gate *g = calloc(1, sizeof *g);
	wchar_t exe[1100], saved[MAX_PATH * 2];
	bool ok = false;
	int fails = e->fails;

	if (!g || !gate_wanted(tag)) {
		free(g);
		return true;
	}
	wcscpy_s(saved, MAX_PATH * 2, g_child);
	if (!gate_open(g, e, tag)) {
		goto done;
	}
	{
		wchar_t p[1100];

		if (!gate_path(g, L"browse\\a.json", p) ||
		        !gate_write(p, SYN_JSON) ||
		        !gate_path(g, L"browse\\b.yaml", p) ||
		        !gate_write(p, SYN_YAML) ||
		        !gate_path(g, L"browse\\c.toml", p) ||
		        !gate_write(p, SYN_TOML) ||
		        !gate_write(g->cfg, SYN_CONFIG)) {
			goto done;
		}
		if (in->big && !syn_make_big(g)) {
			goto done;
		}
	}
	if (!syn_install(g, in, exe, 1100)) {
		ev_check(e, "syntax_install_made", 0,
		        "the install is in the lab", "failed", 0);
		goto done;
	}
	wcscpy_s(g_child, MAX_PATH * 2, exe);
	/* SYN_CONFIG fixes the colour depth for the exact RGB checks. */
	ok = gate_launch(g);
	if (!ok) {
		goto done;
	}
	ok = steps(e, g);
	ok = ok && gate_quit(g);
done:
	wcscpy_s(g_child, MAX_PATH * 2, saved);
	return gate_first_run_end(g, e, ok, fails);
}

static bool syn_text(Gate *g, const char *needle, DWORD ms)
{
	return product_wait_ms(
	        &g->pr.p, needle, g->pr.screen, sizeof g->pr.screen, ms);
}

static int syn_steps_core(Ev *e, Gate *g)
{
	char mods[8192];
	bool ok = syn_text(g, "\"v\"", 10000);

	Sleep(600); /* long enough for a colour to arrive, if anything could */
	ev_check(e, "syntax_core_shows_the_text", 0, "the JSON text is shown",
	        ok ? "observed" : "missing", ok);
	ok &= syn_modules(g, mods, sizeof mods);
	ev_check(e, "syntax_core_loads_no_syntax_dll", 0,
	        "no piero-syntax.dll, no parser.dll in the process",
	        !syn_has_module(mods, "piero-syntax.dll") &&
	                        !syn_has_module(mods, "parser.dll")
	                ? "none loaded"
	                : "LOADED",
	        ok && !syn_has_module(mods, "piero-syntax.dll") &&
	                !syn_has_module(mods, "parser.dll"));
	ok = ok && syn_never_colour(g, WF_THEME_SPADE, WF_SC_KEY) &&
	        syn_never_colour(g, WF_THEME_SPADE, WF_SC_BOOLEAN) &&
	        syn_never_colour(g, WF_THEME_SPADE, WF_SC_NULL);
	ev_check(e, "syntax_core_text_is_plain", 0, "no class colours",
	        ok ? "plain" : "COLOURED", ok);
	return ok;
}

static int syn_steps_json_only(Ev *e, Gate *g)
{
	char mods[8192];
	bool ok = syn_text(g, "\"v\"", 10000);

	ok = ok &&
	        syn_wait_colour(g, "\"key\"", WF_THEME_SPADE, WF_SC_KEY, 10000);
	ev_check(e, "syntax_json_key_is_the_key_colour", 0,
	        "\"name\" in the key colour", ok ? "observed" : "missing", ok);
	product_snapshot(&g->pr, "syntax_json");
	{
		static const struct
		{
			const char *text, *name;
			unsigned cls;
		} each[] = {{"\"v\"", "string", WF_SC_STRING},
		        {"42", "number", WF_SC_NUMBER},
		        {"true", "boolean", WF_SC_BOOLEAN},
		        {"null", "null", WF_SC_NULL}};

		for (size_t i = 0; i < sizeof each / sizeof each[0]; i++) {
			char name[64];
			bool one = syn_wait_colour(g, each[i].text,
			        WF_THEME_SPADE, each[i].cls, 3000);

			snprintf(name, sizeof name,
			        "syntax_json_%s_has_its_colour", each[i].name);
			ev_check(e, name, 0, "the colour of the class",
			        one ? "observed" : "missing", one);
			ok = ok && one;
		}
	}
	ok &= syn_modules(g, mods, sizeof mods);
	ev_check(e, "syntax_json_only_the_json_grammar_is_loaded", 0,
	        "piero-syntax.dll and the json parser.dll",
	        syn_has_module(mods, "\\grammars\\json\\parser.dll") &&
	                        1 == syn_count_grammars(mods)
	                ? "observed"
	                : "WRONG",
	        ok && syn_has_module(mods, "piero-syntax.dll") &&
	                syn_has_module(mods, "\\grammars\\json\\parser.dll") &&
	                1 == syn_count_grammars(mods));
	/* YAML has no grammar here: shown plain, nothing more is loaded */
	pty_send(&g->pr.p, "j", 1);
	ok = ok && syn_text(g, "count: 42", 8000);
	Sleep(500);
	ok &= syn_modules(g, mods, sizeof mods);
	ev_check(e, "syntax_json_only_yaml_is_plain_and_not_loaded", 0,
	        "no yaml parser.dll",
	        !syn_has_module(mods, "\\grammars\\yaml\\") ? "none" : "LOADED",
	        ok && !syn_has_module(mods, "\\grammars\\yaml\\") &&
	                1 == syn_count_grammars(mods));
	return ok;
}

static int syn_steps_all(Ev *e, Gate *g)
{
	char mods[8192];
	bool ok =
	        syn_wait_colour(g, "\"key\"", WF_THEME_SPADE, WF_SC_KEY, 10000);

	ev_check(e, "syntax_all_json_is_coloured", 0, "key colour",
	        ok ? "observed" : "missing", ok);
	ok &= syn_modules(g, mods, sizeof mods);
	ev_check(e, "syntax_all_only_json_loaded_at_first", 0,
	        "1 grammar of 10", 1 == syn_count_grammars(mods) ? "1" : "MORE",
	        ok && 1 == syn_count_grammars(mods));
	pty_send(&g->pr.p, "j", 1); /* b.yaml */
	ok = ok &&
	        syn_wait_colour(g, "count", WF_THEME_SPADE, WF_SC_KEY, 10000);
	ev_check(e, "syntax_all_yaml_is_coloured_when_shown", 0,
	        "count in the key colour", ok ? "observed" : "missing", ok);
	pty_send(&g->pr.p, "j", 1); /* c.toml */
	ok = ok &&
	        syn_wait_colour(
	                g, "server", WF_THEME_SPADE, WF_SC_HEADING, 10000);
	ev_check(e, "syntax_all_toml_is_coloured_when_shown", 0,
	        "server in the heading colour", ok ? "observed" : "missing",
	        ok);
	product_snapshot(&g->pr, "syntax_toml");
	ok &= syn_modules(g, mods, sizeof mods);
	ev_write_text(e, L"artifacts\\syntax_all_modules.txt", mods);
	ev_check(e, "syntax_all_three_of_ten_loaded", 0,
	        "json, yaml, toml and no other",
	        3 == syn_count_grammars(mods) ? "3" : "WRONG",
	        ok && 3 == syn_count_grammars(mods) &&
	                !syn_has_module(mods, "\\grammars\\bash\\") &&
	                !syn_has_module(mods, "\\grammars\\xml\\") &&
	                !syn_has_module(mods, "\\grammars\\powershell\\"));
	return ok;
}

static int syn_steps_refused(Ev *e, Gate *g)
{
	char mods[8192];
	bool ok = syn_text(g, "\"v\"", 10000);

	Sleep(800);
	ev_check(e, "syntax_refused_the_text_is_shown", 0, "the JSON text",
	        ok ? "observed" : "missing", ok);
	ok &= syn_modules(g, mods, sizeof mods);
	ev_check(e, "syntax_refused_grammar_is_never_loaded", 0,
	        "no json parser.dll in the process",
	        !syn_has_module(mods, "\\grammars\\json\\") ? "not loaded"
	                                                    : "LOADED",
	        ok && !syn_has_module(mods, "\\grammars\\json\\"));
	ok = ok && syn_never_colour(g, WF_THEME_SPADE, WF_SC_KEY);
	ev_check(e, "syntax_refused_text_is_plain", 0, "no class colours",
	        ok ? "plain" : "COLOURED", ok);
	/* and the program answers: the next file works (YAML has no refusal) */
	pty_send(&g->pr.p, "j", 1);
	ok = ok && syn_text(g, "count: 42", 8000);
	ev_check(e, "syntax_refused_program_still_answers", 0,
	        "the next file is shown", ok ? "observed" : "missing", ok);
	return ok;
}

static int syn_steps_theme(Ev *e, Gate *g)
{
	char text[4096];
	bool ok =
	        syn_wait_colour(g, "\"key\"", WF_THEME_SPADE, WF_SC_KEY, 10000);
	WfThemeId now = WF_THEME_SPADE;
	char mods[8192], mods2[8192];

	ev_check(e, "syntax_theme_starts_in_spade", 0, "key colour of Spade",
	        ok ? "observed" : "missing", ok);
	ok &= syn_modules(g, mods, sizeof mods);
	/* Settings, Appearance, the next theme, save (the sequence of the theme test) */
	pty_send(&g->pr.p, ",", 1);
	ok = ok && syn_text(g, "Settings", 8000);
	pty_send(&g->pr.p, "\x1b[B\x1b[B\x1b[C", 9);
	ok = ok && syn_text(g, "d duplicate", 8000);
	pty_send(&g->pr.p, "\x1b[B\r", 4);
	ok = ok && syn_text(g, "unsaved changes", 8000);
	pty_send(&g->pr.p, "\x13", 1);
	ok = ok && syn_text(g, "Saved", 8000);
	pty_send(&g->pr.p, "\x1b", 1);
	ok = ok && syn_text(g, "doc.txt", 8000);
	if (ok && read_text_file(g->cfg, text, sizeof text)) {
		const char *t = strstr(text, "theme = ");
		char word[16] = {0};
		size_t n = 0;

		while (t && t[8 + n] >= 'a' && t[8 + n] <= 'z' && n < 15) {
			word[n] = t[8 + n];
			n++;
		}
		ok = t && wf_theme_id_parse(word, &now);
	} else {
		ok = false;
	}
	ev_check(e, "syntax_theme_changed_in_settings", 0,
	        "a theme other than Spade",
	        WF_THEME_SPADE != now ? "changed" : "unchanged",
	        ok && WF_THEME_SPADE != now);
	ok = ok && WF_THEME_SPADE != now &&
	        syn_wait_colour(g, "\"key\"", now, WF_SC_KEY, 10000);
	ev_check(e, "syntax_theme_colours_follow_the_theme", 0,
	        "key colour of the new theme", ok ? "observed" : "missing", ok);
	product_snapshot(&g->pr, "syntax_theme");
	/* no new grammar, no new DLL: a theme change does not load anything */
	ok &= syn_modules(g, mods2, sizeof mods2);
	ok = ok && 1 == syn_count_grammars(mods2) &&
	        syn_count_grammars(mods) == syn_count_grammars(mods2);
	ev_check(e, "syntax_theme_change_loads_nothing", 0,
	        "the same modules before and after", ok ? "same" : "DIFFERENT",
	        ok);
	return ok;
}

static int syn_steps_editor(Ev *e, Gate *g)
{
	wchar_t json[1100];
	char text[4096];
	bool ok =
	        syn_wait_colour(g, "\"key\"", WF_THEME_SPADE, WF_SC_KEY, 10000);
	size_t mark;

	ev_check(e, "syntax_editor_preview_is_coloured_first", 0, "key colour",
	        ok ? "observed" : "missing", ok);
	pty_send(&g->pr.p, "e", 1);
	ok = ok && syn_text(g, "UTF-8", 10000);
	Sleep(300);
	/* the editor draws the same text in the same colours */
	mark = syn_mark(g);
	pty_send(&g->pr.p, "X", 1);
	ok = ok && syn_text(g, "X{", 8000);
	{
		double t0 = now_ms();
		bool again = false;

		while (!again && now_ms() - t0 < 8000.0) {
			again = syn_colour_since(
			                g, mark, WF_THEME_SPADE, WF_SC_KEY) &&
			        syn_colour_since(g, mark, WF_THEME_SPADE,
			                WF_SC_BOOLEAN) &&
			        syn_colour_since(
			                g, mark, WF_THEME_SPADE, WF_SC_NULL);
			Sleep(50);
		}
		ev_check(e, "syntax_editor_colours_follow_the_typing", 0,
		        "key, boolean and null colours after the key was typed "
		        "(the hand lexer has no such classes)",
		        again ? "observed" : "missing", again);
		ok = ok && again;
	}
	product_snapshot(&g->pr, "syntax_editor");
	/* save: the bytes are the typed text and the file's own */
	pty_send(&g->pr.p, "\x13", 1);
	ok = ok && syn_text(g, "Saved", 8000);
	ok = ok && gate_path(g, L"browse\\a.json", json) &&
	        read_text_file(json, text, sizeof text);
	ev_check(e, "syntax_editor_saves_exactly_the_typed_text", 0,
	        "X then the original bytes",
	        ok && !strncmp(text, "X{", 2) && !strcmp(text + 1, SYN_JSON)
	                ? "same"
	                : "DIFFERENT",
	        ok && !strncmp(text, "X{", 2) && !strcmp(text + 1, SYN_JSON));
	ok = ok && !strncmp(text, "X{", 2) && !strcmp(text + 1, SYN_JSON);
	pty_send(&g->pr.p, "\x1b", 1);
	ok = ok && syn_text(g, "doc.txt", 10000);
	return ok;
}

static int syn_steps_big(Ev *e, Gate *g)
{
	bool ok = syn_text(g, "k", 10000);
	size_t mark;
	wchar_t json[1100];
	char text[64];

	/* the preview of it is bounded (256 KiB) and is coloured */
	Sleep(1500);
	product_snapshot(&g->pr, "syntax_big_preview");
	ok = ok && syn_wait_colour(g, "\"k", WF_THEME_SPADE, WF_SC_KEY, 10000);
	ev_check(e, "syntax_big_preview_is_coloured", 0, "key colour",
	        ok ? "observed" : "missing", ok);
	mark = syn_mark(g);
	pty_send(&g->pr.p, "e", 1);
	ok = ok && syn_text(g, "UTF-8", 15000);
	Sleep(1500); /* time enough for a colour to arrive, if one could */
	pty_send(&g->pr.p, "Q", 1);
	ok = ok && syn_text(g, "Q{", 8000);
	Sleep(800);
	ev_check(e, "syntax_big_editor_stays_plain", 0,
	        "no key colour after the editor opened",
	        syn_colour_since(g, mark, WF_THEME_SPADE, WF_SC_KEY)
	                ? "COLOURED"
	                : "plain",
	        ok && !syn_colour_since(g, mark, WF_THEME_SPADE, WF_SC_KEY));
	ok = ok && !syn_colour_since(g, mark, WF_THEME_SPADE, WF_SC_KEY);
	/* and it can still be edited and saved (the text is what was typed and no more) */
	pty_send(&g->pr.p, "\x13", 1);
	ok = ok && syn_text(g, "Saved", 30000);
	ok = ok && gate_path(g, L"browse\\0big.json", json);
	{
		HANDLE h = ok
		        ? CreateFileW(json, GENERIC_READ,
		                  FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
		                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)
		        : INVALID_HANDLE_VALUE;
		DWORD got = 0;

		memset(text, 0, sizeof text);
		if (INVALID_HANDLE_VALUE != h) {
			ReadFile(h, text, 3, &got, NULL);
			CloseHandle(h);
		}
		ev_check(e, "syntax_big_editor_saved_what_was_typed", 0,
		        "Q{ at the start",
		        0 == strncmp(text, "Q{", 2) ? "same" : "DIFFERENT",
		        0 == strncmp(text, "Q{", 2));
		ok = ok && 0 == strncmp(text, "Q{", 2);
	}
	pty_send(&g->pr.p, "\x1b", 1);
	ok = ok && syn_text(g, "doc.txt", 15000);
	return ok;
}

static int syn_steps_resize(Ev *e, Gate *g)
{
	bool ok =
	        syn_wait_colour(g, "\"key\"", WF_THEME_SPADE, WF_SC_KEY, 10000);
	static const struct
	{
		int cols, rows;
	} sizes[] = {{60, 15}, {30, 8}, {12, 4}, {200, 50}, {160, 30}};

	for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
		pty_resize(&g->pr.p, sizes[i].cols, sizes[i].rows);
		Sleep(250);
	}
	/* back at the first size: the colours are still there, the program still answers */
	Sleep(300);
	ok = ok &&
	        syn_wait_colour(g, "\"key\"", WF_THEME_SPADE, WF_SC_KEY, 8000);
	ev_check(e, "syntax_resize_colours_survive_every_size", 0,
	        "key colour after the resizes", ok ? "observed" : "missing",
	        ok);
	pty_send(&g->pr.p, "j", 1);
	ok = ok && syn_text(g, "count: 42", 8000);
	ev_check(e, "syntax_resize_program_still_answers", 0,
	        "the next file is shown", ok ? "observed" : "missing", ok);
	return ok;
}

static bool syn_scenario(Ev *e)
{
	static const SynInstall core = {false, NULL, NULL, NULL, false};
	static const SynInstall json_only = {true, "json", NULL, NULL, false};
	static const SynInstall big = {true, "json", NULL, NULL, true};
	static const SynInstall all = {true, NULL, NULL, NULL, false};
	static const SynInstall corrupt = {
	        true, "json;yaml", "json", NULL, false};
	static const SynInstall swapped = {
	        true, "json;yaml", NULL, "json", false};
	bool ok = true;

	ok &= syn_case(e, "syntax_core_only", &core, syn_steps_core);
	ok &= syn_case(e, "syntax_json_only", &json_only, syn_steps_json_only);
	ok &= syn_case(e, "syntax_all_grammars", &all, syn_steps_all);
	ok &= syn_case(
	        e, "syntax_corrupt_grammar", &corrupt, syn_steps_refused);
	ok &= syn_case(
	        e, "syntax_replaced_grammar", &swapped, syn_steps_refused);
	ok &= syn_case(e, "syntax_theme_change", &json_only, syn_steps_theme);
	ok &= syn_case(e, "syntax_editor", &json_only, syn_steps_editor);
	ok &= syn_case(e, "syntax_over_the_limit", &big, syn_steps_big);
	ok &= syn_case(e, "syntax_resize", &all, syn_steps_resize);
	return ok;
}

static int scn_syntax(Ev *e)
{
	return syn_scenario(e) ? (g_gate_skips ? 77 : 0) : 1;
}
