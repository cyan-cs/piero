/* External translations are bounded data, never format strings or code.
 * Lookups return immutable strings: the bounded, deduplicating pool preserves
 * pointers held by dialogs or workers across language changes. Only startup,
 * settings discovery and explicit language changes read files. */
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "wf_i18n.h"
#include "../text/wf_text.h"
#include "../edit/wf_eenc.h"
#include "../base/wf_macro.h"

typedef struct
{
	const char *name;
	unsigned arity;
	const char *en;
} Entry;
#include "wf_catalog_generated.h"

#define FILE_LIMIT (1u << 20)
#define TEXT_LIMIT 4096u
#define KEY_LIMIT 8192u
#define PATH_LIMIT 32768u
#define CANDIDATES 64u
#define POOL_LIMIT (16u << 20)
#define POOL_BUCKETS 4096u

typedef struct String String;
struct String
{
	String *next;
	uint32_t hash;
	char text[];
};
typedef struct
{
	char code[32];
	const char *name;
	bool none;
	bool available;
	const char *text[WF_MSG_COUNT];
} Language;
typedef struct
{
	const char *section, *key;
} Seen;
typedef struct
{
	char *bytes;
	char *name;
	bool none;
	bool has_plural;
	const char *text[WF_MSG_COUNT];
} Loaded;

static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static SRWLOCK lock = SRWLOCK_INIT;
static Language languages[WF_I18N_LANG_LIMIT];
static LONG language_count;
static volatile LONG current_language;
static String *strings[POOL_BUCKETS];
static size_t storage_used;
static wchar_t bundle_root[PATH_LIMIT], user_root[PATH_LIMIT];
char wf_langset_choices[WF_I18N_LANG_LIMIT * 33 + 5] = "auto";

/* Called with the exclusive registry lock. Never releases published strings. */
static const char *intern(const char *text)
{
	uint32_t hash = 2166136261u;
	size_t len = strlen(text), bucket, bytes;
	size_t chain = 0;
	for (size_t i = 0; i < len; i++) {
		hash = (hash ^ (unsigned char)text[i]) * 16777619u;
	}
	bucket = hash % POOL_BUCKETS;
	for (String *p = strings[bucket]; p; p = p->next) {
		if (p->hash == hash && !strcmp(p->text, text)) {
			return p->text;
		}
		if (++chain >= 64) {
			return NULL;
		}
	}
	bytes = sizeof(String) + len + 1;
	if (bytes > POOL_LIMIT - storage_used) {
		return NULL;
	}
	String *p = malloc(bytes);
	if (!p) {
		return NULL;
	}
	p->hash = hash;
	p->next = strings[bucket];
	memcpy(p->text, text, len + 1);
	strings[bucket] = p;
	storage_used += bytes;
	return p->text;
}

static bool valid_code(const char *code)
{
	size_t n = strlen(code);
	if (!n || n > 31 || code[0] < 'a' || code[0] > 'z' ||
	        !strcmp(code, "auto") || code[n - 1] == '-') {
		return false;
	}
	for (size_t i = 1; i < n; i++) {
		char c = code[i];
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
		            (c == '-' && code[i - 1] != '-'))) {
			return false;
		}
	}
	return true;
}

static bool safe_text(const char *text, bool multiline)
{
	size_t n = strlen(text);
	if (n > TEXT_LIMIT || !wf_utf8_strict(text, n, NULL)) {
		return false;
	}
	for (size_t i = 0; i < n;) {
		uint32_t cp;
		size_t used = wf_utf8_decode(text + i, n - i, &cp);
		if ((cp < 32 && !(multiline && cp == '\n')) ||
		        (cp >= 127 && cp <= 159) ||
		        (cp >= 0x202a && cp <= 0x202e) ||
		        (cp >= 0x2066 && cp <= 0x2069)) {
			return false;
		}
		i += used;
	}
	for (const char *p = text; *p; p++) {
		if (*p == '%') {
			const char *q = p + 1;
			while (*q && strchr("0123456789$.*+- #hljztLI", *q)) {
				q++;
			}
			if (*q == 'n') {
				return false;
			}
		}
	}
	return true;
}

static bool placeholder_counts(
        const char *p, unsigned arity, unsigned counts[WF_MSG_MAX_ARGS])
{
	unsigned mask = 0;
	for (; *p; p++) {
		if (p[0] == '{' && p[1] == '{') {
			p++;
		} else if (*p == '{') {
			if (p[1] < '0' || p[1] > '3' || p[2] != '}') {
				return false;
			}
			mask |= 1u << (unsigned)(p[1] - '0');
			counts[p[1] - '0']++;
			p += 2;
		} else if (*p == '}') {
			return false;
		}
	}
	return mask == (1u << arity) - 1u;
}

static bool placeholders(const char *text, int id)
{
	unsigned translated[WF_MSG_MAX_ARGS] = {0}, base[WF_MSG_MAX_ARGS] = {0};
	if (!placeholder_counts(text, catalog[id].arity, translated) ||
	        !placeholder_counts(catalog[id].en, catalog[id].arity, base)) {
		return false;
	}
	for (unsigned i = 0; i < WF_MSG_MAX_ARGS; i++) {
		if (translated[i] != base[i]) {
			return false;
		}
	}
	return true;
}

static char *trim(char *s)
{
	while (*s == ' ' || *s == '\t') {
		s++;
	}
	size_t n = strlen(s);
	while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
		s[--n] = 0;
	}
	return s;
}

static bool unescape(char *s)
{
	char *to = s;
	for (char *p = s; *p; p++) {
		if (*p != '\\') {
			*to++ = *p;
		} else {
			p++;
			if (*p == '\\') {
				*to++ = '\\';
			} else if (*p == 'n') {
				*to++ = '\n';
			} else if (*p == 's') {
				*to++ = ' ';
			} else {
				return false;
			}
		}
	}
	*to = 0;
	return safe_text(s, true);
}

static bool identifier(const char *s, size_t max)
{
	size_t n = strlen(s);
	if (!n || n > max) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		char c = s[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		            c == '_' ||
		            (i &&
		                    ((c >= '0' && c <= '9') || c == '.' ||
		                            c == '-')))) {
			return false;
		}
	}
	return true;
}

static int seen_compare(const void *a, const void *b)
{
	const Seen *x = a, *y = b;
	int order = strcmp(x->section, y->section);
	return order ? order : strcmp(x->key, y->key);
}

static int message_id(const char *key)
{
	for (int i = 0; i < WF_MSG_COUNT; i++) {
		if (!strcmp(key, catalog[i].name)) {
			return i;
		}
	}
	return -1;
}

static bool parse(Loaded *out, const char *code)
{
	Seen *seen = calloc(KEY_LIMIT, sizeof *seen);
	size_t count = 0;
	char *section = NULL, *next = out->bytes;
	const char *declared = NULL;
	bool good = false;
	if (!seen) {
		return false;
	}
	if (!strncmp(next, "\xef\xbb\xbf", 3)) {
		next += 3;
	}
	while (*next) {
		char *line = next, *end = strchr(next, '\n');
		if (end) {
			*end = 0;
			next = end + 1;
		} else {
			next += strlen(next);
		}
		size_t length = strlen(line);
		if (length && line[length - 1] == '\r') {
			line[--length] = 0;
		}
		line = trim(line);
		if (!*line || *line == ';' || *line == '#') {
			continue;
		}
		length = strlen(line);
		if (line[0] == '[' && length >= 3 && line[length - 1] == ']') {
			line[length - 1] = 0;
			section = line + 1;
			if (!identifier(section, 64)) {
				goto done;
			}
			continue;
		}
		char *equals = strchr(line, '=');
		if (!section || !equals || count >= KEY_LIMIT) {
			goto done;
		}
		*equals = 0;
		char *key = trim(line), *value = trim(equals + 1);
		if (!identifier(key, 96) || !unescape(value)) {
			goto done;
		}
		seen[count++] = (Seen){section, key};
		if (!strcmp(section, "language")) {
			if (!strcmp(key, "code")) {
				declared = value;
			} else if (!strcmp(key, "name")) {
				out->name = value;
			} else if (!strcmp(key, "plural")) {
				if (strcmp(value, "none") &&
				        strcmp(value, "one-other")) {
					goto done;
				}
				out->none = !strcmp(value, "none");
				out->has_plural = true;
			}
		} else if (!strcmp(section, "messages") ||
		        !strcmp(section, "arguments")) {
			int id = message_id(key);
			if (!strcmp(section, "arguments")) {
				if (strlen(value) != 1 || *value < '0' ||
				        *value > '4' ||
				        (id >= 0 &&
				                (unsigned)(*value - '0') !=
				                        catalog[id].arity)) {
					goto done;
				}
			} else if (id >= 0) {
				if (!*value || !placeholders(value, id)) {
					goto done;
				}
				out->text[id] = value;
			}
		}
	}
	if (!declared || strcmp(declared, code) || !valid_code(declared) ||
	        !out->name || !*out->name || strlen(out->name) > 128 ||
	        !safe_text(out->name, false)) {
		goto done;
	}
	qsort(seen, count, sizeof *seen, seen_compare);
	for (size_t i = 1; i < count; i++) {
		if (!seen_compare(&seen[i - 1], &seen[i])) {
			goto done;
		}
	}
	good = true;
done:
	free(seen);
	return good;
}

static void loaded_free(Loaded *data)
{
	if (data) {
		free(data->bytes);
		free(data);
	}
}

static bool join_file(const wchar_t *root, const char *code, wchar_t *path)
{
	size_t n = wcslen(root), len = strlen(code);
	if (!n || n + len + 6 >= PATH_LIMIT) {
		return false;
	}
	memcpy(path, root, n * sizeof *path);
	if (path[n - 1] != L'\\' && path[n - 1] != L'/') {
		path[n++] = L'\\';
	}
	for (size_t i = 0; i < len; i++) {
		path[n++] = (wchar_t)(unsigned char)code[i];
	}
	memcpy(path + n, L".ini", 5 * sizeof *path);
	return true;
}

static Loaded *read_language(const wchar_t *root, const char *code)
{
	wchar_t *path = malloc(PATH_LIMIT * sizeof *path);
	Loaded *data = NULL;
	HANDLE file = INVALID_HANDLE_VALUE;
	FILE_BASIC_INFO before, after;
	LARGE_INTEGER size, final_size;
	DWORD got;
	if (!path || !join_file(root, code, path)) {
		goto done;
	}
	file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING,
	        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		goto done;
	}
	if (GetFileType(file) != FILE_TYPE_DISK ||
	        !GetFileInformationByHandleEx(
	                file, FileBasicInfo, &before, sizeof before) ||
	        (before.FileAttributes &
	                (FILE_ATTRIBUTE_DIRECTORY |
	                        FILE_ATTRIBUTE_REPARSE_POINT)) ||
	        !GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
	        size.QuadPart > FILE_LIMIT) {
		goto done;
	}
	data = calloc(1, sizeof *data);
	if (!data) {
		goto done;
	}
	data->bytes = malloc((size_t)size.QuadPart + 1);
	if (!data->bytes ||
	        !ReadFile(
	                file, data->bytes, (DWORD)size.QuadPart, &got, NULL) ||
	        got != (DWORD)size.QuadPart ||
	        !GetFileSizeEx(file, &final_size) ||
	        final_size.QuadPart != size.QuadPart ||
	        !GetFileInformationByHandleEx(
	                file, FileBasicInfo, &after, sizeof after) ||
	        before.LastWriteTime.QuadPart != after.LastWriteTime.QuadPart ||
	        before.ChangeTime.QuadPart != after.ChangeTime.QuadPart ||
	        memchr(data->bytes, 0, got) ||
	        !wf_utf8_strict(data->bytes, got, NULL)) {
		goto invalid;
	}
	data->bytes[got] = 0;
	/* Reject controls even in comments; separators are syntax, not values. */
	for (size_t i = 0; i < got;) {
		uint32_t c;
		size_t used = wf_utf8_decode(data->bytes + i, got - i, &c);
		if ((c < 32 && c != '\r' && c != '\n' && c != '\t') ||
		        (c >= 127 && c <= 159) ||
		        (c >= 0x202a && c <= 0x202e) ||
		        (c >= 0x2066 && c <= 0x2069)) {
			goto invalid;
		}
		i += used;
	}
	if (!parse(data, code)) {
		goto invalid;
	}
	goto done;
invalid:
	loaded_free(data);
	data = NULL;
done:
	if (file != INVALID_HANDLE_VALUE) {
		CloseHandle(file);
	}
	free(path);
	return data;
}

static int find_language(const char *code)
{
	for (int i = 0; i < language_count; i++) {
		if (!strcmp(languages[i].code, code)) {
			return i;
		}
	}
	return -1;
}

static void reload_language(int index)
{
	char code[32];
	AcquireSRWLockShared(&lock);
	memcpy(code, languages[index].code, sizeof code);
	ReleaseSRWLockShared(&lock);
	Loaded *bundle = read_language(bundle_root, code);
	Loaded *user = read_language(user_root, code);
	AcquireSRWLockExclusive(&lock);
	Language *lang = &languages[index];
	lang->available = index == WF_UILANG_EN || bundle || user;
	for (int id = 0; id < WF_MSG_COUNT; id++) {
		lang->text[id] = NULL;
	}
	lang->name = index < 2 ? base_langs[index].name : lang->code;
	lang->none = index < 2 && base_langs[index].none;
	Loaded *layers[] = {bundle, user};
	for (size_t layer = 0; layer < WF_ARRAY_LEN(layers); layer++) {
		Loaded *data = layers[layer];
		if (!data) {
			continue;
		}
		const char *name = intern(data->name);
		if (name) {
			lang->name = name;
		}
		if (data->has_plural) {
			lang->none = data->none;
		}
		for (int id = 0; id < WF_MSG_COUNT; id++) {
			if (data->text[id]) {
				const char *value = intern(data->text[id]);
				if (value) {
					lang->text[id] = value;
				}
			}
		}
	}
	ReleaseSRWLockExclusive(&lock);
	loaded_free(bundle);
	loaded_free(user);
}

static int code_compare(const void *a, const void *b)
{
	return strcmp(a, b);
}

/* Indices never move during a session, including for missing configured codes. */
static int register_language(const char *code)
{
	int slot = find_language(code);
	if (slot >= 0) {
		return slot;
	}
	if (language_count >= WF_I18N_LANG_LIMIT) {
		return -1;
	}
	slot = (int)language_count;
	(void)strcpy_s(languages[slot].code, sizeof languages[slot].code, code);
	languages[slot].name = languages[slot].code;
	(void)strcat_s(wf_langset_choices, sizeof wf_langset_choices, "|");
	(void)strcat_s(wf_langset_choices, sizeof wf_langset_choices, code);
	InterlockedIncrement(&language_count);
	return slot;
}

static void discover(const wchar_t *root)
{
	wchar_t *pattern = malloc(PATH_LIMIT * sizeof *pattern);
	char codes[CANDIDATES][32];
	size_t n = 0;
	WIN32_FIND_DATAW entry;
	HANDLE search = INVALID_HANDLE_VALUE;
	if (!pattern || !join_file(root, "*", pattern)) {
		goto done;
	}
	search = FindFirstFileW(pattern, &entry);
	if (search == INVALID_HANDLE_VALUE) {
		goto done;
	}
	do {
		size_t len = wcslen(entry.cFileName);
		if (entry.dwFileAttributes &
		        (FILE_ATTRIBUTE_DIRECTORY |
		                FILE_ATTRIBUTE_REPARSE_POINT)) {
			continue;
		}
		if (len <= 4 || len > 35 ||
		        _wcsicmp(entry.cFileName + len - 4, L".ini")) {
			continue;
		}
		char code[32] = {0};
		for (size_t i = 0; i < len - 4; i++) {
			wchar_t c = entry.cFileName[i];
			if (c >= L'A' && c <= L'Z') {
				c += L'a' - L'A';
			}
			code[i] = c < 128 ? (char)c : '?';
		}
		if (!valid_code(code)) {
			continue;
		}
		if (n < CANDIDATES) {
			memcpy(codes[n++], code, sizeof code);
		}
	} while (n < CANDIDATES && FindNextFileW(search, &entry));
	qsort(codes, n, sizeof codes[0], code_compare);
	for (size_t i = 0; i < n; i++) {
		AcquireSRWLockShared(&lock);
		int found = find_language(codes[i]);
		bool room = language_count < WF_I18N_LANG_LIMIT;
		ReleaseSRWLockShared(&lock);
		if (found >= 0 || !room) {
			continue;
		}
		Loaded *data = read_language(root, codes[i]);
		if (!data) {
			continue;
		}
		AcquireSRWLockExclusive(&lock);
		(void)register_language(codes[i]);
		ReleaseSRWLockExclusive(&lock);
		loaded_free(data);
	}
done:
	if (search != INVALID_HANDLE_VALUE) {
		FindClose(search);
	}
	free(pattern);
}

static void refresh(void)
{
	discover(bundle_root);
	discover(user_root);
	LONG count = InterlockedCompareExchange(&language_count, 0, 0);
	for (int i = 0; i < count; i++) {
		reload_language(i);
	}
}

typedef struct
{
	const wchar_t *bundle, *user;
} Roots;

static BOOL CALLBACK initialize(
        PINIT_ONCE init, PVOID parameter, PVOID *context)
{
	(void)init;
	(void)context;
	const Roots *roots = parameter;
	for (int i = 0; i < 2; i++) {
		(void)strcpy_s(languages[i].code, sizeof languages[i].code,
		        base_langs[i].id);
		languages[i].name = base_langs[i].name;
		languages[i].none = base_langs[i].none;
		(void)strcat_s(
		        wf_langset_choices, sizeof wf_langset_choices, "|");
		(void)strcat_s(wf_langset_choices, sizeof wf_langset_choices,
		        base_langs[i].id);
	}
	language_count = 2;
	if (roots) {
		if (roots->bundle && wcslen(roots->bundle) < PATH_LIMIT) {
			wcscpy_s(bundle_root, PATH_LIMIT, roots->bundle);
		}
		if (roots->user && wcslen(roots->user) < PATH_LIMIT) {
			wcscpy_s(user_root, PATH_LIMIT, roots->user);
		}
	} else {
		DWORD size = GetModuleFileNameW(NULL, bundle_root, PATH_LIMIT);
		if (size && size < PATH_LIMIT) {
			wchar_t *slash = wcsrchr(bundle_root, L'\\');
			if (slash &&
			        (size_t)(slash - bundle_root) + 11 <
			                PATH_LIMIT) {
				wcscpy_s(slash + 1,
				        PATH_LIMIT -
				                (size_t)(slash + 1 -
				                        bundle_root),
				        L"languages");
			} else {
				bundle_root[0] = 0;
			}
		} else {
			bundle_root[0] = 0;
		}
		size = GetEnvironmentVariableW(
		        L"LOCALAPPDATA", user_root, PATH_LIMIT);
		if (size && size + 18u < PATH_LIMIT) {
			wcscat_s(user_root, PATH_LIMIT, L"\\piero\\languages");
		} else {
			user_root[0] = 0;
		}
	}
	refresh();
	return TRUE;
}

void wf_i18n_initialize(const wchar_t *bundle, const wchar_t *user)
{
	Roots roots = {bundle, user};
	(void)InitOnceExecuteOnce(&once, initialize, &roots, NULL);
}

static void ensure_initialized(void)
{
	(void)InitOnceExecuteOnce(&once, initialize, NULL, NULL);
}
void wf_i18n_refresh(void)
{
	ensure_initialized();
	refresh();
}
int wf_i18n_lang_count(void)
{
	ensure_initialized();
	return (int)InterlockedCompareExchange(&language_count, 0, 0);
}
size_t wf_i18n_storage_used(void)
{
	ensure_initialized();
	AcquireSRWLockShared(&lock);
	size_t n = storage_used;
	ReleaseSRWLockShared(&lock);
	return n;
}

WfUiLang wf_i18n_language(void)
{
	ensure_initialized();
	return (WfUiLang)InterlockedCompareExchange(&current_language, 0, 0);
}

void wf_i18n_set_language(WfUiLang lang)
{
	if (lang >= 0 && lang < wf_i18n_lang_count()) {
		reload_language(lang);
		InterlockedExchange(&current_language, (LONG)lang);
	}
}

const char *wf_i18n_lang_id(WfUiLang lang)
{
	ensure_initialized();
	AcquireSRWLockShared(&lock);
	const char *id =
	        languages[lang >= 0 && lang < language_count ? lang : 0].code;
	ReleaseSRWLockShared(&lock);
	return id;
}

const char *wf_i18n_lang_name(WfUiLang lang)
{
	ensure_initialized();
	AcquireSRWLockShared(&lock);
	const char *name =
	        languages[lang >= 0 && lang < language_count ? lang : 0].name;
	ReleaseSRWLockShared(&lock);
	return name;
}

bool wf_i18n_lang_available(WfUiLang lang)
{
	ensure_initialized();
	AcquireSRWLockShared(&lock);
	bool available =
	        lang >= 0 && lang < language_count && languages[lang].available;
	ReleaseSRWLockShared(&lock);
	return available;
}

bool wf_i18n_config_setting(const char *text, WfLangSetting *out)
{
	if (wf_i18n_parse_setting(text, out)) {
		return true;
	}
	if (!text || !out || !valid_code(text)) {
		return false;
	}
	AcquireSRWLockExclusive(&lock);
	int index = register_language(text);
	ReleaseSRWLockExclusive(&lock);
	if (index < 0) {
		return false;
	}
	*out = index + 1;
	return true;
}

WfLangSetting wf_i18n_setting_of(WfUiLang lang)
{
	return lang >= 0 && lang < wf_i18n_lang_count() ? lang + 1
	                                                : WF_LANGSET_AUTO;
}
const char *wf_i18n_setting_id(WfLangSetting setting)
{
	return setting > 0 && setting <= wf_i18n_lang_count()
	        ? wf_i18n_lang_id(setting - 1)
	        : "auto";
}
bool wf_i18n_parse_setting(const char *text, WfLangSetting *out)
{
	if (!text || !out) {
		return false;
	}
	ensure_initialized();
	if (!strcmp(text, "auto")) {
		*out = WF_LANGSET_AUTO;
		return true;
	}
	AcquireSRWLockShared(&lock);
	int index = find_language(text);
	ReleaseSRWLockShared(&lock);
	if (index < 0) {
		return false;
	}
	*out = index + 1;
	return true;
}

const char *wf_msg_name(WfMsg id)
{
	return (unsigned)id < WF_MSG_COUNT ? catalog[id].name : "MSG_INVALID";
}
size_t wf_msg_arity(WfMsg id)
{
	return (unsigned)id < WF_MSG_COUNT ? catalog[id].arity : 0;
}
bool wf_msg_has(WfUiLang lang, WfMsg id)
{
	ensure_initialized();
	AcquireSRWLockShared(&lock);
	bool yes = (unsigned)id < WF_MSG_COUNT && lang >= 0 &&
	        lang < language_count &&
	        (lang == WF_UILANG_EN || languages[lang].text[id] != NULL);
	ReleaseSRWLockShared(&lock);
	return yes;
}

const char *wf_msg_in(WfUiLang lang, WfMsg id)
{
	if ((unsigned)id >= WF_MSG_COUNT) {
		return "MSG_INVALID";
	}
	ensure_initialized();
	AcquireSRWLockShared(&lock);
	const char *text = lang >= 0 && lang < language_count
	        ? languages[lang].text[id]
	        : NULL;
	ReleaseSRWLockShared(&lock);
	return text ? text : catalog[id].en;
}
const char *wf_msg(WfMsg id)
{
	return wf_msg_in(wf_i18n_language(), id);
}
const char *wf_msg_reason(const char *english)
{
	if (!english) {
		return NULL;
	}
	for (int id = 0; id < WF_MSG_COUNT; id++) {
		if (!strncmp(catalog[id].name, "MSG_REASON_", 11) &&
		        !strcmp(catalog[id].en, english)) {
			return wf_msg((WfMsg)id);
		}
	}
	return english;
}
WfMsg wf_msg_count(WfMsg one, WfMsg other, long long n)
{
	WfUiLang lang = wf_i18n_language();
	AcquireSRWLockShared(&lock);
	bool none = languages[lang].none;
	ReleaseSRWLockShared(&lock);
	return !none && n == 1 ? one : other;
}
