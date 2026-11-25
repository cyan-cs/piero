#ifndef WF_GITENV_H
#define WF_GITENV_H

#include <windows.h>

#include <stdbool.h>
#include <stddef.h>

#include "../base/wf_status.h"
#include "../fs/wf_wsl_win.h"

/*
 * Which Git operates on a path (DR-DRAFT-038): the one of the environment that
 * owns the repository, and no other. A path on a Windows drive is for the Windows
 * Git, a path inside a WSL distribution is for the Git inside that distribution,
 * and every other place (an archive, an SFTP server, a network share) has no Git
 * here. There is no fallback from one to another: where the Git of the owning
 * environment is missing, Git is off for the path.
 */
typedef enum
{
	WF_GENV_OTHER, /* no Git for this path */
	WF_GENV_WINDOWS,
	WF_GENV_WSL
} WfGitEnvKind;

typedef struct
{
	WfGitEnvKind kind;
	char distro
	        [WF_WSL_NAME_MAX]; /* WF_GENV_WSL: the distribution, as written */
} WfGitEnv;

/* The kind from the text of `path` alone (no I/O): `X:\...` and `\\?\X:\...`
 * are Windows, `\\wsl.localhost\<d>\...` and `\\wsl$\<d>\...` (also as
 * `\\?\UNC\...`) are WSL, a path inside an archive is OTHER, everything else is
 * OTHER. A WSL distribution name that cannot be one (wf_locid_wsl) makes the
 * path OTHER, so a name is never an option on a command line. */
WfGitEnvKind wf_gitenv_classify_text(const wchar_t *path, WfGitEnv *out);

/* The same, and a Windows path on a drive that is remote, optical or missing is
 * OTHER (a repository on a network drive is not asked). */
WfGitEnvKind wf_gitenv_classify(const wchar_t *path, WfGitEnv *out);

/* The Linux path of a WSL path as UTF-8 ("/home/u/project"; "/" for the root
 * of the distribution). WF_E_INVAL for a path that is not WSL or that is not a
 * valid POSIX path (a "." or ".." component, a lone surrogate, over 4096
 * bytes). */
WfStatus wf_gitenv_wsl_linux_path(const wchar_t *path, char *out, size_t cap);

/* The nearest folder at or above `dir` that holds a `.git` (a folder, or a file
 * as a linked worktree and a submodule have), by file attributes only: no
 * process. Stops at the drive/share root or after 128 levels. False also covers
 * insufficient path/output capacity and failed attribute queries; it does not
 * distinguish absence from inability to inspect. `root` receives a found folder. */
bool wf_git_nearest_dotgit(const wchar_t *dir, wchar_t *root, size_t cap);

/* The path of a file below `root`, as the repository writes it: UTF-8 with '/'
 * (a Windows root compares without regard to case, a WSL one exactly). False when
 * the path is not below the root, is the root itself, or does not convert.
 * `root` must have no trailing separator. */
bool wf_gitenv_rel_path(const WfGitEnv *env,
        const wchar_t *root,
        const wchar_t *path,
        char *out,
        size_t cap);

#endif
