#include "wf_config_save_win.h"

#include <stdlib.h>
#include <string.h>

#define UTF8_CODEPAGE 65001u

WfStatus wf_config_file_open(WfConfigFile *f, const wchar_t *path)
{
	WfLoaded loaded;
	WfStatus st;

	if (!f || !path || !*path) {
		return WF_E_INVAL;
	}
	memset(f, 0, sizeof *f);
	f->path = _wcsdup(path);
	if (!f->path) {
		return WF_E_NOMEM;
	}
	st = wf_efile_load(
	        path, WF_CONFIG_MAX_BYTES, UTF8_CODEPAGE, NULL, &loaded);
	if (WF_OK != st) {
		wf_config_file_close(f);
		return st;
	}
	if (WF_FL_OK == loaded.verdict) {
		f->stamp = loaded.stamp;
	}
	/* anything else (missing, or not something to edit): nothing is
	 * remembered, so a later save either creates the file or is refused as
	 * a conflict by the exclusive create */
	wf_efile_loaded_free(&loaded);
	return WF_OK;
}

void wf_config_file_close(WfConfigFile *f)
{
	if (!f) {
		return;
	}
	free(f->path);
	memset(f, 0, sizeof *f);
}

static WfStatus fail(WfConfigSaveResult *r,
        WfConfigSaveVerdict v,
        DWORD error,
        const char *reason)
{
	r->verdict = v;
	r->error = error;
	r->reason = reason;
	return WF_OK;
}

static WfConfigSaveVerdict from_save(const WfSaveResult *s)
{
	switch (s->verdict) {
	case WF_SV_OK:                   return WF_CS_OK;
	case WF_SV_COMMITTED_UNVERIFIED: return WF_CS_COMMITTED_UNVERIFIED;
	case WF_SV_CONFLICT:
	case WF_SV_GONE:
	case WF_SV_EXISTS:               return WF_CS_CONFLICT;
	case WF_SV_UNSAFE:
	case WF_SV_ENCODE:               return WF_CS_REFUSED;
	case WF_SV_IO:                   break;
	}
	return WF_CS_IO;
}

/* "<dir>\config.ini": makes <dir> if it is missing (one level, the program's
 * own folder under %APPDATA%). */
static void make_parent(const wchar_t *path)
{
	wchar_t *dir = _wcsdup(path), *slash;

	if (!dir) {
		return;
	}
	slash = wcsrchr(dir, L'\\');
	if (slash && slash > dir) {
		*slash = L'\0';
		(void)CreateDirectoryW(dir, NULL);
	}
	free(dir);
}

WfStatus wf_config_file_save(WfConfigFile *f,
        const WfIniSet *sets,
        size_t count,
        WfConfigSaveResult *result)
{
	WfConfigSaveResult local;
	WfSaveResult saved;
	WfSaveOptions opt = {false};
	WfEncoding enc = {WF_ENC_UTF8, 0};
	WfLoaded loaded;
	char *text = NULL;
	size_t text_n = 0;
	WfStatus st;

	if (!result) {
		result = &local;
	}
	memset(result, 0, sizeof *result);
	if (!f || !f->path || (count && !sets)) {
		return WF_E_INVAL;
	}
	if (f->unverified_error) {
		return fail(result, WF_CS_COMMITTED_UNVERIFIED,
		        f->unverified_error,
		        "the updated configuration must be reopened before "
		        "saving again");
	}
	memset(&loaded, 0, sizeof loaded);
	if (f->stamp.valid) {
		st = wf_efile_load(f->path, WF_CONFIG_MAX_BYTES, UTF8_CODEPAGE,
		        NULL, &loaded);
		if (WF_OK != st) {
			return st;
		}
		if (WF_FL_MISSING == loaded.verdict) {
			wf_efile_loaded_free(&loaded);
			return fail(result, WF_CS_CONFLICT, 0,
			        "the configuration file was removed");
		}
		if (WF_FL_OK != loaded.verdict) {
			DWORD error = loaded.error;
			const char *why = loaded.reason;

			wf_efile_loaded_free(&loaded);
			return fail(result, WF_CS_REFUSED, error,
			        why ? why : "the file cannot be edited safely");
		}
		if (memcmp(loaded.stamp.sha256, f->stamp.sha256,
		            sizeof f->stamp.sha256) != 0) {
			wf_efile_loaded_free(&loaded);
			return fail(result, WF_CS_CONFLICT, 0,
			        "the configuration file changed on disk");
		}
		enc = loaded.enc;
		st = wf_config_apply(loaded.utf8, loaded.utf8_n, sets, count,
		        &text, &text_n);
		wf_efile_loaded_free(&loaded);
		if (WF_OK != st) {
			return st;
		}
		st = wf_efile_save_over(
		        f->path, &f->stamp, text, text_n, enc, &opt, &saved);
	} else {
		st = wf_config_apply("", 0, sets, count, &text, &text_n);
		if (WF_OK != st) {
			return st;
		}
		make_parent(f->path);
		st = wf_efile_save_new(f->path, text, text_n, enc, &saved);
	}
	free(text);
	if (WF_OK != st) {
		return st;
	}
	result->verdict = from_save(&saved);
	result->error = saved.error;
	result->reason = saved.reason;
	if (WF_CS_OK == result->verdict) {
		f->stamp = saved.stamp;
	} else if (WF_CS_COMMITTED_UNVERIFIED == result->verdict) {
		f->unverified_error =
		        saved.error ? saved.error : ERROR_INVALID_DATA;
	}
	return WF_OK;
}
