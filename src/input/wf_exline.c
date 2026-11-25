#include "wf_exline.h"

#include <string.h>

#include "../fs/wf_dir.h"
#include "../i18n/wf_i18n.h"
#include "../base/wf_macro.h"

typedef enum
{
	A_NONE,
	A_REST, /* the rest of the line, required */
	A_SORT,
	A_SET
} ArgKind;

static const struct
{
	const char *name;
	WfExId id;
	ArgKind arg;
	bool fm, editor;
} COMMANDS[] = {{"go", WF_EX_GO, A_REST, true, false},
        {"sort", WF_EX_SORT, A_SORT, true, false},
        {"set", WF_EX_SET_HIDDEN, A_SET, true, false},
        {"filter", WF_EX_FILTER, A_REST, true, false},
        {"clear-filter", WF_EX_CLEAR_FILTER, A_NONE, true, false},
        {"reload", WF_EX_RELOAD, A_NONE, true, false},
        {"settings", WF_EX_SETTINGS, A_NONE, true, false},
        {"help", WF_EX_HELP, A_NONE, true, false},
        {"q", WF_EX_QUIT, A_NONE, true, true},
        {"quit", WF_EX_QUIT, A_NONE, true, true},
        {"w", WF_EX_WRITE, A_NONE, false, true},
        {"write", WF_EX_WRITE, A_NONE, false, true},
        {"wq", WF_EX_WRITE_QUIT, A_NONE, false, true},
        {"q!", WF_EX_QUIT_FORCE, A_NONE, false, true},
        {"replace", WF_EX_REPLACE, A_NONE, false, true},
        {"buffers", WF_EX_BUFFERS, A_NONE, false, true},
        {"ls", WF_EX_BUFFERS, A_NONE, false, true},
        {"bnext", WF_EX_BUFFER_NEXT, A_NONE, false, true},
        {"bn", WF_EX_BUFFER_NEXT, A_NONE, false, true},
        {"bprev", WF_EX_BUFFER_PREV, A_NONE, false, true},
        {"bp", WF_EX_BUFFER_PREV, A_NONE, false, true},
        {"preview", WF_EX_PREVIEW, A_NONE, false, true}};

static bool blank(char c)
{
	return ' ' == c || '\t' == c;
}

static bool fail(WfEx *out, WfExError e)
{
	out->id = WF_EX_NONE;
	out->error = e;
	return false;
}

/* The next word of [*p, end): quotes are removed. False on an unclosed
 * quote. *found says whether there was a word at all: "" is a word with no
 * characters, and the end of the input is no word. */
static bool next_word(
        const char **p, const char *end, char *buf, size_t cap, bool *found)
{
	const char *s = *p;
	size_t n = 0;
	char quote = 0;

	while (s < end && blank(*s)) {
		s++;
	}
	*found = s < end;
	while (s < end && (quote || !blank(*s))) {
		if (quote && *s == quote) {
			quote = 0;
		} else if (!quote && ('"' == *s || '\'' == *s)) {
			quote = *s;
		} else if (n + 1 < cap) {
			buf[n++] = *s;
		}
		s++;
	}
	buf[n] = '\0';
	*p = s;
	return !quote;
}

/* The word that was not understood, for the message. */
static void keep_arg(WfEx *out, const char *word)
{
	size_t n = strlen(word);

	if (n >= sizeof out->arg) {
		n = sizeof out->arg - 1;
	}
	memcpy(out->arg, word, n);
	out->arg[n] = '\0';
}

static bool equals(const char *a, const char *b)
{
	return !strcmp(a, b);
}

static bool rest_arg(const char *p, const char *end, WfEx *out)
{
	size_t n;

	while (p < end && blank(*p)) {
		p++;
	}
	while (end > p && blank(end[-1])) {
		end--;
	}
	if (p == end) {
		return fail(out, WF_EXE_MISSING_ARG);
	}
	n = (size_t)(end - p);
	if ('"' == *p || '\'' == *p) {
		if (n < 2 || end[-1] != *p || memchr(p + 1, *p, n - 2)) {
			return fail(out, WF_EXE_QUOTE);
		}
		p++;
		n -= 2;
	}
	if (0 == n) {
		return fail(out, WF_EXE_MISSING_ARG);
	}
	if (n >= sizeof out->arg) {
		return fail(out, WF_EXE_TOO_LONG);
	}
	memcpy(out->arg, p, n);
	out->arg[n] = '\0';
	return true;
}

static bool sort_args(const char *p, const char *end, WfEx *out)
{
	char key[32], dir[32];
	bool found;

	if (!next_word(&p, end, key, sizeof key, &found)) {
		return fail(out, WF_EXE_QUOTE);
	}
	if (!key[0]) {
		return fail(out, WF_EXE_MISSING_ARG);
	}
	if (equals(key, "name")) {
		out->sort_key = WF_SORT_NAME;
	} else if (equals(key, "size")) {
		out->sort_key = WF_SORT_SIZE;
	} else if (equals(key, "modified") || equals(key, "date") ||
	        equals(key, "mtime")) {
		out->sort_key = WF_SORT_DATE;
	} else if (equals(key, "extension") || equals(key, "ext")) {
		out->sort_key = WF_SORT_EXT;
	} else {
		keep_arg(out, key);
		return fail(out,
		        equals(key, "created") || equals(key, "ctime")
		                ? WF_EXE_UNSUPPORTED
		                : WF_EXE_BAD_ARG);
	}
	if (!next_word(&p, end, dir, sizeof dir, &found)) {
		return fail(out, WF_EXE_QUOTE);
	}
	if (!found) {
		return true;
	}
	/* An omitted or empty direction uses the default ascending order. */
	if (dir[0] && equals(dir, "asc")) {
		out->descending = false;
	} else if (dir[0] && equals(dir, "desc")) {
		out->descending = true;
	} else if (dir[0]) {
		keep_arg(out, dir);
		return fail(out, WF_EXE_BAD_ARG);
	}
	if (!next_word(&p, end, dir, sizeof dir, &found)) {
		return fail(out, WF_EXE_QUOTE);
	}
	return found ? fail(out, WF_EXE_TRAILING) : true;
}

static bool set_args(const char *p, const char *end, WfEx *out)
{
	char option[32], value[32];
	bool found;

	if (!next_word(&p, end, option, sizeof option, &found)) {
		return fail(out, WF_EXE_QUOTE);
	}
	if (!option[0]) {
		return fail(out, WF_EXE_MISSING_ARG);
	}
	if (!equals(option, "hidden")) {
		keep_arg(out, option);
		return fail(out, WF_EXE_BAD_ARG);
	}
	if (!next_word(&p, end, value, sizeof value, &found)) {
		return fail(out, WF_EXE_QUOTE);
	}
	if (!value[0]) {
		return fail(out, WF_EXE_MISSING_ARG);
	}
	if (equals(value, "on") || equals(value, "true")) {
		out->on = true;
	} else if (equals(value, "off") || equals(value, "false")) {
		out->on = false;
	} else {
		keep_arg(out, value);
		return fail(out, WF_EXE_BAD_ARG);
	}
	if (!next_word(&p, end, value, sizeof value, &found)) {
		return fail(out, WF_EXE_QUOTE);
	}
	return found ? fail(out, WF_EXE_TRAILING) : true;
}

bool wf_ex_parse(const char *text, size_t n, WfExScope scope, WfEx *out)
{
	const char *p, *end;
	char word[24];
	size_t i, len = 0;

	memset(out, 0, sizeof *out);
	if (!n) {
		return fail(out, WF_EXE_EMPTY);
	}
	if (!text) {
		return fail(out, WF_EXE_UNKNOWN);
	}
	if (n > 2 * WF_EX_ARG_MAX) {
		return fail(out, WF_EXE_TOO_LONG);
	}
	p = text;
	end = text + n;
	/* Embedded NULs must not hide trailing input from strcmp below. */
	if (n && memchr(text, 0, n)) {
		const char *stop = memchr(text, 0, n);

		for (len = 0; text + len < stop && !blank(text[len]) &&
		        len < sizeof out->name - 1;
		        len++) {
			out->name[len] = text[len];
		}
		return fail(out, WF_EXE_UNKNOWN);
	}
	while (p < end && (blank(*p) || ':' == *p)) {
		p++;
	}
	if (p == end) {
		return fail(out, WF_EXE_EMPTY);
	}
	while (p + len < end && !blank(p[len])) {
		len++;
	}
	if (len >= sizeof word) {
		len = sizeof word - 1;
	}
	memcpy(word, p, len);
	word[len] = '\0';
	memcpy(out->name, word, len + 1);
	while (p < end && !blank(*p)) {
		p++;
	}
	for (i = 0; i < WF_ARRAY_LEN(COMMANDS); i++) {
		if (!equals(word, COMMANDS[i].name)) {
			continue;
		}
		if (!(WF_EXS_FM == scope ? COMMANDS[i].fm
		                         : COMMANDS[i].editor)) {
			return fail(out, WF_EXE_WRONG_SCOPE);
		}
		out->id = COMMANDS[i].id;
		switch (COMMANDS[i].arg) {
		case A_NONE:
			while (p < end && blank(*p)) {
				p++;
			}
			if (p != end) {
				return fail(out, WF_EXE_TRAILING);
			}
			return true;
		case A_REST: return rest_arg(p, end, out);
		case A_SORT: return sort_args(p, end, out);
		case A_SET:  return set_args(p, end, out);
		}
	}
	return fail(out, WF_EXE_UNKNOWN);
}

void wf_ex_error_text(const WfEx *ex, char *out, size_t cap)
{
	WfMsg id;
	WfMsgArg args[2];
	size_t n = 1;

	args[0] = WF_ARG_S(ex->name);
	args[1] = WF_ARG_S(ex->arg);
	switch (ex->error) {
	case WF_EXE_UNKNOWN:     id = MSG_EX_UNKNOWN; break;
	case WF_EXE_MISSING_ARG: id = MSG_EX_MISSING; break;
	case WF_EXE_BAD_ARG:
		id = MSG_EX_BAD_ARG;
		n = 2;
		break;
	case WF_EXE_TRAILING: id = MSG_EX_TRAILING; break;
	case WF_EXE_QUOTE:    id = MSG_EX_QUOTE; break;
	case WF_EXE_TOO_LONG: id = MSG_EX_TOO_LONG; break;
	case WF_EXE_UNSUPPORTED:
		id = MSG_EX_UNSUPPORTED;
		n = 2;
		break;
	case WF_EXE_WRONG_SCOPE: id = MSG_EX_SCOPE; break;
	default:
		if (cap) {
			out[0] = '\0';
		}
		return;
	}
	(void)wf_msgf(out, cap, id, args, n);
}
