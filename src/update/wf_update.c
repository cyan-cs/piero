#include "wf_update.h"
#include <limits.h>
#include <string.h>
#include "text/wf_text.h"
#include "edit/wf_eenc.h"

typedef struct
{
	uint32_t part[3];
	bool pre;
} Version;
static bool version(const char *s, Version *v)
{
	memset(v, 0, sizeof *v);
	if (!s || strlen(s) >= 64) {
		return false;
	}
	if (*s == 'v') {
		s++;
	}
	for (int i = 0; i < 3; i++) {
		const char *start = s;
		if (*s < '0' || *s > '9') {
			return false;
		}
		while (*s >= '0' && *s <= '9') {
			unsigned n = (unsigned)(*s++ - '0');
			if (v->part[i] > (UINT32_MAX - n) / 10) {
				return false;
			}
			v->part[i] = v->part[i] * 10 + n;
		}
		if (s - start > 1 && *start == '0') {
			return false;
		}
		if (i < 2) {
			if (*s != '.') {
				return false;
			}
			s++;
		}
	}
	if (*s == '-' || *s == '+') {
		bool pre = *s == '-';
		v->pre = pre;
		s++;
		for (;;) {
			const char *start = s;
			bool numeric = true;
			while ((*s >= '0' && *s <= '9') ||
			        (*s >= 'a' && *s <= 'z') ||
			        (*s >= 'A' && *s <= 'Z') || *s == '-') {
				if (*s < '0' || *s > '9') {
					numeric = false;
				}
				s++;
			}
			if (s == start ||
			        (pre && numeric && s - start > 1 &&
			                *start == '0')) {
				return false;
			}
			if (*s == '.') {
				s++;
				continue;
			}
			if (*s == '+' && pre) {
				pre = false;
				s++;
				continue;
			}
			break;
		}
	}
	return *s == '\0';
}
typedef struct
{
	const char *p, *end;
	unsigned depth;
} Json;
static void space(Json *j)
{
	while (j->p < j->end &&
	        (*j->p == ' ' || *j->p == '\n' || *j->p == '\r' ||
	                *j->p == '\t')) {
		j->p++;
	}
}
static bool string(Json *j, char *out, size_t cap)
{
	size_t n = 0;
	if (j->p == j->end || *j->p++ != '"') {
		return false;
	}
	while (j->p < j->end) {
		unsigned char c = (unsigned char)*j->p++;
		if (c == '"') {
			if (out) {
				out[n] = '\0';
			}
			return true;
		}
		if (c < 32) {
			return false;
		}
		if (c == '\\') {
			if (j->p == j->end || out) {
				return false;
			}
			c = (unsigned char)*j->p++;
			if (c == 'u') {
				for (int i = 0; i < 4; i++) {
					if (j->p == j->end) {
						return false;
					}
					char h = *j->p++;
					if (!((h >= '0' && h <= '9') ||
					            (h >= 'a' && h <= 'f') ||
					            (h >= 'A' && h <= 'F'))) {
						return false;
					}
				}
			} else if (!strchr("\"\\/bfnrt", c)) {
				return false;
			}
		}
		if (out) {
			if (n + 1 >= cap || c >= 128) {
				return false;
			}
			out[n++] = (char)c;
		}
	}
	return false;
}
static bool value(Json *j);
static bool compound(Json *j, bool object)
{
	char close = object ? '}' : ']';
	if (++j->depth > 16) {
		return false;
	}
	j->p++;
	space(j);
	if (j->p < j->end && *j->p == close) {
		j->p++;
		j->depth--;
		return true;
	}
	for (;;) {
		if (object) {
			if (!string(j, NULL, 0)) {
				return false;
			}
			space(j);
			if (j->p == j->end || *j->p++ != ':') {
				return false;
			}
		}
		if (!value(j)) {
			return false;
		}
		space(j);
		if (j->p == j->end) {
			return false;
		}
		if (*j->p == close) {
			j->p++;
			j->depth--;
			return true;
		}
		if (*j->p++ != ',') {
			return false;
		}
		space(j);
	}
}
static bool literal(Json *j, const char *word)
{
	size_t n = strlen(word);
	if ((size_t)(j->end - j->p) < n || memcmp(j->p, word, n)) {
		return false;
	}
	j->p += n;
	return true;
}
static bool value(Json *j)
{
	space(j);
	if (j->p == j->end) {
		return false;
	}
	if (*j->p == '{') {
		return compound(j, true);
	}
	if (*j->p == '[') {
		return compound(j, false);
	}
	if (*j->p == '"') {
		return string(j, NULL, 0);
	}
	if (*j->p == 't') {
		return literal(j, "true");
	}
	if (*j->p == 'f') {
		return literal(j, "false");
	}
	if (*j->p == 'n') {
		return literal(j, "null");
	}
	if (*j->p == '-') {
		j->p++;
	}
	if (j->p == j->end) {
		return false;
	}
	if (*j->p == '0') {
		j->p++;
	} else {
		if (*j->p < '1' || *j->p > '9') {
			return false;
		}
		do {
			j->p++;
		} while (j->p < j->end && *j->p >= '0' && *j->p <= '9');
	}
	if (j->p < j->end && *j->p == '.') {
		j->p++;
		const char *start = j->p;
		while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
			j->p++;
		}
		if (start == j->p) {
			return false;
		}
	}
	if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) {
		j->p++;
		if (j->p < j->end && (*j->p == '+' || *j->p == '-')) {
			j->p++;
		}
		const char *start = j->p;
		while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
			j->p++;
		}
		if (start == j->p) {
			return false;
		}
	}
	return true;
}
WfUpdateResult wf_update_response(
        const char *current, unsigned status, const void *data, size_t size)
{
	WfUpdateResult r = {WF_UPDATE_INVALID, {0}};
	Version a, b;
	if (status == 404) {
		r.state = WF_UPDATE_NO_RELEASE;
		return r;
	}
	if (status == 403 || status == 429) {
		r.state = WF_UPDATE_LIMITED;
		return r;
	}
	if (status != 200) {
		r.state = WF_UPDATE_UNAVAILABLE;
		return r;
	}
	if (!data || size > WF_UPDATE_BODY_MAX ||
	        !wf_utf8_strict(data, size, NULL) || !version(current, &a)) {
		return r;
	}
	Json j = {data, (const char *)data + size, 0};
	unsigned seen = 0;
	space(&j);
	if (j.p == j.end || *j.p++ != '{') {
		return r;
	}
	for (;;) {
		char key[64];
		space(&j);
		if (!string(&j, key, sizeof key)) {
			return r;
		}
		space(&j);
		if (j.p == j.end || *j.p++ != ':') {
			return r;
		}
		space(&j);
		unsigned bit = !strcmp(key, "tag_name") ? 1u
		        : !strcmp(key, "draft")         ? 2u
		        : !strcmp(key, "prerelease")    ? 4u
		                                        : 0u;
		if (bit && (seen & bit)) {
			return r;
		}
		seen |= bit;
		if (bit == 1) {
			if (!string(&j, r.latest, sizeof r.latest)) {
				return r;
			}
		} else if (bit) {
			if (!literal(&j, "false")) {
				return r;
			}
		} else if (!value(&j)) {
			return r;
		}
		space(&j);
		if (j.p == j.end) {
			return r;
		}
		if (*j.p == '}') {
			j.p++;
			break;
		}
		if (*j.p++ != ',') {
			return r;
		}
	}
	space(&j);
	if (j.p != j.end || seen != 7 || !version(r.latest, &b) || b.pre) {
		return r;
	}
	r.state = WF_UPDATE_CURRENT;
	for (int i = 0; i < 3; i++) {
		if (b.part[i] != a.part[i]) {
			if (b.part[i] > a.part[i]) {
				r.state = WF_UPDATE_AVAILABLE;
			}
			return r;
		}
	}
	if (a.pre) {
		r.state = WF_UPDATE_AVAILABLE;
	}
	return r;
}
bool wf_update_due(
        bool enabled, bool consent, bool valid, uint64_t now, uint64_t previous)
{
	return wf_update_allowed(enabled, consent, valid, false) &&
	        now >= previous && (!previous || now - previous >= 86400);
}
bool wf_update_allowed(bool enabled, bool consent, bool valid, bool manual)
{
	return manual || (enabled && consent && valid);
}
