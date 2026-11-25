/*
 * The editor's side of Tree-sitter colouring (DR-DRAFT-033): which buffers a
 * grammar colours, when the text is handed to the syntax worker, and the answer
 * taken back in. The worker parses; this file only flattens a text, asks, and
 * keeps the runs. Colours are display data: nothing here touches the text, its
 * encoding, its line ends or a save.
 */
#include <stdlib.h>
#include <string.h>

#include "../syntax/wf_synlang.h"
#include "wf_esession_internal.h"

void wf_ts_free(WfTsState *ts)
{
	wf_synov_clear(&ts->ov);
	free(ts->flat);
	free(ts->off);
	memset(ts, 0, sizeof *ts);
}

static const wchar_t *leaf_of(const wchar_t *path)
{
	const wchar_t *p = path ? wcsrchr(path, L'\\') : NULL;

	return p ? p + 1 : path;
}

void wf_es_set_syntax(WfEditSession *s, WfSynPort port)
{
	s->syn_port = port;
}

void wf_es_syntax_assign(WfEditSession *s, WfBuffer *b, const wchar_t *path)
{
	WfTsState *ts = &b->syn.ts;
	const WfSynLang *l;

	wf_ts_free(ts);
	if (!s->cfg.syntax || !s->syn_port.request || !path) {
		return;
	}
	l = wf_synlang_for_name(leaf_of(path));
	if (!l || l->companion) {
		return;
	}
	if (s->syn_port.installed &&
	        !s->syn_port.installed(s->syn_port.ctx, l->id)) {
		return; /* no grammar: the hand lexer, as ever */
	}
	strcpy_s(ts->lang, sizeof ts->lang, l->id);
	ts->state = WF_TS_PENDING;
}

/* The buffer's text as the parser reads it: its lines, each followed by '\n'
 * (whatever line ends the file has); the line starts too. False when it does not
 * fit in memory or in the limit. */
static bool flatten(const WfEText *t, WfTsState *ts)
{
	size_t total = 0, at = 0;
	char *buf;
	size_t *off;

	for (size_t i = 0; i < t->n; i++) {
		total += t->lines[i].len + 1;
		if (total > WF_SYN_DEFAULT_MAX_TEXT) {
			return false;
		}
	}
	buf = wf_et_realloc(ts->flat, total ? total : 1u);
	if (!buf) {
		return false;
	}
	/* the buffer may have shrunk or moved: what was in it, and its length, are
	 * gone together, so that no answer for the old text is read from it */
	ts->flat = buf;
	ts->flat_len = 0;
	off = wf_et_realloc(ts->off, (t->n ? t->n : 1u) * sizeof *off);
	if (!off) {
		return false;
	}
	ts->off = off;
	for (size_t i = 0; i < t->n; i++) {
		ts->off[i] = at;
		if (t->lines[i].len) {
			memcpy(buf + at, t->lines[i].s, t->lines[i].len);
		}
		at += t->lines[i].len;
		buf[at++] = '\n';
	}
	ts->flat_len = total;
	ts->off_n = t->n;
	ts->flat_gen = ts->off_gen = t->gen;
	return true;
}

void wf_es_syntax_sync(WfEditSession *s, WfBuffer *b)
{
	WfTsState *ts = &b->syn.ts;
	const WfEText *t = &b->ed.text;
	uint32_t gen;

	if (WF_TS_OFF == ts->state || !s->cfg.syntax || !s->syn_port.request) {
		return;
	}
	if (ts->flat && ts->flat_gen == t->gen && s->syn_buf == b) {
		return; /* nothing changed since the last look: no work at all */
	}
	if (!flatten(t, ts)) {
		wf_synov_clear(&ts->ov);
		ts->state = WF_TS_FAILED; /* too large: plain text, as before */
		ts->flat_gen = (uint64_t)-1;
		s->syn_req_gen =
		        0; /* the answer on its way is for text that is gone */
		return;
	}
	if (WF_TS_READY == ts->state) {
		wf_synov_retarget(&ts->ov, ts->flat, ts->flat_len);
	}
	gen = ++s->syn_gen;
	if (!gen) {
		gen = ++s->syn_gen;
	}
	if (WF_OK ==
	        s->syn_port.request(s->syn_port.ctx, WF_SYN_SLOT_EDITOR, gen,
	                ts->lang, ts->flat, ts->flat_len)) {
		s->syn_req_gen = gen;
		s->syn_buf = b;
	}
}

void wf_es_on_syntax(WfEditSession *s, WfSynAnswer *a)
{
	WfBuffer *b = s->syn_buf;
	WfTsState *ts;
	bool known = false;

	if (!a || WF_SYN_SLOT_EDITOR != a->slot || a->gen != s->syn_req_gen ||
	        !b) {
		return;
	}
	for (size_t i = 0; i < s->n; i++) {
		known |= s->buf[i] == b;
	}
	if (!known) {
		return;
	}
	ts = &b->syn.ts;
	ts->status = a->status;
	ts->parse_us = a->parse_us;
	ts->incremental = a->incremental;
	if (WF_SYN_OK == a->status && ts->flat && a->text_len == ts->flat_len) {
		char *base = wf_et_malloc(ts->flat_len ? ts->flat_len : 1u);

		if (base) {
			memcpy(base, ts->flat, ts->flat_len);
			bool kept = wf_synov_set(&ts->ov, a->runs, a->nruns,
			        base, ts->flat_len, ts->flat_len);

			a->runs = NULL;
			a->nruns = 0;
			ts->state = kept ? WF_TS_READY : WF_TS_FAILED;
			return;
		}
	}
	wf_synov_clear(&ts->ov);
	ts->state = WF_TS_FAILED;
}

void wf_es_syntax_close(WfEditSession *s)
{
	if (s->syn_port.close) {
		s->syn_port.close(s->syn_port.ctx, WF_SYN_SLOT_EDITOR);
	}
	s->syn_req_gen = 0;
	s->syn_buf = NULL;
}

/* The classes of line `line` of a buffer a grammar colours: true with classes
 * (plain until the first answer, plain for a text over the limit). */
bool wf_ts_line(const WfSyntax *syn,
        const WfEText *t,
        size_t line,
        uint8_t *out,
        size_t cap)
{
	const WfTsState *ts = &syn->ts;
	const WfELine *l;

	if (line >= t->n) {
		return false;
	}
	l = &t->lines[line];
	if (l->len > cap) {
		return false;
	}
	if (WF_TS_READY != ts->state || ts->off_gen != t->gen ||
	        line >= ts->off_n) {
		memset(out, 0, l->len);
		return true;
	}
	wf_synov_fill(&ts->ov, ts->off[line], l->len, out);
	return true;
}
