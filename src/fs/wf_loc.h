#ifndef WF_LOC_H
#define WF_LOC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#include "../base/wf_status.h"

/*
 * Where a file lives, for the three kinds of places piero can show: the
 * Windows file system, a WSL distribution, and a server reached over SFTP.
 * They are not the same thing and this header does not pretend they are:
 *
 *  - Windows names are UTF-16 and compare case-insensitively; WSL and SFTP
 *    names are BYTES (no NUL, no '/') that compare exactly. Two entries that
 *    differ only by case are two entries.
 *  - Windows has volume serial + file id; a POSIX place has (device, inode)
 *    only where the backend could read them, and often has nothing.
 *  - What may be done to a file is a set of capabilities the backend proved,
 *    not a property of the kind. The default is read-only.
 *
 * Everything here is pure data and string handling; no I/O.
 */

typedef enum
{
	WF_BK_LOCAL, /* Win32 paths (the existing code) */
	WF_BK_WSL,   /* \\wsl.localhost\<distro>\... */
	WF_BK_SFTP   /* a server over ssh.exe -s sftp */
} WfBackendKind;

/* Capabilities. A backend publishes only what it verified for the place. */
enum
{
	WF_CAP_LIST = 1u << 0,       /* list a directory */
	WF_CAP_READ = 1u << 1,       /* read a regular file */
	WF_CAP_READ_RANGE = 1u << 2, /* read part of a file (preview) */
	WF_CAP_NEW_FILE = 1u << 3,   /* create a file that must not exist */
	WF_CAP_SAVE_OVER = 1u << 4,  /* replace an existing file safely */
	WF_CAP_RENAME = 1u << 5,     /* rename inside a directory */
	WF_CAP_FSYNC = 1u << 6,      /* make written data durable */
	WF_CAP_CASE_SENSITIVE = 1u
	        << 7,               /* names differing by case are distinct */
	WF_CAP_IDENTITY = 1u << 8,  /* an object identity can be read back */
	WF_CAP_POSIX_MODE = 1u << 9 /* permission bits are reported */
};

/*
 * What a replacing save needs from a backend, each proven for this place (a
 * test against it, not the kind of place). Every one is required: a rename
 * that is atomic does not stop another writer, and a check made before a
 * rename can be outrun by one made after it.
 */
typedef struct
{
	bool staging_in_same_directory; /* a sibling can be created and owned */
	bool noclobber_publish;         /* publish fails if the name appeared */
	bool atomic_replace;            /* the old name is never absent */
	bool identity_checked_at_commit; /* the object swapped is the one read */
	bool data_durable_before_commit; /* fsync or equivalent, verified */
	bool readback_verified;          /* the staged bytes were read back */
} WfSaveProof;

/* The capabilities a kind starts with before anything is probed. Windows is
 * left to its own code, so it is "everything the old code did"; WSL and SFTP
 * are read-only. */
uint32_t wf_caps_default(WfBackendKind kind);

/* Adds WF_CAP_NEW_FILE and WF_CAP_SAVE_OVER to `caps` only when the proof is
 * complete; with any piece missing `caps` is returned unchanged, and a
 * non-Windows kind never gains rename or save-over from a partial proof. */
uint32_t wf_caps_grant_save(
        WfBackendKind kind, uint32_t caps, const WfSaveProof *proof);

static inline bool wf_cap_has(uint32_t caps, uint32_t need)
{
	return need && need == (caps & need);
}

#define WF_LOC_KEY_MAX 160

typedef struct
{
	WfBackendKind kind;
	char key[WF_LOC_KEY_MAX]; /* "" for local; "Ubuntu"; "alias:22" */
} WfLocId;

/* Builders validate; they never normalise. A WSL distribution name is 1..128
 * characters of letters, digits, '.', '_', '-', '+' and no leading '-'. An
 * SFTP alias is a name the user chose in their ssh configuration (same
 * alphabet, ':' is not allowed) with a port 1..65535 (0 means "22"). */
WfStatus wf_locid_local(WfLocId *out);
WfStatus wf_locid_wsl(WfLocId *out, const char *distro);
WfStatus wf_locid_sftp(WfLocId *out, const char *alias, unsigned port);

/* Exact, case-sensitive: "Ubuntu" and "ubuntu" are different distributions of
 * different hosts, and /home/u on two servers is two places. */
bool wf_locid_equal(const WfLocId *a, const WfLocId *b);

/* The key Navigation Memory (and any cache) uses for a folder: the place,
 * a separator, then the path bytes unchanged. Fails when `cap` is too small.
 * The result never folds case, whatever the kind. */
WfStatus wf_loc_memory_key(const WfLocId *loc,
        const uint8_t *path,
        size_t path_n,
        uint8_t *out,
        size_t cap,
        size_t *out_n);

#define WF_POSIX_NAME_MAX 255u
#define WF_POSIX_PATH_MAX 4096u

/* A directory entry name: 1..255 bytes, no NUL, no '/', not "." or "..".
 * Bytes are not required to be UTF-8, and '\\' is an ordinary byte. */
bool wf_posix_name_ok(const uint8_t *name, size_t n);

/* An absolute path made of such names: starts with '/', at most 4096 bytes,
 * no NUL, no empty component, no "." or ".." component. The client never
 * synthesises ".." (the server's realpath does); a path that has one is
 * refused rather than rewritten. "/" alone is the root. */
bool wf_posix_path_ok(const uint8_t *path, size_t n);

/* dir + '/' + name, into out (NUL-terminated for convenience, the length is
 * returned without it). Fails if either part is invalid or it does not fit.
 * "/" + "a" is "/a", not "//a". */
WfStatus wf_posix_join(const uint8_t *dir,
        size_t dir_n,
        const uint8_t *name,
        size_t name_n,
        uint8_t *out,
        size_t cap,
        size_t *out_n);

/*
 * Turns a raw name (or path) into text that is safe to put on a terminal and
 * cannot be mistaken for another name. Valid UTF-8 is kept except what could
 * act on the terminal or on the reader: C0 and C1 controls, DEL, bidirectional
 * controls, line and paragraph separators, and the backslash. Those and every
 * byte that is not part of a valid sequence are written as \xNN (bytes) or
 * \u{XXXX} (scalars); a backslash is written \\. The mapping is injective, so
 * two different names never display the same. Returns the length written
 * (without the NUL); the output is always NUL-terminated and, if `cap` is
 * too small, cut at a character boundary with *truncated set.
 */
size_t wf_name_display(
        const uint8_t *raw, size_t n, char *out, size_t cap, bool *truncated);

/* The path points into a WSL distribution: \\wsl.localhost\<distro>[\...],
 * the older \\wsl$\<distro>[\...], or either written as \\?\UNC\...; the
 * prefix is matched without regard to case because Windows does. Text only,
 * no I/O. Writing through such a path is refused by name until the place has
 * proven what a save needs (wf_caps_grant_save): the Win32 view of a
 * distribution has no file id and no case information. */
bool wf_wsl_unc_is(const wchar_t *path);

typedef enum
{
	WF_NEW_EOL_LF,
	WF_NEW_EOL_CRLF
} WfNewEol;

/* The line ending of a NEW text file (an existing file keeps its own). The
 * encoding of a new file is UTF-8 without a byte order mark everywhere. */
WfNewEol wf_new_file_eol(WfBackendKind kind);

/* Both paths are in the same WSL distribution (name compared exactly: a
 * different spelling is treated as a different distribution). */
bool wf_wsl_same_distro(const wchar_t *a, const wchar_t *b);

#endif
