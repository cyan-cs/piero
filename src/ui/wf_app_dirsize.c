/*
 * Folder sizes (D): the size of every folder in the list, counted by a worker
 * and written at the end of each folder's row as the numbers arrive, so the
 * list stays usable while it runs. Esc stops the count and keeps what has
 * been counted, marked "at least".
 *
 * Nothing here reads or changes a file: the totals are numbers for the screen.
 * They belong to the folder they were counted in (a row of another folder never
 * shows them) and are as of the count; r reloads the list and forgets them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../archive/wf_vpath.h"
#include "../fs/wf_sftpplace_win.h"
#include "../path/wf_path.h"
#include "wf_app_priv.h"
#include "wf_info.h"

void wf_app_set_sizes(WfApp *app, WfSizePort port)
{
	app->size_port = port;
}

void wf_app_dirsize_destroy(WfApp *app)
{
	if (app->size_port.cancel) {
		app->size_port.cancel(app->size_port.ctx);
	}
	wf_dirsizes_free(&app->dirsize);
}

/* The state line while the count runs, and its last word when it is over. */
static void say_progress(WfApp *app)
{
	const WfDirSizes *s = &app->dirsize;
	char text[sizeof app->msg];

	(void)wf_msgf(text, sizeof text, MSG_DIRSIZE_RUNNING,
	        (WfMsgArg[]){WF_ARG_N(s->done), WF_ARG_N(s->asked)}, 2);
	wf_app_set_msg(app, text);
}

static void say_result(WfApp *app, bool stopped)
{
	const WfDirSizes *s = &app->dirsize;
	size_t partial = 0, counted = 0;
	char text[sizeof app->msg];

	for (size_t i = 0; i < s->n; i++) {
		if (s->u[i].finished) {
			counted++;
			partial += s->u[i].partial ? 1 : 0;
		}
	}
	(void)wf_msgf(text, sizeof text,
	        stopped ? MSG_DIRSIZE_STOPPED
	                : partial ? MSG_DIRSIZE_DONE_PARTIAL
	                          : MSG_DIRSIZE_DONE,
	        (WfMsgArg[]){WF_ARG_N(counted),
	                WF_ARG_N(stopped ? s->asked : partial)},
	        2);
	wf_app_set_msg(app, text);
}

bool wf_app_dirsize_running(const WfApp *app)
{
	return app->dirsize.running;
}

bool wf_app_dirsize_stop(WfApp *app)
{
	if (!app->dirsize.running) {
		return false;
	}
	if (app->size_port.cancel) {
		app->size_port.cancel(app->size_port.ctx);
	}
	/* what the worker still delivers is dropped: nothing is running */
	app->dirsize.running = false;
	say_result(app, true);
	return true;
}

void wf_app_dirsize_forget(WfApp *app)
{
	if (app->size_port.cancel && app->dirsize.running) {
		app->size_port.cancel(app->size_port.ctx);
	}
	wf_dirsizes_clear(&app->dirsize);
}

void wf_app_cmd_dirsize(WfApp *app)
{
	const WfView *v = &app->cur;
	const wchar_t **names;
	size_t n = 0;
	WfStatus st;

	if (!app->size_port.start || wf_vpath_is(app->cwd) ||
	        wf_sftp_place_is(app->cwd) || wf_pcroot_is(app->cwd)) {
		wf_app_set_msg(app, wf_msg(MSG_DIRSIZE_UNAVAILABLE));
		return;
	}
	if (app->want_path || !v->dir) {
		return; /* the list is being replaced; the key can be pressed again */
	}
	names = calloc(v->nvis ? v->nvis : 1, sizeof *names);
	if (!names) {
		wf_app_set_error(app, WF_E_NOMEM);
		return;
	}
	for (size_t i = 0; i < v->nvis; i++) {
		const WfEntry *e = &v->dir->e[v->vis[i]];

		/* a link is never followed, so it has nothing to count */
		if ((e->flags & WF_ENT_DIR) && !(e->flags & WF_ENT_LINK)) {
			names[n++] = e->name;
		}
	}
	if (!n) {
		free(names);
		wf_app_set_msg(app, wf_msg(MSG_DIRSIZE_NONE));
		return;
	}
	if (n > WF_SIZE_MAX_FOLDERS) {
		n = WF_SIZE_MAX_FOLDERS;
	}
	wf_app_dirsize_forget(app);
	if (!wf_dirsizes_begin(
	            &app->dirsize, ++app->dirsize_gen, app->cwd, names, n)) {
		free(names);
		wf_app_set_error(app, WF_E_NOMEM);
		return;
	}
	free(names);
	st = app->size_port.start(app->size_port.ctx, app->dirsize.gen,
	        app->cwd, (const wchar_t *const *)app->dirsize.name, n);
	if (WF_OK != st) {
		wf_dirsizes_clear(&app->dirsize);
		wf_app_set_msg(app, wf_msg(MSG_DIRSIZE_FAILED));
		return;
	}
	say_progress(app);
}

void wf_app_on_sizes(WfApp *app, const WfSizeUpdate *u)
{
	if (!app->dirsize.running || !wf_dirsizes_take(&app->dirsize, u)) {
		return;
	}
	/* a count for a folder that is no longer shown is not worth finishing */
	if (!app->cwd || !app->dirsize.dir ||
	        wcscmp(app->cwd, app->dirsize.dir)) {
		wf_app_dirsize_forget(app);
		return;
	}
	if (app->dirsize.running) {
		say_progress(app);
	} else {
		say_result(app, false);
	}
}

/* The total of the folder under the cursor, for the details window; false
 * when nothing was counted for it. */
bool wf_app_dirsize_total(
        const WfApp *app, const WfEntry *entry, WfSizeUpdate *out)
{
	const WfSizeUpdate *u;

	if (!entry || !(entry->flags & WF_ENT_DIR)) {
		return false;
	}
	u = wf_dirsizes_find(&app->dirsize, app->cwd, entry->name);
	if (!u) {
		return false;
	}
	*out = *u;
	return true;
}

bool wf_app_dirsize_describe(
        const WfApp *app, const WfEntry *entry, char *out, size_t cap)
{
	WfSizeUpdate u;
	char size[64], files[48], folders[48];

	if (!wf_app_dirsize_total(app, entry, &u)) {
		return false;
	}
	wf_info_format_size(u.bytes, size, sizeof size);
	if (!u.finished || u.partial) {
		char exact[sizeof size];

		snprintf(exact, sizeof exact, "%s", size);
		(void)wf_msgf(size, sizeof size, MSG_INFO_AT_LEAST,
		        &WF_ARG_S(exact), 1);
	}
	(void)wf_msgf(files, sizeof files,
	        wf_msg_count(MSG_INFO_FILES_ONE, MSG_INFO_FILES_OTHER,
	                (long long)u.files),
	        &WF_ARG_N(u.files), 1);
	(void)wf_msgf(folders, sizeof folders,
	        wf_msg_count(MSG_INFO_FOLDERS_ONE, MSG_INFO_FOLDERS_OTHER,
	                (long long)u.folders),
	        &WF_ARG_N(u.folders), 1);
	(void)wf_msgf(out, cap, MSG_DIRSIZE_CONTENTS,
	        (WfMsgArg[]){
	                WF_ARG_S(size), WF_ARG_S(files), WF_ARG_S(folders)},
	        3);
	return true;
}
