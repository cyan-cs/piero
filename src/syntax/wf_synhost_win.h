#ifndef WF_SYNHOST_WIN_H
#define WF_SYNHOST_WIN_H

#include <stdbool.h>
#include <stdint.h>
#include <windows.h>

#include "wf_synlang.h"
#include "wf_syntax_abi.h"

/*
 * piero.exe's side of syntax\piero-syntax.dll (DR-DRAFT-033): finds out
 * cheaply whether a language is installed, and loads the DLL only when
 * something has to be coloured. Nothing here is linked to Tree-sitter.
 *
 * The DLL is loaded once, by the syntax worker's thread, after it has been
 * checked against the SHA-256 that was built into piero.exe, and stays loaded
 * until wf_synhost_shutdown is called after the worker has stopped.
 */

/* `root` is the folder that holds "syntax" (NULL: the folder of piero.exe);
 * `pin_hex` is the SHA-256 of piero-syntax.dll (64 hex digits). Called once,
 * before anything else; the program passes WF_SYNTAX_DLL_SHA256. A host that
 * was never configured refuses to load anything. */
void wf_synhost_configure(const wchar_t *root, const char *pin_hex);

/* True when the folder of `lang` has parser.dll, highlights.scm and
 * manifest.toml and piero-syntax.dll is there: attribute reads only, nothing is
 * loaded or opened for longer than that. A built-in language (.env) is always
 * installed. */
bool wf_synhost_installed(const WfSynLang *lang);

/* A session for `lang_id`; loads piero-syntax.dll first when needed. */
uint32_t wf_synhost_open(
        const char *lang_id, const WfSynLimits *limits, WfSynSession **out);
uint32_t wf_synhost_highlight(
        WfSynSession *s, const char *text, size_t len, WfSynResult *out);
void wf_synhost_result_free(WfSynResult *r);
void wf_synhost_close(WfSynSession *s);

/* Counters of the DLL (0 when it is not loaded). */
uint64_t wf_synhost_stat(uint32_t which);
bool wf_synhost_loaded(void);

/* Releases the grammars and the DLL. Every session must have been closed and
 * no thread may be inside the DLL. A no-op when nothing is loaded. */
void wf_synhost_shutdown(void);

#endif
