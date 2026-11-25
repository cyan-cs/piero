#ifndef WF_DLLLOAD_WIN_H
#define WF_DLLLOAD_WIN_H

#include <stddef.h>
#include <stdint.h>
#include <windows.h>

/*
 * Loading one of our own DLLs, strictly (DR-DRAFT-033, section 4). Used by
 * piero.exe for piero-syntax.dll and by piero-syntax.dll for the grammars.
 *
 * What it does, in this order, and every step fails closed (the status is a
 * WfSynStatus, never a partly loaded module):
 *  1. the root is a real directory path on a fixed or removable drive (not a
 *     UNC path, not a device path, not a remote or optical drive);
 *  2. every component below the root is a plain name, exists, and is not a
 *     reparse point (symlink, junction, mount point);
 *  3. the file is opened read-only with sharing that forbids anyone to write,
 *     delete or rename it while it is held, is a regular file under a size
 *     limit, and its SHA-256 is compared with the expected one;
 *  4. LoadLibraryExW with the full path and LOAD_LIBRARY_SEARCH_SYSTEM32 |
 *     LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR while the file is still held, so the file
 *     that was hashed is the file that is mapped;
 *  5. the module's own path is read back and must be the path asked for.
 * None of this makes a DLL that was made by an attacker safe to run; it makes a
 * wrong, partly replaced or redirected installation fail.
 */

/* The directory the running program really is in: a full path without a
 * trailing separator, with every link above it resolved. */
uint32_t wf_dll_exe_dir(wchar_t *out, size_t cap);
/* The directory of the module that contains `addr`, resolved the same way. */
uint32_t wf_dll_module_dir(const void *addr, wchar_t *out, size_t cap);

/* root\parts[0]\parts[1]... into out. `root` must be what wf_dll_exe_dir or
 * wf_dll_module_dir returned (or a path with the same properties). Each part
 * is a plain name ([A-Za-z0-9._-], no leading dot, no "..", at most 64). The
 * components that exist must not be reparse points; the last one need not
 * exist (WF_SYN_E_NOGRAMMAR is for the caller to say). */
uint32_t wf_dll_resolve(const wchar_t *root,
        const wchar_t *const *parts,
        size_t nparts,
        wchar_t *out,
        size_t cap);

/* True when the path is a plain file below the root that exists and is not a
 * link: the cheap check that decides whether a grammar is installed. Opens
 * nothing for longer than one attribute read. */
int wf_dll_present(const wchar_t *path);

typedef struct
{
	HANDLE h; /* INVALID_HANDLE_VALUE when nothing is held */
	uint8_t sha256[32];
	uint32_t size;
} WfDllHold;

/* Opens `path`, holds it (see 3 above), reads at most `max_bytes` and fills
 * `hold`. When `expect` is not NULL the digest must equal it. On any failure
 * nothing is held. */
uint32_t wf_dll_hold(const wchar_t *path,
        uint32_t max_bytes,
        const uint8_t *expect,
        WfDllHold *hold);
/* The bytes of a held file (a manifest, a query), up to its size; the caller
 * frees *out. Reading goes through the held handle. */
uint32_t wf_dll_hold_read(const WfDllHold *hold, char **out, size_t *n);
void wf_dll_release(WfDllHold *hold);

/* Steps 4 and 5. The file must be held (wf_dll_hold) by the caller. */
uint32_t wf_dll_load(const wchar_t *path, HMODULE *out);

/* SHA-256 of a buffer (BCrypt). */
int wf_dll_sha256(const void *data, size_t n, uint8_t out[32]);
/* 64 hex digits (either case) into 32 bytes; 0 when it is not exactly that. */
int wf_dll_hex32(const char *hex, uint8_t out[32]);

#endif
