#include "wf_smartgo_path.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "wf_name.h"
#include "../base/wf_macro.h"
#include "../base/wf_mem.h"

#define PATH_MAX_UNITS 32000u
#define COMPONENT_MAX 255u

static const wchar_t *const alias_names[WF_SMART_ALIASES] = {L"home",
        L"desktop", L"downloads", L"documents", L"pictures", L"music",
        L"videos", L"appdata"};

const wchar_t *wf_smart_alias_name(size_t index)
{
	return index < WF_SMART_ALIASES ? alias_names[index] : NULL;
}

void wf_smart_result_free(WfSmartResult *r)
{
	if (!r) {
		return;
	}
	free(r->target);
	free(r->list_dir);
	free(r->prefix);
	memset(r, 0, sizeof *r);
}

const char *wf_smart_error_text(WfSmartError e)
{
	switch (e) {
	case WF_SG_OK:
	case WF_SG_EMPTY:
	case WF_SG_ALIAS_PARTIAL: return "";
	case WF_SG_INVALID:       return "Invalid path";
	case WF_SG_DRIVE_RELATIVE:
		return "Drive-relative paths are not supported (use "
		       "C:\\folder)";
	case WF_SG_ENV_UNDEFINED: return "Undefined variable";
	case WF_SG_NO_HOME:       return "Home folder unavailable";
	case WF_SG_NO_PREVIOUS:   return "No previous folder";
	case WF_SG_UNKNOWN_ALIAS: return "Unknown alias";
	case WF_SG_TOO_LONG:      return "Path too long";
	case WF_SG_NOMEM:         return "Out of memory";
	case WF_SG_WSL_NAME:      return "Type wsl:<distribution>";
	case WF_SG_SFTP_NAME:     return "Type sftp:<ssh host name>";
	}
	return "Invalid path";
}

static bool is_sep(wchar_t c)
{
	return L'\\' == c || L'/' == c;
}

static bool is_letter(wchar_t c)
{
	return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
}

static bool equal_ci(const wchar_t *a, size_t n, const wchar_t *b)
{
	size_t i;

	if (wcslen(b) != n) {
		return false;
	}
	for (i = 0; i < n; i++) {
		wchar_t x = a[i], y = b[i];

		if (x >= L'A' && x <= L'Z') {
			x = (wchar_t)(x - L'A' + L'a');
		}
		if (y >= L'A' && y <= L'Z') {
			y = (wchar_t)(y - L'A' + L'a');
		}
		if (x != y) {
			return false;
		}
	}
	return true;
}

typedef struct
{
	wchar_t *s;
	size_t n, cap;
} Text;

static void text_free(Text *t)
{
	free(t->s);
	memset(t, 0, sizeof *t);
}

static bool text_add(Text *t, const wchar_t *p, size_t n)
{
	if (n > SIZE_MAX / sizeof *t->s - t->n - 1) {
		return false;
	}
	if (t->n + n + 1 > t->cap) {
		size_t cap = t->cap ? t->cap : 64;
		wchar_t *grown;

		while (cap < t->n + n + 1) {
			if (cap > SIZE_MAX / 2 / sizeof *t->s) {
				return false;
			}
			cap *= 2;
		}
		grown = realloc(t->s, cap * sizeof *grown);
		if (!grown) {
			return false;
		}
		t->s = grown;
		t->cap = cap;
	}
	if (n) {
		memcpy(t->s + t->n, p, n * sizeof *p);
	}
	t->n += n;
	t->s[t->n] = L'\0';
	return true;
}

static bool text_addc(Text *t, wchar_t c)
{
	return text_add(t, &c, 1);
}

/*
 * Length of the root of the absolute path `p` (slashes already backslashes),
 * including its closing backslash when there is one; 0 if `p` does not start
 * with a complete root. *unc_incomplete is set for "\\server" and
 * "\\server\" (a start that needs a share name).
 */
static size_t root_length(const wchar_t *p, size_t n, bool *incomplete)
{
	size_t i, start;

	*incomplete = false;
	if (n >= 7 && !wcsncmp(p, L"\\\\?\\", 4)) {
		if (is_letter(p[4]) && L':' == p[5] && L'\\' == p[6]) {
			return 7;
		}
		if (n >= 8 && equal_ci(p + 4, 3, L"unc") && L'\\' == p[7]) {
			i = 8;
			start = i;
			while (i < n && L'\\' != p[i]) {
				i++;
			}
			if (i == start || i >= n) {
				*incomplete = true;
				return 0;
			}
			start = ++i;
			while (i < n && L'\\' != p[i]) {
				i++;
			}
			if (i == start) {
				*incomplete = true;
				return 0;
			}
			return i < n ? i + 1 : i;
		}
		return 0;
	}
	if (n >= 3 && is_letter(p[0]) && L':' == p[1] && L'\\' == p[2]) {
		return 3;
	}
	if (n >= 2 && L'\\' == p[0] && L'\\' == p[1]) {
		i = 2;
		start = i;
		while (i < n && L'\\' != p[i]) {
			i++;
		}
		if (i == start || i >= n) {
			*incomplete = true;
			return 0;
		}
		start = ++i;
		while (i < n && L'\\' != p[i]) {
			i++;
		}
		if (i == start) {
			*incomplete = true;
			return 0;
		}
		return i < n ? i + 1 : i;
	}
	return 0;
}

static bool has_forbidden(const wchar_t *p, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if ((uint16_t)p[i] < 32 || 127 == p[i]) {
			return true;
		}
	}
	return false;
}

/* %NAME% expanded in place of `src[0..n)` into `out`. A '%' without a closing
 * partner stays a '%'. */
static WfSmartError expand_env(const WfSmartEnv *env,
        const wchar_t *src,
        size_t n,
        Text *out,
        wchar_t name_out[48])
{
	size_t i = 0;

	while (i < n) {
		size_t close = i + 1;

		if (L'%' != src[i]) {
			if (!text_addc(out, src[i++])) {
				return WF_SG_NOMEM;
			}
			continue;
		}
		while (close < n && L'%' != src[close] && !is_sep(src[close])) {
			close++;
		}
		if (close >= n || L'%' != src[close] || close == i + 1) {
			if (!text_addc(out, src[i++])) {
				return WF_SG_NOMEM;
			}
			continue;
		}
		{
			wchar_t name[256], *value;
			size_t len = close - i - 1;

			if (len >= WF_ARRAY_LEN(name)) {
				return WF_SG_INVALID;
			}
			memcpy(name, src + i + 1, len * sizeof *name);
			name[len] = L'\0';
			value = env && env->getenv ? env->getenv(env->ctx, name)
			                           : NULL;
			if (!value) {
				wcsncpy_s(name_out, 48, name, _TRUNCATE);
				return WF_SG_ENV_UNDEFINED;
			}
			if (has_forbidden(value, wcslen(value))) {
				free(value);
				return WF_SG_INVALID;
			}
			if (!text_add(out, value, wcslen(value))) {
				free(value);
				return WF_SG_NOMEM;
			}
			free(value);
		}
		i = close + 1;
	}
	return WF_SG_OK;
}

typedef struct
{
	size_t start, len;
} Span;

typedef struct
{
	Span *v;
	size_t n, cap;
} Spans;

static bool spans_add(Spans *s, size_t start, size_t len)
{
	if (s->n == s->cap) {
		size_t cap = s->cap ? s->cap * 2 : 16;
		Span *grown;

		if (cap > SIZE_MAX / sizeof *grown) {
			return false;
		}
		grown = realloc(s->v, cap * sizeof *grown);
		if (!grown) {
			return false;
		}
		s->v = grown;
		s->cap = cap;
	}
	s->v[s->n].start = start;
	s->v[s->n].len = len;
	s->n++;
	return true;
}

/* Adds the components of `p[from..n)` to the list, handling "." and "..". */
static WfSmartError add_components(
        const wchar_t *p, size_t from, size_t n, Spans *list, size_t base)
{
	size_t i = from;

	while (i < n) {
		size_t start = i, len;

		while (i < n && L'\\' != p[i]) {
			i++;
		}
		len = i - start;
		if (i < n) {
			i++;
		}
		if (!len || (1 == len && L'.' == p[start])) {
			continue;
		}
		if (2 == len && L'.' == p[start] && L'.' == p[start + 1]) {
			if (list->n > base) {
				list->n--;
			}
			continue; /* at a root it stays there */
		}
		if (WF_NAME_OK !=
		        wf_name_validate(p + start, len, COMPONENT_MAX)) {
			return WF_SG_INVALID;
		}
		if (!spans_add(list, start, len)) {
			return WF_SG_NOMEM;
		}
	}
	return WF_SG_OK;
}

static wchar_t *dup_n(const wchar_t *p, size_t n)
{
	wchar_t *c = wf_malloc_array((n + 1), sizeof *c);

	if (!c) {
		return NULL;
	}
	memcpy(c, p, n * sizeof *c);
	c[n] = L'\0';
	return c;
}

/* root + the first `count` components, no trailing separator except on a
 * root. */
static wchar_t *build(const wchar_t *root,
        size_t root_len,
        const wchar_t *buf,
        const Span *v,
        size_t count)
{
	Text t = {0};
	size_t i;

	if (!text_add(&t, root, root_len)) {
		return NULL;
	}
	for (i = 0; i < count; i++) {
		if (i && !text_addc(&t, L'\\')) {
			break;
		}
		if (0 == i && root_len && L'\\' != root[root_len - 1] &&
		        !text_addc(&t, L'\\')) {
			break;
		}
		if (!text_add(&t, buf + v[i].start, v[i].len)) {
			break;
		}
		if (i + 1 < count) {
			continue;
		}
	}
	if (t.s && i < count) {
		text_free(&t);
		return NULL;
	}
	/* a share's root keeps no trailing separator (a drive's does) */
	if (t.s && !count && t.n > 2 && L'\\' == t.s[t.n - 1] &&
	        (!wcsncmp(t.s, L"\\\\?\\UNC\\", 8) ||
	                (!wcsncmp(t.s, L"\\\\", 2) &&
	                        wcsncmp(t.s, L"\\\\?\\", 4)))) {
		t.s[--t.n] = L'\0';
	}
	return t.s;
}

static void slashes(wchar_t *p, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (L'/' == p[i]) {
			p[i] = L'\\';
		}
	}
}

/* The start index of the last element of the user's original text. */
static size_t last_element_start(const wchar_t *t, size_t n)
{
	size_t i = n;

	while (i && !is_sep(t[i - 1])) {
		i--;
	}
	return i;
}

static bool wsl_distro_char(wchar_t c)
{
	return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') ||
	        (c >= L'A' && c <= L'Z') || L'.' == c || L'_' == c ||
	        L'-' == c || L'+' == c;
}

/*
 * Rewrites a wsl: or sftp: prefix into its UNC namespace, preserving case
 * and converting '/' to '\\'. wf_smart_resolve then validates components and
 * resolves "." / ".." as for other UNC input. Host/distribution names use
 * the same restricted alphabet and cannot start with '-'.
 */
static WfStatus resolve_scheme(const WfSmartEnv *env,
        const wchar_t *cwd,
        const wchar_t *previous,
        const wchar_t *text,
        size_t n,
        size_t quote_off,
        const wchar_t *head,
        size_t scheme_n,
        WfSmartError name_error,
        WfSmartResult *r)
{
	size_t head_n = wcslen(head), d = scheme_n, i, total;
	wchar_t *full;
	WfStatus st;

	while (d < n && !is_sep(text[d])) {
		if (!wsl_distro_char(text[d]) || d - scheme_n >= 128) {
			r->error = name_error;
			return WF_OK;
		}
		d++;
	}
	if (scheme_n == d || L'-' == text[scheme_n]) {
		r->error = name_error;
		return WF_OK;
	}
	if (n > SIZE_MAX / sizeof(wchar_t) - head_n - 2) {
		r->error = WF_SG_TOO_LONG;
		return WF_OK;
	}
	total = head_n + (n - scheme_n);
	full = malloc((total + 1) * sizeof *full);
	if (!full) {
		r->error = WF_SG_NOMEM;
		return WF_OK;
	}
	memcpy(full, head, head_n * sizeof *head);
	for (i = scheme_n; i < n; i++) {
		full[head_n + i - scheme_n] = L'/' == text[i] ? L'\\' : text[i];
	}
	full[total] = 0;
	st = wf_smart_resolve(env, cwd, previous, full, total, r);
	free(full);
	if (WF_OK == st && WF_SG_OK == r->error && r->prefix) {
		/* positions are in what the user typed, not in the rewrite */
		size_t p = wcslen(r->prefix);

		r->prefix_start = quote_off + (n >= p ? n - p : 0);
	}
	return st;
}

WfStatus wf_smart_resolve(const WfSmartEnv *env,
        const wchar_t *cwd,
        const wchar_t *previous,
        const wchar_t *text,
        size_t n,
        WfSmartResult *r)
{
	Text full = {0}, expanded = {0};
	Spans comps = {0};
	wchar_t *root = NULL;
	size_t root_len = 0, off = 0, rest_from = 0, full_root, i;
	size_t cwd_n = cwd ? wcslen(cwd) : 0;
	bool incomplete = false, whole = false, trailing_sep;
	WfSmartError err = WF_SG_OK;
	WfStatus st = WF_OK;

	memset(r, 0, sizeof *r);
	if (!cwd || !cwd_n || (!text && n)) {
		return WF_E_INVAL;
	}
	/* one matching pair of quotes (the way Explorer copies a path) */
	if (n >= 2 && L'"' == text[0] && L'"' == text[n - 1]) {
		text++;
		n -= 2;
		off = 1;
	}
	if (!n) {
		r->error = WF_SG_EMPTY;
		return WF_OK;
	}
	for (i = 0; i < n; i++) {
		if ((uint16_t)text[i] < 32 || 127 == text[i] ||
		        L'"' == text[i] || L'<' == text[i] || L'>' == text[i] ||
		        L'|' == text[i] || L'*' == text[i] ||
		        (L'?' == text[i] &&
		                !(2 == i && n >= 4 &&
		                        !wcsncmp(text, L"\\\\?\\", 4)) &&
		                !(2 == i && n >= 4 &&
		                        !wcsncmp(text, L"//?/", 4)))) {
			r->error = WF_SG_INVALID;
			return WF_OK;
		}
	}

	if (n >= 4 && !_wcsnicmp(text, L"wsl:", 4)) {
		return resolve_scheme(env, cwd, previous, text, n, off,
		        L"\\\\wsl.localhost\\", 4, WF_SG_WSL_NAME, r);
	}
	/* SFTP dispatch recognizes this stand-in namespace; it must not reach
	 * ordinary Win32 filesystem I/O. */
	if (n >= 5 && !_wcsnicmp(text, L"sftp:", 5)) {
		return resolve_scheme(env, cwd, previous, text, n, off,
		        L"\\\\sftp.invalid\\", 5, WF_SG_SFTP_NAME, r);
	}

	if (1 == n && L'-' == text[0]) {
		if (!previous || !*previous) {
			r->error = WF_SG_NO_PREVIOUS;
			return WF_OK;
		}
		r->target = dup_n(previous, wcslen(previous));
		r->list_dir =
		        r->target ? dup_n(previous, wcslen(previous)) : NULL;
		r->prefix = dup_n(L"", 0);
		if (!r->target || !r->list_dir || !r->prefix) {
			wf_smart_result_free(r);
			r->error = WF_SG_NOMEM;
			return WF_OK;
		}
		r->prefix_start = n;
		r->insert_separator = true;
		r->listed_as_whole = true;
		return WF_OK;
	}
	if (L'@' == text[0]) {
		size_t k = 1, a;
		wchar_t *base = NULL;

		while (k < n && !is_sep(text[k])) {
			k++;
		}
		for (a = 0; a < WF_SMART_ALIASES; a++) {
			if (equal_ci(text + 1, k - 1, alias_names[a])) {
				break;
			}
		}
		if (WF_SMART_ALIASES == a) {
			if (k == n) { /* "@" or "@do": still typing */
				r->error = WF_SG_ALIAS_PARTIAL;
				r->prefix = dup_n(text + 1, n - 1);
				r->prefix_start = off + 1;
				if (!r->prefix) {
					r->error = WF_SG_NOMEM;
				}
				return WF_OK;
			}
			r->error = WF_SG_UNKNOWN_ALIAS;
			wcsncpy_s(r->name, 48, text, k < 47 ? k : 47);
			return WF_OK;
		}
		if (0 == a) {
			base = env && env->home ? env->home(env->ctx) : NULL;
			if (!base) {
				r->error = WF_SG_NO_HOME;
				return WF_OK;
			}
		} else {
			base = env && env->known
			        ? env->known(env->ctx, alias_names[a])
			        : NULL;
			if (!base) {
				r->error = WF_SG_INVALID;
				return WF_OK;
			}
		}
		if (!text_add(&full, base, wcslen(base))) {
			free(base);
			r->error = WF_SG_NOMEM;
			return WF_OK;
		}
		free(base);
		rest_from = k;
		whole = k == n;
	} else if (L'~' == text[0] && (1 == n || is_sep(text[1]))) {
		wchar_t *base = env && env->home ? env->home(env->ctx) : NULL;

		if (!base) {
			r->error = WF_SG_NO_HOME;
			return WF_OK;
		}
		if (!text_add(&full, base, wcslen(base))) {
			free(base);
			r->error = WF_SG_NOMEM;
			return WF_OK;
		}
		free(base);
		rest_from = 1;
		whole = 1 == n;
	}
	/* the rest of what was typed: environment variables, then slashes */
	err = expand_env(
	        env, text + rest_from, n - rest_from, &expanded, r->name);
	if (WF_SG_OK != err) {
		r->error = err;
		text_free(&full);
		text_free(&expanded);
		return WF_OK;
	}
	if (rest_from && expanded.n && !is_sep(expanded.s[0])) {
		/* "@home" followed by something that is not a separator cannot
		 * happen: the alias ends at a separator */
		r->error = WF_SG_INVALID;
		text_free(&full);
		text_free(&expanded);
		return WF_OK;
	}
	if (!text_add(&full, expanded.s ? expanded.s : L"", expanded.n)) {
		r->error = WF_SG_NOMEM;
		text_free(&full);
		text_free(&expanded);
		return WF_OK;
	}
	text_free(&expanded);
	if (!full.s) {
		text_add(&full, L"", 0);
	}
	slashes(full.s, full.n);
	/* an extended path keeps its prefix; a "\\.\" device is never a folder */
	if (full.n >= 4 && !wcsncmp(full.s, L"\\\\.\\", 4)) {
		err = WF_SG_INVALID;
		goto done;
	}

	{
		bool inc = false;

		full_root = root_length(full.s, full.n, &inc);
		if (inc) {
			err = WF_SG_INVALID; /* "\\server" needs a share */
			goto done;
		}
	}
	if (full_root) {
		root = dup_n(full.s, full_root);
		root_len = full_root;
		if (!root) {
			st = WF_E_NOMEM;
			goto done;
		}
		/* "\\?\C:" style roots end in a separator; "\\srv\share" may not */
		err = add_components(full.s, full_root, full.n, &comps, 0);
	} else if (full.n >= 2 && is_letter(full.s[0]) && L':' == full.s[1]) {
		err = WF_SG_DRIVE_RELATIVE;
		goto done;
	} else if (full.n >= 2 && L'\\' == full.s[0] && L'\\' == full.s[1]) {
		err = WF_SG_INVALID;
		goto done;
	} else {
		/* relative to the shown folder, or to its root for a leading \ */
		bool rooted = L'\\' == full.s[0];
		size_t base_root;
		wchar_t *abs_cwd = dup_n(cwd, cwd_n);

		if (!abs_cwd) {
			st = WF_E_NOMEM;
			goto done;
		}
		slashes(abs_cwd, cwd_n);
		base_root = root_length(abs_cwd, cwd_n, &incomplete);
		if (!base_root) {
			free(abs_cwd);
			err = WF_SG_INVALID;
			goto done;
		}
		root = dup_n(abs_cwd, base_root);
		root_len = base_root;
		if (!root) {
			free(abs_cwd);
			st = WF_E_NOMEM;
			goto done;
		}
		if (!rooted) {
			/* the shown folder's own components first */
			Text joined = {0};

			if (!text_add(&joined, abs_cwd, cwd_n) ||
			        !text_addc(&joined, L'\\') ||
			        !text_add(&joined, full.s, full.n)) {
				text_free(&joined);
				free(abs_cwd);
				st = WF_E_NOMEM;
				goto done;
			}
			free(abs_cwd);
			text_free(&full);
			full = joined;
		} else {
			free(abs_cwd);
		}
		err = add_components(
		        full.s, rooted ? 0 : root_len, full.n, &comps, 0);
	}
	if (WF_SG_OK != err) {
		goto done;
	}

	trailing_sep = n && is_sep(text[n - 1]);
	r->target = build(root, root_len, full.s, comps.v, comps.n);
	if (!r->target) {
		st = WF_E_NOMEM;
		goto done;
	}
	if (wcslen(r->target) > PATH_MAX_UNITS) {
		err = WF_SG_TOO_LONG;
		goto done;
	}
	{
		/* what the last element is, for completion */
		size_t last = last_element_start(text, n);
		const wchar_t *tail = text + last;
		size_t tail_n = n - last;
		bool tail_special = !tail_n ||
		        (1 == tail_n && L'.' == tail[0]) ||
		        (2 == tail_n && L'.' == tail[0] && L'.' == tail[1]) ||
		        NULL != wmemchr(tail, L'%', tail_n);

		/* a bare "C:\" or "\\srv\share" is a folder as a whole too */
		if (!last && full_root && !comps.n) {
			whole = true;
		}
		if (whole || tail_special || (!comps.n)) {
			r->list_dir = dup_n(r->target, wcslen(r->target));
			r->prefix = dup_n(L"", 0);
			r->prefix_start = off + n;
			r->insert_separator =
			        !trailing_sep && (whole || tail_n > 0 || !n);
			if (trailing_sep) {
				r->insert_separator = false;
			}
			r->listed_as_whole = true;
		} else {
			/* list the target's parent, search for the last element */
			r->list_dir = build(
			        root, root_len, full.s, comps.v, comps.n - 1);
			r->prefix = dup_n(tail, tail_n);
			r->prefix_start = off + last;
			r->insert_separator = false;
		}
		if (!r->list_dir || !r->prefix) {
			st = WF_E_NOMEM;
			goto done;
		}
	}
done:
	free(root);
	free(comps.v);
	text_free(&full);
	if (WF_OK != st) {
		wf_smart_result_free(r);
		return st;
	}
	if (WF_SG_OK != err) {
		wf_smart_result_free(r);
		r->error = err;
	}
	return WF_OK;
}

WfStatus wf_smart_complete(const wchar_t *input,
        size_t n,
        size_t cursor,
        const WfSmartResult *r,
        const wchar_t *name,
        bool is_dir,
        wchar_t sep,
        wchar_t **out,
        size_t *out_n,
        size_t *out_cursor)
{
	Text t = {0};
	size_t start, nl;

	*out = NULL;
	if (!input || !r || !name || cursor > n || r->prefix_start > cursor) {
		return WF_E_INVAL;
	}
	start = r->prefix_start;
	nl = wcslen(name);
	if (!text_add(&t, input, start) ||
	        (r->insert_separator && start && !is_sep(input[start - 1]) &&
	                !text_addc(&t, sep)) ||
	        !text_add(&t, name, nl) || (is_dir && !text_addc(&t, sep))) {
		text_free(&t);
		return WF_E_NOMEM;
	}
	*out_cursor = t.n;
	if (!text_add(&t, input + cursor, n - cursor)) {
		text_free(&t);
		return WF_E_NOMEM;
	}
	*out = t.s;
	*out_n = t.n;
	return WF_OK;
}

static bool high_surrogate(wchar_t c)
{
	return (uint16_t)c >= 0xD800 && (uint16_t)c <= 0xDBFF;
}

static bool low_surrogate(wchar_t c)
{
	return (uint16_t)c >= 0xDC00 && (uint16_t)c <= 0xDFFF;
}

size_t wf_smart_element_start(const wchar_t *input, size_t cursor)
{
	size_t i = cursor, floor = 0, root_n;
	bool inc;
	wchar_t *probe;

	if (!input || !cursor) {
		return 0;
	}
	/* a root at the start of the text cannot be deleted into */
	probe = wf_malloc_array((cursor + 1), sizeof *probe);
	if (probe) {
		memcpy(probe, input, cursor * sizeof *probe);
		probe[cursor] = L'\0';
		slashes(probe, cursor);
		root_n = root_length(probe, cursor, &inc);
		if (!root_n && inc) {
			/* "\\server\sh": stop at the share's start */
			size_t k = 2;

			while (k < cursor && L'\\' != probe[k]) {
				k++;
			}
			root_n = k < cursor ? k + 1 : 0;
		}
		floor = root_n;
		free(probe);
	}
	if (i <= floor) {
		return 0;
	}
	while (i > floor && is_sep(input[i - 1])) {
		i--;
	}
	while (i > floor && !is_sep(input[i - 1])) {
		i--;
		if (i > floor && low_surrogate(input[i]) &&
		        high_surrogate(input[i - 1])) {
			i--;
		}
	}
	return i;
}

WfStatus wf_smart_clean_paste(
        const wchar_t *text, size_t n, wchar_t **out, size_t *out_n)
{
	size_t a = 0, b = n, i;
	wchar_t *copy;

	*out = NULL;
	*out_n = 0;
	if (!text && n) {
		return WF_E_INVAL;
	}
	while (a < b &&
	        (L' ' == text[a] || L'\t' == text[a] || L'\r' == text[a] ||
	                L'\n' == text[a])) {
		a++;
	}
	while (b > a &&
	        (L' ' == text[b - 1] || L'\t' == text[b - 1] ||
	                L'\r' == text[b - 1] || L'\n' == text[b - 1])) {
		b--;
	}
	if (b - a >= 2 && L'"' == text[a] && L'"' == text[b - 1]) {
		a++;
		b--;
	}
	for (i = a; i < b; i++) {
		if ((uint16_t)text[i] < 32 || 127 == text[i]) {
			return WF_E_INVAL; /* a second line, or a control character */
		}
	}
	copy = dup_n(text ? text + a : L"", b - a);
	if (!copy) {
		return WF_E_NOMEM;
	}
	*out = copy;
	*out_n = b - a;
	return WF_OK;
}
