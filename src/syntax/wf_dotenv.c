#include "wf_dotenv.h"

#include <stdlib.h>
#include <string.h>

#include "wf_synruns.h"

static void paint(uint8_t *cls, size_t a, size_t b, uint8_t k)
{
	if (a < b) {
		memset(cls + a, k, b - a);
	}
}

static int name_char(unsigned char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
	        (c >= '0' && c <= '9') || '_' == c || '.' == c || '-' == c;
}

/* $NAME and ${NAME} inside [a, b) become variables. */
static void variables(const char *t, uint8_t *cls, size_t a, size_t b)
{
	for (size_t i = a; i < b; i++) {
		size_t j;

		if ('\\' == t[i]) {
			i++; /* an escaped dollar is not a variable */
			continue;
		}
		if ('$' != t[i]) {
			continue;
		}
		j = i + 1;
		if (j < b && '{' == t[j]) {
			while (j < b && '}' != t[j]) {
				j++;
			}
			if (j < b) {
				j++;
			}
		} else {
			while (j < b && name_char((unsigned char)t[j]) &&
			        '.' != t[j] && '-' != t[j]) {
				j++;
			}
		}
		if (j > i + 1) {
			paint(cls, i, j, WF_SC_VARIABLE);
			i = j - 1;
		}
	}
}

static void one_line(const char *t, uint8_t *cls, size_t a, size_t b)
{
	size_t i = a, name_start, name_end, v;

	while (i < b && (' ' == t[i] || '\t' == t[i])) {
		i++;
	}
	if (i >= b) {
		return;
	}
	if ('#' == t[i]) {
		paint(cls, i, b, WF_SC_COMMENT);
		return;
	}
	if (b - i > 7 && !memcmp(t + i, "export", 6) &&
	        (' ' == t[i + 6] || '\t' == t[i + 6])) {
		paint(cls, i, i + 6, WF_SC_KEYWORD);
		i += 6;
		while (i < b && (' ' == t[i] || '\t' == t[i])) {
			i++;
		}
	}
	name_start = i;
	while (i < b && name_char((unsigned char)t[i])) {
		i++;
	}
	name_end = i;
	while (i < b && (' ' == t[i] || '\t' == t[i])) {
		i++;
	}
	if (name_end == name_start || i >= b || '=' != t[i]) {
		return; /* not NAME=value: left plain */
	}
	paint(cls, name_start, name_end, WF_SC_KEY);
	paint(cls, i, i + 1, WF_SC_OPERATOR);
	v = i + 1;
	while (v < b && (' ' == t[v] || '\t' == t[v])) {
		v++;
	}
	if (v < b && ('"' == t[v] || '\'' == t[v])) {
		char q = t[v];
		size_t e = v + 1;

		while (e < b && q != t[e]) {
			if ('"' == q && '\\' == t[e] && e + 1 < b) {
				e++;
			}
			e++;
		}
		if (e < b) {
			e++; /* the closing quote */
		}
		paint(cls, v, e, WF_SC_STRING);
		if ('"' == q) {
			for (size_t k = v; k + 1 < e; k++) {
				if ('\\' == t[k]) {
					paint(cls, k, k + 2, WF_SC_ESCAPE);
					k++;
				}
			}
			variables(t, cls, v, e);
		}
		/* a comment after the closing quote */
		while (e < b && (' ' == t[e] || '\t' == t[e])) {
			e++;
		}
		if (e < b && '#' == t[e]) {
			paint(cls, e, b, WF_SC_COMMENT);
		}
		return;
	}
	{
		size_t e = v, end = b;

		/* an unquoted value ends at " #" */
		for (; e < b; e++) {
			if ('#' == t[e] && e > v &&
			        (' ' == t[e - 1] || '\t' == t[e - 1])) {
				paint(cls, e, b, WF_SC_COMMENT);
				end = e;
				break;
			}
		}
		variables(t, cls, v, end);
	}
}

uint32_t wf_dotenv_highlight(const char *text,
        size_t len,
        const WfSynLimits *limits,
        WfSynRun **runs,
        uint32_t *nruns)
{
	uint32_t max_text = limits && limits->max_text
	        ? limits->max_text
	        : WF_SYN_DEFAULT_MAX_TEXT;
	uint32_t max_line = limits && limits->max_line
	        ? limits->max_line
	        : WF_SYN_DEFAULT_MAX_LINE;
	uint32_t max_runs = limits && limits->max_runs
	        ? limits->max_runs
	        : WF_SYN_DEFAULT_MAX_RUNS;
	uint8_t *cls;
	size_t at = 0;
	uint32_t st;

	if (!runs || !nruns || (!text && len)) {
		return WF_SYN_E_BADARG;
	}
	*runs = NULL;
	*nruns = 0;
	if (len > max_text || len > 0xfffffffeu) {
		return WF_SYN_E_LIMIT;
	}
	cls = calloc(len ? len : 1u, 1);
	if (!cls) {
		return WF_SYN_E_NOMEM;
	}
	while (at < len) {
		const char *nl = memchr(text + at, '\n', len - at);
		size_t end = nl ? (size_t)(nl - text) : len;
		size_t stop = end > at && '\r' == text[end - 1] ? end - 1 : end;

		if (end - at > max_line) {
			free(cls);
			return WF_SYN_E_LIMIT;
		}
		one_line(text, cls, at, stop);
		at = end + 1;
	}
	st = wf_syn_runs_build(cls, (uint32_t)len, max_runs, runs, nruns);
	free(cls);
	return st;
}
