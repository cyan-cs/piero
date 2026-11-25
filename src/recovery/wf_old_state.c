#include "wf_old_state.h"

#include <stdlib.h>
#include <string.h>

#include "../xfer/wf_copyjournal_win.h"
#include "wf_recovery_store.h"

#define STORE_LOOK_MAX 1024

void wf_state_look_store(WfStateReport *r, const wchar_t *root)
{
	WfRecoveryStore *store = NULL;
	WfRecoveryLocalInfo info;
	DWORD error = 0;
	size_t i;
	WfRecoveryLookup got;

	if (!wf_recovery_store_open(root, false, &store, &error)) {
		if (ERROR_FILE_NOT_FOUND != error &&
		        ERROR_PATH_NOT_FOUND != error) {
			r->store_unknown = true;
		} else if (INVALID_FILE_ATTRIBUTES !=
		        GetFileAttributesW(root)) {
			/* the folder is there but is not a store we can open */
			r->store_unknown = true;
		}
		if (r->store_unknown) {
			r->error = error;
		}
		return;
	}
	for (i = 0; i < STORE_LOOK_MAX; i++) {
		got = wf_recovery_store_get(store, i, &info, &error);
		if (WF_RECOVERY_LOOKUP_NONE == got) {
			break;
		}
		if (WF_RECOVERY_LOOKUP_ERROR == got) {
			r->store_unknown = true;
			r->error = error;
			break;
		}
		if (info.state == WF_RECOVERY_LOCAL_ABSENT ||
		        info.state == WF_RECOVERY_LOCAL_ABANDONED) {
			continue; /* proven terminal state, not unresolved work */
		}
		r->unresolved++;
		if (WF_RECOVERY_ABANDON_HOLD == info.action) {
			r->store_unknown = true;
		}
	}
	if (STORE_LOOK_MAX == i) {
		r->limit = true;
	}
	wf_recovery_store_close(store);
}

void wf_state_look_copy_journal(WfStateReport *r, const wchar_t *path)
{
	WfCopyJournalInfo inventory = {0};

	if (!wf_cj_inspect(path, &inventory)) {
		r->copy_invalid = true;
		r->error = inventory.error;
	} else if (WF_CJ_LEGACY == inventory.format) {
		r->copy_legacy = true;
	} else if (WF_CJ_INVALID == inventory.format) {
		r->copy_invalid = true;
		r->error = inventory.error;
	} else if (inventory.records) {
		r->receipts = true;
	}
}

void wf_state_look_drafts(WfStateReport *r, const wchar_t *dir)
{
	WIN32_FIND_DATAW data;
	HANDLE find;
	size_t n = wcslen(dir);
	wchar_t *pattern = malloc((n + 3) * sizeof *pattern);
	bool any = false;

	if (!pattern) {
		r->drafts = true; /* cannot tell: say there may be some */
		return;
	}
	memcpy(pattern, dir, n * sizeof *pattern);
	pattern[n] = L'\\';
	pattern[n + 1] = L'*';
	pattern[n + 2] = L'\0';
	find = FindFirstFileW(pattern, &data);
	free(pattern);
	if (INVALID_HANDLE_VALUE == find) {
		DWORD error = GetLastError();

		if (ERROR_FILE_NOT_FOUND != error &&
		        ERROR_PATH_NOT_FOUND != error) {
			r->drafts = true;
			r->error = error;
		}
		return;
	}
	do {
		if (wcscmp(data.cFileName, L".") &&
		        wcscmp(data.cFileName, L"..")) {
			any = true;
		}
	} while (!any && FindNextFileW(find, &data));
	FindClose(find);
	if (any) {
		r->drafts = true;
	}
}

bool wf_state_report_blocks(const WfStateReport *r)
{
	return r->unresolved || r->store_unknown || r->limit ||
	        r->copy_legacy || r->copy_invalid;
}

bool wf_state_report_any(const WfStateReport *r)
{
	return wf_state_report_blocks(r) || r->receipts || r->drafts;
}
