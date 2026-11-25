#include "wf_ini_edit.h"

#include <stdlib.h>
#include <string.h>

typedef struct
{
	char *s; /* owned, no line ending */
	size_t n;
} Line;

typedef struct
{
	Line *v;
	size_t n, cap;
} Lines;

static void lines_free(Lines *l)
{
	size_t i;

	for (i = 0; i < l->n; i++) {
		free(l->v[i].s);
	}
	free(l->v);
	memset(l, 0, sizeof *l);
}

/* Puts `owned` (malloc'd, n bytes, NUL after them) in as line `at`. On success the
 * line is the list's; on failure it is still the caller's to free. */
static WfStatus lines_insert_owned(Lines *l, size_t at, char *owned, size_t n)
{
	if (l->n == l->cap) {
		size_t cap = l->cap ? l->cap * 2 : 32;
		Line *grown;

		if (cap > SIZE_MAX / sizeof *grown) {
			return WF_E_RANGE;
		}
		grown = realloc(l->v, cap * sizeof *grown);
		if (!grown) {
			return WF_E_NOMEM;
		}
		l->v = grown;
		l->cap = cap;
	}
	memmove(&l->v[at + 1], &l->v[at], (l->n - at) * sizeof *l->v);
	l->v[at].s = owned;
	l->v[at].n = n;
	l->n++;
	return WF_OK;
}

/* The same for a string that is only borrowed: a copy goes in. */
static WfStatus lines_insert(Lines *l, size_t at, const char *s, size_t n)
{
	char *copy = malloc(n + 1);
	WfStatus st;

	if (!copy) {
		return WF_E_NOMEM;
	}
	memcpy(copy, s, n);
	copy[n] = '\0';
	st = lines_insert_owned(l, at, copy, n);
	if (WF_OK != st) {
		free(copy);
	}
	return st;
}

static bool blank_or_comment(const Line *l)
{
	size_t i = 0;

	while (i < l->n && (' ' == l->s[i] || '\t' == l->s[i])) {
		i++;
	}
	return i == l->n || ';' == l->s[i] || '#' == l->s[i];
}

/* "[name]" -> the trimmed name in [*name, *name + *len). */
static bool header(const Line *l, const char **name, size_t *len)
{
	size_t a = 0, b = l->n;

	while (a < b && (' ' == l->s[a] || '\t' == l->s[a])) {
		a++;
	}
	while (b > a && (' ' == l->s[b - 1] || '\t' == l->s[b - 1])) {
		b--;
	}
	if (b - a < 3 || '[' != l->s[a] || ']' != l->s[b - 1]) {
		return false;
	}
	a++;
	b--;
	while (a < b && (' ' == l->s[a] || '\t' == l->s[a])) {
		a++;
	}
	while (b > a && (' ' == l->s[b - 1] || '\t' == l->s[b - 1])) {
		b--;
	}
	*name = l->s + a;
	*len = b - a;
	return b > a;
}

/* The key of "key = value", trimmed; false for a line that is not one. */
static bool assignment(const Line *l, const char **key, size_t *len)
{
	const char *eq;
	size_t a = 0, b;

	if (blank_or_comment(l)) {
		return false;
	}
	eq = memchr(l->s, '=', l->n);
	if (!eq) {
		return false;
	}
	b = (size_t)(eq - l->s);
	while (a < b && (' ' == l->s[a] || '\t' == l->s[a])) {
		a++;
	}
	while (b > a && (' ' == l->s[b - 1] || '\t' == l->s[b - 1])) {
		b--;
	}
	*key = l->s + a;
	*len = b - a;
	return b > a;
}

static bool text_ok(const char *s, bool name)
{
	if (!s || !*s) {
		return false;
	}
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;

		if (c < 0x20 || 0x7F == c) {
			return false;
		}
		if (name && ('=' == c || '[' == c || ']' == c)) {
			return false;
		}
	}
	return true;
}

static bool same(const char *a, size_t an, const char *b)
{
	return an == strlen(b) && !memcmp(a, b, an);
}

static WfStatus replace_line(
        Lines *l, size_t at, const char *key, const char *value)
{
	size_t kn = strlen(key), vn = strlen(value), n;
	char *fresh;

	if (kn > SIZE_MAX - vn - 4) {
		return WF_E_RANGE;
	}
	n = kn + vn + 3;
	fresh = malloc(n + 1);
	if (!fresh) {
		return WF_E_NOMEM;
	}
	memcpy(fresh, key, kn);
	memcpy(fresh + kn, " = ", 3);
	memcpy(fresh + kn + 3, value, vn);
	fresh[n] = '\0';
	free(l->v[at].s);
	l->v[at].s = fresh;
	l->v[at].n = n;
	return WF_OK;
}

static bool section_matches(const char *name,
        size_t len,
        const WfIniSet *set,
        WfIniSectionMatch match)
{
	return match ? match(name, len, set->section, set->key)
	             : same(name, len, set->section);
}

/* Drops every occurrence of the key before line `found`, in whatever repeated
 * section, and handles line `found` itself: it takes the new value, or goes too
 * when there is none. Lines are moved once each (a read position and a write
 * position), not once for every line that goes. The one allocation, the new
 * text of line `found`, is made before anything is changed, so a failure leaves
 * the list as it was. */
static WfStatus drop_earlier_duplicates(
        Lines *l, const WfIniSet *set, WfIniSectionMatch match, size_t found)
{
	size_t r, w = 0;
	bool in = false;

	if (set->value) {
		WfStatus st = replace_line(l, found, set->key, set->value);

		if (WF_OK != st) {
			return st;
		}
	}
	for (r = 0; r <= found; r++) {
		const char *name, *key;
		size_t len;
		bool is_key = false;

		if (header(&l->v[r], &name, &len)) {
			in = section_matches(name, len, set, match);
		} else if (in && assignment(&l->v[r], &key, &len) &&
		        same(key, len, set->key)) {
			is_key = true;
		}
		if (is_key && (r < found || !set->value)) {
			free(l->v[r].s); /* a duplicate, or the key to delete */
			continue;
		}
		l->v[w++] = l->v[r];
	}
	memmove(&l->v[w], &l->v[found + 1], (l->n - found - 1) * sizeof *l->v);
	l->n -= found + 1 - w;
	return WF_OK;
}

static WfStatus apply_one(
        Lines *l, const WfIniSet *set, WfIniSectionMatch match)
{
	bool in = false, have_section = false;
	size_t i, found = 0, section_end = 0;
	bool found_key = false;
	WfStatus st;
	char *entry, *head = NULL;
	size_t en;

	for (i = 0; i < l->n; i++) {
		const char *name, *key;
		size_t len;

		if (header(&l->v[i], &name, &len)) {
			in = section_matches(name, len, set, match);
			if (in) {
				have_section = true;
				section_end = i;
			}
			continue;
		}
		if (!in) {
			continue;
		}
		if (assignment(&l->v[i], &key, &len)) {
			section_end = i;
			if (same(key, len, set->key)) {
				found = i;
				found_key = true;
			}
		}
	}
	if (found_key) {
		/* Delete every earlier occurrence, including in repeated sections,
		 * so first-wins readers agree with last-wins readers after a save. */
		return drop_earlier_duplicates(l, set, match, found);
	}
	if (!set->value) {
		return WF_OK;
	}
	en = strlen(set->key) + strlen(set->value) + 3;
	entry = malloc(en + 1);
	if (!entry) {
		return WF_E_NOMEM;
	}
	memcpy(entry, set->key, strlen(set->key));
	memcpy(entry + strlen(set->key), " = ", 3);
	memcpy(entry + strlen(set->key) + 3, set->value, strlen(set->value));
	entry[en] = '\0';
	if (have_section) {
		st = lines_insert_owned(l, section_end + 1, entry, en);
		if (WF_OK == st) {
			entry = NULL; /* the list's now */
		}
	} else {
		size_t sn = strlen(set->section);

		head = malloc(sn + 3);
		if (!head) {
			free(entry);
			return WF_E_NOMEM;
		}
		head[0] = '[';
		memcpy(head + 1, set->section, sn);
		head[sn + 1] = ']';
		head[sn + 2] = '\0';
		st = WF_OK;
		if (l->n && l->v[l->n - 1].n) {
			st = lines_insert(l, l->n, "", 0);
		}
		if (WF_OK == st) {
			st = lines_insert_owned(l, l->n, head, sn + 2);
			if (WF_OK == st) {
				head = NULL;
			}
		}
		if (WF_OK == st) {
			st = lines_insert_owned(l, l->n, entry, en);
			if (WF_OK == st) {
				entry = NULL;
			}
		}
	}
	free(head); /* each is NULL once the list owns it */
	free(entry);
	return st;
}

WfStatus wf_ini_apply_matching(const char *text,
        size_t n,
        const WfIniSet *sets,
        size_t count,
        WfIniSectionMatch match,
        char **out,
        size_t *out_n)
{
	Lines l = {0};
	const char *eol = "\r\n";
	size_t eol_n = 2, i, total = 0, at = 0;
	WfStatus st = WF_OK;
	char *result;

	if (!out || !out_n || (!text && n) || (count && !sets)) {
		return WF_E_INVAL;
	}
	*out = NULL;
	*out_n = 0;
	for (i = 0; i < count; i++) {
		if (!text_ok(sets[i].section, true) ||
		        !text_ok(sets[i].key, true) ||
		        (sets[i].value && !text_ok(sets[i].value, false))) {
			return WF_E_INVAL;
		}
	}
	for (i = 0; i < n; i++) {
		if ('\n' == text[i]) {
			if (0 == i || '\r' != text[i - 1]) {
				eol = "\n";
				eol_n = 1;
			}
			break;
		}
	}
	for (i = 0; i < n;) {
		size_t end = i, next;

		while (end < n && '\n' != text[end]) {
			end++;
		}
		next = end < n ? end + 1 : end;
		if (end > i && '\r' == text[end - 1]) {
			end--;
		}
		st = lines_insert(&l, l.n, text + i, end - i);
		if (WF_OK != st) {
			goto done;
		}
		i = next;
	}
	for (i = 0; i < count && WF_OK == st; i++) {
		st = apply_one(&l, &sets[i], match);
	}
	if (WF_OK != st) {
		goto done;
	}
	for (i = 0; i < l.n; i++) {
		if (l.v[i].n > SIZE_MAX - total - eol_n) {
			st = WF_E_RANGE;
			goto done;
		}
		total += l.v[i].n + eol_n;
	}
	result = malloc(total + 1);
	if (!result) {
		st = WF_E_NOMEM;
		goto done;
	}
	for (i = 0; i < l.n; i++) {
		memcpy(result + at, l.v[i].s, l.v[i].n);
		at += l.v[i].n;
		memcpy(result + at, eol, eol_n);
		at += eol_n;
	}
	result[at] = '\0';
	*out = result;
	*out_n = at;
done:
	lines_free(&l);
	return st;
}

WfStatus wf_ini_apply(const char *text,
        size_t n,
        const WfIniSet *sets,
        size_t count,
        char **out,
        size_t *out_n)
{
	return wf_ini_apply_matching(text, n, sets, count, NULL, out, out_n);
}
