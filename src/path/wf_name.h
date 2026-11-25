#ifndef WF_NAME_H
#define WF_NAME_H
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

typedef enum
{
	WF_NAME_OK,
	WF_NAME_EMPTY,
	WF_NAME_TOO_LONG,
	WF_NAME_INVALID,
	WF_NAME_RESERVED
} WfNameResult;
/* One raw UTF-16 component, explicit length (embedded NUL is invalid).
 * This is validation, never normalization or permission to overwrite. */
WfNameResult wf_name_validate(
        const wchar_t *name, size_t units, uint32_t max_component);
/* A new name that may name folders on the way: "a/b/c" is c inside b inside a.
 * Both '/' and '\' separate. A relative path of at most WF_NAME_PATH_DEPTH
 * components; each one is valid by wf_name_validate (so "." and ".." never
 * are), and none is empty (no leading, trailing or doubled separator). */
#define WF_NAME_PATH_DEPTH 16
WfNameResult wf_name_validate_path(
        const wchar_t *path, size_t units, uint32_t max_component);
/* The first component of `path` (its length is returned); *rest is the text
 * after the separator that ends it, or NULL when it is the last component. */
size_t wf_name_split_first(const wchar_t *path, const wchar_t **rest);
const char *wf_name_reason(WfNameResult result);
#endif
