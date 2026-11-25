#ifndef WF_GITPORT_H
#define WF_GITPORT_H

#include <windows.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../base/wf_status.h"
#include "../text/wf_linediff.h"
#include "wf_gitenv.h"
#include "wf_gitstatus.h"

/*
 * What the screen and the Git worker say to each other (DR-DRAFT-038). The
 * screen (wfcore) knows only these types; the worker (wfgit) fills them.
 *
 * Every request carries a generation, and so does its answer: the screen shows
 * the answer of the newest generation and drops an older one, whatever it holds.
 * An answer is owned by whoever polls it; wf_gitanswer_free releases it.
 */

/* The content of a file at HEAD: counted, so the editor and a diff request can
 * hold it at once without a copy. */
typedef struct
{
	volatile LONG refs;
	size_t n;
	char *data; /* n bytes, NUL-terminated after them */
} WfGitBlob;

WfGitBlob *wf_gitblob_new(
        const void *bytes, size_t n);    /* refs = 1; NULL: no memory */
WfGitBlob *wf_gitblob_ref(WfGitBlob *b); /* NULL-safe */
void wf_gitblob_release(WfGitBlob *b);   /* NULL-safe */

typedef enum
{
	WF_GITR_OK,
	WF_GITR_NOT_REPO,    /* Git says this is not a work tree */
	WF_GITR_UNAVAILABLE, /* the Git of this environment is not there */
	WF_GITR_FAILED,      /* it ran and could not answer (or timed out) */
	WF_GITR_TOO_LARGE    /* more than the screen compares */
} WfGitState;

typedef enum
{
	WF_GIT_A_REPO,
	WF_GIT_A_BASE,
	WF_GIT_A_DIFF
} WfGitAnswerKind;

/* What a REPO request asks for. */
enum
{
	WF_GIT_REPO_PLAIN = 0,
	WF_GIT_REPO_RESOLVE = 1,
	WF_GIT_REPO_LOCATE = 2
};

typedef struct
{
	WfGitAnswerKind kind;
	uint32_t gen;
	uint64_t id; /* BASE and DIFF: the buffer the request was for */
	WfGitState state;
	wchar_t *asked; /* REPO: the folder the request was made with (owned) */
	wchar_t *
	        root; /* REPO: the folder that holds .git, if one was found (owned) */
	WfGitStatus table; /* REPO, OK: sorted records (owned) */
	bool located; /* REPO: only the .git was looked for (WF_GIT_REPO_LOCATE): no status */
	WfGitBlob *base; /* BASE, OK: the file at HEAD, empty for a new file */
	WfLineDiff diff; /* DIFF, OK */
	bool diff_too_many; /* DIFF: past the bounds of the line diff */
} WfGitAnswer;

void wf_gitanswer_free(WfGitAnswer *a); /* empties it; safe to repeat */

/* What the screen can learn about a folder without waiting. */
typedef enum
{
	WF_GIT_ROOT_NONE, /* no Git here: another kind of place, or no .git above it */
	WF_GIT_ROOT_FOUND, /* the nearest folder with a .git is in `root` */
	WF_GIT_ROOT_LATER /* Git may apply, but finding the .git is I/O that can wait:
	                     the worker does it */
} WfGitRootKind;

/* The screen's way to ask. A request that cannot be made (no memory, not a
 * place Git is asked about) returns an error and no answer will come. The
 * callbacks are called on the UI thread only. */
typedef struct
{
	void *ctx;
	/* Whether Git is asked about `dir` at all, and where: the environment that
	 * owns it and, where that can be told without waiting (the file attributes
	 * of a fixed drive), the nearest folder above it that holds a `.git`. Nothing
	 * is run, and nothing that may block the screen is touched: for a place where
	 * the walk could wait (a WSL folder, reached through the distribution, or a
	 * removable drive) the answer is WF_GIT_ROOT_LATER and `root` is `dir`. */
	WfGitRootKind (*root_of)(void *ctx,
	        const wchar_t *dir,
	        WfGitEnv *env,
	        wchar_t *root,
	        size_t cap);
	/* The state of the repository whose `.git` is in `root` (`mode` is
	 * WF_GIT_REPO_PLAIN). With WF_GIT_REPO_RESOLVE, `root` is any folder and the
	 * worker first finds the nearest `.git` at or above it (the answer says which).
	 * With WF_GIT_REPO_LOCATE it only does that: the answer says where the nearest
	 * `.git` is (`root`), or that there is none, and no Git process is run. */
	WfStatus (*repo)(
	        void *ctx, uint32_t gen, const wchar_t *root, int mode);
	/* The file at HEAD for the buffer `id` that edits `path` (absolute). */
	WfStatus (*base)(
	        void *ctx, uint32_t gen, uint64_t id, const wchar_t *path);
	/* The lines of `base` against `text` (copied). */
	WfStatus (*diff)(void *ctx,
	        uint32_t gen,
	        uint64_t id,
	        WfGitBlob *base,
	        const char *text,
	        size_t n);
	/* Drops the editor's requests (the base and the diff) and ends what runs for
	 * them. The state of the repository, which the file list waits for, is not
	 * touched. */
	void (*cancel)(void *ctx);
} WfGitPort;

#endif
