#include "wf_esearch.h"

#include <stdlib.h>
#include <string.h>

bool wf_es_ok(const WfSearch *s)
{
	return s && s->pat && s->pat_n && !memchr(s->pat, '\n', s->pat_n) &&
	        !memchr(s->pat, '\r', s->pat_n);
}

static unsigned char fold(unsigned char c)
{
	return c >= 'A' && c <= 'Z' ? (unsigned char)(c + 32) : c;
}

static bool word_byte(unsigned char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
	        (c >= 'A' && c <= 'Z') || '_' == c || c >= 0x80;
}

/* The byte before `at` in the line, or -1 at its start. */
static int left_of(const WfELine *l, size_t at)
{
	return at > 0 ? (unsigned char)l->s[at - 1] : -1;
}

/* Whether two bytes are the same for this search. */
static bool same(unsigned char a, unsigned char b, const WfSearch *s)
{
	return s->match_case ? a == b : fold(a) == fold(b);
}

/* Whether a match of the pattern at byte `at` of the line is a whole word, if
 * that is asked for; `left` is the byte before it (-1: none). */
static bool word_ok(const WfELine *l, size_t at, const WfSearch *s, int left)
{
	if (!s->whole_word) {
		return true;
	}
	if (left >= 0 && word_byte((unsigned char)left)) {
		return false;
	}
	return at + s->pat_n >= l->len ||
	        !word_byte((unsigned char)l->s[at + s->pat_n]);
}

/* Whether the pattern matches l->s at byte `at`; `left` is the byte before it
 * (-1: none), which a whole-word test looks at. */
static bool match_at(const WfELine *l, size_t at, const WfSearch *s, int left)
{
	size_t i;

	if (l->len - at < s->pat_n) {
		return false;
	}
	for (i = 0; i < s->pat_n; i++) {
		if (!same((unsigned char)l->s[at + i], (unsigned char)s->pat[i],
		            s)) {
			return false;
		}
	}
	return word_ok(l, at, s, left);
}

/*
 * A pattern longer than this is looked for with the failure table of
 * Knuth-Morris-Pratt, so that a text that nearly matches at every byte (a long
 * run of one letter, a pattern of that letter and one other) is read once and not
 * once for every start. A shorter pattern is compared where it starts: its
 * worst case is bounded by its length, and it needs no table.
 */
#define KMP_MIN_PATTERN 64

/* For each prefix of the pattern, the length of its longest proper prefix that is
 * also its suffix (equal as `same` says). NULL when there is no memory: the
 * caller compares where the pattern starts, as for a short one. */
static size_t *failure_table(const WfSearch *s)
{
	size_t *f = malloc(s->pat_n * sizeof *f), k = 0;

	if (!f) {
		return NULL;
	}
	f[0] = 0;
	for (size_t i = 1; i < s->pat_n; i++) {
		while (k > 0 &&
		        !same((unsigned char)s->pat[i],
		                (unsigned char)s->pat[k], s)) {
			k = f[k - 1];
		}
		if (same((unsigned char)s->pat[i], (unsigned char)s->pat[k],
		            s)) {
			k++;
		}
		f[i] = k;
	}
	return f;
}

/* The failure table of a search operation, made when it is first needed and
 * kept for the rest of the operation (a search over many lines, or over all the
 * matches of a line, builds it once, not once for every line it looks in). It
 * is not kept beyond the operation: whoever begins one frees it. */
typedef struct
{
	size_t *f;
	bool tried; /* a failed build is not tried again within the operation */
} Prep;

static const size_t *prep_table(Prep *p, const WfSearch *s)
{
	if (!p->tried) {
		p->tried = true;
		p->f = failure_table(s);
	}
	return p->f;
}

static void prep_free(Prep *p)
{
	free(p->f);
	p->f = NULL;
}

/* first_in_line_ctx for a long pattern: each byte of the line is read once. A
 * place where the pattern matches but is not a start (inside a scalar, or not a
 * whole word) is passed over with the table, as a mismatch would be, because
 * the next match may overlap it. */
static bool kmp_first(const WfELine *l,
        size_t from_col,
        int from_left,
        const WfSearch *s,
        const size_t *f,
        size_t *at)
{
	size_t k = 0;

	for (size_t i = from_col; i < l->len; i++) {
		while (k > 0 &&
		        !same((unsigned char)l->s[i], (unsigned char)s->pat[k],
		                s)) {
			k = f[k - 1];
		}
		if (same((unsigned char)l->s[i], (unsigned char)s->pat[k], s)) {
			k++;
		}
		if (k == s->pat_n) {
			size_t start = i + 1 - s->pat_n;

			if (0x80 != ((unsigned char)l->s[start] & 0xC0) &&
			        word_ok(l, start, s,
			                start == from_col && from_left != -2
			                        ? from_left
			                        : left_of(l, start))) {
				*at = start;
				return true;
			}
			k = f[k - 1];
		}
	}
	return false;
}

/* The first match in the line starting at or after `from_col`. When
 * `from_left` is not -2 it stands for the byte before `from_col`. */
static bool first_in_line_ctx(const WfELine *l,
        size_t from_col,
        int from_left,
        const WfSearch *s,
        Prep *prep,
        size_t *at)
{
	size_t i;

	if (l->len < s->pat_n) {
		return false;
	}
	if (s->pat_n > KMP_MIN_PATTERN && from_col <= l->len &&
	        l->len - from_col >= s->pat_n) {
		const size_t *f = prep_table(prep, s);

		if (f) {
			return kmp_first(l, from_col, from_left, s, f, at);
		}
	}
	for (i = from_col; i + s->pat_n <= l->len; i++) {
		if (0x80 == ((unsigned char)l->s[i] & 0xC0)) {
			continue; /* never start inside a scalar */
		}
		if (match_at(l, i, s,
		            i == from_col && from_left != -2 ? from_left
		                                             : left_of(l, i))) {
			*at = i;
			return true;
		}
	}
	return false;
}

static bool first_in_line(const WfELine *l,
        size_t from_col,
        const WfSearch *s,
        Prep *prep,
        size_t *at)
{
	return first_in_line_ctx(l, from_col, -2, s, prep, at);
}

/* The last match in the line that starts before `before_col` (a match may overlap
 * the one after it): looked for backwards from there, so only the stretch between
 * the answer and `before_col` is read. */
static bool last_in_line(const WfELine *l,
        size_t before_col,
        const WfSearch *s,
        Prep *prep,
        size_t *at)
{
	size_t i, last_start;

	if (l->len < s->pat_n || 0 == before_col) {
		return false;
	}
	if (s->pat_n > KMP_MIN_PATTERN) {
		/* Backwards, a start is tried at each byte and may read the whole
		 * pattern before it fails: a long run of one letter and a pattern of it
		 * with one other is read pattern-length times over. With the table,
		 * the matches are found forwards in one read, and the last of those
		 * that start before `before_col` is the answer. */
		const size_t *f = prep_table(prep, s);

		if (f) {
			size_t col = 0, m, found = 0;
			bool any = false;

			while (col < before_col &&
			        kmp_first(l, col, -2, s, f, &m) &&
			        m < before_col) {
				any = true;
				found = m;
				col = m + 1;
			}
			if (any) {
				*at = found;
			}
			return any;
		}
	}
	last_start = l->len - s->pat_n; /* a match must fit */
	i = before_col - 1 < last_start ? before_col - 1 : last_start;
	for (;;) {
		if (0x80 !=
		                ((unsigned char)l->s[i] &
		                        0xC0) && /* never inside a scalar */
		        match_at(l, i, s, left_of(l, i))) {
			*at = i;
			return true;
		}
		if (0 == i) {
			return false;
		}
		i--;
	}
}

static bool hit(WfMatch *out,
        bool *wrapped,
        size_t line,
        size_t at,
        size_t len,
        bool was_wrapped)
{
	out->start.line = out->end.line = line;
	out->start.col = at;
	out->end.col = at + len;
	if (wrapped) {
		*wrapped = was_wrapped;
	}
	return true;
}

/* The search of one operation: `prep` is shared by every line looked in. */
static bool find_in(const WfEText *t,
        const WfSearch *s,
        WfPos from,
        bool forward,
        bool wrap,
        WfMatch *out,
        bool *wrapped,
        Prep *prep)
{
	size_t line, at;

	if (forward) {
		for (line = from.line; line < t->n; line++) {
			if (first_in_line(&t->lines[line],
			            line == from.line ? from.col : 0, s, prep,
			            &at)) {
				return hit(out, wrapped, line, at, s->pat_n,
				        false);
			}
		}
		if (!wrap) {
			return false;
		}
		for (line = 0; line <= from.line; line++) {
			if (first_in_line(&t->lines[line], 0, s, prep, &at)) {
				return hit(
				        out, wrapped, line, at, s->pat_n, true);
			}
		}
		return false;
	}
	for (line = from.line + 1; line-- > 0;) {
		if (last_in_line(&t->lines[line],
		            line == from.line ? from.col
		                              : t->lines[line].len + 1,
		            s, prep, &at)) {
			return hit(out, wrapped, line, at, s->pat_n, false);
		}
	}
	if (!wrap) {
		return false;
	}
	for (line = t->n; line-- > from.line;) {
		if (last_in_line(&t->lines[line], t->lines[line].len + 1, s,
		            prep, &at)) {
			return hit(out, wrapped, line, at, s->pat_n, true);
		}
	}
	return false;
}

bool wf_es_find(const WfEText *t,
        const WfSearch *s,
        WfPos from,
        bool forward,
        bool wrap,
        WfMatch *out,
        bool *wrapped)
{
	Prep prep = {NULL, false};
	bool found;

	if (wrapped) {
		*wrapped = false;
	}
	if (!wf_es_ok(s) || !wf_et_valid(t, from)) {
		return false;
	}
	found = find_in(t, s, from, forward, wrap, out, wrapped, &prep);
	prep_free(&prep);
	return found;
}

bool wf_es_next_match(
        const WfELine *l, const WfSearch *s, size_t from_col, size_t *at)
{
	Prep prep = {NULL, false};
	bool found = wf_es_ok(s) && first_in_line(l, from_col, s, &prep, at);

	prep_free(&prep); /* one call, one use: nothing to share */
	return found;
}

bool wf_es_next_match_after(const WfELine *l,
        const WfSearch *s,
        size_t from_col,
        int left,
        size_t *at)
{
	Prep prep = {NULL, false};
	bool found = wf_es_ok(s) &&
	        first_in_line_ctx(l, from_col, left, s, &prep, at);

	prep_free(&prep);
	return found;
}

size_t wf_es_line_matches(
        const WfELine *l, const WfSearch *s, size_t *starts, size_t cap)
{
	size_t col = 0, n = 0, at;
	Prep prep = {NULL, false};

	if (!wf_es_ok(s)) {
		return 0;
	}
	while (n < cap && first_in_line(l, col, s, &prep, &at)) {
		starts[n++] = at;
		col = at + s->pat_n;
	}
	prep_free(&prep);
	return n;
}

size_t wf_es_positions(
        const WfEText *t, const WfSearch *s, size_t cap, WfPos *out)
{
	size_t line, n = 0;
	Prep prep = {NULL, false};

	if (!wf_es_ok(s)) {
		return 0;
	}
	for (line = 0; line < t->n; line++) {
		size_t col = 0, m;

		while (first_in_line(&t->lines[line], col, s, &prep, &m)) {
			if (n >= cap) {
				prep_free(&prep);
				return n;
			}
			out[n].line = line;
			out[n].col = m;
			n++;
			col = m + s->pat_n;
		}
	}
	prep_free(&prep);
	return n;
}

size_t wf_es_count(const WfEText *t,
        const WfSearch *s,
        size_t cap,
        WfPos at,
        size_t *index)
{
	size_t line, count = 0, col;
	Prep prep = {NULL, false};

	if (index) {
		*index = 0;
	}
	if (!wf_es_ok(s)) {
		return 0;
	}
	for (line = 0; line < t->n; line++) {
		col = 0;
		for (;;) {
			size_t m;

			if (!first_in_line(
			            &t->lines[line], col, s, &prep, &m)) {
				break;
			}
			count++;
			if (index && line == at.line && m == at.col) {
				*index = count;
			}
			if (count >= cap) {
				prep_free(&prep);
				return count;
			}
			col = m + s->pat_n;
		}
	}
	prep_free(&prep);
	return count;
}
