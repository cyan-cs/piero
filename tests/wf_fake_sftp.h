#ifndef WF_FAKE_SFTP_H
#define WF_FAKE_SFTP_H

/*
 * An in-memory SFTP v3 server for tests: a small tree, the requests a
 * read-only client sends, and ways to misbehave. It only computes replies
 * (on_request appends to Srv.out); how the bytes travel is up to the user of
 * this header (a function table in test_sftpc.c, a child process's stdio in
 * fake_sftpd.c).
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs/wf_sftp.h"

typedef enum
{
	F_NONE,
	F_EOF_AFTER,     /* the stream ends after `after` requests */
	F_TIMEOUT_AFTER, /* the server goes silent after `after` requests */
	F_WRONG_ID,      /* every reply carries another request's id */
	F_HUGE_LEN,      /* the reply announces 100 MB */
	F_LONG_DATA,     /* DATA longer than the READ asked for */
	F_EMPTY_NAMES,   /* READDIR answers with no entries for ever */
	F_PERM_DENIED,   /* OPENDIR is refused */
	F_NO_VERSION,    /* the first reply is not VERSION */
	F_STATUS_OK /* every request but INIT and CLOSE is answered STATUS OK */
} Fault;

typedef struct
{
	Fault fault;
	int after;
	int requests;
	size_t piece;     /* bytes per read() call: 0 = all that is there */
	unsigned slow_ms; /* real time each read() takes */
	uint8_t in[70000];
	size_t in_n;
	uint8_t out[400000];
	size_t out_n, out_at;
	bool dots_sent;
	size_t rd_index;
	bool ext;
	int open_handles;
	int closed_handles;
} Srv;

static void put32(Srv *s, uint32_t v)
{
	s->out[s->out_n++] = (uint8_t)(v >> 24);
	s->out[s->out_n++] = (uint8_t)(v >> 16);
	s->out[s->out_n++] = (uint8_t)(v >> 8);
	s->out[s->out_n++] = (uint8_t)v;
}

static void putstr(Srv *s, const void *p, size_t n)
{
	put32(s, (uint32_t)n);
	memcpy(s->out + s->out_n, p, n);
	s->out_n += n;
}

static size_t begin_reply(Srv *s, uint8_t type, uint32_t id)
{
	size_t at = s->out_n;

	put32(s, 0);
	s->out[s->out_n++] = type;
	if (WF_SFTP_VERSION != type) {
		put32(s, F_WRONG_ID == s->fault ? id + 7 : id);
	}
	return at;
}

static void end_reply(Srv *s, size_t at)
{
	uint32_t len = (uint32_t)(s->out_n - at - 4);

	if (F_HUGE_LEN == s->fault) {
		len = 100u << 20;
	}
	s->out[at] = (uint8_t)(len >> 24);
	s->out[at + 1] = (uint8_t)(len >> 16);
	s->out[at + 2] = (uint8_t)(len >> 8);
	s->out[at + 3] = (uint8_t)len;
}

static void status(Srv *s, uint32_t id, uint32_t code)
{
	size_t at = begin_reply(s, WF_SFTP_STATUS, id);

	put32(s, code);
	putstr(s, "msg", 3);
	putstr(s, "", 0);
	end_reply(s, at);
}

typedef struct
{
	const char *path;
	const char *name;
	uint32_t mode;
	uint64_t size;
} Node;

static const Node tree[] = {
        {"/", "a.txt", WF_SFTP_S_IFREG | 0644, 100},
        {"/", "A.txt", WF_SFTP_S_IFREG | 0644, 50},
        {"/", "dir", WF_SFTP_S_IFDIR | 0755, 0},
        {"/", "link", WF_SFTP_S_IFLNK | 0777, 5},
        {"/", "dev", WF_SFTP_S_IFCHR | 0600, 0},
        {"/", "bad\x1b[2J", WF_SFTP_S_IFREG | 0644, 3},
        {"/dir", "x", WF_SFTP_S_IFREG | 0644, 70000},
};

static const Node *node_at(const uint8_t *p, size_t n)
{
	char full[512];

	if (n >= sizeof full) {
		return NULL;
	}
	for (size_t i = 0; i < sizeof tree / sizeof *tree; i++) {
		size_t dn = strlen(tree[i].path);

		if (1 == dn) {
			snprintf(full, sizeof full, "/%s", tree[i].name);
		} else {
			snprintf(full, sizeof full, "%s/%s", tree[i].path,
			        tree[i].name);
		}
		if (strlen(full) == n && !memcmp(full, p, n)) {
			return &tree[i];
		}
	}
	return NULL;
}

static void put_attrs(Srv *s, uint32_t mode, uint64_t size)
{
	put32(s, 0x1 | 0x4 | 0x8);
	put32(s, (uint32_t)(size >> 32));
	put32(s, (uint32_t)size);
	put32(s, mode);
	put32(s, 1);
	put32(s, 1700000000);
}

static uint8_t content(uint64_t i)
{
	return (uint8_t)(i * 7 + 3);
}

static void on_request(Srv *s, const uint8_t *r, size_t n)
{
	uint8_t type = r[0];
	uint32_t id = n >= 5 ? (uint32_t)r[1] << 24 | (uint32_t)r[2] << 16 |
	                (uint32_t)r[3] << 8 | r[4]
	                     : 0;
	const uint8_t *str = r + 9;
	size_t sn = n >= 9
	        ? (size_t)((uint32_t)r[5] << 24 | (uint32_t)r[6] << 16 |
	                  (uint32_t)r[7] << 8 | r[8])
	        : 0;
	size_t at;

	s->requests++;
	if (F_NO_VERSION == s->fault && WF_SFTP_INIT == type) {
		status(s, 0, WF_SFTP_OK);
		return;
	}
	if (F_STATUS_OK == s->fault && WF_SFTP_INIT != type &&
	        WF_SFTP_CLOSE != type) {
		/* "done", where a handle or data was asked for; after 50 it ends in
		 * EOF, so that a client that asks again for ever fails a check
		 * instead of hanging the test */
		status(s, id, s->requests > 50 ? WF_SFTP_EOF : WF_SFTP_OK);
		return;
	}
	switch (type) {
	case WF_SFTP_INIT:
		at = begin_reply(s, WF_SFTP_VERSION, 0);
		put32(s, 3);
		if (s->ext) {
			putstr(s, "posix-rename@openssh.com", 24);
			putstr(s, "1", 1);
			putstr(s, "fsync@openssh.com", 17);
			putstr(s, "1", 1);
		}
		end_reply(s, at);
		break;
	case WF_SFTP_OPENDIR:
		if (F_PERM_DENIED == s->fault) {
			status(s, id, WF_SFTP_PERMISSION_DENIED);
			break;
		}
		if ((1 == sn && '/' == str[0]) ||
		        (4 == sn && !memcmp(str, "/dir", 4))) {
			at = begin_reply(s, WF_SFTP_HANDLE, id);
			putstr(s, 1 == sn ? "D1" : "D2", 2);
			end_reply(s, at);
			s->dots_sent = false;
			s->rd_index = 0;
			s->open_handles++;
		} else {
			status(s, id, WF_SFTP_NO_SUCH_FILE);
		}
		break;
	case WF_SFTP_READDIR: {
		bool root = 'D' == str[0] && '1' == str[1];
		size_t count_at, count = 0, shown = 0;

		at = begin_reply(s, WF_SFTP_NAME, id);
		if (F_EMPTY_NAMES == s->fault) {
			put32(s, 0);
			end_reply(s, at);
			break;
		}
		count_at = s->out_n;
		put32(s, 0);
		if (!s->dots_sent) {
			putstr(s, ".", 1);
			putstr(s, "d .", 3);
			put32(s, 0);
			putstr(s, "..", 2);
			putstr(s, "d ..", 4);
			put32(s, 0);
			count += 2;
			s->dots_sent = true;
		}
		while (s->rd_index < sizeof tree / sizeof *tree && shown < 2) {
			const Node *nd = &tree[s->rd_index++];

			if (strcmp(nd->path, root ? "/" : "/dir")) {
				continue;
			}
			putstr(s, nd->name, strlen(nd->name));
			putstr(s, "l", 1);
			put_attrs(s, nd->mode, nd->size);
			count++;
			shown++;
		}
		if (0 == count) { /* nothing left */
			s->out_n = at;
			status(s, id, WF_SFTP_EOF);
			break;
		}
		s->out[count_at] = (uint8_t)(count >> 24);
		s->out[count_at + 1] = (uint8_t)(count >> 16);
		s->out[count_at + 2] = (uint8_t)(count >> 8);
		s->out[count_at + 3] = (uint8_t)count;
		end_reply(s, at);
		break;
	}
	case WF_SFTP_LSTAT:
	case WF_SFTP_STAT:  {
		const Node *nd = node_at(str, sn);

		if (1 == sn && '/' == str[0]) {
			at = begin_reply(s, WF_SFTP_ATTRS, id);
			put_attrs(s, WF_SFTP_S_IFDIR | 0755, 0);
			end_reply(s, at);
		} else if (nd) {
			at = begin_reply(s, WF_SFTP_ATTRS, id);
			put_attrs(s, nd->mode, nd->size);
			end_reply(s, at);
		} else {
			status(s, id, WF_SFTP_NO_SUCH_FILE);
		}
		break;
	}
	case WF_SFTP_OPEN: {
		const Node *nd = node_at(str, sn);

		if (nd) {
			at = begin_reply(s, WF_SFTP_HANDLE, id);
			putstr(s,
			        sn > 5 && !memcmp(str, "/dir/", 5) ? "F2"
			                                           : "F1",
			        2);
			end_reply(s, at);
			s->open_handles++;
		} else {
			status(s, id, WF_SFTP_NO_SUCH_FILE);
		}
		break;
	}
	case WF_SFTP_READ: {
		const uint8_t *q = str + sn;
		uint64_t off =
		        (uint64_t)((uint32_t)q[0] << 24 | (uint32_t)q[1] << 16 |
		                (uint32_t)q[2] << 8 | q[3])
		                << 32 |
		        ((uint32_t)q[4] << 24 | (uint32_t)q[5] << 16 |
		                (uint32_t)q[6] << 8 | q[7]);
		uint32_t want = (uint32_t)q[8] << 24 | (uint32_t)q[9] << 16 |
		        (uint32_t)q[10] << 8 | q[11];
		uint64_t size = 'F' == str[0] && '2' == str[1] ? 70000 : 100;
		uint64_t give;

		if (off >= size) {
			status(s, id, WF_SFTP_EOF);
			break;
		}
		give = size - off < want ? size - off : want;
		if (F_LONG_DATA == s->fault) {
			give = want + 10;
		}
		at = begin_reply(s, WF_SFTP_DATA, id);
		put32(s, (uint32_t)give);
		for (uint64_t i = 0; i < give; i++) {
			s->out[s->out_n++] = content(off + i);
		}
		end_reply(s, at);
		break;
	}
	case WF_SFTP_CLOSE:
		s->closed_handles++;
		status(s, id, WF_SFTP_OK);
		break;
	case WF_SFTP_REALPATH: {
		at = begin_reply(s, WF_SFTP_NAME, id);
		put32(s, 1);
		if (sn == 3 && !memcmp(str, "/..", 3)) {
			putstr(s, "/", 1);
		} else if (sn == 4 && !memcmp(str, "bad!", 4)) {
			putstr(s, "relative", 8);
		} else {
			putstr(s, str, sn);
		}
		putstr(s, "l", 1);
		put32(s, 0);
		end_reply(s, at);
		break;
	}
	default: status(s, id, WF_SFTP_OP_UNSUPPORTED); break;
	}
}

#endif
