#ifndef WF_LOCK_H
#define WF_LOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../base/wf_status.h"

/*
 * Who has a file open (DR-DRAFT-024). This header is the contract between the
 * screen and the process layer: plain data, the verdict of the policy, and the
 * port the screen uses to ask the lock worker. Policy data has no dependency
 * on Windows headers.
 *
 * Nothing here opens, changes or removes a file. Naming a program is not a
 * claim that it is the only reason an operation failed: an empty answer is
 * never "no lock".
 */

#define WF_LOCK_MAX_HOLDERS 32u
#define WF_LOCK_APP_CHARS 128u
#define WF_LOCK_EXE_CHARS 520u

/* How a failed operation's Win32 code is read. */
typedef enum
{
	WF_LOCK_NOT,   /* not a lock: say nothing about programs */
	WF_LOCK_MAYBE, /* access denied: can be a lock, can be much else */
	WF_LOCK_IN_USE /* sharing or lock violation, file mapped by a program */
} WfLockClass;

WfLockClass wf_lock_classify(unsigned long win32_error);

/* RM_APP_TYPE, kept as our own values so the screen needs no Windows header. */
typedef enum
{
	WF_LOCKAPP_UNKNOWN,
	WF_LOCKAPP_MAIN_WINDOW,
	WF_LOCKAPP_OTHER_WINDOW,
	WF_LOCKAPP_SERVICE,
	WF_LOCKAPP_EXPLORER,
	WF_LOCKAPP_CONSOLE,
	WF_LOCKAPP_CRITICAL
} WfLockAppType;

/* Reasons for refusing a process action. wf_lock_decide defines the check
 * order; enum order does not determine priority. */
typedef enum
{
	WF_DENY_NONE = 0,
	WF_DENY_SELF,      /* piero itself */
	WF_DENY_ANCESTOR,  /* hosts piero (shell, terminal) */
	WF_DENY_IDENTITY,  /* PID and start time could not be matched */
	WF_DENY_UNKNOWN,   /* a fact needed for the verdict could not be read */
	WF_DENY_SYSTEM,    /* the System process, or session 0 */
	WF_DENY_CRITICAL,  /* a critical process */
	WF_DENY_PROTECTED, /* a protected process */
	WF_DENY_KIND,      /* a service, Explorer, or RM's "critical" */
	WF_DENY_OTHER_USER,   /* another user's process */
	WF_DENY_SYSTEM_IMAGE, /* a core Windows component */
	WF_DENY_NO_ACCESS, /* the system would not give the right (ending only) */
	WF_DENY_NO_WINDOW /* nothing to ask (closing only) */
} WfLockDeny;

/* Everything the verdict depends on. `facts_ok` false means a read failed. */
typedef struct
{
	uint32_t pid;
	uint32_t self_pid;
	bool ancestor;
	bool identity_ok; /* RM's start time equals the process's own */
	bool facts_ok;
	bool critical;
	bool is_protected;
	uint32_t session;
	bool user_known, same_user;
	WfLockAppType type;
	bool system_image;  /* under the Windows folder, a core component */
	bool can_terminate; /* the system gave PROCESS_TERMINATE */
	bool has_window;    /* a visible top-level window of that process */
} WfProcFacts;

/* The verdict for ending (`ending` true) or asking to close (false). */
WfLockDeny wf_lock_decide(const WfProcFacts *f, bool ending);

/* True for the name of a core Windows component (leaf of an image, any case). */
bool wf_lock_core_image_name(const wchar_t *leaf);

typedef struct
{
	uint32_t pid;
	uint64_t start; /* FILETIME ticks (100 ns since 1601), as RM reported */
	bool identity_ok;
	wchar_t app[WF_LOCK_APP_CHARS]; /* RM's application name, "" if none */
	wchar_t exe[WF_LOCK_EXE_CHARS]; /* image path; "" when not readable */
	WfLockAppType type;
	bool restartable;
	bool has_window;
	uint32_t session;
	WfLockDeny close_deny; /* WF_DENY_NONE: closing may be tried */
	WfLockDeny
	        end_deny; /* WF_DENY_NONE: termination may be tried; rechecked on action */
} WfLockHolder;

typedef struct
{
	WfLockHolder *h; /* owned */
	size_t n;
	size_t more; /* programs RM named beyond WF_LOCK_MAX_HOLDERS */
} WfLockList;

void wf_locklist_free(WfLockList *l); /* leaves it empty */

/* The same process: equal PID and equal start time (both known). */
bool wf_lock_same_process(const WfLockHolder *a, const WfLockHolder *b);

typedef enum
{
	WF_LOCKREQ_QUERY,
	WF_LOCKREQ_CLOSE,
	WF_LOCKREQ_END
} WfLockReq;

typedef enum
{
	WF_LOCKRES_EXITED, /* the process is gone (verified on its handle) */
	WF_LOCKRES_STILL, /* exit not observed: timeout, cancellation or failed wait */
	WF_LOCKRES_DENIED, /* policy or identity refused it; see deny */
	WF_LOCKRES_GONE, /* it was not running any more when the action began */
	WF_LOCKRES_FAILED /* a system call failed; see error */
} WfLockActionResult;

/* The answer of the worker to one request. */
typedef struct
{
	uint32_t gen; /* the request this answers */
	WfLockReq req;
	WfStatus st;         /* query: OK, or why it could not look */
	unsigned long error; /* the Win32 code behind st / a failed action */
	WfLockList list;     /* query: owned, may be empty */
	WfLockActionResult result; /* close / end */
	WfLockDeny deny;
	uint32_t pid;
	uint64_t start;
} WfLockAnswer;

void wf_lockanswer_free(WfLockAnswer *a);

/*
 * The service the screen asks. Each call only queues work and returns; the
 * answer comes back with the same generation. WF_E_INVAL means a request is
 * already out (the screen says "still working") or the arguments are bad. A
 * zeroed port means "not available".
 */
typedef struct
{
	void *ctx;
	WfStatus (*query)(void *ctx, uint32_t gen, const wchar_t *path);
	/* pid, start and type are the row the person chose, as listed */
	WfStatus (*close)(void *ctx,
	        uint32_t gen,
	        uint32_t pid,
	        uint64_t start,
	        WfLockAppType type);
	WfStatus (*end)(void *ctx,
	        uint32_t gen,
	        uint32_t pid,
	        uint64_t start,
	        WfLockAppType type);
} WfLockPort;

#endif
