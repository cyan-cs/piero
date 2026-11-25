/*
 * NTFS facts of one file or folder (DR-DRAFT-026). One handle, FILE_READ_ATTRIBUTES
 * only, full sharing, the reparse point not followed: nothing here reads data,
 * recalls a cloud file, blocks another program or changes anything.
 */
#include <windows.h>
#include <winioctl.h>

#include <stdlib.h>
#include <string.h>

#include "../base/wf_buf.h"
#include "../text/wf_text.h"
#include "wf_ntfsfacts.h"

#define REPARSE_BYTES (16u * 1024u)
#define STREAM_FIRST (16u * 1024u)
#define STREAM_MAX (1024u * 1024u)
#define TAG_SYMLINK 0xA000000Cu
#define TAG_MOUNT_POINT 0xA0000003u

/* UTF-16 (`n` units) to sanitized UTF-8 in out[cap]; always terminated. */
static void to_text(const wchar_t *s, size_t n, char *out, size_t cap)
{
	WfBuf b;

	out[0] = 0;
	wf_buf_init(&b);
	if (WF_OK == wf_sanitize_utf16(s, n, &b) && b.data) {
		size_t k = b.len < cap - 1 ? b.len : cap - 1;

		/* never cut inside a UTF-8 sequence */
		k = wf_utf8_prefix(b.data, b.len, k);
		memcpy(out, b.data, k);
		out[k] = 0;
	}
	wf_buf_free(&b);
}

bool wf_ntfsfacts_remote(const wchar_t *path)
{
	wchar_t drive[4] = {0, L':', L'\\', 0};
	const wchar_t *p = path;

	if (!path) {
		return true;
	}
	if (0 == wcsncmp(p, L"\\\\?\\", 4)) {
		if (0 == _wcsnicmp(p + 4, L"UNC\\", 4)) {
			return true;
		}
		p += 4;
	} else if (L'\\' == p[0] && L'\\' == p[1]) {
		return true; /* a share, a WSL distribution */
	}
	if (!((p[0] >= L'A' && p[0] <= L'Z') ||
	            (p[0] >= L'a' && p[0] <= L'z')) ||
	        L':' != p[1] || (p[2] && L'\\' != p[2])) {
		return true; /* not an absolute path on a drive: fail closed */
	}
	drive[0] = p[0];
	return DRIVE_REMOTE == GetDriveTypeW(drive);
}

static uint16_t u16_at(const BYTE *b, size_t at)
{
	uint16_t v;

	memcpy(&v, b + at, sizeof v);
	return v;
}

/* `units` UTF-16 units that sit at any byte of a reply, as sanitized text in
 * out[WF_NF_TEXT] (copied out first: no unaligned wide read). `nt`: the NT form
 * of a path starts with a prefix that is dropped. */
static bool text_at(const BYTE *at, size_t units, bool nt, char *out)
{
	wchar_t *w = malloc((units + 1) * sizeof *w);
	const wchar_t *s;
	size_t n = units;

	out[0] = 0;
	if (!w) {
		return false;
	}
	memcpy(w, at, units * sizeof *w);
	w[units] = 0;
	s = w;
	if (nt && n >= 4 && 0 == wcsncmp(s, L"\\??\\", 4)) {
		s += 4;
		n -= 4;
	}
	to_text(s, n, out, WF_NF_TEXT);
	free(w);
	return true;
}

/* Symbolic link and junction: the print name, else the substitute name without
 * its \??\ prefix. Offsets and lengths are checked against what came back. */
static void read_target(HANDLE h, WfNtfsFacts *f)
{
	BYTE *buf;
	DWORD got = 0;
	uint32_t tag;
	size_t head, sub_off, sub_len, prn_off, prn_len;
	size_t avail;

	buf = malloc(REPARSE_BYTES);
	if (!buf) {
		return;
	}
	if (!DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, NULL, 0, buf,
	            REPARSE_BYTES, &got, NULL) ||
	        got < 8u + 8u) {
		free(buf);
		return;
	}
	memcpy(&tag, buf, sizeof tag);
	if (TAG_SYMLINK != tag && TAG_MOUNT_POINT != tag) {
		free(buf); /* another kind of reparse point: no target to show */
		return;
	}
	head = TAG_SYMLINK == tag ? 20u : 16u; /* where PathBuffer starts */
	if (got < head) {
		free(buf);
		return;
	}
	sub_off = u16_at(buf, 8);
	sub_len = u16_at(buf, 10);
	prn_off = u16_at(buf, 12);
	prn_len = u16_at(buf, 14);
	avail = got - head;
	if (prn_len && prn_off + prn_len <= avail && !(prn_off & 1u) &&
	        !(prn_len & 1u)) {
		f->target_known = text_at(
		        buf + head + prn_off, prn_len / 2, false, f->target);
	} else if (sub_len && sub_off + sub_len <= avail && !(sub_off & 1u) &&
	        !(sub_len & 1u)) {
		f->target_known = text_at(
		        buf + head + sub_off, sub_len / 2, true, f->target);
	}
	free(buf);
}

typedef struct
{
	DWORD next;
	DWORD name_bytes;
	LARGE_INTEGER size;
	LARGE_INTEGER allocated;
	WCHAR name[1];
} StreamRec;

static void read_streams(HANDLE h, WfNtfsFacts *f)
{
	size_t cap = STREAM_FIRST;
	BYTE *buf = NULL;
	size_t at = 0;

	for (;;) {
		BYTE *grown = realloc(buf, cap);

		if (!grown) {
			free(buf);
			return;
		}
		buf = grown;
		if (GetFileInformationByHandleEx(
		            h, FileStreamInfo, buf, (DWORD)cap)) {
			break;
		}
		switch (GetLastError()) {
		case ERROR_HANDLE_EOF: /* no streams at all (an empty folder) */
			free(buf);
			f->streams_known = true;
			return;
		case ERROR_MORE_DATA:
		case ERROR_INSUFFICIENT_BUFFER:
			if (cap >= STREAM_MAX) {
				free(buf);
				return; /* streams_known stays false */
			}
			cap *= 4;
			continue;
		default: free(buf); return;
		}
	}
	for (;;) {
		StreamRec r;
		wchar_t *name;
		size_t n;

		if (at + offsetof(StreamRec, name) > cap) {
			break;
		}
		memcpy(&r, buf + at, offsetof(StreamRec, name));
		n = r.name_bytes / 2;
		if (at + offsetof(StreamRec, name) + r.name_bytes > cap) {
			free(buf);
			return; /* damaged answer: say nothing about streams */
		}
		name = malloc((n + 1) * sizeof *name);
		if (!name) {
			free(buf);
			return;
		}
		memcpy(name, buf + at + offsetof(StreamRec, name),
		        n * sizeof *name);
		name[n] = 0;
		/* ":name:$DATA"; the main stream is "::$DATA" */
		if (!(7 == n && 0 == wcsncmp(name, L"::$DATA", 7))) {
			const wchar_t *s = name;
			size_t len = n;

			if (len && L':' == s[0]) {
				s++;
				len--;
			}
			if (len >= 6 &&
			        0 == wcsncmp(s + len - 6, L":$DATA", 6)) {
				len -= 6;
			}
			if (f->n_streams < WF_NF_STREAMS) {
				WfNtfsStream *o = &f->stream[f->n_streams];

				to_text(s, len, o->name, sizeof o->name);
				o->size = (uint64_t)r.size.QuadPart;
			}
			f->n_streams++;
		}
		free(name);
		if (!r.next) {
			break;
		}
		at += r.next;
	}
	free(buf);
	f->streams_known = true;
}

void wf_ntfsfacts_read(const wchar_t *path, WfNtfsFacts *f)
{
	HANDLE h;
	BY_HANDLE_FILE_INFORMATION bi;
	wchar_t fs[WF_NF_FS_NAME], root[MAX_PATH + 1];
	DWORD serial = 0, flags = 0;

	memset(f, 0, sizeof *f);
	if (!path || wf_ntfsfacts_remote(path)) {
		f->state = WF_NF_REMOTE;
		return;
	}
	h = CreateFileW(path, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	if (INVALID_HANDLE_VALUE == h) {
		f->state = WF_NF_FAILED;
		f->error = GetLastError();
		return;
	}
	if (!GetFileInformationByHandle(h, &bi)) {
		f->state = WF_NF_FAILED;
		f->error = GetLastError();
		CloseHandle(h);
		return;
	}
	f->state = WF_NF_OK;
	f->attributes = bi.dwFileAttributes;
	f->links = bi.nNumberOfLinks;
	f->links_known = bi.nNumberOfLinks > 0;
	if (GetVolumeInformationByHandleW(
	            h, NULL, 0, &serial, NULL, &flags, fs, WF_NF_FS_NAME)) {
		f->volume_known = true;
		f->serial = serial;
		to_text(fs, wcslen(fs), f->fs_name, sizeof f->fs_name);
		if (GetVolumePathNameW(path, root, MAX_PATH)) {
			to_text(root, wcslen(root), f->root, sizeof f->root);
		}
	}
	if (bi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
		read_target(h, f);
	}
	read_streams(h, f);
	CloseHandle(h);
}
