#ifndef WF_MANIFEST_H
#define WF_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wf_synlang.h"

/*
 * manifest.toml of a grammar (DR-DRAFT-033). It describes; it is never trusted
 * (it sits next to the DLL it describes) and never adds a language: it must
 * agree with the official table. Only this shape is read, and anything else is
 * refused rather than skipped:
 *
 *   key = "text"        key = 15        key = [".a", ".b"]        # comment
 *
 * Keys: id version abi library entrypoint extensions query release (all
 * required, none twice, no others). Text is ASCII without a backslash, at most
 * 63 bytes; a file name has no separator, `..` or drive; a file is at most
 * 4 KiB; at most 32 extensions of at most 15 bytes.
 */
#define WF_MANIFEST_MAX_BYTES 4096u
#define WF_MANIFEST_MAX_EXTS 32u

typedef enum
{
	WF_MF_OK = 0,
	WF_MF_E_SIZE,     /* empty or over the file limit */
	WF_MF_E_ENCODING, /* a NUL, a control character or a byte over 0x7e */
	WF_MF_E_SYNTAX,
	WF_MF_E_DUPLICATE,
	WF_MF_E_UNKNOWN_KEY,
	WF_MF_E_LENGTH,
	WF_MF_E_RANGE, /* a number or a list over its limit */
	WF_MF_E_FILENAME,
	WF_MF_E_MISSING,
	WF_MF_E_DISAGREES /* wf_manifest_agrees only */
} WfMfError;

typedef struct
{
	char id[64], version[64], release[64];
	char library[64], entrypoint[64], query[64];
	uint32_t abi;
	char exts[WF_MANIFEST_MAX_EXTS][16];
	size_t nexts;
} WfManifest;

/* Parses `text` (n bytes, not necessarily NUL-terminated). On failure `*line`
 * (when given) is the 1-based line of the fault, 0 when it is not a line. */
WfMfError wf_manifest_parse(
        const char *text, size_t n, WfManifest *out, unsigned *line);

/* WF_MF_OK when the manifest says what the table says of its language:
 * id, entrypoint, the files "parser.dll" and "highlights.scm" and the
 * extensions (the same set). */
WfMfError wf_manifest_agrees(const WfManifest *m, const WfSynLang *lang);

const char *wf_manifest_error_name(WfMfError e);

#endif
