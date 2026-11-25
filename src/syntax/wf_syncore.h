#ifndef WF_SYNCORE_H
#define WF_SYNCORE_H

#include <stddef.h>
#include <wchar.h>

#include "wf_syntax_abi.h"

/*
 * What is inside syntax\piero-syntax.dll (DR-DRAFT-033): the grammar loader
 * and the sessions, on top of the Tree-sitter runtime. It is a separate
 * source file so that tests can link it directly with their own trust table
 * and fixtures; the DLL exports none of this, only wf_syntax_api.
 */

/* What the official release looks like: the SHA-256 (64 hex digits) of the
 * three files of a grammar. Anything that does not match is refused. */
typedef struct
{
	const char *id;
	const char *dll, *query, *manifest;
} WfSynTrust;

/* `dir` holds the folder "grammars"; `table` is the trust table. Called once,
 * before the first open (the DLL does it from wf_syntax_api with its own
 * folder and the table built into it). */
void wf_syncore_configure(
        const wchar_t *dir, const WfSynTrust *table, size_t n);

const WfSyntaxApi *wf_syncore_api(void);

#endif
