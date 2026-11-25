#include "wf_batchlog_win.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../path/wf_path.h"

#define JOURNAL_MAX_BYTES (64u * 1024u * 1024u)

struct WfBatchLog
{
	HANDLE file;
	WfBatchLogStats stats;
};

/* CRC-32, reflected polynomial 0xEDB88320. */

uint32_t wf_batchlog_crc32(const void *data, size_t n)
{
	static uint32_t table[256];
	static bool ready;
	const uint8_t *p = data;
	uint32_t c = 0xffffffffu;

	if (!ready) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t v = i;

			for (int k = 0; k < 8; k++) {
				v = (v & 1) ? 0xedb88320u ^ (v >> 1) : v >> 1;
			}
			table[i] = v;
		}
		ready = true; /* the same value from every thread: benign */
	}
	for (size_t i = 0; i < n; i++) {
		c = table[(c ^ p[i]) & 0xff] ^ (c >> 8);
	}
	return ~c;
}

typedef struct
{
	char *s;
	size_t n, cap;
	bool failed;
} Text;

static void text_put(Text *t, const char *s, size_t n)
{
	if (t->failed) {
		return;
	}
	if (t->n + n + 1 > t->cap) {
		size_t cap = t->cap ? t->cap : 4096;
		char *grown;

		while (cap < t->n + n + 1) {
			cap *= 2;
		}
		grown = realloc(t->s, cap);
		if (!grown) {
			t->failed = true;
			return;
		}
		t->s = grown;
		t->cap = cap;
	}
	memcpy(t->s + t->n, s, n);
	t->n += n;
}

/* Lines are printf-style; the attribute lets clang check them. */
#if defined(__clang__) || defined(__GNUC__)
#	define TEXT_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#	define TEXT_PRINTF(a, b)
#endif
static void text_fmt(Text *t, const char *fmt, ...) TEXT_PRINTF(2, 3);

static void text_fmt(Text *t, const char *fmt, ...)
{
	char buf[128];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= sizeof buf) {
		t->failed = true;
		return;
	}
	text_put(t, buf, (size_t)n);
}

static bool safe_unit(wchar_t c)
{
	return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') ||
	        (c >= L'A' && c <= L'Z') || L'.' == c || L'_' == c ||
	        L'-' == c || L'\\' == c || L':' == c;
}

static void text_path(Text *t, const wchar_t *p)
{
	for (; *p; p++) {
		if (safe_unit(*p)) {
			char c = (char)*p;

			text_put(t, &c, 1);
		} else {
			text_fmt(t, "%%u%04X", (unsigned)(uint16_t)*p);
		}
	}
}

static void text_hex(Text *t, const uint8_t *b, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		text_fmt(t, "%02x", b[i]);
	}
}

/* Closes the line that started at `from` with its CRC. */
static void text_close_line(Text *t, size_t from)
{
	uint32_t crc;

	if (t->failed) {
		return;
	}
	crc = wf_batchlog_crc32(t->s + from, t->n - from);
	text_fmt(t, "*%08x\n", (unsigned)crc);
}

static WfStatus write_all(WfBatchLog *log, const Text *t, bool flush)
{
	size_t done = 0;

	while (done < t->n) {
		DWORD w = 0,
		      want = (DWORD)(t->n - done > 1u << 20 ? 1u << 20
		                                            : t->n - done);

		if (!WriteFile(log->file, t->s + done, want, &w, NULL) || !w) {
			return WF_E_OS;
		}
		log->stats.writes++;
		done += w;
	}
	if (!flush) {
		return WF_OK;
	}
	if (!FlushFileBuffers(log->file)) {
		return WF_E_OS;
	}
	log->stats.flushes++;
	return WF_OK;
}

WfStatus wf_batchlog_create(
        const wchar_t *dir, const uint8_t batch[16], WfBatchLog **out)
{
	wchar_t name[40], *path = NULL;
	WfBatchLog *log;
	WfStatus st;

	*out = NULL;
	for (size_t i = 0; i < 16; i++) {
		swprintf(name + 2 * i, 3, L"%02x", batch[i]);
	}
	wcscpy_s(name + 32, 8, L".pbj");
	if (!CreateDirectoryW(dir, NULL) &&
	        ERROR_ALREADY_EXISTS != GetLastError()) {
		return WF_E_OS;
	}
	st = wf_path_join(dir, name, &path);
	if (WF_OK != st) {
		return st;
	}
	log = calloc(1, sizeof *log);
	if (!log) {
		free(path);
		return WF_E_NOMEM;
	}
	log->file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
	        CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
	        NULL);
	free(path);
	if (INVALID_HANDLE_VALUE == log->file) {
		DWORD e = GetLastError();

		free(log);
		return ERROR_FILE_EXISTS == e || ERROR_ALREADY_EXISTS == e
		        ? WF_E_INVAL
		        : WF_E_OS;
	}
	*out = log;
	return WF_OK;
}

WfStatus wf_batchlog_resume(
        const wchar_t *file, uint64_t valid_bytes, WfBatchLog **out)
{
	WfBatchLog *log = calloc(1, sizeof *log);
	LARGE_INTEGER at;

	*out = NULL;
	if (!log) {
		return WF_E_NOMEM;
	}
	log->file = CreateFileW(file, GENERIC_WRITE, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
	        NULL);
	at.QuadPart = (LONGLONG)valid_bytes;
	if (INVALID_HANDLE_VALUE == log->file) {
		free(log);
		return WF_E_OS;
	}
	if (!SetFilePointerEx(log->file, at, NULL, FILE_BEGIN) ||
	        !SetEndOfFile(log->file)) {
		CloseHandle(log->file);
		free(log);
		return WF_E_OS;
	}
	*out = log;
	return WF_OK;
}

void wf_batchlog_close(WfBatchLog *log)
{
	if (!log) {
		return;
	}
	if (INVALID_HANDLE_VALUE != log->file) {
		CloseHandle(log->file);
	}
	free(log);
}

WfBatchLogStats wf_batchlog_stats(const WfBatchLog *log)
{
	return log->stats;
}

WfStatus wf_batchlog_plan(WfBatchLog *log,
        const WfBjItem *item,
        size_t n_item,
        const WfBjStep *step,
        size_t n_step)
{
	Text t = {0};
	size_t from;
	WfStatus st;

	/* the batch id is in the file's name; the header repeats it */
	from = t.n;
	text_fmt(&t, "PBJ1 %zu %zu", n_item, n_step);
	text_close_line(&t, from);
	for (size_t i = 0; i < n_item; i++) {
		from = t.n;
		text_fmt(&t, "I %zu %016llx ", i,
		        (unsigned long long)item[i].volume);
		text_hex(&t, item[i].id, 16);
		text_fmt(&t, " %c ", item[i].dir ? 'd' : 'f');
		text_path(&t, item[i].path);
		text_close_line(&t, from);
	}
	for (size_t i = 0; i < n_step; i++) {
		from = t.n;
		text_fmt(&t, "S %zu %u ", i, (unsigned)step[i].item);
		text_path(&t, step[i].to);
		text_fmt(&t, " %d", step[i].temp ? 1 : 0);
		text_close_line(&t, from);
	}
	from = t.n;
	text_put(&t, "G", 1);
	text_close_line(&t, from);
	if (t.failed) {
		free(t.s);
		return WF_E_NOMEM;
	}
	st = write_all(log, &t, true);
	free(t.s);
	return st;
}

static WfStatus mark_line(WfBatchLog *log, char what, size_t step, bool flush)
{
	Text t = {0};
	WfStatus st;

	if ('B' != what && 'D' != what && 'R' != what) {
		return WF_E_INVAL;
	}
	text_fmt(&t, "%c %zu", what, step);
	text_close_line(&t, 0);
	if (t.failed) {
		free(t.s);
		return WF_E_NOMEM;
	}
	st = write_all(log, &t, flush);
	free(t.s);
	return st;
}

WfStatus wf_batchlog_mark(WfBatchLog *log, char what, size_t step)
{
	return mark_line(log, what, step, true);
}

WfStatus wf_batchlog_mark_lazy(WfBatchLog *log, char what, size_t step)
{
	return mark_line(log, what, step, false);
}

WfStatus wf_batchlog_end(WfBatchLog *log, WfBatchEnd end)
{
	static const char *const word[] = {
	        "SUCCESS", "ROLLED_BACK", "RECOVERY_REQUIRED"};
	Text t = {0};
	WfStatus st;

	if ((unsigned)end > WF_BJ_RECOVERY_REQUIRED) {
		return WF_E_INVAL;
	}
	text_fmt(&t, "E %s", word[end]);
	text_close_line(&t, 0);
	if (t.failed) {
		free(t.s);
		return WF_E_NOMEM;
	}
	st = write_all(log, &t, true);
	free(t.s);
	return st;
}

void wf_batchjournal_free(WfBatchJournal *j)
{
	if (!j) {
		return;
	}
	for (size_t i = 0; i < j->n_items; i++) {
		free(j->item[i].path);
	}
	for (size_t i = 0; i < j->n_steps; i++) {
		free(j->step[i].to);
	}
	free(j->item);
	free(j->step);
	free(j->state);
	memset(j, 0, sizeof *j);
}

static char *slurp(const wchar_t *file, size_t *n)
{
	HANDLE h = CreateFileW(file, GENERIC_READ,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	LARGE_INTEGER size;
	char *buf = NULL;
	DWORD got = 0;

	*n = 0;
	if (INVALID_HANDLE_VALUE == h) {
		return NULL;
	}
	if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 ||
	        (uint64_t)size.QuadPart > JOURNAL_MAX_BYTES) {
		CloseHandle(h);
		return NULL;
	}
	buf = malloc((size_t)size.QuadPart + 1);
	if (buf &&
	        (size.QuadPart == 0 ||
	                (ReadFile(h, buf, (DWORD)size.QuadPart, &got, NULL) &&
	                        got == (DWORD)size.QuadPart))) {
		*n = (size_t)size.QuadPart;
		buf[*n] = '\0';
	} else {
		free(buf);
		buf = NULL;
	}
	CloseHandle(h);
	return buf;
}

/* A line without its "*crc": false when the CRC is wrong or missing. */
static bool line_ok(const char *line, size_t n, size_t *body)
{
	char *end;
	unsigned long want;

	if (n < 10 || '*' != line[n - 9]) {
		return false;
	}
	want = strtoul(line + n - 8, &end, 16);
	if (end != line + n) {
		return false;
	}
	if (wf_batchlog_crc32(line, n - 9) != (uint32_t)want) {
		return false;
	}
	*body = n - 9;
	return true;
}

static bool unhex(const char *s, uint8_t *out, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		char two[3] = {s[2 * i], s[2 * i + 1], 0};
		char *end;
		unsigned v;

		if (!two[0] || !two[1]) {
			return false;
		}
		v = (unsigned)strtoul(two, &end, 16);
		if (*end) {
			return false;
		}
		out[i] = (uint8_t)v;
	}
	return true;
}

/* "%uXXXX" and the safe characters, back to UTF-16 units. */
static wchar_t *path_in(const char *s)
{
	size_t n = strlen(s), k = 0;
	wchar_t *w = malloc((n + 1) * sizeof *w);

	if (!w) {
		return NULL;
	}
	for (size_t i = 0; i < n; i++) {
		if ('%' == s[i]) {
			char hex[5] = {0};
			char *end;

			if (i + 6 > n || 'u' != s[i + 1]) {
				free(w);
				return NULL;
			}
			memcpy(hex, s + i + 2, 4);
			w[k++] = (wchar_t)strtoul(hex, &end, 16);
			if (*end) {
				free(w);
				return NULL;
			}
			i += 5;
		} else {
			w[k++] = (wchar_t)(unsigned char)s[i];
		}
	}
	w[k] = L'\0';
	return w;
}

typedef struct
{
	WfBatchJournal *j;
	bool header, planned;
	size_t items_seen, steps_seen;
} Parse;

/* The next space-separated word of the line, or NULL. */
static char *word(char **cursor)
{
	char *w = *cursor, *sp;

	if (!w || !*w) {
		return NULL;
	}
	sp = strchr(w, ' ');
	if (sp) {
		*sp = '\0';
		*cursor = sp + 1;
	} else {
		*cursor = NULL;
	}
	return w;
}

static bool number_of(const char *s, int base, unsigned long long *v)
{
	char *end;

	if (!s || !*s) {
		return false;
	}
	*v = strtoull(s, &end, base);
	return !*end;
}

static bool take_header(Parse *p, char *rest)
{
	WfBatchJournal *j = p->j;
	unsigned long long ni, ns;

	if (!number_of(word(&rest), 10, &ni) ||
	        !number_of(word(&rest), 10, &ns) || ni > WF_BJ_MAX_ITEMS ||
	        ns > WF_BJ_MAX_STEPS || rest) {
		return false;
	}
	j->item = calloc(ni ? (size_t)ni : 1, sizeof *j->item);
	j->step = calloc(ns ? (size_t)ns : 1, sizeof *j->step);
	j->state = calloc(ns ? (size_t)ns : 1, sizeof *j->state);
	if (!j->item || !j->step || !j->state) {
		return false;
	}
	j->n_items = (size_t)ni;
	j->n_steps = (size_t)ns;
	p->header = true;
	return true;
}

static bool take_item(Parse *p, char *rest)
{
	WfBatchJournal *j = p->j;
	unsigned long long idx, vol;
	char *id, *kind, *path;
	WfBjItem *it;

	if (!number_of(word(&rest), 10, &idx) ||
	        !number_of(word(&rest), 16, &vol) || !(id = word(&rest)) ||
	        !(kind = word(&rest)) || !(path = word(&rest)) || rest ||
	        idx != p->items_seen || idx >= j->n_items ||
	        (strcmp(kind, "d") && strcmp(kind, "f"))) {
		return false;
	}
	it = &j->item[idx];
	it->volume = vol;
	if (strlen(id) != 32 || !unhex(id, it->id, 16)) {
		return false;
	}
	it->dir = 'd' == kind[0];
	it->path = path_in(path);
	p->items_seen++;
	return NULL != it->path;
}

static bool take_step(Parse *p, char *rest)
{
	WfBatchJournal *j = p->j;
	unsigned long long idx, item, temp;
	char *path;

	if (!number_of(word(&rest), 10, &idx) ||
	        !number_of(word(&rest), 10, &item) || !(path = word(&rest)) ||
	        !number_of(word(&rest), 10, &temp) || rest ||
	        idx != p->steps_seen || idx >= j->n_steps ||
	        item >= j->n_items || temp > 1) {
		return false;
	}
	j->step[idx].item = (uint32_t)item;
	j->step[idx].temp = 1 == temp;
	j->step[idx].to = path_in(path);
	p->steps_seen++;
	return NULL != j->step[idx].to;
}

/* B, D, R: the life of a step goes planned, begun, done, returned. */
static bool take_mark(Parse *p, char what, char *rest)
{
	WfBatchJournal *j = p->j;
	unsigned long long idx;

	if (!number_of(word(&rest), 10, &idx) || rest || idx >= j->n_steps ||
	        j->ended) {
		return false;
	}
	if ('B' == what && WF_BJ_STEP_PLANNED == j->state[idx]) {
		j->state[idx] = WF_BJ_STEP_BEGUN;
	} else if ('D' == what && WF_BJ_STEP_BEGUN == j->state[idx]) {
		j->state[idx] = WF_BJ_STEP_DONE;
	} else if ('R' == what &&
	        (WF_BJ_STEP_DONE == j->state[idx] ||
	                WF_BJ_STEP_BEGUN == j->state[idx])) {
		j->state[idx] = WF_BJ_STEP_RETURNED;
	} else {
		return false;
	}
	return true;
}

static bool take_end(Parse *p, const char *state)
{
	WfBatchJournal *j = p->j;

	if (j->ended) {
		return false;
	}
	if (!strcmp(state, "SUCCESS")) {
		j->end = WF_BJ_SUCCESS;
	} else if (!strcmp(state, "ROLLED_BACK")) {
		j->end = WF_BJ_ROLLED_BACK;
	} else if (!strcmp(state, "RECOVERY_REQUIRED")) {
		j->end = WF_BJ_RECOVERY_REQUIRED;
	} else {
		return false;
	}
	j->ended = true;
	return true;
}

/* Takes one valid line apart; false when it is not a line of this format. */
static bool take(Parse *p, char *line, size_t n)
{
	char kind = line[0];
	char *rest = n > 2 ? line + 2 : NULL;

	line[n] = '\0';
	if (!p->header) {
		return !strncmp(line, "PBJ1 ", 5) && take_header(p, line + 5);
	}
	if (!p->planned) {
		if ('I' == kind && ' ' == line[1]) {
			return take_item(p, rest);
		}
		if ('S' == kind && ' ' == line[1]) {
			return take_step(p, rest);
		}
		if ('G' == kind && 1 == n) {
			p->planned = p->items_seen == p->j->n_items &&
			        p->steps_seen == p->j->n_steps;
			return p->planned;
		}
		return false;
	}
	if (('B' == kind || 'D' == kind || 'R' == kind) && ' ' == line[1]) {
		return take_mark(p, kind, rest);
	}
	if ('E' == kind && ' ' == line[1] && rest) {
		return take_end(p, rest);
	}
	return false;
}

WfBjReadResult wf_batchlog_read(const wchar_t *file, WfBatchJournal *out)
{
	size_t n, at = 0, body;
	char *buf = slurp(file, &n);
	Parse p = {0};
	WfBjReadResult result = WF_BJ_READ_OK;

	memset(out, 0, sizeof *out);
	if (!buf) {
		return WF_BJ_READ_DAMAGED;
	}
	if (n >= 5 && !memcmp(buf, "PBJ", 3) && memcmp(buf, "PBJ1 ", 5)) {
		free(buf);
		return WF_BJ_READ_UNSUPPORTED;
	}
	p.j = out;
	while (at < n) {
		char *nl = memchr(buf + at, '\n', n - at);
		size_t len;

		if (!nl) {
			out->torn = true; /* cut before its end of line */
			break;
		}
		len = (size_t)(nl - (buf + at));
		if (!line_ok(buf + at, len, &body)) {
			if (nl + 1 >= buf + n) {
				out->torn =
				        true; /* the last line, its CRC wrong: a torn write */
				break;
			}
			result = WF_BJ_READ_DAMAGED;
			break;
		}
		if (!take(&p, buf + at, body)) {
			result =
			        WF_BJ_READ_DAMAGED; /* a good CRC, a line that cannot be here */
			break;
		}
		at = (size_t)(nl - buf) + 1;
		out->valid_bytes = at;
	}
	free(buf);
	if (WF_BJ_READ_OK == result && !p.planned) {
		result = WF_BJ_READ_NO_PLAN;
	}
	if (WF_BJ_READ_OK != result) {
		wf_batchjournal_free(out);
	}
	return result;
}
