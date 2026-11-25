#include "wf_esyntax.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* What carries from one line to the next. */
enum
{
	S_NORMAL = 0,
	S_BLOCK,  /* inside a C block comment */
	S_FENCE,  /* inside a Markdown code fence */
	S_PSBLOCK /* inside a PowerShell block comment */
};

static bool ext_is(const wchar_t *ext, const wchar_t *const *list)
{
	for (; *list; list++) {
		if (!_wcsicmp(ext, *list)) {
			return true;
		}
	}
	return false;
}

WfLang wf_syn_lang_for_name(const wchar_t *name)
{
	static const wchar_t *const c[] = {L".c", L".h", NULL};
	static const wchar_t *const cpp[] = {L".cpp", L".cc", L".cxx", L".hpp",
	        L".hh", L".hxx", L".ino", NULL};
	static const wchar_t *const json[] = {L".json", L".jsonc", NULL};
	static const wchar_t *const yaml[] = {L".yml", L".yaml", NULL};
	static const wchar_t *const ini[] = {
	        L".ini", L".cfg", L".conf", L".properties", L".toml", NULL};
	static const wchar_t *const md[] = {L".md", L".markdown", NULL};
	static const wchar_t *const ps[] = {L".ps1", L".psm1", L".psd1", NULL};
	const wchar_t *dot = name ? wcsrchr(name, L'.') : NULL;

	if (!dot) {
		return WF_LANG_TEXT;
	}
	if (ext_is(dot, c)) {
		return WF_LANG_C;
	}
	if (ext_is(dot, cpp)) {
		return WF_LANG_CPP;
	}
	if (ext_is(dot, json)) {
		return WF_LANG_JSON;
	}
	if (ext_is(dot, yaml)) {
		return WF_LANG_YAML;
	}
	if (ext_is(dot, ini)) {
		return WF_LANG_INI;
	}
	if (ext_is(dot, md)) {
		return WF_LANG_MARKDOWN;
	}
	if (ext_is(dot, ps)) {
		return WF_LANG_POWERSHELL;
	}
	return WF_LANG_TEXT;
}

const char *wf_syn_lang_name(WfLang lang)
{
	switch (lang) {
	case WF_LANG_C:          return "C";
	case WF_LANG_CPP:        return "C++";
	case WF_LANG_JSON:       return "JSON";
	case WF_LANG_YAML:       return "YAML";
	case WF_LANG_INI:        return "INI";
	case WF_LANG_MARKDOWN:   return "Markdown";
	case WF_LANG_POWERSHELL: return "PowerShell";
	case WF_LANG_TEXT:       break;
	}
	return "Text";
}

const char *wf_syn_comment_prefix(WfLang lang)
{
	switch (lang) {
	case WF_LANG_C:
	case WF_LANG_CPP:        return "//";
	case WF_LANG_YAML:
	case WF_LANG_POWERSHELL: return "#";
	case WF_LANG_INI:        return ";";
	default: return NULL; /* JSON and Markdown have no line comments */
	}
}

static bool word_start(unsigned char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || '_' == c;
}

static bool word_char(unsigned char c)
{
	return word_start(c) || (c >= '0' && c <= '9');
}

static bool digit(unsigned char c)
{
	return c >= '0' && c <= '9';
}

static void paint(uint8_t *out, size_t from, size_t to, WfTok tok)
{
	if (to > from) {
		memset(out + from, tok, to - from);
	}
}

static bool in_list(const char *w, size_t n, const char *const *list)
{
	for (; *list; list++) {
		if (strlen(*list) == n && !memcmp(*list, w, n)) {
			return true;
		}
	}
	return false;
}

static bool hex_digit(unsigned char c)
{
	return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* Bytes of the escape sequence that starts with the backslash at s[i]:
 * \uXXXX, \UXXXXXXXX, \xH.., \ooo, or a backslash and one character. */
static size_t escape_len(const char *s, size_t n, size_t i)
{
	size_t k = i + 2, want;

	if ('u' == s[i + 1] || 'U' == s[i + 1]) {
		want = 'u' == s[i + 1] ? 4 : 8;
		while (k < n && k < i + 2 + want &&
		        hex_digit((unsigned char)s[k])) {
			k++;
		}
		return k - i;
	}
	if ('x' == s[i + 1]) {
		while (k < n && k < i + 10 && hex_digit((unsigned char)s[k])) {
			k++;
		}
		return k - i;
	}
	if (s[i + 1] >= '0' && s[i + 1] <= '7') {
		while (k < n && k < i + 4 && s[k] >= '0' && s[k] <= '7') {
			k++;
		}
		return k - i;
	}
	return 2;
}

/* A quoted string starting at s[i] (quote q); returns the index after it.
 * `esc`: backslash escapes. The escapes are painted as ESCAPE. */
static size_t lex_string(
        const char *s, size_t n, size_t i, char q, bool esc, uint8_t *out)
{
	size_t start = i;

	i++;
	while (i < n && s[i] != q) {
		if (esc && '\\' == s[i] && i + 1 < n) {
			size_t len = escape_len(s, n, i);

			paint(out, i, i + len, WF_TOK_ESCAPE);
			i += len;
			continue;
		}
		i++;
	}
	if (i < n) {
		i++; /* the closing quote */
	}
	/* paint the string, keeping the escapes painted above */
	{
		size_t k;

		for (k = start; k < i; k++) {
			if (WF_TOK_ESCAPE != out[k]) {
				out[k] = WF_TOK_STRING;
			}
		}
	}
	return i;
}

static size_t lex_number(const char *s, size_t n, size_t i)
{
	bool hex = '0' == s[i] && i + 1 < n &&
	        ('x' == s[i + 1] || 'X' == s[i + 1]);

	while (i < n) {
		unsigned char c = (unsigned char)s[i];

		if (word_char(c) || '.' == c || '\'' == c) {
			i++;
		} else if (('+' == c || '-' == c) && i > 0 &&
		        ((!hex && ('e' == s[i - 1] || 'E' == s[i - 1])) ||
		                'p' == s[i - 1] || 'P' == s[i - 1])) {
			i++;
		} else {
			break;
		}
	}
	return i;
}

static const char *const C_KEYWORDS[] = {"auto", "break", "case", "const",
        "continue", "default", "do", "else", "enum", "extern", "for", "goto",
        "if", "inline", "register", "restrict", "return", "sizeof", "static",
        "struct", "switch", "typedef", "union", "volatile", "while", "_Alignas",
        "_Alignof", "_Atomic", "_Complex", "_Generic", "_Noreturn",
        "_Static_assert", "_Thread_local", "NULL", "true", "false", NULL};
static const char *const CPP_KEYWORDS[] = {"class", "namespace", "template",
        "typename", "using", "new", "delete", "try", "catch", "throw", "public",
        "private", "protected", "virtual", "override", "final", "constexpr",
        "noexcept", "nullptr", "operator", "this", "static_cast",
        "dynamic_cast", "reinterpret_cast", "const_cast", "explicit", "friend",
        "mutable", "decltype", "and", "or", "not", NULL};
static const char *const C_TYPES[] = {"void", "char", "short", "int", "long",
        "float", "double", "signed", "unsigned", "bool", "_Bool", "size_t",
        "ssize_t", "ptrdiff_t", "wchar_t", "FILE", "va_list", "HANDLE", "DWORD",
        "BOOL", "WORD", "BYTE", "LPVOID", "LPCWSTR", "LPWSTR", "WCHAR",
        "HRESULT", "string", "auto", NULL};

static WfTok c_word(const char *w, size_t n, bool cpp)
{
	if (in_list(w, n, C_KEYWORDS) || (cpp && in_list(w, n, CPP_KEYWORDS))) {
		return WF_TOK_KEYWORD;
	}
	if (in_list(w, n, C_TYPES) || (n > 2 && !memcmp(w + n - 2, "_t", 2))) {
		return WF_TOK_TYPE;
	}
	return WF_TOK_PLAIN;
}

static uint8_t lex_c(
        const char *s, size_t n, uint8_t st, uint8_t *out, bool cpp)
{
	size_t i = 0;
	bool preproc = false;

	if (S_BLOCK == st) {
		while (i < n) {
			if ('*' == s[i] && i + 1 < n && '/' == s[i + 1]) {
				paint(out, 0, i + 2, WF_TOK_COMMENT);
				i += 2;
				st = S_NORMAL;
				break;
			}
			i++;
		}
		if (S_BLOCK == st) {
			paint(out, 0, n, WF_TOK_COMMENT);
			return S_BLOCK;
		}
	} else {
		size_t k = 0;

		while (k < n && (' ' == s[k] || '\t' == s[k])) {
			k++;
		}
		preproc = k < n && '#' == s[k];
	}
	while (i < n) {
		unsigned char c = (unsigned char)s[i];

		if ('/' == c && i + 1 < n && '/' == s[i + 1]) {
			paint(out, i, n, WF_TOK_COMMENT);
			return S_NORMAL;
		}
		if ('/' == c && i + 1 < n && '*' == s[i + 1]) {
			size_t j = i + 2;

			while (j + 1 < n && !('*' == s[j] && '/' == s[j + 1])) {
				j++;
			}
			if (j + 1 < n) {
				paint(out, i, j + 2, WF_TOK_COMMENT);
				i = j + 2;
				continue;
			}
			paint(out, i, n, WF_TOK_COMMENT);
			return S_BLOCK;
		}
		if ('"' == c || '\'' == c) {
			i = lex_string(s, n, i, (char)c, true, out);
			continue;
		}
		if (preproc) {
			if ('<' == c && i > 0) { /* #include <name> */
				size_t j = i;

				while (j < n && '>' != s[j]) {
					j++;
				}
				if (j < n) {
					j++;
				}
				paint(out, i, j, WF_TOK_STRING);
				i = j;
				continue;
			}
			out[i++] = WF_TOK_PREPROC;
			continue;
		}
		if (digit(c) ||
		        ('.' == c && i + 1 < n &&
		                digit((unsigned char)s[i + 1]))) {
			size_t j = lex_number(s, n, i);

			paint(out, i, j, WF_TOK_NUMBER);
			i = j;
			continue;
		}
		if (word_start(c)) {
			size_t j = i;

			while (j < n && word_char((unsigned char)s[j])) {
				j++;
			}
			paint(out, i, j, c_word(s + i, j - i, cpp));
			i = j;
			continue;
		}
		out[i++] = WF_TOK_PLAIN;
	}
	return S_NORMAL;
}

static uint8_t lex_json(const char *s, size_t n, uint8_t *out)
{
	size_t i = 0;

	while (i < n) {
		unsigned char c = (unsigned char)s[i];

		if ('"' == c) {
			size_t j = lex_string(s, n, i, '"', true, out), k = j;

			while (k < n && (' ' == s[k] || '\t' == s[k])) {
				k++;
			}
			if (k < n && ':' == s[k]) { /* a key */
				size_t q;

				for (q = i; q < j; q++) {
					if (WF_TOK_STRING == out[q]) {
						out[q] = WF_TOK_KEY;
					}
				}
			}
			i = j;
			continue;
		}
		if (digit(c) ||
		        ('-' == c && i + 1 < n &&
		                digit((unsigned char)s[i + 1]))) {
			size_t j = lex_number(s, n, i + 1);

			paint(out, i, j, WF_TOK_NUMBER);
			i = j;
			continue;
		}
		if (word_start(c)) {
			size_t j = i;

			while (j < n && word_char((unsigned char)s[j])) {
				j++;
			}
			if ((j - i == 4 &&
			            (!memcmp(s + i, "true", 4) ||
			                    !memcmp(s + i, "null", 4))) ||
			        (j - i == 5 && !memcmp(s + i, "false", 5))) {
				paint(out, i, j, WF_TOK_KEYWORD);
			} else {
				paint(out, i, j, WF_TOK_PLAIN);
			}
			i = j;
			continue;
		}
		out[i++] = WF_TOK_PLAIN;
	}
	return S_NORMAL;
}

static bool bare_literal(const char *w, size_t n)
{
	static const char *const L[] = {"true", "false", "null", "yes", "no",
	        "on", "off", "True", "False", "Null", "~", NULL};

	return in_list(w, n, L);
}

/* A scalar value: quotes, numbers and the bare literals. */
static void lex_value(
        const char *s, size_t n, size_t i, uint8_t *out, bool comments)
{
	while (i < n) {
		unsigned char c = (unsigned char)s[i];

		if (comments && '#' == c &&
		        (0 == i || ' ' == s[i - 1] || '\t' == s[i - 1])) {
			paint(out, i, n, WF_TOK_COMMENT);
			return;
		}
		if ('"' == c || '\'' == c) {
			i = lex_string(s, n, i, (char)c, '"' == c, out);
			continue;
		}
		if (digit(c) ||
		        (('-' == c || '+' == c) && i + 1 < n &&
		                digit((unsigned char)s[i + 1]))) {
			size_t j = lex_number(s, n, i + 1);

			paint(out, i, j, WF_TOK_NUMBER);
			i = j;
			continue;
		}
		if (word_start(c)) {
			size_t j = i;

			while (j < n &&
			        (word_char((unsigned char)s[j]) ||
			                '-' == s[j] || '.' == s[j])) {
				j++;
			}
			paint(out, i, j,
			        bare_literal(s + i, j - i) ? WF_TOK_KEYWORD
			                                   : WF_TOK_PLAIN);
			i = j;
			continue;
		}
		out[i++] = WF_TOK_PLAIN;
	}
}

static uint8_t lex_yaml(const char *s, size_t n, uint8_t *out)
{
	size_t i = 0, key_end;

	while (i < n && (' ' == s[i] || '\t' == s[i])) {
		i++;
	}
	if (i < n && '#' == s[i]) {
		paint(out, i, n, WF_TOK_COMMENT);
		return S_NORMAL;
	}
	if (n >= 3 && (!memcmp(s, "---", 3) || !memcmp(s, "...", 3))) {
		paint(out, 0, n, WF_TOK_MARKUP);
		return S_NORMAL;
	}
	while (i + 1 < n && '-' == s[i] && ' ' == s[i + 1]) { /* list items */
		out[i] = WF_TOK_MARKUP;
		i += 2;
		while (i < n && ' ' == s[i]) {
			i++;
		}
	}
	/* "key:" followed by a space or the end, outside quotes */
	key_end = i;
	if (i < n && '"' != s[i] && '\'' != s[i]) {
		while (key_end < n) {
			if (':' == s[key_end] &&
			        (key_end + 1 == n || ' ' == s[key_end + 1] ||
			                '\t' == s[key_end + 1])) {
				break;
			}
			if ('#' == s[key_end] && key_end > 0 &&
			        ' ' == s[key_end - 1]) {
				key_end = n;
				break;
			}
			key_end++;
		}
	} else {
		key_end = n;
	}
	if (key_end < n) {
		paint(out, i, key_end, WF_TOK_KEY);
		out[key_end] = WF_TOK_PLAIN;
		lex_value(s, n, key_end + 1, out, true);
	} else {
		lex_value(s, n, i, out, true);
	}
	return S_NORMAL;
}

static uint8_t lex_ini(const char *s, size_t n, uint8_t *out)
{
	size_t i = 0, eq;

	while (i < n && (' ' == s[i] || '\t' == s[i])) {
		i++;
	}
	if (i < n && (';' == s[i] || '#' == s[i])) {
		paint(out, i, n, WF_TOK_COMMENT);
		return S_NORMAL;
	}
	if (i < n && '[' == s[i]) {
		paint(out, i, n, WF_TOK_HEADING);
		return S_NORMAL;
	}
	for (eq = i; eq < n && '=' != s[eq]; eq++) {
	}
	if (eq < n) {
		paint(out, i, eq, WF_TOK_KEY);
		out[eq] = WF_TOK_PLAIN;
		lex_value(s, n, eq + 1, out, false);
		return S_NORMAL;
	}
	lex_value(s, n, i, out, false);
	return S_NORMAL;
}

/* "Where is the next `ch` at or after `from`", remembered: no `ch` lies in
 * [from, at). The inline loop below asks with a growing `from`, so every byte
 * is looked at once per character searched for, not once per candidate (a line
 * of 10000 "[" must not cost 10000 x 10000). */
typedef struct
{
	size_t from, at;
	bool set;
} Seek;

static size_t seek(const char *s, size_t n, size_t from, char ch, Seek *c)
{
	size_t j = from;

	if (c->set && from >= c->from && from <= c->at) {
		return c->at;
	}
	while (j < n && s[j] != ch) {
		j++;
	}
	c->from = from;
	c->at = j;
	c->set = true;
	return j;
}

static uint8_t lex_markdown(const char *s, size_t n, uint8_t st, uint8_t *out)
{
	size_t i = 0;
	size_t run_end = 0;
	Seek tick = {0}, star = {0}, under = {0}, close_b = {0}, close_p = {0};
	bool fence;

	while (i < n && ' ' == s[i]) {
		i++;
	}
	fence = i + 3 <= n &&
	        (!memcmp(s + i, "```", 3) || !memcmp(s + i, "~~~", 3));
	if (S_FENCE == st) {
		paint(out, 0, n, fence ? WF_TOK_MARKUP : WF_TOK_STRING);
		return fence ? S_NORMAL : S_FENCE;
	}
	if (fence) {
		paint(out, 0, n, WF_TOK_MARKUP);
		return S_FENCE;
	}
	if (i < n && '#' == s[i]) {
		size_t h = i;

		while (h < n && '#' == s[h]) {
			h++;
		}
		if (h - i <= 6 && (h == n || ' ' == s[h])) {
			paint(out, 0, n, WF_TOK_HEADING);
			return S_NORMAL;
		}
	}
	if (i < n && '>' == s[i]) {
		paint(out, 0, n, WF_TOK_COMMENT);
		return S_NORMAL;
	}
	if (n - i >= 3 &&
	        (!memcmp(s + i, "---", 3) || !memcmp(s + i, "***", 3) ||
	                !memcmp(s + i, "___", 3))) {
		size_t k;

		for (k = i; k < n && (s[k] == s[i] || ' ' == s[k]); k++) {
		}
		if (k == n) {
			paint(out, 0, n, WF_TOK_MARKUP);
			return S_NORMAL;
		}
	}
	if (i + 1 < n && ('-' == s[i] || '*' == s[i] || '+' == s[i]) &&
	        ' ' == s[i + 1]) {
		out[i] = WF_TOK_MARKUP;
		i += 1;
	} else {
		size_t k = i;

		while (k < n && digit((unsigned char)s[k])) {
			k++;
		}
		if (k > i && k + 1 < n && '.' == s[k] && ' ' == s[k + 1]) {
			paint(out, i, k + 1, WF_TOK_MARKUP);
			i = k + 1;
		}
	}
	while (i < n) {
		unsigned char c = (unsigned char)s[i];

		if ('`' == c) {
			size_t j = seek(s, n, i + 1, '`', &tick);

			if (j < n) {
				paint(out, i, j + 1, WF_TOK_STRING);
				i = j + 1;
				continue;
			}
		} else if ('*' == c || '_' == c) {
			size_t d = i, j;

			if (i < run_end) {
				d = run_end;
			} else {
				while (d < n && s[d] == (char)c) {
					d++;
				}
				run_end = d;
			}
			j = seek(s, n, d, (char)c, '*' == c ? &star : &under);
			if (j < n && j > d && ' ' != s[d]) {
				size_t e = j;

				while (e < n && s[e] == (char)c &&
				        e - j < d - i) {
					e++;
				}
				paint(out, i, e, WF_TOK_MARKUP);
				i = e;
				continue;
			}
		} else if ('[' == c) {
			size_t j = seek(s, n, i + 1, ']', &close_b);

			if (j + 1 < n && '(' == s[j + 1]) {
				size_t k = seek(s, n, j + 2, ')', &close_p);

				if (k < n) {
					paint(out, i, j + 1, WF_TOK_KEY);
					paint(out, j + 1, k + 1, WF_TOK_STRING);
					i = k + 1;
					continue;
				}
			}
		} else if ('|' == c) {
			out[i++] = WF_TOK_MARKUP;
			continue;
		}
		out[i++] = WF_TOK_PLAIN;
	}
	return S_NORMAL;
}

static const char *const PS_KEYWORDS[] = {"begin", "break", "catch", "class",
        "continue", "data", "do", "dynamicparam", "else", "elseif", "end",
        "exit", "filter", "finally", "for", "foreach", "from", "function", "if",
        "in", "param", "process", "return", "switch", "throw", "trap", "try",
        "until", "using", "var", "while", "workflow", "true", "false", "null",
        NULL};
static const char *const PS_VERBS[] = {"Get", "Set", "New", "Remove", "Write",
        "Read", "Invoke", "Start", "Stop", "Test", "Add", "Out", "Select",
        "Where", "ForEach", "Import", "Export", "Copy", "Move", "Join", "Split",
        "Format", "Convert", "Clear", "Enable", "Disable", "Install", "Update",
        "Wait", "Measure", "Sort", "Group", "Compare", NULL};

static uint8_t lex_ps(const char *s, size_t n, uint8_t st, uint8_t *out)
{
	size_t i = 0;

	if (S_PSBLOCK == st) {
		while (i + 1 < n) {
			if ('#' == s[i] && '>' == s[i + 1]) {
				paint(out, 0, i + 2, WF_TOK_COMMENT);
				i += 2;
				st = S_NORMAL;
				break;
			}
			i++;
		}
		if (S_PSBLOCK == st) {
			paint(out, 0, n, WF_TOK_COMMENT);
			return S_PSBLOCK;
		}
	}
	while (i < n) {
		unsigned char c = (unsigned char)s[i];

		if ('<' == c && i + 1 < n && '#' == s[i + 1]) {
			size_t j = i + 2;

			while (j + 1 < n && !('#' == s[j] && '>' == s[j + 1])) {
				j++;
			}
			if (j + 1 < n) {
				paint(out, i, j + 2, WF_TOK_COMMENT);
				i = j + 2;
				continue;
			}
			paint(out, i, n, WF_TOK_COMMENT);
			return S_PSBLOCK;
		}
		if ('#' == c) {
			paint(out, i, n, WF_TOK_COMMENT);
			return S_NORMAL;
		}
		if ('\'' == c) {
			size_t j = i + 1;

			while (j < n) {
				if ('\'' == s[j]) {
					if (j + 1 < n && '\'' == s[j + 1]) {
						j += 2;
						continue;
					}
					break;
				}
				j++;
			}
			if (j < n) {
				j++;
			}
			paint(out, i, j, WF_TOK_STRING);
			i = j;
			continue;
		}
		if ('"' == c) {
			size_t j = i + 1;

			out[i] = WF_TOK_STRING;
			while (j < n && '"' != s[j]) {
				if ('`' == s[j] && j + 1 < n) {
					paint(out, j, j + 2, WF_TOK_ESCAPE);
					j += 2;
					continue;
				}
				if ('$' == s[j] && j + 1 < n &&
				        word_start((unsigned char)s[j + 1])) {
					size_t k = j + 1;

					while (k < n &&
					        word_char(
					                (unsigned char)s[k])) {
						k++;
					}
					paint(out, j, k, WF_TOK_KEY);
					j = k;
					continue;
				}
				out[j++] = WF_TOK_STRING;
			}
			if (j < n) {
				out[j++] = WF_TOK_STRING;
			}
			i = j;
			continue;
		}
		if ('$' == c && i + 1 < n &&
		        (word_start((unsigned char)s[i + 1]) ||
		                '{' == s[i + 1])) {
			size_t j = i + 1;

			if ('{' == s[j]) {
				while (j < n && '}' != s[j]) {
					j++;
				}
				if (j < n) {
					j++;
				}
			} else {
				while (j < n &&
				        (word_char((unsigned char)s[j]) ||
				                ':' == s[j])) {
					j++;
				}
			}
			paint(out, i, j, WF_TOK_KEY);
			i = j;
			continue;
		}
		if (digit(c)) {
			size_t j = lex_number(s, n, i);

			paint(out, i, j, WF_TOK_NUMBER);
			i = j;
			continue;
		}
		if ('-' == c && i + 1 < n &&
		        word_start((unsigned char)s[i + 1]) &&
		        (0 == i || ' ' == s[i - 1] || '\t' == s[i - 1])) {
			size_t j = i + 1;

			while (j < n && word_char((unsigned char)s[j])) {
				j++;
			}
			paint(out, i, j, WF_TOK_KEYWORD);
			i = j;
			continue;
		}
		if (word_start(c)) {
			size_t j = i;
			bool cmdlet = false;

			while (j < n &&
			        (word_char((unsigned char)s[j]) ||
			                '-' == s[j])) {
				j++;
			}
			{
				const char *dash = memchr(s + i, '-', j - i);

				if (dash) {
					size_t vl = (size_t)(dash - (s + i));
					const char *const *v;

					for (v = PS_VERBS; *v; v++) {
						if (strlen(*v) == vl &&
						        !_strnicmp(*v, s + i,
						                vl)) {
							cmdlet = true;
						}
					}
				}
			}
			if (cmdlet) {
				paint(out, i, j, WF_TOK_TYPE);
			} else {
				size_t e = i;
				char low[16];
				size_t k;

				while (e < j &&
				        word_char((unsigned char)s[e])) {
					e++;
				}
				for (k = 0; k < e - i && k < sizeof low - 1;
				        k++) {
					low[k] = (char)((s[i + k] >= 'A' &&
					                        s[i + k] <= 'Z')
					                ? s[i + k] + 32
					                : s[i + k]);
				}
				low[k < sizeof low - 1 ? k : sizeof low - 1] =
				        '\0';
				paint(out, i, j,
				        e - i < sizeof low &&
				                        in_list(low,
				                                strlen(low),
				                                PS_KEYWORDS)
				                ? WF_TOK_KEYWORD
				                : WF_TOK_PLAIN);
			}
			i = j;
			continue;
		}
		out[i++] = WF_TOK_PLAIN;
	}
	return S_NORMAL;
}

static uint8_t lex(
        WfLang lang, const char *s, size_t n, uint8_t st, uint8_t *out)
{
	memset(out, WF_TOK_PLAIN, n);
	switch (lang) {
	case WF_LANG_C:          return lex_c(s, n, st, out, false);
	case WF_LANG_CPP:        return lex_c(s, n, st, out, true);
	case WF_LANG_JSON:       return lex_json(s, n, out);
	case WF_LANG_YAML:       return lex_yaml(s, n, out);
	case WF_LANG_INI:        return lex_ini(s, n, out);
	case WF_LANG_MARKDOWN:   return lex_markdown(s, n, st, out);
	case WF_LANG_POWERSHELL: return lex_ps(s, n, st, out);
	case WF_LANG_TEXT:       break;
	}
	return S_NORMAL;
}

void wf_syn_init(WfSyntax *s, WfLang lang)
{
	memset(s, 0, sizeof *s);
	s->lang = lang;
}

void wf_syn_free(WfSyntax *s)
{
	wf_ts_free(&s->ts);
	free(s->state);
	memset(s, 0, sizeof *s);
}

void wf_syn_invalidate(WfSyntax *s, size_t line)
{
	if (line < s->valid) {
		s->valid = line;
	}
}

/* The state after a line too long to lex. It is not coloured, so what it
 * contains is not looked at; only a Markdown fence line (which is recognised
 * from its first bytes) still toggles, so one huge line cannot swallow the
 * fence structure of the rest of the file. */
static uint8_t state_after_long_line(
        WfLang lang, const char *s, size_t n, uint8_t st)
{
	size_t i = 0;

	if (WF_LANG_MARKDOWN != lang) {
		return st;
	}
	while (i < n && i < 3 && ' ' == s[i]) {
		i++;
	}
	if (i + 3 <= n &&
	        (!memcmp(s + i, "```", 3) || !memcmp(s + i, "~~~", 3))) {
		return S_FENCE == st ? S_NORMAL : S_FENCE;
	}
	return st;
}

/* Brings the state at the start of `line` up to date. */
static bool ensure(WfSyntax *s, const WfEText *t, size_t line)
{
	uint8_t *scratch = NULL;
	size_t cap = 0;

	if (s->n < t->n + 1) {
		uint8_t *p = realloc(s->state, t->n + 1);

		if (!p) {
			return false;
		}
		s->state = p;
		s->n = t->n + 1;
	}
	if (s->valid > t->n + 1) {
		s->valid = t->n + 1;
	}
	if (0 == s->valid) {
		s->state[0] = S_NORMAL;
		s->valid = 1;
	}
	while (s->valid <= line) {
		const WfELine *l = &t->lines[s->valid - 1];

		if (l->len > WF_SYN_LINE_MAX) {
			s->state[s->valid] = state_after_long_line(
			        s->lang, l->s, l->len, s->state[s->valid - 1]);
			s->valid++;
			continue;
		}
		if (l->len > cap) {
			uint8_t *p = realloc(scratch, l->len);

			if (!p) {
				free(scratch);
				return false;
			}
			scratch = p;
			cap = l->len;
		}
		s->state[s->valid] = lex(
		        s->lang, l->s, l->len, s->state[s->valid - 1], scratch);
		s->valid++;
	}
	free(scratch);
	return true;
}

bool wf_syn_line(
        WfSyntax *s, const WfEText *t, size_t line, uint8_t *out, size_t cap)
{
	const WfELine *l;

	if (WF_TS_OFF != s->ts.state) { /* a grammar colours this buffer */
		return wf_ts_line(s, t, line, out, cap);
	}
	if (WF_LANG_TEXT == s->lang || line >= t->n) {
		return false;
	}
	l = &t->lines[line];
	if (l->len > cap || !ensure(s, t, line)) {
		return false;
	}
	{
		uint8_t end = lex(s->lang, l->s, l->len, s->state[line], out);

		/* The state this line ends in is the state the next one starts in. When
		 * it is the next one to be worked out, keep it: lines shown one after the
		 * other are then lexed once each, not once more to find where they end. */
		if (s->valid == line + 1) {
			s->state[line + 1] = end;
			s->valid++;
		}
	}
	return true;
}
