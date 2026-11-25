#ifndef WF_DIAG_FORMAT_H
#define WF_DIAG_FORMAT_H

#include <windows.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define WF_DIAG_FILE_BYTES (1024u * 1024u)
#define WF_DIAG_SLOTS 3u
#define WF_DIAG_PATH_UNITS 32767u
#define WF_DIAG_MARKER_NAME L".wfile-log-root"

#pragma pack(push, 1)
typedef struct
{
	uint8_t magic[8]; /* WFDROOT1 */
	FILE_ID_INFO root, marker;
	uint8_t nonce[32];
	uint32_t path_units, crc32;
} WfDiagDiskMarker;

typedef struct
{
	uint8_t magic[8]; /* WFDLOG01 */
	FILE_ID_INFO root, marker, object;
	uint8_t root_nonce[32], object_nonce[32];
	uint64_t generation;
	uint32_t slot, path_units, crc32, reserved;
} WfDiagDiskHeader;

typedef struct
{
	uint32_t bytes, crc32;
} WfDiagDiskRecord;
#pragma pack(pop)

_Static_assert(
        sizeof(wchar_t) == 2, "diagnostics disk paths use Windows UTF-16");
_Static_assert(
        sizeof(WfDiagDiskMarker) == 96, "diagnostics marker format changed");
_Static_assert(
        sizeof(WfDiagDiskHeader) == 168, "diagnostics header format changed");
_Static_assert(
        sizeof(WfDiagDiskRecord) == 8, "diagnostics record format changed");

/* Header/marker are followed by path_units UTF-16 code units, with no NUL.
 * Header CRC covers the entire fixed header with crc32=0 then those units.
 * Each remaining record is prefix + exactly bytes UTF-8 bytes including LF.
 * CRC32 is IEEE 802.3, initial/final XOR 0xffffffff. No trailing bytes.
 * Slot filenames are exactly log0.wfl / log1.wfl / log2.wfl, with
 * slot == (generation-1)%3. The canonical root path + slot binds exact path.
 * These helpers provide representation checks, never deletion authority. */
static inline uint32_t wf_diag_crc_update(
        uint32_t crc, const void *bytes, size_t n)
{
	const unsigned char *p = bytes;
	for (size_t i = 0; i < n; i++) {
		crc ^= p[i];
		for (unsigned bit = 0; bit < 8; bit++) {
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
		}
	}
	return crc;
}
static inline uint32_t wf_diag_crc32(const void *bytes, size_t n)
{
	return wf_diag_crc_update(0xffffffffu, bytes, n) ^ 0xffffffffu;
}
static inline uint32_t wf_diag_marker_crc(
        const WfDiagDiskMarker *header, const wchar_t *path)
{
	WfDiagDiskMarker copy = *header;
	copy.crc32 = 0;
	return wf_diag_crc_update(
	               wf_diag_crc_update(0xffffffffu, &copy, sizeof copy),
	               path, (size_t)copy.path_units * sizeof *path) ^
	        0xffffffffu;
}
static inline uint32_t wf_diag_header_crc(
        const WfDiagDiskHeader *header, const wchar_t *path)
{
	WfDiagDiskHeader copy = *header;
	copy.crc32 = 0;
	return wf_diag_crc_update(
	               wf_diag_crc_update(0xffffffffu, &copy, sizeof copy),
	               path, (size_t)copy.path_units * sizeof *path) ^
	        0xffffffffu;
}
static inline bool wf_diag_records_valid(const void *bytes, size_t size)
{
	const unsigned char *p = bytes;
	while (size) {
		WfDiagDiskRecord record;
		if (size < sizeof record) {
			return 0;
		}
		memcpy(&record, p, sizeof record);
		p += sizeof record;
		size -= sizeof record;
		if (!record.bytes || record.bytes > size ||
		        '\n' != p[record.bytes - 1] ||
		        wf_diag_crc32(p, record.bytes) != record.crc32) {
			return 0;
		}
		p += record.bytes;
		size -= record.bytes;
	}
	return 1;
}

#endif
