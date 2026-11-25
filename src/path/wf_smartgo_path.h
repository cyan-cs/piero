#ifndef WF_SMARTGO_PATH_H
#define WF_SMARTGO_PATH_H

#include <stdbool.h>
#include <stddef.h>

#include "../base/wf_status.h"

/*
 * Resolves Smart Go input (`g`, Ctrl+L) against the supplied directory.
 * Home, known-folder and environment lookups use WfSmartEnv callbacks;
 * resolution never reads the process current directory.
 *
 * Supported: relative paths (src, ./src, .., ../..), ~ (the user's profile
 * folder, only as ~ ~/ ~\ and ~/rest; ~name is a plain name), \ and / at the
 * start (root of the shown volume or share), C:\ and C:/, \\server\share and
 * //server/share, \\?\ extended paths, %VARIABLES%, @aliases (@home @desktop
 * @downloads @documents @pictures @music @videos @appdata) and - (the folder
 * before this one), wsl:<distribution> and sftp:<ssh host name>.
 * Drive-relative forms ("C:" and "C:name") are refused.
 *
 * Every component is checked like a file name (reserved device names,
 * forbidden and invisible characters, trailing dot or space); ".." never goes
 * above a drive root or a share root.
 */

typedef struct
{
	void *ctx;
	/* Each returns a malloc'd, NUL-terminated UTF-16 string, or NULL when the
	 * thing is not known. */
	wchar_t *(*home)(void *ctx);
	/* alias without the '@': "desktop", "downloads", ... */
	wchar_t *(*known)(void *ctx, const wchar_t *alias);
	wchar_t *(*getenv)(void *ctx, const wchar_t *name);
} WfSmartEnv;

#define WF_SMART_ALIASES 8
const wchar_t *wf_smart_alias_name(size_t index); /* "home" ... */

typedef enum
{
	WF_SG_OK,
	WF_SG_EMPTY,          /* nothing typed */
	WF_SG_ALIAS_PARTIAL,  /* "@d": candidates are aliases */
	WF_SG_INVALID,        /* not a usable path */
	WF_SG_DRIVE_RELATIVE, /* C: or C:name */
	WF_SG_ENV_UNDEFINED,  /* a %VARIABLE% that does not exist */
	WF_SG_NO_HOME,
	WF_SG_NO_PREVIOUS, /* "-" with nowhere to go back to */
	WF_SG_UNKNOWN_ALIAS,
	WF_SG_TOO_LONG,
	WF_SG_NOMEM,
	WF_SG_WSL_NAME, /* "wsl:" without a usable distribution name */
	WF_SG_SFTP_NAME /* "sftp:" without a usable ssh host name */
} WfSmartError;

typedef struct
{
	WfSmartError error;
	wchar_t name
	        [48]; /* ENV_UNDEFINED / UNKNOWN_ALIAS: the name, for the message */

	/* OK: where Enter goes. Absolute, backslashes, no trailing separator
	 * except on a root. */
	wchar_t *target;
	/* OK: the folder whose entries complete the last element, and the
	 * element's text so far (the search term). NULL when nothing can be
	 * listed (a share that is not named yet, for instance). */
	wchar_t *list_dir;
	/* ALIAS_PARTIAL: the text after '@'; otherwise the last element's prefix. */
	wchar_t *prefix;
	/* Where the last element starts in the input, and whether a separator has
	 * to be put before a completion (the input names a folder as a whole:
	 * "~", "@home", "C:\"). */
	size_t prefix_start;
	bool insert_separator;
	bool listed_as_whole; /* list_dir is the target itself (no prefix) */
} WfSmartResult;

void wf_smart_result_free(WfSmartResult *r);

/*
 * `cwd` is the folder being shown (absolute); `previous` the one before it
 * (NULL if none). `n` units of `text` are read; they need not be terminated.
 * WF_OK reports the input outcome in r->error (WF_SG_*), including some
 * allocation failures as WF_SG_NOMEM. WF_E_NOMEM reports later allocation
 * failures; WF_E_INVAL rejects a missing/empty cwd or NULL text with n > 0.
 */
WfStatus wf_smart_resolve(const WfSmartEnv *env,
        const wchar_t *cwd,
        const wchar_t *previous,
        const wchar_t *text,
        size_t n,
        WfSmartResult *r);

/* Short, plain text for an error ("Path not found" style): never NULL. */
const char *wf_smart_error_text(WfSmartError e);

/*
 * The input after completing the last element with `name`. `input[0..n)` is
 * the text, `cursor` the caret, `r` the resolution of that same text. A
 * folder gets a separator; `sep` is the one to use ('\\' or '/'). Writes a
 * malloc'd string to *out (and its length and the new caret). The part after
 * the caret is kept.
 */
WfStatus wf_smart_complete(const wchar_t *input,
        size_t n,
        size_t cursor,
        const WfSmartResult *r,
        const wchar_t *name,
        bool is_dir,
        wchar_t sep,
        wchar_t **out,
        size_t *out_n,
        size_t *out_cursor);

/* Start of the path element that ends at `cursor` ("a\b\c" with the caret at
 * the end: the start of "c"; with a trailing separator, the start of "b").
 * For Ctrl+Backspace. Never splits a root ("C:\", "\\srv\share\") or a
 * surrogate pair. */
size_t wf_smart_element_start(const wchar_t *input, size_t cursor);

/* Pasted text made safe for the input line: one matching pair of quotes and
 * surrounding white space removed, line breaks refused. WF_E_INVAL for
 * anything that is not a single line; the result is malloc'd.
 * NULL with n = 0 produces an empty string. */
WfStatus wf_smart_clean_paste(
        const wchar_t *text, size_t n, wchar_t **out, size_t *out_n);

#endif
