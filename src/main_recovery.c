#include "main_loop.h"
#include "recovery/wf_old_state.h"

static bool print_recovery_info(const WfRecoveryLocalInfo *info)
{
	WfBuf text;

	wf_buf_init(&text);
	if (wf_sanitize_utf16(info->volume_identity,
	            wcslen(info->volume_identity), &text) != WF_OK) {
		wf_buf_free(&text);
		return false;
	}
	printf("Recovery ID: %ls\nRecorded volume: %s\nState: %s\nAction: %s\n",
	        info->recovery_id, wf_buf_cstr(&text),
	        wf_recovery_local_state_name(info->state),
	        wf_recovery_abandon_action_name(info->action));
	wf_buf_free(&text);
	return true;
}

/* The abandon needs the exact ID typed back; anything else changes nothing.
 * On success `typed` holds the confirmed ID. */
static bool confirm_recovery_id(
        const WfRecoveryLocalInfo *info, wchar_t typed[64])
{
	char input[128];

	fputs("Only local journal/metadata will be abandoned. Type the exact "
	      "ID: ",
	        stdout);
	fflush(stdout);
	if (!fgets(input, sizeof input, stdin)) {
		fputs("\npiero: confirmation missing; nothing changed\n",
		        stderr);
		return false;
	}
	input[strcspn(input, "\r\n")] = 0;
	if (!MultiByteToWideChar(
	            CP_UTF8, MB_ERR_INVALID_CHARS, input, -1, typed, 64) ||
	        wcscmp(typed, info->recovery_id)) {
		fputs("piero: exact ID confirmation did not match; nothing "
		      "changed\n",
		        stderr);
		return false;
	}
	return true;
}

static bool abandon_recovery(WfRecoveryStore *store,
        const WfRecoveryLocalInfo *info,
        const wchar_t *typed)
{
	WfRecoveryAbandonReport report;

	if (wf_recovery_store_abandon(
	            store, info->recovery_id, typed, &report)) {
		puts("RECOVERY_ABANDONED: local metadata only; target volume "
		     "untouched");
		return true;
	}
	fprintf(stderr,
	        "piero: recovery abandon incomplete (Win32 %lu); %s -> %s; "
	        "action=%s journal=%u metadata=%u audit=%d\n",
	        report.error, wf_recovery_local_state_name(report.state_before),
	        wf_recovery_local_state_name(report.state_after),
	        wf_recovery_abandon_action_name(report.action),
	        report.journal_deleted, report.metadata_deleted, report.logged);
	return false;
}

/* --abandon-recovery: inspect, show, confirm, then abandon (local journal and
 * metadata only; the recorded target volume is never opened). */
int wf_main_recovery_command(const WfCli *cli)
{
	wchar_t *root = NULL, typed[64];
	WfStateKind kind = WF_STATE_FRESH;
	DWORD error;
	WfRecoveryStore *store = NULL;
	WfRecoveryLocalInfo info;
	int result = 3;
	WfRecoveryLookup got;

	if (WF_CLI_PURGE_STATE == cli->action) {
		fputs("piero: purge-state blocked: unresolved/unknown recovery "
		      "and owned worker/cache cleanup must be verified "
		      "first\n",
		        stderr);
		return 3;
	}
	if (wf_startup_state_path(L"LOCALAPPDATA", L"journal", &root, &kind,
	            &error) != WF_OK ||
	        !wf_recovery_store_open(root, false, &store, &error)) {
		fprintf(stderr,
		        "piero: recovery metadata unavailable (Win32 %lu); "
		        "target volume was not accessed\n",
		        error);
		goto done;
	}
	got = wf_recovery_store_inspect(store, cli->recovery_id, &info, &error);
	if (WF_RECOVERY_LOOKUP_NONE == got && WF_STATE_BOTH == kind) {
		/* both folders exist: the ID may be one the older folder holds */
		wf_recovery_store_close(store);
		store = NULL;
		free(root);
		root = NULL;
		if (wf_startup_legacy_path(L"LOCALAPPDATA", L"journal", &root,
		            &error) != WF_OK) {
			fprintf(stderr,
			        "piero: older recovery metadata unavailable "
				"(Win32 "
			        "%lu); target volume was not accessed\n",
			        error);
			goto done;
		}
		if (wf_recovery_store_open(root, false, &store, &error)) {
			got = wf_recovery_store_inspect(
			        store, cli->recovery_id, &info, &error);
		} else if (ERROR_FILE_NOT_FOUND != error &&
		        ERROR_PATH_NOT_FOUND != error) {
			fprintf(stderr,
			        "piero: older recovery metadata unavailable "
			        "(Win32 "
			        "%lu); target volume was not accessed\n",
			        error);
			goto done;
		}
		/* else: the older folder has no store, so it has no such ID */
	}
	if (WF_RECOVERY_LOOKUP_ERROR == got) {
		fprintf(stderr,
		        "piero: recovery metadata requires manual inspection "
		        "(Win32 %lu)\n",
		        error);
		goto done;
	}
	if (WF_RECOVERY_LOOKUP_NONE == got) {
		fputs("piero: recovery ID not found; nothing changed\n",
		        stderr);
		goto done;
	}
	if (!print_recovery_info(&info)) {
		goto done;
	}
	if (WF_RECOVERY_ABANDON_HOLD == info.action) {
		fprintf(stderr,
		        "piero: recovery retained for manual inspection: %s "
		        "(Win32 %lu)\n",
		        info.reason ? info.reason : "ownership incomplete",
		        info.error);
		goto done;
	}
	if (confirm_recovery_id(&info, typed) &&
	        abandon_recovery(store, &info, typed)) {
		result = 0;
	}
done:
	wf_recovery_store_close(store);
	free(root);
	return result;
}

/* What a look at a state folder found, as the causes the Info screen names. */
static void note_report(const WfStateReport *r, WfApp *app)
{
	app->recovery.unresolved += r->unresolved;
	if (r->unresolved) {
		app->recovery.causes |= WF_RECOVERY_NOTICE_PENDING;
	}
	if (r->store_unknown) {
		app->recovery.causes |= WF_RECOVERY_NOTICE_STORE;
	}
	if (r->limit) {
		app->recovery.causes |= WF_RECOVERY_NOTICE_LIMIT;
	}
	if (r->copy_legacy) {
		app->recovery.causes |= WF_RECOVERY_NOTICE_LEGACY;
	}
	if (r->copy_invalid) {
		app->recovery.causes |= WF_RECOVERY_NOTICE_COPY_INVALID;
	}
	if (r->store_unknown || r->limit || r->copy_legacy || r->copy_invalid) {
		app->recovery.unknown = true;
	}
	if (r->error) {
		app->recovery.error = r->error;
	}
}

/*
 * Both %LOCALAPPDATA%\piero and the older \wfile exist (DR-DRAFT-017): what
 * the older folder still holds must not be overlooked. Fills `r` from its
 * journal store, receipt inventory and drafts. False if a path cannot be made.
 */
static bool look_old_state(WfStateReport *r)
{
	wchar_t *path = NULL;
	DWORD error = 0;

	if (wf_startup_legacy_path(
	            L"LOCALAPPDATA", L"journal", &path, &error) != WF_OK) {
		r->store_unknown = true;
		r->error = error;
		return false;
	}
	wf_state_look_store(r, path);
	free(path);
	if (wf_startup_legacy_path(L"LOCALAPPDATA", L"copy-journal.txt", &path,
	            &error) != WF_OK) {
		r->copy_invalid = true;
		r->error = error;
		return false;
	}
	wf_state_look_copy_journal(r, path);
	free(path);
	if (wf_startup_legacy_path(L"LOCALAPPDATA", L"drafts", &path, &error) !=
	        WF_OK) {
		r->drafts = true;
		return false;
	}
	wf_state_look_drafts(r, path);
	free(path);
	return true;
}

bool wf_main_old_state_blocks(void)
{
	WfStateReport r = {0};

	return !look_old_state(&r) || wf_state_report_blocks(&r);
}

static void count_local_recovery(WfApp *app)
{
	WfStateReport r = {0};
	wchar_t *root = NULL;
	WfStateKind kind = WF_STATE_FRESH;
	DWORD error = 0;

	if (wf_startup_state_path(L"LOCALAPPDATA", L"journal", &root, &kind,
	            &error) != WF_OK) {
		app->recovery.unknown = true;
		app->recovery.causes |= WF_RECOVERY_NOTICE_STORE;
		app->recovery.error = error;
		return;
	}
	wf_state_look_store(&r, root);
	free(root);
	note_report(&r, app);
	if (WF_STATE_BOTH == kind) {
		WfStateReport old = {0};

		(void)look_old_state(&old);
		note_report(&old, app);
		if (wf_state_report_any(&old)) {
			/* "needs a look" even when only receipts or drafts are
			 * there: Settings > About says where */
			app->recovery.causes |= WF_RECOVERY_NOTICE_OLD_FOLDER;
			app->recovery.unknown = true;
		}
	}
}

void wf_main_refresh_recovery(void *ctx)
{
	Loop *lp = ctx;
	wchar_t *path = NULL;
	DWORD error = 0;
	WfCopyJournalInfo inventory = {0};
	if (lp->app.copy_jobs || lp->app.edit_busy) {
		return; /* do not reclassify a receipt being appended by our writer */
	}
	lp->app.recovery.unresolved = 0;
	lp->app.recovery.unknown = false;
	lp->app.recovery.causes = 0;
	lp->app.recovery.error = 0;
	count_local_recovery(&lp->app);
	if (wf_startup_state_path(L"LOCALAPPDATA", L"copy-journal.txt", &path,
	            NULL, &error) != WF_OK ||
	        !wf_cj_inspect(path, &inventory) ||
	        inventory.format == WF_CJ_LEGACY) {
		lp->app.recovery.unknown = true;
		lp->app.recovery.causes |= inventory.format == WF_CJ_LEGACY
		        ? WF_RECOVERY_NOTICE_LEGACY
		        : WF_RECOVERY_NOTICE_COPY_INVALID;
		lp->app.recovery.error = error ? error : inventory.error;
	}
	free(path);
	/* Read-only inspection never starts a disabled writer or resolves records. */
}
