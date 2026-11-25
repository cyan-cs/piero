#include "../async/wf_blocking.h"
#include "wf_efile_win.h"

#include <aclapi.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../fs/owned_delete.h"
#include "../fs/wf_loc.h"
#include "../fs/wf_sftpplace_win.h"
#include "../fs/wf_wslrm_win.h"
#include "../path/wf_path.h"
#include "../xfer/wf_copyjournal_win.h"
#include "../base/wf_macro.h"

#define STREAM_MAX_COUNT 16u
#define STREAM_MAX_BYTES (1u << 20)
#define FREE_SPACE_MARGIN (1u << 20)
#define CHUNK (1u << 20)

#define BAD_ATTRIBUTES                                                         \
	(FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN |              \
	        FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)

bool wf_efile_test_force_wsl;

static bool is_wsl(const wchar_t *path)
{
	return wf_efile_test_force_wsl || wf_wsl_unc_is(path);
}

bool wf_efile_sha256(const void *data, size_t n, uint8_t out[32])
{
	const uint8_t *p = data;
	BCRYPT_ALG_HANDLE alg = NULL;
	BCRYPT_HASH_HANDLE h = NULL;
	bool ok = false;

	if (BCryptOpenAlgorithmProvider(
	            &alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0) {
		return false;
	}
	if (BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) < 0) {
		goto done;
	}
	while (n) {
		ULONG step = n > (size_t)0x40000000 ? 0x40000000u : (ULONG)n;

		if (BCryptHashData(h, (PUCHAR)(uintptr_t)p, step, 0) < 0) {
			goto done;
		}
		p += step;
		n -= step;
	}
	ok = BCryptFinishHash(h, out, 32, 0) >= 0;
done:
	if (h) {
		BCryptDestroyHash(h);
	}
	BCryptCloseAlgorithmProvider(alg, 0);
	return ok;
}

/* Everything the editor cares about for the object behind `h`. */
static bool stamp_of_ex(HANDLE h, WfFileStamp *s, DWORD *error, bool no_id_ok)
{
	FILE_BASIC_INFO basic;
	FILE_STANDARD_INFO standard;

	memset(s, 0, sizeof *s);
	/* a WSL distribution has no file identity: it stays all zero there, and
	 * the bytes (hash) are what say whether the file is the one that was read */
	if ((!GetFileInformationByHandleEx(
	             h, FileIdInfo, &s->id, sizeof s->id) &&
	            !no_id_ok) ||
	        !GetFileInformationByHandleEx(
	                h, FileBasicInfo, &basic, sizeof basic) ||
	        !GetFileInformationByHandleEx(
	                h, FileStandardInfo, &standard, sizeof standard)) {
		*error = GetLastError();
		return false;
	}
	s->size = (uint64_t)standard.EndOfFile.QuadPart;
	s->mtime = (uint64_t)basic.LastWriteTime.QuadPart;
	s->attributes = basic.FileAttributes;
	s->links = standard.NumberOfLinks;
	s->valid = true;
	if (standard.Directory) {
		s->attributes |= FILE_ATTRIBUTE_DIRECTORY;
	}
	return true;
}

static bool stamp_of(HANDLE h, WfFileStamp *s, DWORD *error)
{
	return stamp_of_ex(h, s, error, false);
}

static bool same_object(const WfFileStamp *a, const WfFileStamp *b)
{
	return 0 == memcmp(&a->id, &b->id, sizeof a->id);
}

bool wf_efile_probably_changed(const wchar_t *path, const WfFileStamp *was)
{
	WfFileStamp now;
	DWORD error = 0;
	bool ok;
	HANDLE h;

	if (!was || !was->valid) {
		return false; /* nothing to compare with: no claim */
	}
	h = CreateFileW(path, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING,
	        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		DWORD e = GetLastError();

		return ERROR_FILE_NOT_FOUND == e || ERROR_PATH_NOT_FOUND == e;
	}
	ok = stamp_of(h, &now, &error);
	CloseHandle(h);
	if (!ok) {
		return false; /* cannot tell: a hint says nothing it does not know */
	}
	return !same_object(&now, was) || now.size != was->size ||
	        now.mtime != was->mtime;
}

/* Stopping is asked of the job's thread by its caller (wf_blocking.h); the
 * places that may be stopped are the only ones that look. */
static bool cancel_asked(void)
{
	return wf_cancel_requested();
}

static bool read_exact(HANDLE h, void *buf, size_t n, DWORD *error)
{
	uint8_t *p = buf;

	while (n) {
		DWORD got = 0, want = n > CHUNK ? CHUNK : (DWORD)n;

		if (!ReadFile(h, p, want, &got, NULL)) {
			*error = GetLastError();
			return false;
		}
		if (0 == got) {
			*error = ERROR_HANDLE_EOF;
			return false;
		}
		p += got;
		n -= got;
	}
	return true;
}

static bool write_all_c(
        HANDLE h, const void *buf, size_t n, DWORD *error, bool stoppable)
{
	const uint8_t *p = buf;

	while (n) {
		DWORD put = 0, want = n > CHUNK ? CHUNK : (DWORD)n;

		if (stoppable && cancel_asked()) {
			*error = ERROR_OPERATION_ABORTED;
			return false;
		}
		if (!WriteFile(h, p, want, &put, NULL) || 0 == put) {
			*error = GetLastError();
			if (0 == *error) {
				*error = ERROR_WRITE_FAULT;
			}
			return false;
		}
		p += put;
		n -= put;
	}
	return true;
}

static bool write_all(HANDLE h, const void *buf, size_t n, DWORD *error)
{
	return write_all_c(h, buf, n, error, false);
}

void wf_efile_loaded_free(WfLoaded *l)
{
	if (!l) {
		return;
	}
	free(l->utf8);
	memset(l, 0, sizeof *l);
}

static WfStatus verdict(
        WfLoaded *out, WfLoadVerdict v, DWORD error, const char *reason)
{
	out->verdict = v;
	out->error = error;
	out->reason = reason;
	return WF_OK;
}

/* Detects the encoding and decodes `bytes` into `out` (stamp already set). */
static WfStatus decode_loaded(WfLoaded *out,
        const uint8_t *bytes,
        size_t n,
        unsigned fallback_codepage,
        const WfEncoding *force)
{
	WfDetect d;
	WfStatus st;

	if (force) {
		out->enc = *force;
	} else {
		d = wf_enc_detect(bytes, n, fallback_codepage);
		out->enc = d.enc;
		if (WF_DETECT_BINARY == d.kind) {
			return verdict(out, WF_FL_BINARY, 0, d.reason);
		}
		if (WF_DETECT_AMBIGUOUS == d.kind) {
			out->candidate_ok = d.candidate_ok;
			return verdict(out, WF_FL_AMBIGUOUS, 0, d.reason);
		}
	}
	st = wf_enc_decode(
	        bytes, n, out->enc, &out->utf8, &out->utf8_n, &out->bad_offset);
	if (WF_E_NOMEM != st) {
		st = WF_OK != st
		        ? verdict(out, WF_FL_BAD_ENCODING, 0,
		                  "the text is not valid in that encoding")
		        : verdict(out, WF_FL_OK, 0, NULL);
	}
	return st;
}

/* A file on an SFTP place: read through the session, never written back. The
 * stamp says read-only, so the editor opens it read-only. */
static WfStatus load_remote(const wchar_t *path,
        size_t max_bytes,
        unsigned fallback_codepage,
        const WfEncoding *force,
        WfLoaded *out)
{
	uint8_t *bytes = NULL;
	size_t n = 0;
	uint32_t mtime = 0;
	WfFileStamp st_;
	WfStatus st = wf_sftp_place_read(path, max_bytes, &bytes, &n, &mtime);

	if (WF_E_NOENT == st) {
		return verdict(
		        out, WF_FL_MISSING, 0, "the file does not exist");
	}
	if (WF_E_RANGE == st) {
		return verdict(
		        out, WF_FL_TOO_BIG, 0, "larger than the editing limit");
	}
	if (WF_E_INVAL == st) {
		return verdict(out, WF_FL_NOT_FILE, 0, "not a plain file");
	}
	if (WF_OK != st) {
		return WF_E_NOMEM == st
		        ? st
		        : verdict(out, WF_FL_IO, 0,
		                  "the file cannot be read over SFTP");
	}
	memset(&st_, 0, sizeof st_);
	st_.valid = true;
	st_.size = n;
	st_.mtime = ((uint64_t)mtime + 11644473600ull) * 10000000ull;
	st_.attributes = FILE_ATTRIBUTE_READONLY;
	st_.links = 1;
	if (!wf_efile_sha256(bytes, n, st_.sha256)) {
		free(bytes);
		return verdict(out, WF_FL_IO, 0, "hashing failed");
	}
	out->stamp = st_;
	st = decode_loaded(out, bytes, n, fallback_codepage, force);
	free(bytes);
	return st;
}

/*
 * Reads a whole file for the editor. A file that cannot be edited is not a
 * failure of the call: it is a verdict in `out` and the status is WF_OK.
 * Only a refused argument or an allocation that failed is returned as such.
 *
 * Owned here, and given back at `cleanup` on every path: the file handle `h`
 * (closed early, as soon as the bytes are read, so the file is not held while
 * it is hashed and decoded) and the bytes read. Nothing else is handed on but
 * `out`.
 */
WfStatus wf_efile_load(const wchar_t *path,
        size_t max_bytes,
        unsigned fallback_codepage,
        const WfEncoding *force,
        WfLoaded *out)
{
	wchar_t *ext = NULL;
	HANDLE h = INVALID_HANDLE_VALUE;
	WfFileStamp before, after;
	uint8_t *bytes = NULL;
	DWORD error = 0;
	WfStatus st;

	if (!path || !out) {
		return WF_E_INVAL;
	}
	memset(out, 0, sizeof *out);
	if (wf_sftp_place_is(path)) {
		return load_remote(
		        path, max_bytes, fallback_codepage, force, out);
	}
	st = wf_path_to_extended(path, &ext);
	if (WF_OK != st) {
		return st;
	}
	h = CreateFileW(ext, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
	        NULL, OPEN_EXISTING,
	        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN |
	                FILE_FLAG_BACKUP_SEMANTICS,
	        NULL);
	error = INVALID_HANDLE_VALUE == h ? GetLastError() : 0; /* at once */
	free(ext);
	if (INVALID_HANDLE_VALUE == h) {
		st = ERROR_FILE_NOT_FOUND == error ||
		                ERROR_PATH_NOT_FOUND == error
		        ? verdict(out, WF_FL_MISSING, error,
		                  "the file does not exist")
		        : verdict(out, WF_FL_IO, error,
		                  "the file cannot be opened");
		goto cleanup;
	}
	if (FILE_TYPE_DISK != GetFileType(h)) {
		st = verdict(out, WF_FL_NOT_FILE, 0, "not a disk file");
		goto cleanup;
	}
	if (!stamp_of_ex(h, &before, &error, is_wsl(path))) {
		st = verdict(
		        out, WF_FL_IO, error, "the file cannot be inspected");
		goto cleanup;
	}
	if (before.attributes & FILE_ATTRIBUTE_DIRECTORY) {
		st = verdict(out, WF_FL_NOT_FILE, 0, "a folder");
		goto cleanup;
	}
	if (before.attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
		st = verdict(out, WF_FL_LINK, 0, "a link, not a file");
		goto cleanup;
	}
	if (before.attributes & BAD_ATTRIBUTES) {
		st = verdict(out, WF_FL_CLOUD, 0,
		        "stored online only (opening would download it)");
		goto cleanup;
	}
	if (max_bytes && before.size > max_bytes) {
		st = verdict(
		        out, WF_FL_TOO_BIG, 0, "larger than the editing limit");
		goto cleanup;
	}
	if (before.size > (uint64_t)SIZE_MAX / 2) {
		st = verdict(out, WF_FL_TOO_BIG, 0, "too large");
		goto cleanup;
	}
	bytes = malloc(before.size ? (size_t)before.size : 1);
	if (!bytes) {
		st = WF_E_NOMEM;
		goto cleanup;
	}
	if (cancel_asked()) {
		st = verdict(out, WF_FL_IO, ERROR_OPERATION_ABORTED,
		        "stopped before the file was read");
		goto cleanup;
	}
	if (!read_exact(h, bytes, (size_t)before.size, &error)) {
		st = verdict(out, WF_FL_IO, error, "reading failed");
		goto cleanup;
	}
	if (!stamp_of_ex(h, &after, &error, is_wsl(path)) ||
	        !same_object(&before, &after) || after.size != before.size ||
	        after.mtime != before.mtime) {
		st = verdict(out, WF_FL_CHANGED, 0,
		        "the file changed while it was being read");
		goto cleanup;
	}
	CloseHandle(
	        h); /* the file is not held while it is hashed and decoded */
	h = INVALID_HANDLE_VALUE;
	before.size = after.size;
	if (!wf_efile_sha256(bytes, (size_t)before.size, before.sha256)) {
		st = verdict(out, WF_FL_IO, 0, "hashing failed");
		goto cleanup;
	}
	out->stamp = before;
	st = decode_loaded(
	        out, bytes, (size_t)before.size, fallback_codepage, force);

cleanup:
	free(bytes);
	if (INVALID_HANDLE_VALUE != h) {
		CloseHandle(h);
	}
	return st;
}

static WfStatus sv(
        WfSaveResult *out, WfSaveVerdict v, DWORD error, const char *reason)
{
	out->verdict = v;
	out->error = error;
	out->reason = reason;
	return WF_OK;
}

typedef struct
{
	WCHAR streams[STREAM_MAX_COUNT][MAX_PATH + 36];
	uint64_t stream_size[STREAM_MAX_COUNT];
	unsigned n;
} Streams;

/* The named streams of a file; false if they would not all be kept. */
static bool list_streams(
        const wchar_t *path, Streams *s, DWORD *error, const char **why)
{
	WIN32_FIND_STREAM_DATA data;
	HANDLE f = FindFirstStreamW(path, FindStreamInfoStandard, &data, 0);

	s->n = 0;
	if (INVALID_HANDLE_VALUE == f) {
		*error = GetLastError();
		if (ERROR_HANDLE_EOF == *error ||
		        ERROR_INVALID_PARAMETER == *error ||
		        ERROR_NOT_SUPPORTED == *error) {
			*error = 0; /* no streams, or a volume without them */
			return true;
		}
		*why = "alternate streams could not be listed";
		return false;
	}
	do {
		if (!wcscmp(data.cStreamName, L"::$DATA")) {
			continue;
		}
		if (s->n >= STREAM_MAX_COUNT ||
		        (uint64_t)data.StreamSize.QuadPart > STREAM_MAX_BYTES) {
			FindClose(f);
			*why = "has alternate data streams that would not be "
			       "kept";
			return false;
		}
		wcsncpy_s(s->streams[s->n], MAX_PATH + 36, data.cStreamName,
		        _TRUNCATE);
		s->stream_size[s->n] = (uint64_t)data.StreamSize.QuadPart;
		s->n++;
	} while (FindNextStreamW(f, &data));
	*error = GetLastError();
	FindClose(f);
	if (*error != ERROR_HANDLE_EOF) {
		*why = "alternate streams could not be listed";
		return false;
	}
	*error = 0;
	return true;
}

static bool copy_stream(const wchar_t *from,
        const wchar_t *to,
        const wchar_t *stream,
        uint64_t size,
        DWORD *error)
{
	wchar_t a[MAX_PATH + 160], b[MAX_PATH + 160];
	HANDLE in = INVALID_HANDLE_VALUE, outh = INVALID_HANDLE_VALUE;
	uint8_t *buf;
	bool ok = false;

	swprintf(a, MAX_PATH + 160, L"%ls%ls", from, stream);
	swprintf(b, MAX_PATH + 160, L"%ls%ls", to, stream);
	buf = malloc(size ? (size_t)size : 1);
	if (!buf) {
		*error = ERROR_NOT_ENOUGH_MEMORY;
		return false;
	}
	in = CreateFileW(a, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
	        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == in) {
		*error = GetLastError();
		goto out_free_buffer;
	}
	if (!read_exact(in, buf, (size_t)size, error)) {
		goto out_close_input;
	}
	/* DELETE is shared: the file's own handle holds delete access,
	 * which is file-wide */
	outh = CreateFileW(b, GENERIC_WRITE, FILE_SHARE_DELETE, NULL,
	        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == outh) {
		*error = GetLastError();
		goto out_close_input;
	}
	ok = write_all(outh, buf, (size_t)size, error);
	CloseHandle(outh);
out_close_input:
	CloseHandle(in);
out_free_buffer:
	free(buf);
	return ok;
}

/* The folder part of `path` (without the last separator). */
static bool parent_of(
        const wchar_t *path, wchar_t *out, size_t cap, const wchar_t **leaf)
{
	size_t n = wf_path_parent_len(path);

	if (!n || n >= cap) {
		return false;
	}
	memcpy(out, path, n * sizeof *out);
	out[n] = L'\0';
	*leaf = path + n;
	while (**leaf == L'\\' || **leaf == L'/') {
		(*leaf)++;
	}
	return **leaf != L'\0';
}

static void temp_name(
        const wchar_t *dir, const wchar_t *leaf, wchar_t *out, size_t cap)
{
	unsigned char r[4] = {0};
	unsigned long v;

	(void)BCryptGenRandom(
	        NULL, r, sizeof r, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
	v = (unsigned long)r[0] << 24 | (unsigned long)r[1] << 16 |
	        (unsigned long)r[2] << 8 | r[3];
	/* A drive root already ends in a separator. Extended paths do not
	 * normalize doubled separators, so keep exactly one before the leaf. */
	swprintf(out, cap, L"%ls%ls.%ls.%08lx.tmp", dir,
	        dir[wcslen(dir) - 1] == L'\\' ? L"" : L"\\", leaf, v);
}

/* Reads `path` back and checks it is exactly `bytes`. */
/* Reads what the open handle holds from the start and checks it is exactly
 * `bytes`: the object that was verified is the object we hold. */
static bool matches_handle(
        HANDLE h, const uint8_t *bytes, size_t n, DWORD *error)
{
	LARGE_INTEGER zero = {0}, size;
	uint8_t *back;
	bool ok = false;

	if (!SetFilePointerEx(h, zero, NULL, FILE_BEGIN) ||
	        !GetFileSizeEx(h, &size)) {
		*error = GetLastError();
		return false;
	}
	if ((uint64_t)size.QuadPart != n) {
		*error = ERROR_WRITE_FAULT;
		return false;
	}
	back = malloc(65536);
	if (!back) {
		*error = ERROR_NOT_ENOUGH_MEMORY;
		return false;
	}
	ok = true;
	for (size_t at = 0; at < n;) {
		size_t take = n - at < 65536 ? n - at : 65536;
		if (!read_exact(h, back, take, error) ||
		        memcmp(back, bytes + at, take)) {
			if (!*error) {
				*error = ERROR_WRITE_FAULT;
			}
			ok = false;
			break;
		}
		at += take;
	}
	free(back);
	return ok;
}

/*
 * The save temporary is created with the original's permissions from the first
 * moment, not the folder's: a file only its owner could read must not be
 * readable by anyone else while its new text sits in a temporary beside it,
 * and must not stay so if a crash leaves the temporary behind. The DACL is
 * copied as it is and marked protected, so nothing is added from the folder.
 * If the original's permissions cannot be read the save is refused.
 */
bool wf_efile_create_temp_like(
        const wchar_t *model, const wchar_t *tmp, HANDLE *out, DWORD *error)
{
	HANDLE m = CreateFileW(model, READ_CONTROL,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	PSECURITY_DESCRIPTOR rel = NULL;
	PACL dacl = NULL;
	SECURITY_DESCRIPTOR abs;
	SECURITY_ATTRIBUTES sa;
	DWORD e;
	HANDLE h;

	*out = INVALID_HANDLE_VALUE;
	if (INVALID_HANDLE_VALUE == m) {
		*error = GetLastError();
		return false;
	}
	e = GetSecurityInfo(m, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL,
	        NULL, &dacl, NULL, &rel);
	CloseHandle(m);
	if (ERROR_SUCCESS != e) {
		*error = e;
		return false;
	}
	if (!InitializeSecurityDescriptor(&abs, SECURITY_DESCRIPTOR_REVISION) ||
	        !SetSecurityDescriptorDacl(&abs, TRUE, dacl, FALSE) ||
	        !SetSecurityDescriptorControl(
	                &abs, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) {
		*error = GetLastError();
		LocalFree(rel);
		return false;
	}
	memset(&sa, 0, sizeof sa);
	sa.nLength = sizeof sa;
	sa.lpSecurityDescriptor = &abs;
	sa.bInheritHandle = FALSE;
	/* Keep unnamed data immutable to other handles from creation through
	 * verification and commit. Named streams have independent write sharing;
	 * copying them does not require sharing writes to the unnamed stream. */
	h = CreateFileW(tmp, GENERIC_READ | GENERIC_WRITE | DELETE,
	        FILE_SHARE_READ, &sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		*error = GetLastError();
	}
	LocalFree(rel);
	*out = h;
	return INVALID_HANDLE_VALUE != h;
}

bool (*wf_efile_test_after_aside)(const wchar_t *dest, const wchar_t *aside);

static wchar_t journal_path[WF_EFILE_PATH];
static enum
{
	JOURNAL_OFF,
	JOURNAL_ON,
	JOURNAL_MISSING
} journal_mode = JOURNAL_OFF;
static WfEfileJournalGate journal_gate = WF_EFJ_UNKNOWN;
static DWORD journal_gate_error;

void wf_efile_set_journal(const wchar_t *path)
{
	journal_gate = WF_EFJ_UNKNOWN;
	journal_gate_error = 0;
	if (path && wcslen(path) < WF_EFILE_PATH) {
		wcscpy_s(journal_path, WF_EFILE_PATH, path);
		journal_mode = JOURNAL_ON;
	} else {
		journal_mode = JOURNAL_MISSING;
	}
}

void wf_efile_set_journal_unavailable(WfEfileJournalGate why, DWORD error)
{
	journal_mode = JOURNAL_MISSING;
	journal_gate = why;
	journal_gate_error = error;
}

/*
 * The refusal when the swap cannot be recorded. Said by cause: a gate (the
 * record is kept for review) is a refusal and carries no error; a system
 * failure is an I/O failure with the error taken where it happened. Nothing
 * has been moved when this is called.
 */
static WfStatus journal_refusal(WfSaveResult *out,
        WfCopyJournalOpenWhy open_why,
        DWORD open_error,
        bool recording_failed)
{
	if (JOURNAL_MISSING == journal_mode) {
		switch (journal_gate) {
		case WF_EFJ_FORMAT:
			return sv(out, WF_SV_UNSAFE, 0,
			        "an old or unknown operation history is kept "
			        "for "
			        "review (Settings > About): replacing saves "
			        "are "
			        "locked; original unchanged");
		case WF_EFJ_NO_STATE:
			return sv(out, WF_SV_IO, journal_gate_error,
			        "the local state folder is unavailable, so the "
			        "swap cannot be recorded; original unchanged");
		case WF_EFJ_INSPECT:
			return sv(out, WF_SV_IO, journal_gate_error,
			        "the operation history could not be inspected; "
			        "original unchanged");
		case WF_EFJ_UNKNOWN: break;
		}
		return sv(out, WF_SV_IO, 0,
		        "Settings > About: recovery recording unavailable; "
		        "original unchanged");
	}
	if (recording_failed) {
		return sv(out, WF_SV_IO, open_error,
		        "the swap could not be recorded; original unchanged");
	}
	switch (open_why) {
	case WF_CJ_OPEN_FORMAT:
		return sv(out, WF_SV_UNSAFE, 0,
		        "an old or unknown operation history is kept for "
		        "review (Settings > About): replacing saves are "
		        "locked; original unchanged");
	case WF_CJ_OPEN_NOMEM:
		return sv(out, WF_SV_IO, open_error,
		        "out of memory; original unchanged");
	default:
		return sv(out, WF_SV_IO, open_error,
		        "the operation history could not be opened for "
		        "recording; original unchanged");
	}
}

/* Renames the object behind `h` to `target` (an extended path) without ever
 * replacing something: a taken name is an error. */
static bool rename_noclobber(HANDLE h, const wchar_t *target, DWORD *error)
{
	size_t bytes = wcslen(target) * sizeof(wchar_t);
	FILE_RENAME_INFO *info = calloc(1, sizeof *info + bytes);
	bool ok;

	if (!info) {
		*error = ERROR_NOT_ENOUGH_MEMORY;
		return false;
	}
	info->ReplaceIfExists = FALSE;
	info->RootDirectory = NULL;
	info->FileNameLength = (DWORD)bytes;
	memcpy(info->FileName, target, bytes);
	ok = SetFileInformationByHandle(h, FileRenameInfo, info,
	             (DWORD)(sizeof *info + bytes)) != 0;
	if (!ok) {
		*error = GetLastError();
	}
	free(info);
	return ok;
}

/* The original's creation time and file attributes, onto the new file. */
static void carry_basic(HANDLE to, HANDLE from)
{
	FILE_BASIC_INFO b;

	if (!GetFileInformationByHandleEx(from, FileBasicInfo, &b, sizeof b)) {
		return;
	}
	b.LastAccessTime.QuadPart = 0;
	b.LastWriteTime.QuadPart = 0;
	b.ChangeTime.QuadPart = 0;
	b.FileAttributes &= FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
	        FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
	if (!b.FileAttributes) {
		b.FileAttributes = FILE_ATTRIBUTE_NORMAL;
	}
	(void)SetFileInformationByHandle(to, FileBasicInfo, &b, sizeof b);
}

/* Removes a temporary this call created. */
static void drop_temp(
        const wchar_t *path, const FILE_ID_INFO *created, WfSaveResult *out)
{
	WfOwnedReport rep;
	WfOwnedResult r;

	if (0 == created->VolumeSerialNumber) {
		/* no identity to prove ownership with (a WSL distribution): the
		 * file was made by this call with CREATE_NEW and is still held
		 * by no one else; remove exactly that name, or keep it named */
		if (!wf_wslrm_created_file(path)) {
			wcsncpy_s(out->kept_temp, WF_ARRAY_LEN(out->kept_temp),
			        path, _TRUNCATE);
		}
		return;
	}
	r = wf_owned_delete_save_temp(path, created, &rep);

	if (WF_OWNED_DELETED != r && WF_OWNED_ABSENT != r) {
		wcsncpy_s(out->kept_temp, WF_ARRAY_LEN(out->kept_temp), path,
		        _TRUNCATE);
	}
}

static WfStatus encode_for_save(const char *utf8,
        size_t n,
        WfEncoding enc,
        uint8_t **bytes,
        size_t *bytes_n,
        WfSaveResult *out)
{
	size_t bad = 0;
	WfStatus st = wf_enc_encode(utf8, n, enc, bytes, bytes_n, &bad);

	if (WF_OK == st) {
		return WF_OK;
	}
	if (WF_E_INVAL == st) {
		out->bad_offset = bad;
		return sv(out, WF_SV_ENCODE, 0,
		        "the text has a character this encoding cannot hold");
	}
	return st;
}

WfStatus wf_efile_save_new(const wchar_t *path,
        const char *utf8,
        size_t n,
        WfEncoding enc,
        WfSaveResult *out)
{
	uint8_t *bytes = NULL;
	uint8_t digest[32];
	size_t bytes_n = 0;
	wchar_t *ext = NULL;
	HANDLE h;
	DWORD error = 0;
	FILE_ID_INFO created;
	WfFileStamp st;
	WfStatus s;
	bool wsl;

	if (!path || !out || (n && !utf8)) {
		return WF_E_INVAL;
	}
	memset(out, 0, sizeof *out);
	if (wf_sftp_place_is(path)) {
		return sv(out, WF_SV_UNSAFE, 0,
		        "SFTP places are read-only: nothing is written there");
	}
	wsl = is_wsl(path);
	s = encode_for_save(utf8, n, enc, &bytes, &bytes_n, out);
	if (WF_OK != s || WF_SV_ENCODE == out->verdict) {
		return s;
	}
	if (!wf_efile_sha256(bytes, bytes_n, digest)) {
		free(bytes);
		return sv(out, WF_SV_IO, ERROR_GEN_FAILURE, "hashing failed");
	}
	s = wf_path_to_extended(path, &ext);
	if (WF_OK != s) {
		free(bytes);
		return s;
	}
	h = CreateFileW(ext, GENERIC_WRITE | GENERIC_READ, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		error = GetLastError();
		free(ext);
		free(bytes);
		if (ERROR_FILE_EXISTS == error ||
		        ERROR_ALREADY_EXISTS == error) {
			return sv(
			        out, WF_SV_EXISTS, error, "that name is taken");
		}
		return sv(out, WF_SV_IO, error, "the file cannot be created");
	}
	if (!stamp_of_ex(h, &st, &error, wsl)) {
		CloseHandle(h);
		free(ext);
		free(bytes);
		return sv(out, WF_SV_IO, error,
		        "the new file cannot be inspected");
	}
	created = st.id;
	if (!write_all_c(h, bytes, bytes_n, &error, true) ||
	        !FlushFileBuffers(h)) {
		if (!error) {
			error = GetLastError();
		}
		CloseHandle(h);
		drop_temp(ext, &created, out);
		free(ext);
		free(bytes);
		return sv(out, WF_SV_IO, error,
		        ERROR_OPERATION_ABORTED == error
		                ? "stopped before the file was replaced"
		                : "writing failed");
	}
	if (!matches_handle(h, bytes, bytes_n, &error)) {
		CloseHandle(h);
		drop_temp(ext, &created, out);
		free(ext);
		free(bytes);
		return sv(out, WF_SV_IO, error,
		        "the saved file did not read back the same");
	}
	bool inspected = stamp_of_ex(h, &out->stamp, &error, wsl);
	CloseHandle(h);
	free(ext);
	if (!inspected) {
		free(bytes);
		return sv(out, WF_SV_COMMITTED_UNVERIFIED, error,
		        "the file was written but its save identity could not "
		        "be verified");
	}
	memcpy(out->stamp.sha256, digest, sizeof digest);
	free(bytes);
	return sv(out, WF_SV_OK, 0, NULL);
}

#define BACKUPS_MAX 9u

/* <path>~, else <path>~2 ... <path>~9: the first name nothing has. A backup is
 * never overwritten or removed, so an old one does not stop the next save;
 * after nine the user is asked to clear some. `exists` tells a name from
 * "cannot be checked". */
static bool pick_backup(
        const wchar_t *path, wchar_t *out, size_t cap, DWORD *error)
{
	for (unsigned n = 1; n <= BACKUPS_MAX; n++) {
		int w = 1 == n ? swprintf(out, cap, L"%ls~", path)
		               : swprintf(out, cap, L"%ls~%u", path, n);

		if (w < 0) {
			*error = ERROR_FILENAME_EXCED_RANGE;
			return false;
		}
		if (INVALID_FILE_ATTRIBUTES == GetFileAttributesW(out)) {
			DWORD e = GetLastError();

			if (ERROR_FILE_NOT_FOUND == e) {
				return true;
			}
			*error = e;
			return false;
		}
	}
	*error = ERROR_ALREADY_EXISTS;
	return false;
}

static bool hash_handle(
        HANDLE file, uint64_t size, uint8_t digest[32], DWORD *error)
{
	BCRYPT_ALG_HANDLE alg = NULL;
	BCRYPT_HASH_HANDLE hash = NULL;
	uint8_t *buffer = malloc(65536);
	LARGE_INTEGER zero = {0};
	bool ok = false;
	if (!buffer) {
		*error = ERROR_NOT_ENOUGH_MEMORY;
		return false;
	}
	if (!SetFilePointerEx(file, zero, NULL, FILE_BEGIN)) {
		*error = GetLastError();
		goto done;
	}
	if (BCryptOpenAlgorithmProvider(
	            &alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0 ||
	        BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) < 0) {
		goto done;
	}
	while (size) {
		DWORD n = size < 65536 ? (DWORD)size : 65536;
		if (!read_exact(file, buffer, n, error) ||
		        BCryptHashData(hash, buffer, n, 0) < 0) {
			goto done;
		}
		size -= n;
	}
	ok = BCryptFinishHash(hash, digest, 32, 0) >= 0;
done:
	if (hash && BCryptDestroyHash(hash) < 0) {
		ok = false;
	}
	if (alg && BCryptCloseAlgorithmProvider(alg, 0) < 0) {
		ok = false;
	}
	SecureZeroMemory(buffer, 65536);
	free(buffer);
	if (!ok && !*error) {
		*error = ERROR_GEN_FAILURE;
	}
	return ok;
}

/*
 * Saving over a file in a WSL distribution. There is no file identity there,
 * and replacing by rename would give the file a new owner and mode (an
 * executable script would stop being one), so the bytes are overwritten in
 * place. That is not atomic: the original is first copied to <path>~, the
 * new bytes are read back and compared, and a failed write puts the original
 * bytes back. The file must still hold exactly the bytes that were read.
 */
static void wsl_restore(HANDLE h, const uint8_t *old, size_t old_n, bool *ok)
{
	LARGE_INTEGER zero = {0};
	DWORD error = 0;

	*ok = SetFilePointerEx(h, zero, NULL, FILE_BEGIN) &&
	        write_all(h, old, old_n, &error) && SetEndOfFile(h) &&
	        FlushFileBuffers(h) && matches_handle(h, old, old_n, &error);
}

static WfStatus wsl_save_over(const wchar_t *path,
        const WfFileStamp *was,
        const uint8_t *bytes,
        size_t bytes_n,
        const uint8_t digest[32],
        WfSaveResult *out)
{
	wchar_t *ext = NULL, *eback = NULL, backup[WF_EFILE_PATH];
	HANDLE h = INVALID_HANDLE_VALUE, hb = INVALID_HANDLE_VALUE;
	FILE_BASIC_INFO basic;
	FILE_STANDARD_INFO standard;
	LARGE_INTEGER zero = {0};
	uint8_t *old = NULL, seen[32];
	size_t old_n;
	DWORD error = 0;
	WfStatus s = WF_OK;
	bool wrote = false, restored;

	if (wcslen(path) + 4 >= WF_EFILE_PATH) {
		return sv(out, WF_SV_UNSAFE, 0, "the path is too long");
	}
	if (WF_OK != wf_path_to_extended(path, &ext)) {
		return WF_E_NOMEM;
	}
	h = CreateFileW(ext, GENERIC_READ | GENERIC_WRITE, 0, NULL,
	        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		error = GetLastError();
		if (ERROR_ACCESS_DENIED == error) {
			DWORD attrs = GetFileAttributesW(ext);

			if (INVALID_FILE_ATTRIBUTES != attrs &&
			        (attrs & FILE_ATTRIBUTE_READONLY)) {
				s = sv(out, WF_SV_UNSAFE, error,
				        "the file is read-only");
				goto done;
			}
		}
		s = ERROR_FILE_NOT_FOUND == error ||
		                ERROR_PATH_NOT_FOUND == error
		        ? sv(out, WF_SV_GONE, error, "the file is gone")
		        : sv(out, WF_SV_IO, error, "the file cannot be opened");
		goto done;
	}
	if (!GetFileInformationByHandleEx(
	            h, FileBasicInfo, &basic, sizeof basic) ||
	        !GetFileInformationByHandleEx(
	                h, FileStandardInfo, &standard, sizeof standard)) {
		s = sv(out, WF_SV_IO, GetLastError(),
		        "the file cannot be inspected");
		goto done;
	}
	if ((basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
	        standard.Directory) {
		s = sv(out, WF_SV_GONE, 0, "a folder has taken the name");
		goto done;
	}
	if (basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
		s = sv(out, WF_SV_UNSAFE, 0,
		        "the name is now a link; a link is never written "
		        "through");
		goto done;
	}
	if (basic.FileAttributes & FILE_ATTRIBUTE_READONLY) {
		s = sv(out, WF_SV_UNSAFE, 0, "the file is read-only");
		goto done;
	}
	if ((uint64_t)standard.EndOfFile.QuadPart != was->size ||
	        was->size > (uint64_t)SIZE_MAX / 2) {
		s = sv(out, WF_SV_CONFLICT, 0,
		        "the file changed on disk since it was read");
		goto done;
	}
	old_n = (size_t)was->size;
	old = malloc(old_n ? old_n : 1);
	if (!old) {
		s = WF_E_NOMEM;
		goto done;
	}
	if (!read_exact(h, old, old_n, &error)) {
		s = sv(out, WF_SV_IO, error, "reading the file failed");
		goto done;
	}
	if (!wf_efile_sha256(old, old_n, seen) ||
	        memcmp(seen, was->sha256, sizeof seen)) {
		s = sv(out, WF_SV_CONFLICT, 0,
		        "the file changed on disk since it was read");
		goto done;
	}

	/* the original goes to <path>~ (or ~2 ... ~9) first, and is verified
	 * there; CREATE_NEW makes the choice of a free name race-free */
	for (unsigned n = 1; n <= BACKUPS_MAX; n++) {
		free(eback);
		eback = NULL;
		if (1 == n) {
			swprintf(backup, WF_EFILE_PATH, L"%ls~", path);
		} else {
			swprintf(backup, WF_EFILE_PATH, L"%ls~%u", path, n);
		}
		if (WF_OK != wf_path_to_extended(backup, &eback)) {
			s = WF_E_NOMEM;
			goto done;
		}
		hb = CreateFileW(eback, GENERIC_READ | GENERIC_WRITE, 0, NULL,
		        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
		error = INVALID_HANDLE_VALUE == hb ? GetLastError() : 0;
		if (INVALID_HANDLE_VALUE != hb ||
		        (ERROR_FILE_EXISTS != error &&
		                ERROR_ALREADY_EXISTS != error)) {
			break;
		}
	}
	if (INVALID_HANDLE_VALUE == hb) {
		s = sv(out, WF_SV_UNSAFE, error,
		        ERROR_FILE_EXISTS == error ||
		                        ERROR_ALREADY_EXISTS == error
		                ? "nine retained originals (<file>~ to "
		                  "<file>~9) "
		                  "exist: move or delete some before saving "
		                  "again"
		                : "the backup cannot be created");
		goto done;
	}
	if (!write_all(hb, old, old_n, &error) || !FlushFileBuffers(hb) ||
	        !matches_handle(hb, old, old_n, &error)) {
		CloseHandle(hb);
		hb = INVALID_HANDLE_VALUE;
		(void)wf_wslrm_created_file(eback); /* made by this call */
		s = sv(out, WF_SV_IO, error ? error : GetLastError(),
		        "the backup could not be written; the file is "
		        "unchanged");
		goto done;
	}
	CloseHandle(hb);
	hb = INVALID_HANDLE_VALUE;
	out->backup_made = true;

	wrote = true;
	if (!SetFilePointerEx(h, zero, NULL, FILE_BEGIN) ||
	        !write_all(h, bytes, bytes_n, &error) || !SetEndOfFile(h) ||
	        !FlushFileBuffers(h) ||
	        !matches_handle(h, bytes, bytes_n, &error)) {
		if (!error) {
			error = GetLastError();
		}
		wsl_restore(h, old, old_n, &restored);
		if (!restored) {
			wcsncpy_s(out->kept_original, WF_EFILE_PATH, backup,
			        _TRUNCATE);
		}
		s = sv(out, WF_SV_IO, error,
		        restored
		                ? "writing failed; the original bytes were put "
		                  "back"
		                : "writing failed; the original is kept in "
		                  "<file>~");
		goto done;
	}
	if (!GetFileInformationByHandleEx(
	            h, FileBasicInfo, &basic, sizeof basic)) {
		s = sv(out, WF_SV_COMMITTED_UNVERIFIED, GetLastError(),
		        "the file was written but its save state could not be "
		        "read");
		goto done;
	}
	memset(&out->stamp, 0, sizeof out->stamp);
	out->stamp.valid = true;
	out->stamp.size = bytes_n;
	out->stamp.mtime = (uint64_t)basic.LastWriteTime.QuadPart;
	out->stamp.attributes = basic.FileAttributes;
	out->stamp.links = 1;
	memcpy(out->stamp.sha256, digest, 32);
	s = sv(out, WF_SV_OK, 0, NULL);
done:
	(void)wrote;
	if (INVALID_HANDLE_VALUE != h) {
		CloseHandle(h);
	}
	if (INVALID_HANDLE_VALUE != hb) {
		CloseHandle(hb);
	}
	free(old);
	free(ext);
	free(eback);
	return s;
}

/* The checks before a replacement, on the file as it is now. Fills `now`. */
static WfStatus check_target(const wchar_t *ext,
        const WfFileStamp *was,
        WfFileStamp *now,
        Streams *streams,
        WfSaveResult *out,
        HANDLE *keep)
{
	HANDLE h = CreateFileW(ext,
	        FILE_READ_ATTRIBUTES | READ_CONTROL | GENERIC_READ |
	                (keep ? DELETE : 0),
	        FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
	        FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	DWORD error = 0;
	const char *why = NULL;

	if (INVALID_HANDLE_VALUE == h) {
		error = GetLastError();
		if (ERROR_FILE_NOT_FOUND == error ||
		        ERROR_PATH_NOT_FOUND == error) {
			return sv(out, WF_SV_GONE, error,
			        "the file was deleted or renamed");
		}
		return sv(out, WF_SV_IO, error, "the file cannot be opened");
	}
	if (!stamp_of(h, now, &error)) {
		CloseHandle(h);
		return sv(out, WF_SV_IO, error, "the file cannot be inspected");
	}
	if (!same_object(now, was)) {
		CloseHandle(h);
		return sv(out, WF_SV_GONE, 0,
		        "the name now belongs to a different file");
	}
	if (now->attributes &
	        (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
		CloseHandle(h);
		return sv(out, WF_SV_UNSAFE, 0,
		        "the name is now a folder or a link");
	}
	if (now->attributes & FILE_ATTRIBUTE_READONLY) {
		CloseHandle(h);
		return sv(out, WF_SV_UNSAFE, 0, "the file is read-only");
	}
	if (now->attributes & (FILE_ATTRIBUTE_ENCRYPTED | BAD_ATTRIBUTES)) {
		CloseHandle(h);
		return sv(out, WF_SV_UNSAFE, 0,
		        "the file is encrypted or stored online only");
	}
	if (now->links > 1) {
		CloseHandle(h);
		return sv(out, WF_SV_UNSAFE, 0,
		        "the file has several hard links; a replacement would "
		        "detach them");
	}
	{
		/* only the content decides, always: a file rewritten with the same
		 * length and its old time put back is a different file, and a
		 * touched one is still the same */
		uint8_t digest[32];
		bool same = now->size == was->size &&
		        hash_handle(h, now->size, digest, &error) &&
		        !memcmp(digest, was->sha256, sizeof digest);
		SecureZeroMemory(digest, sizeof digest);
		if (error) {
			CloseHandle(h);
			return sv(out, WF_SV_IO, error,
			        "the file cannot be inspected");
		}
		if (!same) {
			CloseHandle(h);
			return sv(out, WF_SV_CONFLICT, 0,
			        "the file changed on disk since it was opened");
		}
	}
	if (!list_streams(ext, streams, &error, &why)) {
		CloseHandle(h);
		return sv(out, WF_SV_UNSAFE, error, why);
	}
	if (keep) {
		*keep = h; /* the object that was just checked, still held */
	} else {
		CloseHandle(h);
	}
	return WF_OK;
}

WfStatus wf_efile_save_over(const wchar_t *path,
        const WfFileStamp *was,
        const char *utf8,
        size_t n,
        WfEncoding enc,
        const WfSaveOptions *opt,
        WfSaveResult *out)
{
	uint8_t *bytes = NULL;
	uint8_t digest[32];
	size_t bytes_n = 0;
	wchar_t *ext = NULL;
	wchar_t dir[WF_EFILE_PATH], tmp[WF_EFILE_PATH], backup[WF_EFILE_PATH],
	        aside[WF_EFILE_PATH];
	HANDLE oh = INVALID_HANDLE_VALUE;
	const wchar_t *leaf;
	WfFileStamp now, again;
	Streams streams;
	HANDLE h;
	DWORD error = 0;
	FILE_ID_INFO created;
	WfFileStamp tmp_stamp;
	ULARGE_INTEGER avail;
	WfStatus s;
	unsigned i;
	bool inspected;
	WfCopyJournal *jr = NULL;
	WfCopyJournalOpenWhy open_why = WF_CJ_OPEN_OK;
	DWORD open_error = 0;
	bool recording_failed = false;

	if (!path || !was || !was->valid || !out || (n && !utf8)) {
		return WF_E_INVAL;
	}
	memset(out, 0, sizeof *out);
	if (wf_sftp_place_is(path)) {
		return sv(out, WF_SV_UNSAFE, 0,
		        "SFTP places are read-only: nothing is written there");
	}
	(void)opt; /* Every replacement now retains its original. */
	s = encode_for_save(utf8, n, enc, &bytes, &bytes_n, out);
	if (WF_OK != s || WF_SV_ENCODE == out->verdict) {
		return s;
	}
	if (!wf_efile_sha256(bytes, bytes_n, digest)) {
		free(bytes);
		return sv(out, WF_SV_IO, ERROR_GEN_FAILURE, "hashing failed");
	}
	if (is_wsl(path)) {
		s = wsl_save_over(path, was, bytes, bytes_n, digest, out);
		free(bytes);
		return s;
	}
	s = wf_path_to_extended(path, &ext);
	if (WF_OK != s) {
		free(bytes);
		return s;
	}
	if (!parent_of(ext, dir, WF_ARRAY_LEN(dir), &leaf)) {
		s = sv(out, WF_SV_IO, 0, "the path has no folder");
		goto done;
	}
	s = check_target(ext, was, &now, &streams, out, NULL);
	if (WF_OK != s || WF_SV_OK != out->verdict) {
		goto done;
	}
	/* Sharing guards protect unnamed data, but cannot prevent a new named
	 * stream from appearing. Never retire the original: preserve one bounded,
	 * visible backup under the first free name (~, ~2 ... ~9). */
	if (wcslen(ext) + 4 >= WF_ARRAY_LEN(backup)) {
		s = sv(out, WF_SV_UNSAFE, ERROR_FILENAME_EXCED_RANGE,
		        "the retained original's name is too long");
		goto done;
	}
	if (!pick_backup(ext, backup, WF_ARRAY_LEN(backup), &error)) {
		s = ERROR_ALREADY_EXISTS == error
		        ? sv(out, WF_SV_UNSAFE, error,
		                  "nine retained originals (<file>~ to "
		                  "<file>~9) "
		                  "exist: move or delete some before saving "
		                  "again")
		        : sv(out, WF_SV_IO, error,
		                  "the retained original's name cannot be "
		                  "checked");
		goto done;
	}
	error = 0;
	if (!GetDiskFreeSpaceExW(dir, &avail, NULL, NULL)) {
		/* unknown: let the write decide */
		avail.QuadPart = (ULONGLONG)-1;
	}
	if (avail.QuadPart < (ULONGLONG)bytes_n + FREE_SPACE_MARGIN) {
		s = sv(out, WF_SV_IO, ERROR_DISK_FULL,
		        "not enough free space to save safely");
		goto done;
	}

	temp_name(dir, leaf, tmp, WF_ARRAY_LEN(tmp));
	if (!wf_efile_create_temp_like(ext, tmp, &h, &error)) {
		s = sv(out, WF_SV_IO, error,
		        ERROR_ACCESS_DENIED == error ||
		                        ERROR_PRIVILEGE_NOT_HELD == error
		                ? "the file's permissions cannot be copied to "
		                  "the "
		                  "new file"
		                : "the temporary file cannot be created");
		goto done;
	}
	if (!stamp_of(h, &tmp_stamp, &error)) {
		CloseHandle(h);
		s = sv(out, WF_SV_IO, error,
		        "the temporary file cannot be inspected");
		goto done;
	}
	created = tmp_stamp.id;
	if (!write_all_c(h, bytes, bytes_n, &error, true) ||
	        !FlushFileBuffers(h)) {
		if (!error) {
			error = GetLastError();
		}
		CloseHandle(h);
		drop_temp(tmp, &created, out);
		s = sv(out, WF_SV_IO, error,
		        ERROR_OPERATION_ABORTED == error
		                ? "stopped before the file was replaced"
		                : "writing the temporary file failed");
		goto done;
	}
	for (i = 0; i < streams.n; i++) {
		if (!copy_stream(ext, tmp, streams.streams[i],
		            streams.stream_size[i], &error)) {
			CloseHandle(h);
			drop_temp(tmp, &created, out);
			s = sv(out, WF_SV_IO, error,
			        "an alternate data stream could not be carried "
			        "over");
			goto done;
		}
	}
	/* the object that was written is the object that is read back, and the
	 * handle to it stays open (nobody else may write to it or remove it)
	 * until it has been renamed into place */
	if (!matches_handle(h, bytes, bytes_n, &error)) {
		CloseHandle(h);
		drop_temp(tmp, &created, out);
		s = sv(out, WF_SV_IO, error,
		        "the temporary file did not read back the same");
		goto done;
	}
	/* once more, as late as possible; the object checked is the one held */
	s = check_target(ext, was, &again, &streams, out, &oh);
	if (WF_OK != s || WF_SV_OK != out->verdict) {
		CloseHandle(h);
		drop_temp(tmp, &created, out);
		goto done;
	}
	if (!matches_handle(h, bytes, bytes_n, &error)) {
		CloseHandle(oh);
		CloseHandle(h);
		drop_temp(tmp, &created, out);
		s = sv(out, WF_SV_IO, error,
		        "the temporary file did not read back the same");
		goto done;
	}
	carry_basic(h, oh);
	wcscpy_s(aside, WF_ARRAY_LEN(aside), backup);
	/*
	 * The swap never replaces anything and only touches the two objects that
	 * were checked, through their handles: the original goes to a name of its
	 * own, then the new file takes the original's name; if that fails the
	 * original is put back. Nothing here looks a file up by name and trusts it.
	 */
	/*
	 * Between the two renames the original's name does not exist. The move is
	 * recorded first, as the user's file (never as a temporary), so a process
	 * that dies in between leaves an inventory for manual recovery. Journal
	 * text alone cannot authorize a later rename. Without a record, nothing
	 * is moved.
	 */
	if (JOURNAL_ON == journal_mode) {
		jr = wf_cj_open_why(journal_path, &open_why, &open_error);
		if (jr && !wf_cj_aside(jr, ext, aside, &again.id)) {
			recording_failed = true;
			open_error = GetLastError();
		}
	}
	if (JOURNAL_MISSING == journal_mode ||
	        (JOURNAL_ON == journal_mode && (!jr || recording_failed))) {
		CloseHandle(oh);
		CloseHandle(h);
		drop_temp(tmp, &created, out);
		s = journal_refusal(
		        out, open_why, open_error, recording_failed);
		goto done;
	}
	if (!rename_noclobber(oh, aside, &error)) {
		CloseHandle(oh);
		CloseHandle(h);
		drop_temp(tmp, &created, out);
		s = sv(out, WF_SV_IO, error,
		        "the final swap failed; the original is unchanged");
		goto done;
	}
	if (wf_efile_test_after_aside &&
	        wf_efile_test_after_aside(ext, aside)) {
		/* a test stands in for the process dying here: nothing is
		 * undone and nothing is recorded */
		CloseHandle(oh);
		CloseHandle(h);
		s = sv(out, WF_SV_IO, 0, "interrupted");
		goto done;
	}
	if (!rename_noclobber(h, ext, &error)) {
		DWORD back = 0;

		if (rename_noclobber(oh, ext, &back)) {
			CloseHandle(oh);
			CloseHandle(h);
			drop_temp(tmp, &created, out);
			s = sv(out, WF_SV_IO, error,
			        "the final swap failed; the original is "
			        "unchanged");
		} else {
			/* both stay where they are, and both are named */
			CloseHandle(oh);
			CloseHandle(h);
			wcsncpy_s(out->kept_temp, WF_ARRAY_LEN(out->kept_temp),
			        tmp, _TRUNCATE);
			wcsncpy_s(out->kept_original,
			        WF_ARRAY_LEN(out->kept_original), aside,
			        _TRUNCATE);
			s = sv(out, WF_SV_IO, error,
			        "the final swap failed; the new text is kept "
			        "in "
			        "the temporary file");
		}
		goto done;
	}
	CloseHandle(oh);
	oh = INVALID_HANDLE_VALUE;
	inspected = stamp_of(h, &out->stamp, &error);
	CloseHandle(h);
	if (!inspected) {
		wcsncpy_s(out->kept_original, WF_ARRAY_LEN(out->kept_original),
		        aside, _TRUNCATE);
		out->backup_made = true;
		s = sv(out, WF_SV_COMMITTED_UNVERIFIED, error,
		        "the file was written but its save identity could not "
		        "be verified");
		goto done;
	}
	wcscpy_s(out->kept_original, WF_ARRAY_LEN(out->kept_original), aside);
	out->backup_made = true;
	memcpy(out->stamp.sha256, digest, sizeof digest);
	s = sv(out, WF_SV_OK, 0, NULL);
done:
	wf_cj_close(jr);
	free(ext);
	free(bytes);
	return s;
}
