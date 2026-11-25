#include "wf_markdown.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../base/wf_buf.h"
#include "../base/wf_mem.h"
#include "wf_mdinline.h"
#include "wf_text.h"

/*
 * Blocks and layout (DR-DRAFT-025). The source is split into sanitized lines;
 * `blocks` walks an array of line views, recognising leaf blocks itself and
 * collecting the lines of a container (block quote, list item) into a new
 * array with the container's marker taken off, which it parses the same way
 * one level deeper. Nesting is bounded (WF_MD_NEST_MAX), so a hostile input
 * costs a few passes, not a stack. Text is laid out into WfMdDoc lines as it
 * is found; nothing is kept between blocks except link definitions.
 */

/* A line of source, or of a container's content. */
typedef struct
{
	const char *p;
	size_t n;
	uint32_t src;
} V;

typedef struct
{
	uint32_t s, e;
} Rng;

typedef struct
{
	char label[64];
	const char *dest;
	size_t dn;
} Ref;

#define PFX 160

/* Where a container's lines start: `lead` for its first line, `cont` after. */
typedef struct
{
	char lead[PFX], cont[PFX];
	int lead_cells, cont_cells;
	bool pending; /* the next line is the container's first */
	bool used;    /* a line has been written with its lead */
	bool any;     /* a block has been written here */
	bool tight;   /* no blank rows between blocks (a tight list item) */
	bool quote;
	unsigned ldepth; /* lists around this place */
} Ctx;

typedef struct
{
	WfMdDoc *doc;
	size_t text_cap, runs_cap, lines_cap;
	int width;
	bool collect; /* first pass: only the link definitions are wanted */
	bool full;
	bool open; /* a line is being written */
	WfStatus err;
	Ref refs[WF_MD_REFS];
	size_t nref;
	size_t ref_bytes;
	WfMdUnits scratch;
	Rng *rg;
	size_t rg_cap;
} R;

static uint32_t u32(size_t v)
{
	return v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
}

static void fail(R *r, WfStatus st)
{
	if (WF_OK == r->err) {
		r->err = st;
	}
	r->full = true;
}

/* Room for `need` elements: the block itself when it is big enough, a larger
 * one when not, NULL when memory ran out (the old block is then untouched). */
static void *grow(void *p, size_t *cap, size_t need, size_t elem)
{
	size_t c = *cap ? *cap : 256;
	void *q;

	if (need <= *cap && p) {
		return p;
	}
	while (c < need) {
		c *= 2;
	}
	q = wf_realloc_array(p, c, elem);
	if (q) {
		*cap = c;
	}
	return q;
}

static bool line_begin(R *r, uint32_t src, uint16_t flags)
{
	WfMdDoc *d = r->doc;
	WfMdLine *l;
	void *tmp;

	r->open = false;
	if (r->collect || r->full) {
		return false;
	}
	if (d->nlines >= WF_MD_OUT_LINES || d->text_len >= WF_MD_OUT_BYTES) {
		d->truncated = true;
		r->full = true;
		return false;
	}
	tmp = grow(d->lines, &r->lines_cap, d->nlines + 1, sizeof *d->lines);
	if (!tmp) {
		fail(r, WF_E_NOMEM);
		return false;
	}
	d->lines = tmp;
	l = &d->lines[d->nlines];
	if (d->nlines && src < d->lines[d->nlines - 1].src) {
		src = d->lines[d->nlines - 1].src;
	}
	l->run = u32(d->nruns);
	l->nruns = 0;
	l->src = src;
	l->flags = flags;
	d->nlines++;
	r->open = true;
	return true;
}

static void line_put(R *r, const char *t, size_t n, uint16_t style)
{
	WfMdDoc *d = r->doc;
	WfMdLine *l;
	void *tmp;

	if (!r->open || !n) {
		return;
	}
	if (n > WF_MD_OUT_BYTES - d->text_len) {
		d->truncated = true;
		r->full = true;
		r->open = false;
		return;
	}
	l = &d->lines[d->nlines - 1];
	tmp = grow(d->text, &r->text_cap, d->text_len + n + 1, sizeof *d->text);
	if (!tmp) {
		fail(r, WF_E_NOMEM);
		r->open = false;
		return;
	}
	d->text = tmp;
	if (l->nruns && d->runs[d->nruns - 1].style == style) {
		memcpy(d->text + d->text_len, t, n);
		d->runs[d->nruns - 1].len += u32(n);
		d->text_len += n;
		return;
	}
	tmp = grow(d->runs, &r->runs_cap, d->nruns + 1, sizeof *d->runs);
	if (!tmp) {
		fail(r, WF_E_NOMEM);
		r->open = false;
		return;
	}
	d->runs = tmp;
	memcpy(d->text + d->text_len, t, n);
	d->runs[d->nruns].off = u32(d->text_len);
	d->runs[d->nruns].len = u32(n);
	d->runs[d->nruns].style = style;
	d->nruns++;
	l->nruns++;
	d->text_len += n;
}

static void put_spaces(R *r, size_t n, uint16_t style)
{
	static const char sp[33] = "                                ";

	while (n) {
		size_t k = n > 32 ? 32 : n;

		line_put(r, sp, k, style);
		n -= k;
	}
}

static void put_unit(R *r, const WfMdUnit *u)
{
	char b[4];
	size_t k = wf_utf8_encode(u->cp, b);

	line_put(r, b, k, u->style);
}

/* Starts a line of a container: the marker of its first line, or the indent
 * of the following ones. */
static bool ctx_line(R *r, Ctx *c, uint32_t src, uint16_t flags)
{
	const char *pfx = c->pending ? c->lead : c->cont;

	if (!line_begin(r, src, flags)) {
		return false;
	}
	if (c->pending) {
		c->pending = false;
		c->used = true;
	}
	line_put(r, pfx, strlen(pfx), WF_MDS_MARKER);
	return true;
}

static void emit_blank(R *r, Ctx *c, uint32_t src)
{
	char t[PFX];
	size_t n = strlen(c->cont);

	memcpy(t, c->cont, n + 1);
	while (n && ' ' == t[n - 1]) {
		t[--n] = '\0';
	}
	if (line_begin(r, src, 0)) {
		line_put(r, t, n, WF_MDS_MARKER);
	}
}

static void begin_block(R *r, Ctx *c, uint32_t src)
{
	if (c->any && !c->tight) {
		emit_blank(r, c, src);
	}
	c->any = true;
}

static int avail_of(const R *r, const Ctx *c)
{
	int p = c->lead_cells > c->cont_cells ? c->lead_cells : c->cont_cells;
	int a = r->width - p;

	return a < 1 ? 1 : a;
}

static void add_piece(char *dst, int *cells, const char *piece)
{
	size_t n = strlen(dst), k = strlen(piece);

	if (n + k >= PFX) {
		return;
	}
	memcpy(dst + n, piece, k + 1);
	*cells += wf_utf8_cells(piece, k);
}

/* The context inside a container whose first line begins with `lead_piece`
 * and whose others begin with `cont_piece`. */
static void ctx_child(const Ctx *par,
        Ctx *ch,
        const char *lead_piece,
        const char *cont_piece,
        int width)
{
	int cap =
	        width / 2 < WF_MD_PREFIX_CELLS ? width / 2 : WF_MD_PREFIX_CELLS;
	int piece = wf_utf8_cells(cont_piece, strlen(cont_piece));

	memset(ch, 0, sizeof *ch);
	snprintf(ch->lead, PFX, "%s", par->pending ? par->lead : par->cont);
	ch->lead_cells = par->pending ? par->lead_cells : par->cont_cells;
	snprintf(ch->cont, PFX, "%s", par->cont);
	ch->cont_cells = par->cont_cells;
	add_piece(ch->lead, &ch->lead_cells, lead_piece);
	if (par->cont_cells + piece <= cap) { /* deeper: the mark only */
		add_piece(ch->cont, &ch->cont_cells, cont_piece);
	}
	ch->pending = true;
	ch->quote = par->quote;
	ch->ldepth = par->ldepth;
}

static void ctx_done(Ctx *par, const Ctx *ch)
{
	if (ch->used) {
		par->pending = false;
		par->used = true;
	}
}

static bool in_set(const uint32_t *set, size_t n, uint32_t cp)
{
	for (size_t i = 0; i < n; i++) {
		if (set[i] == cp) {
			return true;
		}
	}
	return false;
}

/* Marks that must not begin a line, and those that must not end one (the
 * usual rules for Japanese and Chinese text). */
static bool no_start(uint32_t cp)
{
	static const uint32_t set[] = {0x3001, 0x3002, 0xFF0C, 0xFF0E, 0x30FB,
	        0xFF1A, 0xFF1B, 0xFF1F, 0xFF01, 0xFF09, 0x3015, 0xFF3D, 0xFF5D,
	        0x3009, 0x300B, 0x300D, 0x300F, 0x3011, 0x2019, 0x201D, 0x30FC,
	        0x3041, 0x3043, 0x3045, 0x3047, 0x3049, 0x3063, 0x3083, 0x3085,
	        0x3087, 0x308E, 0x30A1, 0x30A3, 0x30A5, 0x30A7, 0x30A9, 0x30C3,
	        0x30E3, 0x30E5, 0x30E7, 0x30EE, 0x309B, 0x309C, 0x3005};

	return in_set(set, sizeof set / sizeof set[0], cp);
}

static bool no_end(uint32_t cp)
{
	static const uint32_t set[] = {0xFF08, 0x3014, 0xFF3B, 0xFF5B, 0x3008,
	        0x300A, 0x300C, 0x300E, 0x3010, 0x2018, 0x201C};

	return in_set(set, sizeof set / sizeof set[0], cp);
}

static bool can_break(const WfMdUnit *a, const WfMdUnit *b)
{
	if (0 == b->w || (a->flags & WF_MDU_SPACE) ||
	        (b->flags & WF_MDU_SPACE)) {
		return false;
	}
	return (2 == a->w || 2 == b->w) && !no_start(b->cp) && !no_end(a->cp);
}

static bool rg_push(Rng **rg, size_t *cap, size_t *cnt, size_t s, size_t e)
{
	void *tmp = grow(*rg, cap, *cnt + 1, sizeof **rg);

	if (!tmp) {
		return false;
	}
	*rg = tmp;
	(*rg)[*cnt].s = u32(s);
	(*rg)[*cnt].e = u32(e);
	(*cnt)++;
	return true;
}

/* Greedy line breaking of units into rows of at most `avail` cells. A space
 * is where a row may break (and is not kept there); between wide characters
 * too, except where the rules above forbid. `breakable` false: cut only at
 * the width (code). A unit wider than the row still takes a row by itself. */
static bool wrap(const WfMdUnit *u,
        size_t n,
        int avail,
        bool breakable,
        Rng **rg,
        size_t *cap,
        size_t *cnt)
{
	size_t s = 0;

	*cnt = 0;
	while (s < n) {
		size_t i, brk = 0, end = 0, next = 0;
		int width = 0;
		bool done = false;

		while (breakable && s < n && (u[s].flags & WF_MDU_SPACE)) {
			s++;
		}
		if (s >= n) {
			break;
		}
		for (i = s; i < n; i++) {
			if (u[i].flags & WF_MDU_HARD) {
				end = i;
				next = i + 1;
				done = true;
				break;
			}
			if (breakable && (u[i].flags & WF_MDU_SPACE)) {
				if (width + 1 > avail) {
					end = i;
					next = i + 1;
					done = true;
					break;
				}
				width++;
				brk = i + 1;
				continue;
			}
			if (width + u[i].w > avail && i > s) {
				if (breakable && brk > s) {
					end = brk;
					next = brk;
				} else {
					end = i;
					next = i;
				}
				done = true;
				break;
			}
			width += u[i].w;
			if (breakable && i + 1 < n &&
			        can_break(&u[i], &u[i + 1])) {
				brk = i + 1;
			}
		}
		if (!done) {
			end = n;
			next = n;
		}
		while (end > s && (u[end - 1].flags & WF_MDU_SPACE)) {
			end--;
		}
		if (!rg_push(rg, cap, cnt, s, end)) {
			return false;
		}
		s = next;
	}
	return true;
}

static uint32_t src_at(uint32_t first, uint32_t last, size_t k, size_t count)
{
	if (count <= 1 || last <= first) {
		return first;
	}
	return first + (uint32_t)((uint64_t)(last - first) * k / (count - 1));
}

static void emit_range(R *r, const WfMdUnit *u, size_t s, size_t e)
{
	for (size_t k = s; k < e; k++) {
		put_unit(r, &u[k]);
	}
}

/* Writes units as rows of a container, wrapped to its width. */
static void emit_units(R *r,
        Ctx *c,
        const WfMdUnit *u,
        size_t n,
        bool breakable,
        uint32_t first,
        uint32_t last,
        uint16_t flags)
{
	size_t cnt = 0;

	if (r->collect || r->full) {
		return;
	}
	if (!wrap(u, n, avail_of(r, c), breakable, &r->rg, &r->rg_cap, &cnt)) {
		fail(r, WF_E_NOMEM);
		return;
	}
	for (size_t k = 0; k < cnt && !r->full; k++) {
		if (ctx_line(r, c, src_at(first, last, k, cnt), flags)) {
			emit_range(r, u, r->rg[k].s, r->rg[k].e);
		}
	}
}

static bool ref_lookup(
        void *ctx, const char *label, size_t n, const char **dest, size_t *dn)
{
	R *r = ctx;
	char key[64];

	(void)wf_md_ref_normalize(label, n, key, sizeof key);
	for (size_t i = 0; i < r->nref; i++) {
		if (!strcmp(r->refs[i].label, key)) {
			if (r->refs[i].dn > WF_MD_REF_BYTES - r->ref_bytes) {
				r->doc->truncated = true;
				return false;
			}
			r->ref_bytes += r->refs[i].dn;
			*dest = r->refs[i].dest;
			*dn = r->refs[i].dn;
			return true;
		}
	}
	return false;
}

/* "[label]: address" and nothing else on the line (a title in quotes may
 * follow). Registers it in the first pass. */
static bool ref_def(R *r, const V *v)
{
	size_t i = 0, ls, le, ds, de;

	while (i < v->n && ' ' == v->p[i]) {
		i++;
	}
	if (i > 3 || i >= v->n || '[' != v->p[i]) {
		return false;
	}
	ls = ++i;
	while (i < v->n && ']' != v->p[i] && '[' != v->p[i]) {
		if ('\\' == v->p[i]) {
			i++;
		}
		i++;
	}
	if (i >= v->n || ']' != v->p[i] || i == ls || i + 1 >= v->n ||
	        ':' != v->p[i + 1]) {
		return false;
	}
	le = i;
	i += 2;
	while (i < v->n && ' ' == v->p[i]) {
		i++;
	}
	if (i < v->n && '<' == v->p[i]) {
		ds = ++i;
		while (i < v->n && '>' != v->p[i]) {
			i++;
		}
		if (i >= v->n) {
			return false;
		}
		de = i++;
	} else {
		ds = i;
		while (i < v->n && ' ' != v->p[i]) {
			i++;
		}
		de = i;
	}
	if (de == ds) {
		return false;
	}
	while (i < v->n && ' ' == v->p[i]) {
		i++;
	}
	if (i < v->n) {
		char q = v->p[i];

		if (!(('"' == q || '\'' == q || '(' == q) && i > de &&
		            v->n >= i + 2 &&
		            (')' == v->p[v->n - 1] || q == v->p[v->n - 1]))) {
			return false;
		}
	}
	if (r->collect && r->nref < WF_MD_REFS) {
		Ref *f = &r->refs[r->nref];
		size_t k;

		k = wf_md_ref_normalize(
		        v->p + ls, le - ls, f->label, sizeof f->label);
		if (k) {
			int dup = 0;

			for (size_t t = 0; t < r->nref; t++) {
				dup |= !strcmp(r->refs[t].label, f->label);
			}
			if (!dup) { /* the first definition wins */
				f->dest = v->p + ds;
				f->dn = de - ds;
				r->nref++;
			}
		}
	}
	return true;
}

static size_t lead_spaces(const V *v)
{
	size_t k = 0;

	while (k < v->n && ' ' == v->p[k]) {
		k++;
	}
	return k;
}

static bool is_blank(const V *v)
{
	return lead_spaces(v) == v->n;
}

static bool is_hr(const V *v)
{
	size_t ind = lead_spaces(v), i, count = 0;
	char ch;

	if (ind > 3 || ind >= v->n) {
		return false;
	}
	ch = v->p[ind];
	if ('-' != ch && '*' != ch && '_' != ch) {
		return false;
	}
	for (i = ind; i < v->n; i++) {
		if (v->p[i] == ch) {
			count++;
		} else if (' ' != v->p[i]) {
			return false;
		}
	}
	return count >= 3;
}

static bool is_atx(const V *v, unsigned *level, size_t *cs, size_t *ce)
{
	size_t ind = lead_spaces(v), i = ind, e, k;

	if (ind > 3) {
		return false;
	}
	while (i < v->n && '#' == v->p[i]) {
		i++;
	}
	if (i == ind || i - ind > 6 || (i < v->n && ' ' != v->p[i])) {
		return false;
	}
	*level = (unsigned)(i - ind);
	while (i < v->n && ' ' == v->p[i]) {
		i++;
	}
	*cs = i;
	e = v->n;
	while (e > i && ' ' == v->p[e - 1]) {
		e--;
	}
	k = e;
	while (k > i && '#' == v->p[k - 1]) {
		k--;
	}
	if (k == i) {
		e = i;
	} else if (k < e && ' ' == v->p[k - 1]) {
		e = k;
		while (e > i && ' ' == v->p[e - 1]) {
			e--;
		}
	}
	*ce = e;
	return true;
}

static bool is_fence(const V *v, char *ch, size_t *len, size_t *ind)
{
	size_t i = lead_spaces(v), k;

	*ind = i;
	if (i > 3 || i >= v->n || ('`' != v->p[i] && '~' != v->p[i])) {
		return false;
	}
	*ch = v->p[i];
	k = i;
	while (k < v->n && v->p[k] == *ch) {
		k++;
	}
	if (k - i < 3) {
		return false;
	}
	if ('`' == *ch && memchr(v->p + k, '`', v->n - k)) {
		return false;
	}
	*len = k - i;
	return true;
}

static bool is_quote(const V *v)
{
	size_t ind = lead_spaces(v);

	return ind <= 3 && ind < v->n && '>' == v->p[ind];
}

typedef struct
{
	size_t ind, off; /* off: where the item's text starts */
	bool ordered, empty;
	char ch;
	uint32_t num;
	size_t mw;
} Mk;

static bool is_marker(const V *v, Mk *m)
{
	size_t ind = lead_spaces(v), i, after, sp;

	if (ind > 3 || ind >= v->n) {
		return false;
	}
	memset(m, 0, sizeof *m);
	m->ind = ind;
	i = ind;
	if ('-' == v->p[i] || '+' == v->p[i] || '*' == v->p[i]) {
		m->ch = v->p[i];
		m->mw = 1;
	} else if (v->p[i] >= '0' && v->p[i] <= '9') {
		uint32_t num = 0;

		while (i < v->n && v->p[i] >= '0' && v->p[i] <= '9' &&
		        i - ind < 9) {
			num = num * 10 + (uint32_t)(v->p[i] - '0');
			i++;
		}
		if (i >= v->n || ('.' != v->p[i] && ')' != v->p[i]) ||
		        (i < v->n && v->p[ind] >= '0' && v->p[ind] <= '9' &&
		                i - ind >= 9 && v->p[i] >= '0' &&
		                v->p[i] <= '9')) {
			return false;
		}
		m->ordered = true;
		m->ch = v->p[i];
		m->num = num;
		m->mw = i - ind + 1;
	} else {
		return false;
	}
	after = ind + m->mw;
	if (after < v->n && ' ' != v->p[after]) {
		return false;
	}
	sp = 0;
	while (after + sp < v->n && ' ' == v->p[after + sp]) {
		sp++;
	}
	if (after + sp >= v->n) {
		m->empty = true;
		m->off = after + 1;
	} else {
		m->off = sp >= 5 ? after + 1 : after + sp;
	}
	return true;
}

static bool is_setext(const V *v, unsigned *level)
{
	size_t ind = lead_spaces(v), i = ind;
	char ch;

	if (ind > 3 || ind >= v->n || ('=' != v->p[ind] && '-' != v->p[ind])) {
		return false;
	}
	ch = v->p[ind];
	while (i < v->n && v->p[i] == ch) {
		i++;
	}
	while (i < v->n && ' ' == v->p[i]) {
		i++;
	}
	if (i != v->n) {
		return false;
	}
	*level = '=' == ch ? 1 : 2;
	return true;
}

/* Whether this line ends a paragraph that is running. */
static bool interrupts(const V *v)
{
	unsigned lv;
	size_t cs, ce, len, ind;
	char ch;
	Mk m;

	return is_atx(v, &lv, &cs, &ce) || is_fence(v, &ch, &len, &ind) ||
	        is_hr(v) || is_quote(v) ||
	        (is_marker(v, &m) && !m.empty && (!m.ordered || 1 == m.num));
}

typedef struct
{
	size_t s, e;
} Span;

/* The cells of a table row: split at '|' that is not escaped, the outer
 * pipes optional, each cell trimmed. Returns the count (more than `cap` are
 * counted but not stored). */
static size_t split_row(const V *v, Span *out, size_t cap)
{
	size_t s = 0, e = v->n, i, count = 0, from;

	while (s < e && ' ' == v->p[s]) {
		s++;
	}
	while (e > s && ' ' == v->p[e - 1]) {
		e--;
	}
	if (s < e && '|' == v->p[s]) {
		s++;
	}
	if (e > s && '|' == v->p[e - 1]) {
		size_t bs = 0, k = e - 1;

		while (k > s && '\\' == v->p[k - 1]) {
			bs++;
			k--;
		}
		if (!(bs & 1u)) {
			e--;
		}
	}
	from = s;
	for (i = s; i <= e; i++) {
		if (i == e || '|' == v->p[i]) {
			size_t bs = 0, k = i;

			if (i < e) {
				while (k > from && '\\' == v->p[k - 1]) {
					bs++;
					k--;
				}
				if (bs & 1u) {
					continue; /* an escaped pipe belongs to the cell */
				}
			}
			if (count < cap) {
				size_t a = from, b = i;

				while (a < b && ' ' == v->p[a]) {
					a++;
				}
				while (b > a && ' ' == v->p[b - 1]) {
					b--;
				}
				out[count].s = a;
				out[count].e = b;
			}
			count++;
			from = i + 1;
		}
	}
	return count;
}

static bool table_head(const V *h, const V *d, size_t *ncols, uint8_t *align)
{
	Span hc[WF_MD_TABLE_COLS + 1], dc[WF_MD_TABLE_COLS + 1];
	size_t nh, nd, i;

	if (!memchr(d->p, '|', d->n) || !memchr(h->p, '|', h->n) ||
	        lead_spaces(d) > 3) {
		return false;
	}
	nd = split_row(d, dc, WF_MD_TABLE_COLS + 1);
	nh = split_row(h, hc, WF_MD_TABLE_COLS + 1);
	if (!nd || nd != nh || nd > WF_MD_TABLE_COLS) {
		return false;
	}
	for (i = 0; i < nd; i++) {
		size_t a = dc[i].s, b = dc[i].e;
		bool left = a < b && ':' == d->p[a], right;

		if (left) {
			a++;
		}
		right = b > a && ':' == d->p[b - 1];
		if (right) {
			b--;
		}
		if (a >= b) {
			return false;
		}
		for (size_t k = a; k < b; k++) {
			if ('-' != d->p[k]) {
				return false;
			}
		}
		align[i] = left && right ? 2 : right ? 3 : left ? 1 : 0;
	}
	*ncols = nd;
	return true;
}

typedef struct
{
	WfMdUnits u;
	int natural;
} Cell;

/* The cell of row i, column j. Only the cells a row has are kept (row i owns
 * cells[first[i]] up to cells[first[i + 1]]); the others read as the empty cell
 * that a zeroed one is. */
static const Cell *table_cell(
        const Cell *cells, const size_t *first, size_t i, size_t j)
{
	static const Cell none = {{NULL, 0, 0}, 0};

	return j < first[i + 1] - first[i] ? &cells[first[i] + j] : &none;
}

static int units_cells(const WfMdUnit *u, size_t s, size_t e)
{
	int w = 0;

	for (size_t k = s; k < e; k++) {
		w += u[k].w;
	}
	return w;
}

static void table_emit(R *r,
        Ctx *c,
        const V *rows,
        size_t nrows,
        size_t ncols,
        const uint8_t *align)
{
	Cell *cells = NULL;
	size_t *first; /* nrows + 1 entries: where each row's cells start */
	size_t used = 0, ccap = 0;
	int w[WF_MD_TABLE_COLS];
	size_t i, j;
	int room, overhead, sum;
	bool plain = false;
	Rng *rg[WF_MD_TABLE_COLS];
	size_t rgcap[WF_MD_TABLE_COLS], rgn[WF_MD_TABLE_COLS];

	if (r->collect || r->full) {
		return;
	}
	first = calloc(nrows + 1, sizeof *first);
	if (!first) {
		fail(r, WF_E_NOMEM);
		return;
	}
	for (j = 0; j < ncols; j++) {
		rg[j] = NULL;
		rgcap[j] = rgn[j] = 0;
		w[j] = 3;
	}
	for (i = 0; i < nrows && WF_OK == r->err; i++) {
		Span sp[WF_MD_TABLE_COLS + 1];
		size_t got = split_row(&rows[i], sp, WF_MD_TABLE_COLS + 1);
		size_t have = got < ncols ? got : ncols;

		first[i] = used;
		for (j = 0; j < have && WF_OK == r->err; j++) {
			Cell *ce;
			WfStatus st;

			if (used == ccap) {
				size_t cap = ccap ? ccap * 2 : 64;
				Cell *grown = wf_realloc_array(
				        cells, cap, sizeof *cells);

				if (!grown) {
					fail(r, WF_E_NOMEM);
					break;
				}
				cells = grown;
				ccap = cap;
			}
			ce = &cells[used++]; /* counted before it is filled: it is freed below */
			memset(ce, 0, sizeof *ce);
			st = wf_md_inline(rows[i].p + sp[j].s,
			        sp[j].e - sp[j].s,
			        (uint16_t)((0 == i ? WF_MDS_BOLD : 0) |
			                (c->quote ? WF_MDS_QUOTE : 0)),
			        ref_lookup, r, &ce->u);
			if (WF_OK != st) {
				fail(r, st);
				break;
			}
			wf_mdu_trim(
			        &ce->u); /* the table keeps every cell to the end */
			ce->natural = units_cells(ce->u.u, 0, ce->u.n);
			if (ce->natural > w[j]) {
				w[j] = ce->natural;
			}
		}
		first[i + 1] = used;
	}
	overhead = (int)(3 * ncols + 1);
	room = avail_of(r, c) - overhead;
	sum = 0;
	for (j = 0; j < ncols; j++) {
		sum += w[j];
	}
	if (room < (int)(3 * ncols)) {
		plain = true; /* not even the narrowest columns fit */
	} else {
		while (sum > room) {
			size_t widest = 0;

			for (j = 1; j < ncols; j++) {
				if (w[j] > w[widest]) {
					widest = j;
				}
			}
			if (w[widest] <= 3) {
				break;
			}
			/* take a good part of the excess off the widest at once */
			int cut = (sum - room) / 2;

			cut = cut < 1 ? 1 : cut;
			cut = cut > w[widest] - 3 ? w[widest] - 3 : cut;
			w[widest] -= cut;
			sum -= cut;
		}
	}
	for (i = 0; i < nrows && WF_OK == r->err && !r->full; i++) {
		uint32_t src = rows[i].src;
		size_t height = 1;

		if (plain) {
			WfMdUnits row = {0};

			for (j = 0; j < ncols; j++) {
				const Cell *ce = table_cell(cells, first, i, j);

				if (j &&
				        (WF_OK !=
				                        wf_mdu_push(&row, '|',
				                                0, 0) ||
				                WF_OK !=
				                        wf_mdu_push(&row, ' ',
				                                0,
				                                WF_MDU_SPACE))) {
					fail(r, WF_E_NOMEM);
				}
				for (size_t k = 0; k < ce->u.n; k++) {
					if (WF_OK !=
					        wf_mdu_push(&row, ce->u.u[k].cp,
					                ce->u.u[k].style,
					                ce->u.u[k].flags)) {
						fail(r, WF_E_NOMEM);
					}
				}
				if (j + 1 < ncols &&
				        WF_OK !=
				                wf_mdu_push(&row, ' ', 0,
				                        WF_MDU_SPACE)) {
					fail(r, WF_E_NOMEM);
				}
			}
			emit_units(r, c, row.u, row.n, true, src, src, 0);
			wf_mdu_free(&row);
			continue;
		}
		for (j = 0; j < ncols; j++) {
			const Cell *ce = table_cell(cells, first, i, j);

			if (!wrap(ce->u.u, ce->u.n, w[j], true, &rg[j],
			            &rgcap[j], &rgn[j])) {
				fail(r, WF_E_NOMEM);
				break;
			}
			if (rgn[j] > height) {
				height = rgn[j];
			}
		}
		for (size_t k = 0; k < height && WF_OK == r->err; k++) {
			if (!ctx_line(r, c, src, 0)) {
				break;
			}
			line_put(r, "| ", 2, WF_MDS_MARKER);
			for (j = 0; j < ncols; j++) {
				const Cell *ce = table_cell(cells, first, i, j);
				size_t s = 0, e = 0;
				int cw, pad, left;

				if (k < rgn[j]) {
					s = rg[j][k].s;
					e = rg[j][k].e;
				}
				cw = units_cells(ce->u.u, s, e);
				pad = w[j] - cw;
				pad = pad < 0 ? 0 : pad;
				left = 2 == align[j]    ? pad / 2
				        : 3 == align[j] ? pad
				                        : 0;
				put_spaces(r, (size_t)left, 0);
				emit_range(r, ce->u.u, s, e);
				put_spaces(r, (size_t)(pad - left), 0);
				line_put(r, j + 1 < ncols ? " | " : " |",
				        j + 1 < ncols ? 3 : 2, WF_MDS_MARKER);
			}
		}
		if (0 == i && !r->full && ctx_line(r, c, src, 0)) {
			line_put(r, "|", 1, WF_MDS_MARKER);
			for (j = 0; j < ncols; j++) {
				size_t dashes = (size_t)w[j] + 2;

				if (1 == align[j] || 2 == align[j]) {
					line_put(r, ":", 1, WF_MDS_MARKER);
					dashes--;
				}
				if (2 == align[j] || 3 == align[j]) {
					dashes--;
				}
				for (size_t k = 0; k < dashes; k++) {
					line_put(r, "-", 1, WF_MDS_MARKER);
				}
				if (2 == align[j] || 3 == align[j]) {
					line_put(r, ":", 1, WF_MDS_MARKER);
				}
				line_put(r, "|", 1, WF_MDS_MARKER);
			}
		}
	}
	for (j = 0; j < ncols; j++) {
		free(rg[j]);
	}
	for (i = 0; i < used; i++) {
		wf_mdu_free(&cells[i].u);
	}
	free(cells);
	free(first);
}

static void blocks(R *r, Ctx *c, V *L, size_t n, unsigned depth);

static uint16_t qbase(const Ctx *c)
{
	return c->quote ? WF_MDS_QUOTE : 0;
}

static void emit_text_block(R *r,
        Ctx *c,
        const char *t,
        size_t tn,
        uint16_t base,
        uint32_t first,
        uint32_t last)
{
	WfMdUnits *u = &r->scratch;
	WfStatus st;

	if (r->collect || r->full) {
		return;
	}
	u->n = 0;
	st = wf_md_inline(t, tn, base, ref_lookup, r, u);
	if (WF_OK != st) {
		fail(r, st);
		return;
	}
	emit_units(r, c, u->u, u->n, true, first, last, 0);
}

static void code_lines(R *r, Ctx *c, const V *L, size_t n, size_t strip)
{
	WfMdUnits *u = &r->scratch;

	if (r->collect) {
		return;
	}
	for (size_t i = 0; i < n && !r->full; i++) {
		size_t k = 0;
		const char *p = L[i].p;
		size_t pn = L[i].n;
		WfStatus st;

		while (k < strip && k < pn && ' ' == p[k]) {
			k++;
		}
		u->n = 0;
		st = wf_mdu_push_text(u, p + k, pn - k, WF_MDS_BLOCK);
		if (WF_OK != st) {
			fail(r, st);
			return;
		}
		if (!u->n) {
			if (ctx_line(r, c, L[i].src, WF_MDL_CODE)) {
				line_put(r, "", 0, WF_MDS_BLOCK);
			}
			continue;
		}
		emit_units(r, c, u->u, u->n, false, L[i].src, L[i].src,
		        WF_MDL_CODE);
	}
}

static void do_quote(R *r, Ctx *c, V *L, size_t n, size_t *pi, unsigned depth)
{
	size_t i = *pi, cnt = 0;
	V *q = malloc((n - i) * sizeof *q);
	bool para = false;
	Ctx ch;

	if (!q) {
		fail(r, WF_E_NOMEM);
		*pi = n;
		return;
	}
	while (i < n) {
		V v = L[i];

		if (is_quote(&v)) {
			size_t ind = lead_spaces(&v);

			v.p += ind + 1;
			v.n -= ind + 1;
			if (v.n && ' ' == v.p[0]) {
				v.p++;
				v.n--;
			}
			q[cnt++] = v;
			para = !is_blank(&v) && lead_spaces(&v) < 4 &&
			        !interrupts(&v);
		} else if (para && !is_blank(&L[i]) && !interrupts(&L[i])) {
			q[cnt++] =
			        L[i]; /* a lazy continuation of the paragraph */
		} else {
			break;
		}
		i++;
	}
	begin_block(r, c, L[*pi].src);
	ctx_child(c, &ch, "| ", "| ", r->width);
	ch.quote = true;
	blocks(r, &ch, q, cnt, depth + 1);
	ctx_done(c, &ch);
	free(q);
	*pi = i;
}

typedef struct
{
	size_t start, end;
	Mk mk;
	bool inner_blank;
} Item;

static bool same_list(const Mk *a, const Mk *b)
{
	return a->ordered == b->ordered && a->ch == b->ch;
}

static void do_list(R *r, Ctx *c, V *L, size_t n, size_t *pi, unsigned depth)
{
	size_t i = *pi, ni = 0, icap = 0, t;
	Item *items = NULL;
	Mk first;
	bool loose = false;
	size_t stop;

	(void)is_marker(&L[i], &first);
	for (;;) {
		Mk mk;
		size_t j, k, W;
		bool para, inner = false;
		const V *v = &L[i];

		(void)is_marker(v, &mk);
		W = mk.off;
		void *grown = grow(items, &icap, ni + 1, sizeof *items);

		if (!grown) {
			fail(r, WF_E_NOMEM);
			free(items);
			*pi = n;
			return;
		}
		items = grown;
		para = !mk.empty && v->n > W &&
		        !is_blank(&(V){v->p + W, v->n - W, 0});
		j = i + 1;
		while (j < n) {
			if (is_blank(&L[j])) {
				k = j;
				while (k < n && is_blank(&L[k])) {
					k++;
				}
				if (k < n && lead_spaces(&L[k]) >= W) {
					inner = true;
					para = false;
					j = k;
					continue;
				}
				break;
			}
			if (lead_spaces(&L[j]) >= W) {
				V s = {L[j].p + W, L[j].n - W, 0};

				para = !is_blank(&s) && lead_spaces(&s) < 4 &&
				        !interrupts(&s);
				j++;
				continue;
			}
			{
				Mk other;

				if (para && !interrupts(&L[j]) &&
				        !is_marker(&L[j], &other)) {
					j++; /* a lazy continuation */
					continue;
				}
			}
			break;
		}
		items[ni].start = i;
		items[ni].end = j;
		items[ni].mk = mk;
		items[ni].inner_blank = inner;
		ni++;
		loose = loose || inner;
		k = j;
		while (k < n && is_blank(&L[k])) {
			k++;
		}
		{
			Mk next;

			if (k < n && is_marker(&L[k], &next) &&
			        same_list(&first, &next) && !is_hr(&L[k])) {
				if (k > j) {
					loose = true;
				}
				i = k;
				continue;
			}
		}
		break;
	}
	stop = items[ni - 1].end;
	begin_block(r, c, L[*pi].src);
	for (t = 0; t < ni && !r->full; t++) {
		const Item *it = &items[t];
		size_t W = it->mk.off, cnt = it->end - it->start, k;
		V *I = malloc(cnt * sizeof *I);
		char lead[40], pad[40];
		bool task = false, checked = false;
		Ctx ch;

		if (!I) {
			fail(r, WF_E_NOMEM);
			break;
		}
		for (k = 0; k < cnt; k++) {
			const V *s = &L[it->start + k];

			if (0 == k) {
				size_t off = W < s->n ? W : s->n;

				I[k] = (V){s->p + off, s->n - off, s->src};
			} else if (is_blank(s)) {
				I[k] = (V){s->p + s->n, 0, s->src};
			} else {
				size_t ind = lead_spaces(s);
				size_t off = ind < W ? ind : W;

				I[k] = (V){s->p + off, s->n - off, s->src};
			}
		}
		if (I[0].n >= 3 && '[' == I[0].p[0] && ']' == I[0].p[2] &&
		        (' ' == I[0].p[1] || 'x' == I[0].p[1] ||
		                'X' == I[0].p[1]) &&
		        (3 == I[0].n || ' ' == I[0].p[3])) {
			task = true;
			checked = ' ' != I[0].p[1];
			I[0].p += 3 < I[0].n ? 4 : 3;
			I[0].n -= 3 < I[0].n ? 4 : 3;
		}
		if (it->mk.ordered) {
			snprintf(lead, sizeof lead, "%u%c ",
			        (unsigned)(first.num + (uint32_t)t), first.ch);
		} else {
			snprintf(lead, sizeof lead, "%c ",
			        0 == c->ldepth % 3           ? '-'
			                : 1 == c->ldepth % 3 ? '*'
			                                     : '+');
		}
		if (task) {
			snprintf(lead, sizeof lead, "%s ",
			        checked ? "[x]" : "[ ]");
		}
		memset(pad, ' ', sizeof pad);
		pad[strlen(lead)] = '\0';
		if (t > 0 && loose) {
			emit_blank(r, c, L[it->start].src);
		}
		ctx_child(c, &ch, lead, pad, r->width);
		ch.ldepth = c->ldepth + 1;
		ch.tight = !loose;
		blocks(r, &ch, I, cnt, depth + 1);
		if (!ch.used &&
		        !r->collect) { /* an empty item still has its mark */
			if (ctx_line(r, &ch, L[it->start].src, 0)) {
				line_put(r, "", 0, 0);
			}
		}
		ctx_done(c, &ch);
		free(I);
	}
	*pi = stop;
	free(items);
}

static void blocks(R *r, Ctx *c, V *L, size_t n, unsigned depth)
{
	size_t i = 0;
	bool flat = depth >= WF_MD_NEST_MAX;

	while (i < n && !r->full) {
		const V *v = &L[i];
		size_t ind, len, fi, cs, ce;
		unsigned level;
		char fch;
		Mk mk;

		if (is_blank(v)) {
			i++;
			continue;
		}
		ind = lead_spaces(v);
		if (ind >= 4) { /* indented code */
			size_t j = i, last = i;

			while (j < n) {
				if (is_blank(&L[j])) {
					j++;
					continue;
				}
				if (lead_spaces(&L[j]) >= 4) {
					j++;
					last = j;
					continue;
				}
				break;
			}
			if (!r->collect) {
				begin_block(r, c, v->src);
			}
			code_lines(r, c, L + i, last - i, 4);
			i = last;
			continue;
		}
		if (is_fence(v, &fch, &len, &fi)) {
			size_t j = i + 1;

			while (j < n) {
				V t = L[j];
				char ch2;
				size_t l2, i2, k;

				if (is_fence(&t, &ch2, &l2, &i2) &&
				        ch2 == fch && l2 >= len) {
					k = i2 + l2;
					while (k < t.n && ' ' == t.p[k]) {
						k++;
					}
					if (k == t.n) {
						break;
					}
				}
				j++;
			}
			if (!r->collect) {
				begin_block(r, c, v->src);
			}
			code_lines(r, c, L + i + 1, j - (i + 1), fi);
			i = j < n ? j + 1 : n;
			continue;
		}
		if (is_atx(v, &level, &cs, &ce)) {
			if (!r->collect) {
				begin_block(r, c, v->src);
				emit_text_block(r, c, v->p + cs, ce - cs,
				        (uint16_t)(qbase(c) |
				                (level << WF_MDS_HEAD_SHIFT)),
				        v->src, v->src);
			}
			i++;
			continue;
		}
		if (is_hr(v)) {
			if (!r->collect) {
				int w = avail_of(r, c);

				begin_block(r, c, v->src);
				if (ctx_line(r, c, v->src, 0)) {
					for (int k = 0; k < (w < 200 ? w : 200);
					        k++) {
						line_put(r, "-", 1,
						        WF_MDS_MARKER);
					}
				}
			}
			i++;
			continue;
		}
		if (!flat && is_quote(v)) {
			do_quote(r, c, L, n, &i, depth);
			continue;
		}
		if (!flat && is_marker(v, &mk)) {
			do_list(r, c, L, n, &i, depth);
			continue;
		}
		{ /* a paragraph, a setext heading or a table */
			size_t j = i + 1, k;
			unsigned sl = 0;
			size_t ncols = 0;
			uint8_t align[WF_MD_TABLE_COLS];
			size_t end;
			WfBuf text;
			size_t first = i;

			/* link definitions at the start are not text */
			while (first < n && ref_def(r, &L[first])) {
				first++;
			}
			if (first > i) {
				i = first;
				continue;
			}
			if (i + 1 < n &&
			        table_head(v, &L[i + 1], &ncols, align)) {
				size_t rows_end = i + 2;

				while (rows_end < n &&
				        !is_blank(&L[rows_end]) &&
				        !interrupts(&L[rows_end])) {
					rows_end++;
				}
				if (!r->collect) {
					size_t nr = 1 + (rows_end - (i + 2));
					V *rows = malloc(nr * sizeof *rows);

					if (!rows) {
						fail(r, WF_E_NOMEM);
						return;
					}
					rows[0] = *v;
					for (k = 0; k < nr - 1; k++) {
						rows[k + 1] = L[i + 2 + k];
					}
					begin_block(r, c, v->src);
					table_emit(
					        r, c, rows, nr, ncols, align);
					free(rows);
				}
				i = rows_end;
				continue;
			}
			end = n;
			for (; j < n; j++) {
				if (is_blank(&L[j])) {
					end = j;
					break;
				}
				if (is_setext(&L[j], &sl)) {
					end = j;
					break;
				}
				sl = 0;
				if (interrupts(&L[j])) {
					end = j;
					break;
				}
				if (j + 1 < n &&
				        table_head(&L[j], &L[j + 1], &ncols,
				                align)) {
					end = j;
					break;
				}
			}
			wf_buf_init(&text);
			for (k = i; k < end; k++) {
				size_t s = lead_spaces(&L[k]), e = L[k].n;
				WfStatus st = WF_OK;

				if (k + 1 == end || sl) {
					while (e > s && ' ' == L[k].p[e - 1]) {
						e--;
					}
				}
				if (k > i) {
					st = wf_buf_putc(
					        &text, sl ? ' ' : '\n');
				}
				if (WF_OK == st) {
					st = wf_buf_append(
					        &text, L[k].p + s, e - s);
				}
				if (WF_OK != st) {
					fail(r, st);
					wf_buf_free(&text);
					return;
				}
			}
			if (!r->collect) {
				begin_block(r, c, v->src);
				emit_text_block(r, c, wf_buf_cstr(&text),
				        text.len,
				        (uint16_t)(qbase(c) |
				                (sl ? (sl << WF_MDS_HEAD_SHIFT)
				                    : 0)),
				        v->src, L[end - 1].src);
			}
			wf_buf_free(&text);
			i = sl && end < n ? end + 1 : end;
		}
	}
}

void wf_md_free(WfMdDoc *d)
{
	if (!d) {
		return;
	}
	free(d->text);
	free(d->runs);
	free(d->lines);
	free(d);
}

size_t wf_md_line_for_source(const WfMdDoc *d, size_t src)
{
	size_t lo = 0, hi;

	if (!d || !d->nlines) {
		return 0;
	}
	hi = d->nlines;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;

		if (d->lines[mid].src < src) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return lo < d->nlines ? lo : d->nlines - 1;
}

size_t wf_md_line_text(const WfMdDoc *d, size_t i, char *out, size_t cap)
{
	size_t k = 0;

	if (!cap) {
		return 0;
	}
	out[0] = '\0';
	if (!d || i >= d->nlines) {
		return 0;
	}
	for (uint32_t r = 0; r < d->lines[i].nruns; r++) {
		const WfMdRun *run = &d->runs[d->lines[i].run + r];

		for (uint32_t b = 0; b < run->len && k + 1 < cap; b++) {
			out[k++] = d->text[run->off + b];
		}
	}
	out[k] = '\0';
	return k;
}

bool wf_md_name_is_markdown(const wchar_t *name)
{
	static const wchar_t *const ext[] = {L".md", L".markdown"};
	size_t n = name ? wcslen(name) : 0;

	for (size_t e = 0; e < 2; e++) {
		size_t k = wcslen(ext[e]);

		if (n > k) {
			size_t i;

			for (i = 0; i < k; i++) {
				wchar_t ch = name[n - k + i];

				if (ch >= L'A' && ch <= L'Z') {
					ch = (wchar_t)(ch + 32);
				}
				if (ch != ext[e][i]) {
					break;
				}
			}
			if (i == k) {
				return true;
			}
		}
	}
	return false;
}

typedef struct
{
	size_t off, len;
} LineAt;

static bool plain_line(const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];

		if (c < 0x20 || c >= 0x7F) {
			return false;
		}
	}
	return true;
}

static void init_ctx(Ctx *c)
{
	memset(c, 0, sizeof *c);
}

static void free_r(R *r)
{
	wf_mdu_free(&r->scratch);
	free(r->rg);
}

WfStatus wf_md_render(const char *src, size_t n, int width, WfMdDoc **out)
{
	WfMdDoc *doc;
	WfBuf arena;
	LineAt *at = NULL;
	size_t nl = 0, lcap = 0, i, start = 0;
	bool cut = false;
	V *L = NULL;
	R *r;
	Ctx ctx;
	WfStatus st = WF_OK;

	if (!out) {
		return WF_E_INVAL;
	}
	*out = NULL;
	if (!src && n) {
		return WF_E_INVAL;
	}
	width = width < 8 ? 8 : width > 400 ? 400 : width;
	if (n > WF_MD_SRC_MAX) {
		n = wf_utf8_prefix(src, n, WF_MD_SRC_MAX);
		cut = true;
	}
	doc = calloc(1, sizeof *doc);
	r = calloc(1, sizeof *r);
	if (!doc || !r) {
		free(doc);
		free(r);
		return WF_E_NOMEM;
	}
	doc->width = width;
	r->doc = doc;
	r->width = width;
	wf_buf_init(&arena);
	for (i = 0; i <= n; i++) {
		bool eol = i == n || '\n' == src[i] || '\r' == src[i];

		if (!eol) {
			continue;
		}
		if (i == n &&
		        start == n) { /* the text ended with a line break */
			break;
		}
		if (nl >= WF_MD_SRC_LINES) {
			cut = true;
			break;
		}
		void *grown = grow(at, &lcap, nl + 1, sizeof *at);

		if (!grown) {
			st = WF_E_NOMEM;
			goto done;
		}
		at = grown;
		at[nl].off = arena.len;
		if (plain_line(src + start, i - start)) {
			st = wf_buf_append(&arena, src + start, i - start);
		} else {
			st = wf_sanitize_utf8(src + start, i - start, &arena);
		}
		if (WF_OK != st) {
			goto done;
		}
		at[nl].len = arena.len - at[nl].off;
		nl++;
		if (i < n && '\r' == src[i] && i + 1 < n &&
		        '\n' == src[i + 1]) {
			i++;
		}
		start = i + 1;
	}
	L = malloc((nl ? nl : 1) * sizeof *L);
	if (!L) {
		st = WF_E_NOMEM;
		goto done;
	}
	for (i = 0; i < nl; i++) {
		L[i] = (V){arena.data ? arena.data + at[i].off : "", at[i].len,
		        (uint32_t)i};
	}
	if (nl && memchr(src, ']', n) && strstr(wf_buf_cstr(&arena), "]:")) {
		init_ctx(&ctx);
		r->collect = true;
		blocks(r, &ctx, L, nl, 0);
		r->collect = false;
		r->full = false;
	}
	init_ctx(&ctx);
	blocks(r, &ctx, L, nl, 0);
	st = r->err;
	if (cut) {
		doc->truncated = true;
	}
done:
	free_r(r);
	free(r);
	free(L);
	free(at);
	wf_buf_free(&arena);
	if (WF_OK != st) {
		wf_md_free(doc);
		return st;
	}
	*out = doc;
	return WF_OK;
}
