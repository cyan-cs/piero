#ifndef WF_GITWORKER_H
#define WF_GITWORKER_H

#include <windows.h>

#include <stdbool.h>

#include "../async/wf_worker.h"
#include "wf_gitbackend_win.h"
#include "wf_gitport.h"

/*
 * The thread that talks to Git (DR-DRAFT-038): one request of each kind at a
 * time, the newest wins, and what it answers carries the generation it was asked
 * with.
 *
 *  REPO  detect the work tree in a folder that holds `.git`, then read its
 *        status (one `git status --porcelain=v2 -z` for the whole repository);
 *  BASE  the content of one file at HEAD (nothing is run for a file that is
 *        known to be new);
 *  DIFF  the line diff of that base against a text: no process at all.
 *
 * DIFF is served before BASE and BASE before REPO, so that what the editor waits
 * for does not wait behind a status of a large repository. A newer request of
 * a kind ends the process that is running for an older one; so does cancel and
 * stop. An answer that is no longer the newest is dropped here, not delivered.
 * Everything that is run only reads, and every run is in the environment of the
 * path (wf_gitbackend_win.h): there is no other Git to fall back to.
 */
typedef struct WfGitWorker WfGitWorker;

WfStatus wf_gitworker_start(WfGitWorker **out);
WfWorkerStopReport wf_gitworker_stop(WfGitWorker *w);
/* True when nothing is in flight or waiting and no answer waits to be polled:
 * the worker can be stopped without losing one (DR-DRAFT-036). NULL: true. */
bool wf_gitworker_idle(WfGitWorker *w);

/* Manual-reset event: signalled while an answer waits. */
HANDLE wf_gitworker_event(const WfGitWorker *w);
bool wf_gitworker_poll(WfGitWorker *w, WfGitAnswer *out);

/* Requests. `gen` is not 0. The strings are copied. WF_E_INVAL for a path the
 * environment does not accept; WF_E_NOMEM. */
WfStatus wf_gitworker_repo(WfGitWorker *w,
        uint32_t gen,
        const WfGitEnv *env,
        const wchar_t *root,
        int mode); /* WF_GIT_REPO_* */
/* `rel` is the file's path from the root, UTF-8 with '/'. `orig` is the path it
 * had at HEAD when it was renamed, else NULL. `expect_new`: Git has said the
 * file is new (untracked or added), so its base is empty and nothing is run. */
WfStatus wf_gitworker_base(WfGitWorker *w,
        uint32_t gen,
        uint64_t id,
        const WfGitEnv *env,
        const wchar_t *root,
        const char *rel,
        const char *orig,
        bool expect_new);
/* The base of the file at `path` when the caller does not know where its
 * repository is: the worker finds the `.git` above it (where that may wait) and
 * the path from there, and then goes as above (a new file is found by asking Git). */
WfStatus wf_gitworker_base_of(WfGitWorker *w,
        uint32_t gen,
        uint64_t id,
        const WfGitEnv *env,
        const wchar_t *path);
WfStatus wf_gitworker_diff(WfGitWorker *w,
        uint32_t gen,
        uint64_t id,
        WfGitBlob *base,
        const char *text,
        size_t n);
/* Drops every request that waits and ends the process that runs. */
void wf_gitworker_cancel(WfGitWorker *w);
/* The same for the editor's requests only (the base and the diff): the state of
 * the repository the file list waits for is left alone. */
void wf_gitworker_cancel_editor(WfGitWorker *w);

#endif
