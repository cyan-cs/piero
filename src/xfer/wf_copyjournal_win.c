#include "wf_copyjournal_win.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../path/wf_path.h"
#include "../base/wf_macro.h"

#define LINE_MAX_BYTES 6000u
#define JOURNAL_MAX_BYTES (16u << 20)
static const char receipt_header[] = "PIERO-COPY-RECEIPTS 2\n";

struct WfCopyJournal
{
	HANDLE file;
	DWORD pid;
	uint64_t start;
	bool failed;
};

/* Receipts are an untrusted inventory, never deletion/rename authority.
 * A whole-file byte lock serializes cooperating readers/writers without waiting.
 * It does not protect against mapped writes; inspection still cannot mutate
 * any recorded target or truncate the log, even if its contents are forged. */
static bool journal_lock(HANDLE file)
{
	OVERLAPPED lock = {0};
	return LockFileEx(file,
	               LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0,
	               MAXDWORD, 0, &lock) != 0;
}

static bool journal_unlock(HANDLE file)
{
	OVERLAPPED lock = {0};
	return UnlockFileEx(file, 0, MAXDWORD, 0, &lock) != 0;
}

static bool journal_size(HANDLE file, uint64_t *bytes)
{
	FILE_STANDARD_INFO standard;
	FILE_ATTRIBUTE_TAG_INFO tag;
	if (GetFileType(file) != FILE_TYPE_DISK) {
		SetLastError(ERROR_ACCESS_DENIED);
		return false;
	}
	if (!GetFileInformationByHandleEx(
	            file, FileStandardInfo, &standard, sizeof standard) ||
	        !GetFileInformationByHandleEx(
	                file, FileAttributeTagInfo, &tag, sizeof tag)) {
		return false;
	}
	if (standard.Directory || standard.DeletePending ||
	        standard.NumberOfLinks != 1 ||
	        (tag.FileAttributes &
	                (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE |
	                        FILE_ATTRIBUTE_RECALL_ON_OPEN |
	                        FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS))) {
		SetLastError(ERROR_ACCESS_DENIED);
		return false;
	}
	if (standard.EndOfFile.QuadPart < 0 ||
	        (uint64_t)standard.EndOfFile.QuadPart > JOURNAL_MAX_BYTES) {
		SetLastError(ERROR_FILE_TOO_LARGE);
		return false;
	}
	*bytes = (uint64_t)standard.EndOfFile.QuadPart;
	return true;
}

static bool hexadecimal(const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (!((s[i] >= '0' && s[i] <= '9') ||
		            (s[i] >= 'a' && s[i] <= 'f') ||
		            (s[i] >= 'A' && s[i] <= 'F'))) {
			return false;
		}
	}
	return n != 0;
}

static bool receipt_valid(const char *s, size_t n)
{
	if (n < 9 || !strchr("ICAVGD", s[0]) || s[1] != ' ') {
		return false;
	}
	size_t begin = 2, end = begin;
	uint64_t pid = 0;
	while (end < n && s[end] != ' ') {
		if (s[end] < '0' || s[end] > '9' || end - begin >= 10) {
			return false;
		}
		pid = pid * 10 + (unsigned)(s[end++] - '0');
	}
	if (end == begin || pid == 0 || pid > MAXDWORD || end == n) {
		return false;
	}
	begin = ++end;
	while (end < n && s[end] != ' ') {
		end++;
	}
	if (end == n || end - begin > 16 ||
	        !hexadecimal(s + begin, end - begin)) {
		return false;
	}
	begin = ++end;
	while (end < n && s[end] != ' ') {
		end++;
	}
	if (end == n) {
		return false;
	}
	if (s[0] == 'I') {
		if (end - begin != 1 || s[begin] != '-') {
			return false;
		}
	} else if (end - begin != 49 || s[begin + 16] != ':' ||
	        !hexadecimal(s + begin, 16) ||
	        !hexadecimal(s + begin + 17, 32)) {
		return false;
	}
	if (++end == n) {
		return false;
	}
	if (strchr("ICA", s[0]) && n - end == 1 && s[end] == '-') {
		return false;
	}
	size_t separators = 0;
	for (size_t i = end; i < n; i++) {
		if (s[i] == '|') {
			separators++;
		}
		if (s[i] == '%') {
			if (n - i < 3 || !hexadecimal(s + i + 1, 2)) {
				return false;
			}
			i += 2;
		}
	}
	if ((s[0] == 'A' && separators != 1) || (s[0] != 'A' && separators)) {
		return false;
	}
	if (strchr("VGD", s[0]) && (n - end != 1 || s[end] != '-')) {
		return false;
	}
	return MultiByteToWideChar(
	               CP_UTF8, MB_ERR_INVALID_CHARS, s, (int)n, NULL, 0) != 0;
}

/* The lock is held by the caller. No paths in the contents are opened. */
static bool inspect_handle(HANDLE file, WfCopyJournalInfo *out)
{
	LARGE_INTEGER zero = {0};
	char header[sizeof receipt_header - 1], buffer[4096],
	        line[LINE_MAX_BYTES];
	DWORD got = 0;
	size_t length = 0;
	if (!journal_size(file, &out->bytes)) {
		return false;
	}
	out->format = WF_CJ_EMPTY;
	if (!out->bytes) {
		return true;
	}
	if (!SetFilePointerEx(file, zero, NULL, FILE_BEGIN) ||
	        !ReadFile(file, header, sizeof header, &got, NULL)) {
		return false;
	}
	if (got != sizeof header ||
	        memcmp(header, receipt_header, sizeof header)) {
		out->format = WF_CJ_LEGACY;
		return true;
	}
	out->format = WF_CJ_RECEIPTS;
	uint64_t consumed = got;
	while (consumed < out->bytes) {
		DWORD want = out->bytes - consumed < sizeof buffer
		        ? (DWORD)(out->bytes - consumed)
		        : sizeof buffer;
		if (!ReadFile(file, buffer, want, &got, NULL)) {
			return false;
		}
		if (got != want) {
			SetLastError(ERROR_READ_FAULT);
			return false;
		}
		consumed += got;
		for (DWORD i = 0; i < got; i++) {
			unsigned char c = (unsigned char)buffer[i];
			if (c == '\n') {
				if (!receipt_valid(line, length)) {
					goto invalid;
				}
				out->records++;
				length = 0;
			} else {
				if (c < 32 || c == 127 ||
				        length == sizeof line) {
					goto invalid;
				}
				line[length++] = (char)c;
			}
		}
	}
	if (length) {
		goto invalid;
	}
	return true;
invalid:
	out->format = WF_CJ_INVALID;
	SetLastError(ERROR_BAD_FORMAT);
	return false;
}

bool wf_cj_inspect(const wchar_t *path, WfCopyJournalInfo *out)
{
	WfCopyJournalInfo local = {0};
	wchar_t *extended = NULL;
	if (!out) {
		out = &local;
	}
	memset(out, 0, sizeof *out);
	out->format = WF_CJ_INVALID;
	if (!path || wf_path_to_extended(path, &extended) != WF_OK) {
		out->error = ERROR_INVALID_NAME;
		return false;
	}
	HANDLE file = CreateFileW(extended, GENERIC_READ,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL, NULL);
	DWORD open_error = file == INVALID_HANDLE_VALUE ? GetLastError() : 0;
	free(extended);
	if (file == INVALID_HANDLE_VALUE) {
		out->error = open_error;
		if (open_error == ERROR_FILE_NOT_FOUND) {
			out->format = WF_CJ_MISSING;
		}
		return open_error == ERROR_FILE_NOT_FOUND;
	}
	bool locked = journal_lock(file);
	bool ok = locked && inspect_handle(file, out);
	DWORD error = ok ? 0 : GetLastError();
	if (locked && !journal_unlock(file) && ok) {
		error = GetLastError();
		ok = false;
	}
	CloseHandle(file);
	out->error = error;
	return ok;
}

WfCopyJournal *wf_cj_open_why(
        const wchar_t *path, WfCopyJournalOpenWhy *why, DWORD *error)
{
	WfCopyJournal *j = calloc(1, sizeof *j);
	wchar_t *extended = NULL;
	FILETIME created, exited, kernel, user;
	WfCopyJournalInfo info = {0};
	WfCopyJournalOpenWhy reason = WF_CJ_OPEN_NOMEM;
	DWORD failed = 0;
	bool locked, ok;

	if (!j) {
		failed = ERROR_NOT_ENOUGH_MEMORY;
		goto fail_nothing_to_close;
	}
	j->file = INVALID_HANDLE_VALUE;
	j->pid = GetCurrentProcessId();
	if (!GetProcessTimes(
	            GetCurrentProcess(), &created, &exited, &kernel, &user)) {
		reason = WF_CJ_OPEN_PROCESS;
		failed = GetLastError();
		goto fail;
	}
	j->start = ((uint64_t)created.dwHighDateTime << 32) |
	        created.dwLowDateTime;
	if (!path) {
		*why = WF_CJ_OPEN_OK;
		*error = 0;
		return j;
	}
	if (wf_path_to_extended(path, &extended) != WF_OK) {
		reason = WF_CJ_OPEN_PATH;
		goto fail;
	}
	j->file = CreateFileW(extended,
	        GENERIC_READ | FILE_APPEND_DATA | SYNCHRONIZE,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
	        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL |
	                FILE_FLAG_WRITE_THROUGH,
	        NULL);
	failed = GetLastError(); /* before free() can change it */
	free(extended);
	if (j->file == INVALID_HANDLE_VALUE) {
		reason = WF_CJ_OPEN_FILE;
		goto fail;
	}
	locked = journal_lock(j->file);
	if (!locked) {
		reason = WF_CJ_OPEN_LOCK;
		failed = GetLastError();
		goto fail;
	}
	ok = inspect_handle(j->file, &info);
	if (!ok) {
		reason = WF_CJ_OPEN_INSPECT;
		failed = GetLastError();
	} else if (info.format != WF_CJ_EMPTY &&
	        info.format != WF_CJ_RECEIPTS) {
		reason = WF_CJ_OPEN_FORMAT; /* kept as it is; not for append */
		ok = false;
	} else if (info.format == WF_CJ_EMPTY) {
		DWORD put = 0;

		ok = WriteFile(j->file, receipt_header,
		             sizeof receipt_header - 1, &put, NULL) &&
		        put == sizeof receipt_header - 1 &&
		        FlushFileBuffers(j->file);
		if (!ok) {
			reason = WF_CJ_OPEN_HEADER;
			failed = GetLastError();
		}
	}
	if (!journal_unlock(j->file) && ok) {
		reason = WF_CJ_OPEN_LOCK;
		failed = GetLastError();
		ok = false;
	}
	if (!ok) {
		goto fail;
	}
	*why = WF_CJ_OPEN_OK;
	*error = 0;
	return j;
fail:
	wf_cj_close(j); /* closes the handle only if it was opened */
fail_nothing_to_close:
	*why = reason;
	*error = failed;
	return NULL;
}

WfCopyJournal *wf_cj_open(const wchar_t *path)
{
	WfCopyJournalOpenWhy why;
	DWORD error;

	return wf_cj_open_why(path, &why, &error);
}

void wf_cj_close(WfCopyJournal *j)
{
	if (!j) {
		return;
	}
	if (j->file != INVALID_HANDLE_VALUE) {
		CloseHandle(j->file);
	}
	free(j);
}

static bool append_line(WfCopyJournal *j, const char *line, size_t n)
{
	if (!j || j->file == INVALID_HANDLE_VALUE) {
		return true;
	}
	if (j->failed) {
		return false;
	}
	if (!journal_lock(j->file)) {
		return false;
	}
	uint64_t size = 0;
	DWORD put = 0;
	char header[sizeof receipt_header - 1], last = 0;
	DWORD got = 0;
	LARGE_INTEGER position = {0};
	bool ok = journal_size(j->file, &size) && size >= sizeof header &&
	        n <= JOURNAL_MAX_BYTES - size &&
	        SetFilePointerEx(j->file, position, NULL, FILE_BEGIN) &&
	        ReadFile(j->file, header, sizeof header, &got, NULL) &&
	        got == sizeof header &&
	        !memcmp(header, receipt_header, sizeof header);
	if (ok) {
		position.QuadPart = (LONGLONG)size - 1;
		ok = SetFilePointerEx(j->file, position, NULL, FILE_BEGIN) &&
		        ReadFile(j->file, &last, 1, &got, NULL) && got == 1 &&
		        last == '\n';
	}
	if (ok) {
		ok = WriteFile(j->file, line, (DWORD)n, &put, NULL) &&
		        put == n && FlushFileBuffers(j->file);
	}
	DWORD error = ok ? 0 : GetLastError();
	if (!journal_unlock(j->file) && ok) {
		error = GetLastError();
		ok = false;
	}
	if (!ok) {
		j->failed = true;
	}
	if (!ok) {
		SetLastError(error ? error : ERROR_BAD_FORMAT);
	}
	return ok;
}

static void id_hex(const FILE_ID_INFO *id, char *out, size_t cap)
{
	size_t i;
	int n = snprintf(out, cap,
	        "%016llx:", (unsigned long long)id->VolumeSerialNumber);

	for (i = 0; i < 16 && (size_t)n + 2 < cap; i++) {
		n += snprintf(out + n, cap - (size_t)n, "%02x",
		        id->FileId.Identifier[i]);
	}
}

static bool path_utf8(const wchar_t *path, char *out, size_t cap)
{
	char raw[LINE_MAX_BYTES];
	int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1,
	        raw, sizeof raw, NULL, NULL);
	size_t used = 0;
	static const char hex[] = "0123456789ABCDEF";
	if (n <= 0) {
		return false;
	}
	for (int i = 0; i < n - 1; i++) {
		unsigned char c = (unsigned char)raw[i];
		size_t need = c < 32 || c == 127 || c == '%' ? 3 : 1;
		if (used >= cap || need >= cap - used) {
			return false;
		}
		if (need == 3) {
			out[used++] = '%';
			out[used++] = hex[c >> 4];
			out[used++] = hex[c & 15];
		} else {
			out[used++] = (char)c;
		}
	}
	out[used] = 0;
	return true;
}

static bool
record(WfCopyJournal *j, char kind, const wchar_t *path, const FILE_ID_INFO *id)
{
	char line[LINE_MAX_BYTES], idtext[64] = "-",
	                           utf8[LINE_MAX_BYTES - 200] = "-";
	int n;

	if (!j || INVALID_HANDLE_VALUE == j->file) {
		return true;
	}
	if (id) {
		id_hex(id, idtext, sizeof idtext);
	}
	if (path && !path_utf8(path, utf8, sizeof utf8)) {
		return false;
	}
	n = snprintf(line, sizeof line, "%c %lu %llx %s %s\n", kind,
	        (unsigned long)j->pid, (unsigned long long)j->start, idtext,
	        utf8);
	return n > 0 && (size_t)n < sizeof line &&
	        receipt_valid(line, (size_t)n - 1) &&
	        append_line(j, line, (size_t)n);
}

bool wf_cj_intent(WfCopyJournal *j, const wchar_t *tmp_path)
{
	return record(j, 'I', tmp_path, NULL);
}

bool wf_cj_created(
        WfCopyJournal *j, const wchar_t *tmp_path, const FILE_ID_INFO *id)
{
	return record(j, 'C', tmp_path, id);
}

/* The path field of an A record is "<dest>|<aside>" (no Windows path holds a
 * '|'). */
bool wf_cj_aside(WfCopyJournal *j,
        const wchar_t *dest,
        const wchar_t *aside,
        const FILE_ID_INFO *old_id)
{
	wchar_t both[LINE_MAX_BYTES / 4];

	if (!j || INVALID_HANDLE_VALUE == j->file) {
		return true;
	}
	if (swprintf(both, WF_ARRAY_LEN(both), L"%ls|%ls", dest, aside) < 0) {
		return false;
	}
	return record(j, 'A', both, old_id);
}

bool wf_cj_discard(WfCopyJournal *j, const FILE_ID_INFO *old_id)
{
	return record(j, 'D', NULL, old_id);
}

bool wf_cj_verified(WfCopyJournal *j, const FILE_ID_INFO *id)
{
	return record(j, 'V', NULL, id);
}

bool wf_cj_gone(WfCopyJournal *j, const FILE_ID_INFO *id)
{
	return record(j, 'G', NULL, id);
}
