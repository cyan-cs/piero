#include "wf_query.h"

#include <windows.h>

#include <string.h>
#include <wchar.h>

/* Lower-cases `s` (n units) into out[cap]; false when it does not fit. */
static bool fold_into(const wchar_t *s, size_t n, wchar_t *out, size_t cap)
{
	int got;

	if (n + 1 > cap || n > 0x7fffffffu) {
		return false;
	}
	if (0 == n) {
		out[0] = 0;
		return true;
	}
	got = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s, (int)n,
	        out, (int)cap - 1, NULL, NULL, 0);
	if (got <= 0) {
		/* the system could not: ASCII only, which is never wrong, only less */
		for (size_t i = 0; i < n; i++) {
			out[i] = s[i] >= L'A' && s[i] <= L'Z'
			        ? (wchar_t)(s[i] + 32)
			        : s[i];
		}
		got = (int)n;
	}
	out[got] = 0;
	return true;
}

static bool is_space(wchar_t c)
{
	return L' ' == c || L'\t' == c;
}

/* Case-insensitive ASCII prefix: the length of `prefix` when `s` starts with it. */
static size_t prefix_len(const wchar_t *s, const wchar_t *prefix)
{
	size_t i = 0;

	for (; prefix[i]; i++) {
		wchar_t c = s[i];

		if (c >= L'A' && c <= L'Z') {
			c = (wchar_t)(c + 32);
		}
		if (c != prefix[i]) {
			return 0;
		}
	}
	return i;
}

/* "10", "10k", "10KiB", "2M": bytes. False: not a size, or it does not fit. */
static bool parse_size(const wchar_t *s, size_t n, uint64_t *out)
{
	uint64_t v = 0;
	size_t i = 0;
	unsigned shift = 0;

	if (!n || s[0] < L'0' || s[0] > L'9') {
		return false;
	}
	for (; i < n && s[i] >= L'0' && s[i] <= L'9'; i++) {
		unsigned d = (unsigned)(s[i] - L'0');

		if (v > (UINT64_MAX - d) / 10) {
			return false;
		}
		v = v * 10 + d;
	}
	if (i < n) {
		wchar_t u = s[i] >= L'A' && s[i] <= L'Z' ? (wchar_t)(s[i] + 32)
		                                         : s[i];
		size_t rest = n - i - 1;
		const wchar_t *tail = s + i + 1;

		switch (u) {
		case L'k': shift = 10; break;
		case L'm': shift = 20; break;
		case L'g': shift = 30; break;
		case L't': shift = 40; break;
		case L'b':
			if (rest) {
				return false;
			}
			break;
		default: return false;
		}
		/* after a unit letter: nothing, "b", or "ib" */
		if (L'b' != u && rest) {
			if (!((1 == rest &&
			              (L'b' == tail[0] || L'B' == tail[0])) ||
			            (2 == rest &&
			                    (L'i' == tail[0] ||
			                            L'I' == tail[0]) &&
			                    (L'b' == tail[1] ||
			                            L'B' == tail[1])))) {
				return false;
			}
		}
	}
	if (shift && v > (UINT64_MAX >> shift)) {
		return false;
	}
	*out = v << shift;
	return true;
}

/* "2026-10-01": that day's midnight UTC as a FILETIME. */
static bool parse_day(const wchar_t *s, size_t n, WfFileTime *out)
{
	unsigned y, m, d;
	SYSTEMTIME st;
	FILETIME ft;

	if (10 != n || L'-' != s[4] || L'-' != s[7]) {
		return false;
	}
	for (size_t i = 0; i < 10; i++) {
		if (4 != i && 7 != i && (s[i] < L'0' || s[i] > L'9')) {
			return false;
		}
	}
	y = (unsigned)((s[0] - L'0') * 1000 + (s[1] - L'0') * 100 +
	        (s[2] - L'0') * 10 + (s[3] - L'0'));
	m = (unsigned)((s[5] - L'0') * 10 + (s[6] - L'0'));
	d = (unsigned)((s[8] - L'0') * 10 + (s[9] - L'0'));
	if (y < 1601 || y > 9999 || m < 1 || m > 12 || d < 1 || d > 31) {
		return false;
	}
	memset(&st, 0, sizeof st);
	st.wYear = (WORD)y;
	st.wMonth = (WORD)m;
	st.wDay = (WORD)d;
	/* SystemTimeToFileTime refuses a day the month does not have (31 Feb) */
	if (!SystemTimeToFileTime(&st, &ft)) {
		return false;
	}
	*out = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
	return true;
}

WfStatus wf_query_parse(const wchar_t *text, WfQuery *q, size_t *pos)
{
	size_t i = 0, n;

	if (!text || !q) {
		return WF_E_INVAL;
	}
	memset(q, 0, sizeof *q);
	n = wcslen(text);
	while (i < n) {
		size_t start, len;
		const wchar_t *t;
		size_t k;
		bool quoted = false;

		while (i < n && is_space(text[i])) {
			i++;
		}
		if (i >= n) {
			break;
		}
		start = i;
		if (L'"' == text[i]) {
			quoted = true;
			i++;
			start = i;
			while (i < n && L'"' != text[i]) {
				i++;
			}
			if (i >= n) {
				goto bad_at_quote; /* no closing quote */
			}
			len = i - start;
			i++; /* the closing quote */
			t = text + start;
		} else {
			while (i < n && !is_space(text[i])) {
				i++;
			}
			len = i - start;
			t = text + start;
		}
		if (quoted) {
			if (!len || q->n_word >= WF_QUERY_WORDS ||
			        !fold_into(t, len, q->word[q->n_word],
			                WF_QUERY_WORD_UNITS)) {
				goto bad_at_quote;
			}
			q->n_word++;
			continue;
		}
		if ((k = prefix_len(t, L"ext:")) && len > k) {
			const wchar_t *v = t + k;
			size_t vn = len - k;

			if (L'.' == v[0]) {
				v++;
				vn--;
			}
			if (!vn || vn >= 16 || q->n_ext >= WF_QUERY_EXTS ||
			        !fold_into(v, vn, q->ext[q->n_ext], 16)) {
				goto bad;
			}
			q->n_ext++;
		} else if ((k = prefix_len(t, L"size>")) && len > k) {
			if (q->has_gt || !parse_size(t + k, len - k, &q->gt)) {
				goto bad;
			}
			q->has_gt = true;
		} else if ((k = prefix_len(t, L"size<")) && len > k) {
			if (q->has_lt || !parse_size(t + k, len - k, &q->lt)) {
				goto bad;
			}
			q->has_lt = true;
		} else if ((k = prefix_len(t, L"newer:")) && len > k) {
			if (q->has_newer ||
			        !parse_day(t + k, len - k, &q->newer)) {
				goto bad;
			}
			q->has_newer = true;
		} else if ((k = prefix_len(t, L"older:")) && len > k) {
			if (q->has_older ||
			        !parse_day(t + k, len - k, &q->older)) {
				goto bad;
			}
			q->has_older = true;
		} else if (prefix_len(t, L"ext:") || prefix_len(t, L"size>") ||
		        prefix_len(t, L"size<") || prefix_len(t, L"newer:") ||
		        prefix_len(t, L"older:")) {
			goto bad; /* a term with nothing after it */
		} else {
			if (q->n_word >= WF_QUERY_WORDS ||
			        !fold_into(t, len, q->word[q->n_word],
			                WF_QUERY_WORD_UNITS)) {
				goto bad;
			}
			q->n_word++;
		}
		continue;
	bad:
		if (pos) {
			*pos = start;
		}
		return WF_E_INVAL;
	bad_at_quote:
		if (pos) {
			*pos = quoted && start > 0 ? start - 1 : start;
		}
		return WF_E_INVAL;
	}
	return WF_OK;
}

bool wf_query_empty(const WfQuery *q)
{
	return !q->n_word && !q->n_ext && !q->has_gt && !q->has_lt &&
	        !q->has_newer && !q->has_older;
}

bool wf_query_match(const WfQuery *q,
        const wchar_t *name,
        bool dir,
        uint64_t size,
        WfFileTime mtime)
{
	wchar_t folded[512];
	size_t n = wcslen(name);

	if (q->has_newer && mtime < q->newer) {
		return false;
	}
	if (q->has_older && mtime >= q->older) {
		return false;
	}
	if (dir && (q->n_ext || q->has_gt || q->has_lt)) {
		return false;
	}
	if (!dir) {
		if (q->has_gt && size <= q->gt) {
			return false;
		}
		if (q->has_lt && size >= q->lt) {
			return false;
		}
	}
	if (!q->n_word && !q->n_ext) {
		return true;
	}
	if (!fold_into(name, n, folded, sizeof folded / sizeof folded[0])) {
		return false; /* a name that long is not one a word is looked for in */
	}
	for (size_t i = 0; i < q->n_word; i++) {
		if (!wcsstr(folded, q->word[i])) {
			return false;
		}
	}
	if (q->n_ext) {
		const wchar_t *dot = wcsrchr(folded, L'.');
		bool any = false;

		if (!dot ||
		        dot == folded) { /* no extension, or a dotfile's name */
			return false;
		}
		for (size_t i = 0; i < q->n_ext && !any; i++) {
			any = 0 == wcscmp(dot + 1, q->ext[i]);
		}
		if (!any) {
			return false;
		}
	}
	return true;
}
