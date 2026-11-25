#ifndef PIERO_PATH_ENTRY_H
#define PIERO_PATH_ENTRY_H
#include <stdbool.h>
#include <wchar.h>
/* directory is an unquoted absolute path using backslashes. True for a match
 * or an allocation/expansion/comparison failure: callers must avoid modifying
 * PATH when presence cannot be ruled out. These functions do not write PATH. */
bool piero_path_contains(const wchar_t *path, const wchar_t *directory);
/* Owned result (free()), or NULL for a refused append/allocation failure. */
wchar_t *piero_path_append(const wchar_t *before, const wchar_t *directory);
/* Exact string checks of the supplied snapshots; no registry synchronization. */
bool piero_path_can_restore(const wchar_t *current,
        const wchar_t *snapshot,
        const wchar_t *before,
        const wchar_t *directory);
#endif
