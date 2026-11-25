#ifndef WF_INI_EDIT_H
#define WF_INI_EDIT_H

#include <stddef.h>

#include "base/wf_status.h"

/*
 * Changing a few settings in an INI text without disturbing the rest of it.
 * Comments, blank lines, unknown keys and the order of everything else stay as
 * they are; only the named keys are replaced, added to their section, or (with
 * their section) added at the end. The line-ending style of the text is kept
 * (LF for a first bare LF, otherwise CRLF). Pure text in, text out:
 * reading and writing the file is the caller's business.
 */
typedef struct
{
	const char *section;
	const char *key;
	const char *
	        value; /* NULL: remove the key (a missing key is not an error) */
} WfIniSet;

/* Optional section-alias matcher. `section` is a borrowed slice;
 * target/key are NUL-terminated. The same matcher is used for replacements
 * and removals; new sections always use the target's canonical spelling. */
typedef bool (*WfIniSectionMatch)(const char *section,
        size_t section_n,
        const char *target,
        const char *key);

/* `text` may be empty. Empty values/keys/sections are invalid (NULL removes a key).
 * A value, key or section name with a control character,
 * or a key/section containing '=', '[' or ']', is WF_E_INVAL and nothing is
 * produced. All occurrences of a named key across repeated matching sections
 * are removed or replaced by one canonical assignment at the last occurrence.
 * This also supports readers whose duplicate policy keeps the first value.
 * On success *out is malloc'd,
 * NUL-terminated; a nonempty result ends with a line ending. */
WfStatus wf_ini_apply(const char *text,
        size_t n,
        const WfIniSet *sets,
        size_t count,
        char **out,
        size_t *out_n);

WfStatus wf_ini_apply_matching(const char *text,
        size_t n,
        const WfIniSet *sets,
        size_t count,
        WfIniSectionMatch match,
        char **out,
        size_t *out_n);

#endif
