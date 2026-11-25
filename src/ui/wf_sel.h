#ifndef WF_SEL_H
#define WF_SEL_H

#include <stddef.h>
#include <stdint.h>

#include "../base/wf_status.h"
#include "../fs/wf_dir.h"

/*
 * Selections are remembered per folder and by entry name, so they survive
 * reloads, filtering and hidden-toggling (spec 2.3). Folder paths are
 * compared exactly (ordinal): callers always derive them the same way.
 *
 * A name set is a sorted array of owned strings (binary search, ordinal).
 */
typedef struct
{
	wchar_t **names;
	size_t n, cap;
} WfNameSet;

typedef struct
{
	wchar_t *path;
	WfNameSet set;
} WfSelFolder;

typedef struct
{
	WfSelFolder *f;
	size_t n, cap;
	uint32_t
	        gen; /* process-wide change counter (UI thread); wraps as uint32_t */
} WfSelStore;

void wf_sel_init(WfSelStore *s);
void wf_sel_free(WfSelStore *s);

/* NULL when nothing is selected in `dir`. Borrowed; invalidated by any
 * mutation of the store. */
const WfNameSet *wf_sel_folder(const WfSelStore *s, const wchar_t *dir);

bool wf_nameset_has(const WfNameSet *set, const wchar_t *name);
size_t wf_sel_count(const WfSelStore *s, const wchar_t *dir);

WfStatus wf_sel_toggle(WfSelStore *s, const wchar_t *dir, const wchar_t *name);
void wf_sel_clear(WfSelStore *s, const wchar_t *dir);

/* Forgets selected names that no longer exist in the freshly listed `d`. */
void wf_sel_prune(WfSelStore *s, const wchar_t *dir, const WfDir *d);

#endif
