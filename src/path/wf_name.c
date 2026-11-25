#include "wf_name.h"

#include <stdbool.h>

static bool ascii_equal(const wchar_t *s, size_t n, const wchar_t *word)
{
	size_t i;
	if (n != wcslen(word)) {
		return 0;
	}
	for (i = 0; i < n; i++) {
		wchar_t c = s[i];
		if (c >= L'a' && c <= L'z') {
			c -= L'a' - L'A';
		}
		if (c != word[i]) {
			return 0;
		}
	}
	return 1;
}

static bool invisible(uint32_t c)
{
	return 0x061c == c || 0x180e == c || (c >= 0x200b && c <= 0x200f) ||
	        (c >= 0x202a && c <= 0x202e) || (c >= 0x2060 && c <= 0x206f) ||
	        0xfeff == c;
}

WfNameResult wf_name_validate(
        const wchar_t *name, size_t units, uint32_t max_component)
{
	size_t i, stem;
	if (!name || !units) {
		return WF_NAME_EMPTY;
	}
	if (!max_component || units > max_component) {
		return WF_NAME_TOO_LONG;
	}
	if (L'.' == name[units - 1] || L' ' == name[units - 1]) {
		return WF_NAME_INVALID;
	}
	for (i = 0; i < units; i++) {
		uint32_t c = (uint16_t)name[i];
		if (c < 32 || 127 == c || (c >= 128 && c <= 159) ||
		        invisible(c) || wcschr(L"<>:\"/\\|?*", (wchar_t)c)) {
			return WF_NAME_INVALID;
		}
		if (c >= 0xd800 && c <= 0xdbff) {
			if (++i >= units || (uint16_t)name[i] < 0xdc00 ||
			        (uint16_t)name[i] > 0xdfff) {
				return WF_NAME_INVALID;
			}
		} else if (c >= 0xdc00 && c <= 0xdfff) {
			return WF_NAME_INVALID;
		}
	}
	for (stem = 0; stem < units && L'.' != name[stem]; stem++) {
	}
	/* Win32 also recognizes device stems with spaces before an extension.
	 */
	while (stem && L' ' == name[stem - 1]) {
		stem--;
	}
	if (ascii_equal(name, stem, L"CON") ||
	        ascii_equal(name, stem, L"PRN") ||
	        ascii_equal(name, stem, L"AUX") ||
	        ascii_equal(name, stem, L"NUL") ||
	        ascii_equal(name, stem, L"CONIN$") ||
	        ascii_equal(name, stem, L"CONOUT$")) {
		return WF_NAME_RESERVED;
	}
	if (4 == stem &&
	        (ascii_equal(name, 3, L"COM") ||
	                ascii_equal(name, 3, L"LPT")) &&
	        ((name[3] >= L'1' && name[3] <= L'9') || 0xb9 == name[3] ||
	                0xb2 == name[3] || 0xb3 == name[3])) {
		return WF_NAME_RESERVED;
	}
	return WF_NAME_OK;
}

static bool is_separator(wchar_t c)
{
	return L'/' == c || L'\\' == c;
}

size_t wf_name_split_first(const wchar_t *path, const wchar_t **rest)
{
	size_t n = 0;

	while (path[n] && !is_separator(path[n])) {
		n++;
	}
	*rest = path[n] ? path + n + 1 : NULL;
	return n;
}

WfNameResult wf_name_validate_path(
        const wchar_t *path, size_t units, uint32_t max_component)
{
	size_t depth = 0, start = 0;

	if (!path || !units) {
		return WF_NAME_EMPTY;
	}
	for (size_t i = 0; i <= units; i++) {
		WfNameResult r;

		if (i < units && !is_separator(path[i])) {
			continue;
		}
		if (++depth > WF_NAME_PATH_DEPTH) {
			return WF_NAME_TOO_LONG;
		}
		r = wf_name_validate(path + start, i - start, max_component);
		if (WF_NAME_OK != r) {
			return r;
		}
		start = i + 1;
	}
	return WF_NAME_OK;
}

const char *wf_name_reason(WfNameResult result)
{
	switch (result) {
	case WF_NAME_OK:       return "ok";
	case WF_NAME_EMPTY:    return "name is empty";
	case WF_NAME_TOO_LONG: return "name exceeds volume component limit";
	case WF_NAME_INVALID:
		return "name contains forbidden or invisible characters";
	case WF_NAME_RESERVED: return "reserved device name";
	}
	return "invalid name";
}
