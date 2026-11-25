#ifndef WF_ARCHIVE_H
#define WF_ARCHIVE_H

#include <windows.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../fs/wf_dir.h"
#include "wf_vpath.h"

/*
 * Reading archives (ZIP, 7z, TAR, and single gzip / xz / zstd streams) so that
 * they can be browsed like folders. Read only: nothing here writes an
 * archive. ZIP extraction uses the separate sink API declared below.
 *
 * What a caller sees
 *   - A path inside an archive is a "virtual path" (wf_vpath.h). Nothing in
 *     this module ever turns one into a file-system path.
 *   - wf_archive_list() answers "what is in this folder of the archive" from
 *     an index. Building the index reads the archive once (headers only; a
 *     stream compressed with gzip, xz or zstd has to be decompressed to get
 *     past its entries, which is bounded below). The index is kept, so moving
 *     around inside the archive costs no further reading.
 *   - wf_archive_read() returns the first bytes of one file in the archive
 *     for the preview. It never returns more than the caller asked for.
 *
 * Ownership and threads
 *   - An index (WfArchive) is immutable once built, reference counted, and
 *     shared by every thread. wf_archive_open() returns a counted reference;
 *     wf_archive_release() gives it back. The process keeps a few of them in
 *     a cache scoped to its path and observed identity, size and write time.
 *     A lookup re-reads those from the open file before reuse. A rewrite that
 *     preserves these values, or happens after the check, can evade detection.
 *   - A libarchive reader is private to one call: it is created, used and
 *     freed inside a function and never shared between threads.
 *   - Every call can be given a WfArchiveCtl; cancelled() is polled at every
 *     block read from the file and at every entry. A cancelled call returns
 *     WF_E_CANCELLED and releases its temporary index/data. Cancellation is
 *     cooperative: it can wait for libarchive to finish processing buffered
 *     input. Extraction may already have handed partial data to its sink.
 *
 * The archive is untrusted input. Limits (see the .c file for the numbers):
 * entries, name length and depth, total name bytes, bytes of a compressed
 * stream decompressed to index it, and bytes returned to a preview. Names are
 * normalised into a namespace where every component is a plain name; entries
 * that cannot be (".." components) are skipped and counted, never mapped to
 * somewhere else.
 */

typedef struct
{
	bool (*cancelled)(void *ctx);
	void *ctx;
} WfArchiveCtl;

typedef struct WfArchive WfArchive;

typedef enum
{
	WF_AF_ZIP,
	WF_AF_7Z,
	WF_AF_TAR,
	WF_AF_SINGLE /* one gzip / xz / zstd compressed file */
} WfArchiveFormat;

typedef struct
{
	WfArchiveFormat format;
	char filters[48]; /* e.g. "gzip", "xz", "zstd"; "" for none */
	size_t entries;   /* nodes in the index, folders included */
	size_t skipped; /* entries left out: unsafe names, conflicts, limits */
	bool seekable;  /* ZIP: entries are reached without decompressing the
	                   ones before */
	size_t bytes;   /* memory held by the index */
} WfArchiveInfo;

/* The code page used for names that are not UTF-8 (0: the system's). Call
 * once at start-up; ZIP files made by older Windows tools carry such names. */
void wf_archive_set_codepage(UINT codepage);

/* Limits on what one archive may cost to index: nodes (folders included),
 * physical headers (including skipped/duplicate names), and stream scan bytes.
 * The entry limit applies to both node and header counts. 0 keeps the default
 * (500000, 8 GiB). Tests lower them. */
void wf_archive_set_limits(size_t max_entries, uint64_t max_scan_bytes);

/* Cache budget (0: the default). Installing an index evicts older idle indexes
 * to fit; the newly installed index is kept even if it alone exceeds the budget.
 * Tests use a small value. */
void wf_archive_set_cache_limit(size_t bytes);

/* Counted reference to the index of the archive file `file` (a real path).
 * Builds the index if it is not cached. Errors: WF_E_NOENT, WF_E_ACCESS,
 * WF_E_FORMAT (not an archive piero reads), WF_E_CORRUPT, WF_E_ENCRYPTED,
 * WF_E_RANGE (over a limit), WF_E_CHANGED (rewritten while it was read),
 * WF_E_CANCELLED, WF_E_NOMEM, WF_E_OS (other read/seek failures). Format,
 * corruption, encryption and limit failures are cached for the same stamp;
 * operational failures are retried on the next open. */
WfStatus wf_archive_open(
        const wchar_t *file, const WfArchiveCtl *ctl, WfArchive **out);
void wf_archive_release(WfArchive *a);
WfArchiveInfo wf_archive_info(const WfArchive *a);

/* The folder `inner` ("" is the top; names separated by '\') as a listing
 * the browser can show. Entries carry WF_ENT_DIR / WF_ENT_LINK (a link is
 * listed, never followed). WF_E_NOENT when there is no such folder. */
WfStatus wf_archive_list(
        const WfArchive *a, const wchar_t *inner, size_t limit, WfDir **out);

/* Reads whatever the virtual path `vpath` names, in one step:
 * index + list. */
WfStatus wf_vpath_list(const wchar_t *vpath,
        size_t limit,
        const WfArchiveCtl *ctl,
        WfDir **out);

typedef struct
{
	uint8_t *bytes; /* malloc'd; the first n bytes of the file */
	size_t n;
	bool truncated;  /* the file has more than was returned */
	bool size_known; /* size below is the entry's own */
	uint64_t size;
	WfArchiveFormat format;
	bool verified; /* complete read with no error reported by the format reader,
	                  including any checksum it enforces; false when truncated */
} WfArchiveData;

void wf_archive_data_free(WfArchiveData *d);

#define WF_ARCHIVE_READ_MAX (1024u * 1024u) /* what a text preview asks */
/* The most one entry is read for: a picture or a PDF page must be whole. */
#define WF_ARCHIVE_ENTRY_MAX (32u * 1024u * 1024u)
/* The first `max_bytes` (at most WF_ARCHIVE_ENTRY_MAX) of the file named by
 * `vpath`. WF_E_NOENT / WF_E_INVAL (a folder) / WF_E_ACCESS (a link or a
 * special entry) / WF_E_ENCRYPTED / WF_E_CHANGED (the archive is no longer
 * the one that was indexed, or the entry is not what the index says) plus the
 * errors of wf_archive_open(). */
WfStatus wf_vpath_read(const wchar_t *vpath,
        size_t max_bytes,
        const WfArchiveCtl *ctl,
        WfArchiveData *out);

/*
 * Extraction (ZIP only). The archive is read once, front to back, and what
 * the caller selected is handed to a sink as a stream. The sink creates and
 * writes output files; this module never buffers a whole extracted file.
 *
 * What is extracted is what the browser shows: the selection is a set of
 * paths inside the archive (a file, a folder with everything under it, or ""
 * for the whole archive, whose top-level items are then the items), and an
 * item keeps its place under its own name ("a\b\c.txt" selected extracts as
 * "c.txt"; folder "a\b" extracts as "b\...").
 *
 * A name is checked again here, strictly, before anything is written. An
 * entry is not extracted, and is reported to the sink as an issue, when its
 * name is absolute (a leading separator, a drive, a UNC prefix), has a ".."
 * component, a character Windows does not allow in a name, a component that
 * ends in '.' or ' ', a reserved device name (CON, NUL, COM1 ... also with an
 * extension), a replacement character (a name that was not valid text), or is
 * too long; when it differs only in letter case from another entry that was
 * taken (Windows would put both in one place); when it is a link or needs a
 * password; and when its data cannot be trusted (checksum, size).
 * Entries whose names the index could not place at all (".." and the like,
 * counted in WfArchiveInfo.skipped) are never extracted.
 *
 * Limits are enforced on the bytes that really come out of the archive, not
 * on the sizes it declares (a declared size is only used to refuse early).
 */
typedef enum
{
	WF_EX_CONTINUE, /* go on */
	WF_EX_SKIP,     /* give this entry up (the sink has said why) */
	WF_EX_ABORT     /* stop the whole extraction */
} WfExtractAct;

typedef enum
{
	WF_XI_NAME,      /* unsafe or invalid name */
	WF_XI_COLLISION, /* the same name once case is ignored */
	WF_XI_LINK,      /* a link or a special entry */
	WF_XI_ENCRYPTED, /* needs a password */
	WF_XI_TOO_BIG,   /* over the limit for one file */
	WF_XI_CHECKSUM,  /* the data does not match its checksum */
	WF_XI_SIZE,      /* the data is not the size the entry says */
	WF_XI_READ,      /* the archive could not be read for this entry */
	WF_XI_WRITE      /* the sink could not take it */
} WfExtractIssue;

typedef struct
{
	uint64_t max_file_bytes;  /* one file */
	uint64_t max_total_bytes; /* all of it */
	size_t max_files;         /* files and folders taken */
	size_t max_name_bytes;    /* memory for the names it has to remember */
} WfExtractLimits;

/* The defaults: 16 GiB a file, 64 GiB in all, 200000 entries, 64 MiB of names. */
WfExtractLimits wf_extract_default_limits(void);

typedef struct
{
	void *ctx;
	/* A folder. `rel` is the path below the destination, parts joined by '\'. */
	WfExtractAct (*dir)(void *ctx, const wchar_t *rel, uint64_t mtime);
	/* A file begins: `size` is the entry's own (size_known), or not. */
	WfExtractAct (*file_begin)(void *ctx,
	        const wchar_t *rel,
	        uint64_t size,
	        bool size_known,
	        uint64_t mtime);
	WfExtractAct (*file_data)(void *ctx, const void *p, size_t n);
	/* The file is over; complete is false when it is to be thrown away. */
	void (*file_end)(void *ctx, bool complete);
	/* An entry was not taken. */
	void (*issue)(void *ctx, const wchar_t *rel, WfExtractIssue kind);
	bool (*cancelled)(void *ctx);
} WfExtractSink;

typedef struct
{
	size_t files, dirs, issues;
	uint64_t bytes;
	size_t unplaced; /* entries of the archive that could never be named */
	size_t declared_files, declared_dirs; /* what the selection holds */
	uint64_t declared_bytes;
} WfExtractStats;

/* Counts what a selection holds, from the index alone (nothing is read):
 * folders and files below the items, and the sizes the entries declare.
 * WF_E_NOENT when an item is not in the archive; WF_E_FORMAT unless ZIP. */
WfStatus wf_archive_extract_measure(const wchar_t *file,
        const wchar_t *const *inner,
        size_t n_inner,
        const WfArchiveCtl *ctl,
        WfExtractStats *out);

/* Extracts. WF_OK: every selected entry was taken or reported; anything else
 * (cancelled, changed, over a limit, unreadable): the run stopped, what was
 * handed to the sink so far is partial and the caller must discard it. */
WfStatus wf_archive_extract(const wchar_t *file,
        const wchar_t *const *inner,
        size_t n_inner,
        const WfExtractLimits *limits,
        const WfExtractSink *sink,
        const WfArchiveCtl *ctl,
        WfExtractStats *out);

/* Drops every cached index (tests, and after a settings change). */
void wf_archive_cache_clear(void);

/* Idle release (DR-DRAFT-037). The cache remembers when it was last used: by
 * an open, a hit, or a touch. A caller that knows the archive is still on the
 * screen touches it, so the time counts from when the screen left it.
 * wf_archive_cache_idle_wait() says how long until the cache has been unused
 * for idle_ms: 0 when it has been (the caller then clears it), UINT64_MAX when
 * nothing is kept. A reference already handed out is not affected by a clear. */
void wf_archive_cache_touch(uint64_t now_ms);
uint64_t wf_archive_cache_idle_wait(uint64_t now_ms, uint64_t idle_ms);

#endif
