/*
 * Enter on a file: open it with the associated program (design 8.3). The
 * launch goes through the launch port, which re-checks the file's identity
 * and calls ShellExecuteExW off the UI thread. Files whose type can run code
 * ask for one confirmation first; nothing is ever elevated.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../archive/wf_vpath.h"
#include "../fs/wf_loc.h"
#include "../fs/wf_ntfsfacts.h"
#include "../fs/wf_sftpplace_win.h"
#include "../path/wf_path.h"
#include "../text/wf_text.h"
#include "wf_app_priv.h"

static void pending_clear(WfApp *app)
{
	free(app->open_pending.path);
	memset(&app->open_pending, 0, sizeof app->open_pending);
}

void wf_app_open_destroy(WfApp *app)
{
	pending_clear(app);
	free(app->launch_directory);
	app->launch_directory = NULL;
}

void wf_app_set_open_off(WfApp *app, WfMsg why)
{
	app->open_off_set = true;
	app->open_off = why;
}

WfStatus wf_app_set_launch(
        WfApp *app, WfLaunchPort port, const wchar_t *neutral_directory)
{
	wchar_t *copy = NULL;

	if (!app) {
		return WF_E_INVAL;
	}
	if (neutral_directory) {
		copy = wf_path_dup(neutral_directory);
		if (!copy) {
			return WF_E_NOMEM;
		}
	}
	free(app->launch_directory);
	app->launch_directory = copy;
	app->launch = port;
	return WF_OK;
}

void wf_app_set_terminal(WfApp *app, WfLaunchPort port)
{
	if (app) {
		app->terminal = port;
	}
}

/* A press within this time of the one before it, started or not, is the same
 * press: held keys open one window, however long they are held. */
#define TERMINAL_REPEAT_MS 1000u

/* Open Terminal Here. The folder is the one on screen (the tab's own), never
 * the entry under the cursor. */
void wf_app_cmd_terminal(WfApp *app)
{
	WfLaunchRequest req;
	WfStatus st;
	uint32_t gen = app->launch_gen + 1;
	bool repeat;

	if (!app->terminal.submit) {
		wf_app_set_msg(app, wf_msg(MSG_TERMINAL_OFF));
		return;
	}
	if (!app->cwd) {
		return;
	}
	repeat = app->terminal_ms && app->now_ms >= app->terminal_ms &&
	        app->now_ms - app->terminal_ms < TERMINAL_REPEAT_MS;
	app->terminal_ms =
	        app->now_ms ? app->now_ms : 1; /* every press counts */
	if (repeat) {
		return;
	}
	if (app->launch_busy) {
		wf_app_set_msg(app, wf_msg(MSG_OPEN_BUSY));
		return;
	}
	/* a folder that is not an ordinary one of a local drive: not guessed at */
	if (wf_vpath_is(app->cwd) || wf_sftp_place_is(app->cwd) ||
	        wf_pcroot_is(app->cwd) || wf_wsl_unc_is(app->cwd) ||
	        wf_ntfsfacts_remote(app->cwd)) {
		wf_app_set_msg(app, wf_msg(MSG_TERMINAL_LOCAL));
		return;
	}
	/* the folder is looked at, and the terminal started, by the worker: this
	 * thread asks the disk for nothing */
	memset(&req, 0, sizeof req);
	req.terminal = true;
	req.directory = app->cwd;
	st = app->terminal.submit(app->terminal.ctx, gen, &req);
	if (WF_OK != st) {
		wf_app_set_msg(app, wf_msg(MSG_TERMINAL_FAILED_START));
		return;
	}
	app->launch_gen = gen;
	app->launch_busy = true;
	app->launch_terminal = true;
	wf_app_set_msg(app, wf_msg(MSG_TERMINAL_STARTING));
}

/* The answer to a terminal request: `dir` is the folder it was for. */
static void terminal_answer(
        WfApp *app, const WfLaunchOutcome *outcome, const wchar_t *dir)
{
	WfBuf shown;
	char text[160], name[96], code[16];
	WfMsg pick = WF_OK == outcome->st    ? MSG_TERMINAL_OPENED
	        : WF_E_INVAL == outcome->st  ? MSG_TERMINAL_LOCAL
	        : WF_E_NOENT == outcome->st  ? MSG_TERMINAL_GONE
	        : WF_E_ACCESS == outcome->st ? MSG_TERMINAL_DENIED
	                                     : MSG_TERMINAL_FAILED;

	wf_buf_init(&shown);
	if (!dir || WF_OK != wf_sanitize_utf16(dir, wcslen(dir), &shown)) {
		(void)wf_buf_puts(&shown, "?");
	}
	wf_utf8_elide(name, sizeof name, wf_buf_cstr(&shown), 60);
	wf_buf_free(&shown);
	snprintf(code, sizeof code, "%lu", outcome->error);
	(void)wf_msgf(text, sizeof text, pick,
	        (WfMsgArg[]){WF_ARG_S(name), WF_ARG_S(code)}, 2);
	app->msg_keep_until = app->now_ms + WF_RECEIPT_KEEP_MS;
	wf_app_set_msg(app, text);
}

/* Sends the request. `path` and `id` were checked by the caller. */
static void send_request(WfApp *app, const wchar_t *path, const WfFileId *id)
{
	WfLaunchRequest req;
	wchar_t *folder = NULL;
	WfStatus st;
	uint32_t gen = app->launch_gen + 1;

	memset(&req, 0, sizeof req);
	req.path = path;
	req.id = *id;
	if (app->launch_directory) {
		req.directory = app->launch_directory;
	} else {
		folder = wf_path_dup_n(path, wf_path_parent_len(path));
		if (!folder) {
			wf_app_set_error(app, WF_E_NOMEM);
			return;
		}
		req.directory = folder;
	}
	st = app->launch.submit(app->launch.ctx, gen, &req);
	free(folder);
	if (WF_OK != st) {
		wf_app_set_msg(app, wf_msg(MSG_OPEN_START_FAILED));
		return;
	}
	app->launch_gen = gen;
	app->launch_busy = true;
	wf_app_set_msg(app, wf_msg(MSG_OPEN_OPENING));
}

static void open_yes(WfApp *app, void *ctx)
{
	(void)ctx;
	if (app->open_pending.path && !app->launch_busy) {
		send_request(
		        app, app->open_pending.path, &app->open_pending.id);
	} else if (app->launch_busy) {
		wf_app_set_msg(app, wf_msg(MSG_OPEN_BUSY));
	}
	pending_clear(app);
}

void wf_app_open_selected(WfApp *app)
{
	const WfEntry *entry = wf_view_current(&app->cur);
	wchar_t *path = NULL;
	char title[WF_CONFIRM_LINE_BYTES];
	const char *lines[4];
	WfStatus st;

	if (!entry || wf_app_archive_guard(app)) {
		return;
	}
	if (wf_wsl_unc_is(app->cwd)) {
		wf_app_set_msg(app, wf_msg(MSG_WSL_READONLY));
		return;
	}
	if (wf_sftp_place_is(app->cwd)) {
		wf_app_set_msg(app, wf_msg(MSG_SFTP_READONLY));
		return;
	}
	if (!app->launch.submit) {
		wf_app_set_msg(app,
		        wf_msg(app->open_off_set ? app->open_off
		                                 : MSG_OPEN_GATES));
		return;
	}
	if (app->launch_busy) {
		wf_app_set_msg(app, wf_msg(MSG_OPEN_BUSY));
		return;
	}
	if (entry->flags & WF_ENT_CLOUD) {
		wf_app_set_msg(app, wf_msg(MSG_OPEN_CLOUD));
		return;
	}
	if (entry->flags & WF_ENT_LINK) {
		wf_app_set_msg(app, wf_msg(MSG_OPEN_LINK));
		return;
	}
	if (!wf_fileid_usable_to_read(&entry->identity)) {
		wf_app_set_msg(app, wf_msg(MSG_OPEN_IDENTITY));
		return;
	}
	st = wf_path_join(app->cwd, entry->name, &path);
	if (WF_OK != st) {
		wf_app_set_error(app, st);
		return;
	}
	if (!wf_launch_needs_confirm(entry->name)) {
		send_request(app, path, &entry->identity);
		free(path);
		return;
	}
	pending_clear(app);
	app->open_pending.path = path;
	app->open_pending.id = entry->identity;
	snprintf(title, sizeof title, "%s", entry->display);
	lines[0] = wf_msg(MSG_OPEN_CONFIRM_TITLE);
	lines[1] = title;
	lines[2] = wf_msg(MSG_OPEN_CONFIRM_1);
	lines[3] = wf_msg(MSG_OPEN_CONFIRM_2);
	st = wf_app_confirm_begin_ex(app, lines, 4, 'o',
	        wf_msg(MSG_OPEN_CONFIRM_KEYS), open_yes, NULL, NULL);
	if (WF_OK != st) {
		pending_clear(app);
		wf_app_set_error(app, st);
	}
}

void wf_app_on_launch(WfApp *app,
        uint32_t gen,
        const WfLaunchOutcome *outcome,
        const wchar_t *name)
{
	WfBuf shown;
	char text[128];

	if (!app || !outcome || gen != app->launch_gen || !app->launch_busy) {
		return;
	}
	app->launch_busy = false;
	if (app->launch_terminal) {
		app->launch_terminal = false;
		terminal_answer(app, outcome, name);
		return;
	}
	wf_buf_init(&shown);
	if (!name || WF_OK != wf_sanitize_utf16(name, wcslen(name), &shown)) {
		wf_buf_puts(&shown, "?");
	}
	{
		char code[16];
		const WfMsgArg args[2] = {
		        WF_ARG_S(wf_buf_cstr(&shown)), WF_ARG_S(code)};

		snprintf(code, sizeof code, "%lu", outcome->error);
		(void)wf_msgf(text, sizeof text,
		        WF_OK == outcome->st                 ? MSG_OPEN_OPENED
		                : WF_E_INVAL == outcome->st  ? MSG_OPEN_REFUSED
		                : WF_E_NOENT == outcome->st  ? MSG_OPEN_GONE
		                : WF_E_ACCESS == outcome->st ? MSG_OPEN_DENIED
		                                             : MSG_OPEN_FAILED,
		        args, 2);
	}
	wf_buf_free(&shown);
	app->msg_keep_until = app->now_ms + WF_RECEIPT_KEEP_MS;
	wf_app_set_msg(app, text);
}
