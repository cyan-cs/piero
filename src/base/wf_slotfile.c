#include "wf_slotfile.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIN_BYTES 16      /* a header alone is longer than this */
#define HEADER_SLACK 4096 /* what a header may add to the body's limit */

void wf_slotfile_free(WfSlotFile *s)
{
	free(s->header);
	free(s->body);
	memset(s, 0, sizeof *s);
}

/* The decimal after `key` in the header, or false: digits only (no sign, no
 * space), ending the line, and a value that fits and is not the largest (which
 * is what strtoull answers to one that does not, and which has no successor).
 * The field is given once: a header that says it twice does not say which
 * number counts (the first is not taken because it is first), so it is refused.
 * The read of a slot and the look a write takes at one both come here. */
static bool header_number(
        const char *header, const char *key, unsigned long long *out)
{
	const char *at = strstr(header, key);
	char *end = NULL;
	unsigned long long v;

	if (!at) {
		return false;
	}
	if (strstr(at + 1, key)) {
		return false;
	}
	at += strlen(key);
	if (*at < '0' || *at > '9') {
		return false;
	}
	errno = 0;
	v = strtoull(at, &end, 10);
	if (ERANGE == errno || ULLONG_MAX == v || (*end && '\n' != *end)) {
		return false;
	}
	*out = v;
	return true;
}

void wf_slotfile_read(const wchar_t *file,
        const char *magic,
        size_t max_bytes,
        WfSlotFile *out)
{
	HANDLE h;
	LARGE_INTEGER size;
	char *buf, *body, line[64];
	DWORD got = 0;
	unsigned long long want = 0;
	size_t magic_n = strlen(magic);

	memset(out, 0, sizeof *out);
	h = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
	        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return;
	}
	if (!GetFileSizeEx(h, &size) || size.QuadPart < MIN_BYTES ||
	        size.QuadPart > (LONGLONG)(max_bytes + HEADER_SLACK)) {
		CloseHandle(h);
		return;
	}
	buf = malloc((size_t)size.QuadPart + 1);
	if (!buf || !ReadFile(h, buf, (DWORD)size.QuadPart, &got, NULL) ||
	        got != size.QuadPart) {
		CloseHandle(h);
		free(buf);
		return;
	}
	CloseHandle(h);
	buf[got] = '\0';
	body = strstr(buf, "\n--\n");
	snprintf(line, sizeof line, "%s\n", magic);
	if (strncmp(buf, line, magic_n + 1) || !body) {
		free(buf);
		return;
	}
	*body = '\0'; /* the fields below are looked for in the header only */
	if (!header_number(buf, "\nseq ", &out->seq) ||
	        !header_number(buf, "\nsize ", &want) || want > max_bytes) {
		free(buf);
		memset(out, 0, sizeof *out);
		return;
	}
	body += 4;
	if ((unsigned long long)(buf + got - body) != want) {
		free(buf); /* cut short: not trusted */
		memset(out, 0, sizeof *out);
		return;
	}
	out->header = _strdup(buf);
	out->body = malloc((size_t)want + 1);
	if (!out->header || !out->body) {
		free(buf);
		wf_slotfile_free(out);
		return;
	}
	memcpy(out->body, body, (size_t)want);
	out->body[want] = '\0';
	out->body_n = (size_t)want;
	out->whole = true;
	free(buf);
}

/* What a write needs to know of a slot: whether it holds a whole record, and its
 * number. Decided as wf_slotfile_read decides it, from the start of the file
 * and its size: a record is whole when its size line says how many bytes follow
 * the header and that many are there. The body is not read, and so not copied
 * (a draft can be megabytes, and a write is made every few seconds). */
typedef struct
{
	bool whole;
	unsigned long long seq;
} SlotProbe;

static SlotProbe slot_probe(
        const wchar_t *file, const char *magic, size_t max_bytes)
{
	SlotProbe pr = {false, 0};
	HANDLE h;
	LARGE_INTEGER size;
	char head[2 * HEADER_SLACK + 1], line[64], *marker;
	DWORD got = 0, ask;
	size_t magic_n = strlen(magic);
	unsigned long long seq = 0, want = 0;

	h = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
	        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return pr;
	}
	if (!GetFileSizeEx(h, &size) || size.QuadPart < MIN_BYTES ||
	        size.QuadPart > (LONGLONG)(max_bytes + HEADER_SLACK)) {
		CloseHandle(h);
		return pr;
	}
	ask = size.QuadPart < (LONGLONG)(sizeof head - 1)
	        ? (DWORD)size.QuadPart
	        : (DWORD)(sizeof head - 1);
	if (!ReadFile(h, head, ask, &got, NULL) || got != ask) {
		CloseHandle(h);
		return pr;
	}
	CloseHandle(h);
	head[got] = '\0';
	marker = strstr(head, "\n--\n");
	snprintf(line, sizeof line, "%s\n", magic);
	if (strncmp(head, line, magic_n + 1) || !marker) {
		return pr;
	}
	*marker = '\0'; /* the fields are looked for in the header only */
	if (!header_number(head, "\nseq ", &seq) ||
	        !header_number(head, "\nsize ", &want) || want > max_bytes) {
		return pr;
	}
	/* the bytes after the header marker are exactly the size it names */
	if ((unsigned long long)size.QuadPart !=
	        (unsigned long long)(marker - head) + 4 + want) {
		return pr;
	}
	pr.whole = true;
	pr.seq = seq;
	return pr;
}

int wf_slotfile_pick(const WfSlotFile *a, const WfSlotFile *b)
{
	if (!a->whole && !b->whole) {
		return -1;
	}
	if (!a->whole) {
		return 1;
	}
	if (!b->whole) {
		return 0;
	}
	return a->seq >= b->seq ? 0 : 1;
}

/* Opens a slot's file for writing as it is (a link is not followed), and
 * shows it to be a plain file with no other name, on the handle that will be
 * changed. A link or a second name leads to some other file, and CREATE_ALWAYS
 * or a truncation would empty that one. The write and the emptying of a slot
 * both go through here, so that what is safe to change is defined once.
 * WF_E_NOENT when `disposition` is OPEN_EXISTING and there is no such file. */
static WfStatus open_plain(const wchar_t *file, DWORD disposition, HANDLE *out)
{
	BY_HANDLE_FILE_INFORMATION info;
	HANDLE h = CreateFileW(file, GENERIC_WRITE | FILE_READ_ATTRIBUTES, 0,
	        NULL, disposition,
	        FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED |
	                FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);

	*out = INVALID_HANDLE_VALUE;
	if (INVALID_HANDLE_VALUE == h) {
		DWORD error = GetLastError();

		return ERROR_FILE_NOT_FOUND == error ||
		                ERROR_PATH_NOT_FOUND == error
		        ? WF_E_NOENT
		        : WF_E_OS;
	}
	if (!GetFileInformationByHandle(h, &info)) {
		CloseHandle(h);
		return WF_E_OS;
	}
	if ((info.dwFileAttributes &
	            (FILE_ATTRIBUTE_REPARSE_POINT |
	                    FILE_ATTRIBUTE_DIRECTORY)) ||
	        1 != info.nNumberOfLinks) {
		CloseHandle(h);
		return WF_E_ACCESS;
	}
	*out = h;
	return WF_OK;
}

/* The file empty, on the handle that was checked. */
static bool cut_short(HANDLE h)
{
	return INVALID_SET_FILE_POINTER !=
	        SetFilePointer(h, 0, NULL, FILE_BEGIN) &&
	        SetEndOfFile(h);
}

WfStatus wf_slotfile_empty(const wchar_t *file)
{
	HANDLE h;
	WfStatus st = open_plain(file, OPEN_EXISTING, &h);
	bool ok;

	if (WF_E_NOENT == st) {
		return WF_OK; /* a file that is not there stays not there */
	}
	if (WF_OK != st) {
		return st;
	}
	ok = cut_short(h) && FlushFileBuffers(h);
	CloseHandle(h);
	return ok ? WF_OK : WF_E_OS;
}

WfStatus wf_slotfile_write(const wchar_t *const file[2],
        const char *magic,
        const char *before_seq,
        const char *after_seq,
        const void *body,
        size_t n,
        size_t max_bytes)
{
	SlotProbe sl[2];
	char *head;
	size_t cap;
	HANDLE h;
	DWORD put;
	int target;
	unsigned long long seq;
	bool ok;
	WfStatus st;

	if (n > max_bytes) {
		return WF_E_RANGE;
	}
	for (int i = 0; i < 2; i++) {
		sl[i] = slot_probe(file[i], magic, max_bytes);
	}
	/* write over the slot that is not whole, else the older one */
	target = !sl[0].whole            ? 0
	        : !sl[1].whole           ? 1
	        : sl[0].seq <= sl[1].seq ? 0
	                                 : 1;
	seq = (sl[0].whole ? sl[0].seq : 0) > (sl[1].whole ? sl[1].seq : 0)
	        ? sl[0].seq
	        : sl[1].seq;
	if (seq >=
	        ULLONG_MAX - 1) { /* no number after it that a reader accepts */
		return WF_E_RANGE;
	}
	seq++;
	cap = strlen(magic) + (before_seq ? strlen(before_seq) : 0) +
	        (after_seq ? strlen(after_seq) : 0) + 96;
	head = malloc(cap);
	if (!head) {
		return WF_E_NOMEM;
	}
	snprintf(head, cap, "%s\n%sseq %llu\n%ssize %zu\n--\n", magic,
	        before_seq ? before_seq : "", seq, after_seq ? after_seq : "",
	        n);
	st = open_plain(file[target], OPEN_ALWAYS, &h);
	if (WF_OK != st) {
		free(head);
		/* a write has no "no such file" answer: the place cannot be used */
		return WF_E_NOENT == st ? WF_E_OS : st;
	}
	ok = cut_short(h) &&
	        WriteFile(h, head, (DWORD)strlen(head), &put, NULL) &&
	        (0 == n || WriteFile(h, body, (DWORD)n, &put, NULL)) &&
	        FlushFileBuffers(h);
	CloseHandle(h);
	free(head);
	return ok ? WF_OK : WF_E_OS;
}
