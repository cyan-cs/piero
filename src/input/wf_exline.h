#ifndef WF_EXLINE_H
#define WF_EXLINE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * The command line that Vim's ':' opens, for the file manager and the
 * editor: a command name and, where it takes one, an argument. Nothing here
 * runs anything. The text is split and checked; what a command does is the
 * business of the code that asked. It is never given to a shell, and there is
 * no way to say "run this program".
 *
 * Words are separated by blanks; a word may be quoted with " or ' (the quotes
 * are removed, there are no escapes: backslash is a path separator). The
 * argument of `go` and `filter` is the rest of the line, quotes removed when
 * the whole argument is quoted.
 */
typedef enum
{
	WF_EXS_FM,
	WF_EXS_EDITOR
} WfExScope;

typedef enum
{
	WF_EX_NONE,
	/* file manager */
	WF_EX_GO,
	WF_EX_SORT,
	WF_EX_SET_HIDDEN,
	WF_EX_FILTER,
	WF_EX_CLEAR_FILTER,
	WF_EX_RELOAD,
	WF_EX_SETTINGS,
	WF_EX_HELP,
	WF_EX_QUIT,
	/* editor */
	WF_EX_WRITE,
	WF_EX_WRITE_QUIT,
	WF_EX_QUIT_FORCE,
	WF_EX_REPLACE,
	WF_EX_BUFFERS,
	WF_EX_BUFFER_NEXT,
	WF_EX_BUFFER_PREV,
	WF_EX_PREVIEW
} WfExId;

typedef enum
{
	WF_EXE_NONE,
	WF_EXE_EMPTY,
	WF_EXE_UNKNOWN,     /* no such command */
	WF_EXE_MISSING_ARG, /* needs an argument */
	WF_EXE_BAD_ARG,     /* the argument is not one of the choices */
	WF_EXE_TRAILING,    /* text after a command that takes none */
	WF_EXE_QUOTE,       /* a quote that is not closed */
	WF_EXE_TOO_LONG,
	WF_EXE_UNSUPPORTED, /* a word that is understood and not available */
	WF_EXE_WRONG_SCOPE  /* a command of the other place */
} WfExError;

#define WF_EX_ARG_MAX 512

typedef struct
{
	WfExId id;
	WfExError error;
	char name[24]; /* the command word, for messages */
	char arg[WF_EX_ARG_MAX];
	int sort_key; /* WfSortKey */
	bool descending;
	bool on; /* set hidden on / off */
} WfEx;

/* Returns true and fills `out` for a command that can be run; otherwise false
 * with out->error and out->name (and out->arg for BAD_ARG / UNSUPPORTED).
 * text may be NULL only when n == 0; empty input returns WF_EXE_EMPTY. */
bool wf_ex_parse(const char *text, size_t n, WfExScope scope, WfEx *out);

/* The sentence for a refused command line, in the language in use. */
void wf_ex_error_text(const WfEx *ex, char *out, size_t cap);

#endif
