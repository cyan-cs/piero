#include "wf_manifest.h"

#include <string.h>

typedef struct
{
	const char *p, *end;
	unsigned line;
} Rd;

static bool is_space(char c)
{
	return ' ' == c || '\t' == c;
}

static void skip_space(Rd *r)
{
	while (r->p < r->end && is_space(*r->p)) {
		r->p++;
	}
}

/* A quoted ASCII string with no backslash into out[cap]. */
static WfMfError read_string(Rd *r, char *out, size_t cap)
{
	size_t n = 0;

	if (r->p >= r->end || '"' != *r->p) {
		return WF_MF_E_SYNTAX;
	}
	r->p++;
	while (r->p < r->end && '"' != *r->p) {
		unsigned char c = (unsigned char)*r->p;

		if ('\n' == c || '\r' == c || '\\' == c) {
			return WF_MF_E_SYNTAX;
		}
		if (n + 1 >= cap) {
			return WF_MF_E_LENGTH;
		}
		out[n++] = (char)c;
		r->p++;
	}
	if (r->p >= r->end) {
		return WF_MF_E_SYNTAX;
	}
	r->p++; /* the closing quote */
	out[n] = '\0';
	return WF_MF_OK;
}

static WfMfError read_uint(Rd *r, uint32_t *out)
{
	uint32_t v = 0;
	size_t digits = 0;

	while (r->p < r->end && *r->p >= '0' && *r->p <= '9') {
		if (digits >= 6) {
			return WF_MF_E_RANGE;
		}
		v = v * 10u + (uint32_t)(*r->p - '0');
		r->p++;
		digits++;
	}
	if (!digits) {
		return WF_MF_E_SYNTAX;
	}
	*out = v;
	return WF_MF_OK;
}

/* A bare file name: [A-Za-z0-9._-], not starting with '.', no "..". */
static bool plain_filename(const char *s)
{
	size_t n = strlen(s);

	if (!n || '.' == s[0]) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		char c = s[i];

		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		            (c >= '0' && c <= '9') || '.' == c || '_' == c ||
		            '-' == c)) {
			return false;
		}
		if ('.' == c && i + 1 < n && '.' == s[i + 1]) {
			return false;
		}
	}
	return true;
}

static bool valid_ext(const char *e)
{
	size_t n = strlen(e);

	if (n < 2 || '.' != e[0]) {
		return false;
	}
	for (size_t i = 1; i < n; i++) {
		char c = e[i];

		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
		            '-' == c || '_' == c)) {
			return false;
		}
	}
	return true;
}

enum
{
	K_ID,
	K_VERSION,
	K_ABI,
	K_LIBRARY,
	K_ENTRY,
	K_EXTS,
	K_QUERY,
	K_RELEASE,
	K_COUNT
};

static const char *const key_names[K_COUNT] = {"id", "version", "abi",
        "library", "entrypoint", "extensions", "query", "release"};

static WfMfError read_exts(Rd *r, WfManifest *m)
{
	if (r->p >= r->end || '[' != *r->p) {
		return WF_MF_E_SYNTAX;
	}
	r->p++;
	m->nexts = 0;
	skip_space(r);
	if (r->p < r->end && ']' == *r->p) {
		r->p++;
		return WF_MF_OK;
	}
	for (;;) {
		WfMfError e;

		skip_space(r);
		if (m->nexts >= WF_MANIFEST_MAX_EXTS) {
			return WF_MF_E_RANGE;
		}
		e = read_string(r, m->exts[m->nexts], sizeof m->exts[0]);
		if (WF_MF_OK != e) {
			return e;
		}
		if (!valid_ext(m->exts[m->nexts])) {
			return WF_MF_E_FILENAME;
		}
		m->nexts++;
		skip_space(r);
		if (r->p < r->end && ',' == *r->p) {
			r->p++;
			continue;
		}
		if (r->p < r->end && ']' == *r->p) {
			r->p++;
			return WF_MF_OK;
		}
		return WF_MF_E_SYNTAX;
	}
}

static unsigned line_of(const char *text, size_t at)
{
	unsigned line = 1;

	for (size_t k = 0; k < at; k++) {
		line += '\n' == text[k];
	}
	return line;
}

WfMfError wf_manifest_parse(
        const char *text, size_t n, WfManifest *out, unsigned *line)
{
	Rd r = {text, text + n, 1};
	unsigned seen = 0;
	WfMfError e = WF_MF_OK;

	if (line) {
		*line = 0;
	}
	if (!out || !text) {
		return WF_MF_E_SYNTAX;
	}
	memset(out, 0, sizeof *out);
	if (!n || n > WF_MANIFEST_MAX_BYTES) {
		return WF_MF_E_SIZE;
	}
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)text[i];

		if (c > 0x7e ||
		        (c < 0x20 && '\n' != c && '\r' != c && '\t' != c)) {
			if (line) {
				*line = line_of(text, i);
			}
			return WF_MF_E_ENCODING;
		}
	}
	while (r.p < r.end) {
		char key[16];
		size_t kn = 0;
		int k;

		skip_space(&r);
		if (r.p >= r.end) {
			break;
		}
		if ('\r' == *r.p || '\n' == *r.p) {
			if ('\r' == *r.p) {
				r.p++;
			}
			if (r.p < r.end && '\n' == *r.p) {
				r.p++;
			}
			r.line++;
			continue;
		}
		if ('#' == *r.p) {
			while (r.p < r.end && '\n' != *r.p) {
				r.p++;
			}
			continue;
		}
		while (r.p < r.end && *r.p >= 'a' && *r.p <= 'z') {
			if (kn + 1 >= sizeof key) {
				e = WF_MF_E_LENGTH;
				goto fail;
			}
			key[kn++] = *r.p++;
		}
		key[kn] = '\0';
		for (k = 0; k < K_COUNT; k++) {
			if (!strcmp(key, key_names[k])) {
				break;
			}
		}
		if (!kn || K_COUNT == k) {
			e = kn ? WF_MF_E_UNKNOWN_KEY : WF_MF_E_SYNTAX;
			goto fail;
		}
		if (seen & (1u << k)) {
			e = WF_MF_E_DUPLICATE;
			goto fail;
		}
		seen |= 1u << k;
		skip_space(&r);
		if (r.p >= r.end || '=' != *r.p) {
			e = WF_MF_E_SYNTAX;
			goto fail;
		}
		r.p++;
		skip_space(&r);
		switch (k) {
		case K_ID: e = read_string(&r, out->id, sizeof out->id); break;
		case K_VERSION:
			e = read_string(&r, out->version, sizeof out->version);
			break;
		case K_RELEASE:
			e = read_string(&r, out->release, sizeof out->release);
			break;
		case K_LIBRARY:
			e = read_string(&r, out->library, sizeof out->library);
			if (WF_MF_OK == e && !plain_filename(out->library)) {
				e = WF_MF_E_FILENAME;
			}
			break;
		case K_QUERY:
			e = read_string(&r, out->query, sizeof out->query);
			if (WF_MF_OK == e && !plain_filename(out->query)) {
				e = WF_MF_E_FILENAME;
			}
			break;
		case K_ENTRY:
			e = read_string(
			        &r, out->entrypoint, sizeof out->entrypoint);
			break;
		case K_ABI:  e = read_uint(&r, &out->abi); break;
		case K_EXTS: e = read_exts(&r, out); break;
		default:     e = WF_MF_E_SYNTAX; break;
		}
		if (WF_MF_OK != e) {
			goto fail;
		}
		skip_space(&r);
		if (r.p < r.end && '#' == *r.p) {
			while (r.p < r.end && '\n' != *r.p) {
				r.p++;
			}
		}
		if (r.p < r.end && '\r' != *r.p && '\n' != *r.p) {
			e = WF_MF_E_SYNTAX; /* something after the value */
			goto fail;
		}
	}
	if (seen != (1u << K_COUNT) - 1u) {
		memset(out, 0, sizeof *out);
		return WF_MF_E_MISSING;
	}
	return WF_MF_OK;
fail:
	if (line) {
		*line = r.line;
	}
	memset(out, 0, sizeof *out);
	return e;
}

static bool has_ext(const char *list, const char *ext)
{
	size_t n = strlen(ext);

	while (list && *list) {
		const char *end = strchr(list, ';');
		size_t len = end ? (size_t)(end - list) : strlen(list);

		if (len == n && !memcmp(list, ext, n)) {
			return true;
		}
		list = end ? end + 1 : NULL;
	}
	return false;
}

static size_t count_list(const char *list)
{
	size_t n = 0;

	while (list && *list) {
		const char *end = strchr(list, ';');

		n++;
		list = end ? end + 1 : NULL;
	}
	return n;
}

WfMfError wf_manifest_agrees(const WfManifest *m, const WfSynLang *lang)
{
	if (!m || !lang || lang->builtin) {
		return WF_MF_E_DISAGREES;
	}
	if (strcmp(m->id, lang->id) || strcmp(m->entrypoint, lang->entry) ||
	        strcmp(m->library, "parser.dll") ||
	        strcmp(m->query, "highlights.scm")) {
		return WF_MF_E_DISAGREES;
	}
	if (m->nexts != count_list(lang->exts)) {
		return WF_MF_E_DISAGREES;
	}
	for (size_t i = 0; i < m->nexts; i++) {
		if (!has_ext(lang->exts, m->exts[i])) {
			return WF_MF_E_DISAGREES;
		}
		for (size_t j = 0; j < i; j++) {
			if (!strcmp(m->exts[i], m->exts[j])) {
				return WF_MF_E_DISAGREES;
			}
		}
	}
	return WF_MF_OK;
}

const char *wf_manifest_error_name(WfMfError e)
{
	static const char *const names[] = {"ok", "size", "encoding", "syntax",
	        "duplicate-key", "unknown-key", "length", "range", "file-name",
	        "missing-key", "disagrees-with-the-table"};

	return (unsigned)e < sizeof names / sizeof names[0] ? names[e]
	                                                    : "unknown";
}
