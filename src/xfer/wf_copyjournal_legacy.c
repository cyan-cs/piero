#include "wf_copyjournal_legacy.h"

#include <stdlib.h>
#include <string.h>
#include "../base/wf_mem.h"

#define ID_MAX 63u

typedef struct
{
	char kind;
	char id[ID_MAX + 1];
	const char *path; /* into the caller's data; not terminated */
	size_t path_n;
} Record;

static bool is_digit(char c)
{
	return c >= '0' && c <= '9';
}

static bool is_hex(char c)
{
	return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* The next space-separated token of [p, end); advances *p past one space. */
static bool token(const char **p, const char *end, const char **t, size_t *n)
{
	const char *s = *p;

	while (s < end && ' ' != *s) {
		s++;
	}
	if (s == end) {
		return false;
	}
	*t = *p;
	*n = (size_t)(s - *p);
	*p = s + 1;
	return *n > 0;
}

static bool parse_line(const char *line, size_t len, Record *r)
{
	const char *p = line, *end = line + len, *t;
	size_t n;

	if (len < 9 || !strchr("ICAVGD", line[0]) || ' ' != line[1]) {
		return false;
	}
	r->kind = line[0];
	p += 2;
	if (!token(&p, end, &t, &n) || n > 10) { /* pid, decimal */
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		if (!is_digit(t[i])) {
			return false;
		}
	}
	if (!token(&p, end, &t, &n) || n > 16) { /* start time, hex */
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		if (!is_hex(t[i])) {
			return false;
		}
	}
	if (!token(&p, end, &t, &n) || n > ID_MAX) { /* id or "-" */
		return false;
	}
	memcpy(r->id, t, n);
	r->id[n] = '\0';
	r->path = p;
	r->path_n = (size_t)(end - p);
	if (!r->path_n) {
		return false;
	}
	if ('A' == r->kind) { /* "<dest>|<aside>": exactly one separator */
		size_t bars = 0;

		for (size_t i = 0; i < r->path_n; i++) {
			bars += '|' == r->path[i];
		}
		return 1 == bars;
	}
	if (strchr("VGD", r->kind)) { /* these carry only an id */
		return 1 == r->path_n && '-' == r->path[0];
	}
	return true;
}

typedef struct
{
	const Record *r;
	size_t at; /* position in the journal */
} Ref;

typedef int (*RecOrder)(const Record *a, const Record *b);

static int id_order(const Record *a, const Record *b)
{
	return strcmp(a->id, b->id);
}

static int path_order(const Record *a, const Record *b)
{
	if (a->path_n != b->path_n) {
		return a->path_n < b->path_n ? -1 : 1;
	}
	return memcmp(a->path, b->path, a->path_n);
}

static int ref_cmp(const Ref *a, const Ref *b, RecOrder order)
{
	int c = order(a->r, b->r);

	return c ? c : (a->at > b->at) - (a->at < b->at);
}

static int ref_by_id(const void *a, const void *b)
{
	return ref_cmp(a, b, id_order);
}

static int ref_by_path(const void *a, const void *b)
{
	return ref_cmp(a, b, path_order);
}

/* Whether v (sorted by `order`, then position) holds a record equal to `key`
 * that comes after position `from`: the last of the equal ones decides. */
static bool later_match(
        const Ref *v, size_t n, const Record *key, size_t from, RecOrder order)
{
	size_t lo = 0, hi = n; /* the first entry greater than key */

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;

		if (order(v[mid].r, key) <= 0) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return lo && 0 == order(v[lo - 1].r, key) && v[lo - 1].at > from;
}

/* Counts the created and set-aside records that no later "gone" record with
 * the same id closes, and the intents that no later "created" record for the
 * same path closes. One sort of the closing records and a search for each
 * opening one: a journal of any length costs n log n. false: out of memory. */
static bool close_scan(
        const Record *rec, size_t count, WfLegacyJournalReport *out)
{
	Ref *gone, *made;
	size_t n_gone = 0, n_made = 0;

	gone = wf_malloc_array(count ? count : 1, sizeof *gone);
	made = wf_malloc_array(count ? count : 1, sizeof *made);
	if (!gone || !made) {
		free(gone);
		free(made);
		return false;
	}
	for (size_t i = 0; i < count; i++) {
		Ref r = {&rec[i], i};

		if ('G' == rec[i].kind) {
			gone[n_gone++] = r;
		} else if ('C' == rec[i].kind) {
			made[n_made++] = r;
		}
	}
	qsort(gone, n_gone, sizeof *gone, ref_by_id);
	qsort(made, n_made, sizeof *made, ref_by_path);
	for (size_t i = 0; i < count; i++) {
		if ('C' == rec[i].kind || 'A' == rec[i].kind) {
			if (!later_match(gone, n_gone, &rec[i], i, id_order)) {
				if ('C' == rec[i].kind) {
					out->unresolved_created++;
				} else {
					out->unresolved_asides++;
				}
			}
		} else if ('I' == rec[i].kind &&
		        !later_match(made, n_made, &rec[i], i, path_order)) {
			out->unresolved_intents++;
		}
	}
	free(gone);
	free(made);
	return true;
}

bool wf_cj_legacy_analyze(
        const char *data, size_t n, WfLegacyJournalReport *out)
{
	Record *rec = NULL;
	size_t count = 0, cap = 0, at = 0;

	if (!out || (n && !data)) {
		return false;
	}
	memset(out, 0, sizeof *out);
	out->bytes = n;
	out->empty = 0 == n;
	while (at < n) {
		const char *nl = memchr(data + at, '\n', n - at);
		size_t line_end = nl ? (size_t)(nl - data) : n;
		size_t len = line_end - at;
		size_t next = nl ? line_end + 1 : n;
		Record r;

		if (!nl) {
			out->unterminated_tail = true;
		}
		if (len && '\r' == data[at + len - 1]) {
			len--;
		}
		out->lines++;
		if (parse_line(data + at, len, &r)) {
			if (count == cap) {
				size_t grown = cap ? cap * 2 : 16;
				Record *g = wf_realloc_array(
				        rec, grown, sizeof *rec);

				if (!g) {
					free(rec);
					memset(out, 0, sizeof *out);
					return false;
				}
				rec = g;
				cap = grown;
			}
			rec[count++] = r;
			switch (r.kind) {
			case 'I': out->intents++; break;
			case 'C': out->created++; break;
			case 'A': out->asides++; break;
			case 'V': out->verified++; break;
			case 'D': out->discards++; break;
			default:  out->gone++; break;
			}
		} else {
			out->malformed++;
		}
		at = next;
	}
	if (!close_scan(rec, count, out)) {
		free(rec);
		memset(out, 0, sizeof *out);
		return false;
	}
	free(rec);
	out->complete_proven = !out->empty && !out->malformed &&
	        !out->unterminated_tail && !out->unresolved_created &&
	        !out->unresolved_asides && !out->unresolved_intents;
	return true;
}
