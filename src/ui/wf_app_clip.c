/*
 * Files from the Windows clipboard (V): Explorer's copy pasted into the folder
 * on screen (DR-DRAFT-021).
 *
 * The clipboard is read by its own worker; what comes back is only names. Those
 * names are captured by the same call `y` uses (so each one is opened by handle
 * and its identity, size and links are looked at), and when that is done the
 * ordinary paste runs: the preflight, the question about an existing name, the
 * copy job with its verification and journal. Nothing here opens, copies or
 * removes a file, and a path from another program is trusted no more than one
 * that was typed.
 */
#include <stdlib.h>
#include <string.h>

#include "../path/wf_path.h"
#include "wf_app_priv.h"

void wf_app_set_clip_files(WfApp *app, WfClipFilesPort port)
{
	app->clip_files = port;
}

static void say_reason(WfApp *app, WfClipReason why)
{
	WfMsg m = WF_CLIP_NONE == why        ? MSG_CLIP_NONE
	        : WF_CLIP_UNSUPPORTED == why ? MSG_CLIP_UNSUPPORTED
	        : WF_CLIP_BAD == why         ? MSG_CLIP_BAD
	        : WF_CLIP_BUSY == why        ? MSG_CLIP_BUSY
	                                     : MSG_CLIP_FAILED;

	wf_app_set_msg(app, wf_msg(m));
}

/* Y: the selection (or the entry under the cursor) goes on the clipboard as a copy,
 * for Explorer or any program that pastes files. Only names are published; piero
 * copies nothing here and holds no file. */
void wf_app_cmd_clip_copy(WfApp *app)
{
	const wchar_t **names;
	wchar_t **path;
	size_t n = 0, hidden = 0, i;
	bool ok = true;

	if (wf_app_archive_guard(app)) {
		return;
	}
	if (!app->clip_files.publish) {
		wf_app_set_msg(app, wf_msg(MSG_CLIP_UNAVAILABLE));
		return;
	}
	if (app->clip_busy) {
		wf_app_set_msg(app, wf_msg(MSG_XFER_BUSY_CHECK));
		return;
	}
	if (!app->cur.dir || app->want_path) {
		wf_app_set_msg(app, wf_msg(MSG_XFER_STALE_LISTING));
		return;
	}
	names = calloc(app->cur.nvis ? app->cur.nvis : 1, sizeof *names);
	path = calloc(app->cur.nvis ? app->cur.nvis : 1, sizeof *path);
	if (!names || !path) {
		wf_app_set_msg(app, wf_msg(MSG_XFER_NOMEM));
		goto done;
	}
	if (!wf_app_collect_targets(app, names, &n, &hidden)) {
		goto done;
	}
	if (n > WF_CLIPFILES_MAX) {
		wf_app_set_msg(app, wf_msg(MSG_CLIP_TOO_MANY_TO_COPY));
		goto done;
	}
	for (i = 0; i < n; i++) {
		wchar_t *full = NULL;

		if (WF_OK != wf_path_join(app->cwd, names[i], &full)) {
			ok = false;
			break;
		}
		path[i] = full;
	}
	if (ok) {
		app->clip_busy = true;
		if (WF_OK !=
		        app->clip_files.publish(app->clip_files.ctx,
		                ++app->clip_gen, (const wchar_t *const *)path,
		                n)) {
			app->clip_busy = false;
			wf_app_set_msg(app, wf_msg(MSG_CLIP_COPY_FAILED));
		}
	} else {
		wf_app_set_msg(app, wf_msg(MSG_XFER_NOMEM));
	}
done:
	for (i = 0; path && i < n; i++) {
		free(path[i]);
	}
	free(path);
	free(names);
}

void wf_app_cmd_clip_paste(WfApp *app)
{
	if (wf_app_archive_guard(app)) {
		return;
	}
	if (!app->clip_files.read) {
		wf_app_set_msg(app, wf_msg(MSG_CLIP_UNAVAILABLE));
		return;
	}
	if (app->clip_busy || WF_XB_IDLE != app->xfer_busy) {
		wf_app_set_msg(app, wf_msg(MSG_XFER_BUSY_CHECK));
		return;
	}
	app->clip_busy = true;
	if (WF_OK !=
	        app->clip_files.read(app->clip_files.ctx, ++app->clip_gen)) {
		app->clip_busy = false;
		wf_app_set_msg(app, wf_msg(MSG_CLIP_FAILED));
		return;
	}
	wf_app_set_msg(app, wf_msg(MSG_CLIP_READING));
}

/* The files must all be in one folder (the capture holds one): the folder, and
 * the names, or false. The folder is the same folder only when it is spelled the
 * same (ordinal, case included): in a folder that tells case apart a and A are
 * two folders, and whether one does is not known here, so a spelling that
 * differs only by case is not taken for the same folder. File names are compared
 * as they are spelled too: foo.txt and Foo.txt may be two files. A name given twice the same is one; two names that differ only by case
 * both go on, and the transfer meets them as it meets any two sources with one
 * destination. */
static bool one_folder(
        const WfClipFiles *f, wchar_t **dir, const wchar_t **names, size_t *n)
{
	size_t parent = wf_path_parent_len(f->path[0]);

	*n = 0;
	if (!parent) {
		return false;
	}
	for (size_t i = 0; i < f->n; i++) {
		size_t p = wf_path_parent_len(f->path[i]);
		const wchar_t *leaf = f->path[i] + p;
		bool repeated = false;

		if (p != parent ||
		        CSTR_EQUAL !=
		                CompareStringOrdinal(f->path[i], (int)p,
		                        f->path[0], (int)parent, FALSE)) {
			return false;
		}
		while (L'\\' == *leaf) {
			leaf++; /* the root keeps its backslash in the parent */
		}
		if (!*leaf) {
			return false; /* a drive root is not a file to copy */
		}
		for (size_t k = 0; k < *n; k++) {
			repeated |= CSTR_EQUAL ==
			        CompareStringOrdinal(
			                names[k], -1, leaf, -1, FALSE);
		}
		if (!repeated) {
			names[(*n)++] = leaf;
		}
	}
	*dir = wf_path_dup_n(f->path[0], parent);
	return NULL != *dir;
}

void wf_app_on_clip_files(WfApp *app, WfClipFiles *files)
{
	const wchar_t **names = NULL;
	wchar_t *dir = NULL;
	size_t n = 0;

	if (!files) {
		return;
	}
	if (files->gen != app->clip_gen || !app->clip_busy) {
		wf_clipfiles_free(files); /* an older request's answer */
		return;
	}
	app->clip_busy = false;
	if (files->published) { /* the answer to a Y */
		if (WF_CLIP_OK == files->why) {
			char text[sizeof app->msg];

			(void)wf_msgf(text, sizeof text, MSG_CLIP_COPIED,
			        &WF_ARG_N(files->n), 1);
			wf_app_set_msg(app, text);
		} else {
			wf_app_set_msg(app,
			        wf_msg(WF_CLIP_BUSY == files->why
			                        ? MSG_CLIP_BUSY
			                        : MSG_CLIP_COPY_FAILED));
		}
		goto done;
	}
	if (WF_CLIP_OK != files->why) {
		say_reason(app, files->why);
		goto done;
	}
	if (wf_clip_effect_is_cut(files->effect)) {
		/* a cut moves the originals: not built until it can be shown safe */
		wf_app_set_msg(app, wf_msg(MSG_CLIP_CUT_LATER));
		goto done;
	}
	names = calloc(files->n, sizeof *names);
	if (!names || !one_folder(files, &dir, names, &n)) {
		wf_app_set_msg(app,
		        wf_msg(names ? MSG_CLIP_SEVERAL_FOLDERS
		                     : MSG_XFER_NOMEM));
		goto done;
	}
	if (wf_app_unc_root_length(dir)) {
		/* the program's question about a share is asked for the folder on
		 * screen, not for one that another program names */
		wf_app_set_msg(app, wf_msg(MSG_CLIP_NETWORK));
		goto done;
	}
	if (!app->xfer.capture) {
		wf_app_set_msg(app, wf_msg(MSG_XFER_NOT_AVAILABLE));
		goto done;
	}
	app->capture_mode = WF_XFER_COPY;
	app->paste_after_capture = true; /* the capture, then the same as `p` */
	app->xfer_busy = WF_XB_CAPTURE;
	if (WF_OK !=
	        app->xfer.capture(app->xfer.ctx, ++app->xfer_gen, WF_XFER_COPY,
	                dir, names, n, 0)) {
		app->xfer_busy = WF_XB_IDLE;
		app->paste_after_capture = false;
		wf_app_set_msg(app, wf_msg(MSG_XFER_CAPTURE_START_FAILED));
		goto done;
	}
done:
	free(names);
	free(dir);
	wf_clipfiles_free(files);
}
