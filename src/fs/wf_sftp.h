#ifndef WF_SFTP_H
#define WF_SFTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../base/wf_status.h"

/*
 * SFTP version 3 (draft-ietf-secsh-filexfer-02) packets: building requests
 * and checking replies. Nothing here does I/O; the bytes come from and go to
 * a pipe that something else owns. Every length a server sends is untrusted:
 * it is checked against the bytes that really arrived and against a fixed
 * limit before it is used, and a reply that does not fit is refused, never
 * trimmed to fit.
 *
 * Only what read-only browsing and preview need is here: init, opendir,
 * readdir, close, lstat/stat, open for reading, read, realpath, readlink.
 */

#define WF_SFTP_MAX_PACKET 262144u /* payload + header, a bound on any reply */
#define WF_SFTP_MAX_HANDLE 256u
#define WF_SFTP_MAX_NAMES 8192u /* entries in one NAME reply */

enum
{
	WF_SFTP_INIT = 1,
	WF_SFTP_VERSION = 2,
	WF_SFTP_OPEN = 3,
	WF_SFTP_CLOSE = 4,
	WF_SFTP_READ = 5,
	WF_SFTP_LSTAT = 7,
	WF_SFTP_OPENDIR = 11,
	WF_SFTP_READDIR = 12,
	WF_SFTP_STAT = 17,
	WF_SFTP_REALPATH = 16,
	WF_SFTP_READLINK = 19,
	WF_SFTP_STATUS = 101,
	WF_SFTP_HANDLE = 102,
	WF_SFTP_DATA = 103,
	WF_SFTP_NAME = 104,
	WF_SFTP_ATTRS = 105
};

enum
{
	WF_SFTP_OK = 0,
	WF_SFTP_EOF = 1,
	WF_SFTP_NO_SUCH_FILE = 2,
	WF_SFTP_PERMISSION_DENIED = 3,
	WF_SFTP_FAILURE = 4,
	WF_SFTP_BAD_MESSAGE = 5,
	WF_SFTP_NO_CONNECTION = 6,
	WF_SFTP_CONNECTION_LOST = 7,
	WF_SFTP_OP_UNSUPPORTED = 8
};

/* Request builders return the packet length, or 0 if it does not fit. */

size_t wf_sftp_build_init(uint8_t *out, size_t cap);
/* LSTAT, STAT, OPENDIR, OPEN(read-only), REALPATH, READLINK: a path request.
 * `type` must be one of those. */
size_t wf_sftp_build_path(uint8_t *out,
        size_t cap,
        uint8_t type,
        uint32_t id,
        const uint8_t *path,
        size_t path_n);
/* READDIR and CLOSE: a handle request. */
size_t wf_sftp_build_handle(uint8_t *out,
        size_t cap,
        uint8_t type,
        uint32_t id,
        const uint8_t *handle,
        size_t handle_n);
size_t wf_sftp_build_read(uint8_t *out,
        size_t cap,
        uint32_t id,
        const uint8_t *handle,
        size_t handle_n,
        uint64_t offset,
        uint32_t len);

/* Looks at the start of a byte stream. WF_OK: a whole packet is there and
 * *total is its length including the 4-byte length field. WF_E_RANGE: more
 * bytes are needed (*total is 0 until the length is known). WF_E_CORRUPT: the
 * length is zero or above WF_SFTP_MAX_PACKET; the stream cannot be trusted
 * any more and the session must be dropped. */
WfStatus wf_sftp_frame(const uint8_t *buf, size_t n, size_t *total);

typedef struct
{
	bool has_size, has_perm, has_time;
	uint64_t size;
	uint32_t perm;  /* st_mode: type bits and permissions */
	uint32_t mtime; /* seconds since 1970, whole seconds only (v3) */
} WfSftpAttrs;

/* The file type bits of a st_mode, as the server reported them. */
enum
{
	WF_SFTP_S_IFMT = 0170000,
	WF_SFTP_S_IFDIR = 0040000,
	WF_SFTP_S_IFREG = 0100000,
	WF_SFTP_S_IFLNK = 0120000,
	WF_SFTP_S_IFCHR = 0020000,
	WF_SFTP_S_IFBLK = 0060000,
	WF_SFTP_S_IFIFO = 0010000,
	WF_SFTP_S_IFSOCK = 0140000
};

typedef struct
{
	uint8_t type;
	uint32_t id;           /* 0 for VERSION */
	uint32_t status;       /* STATUS */
	const uint8_t *handle; /* HANDLE: points into the packet */
	size_t handle_n;
	const uint8_t *data; /* DATA */
	size_t data_n;
	WfSftpAttrs attrs; /* ATTRS */
	/* NAME: iterate with wf_sftp_name_next */
	uint32_t names;
	const uint8_t *cursor;
	size_t left;
	/* VERSION */
	uint32_t version;
	bool ext_posix_rename; /* posix-rename@openssh.com "1" */
	bool ext_fsync;        /* fsync@openssh.com "1" */
} WfSftpMsg;

/* Parses one whole packet (`pkt` is the bytes after the length field, `n`
 * their count). WF_E_CORRUPT for anything malformed: a short field, a string
 * longer than the packet, trailing bytes, a type that is not a reply, an
 * oversized handle. NAME entries are checked lazily by the name readers.
 * The message points into `pkt`. */
WfStatus wf_sftp_parse(const uint8_t *pkt, size_t n, WfSftpMsg *m);

typedef struct
{
	const uint8_t *name; /* a view into the packet; not NUL-terminated */
	size_t name_n;
	WfSftpAttrs attrs;
} WfSftpName;

/* Next entry of a NAME reply, "." and ".." skipped. WF_OK with *got = false at
 * the end. A name that is not a valid entry name (empty, contains '/' or NUL,
 * longer than 255 bytes) or whose attributes do not parse is WF_E_CORRUPT. */
WfStatus wf_sftp_name_next(WfSftpMsg *m, WfSftpName *e, bool *got);

/* The first name of a NAME reply as the server wrote it, with no check of its
 * shape (a REALPATH answer is a whole path, which wf_sftp_name_next would
 * refuse as an entry name). Requires exactly one complete entry and no trailing
 * bytes; WF_E_CORRUPT clears both outputs. The caller validates the path. */
WfStatus wf_sftp_name_first(
        const WfSftpMsg *m, const uint8_t **name, size_t *n);

/* The reply to a request, reduced to what a caller may rely on. */
const char *wf_sftp_status_text(uint32_t status);

#endif
