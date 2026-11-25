#ifndef WF_SAVEMODEL_H
#define WF_SAVEMODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wf_loc.h"

/*
 * The decisions of saving that do not depend on which file system the file is
 * on: what was seen when the file was opened, whether what is there now is
 * still that, and what a save may do given what the place has proven. Pure
 * logic; the backends do the I/O and feed it facts.
 *
 * Everything fails closed: a fact that was not established is not assumed.
 */

/* What a backend could tell about a file. A field that was not read has its
 * `have_*` false and is never compared. */
typedef struct
{
	bool exists;
	bool have_size, have_mtime, have_hash, have_identity;
	uint64_t size;
	int64_t mtime;              /* any unit, same for both sides */
	uint32_t mtime_granularity; /* in that unit; 0 = unknown */
	uint8_t sha256[32];         /* of the bytes as stored */
	uint64_t dev, ino;          /* have_identity */
	bool is_regular;
	bool is_link;
} WfSnapshot;

typedef enum
{
	WF_SNAP_SAME,     /* nothing that was compared differs */
	WF_SNAP_CHANGED,  /* something differs: somebody else wrote */
	WF_SNAP_GONE,     /* it is not there, or it is not a regular file now */
	WF_SNAP_REPLACED, /* a different object stands under the name */
	WF_SNAP_UNKNOWN   /* too little was known to say it is unchanged */
} WfSnapVerdict;

/*
 * `base` is from the open, `now` from just before saving. The hash is the
 * strongest evidence after identity and size checks. Without both hashes,
 * SAME requires matching mtimes with granularity 1 on both sides; size alone
 * or a coarse/unknown time yields UNKNOWN even if sizes match. A link or a
 * non-regular file under the name is GONE.
 */
WfSnapVerdict wf_snapshot_compare(
        const WfSnapshot *base, const WfSnapshot *now);

typedef enum
{
	WF_PLAN_REFUSE,   /* nothing may be written */
	WF_PLAN_NEW_ONLY, /* a file that does not exist yet may be created */
	WF_PLAN_REPLACE   /* the existing file may be replaced */
} WfSavePlan;

typedef enum
{
	WF_WHY_OK,
	WF_WHY_READ_ONLY_PLACE, /* the place has no write capability at all */
	WF_WHY_NO_NEW_FILE,
	WF_WHY_NO_REPLACE,
	WF_WHY_CHANGED, /* the file changed since it was opened */
	WF_WHY_GONE,
	WF_WHY_REPLACED,
	WF_WHY_UNVERIFIED, /* could not show it is unchanged */
	WF_WHY_EXISTS      /* a new file's name is taken */
} WfSaveWhy;

/*
 * What a save may do. `target_exists` is whether the target is there now
 * (from the pre-save snapshot); `verdict` is the comparison with the base when the
 * buffer came from that file (ignored for a new file).
 */
WfSavePlan wf_save_plan(uint32_t caps,
        bool target_exists,
        bool buffer_from_file,
        WfSnapVerdict verdict,
        WfSaveWhy *why);

#endif
