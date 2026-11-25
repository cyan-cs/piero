#include "wf_restart_manager_win.h"

#include <restartmanager.h>

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define RM_TRIES 4u

static volatile LONG g_open_sessions;

long wf_rm_open_sessions(void)
{
	return (long)g_open_sessions;
}

static WfLockAppType app_type_of(RM_APP_TYPE t)
{
	switch (t) {
	case RmMainWindow:  return WF_LOCKAPP_MAIN_WINDOW;
	case RmOtherWindow: return WF_LOCKAPP_OTHER_WINDOW;
	case RmService:     return WF_LOCKAPP_SERVICE;
	case RmExplorer:    return WF_LOCKAPP_EXPLORER;
	case RmConsole:     return WF_LOCKAPP_CONSOLE;
	case RmCritical:    return WF_LOCKAPP_CRITICAL;
	case RmUnknownApp:  break;
	}
	return WF_LOCKAPP_UNKNOWN;
}

/* A drive path of the form X:\... that RM can take. */
static WfStatus check_path(const wchar_t *path)
{
	DWORD attr;

	if (!path ||
	        !((path[0] >= L'A' && path[0] <= L'Z') ||
	                (path[0] >= L'a' && path[0] <= L'z')) ||
	        L':' != path[1] || L'\\' != path[2]) {
		return WF_E_INVAL;
	}
	if (wcslen(path) >= MAX_PATH) {
		return WF_E_RANGE;
	}
	attr = GetFileAttributesW(path);
	if (INVALID_FILE_ATTRIBUTES == attr) {
		DWORD e = GetLastError();

		return ERROR_FILE_NOT_FOUND == e || ERROR_PATH_NOT_FOUND == e
		        ? WF_E_NOENT
		        : WF_E_OS;
	}
	if (attr & FILE_ATTRIBUTE_DIRECTORY) {
		return WF_E_INVAL; /* RM is for files */
	}
	return WF_OK;
}

/* RmGetList with the buffer RM asks for, a few times (the list can grow
 * between the two calls). */
static WfStatus get_list(DWORD session,
        RM_PROCESS_INFO **info,
        UINT *count,
        unsigned long *error)
{
	UINT have = 0, needed = 0;
	DWORD reasons = 0;
	RM_PROCESS_INFO *buf = NULL;

	*info = NULL;
	*count = 0;
	for (unsigned t = 0; t < RM_TRIES; t++) {
		DWORD r = RmGetList(session, &needed, &have, buf, &reasons);

		if (ERROR_SUCCESS == r) {
			*info = buf;
			*count = have;
			return WF_OK;
		}
		if (ERROR_MORE_DATA != r) {
			free(buf);
			*error = r;
			return WF_E_OS;
		}
		free(buf);
		buf = NULL;
		have = needed +
		        4; /* a little room for one that just opened it */
		buf = calloc(have, sizeof *buf);
		if (!buf) {
			return WF_E_NOMEM;
		}
	}
	free(buf);
	*error = ERROR_MORE_DATA;
	return WF_E_OS;
}

static bool listed(const WfLockList *l, uint32_t pid, uint64_t start)
{
	for (size_t i = 0; i < l->n; i++) {
		if (l->h[i].pid == pid && l->h[i].start == start) {
			return true;
		}
	}
	return false;
}

WfStatus wf_rm_query(const wchar_t *path,
        const WfProcEnv *env,
        WfLockList *out,
        unsigned long *error)
{
	DWORD session = 0, r;
	wchar_t key[CCH_RM_SESSION_KEY + 1];
	RM_PROCESS_INFO *info = NULL;
	UINT count = 0;
	WfStatus st;
	WfProcSnap snap = {0}; /* the process list, read once for all holders */
	unsigned long scratch = 0;

	if (!out) {
		return WF_E_INVAL;
	}
	memset(out, 0, sizeof *out);
	if (!error) {
		error = &scratch;
	}
	*error = 0;
	st = check_path(path);
	if (WF_OK != st) {
		if (WF_E_OS == st) {
			*error = GetLastError();
		}
		return st;
	}
	memset(key, 0, sizeof key);
	r = RmStartSession(&session, 0, key);
	if (ERROR_SUCCESS != r) {
		*error = r;
		return WF_E_OS;
	}
	InterlockedIncrement(&g_open_sessions);
	r = RmRegisterResources(session, 1, &path, 0, NULL, 0, NULL);
	if (ERROR_SUCCESS != r) {
		*error = r;
		st = WF_E_OS;
	} else {
		st = get_list(session, &info, &count, error);
	}
	(void)RmEndSession(session);
	InterlockedDecrement(&g_open_sessions);
	if (WF_OK != st) {
		return st;
	}
	if (count) {
		size_t keep = count < WF_LOCK_MAX_HOLDERS ? count
		                                          : WF_LOCK_MAX_HOLDERS;

		out->h = calloc(keep, sizeof *out->h);
		if (!out->h) {
			free(info);
			return WF_E_NOMEM;
		}
	}
	for (UINT i = 0; i < count; i++) {
		const RM_PROCESS_INFO *p = &info[i];
		uint64_t start = wf_proc_ft64(p->Process.ProcessStartTime);
		WfLockHolder *h;

		if (listed(out, p->Process.dwProcessId, start)) {
			continue;
		}
		if (out->n >= WF_LOCK_MAX_HOLDERS) {
			out->more++;
			continue;
		}
		h = &out->h[out->n++];
		h->pid = p->Process.dwProcessId;
		h->start = start;
		h->type = app_type_of(p->ApplicationType);
		h->restartable = 0 != p->bRestartable;
		wcsncpy_s(h->app, WF_LOCK_APP_CHARS, p->strAppName, _TRUNCATE);
		wf_proc_describe_with(h, env, &snap);
	}
	wf_proc_snap_free(&snap);
	free(info);
	return WF_OK;
}
