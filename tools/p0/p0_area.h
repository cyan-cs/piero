#ifndef P0_AREA_H
#define P0_AREA_H

/*
 * A dedicated, ownership-proven scratch area for P0 experiments and tests.
 *
 * The rule it enforces: a run may delete only objects THAT RUN created and
 * recorded, inside a directory THAT RUN created. There is no enumerate-and-
 * delete, no wildcard, no recursive removal anywhere in this file.
 *
 *   <base>\wfile-p0-<run_id>\
 *       .p0-owner      nonce + the directory's FileId (written at creation)
 *       .p0-manifest   one line per created object, appended and flushed
 *
 * Every removal first re-proves, in this order:
 *   1. the root directory is the one we created (FileId equal),
 *   2. .p0-owner still holds our nonce,
 *   3. the target was recorded in the manifest (exact, case-sensitive path),
 *   4. the target's parent chain resolves inside the root (no junction or
 *      symlink on the way leads out of it).
 * Anything that fails a proof is left alone and reported. Directories are
 * removed with RemoveDirectoryW, which only succeeds when empty.
 *
 * A crashed run leaves its area behind. It is never swept automatically; it is
 * cleaned by p0_area_reap(), which repeats the same proofs from the files on
 * disk, or by hand after reading .p0-owner.
 */
#include <windows.h>

#include <stddef.h>

typedef struct P0Area P0Area;

/* Creates a NEW area under `base` (which must already exist). NULL on failure,
 * including when the directory already exists. */
P0Area *p0_area_create(const wchar_t *base, const char *run_id);

/* Re-opens an area left by an earlier run (reads marker and manifest). */
P0Area *p0_area_open(const wchar_t *root);

const wchar_t *p0_area_root(const P0Area *a);

/* Diagnostic only: count named handles in this process for this unique owned
 * root. No other process is opened and no names are printed. The snapshot is
 * released before returning. A zero count does not identify an external holder. */
DWORD p0_area_open_handles(const P0Area *a, size_t *count);

/*
 * Maps a relative name to an absolute path inside the area. Rejects absolute
 * names, drive letters, "..", "." components, wildcard characters, ADS colons
 * and empty names. Returns 0 on rejection.
 */
int p0_area_path(
        const P0Area *a, const wchar_t *rel, wchar_t *out, size_t cap_units);

/* Same mapping in extended (\?\) form, for paths beyond MAX_PATH. */
int p0_area_xpath(
        const P0Area *a, const wchar_t *rel, wchar_t *out, size_t cap_units);

/* Records an object that now exists at `rel` (created by this run). */
int p0_area_track(P0Area *a, const wchar_t *rel, int is_dir);

/* Creates the directory and records it. */
int p0_area_mkdir(P0Area *a, const wchar_t *rel);

/* Removes one recorded object (after the proofs). Returns 1 if it is gone. */
int p0_area_remove(P0Area *a, const wchar_t *rel);

/*
 * Removes every recorded object in reverse order, then the marker, manifest
 * and root. Returns the number of things left behind. `report` (optional)
 * receives a human-readable list of leftovers; unexpected children are named,
 * never deleted.
 */
int p0_area_destroy(P0Area *a, wchar_t *report, size_t report_units);

/* Frees the handle without deleting anything (for runs that keep evidence). */
void p0_area_forget(P0Area *a);

/* For tests: why the last refusal happened. */
const char *p0_area_last_refusal(const P0Area *a);

#endif
