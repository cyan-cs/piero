#ifndef WF_CWD_NEUTRALIZE_H
#define WF_CWD_NEUTRALIZE_H
#include <windows.h>

#include <stdbool.h>
/* *error when the folder exists but is not private to the user: its owner is
 * someone else, or its rights are not the one protected entry. It is not an
 * access denial, and it is not a Win32 code (customer-defined bit set). */
#define WF_PRIVATE_E_NOT_PRIVATE 0x20000001UL
/* Creates only this directory if missing, owned by TokenUser with a protected
 * user-only DACL. Existing owners/ACLs are inspected, never changed.
 * Caller closes *pin. */
bool wf_private_directory_open(
        const wchar_t *path, bool empty, HANDLE *pin, DWORD *error);
/* Startup only, after resolving all CLI paths. Retain *pin for the session. */
bool wf_cwd_neutralize(const wchar_t *path, HANDLE *pin, DWORD *error);
#endif
