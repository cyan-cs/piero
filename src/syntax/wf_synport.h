#ifndef WF_SYNPORT_H
#define WF_SYNPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../base/wf_status.h"
#include "wf_syntax_abi.h"

/*
 * What the screen and the editor know of the syntax worker (DR-DRAFT-033): a
 * port of function pointers and the shape of an answer. Nothing here loads a
 * DLL; a zeroed port means "no colouring".
 */
#define WF_SYN_SLOTS 2
enum
{
	WF_SYN_SLOT_PREVIEW = 0,
	WF_SYN_SLOT_EDITOR = 1
};

typedef struct
{
	uint32_t slot, gen;
	uint32_t status; /* WfSynStatus */
	char lang[24];
	uint32_t text_len; /* the length of the text the runs are of */
	WfSynRun *
	        runs; /* malloc'd: wf_synanswer_free (the receiver may take it) */
	uint32_t nruns;
	uint32_t parse_us, query_us;
	uint32_t incremental;
} WfSynAnswer;

void wf_synanswer_free(WfSynAnswer *a);

/* request() colours `text` (copied) as `lang`, replacing the slot's older
 * request; close() ends the slot's session and drops what is pending;
 * installed() says, from attributes only, whether a grammar for `lang` is
 * installed (nothing is loaded to find out). */
typedef struct
{
	void *ctx;
	WfStatus (*request)(void *ctx,
	        uint32_t slot,
	        uint32_t gen,
	        const char *lang,
	        const char *text,
	        size_t len);
	void (*close)(void *ctx, uint32_t slot);
	bool (*installed)(void *ctx, const char *lang);
} WfSynPort;

#endif
