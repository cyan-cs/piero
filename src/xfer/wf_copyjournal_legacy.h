#ifndef WF_COPYJOURNAL_LEGACY_H
#define WF_COPYJOURNAL_LEGACY_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Reads a copy journal WITHOUT the "PIERO-COPY-RECEIPTS 2" header as data and
 * says what its own records prove. Two eras are headerless: the first one
 * (I C V G) and a later one that already used the current record kinds but
 * had no header yet (I C A V G D). It touches no file and no path named in
 * the records: a journal is an untrusted inventory, never authority to delete
 * or rename anything.
 *
 * Line: "K pid start id path" ended by a newline, with K one of
 *   I  intent: a temporary is about to be made (no id yet)
 *   C  created: the temporary exists, with its file id
 *   A  aside: an overwritten file was set aside; path is "<dest>|<aside>",
 *      id is the id of the OLD file
 *   V  verified: its content was read back
 *   D  discard: the set-aside old file is to be dropped (an intent only)
 *   G  gone: the file with this id no longer exists; resolves every EARLIER
 *      record with the same id.
 *
 * complete_proven is true only when the records alone close everything they
 * opened: no malformed line, no unterminated last line, every C and every A
 * followed by a G with its id, every I followed by a C of the same path. A
 * "D" alone closes nothing (it states an intent, not a result). It says
 * nothing about the files themselves; the caller must not act on it.
 */
typedef struct
{
	size_t bytes, lines;
	size_t intents, created, asides, verified, discards, gone;
	size_t malformed;          /* lines that are not a record */
	size_t unresolved_created; /* C with no later G */
	size_t unresolved_asides;  /* A with no later G */
	size_t unresolved_intents; /* I with no later C of the same path */
	bool unterminated_tail;    /* the last line has no newline */
	bool empty;
	bool complete_proven;
} WfLegacyJournalReport;

/* False only for invalid arguments. Never fails on odd content. */
bool wf_cj_legacy_analyze(
        const char *data, size_t n, WfLegacyJournalReport *out);

#endif
