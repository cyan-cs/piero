#ifndef WF_STATUS_H
#define WF_STATUS_H

#include <stdbool.h>

/* Shared status codes. APIs that expose Win32 error details carry them
 * separately from this enum. */
typedef enum
{
	WF_OK = 0,
	WF_E_NOMEM,
	WF_E_INVAL,
	WF_E_RANGE,  /* size arithmetic would overflow, or value out of contract
	              */
	WF_E_ACCESS, /* permission denied */
	WF_E_NOENT,  /* path does not exist */
	WF_E_OS,
	WF_E_TIMEOUT,
	WF_E_FORMAT,    /* unsupported or invalid format */
	WF_E_CORRUPT,   /* damaged or inconsistent data */
	WF_E_ENCRYPTED, /* a password is required */
	WF_E_CHANGED,   /* the file no longer matches the observed state */
	WF_E_CANCELLED  /* cancellation requested, including superseded work */
} WfStatus;

const char *wf_status_name(WfStatus s);

#endif
