#include "wf_mdinline.h"

#include <stdlib.h>
#include <string.h>

#include "../base/wf_buf.h"
#include "../base/wf_mem.h"
#include "wf_text.h"

/*
 * Inline Markdown: code spans, escapes, emphasis by CommonMark's delimiter
 * stack, links, images, autolinks, entities. The text is first cut into
 * nodes (text, delimiter runs, brackets, breaks); emphasis and links are
 * then resolved over the nodes; at the end every node becomes display units.
 * Emphasis is applied by interval marks and a running sum, so deeply nested
 * input costs a pass, not a pass per level.
 */

void wf_mdu_trim(WfMdUnits *u)
{
	WfMdUnit *p;

	if (u->n == u->cap) {
		return;
	}
	if (!u->n) {
		wf_mdu_free(u);
		return;
	}
	p = realloc(u->u, u->n * sizeof *p);
	if (p) {
		u->u = p;
		u->cap = u->n;
	}
}

void wf_mdu_free(WfMdUnits *u)
{
	free(u->u);
	u->u = NULL;
	u->n = u->cap = 0;
}

WfStatus wf_mdu_push(WfMdUnits *u, uint32_t cp, uint16_t style, uint8_t flags)
{
	int w;

	if (u->n == u->cap) {
		size_t cap = u->cap ? u->cap * 2 : 64;
		WfMdUnit *p = wf_realloc_array(u->u, cap, sizeof *p);

		if (!p) {
			return WF_E_NOMEM;
		}
		u->u = p;
		u->cap = cap;
	}
	w = wf_cp_width(cp);
	u->u[u->n].cp = cp;
	u->u[u->n].style = style;
	u->u[u->n].w = (uint8_t)(w < 0 ? 0 : w > 2 ? 2 : w);
	u->u[u->n].flags = flags;
	u->n++;
	return WF_OK;
}

WfStatus wf_mdu_push_text(WfMdUnits *u, const char *s, size_t n, uint16_t style)
{
	size_t i = 0;

	while (i < n) {
		uint32_t cp;
		size_t k = wf_utf8_decode(s + i, n - i, &cp);
		WfStatus st;

		if (!k) {
			break;
		}
		i += k;
		st = wf_mdu_push(u, cp, style, ' ' == cp ? WF_MDU_SPACE : 0);
		if (WF_OK != st) {
			return st;
		}
	}
	return WF_OK;
}

size_t wf_md_ref_normalize(const char *s, size_t n, char *out, size_t cap)
{
	size_t i, k = 0;
	bool gap = false;

	if (!cap) {
		return 0;
	}
	for (i = 0; i < n && k + 1 < cap; i++) {
		unsigned char c = (unsigned char)s[i];

		if (' ' == c || '\t' == c || '\n' == c || '\r' == c) {
			gap = k > 0;
			continue;
		}
		if (gap) {
			out[k++] = ' ';
			gap = false;
			if (k + 1 >= cap) {
				break;
			}
		}
		out[k++] = c >= 'A' && c <= 'Z' ? (char)(c + 32) : (char)c;
	}
	out[k] = '\0';
	return k;
}

static bool ascii_punct(uint32_t c)
{
	return (c >= '!' && c <= '/') || (c >= ':' && c <= '@') ||
	        (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
}

static bool is_ws(uint32_t c)
{
	return ' ' == c || '\t' == c || '\n' == c || '\r' == c || 0x0B == c ||
	        0x0C == c || 0xA0 == c || 0x3000 == c ||
	        (c >= 0x2000 && c <= 0x200A) || 0x202F == c || 0x205F == c ||
	        0x1680 == c;
}

/* An approximation of Unicode punctuation and symbols: enough to decide
 * where emphasis may start and stop next to CJK and emoji. */
static bool is_punct(uint32_t c)
{
	if (c < 0x80) {
		return ascii_punct(c);
	}
	return (c >= 0xA1 && c <= 0xBF && 0xAA != c && 0xB5 != c &&
	               0xBA != c) ||
	        0xD7 == c || 0xF7 == c || (c >= 0x2010 && c <= 0x2027) ||
	        (c >= 0x2030 && c <= 0x205E) || (c >= 0x20A0 && c <= 0x20CF) ||
	        (c >= 0x2190 && c <= 0x2BFF) || (c >= 0x3001 && c <= 0x3003) ||
	        (c >= 0x3008 && c <= 0x3020) || 0x3030 == c || 0x30FB == c ||
	        (c >= 0xFE30 && c <= 0xFE6F) || (c >= 0xFF01 && c <= 0xFF0F) ||
	        (c >= 0xFF1A && c <= 0xFF20) || (c >= 0xFF3B && c <= 0xFF40) ||
	        (c >= 0xFF5B && c <= 0xFF65) || (c >= 0x1F000 && c <= 0x1FAFF);
}

static bool alnum_byte(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
	        (c >= 'A' && c <= 'Z') || (unsigned char)c >= 0x80;
}

enum
{
	N_TEXT,
	N_CODE,
	N_DELIM,
	N_SOFT,
	N_HARD,
	N_OPEN,
	N_DEAD
};

typedef struct
{
	uint8_t type;
	uint8_t ch;
	bool can_open, can_close, image, active;
	uint16_t style;
	uint32_t off, len; /* text in the arena */
	uint32_t cnt, orig;
	uint32_t spos; /* N_OPEN: where its content starts in the source */
	int32_t dprev, dnext;
} Node;

typedef struct
{
	int32_t node;
	int32_t last_delim;
	uint8_t images; /* images open in the stack, this one included */
} Bracket;

/* The description of an image is the text of what is inside it, copied once for
 * each image around that text. Deeper than this an opener stays literal, so
 * the copying is at most this many times the input. */
#define IMAGE_NEST_MAX 16u

typedef struct
{
	uint32_t lo, hi;
	uint8_t flag; /* 0 bold, 1 italic, 2 strike, 3 link */
} Span;

typedef struct
{
	const char *s;
	size_t n;
	WfBuf arena;
	Node *nd;
	size_t nn, ncap;
	Span *sp;
	size_t ns, scap;
	Bracket *bk;
	size_t nb, bcap;
	/* bk[0 .. nb_dead) are the openers a link has already turned off */
	size_t nb_dead;
	int32_t dhead, dtail;
	size_t code_fail[65];
	WfStatus err;
	WfMdRefFn ref;
	void *ref_ctx;
} P;

static void fail(P *p, WfStatus st)
{
	if (WF_OK == p->err) {
		p->err = st;
	}
}

static int32_t add_node(P *p, uint8_t type)
{
	Node *n;

	if (WF_OK != p->err) {
		return -1;
	}
	if (p->nn == p->ncap) {
		size_t cap = p->ncap ? p->ncap * 2 : 64;
		Node *q = wf_realloc_array(p->nd, cap, sizeof *q);

		if (!q) {
			fail(p, WF_E_NOMEM);
			return -1;
		}
		p->nd = q;
		p->ncap = cap;
	}
	n = &p->nd[p->nn];
	memset(n, 0, sizeof *n);
	n->type = type;
	n->dprev = n->dnext = -1;
	return (int32_t)p->nn++;
}

/* Copies text into the arena and returns its offset. */
static uint32_t arena_put(P *p, const char *t, size_t len)
{
	uint32_t off = (uint32_t)p->arena.len;

	if (WF_OK != wf_buf_append(&p->arena, t, len)) {
		fail(p, WF_E_NOMEM);
	}
	return off;
}

static void text_node(
        P *p, uint8_t type, const char *t, size_t len, uint16_t st)
{
	int32_t i;

	if (!len) {
		return;
	}
	i = add_node(p, type);
	if (i < 0) {
		return;
	}
	p->nd[i].off = arena_put(p, t, len);
	p->nd[i].len = (uint32_t)len;
	p->nd[i].style = st;
}

static void add_span(P *p, uint32_t lo, uint32_t hi, uint8_t flag)
{
	if (WF_OK != p->err || lo >= hi) {
		return;
	}
	if (p->ns == p->scap) {
		size_t cap = p->scap ? p->scap * 2 : 32;
		Span *q = wf_realloc_array(p->sp, cap, sizeof *q);

		if (!q) {
			fail(p, WF_E_NOMEM);
			return;
		}
		p->sp = q;
		p->scap = cap;
	}
	p->sp[p->ns].lo = lo;
	p->sp[p->ns].hi = hi;
	p->sp[p->ns].flag = flag;
	p->ns++;
}

static void delim_link(P *p, int32_t i)
{
	p->nd[i].dprev = p->dtail;
	p->nd[i].dnext = -1;
	if (p->dtail >= 0) {
		p->nd[p->dtail].dnext = i;
	} else {
		p->dhead = i;
	}
	p->dtail = i;
}

static void delim_unlink(P *p, int32_t i)
{
	Node *x = &p->nd[i];

	if (x->dprev >= 0) {
		p->nd[x->dprev].dnext = x->dnext;
	} else {
		p->dhead = x->dnext;
	}
	if (x->dnext >= 0) {
		p->nd[x->dnext].dprev = x->dprev;
	} else {
		p->dtail = x->dprev;
	}
	x->dprev = x->dnext = -1;
}

static void process_emphasis(P *p, int32_t bottom)
{
	int32_t ob[3][2][3];
	int32_t closer;
	int a, b, c3;

	for (a = 0; a < 3; a++) {
		for (b = 0; b < 2; b++) {
			for (c3 = 0; c3 < 3; c3++) {
				ob[a][b][c3] = bottom;
			}
		}
	}
	closer = bottom < 0 ? p->dhead : p->nd[bottom].dnext;
	while (closer >= 0 && WF_OK == p->err) {
		Node *c = &p->nd[closer];
		int kind = '*' == c->ch ? 0 : '_' == c->ch ? 1 : 2;
		int32_t opener, lim;
		bool found = false;

		if (!c->can_close) {
			closer = c->dnext;
			continue;
		}
		lim = ob[kind][c->can_open ? 1 : 0][c->orig % 3];
		opener = c->dprev;
		while (opener >= 0 && opener != bottom && opener != lim) {
			Node *o = &p->nd[opener];

			if (o->ch == c->ch && o->can_open) {
				bool odd;

				if ('~' == c->ch) {
					odd = o->cnt != c->cnt;
				} else {
					odd = (c->can_open || o->can_close) &&
					        0 == (o->orig + c->orig) % 3 &&
					        !(0 == o->orig % 3 &&
					                0 == c->orig % 3);
				}
				if (!odd) {
					found = true;
					break;
				}
			}
			opener = o->dprev;
		}
		if (found) {
			Node *o = &p->nd[opener];
			uint32_t use = '~' == c->ch            ? c->cnt
			        : (c->cnt >= 2 && o->cnt >= 2) ? 2
			                                       : 1;
			uint8_t flag = '~' == c->ch ? 2 : 2 == use ? 0 : 1;

			add_span(p, (uint32_t)opener + 1, (uint32_t)closer,
			        flag);
			o->cnt -= use;
			c->cnt -= use;
			o->dnext = closer; /* the ones between stay as text */
			c->dprev = opener;
			if (0 == o->cnt) {
				delim_unlink(p, opener);
			}
			if (0 == c->cnt) {
				int32_t nx = c->dnext;

				delim_unlink(p, closer);
				closer = nx;
			}
		} else {
			int32_t nx = c->dnext;

			ob[kind][c->can_open ? 1 : 0][c->orig % 3] = c->dprev;
			if (!c->can_open) {
				delim_unlink(p, closer);
			}
			closer = nx;
		}
	}
	p->dtail = bottom;
	if (bottom >= 0) {
		p->nd[bottom].dnext = -1;
	} else {
		p->dhead = -1;
	}
}

static uint32_t scalar_before(const char *s, size_t i)
{
	size_t k = i, steps = 0;
	uint32_t cp;

	if (!i) {
		return ' ';
	}
	k--;
	while (k > 0 && steps < 3 && 0x80 == ((unsigned char)s[k] & 0xC0)) {
		k--;
		steps++;
	}
	(void)wf_utf8_decode(s + k, i - k, &cp);
	return cp;
}

static uint32_t scalar_at(const char *s, size_t n, size_t i)
{
	uint32_t cp;

	if (i >= n) {
		return ' ';
	}
	(void)wf_utf8_decode(s + i, n - i, &cp);
	return cp;
}

static bool starts_with(const char *s, size_t n, size_t i, const char *lit)
{
	size_t k = strlen(lit);

	return i + k <= n && !memcmp(s + i, lit, k);
}

static bool bare_link_at(const char *s, size_t n, size_t i)
{
	if (i > 0 && alnum_byte(s[i - 1])) {
		return false;
	}
	if (starts_with(s, n, i, "http://")) {
		return i + 7 < n && (unsigned char)s[i + 7] > ' ';
	}
	if (starts_with(s, n, i, "https://")) {
		return i + 8 < n && (unsigned char)s[i + 8] > ' ';
	}
	if (starts_with(s, n, i, "www.")) {
		return i + 4 < n && alnum_byte(s[i + 4]);
	}
	return false;
}

static bool special_at(const char *s, size_t n, size_t i)
{
	switch (s[i]) {
	case '\\':
	case '`':
	case '*':
	case '_':
	case '~':
	case '[':
	case ']':
	case '<':
	case '&':
	case '\n': return true;
	case '!':  return i + 1 < n && '[' == s[i + 1];
	case 'h':
	case 'w':  return bare_link_at(s, n, i);
	default:   return false;
	}
}

static size_t do_escape(P *p, size_t i)
{
	const char *s = p->s;

	if (i + 1 < p->n && ascii_punct((unsigned char)s[i + 1])) {
		text_node(p, N_TEXT, s + i + 1, 1, 0);
		return i + 2;
	}
	if (i + 1 < p->n && '\n' == s[i + 1]) {
		(void)add_node(p, N_HARD);
		return i + 2;
	}
	text_node(p, N_TEXT, "\\", 1, 0);
	return i + 1;
}

static size_t do_code(P *p, size_t i)
{
	const char *s = p->s;
	size_t n = p->n, run = 0, j;
	bool all_space = true;
	char *tmp;
	size_t len;

	while (i + run < n && '`' == s[i + run]) {
		run++;
	}
	if (run > 64 || p->code_fail[run] <= i + run) {
		text_node(p, N_TEXT, s + i, run, 0);
		return i + run;
	}
	j = i + run;
	while (j < n) {
		size_t k = 0;

		if ('`' != s[j]) {
			j++;
			continue;
		}
		while (j + k < n && '`' == s[j + k]) {
			k++;
		}
		if (k == run) {
			break;
		}
		j += k;
	}
	if (j >= n) {
		p->code_fail[run] = i + run;
		text_node(p, N_TEXT, s + i, run, 0);
		return i + run;
	}
	len = j - (i + run);
	tmp = malloc(len + 1);
	if (!tmp) {
		fail(p, WF_E_NOMEM);
		return n;
	}
	for (size_t k = 0; k < len; k++) {
		char c = s[i + run + k];

		tmp[k] = '\n' == c ? ' ' : c;
		if (' ' != tmp[k]) {
			all_space = false;
		}
	}
	if (len >= 2 && ' ' == tmp[0] && ' ' == tmp[len - 1] && !all_space) {
		text_node(p, N_CODE, tmp + 1, len - 2, 0);
	} else {
		text_node(p, N_CODE, tmp, len, 0);
	}
	free(tmp);
	return j + run;
}

static size_t do_delim(P *p, size_t i)
{
	const char *s = p->s;
	size_t n = p->n, j = i;
	char ch = s[i];
	uint32_t prev, next;
	bool ws_n, ws_p, pn, pp, left, right;
	int32_t k;

	while (j < n && s[j] == ch) {
		j++;
	}
	if ('~' == ch && j - i > 2) {
		text_node(p, N_TEXT, s + i, j - i, 0);
		return j;
	}
	prev = scalar_before(s, i);
	next = scalar_at(s, n, j);
	ws_n = is_ws(next);
	ws_p = is_ws(prev);
	pn = is_punct(next);
	pp = is_punct(prev);
	left = !ws_n && (!pn || ws_p || pp);
	right = !ws_p && (!pp || ws_n || pn);
	k = add_node(p, N_DELIM);
	if (k < 0) {
		return n;
	}
	p->nd[k].ch = (uint8_t)ch;
	p->nd[k].cnt = p->nd[k].orig = (uint32_t)(j - i);
	if ('_' == ch) {
		p->nd[k].can_open = left && (!right || pp);
		p->nd[k].can_close = right && (!left || pn);
	} else {
		p->nd[k].can_open = left;
		p->nd[k].can_close = right;
	}
	delim_link(p, k);
	return j;
}

/* False when an image would nest deeper than IMAGE_NEST_MAX: nothing is
 * pushed, and the caller keeps the "![" as text. */
static bool push_bracket(P *p, size_t i, bool image)
{
	unsigned images = p->nb ? p->bk[p->nb - 1].images : 0;
	int32_t k;

	if (image && images >= IMAGE_NEST_MAX) {
		return false;
	}
	k = add_node(p, N_OPEN);
	if (k < 0) {
		return true;
	}
	p->nd[k].image = image;
	p->nd[k].active = true;
	p->nd[k].spos = (uint32_t)(i + (image ? 2 : 1));
	if (p->nb == p->bcap) {
		size_t cap = p->bcap ? p->bcap * 2 : 16;
		Bracket *q = wf_realloc_array(p->bk, cap, sizeof *q);

		if (!q) {
			fail(p, WF_E_NOMEM);
			return true;
		}
		p->bk = q;
		p->bcap = cap;
	}
	p->bk[p->nb].node = k;
	p->bk[p->nb].last_delim = p->dtail;
	p->bk[p->nb].images = (uint8_t)(images + (image ? 1u : 0u));
	p->nb++;
	return true;
}

/* (dest "title") starting at the '(' at `pos`. */
static bool parse_dest(const char *s,
        size_t n,
        size_t pos,
        size_t *dest,
        size_t *dn,
        size_t *end)
{
	size_t i = pos + 1;
	size_t start, limit;
	int depth = 0;

	while (i < n && (' ' == s[i] || '\n' == s[i])) {
		i++;
	}
	if (i < n && '<' == s[i]) {
		start = ++i;
		while (i < n && '>' != s[i] && '\n' != s[i] && '<' != s[i] &&
		        i - start < 2048) {
			if ('\\' == s[i] && i + 1 < n) {
				i++;
			}
			i++;
		}
		if (i >= n || '>' != s[i]) {
			return false;
		}
		*dest = start;
		*dn = i - start;
		i++;
	} else {
		start = i;
		limit = start + 2048;
		while (i < n && i < limit) {
			unsigned char c = (unsigned char)s[i];

			if ('\\' == c && i + 1 < n &&
			        ascii_punct((unsigned char)s[i + 1])) {
				i += 2;
				continue;
			}
			if (c <= ' ' || 0x7F == c) {
				break;
			}
			if ('(' == c) {
				depth++;
			} else if (')' == c) {
				if (!depth) {
					break;
				}
				depth--;
			}
			i++;
		}
		if (depth || i >= n || i >= limit) {
			return false;
		}
		*dest = start;
		*dn = i - start;
	}
	while (i < n && (' ' == s[i] || '\n' == s[i])) {
		i++;
	}
	if (i < n && ('"' == s[i] || '\'' == s[i] || '(' == s[i])) {
		char close = '(' == s[i] ? ')' : s[i];
		size_t t = i + 1, cap = i + 1 + 1024;

		while (t < n && t < cap && s[t] != close) {
			if ('\\' == s[t] && t + 1 < n) {
				t++;
			}
			t++;
		}
		if (t >= n || t >= cap || s[t] != close) {
			return false;
		}
		i = t + 1;
		while (i < n && (' ' == s[i] || '\n' == s[i])) {
			i++;
		}
	}
	if (i >= n || ')' != s[i]) {
		return false;
	}
	*end = i + 1;
	return true;
}

/* Plain text of nodes [lo, hi), into a malloc'd string. */
static char *plain_of(const P *p, size_t lo, size_t hi, size_t *len)
{
	size_t total = 1, i, k = 0;
	char *out;

	for (i = lo; i < hi;
	        i++) { /* the nodes that are still there, not the dead */
		const Node *x = &p->nd[i];

		if (N_TEXT == x->type || N_CODE == x->type) {
			total += x->len;
		} else if (N_DELIM == x->type) {
			total += x->cnt;
		} else if (N_SOFT == x->type || N_HARD == x->type) {
			total++;
		}
	}
	out = malloc(total);
	if (!out) {
		return NULL;
	}
	for (i = lo; i < hi; i++) {
		const Node *x = &p->nd[i];

		if (N_TEXT == x->type || N_CODE == x->type) {
			memcpy(out + k, p->arena.data + x->off, x->len);
			k += x->len;
		} else if (N_DELIM == x->type) {
			memset(out + k, x->ch, x->cnt);
			k += x->cnt;
		} else if (N_SOFT == x->type || N_HARD == x->type) {
			out[k++] = ' ';
		}
	}
	out[k] = '\0';
	*len = k;
	return out;
}

/* A destination, with its backslash escapes taken out. */
static size_t unescape(const char *s, size_t n, char *out)
{
	size_t i, k = 0;

	for (i = 0; i < n; i++) {
		if ('\\' == s[i] && i + 1 < n &&
		        ascii_punct((unsigned char)s[i + 1])) {
			i++;
		}
		out[k++] = s[i];
	}
	return k;
}

/* Takes the top opener off the stack. */
static void bracket_pop(P *p)
{
	p->nb--;
	if (p->nb_dead > p->nb) {
		p->nb_dead = p->nb;
	}
}

static size_t do_close(P *p, size_t i)
{
	const char *s = p->s;
	size_t n = p->n, after = i + 1, end = 0, d0 = 0, dn = 0;
	const char *dest = NULL;
	bool ok = false;
	Bracket b;
	Node *o;
	size_t lo, hi;
	char *ubuf = NULL;

	if (!p->nb) {
		text_node(p, N_TEXT, "]", 1, 0);
		return i + 1;
	}
	b = p->bk[p->nb - 1];
	o = &p->nd[b.node];
	if (!o->active) {
		bracket_pop(p);
		text_node(p, N_TEXT, "]", 1, 0);
		return i + 1;
	}
	if (after < n && '(' == s[after]) {
		ok = parse_dest(s, n, after, &d0, &dn, &end);
		if (ok) {
			dest = s + d0;
		}
	}
	if (!ok && p->ref) {
		const char *lbl = NULL;
		size_t ln = 0;

		if (after < n && '[' == s[after]) {
			size_t j = after + 1;

			while (j < n && ']' != s[j] && '[' != s[j] &&
			        j - after < 1000) {
				if ('\\' == s[j] && j + 1 < n) {
					j++;
				}
				j++;
			}
			if (j < n && ']' == s[j]) {
				if (j == after + 1) { /* [text][] */
					lbl = s + o->spos;
					ln = i - o->spos;
				} else {
					lbl = s + after + 1;
					ln = j - after - 1;
				}
				end = j + 1;
			}
		} else {
			lbl = s + o->spos;
			ln = i - o->spos;
			end = after;
		}
		if (lbl && ln && p->ref(p->ref_ctx, lbl, ln, &dest, &dn)) {
			ok = true;
		}
	}
	if (!ok) {
		bracket_pop(p);
		text_node(p, N_TEXT, "]", 1, 0);
		return i + 1;
	}
	process_emphasis(p, b.last_delim);
	bracket_pop(p);
	o->type = N_DEAD;
	lo = (size_t)b.node + 1;
	hi = p->nn;
	if (o->image) {
		size_t plen = 0;
		char *alt = plain_of(p, lo, hi, &plen);
		char *text;
		size_t tl;

		if (!alt) {
			fail(p, WF_E_NOMEM);
			return end;
		}
		for (size_t k = lo; k < hi; k++) {
			p->nd[k].type = N_DEAD;
		}
		text = malloc(plen + 16);
		if (!text) {
			free(alt);
			fail(p, WF_E_NOMEM);
			return end;
		}
		if (plen) {
			memcpy(text, "[image: ", 8);
			tl = 8;
			memcpy(text + tl, alt, plen);
			tl += plen;
			text[tl++] = ']';
		} else {
			memcpy(text, "[image]", 7);
			tl = 7;
		}
		text_node(p, N_TEXT, text, tl, WF_MDS_IMAGE);
		free(text);
		free(alt);
		return end;
	}
	add_span(p, (uint32_t)lo, (uint32_t)hi, 3);
	/* a link cannot hold a link: the openers below are done with once, and not
	 * again for every link that follows */
	for (size_t k = p->nb_dead; k < p->nb; k++) {
		if (!p->nd[p->bk[k].node].image) {
			p->nd[p->bk[k].node].active = false;
		}
	}
	p->nb_dead = p->nb;
	if (dn) {
		size_t plen = 0, ul;
		char *plain = plain_of(p, lo, hi, &plen);

		ubuf = malloc(dn + 4);
		if (!plain || !ubuf) {
			free(plain);
			free(ubuf);
			fail(p, WF_E_NOMEM);
			return end;
		}
		ul = unescape(dest, dn, ubuf);
		if (!plen) { /* [](url): the address is the link */
			text_node(p, N_TEXT, ubuf, ul, 0);
			add_span(p, (uint32_t)(p->nn - 1), (uint32_t)p->nn, 3);
		} else if (plen != ul || memcmp(plain, ubuf, ul)) {
			char *shown = malloc(ul + 4);

			if (!shown) {
				fail(p, WF_E_NOMEM);
			} else {
				shown[0] = ' ';
				shown[1] = '(';
				memcpy(shown + 2, ubuf, ul);
				shown[2 + ul] = ')';
				text_node(p, N_TEXT, shown, ul + 3, WF_MDS_URL);
				free(shown);
			}
		}
		free(plain);
		free(ubuf);
	}
	return end;
}

static size_t do_angle(P *p, size_t i)
{
	const char *s = p->s;
	size_t n = p->n, j = i + 1;
	bool scheme = false, at = false;

	if (j < n &&
	        ((s[j] >= 'a' && s[j] <= 'z') ||
	                (s[j] >= 'A' && s[j] <= 'Z'))) {
		size_t k = j;

		while (k < n && k - j < 32 &&
		        (alnum_byte(s[k]) || '+' == s[k] || '.' == s[k] ||
		                '-' == s[k])) {
			k++;
		}
		scheme = k > j + 1 && k < n && ':' == s[k];
	}
	while (j < n && j - i < 2048 && '>' != s[j] && '<' != s[j] &&
	        (unsigned char)s[j] > ' ') {
		if ('@' == s[j]) {
			at = true;
		}
		j++;
	}
	if (j < n && '>' == s[j] && j > i + 1 && (scheme || at)) {
		int32_t k;

		text_node(p, N_TEXT, s + i + 1, j - i - 1, 0);
		k = (int32_t)p->nn - 1;
		if (k >= 0) {
			add_span(p, (uint32_t)k, (uint32_t)k + 1, 3);
		}
		return j + 1;
	}
	text_node(p, N_TEXT, "<", 1, 0);
	return i + 1;
}

static size_t do_bare(P *p, size_t i)
{
	const char *s = p->s;
	size_t n = p->n, j = i, parens = 0, close = 0;
	int32_t k;

	while (j < n && (unsigned char)s[j] > ' ' && '<' != s[j] &&
	        ']' != s[j]) {
		parens += '(' == s[j];
		close += ')' == s[j];
		j++;
	}
	while (j > i + 1) { /* trailing punctuation is not the address */
		char c = s[j - 1];

		if (strchr("?!.,:*_~'\";", c)) {
			j--;
		} else if (')' == c) {
			/* `close` counts the ")" left in the address: counted once,
			 * and one fewer for each that is cut */
			if (close <= parens) {
				break;
			}
			close--;
			j--;
		} else {
			break;
		}
	}
	text_node(p, N_TEXT, s + i, j - i, 0);
	k = (int32_t)p->nn - 1;
	if (k >= 0) {
		add_span(p, (uint32_t)k, (uint32_t)k + 1, 3);
	}
	return j;
}

static const struct
{
	const char *name;
	uint32_t cp;
} ENTITIES[] = {{"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'},
        {"apos", '\''}, {"nbsp", 0xA0}, {"copy", 0xA9}, {"reg", 0xAE},
        {"mdash", 0x2014}, {"ndash", 0x2013}, {"hellip", 0x2026},
        {"laquo", 0xAB}, {"raquo", 0xBB}, {"trade", 0x2122}};

static size_t do_entity(P *p, size_t i)
{
	const char *s = p->s;
	size_t n = p->n, j = i + 1;
	uint32_t cp = 0;
	bool ok = false;
	char enc[4];
	size_t el;

	if (j < n && '#' == s[j]) {
		bool hex = j + 1 < n && ('x' == s[j + 1] || 'X' == s[j + 1]);
		size_t digits = 0;

		j += hex ? 2 : 1;
		while (j < n && digits < 8) {
			char c = s[j];
			uint32_t d;

			if (c >= '0' && c <= '9') {
				d = (uint32_t)(c - '0');
			} else if (hex && c >= 'a' && c <= 'f') {
				d = (uint32_t)(c - 'a' + 10);
			} else if (hex && c >= 'A' && c <= 'F') {
				d = (uint32_t)(c - 'A' + 10);
			} else {
				break;
			}
			cp = cp * (hex ? 16u : 10u) + d;
			digits++;
			j++;
		}
		ok = digits && digits <= 7 && j < n && ';' == s[j];
		if (ok &&
		        (0 == cp || cp > 0x10FFFF ||
		                (cp >= 0xD800 && cp < 0xE000) || cp < 0x20 ||
		                (cp >= 0x7F && cp < 0xA0))) {
			cp = WF_CP_REPLACEMENT;
		}
	} else {
		size_t k;

		for (k = 0; k < sizeof ENTITIES / sizeof ENTITIES[0]; k++) {
			size_t l = strlen(ENTITIES[k].name);

			if (j + l < n && !memcmp(s + j, ENTITIES[k].name, l) &&
			        ';' == s[j + l]) {
				cp = ENTITIES[k].cp;
				j += l;
				ok = true;
				break;
			}
		}
	}
	if (!ok) {
		text_node(p, N_TEXT, "&", 1, 0);
		return i + 1;
	}
	el = wf_utf8_encode(cp, enc);
	/* Decoding an entity must not reintroduce controls sanitized in the source. */
	{
		size_t off = p->arena.len;
		WfStatus st = wf_sanitize_utf8(enc, el, &p->arena);
		int32_t node;

		if (WF_OK != st) {
			fail(p, st);
			return j + 1;
		}
		node = add_node(p, N_TEXT);
		if (node >= 0) {
			p->nd[node].off = (uint32_t)off;
			p->nd[node].len = (uint32_t)(p->arena.len - off);
		}
	}
	return j + 1;
}

static void tokenize(P *p)
{
	const char *s = p->s;
	size_t n = p->n, i = 0;

	while (i < n && WF_OK == p->err) {
		size_t k = i;

		if (p->nn >=
		        WF_MD_NODES) { /* too many: the rest is just text */
			text_node(p, N_TEXT, s + i, n - i, 0);
			return;
		}
		while (k < n && !special_at(s, n, k)) {
			k++;
		}
		if (k > i) {
			text_node(p, N_TEXT, s + i, k - i, 0);
			i = k;
			continue;
		}
		switch (s[i]) {
		case '\\': i = do_escape(p, i); break;
		case '`':  i = do_code(p, i); break;
		case '*':
		case '_':
		case '~':  i = do_delim(p, i); break;
		case '[':
			push_bracket(p, i, false);
			i++;
			break;
		case '!':
			if (!push_bracket(p, i, true)) {
				text_node(p, N_TEXT, "![", 2, 0);
			}
			i += 2;
			break;
		case ']': i = do_close(p, i); break;
		case '<': i = do_angle(p, i); break;
		case '&': i = do_entity(p, i); break;
		case 'h':
		case 'w': i = do_bare(p, i); break;
		case '\n':
			if (i >= 2 && ' ' == s[i - 1] && ' ' == s[i - 2]) {
				(void)add_node(p, N_HARD);
			} else {
				(void)add_node(p, N_SOFT);
			}
			i++;
			break;
		default: /* cannot happen: special_at said so */
			text_node(p, N_TEXT, s + i, 1, 0);
			i++;
			break;
		}
	}
}

static WfStatus emit_text(WfMdUnits *out,
        const char *t,
        size_t len,
        uint16_t style,
        bool collapse,
        bool *last_space)
{
	size_t i = 0;

	while (i < len) {
		uint32_t cp;
		size_t k = wf_utf8_decode(t + i, len - i, &cp);
		WfStatus st = WF_OK;

		if (!k) {
			break;
		}
		i += k;
		if (' ' == cp || '\t' == cp) {
			if (!collapse || !*last_space) {
				st = wf_mdu_push(out, ' ', style, WF_MDU_SPACE);
			}
			*last_space = true;
		} else {
			st = wf_mdu_push(out, cp, style, 0);
			*last_space = false;
		}
		if (WF_OK != st) {
			return st;
		}
	}
	return WF_OK;
}

static WfStatus emit_units(P *p, uint16_t base, WfMdUnits *out)
{
	static const uint16_t FLAGS[4] = {
	        WF_MDS_BOLD, WF_MDS_ITALIC, WF_MDS_STRIKE, WF_MDS_LINK};
	int *d[4] = {NULL, NULL, NULL, NULL};
	int run[4] = {0, 0, 0, 0};
	bool last_space = true;
	WfStatus st = WF_OK;
	size_t i, f;

	for (f = 0; f < 4; f++) {
		d[f] = calloc(p->nn + 1, sizeof(int));
		if (!d[f]) {
			st = WF_E_NOMEM;
			goto done;
		}
	}
	for (i = 0; i < p->ns; i++) {
		d[p->sp[i].flag][p->sp[i].lo]++;
		d[p->sp[i].flag][p->sp[i].hi]--;
	}
	for (i = 0; i < p->nn && WF_OK == st; i++) {
		const Node *x = &p->nd[i];
		uint16_t style = base;

		for (f = 0; f < 4; f++) {
			run[f] += d[f][i];
			if (run[f] > 0) {
				style = (uint16_t)(style | FLAGS[f]);
			}
		}
		style = (uint16_t)(style | x->style);
		switch (x->type) {
		case N_TEXT:
			st = emit_text(out, p->arena.data + x->off, x->len,
			        style, true, &last_space);
			break;
		case N_CODE:
			st = emit_text(out, p->arena.data + x->off, x->len,
			        (uint16_t)(style | WF_MDS_CODE), false,
			        &last_space);
			last_space = false;
			break;
		case N_DELIM:
			for (f = 0; f < x->cnt && WF_OK == st; f++) {
				st = wf_mdu_push(out, x->ch, style, 0);
			}
			if (x->cnt) {
				last_space = false;
			}
			break;
		case N_OPEN:
			for (f = 0;
			        f < 1u + (x->image ? 1u : 0u) && WF_OK == st;
			        f++) {
				st = wf_mdu_push(out,
				        0 == f && x->image ? '!' : '[', style,
				        0);
			}
			last_space = false;
			break;
		case N_SOFT:
			if (!last_space) {
				st = wf_mdu_push(out, ' ', style, WF_MDU_SPACE);
			}
			last_space = true;
			break;
		case N_HARD:
			st = wf_mdu_push(out, 0, style, WF_MDU_HARD);
			last_space = true;
			break;
		default: break; /* N_DEAD */
		}
	}
done:
	for (f = 0; f < 4; f++) {
		free(d[f]);
	}
	return st;
}

WfStatus wf_md_inline(const char *s,
        size_t n,
        uint16_t base,
        WfMdRefFn ref,
        void *ref_ctx,
        WfMdUnits *out)
{
	P p;
	WfStatus st;
	size_t i;

	memset(&p, 0, sizeof p);
	p.s = s;
	p.n = n;
	p.dhead = p.dtail = -1;
	p.ref = ref;
	p.ref_ctx = ref_ctx;
	for (i = 0; i < sizeof p.code_fail / sizeof p.code_fail[0]; i++) {
		p.code_fail[i] = (size_t)-1;
	}
	wf_buf_init(&p.arena);
	tokenize(&p);
	process_emphasis(&p, -1);
	st = p.err;
	if (WF_OK == st) {
		st = emit_units(&p, base, out);
	}
	wf_buf_free(&p.arena);
	free(p.nd);
	free(p.sp);
	free(p.bk);
	return st;
}
