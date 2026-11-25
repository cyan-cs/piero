#include "wf_syncore.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include <tree_sitter/api.h>

#include "wf_dllload_win.h"
#include "wf_manifest.h"
#include "wf_synlang.h"
#include "wf_synruns.h"

#define PATH_CAP 1024u
#define MAX_DLL_BYTES (8u * 1024u * 1024u)
#define MAX_QUERY_BYTES (256u * 1024u)
#define MAX_INLINE_RANGES 8192u

typedef struct
{
	const WfSynLang *lang;
	int state; /* 0 not tried, 1 loaded, 2 refused (kept: do not hash it again) */
	uint32_t fail;
	HMODULE mod;
	const TSLanguage *tl;
	TSQuery *query;
	uint8_t *capcls; /* class of each capture of the query */
	uint32_t ncap;
	uint32_t abi;
} Gram;

static Gram grams[WF_SYNLANG_COUNT];
static SRWLOCK gram_lock = SRWLOCK_INIT;
static wchar_t base_dir[PATH_CAP];
static const WfSynTrust *trust_table;
static size_t trust_n;
static volatile LONG64 stat_parses, stat_queries;
static volatile LONG stat_loaded, stat_sessions;

void wf_syncore_configure(const wchar_t *dir, const WfSynTrust *table, size_t n)
{
	AcquireSRWLockExclusive(&gram_lock);
	if (dir && wcslen(dir) < PATH_CAP) {
		wcscpy_s(base_dir, PATH_CAP, dir);
	} else {
		base_dir[0] = L'\0';
	}
	trust_table = table;
	trust_n = n;
	ReleaseSRWLockExclusive(&gram_lock);
}

typedef struct
{
	const char *name;
	uint8_t cls;
} CapName;

static const CapName cap_names[] = {
        {"comment", WF_SC_COMMENT},
        {"string", WF_SC_STRING},
        {"string.escape", WF_SC_ESCAPE},
        {"escape", WF_SC_ESCAPE},
        {"number", WF_SC_NUMBER},
        {"boolean", WF_SC_BOOLEAN},
        {"null", WF_SC_NULL},
        {"keyword", WF_SC_KEYWORD},
        {"type", WF_SC_TYPE},
        {"function", WF_SC_FUNCTION},
        {"variable", WF_SC_VARIABLE},
        {"constant", WF_SC_CONSTANT},
        {"operator", WF_SC_OPERATOR},
        {"punctuation", WF_SC_PUNCTUATION},
        {"key", WF_SC_KEY},
        {"property", WF_SC_KEY},
        {"heading", WF_SC_HEADING},
        {"markup", WF_SC_MARKUP},
        {"link", WF_SC_LINK},
        {"error", WF_SC_ERROR},
        {"preproc", WF_SC_PREPROC},
};

/* The class of a capture name: the whole name, then the name without its last
 * ".part", and so on. Not found: plain (nothing is painted). */
static uint8_t class_of_capture(const char *name, uint32_t n)
{
	char buf[64];

	if (!n || n >= sizeof buf) {
		return WF_SC_PLAIN;
	}
	memcpy(buf, name, n);
	buf[n] = '\0';
	for (;;) {
		char *dot;

		for (size_t i = 0; i < sizeof cap_names / sizeof cap_names[0];
		        i++) {
			if (!strcmp(buf, cap_names[i].name)) {
				return cap_names[i].cls;
			}
		}
		dot = strrchr(buf, '.');
		if (!dot) {
			return WF_SC_PLAIN;
		}
		*dot = '\0';
	}
}

static const WfSynTrust *trust_for(const char *id)
{
	for (size_t i = 0; i < trust_n; i++) {
		if (!strcmp(trust_table[i].id, id)) {
			return &trust_table[i];
		}
	}
	return NULL;
}

static void drop_gram(Gram *g)
{
	if (g->query) {
		ts_query_delete(g->query);
	}
	free(g->capcls);
	if (g->mod) {
		FreeLibrary(g->mod);
		InterlockedDecrement(&stat_loaded);
	}
	memset(g, 0, sizeof *g);
}

typedef const TSLanguage *(*LangFn)(void);

/* All of the steps of DR-DRAFT-033 section 4, in order. Called with the lock. */
static uint32_t load_gram(Gram *g)
{
	const WfSynLang *lang = g->lang;
	const WfSynTrust *tr = trust_for(lang->id);
	wchar_t wid[32], dir[PATH_CAP], pdll[PATH_CAP], pq[PATH_CAP],
	        pm[PATH_CAP];
	const wchar_t *parts[3];
	uint8_t want_dll[32], want_q[32], want_m[32];
	WfDllHold hm = {INVALID_HANDLE_VALUE, {0}, 0};
	WfDllHold hq = hm, hd = hm;
	char *mtext = NULL, *qtext = NULL;
	size_t mn = 0, qn = 0;
	WfManifest man;
	uint32_t st;
	FARPROC fp;
	LangFn fn;
	TSParser *probe;
	uint32_t err_off = 0;
	TSQueryError err_type = TSQueryErrorNone;
	size_t i;

	if (!tr || !wf_dll_hex32(tr->dll, want_dll) ||
	        !wf_dll_hex32(tr->query, want_q) ||
	        !wf_dll_hex32(tr->manifest, want_m) || !base_dir[0]) {
		return WF_SYN_E_TRUST;
	}
	for (i = 0; lang->id[i] && i + 1 < sizeof wid / sizeof wid[0]; i++) {
		wid[i] = (wchar_t)(unsigned char)lang->id[i];
	}
	wid[i] = L'\0';
	parts[0] = L"grammars";
	parts[1] = wid;
	parts[2] = L"manifest.toml";
	st = wf_dll_resolve(base_dir, parts, 3, pm, PATH_CAP);
	if (WF_SYN_OK != st) {
		return st;
	}
	parts[2] = L"highlights.scm";
	st = wf_dll_resolve(base_dir, parts, 3, pq, PATH_CAP);
	if (WF_SYN_OK != st) {
		return st;
	}
	parts[2] = L"parser.dll";
	st = wf_dll_resolve(base_dir, parts, 3, pdll, PATH_CAP);
	if (WF_SYN_OK != st) {
		return st;
	}
	(void)dir;
	if (!wf_dll_present(pm) || !wf_dll_present(pq) ||
	        !wf_dll_present(pdll)) {
		return WF_SYN_E_NOGRAMMAR;
	}

	/* the manifest: hashed, then read strictly, then compared with the table */
	st = wf_dll_hold(pm, WF_MANIFEST_MAX_BYTES, want_m, &hm);
	if (WF_SYN_OK != st) {
		goto out;
	}
	st = wf_dll_hold_read(&hm, &mtext, &mn);
	if (WF_SYN_OK != st) {
		goto out;
	}
	if (WF_MF_OK != wf_manifest_parse(mtext, mn, &man, NULL) ||
	        WF_MF_OK != wf_manifest_agrees(&man, lang)) {
		st = WF_SYN_E_MANIFEST;
		goto out;
	}
	/* the query */
	st = wf_dll_hold(pq, MAX_QUERY_BYTES, want_q, &hq);
	if (WF_SYN_OK != st) {
		goto out;
	}
	st = wf_dll_hold_read(&hq, &qtext, &qn);
	if (WF_SYN_OK != st) {
		goto out;
	}
	/* the DLL: held (nobody can change it) from the hash to the load */
	st = wf_dll_hold(pdll, MAX_DLL_BYTES, want_dll, &hd);
	if (WF_SYN_OK != st) {
		goto out;
	}
	st = wf_dll_load(pdll, &g->mod);
	if (WF_SYN_OK != st) {
		goto out;
	}
	InterlockedIncrement(&stat_loaded);
	fp = GetProcAddress(g->mod, lang->entry);
	if (!fp) {
		st = WF_SYN_E_SYMBOL;
		goto out;
	}
	fn = (LangFn)(void *)fp;
	g->tl = fn();
	if (!g->tl) {
		st = WF_SYN_E_SYMBOL;
		goto out;
	}
	g->abi = ts_language_abi_version(g->tl);
	if (g->abi < TREE_SITTER_MIN_COMPATIBLE_LANGUAGE_VERSION ||
	        g->abi > TREE_SITTER_LANGUAGE_VERSION || g->abi != man.abi) {
		st = WF_SYN_E_ABI;
		goto out;
	}
	{
		const char *name = ts_language_name(g->tl);

		if (name && strcmp(name, lang->id)) {
			st = WF_SYN_E_ABI; /* not the language that was asked for */
			goto out;
		}
	}
	probe = ts_parser_new();
	if (!probe) {
		st = WF_SYN_E_NOMEM;
		goto out;
	}
	if (!ts_parser_set_language(probe, g->tl)) {
		ts_parser_delete(probe);
		st = WF_SYN_E_ABI;
		goto out;
	}
	ts_parser_delete(probe);
	g->query =
	        ts_query_new(g->tl, qtext, (uint32_t)qn, &err_off, &err_type);
	if (!g->query) {
		st = WF_SYN_E_QUERY;
		goto out;
	}
	g->ncap = ts_query_capture_count(g->query);
	g->capcls = calloc(g->ncap ? g->ncap : 1u, 1);
	if (!g->capcls) {
		st = WF_SYN_E_NOMEM;
		goto out;
	}
	for (uint32_t c = 0; c < g->ncap; c++) {
		uint32_t len = 0;
		const char *name =
		        ts_query_capture_name_for_id(g->query, c, &len);

		g->capcls[c] = class_of_capture(name, len);
	}
	st = WF_SYN_OK;
out:
	free(mtext);
	free(qtext);
	wf_dll_release(&hm);
	wf_dll_release(&hq);
	wf_dll_release(&hd);
	if (WF_SYN_OK != st) {
		drop_gram(g);
	}
	return st;
}

/* The grammar of `lang`, loaded once. */
static uint32_t gram_get(const WfSynLang *lang, Gram **out)
{
	Gram *g = &grams[lang - wf_synlang_all(NULL)];
	uint32_t st;

	AcquireSRWLockExclusive(&gram_lock);
	if (1 == g->state) {
		*out = g;
		ReleaseSRWLockExclusive(&gram_lock);
		return WF_SYN_OK;
	}
	if (2 == g->state) {
		st = g->fail;
		ReleaseSRWLockExclusive(&gram_lock);
		return st;
	}
	g->lang = lang;
	st = load_gram(g);
	if (WF_SYN_OK == st) {
		g->state = 1;
		*out = g;
	} else if (WF_SYN_E_NOGRAMMAR != st) {
		g->lang = lang;
		g->state = 2; /* a refusal is kept: it is not hashed again */
		g->fail = st;
	}
	ReleaseSRWLockExclusive(&gram_lock);
	return st;
}

struct WfSynSession
{
	Gram *g, *inl;
	TSParser *parser, *iparser;
	TSQueryCursor *qc;
	TSTree *tree;
	char *prev;
	size_t prev_len, prev_cap;
	WfSynLimits lim;
};

static uint64_t qpc_us(void)
{
	LARGE_INTEGER f, c;

	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c);
	return (uint64_t)c.QuadPart * 1000000u / (uint64_t)f.QuadPart;
}

static uint32_t sane(uint32_t v, uint32_t dflt, uint32_t cap)
{
	return !v ? dflt : v > cap ? cap : v;
}

static uint32_t session_open(
        const char *lang_id, const WfSynLimits *limits, WfSynSession **out)
{
	const WfSynLang *lang = wf_synlang_by_id(lang_id);
	WfSynSession *s;
	Gram *g = NULL;
	uint32_t st;

	if (!out || !lang_id) {
		return WF_SYN_E_BADARG;
	}
	*out = NULL;
	if (!lang || lang->builtin || lang->companion) {
		return WF_SYN_E_UNKNOWN;
	}
	st = gram_get(lang, &g);
	if (WF_SYN_OK != st) {
		return st;
	}
	s = calloc(1, sizeof *s);
	if (!s) {
		return WF_SYN_E_NOMEM;
	}
	s->g = g;
	s->parser = ts_parser_new();
	s->qc = ts_query_cursor_new();
	if (!s->parser || !s->qc || !ts_parser_set_language(s->parser, g->tl)) {
		goto fail;
	}
	ts_query_cursor_set_match_limit(s->qc, 4096);
	if (!strcmp(lang->id, "markdown")) {
		const WfSynLang *il = wf_synlang_by_id("markdown_inline");
		Gram *ig = NULL;

		if (il && WF_SYN_OK == gram_get(il, &ig)) {
			s->iparser = ts_parser_new();
			if (s->iparser &&
			        ts_parser_set_language(s->iparser, ig->tl)) {
				s->inl = ig;
			} else if (s->iparser) {
				ts_parser_delete(s->iparser);
				s->iparser = NULL;
			}
		}
	}
	s->lim.max_text = sane(limits ? limits->max_text : 0,
	        WF_SYN_DEFAULT_MAX_TEXT, 16u * 1024u * 1024u);
	s->lim.max_line = sane(limits ? limits->max_line : 0,
	        WF_SYN_DEFAULT_MAX_LINE, 1024u * 1024u);
	s->lim.parse_ms = sane(
	        limits ? limits->parse_ms : 0, WF_SYN_DEFAULT_PARSE_MS, 10000u);
	s->lim.query_ms = sane(
	        limits ? limits->query_ms : 0, WF_SYN_DEFAULT_QUERY_MS, 10000u);
	s->lim.max_runs = sane(limits ? limits->max_runs : 0,
	        WF_SYN_DEFAULT_MAX_RUNS, 4u * 1000u * 1000u);
	InterlockedIncrement(&stat_sessions);
	*out = s;
	return WF_SYN_OK;
fail:
	if (s->parser) {
		ts_parser_delete(s->parser);
	}
	if (s->qc) {
		ts_query_cursor_delete(s->qc);
	}
	free(s);
	return WF_SYN_E_NOMEM;
}

static void session_close(WfSynSession *s)
{
	if (!s) {
		return;
	}
	if (s->tree) {
		ts_tree_delete(s->tree);
	}
	if (s->iparser) {
		ts_parser_delete(s->iparser);
	}
	ts_parser_delete(s->parser);
	ts_query_cursor_delete(s->qc);
	free(s->prev);
	free(s);
	InterlockedDecrement(&stat_sessions);
}

typedef struct
{
	const char *t;
	uint32_t n;
} Src;

static const char *src_read(
        void *payload, uint32_t at, TSPoint pos, uint32_t *got)
{
	const Src *s = payload;

	(void)pos;
	if (at >= s->n) {
		*got = 0;
		return "";
	}
	*got = s->n - at;
	return s->t + at;
}

static bool parse_progress(TSParseState *state)
{
	return GetTickCount64() > *(const ULONGLONG *)state->payload;
}

static bool query_progress(TSQueryCursorState *state)
{
	return GetTickCount64() > *(const ULONGLONG *)state->payload;
}

static TSTree *parse_text(
        TSParser *p, TSTree *old, const char *t, uint32_t n, uint32_t ms)
{
	Src src = {t, n};
	TSInput in = {&src, src_read, TSInputEncodingUTF8, NULL};
	ULONGLONG deadline = GetTickCount64() + ms;
	TSParseOptions opt = {&deadline, parse_progress};
	TSTree *tree;

	InterlockedIncrement64(&stat_parses);
	tree = ts_parser_parse_with_options(p, old, in, opt);
	if (!tree) {
		ts_parser_reset(p);
	}
	return tree;
}

/* The point at byte `to` of `t`, given the point `p` at byte `from` of it. */
static TSPoint point_from(TSPoint p, const char *t, size_t from, size_t to)
{
	for (size_t i = from; i < to; i++) {
		if ('\n' == t[i]) {
			p.row++;
			p.column = 0;
		} else {
			p.column++;
		}
	}
	return p;
}

/* Tells the old tree what changed: the one edit that turns the last text into
 * this one (common prefix and suffix). Returns false when the text is the
 * same, so nothing has to be parsed. */
static bool edit_old_tree(
        WfSynSession *s, const char *text, size_t len, bool *reuse)
{
	size_t pre = 0, suf = 0, max;
	TSInputEdit e;

	*reuse = false;
	if (!s->tree || !s->prev) {
		return true;
	}
	max = s->prev_len < len ? s->prev_len : len;
	while (pre < max && s->prev[pre] == text[pre]) {
		pre++;
	}
	if (pre == len && pre == s->prev_len) {
		*reuse = true;
		return false;
	}
	while (suf < max - pre &&
	        s->prev[s->prev_len - 1 - suf] == text[len - 1 - suf]) {
		suf++;
	}
	e.start_byte = (uint32_t)pre;
	e.old_end_byte = (uint32_t)(s->prev_len - suf);
	e.new_end_byte = (uint32_t)(len - suf);
	/* the two texts are the same up to `pre`, so the start is worked out once and
	 * each end continues from it through the changed stretch only */
	e.start_point = point_from((TSPoint){0, 0}, s->prev, 0, pre);
	e.old_end_point =
	        point_from(e.start_point, s->prev, pre, s->prev_len - suf);
	e.new_end_point = point_from(e.start_point, text, pre, len - suf);
	ts_tree_edit(s->tree, &e);
	*reuse = true;
	return true;
}

static int remember_text(WfSynSession *s, const char *text, size_t len)
{
	if (len > s->prev_cap) {
		char *p = realloc(s->prev, len);

		if (!p) {
			return 0;
		}
		s->prev = p;
		s->prev_cap = len;
	}
	if (len) {
		memcpy(s->prev, text, len);
	}
	s->prev_len = len;
	return 1;
}

/* Paints the captures of `query` over `node` into cls (one byte per text
 * byte), in the order the cursor gives them: a later pattern and a deeper
 * node win. */
static uint32_t paint(
        WfSynSession *s, const Gram *g, TSNode node, uint8_t *cls, uint32_t len)
{
	ULONGLONG deadline = GetTickCount64() + s->lim.query_ms;
	TSQueryCursorOptions opt = {&deadline, query_progress};
	TSQueryMatch m;
	uint32_t ci;

	InterlockedIncrement64(&stat_queries);
	ts_query_cursor_exec_with_options(s->qc, g->query, node, &opt);
	while (ts_query_cursor_next_capture(s->qc, &m, &ci)) {
		const TSQueryCapture *c = &m.captures[ci];
		uint8_t k;
		uint32_t a, b;

		if (c->index >= g->ncap) {
			continue;
		}
		k = g->capcls[c->index];
		if (WF_SC_PLAIN == k) {
			continue;
		}
		a = ts_node_start_byte(c->node);
		b = ts_node_end_byte(c->node);
		if (b > len) {
			b = len;
		}
		if (a < b) {
			memset(cls + a, k, b - a);
		}
	}
	return GetTickCount64() > deadline ? WF_SYN_E_TIMEOUT : WF_SYN_OK;
}

/* Markdown: the inline grammar over the `inline` nodes of the block tree. */
static uint32_t paint_inline(WfSynSession *s,
        TSNode root,
        const char *text,
        uint32_t len,
        uint8_t *cls)
{
	TSRange *ranges;
	uint32_t n = 0, st = WF_SYN_OK;
	TSTreeCursor cur;
	TSTree *itree;

	ranges = malloc(MAX_INLINE_RANGES * sizeof *ranges);
	if (!ranges) {
		return WF_SYN_E_NOMEM;
	}
	cur = ts_tree_cursor_new(root);
	for (;;) {
		TSNode node = ts_tree_cursor_current_node(&cur);

		if (!strcmp(ts_node_type(node), "inline")) {
			if (n >= MAX_INLINE_RANGES) {
				n = 0; /* too many: leave the inline markup plain */
				break;
			}
			ranges[n].start_byte = ts_node_start_byte(node);
			ranges[n].end_byte = ts_node_end_byte(node);
			ranges[n].start_point = ts_node_start_point(node);
			ranges[n].end_point = ts_node_end_point(node);
			n++;
		} else if (ts_tree_cursor_goto_first_child(&cur)) {
			continue;
		}
		while (!ts_tree_cursor_goto_next_sibling(&cur)) {
			if (!ts_tree_cursor_goto_parent(&cur)) {
				goto walked;
			}
		}
	}
walked:
	ts_tree_cursor_delete(&cur);
	if (n && ts_parser_set_included_ranges(s->iparser, ranges, n)) {
		itree = parse_text(
		        s->iparser, NULL, text, len, s->lim.parse_ms);
		if (itree) {
			st = paint(
			        s, s->inl, ts_tree_root_node(itree), cls, len);
			ts_tree_delete(itree);
		} else {
			st = WF_SYN_E_TIMEOUT;
		}
	}
	free(ranges);
	return st;
}

static uint32_t lines_ok(const char *t, size_t len, uint32_t max_line)
{
	size_t at = 0;

	while (at < len) {
		const char *nl = memchr(t + at, '\n', len - at);
		size_t end = nl ? (size_t)(nl - t) : len;

		if (end - at > max_line) {
			return 0;
		}
		at = end + 1;
	}
	return 1;
}

static uint32_t session_highlight(
        WfSynSession *s, const char *text, size_t len, WfSynResult *out)
{
	uint8_t *cls = NULL;
	uint32_t st = WF_SYN_OK;
	bool reuse = false, changed;
	uint64_t t0, t1, t2;
	TSTree *tree;
	TSNode root;

	if (!s || !out || (!text && len)) {
		return WF_SYN_E_BADARG;
	}
	memset(out, 0, sizeof *out);
	if (len > s->lim.max_text || len > 0xfffffffeu ||
	        !lines_ok(text, len, s->lim.max_line)) {
		return out->status = WF_SYN_E_LIMIT;
	}
	t0 = qpc_us();
	changed = edit_old_tree(s, text, len, &reuse);
	if (changed) {
		tree = parse_text(s->parser, reuse ? s->tree : NULL, text,
		        (uint32_t)len, s->lim.parse_ms);
		if (!tree) {
			/* the old tree is not usable after a stopped parse */
			if (s->tree) {
				ts_tree_delete(s->tree);
				s->tree = NULL;
			}
			s->prev_len = 0;
			return out->status = WF_SYN_E_TIMEOUT;
		}
		if (s->tree) {
			ts_tree_delete(s->tree);
		}
		s->tree = tree;
		if (!remember_text(s, text, len)) {
			ts_tree_delete(s->tree);
			s->tree = NULL;
			return out->status = WF_SYN_E_NOMEM;
		}
	}
	out->incremental = reuse ? 1u : 0u;
	t1 = qpc_us();
	out->parse_us = (uint32_t)(t1 - t0);
	root = ts_tree_root_node(s->tree);
	cls = calloc(len ? len : 1u, 1);
	if (!cls) {
		return out->status = WF_SYN_E_NOMEM;
	}
	st = paint(s, s->g, root, cls, (uint32_t)len);
	if (WF_SYN_OK == st && s->inl) {
		st = paint_inline(s, root, text, (uint32_t)len, cls);
	}
	t2 = qpc_us();
	out->query_us = (uint32_t)(t2 - t1);
	if (WF_SYN_OK == st) {
		st = wf_syn_runs_build(cls, (uint32_t)len, s->lim.max_runs,
		        &out->runs, &out->nruns);
	}
	free(cls);
	out->status = st;
	return st;
}

static void result_free(WfSynResult *r)
{
	if (r) {
		free(r->runs);
		r->runs = NULL;
		r->nruns = 0;
	}
}

static uint64_t stat_get(uint32_t which)
{
	switch (which) {
	case WF_SYN_STAT_PARSES:   return (uint64_t)stat_parses;
	case WF_SYN_STAT_LOADED:   return (uint64_t)stat_loaded;
	case WF_SYN_STAT_SESSIONS: return (uint64_t)stat_sessions;
	case WF_SYN_STAT_QUERIES:  return (uint64_t)stat_queries;
	default:                   return 0;
	}
}

static void core_shutdown(void)
{
	AcquireSRWLockExclusive(&gram_lock);
	if (!stat_sessions) {
		for (size_t i = 0; i < WF_SYNLANG_COUNT; i++) {
			drop_gram(&grams[i]);
		}
	}
	ReleaseSRWLockExclusive(&gram_lock);
}

static const char *core_version(void)
{
	return "piero-syntax 1 (tree-sitter 0.27.2)";
}

static const WfSyntaxApi api = {WF_SYNTAX_HOST_ABI, sizeof(WfSyntaxApi),
        core_version, session_open, session_highlight, result_free,
        session_close, stat_get, core_shutdown};

const WfSyntaxApi *wf_syncore_api(void)
{
	return &api;
}
