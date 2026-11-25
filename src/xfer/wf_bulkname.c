#include "wf_bulkname.h"

#include <windows.h>

#include <stdlib.h>
#include <string.h>

typedef struct
{
	wchar_t *s;
	size_t n, cap;
	WfStatus
	        st; /* the first thing that went wrong; later puts do nothing */
} Out;

static void put(Out *o, const wchar_t *s, size_t n)
{
	if (WF_OK != o->st) {
		return;
	}
	if (o->n + n > WF_BULK_NAME_MAX) {
		o->st = WF_E_RANGE;
		return;
	}
	if (o->n + n + 1 > o->cap) {
		size_t cap = o->cap ? o->cap : 64;
		wchar_t *grown;

		while (cap < o->n + n + 1) {
			cap *= 2;
		}
		grown = realloc(o->s, cap * sizeof *grown);
		if (!grown) {
			o->st = WF_E_NOMEM;
			return;
		}
		o->s = grown;
		o->cap = cap;
	}
	memcpy(o->s + o->n, s, n * sizeof *s);
	o->n += n;
	o->s[o->n] = L'\0';
}

static const wchar_t *template_of(const WfBulkPattern *p)
{
	return p->tmpl ? p->tmpl : L"{name}";
}

/* One token at t[i] == '{' or '}'. Returns the units it takes, 0 when wrong.
 * kind: 'n' number (width in *width), 'a' name, '{' or '}' a brace. */
static size_t token(const wchar_t *t, size_t i, char *kind, int *width)
{
	*width = 0;
	*kind = 0;
	if (L'}' == t[i]) {
		if (L'}' != t[i + 1]) {
			return 0;
		}
		*kind = '}';
		return 2;
	}
	if (L'{' == t[i + 1]) {
		*kind = '{';
		return 2;
	}
	if (!wcsncmp(t + i, L"{name}", 6)) {
		*kind = 'a';
		return 6;
	}
	if (!wcsncmp(t + i, L"{n}", 3)) {
		*kind = 'n';
		return 3;
	}
	if (!wcsncmp(t + i, L"{n:", 3) && t[i + 3] >= L'1' &&
	        t[i + 3] <= L'9' && L'}' == t[i + 4]) {
		*kind = 'n';
		*width = t[i + 3] - L'0';
		return 5;
	}
	return 0;
}

WfStatus wf_bulk_check(const WfBulkPattern *p, size_t *pos)
{
	const wchar_t *t = template_of(p);

	for (size_t i = 0; t[i];) {
		char kind;
		int width;
		size_t n;

		if (L'{' != t[i] && L'}' != t[i]) {
			i++;
			continue;
		}
		n = token(t, i, &kind, &width);
		if (!n) {
			if (pos) {
				*pos = i;
			}
			return WF_E_INVAL;
		}
		i += n;
	}
	return WF_OK;
}

static void change_case(wchar_t *s, size_t n, WfBulkCase c)
{
	if (n && WF_BULK_CASE_UPPER == c) {
		CharUpperBuffW(s, (DWORD)n);
	} else if (n && WF_BULK_CASE_LOWER == c) {
		CharLowerBuffW(s, (DWORD)n);
	}
}

/* The stem after find and replace; owned. NULL with *st set (too long, or out
 * of memory). */
static wchar_t *replaced(
        const WfBulkPattern *p, const wchar_t *stem, size_t n, WfStatus *st)
{
	Out o = {0};
	size_t fl = p->find ? wcslen(p->find) : 0;
	const wchar_t *rep = p->replace ? p->replace : L"";

	for (size_t i = 0; i < n;) {
		if (fl && i + fl <= n && !wcsncmp(stem + i, p->find, fl)) {
			put(&o, rep, wcslen(rep));
			i += fl;
		} else {
			put(&o, stem + i, 1);
			i++;
		}
	}
	if (!o.s) {
		put(&o, L"", 0); /* an empty stem is still a string */
	}
	if (WF_OK != o.st) {
		free(o.s);
		*st = o.st;
		return NULL;
	}
	return o.s;
}

static void number(int64_t value, int width, wchar_t out[32])
{
	wchar_t digits[24];
	size_t d = 0, n = 0;
	uint64_t u = value < 0 ? 0 - (uint64_t)value : (uint64_t)value;

	do {
		digits[d++] = (wchar_t)(L'0' + u % 10);
		u /= 10;
	} while (u);
	if (value < 0) {
		out[n++] = L'-';
	}
	for (int pad = width - (int)d; pad > 0; pad--) {
		out[n++] = L'0';
	}
	while (d) {
		out[n++] = digits[--d];
	}
	out[n] = L'\0';
}

/* start + index * step, or false when it does not fit. */
static bool number_at(const WfBulkPattern *p, size_t index, int64_t *value)
{
	int64_t v, mag;

	if (index > (size_t)INT64_MAX || p->step == INT64_MIN) {
		return false;
	}
	mag = p->step < 0 ? -p->step : p->step;
	if (mag && (int64_t)index > INT64_MAX / mag) {
		return false;
	}
	v = (int64_t)index * p->step;
	if ((v > 0 && p->start > INT64_MAX - v) ||
	        (v < 0 && p->start < INT64_MIN - v)) {
		return false;
	}
	*value = p->start + v;
	return true;
}

WfStatus wf_bulk_apply(const WfBulkPattern *p,
        const wchar_t *old_name,
        bool dir,
        size_t index,
        wchar_t **out)
{
	const wchar_t *t = template_of(p), *dot = NULL;
	size_t len = wcslen(old_name), stem_n = len;
	wchar_t *stem;
	Out o = {0};
	WfStatus st = WF_OK;

	*out = NULL;
	if (WF_OK != wf_bulk_check(p, NULL)) {
		return WF_E_INVAL;
	}
	if (!dir) {
		for (size_t i = 1; i < len; i++) {
			if (L'.' == old_name[i]) {
				dot = old_name + i;
			}
		}
	}
	if (dot) {
		stem_n = (size_t)(dot - old_name);
	}
	stem = replaced(p, old_name, stem_n, &st);
	if (!stem) {
		return st;
	}
	change_case(stem, wcslen(stem), p->stem_case);
	for (size_t i = 0; t[i] && WF_OK == o.st;) {
		char kind;
		int width;
		size_t n;

		if (L'{' != t[i] && L'}' != t[i]) {
			put(&o, t + i, 1);
			i++;
			continue;
		}
		n = token(t, i, &kind, &width);
		if ('a' == kind) {
			put(&o, stem, wcslen(stem));
		} else if ('n' == kind) {
			wchar_t digits[32];
			int64_t v;

			if (number_at(p, index, &v)) {
				number(v, width, digits);
				put(&o, digits, wcslen(digits));
			} else {
				o.st = WF_E_RANGE;
			}
		} else {
			put(&o, '{' == kind ? L"{" : L"}", 1);
		}
		i += n;
	}
	free(stem);
	if (dot) { /* the extension goes straight into the name, then takes its case */
		size_t at = o.n, en = wcslen(dot);

		put(&o, dot, en);
		if (WF_OK == o.st) {
			change_case(o.s + at, en, p->ext_case);
		}
	}
	if (WF_OK == o.st && !o.s) {
		put(&o, L"", 0);
	}
	if (WF_OK != o.st) {
		free(o.s);
		return o.st;
	}
	*out = o.s;
	return WF_OK;
}
