/*
 * Git in the file list (DR-DRAFT-038): the repository whose folder is on screen,
 * its status as a table, and the one letter beside a row. The screen never waits
 * for Git: the list is drawn from the listing, and the letters appear when the
 * answer of the worker arrives.
 */
#include "wf_app_priv.h"

#include <stdlib.h>
#include <string.h>

#include "../git/wf_gitenv.h"
#include "../path/wf_path.h"

#define GIT_REFRESH_GAP_MS 1000u /* a reload asks again at most this often */
#define GIT_STALE_MS                                                           \
	10000u /* moving inside the repository asks again after this */
#define GIT_RETRY_MS 30000u /* after "no Git" or a failure: not before this */
#define GIT_LOST_MS                                                            \
	75000u /* nothing came back for a request (a status may take 60 s): ask again */
#define GIT_KEY_MAX 4400u

void wf_app_set_git(WfApp *app, WfGitPort port)
{
	app->git.port = port;
}

static void drop(WfApp *app)
{
	WfGitPort port = app->git.port;

	uint32_t gen = app->git.gen;

	free(app->git.root);
	free(app->git.prefix);
	free(app->git.vcwd);
	free(app->git.lcwd);
	wf_gitstatus_free(&app->git.table);
	memset(&app->git, 0, sizeof app->git);
	app->git.port = port;
	/* the generation goes on: the first request of another repository must not
	 * look like the first request of this one */
	app->git.gen = gen;
}

void wf_app_git_destroy(WfApp *app)
{
	drop(app);
	memset(&app->git, 0, sizeof app->git);
}

/* Windows compares paths without regard to case, a WSL distribution does not. */
static bool same_root(const WfGitEnv *env, const wchar_t *a, const wchar_t *b)
{
	if (WF_GENV_WSL == env->kind) {
		return 0 == wcscmp(a, b);
	}
	return CSTR_EQUAL == CompareStringOrdinal(a, -1, b, -1, TRUE);
}

/* The folder shown, from the root: "src/sub/", or "" at the root. NULL when the
 * folder is not under the root. */
static char *prefix_of(
        const WfGitEnv *env, const wchar_t *root, const wchar_t *cwd)
{
	size_t rl = wcslen(root), n;
	const wchar_t *rest;
	wchar_t *tmp;
	char *out;
	int bytes;
	bool under;

	if (wcslen(cwd) < rl) {
		return NULL;
	}
	tmp = wf_path_dup_n(cwd, rl);
	if (!tmp) {
		return NULL;
	}
	under = same_root(env, root, tmp);
	free(tmp);
	if (!under) {
		return NULL;
	}
	rest = cwd + rl;
	if (*rest && L'\\' != *rest && L'/' != *rest) {
		return NULL; /* "C:\repo2" is not under "C:\repo" */
	}
	while (L'\\' == *rest || L'/' == *rest) {
		rest++;
	}
	n = wcslen(rest);
	while (n && (L'\\' == rest[n - 1] || L'/' == rest[n - 1])) {
		n--;
	}
	if (0 == n) {
		return _strdup("");
	}
	tmp = wf_path_dup_n(rest, n);
	if (!tmp) {
		return NULL;
	}
	for (wchar_t *c = tmp; *c; c++) {
		if (L'\\' == *c) {
			*c = L'/';
		}
	}
	bytes = WideCharToMultiByte(
	        CP_UTF8, WC_ERR_INVALID_CHARS, tmp, -1, NULL, 0, NULL, NULL);
	out = bytes > 0 ? malloc((size_t)bytes + 1) : NULL;
	if (out) {
		WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, tmp, -1, out,
		        bytes, NULL, NULL);
		strcat_s(out, (size_t)bytes + 1, "/");
	}
	free(tmp);
	return out;
}

/* Whether the letters on screen belong to the folder on screen. Where the worker
 * finds the .git, being under the root says nothing: a repository or a submodule
 * may be nested below it, and only the worker can tell without waiting. */
static bool git_verified(const WfApp *app)
{
	return !app->git.deferred ||
	        (app->git.vcwd && app->cwd &&
	                same_root(&app->git.env, app->git.vcwd, app->cwd));
}

static void ask(WfApp *app);

/* Asks the worker for the nearest .git of the folder shown (no Git process). The
 * letters stay hidden until the answer says which repository the folder is in. */
static void locate(WfApp *app)
{
	if (app->git.lcwd &&
	        same_root(&app->git.env, app->git.lcwd, app->cwd)) {
		return; /* already out for this folder */
	}
	free(app->git.lcwd);
	app->git.lcwd = wf_path_dup(app->cwd);
	if (!app->git.lcwd) {
		drop(app);
		return;
	}
	if (++app->git.gen == 0) {
		app->git.gen = 1;
	}
	if (WF_OK !=
	        app->git.port.repo(app->git.port.ctx, app->git.gen, app->cwd,
	                WF_GIT_REPO_LOCATE)) {
		drop(app);
	}
}

static void ask(WfApp *app)
{
	app->git.due_ms = 0;
	if (app->git.lcwd) {
		/* a check for a nearer .git is out: a status asked now would overtake it
		 * (its answer would be dropped as old and the folder never verified).
		 * Noted, and asked when the check is answered. */
		app->git.stale = true;
		return;
	}
	if (++app->git.gen == 0) {
		app->git.gen = 1;
	}
	app->git.asked_ms = app->now_ms;
	app->git.stale = false;
	/* a root that is only the folder shown is resolved by the worker, where the
	 * walk may wait: the screen never waits for it */
	if (WF_OK !=
	        app->git.port.repo(app->git.port.ctx, app->git.gen,
	                app->git.root,
	                app->git.resolved ? WF_GIT_REPO_PLAIN
	                                  : WF_GIT_REPO_RESOLVE)) {
		drop(app); /* no letters; the list is as it was */
	}
}

/* Whether the folder shown is in the repository the screen already has. */
static bool in_known_repository(const WfApp *app,
        WfGitRootKind kind,
        const WfGitEnv *env,
        const wchar_t *root)
{
	if (!app->git.root || env->kind != app->git.env.kind ||
	        0 != strcmp(env->distro, app->git.env.distro)) {
		return false;
	}
	if (WF_GIT_ROOT_FOUND == kind) {
		return same_root(env, root, app->git.root);
	}
	if (app->git.resolved) { /* found by the worker: whatever is below it is its */
		char *below = prefix_of(env, app->git.root, app->cwd);

		free(below);
		return NULL != below;
	}
	return same_root(
	        env, app->cwd, app->git.root); /* the same folder again */
}

void wf_app_git_note_listing(WfApp *app, bool reload)
{
	WfGitEnv env;
	wchar_t root[MAX_PATH * 4];
	WfGitRootKind kind;

	if (!app->git.port.repo || !app->git.port.root_of || !app->cwd) {
		return;
	}
	kind = app->git.port.root_of(
	        app->git.port.ctx, app->cwd, &env, root, ARRAYSIZE(root));
	if (WF_GIT_ROOT_NONE == kind) {
		drop(app); /* not a place with a work tree: nothing of Git is kept */
		return;
	}
	if (in_known_repository(app, kind, &env, root)) {
		uint64_t age = app->now_ms - app->git.asked_ms;
		bool again;

		free(app->git.prefix);
		app->git.prefix = prefix_of(&env, app->git.root, app->cwd);
		if (app->git.deferred && !git_verified(app)) {
			locate(app); /* a nearer .git may be below the root */
			if (!app->git.root) {
				return;
			}
		}
		if (!app->git.answered) {
			/* a list read again while the answer is awaited may have changed
			 * what Git is being asked about: noted, and asked again when the
			 * answer is in (the request under way is not cancelled for it) */
			app->git.stale = app->git.stale || reload;
			/* the answer is on its way, unless it was lost (a request dropped by
			 * the worker for another reason): then it is asked again, late enough
			 * not to cut a slow status short */
			if (age >= GIT_LOST_MS) {
				ask(app);
			}
			return;
		}
		again = WF_GITR_OK == app->git.state
		        ? (reload ? age >= GIT_REFRESH_GAP_MS
		                  : age >= GIT_STALE_MS)
		        : age >= GIT_RETRY_MS;
		if (again) {
			ask(app);
		} else if (reload && WF_GITR_OK == app->git.state &&
		        !app->git.due_ms) {
			/* too soon: not dropped, made when the gap is over */
			app->git.due_ms =
			        app->git.asked_ms + GIT_REFRESH_GAP_MS;
		}
		return;
	}
	/* another repository, or the first */
	drop(app);
	app->git.root = wf_path_dup(root);
	app->git.env = env;
	app->git.resolved = WF_GIT_ROOT_FOUND == kind;
	app->git.deferred = WF_GIT_ROOT_LATER == kind;
	app->git.prefix = prefix_of(&env, root, app->cwd);
	if (!app->git.root || !app->git.prefix) {
		drop(app);
		return;
	}
	ask(app);
}

/* The answer to a check for a nearer .git: the folder is in the repository the
 * screen has (the letters come back), in one nested below it (that one is asked
 * about), or in none. */
static bool on_located(WfApp *app, WfGitAnswer *a)
{
	bool had = app->git.answered && WF_GITR_OK == app->git.state &&
	        app->git.table.n > 0;
	wchar_t *vcwd = wf_path_dup(a->asked);

	free(app->git.lcwd);
	app->git.lcwd = NULL;
	if (!vcwd || WF_GITR_OK != a->state || !a->root) {
		free(vcwd);
		drop(app); /* no .git above the folder any more */
		return had;
	}
	if (same_root(&app->git.env, a->root, app->git.root)) {
		free(app->git.vcwd);
		app->git.vcwd = vcwd;
		if (!app->git.answered || app->git.stale) {
			/* the status under way was overtaken by this check, or one was
			 * due while it was out */
			ask(app);
		}
		return app->git.answered && WF_GITR_OK == app->git.state &&
		        app->git.table.n > 0;
	}
	{
		char *prefix = prefix_of(&app->git.env, a->root, app->cwd);
		wchar_t *root = prefix ? wf_path_dup(a->root) : NULL;

		if (!root) {
			free(prefix);
			free(vcwd);
			drop(app);
			return had;
		}
		free(app->git.root);
		free(app->git.prefix);
		free(app->git.vcwd);
		wf_gitstatus_free(&app->git.table);
		app->git.root = root;
		app->git.prefix = prefix;
		app->git.vcwd = vcwd;
		app->git.answered = false;
		app->git.state = WF_GITR_FAILED;
		ask(app); /* the nested repository's own status */
	}
	return had;
}

bool wf_app_on_git(WfApp *app, WfGitAnswer *a)
{
	bool had, was_resolve;

	if (a && WF_GIT_A_REPO == a->kind && app->git.root && app->git.lcwd &&
	        a->gen == app->git.gen && a->asked &&
	        same_root(&app->git.env, a->asked, app->git.lcwd)) {
		return on_located(app, a);
	}
	if (!a || WF_GIT_A_REPO != a->kind || !app->git.root ||
	        a->gen != app->git.gen || !a->asked ||
	        !same_root(&app->git.env, a->asked, app->git.root)) {
		return false; /* not the newest request, or not for what is on screen */
	}
	had = app->git.answered && WF_GITR_OK == app->git.state &&
	        app->git.table.n > 0;
	if (a->root && !same_root(&app->git.env, a->root, app->git.root)) {
		/* the worker found the repository above the folder that was asked about */
		char *prefix = prefix_of(&app->git.env, a->root, app->cwd);
		wchar_t *root = prefix ? wf_path_dup(a->root) : NULL;

		if (!root) { /* the screen has gone elsewhere meanwhile */
			free(prefix);
			return false;
		}
		free(app->git.root);
		free(app->git.prefix);
		app->git.root = root;
		app->git.prefix = prefix;
	}
	was_resolve = !app->git.resolved;
	app->git.resolved = app->git.resolved || NULL != a->root;
	if (was_resolve) { /* the worker looked for the .git from this folder */
		free(app->git.vcwd);
		app->git.vcwd = wf_path_dup(a->asked);
	}
	wf_gitstatus_free(&app->git.table);
	app->git.answered = true;
	app->git.state = a->state;
	if (WF_GITR_OK == a->state) {
		app->git.table = a->table;
		memset(&a->table, 0, sizeof a->table); /* ours now */
	}
	if (app->git.stale && !app->git.due_ms) {
		/* the list changed while this was being asked: what came back may be
		 * old. Asked again once the gap since the request has passed. */
		uint64_t at = app->git.asked_ms + GIT_REFRESH_GAP_MS;

		app->git.due_ms = at > app->now_ms ? at : app->now_ms;
		app->git.stale = false;
	}
	if (app->git.deferred && !git_verified(app)) {
		locate(app); /* the screen moved on while the worker was looking */
	}
	return had || app->git.table.n > 0;
}

/* The key Git knows `e` by: the shown folder's prefix, the name in UTF-8, and a
 * '/' for a folder. Returns its length, or 0 when nothing can be said: no
 * verified answer, or no key fits. */
static size_t entry_key(const WfApp *app, const WfEntry *e, char *key)
{
	size_t n;
	int bytes;

	if (!app->git.answered || WF_GITR_OK != app->git.state ||
	        !app->git.prefix || 0 == app->git.table.n ||
	        !git_verified(app)) {
		return 0;
	}
	n = strlen(app->git.prefix);
	if (n + 2 >= GIT_KEY_MAX) {
		return 0;
	}
	memcpy(key, app->git.prefix, n);
	bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, e->name, -1,
	        key + n, (int)(GIT_KEY_MAX - n - 2), NULL, NULL);
	if (bytes <= 1) {
		return 0;
	}
	n += (size_t)bytes - 1;
	if (e->flags & WF_ENT_DIR) {
		key[n++] =
		        '/'; /* Git reports an untracked folder as one record */
	}
	return n;
}

/* A folder's record. Git reports a submodule by its bare name, without the '/'
 * that every other folder record has, so a folder is looked up both ways. */
static const WfGitEntry *dir_record(
        const WfApp *app, const WfEntry *e, const char *key, size_t n)
{
	const WfGitEntry *r =
	        wf_gitstatus_find_covering(&app->git.table, key, n);

	return !r && (e->flags & WF_ENT_DIR)
	        ? wf_gitstatus_find(&app->git.table, key, n - 1)
	        : r;
}

char wf_app_git_letter(const WfApp *app, const WfEntry *e)
{
	char key[GIT_KEY_MAX];
	size_t n = entry_key(app, e, key);

	return n ? wf_gitentry_letter(dir_record(app, e, key, n)) : 0;
}

bool wf_app_git_dir_changed(const WfApp *app, const WfEntry *e)
{
	char key[GIT_KEY_MAX];
	size_t n;

	if (!(e->flags & WF_ENT_DIR)) {
		return false;
	}
	n = entry_key(app, e, key);
	return n &&
	        (wf_gitstatus_has_under(&app->git.table, key, n) ||
	                NULL != dir_record(app, e, key, n));
}

bool wf_app_git_file(const WfApp *app,
        const wchar_t *root,
        const char *rel,
        char *letter,
        char *orig,
        size_t orig_cap)
{
	const WfGitEntry *e;

	*letter = 0;
	if (orig_cap) {
		orig[0] = '\0';
	}
	if (!app->git.root || !app->git.answered ||
	        WF_GITR_OK != app->git.state || !git_verified(app) ||
	        !same_root(&app->git.env, root, app->git.root)) {
		return false;
	}
	e = wf_gitstatus_find_covering(&app->git.table, rel, strlen(rel));
	if (e) {
		*letter = wf_gitentry_letter(e);
		if (e->orig && strlen(e->orig) < orig_cap) {
			strcpy_s(orig, orig_cap, e->orig);
		}
	}
	return true;
}

bool wf_app_git_base_hint(const WfApp *app,
        const WfGitEnv *env,
        const wchar_t *path,
        const wchar_t **root,
        char *rel,
        size_t rel_cap,
        bool *expect_new,
        char *orig,
        size_t orig_cap)
{
	char letter = 0;

	if (!app->git.root || !app->git.resolved ||
	        env->kind != app->git.env.kind ||
	        0 != strcmp(env->distro, app->git.env.distro) ||
	        !wf_gitenv_rel_path(env, app->git.root, path, rel, rel_cap) ||
	        !wf_app_git_file(
	                app, app->git.root, rel, &letter, orig, orig_cap)) {
		return false; /* includes: the folder is not yet checked (wf_app_git_file) */
	}
	*root = app->git.root;
	*expect_new = 'U' == letter || 'A' == letter;
	return true;
}

uint32_t wf_app_git_timeout(const WfApp *app)
{
	if (!app->git.due_ms || !app->git.root) {
		return UINT32_MAX;
	}
	return app->now_ms >= app->git.due_ms
	        ? 0
	        : (uint32_t)(app->git.due_ms - app->now_ms);
}

bool wf_app_git_tick(WfApp *app)
{
	if (app->git.due_ms && app->git.root &&
	        app->now_ms >= app->git.due_ms) {
		ask(app); /* the answer is what changes the screen, not this */
	}
	return false;
}
