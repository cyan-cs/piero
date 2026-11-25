#ifndef WF_KEYMAP_H
#define WF_KEYMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../base/wf_status.h"
#include "wf_commands.h"
#include "wf_stroke.h"

/*
 * Which keys do what, as data.
 *
 *   key press --> stroke --> (context, key state) --> command --> handler
 *
 * A profile is a set of bindings per context. Nano and Vim are built in; a
 * Custom profile is one of those two plus the user's overrides, kept as a
 * short list of "this command, in this context, has these keys". The effective
 * bindings are built once (at start, and when settings are saved) into a
 * WfKeymap; a key press then costs a scan of one small array.
 *
 * Nothing in here knows what a command does, and no handler knows what key
 * asked for it.
 */
typedef enum
{
	WF_PROFILE_LEGACY, /* no [keyboard] section: the keys as they were */
	WF_PROFILE_NANO,
	WF_PROFILE_VIM,
	WF_PROFILE_CUSTOM
} WfProfile;

typedef enum
{
	WF_BASE_NANO,
	WF_BASE_VIM
} WfBase;

/* "this command, in this context, has these keys instead" */
typedef struct
{
	uint8_t ctx;  /* WfKeyContext (a bindable one) */
	uint8_t n;    /* 0: no key at all (never for a required command) */
	uint16_t cmd; /* command number */
	WfSeq seq[2];
} WfKeyOverride;

#define WF_KEY_OVERRIDES 192u

enum
{
	WF_BINDINGS_COMPAT = 1,
	WF_BINDINGS_CURRENT = 2
};

typedef struct
{
	int profile; /* WfProfile */
	int base;    /* WfBase, for Custom */
	int version; /* 1: legacy Custom Nano navigation; 2: arrow keys */
	size_t count;
	WfKeyOverride o[WF_KEY_OVERRIDES];
} WfKeyConfig;

void wf_keyconfig_init(WfKeyConfig *k);
bool wf_keyconfig_equal(const WfKeyConfig *a, const WfKeyConfig *b);
/* The override of (ctx, cmd), or NULL. */
const WfKeyOverride *wf_keyconfig_find(
        const WfKeyConfig *k, WfKeyContext ctx, uint16_t cmd);
/* Sets (replaces or adds) an override; false if the table is full. */
bool wf_keyconfig_set(WfKeyConfig *k, const WfKeyOverride *o);
/* Removes the override of (ctx, cmd); returns whether there was one. */
bool wf_keyconfig_remove(WfKeyConfig *k, WfKeyContext ctx, uint16_t cmd);

/* "k, Up" / "none" and back: sequences separated by commas, at most two. */
bool wf_keys_parse_list(const char *text, WfKeyOverride *out);
/* Formatters return bytes stored, excluding NUL. cap == 0 permits NULL out
 * and returns 0; otherwise output is terminated and at most cap - 1 bytes. */
size_t wf_keys_format_list(const WfKeyOverride *o, char *out, size_t cap);

typedef struct
{
	WfSeq seq;
	uint16_t cmd;
	uint8_t extend; /* motion that grows the selection (Shift+arrows) */
} WfBinding;

typedef enum
{
	WF_KI_NONE,
	WF_KI_BAD_KEY,     /* a stroke the terminal cannot report */
	WF_KI_SAME_KEYS,   /* two commands, the same keys */
	WF_KI_PREFIX,      /* one command's keys begin another's */
	WF_KI_COUNT_DIGIT, /* a digit that is a count in this context */
	WF_KI_REQUIRED,    /* a required command left without a key */
	WF_KI_UNKNOWN      /* no such command / not in that context */
} WfKeyIssueKind;

typedef struct
{
	WfKeyIssueKind kind;
	WfKeyContext ctx;
	uint16_t cmd, other; /* the commands involved */
	const char *why;     /* WF_KI_BAD_KEY: English reason */
} WfKeyIssue;

typedef struct WfKeymap WfKeymap;

/*
 * Builds the effective bindings of `cfg`. `editor_legacy_keymap` is the
 * editor preset (WfKeymapId: 1 Piero, 2 Nano) used when the profile is
 * Legacy. On a problem in the overrides returns WF_E_INVAL with *issue
 * filled and *out NULL. Allocation failure returns WF_E_NOMEM with *out NULL;
 * wf_keymap_build_defaults() never fails except for memory.
 */
WfStatus wf_keymap_build(const WfKeyConfig *cfg,
        int editor_legacy_keymap,
        WfKeymap **out,
        WfKeyIssue *issue);
WfStatus wf_keymap_build_defaults(
        const WfKeyConfig *cfg, int editor_legacy_keymap, WfKeymap **out);
void wf_keymap_free(WfKeymap *m);

WfProfile wf_keymap_profile(const WfKeymap *m);
WfBase wf_keymap_base(const WfKeymap *m); /* Nano or Vim keys underneath */
/* Whether the editor runs modal (Vim) in this profile. */
bool wf_keymap_modal(const WfKeymap *m);
/* Whether numbers before a command are counts in `ctx`. */
bool wf_keymap_counts(const WfKeymap *m, WfKeyContext ctx);
const WfBinding *wf_keymap_bindings(
        const WfKeymap *m, WfKeyContext ctx, size_t *n);

/* "k, Up": the keys of `cmd` in `ctx` (without selection variants), at most
 * three, for help, hints and the settings list. Empty when it has none. */
size_t wf_keymap_keys_text(const WfKeymap *m,
        WfKeyContext ctx,
        uint16_t cmd,
        char *out,
        size_t cap);

/* The first key of `cmd` in `ctx` ("Ctrl+S"), for a hint. Empty if none. */
size_t wf_keymap_first_key(const WfKeymap *m,
        WfKeyContext ctx,
        uint16_t cmd,
        char *out,
        size_t cap);

/* Calls `sink` for every command that has a key in the given contexts, in
 * table order, with its keys and description; a header (the context's name)
 * comes before each context that has any. This is what the help screens are
 * made of: nothing is typed twice. */
typedef void (*WfHelpSink)(
        void *ctx, const char *keys, const char *desc, bool header);
void wf_help_walk(const WfKeymap *m,
        const WfKeyContext *ctxs,
        size_t n,
        WfHelpSink sink,
        void *ctx);

/* The keys the base profile gives `cmd` in `ctx` (for "reset"). */
size_t wf_keymap_default_text(
        WfBase base, WfKeyContext ctx, uint16_t cmd, char *out, size_t cap);

/* Whether `seq` could be given to `cmd` in `ctx` without a clash; if not,
 * says which command is in the way. */
WfKeyIssue wf_keymap_check(
        const WfKeymap *m, WfKeyContext ctx, uint16_t cmd, const WfSeq *seq);

#define WF_SEQ_TIMEOUT_MS 1500u
#define WF_COUNT_MAX 9999u

typedef struct
{
	WfSeq pending;
	uint32_t count; /* 0: none typed */
	uint64_t deadline;
} WfKeyState;

typedef enum
{
	WF_KR_NONE,      /* the key means nothing here */
	WF_KR_PENDING,   /* the start of a longer sequence, or of a count */
	WF_KR_COMMAND,   /* run `cmd` (`count` times) */
	WF_KR_CANCELLED, /* Esc dropped what was typed so far */
	WF_KR_REFUSED    /* a count before a command that takes none */
} WfKeyVerdict;

typedef struct
{
	WfKeyVerdict verdict;
	uint16_t cmd;
	uint8_t extend;
	uint32_t count; /* 1 when none was typed */
	bool counted;   /* a number was typed */
} WfKeyResult;

void wf_keystate_reset(WfKeyState *st);
bool wf_keystate_active(const WfKeyState *st);
WfKeyResult wf_keymap_feed(const WfKeymap *m,
        WfKeyContext ctx,
        WfKeyState *st,
        WfStroke stroke,
        uint64_t now_ms);

#endif
