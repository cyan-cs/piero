/*
 * Settings license and project texts. The project license is embedded from
 * the root LICENSE; dependency notices are read from LICENSES\ beside the
 * executable. Returned text is owned by the caller.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../text/wf_text.h"
#include "wf_app_settings_priv.h"
#include "../base/wf_macro.h"
#include "wf_project_info.h"

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define LICENSE_MAX_BYTES (256u * 1024u)

enum
{
	ABOUT_PROJECT,
	ABOUT_LICENSE,
	ABOUT_SAFETY,
	ABOUT_COUNT
};

static bool license_folder(const WfApp *app, wchar_t *out, size_t cap)
{
	if (app->license_dir) {
		if (wcslen(app->license_dir) + 1 > cap) {
			return false;
		}
		wcscpy_s(out, cap, app->license_dir);
		return true;
	}
	{
		DWORD n = GetModuleFileNameW(NULL, out, (DWORD)cap);
		wchar_t *slash;

		if (!n || n >= cap) {
			return false;
		}
		slash = wcsrchr(out, L'\\');
		if (!slash) {
			return false;
		}
		*slash = L'\0';
		if (wcslen(out) + 10 >= cap) {
			return false;
		}
		wcscat_s(out, cap, L"\\LICENSES");
		return true;
	}
}

/* "libarchive-3.8.10.txt" -> "libarchive-3.8.10" */
static bool title_of(const wchar_t *file, char out[32])
{
	char utf8[64];
	int n = WideCharToMultiByte(
	        CP_UTF8, 0, file, -1, utf8, sizeof utf8, NULL, NULL);
	size_t len;

	if (n <= 0) {
		return false;
	}
	len = strlen(utf8);
	if (len > 4 && !_stricmp(utf8 + len - 4, ".txt")) {
		utf8[len - 4] = '\0';
	}
	if (strlen(utf8) >= 32) {
		return false;
	}
	memcpy(out, utf8, strlen(utf8) + 1);
	return true;
}

void wf_about_scan(WfApp *app)
{
	wchar_t dir[MAX_PATH + 16], pattern[MAX_PATH + 24];
	WIN32_FIND_DATAW fd;
	HANDLE h;
	int i, k;

	app->settings.lic_n = 0;
	if (!license_folder(app, dir, WF_ARRAY_LEN(dir))) {
		return;
	}
	if (swprintf(pattern, WF_ARRAY_LEN(pattern), L"%ls\\*.txt", dir) < 0) {
		return;
	}
	h = FindFirstFileW(pattern, &fd);
	if (INVALID_HANDLE_VALUE == h) {
		return;
	}
	do {
		char title[32];

		if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
		        app->settings.lic_n >= WF_LICENSE_FILES ||
		        !title_of(fd.cFileName, title)) {
			continue;
		}
		memcpy(app->settings.lic_name[app->settings.lic_n++], title,
		        sizeof title);
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	/* in name order, whatever order the folder gave */
	for (i = 1; i < app->settings.lic_n; i++) {
		for (k = i; k > 0 &&
		        _stricmp(app->settings.lic_name[k - 1],
		                app->settings.lic_name[k]) > 0;
		        k--) {
			char t[32];

			memcpy(t, app->settings.lic_name[k - 1], sizeof t);
			memcpy(app->settings.lic_name[k - 1],
			        app->settings.lic_name[k], sizeof t);
			memcpy(app->settings.lic_name[k], t, sizeof t);
		}
	}
}

size_t wf_about_count(const WfApp *app, int section)
{
	if (WF_SEC_LICENSES == section) {
		return 1 + (size_t)app->settings.lic_n;
	}
	if (WF_SEC_ABOUT == section) {
		return ABOUT_COUNT;
	}
	return 0;
}

void wf_about_title(
        const WfApp *app, int section, size_t i, char *out, size_t cap)
{
	out[0] = '\0';
	if (WF_SEC_ABOUT == section && i < ABOUT_COUNT) {
		if (i == ABOUT_SAFETY) {
			snprintf(out, cap, "%s: %s", wf_msg(MSG_RECOVERY_TITLE),
			        wf_msg(app->recovery.unknown ||
			                                app->recovery.unresolved
			                        ? MSG_SAFETY_REVIEW
			                        : MSG_SAFETY_NORMAL));
			return;
		}
		snprintf(out, cap, "%s",
		        i == ABOUT_PROJECT ? "piero"
		                           : wf_msg(MSG_ABOUT_LICENSE));
	} else if (WF_SEC_LICENSES == section && 0 == i) {
		snprintf(out, cap, "piero");
	} else if (WF_SEC_LICENSES == section &&
	        i - 1 < (size_t)app->settings.lic_n) {
		snprintf(out, cap, "%s", app->settings.lic_name[i - 1]);
	}
}

static char *dup_text(const char *s, size_t *n)
{
	size_t len = strlen(s);
	char *p = malloc(len + 1);

	if (!p) {
		return NULL;
	}
	memcpy(p, s, len + 1);
	*n = len;
	return p;
}

/* A licence file: bytes read as UTF-8, anything that is not valid becomes
 * '?', so the screen never shows broken characters. */
static char *read_license(const WfApp *app, const char *title, size_t *n)
{
	wchar_t dir[MAX_PATH + 16], path[MAX_PATH + 64], wide[40];
	HANDLE h;
	LARGE_INTEGER size;
	char *buf, *clean;
	DWORD got = 0;
	size_t i, o = 0;

	if (!license_folder(app, dir, WF_ARRAY_LEN(dir)) ||
	        MultiByteToWideChar(
	                CP_UTF8, 0, title, -1, wide, WF_ARRAY_LEN(wide)) <= 0 ||
	        swprintf(path, WF_ARRAY_LEN(path), L"%ls\\%ls.txt", dir, wide) <
	                0) {
		return dup_text(wf_msg(MSG_LIC_MISSING), n);
	}
	h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return dup_text(wf_msg(MSG_LIC_MISSING), n);
	}
	if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 ||
	        size.QuadPart > LICENSE_MAX_BYTES) {
		CloseHandle(h);
		return dup_text(wf_msg(MSG_LIC_MISSING), n);
	}
	buf = malloc((size_t)size.QuadPart + 1);
	if (!buf) {
		CloseHandle(h);
		return NULL;
	}
	if (!ReadFile(h, buf, (DWORD)size.QuadPart, &got, NULL) ||
	        got != (DWORD)size.QuadPart) {
		CloseHandle(h);
		free(buf);
		return dup_text(wf_msg(MSG_LIC_MISSING), n);
	}
	CloseHandle(h);
	clean = malloc((size_t)got + 1);
	if (!clean) {
		free(buf);
		return NULL;
	}
	for (i = 0; i < got;) {
		unsigned char c = (unsigned char)buf[i];
		uint32_t cp;
		size_t k;

		if (c < 0x80) {
			if ('\r' == c) {
				i++;
				continue;
			}
			clean[o++] = '\t' == c || '\n' == c || c >= 0x20
			        ? (char)c
			        : ' ';
			i++;
			continue;
		}
		k = wf_utf8_decode(buf + i, got - i, &cp);
		if (k > 1) {
			memcpy(clean + o, buf + i, k);
			o += k;
			i += k;
		} else {
			clean[o++] = '?';
			i++;
		}
	}
	clean[o] = '\0';
	free(buf);
	*n = o;
	return clean;
}

char *wf_about_text(const WfApp *app, int section, size_t i, size_t *n)
{
	char text[1024];

	*n = 0;
	if (WF_SEC_LICENSES == section) {
		if (0 == i) {
			return dup_text(wf_project_license, n);
		}
		if (i - 1 < (size_t)app->settings.lic_n) {
			return read_license(
			        app, app->settings.lic_name[i - 1], n);
		}
		return dup_text("", n);
	}
	if (WF_SEC_ABOUT != section) {
		return dup_text("", n);
	}
	if (i == ABOUT_SAFETY) {
		WfInfo info;
		WfBuf doc;
		wf_app_safety_info(app, &info);
		wf_buf_init(&doc);
		for (int row = 0; row < info.n; row++) {
			if (wf_buf_puts(&doc, info.text[row]) != WF_OK ||
			        wf_buf_putc(&doc, '\n') != WF_OK) {
				wf_buf_free(&doc);
				return NULL;
			}
		}
		*n = doc.len;
		return doc.data;
	}
	if (i == ABOUT_LICENSE) {
		return dup_text(wf_project_license, n);
	}
	if (i != ABOUT_PROJECT) {
		return dup_text("", n);
	}
	int len = snprintf(text, sizeof text,
	        "piero\n%s\n\n%s%s%sMIT License\n\n%s\n%s\n\n%s\n%s",
	        wf_msg(MSG_ABOUT_DESCRIPTION),
	        WF_PROJECT_URL[0] ? "GitHub: " : "", WF_PROJECT_URL,
	        WF_PROJECT_URL[0] ? "\n" : "", wf_msg(MSG_ABOUT_SUPPORT_TITLE),
	        wf_msg(MSG_ABOUT_SUPPORT_TEXT), wf_msg(MSG_ABOUT_SUPPORT_DM),
	        WF_SPONSOR_URL);
	return len >= 0 && (size_t)len < sizeof text ? dup_text(text, n) : NULL;
}
