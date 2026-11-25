#include "wf_sftp.h"

#include <string.h>

#include "wf_loc.h"

typedef struct
{
	uint8_t *p;
	size_t cap, n;
	bool ok;
} Out;

static void put(Out *o, const void *src, size_t len)
{
	if (!o->ok || len > o->cap - o->n) {
		o->ok = false;
		return;
	}
	if (len) {
		memcpy(o->p + o->n, src, len);
	}
	o->n += len;
}

static void put_u32(Out *o, uint32_t v)
{
	uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16),
	        (uint8_t)(v >> 8), (uint8_t)v};

	put(o, b, 4);
}

static void put_u64(Out *o, uint64_t v)
{
	put_u32(o, (uint32_t)(v >> 32));
	put_u32(o, (uint32_t)v);
}

static void put_str(Out *o, const uint8_t *s, size_t n)
{
	if (n > WF_SFTP_MAX_PACKET) {
		o->ok = false;
		return;
	}
	put_u32(o, (uint32_t)n);
	put(o, s, n);
}

/* Starts a packet: room for the length, the type, then the caller's fields. */
static Out begin(uint8_t *out, size_t cap, uint8_t type)
{
	Out o = {out, cap, 0, out && cap >= 5};

	put_u32(&o, 0);
	put(&o, &type, 1);
	return o;
}

static size_t finish(Out *o)
{
	uint32_t len;

	if (!o->ok || o->n < 5 || o->n - 4 > WF_SFTP_MAX_PACKET) {
		return 0;
	}
	len = (uint32_t)(o->n - 4);
	o->p[0] = (uint8_t)(len >> 24);
	o->p[1] = (uint8_t)(len >> 16);
	o->p[2] = (uint8_t)(len >> 8);
	o->p[3] = (uint8_t)len;
	return o->n;
}

size_t wf_sftp_build_init(uint8_t *out, size_t cap)
{
	Out o = begin(out, cap, WF_SFTP_INIT);

	put_u32(&o, 3);
	return finish(&o);
}

size_t wf_sftp_build_path(uint8_t *out,
        size_t cap,
        uint8_t type,
        uint32_t id,
        const uint8_t *path,
        size_t path_n)
{
	Out o;

	if (type != WF_SFTP_LSTAT && type != WF_SFTP_STAT &&
	        type != WF_SFTP_OPENDIR && type != WF_SFTP_OPEN &&
	        type != WF_SFTP_REALPATH && type != WF_SFTP_READLINK) {
		return 0;
	}
	if (!path || path_n < 1 || path_n > WF_POSIX_PATH_MAX) {
		return 0;
	}
	for (size_t i = 0; i < path_n; i++) {
		if (0 == path[i]) {
			return 0; /* a NUL would end the name at the server */
		}
	}
	o = begin(out, cap, type);
	put_u32(&o, id);
	put_str(&o, path, path_n);
	if (WF_SFTP_OPEN == type) {
		put_u32(&o,
		        1); /* SSH_FXF_READ: there is no way to ask for more */
		put_u32(&o, 0); /* attrs: no flags */
	}
	return finish(&o);
}

size_t wf_sftp_build_handle(uint8_t *out,
        size_t cap,
        uint8_t type,
        uint32_t id,
        const uint8_t *handle,
        size_t handle_n)
{
	Out o;

	if ((type != WF_SFTP_READDIR && type != WF_SFTP_CLOSE) || !handle ||
	        handle_n < 1 || handle_n > WF_SFTP_MAX_HANDLE) {
		return 0;
	}
	o = begin(out, cap, type);
	put_u32(&o, id);
	put_str(&o, handle, handle_n);
	return finish(&o);
}

size_t wf_sftp_build_read(uint8_t *out,
        size_t cap,
        uint32_t id,
        const uint8_t *handle,
        size_t handle_n,
        uint64_t offset,
        uint32_t len)
{
	Out o;

	if (!handle || handle_n < 1 || handle_n > WF_SFTP_MAX_HANDLE ||
	        0 == len || len > WF_SFTP_MAX_PACKET - 64) {
		return 0;
	}
	o = begin(out, cap, WF_SFTP_READ);
	put_u32(&o, id);
	put_str(&o, handle, handle_n);
	put_u64(&o, offset);
	put_u32(&o, len);
	return finish(&o);
}

typedef struct
{
	const uint8_t *p;
	size_t left;
	bool ok;
} In;

static uint32_t get_u32(In *r)
{
	uint32_t v;

	if (!r->ok || r->left < 4) {
		r->ok = false;
		return 0;
	}
	v = (uint32_t)r->p[0] << 24 | (uint32_t)r->p[1] << 16 |
	        (uint32_t)r->p[2] << 8 | r->p[3];
	r->p += 4;
	r->left -= 4;
	return v;
}

static uint64_t get_u64(In *r)
{
	uint64_t hi = get_u32(r);

	return hi << 32 | get_u32(r);
}

/* A length-prefixed string that must lie inside what arrived. */
static const uint8_t *get_str(In *r, size_t *n)
{
	uint32_t len = get_u32(r);
	const uint8_t *s = r->p;

	if (!r->ok || len > r->left) {
		r->ok = false;
		*n = 0;
		return NULL;
	}
	r->p += len;
	r->left -= len;
	*n = len;
	return s;
}

WfStatus wf_sftp_frame(const uint8_t *buf, size_t n, size_t *total)
{
	uint32_t len;

	if (!buf || !total) {
		return WF_E_INVAL;
	}
	*total = 0;
	if (n < 4) {
		return WF_E_RANGE;
	}
	len = (uint32_t)buf[0] << 24 | (uint32_t)buf[1] << 16 |
	        (uint32_t)buf[2] << 8 | buf[3];
	if (0 == len || len > WF_SFTP_MAX_PACKET) {
		return WF_E_CORRUPT;
	}
	*total = (size_t)len + 4;
	return n >= *total ? WF_OK : WF_E_RANGE;
}

static bool parse_attrs(In *r, WfSftpAttrs *a)
{
	uint32_t flags = get_u32(r);

	memset(a, 0, sizeof *a);
	if (!r->ok || (flags & ~(0x1u | 0x2u | 0x4u | 0x8u | 0x80000000u))) {
		return false; /* a flag we cannot size: the rest is unreadable */
	}
	if (flags & 0x1u) {
		a->has_size = true;
		a->size = get_u64(r);
	}
	if (flags & 0x2u) {
		(void)get_u32(r); /* uid */
		(void)get_u32(r); /* gid */
	}
	if (flags & 0x4u) {
		a->has_perm = true;
		a->perm = get_u32(r);
	}
	if (flags & 0x8u) {
		(void)get_u32(r); /* atime */
		a->has_time = true;
		a->mtime = get_u32(r);
	}
	if (flags & 0x80000000u) {
		uint32_t count = get_u32(r);

		if (count > 1024u) {
			return false;
		}
		for (uint32_t i = 0; i < count && r->ok; i++) {
			size_t n;

			(void)get_str(r, &n);
			(void)get_str(r, &n);
		}
	}
	return r->ok;
}

WfStatus wf_sftp_parse(const uint8_t *pkt, size_t n, WfSftpMsg *m)
{
	In r = {pkt, n, pkt && n >= 1};

	if (!pkt || !m) {
		return WF_E_INVAL;
	}
	memset(m, 0, sizeof *m);
	if (n < 1 || n > WF_SFTP_MAX_PACKET) {
		return WF_E_CORRUPT;
	}
	m->type = r.p[0];
	r.p++;
	r.left--;

	switch (m->type) {
	case WF_SFTP_VERSION: {
		m->version = get_u32(&r);
		if (!r.ok) {
			return WF_E_CORRUPT;
		}
		if (m->version != 3) {
			return WF_E_FORMAT; /* only version 3 is spoken */
		}
		while (r.left) {
			size_t nn, dn;
			const uint8_t *name = get_str(&r, &nn);
			const uint8_t *data = get_str(&r, &dn);

			if (!r.ok) {
				return WF_E_CORRUPT;
			}
			if (1 == dn && '1' == data[0]) {
				if (nn == 24 &&
				        !memcmp(name,
				                "posix-rename@openssh.com",
				                24)) {
					m->ext_posix_rename = true;
				}
				if (nn == 17 &&
				        !memcmp(name, "fsync@openssh.com",
				                17)) {
					m->ext_fsync = true;
				}
			}
		}
		return WF_OK;
	}
	case WF_SFTP_STATUS: {
		size_t sn;

		m->id = get_u32(&r);
		m->status = get_u32(&r);
		if (r.left) {
			(void)get_str(&r, &sn); /* message */
		}
		if (r.left) {
			(void)get_str(&r, &sn); /* language tag */
		}
		return r.ok && 0 == r.left ? WF_OK : WF_E_CORRUPT;
	}
	case WF_SFTP_HANDLE: {
		m->id = get_u32(&r);
		m->handle = get_str(&r, &m->handle_n);
		if (!r.ok || r.left) {
			return WF_E_CORRUPT;
		}
		return m->handle_n >= 1 && m->handle_n <= WF_SFTP_MAX_HANDLE
		        ? WF_OK
		        : WF_E_CORRUPT;
	}
	case WF_SFTP_DATA:
		m->id = get_u32(&r);
		m->data = get_str(&r, &m->data_n);
		return r.ok && 0 == r.left ? WF_OK : WF_E_CORRUPT;
	case WF_SFTP_ATTRS:
		m->id = get_u32(&r);
		return parse_attrs(&r, &m->attrs) && 0 == r.left ? WF_OK
		                                                 : WF_E_CORRUPT;
	case WF_SFTP_NAME:
		m->id = get_u32(&r);
		m->names = get_u32(&r);
		/* an entry is at least a name, a long name and a flags word */
		if (!r.ok || m->names > WF_SFTP_MAX_NAMES ||
		        (size_t)m->names > r.left / 12) {
			return WF_E_CORRUPT;
		}
		m->cursor = r.p;
		m->left = r.left;
		return WF_OK;
	default: return WF_E_CORRUPT; /* a request type or garbage */
	}
}

WfStatus wf_sftp_name_next(WfSftpMsg *m, WfSftpName *e, bool *got)
{
	if (!m || !e || !got || m->type != WF_SFTP_NAME) {
		return WF_E_INVAL;
	}
	*got = false;
	while (m->names) {
		In r = {m->cursor, m->left, true};
		size_t long_n;

		e->name = get_str(&r, &e->name_n);
		(void)get_str(&r, &long_n);
		if (!r.ok || !parse_attrs(&r, &e->attrs)) {
			return WF_E_CORRUPT;
		}
		m->cursor = r.p;
		m->left = r.left;
		m->names--;
		if ((1 == e->name_n && '.' == e->name[0]) ||
		        (2 == e->name_n && '.' == e->name[0] &&
		                '.' == e->name[1])) {
			continue;
		}
		if (!wf_posix_name_ok(e->name, e->name_n)) {
			return WF_E_CORRUPT;
		}
		*got = true;
		return WF_OK;
	}
	if (m->left) {
		return WF_E_CORRUPT; /* bytes after the last entry */
	}
	return WF_OK;
}

WfStatus wf_sftp_name_first(const WfSftpMsg *m, const uint8_t **name, size_t *n)
{
	In r;
	WfSftpAttrs attrs;
	const uint8_t *first;
	size_t first_n, long_n;

	if (!m || !name || !n || m->type != WF_SFTP_NAME || 1 != m->names) {
		return WF_E_INVAL;
	}
	*name = NULL;
	*n = 0;
	r.p = m->cursor;
	r.left = m->left;
	r.ok = true;
	first = get_str(&r, &first_n);
	(void)get_str(&r, &long_n);
	if (!r.ok || !parse_attrs(&r, &attrs) || r.left) {
		return WF_E_CORRUPT;
	}
	*name = first;
	*n = first_n;
	return WF_OK;
}

const char *wf_sftp_status_text(uint32_t s)
{
	switch (s) {
	case WF_SFTP_OK:                return "ok";
	case WF_SFTP_EOF:               return "end of file";
	case WF_SFTP_NO_SUCH_FILE:      return "no such file";
	case WF_SFTP_PERMISSION_DENIED: return "permission denied";
	case WF_SFTP_FAILURE:           return "the server reported a failure";
	case WF_SFTP_BAD_MESSAGE:     return "the server rejected the request";
	case WF_SFTP_NO_CONNECTION:   return "no connection";
	case WF_SFTP_CONNECTION_LOST: return "the connection was lost";
	case WF_SFTP_OP_UNSUPPORTED:  return "the server does not support this";
	default:                      return "unknown status";
	}
}
