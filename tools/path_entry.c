#include "path_entry.h"
#include <windows.h>
#include <stdlib.h>
#include <string.h>

bool piero_path_contains(const wchar_t *path, const wchar_t *directory)
{
	const wchar_t *p = path;
	size_t dn = wcslen(directory);
	while (dn > 3 &&
	        (directory[dn - 1] == '\\' || directory[dn - 1] == '/')) {
		dn--;
	}
	while (*p) {
		const wchar_t *end = wcschr(p, ';');
		size_t n = end ? (size_t)(end - p) : wcslen(p);
		if (n >= 2 && p[0] == '"' && p[n - 1] == '"') {
			p++;
			n -= 2;
		}
		if (n < 32767) {
			wchar_t *part = malloc((n + 1) * sizeof *part);
			if (!part) {
				return true; /* refuse a modification on allocation failure */
			}
			memcpy(part, p, n * sizeof *part);
			part[n] = 0;
			DWORD needed = ExpandEnvironmentStringsW(part, NULL, 0);
			if (!needed || needed > 32768) {
				free(part);
				return true;
			}
			wchar_t *value = malloc((size_t)needed * sizeof *value);
			if (!value) {
				free(part);
				return true;
			}
			if (ExpandEnvironmentStringsW(part, value, needed) !=
			        needed) {
				free(value);
				free(part);
				return true;
			}
			free(part);
			size_t length = wcslen(value);
			for (size_t i = 0; i < length; i++) {
				if (value[i] == '/') {
					value[i] = '\\';
				}
			}
			while (length > 3 &&
			        (value[length - 1] == '\\' ||
			                value[length - 1] == '/')) {
				length--;
			}
			int comparison = length == dn
			        ? CompareStringOrdinal(value, (int)length,
			                  directory, (int)dn, TRUE)
			        : CSTR_LESS_THAN;
			free(value);
			if (!comparison || comparison == CSTR_EQUAL) {
				return true;
			}
		}
		if (!end) {
			break;
		}
		p = end + 1;
	}
	return false;
}
wchar_t *piero_path_append(const wchar_t *before, const wchar_t *directory)
{
	size_t a = wcslen(before), b = wcslen(directory);
	bool separator = a && before[a - 1] != ';';
	if (!b || wcschr(directory, ';') || wcschr(directory, '"') ||
	        a + b + 2 > 32767 || piero_path_contains(before, directory)) {
		return NULL;
	}
	wchar_t *out = malloc((a + b + 2) * sizeof *out);
	if (!out) {
		return NULL;
	}
	memcpy(out, before, a * sizeof *out);
	if (separator) {
		out[a++] = ';';
	}
	memcpy(out + a, directory, (b + 1) * sizeof *out);
	return out;
}
bool piero_path_can_restore(const wchar_t *current,
        const wchar_t *snapshot,
        const wchar_t *before,
        const wchar_t *directory)
{
	wchar_t *expected = piero_path_append(before, directory);
	bool ok = expected && !wcscmp(current, snapshot) &&
	        !wcscmp(expected, snapshot);
	free(expected);
	return ok;
}
