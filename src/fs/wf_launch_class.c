#include <wchar.h>

#include "wf_launch.h"
#include "../base/wf_macro.h"

/*
 * Opening hands the file to whatever program Windows associates with its
 * type. Only extensions in QUIET bypass confirmation; missing or unknown
 * extensions require it. Associations are configurable, so this list is a
 * confirmation policy, not proof that the associated program is a viewer.
 * Extensions are stored in lower case and compared case-insensitively.
 */
static const wchar_t *const QUIET[] = {
        L"txt", L"md", L"markdown", L"log", L"csv", L"tsv", L"json", L"xml",
        L"yaml", L"yml", L"ini", L"toml", L"cfg", L"conf", L"rtf", L"nfo",
        L"pdf", L"docx", L"xlsx", L"pptx", L"odt", L"ods", L"odp", L"epub",
        L"png", L"jpg", L"jpeg", L"gif", L"bmp", L"webp", L"ico", L"tif",
        L"tiff", L"heic", L"avif",
        L"mp3", L"wav", L"flac", L"ogg", L"m4a", L"aac", L"opus", L"mp4",
        L"mkv", L"avi", L"mov", L"webm", L"wmv",
        L"zip", L"7z", L"rar", L"gz", L"tgz", L"bz2", L"xz",
        L"c", L"h", L"cpp", L"hpp", L"cc", L"cxx", L"hxx", L"rs", L"go", L"cs",
        L"java", L"kt", L"swift", L"sql", L"css", L"tex", L"bib"};

static wchar_t fold(wchar_t c)
{
	return c >= L'A' && c <= L'Z' ? (wchar_t)(c + (L'a' - L'A')) : c;
}

static bool same_extension(const wchar_t *ext, size_t n, const wchar_t *known)
{
	size_t i;

	if (wcslen(known) != n) {
		return false;
	}
	for (i = 0; i < n; i++) {
		if (fold(ext[i]) != known[i]) {
			return false;
		}
	}
	return true;
}

bool wf_launch_needs_confirm(const wchar_t *name)
{
	size_t len, end, dot, i;

	if (!name) {
		return true;
	}
	len = wcslen(name);
	end = len;
	while (end > 0 && (L'.' == name[end - 1] || L' ' == name[end - 1])) {
		end--;
	}
	if (L'\0' == name[0] || 0 == end) {
		return true;
	}
	for (dot = end; dot > 0 && L'.' != name[dot - 1]; dot--) {
	}
	if (0 == dot) {
		return true;
	}
	for (i = 0; i < WF_ARRAY_LEN(QUIET); i++) {
		if (same_extension(name + dot, end - dot, QUIET[i])) {
			return false;
		}
	}
	return true;
}
