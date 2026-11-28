#include "wf_eundo.h"

#include <stdlib.h>
#include <string.h>

#define REC_OVERHEAD (sizeof(WfURec))
/* A merged typing run stays small enough to undo as "one word or so". */
#define MERGE_MAX 256u

void wf_un_init(WfUndo *u, size_t max_bytes)
{
	memset(u, 0, sizeof *u);
	u->max_bytes = max_bytes;
	u->next_group = 1;
}

static void rec_free(WfURec *r)
{
	free(r->del);
	free(r->ins);
	memset(r, 0, sizeof *r);
}

void wf_un_free(WfUndo *u)
{
	size_t i;

	for (i = 0; i < u->n; i++) {
		rec_free(&u->rec[i]);
	}
	free(u->rec);
	memset(u, 0, sizeof *u);
}

void wf_un_group_begin(WfUndo *u)
{
	if (!u->open_group) {
		u->open_group = u->next_group++;
		u->open_bytes = 0;
	}
}

void wf_un_group_end(WfUndo *u)
{
	u->open_group = 0;
	u->open_bytes = 0;
}

static size_t rec_bytes(const WfURec *r)
{
	return r->del_n + r->ins_n + REC_OVERHEAD;
}

WfPos wf_et_advance(WfPos at, const char *s, size_t n)
{
	size_t i = 0, piece = 0;
	WfPos p = at;

	while (i < n) {
		if ('\r' == s[i] && i + 1 < n && '\n' == s[i + 1]) {
			p.line++;
			piece = 0;
			p.col = 0;
			i += 2;
			continue;
		}
		if ('\r' == s[i] || '\n' == s[i]) {
			p.line++;
			piece = 0;
			p.col = 0;
			i++;
			continue;
		}
		piece++;
		i++;
	}
	p.col += piece;
	return p;
}

/* Drops the oldest records until the history fits its budget. */
static void trim(WfUndo *u)
{
	size_t drop = 0, freed = 0, i;

	if (!u->max_bytes || u->bytes <= u->max_bytes) {
		return;
	}
	/* leave a little room so that trimming does not run on every push */
	while (drop + 1 < u->pos &&
	        u->bytes - freed > u->max_bytes - u->max_bytes / 8) {
		if (u->open_group && u->rec[drop].group == u->open_group) {
			break;
		}
		freed += rec_bytes(&u->rec[drop]);
		drop++;
	}
	/* never split a group: drop to the end of the group we stopped in */
	while (drop < u->pos && drop > 0 &&
	        u->rec[drop].group == u->rec[drop - 1].group &&
	        0 != u->rec[drop].group) {
		freed += rec_bytes(&u->rec[drop]);
		drop++;
	}
	if (!drop) {
		return;
	}
	for (i = 0; i < drop; i++) {
		rec_free(&u->rec[i]);
	}
	memmove(u->rec, u->rec + drop, (u->n - drop) * sizeof *u->rec);
	u->n -= drop;
	u->pos -= drop;
	u->bytes -= freed;
	u->truncated = true;
	if (u->clean_pos < drop) {
		u->clean_lost = true;
	} else {
		u->clean_pos -= drop;
	}
}

static bool joins_whitespace_rule(const WfURec *prev, const WfURec *next)
{
	char last = prev->ins[prev->ins_n - 1];
	char first = next->ins[0];
	bool prev_ws = ' ' == last || '\t' == last;
	bool next_ws = ' ' == first || '\t' == first;

	/* "hello " then "w": a new word starts a new step */
	return !(prev_ws && !next_ws);
}

static bool try_merge(WfUndo *u, WfURec *r)
{
	WfURec *p;

	if (!u->pos || u->pos == u->clean_pos || u->open_group) {
		return false;
	}
	p = &u->rec[u->pos - 1];
	if (p->kind != r->kind || 0 != p->group || WF_UK_OTHER == r->kind) {
		return false;
	}
	if (WF_UK_TYPE == r->kind) {
		char *joined;

		if (p->del_n || r->del_n || !p->ins_n || !r->ins_n ||
		        p->ins_n + r->ins_n > MERGE_MAX ||
		        0 != wf_pos_cmp(p->after, r->at) ||
		        memchr(r->ins, '\n', r->ins_n) ||
		        memchr(r->ins, '\r', r->ins_n) ||
		        !joins_whitespace_rule(p, r)) {
			return false;
		}
		joined = wf_et_realloc(p->ins, p->ins_n + r->ins_n);
		if (!joined) {
			return false;
		}
		memcpy(joined + p->ins_n, r->ins, r->ins_n);
		p->ins = joined;
		p->ins_n += r->ins_n;
		p->after = r->after;
		u->bytes += r->ins_n;
		rec_free(r);
		return true;
	}
	if (WF_UK_BACK == r->kind) {
		char *joined;

		if (p->ins_n || r->ins_n || !p->del_n || !r->del_n ||
		        p->del_n + r->del_n > MERGE_MAX ||
		        wf_pos_cmp(wf_et_advance(r->at, r->del, r->del_n),
		                p->at) != 0) {
			return false;
		}
		joined = wf_et_malloc(p->del_n + r->del_n);
		if (!joined) {
			return false;
		}
		memcpy(joined, r->del, r->del_n);
		memcpy(joined + r->del_n, p->del, p->del_n);
		free(p->del);
		p->del = joined;
		p->del_n += r->del_n;
		p->at = r->at;
		p->after = r->after;
		u->bytes += r->del_n;
		rec_free(r);
		return true;
	}
	if (WF_UK_DEL == r->kind) {
		char *joined;

		if (p->ins_n || r->ins_n || !p->del_n || !r->del_n ||
		        p->del_n + r->del_n > MERGE_MAX ||
		        0 != wf_pos_cmp(p->at, r->at)) {
			return false;
		}
		joined = wf_et_realloc(p->del, p->del_n + r->del_n);
		if (!joined) {
			return false;
		}
		memcpy(joined + p->del_n, r->del, r->del_n);
		p->del = joined;
		p->del_n += r->del_n;
		p->after = r->after;
		u->bytes += r->del_n;
		rec_free(r);
		return true;
	}
	return false;
}

WfStatus wf_un_push(WfUndo *u, WfURec *r)
{
	size_t i;
	u->capacity_refused = false;
	/* Keep an Insert transaction whole. Refuse the next edit explicitly if
	 * it cannot fit, before throwing away redo or any existing record. */
	if (u->open_group && u->open_bytes && u->max_bytes &&
	        (u->open_bytes >= u->max_bytes ||
	                rec_bytes(r) > u->max_bytes - u->open_bytes)) {
		rec_free(r);
		u->capacity_refused = true;
		return WF_E_RANGE;
	}

	/* room first: a push that fails must not have thrown the redo history
	 * away for an edit that is then taken back */
	if (u->pos == u->cap) {
		size_t cap = u->cap ? u->cap * 2 : 64;
		WfURec *p = wf_et_realloc(u->rec, cap * sizeof *p);

		if (!p) {
			rec_free(r);
			return WF_E_NOMEM;
		}
		u->rec = p;
		u->cap = cap;
	}
	/* a new edit ends the redo history, and with it any saved state there */
	if (u->pos < u->n) {
		for (i = u->pos; i < u->n; i++) {
			u->bytes -= rec_bytes(&u->rec[i]);
			rec_free(&u->rec[i]);
		}
		u->n = u->pos;
		if (u->clean_pos > u->pos) {
			u->clean_lost = true;
		}
	}
	r->group = u->open_group;
	if (try_merge(u, r)) {
		trim(u);
		return WF_OK;
	}
	if (u->n == u->cap) {
		size_t cap = u->cap ? u->cap * 2 : 64;
		WfURec *p = wf_et_realloc(u->rec, cap * sizeof *p);

		if (!p) {
			rec_free(r);
			return WF_E_NOMEM;
		}
		u->rec = p;
		u->cap = cap;
	}
	u->rec[u->n++] = *r;
	memset(r, 0, sizeof *r);
	u->pos = u->n;
	u->bytes += rec_bytes(&u->rec[u->n - 1]);
	if (u->open_group) {
		u->open_bytes += rec_bytes(&u->rec[u->n - 1]);
	}
	trim(u);
	return WF_OK;
}

bool wf_un_can_undo(const WfUndo *u)
{
	return u->pos > 0;
}

bool wf_un_can_redo(const WfUndo *u)
{
	return u->pos < u->n;
}

/* The group that contains record `i`: [first, last]. */
static void group_of(const WfUndo *u, size_t i, size_t *first, size_t *last)
{
	uint32_t g = u->rec[i].group;
	size_t a = i, b = i;

	if (g) {
		while (a > 0 && u->rec[a - 1].group == g) {
			a--;
		}
		while (b + 1 < u->n && u->rec[b + 1].group == g) {
			b++;
		}
	}
	*first = a;
	*last = b;
}

bool wf_un_undo_span(const WfUndo *u, size_t *first, size_t *last)
{
	if (!u->pos) {
		return false;
	}
	group_of(u, u->pos - 1, first, last);
	/* a group undone in part (a step failed) is not undone again from its end */
	*last = u->pos - 1;
	return true;
}

bool wf_un_redo_span(const WfUndo *u, size_t *first, size_t *last)
{
	if (u->pos >= u->n) {
		return false;
	}
	group_of(u, u->pos, first, last);
	*first = u->pos; /* likewise for a group redone in part */
	return true;
}

void wf_un_mark_clean(WfUndo *u)
{
	u->clean_pos = u->pos;
	u->clean_lost = false;
}

bool wf_un_modified(const WfUndo *u)
{
	return u->clean_lost || u->pos != u->clean_pos;
}
