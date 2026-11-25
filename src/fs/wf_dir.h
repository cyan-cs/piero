#ifndef WF_DIR_H
#define WF_DIR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../base/wf_status.h"

/* Windows FILETIME: 100 ns ticks since 1601-01-01 UTC. */
typedef uint64_t WfFileTime;

/* Entries kept per directory; beyond this the snapshot is marked truncated. */
#define WF_DIR_MAX_ENTRIES 500000u

/* What an identity is. The kind is kept with the identity, and what is done with
 * it is read from the kind: it is not worked out again from the bytes.
 *  - STRONG: a 128-bit FileId of the volume, proven;
 *  - WEAK: a FAT or exFAT volume has no FileId; the volume serial and the 64-bit
 *    directory-entry index stand in (wf_weakid_win.h). It can repeat after a
 *    delete, it changes when an entry is renamed, and two cloned sticks can share
 *    a serial: it is never the authority for removing or replacing anything;
 *  - NONE: nothing is known, and nothing may be done on it.
 * Invalid identities never authorize mutations. Everything that asks for a
 * proven identity refuses a weak one; only the operations that leave what exists
 * as it is (reading, opening, creating a new name) also take a weak one. The
 * zero value is NONE. */
typedef enum
{
	WF_ID_NONE = 0,
	WF_ID_STRONG,
	WF_ID_WEAK
} WfIdKind;

typedef struct
{
	uint64_t volume;
	uint8_t id[16];
	WfIdKind kind;
} WfFileId;

static inline bool wf_fileid_proven(const WfFileId *id)
{
	return id && WF_ID_STRONG == id->kind;
}

static inline bool wf_fileid_is_weak(const WfFileId *id)
{
	return id && WF_ID_WEAK == id->kind;
}

enum
{
	WF_ENT_DIR = 1u << 0,
	WF_ENT_HIDDEN = 1u << 1, /* hidden or system attribute */
	WF_ENT_LINK = 1u << 2, /* reparse point (symlink, junction, cloud...) */
	WF_ENT_CLOUD = 1u << 3,  /* recall-on-open/-data-access or offline:
	                            content is not local */
	WF_ENT_ARCHIVE = 1u << 4 /* a file whose name says it is an archive that
	                            can be entered like a folder (set by
	                            wf_dir_add from the name alone) */
};

typedef struct
{
	wchar_t *name; /* owned, raw; the only form used to open the entry */
	char *display; /* owned, sanitized UTF-8; the only form drawn (NULL in a
	                  listing made with `no_display`) */
	uint64_t size;
	WfFileTime mtime;    /* only ordering matters here */
	WfFileTime ctime;    /* valid only when meta_known */
	uint32_t attributes; /* raw Win32 FILE_ATTRIBUTE_*; valid only when
	                        meta_known */
	bool meta_known;     /* ctime/attributes came from the OS listing; false
	                        means unknown, never "zero" */
	uint32_t flags;
	uint32_t reparse_tag; /* meaningful only with WF_ENT_LINK */
	WfFileId identity;    /* captured by the same enumeration as the name */
} WfEntry;

/* An immutable-after-load snapshot; UI state (cursor...) lives elsewhere. */
typedef struct
{
	WfEntry *e; /* owned */
	size_t n, cap;
	size_t owned_bytes; /* struct, reserved entries and allocated string capacities */
	size_t byte_limit; /* zero: ordinary enumeration; otherwise fail before exceeding */
	bool truncated;
	WfFileId
	        identity; /* directory handle used for enumeration, if proven */
	size_t limit;     /* entries kept; 0 means WF_DIR_MAX_ENTRIES */
	bool no_display; /* set before entries are added: no display text is made,
	                    for a walk that reads names and never draws them */
} WfDir;

WfDir *wf_dir_new(void);
void wf_dir_free(WfDir *d); /* NULL-safe */

/* Copies `name` and derives display text unless no_display is set. NULL/0 is
 * an empty name. At d->limit (or WF_DIR_MAX_ENTRIES when zero), sets truncated
 * and returns WF_OK without adding an entry. */
WfStatus wf_dir_add(WfDir *d,
        const wchar_t *name,
        size_t name_len,
        uint32_t flags,
        uint64_t size,
        WfFileTime mtime);
/* Records what the OS reported for entry `index` (the one just added).
 * Entries without this call stay "unknown". */
void wf_dir_set_meta(
        WfDir *d, size_t index, WfFileTime ctime, uint32_t attributes);
WfStatus wf_dir_add_ex(WfDir *d,
        const wchar_t *name,
        size_t name_len,
        uint32_t flags,
        uint64_t size,
        WfFileTime mtime,
        uint32_t reparse_tag);

typedef enum
{
	WF_SORT_NAME,
	WF_SORT_SIZE,
	WF_SORT_DATE,
	WF_SORT_EXT
} WfSortKey;

/* Directories first, then `key`, then natural name, then ordinal name. A
 * descending sort reverses everything except directories-first. */
WfStatus wf_dir_sort(WfDir *d, WfSortKey key, bool descending);

/* The same, but with `dirs_first` false the directories are not set apart:
 * folders and files are ordered together by `key` (`[files] directories_first`). */
WfStatus wf_dir_sort_with(
        WfDir *d, WfSortKey key, bool descending, bool dirs_first);

/* Natural order: digit runs compare numerically, ASCII case is folded.
 * Equal results here are broken by ordinal comparison in wf_dir_sort. */
int wf_natcmp(const wchar_t *a, const wchar_t *b);

#endif
