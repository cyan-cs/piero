#ifndef WF_VPATH_H
#define WF_VPATH_H

#include <stdbool.h>
#include <stddef.h>

#include "../base/wf_status.h"

/*
 * Paths inside an archive. The app keeps one path string per folder; for a
 * folder inside an archive it is
 *
 *     <path of the archive file> '|' [ '\' <name> { '\' <name> } ]
 *
 *     C:\dl\p.zip|                  the top of p.zip
 *     C:\dl\p.zip|\src\main.c       a file in it
 *
 * '|' marks the archive boundary. Callers must route marked paths through
 * the archive API before any filesystem I/O. Indexing splits names into
 * components and replaces C0/DEL controls and Windows-reserved characters
 * (see wf_archive.h), so components can use the ordinary path join/split
 * functions. This module only looks at strings.
 */
#define WF_VPATH_MARK L'|'

/* The string is a path inside an archive. */
bool wf_vpath_is(const wchar_t *path);

/* Splits a path inside an archive. *archive is a copy (free()) of the part
 * before the mark; *inner points into `path` at the first name after the mark
 * (an empty string at the top). False for a real path. */
bool wf_vpath_split(
        const wchar_t *path, wchar_t **archive, const wchar_t **inner);

/* The top of the archive file `file`: file + '|'. */
WfStatus wf_vpath_root(const wchar_t *file, wchar_t **out);

/* Length of the real part: everything before the mark. */
size_t wf_vpath_archive_len(const wchar_t *path);

/* The last component of a path for display or selection purposes: for the top
 * of an archive that is the archive file's own name, without the mark. */
void wf_vpath_leaf(const wchar_t *path, const wchar_t **leaf, size_t *len);

/* The name looks like something piero opens as an archive (decided by the
 * extension only; the content decides whether it really is one). */
bool wf_archive_name_recognized(const wchar_t *name);

#endif
