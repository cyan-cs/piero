#include "wf_term_caps.h"
#include <string.h>

#include "../base/wf_buf.h"

bool wf_sync_begin(WfBuf *out, size_t *mark)
{
	if (!out || !mark || WF_OK != wf_buf_puts(out, WF_SYNC_BEGIN)) {
		return false;
	}
	*mark = out->len;
	return true;
}

bool wf_sync_end(WfBuf *out, size_t mark)
{
	const size_t begin = sizeof WF_SYNC_BEGIN - 1;

	if (!out || mark < begin || mark > out->len) {
		return false;
	}
	if (out->len == mark) { /* nothing was drawn: send nothing at all */
		out->len = mark - begin;
		if (out->data) {
			out->data[out->len] = '\0';
		}
		return true;
	}
	return WF_OK == wf_buf_puts(out, WF_SYNC_END);
}

const char *wf_term_caps_query(void)
{
	return "\x1b[c\x1b[16t\x1b[14t\x1b[18t\x1b[?1;1;0S\x1b[?2026$p";
}

void wf_term_caps_begin(WfTermCaps *c, uint64_t now)
{
	uint32_t version = c->version + 1;
	memset(c, 0, sizeof *c);
	c->version = version;
	c->deadline = now + 750;
}

bool wf_term_caps_ready(const WfTermCaps *c)
{
	return c->da && c->sixel && c->rectangular && c->cell_w && c->cell_h;
}

/* DECRPM: "CSI ? 2026 ; Ps $ y". Ps 1 (set) and 2 (reset) say the mode is
 * known and changeable; 0 (not recognised) and 3/4 (fixed) do not enable it. */
static bool mode_report(WfTermCaps *c)
{
	static const char prefix[] = "\x1b[?2026;";
	size_t n = sizeof prefix - 1;

	if (c->len != n + 3 || memcmp(c->reply, prefix, n) ||
	        c->reply[n + 1] != '$' || c->reply[n + 2] != 'y') {
		return false;
	}
	if (c->reply[n] == '1' || c->reply[n] == '2') {
		c->sync_output = true;
	}
	return true;
}

static void response(WfTermCaps *c)
{
	if (mode_report(c)) {
		c->version++;
		return;
	}
	unsigned v[32] = {0};
	size_t n = 0, i = 2;
	bool digit = false, priv = c->reply[i] == '?';
	char final = c->reply[c->len - 1];
	if (priv) {
		i++;
	}
	for (; i + 1 < c->len; i++) {
		unsigned ch = (unsigned char)c->reply[i];
		if (ch >= '0' && ch <= '9') {
			if (v[n] > 6553 || (v[n] == 6553 && ch > '5')) {
				return;
			}
			v[n] = v[n] * 10 + ch - '0';
			digit = true;
		} else if (ch == ';' && digit && n + 1 < 32) {
			n++;
			digit = false;
		} else {
			return;
		}
	}
	if (!digit) {
		return;
	}
	n++;
	if (priv && final == 'c' && n >= 2 && !c->da) {
		c->da = true;
		for (i = 1; i < n; i++) {
			c->sixel |= v[i] == 4;
			c->rectangular |= v[i] == 28;
		}
	} else if (priv && final == 'S' && n == 3 && v[0] == 1 && v[1] == 0 &&
	        v[2] >= 2) {
		c->colors = v[2] < 256 ? v[2] : 256;
	} else if (!priv && final == 't' && n == 3 && v[1] && v[2]) {
		if (v[0] == 6 && v[1] <= 256 && v[2] <= 256) {
			c->cell_h = v[1];
			c->cell_w = v[2];
		} else if (v[0] == 4) {
			c->pixel_h = v[1];
			c->pixel_w = v[2];
		} else if (v[0] == 8 && v[1] <= 4096 && v[2] <= 4096) {
			c->rows = v[1];
			c->cols = v[2];
		}
		if (!c->cell_w && c->cols && c->rows && c->pixel_w &&
		        c->pixel_h && c->pixel_w % c->cols == 0 &&
		        c->pixel_h % c->rows == 0 &&
		        c->pixel_w / c->cols <= 256 &&
		        c->pixel_h / c->rows <= 256) {
			c->cell_w = c->pixel_w / c->cols;
			c->cell_h = c->pixel_h / c->rows;
		}
	}
	c->version++;
}

void wf_term_caps_feed(WfTermCaps *c, unsigned ch, uint64_t now)
{
	if (ch == 27) {
		c->len = 1;
		c->reply[0] = 27;
		c->dropping = false;
		return;
	}
	if (c->dropping) {
		if (ch >= 0x40 && ch <= 0x7e) {
			c->dropping = false;
		}
		return;
	}
	if (!c->len) {
		return;
	}
	if ((c->len == 1 && ch != '[') || ch < 32 || ch > 126 ||
	        c->len == sizeof c->reply) {
		c->len = 0;
		c->dropping = true;
		return;
	}
	c->reply[c->len++] = (char)ch;
	if (c->len > 2 && ch >= 0x40 && ch <= 0x7e) {
		if (now <= c->deadline) {
			response(c);
		}
		c->len = 0;
	}
}
