#ifndef WF_SLOTFILE_H
#define WF_SLOTFILE_H

#include <windows.h>

#include <stdbool.h>
#include <stddef.h>

#include "wf_status.h"

/*
 * Alternating slot files for drafts and saved sessions. An interrupted write
 * can leave the other slot available; this is not a durability guarantee.
 *
 * A record is a header and a body:
 *
 *     <magic>\n  <before-seq lines>  seq N\n  <after-seq lines>  size M\n--\n<body>
 *
 * A write goes to the slot that holds the older record (or one that is not
 * whole), so the other slot keeps a complete record while this one is being
 * written. A slot whose size does not match its header was cut short and is
 * ignored. Nothing is ever renamed over an existing name (CI-NOCLOBBER-01) and
 * nothing is deleted here: whoever owns the files removes them.
 */
typedef struct
{
	bool whole; /* the file is there, its magic is right and its size matches */
	unsigned long long seq;
	char *header; /* malloc'd, NUL-terminated: the lines before the body */
	char *body;   /* malloc'd, NUL-terminated after body_n bytes */
	size_t body_n;
} WfSlotFile;

/* Reads one slot file with a body of at most `max_bytes` (plus bounded header
 * overhead). Missing, undersized, oversized, wrong-magic or cut-short files
 * return whole = false with nothing allocated. */
void wf_slotfile_read(const wchar_t *file,
        const char *magic,
        size_t max_bytes,
        WfSlotFile *out);
void wf_slotfile_free(WfSlotFile *s);

/* The slot a reader should trust: the whole one with the higher seq, or -1. */
int wf_slotfile_pick(const WfSlotFile *a, const WfSlotFile *b);

/* Empties a slot file (a session or a draft that is to be forgotten). It is
 * changed only when the handle that is opened shows a plain file with no other
 * name: a link, or a second name of another file, is refused (WF_E_ACCESS) and
 * what it leads to is left as it was. A file that is not there is not an
 * error. */
WfStatus wf_slotfile_empty(const wchar_t *file);

/* Writes `body` as the next record: seq is one above the newest whole slot,
 * the target is the one that is not whole, else the older. `before_seq` and
 * `after_seq` are whole header lines ("path x\n") or NULL. The file is flushed
 * before this returns. `max_bytes` is the limit a reader will accept; a larger
 * record is refused (WF_E_RANGE). */
WfStatus wf_slotfile_write(const wchar_t *const file[2],
        const char *magic,
        const char *before_seq,
        const char *after_seq,
        const void *body,
        size_t n,
        size_t max_bytes);

#endif
