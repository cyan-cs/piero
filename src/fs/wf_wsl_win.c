#include "wf_wsl_win.h"

#include <stdbool.h>
#include <string.h>

static bool name_ok(const wchar_t *s)
{
	size_t n = wcslen(s);

	if (n < 1 || n > 128 || L'-' == s[0]) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		wchar_t c = s[i];

		if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') ||
		            (c >= L'A' && c <= L'Z') || L'.' == c ||
		            L'_' == c || L'-' == c || L'+' == c)) {
			return false;
		}
	}
	return true;
}

size_t wf_wsl_distros(WfWslDistro *out, size_t cap)
{
	HKEY root;
	DWORD index = 0;
	size_t n = 0;

	if (!out || !cap ||
	        ERROR_SUCCESS !=
	                RegOpenKeyExW(HKEY_CURRENT_USER,
	                        L"Software\\Microsoft\\Windows\\CurrentVersion"
	                        L"\\Lxss",
	                        0, KEY_READ, &root)) {
		return 0;
	}
	while (n < cap) {
		wchar_t id[64], name[WF_WSL_NAME_MAX + 1];
		DWORD id_n = 64, name_n = sizeof name - sizeof name[0],
		      type = 0, version = 0, version_n = sizeof version;
		HKEY key;

		if (ERROR_SUCCESS !=
		        RegEnumKeyExW(root, index++, id, &id_n, NULL, NULL,
		                NULL, NULL)) {
			break;
		}
		if (ERROR_SUCCESS !=
		        RegOpenKeyExW(root, id, 0, KEY_READ, &key)) {
			continue;
		}
		memset(name, 0, sizeof name);
		if (ERROR_SUCCESS ==
		                RegQueryValueExW(key, L"DistributionName", NULL,
		                        &type, (BYTE *)name, &name_n) &&
		        REG_SZ == type && name_ok(name)) {
			if (ERROR_SUCCESS !=
			        RegQueryValueExW(key, L"Version", NULL, NULL,
			                (BYTE *)&version, &version_n)) {
				version = 0;
			}
			wcscpy_s(out[n].name, WF_WSL_NAME_MAX, name);
			out[n].version = version;
			n++;
		}
		RegCloseKey(key);
	}
	RegCloseKey(root);
	return n;
}

void wf_wsl_names_line(wchar_t *out, size_t cap)
{
	WfWslDistro d[16];
	size_t n = wf_wsl_distros(d, 16), used = 0;

	if (!out || !cap) {
		return;
	}
	out[0] = 0;
	for (size_t i = 0; i < n; i++) {
		size_t len = wcslen(d[i].name), sep = i ? 2 : 0;

		if (used + sep + len + 1 > cap) {
			break; /* whole names only */
		}
		if (sep) {
			wcscpy_s(out + used, cap - used, L", ");
		}
		wcscpy_s(out + used + sep, cap - used - sep, d[i].name);
		used += sep + len;
	}
}
