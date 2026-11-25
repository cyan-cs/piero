#ifndef WF_ESYNTAX_H
#define WF_ESYNTAX_H

#include <stddef.h>
#include <stdint.h>

#include "../syntax/wf_synoverlay.h"
#include "wf_etext.h"

/*
 * Syntax colouring: a display-only lexer. It reads the text and produces a
 * class for every byte of a line; it never changes the text, and nothing
 * depends on it, so a failure to colour leaves editing and saving as they
 * were.
 *
 * Lines are lexed on demand. What carries over from one line to the next (an
 * open block comment, a multi-line string, a Markdown code fence) is a small
 * state per line start, computed lazily from the last line known to be good;
 * an edit invalidates the states from the edited line down, so a keystroke
 * costs the lines on screen, not the file.
 */
typedef enum
{
	WF_LANG_TEXT,
	WF_LANG_C,
	WF_LANG_CPP,
	WF_LANG_JSON,
	WF_LANG_YAML,
	WF_LANG_INI,
	WF_LANG_MARKDOWN,
	WF_LANG_POWERSHELL
} WfLang;

typedef enum
{
	WF_TOK_PLAIN = 0,
	WF_TOK_COMMENT,
	WF_TOK_STRING,
	WF_TOK_ESCAPE, /* an escape sequence inside a string */
	WF_TOK_NUMBER,
	WF_TOK_KEYWORD,
	WF_TOK_TYPE,
	WF_TOK_PREPROC,
	WF_TOK_HEADING, /* Markdown heading, INI section */
	WF_TOK_KEY,     /* JSON/YAML/INI key */
	WF_TOK_MARKUP,  /* Markdown emphasis, list markers, fences, links */
	/* the classes of the Tree-sitter grammars (the numbers of WfSynClass,
	 * DR-DRAFT-033); the hand lexer below never produces them */
	WF_TOK_BOOLEAN,
	WF_TOK_NULL,
	WF_TOK_FUNCTION,
	WF_TOK_VARIABLE,
	WF_TOK_CONSTANT,
	WF_TOK_OPERATOR,
	WF_TOK_PUNCTUATION,
	WF_TOK_LINK,
	WF_TOK_ERROR
} WfTok;

/* The language for a file name (by extension), TEXT when unknown. */
WfLang wf_syn_lang_for_name(const wchar_t *name);
const char *wf_syn_lang_name(WfLang lang);
/* The line-comment prefix used by comment toggling, or NULL. */
const char *wf_syn_comment_prefix(WfLang lang);

/* Colouring by a Tree-sitter grammar (DR-DRAFT-033), when one is installed for
 * the file: OFF is the hand lexer below; PENDING is plain text until the first
 * answer; READY draws the answer's colours; FAILED (a text over the limit, a
 * grammar that was refused) is plain text. */
typedef enum
{
	WF_TS_OFF = 0,
	WF_TS_PENDING,
	WF_TS_READY,
	WF_TS_FAILED
} WfTsStateKind;

typedef struct
{
	int state; /* WfTsStateKind */
	char lang[24];
	WfSynOverlay ov;
	char *flat; /* the text as the parser reads it: each line, then a line feed */
	size_t flat_len;
	size_t *off; /* where each line starts in it */
	size_t off_n;
	uint64_t flat_gen, off_gen; /* the WfEText generation they are of */
	uint32_t status, parse_us, incremental; /* of the last answer */
} WfTsState;

typedef struct WfSyntax_ WfSyntax;

void wf_ts_free(WfTsState *ts);
/* Classes of a line of a buffer a grammar colours; false: draw it plain. */
bool wf_ts_line(const WfSyntax *syn,
        const WfEText *t,
        size_t line,
        uint8_t *out,
        size_t cap);

struct WfSyntax_
{
	WfLang lang;
	WfTsState ts;
	uint8_t *state; /* state at the start of each line; owned */
	size_t n;       /* entries allocated */
	size_t valid;   /* states [0, valid) are known to be right */
};

void wf_syn_init(WfSyntax *s, WfLang lang);
void wf_syn_free(WfSyntax *s);
/* The text changed from `line` on (inclusive). */
void wf_syn_invalidate(WfSyntax *s, size_t line);

/* Lines longer than this are neither coloured nor lexed when the state of the
 * lines after them is worked out (see wf_syn_line). */
#define WF_SYN_LINE_MAX 16384u

/* Classifies every byte of line `line` into out[0..len). Returns false (and
 * leaves out unchanged) when it cannot, e.g. on allocation failure; the
 * caller then draws the line plain. */
bool wf_syn_line(
        WfSyntax *s, const WfEText *t, size_t line, uint8_t *out, size_t cap);

#endif
