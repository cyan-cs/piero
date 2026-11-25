#ifndef WF_SANITIZER_STAGE_WIN_H
#define WF_SANITIZER_STAGE_WIN_H
#include <windows.h>
#include <stdbool.h>

/* Sanitized development builds only. Stages exactly the loaded compiler-rt
 * selected at configure time and its declared private assembly. No overwrite,
 * deletion, DLL search change or ACL changes; caller keeps the decoder sandbox. */
bool wf_sanitizer_stage(const wchar_t *directory, DWORD *error);
#endif
