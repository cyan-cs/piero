#ifndef WF_GITBACKEND_WIN_H
#define WF_GITBACKEND_WIN_H

#include <windows.h>

#include <stdbool.h>
#include <stdint.h>

#include "wf_gitenv.h"
#include "wf_gitproc_win.h"

/*
 * The Git of the environment that owns a repository (DR-DRAFT-038).
 *
 *   WF_GENV_WINDOWS  ->  git.exe, found on PATH (absolute entries only)
 *   WF_GENV_WSL      ->  %SystemRoot%\System32\wsl.exe -d <distribution> --exec git
 *   anything else    ->  no Git
 *
 * There is no fallback from one to the other, and none is possible from this
 * interface: every function takes the environment and builds the command for
 * that environment only. Where its Git is missing the answer is "unavailable".
 *
 * Capability is learned the first time it is needed (never at start-up) and kept:
 * Windows by looking for git.exe on PATH (no process), each WSL distribution by
 * running `git --version` in it. Available is kept for the life of the process;
 * unavailable for WF_GIT_RETRY_MS, so that a timeout or an install made meanwhile
 * is not remembered for ever. At most WF_GIT_DISTROS distributions are kept.
 */
#define WF_GIT_DISTROS 8
#define WF_GIT_RETRY_MS 60000u

typedef enum
{
	WF_GIT_UNAVAILABLE,
	WF_GIT_AVAILABLE
} WfGitCap;

/* Asks the environment's own Git whether it exists. May run wsl.exe (WSL, the
 * first time). WF_GENV_OTHER is unavailable. `cancelled` may be NULL. */
WfGitCap wf_git_capability(
        const WfGitEnv *env, bool (*cancelled)(void *), void *cancel_ctx);

/* Runs `git <args>` with this environment's Git in `dir` (a path of that
 * environment: a drive path, or a WSL UNC path). `args` is a NULL-terminated
 * array. The options of every read-only run are put in front: --no-pager,
 * --no-optional-locks, and core.fsmonitor off (a repository's configuration
 * never starts a program for these). WF_E_INVAL for OTHER or for a WSL path
 * that is not a valid POSIX path or a distribution name that cannot be one;
 * WF_E_NOENT when this environment has no Git. Statuses of wf_gitproc_run
 * otherwise. */
WfStatus wf_git_exec(const WfGitEnv *env,
        const wchar_t *dir,
        const wchar_t *const *args,
        uint32_t timeout_ms,
        size_t max_out,
        bool (*cancelled)(void *),
        void *cancel_ctx,
        WfGitRunResult *res);

/* Tests only: replace the parts that touch the system. NULL members keep the
 * real ones. Not thread safe; set before anything runs. */
typedef struct
{
	WfStatus (*run)(void *ctx, const WfGitRun *run, WfGitRunResult *res);
	void *run_ctx;
	bool (*find_windows_git)(wchar_t *out, size_t cap);
	uint64_t (*now_ms)(void);
} WfGitHooks;
void wf_git_hooks_set(const WfGitHooks *hooks);
void wf_git_cache_reset(void); /* forgets what capability was learned */

#endif
