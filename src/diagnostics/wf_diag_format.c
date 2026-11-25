#include <stdio.h>

#include "../text/wf_text.h"
#include "wf_diag_internal.h"

bool wf_diag_format_event(
        WfDiagnostics *log, const WfDiagEvent *event, WfBuf *text, DWORD *error)
{
	static const char *const levels[] = {
	        "error", "warning", "info", "debug"};
	static const char *const stages[] = {"startup", "config", "list",
	        "preview", "copy", "move", "open", "rename", "create",
	        "recovery"};
	static const char *const results[] = {
	        "begin", "ok", "failed", "refused", "canceled"};
	SYSTEMTIME now;
	char line[256];
	GetSystemTime(&now);
	int n = snprintf(line, sizeof line,
	        "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ level=%s op=%llu stage=%s "
	        "result=%s win32=%lu hresult=0x%08lx",
	        (unsigned)now.wYear, (unsigned)now.wMonth, (unsigned)now.wDay,
	        (unsigned)now.wHour, (unsigned)now.wMinute,
	        (unsigned)now.wSecond, (unsigned)now.wMilliseconds,
	        levels[event->level], (unsigned long long)event->operation_id,
	        stages[event->stage], results[event->result],
	        event->win32_error, (unsigned long)(uint32_t)event->hresult);
	if (n < 0 || (size_t)n >= sizeof line) {
		*error = ERROR_INVALID_DATA;
		return 0;
	}
	if (WF_OK != wf_buf_append(text, line, (size_t)n)) {
		goto nomem;
	}
	if (log->paths && event->path) {
		size_t units = wcsnlen(event->path, WF_DIAG_PATH_UNITS + 1);
		if (units > WF_DIAG_PATH_UNITS) {
			*error = ERROR_FILENAME_EXCED_RANGE;
			return 0;
		}
		if (WF_OK != wf_buf_puts(text, " path=") ||
		        WF_OK != wf_sanitize_utf16(event->path, units, text)) {
			goto nomem;
		}
	}
	if (WF_OK != wf_buf_putc(text, '\n')) {
		goto nomem;
	}
	return 1;
nomem:
	*error = ERROR_NOT_ENOUGH_MEMORY;
	return 0;
}
