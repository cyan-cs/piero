#ifndef WF_INFO_H
#define WF_INFO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../fs/wf_dir.h"
#include "../fs/wf_ntfsfacts.h"
#include "wf_sel.h"

/*
 * Details of the entry under the cursor plus the selection summary. Pure
 * formatting: nothing here touches the disk. Anything the listing did not
 * report is printed as "unknown", never as a default value.
 */
#define WF_INFO_LINES 48
#define WF_INFO_LINE_BYTES 192

enum
{
	WF_RECOVERY_NOTICE_STORE = 1u << 0,
	WF_RECOVERY_NOTICE_PENDING = 1u << 1,
	WF_RECOVERY_NOTICE_LEGACY = 1u << 2,
	WF_RECOVERY_NOTICE_COPY_INVALID = 1u << 3,
	WF_RECOVERY_NOTICE_LIMIT = 1u << 4,
	WF_RECOVERY_NOTICE_OLD_FOLDER = 1u
	        << 5 /* records left in the older folder */
};

typedef struct
{
	char text[WF_INFO_LINES][WF_INFO_LINE_BYTES];
	int n;
	size_t scroll; /* wrapped display rows; refreshed on explicit Info */
} WfInfo;

/* Optional UTC -> local conversion; NULL (or a failing converter returning
 * 0) means times are shown in UTC and labelled so. */
typedef WfFileTime (*WfLocalTime)(WfFileTime utc);

/* `entry` may be NULL (empty directory). `dir`/`sel` supply the selection
 * summary (sel may be NULL). */
void wf_info_build(WfInfo *out,
        const WfEntry *entry,
        const WfDir *dir,
        const WfNameSet *sel,
        WfLocalTime local);

/* The same, with what `D` counted for the folder under the cursor written in
 * its Size line (`folder_total` NULL: nothing was counted, the line says
 * so). */
void wf_info_build_counted(WfInfo *out,
        const WfEntry *entry,
        const WfDir *dir,
        const WfNameSet *sel,
        WfLocalTime local,
        const char *folder_total);

/* The same, with the facts of DR-DRAFT-026 (link target, names, streams, volume,
 * how it is stored) after the link line. `facts` NULL, or a state of
 * WF_NF_NOT_ASKED: nothing is added. */
void wf_info_build_facts(WfInfo *out,
        const WfEntry *entry,
        const WfDir *dir,
        const WfNameSet *sel,
        WfLocalTime local,
        const char *folder_total,
        const WfNtfsFacts *facts);

/* The same, with the SHA-256 line (DR-DRAFT-027): `hash` is the digest in hex
 * or a word for "computing"; NULL: no line. */
void wf_info_build_full(WfInfo *out,
        const WfEntry *entry,
        const WfDir *dir,
        const WfNameSet *sel,
        WfLocalTime local,
        const char *folder_total,
        const WfNtfsFacts *facts,
        const char *hash);

/* Localized size text, e.g. "12,345 bytes (12.05 KiB)". NUL-terminated when
 * out is non-NULL and cap > 0; otherwise writes nothing. */
void wf_info_format_size(uint64_t bytes, char *out, size_t cap);

/* "2026-10-22 12:34:56" for a FILETIME; "unknown" for 0 and "out of range"
 * beyond year 9999. */
void wf_info_format_time(WfFileTime t, char *out, size_t cap);

/* Words for the attribute bits, e.g. "ReadOnly Hidden Archive"; "none" when
 * no bit we name is set. */
void wf_info_format_attributes(uint32_t attributes, char *out, size_t cap);

/* "symbolic link", "junction", ... or "reparse point 0x%08X". */
void wf_info_format_reparse(uint32_t tag, char *out, size_t cap);

#endif
