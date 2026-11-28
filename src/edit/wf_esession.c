#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../archive/wf_vpath.h"
#include "../base/wf_buf.h"
#include "../fs/wf_loc.h"
#include "../fs/wf_sftpplace_win.h"
#include "../i18n/wf_i18n.h"
#include "../input/wf_commands.h"
#include "../input/wf_exline.h"
#include "../path/wf_name.h"
#include "../path/wf_path.h"
#include "../process/wf_lock.h"
#include "../text/wf_text.h"
#include "wf_esession_internal.h"
#include "../base/wf_mem.h"
#include "../base/wf_macro.h"

#define MiB (1024u * 1024u)

void wf_es_config_default(WfEditConfig *c)
{
	memset(c, 0, sizeof *c);
	wf_ed_options_default(&c->ed);
	c->keymap = WF_KM_PIERO;
	c->line_numbers = true;
	c->syntax = true;
	c->soft_wrap = false;
	c->show_whitespace = false;
	c->backup = true;
	c->drafts = true;
	c->fallback_codepage = 0;
}

WfBuffer *wf_es_buffer(WfEditSession *s)
{
	return s->n ? s->buf[s->cur] : NULL;
}

WfEditor *wf_es_current(WfEditSession *s)
{
	WfBuffer *b = wf_es_buffer(s);

	return b ? &b->ed : NULL;
}

bool wf_es_prompt_active(const WfEditSession *s)
{
	return PR_NONE != s->pk;
}

bool wf_es_place(const WfEditSession *s, WfEsPlace *out)
{
	const WfBuffer *b = s && s->n ? s->buf[s->cur] : NULL;

	if (!b || !b->path) {
		return false;
	}
	out->path = b->path;
	out->line = b->ed.cur.line;
	out->col = b->ed.cur.col;
	out->top = b->top;
	out->left = b->left;
	out->modified = wf_ed_modified(&b->ed) && b->has_draft;
	return true;
}

/* The place, made valid for the text there is now. */
static void apply_place(
        WfBuffer *b, size_t line, size_t col, size_t top, size_t left)
{
	WfPos p;

	if (!b->ed.text.n) {
		return;
	}
	p.line = line < b->ed.text.n ? line : b->ed.text.n - 1;
	p.col = col < b->ed.text.lines[p.line].len
	        ? col
	        : b->ed.text.lines[p.line].len;
	{ /* inside a character: at the start of the one it was in, the way the
	   * cursor itself steps */
		const WfELine *l = &b->ed.text.lines[p.line];
		size_t c = 0;

		while (c < p.col && c < l->len) {
			size_t next = wf_et_col_next(l, c);

			if (next > p.col) {
				break;
			}
			c = next;
		}
		p.col = c;
	}
	wf_ed_clear_selection(&b->ed);
	b->ed.cur = p;
	b->ed.want_valid = false; /* the column the cursor wants is this one */
	b->top = top < b->ed.text.n ? top : b->ed.text.n - 1;
	b->top_wrap = 0;
	b->left = left;
}

void wf_es_goto(
        WfEditSession *s, size_t line, size_t col, size_t top, size_t left)
{
	WfBuffer *b = wf_es_buffer(s);

	if (!b) {
		return;
	}
	if (PR_DRAFT == s->pk) {
		s->goto_pending = true;
		s->goto_line = line;
		s->goto_col = col;
		s->goto_top = top;
		s->goto_left = left;
		return;
	}
	apply_place(b, line, col, top, left);
}

size_t wf_es_buffer_count(const WfEditSession *s)
{
	return s->n;
}

static WfEsResult exec(WfEditSession *s, WfEdAction a);

wchar_t *wf_es_lock_path(const WfEditSession *s)
{
	return s->lock_path ? wf_path_dup(s->lock_path) : NULL;
}

bool wf_es_take_lock_look(WfEditSession *s)
{
	bool v = s->lock_look;

	s->lock_look = false;
	return v;
}

WfEsResult wf_es_save_again(WfEditSession *s)
{
	s->pk = PR_NONE;
	return exec(s, (WfEdAction){WF_CMD_SAVE, false});
}

bool wf_es_take_saved(WfEditSession *s)
{
	bool v = s->saved_since;

	s->saved_since = false;
	return v;
}

const char *wf_es_status(const WfEditSession *s)
{
	return s->msg;
}

/* Messages are printf-style; the attribute lets clang check them. */
#if defined(__clang__) || defined(__GNUC__)
#	define ES_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#	define ES_PRINTF(a, b)
#endif
static void say(WfEditSession *s, const char *fmt, ...) ES_PRINTF(2, 3);

static void say(WfEditSession *s, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(s->msg, sizeof s->msg, fmt, ap);
	va_end(ap);
	s->msg_error = false;
}

/* The same, from the catalog. */
#define MA(...)                                                                \
	(WfMsgArg[]){__VA_ARGS__},                                             \
	        sizeof((WfMsgArg[]){__VA_ARGS__}) / sizeof(WfMsgArg)

static void say_m(WfEditSession *s, WfMsg id, const WfMsgArg *a, size_t n)
{
	(void)wf_msgf(s->msg, sizeof s->msg, id, a, n);
	s->msg_error = false;
}

static void fail_m(WfEditSession *s, WfMsg id, const WfMsgArg *a, size_t n)
{
	(void)wf_msgf(s->msg, sizeof s->msg, id, a, n);
	s->msg_error = true;
}

/* A path in the words of a message: its leaf, made safe to draw. */
static void leaf_text(const wchar_t *path, char *out, size_t cap);

/* A save stopped half way and left files behind: say where, and offer to open
 * them on the spot (the new text, the previous version, or both) so that
 * nothing has to be found by hand. Nothing is changed by the question. */
static void recover_begin(WfEditSession *s, const WfSaveResult *r)
{
	char a[96], b[96];

	free(s->recover_new);
	free(s->recover_old);
	s->recover_new = wf_path_dup(r->kept_temp);
	s->recover_old =
	        r->kept_original[0] ? wf_path_dup(r->kept_original) : NULL;
	leaf_text(r->kept_temp, a, sizeof a);
	if (s->recover_old) {
		leaf_text(r->kept_original, b, sizeof b);
		(void)wf_msgf(s->label, sizeof s->label,
		        MSG_ED_ASK_RECOVER_BOTH,
		        (WfMsgArg[]){WF_ARG_S(a), WF_ARG_S(b)}, 2);
		s->pk = PR_RECOVER;
		wf_choice_begin(&s->choice, "tobc", 3);
	} else {
		(void)wf_msgf(s->label, sizeof s->label, MSG_ED_ASK_RECOVER_NEW,
		        &WF_ARG_S(a), 1);
		s->pk = PR_RECOVER;
		wf_choice_begin(&s->choice, "tc", 1);
	}
}

static void leaf_text(const wchar_t *path, char *out, size_t cap)
{
	const wchar_t *leaf = L"";
	size_t len = 0;
	WfBuf b;

	if (path) {
		wf_path_leaf(path, &leaf, &len);
	}
	wf_buf_init(&b);
	if (WF_OK != wf_sanitize_utf16(leaf, len, &b)) {
		snprintf(out, cap, "?");
	} else {
		snprintf(out, cap, "%s", wf_buf_cstr(&b));
	}
	wf_buf_free(&b);
}

static const char *status_text(WfStatus st)
{
	switch (st) {
	case WF_E_ACCESS: return wf_msg(MSG_ED_STATUS_READONLY);
	case WF_E_RANGE:  return wf_msg(MSG_ED_STATUS_LIMIT);
	case WF_E_NOMEM:  return wf_msg(MSG_NAV_NOMEM);
	default:          return wf_msg(MSG_ED_STATUS_NOT_DONE);
	}
}

static void buffer_free(WfBuffer *b)
{
	if (!b) {
		return;
	}
	wf_ed_free(&b->ed);
	wf_syn_free(&b->syn);
	wf_mdp_free(&b->md);
	wf_gp_free(&b->git);
	wf_es_wrap_free(b);
	free(b->found.at);
	free(b->found.pat);
	free(b->path);
	free(b);
}

static WfBuffer *buffer_new(WfEditSession *s)
{
	WfBuffer *b = calloc(1, sizeof *b);

	if (!b) {
		return NULL;
	}
	if (WF_OK != wf_ed_init(&b->ed, &s->cfg.ed)) {
		free(b);
		return NULL;
	}
	b->ed.dirty_from = 0;
	b->git.id = ++s->git_next_id;
	return b;
}

static bool push_buffer(WfEditSession *s, WfBuffer *b)
{
	if (s->n == s->cap) {
		size_t cap = s->cap ? s->cap * 2 : 4;
		WfBuffer **p = wf_realloc_array(s->buf, cap, sizeof *p);

		if (!p) {
			return false;
		}
		s->buf = p;
		s->cap = cap;
	}
	s->buf[s->n] = b;
	s->cur = s->n++;
	return true;
}

WfEditSession *wf_es_new(const WfEditConfig *c, WfEditIo io)
{
	WfEditSession *s = calloc(1, sizeof *s);

	if (!s) {
		return NULL;
	}
	if (c) {
		s->cfg = *c;
	} else {
		wf_es_config_default(&s->cfg);
	}
	wf_vim_init(&s->vim);
	s->io = io;
	s->f_case = (s->cfg.keys && wf_keymap_modal(s->cfg.keys)) ||
	        s->cfg.ed.case_sensitive;
	return s;
}

void wf_es_free(WfEditSession *s)
{
	size_t i;

	if (!s) {
		return;
	}
	if (s->git_port.cancel) {
		s->git_port.cancel(s->git_port
		                .ctx); /* what waits and runs is not wanted */
	}
	for (i = 0; i < s->n; i++) {
		buffer_free(s->buf[i]);
	}
	wf_es_syntax_close(s);
	free(s->buf);
	free(s->base_dir);
	free(s->pending_path);
	free(s->recover_new);
	free(s->recover_old);
	free(s->draft_text);
	free(s->lock_path);
	free(s->nano_cut.text);
	wf_vim_free(&s->vim);
	free(s);
}

/* The buffer holds the file `path` names (or, when the file has just been read,
 * the file whose stamp is `st`). Only the exact path says so without looking at
 * the file: where a folder tells File from file two names are two files, and
 * where it does not, the identity of the file settles it. */
static bool same_file(
        const WfBuffer *b, const WfFileStamp *st, const wchar_t *path)
{
	/* a WSL distribution has no file identity: only the exact path there */
	bool wsl = path && (wf_wsl_unc_is(path) || wf_sftp_place_is(path));
	static const FILE_ID_INFO none = {0}; /* all zero: no identity */

	if (b->path && path && !wcscmp(b->path, path)) {
		return true;
	}
	return !wsl && b->on_disk && st && st->valid &&
	        memcmp(&st->id, &none, sizeof none) &&
	        !memcmp(&b->stamp.id, &st->id, sizeof st->id);
}

/* The last component of `path`, NUL-terminated (the path's own tail). */
static const wchar_t *leaf_ptr(const wchar_t *path)
{
	const wchar_t *leaf;
	size_t len;

	wf_path_leaf(path, &leaf, &len);
	return leaf;
}

/* Puts the loaded text in the buffer. Nothing else about the buffer (where it
 * saves to, the stamp of the file it came from) is set unless the text got in:
 * a buffer that failed to take its text must never be savable over the file. */
static WfStatus apply_loaded(WfEditSession *s,
        WfBuffer *b,
        const wchar_t *path,
        WfLoaded *l,
        bool created)
{
	WfEncoding enc = l ? l->enc : (WfEncoding){WF_ENC_UTF8, 0};
	WfStatus st = wf_ed_set_text(
	        &b->ed, l ? l->utf8 : "", l ? l->utf8_n : 0, enc);

	if (WF_OK != st) {
		return st;
	}
	b->lang = s->cfg.syntax ? wf_syn_lang_for_name(leaf_ptr(path))
	                        : WF_LANG_TEXT;
	wf_syn_free(&b->syn);
	wf_syn_init(&b->syn, b->lang);
	wf_es_syntax_assign(s, b, path);
	if (!created) {
		wf_es_git_assign(s, b, path);
	}
	b->on_disk = !created;
	if (created) {
		/* nothing was read, so there is no break of the file's own: the new
		 * file's is the configured one, not what an empty text defaults to */
		b->ed.text.default_eol = b->ed.opt.new_eol;
	}
	if (l) {
		b->stamp = l->stamp;
		b->external_noted = false;
	}
	b->ed.read_only = l && (l->stamp.attributes & FILE_ATTRIBUTE_READONLY);
	b->ed.opt.max_bytes = s->cfg.ed.max_bytes;
	return WF_OK;
}

/* Takes back the buffer push_buffer just added, and the selection of it. */
static void draft_offer(WfEditSession *s, WfBuffer *b);
static void draft_forget(WfEditSession *s, WfBuffer *b);

static void drop_new_buffer(WfEditSession *s, size_t previous)
{
	buffer_free(s->buf[--s->n]);
	s->cur = previous < s->n ? previous : (s->n ? s->n - 1 : 0);
}

WfOpenResult wf_es_open(
        WfEditSession *s, const wchar_t *path, char *why, size_t why_cap)
{
	WfLoaded l;
	WfStatus st;
	size_t i;
	WfBuffer *b;

	if (why_cap) {
		why[0] = '\0';
	}
	for (i = 0; i < s->n; i++) {
		if (s->buf[i]->path && same_file(s->buf[i], NULL, path)) {
			s->cur = i;
			return WF_OPEN_OK;
		}
	}
	if (!s->base_dir) {
		size_t n = wf_path_parent_len(path);

		if (n) {
			s->base_dir = wf_path_dup_n(path, n);
		}
	}
	st = s->io.load(s->io.ctx, path, s->cfg.ed.max_bytes,
	        s->cfg.fallback_codepage, NULL, &l);
	if (WF_OK != st) {
		(void)wf_msgf(why, why_cap, MSG_ED_WHY_CANNOT_OPEN,
		        &WF_ARG_S(wf_status_name(st)), 1);
		return WF_OPEN_REFUSED;
	}
	switch (l.verdict) {
	case WF_FL_OK: break;
	case WF_FL_MISSING:
		/* only the Open prompt creates; the file manager's key does not */
		snprintf(why, why_cap, "%s", wf_msg(MSG_REASON_FILE_MISSING));
		wf_efile_loaded_free(&l);
		return WF_OPEN_REFUSED;
	case WF_FL_TOO_BIG:
		(void)wf_msgf(why, why_cap, MSG_ED_WHY_TOO_BIG,
		        &WF_ARG_N(s->cfg.ed.max_bytes / MiB), 1);
		wf_efile_loaded_free(&l);
		return WF_OPEN_REFUSED;
	case WF_FL_AMBIGUOUS:
		if (l.candidate_ok) {
			char name[32];

			wf_enc_name(l.enc, name, sizeof name);
			free(s->pending_path);
			s->pending_path = wf_path_dup(path);
			s->pending_enc = l.enc;
			s->pk = PR_ENCODING;
			wf_choice_begin(&s->choice, "yn", 1);
			(void)wf_msgf(s->label, sizeof s->label,
			        MSG_ED_ASK_ENCODING, &WF_ARG_S(name), 1);
			wf_efile_loaded_free(&l);
			return s->pending_path ? WF_OPEN_ASKING
			                       : WF_OPEN_REFUSED;
		}
		snprintf(why, why_cap, "%s",
		        l.reason ? wf_msg_reason(l.reason)
		                 : wf_msg(MSG_ED_WHY_UNKNOWN_ENCODING));
		wf_efile_loaded_free(&l);
		return WF_OPEN_REFUSED;
	case WF_FL_BINARY:
		(void)wf_msgf(why, why_cap, MSG_ED_WHY_BINARY,
		        &WF_ARG_S(l.reason ? wf_msg_reason(l.reason)
		                           : wf_msg(MSG_ED_WHY_DATA)),
		        1);
		wf_efile_loaded_free(&l);
		return WF_OPEN_REFUSED;
	default:
		snprintf(why, why_cap, "%s",
		        l.reason ? wf_msg_reason(l.reason)
		                 : wf_msg(MSG_EDIT_CANNOT));
		wf_efile_loaded_free(&l);
		return WF_OPEN_REFUSED;
	}
	/* the same file under another spelling */
	for (i = 0; i < s->n; i++) {
		if (same_file(s->buf[i], &l.stamp, path)) {
			s->cur = i;
			wf_efile_loaded_free(&l);
			say_m(s, MSG_ED_ALREADY_OPEN, NULL, 0);
			return WF_OPEN_OK;
		}
	}
	b = buffer_new(s);
	if (!b) {
		wf_efile_loaded_free(&l);
		snprintf(why, why_cap, "%s", wf_msg(MSG_NAV_NOMEM));
		return WF_OPEN_REFUSED;
	}
	b->path = wf_path_dup(path);
	{
		size_t previous = s->cur;

		if (!b->path || !push_buffer(s, b)) {
			buffer_free(b);
			wf_efile_loaded_free(&l);
			snprintf(why, why_cap, "%s", wf_msg(MSG_NAV_NOMEM));
			return WF_OPEN_REFUSED;
		}
		st = apply_loaded(s, b, path, &l, false);
		wf_efile_loaded_free(&l);
		if (WF_OK != st) {
			/* the text did not fit (the file can be smaller on disk
			 * than as text) or memory ran out: no buffer, nothing that
			 * could be saved over the file */
			drop_new_buffer(s, previous);
			if (WF_E_RANGE == st) {
				(void)wf_msgf(why, why_cap, MSG_ED_WHY_TOO_BIG,
				        &WF_ARG_N(s->cfg.ed.max_bytes / MiB),
				        1);
			} else {
				snprintf(why, why_cap, "%s",
				        wf_msg(MSG_NAV_NOMEM));
			}
			return WF_OPEN_REFUSED;
		}
	}
	if (b->ed.read_only) {
		say_m(s, MSG_ED_READONLY_OPEN, NULL, 0);
	}
	draft_offer(s, b);
	return WF_OPEN_OK;
}

/* Opens `path` as a new, empty file that does not exist yet. */
static bool create_buffer(WfEditSession *s, const wchar_t *path)
{
	WfBuffer *b = buffer_new(s);

	if (!b) {
		return false;
	}
	b->path = wf_path_dup(path);
	{
		size_t previous = s->cur;

		if (!b->path || !push_buffer(s, b)) {
			buffer_free(b);
			return false;
		}
		if (WF_OK != apply_loaded(s, b, path, NULL, true)) {
			drop_new_buffer(s, previous);
			return false;
		}
	}
	return true;
}

static WfEsResult close_current(WfEditSession *s)
{
	if (!s->n) {
		return WF_ES_CLOSED;
	}
	draft_forget(s, s->buf[s->cur]);
	buffer_free(s->buf[s->cur]);
	memmove(&s->buf[s->cur], &s->buf[s->cur + 1],
	        (s->n - s->cur - 1) * sizeof *s->buf);
	s->n--;
	if (s->cur >= s->n && s->n) {
		s->cur = s->n - 1;
	}
	return s->n ? WF_ES_REDRAW : WF_ES_CLOSED;
}

static void prompt_begin(
        WfEditSession *s, PromptKind k, const char *label, const char *initial)
{
	s->pk = k;
	snprintf(s->label, sizeof s->label, "%s", label);
	s->in_n = 0;
	if (initial) {
		s->in_n = strlen(initial);
		if (s->in_n >= sizeof s->in) {
			s->in_n = sizeof s->in - 1;
		}
		memcpy(s->in, initial, s->in_n);
	}
	s->in[s->in_n] = '\0';
	s->in_cur = s->in_n;
	s->in_fresh = s->in_n > 0;
	s->hist_pos = s->hist_n;
}

static void prompt_end(WfEditSession *s)
{
	s->pk = PR_NONE;
	s->in_n = s->in_cur = 0;
	s->in[0] = '\0';
}

static void prompt_insert(WfEditSession *s, uint32_t cp)
{
	char enc[4];
	size_t n;

	if (cp < 0x20 || 0x7F == cp) {
		return;
	}
	n = wf_utf8_encode(cp, enc);
	if (s->in_n + n >= sizeof s->in) {
		return;
	}
	memmove(s->in + s->in_cur + n, s->in + s->in_cur, s->in_n - s->in_cur);
	memcpy(s->in + s->in_cur, enc, n);
	s->in_cur += n;
	s->in_n += n;
	s->in[s->in_n] = '\0';
}

static size_t prompt_prev(const WfEditSession *s, size_t at)
{
	if (0 == at) {
		return 0;
	}
	at--;
	while (at > 0 && 0x80 == ((unsigned char)s->in[at] & 0xC0)) {
		at--;
	}
	return at;
}

static size_t prompt_next(const WfEditSession *s, size_t at)
{
	uint32_t cp;

	if (at >= s->in_n) {
		return s->in_n;
	}
	return at + wf_utf8_decode(s->in + at, s->in_n - at, &cp);
}

static void prompt_remove(WfEditSession *s, size_t from, size_t to)
{
	memmove(s->in + from, s->in + to, s->in_n - to);
	s->in_n -= to - from;
	s->in_cur = from;
	s->in[s->in_n] = '\0';
}

static void remember(WfEditSession *s, const char *text)
{
	size_t i;

	if (!*text) {
		return;
	}
	for (i = 0; i < s->hist_n; i++) {
		if (!strcmp(s->hist[i], text)) {
			return;
		}
	}
	if (WF_ES_HISTORY == s->hist_n) {
		memmove(s->hist[0], s->hist[1],
		        (WF_ES_HISTORY - 1) * sizeof s->hist[0]);
		s->hist_n--;
	}
	snprintf(s->hist[s->hist_n++], sizeof s->hist[0], "%s", text);
}

/* The whole text as raw UTF-8 for saving. */
static WfStatus snapshot(const WfBuffer *b, char **out, size_t *n)
{
	return wf_et_read(
	        &b->ed.text, (WfPos){0, 0}, wf_et_end(&b->ed.text), out, n);
}

/* The draft of a buffer is of no use any more (saved, restored or thrown
 * away): remove it. */
static void draft_forget(WfEditSession *s, WfBuffer *b)
{
	if (s->io.draft_remove && b->path) {
		s->io.draft_remove(s->io.ctx, b->path);
	}
	b->has_draft = false;
}

/* A file has just been opened: if the last session left a draft of unsaved
 * text for it, say so and ask. A draft equal to the file is of no use and is
 * removed without a word. */
static void draft_offer(WfEditSession *s, WfBuffer *b)
{
	WfEncoding enc;
	char *text = NULL, *now = NULL, name[96];
	size_t n = 0, now_n = 0;

	if (!s->cfg.drafts || !s->io.draft_read || !b->path ||
	        WF_OK !=
	                s->io.draft_read(s->io.ctx, b->path, &enc, &text, &n)) {
		return;
	}
	if (WF_OK == snapshot(b, &now, &now_n) && now_n == n &&
	        0 == memcmp(now, text, n)) {
		free(text);
		free(now);
		draft_forget(s, b);
		return;
	}
	free(now);
	free(s->draft_text);
	s->draft_text = text;
	s->draft_n = n;
	s->draft_enc = enc;
	leaf_text(b->path, name, sizeof name);
	(void)wf_msgf(s->label, sizeof s->label, MSG_ED_ASK_DRAFT,
	        &WF_ARG_S(name), 1);
	s->pk = PR_DRAFT;
	wf_choice_begin(&s->choice, "rd",
	        0); /* the safe one: restoring can be undone */
}

/* Whether the buffer has text that no draft holds yet: the one rule for both
 * "write a draft now" and "wake up for one", so that the wait never asks for a
 * wake-up that the write would not act on. */
static bool draft_wanted(const WfBuffer *b)
{
	return b->path && wf_ed_modified(&b->ed) &&
	        !(b->has_draft && b->draft_gen == b->ed.text.gen);
}

void wf_es_save_drafts(WfEditSession *s, uint64_t now_ms)
{
	if (!s->cfg.drafts || !s->io.draft_write) {
		return;
	}
	for (size_t i = 0; i < s->n; i++) {
		WfBuffer *b = s->buf[i];
		char *text = NULL;
		size_t n = 0;

		if (!b->path) {
			continue;
		}
		/* a write that was carried out later and failed: the draft is not
		 * there, whatever was thought when it was handed over */
		if (b->has_draft && s->io.draft_failed &&
		        s->io.draft_failed(s->io.ctx, b->path)) {
			b->has_draft = false;
		}
		if (!wf_ed_modified(&b->ed)) {
			if (b->has_draft) {
				draft_forget(s, b);
			}
			continue;
		}
		if (!draft_wanted(b)) {
			continue; /* the draft holds this very text: nothing to copy or write */
		}
		if (b->draft_ms &&
		        now_ms - b->draft_ms < WF_ES_DRAFT_EVERY_MS) {
			continue;
		}
		/* a draft found at open and not yet answered is not overwritten */
		if (PR_DRAFT == s->pk && s->draft_text && i == s->cur) {
			continue;
		}
		/* what may not be kept (a secret's name, too big) is known from the
		 * path and the size, before the text is copied */
		if (s->io.draft_allowed &&
		        !s->io.draft_allowed(
		                s->io.ctx, b->path, b->ed.text.bytes)) {
			b->draft_ms = now_ms ? now_ms : 1;
			continue;
		}
		if (WF_OK != snapshot(b, &text, &n)) {
			b->draft_ms = now_ms
			        ? now_ms
			        : 1; /* tried again later, not at once */
			continue;
		}
		if ((!s->io.draft_allowed ||
		            s->io.draft_allowed(s->io.ctx, b->path, n)) &&
		        WF_OK ==
		                s->io.draft_write(s->io.ctx, b->path, b->ed.enc,
		                        text, n)) {
			b->has_draft = true;
			b->draft_gen = b->ed.text.gen;
		}
		b->draft_ms = now_ms ? now_ms : 1; /* 0 means "never tried" */
		free(text);
	}
}

static void after_save(
        WfEditSession *s, WfBuffer *b, const WfSaveResult *r, size_t bytes)
{
	b->stamp = r->stamp;
	b->external_noted = false;
	b->save_uncertain = false;
	b->on_disk = true;
	free(s->lock_path);
	s->lock_path = NULL;
	wf_ed_mark_saved(&b->ed);
	draft_forget(s, b);
	s->saved_since = true;
	say_m(s,
	        r->backup_made              ? MSG_ED_SAVED_BACKUP
	                : r->backup_skipped ? MSG_ED_SAVED_NO_BACKUP
	                                    : MSG_ED_SAVED,
	        MA(WF_ARG_N(bytes)));
}

typedef enum
{
	SAVE_DONE,
	SAVE_ASKING,
	SAVE_FAILED
} SaveOutcome;

static SaveOutcome do_save(WfEditSession *s)
{
	WfBuffer *b = wf_es_buffer(s);
	WfSaveResult r;
	WfSaveOptions opt;
	char *text = NULL;
	size_t n = 0;
	WfStatus st;
	char name[128];

	if (!b) {
		return SAVE_FAILED;
	}
	if (b->save_uncertain) {
		fail_m(s, MSG_ED_SAVE_REOPEN, NULL, 0);
		return SAVE_FAILED;
	}
	if (b->ed.read_only) {
		fail_m(s, MSG_ED_SAVE_READONLY, NULL, 0);
		return SAVE_FAILED;
	}
	st = snapshot(b, &text, &n);
	if (WF_OK != st) {
		fail_m(s, MSG_ED_SAVE_FAILED, MA(WF_ARG_S(wf_status_name(st))));
		return SAVE_FAILED;
	}
	opt.backup = s->cfg.backup;
	leaf_text(b->path, name, sizeof name);
	if (b->on_disk) {
		st = s->io.save_over(s->io.ctx, b->path, &b->stamp, text, n,
		        b->ed.enc, &opt, &r);
	} else {
		st = s->io.save_new(s->io.ctx, b->path, text, n, b->ed.enc, &r);
	}
	free(text);
	if (WF_OK != st) {
		fail_m(s, MSG_ED_SAVE_FAILED, MA(WF_ARG_S(wf_status_name(st))));
		return SAVE_FAILED;
	}
	switch (r.verdict) {
	case WF_SV_OK: after_save(s, b, &r, n); return SAVE_DONE;
	case WF_SV_COMMITTED_UNVERIFIED: {
		WfBuf previous = {0};
		b->save_uncertain = true;
		b->on_disk = true;
		b->stamp.valid = false;
		s->saved_since = true;
		(void)wf_sanitize_utf16(
		        r.kept_original, wcslen(r.kept_original), &previous);
		fail_m(s, MSG_ED_SAVE_UNVERIFIED,
		        MA(WF_ARG_S(wf_buf_cstr(&previous))));
		wf_buf_free(&previous);
		return SAVE_FAILED;
	}
	case WF_SV_CONFLICT:
		s->pk = PR_CONFLICT;
		wf_choice_begin(&s->choice, "rac", 2);
		(void)wf_msgf(s->label, sizeof s->label, MSG_ED_ASK_CONFLICT,
		        &WF_ARG_S(name), 1);
		return SAVE_ASKING;
	case WF_SV_GONE:
		s->pk = PR_GONE;
		wf_choice_begin(&s->choice, "sc", 1);
		(void)wf_msgf(s->label, sizeof s->label, MSG_ED_ASK_GONE,
		        &WF_ARG_S(name), 1);
		return SAVE_ASKING;
	case WF_SV_EXISTS:
		fail_m(s, MSG_ED_SAVE_EXISTS, MA(WF_ARG_S(name)));
		return SAVE_FAILED;
	case WF_SV_UNSAFE:
		fail_m(s, MSG_ED_SAVE_UNSAFE,
		        MA(WF_ARG_S(r.reason ? wf_msg_reason(r.reason)
		                             : wf_msg(MSG_ED_UNSAFE))));
		return SAVE_FAILED;
	case WF_SV_ENCODE:
		fail_m(s, MSG_ED_SAVE_ENCODE, MA(WF_ARG_N(r.bad_offset)));
		return SAVE_FAILED;
	case WF_SV_IO: {
		if (r.kept_temp[0]) {
			recover_begin(s, &r);
		} else if (WF_LOCK_IN_USE == wf_lock_classify(r.error)) {
			char code[16];

			snprintf(code, sizeof code, "%lu",
			        (unsigned long)r.error);
			free(s->lock_path);
			s->lock_path = wf_path_dup(b->path);
			s->pk = PR_LOCKED;
			wf_choice_begin(&s->choice, "lc", 1);
			(void)wf_msgf(s->label, sizeof s->label,
			        MSG_ED_ASK_LOCKED, &WF_ARG_S(code), 1);
		} else {
			fail_m(s, MSG_ED_SAVE_IO,
			        MA(WF_ARG_N(r.error),
			                WF_ARG_S(r.reason
			                                ? wf_msg_reason(
			                                          r.reason)
			                                : wf_msg(MSG_REASON_WRITING_FAILED))));
		}
		return SAVE_FAILED;
	}
	}
	return SAVE_FAILED;
}

/* Reads the file again, dropping the edits. */
static void reload_current(WfEditSession *s)
{
	WfBuffer *b = wf_es_buffer(s);
	WfLoaded l;
	WfStatus st;
	size_t line;

	if (!b) {
		return;
	}
	line = b->ed.cur.line;
	st = s->io.load(s->io.ctx, b->path, s->cfg.ed.max_bytes,
	        s->cfg.fallback_codepage, &b->ed.enc, &l);
	if (WF_OK != st || WF_FL_OK != l.verdict) {
		fail_m(s, MSG_ED_RELOAD_FAILED,
		        MA(WF_ARG_S(WF_OK != st ? wf_status_name(st)
		                        : l.reason
		                        ? wf_msg_reason(l.reason)
		                        : wf_msg(MSG_ED_FILE_UNUSABLE))));
		if (WF_OK == st) {
			wf_efile_loaded_free(&l);
		}
		return;
	}
	st = wf_ed_set_text(&b->ed, l.utf8, l.utf8_n, l.enc);
	if (WF_OK != st) {
		/* the buffer keeps what it had, and the stamp of what it had */
		fail_m(s, MSG_ED_RELOAD_FAILED, MA(WF_ARG_S(status_text(st))));
		wf_efile_loaded_free(&l);
		return;
	}
	b->stamp = l.stamp;
	b->external_noted = false;
	b->save_uncertain = false;
	b->on_disk = true;
	b->ed.read_only = 0 != (l.stamp.attributes & FILE_ATTRIBUTE_READONLY);
	wf_syn_invalidate(&b->syn, 0);
	wf_efile_loaded_free(&l);
	wf_ed_goto(&b->ed, line + 1, 1);
	say_m(s, MSG_ED_RELOADED, NULL, 0);
}

/* "name" or a full path -> an absolute path, or NULL with *why set. */
static wchar_t *resolve_path(
        WfEditSession *s, const char *text, const char **why)
{
	WfBuffer *b = wf_es_buffer(s);
	wchar_t *wide, *out = NULL;
	size_t units, i, start = 0, comps = 0;
	bool absolute, unc;
	size_t used = 0;

	*why = NULL;
	if (!*text) {
		*why = wf_msg(MSG_ED_PATH_NO_NAME);
		return NULL;
	}
	wide = wf_malloc_array((strlen(text) + 1), sizeof *wide);
	if (!wide) {
		return NULL;
	}
	if (wf_utf8_to_utf16(text, strlen(text), wide, strlen(text) + 1,
	            &used) != WF_OK) {
		free(wide);
		*why = wf_msg(MSG_ED_PATH_BAD_TEXT);
		return NULL;
	}
	wide[used] = L'\0';
	units = used;
	/* drop surrounding quotes, as a pasted path often has them */
	if (units >= 2 && L'"' == wide[0] && L'"' == wide[units - 1]) {
		memmove(wide, wide + 1, (units - 2) * sizeof *wide);
		units -= 2;
		wide[units] = L'\0';
	}
	absolute = units >= 3 && L':' == wide[1] && L'\\' == wide[2] &&
	        ((wide[0] >= L'A' && wide[0] <= L'Z') ||
	                (wide[0] >= L'a' && wide[0] <= L'z'));
	unc = units >= 3 && L'\\' == wide[0] && L'\\' == wide[1];
	if (wcschr(wide, L'/')) {
		free(wide);
		*why = wf_msg(MSG_ED_PATH_USE_BACKSLASH);
		return NULL;
	}
	if (absolute) {
		start = 3;
	} else if (unc) {
		start = 2;
	}
	/* every component must be an acceptable name */
	for (i = start; i <= units; i++) {
		if (i == units || L'\\' == wide[i]) {
			size_t from = start;
			WfNameResult r;

			if (i == from && i != units) {
				free(wide);
				*why = wf_msg(MSG_ED_PATH_EMPTY_PART);
				return NULL;
			}
			if (i > from) {
				r = wf_name_validate(
				        wide + from, i - from, 255);
				if (WF_NAME_OK != r ||
				        (i - from == 1 && L'.' == wide[from]) ||
				        (i - from == 2 && L'.' == wide[from] &&
				                L'.' == wide[from + 1])) {
					free(wide);
					*why = WF_NAME_OK != r
					        ? wf_msg_reason(
					                  wf_name_reason(r))
					        : wf_msg(MSG_ED_PATH_DOTS);
					return NULL;
				}
				comps++;
			}
			start = i + 1;
		}
	}
	if (!comps) {
		free(wide);
		*why = wf_msg(MSG_ED_PATH_NO_NAME);
		return NULL;
	}
	if (absolute || unc) {
		out = wide;
	} else {
		const wchar_t *dir = NULL;
		wchar_t *folder = NULL;

		if (b && b->path) {
			size_t n = wf_path_parent_len(b->path);

			folder = n ? wf_path_dup_n(b->path, n) : NULL;
			dir = folder;
		}
		if (!dir) {
			dir = s->base_dir;
		}
		if (!dir) {
			free(wide);
			free(folder);
			*why = wf_msg(MSG_ED_PATH_NO_FOLDER);
			return NULL;
		}
		if (WF_OK != wf_path_join(dir, wide, &out)) {
			out = NULL;
		}
		free(folder);
		free(wide);
	}
	return out;
}

static void save_as_to(WfEditSession *s, const char *text)
{
	WfBuffer *b = wf_es_buffer(s);
	const char *why = NULL;
	wchar_t *path = resolve_path(s, text, &why);
	WfSaveResult r;
	char *content = NULL;
	size_t n = 0;
	WfStatus st;

	if (!path) {
		fail_m(s, MSG_ED_SAVE_AS_FAILED,
		        MA(WF_ARG_S(why ? why : wf_msg(MSG_NAV_NOMEM))));
		return;
	}
	/* Same-name writeout uses the original baseline and all ordinary save
	 * checks. Other existing paths still go through exclusive save_new. */
	if (b->path && !wcscmp(path, b->path) && b->on_disk) {
		free(path);
		(void)do_save(s);
		return;
	}
	st = snapshot(b, &content, &n);
	if (WF_OK != st) {
		free(path);
		fail_m(s, MSG_ED_SAVE_FAILED, MA(WF_ARG_S(wf_status_name(st))));
		return;
	}
	st = s->io.save_new(s->io.ctx, path, content, n, b->ed.enc, &r);
	free(content);
	if (WF_OK != st) {
		free(path);
		fail_m(s, MSG_ED_SAVE_FAILED, MA(WF_ARG_S(wf_status_name(st))));
		return;
	}
	if (WF_SV_EXISTS == r.verdict) {
		free(path);
		fail_m(s, MSG_ED_SAVE_AS_TAKEN, NULL, 0);
		return;
	}
	if (WF_SV_ENCODE == r.verdict) {
		free(path);
		fail_m(s, MSG_ED_SAVE_AS_ENCODE, NULL, 0);
		return;
	}
	if (WF_SV_COMMITTED_UNVERIFIED == r.verdict) {
		free(b->path);
		b->path = path;
		b->on_disk = true;
		b->save_uncertain = true;
		b->stamp.valid = false;
		s->saved_since = true;
		fail_m(s, MSG_ED_SAVE_UNVERIFIED, MA(WF_ARG_S("")));
		return;
	}
	if (WF_SV_OK != r.verdict) {
		free(path);
		fail_m(s, MSG_ED_SAVE_AS_IO,
		        MA(WF_ARG_N(r.error),
		                WF_ARG_S(r.reason
		                                ? wf_msg_reason(r.reason)
		                                : wf_msg(MSG_REASON_WRITING_FAILED))));
		return;
	}
	free(b->path);
	b->path = path;
	b->ed.read_only = false;
	b->lang = s->cfg.syntax ? wf_syn_lang_for_name(leaf_ptr(path))
	                        : WF_LANG_TEXT;
	wf_syn_free(&b->syn);
	wf_syn_init(&b->syn, b->lang);
	wf_es_syntax_assign(s, b, path);
	wf_es_git_assign(s, b, path);
	after_save(s, b, &r, n);
}

static void open_from_prompt(WfEditSession *s, const char *text)
{
	const char *why = NULL;
	wchar_t *path = resolve_path(s, text, &why);
	char reason[160];
	WfLoaded l;

	if (!path) {
		fail_m(s, MSG_ED_OPEN_FAILED,
		        MA(WF_ARG_S(why ? why : wf_msg(MSG_NAV_NOMEM))));
		return;
	}
	if (s->io.load(s->io.ctx, path, s->cfg.ed.max_bytes,
	            s->cfg.fallback_codepage, NULL, &l) == WF_OK) {
		bool missing = WF_FL_MISSING == l.verdict;

		wf_efile_loaded_free(&l);
		if (missing) {
			if (create_buffer(s, path)) {
				say_m(s, MSG_ED_NEW_FILE, NULL, 0);
			} else {
				fail_m(s, MSG_ERROR_OUT_OF_MEMORY, NULL, 0);
			}
			free(path);
			return;
		}
	}
	switch (wf_es_open(s, path, reason, sizeof reason)) {
	case WF_OPEN_OK:     break;
	case WF_OPEN_ASKING: break;
	case WF_OPEN_REFUSED:
		fail_m(s, MSG_ED_CANNOT_OPEN, MA(WF_ARG_S(reason)));
		break;
	}
	free(path);
}

static void insert_file(WfEditSession *s, const char *text)
{
	WfBuffer *b = wf_es_buffer(s);
	const char *why = NULL;
	wchar_t *path = resolve_path(s, text, &why);
	WfLoaded l;
	WfStatus st;

	if (!path) {
		fail_m(s, MSG_ED_INSERT_FAILED,
		        MA(WF_ARG_S(why ? why : wf_msg(MSG_NAV_NOMEM))));
		return;
	}
	st = s->io.load(s->io.ctx, path, s->cfg.ed.max_bytes,
	        s->cfg.fallback_codepage, NULL, &l);
	free(path);
	if (WF_OK != st) {
		fail_m(s, MSG_ED_INSERT_FAILED,
		        MA(WF_ARG_S(wf_status_name(st))));
		return;
	}
	if (WF_FL_OK != l.verdict) {
		fail_m(s, MSG_ED_CANNOT_INSERT,
		        MA(WF_ARG_S(l.reason ? wf_msg_reason(l.reason)
		                             : wf_msg(MSG_ED_FILE_UNUSABLE))));
		wf_efile_loaded_free(&l);
		return;
	}
	st = wf_ed_insert_text(&b->ed, l.utf8, l.utf8_n);
	if (WF_OK == st) {
		say_m(s, MSG_ED_INSERTED, MA(WF_ARG_N(l.utf8_n)));
	} else {
		fail_m(s, MSG_ED_CANNOT_INSERT, MA(WF_ARG_S(status_text(st))));
	}
	wf_efile_loaded_free(&l);
}

static WfSearch search_of(const WfEditSession *s)
{
	WfSearch q;

	q.pat = s->find;
	q.pat_n = s->find_n;
	q.match_case = s->f_case;
	q.whole_word = s->f_word;
	return q;
}

#define WF_ES_COUNT_CAP 10000u

/* How many matches there are (to WF_ES_COUNT_CAP) and which one starts at the
 * cursor's anchor. The positions are kept with the buffer while neither the text
 * nor the pattern changes; without memory for them it is counted the long way. */
static size_t count_matches(WfBuffer *b, const WfSearch *q, size_t *index)
{
	WfPos at = b->ed.anchor;
	size_t lo = 0, hi;

	if (!b->found.valid || b->found.gen != b->ed.text.gen ||
	        b->found.match_case != q->match_case ||
	        b->found.whole_word != q->whole_word ||
	        b->found.pat_n != q->pat_n ||
	        memcmp(b->found.pat, q->pat, q->pat_n)) {
		char *pat = malloc(q->pat_n + 1);

		b->found.valid = false;
		if (!b->found.at) {
			b->found.at =
			        malloc(WF_ES_COUNT_CAP * sizeof *b->found.at);
		}
		if (!pat || !b->found.at) {
			free(pat);
			return wf_es_count(
			        &b->ed.text, q, WF_ES_COUNT_CAP, at, index);
		}
		memcpy(pat, q->pat, q->pat_n);
		pat[q->pat_n] = '\0';
		free(b->found.pat);
		b->found.pat = pat;
		b->found.pat_n = q->pat_n;
		b->found.match_case = q->match_case;
		b->found.whole_word = q->whole_word;
		b->found.gen = b->ed.text.gen;
		b->found.n = wf_es_positions(
		        &b->ed.text, q, WF_ES_COUNT_CAP, b->found.at);
		b->found.valid = true;
	}
	*index = 0;
	hi = b->found.n;
	while (lo < hi) { /* the first position that is not before `at` */
		size_t mid = lo + (hi - lo) / 2;

		if (wf_pos_cmp(b->found.at[mid], at) < 0) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	if (lo < b->found.n && 0 == wf_pos_cmp(b->found.at[lo], at)) {
		*index = lo + 1;
	}
	return b->found.n;
}

static void do_find(WfEditSession *s, bool forward)
{
	WfBuffer *b = wf_es_buffer(s);
	WfSearch q = search_of(s);
	bool wrapped = false;

	if (!wf_es_ok(&q)) {
		fail_m(s, MSG_ED_NOTHING_TO_SEARCH, NULL, 0);
		return;
	}
	bool modal =
	        s->cfg.keys && wf_keymap_modal(s->cfg.keys) && !s->vim_insert;
	WfEditor view = b->ed;
	if (modal && forward) {
		/* Vim starts beyond the cursor. Search on a view so failure cannot
		 * move the real cursor or disturb its selection. */
		const WfELine *line = &view.text.lines[view.cur.line];
		if (view.cur.col < line->len) {
			view.cur.col = wf_et_col_next(line, view.cur.col);
		} else if (view.cur.line + 1 < view.text.n) {
			view.cur.line++;
			view.cur.col = 0;
		}
		wf_ed_clear_selection(&view);
	}
	if (wf_ed_find(modal ? &view : &b->ed, &q, forward, &wrapped)) {
		if (modal) {
			b->ed.cur = view.cur;
			b->ed.anchor = view.anchor;
			b->ed.has_anchor = true;
		}
		if (s->cfg.keys && wf_keymap_modal(s->cfg.keys) &&
		        !s->vim_insert) {
			b->ed.cur = b->ed.anchor;
			wf_ed_clear_selection(&b->ed);
		}
		size_t index = 0, total = count_matches(b, &q, &index);

		s->hl = true;
		say_m(s, MSG_ED_MATCH,
		        MA(WF_ARG_N(index), WF_ARG_N(total),
		                WF_ARG_S(total >= WF_ES_COUNT_CAP ? "+" : ""),
		                WF_ARG_S(wrapped ? wf_msg(MSG_ED_WRAPPED)
		                                 : "")));
	} else {
		s->hl = false;
		fail_m(s, MSG_ED_NOT_FOUND, MA(WF_ARG_S(s->find)));
	}
}

static void replace_next(WfEditSession *s);

static void replace_finish(WfEditSession *s)
{
	prompt_end(s);
	say_m(s,
	        wf_msg_count(MSG_ED_REPLACED_ONE, MSG_ED_REPLACED_OTHER,
	                (long long)s->repl_count),
	        MA(WF_ARG_N(s->repl_count)));
}

static void replace_next(WfEditSession *s)
{
	WfBuffer *b = wf_es_buffer(s);
	WfSearch q = search_of(s);
	WfMatch m;

	if (!wf_es_find(&b->ed.text, &q, b->ed.cur, true, false, &m, NULL)) {
		replace_finish(s);
		return;
	}
	b->ed.anchor = m.start;
	b->ed.cur = m.end;
	b->ed.has_anchor = true;
	s->hl = true;
	s->pk = PR_REPL_ASK;
	{
		char shown[48];

		wf_utf8_elide(shown, sizeof shown, s->repl, 40);
		(void)wf_msgf(s->label, sizeof s->label, MSG_ED_ASK_REPLACE,
		        &WF_ARG_S(shown), 1);
	}
}

static void prefill_from_selection(WfEditSession *s, char *out, size_t cap)
{
	WfBuffer *b = wf_es_buffer(s);
	WfPos a, z;

	out[0] = '\0';
	if (!wf_ed_has_selection(&b->ed)) {
		snprintf(out, cap, "%s", s->find);
		return;
	}
	wf_ed_selection(&b->ed, &a, &z);
	if (a.line == z.line && z.col - a.col < cap) {
		memcpy(out, b->ed.text.lines[a.line].s + a.col, z.col - a.col);
		out[z.col - a.col] = '\0';
	}
}

static void begin_find(WfEditSession *s, PromptKind kind)
{
	char init[WF_ES_INPUT_MAX];

	prefill_from_selection(s, init, sizeof init);
	prompt_begin(s, kind,
	        wf_msg(PR_FIND_BACK == kind       ? MSG_ED_PROMPT_FIND_BACK
	                        : PR_FIND == kind ? MSG_ED_PROMPT_FIND
	                                          : MSG_ED_PROMPT_REPLACE_FIND),
	        init);
}

static size_t page_of(const WfEditSession *s)
{
	return s->rows > 4 ? (size_t)s->rows - 3 : 1;
}

static const WfMove MOVES[] = {WF_MV_LEFT, WF_MV_RIGHT, WF_MV_UP, WF_MV_DOWN,
        WF_MV_LINE_START, WF_MV_LINE_END, WF_MV_PAGE_UP, WF_MV_PAGE_DOWN,
        WF_MV_DOC_START, WF_MV_DOC_END, WF_MV_WORD_LEFT, WF_MV_WORD_RIGHT};

/* Hands the editor's clipboard to the system. False when the system refused:
 * the text is then kept inside the editor and pasting must not replace it by
 * whatever the system still holds (clip_local_only), until a later copy or
 * cut succeeds. */
static bool clip_out(WfEditSession *s, WfBuffer *b)
{
	if (!s->io.clip_set || !b->ed.clip_n) {
		return true;
	}
	if (WF_OK == s->io.clip_set(s->io.ctx, b->ed.clip, b->ed.clip_n)) {
		s->clip_local_only = false;
		return true;
	}
	s->clip_local_only = true;
	return false;
}

/* Vim's p: after the cursor, or below the line when whole lines were cut. */
static WfStatus paste_after(WfEditor *e)
{
	size_t n;
	char *tmp;
	WfStatus st;

	if (e->read_only) {
		return WF_E_ACCESS;
	}
	if (!e->clip_n) {
		return wf_ed_paste(e);
	}
	if (!e->clip_line) {
		if (e->cur.col < e->text.lines[e->cur.line].len) {
			wf_ed_move(e, WF_MV_RIGHT, false, 0);
		}
		return wf_ed_paste(e);
	}
	if (e->cur.line + 1 < e->text.n) {
		e->cur.line++; /* the lines go above the next one */
		e->cur.col = 0;
		e->want_valid = false;
		return wf_ed_paste(e);
	}
	/* the last line: a break, then the lines without their own last break */
	n = e->clip_n;
	if (n && '\n' == e->clip[n - 1]) {
		n--;
	}
	if (n && '\r' == e->clip[n - 1]) {
		n--;
	}
	tmp = malloc(n + 1);
	if (!tmp) {
		return WF_E_NOMEM;
	}
	tmp[0] = '\n';
	memcpy(tmp + 1, e->clip, n);
	wf_ed_move(e, WF_MV_LINE_END, false, 0);
	st = wf_ed_insert_text(e, tmp, n + 1);
	free(tmp);
	return st;
}

/* Whether two clip texts are the same text when a line ending written CRLF in
 * one and LF in the other counts as one: the system clipboard has CRLF where the
 * editor's own clip (which the line attribute belongs to) has LF. */
static bool same_clip_text(const char *a, size_t an, const char *b, size_t bn)
{
	size_t i = 0, j = 0;

	for (;;) {
		if (i + 1 < an && '\r' == a[i] && '\n' == a[i + 1]) {
			i++;
		}
		if (j + 1 < bn && '\r' == b[j] && '\n' == b[j + 1]) {
			j++;
		}
		if (i >= an || j >= bn) {
			return i >= an && j >= bn;
		}
		if (a[i++] != b[j++]) {
			return false;
		}
	}
}

void wf_es_fail_status(WfEditSession *s, WfStatus st)
{
	if (st == WF_E_CANCELLED || st == WF_E_CHANGED) {
		s->input_notice = st;
	}
	fail_m(s, MSG_ED_CANNOT_DO, MA(WF_ARG_S(status_text(st))));
}

static void do_paste(WfEditSession *s, WfBuffer *b, bool after)
{
	char *os = NULL;
	size_t os_n = 0;
	WfStatus st;

	if (!s->clip_local_only && s->io.clip_get &&
	        WF_OK == s->io.clip_get(s->io.ctx, &os, &os_n) && os_n) {
		/* the line clip survives only while the system clipboard still
		 * holds what the editor put there (up to how it writes a line
		 * ending): its text, its ending and its line attribute are kept */
		if (!same_clip_text(b->ed.clip, b->ed.clip_n, os, os_n)) {
			st = wf_ed_set_clip(&b->ed, os, os_n, false);
			if (WF_OK != st) {
				/* the old clip is still there: it is not what was
				 * asked to be pasted */
				free(os);
				fail_m(s, MSG_ED_CANNOT_PASTE,
				        MA(WF_ARG_S(status_text(st))));
				return;
			}
		}
	}
	free(os);
	st = after ? paste_after(&b->ed) : wf_ed_paste(&b->ed);
	if (WF_OK != st) {
		fail_m(s, MSG_ED_CANNOT_PASTE, MA(WF_ARG_S(status_text(st))));
	} else if (b->ed.msg[0]) {
		say(s, "%s", b->ed.msg);
	}
}

static WfEsResult try_close(WfEditSession *s)
{
	WfBuffer *b = wf_es_buffer(s);
	char name[128];

	if (!b) {
		return WF_ES_CLOSED;
	}
	if (!wf_ed_modified(&b->ed)) {
		return close_current(s);
	}
	leaf_text(b->path, name, sizeof name);
	s->pk = PR_QUIT;
	wf_choice_begin(&s->choice, "sdc", 2); /* Enter cancels until moved */
	(void)wf_msgf(
	        s->label, sizeof s->label, MSG_ED_ASK_QUIT, &WF_ARG_S(name), 1);
	return WF_ES_REDRAW;
}

static WfEsResult exec(WfEditSession *s, WfEdAction a);

static bool nano_profile(const WfEditSession *s)
{
	if (!s->cfg.keys ||
	        wf_keymap_profile(s->cfg.keys) == WF_PROFILE_LEGACY) {
		return s->cfg.keymap == WF_KM_NANO;
	}
	return !wf_keymap_modal(s->cfg.keys) &&
	        wf_keymap_base(s->cfg.keys) == WF_BASE_NANO;
}

static WfStatus nano_capture(WfEditSession *s, WfBuffer *b, bool cut)
{
	WfEditor *ed = &b->ed;
	WfPos from, to;
	bool selected = wf_ed_has_selection(ed);
	char *part = NULL, *clip = NULL, *joined = NULL;
	size_t n = 0,
	       prefix = cut && !selected && s->nano_cut.chain &&
	                s->nano_cut.buffer == b
	        ? s->nano_cut.n
	        : 0;
	if (cut && ed->read_only) {
		return WF_E_ACCESS;
	}
	if (selected) {
		wf_ed_selection(ed, &from, &to);
	} else {
		from = (WfPos){ed->cur.line, 0};
		to = ed->cur.line + 1 < ed->text.n
		        ? (WfPos){ed->cur.line + 1, 0}
		        : (WfPos){ed->cur.line,
		                  ed->text.lines[ed->cur.line].len};
	}
	if (!wf_pos_cmp(from, to)) {
		return WF_OK;
	}
	WfStatus st = wf_et_read(&ed->text, from, to, &part, &n);
	if (WF_OK != st) {
		return st;
	}
	if (n > SIZE_MAX - prefix ||
	        (ed->opt.max_bytes &&
	                (prefix > ed->opt.max_bytes ||
	                        n > ed->opt.max_bytes - prefix))) {
		st = WF_E_RANGE;
		goto end;
	}
	joined = wf_et_malloc(prefix + n);
	clip = wf_et_malloc(prefix + n);
	if (!joined || !clip) {
		st = WF_E_NOMEM;
		goto end;
	}
	if (prefix) {
		memcpy(joined, s->nano_cut.text, prefix);
	}
	memcpy(joined + prefix, part, n);
	memcpy(clip, joined, prefix + n);
	if (cut) {
		st = wf_ed_replace_range(ed, from, to, NULL, 0);
		if (WF_OK != st) {
			goto end;
		}
	} else {
		wf_ed_clear_selection(ed);
	}
	free(s->nano_cut.text);
	s->nano_cut.text = joined;
	joined = NULL;
	s->nano_cut.n = prefix + n;
	s->nano_cut.chain = cut && !selected;
	s->nano_cut.buffer = b;
	free(ed->clip);
	ed->clip = clip;
	clip = NULL;
	ed->clip_n = prefix + n;
	ed->clip_line = false;
end:
	free(part);
	free(joined);
	free(clip);
	return st;
}

void wf_es_clip_out(WfEditSession *s)
{
	WfBuffer *b = wf_es_buffer(s);

	if (b) {
		(void)clip_out(s, b);
	}
}

WfEsResult wf_es_run(WfEditSession *s, WfEdCmd cmd, bool extend)
{
	return exec(s, (WfEdAction){cmd, extend});
}

#define WORD_SLICE_BYTES (64u << 10)

bool wf_es_motion_pending(const WfEditSession *s)
{
	return s &&
	        (s->motion.active || s->vim.motion.active ||
	                s->vim.playback.ev || s->input_n);
}

bool wf_es_input_ready(const WfEditSession *s)
{
	return s && s->input_n < WF_ARRAY_LEN(s->input);
}

size_t wf_es_take_closed_input(WfEditSession *s, WfEvent *events, size_t cap)
{
	if (!s || s->n) {
		return 0;
	}
	size_t n = s->input_n < cap ? s->input_n : cap;
	memcpy(events, s->input, n * sizeof *events);
	s->input_n -= n;
	memmove(s->input, s->input + n, s->input_n * sizeof *s->input);
	return n;
}

static bool word_slice(WfEditSession *s)
{
	WfBuffer *b = wf_es_buffer(s);

	if (!s->motion.active) {
		return false;
	}
	if (b != s->motion.buffer || !b || b->ed.text.gen != s->motion.gen ||
	        wf_pos_cmp(b->ed.cur, s->motion.from)) {
		/* A reload, buffer switch or direct model edit supersedes the scan. */
		s->motion.active = false;
		wf_vim_stop(s);
		wf_es_fail_status(s, WF_E_CHANGED);
		return true;
	}
	if (!wf_ed_word_scan_step(&b->ed.text.lines[s->motion.from.line],
	            &s->motion.scan, WORD_SLICE_BYTES)) {
		return false;
	}
	WfPos to = {s->motion.from.line, s->motion.scan.col};
	if (s->motion.deleting) {
		WfStatus st = wf_ed_replace_range(
		        &b->ed, s->motion.from, to, NULL, 0);
		if (WF_OK != st) {
			wf_es_fail_status(s, st);
		}
	} else {
		wf_ed_move_to(&b->ed, to, s->motion.extend);
	}
	s->motion.active = false;
	return true;
}

static WfEsResult exec(WfEditSession *s, WfEdAction a)
{
	WfBuffer *b = wf_es_buffer(s);
	WfStatus st = WF_OK;
	bool clip_bad = false;

	/* The event boundary orders input behind unfinished scans and playback. */
	if (!b) {
		return WF_ES_CLOSED;
	}
	b->ed.msg[0] = '\0';
	b->ed.undo.capacity_refused = false;
	if (WF_CMD_VIM_INSERT == a.cmd || WF_CMD_VIM_APPEND == a.cmd ||
	        WF_CMD_VIM_OPEN_BELOW == a.cmd ||
	        WF_CMD_VIM_OPEN_ABOVE == a.cmd || WF_CMD_VIM_NORMAL == a.cmd ||
	        WF_CMD_NEXT_BUFFER == a.cmd || WF_CMD_PREV_BUFFER == a.cmd) {
		s->vim.visual = s->vim.vline =
		        false; /* typing is not Visual mode */
	}
	switch (a.cmd) {
	case WF_CMD_NONE:      return WF_ES_NONE;
	case WF_CMD_LEFT:
	case WF_CMD_RIGHT:
	case WF_CMD_UP:
	case WF_CMD_DOWN:
	case WF_CMD_HOME:
	case WF_CMD_END:
	case WF_CMD_PGUP:
	case WF_CMD_PGDN:
	case WF_CMD_DOC_START:
	case WF_CMD_DOC_END:
	case WF_CMD_WORD_LEFT:
	case WF_CMD_WORD_RIGHT:
		if (s->vim_insert) {
			wf_un_group_end(&b->ed.undo);
		}
		if (WF_CMD_HOME == a.cmd && nano_profile(s)) {
			wf_ed_move_to(
			        &b->ed, (WfPos){b->ed.cur.line, 0}, a.extend);
			return WF_ES_REDRAW;
		}
		if ((WF_CMD_WORD_LEFT == a.cmd || WF_CMD_WORD_RIGHT == a.cmd) &&
		        b->ed.text.lines[b->ed.cur.line].len >
		                WORD_SLICE_BYTES &&
		        ((WF_CMD_WORD_LEFT == a.cmd && b->ed.cur.col) ||
		                (WF_CMD_WORD_RIGHT == a.cmd &&
		                        b->ed.cur.col <
		                                b->ed.text.lines[b->ed.cur.line]
		                                        .len))) {
			s->motion.buffer = b;
			s->motion.from = b->ed.cur;
			s->motion.gen = b->ed.text.gen;
			s->motion.extend = a.extend;
			s->motion.deleting = false;
			s->motion.active = true;
			wf_ed_word_scan_begin(&s->motion.scan, b->ed.cur.col,
			        WF_CMD_WORD_RIGHT == a.cmd);
			return word_slice(s) ? WF_ES_REDRAW : WF_ES_NONE;
		}
		wf_ed_move(&b->ed, MOVES[a.cmd - WF_CMD_LEFT], a.extend,
		        page_of(s));
		return WF_ES_REDRAW;
	case WF_CMD_NEWLINE:     st = wf_ed_newline(&b->ed); break;
	case WF_CMD_BACKSPACE:   st = wf_ed_backspace(&b->ed); break;
	case WF_CMD_DELETE:      st = wf_ed_delete(&b->ed); break;
	case WF_CMD_TAB:         st = wf_ed_tab(&b->ed, false); break;
	case WF_CMD_BACKTAB:     st = wf_ed_tab(&b->ed, true); break;
	case WF_CMD_DELETE_LINE: st = wf_ed_delete_line(&b->ed); break;
	case WF_CMD_CUT:
		if (nano_profile(s)) {
			s->nano_cut.action = true;
			st = nano_capture(s, b, true);
			break;
		}
		st = wf_ed_cut(&b->ed);
		if (WF_OK == st) {
			clip_bad = !clip_out(s, b);
		}
		break;
	case WF_CMD_COPY:
		if (nano_profile(s)) {
			st = nano_capture(s, b, false);
			break;
		}
		st = wf_ed_copy(&b->ed);
		if (WF_OK == st) {
			clip_bad = !clip_out(s, b);
		}
		break;
	case WF_CMD_PASTE:
		if (nano_profile(s)) {
			if (!s->nano_cut.n) {
				say_m(s, MSG_ED_CLIPBOARD_EMPTY, NULL, 0);
				return WF_ES_REDRAW;
			}
			st = wf_ed_replace_range(&b->ed, b->ed.cur, b->ed.cur,
			        s->nano_cut.text, s->nano_cut.n);
			break;
		}
		do_paste(s, b, false);
		return WF_ES_REDRAW;
	case WF_CMD_SYSTEM_COPY: {
		WfEditor copy = b->ed;
		copy.clip = NULL;
		copy.clip_n = 0;
		st = wf_ed_copy(&copy);
		if (WF_OK == st && s->io.clip_set) {
			st = s->io.clip_set(s->io.ctx, copy.clip, copy.clip_n);
		}
		free(copy.clip);
		break;
	}
	case WF_CMD_SYSTEM_PASTE: {
		char *text = NULL;
		size_t n = 0;
		st = s->io.clip_get ? s->io.clip_get(s->io.ctx, &text, &n)
		                    : WF_E_NOENT;
		if (WF_OK == st) {
			st = wf_ed_insert_text(&b->ed, text, n);
		}
		free(text);
		break;
	}
	case WF_CMD_DELETE_WORD_RIGHT: {
		if (b->ed.text.lines[b->ed.cur.line].len > WORD_SLICE_BYTES) {
			s->motion.buffer = b;
			s->motion.from = b->ed.cur;
			s->motion.gen = b->ed.text.gen;
			s->motion.extend = false;
			s->motion.deleting = true;
			s->motion.active = true;
			wf_ed_word_scan_begin(
			        &s->motion.scan, b->ed.cur.col, true);
			return WF_ES_REDRAW;
		}
		WfEditor view = b->ed;
		wf_ed_clear_selection(&view);
		wf_ed_move(&view, WF_MV_WORD_RIGHT, false, 0);
		st = wf_ed_replace_range(&b->ed, b->ed.cur, view.cur, NULL, 0);
		break;
	}
	case WF_CMD_SELECT_ALL: wf_ed_select_all(&b->ed); return WF_ES_REDRAW;
	case WF_CMD_MARK:       wf_ed_toggle_mark(&b->ed); break;
	case WF_CMD_UNDO:
		s->vim.dot_skip = true;
		st = wf_ed_undo(&b->ed);
		break;
	case WF_CMD_REDO:
		s->vim.dot_skip = true;
		st = wf_ed_redo(&b->ed);
		break;
	case WF_CMD_COMMENT: {
		const char *p = wf_syn_comment_prefix(b->lang);

		if (!p) {
			fail_m(s, MSG_ED_NO_COMMENTS,
			        MA(WF_ARG_S(wf_syn_lang_name(b->lang))));
			return WF_ES_REDRAW;
		}
		st = wf_ed_toggle_comment(&b->ed, p);
		break;
	}
	case WF_CMD_COMPLETE: st = wf_ed_complete_word(&b->ed); break;
	case WF_CMD_SAVE:     {
		SaveOutcome o = do_save(s);

		if (SAVE_DONE == o && s->exit_after_save) {
			s->exit_after_save = false;
			return close_current(s);
		}
		if (SAVE_FAILED == o) {
			s->exit_after_save = false;
		}
		return WF_ES_REDRAW;
	}
	case WF_CMD_SAVE_AS: {
		char init[WF_ES_INPUT_MAX] = "";
		if (nano_profile(s) && b->path) {
			if (!WideCharToMultiByte(CP_UTF8, 0, b->path, -1, init,
			            sizeof init, NULL, NULL)) {
				wf_es_fail_status(s, WF_E_RANGE);
				return WF_ES_REDRAW;
			}
		}

		prompt_begin(
		        s, PR_SAVE_AS, wf_msg(MSG_ED_PROMPT_SAVE_AS), init);
		return WF_ES_REDRAW;
	}
	case WF_CMD_EXIT:      return try_close(s);
	case WF_CMD_FIND:      begin_find(s, PR_FIND); return WF_ES_REDRAW;
	case WF_CMD_FIND_BACK: begin_find(s, PR_FIND_BACK); return WF_ES_REDRAW;
	case WF_CMD_FIND_NEXT:
		if (!s->find_n) {
			begin_find(s, PR_FIND);
		} else {
			do_find(s, true);
		}
		return WF_ES_REDRAW;
	case WF_CMD_FIND_PREV:
		if (!s->find_n) {
			begin_find(s, PR_FIND_BACK);
		} else {
			do_find(s, false);
		}
		return WF_ES_REDRAW;
	case WF_CMD_REPLACE: begin_find(s, PR_REPL_FIND); return WF_ES_REDRAW;
	case WF_CMD_GOTO:
		prompt_begin(s, PR_GOTO, wf_msg(MSG_ED_PROMPT_GOTO), "");
		return WF_ES_REDRAW;
	case WF_CMD_INSERT_FILE:
		prompt_begin(s, PR_INSERT, wf_msg(MSG_ED_PROMPT_INSERT), "");
		return WF_ES_REDRAW;
	case WF_CMD_OPEN_FILE:
		prompt_begin(s, PR_OPEN, wf_msg(MSG_ED_PROMPT_OPEN), "");
		return WF_ES_REDRAW;
	case WF_CMD_NEXT_BUFFER:
		if (s->n > 1) {
			s->cur = (s->cur + 1) % s->n;
		}
		s->vim_insert = false;
		return WF_ES_REDRAW;
	case WF_CMD_PREV_BUFFER:
		if (s->n > 1) {
			s->cur = (s->cur + s->n - 1) % s->n;
		}
		s->vim_insert = false;
		return WF_ES_REDRAW;
	case WF_CMD_VIM_INSERT:
		wf_un_group_begin(&b->ed.undo);
		s->vim_insert = true;
		return WF_ES_REDRAW;
	case WF_CMD_VIM_APPEND:
		wf_un_group_begin(&b->ed.undo);
		if (b->ed.cur.col < b->ed.text.lines[b->ed.cur.line].len) {
			wf_ed_move(&b->ed, WF_MV_RIGHT, false, 0);
		}
		s->vim_insert = true;
		return WF_ES_REDRAW;
	case WF_CMD_VIM_OPEN_BELOW: {
		WfPos before = b->ed.cur;
		wf_un_group_begin(&b->ed.undo);
		wf_ed_move(&b->ed, WF_MV_LINE_END, false, 0);
		st = wf_ed_newline(&b->ed);
		if (WF_OK == st) {
			if (b->ed.undo.pos) {
				b->ed.undo.rec[b->ed.undo.pos - 1].before =
				        before;
			}
			s->vim_insert = true;
		}
		break;
	}
	case WF_CMD_VIM_OPEN_ABOVE: {
		WfPos before = b->ed.cur;
		wf_un_group_begin(&b->ed.undo);
		wf_ed_clear_selection(&b->ed);
		b->ed.cur.col = 0;
		b->ed.want_valid = false;
		st = wf_ed_insert_text(&b->ed, "\n", 1);
		if (WF_OK == st) {
			if (b->ed.undo.pos) {
				b->ed.undo.rec[b->ed.undo.pos - 1].before =
				        before;
			}
			wf_ed_move(&b->ed, WF_MV_UP, false, 0);
			s->vim_insert = true;
		}
		break;
	}
	case WF_CMD_VIM_NORMAL:
		s->vim_insert = false;
		if (b->ed.cur.col) {
			b->ed.cur.col = wf_et_col_prev(
			        &b->ed.text.lines[b->ed.cur.line],
			        b->ed.cur.col);
		}
		wf_un_group_end(&b->ed.undo);
		wf_ed_clear_selection(&b->ed);
		wf_vim_normal_cursor(s);
		if (b->ed.undo.pos) {
			b->ed.undo.rec[b->ed.undo.pos - 1].after = b->ed.cur;
		}
		return WF_ES_REDRAW;
	case WF_CMD_VIM_YANK_LINE:
	case WF_CMD_VIM_CUT_LINE:  return wf_vim_exec(s, a.cmd, s->vim_count);
	case WF_CMD_VIM_PASTE_BEFORE:
	case WF_CMD_VIM_PASTE_AFTER:
		return wf_vim_paste(s, WF_CMD_VIM_PASTE_AFTER == a.cmd);
	case WF_CMD_VIM_DELETE_CHAR: return wf_vim_exec(s, a.cmd, s->vim_count);
	case WF_CMD_VIM_OP_DELETE:
	case WF_CMD_VIM_OP_YANK:
	case WF_CMD_VIM_OP_CHANGE:
	case WF_CMD_VIM_DELETE_EOL:
	case WF_CMD_VIM_CHANGE_EOL:
	case WF_CMD_VIM_VISUAL:
	case WF_CMD_VIM_VISUAL_LINE:
	case WF_CMD_VIM_REGISTER:
	case WF_CMD_VIM_RECORD:
	case WF_CMD_VIM_PLAY:
	case WF_CMD_VIM_DOT:         return wf_vim_exec(s, a.cmd, s->vim_count);
	case WF_CMD_VIM_CMDLINE:
		prompt_begin(s, PR_EX, "", "");
		s->exhist_pos = s->exhist_n;
		return WF_ES_REDRAW;
	case WF_CMD_MD_PREVIEW: return wf_es_md_toggle(s);
	case WF_CMD_LAST:       return WF_ES_NONE;
	case WF_CMD_HELP:
		s->pk = PR_HELP;
		s->help_top = 0;
		return WF_ES_REDRAW;
	case WF_CMD_CANCEL:
		wf_ed_clear_selection(&b->ed);
		wf_vim_cancel(&s->vim);
		s->hl = false;
		return WF_ES_REDRAW;
	}
	if (WF_OK != st) {
		if (b->ed.undo.capacity_refused) {
			fail_m(s, MSG_ED_UNDO_BLOCK_FULL, NULL, 0);
		} else {
			fail_m(s,
			        WF_E_ACCESS == st ? MSG_ED_READONLY_EDIT
			                : WF_E_RANGE == st ? MSG_ED_SIZE_LIMIT
			                                   : MSG_ED_CANNOT_DO,
			        MA(WF_ARG_S(status_text(st))));
		}
	} else if (b->ed.msg[0]) {
		say(s, "%s", b->ed.msg);
	}
	if (WF_OK == st && (a.cmd == WF_CMD_UNDO || a.cmd == WF_CMD_REDO) &&
	        s->cfg.keys && wf_keymap_modal(s->cfg.keys) && !s->vim_insert) {
		wf_vim_normal_cursor(s);
	}
	if (clip_bad) {
		fail_m(s, MSG_ED_CLIP_FAILED, NULL, 0);
	}
	return WF_ES_REDRAW;
}

static bool is_letter(const WfEvent *ev, char lower)
{
	return WF_EV_CHAR == ev->type &&
	        (ev->ch == (uint32_t)lower || ev->ch == (uint32_t)(lower - 32));
}

static bool is_esc(const WfEvent *ev)
{
	return (WF_EV_KEY == ev->type && WF_KEY_ESC == ev->key) ||
	        (WF_EV_CTRL == ev->type && 'c' == ev->ch);
}

static void remember_ex(WfEditSession *s, const char *text)
{
	if (!*text ||
	        (s->exhist_n && !strcmp(s->exhist[s->exhist_n - 1], text))) {
		return;
	}
	if (WF_ES_HISTORY == s->exhist_n) {
		memmove(s->exhist[0], s->exhist[1],
		        (WF_ES_HISTORY - 1) * sizeof s->exhist[0]);
		s->exhist_n--;
	}
	snprintf(s->exhist[s->exhist_n++], sizeof s->exhist[0], "%s", text);
}

/* ":w", ":q", ":wq", ":q!", ":replace", ":buffers", ":bnext", ":bprev". The
 * same save, close and question as the keys use; nothing here runs a
 * program. */
static WfEsResult run_ex(WfEditSession *s, const char *text)
{
	WfEx ex;
	char list[160];
	size_t i, used = 0;

	remember_ex(s, text);
	if (!wf_ex_parse(text, strlen(text), WF_EXS_EDITOR, &ex)) {
		if (WF_EXE_EMPTY != ex.error) {
			wf_ex_error_text(&ex, s->msg, sizeof s->msg);
			s->msg_error = true;
		}
		return WF_ES_REDRAW;
	}
	switch (ex.id) {
	case WF_EX_WRITE:      return exec(s, (WfEdAction){WF_CMD_SAVE, false});
	case WF_EX_WRITE_QUIT: {
		/* closes only if the save is done; a question about a changed
		 * file keeps the wish to close until it is answered */
		SaveOutcome o;

		s->exit_after_save = true;
		o = do_save(s);
		if (SAVE_DONE == o) {
			s->exit_after_save = false;
			return close_current(s);
		}
		if (SAVE_FAILED == o) {
			s->exit_after_save = false;
		}
		return WF_ES_REDRAW;
	}
	case WF_EX_QUIT: return try_close(s);
	case WF_EX_QUIT_FORCE:
		/* the explicit "throw this buffer's changes away"; the other
		 * buffers are not touched */
		return close_current(s);
	case WF_EX_REPLACE: begin_find(s, PR_REPL_FIND); return WF_ES_REDRAW;
	case WF_EX_BUFFER_NEXT:
		return exec(s, (WfEdAction){WF_CMD_NEXT_BUFFER, false});
	case WF_EX_BUFFER_PREV:
		return exec(s, (WfEdAction){WF_CMD_PREV_BUFFER, false});
	case WF_EX_PREVIEW: return wf_es_md_toggle(s);
	case WF_EX_BUFFERS:
		list[0] = '\0';
		for (i = 0; i < s->n && used + 40 < sizeof list; i++) {
			char name[64];

			leaf_text(s->buf[i]->path, name, sizeof name);
			used += (size_t)snprintf(list + used,
			        sizeof list - used, "%s%zu:%s%s%s",
			        i ? "  " : "", i + 1, name,
			        wf_ed_modified(&s->buf[i]->ed) ? "+" : "",
			        i == s->cur ? "*" : "");
		}
		say(s, "%s", list);
		return WF_ES_REDRAW;
	default: break;
	}
	return WF_ES_REDRAW;
}

static WfEsResult accept_text(WfEditSession *s)
{
	WfBuffer *b = wf_es_buffer(s);
	PromptKind kind = s->pk;
	char text[WF_ES_INPUT_MAX];

	memcpy(text, s->in, s->in_n + 1);
	switch (kind) {
	case PR_FIND:
	case PR_FIND_BACK:
		if (!*text) {
			prompt_end(s);
			return WF_ES_REDRAW;
		}
		snprintf(s->find, sizeof s->find, "%s", text);
		s->find_n = strlen(s->find);
		remember(s, s->find);
		prompt_end(s);
		do_find(s, PR_FIND == kind);
		return WF_ES_REDRAW;
	case PR_REPL_FIND:
		if (!*text) {
			prompt_end(s);
			return WF_ES_REDRAW;
		}
		snprintf(s->find, sizeof s->find, "%s", text);
		s->find_n = strlen(s->find);
		remember(s, s->find);
		prompt_begin(s, PR_REPL_WITH,
		        wf_msg(MSG_ED_PROMPT_REPLACE_WITH), s->repl);
		return WF_ES_REDRAW;
	case PR_REPL_WITH:
		snprintf(s->repl, sizeof s->repl, "%s", text);
		s->repl_n = strlen(s->repl);
		s->repl_count = 0;
		prompt_end(s);
		replace_next(s);
		return WF_ES_REDRAW;
	case PR_GOTO: {
		char *end;
		unsigned long line = strtoul(text, &end, 10), col = 1;

		prompt_end(s);
		if (end == text) {
			fail_m(s, MSG_ED_NOT_A_LINE, NULL, 0);
			return WF_ES_REDRAW;
		}
		if (',' == *end || ':' == *end) {
			col = strtoul(end + 1, NULL, 10);
		}
		wf_ed_goto(&b->ed, line, col ? col : 1);
		return WF_ES_REDRAW;
	}
	case PR_SAVE_AS:
		prompt_end(s);
		save_as_to(s, text);
		if (!s->msg_error && s->exit_after_save) {
			s->exit_after_save = false;
			return close_current(s);
		}
		return WF_ES_REDRAW;
	case PR_OPEN:
		prompt_end(s);
		open_from_prompt(s, text);
		return WF_ES_REDRAW;
	case PR_INSERT:
		prompt_end(s);
		insert_file(s, text);
		return WF_ES_REDRAW;
	case PR_EX: prompt_end(s); return run_ex(s, text);
	default:    break;
	}
	prompt_end(s);
	return WF_ES_REDRAW;
}

static WfEsResult text_prompt_event(WfEditSession *s, const WfEvent *ev)
{
	bool find_kind = PR_FIND == s->pk || PR_FIND_BACK == s->pk ||
	        PR_REPL_FIND == s->pk;
	bool ex_kind = PR_EX == s->pk;

	if (is_esc(ev)) {
		prompt_end(s);
		s->exit_after_save = false;
		return WF_ES_REDRAW;
	}
	if (WF_EV_CHAR == ev->type) {
		if (s->in_fresh) {
			prompt_remove(s, 0, s->in_n);
			s->in_fresh = false;
		}
		prompt_insert(s, ev->ch);
		return WF_ES_REDRAW;
	}
	if (WF_EV_ALT == ev->type && find_kind) {
		if ('c' == ev->ch) {
			s->f_case = !s->f_case;
		} else if ('w' == ev->ch) {
			s->f_word = !s->f_word;
		}
		return WF_ES_REDRAW;
	}
	if (WF_EV_CTRL == ev->type) {
		switch (ev->ch) {
		case 'u': prompt_remove(s, 0, s->in_cur); break;
		case 'k': prompt_remove(s, s->in_cur, s->in_n); break;
		case 'a': s->in_cur = 0; break;
		case 'e': s->in_cur = s->in_n; break;
		default:  return WF_ES_NONE;
		}
		return WF_ES_REDRAW;
	}
	if (WF_EV_KEY != ev->type) {
		return WF_ES_NONE;
	}
	if (s->in_fresh && WF_KEY_ENTER != ev->key) {
		/* deleting a suggestion clears all of it; moving keeps it as text */
		s->in_fresh = false;
		if (WF_KEY_BACKSPACE == ev->key || WF_KEY_DELETE == ev->key) {
			prompt_remove(s, 0, s->in_n);
			return WF_ES_REDRAW;
		}
	}
	switch (ev->key) {
	case WF_KEY_ENTER: return accept_text(s);
	case WF_KEY_BACKSPACE:
		if (s->in_cur) {
			prompt_remove(s, prompt_prev(s, s->in_cur), s->in_cur);
		}
		return WF_ES_REDRAW;
	case WF_KEY_DELETE:
		if (s->in_cur < s->in_n) {
			prompt_remove(s, s->in_cur, prompt_next(s, s->in_cur));
		}
		return WF_ES_REDRAW;
	case WF_KEY_LEFT:
		s->in_cur = prompt_prev(s, s->in_cur);
		return WF_ES_REDRAW;
	case WF_KEY_RIGHT:
		s->in_cur = prompt_next(s, s->in_cur);
		return WF_ES_REDRAW;
	case WF_KEY_HOME: s->in_cur = 0; return WF_ES_REDRAW;
	case WF_KEY_END:  s->in_cur = s->in_n; return WF_ES_REDRAW;
	case WF_KEY_UP:
		if (find_kind && s->hist_pos > 0) {
			s->hist_pos--;
			snprintf(s->in, sizeof s->in, "%s",
			        s->hist[s->hist_pos]);
			s->in_n = s->in_cur = strlen(s->in);
		} else if (ex_kind && s->exhist_pos > 0) {
			s->exhist_pos--;
			snprintf(s->in, sizeof s->in, "%s",
			        s->exhist[s->exhist_pos]);
			s->in_n = s->in_cur = strlen(s->in);
		}
		return WF_ES_REDRAW;
	case WF_KEY_DOWN:
		if (find_kind && s->hist_pos + 1 < s->hist_n) {
			s->hist_pos++;
			snprintf(s->in, sizeof s->in, "%s",
			        s->hist[s->hist_pos]);
			s->in_n = s->in_cur = strlen(s->in);
		} else if (ex_kind && s->exhist_pos < s->exhist_n) {
			s->exhist_pos++;
			snprintf(s->in, sizeof s->in, "%s",
			        s->exhist_pos < s->exhist_n
			                ? s->exhist[s->exhist_pos]
			                : "");
			s->in_n = s->in_cur = strlen(s->in);
		}
		return WF_ES_REDRAW;
	default: return WF_ES_NONE;
	}
}

/* The person said yes to an encoding that was only a guess: the file is read again
 * with it and opened in a buffer of its own. Every failure is said, and leaves the
 * buffers exactly as they were. `path` stays the caller's. */
static void open_with_chosen_encoding(WfEditSession *s, const wchar_t *path)
{
	WfLoaded l;
	WfBuffer *nb;
	size_t previous = s->cur;
	WfStatus got;

	if (WF_OK !=
	        s->io.load(s->io.ctx, path, s->cfg.ed.max_bytes,
	                s->cfg.fallback_codepage, &s->pending_enc, &l)) {
		return; /* nothing was read: nothing to free */
	}
	nb = WF_FL_OK == l.verdict ? buffer_new(s) : NULL;
	if (!nb) {
		fail_m(s, MSG_ED_CANNOT_OPEN_ENC, NULL, 0);
		goto out_free_loaded;
	}
	nb->path = wf_path_dup(path);
	if (!nb->path || !push_buffer(s, nb)) {
		buffer_free(nb); /* push_buffer did not take it */
		fail_m(s, MSG_ERROR_OUT_OF_MEMORY, NULL, 0);
		goto out_free_loaded;
	}
	/* the buffer is the session's now: a failure takes it back out */
	got = apply_loaded(s, nb, path, &l, false);
	if (WF_OK != got) {
		drop_new_buffer(s, previous);
		fail_m(s,
		        WF_E_RANGE == got ? MSG_ED_CANNOT_OPEN_ENC
		                          : MSG_ERROR_OUT_OF_MEMORY,
		        NULL, 0);
	}
out_free_loaded:
	wf_efile_loaded_free(&l);
}

static WfEsResult choice_prompt_event(WfEditSession *s, const WfEvent *ev)
{
	WfBuffer *b = wf_es_buffer(s);
	char ans = 0;

	/*
	 * These questions are answered by a letter, or without typing one: Up and
	 * Down select, Enter answers with the selection, which starts on Cancel.
	 * (With an IME on, a letter key never reaches the program as that letter.)
	 * A stray Enter therefore cancels; it never saves, discards or reloads.
	 */
	if (PR_QUIT == s->pk || PR_CONFLICT == s->pk || PR_GONE == s->pk ||
	        PR_ENCODING == s->pk || PR_RECOVER == s->pk ||
	        PR_DRAFT == s->pk || PR_LOCKED == s->pk) {
		switch (wf_choice_event(&s->choice, ev, &ans)) {
		case WF_CHOICE_MOVED:  return WF_ES_REDRAW;
		case WF_CHOICE_IDLE:   return WF_ES_NONE;
		case WF_CHOICE_ANSWER: break;
		}
	}
	switch (s->pk) {
	case PR_LOCKED:
		s->pk = PR_NONE;
		s->exit_after_save = false;
		if ('l' == ans && s->lock_path) {
			s->lock_look = true; /* the screen opens the window */
		}
		return WF_ES_REDRAW;
	case PR_QUIT:
		if ('s' == ans) {
			SaveOutcome o;

			s->pk = PR_NONE;
			s->exit_after_save = true;
			o = do_save(s);
			if (SAVE_DONE == o) {
				s->exit_after_save = false;
				return close_current(s);
			}
			if (SAVE_FAILED == o) {
				s->exit_after_save = false;
			}
			return WF_ES_REDRAW;
		}
		if ('d' == ans) {
			s->pk = PR_NONE;
			return close_current(s);
		}
		if ('c' == ans) {
			s->pk = PR_NONE;
			return WF_ES_REDRAW;
		}
		return WF_ES_NONE;
	case PR_CONFLICT:
		if ('r' == ans) {
			s->pk = PR_NONE;
			s->exit_after_save = false;
			reload_current(s);
			return WF_ES_REDRAW;
		}
		if ('a' == ans) {
			prompt_begin(s, PR_SAVE_AS,
			        wf_msg(MSG_ED_PROMPT_SAVE_AS), "");
			return WF_ES_REDRAW;
		}
		if ('c' == ans) {
			s->pk = PR_NONE;
			s->exit_after_save = false;
			say_m(s, MSG_ED_EDITS_KEPT, NULL, 0);
			return WF_ES_REDRAW;
		}
		return WF_ES_NONE;
	case PR_DRAFT: {
		WfBuffer *cur = wf_es_buffer(s);

		if (!cur || !s->draft_text) {
			s->pk = PR_NONE;
			return WF_ES_REDRAW;
		}
		s->pk = PR_NONE;
		if ('r' == ans) {
			/* the whole text is replaced as one edit the user can undo; the
			 * file on disk is not touched until a save */
			if (WF_OK ==
			        wf_ed_restore_text(
			                &cur->ed, s->draft_text, s->draft_n)) {
				cur->has_draft = true;
				cur->draft_gen =
				        cur->ed.text.gen; /* it is the draft */
				cur->draft_ms = s->now_ms ? s->now_ms : 1;
				say_m(s, MSG_ED_DRAFT_RESTORED, NULL, 0);
			} else {
				fail_m(s, MSG_ED_DRAFT_FAILED, NULL, 0);
			}
		} else {
			draft_forget(s, cur);
			say_m(s, MSG_ED_DRAFT_DISCARDED, NULL, 0);
		}
		free(s->draft_text);
		s->draft_text = NULL;
		s->draft_n = 0;
		if (s->goto_pending) { /* the place asked for while it was open */
			s->goto_pending = false;
			apply_place(cur, s->goto_line, s->goto_col, s->goto_top,
			        s->goto_left);
		}
		return WF_ES_REDRAW;
	}
	case PR_RECOVER: {
		char why[160];
		wchar_t *first = NULL, *second = NULL;

		if ('c' == ans) {
			s->pk = PR_NONE;
			return WF_ES_REDRAW;
		}
		/* t: the new text, o: the previous version, b: both (the new text
		 * first, the previous one beside it to compare) */
		if ('t' == ans || 'b' == ans) {
			first = s->recover_new;
		}
		if ('o' == ans) {
			first = s->recover_old;
		} else if ('b' == ans) {
			second = s->recover_old;
		}
		if (!first) {
			return WF_ES_NONE;
		}
		s->pk = PR_NONE;
		if (WF_OPEN_OK != wf_es_open(s, first, why, sizeof why)) {
			fail_m(s, MSG_ED_RECOVER_OPEN_FAILED,
			        MA(WF_ARG_S(why)));
			return WF_ES_REDRAW;
		}
		if (second &&
		        WF_OPEN_OK != wf_es_open(s, second, why, sizeof why)) {
			fail_m(s, MSG_ED_RECOVER_OPEN_FAILED,
			        MA(WF_ARG_S(why)));
		}
		return WF_ES_REDRAW;
	}
	case PR_GONE:
		if ('s' == ans) {
			prompt_begin(s, PR_SAVE_AS,
			        wf_msg(MSG_ED_PROMPT_SAVE_AS), "");
			return WF_ES_REDRAW;
		}
		if ('c' == ans) {
			s->pk = PR_NONE;
			s->exit_after_save = false;
			return WF_ES_REDRAW;
		}
		return WF_ES_NONE;
	case PR_ENCODING:
		if ('y' == ans) {
			wchar_t *path = s->pending_path;

			s->pending_path = NULL;
			s->pk = PR_NONE;
			if (path) {
				open_with_chosen_encoding(s, path);
			}
			free(path);
			return WF_ES_REDRAW;
		}
		if ('n' == ans) {
			free(s->pending_path);
			s->pending_path = NULL;
			s->pk = PR_NONE;
			fail_m(s, MSG_ED_ENCODING_UNKNOWN, NULL, 0);
			return WF_ES_REDRAW;
		}
		return WF_ES_NONE;
	case PR_REPL_ASK:
		if (is_letter(ev, 'y')) {
			WfSearch q = search_of(s);

			if (wf_ed_replace_selection(
			            &b->ed, &q, s->repl, s->repl_n) == WF_OK) {
				s->repl_count++;
			}
			replace_next(s);
			return WF_ES_REDRAW;
		}
		if (is_letter(ev, 'n')) {
			wf_ed_clear_selection(
			        &b->ed); /* the cursor stays after the match */
			replace_next(s);
			return WF_ES_REDRAW;
		}
		if (is_letter(ev, 'a')) {
			WfSearch q = search_of(s);
			size_t count = 0;
			WfStatus st = wf_ed_replace_all(
			        &b->ed, &q, s->repl, s->repl_n, &count);

			s->repl_count += count;
			prompt_end(s);
			if (WF_OK != st) {
				fail_m(s, MSG_ED_REPLACED_STOPPED,
				        MA(WF_ARG_N(s->repl_count),
				                WF_ARG_S(status_text(st))));
			} else {
				say_m(s,
				        wf_msg_count(MSG_ED_REPLACED_ONE,
				                MSG_ED_REPLACED_OTHER,
				                (long long)s->repl_count),
				        MA(WF_ARG_N(s->repl_count)));
			}
			return WF_ES_REDRAW;
		}
		if (is_esc(ev)) {
			wf_ed_clear_selection(&b->ed);
			replace_finish(s);
			return WF_ES_REDRAW;
		}
		return WF_ES_NONE;
	case PR_HELP:
		if (WF_EV_KEY == ev->type && WF_KEY_DOWN == ev->key) {
			s->help_top++;
			return WF_ES_REDRAW;
		}
		if (WF_EV_KEY == ev->type && WF_KEY_UP == ev->key) {
			if (s->help_top) {
				s->help_top--;
			}
			return WF_ES_REDRAW;
		}
		if (WF_EV_KEY == ev->type &&
		        (WF_KEY_PGDN == ev->key || WF_KEY_PGUP == ev->key)) {
			size_t page = s->rows > 5 ? (size_t)s->rows - 4 : 1;

			if (WF_KEY_PGDN == ev->key) {
				s->help_top +=
				        page; /* the view clamps at the end */
			} else {
				s->help_top = s->help_top > page
				        ? s->help_top - page
				        : 0;
			}
			return WF_ES_REDRAW;
		}
		if (WF_EV_RESIZE == ev->type || WF_EV_RELEASE == ev->type) {
			return WF_ES_NONE;
		}
		s->pk = PR_NONE;
		return WF_ES_REDRAW;
	default: break;
	}
	return WF_ES_NONE;
}

void wf_es_set_clock(WfEditSession *s, uint64_t now_ms)
{
	s->now_ms = now_ms;
}

uint32_t wf_es_timeout(const WfEditSession *s, uint64_t now_ms)
{
	uint32_t t = UINT32_MAX;

	if (!s) {
		return t;
	}
	if (wf_es_motion_pending(s)) {
		return 0;
	}
	if (wf_keystate_active(&s->ks)) {
		t = now_ms >= s->ks.deadline
		        ? 0
		        : (uint32_t)(s->ks.deadline - now_ms);
	}
	/* the next draft that is due (none while the question about one is open) */
	if (s->cfg.drafts && s->io.draft_write && PR_DRAFT != s->pk) {
		for (size_t i = 0; i < s->n; i++) {
			const WfBuffer *b = s->buf[i];
			uint64_t due;
			uint32_t left;

			if (!draft_wanted(b)) {
				continue;
			}
			due = b->draft_ms ? b->draft_ms + WF_ES_DRAFT_EVERY_MS
			                  : 0;
			left = now_ms >= due ? 0 : (uint32_t)(due - now_ms);
			if (left < t) {
				t = left;
			}
		}
	}
	{ /* the Markdown preview waits for the text to stop changing */
		uint32_t left = wf_es_md_wait(s, now_ms);

		if (left < t) {
			t = left;
		}
	}
	{ /* and so does the diff against HEAD */
		uint32_t left = wf_es_git_wait(s, now_ms);

		if (left < t) {
			t = left;
		}
	}
	return t;
}

bool wf_es_tick(WfEditSession *s, uint64_t now_ms)
{
	bool redraw;

	if (!s) {
		return false;
	}
	s->now_ms = now_ms;
	redraw = word_slice(s);
	redraw |= wf_vim_tick(s);
	for (unsigned i = 0; s->n && s->input_n && !s->motion.active &&
	        !s->vim.motion.active && !s->vim.playback.ev && i < 16;
	        i++) {
		WfEvent ev = s->input[0];
		memmove(s->input, s->input + 1,
		        (--s->input_n) * sizeof *s->input);
		s->input_draining = true;
		WfEsResult result = wf_es_event(s, &ev, s->rows, s->cols);
		s->input_draining = false;
		redraw |= result != WF_ES_NONE;
		if (result == WF_ES_CLOSED) {
			break;
		}
	}
	redraw |= wf_es_md_tick(s, now_ms);
	redraw |= wf_es_git_tick(s, now_ms);
	if (s->input_notice != WF_OK) {
		if (!s->msg_error) {
			wf_es_fail_status(s, s->input_notice);
		}
		s->input_notice = WF_OK;
		redraw = true;
	}
	if (!wf_keystate_active(&s->ks) || now_ms < s->ks.deadline) {
		return redraw;
	}
	wf_keystate_reset(&s->ks);
	return true;
}

static WfKeyContext key_context(const WfEditSession *s)
{
	if (!wf_keymap_modal(s->cfg.keys)) {
		return WF_CTX_EDIT;
	}
	return s->vim_insert ? WF_CTX_VIM_INSERT : WF_CTX_VIM_NORMAL;
}

/* A command with its number: moves repeat, "go to line" takes the number as
 * the line; nothing else is ever repeated. */
static WfEsResult exec_counted(WfEditSession *s, WfKeyResult r)
{
	const WfCommandInfo *ci = wf_cmd_info(r.cmd);
	WfEdAction a = {(WfEdCmd)r.cmd, r.extend};
	WfBuffer *b = wf_es_buffer(s);
	WfEsResult res = WF_ES_REDRAW;
	uint32_t i, times = 1;

	if (wf_vim_extends(&s->vim) && a.cmd >= WF_CMD_LEFT &&
	        a.cmd <= WF_CMD_WORD_RIGHT) {
		a.extend = true; /* in Visual mode every motion selects */
	}
	s->vim_count = r.counted ? r.count : 0;
	if (wf_vim_is_cmd(a.cmd) ||
	        a.cmd ==
	                WF_CMD_VIM_DELETE_CHAR) { /* the count belongs to the command */
		return exec(s, a);
	}
	if (!b) {
		return WF_ES_CLOSED;
	}
	if (wf_keymap_modal(s->cfg.keys) && !s->vim_insert &&
	        a.cmd >= WF_CMD_LEFT && a.cmd <= WF_CMD_WORD_RIGHT) {
		return wf_vim_move(s, a.cmd, r.counted ? r.count : 0, a.extend);
	}
	if (r.counted && ci && (ci->flags & WF_CF_LINE)) {
		wf_ed_clear_selection(&b->ed);
		wf_ed_goto(&b->ed, r.count, 1);
		return WF_ES_REDRAW;
	}
	if (r.counted && ci && (ci->flags & WF_CF_COUNT)) {
		times = r.count;
	}
	for (i = 0; i < times; i++) {
		res = exec(s, a);
		if (WF_ES_CLOSED == res) {
			break;
		}
	}
	return res;
}

static WfEsResult keyed_event(WfEditSession *s, WfBuffer *b, const WfEvent *ev)
{
	WfKeyContext ctx = key_context(s);
	WfStroke stroke;
	WfKeyResult r;

	if (s->ks_ctx != ctx) {
		wf_keystate_reset(&s->ks);
		s->ks_ctx = ctx;
	}
	/* the key after d, y, c, ", q or @ belongs to that command */
	if (WF_CTX_VIM_NORMAL == ctx) {
		WfEsResult followed;

		if (wf_vim_follow(s, ev, &followed)) {
			wf_keystate_reset(&s->ks);
			return followed;
		}
	}
	/* Pasted keys are text, whatever mode the editor is in: in Vim's Normal
	 * mode they would otherwise run as commands. A pasted Enter is a line
	 * break there, not "down". */
	if ((ev->vk & WF_VK_PASTED) && WF_CTX_VIM_NORMAL == ctx &&
	        WF_EV_KEY == ev->type && WF_KEY_ENTER == ev->key) {
		WfEdAction nl;

		memset(&nl, 0, sizeof nl);
		nl.cmd = WF_CMD_NEWLINE;
		wf_keystate_reset(&s->ks);
		return exec(s, nl);
	}
	if (WF_EV_CHAR == ev->type &&
	        (WF_CTX_VIM_NORMAL != ctx || (ev->vk & WF_VK_PASTED))) {
		WfStatus st;

		wf_keystate_reset(&s->ks);
		b->ed.undo.capacity_refused = false;
		st = wf_ed_type(&b->ed, ev->ch);
		if (b->ed.undo.capacity_refused) {
			fail_m(s, MSG_ED_UNDO_BLOCK_FULL, NULL, 0);
		} else if (WF_E_ACCESS == st) {
			fail_m(s, MSG_ED_READONLY_EDIT, NULL, 0);
		} else if (WF_E_RANGE == st) {
			fail_m(s, MSG_ED_SIZE_LIMIT_MIB,
			        MA(WF_ARG_N(s->cfg.ed.max_bytes / MiB)));
		} else if (WF_OK != st && WF_E_INVAL != st) {
			fail_m(s, MSG_ED_CANNOT_TYPE,
			        MA(WF_ARG_S(status_text(st))));
		}
		return WF_ES_REDRAW;
	}
	if (!wf_stroke_from_event(ev, &stroke)) {
		return WF_ES_NONE;
	}
	r = wf_keymap_feed(s->cfg.keys, ctx, &s->ks, stroke, s->now_ms);
	switch (r.verdict) {
	case WF_KR_NONE:      return WF_ES_NONE;
	case WF_KR_PENDING:
	case WF_KR_CANCELLED: return WF_ES_REDRAW;
	case WF_KR_REFUSED:
		fail_m(s, MSG_KEYS_COUNT_REFUSED, NULL, 0);
		return WF_ES_REDRAW;
	case WF_KR_COMMAND: break;
	}
	return exec_counted(s, r);
}

static WfEsResult event_inner(
        WfEditSession *s, const WfEvent *ev, int rows, int cols)
{
	WfBuffer *b = wf_es_buffer(s);
	WfEdAction act;

	s->rows = rows;
	s->cols = cols;
	if (!b && PR_NONE == s->pk) {
		return WF_ES_CLOSED;
	}
	if (WF_EV_RESIZE == ev->type) {
		return WF_ES_REDRAW;
	}
	if (WF_EV_RELEASE == ev->type) {
		return WF_ES_NONE;
	}
	if (b) {
		b->ed.wrap_cells =
		        s->cfg.soft_wrap ? (size_t)wf_es_text_width(s, b) : 0;
	}
	s->msg[0] = '\0';
	s->msg_error = false;
	if (PR_NONE != s->pk) {
		bool text = PR_FIND == s->pk || PR_FIND_BACK == s->pk ||
		        PR_REPL_FIND == s->pk || PR_REPL_WITH == s->pk ||
		        PR_GOTO == s->pk || PR_SAVE_AS == s->pk ||
		        PR_OPEN == s->pk || PR_INSERT == s->pk ||
		        PR_EX == s->pk;

		if (ev->vk & WF_VK_PASTED) {
			/* a question is never answered by pasted keys; a text
			 * prompt takes the first pasted line as text, and a
			 * pasted Enter or Tab neither confirms nor goes on */
			if (!text) {
				return WF_ES_NONE;
			}
			if (WF_EV_KEY == ev->type) {
				s->paste_cut = true;
				return WF_ES_NONE;
			}
			if (s->paste_cut) {
				return WF_ES_NONE;
			}
		} else {
			s->paste_cut = false;
		}
		return text ? text_prompt_event(s, ev)
		            : choice_prompt_event(s, ev);
	}
	if (s->cfg.keys) {
		return keyed_event(s, b, ev);
	}
	if (WF_EV_CHAR == ev->type) {
		b->ed.undo.capacity_refused = false;
		WfStatus st = wf_ed_type(&b->ed, ev->ch);

		if (b->ed.undo.capacity_refused) {
			fail_m(s, MSG_ED_UNDO_BLOCK_FULL, NULL, 0);
		} else if (WF_E_ACCESS == st) {
			fail_m(s, MSG_ED_READONLY_EDIT, NULL, 0);
		} else if (WF_E_RANGE == st) {
			fail_m(s, MSG_ED_SIZE_LIMIT_MIB,
			        MA(WF_ARG_N(s->cfg.ed.max_bytes / MiB)));
		} else if (WF_OK != st && WF_E_INVAL != st) {
			fail_m(s, MSG_ED_CANNOT_TYPE,
			        MA(WF_ARG_S(status_text(st))));
		}
		return WF_ES_REDRAW;
	}
	act = wf_ekm_lookup(s->cfg.keymap, ev);
	if (WF_CMD_NONE == act.cmd) {
		return WF_ES_NONE;
	}
	return exec(s, act);
}

WfEsResult wf_es_event(WfEditSession *s, const WfEvent *ev, int rows, int cols)
{
	bool vim = s->cfg.keys && wf_es_buffer(s) &&
	        (WF_EV_KEY == ev->type || WF_EV_CHAR == ev->type);
	WfEsResult res;

	/* Retain type-ahead once, in arrival order. A full queue never takes
	 * ownership; the terminal keeps its bounded batch and applies pressure
	 * upstream. Resize/release can be handled between slices. */
	if (!s->input_draining && wf_es_motion_pending(s) &&
	        WF_EV_RESIZE != ev->type && WF_EV_RELEASE != ev->type) {
		if (!wf_es_input_ready(s)) {
			return WF_ES_BUSY;
		}
		s->rows = rows;
		s->cols = cols;
		s->input[s->input_n++] = *ev;
		if (is_esc(ev) && !(ev->vk & WF_VK_PASTED)) {
			s->motion.active = false;
			wf_vim_stop(s);
			wf_es_fail_status(s, WF_E_CANCELLED);
		}
		return WF_ES_REDRAW;
	}
	WfBuffer *before = wf_es_buffer(s);
	WfPos cursor = before ? before->ed.cur : (WfPos){0, 0};
	WfPos anchor = before ? before->ed.anchor : (WfPos){0, 0};
	bool selected = before && before->ed.has_anchor;
	bool mark = before && before->ed.mark;
	bool want_valid = before && before->ed.want_valid;
	size_t want_cells = before ? before->ed.want_cells : 0;
	bool visual = s->vim.visual, vline = s->vim.vline;
	uint64_t gen = before ? before->ed.text.gen : 0;
	uint32_t group = before ? before->ed.undo.open_group : 0;
	uint32_t next = before ? before->ed.undo.next_group : 0;
	size_t group_bytes = before ? before->ed.undo.open_bytes : 0;
	if (before && s->vim_insert) {
		if (WF_EV_KEY == ev->type &&
		        (ev->key == WF_KEY_LEFT || ev->key == WF_KEY_RIGHT ||
		                ev->key == WF_KEY_UP ||
		                ev->key == WF_KEY_DOWN ||
		                ev->key == WF_KEY_HOME ||
		                ev->key == WF_KEY_END ||
		                ev->key == WF_KEY_PGUP ||
		                ev->key == WF_KEY_PGDN)) {
			wf_un_group_end(&before->ed.undo);
		} else {
			wf_un_group_begin(&before->ed.undo);
		}
	}
	if (vim) { /* a macro takes every key, the answers to prompts too */
		wf_vim_record(s, ev);
		wf_vim_dot_before(s, ev);
	}
	res = event_inner(s, ev, rows, cols);
	if (s->msg_error && before && before == wf_es_buffer(s) &&
	        before->ed.text.gen == gen) {
		before->ed.cur = cursor;
		before->ed.anchor = anchor;
		before->ed.has_anchor = selected;
		before->ed.mark = mark;
		before->ed.want_valid = want_valid;
		before->ed.want_cells = want_cells;
		s->vim.visual = visual;
		s->vim.vline = vline;
		before->ed.undo.open_group = group;
		before->ed.undo.next_group = next;
		before->ed.undo.open_bytes = group_bytes;
	}
	if (WF_EV_RESIZE != ev->type && WF_EV_RELEASE != ev->type &&
	        !s->nano_cut.action) {
		s->nano_cut.chain = false;
	}
	s->nano_cut.action = false;
	if (vim) {
		wf_vim_dot_after(s);
	}
	if (!s->input_draining && s->vim.playback.ev) {
		(void)wf_vim_tick(s);
	}
	return res;
}

bool wf_es_disk_stamp(
        const WfEditSession *s, const wchar_t **path, WfFileStamp *out)
{
	const WfBuffer *b = s && s->n ? s->buf[s->cur] : NULL;

	if (!b || !b->path || !b->on_disk || !b->stamp.valid ||
	        b->save_uncertain) {
		return false;
	}
	*path = b->path;
	*out = b->stamp;
	return true;
}

void wf_es_note_external_change(WfEditSession *s)
{
	WfBuffer *b = wf_es_buffer(s);
	const wchar_t *leaf;
	size_t n;
	WfBuf name;

	if (!b || !b->path || b->external_noted) {
		return;
	}
	b->external_noted = true;
	wf_vpath_leaf(b->path, &leaf, &n);
	wf_buf_init(&name);
	if (WF_OK == wf_sanitize_utf16(leaf, n, &name)) {
		say_m(s, MSG_ED_CHANGED_ELSEWHERE,
		        MA(WF_ARG_S(wf_buf_cstr(&name))));
	}
	wf_buf_free(&name);
}
