#include "wf_state_dir.h"

#include <stdlib.h>
#include <string.h>

typedef enum
{
	PROBE_ABSENT,
	PROBE_FOLDER,
	PROBE_ERROR
} Probe;

static const wchar_t NEW_NAME[] = L"piero";
static const wchar_t OLD_NAME[] = L"wfile";

/* Classifies the final entry at base\name, rejecting files and reparse points.
 * This does not validate or pin the ancestors of base. */
static Probe probe(const wchar_t *base, const wchar_t *name, DWORD *error)
{
	size_t n = wcslen(base), m = wcslen(name);
	wchar_t *path;
	DWORD attributes;

	if (n > 32000u) {
		*error = ERROR_BUFFER_OVERFLOW;
		return PROBE_ERROR;
	}
	path = malloc((n + m + 2) * sizeof *path);
	if (!path) {
		*error = ERROR_NOT_ENOUGH_MEMORY;
		return PROBE_ERROR;
	}
	memcpy(path, base, n * sizeof *path);
	if (n && L'\\' != path[n - 1]) {
		path[n++] = L'\\';
	}
	memcpy(path + n, name, (m + 1) * sizeof *path);
	attributes = GetFileAttributesW(path);
	if (INVALID_FILE_ATTRIBUTES == attributes) {
		*error = GetLastError();
		free(path);
		if (ERROR_FILE_NOT_FOUND == *error ||
		        ERROR_PATH_NOT_FOUND == *error) {
			*error = 0;
			return PROBE_ABSENT;
		}
		return PROBE_ERROR;
	}
	free(path);
	if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
		*error = ERROR_REPARSE_POINT_ENCOUNTERED;
		return PROBE_ERROR;
	}
	if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
		*error = ERROR_DIRECTORY;
		return PROBE_ERROR;
	}
	return PROBE_FOLDER;
}

WfStatus wf_state_classify(const wchar_t *base, WfStateKind *kind, DWORD *error)
{
	DWORD ignored = 0;
	Probe fresh, old;

	if (!error) {
		error = &ignored;
	}
	*error = 0;
	if (!base || !kind || L'\0' == base[0]) {
		return WF_E_INVAL;
	}
	fresh = probe(base, NEW_NAME, error);
	if (PROBE_ERROR == fresh) {
		return WF_E_OS;
	}
	old = probe(base, OLD_NAME, error);
	if (PROBE_ERROR == old) {
		return WF_E_OS;
	}
	if (PROBE_FOLDER == fresh) {
		*kind = PROBE_FOLDER == old ? WF_STATE_BOTH : WF_STATE_PIERO;
	} else {
		*kind = PROBE_FOLDER == old ? WF_STATE_LEGACY : WF_STATE_FRESH;
	}
	return WF_OK;
}

const wchar_t *wf_state_name(WfStateKind kind)
{
	return WF_STATE_LEGACY == kind ? OLD_NAME : NEW_NAME;
}

const wchar_t *wf_state_legacy_name(void)
{
	return OLD_NAME;
}
