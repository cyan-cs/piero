#ifndef P0_JOURNAL_H
#define P0_JOURNAL_H

/*
 * Proof-of-concept journal and READ-ONLY recovery scanner for P0-S02/S03.
 * This is not the production journal: it exists to test ordering (INTENT is
 * durable before any destination mutation; TEMP_ID is durable before a stopped
 * transaction may be considered for automatic cleanup). The scanner only
 * classifies; it deletes nothing.
 *
 * Record: magic(4) len(4) type(1) pad(3) payload(len) crc32(4); the CRC covers
 * type..payload. A torn or missing tail is simply not seen.
 */
#include <windows.h>

#include <stdint.h>

#define P0J_ID_BYTES 24 /* volume serial (8) + FileId (16) */

typedef enum
{
	P0J_INTENT = 1,
	P0J_TEMP_ID = 2,
	P0J_COMMITTED = 3,
	P0J_DIRECTORY_INTENT = 4
} P0JType;

#pragma pack(push, 1)
typedef struct
{
	uint8_t job[16];
	uint8_t txn[16];
	uint64_t src_size;
	wchar_t dest_parent[260];
	wchar_t name[128];
} P0Intent;

/* Everything the design says one durable TEMP_ID record must carry (7.7). */
typedef struct
{
	uint8_t txn[16];
	uint8_t txn_dir_id[P0J_ID_BYTES];
	uint8_t owner_id[P0J_ID_BYTES];
	uint8_t nonce[32];
	uint8_t payload_id[P0J_ID_BYTES];
	uint8_t parent_id[P0J_ID_BYTES];
	uint64_t volume;
	uint64_t src_size;
	wchar_t rel_payload[128]; /* ".wfile-txn-<uuid>\payload.bin" */
} P0TempId;
#pragma pack(pop)

/* This compatibility format stores Windows UTF-16, not host-wide strings. */
_Static_assert(sizeof(wchar_t) == 2, "P0 journal requires Windows UTF-16");
_Static_assert(sizeof(P0Intent) == 816, "P0 INTENT wire size");
_Static_assert(sizeof(P0TempId) == 416, "P0 TEMP_ID wire size");

typedef struct
{
	HANDLE h;
	uint64_t written;
	uint64_t flushed; /* length that has crossed a FlushFileBuffers */
} P0Journal;

/* Creates the file (must not exist); other processes may read it but not
 * write or delete it while it is open, as in design 7.7. */
int p0j_open(P0Journal *j, const wchar_t *path);
int p0j_append(P0Journal *j, P0JType type, const void *payload, uint32_t len);
int p0j_flush(P0Journal *j);
void p0j_close(P0Journal *j);

typedef struct
{
	uint8_t type;
	uint32_t len;
	const uint8_t *data;
} P0Rec;

/* Parses valid records from buf[0..n); stops at the first bad one. */
size_t p0j_parse(const uint8_t *buf, size_t n, P0Rec *out, size_t max);

uint32_t p0j_crc32(const void *p, size_t n);

typedef enum
{
	P0S_END,            /* nothing left to do */
	P0S_MANUAL,         /* a human must look; never automatic */
	P0S_AUTO_CANDIDATE, /* may proceed to the full ownership predicate for
	                       cleanup */
	P0S_ABORTED,   /* identity recorded, payload and destination both absent
	                */
	P0S_COMMITTED, /* exact final identity; verified residue or no residue
	                */
	P0S_SUSPECT    /* transaction-looking name, no journal explains it */
} P0ScanClass;

typedef struct
{
	P0ScanClass cls;
	char reason[240];
	int has_intent, has_temp_id;
	int txn_exists, payload_exists;
	int txn_names_found; /* ".wfile-txn-*" entries seen by name */
	uint8_t temp_payload_id[P0J_ID_BYTES];
	int ownership_checks_passed; /* number of predicate items that held */
	int ownership_checks_total;
} P0Scan;

/*
 * Classifies the state left in `dest_parent` by the journal bytes buf[0..n).
 * The caller decides which bytes are "visible" (the whole file, or only the
 * prefix that was flushed). Nothing is modified.
 */
void p0r_scan(
        const uint8_t *buf, size_t n, const wchar_t *dest_parent, P0Scan *out);

const char *p0r_class_name(P0ScanClass c);

/* Identity of an open handle in the journal's 24-byte form. */
int p0j_id_of_handle(HANDLE h, uint8_t out[P0J_ID_BYTES]);

#endif
