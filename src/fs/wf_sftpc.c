#include "wf_sftpc.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "wf_loc.h"

#define CHUNK 32768u

struct WfSftpClient
{
	WfSftpStream io;
	uint32_t ms;
	uint32_t next_id;
	uint32_t status; /* the server's last STATUS code */
	bool broken;
	bool (*cancelled)(void *ctx); /* NULL: nothing can end a call early */
	void *cctx;
	bool posix_rename, fsync;
	uint8_t *buf; /* one packet: length field + body */
};

static WfStatus fail(WfSftpClient *c, WfStatus s)
{
	c->broken =
	        true; /* the stream or the protocol can no longer be trusted */
	return s;
}

static bool asked_to_stop(const WfSftpClient *c)
{
	return c->cancelled && c->cancelled(c->cctx);
}

/* Exactly `n` bytes, in as many reads as it takes, all within `deadline`
 * (GetTickCount64 time): a reply that trickles in does not get a fresh wait
 * with every few bytes. With a way to be told to stop, the wait is in slices
 * and the question is asked between them; a slice that gets nothing is not an
 * end, the deadline is. */
static WfStatus read_exact(
        WfSftpClient *c, uint8_t *b, size_t n, ULONGLONG deadline)
{
	size_t have = 0;

	while (have < n) {
		size_t got = 0;
		ULONGLONG now = GetTickCount64();
		uint32_t wait;
		WfStatus s;

		if (asked_to_stop(c)) {
			return WF_E_CANCELLED;
		}
		if (now >= deadline) {
			return WF_E_TIMEOUT;
		}
		wait = (uint32_t)(deadline - now);
		if (c->cancelled && wait > WF_SFTP_CANCEL_SLICE_MS) {
			wait = WF_SFTP_CANCEL_SLICE_MS;
		}
		s = c->io.read(c->io.ctx, b + have, n - have, &got, wait);

		if (WF_E_TIMEOUT == s && c->cancelled && 0 == got) {
			continue; /* nothing yet: look at the clock and the question */
		}
		if (WF_OK != s) {
			return s;
		}
		if (0 == got || got > n - have) {
			return WF_E_CORRUPT; /* the stream ended, or lied */
		}
		have += got;
	}
	return WF_OK;
}

/* Reads one whole packet into c->buf and parses its body into *m. */
static WfStatus read_reply(WfSftpClient *c, WfSftpMsg *m)
{
	size_t total = 0;
	ULONGLONG deadline = GetTickCount64() + c->ms; /* the whole packet */
	WfStatus s = read_exact(c, c->buf, 4, deadline);

	if (WF_OK != s) {
		return fail(c, s);
	}
	s = wf_sftp_frame(c->buf, 4, &total);
	if (WF_E_RANGE != s || total < 5 || total > WF_SFTP_MAX_PACKET + 4u) {
		return fail(c, WF_E_CORRUPT);
	}
	s = read_exact(c, c->buf + 4, total - 4, deadline);
	if (WF_OK != s) {
		return fail(c, s);
	}
	s = wf_sftp_parse(c->buf + 4, total - 4, m);
	return WF_OK == s ? WF_OK : fail(c, s);
}

static WfStatus send_all(WfSftpClient *c, const uint8_t *b, size_t n)
{
	WfStatus s = c->io.write(c->io.ctx, b, n, c->ms);

	return WF_OK == s ? WF_OK : fail(c, s);
}

/* One request, one reply that must answer it. */
static WfStatus
call(WfSftpClient *c, const uint8_t *req, size_t n, uint32_t id, WfSftpMsg *m)
{
	WfStatus s;

	if (c->broken) {
		return WF_E_OS;
	}
	if (0 == n) {
		return WF_E_INVAL; /* the request could not be built: nothing sent */
	}
	if (asked_to_stop(c)) {
		return fail(c, WF_E_CANCELLED);
	}
	s = send_all(c, req, n);
	if (WF_OK != s) {
		return s;
	}
	s = read_reply(c, m);
	if (WF_OK != s) {
		return s;
	}
	if (m->id != id) {
		return fail(c, WF_E_CORRUPT);
	}
	return WF_OK;
}

static WfStatus status_to_wf(uint32_t code)
{
	switch (code) {
	case WF_SFTP_OK:                return WF_OK;
	case WF_SFTP_NO_SUCH_FILE:      return WF_E_NOENT;
	case WF_SFTP_PERMISSION_DENIED: return WF_E_ACCESS;
	default:                        return WF_E_OS;
	}
}

/* Saves and maps the STATUS a server answered with where it owed data (a
 * handle, attributes, names, bytes). STATUS OK carries none of them: it is not
 * a success, it is a server that does not follow the protocol, and a caller
 * that took it as one would use what was never written (or ask again for ever,
 * as a READ answered OK again and again). */
static WfStatus server_error(WfSftpClient *c, const WfSftpMsg *m)
{
	c->status = m->status;
	if (WF_SFTP_OK == m->status) {
		return fail(c, WF_E_CORRUPT);
	}
	return status_to_wf(m->status);
}

void wf_sftpc_set_cancel(
        WfSftpClient *c, bool (*cancelled)(void *ctx), void *cctx)
{
	if (c) {
		c->cancelled = cancelled;
		c->cctx = cctx;
	}
}

WfStatus wf_sftpc_open(
        const WfSftpStream *stream, uint32_t ms, WfSftpClient **out)
{
	return wf_sftpc_open_ex(stream, ms, NULL, NULL, out);
}

WfStatus wf_sftpc_open_ex(const WfSftpStream *stream,
        uint32_t ms,
        bool (*cancelled)(void *ctx),
        void *cctx,
        WfSftpClient **out)
{
	uint8_t req[16];
	WfSftpClient *c;
	WfSftpMsg m = {0};
	size_t n;
	WfStatus s;

	if (!stream || !stream->read || !stream->write || !out || 0 == ms) {
		return WF_E_INVAL;
	}
	*out = NULL;
	c = calloc(1, sizeof *c);
	if (!c) {
		return WF_E_NOMEM;
	}
	c->buf = malloc(WF_SFTP_MAX_PACKET + 4u);
	if (!c->buf) {
		free(c);
		return WF_E_NOMEM;
	}
	c->io = *stream;
	c->ms = ms;
	c->cancelled = cancelled;
	c->cctx = cctx;
	c->next_id = 1;
	n = wf_sftp_build_init(req, sizeof req);
	s = n ? send_all(c, req, n) : WF_E_INVAL;
	if (WF_OK == s) {
		s = read_reply(c, &m);
	}
	if (WF_OK == s && WF_SFTP_VERSION != m.type) {
		s = fail(c, WF_E_CORRUPT);
	}
	if (WF_OK != s) {
		wf_sftpc_close(c);
		return s;
	}
	c->posix_rename = m.ext_posix_rename;
	c->fsync = m.ext_fsync;
	*out = c;
	return WF_OK;
}

void wf_sftpc_close(WfSftpClient *c)
{
	if (c) {
		free(c->buf);
		free(c);
	}
}

bool wf_sftpc_broken(const WfSftpClient *c)
{
	return !c || c->broken;
}

uint32_t wf_sftpc_server_status(const WfSftpClient *c)
{
	return c ? c->status : WF_SFTP_FAILURE;
}

bool wf_sftpc_has_posix_rename(const WfSftpClient *c)
{
	return c && c->posix_rename;
}

bool wf_sftpc_has_fsync(const WfSftpClient *c)
{
	return c && c->fsync;
}

void wf_sftp_listing_free(WfSftpListing *l)
{
	if (!l) {
		return;
	}
	for (size_t i = 0; i < l->n; i++) {
		free(l->e[i].name);
	}
	free(l->e);
	memset(l, 0, sizeof *l);
}

/* OPENDIR / OPEN: a handle, copied out because the packet buffer is reused. */
static WfStatus get_handle(WfSftpClient *c,
        uint8_t type,
        const uint8_t *path,
        size_t path_n,
        uint8_t handle[WF_SFTP_MAX_HANDLE],
        size_t *handle_n)
{
	uint8_t req[WF_POSIX_PATH_MAX + 64];
	uint32_t id = c->next_id++;
	WfSftpMsg m;
	WfStatus s = call(c, req,
	        wf_sftp_build_path(req, sizeof req, type, id, path, path_n), id,
	        &m);

	if (WF_OK != s) {
		return s;
	}
	if (WF_SFTP_STATUS == m.type) {
		return server_error(c, &m);
	}
	if (WF_SFTP_HANDLE != m.type) {
		return fail(c, WF_E_CORRUPT);
	}
	memcpy(handle, m.handle, m.handle_n);
	*handle_n = m.handle_n;
	return WF_OK;
}

static void close_handle(WfSftpClient *c, const uint8_t *h, size_t n)
{
	uint8_t req[WF_SFTP_MAX_HANDLE + 32];
	uint32_t id = c->next_id++;
	WfSftpMsg m;

	/* Best-effort CLOSE; its result is not propagated to the operation's caller. */
	if (!c->broken) {
		(void)call(c, req,
		        wf_sftp_build_handle(
		                req, sizeof req, WF_SFTP_CLOSE, id, h, n),
		        id, &m);
	}
}

static WfStatus add_entry(WfSftpListing *l, size_t *cap, const WfSftpName *e)
{
	WfSftpEntry *ne;

	if (l->n == *cap) {
		size_t grown = *cap ? *cap * 2 : 64;

		if (*cap > SIZE_MAX / 2 / sizeof *l->e) {
			return WF_E_RANGE;
		}
		ne = realloc(l->e, grown * sizeof *l->e);
		if (!ne) {
			return WF_E_NOMEM;
		}
		l->e = ne;
		*cap = grown;
	}
	l->e[l->n].name = malloc(e->name_n);
	if (!l->e[l->n].name) {
		return WF_E_NOMEM;
	}
	memcpy(l->e[l->n].name, e->name, e->name_n);
	l->e[l->n].name_n = e->name_n;
	l->e[l->n].attrs = e->attrs;
	l->n++;
	return WF_OK;
}

WfStatus wf_sftpc_list(WfSftpClient *c,
        const uint8_t *path,
        size_t path_n,
        size_t limit,
        WfSftpListing *out)
{
	uint8_t h[WF_SFTP_MAX_HANDLE], req[WF_SFTP_MAX_HANDLE + 32];
	size_t hn = 0, cap = 0, empty = 0;
	WfStatus s;
	bool done = false;

	if (!c || !out || 0 == limit || !wf_posix_path_ok(path, path_n)) {
		return WF_E_INVAL;
	}
	memset(out, 0, sizeof *out);
	s = get_handle(c, WF_SFTP_OPENDIR, path, path_n, h, &hn);
	if (WF_OK != s) {
		return s;
	}
	while (WF_OK == s && !done) {
		uint32_t id = c->next_id++;
		WfSftpMsg m;
		WfSftpName e;
		bool got = false;

		s = call(c, req,
		        wf_sftp_build_handle(
		                req, sizeof req, WF_SFTP_READDIR, id, h, hn),
		        id, &m);
		if (WF_OK != s) {
			break;
		}
		if (WF_SFTP_STATUS == m.type) {
			if (WF_SFTP_EOF == m.status) {
				done = true;
			} else {
				s = server_error(c, &m);
			}
			continue;
		}
		if (WF_SFTP_NAME != m.type) {
			s = fail(c, WF_E_CORRUPT);
			break;
		}
		for (size_t seen = 0;; seen++) {
			s = wf_sftp_name_next(&m, &e, &got);
			if (WF_OK != s) {
				s = fail(c, WF_E_CORRUPT);
				break;
			}
			if (!got) {
				/* a server that keeps answering with nothing
				 * would keep this loop for ever */
				if (0 == seen && ++empty > 4) {
					s = fail(c, WF_E_CORRUPT);
				}
				break;
			}
			if (out->n == limit) {
				out->truncated = true;
				done = true;
				break;
			}
			s = add_entry(out, &cap, &e);
			if (WF_OK != s) {
				break;
			}
		}
	}
	close_handle(c, h, hn);
	if (WF_OK != s) {
		wf_sftp_listing_free(out);
	}
	return s;
}

WfStatus wf_sftpc_stat(WfSftpClient *c,
        const uint8_t *path,
        size_t path_n,
        bool follow,
        WfSftpAttrs *out)
{
	uint8_t req[WF_POSIX_PATH_MAX + 64];
	uint32_t id;
	WfSftpMsg m;
	WfStatus s;

	if (!c || !out || !wf_posix_path_ok(path, path_n)) {
		return WF_E_INVAL;
	}
	id = c->next_id++;
	s = call(c, req,
	        wf_sftp_build_path(req, sizeof req,
	                follow ? WF_SFTP_STAT : WF_SFTP_LSTAT, id, path,
	                path_n),
	        id, &m);
	if (WF_OK != s) {
		return s;
	}
	if (WF_SFTP_STATUS == m.type) {
		return server_error(c, &m);
	}
	if (WF_SFTP_ATTRS != m.type) {
		return fail(c, WF_E_CORRUPT);
	}
	*out = m.attrs;
	return WF_OK;
}

WfStatus wf_sftpc_read_range(WfSftpClient *c,
        const uint8_t *path,
        size_t path_n,
        uint64_t offset,
        size_t len,
        uint8_t *buf,
        size_t *got)
{
	uint8_t h[WF_SFTP_MAX_HANDLE], req[WF_SFTP_MAX_HANDLE + 64];
	size_t hn = 0, done = 0;
	WfSftpAttrs a;
	WfStatus s;
	bool eof = false;

	if (!c || !buf || !got || (len && offset > UINT64_MAX - len)) {
		return WF_E_INVAL;
	}
	*got = 0;
	s = wf_sftpc_stat(c, path, path_n, false, &a);
	if (WF_OK != s) {
		return s;
	}
	/* only a regular file is read: not a link (followed it would be
	 * another file), not a device or a pipe (reading can block or act) */
	if (!a.has_perm || WF_SFTP_S_IFREG != (a.perm & WF_SFTP_S_IFMT)) {
		return WF_E_INVAL;
	}
	if (0 == len) {
		return WF_OK;
	}
	s = get_handle(c, WF_SFTP_OPEN, path, path_n, h, &hn);
	if (WF_OK != s) {
		return s;
	}
	while (WF_OK == s && done < len && !eof) {
		size_t want = len - done < CHUNK ? len - done : CHUNK;
		uint32_t id = c->next_id++;
		WfSftpMsg m;

		s = call(c, req,
		        wf_sftp_build_read(req, sizeof req, id, h, hn,
		                offset + done, (uint32_t)want),
		        id, &m);
		if (WF_OK != s) {
			break;
		}
		if (WF_SFTP_STATUS == m.type) {
			if (WF_SFTP_EOF == m.status) {
				eof = true;
			} else {
				s = server_error(c, &m);
			}
			continue;
		}
		if (WF_SFTP_DATA != m.type || m.data_n > want ||
		        0 == m.data_n) {
			s = fail(c,
			        WF_E_CORRUPT); /* more than asked for, or none */
			break;
		}
		memcpy(buf + done, m.data, m.data_n);
		done += m.data_n;
	}
	close_handle(c, h, hn);
	if (WF_OK == s) {
		*got = done;
	}
	return s;
}

WfStatus wf_sftpc_realpath(WfSftpClient *c,
        const uint8_t *path,
        size_t path_n,
        uint8_t *out,
        size_t cap,
        size_t *out_n)
{
	uint8_t req[WF_POSIX_PATH_MAX + 64];
	uint32_t id;
	WfSftpMsg m;
	const uint8_t *name;
	size_t n;
	WfStatus s;

	/* Dot components are left for the server to resolve. */
	if (!c || !out || !out_n || !path || 0 == path_n ||
	        path_n > WF_POSIX_PATH_MAX) {
		return WF_E_INVAL;
	}
	id = c->next_id++;
	s = call(c, req,
	        wf_sftp_build_path(
	                req, sizeof req, WF_SFTP_REALPATH, id, path, path_n),
	        id, &m);
	if (WF_OK != s) {
		return s;
	}
	if (WF_SFTP_STATUS == m.type) {
		return server_error(c, &m);
	}
	if (WF_SFTP_NAME != m.type ||
	        WF_OK != wf_sftp_name_first(&m, &name, &n) ||
	        !wf_posix_path_ok(name, n)) {
		return fail(c,
		        WF_E_CORRUPT); /* a server must answer an absolute path */
	}
	if (n + 1 > cap) {
		return WF_E_RANGE;
	}
	memcpy(out, name, n);
	out[n] = 0;
	*out_n = n;
	return WF_OK;
}
