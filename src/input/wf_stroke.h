#ifndef WF_STROKE_H
#define WF_STROKE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../ui/wf_event.h"

/*
 * One key press as a key binding sees it, whatever the terminal layer called
 * it: a character (with Ctrl and/or Alt) or a named key (with Ctrl, Alt,
 * Shift). A character carries the character that was typed, not the physical
 * key: on a Japanese keyboard ':' is whatever the layout makes of it, and
 * plain Shift is part of the character (Shift+g is 'G'); Ctrl+Shift uses
 * a separate Shift modifier.
 *
 * wf_stroke_bindable validates the supported combinations: printable
 * characters, Alt characters, Ctrl with lower-case letters, digits, '_' or
 * backslash, and Ctrl+Shift with lower-case letters. Named keys and their
 * supported modifiers come from the KEYS table in wf_stroke.c.
 */
typedef enum
{
	WF_MOD_CTRL = 1,
	WF_MOD_ALT = 2,
	WF_MOD_SHIFT = 4
} WfMod;

typedef enum
{
	WF_NK_NONE = 0,
	WF_NK_UP,
	WF_NK_DOWN,
	WF_NK_LEFT,
	WF_NK_RIGHT,
	WF_NK_ENTER,
	WF_NK_ESC,
	WF_NK_BACKSPACE,
	WF_NK_TAB,
	WF_NK_DELETE,
	WF_NK_HOME,
	WF_NK_END,
	WF_NK_PGUP,
	WF_NK_PGDN,
	WF_NK_INSERT,
	WF_NK_F2,
	WF_NK_F3,
	WF_NK_COUNT
} WfNamedKey;

typedef struct
{
	uint32_t code; /* a Unicode scalar, or a WfNamedKey when `named` */
	uint8_t mods;  /* WfMod bits */
	bool named;
} WfStroke;

#define WF_SEQ_MAX 3

/* One or more strokes that together make a command ("g g", "Ctrl+S"). */
typedef struct
{
	WfStroke s[WF_SEQ_MAX];
	uint8_t n;
} WfSeq;

bool wf_stroke_equal(WfStroke a, WfStroke b);
bool wf_seq_equal(const WfSeq *a, const WfSeq *b);
/* `a` is a proper prefix of `b`. */
bool wf_seq_is_prefix(const WfSeq *a, const WfSeq *b);

/* False for events that are not key presses (release, resize). */
bool wf_stroke_from_event(const WfEvent *ev, WfStroke *out);

/* Whether a key can be given to a command at all. Where the console is in
 * its normal mode (`interrupt_is_signal`: the file manager) Ctrl+C is the
 * program's interrupt and never arrives as a key; the editor takes it as a
 * key. Ctrl+I / Ctrl+M are Tab and Enter on many terminals. `why` receives a
 * short English reason when false (may be NULL). */
bool wf_stroke_bindable(WfStroke s, bool interrupt_is_signal, const char **why);

/* "Ctrl+Alt+Shift+Name"; a character is itself ("g", "Space", "Comma",
 * "Plus"). Returns the bytes written without NUL, at most cap - 1.
 * With cap == 0, out may be NULL and the result is 0. */
size_t wf_stroke_format(WfStroke s, char *out, size_t cap);

/* Parses one stroke: names are case-insensitive ("ctrl+s", "PageUp",
 * "Space", "Esc"), Shift+letter without Ctrl/Alt is the capital letter. */
bool wf_stroke_parse(const char *text, size_t n, WfStroke *out);

/* A sequence: strokes separated by spaces; a run of plain characters with no
 * spaces is a sequence of those characters ("gg"). */
bool wf_seq_parse(const char *text, size_t n, WfSeq *out);
/* Same output and return-value contract as wf_stroke_format. */
size_t wf_seq_format(const WfSeq *q, char *out, size_t cap);

#endif
