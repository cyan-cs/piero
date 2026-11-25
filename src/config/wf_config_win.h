#ifndef WF_CONFIG_WIN_H
#define WF_CONFIG_WIN_H
#include <windows.h>

#include "wf_config.h"
#include "../base/wf_state_dir.h"
/* Reads without creating a file; config must already be initialized.
 * Missing optional config uses defaults. Opens the leaf without following
 * reparse points and permits read sharing only. Refuses reparse/offline/recall
 * attributes and oversized files; a short read is an I/O error. */
WfStatus wf_config_read(WfConfig *config,
        const wchar_t *path,
        bool optional,
        WfConfigDiagnostic diagnostic,
        void *context,
        WfConfigReport *report,
        DWORD *error);
/* Resolves a path against the current process directory; returns an owned
 * absolute path in *out. Call before changing the startup CWD. */
WfStatus wf_startup_absolute(const wchar_t *path, wchar_t **out, DWORD *error);
WfStatus wf_startup_env_path(const wchar_t *variable,
        const wchar_t *suffix,
        wchar_t **out,
        DWORD *error);
/* Path under the variable's state folder, then `rest`. The folder is the one
 * wf_state_classify picks (piero, else an existing older wfile). Nothing is
 * created. `rest` may be empty for the folder itself; `kind` may be NULL. */
WfStatus wf_startup_state_path(const wchar_t *variable,
        const wchar_t *rest,
        wchar_t **out,
        WfStateKind *kind,
        DWORD *error);
/* The same under the older folder name, whether or not it exists. */
WfStatus wf_startup_legacy_path(const wchar_t *variable,
        const wchar_t *rest,
        wchar_t **out,
        DWORD *error);
#endif
