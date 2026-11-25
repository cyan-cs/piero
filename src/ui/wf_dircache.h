#ifndef WF_DIRCACHE_H
#define WF_DIRCACHE_H

#include "wf_app.h"

#define WF_DIRCACHE_BYTES (8u << 20)
#define WF_DIRCACHE_SLOTS 9u
#define WF_DIRCACHE_TTL 2000u

typedef struct
{
	wchar_t *path;
	WfFileId id;
	WfDir *dir;
	uint64_t loaded;
	bool attempted;
} WfDirCacheEntry;

/* UI thread only. A snapshot belongs either to a slot or to dirprev, never
 * both. Worker requests copy paths and return separately owned snapshots.
 * No borrowed pointer to a parent listing survives a window update. */
struct WfDirCache
{
	WfDirCacheEntry entry[WF_DIRCACHE_SLOTS];
	size_t order[WF_DIRCACHE_SLOTS], count, current, cursor;
	int direction;
	WfFileId parent;
	wchar_t filter[WF_FILTER_MAX];
	bool hidden;
	WfSortKey sort;
	bool descending, dirs_first;
	uint32_t flight;
	size_t flight_slot, peak_bytes;
	uint64_t hits, misses, submitted, prefetched, evicted, rejected;
};

void wf_app_dircache_enable(WfApp *app, bool enabled);
void wf_app_dircache_invalidate(WfApp *app);
void wf_app_dircache_destroy(WfApp *app);
size_t wf_app_dircache_bytes(const WfApp *app);
void wf_app_dircache_displayed(WfApp *app);

/* Preview coordinator hooks. result consumes dir only when it returns true;
 * on false, the caller retains it. dirpreview_show always consumes dir. */
void wf_app_dircache_return(WfApp *app);
bool wf_app_dircache_select(WfApp *app);
bool wf_app_dircache_submit(WfApp *app);
bool wf_app_dircache_result(WfApp *app, uint32_t gen, WfStatus st, WfDir *dir);
void wf_app_dirpreview_show(WfApp *app, WfStatus st, WfDir *dir);

#endif
