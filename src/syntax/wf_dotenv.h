#ifndef WF_DOTENV_H
#define WF_DOTENV_H

#include <stddef.h>
#include <stdint.h>

#include "wf_syntax_abi.h"

/*
 * The classes of a .env file (DR-DRAFT-033): a lexer inside piero.exe, not a
 * grammar DLL. It classifies NAME=value lines, # comments, quotes and
 * $VARIABLE without evaluating them or changing the text.
 *
 * Runs follow wf_syntax_abi.h (byte offsets, sorted, no overlap); the array
 * is malloc'd and the caller frees it. Limits as for the grammars.
 */
uint32_t wf_dotenv_highlight(const char *text,
        size_t len,
        const WfSynLimits *limits,
        WfSynRun **runs,
        uint32_t *nruns);

#endif
