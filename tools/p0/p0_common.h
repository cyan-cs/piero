#ifndef P0_COMMON_H
#define P0_COMMON_H

/*
 * Shared plumbing for the P0 evidence collectors (test plan section 13).
 * These programs are measurement tools, not production architecture: they
 * are allowed fixture-style filesystem cleanup that src/ is not.
 */
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define P0_HASH_HEX 65

/* "2026-10-20T12:34:56Z" */
void p0_utc_iso(char out[24]);
/* "20261020T123456Z" (filesystem-safe) */
void p0_utc_compact(char out[20]);

/* Creates every missing directory in `path`. */
int p0_mkdirs(const wchar_t *path);

/* Streaming SHA-256 (BCrypt). */
typedef struct P0Hash P0Hash;
P0Hash *p0_hash_begin(void);
void p0_hash_update(P0Hash *h, const void *p, size_t n);
void p0_hash_end(P0Hash *h, char hex[P0_HASH_HEX]); /* frees h */
int p0_sha256_file(const wchar_t *path, char hex[P0_HASH_HEX]);
/* Polling a live save must not block its DELETE-access pin/rename. The digest
 * belongs to the opened object; the pathname may be replaced during the read.
 * This is an observation, not a stable-source integrity proof. */
int p0_sha256_observe(const wchar_t *path, char hex[P0_HASH_HEX]);

/*
 * A test that writes large files must know the disk can take them before it
 * starts: a disk filled by an earlier killed run also starves the system's
 * page file. True when the volume of `dir` has at least `fixtures` bytes free
 * in addition to a fixed reserve (4 GiB) left for the system. Says why on
 * stderr when it is not, and the test must then stop without writing.
 */
int p0_free_space_for(const wchar_t *dir, unsigned long long fixtures);

/* Writes a JSON string literal, UTF-8, escaped. */
void p0_json_wstr(FILE *f, const wchar_t *s);
void p0_json_str(FILE *f, const char *s);

/*
 * environment.json: OS build, CPU, memory, elevation, the volume that holds
 * `target_path` (filesystem, flags, free space), and the harness binary's
 * SHA-256. No user or machine names are recorded.
 */
int p0_write_environment(const wchar_t *file,
        const wchar_t *target_path,
        const char *harness_name,
        const char *harness_version);

/* One-line OS description for logs, e.g. "Windows 11 Pro 25H2
 * build 10.0.26200.1234". */
void p0_os_summary(char *out, size_t cap);

/*
 * run.json (test plan section 13). `result` is "PASS", "FAIL" or "BLOCKED".
 * `hypotheses` / `decisions` are comma-free arrays of strings terminated by
 * NULL. git_commit comes from the P0_GIT_COMMIT environment variable that
 * run_p0.ps1 sets ("unknown" otherwise).
 */
int p0_write_run_json(const wchar_t *file,
        const char *test_id,
        const char *run_id,
        const char *build_id,
        const char *filesystem,
        const char *const *hypotheses,
        const char *result,
        const char *const *decisions,
        const char *notes);

/* Opens a UTF-8 file for writing; NULL on failure. */
FILE *p0_fopen_w(const wchar_t *path);

#endif
