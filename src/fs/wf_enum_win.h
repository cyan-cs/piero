#ifndef WF_ENUM_WIN_H
#define WF_ENUM_WIN_H

#include "wf_dir.h"

/*
 * Lists `path` (absolute UTF-16) into a sorted snapshot. Matches WfListDirFn.
 * On success *out is owned by the caller (wf_dir_free). "." and ".." are
 * omitted. Access denied and missing paths are distinct statuses; an empty
 * directory is WF_OK with n == 0.
 */
WfStatus wf_enum_dir_win(const wchar_t *path, WfDir **out);
/* Same, but keeps at most `limit` entries (the snapshot is then marked
 * truncated and the enumeration stops early). 0 means the usual maximum. */
WfStatus wf_enum_dir_win_limited(
        const wchar_t *path, size_t limit, WfDir **out);

WfStatus wf_enum_dir_win_budget(
        const wchar_t *path, size_t limit, size_t bytes, WfDir **out);

WfStatus wf_enum_dir_win_controlled(const wchar_t *path,
        size_t limit,
        size_t bytes,
        bool (*cancel)(void *),
        void *ctx,
        WfDir **out);

/* The same, ordered as asked in the one sort (`wf_dir_sort_with`): nothing is
 * sorted by name first and again afterwards. The order is the same as that of a
 * name sort followed by the other, because ties are broken by name. */
WfStatus wf_enum_dir_win_ordered(const wchar_t *path,
        size_t limit,
        size_t bytes,
        WfSortKey key,
        bool descending,
        bool dirs_first,
        bool (*cancel)(void *),
        void *ctx,
        WfDir **out);

/* The same listing (sorted by name, with identities) for a walk that only reads
 * the names and the metadata, as the search does: the entries have no display
 * text (WfEntry.display is NULL) and the unlimited form is the only one. */
WfStatus wf_enum_names_win(
        const wchar_t *path, bool (*cancel)(void *), void *ctx, WfDir **out);

#endif
