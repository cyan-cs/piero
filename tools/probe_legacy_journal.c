/* Read-only look at a copy journal in the OLD format. Opens the file for
 * reading only (no lock, no write, no truncate, no rename), prints counts and
 * a verdict, and never prints a path, an id or any line of the file.
 *
 *   probe_legacy_journal <journal-file>
 *
 * The verdict describes what the journal's own records prove, not the state
 * of any file on disk. It is never permission to delete anything. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "path/wf_path.h"
#include "xfer/wf_copyjournal_legacy.h"

#define MAX_BYTES (16u << 20)
#define RECEIPT_HEADER "PIERO-COPY-RECEIPTS 2\n"

int wmain(int argc, wchar_t **argv)
{
	wchar_t *extended = NULL;
	HANDLE h;
	LARGE_INTEGER size;
	FILE_STANDARD_INFO st;
	char *data = NULL;
	DWORD got = 0;
	WfLegacyJournalReport r;

	if (2 != argc) {
		fputs("usage: probe_legacy_journal <journal-file>\n", stderr);
		return 2;
	}
	if (WF_OK != wf_path_to_extended(argv[1], &extended)) {
		puts("open=FAIL reason=bad-path");
		return 2;
	}
	h = CreateFileW(extended, GENERIC_READ,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING,
	        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL, NULL);
	free(extended);
	if (INVALID_HANDLE_VALUE == h) {
		printf("open=FAIL win32=%lu\n", GetLastError());
		return 2;
	}
	if (!GetFileInformationByHandleEx(
	            h, FileStandardInfo, &st, sizeof st) ||
	        st.Directory || st.EndOfFile.QuadPart < 0 ||
	        (unsigned long long)st.EndOfFile.QuadPart > MAX_BYTES) {
		puts("stat=FAIL reason=not-a-plain-file-or-too-large");
		CloseHandle(h);
		return 2;
	}
	size = st.EndOfFile;
	data = malloc((size_t)size.QuadPart + 1);
	if (!data ||
	        (size.QuadPart &&
	                (!ReadFile(h, data, (DWORD)size.QuadPart, &got, NULL) ||
	                        got != (DWORD)size.QuadPart))) {
		printf("read=FAIL win32=%lu\n", GetLastError());
		free(data);
		CloseHandle(h);
		return 2;
	}
	CloseHandle(h);
	if (size.QuadPart >= (LONGLONG)(sizeof RECEIPT_HEADER - 1) &&
	        !memcmp(data, RECEIPT_HEADER, sizeof RECEIPT_HEADER - 1)) {
		puts("format=CURRENT (has the receipts header; not a legacy "
		     "journal)");
		free(data);
		return 0;
	}
	if (!wf_cj_legacy_analyze(data, got, &r)) {
		puts("analyze=FAIL");
		free(data);
		return 2;
	}
	free(data);
	printf("format=HEADERLESS bytes=%zu lines=%zu intents=%zu created=%zu "
	       "asides=%zu verified=%zu discards=%zu gone=%zu malformed=%zu "
	       "unresolved_created=%zu unresolved_asides=%zu "
	       "unresolved_intents=%zu unterminated_tail=%d\n",
	        r.bytes, r.lines, r.intents, r.created, r.asides, r.verified,
	        r.discards, r.gone, r.malformed, r.unresolved_created,
	        r.unresolved_asides, r.unresolved_intents,
	        (int)r.unterminated_tail);
	printf("verdict=%s\n",
	        r.empty ? "EMPTY (nothing recorded; proves nothing)"
	                : r.complete_proven
	                ? "RECORDS_CLOSED (every temporary the journal opened "
	                  "it also "
	                  "closed; the files themselves were not examined)"
	                : "NOT_PROVEN_COMPLETE (keep the 'needs review' "
	                  "state)");
	puts("modified=NO");
	return 0;
}
