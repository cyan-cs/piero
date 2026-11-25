#ifndef WF_SYNLANG_H
#define WF_SYNLANG_H

#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

/*
 * The official languages (DR-DRAFT-033): the only ones that can be named, by
 * piero.exe (which language a file is, decided without loading anything) and
 * by piero-syntax.dll (which grammar may be loaded). A manifest.toml never
 * adds to this table; it must agree with it.
 */
typedef struct
{
	const char *id;    /* "json": the folder name, lower case */
	const char *entry; /* the export of parser.dll: "tree_sitter_json" */
	const char
	        *exts; /* ".json;.geojson": by the last extension; NULL: none */
	const char
	        *names; /* "tsconfig.json;...": whole file names, lower case */
	bool builtin;   /* no DLL: a lexer inside piero.exe (.env) */
	bool companion; /* never chosen by a file name: used by another language */
} WfSynLang;

#define WF_SYNLANG_COUNT 11

const WfSynLang *wf_synlang_all(size_t *n);
const WfSynLang *wf_synlang_by_id(const char *id);
/* The language of a file by its leaf name (ASCII case ignored): whole names
 * first, then the last extension. NULL when none is official. Deterministic:
 * an extension belongs to one language. */
const WfSynLang *wf_synlang_for_name(const wchar_t *leaf);

#endif
