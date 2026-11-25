/* Read-only inventory of the current user's local recovery state.
 * Never prints payload paths, recovery IDs or journal contents. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "config/wf_config_win.h"
#include "recovery/wf_recovery_store.h"
#include "xfer/wf_copyjournal_win.h"

int main(void)
{
	wchar_t *path = NULL;
	DWORD error = 0;
	WfCopyJournalInfo copy = {0};
	if (wf_startup_state_path(L"LOCALAPPDATA", L"copy-journal.txt", &path,
	            NULL, &error) != WF_OK) {
		printf("copy_path_error=%lu\n", error);
		return 1;
	}
	bool ok = wf_cj_inspect(path, &copy);
	printf("copy_readable=%d format=%d bytes=%llu records=%zu error=%lu\n",
	        (int)ok, (int)copy.format, (unsigned long long)copy.bytes,
	        copy.records, copy.error);
	free(path);
	path = NULL;
	WfRecoveryStore *store = NULL;
	if (wf_startup_state_path(L"LOCALAPPDATA", L"journal", &path, NULL,
	            &error) != WF_OK) {
		return 1;
	}
	bool opened = wf_recovery_store_open(path, false, &store, &error);
	printf("recovery_open=%d error=%lu\n", (int)opened, error);
	size_t pending = 0, terminal = 0, unknown = 0, count = 0;
	if (opened) {
		for (; count < 1024; count++) {
			WfRecoveryLocalInfo info;
			WfRecoveryLookup got = wf_recovery_store_get(
			        store, count, &info, &error);
			if (got == WF_RECOVERY_LOOKUP_NONE) {
				break;
			}
			if (got == WF_RECOVERY_LOOKUP_ERROR) {
				unknown++;
				break;
			}
			if (info.state == WF_RECOVERY_LOCAL_ABSENT ||
			        info.state == WF_RECOVERY_LOCAL_ABANDONED) {
				terminal++;
			} else {
				pending++;
				unknown +=
				        info.action == WF_RECOVERY_ABANDON_HOLD;
			}
		}
	}
	printf("pending=%zu terminal=%zu unknown=%zu limit=%d error=%lu\n",
	        pending, terminal, unknown, count == 1024, error);
	wf_recovery_store_close(store);
	free(path);
	return 0; /* Inventory completed; not a declaration of safety. */
}
