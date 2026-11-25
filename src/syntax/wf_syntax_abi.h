#ifndef WF_SYNTAX_ABI_H
#define WF_SYNTAX_ABI_H

#include <stddef.h>
#include <stdint.h>

/*
 * The boundary between piero.exe and syntax\piero-syntax.dll (DR-DRAFT-033).
 *
 * Plain C: integers, UTF-8 text and memory the DLL owns. No Tree-sitter type
 * crosses it, so the exe never includes a Tree-sitter header and a change of
 * the runtime cannot change this file. The DLL has one export,
 * `wf_syntax_api`; a host that does not know the version it answers with is
 * refused, not guessed at.
 */
#define WF_SYNTAX_HOST_ABI 1u

/* The class of a byte of text. 0..10 are the classes the editor always had
 * (WfTok keeps the same numbers); the rest are new. The colour of a class is
 * the theme's (wf_theme_styles()->syntax[class]); nothing here is a colour. */
typedef enum
{
	WF_SC_PLAIN = 0,
	WF_SC_COMMENT,
	WF_SC_STRING,
	WF_SC_ESCAPE,
	WF_SC_NUMBER,
	WF_SC_KEYWORD,
	WF_SC_TYPE,
	WF_SC_PREPROC,
	WF_SC_HEADING,
	WF_SC_KEY, /* property names */
	WF_SC_MARKUP,
	WF_SC_BOOLEAN,
	WF_SC_NULL,
	WF_SC_FUNCTION,
	WF_SC_VARIABLE,
	WF_SC_CONSTANT,
	WF_SC_OPERATOR,
	WF_SC_PUNCTUATION,
	WF_SC_LINK,
	WF_SC_ERROR,
	WF_SC_COUNT
} WfSynClass;

typedef enum
{
	WF_SYN_OK = 0,
	WF_SYN_E_NOGRAMMAR, /* not installed: plain text */
	WF_SYN_E_UNKNOWN,   /* not an official language id */
	WF_SYN_E_PATH,      /* a path that is not the one it must be */
	WF_SYN_E_TRUST, /* a file that is not the official one of this release */
	WF_SYN_E_MANIFEST, /* manifest.toml is malformed or disagrees */
	WF_SYN_E_LOAD,     /* the loader refused the DLL */
	WF_SYN_E_SYMBOL,   /* the export is missing */
	WF_SYN_E_ABI,      /* a grammar the runtime cannot use */
	WF_SYN_E_QUERY,    /* the Highlight Query does not compile */
	WF_SYN_E_LIMIT,    /* the text is over a limit */
	WF_SYN_E_TIMEOUT,  /* the parse or the query took too long */
	WF_SYN_E_NOMEM,
	WF_SYN_E_BADARG,
	WF_SYN_E_INTERNAL
} WfSynStatus;

const char *wf_syn_status_name(uint32_t status);

/* A run of one class: bytes [start, start + length) of the text that was
 * given. Runs are sorted, never overlap, and plain stretches are not listed. */
typedef struct
{
	uint32_t start, length, cls;
} WfSynRun;

typedef struct
{
	uint32_t max_text; /* bytes; more: WF_SYN_E_LIMIT */
	uint32_t max_line; /* bytes of one line; more: WF_SYN_E_LIMIT */
	uint32_t parse_ms; /* more: WF_SYN_E_TIMEOUT */
	uint32_t query_ms; /* more: WF_SYN_E_TIMEOUT */
	uint32_t max_runs; /* more: WF_SYN_E_LIMIT */
} WfSynLimits;

#define WF_SYN_DEFAULT_MAX_TEXT (2u * 1024u * 1024u)
#define WF_SYN_DEFAULT_MAX_LINE (64u * 1024u)
#define WF_SYN_DEFAULT_PARSE_MS 400u
#define WF_SYN_DEFAULT_QUERY_MS 200u
#define WF_SYN_DEFAULT_MAX_RUNS 1000000u

typedef struct
{
	uint32_t status; /* WfSynStatus */
	uint32_t nruns;
	WfSynRun *runs; /* owned by the DLL: result_free */
	uint32_t parse_us, query_us;
	uint32_t incremental; /* 1: the old tree was reused */
} WfSynResult;

typedef struct WfSynSession WfSynSession;

/* Counters that tests and the details window read. */
typedef enum
{
	WF_SYN_STAT_PARSES,   /* parses made, whole or incremental */
	WF_SYN_STAT_LOADED,   /* grammar DLLs loaded now */
	WF_SYN_STAT_SESSIONS, /* sessions open now */
	WF_SYN_STAT_QUERIES   /* queries run */
} WfSynStat;

typedef struct
{
	uint32_t abi;  /* WF_SYNTAX_HOST_ABI */
	uint32_t size; /* sizeof this table */
	const char *(*version)(void);
	/* A session for one document of the language `lang_id` ("json"...). The
	 * grammar is loaded here, once for the process. */
	uint32_t (*open)(const char *lang_id,
	        const WfSynLimits *limits,
	        WfSynSession **out);
	/* Parses `text` (reusing the tree of the last call by the one edit that
	 * turns the last text into this one) and returns the runs of classes. */
	uint32_t (*highlight)(WfSynSession *s,
	        const char *text,
	        size_t len,
	        WfSynResult *out);
	void (*result_free)(WfSynResult *r);
	void (*close)(WfSynSession *s);
	uint64_t (*stat)(uint32_t which);
	/* Releases every grammar. Every session must be closed first. */
	void (*shutdown)(void);
} WfSyntaxApi;

/* The one export. */
typedef uint32_t (*WfSyntaxApiFn)(uint32_t host_abi, const WfSyntaxApi **out);
#define WF_SYNTAX_EXPORT "wf_syntax_api"

#endif
