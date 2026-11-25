#include "wf_synhost_win.h"

#include <string.h>

#include "wf_dllload_win.h"

#define PATH_CAP 1024u
#define MAX_DLL_BYTES (8u * 1024u * 1024u)

static struct
{
	SRWLOCK lock;
	wchar_t root
	        [PATH_CAP]; /* the folder that holds "syntax"; "" until known */
	bool root_given;
	uint8_t pin[32];
	bool pinned;
	HMODULE mod;
	const WfSyntaxApi *api;
	bool refused; /* the DLL was there and was refused: not hashed again */
	uint32_t refusal;
} H = {SRWLOCK_INIT, L"", false, {0}, false, NULL, NULL, false, 0};

void wf_synhost_configure(const wchar_t *root, const char *pin_hex)
{
	AcquireSRWLockExclusive(&H.lock);
	H.pinned = pin_hex && wf_dll_hex32(pin_hex, H.pin);
	H.root[0] = L'\0';
	H.root_given = false;
	if (root && wcslen(root) < PATH_CAP) {
		wcscpy_s(H.root, PATH_CAP, root);
		H.root_given = true;
	}
	H.refused = false;
	ReleaseSRWLockExclusive(&H.lock);
}

/* The root, found on first use. Called with the lock. */
static uint32_t root_locked(void)
{
	if (H.root[0]) {
		return WF_SYN_OK;
	}
	if (H.root_given) {
		return WF_SYN_E_PATH;
	}
	return wf_dll_exe_dir(H.root, PATH_CAP);
}

static void widen(const char *s, wchar_t *out, size_t cap)
{
	size_t i = 0;

	for (; s[i] && i + 1 < cap; i++) {
		out[i] = (wchar_t)(unsigned char)s[i];
	}
	out[i] = L'\0';
}

bool wf_synhost_installed(const WfSynLang *lang)
{
	wchar_t root[PATH_CAP], id[32], p[PATH_CAP];
	const wchar_t *parts[4];
	static const wchar_t *const files[] = {
	        L"parser.dll", L"highlights.scm", L"manifest.toml"};
	uint32_t st;

	if (!lang) {
		return false;
	}
	if (lang->builtin) {
		return true;
	}
	AcquireSRWLockExclusive(&H.lock);
	st = root_locked();
	if (WF_SYN_OK == st) {
		wcscpy_s(root, PATH_CAP, H.root);
	}
	ReleaseSRWLockExclusive(&H.lock);
	if (WF_SYN_OK != st) {
		return false;
	}
	parts[0] = L"syntax";
	parts[1] = L"piero-syntax.dll";
	if (WF_SYN_OK != wf_dll_resolve(root, parts, 2, p, PATH_CAP) ||
	        !wf_dll_present(p)) {
		return false;
	}
	widen(lang->id, id, sizeof id / sizeof id[0]);
	parts[1] = L"grammars";
	parts[2] = id;
	for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
		parts[3] = files[i];
		if (WF_SYN_OK != wf_dll_resolve(root, parts, 4, p, PATH_CAP) ||
		        !wf_dll_present(p)) {
			return false;
		}
	}
	return true;
}

/* Loads piero-syntax.dll if it is not. Called with the lock. */
static uint32_t ensure_locked(void)
{
	wchar_t p[PATH_CAP];
	const wchar_t *parts[2] = {L"syntax", L"piero-syntax.dll"};
	WfDllHold hold = {INVALID_HANDLE_VALUE, {0}, 0};
	uint32_t st = root_locked(), r;
	FARPROC fp;
	WfSyntaxApiFn fn;
	const WfSyntaxApi *api = NULL;

	if (H.api) {
		return WF_SYN_OK;
	}
	if (H.refused) {
		return H.refusal;
	}
	if (WF_SYN_OK != st) {
		return st;
	}
	st = wf_dll_resolve(H.root, parts, 2, p, PATH_CAP);
	if (WF_SYN_OK != st) {
		return st;
	}
	if (!wf_dll_present(p)) {
		return WF_SYN_E_NOGRAMMAR;
	}
	if (!H.pinned) {
		st = WF_SYN_E_TRUST;
		goto refuse;
	}
	st = wf_dll_hold(p, MAX_DLL_BYTES, H.pin, &hold);
	if (WF_SYN_OK != st) {
		goto refuse;
	}
	st = wf_dll_load(p, &H.mod);
	wf_dll_release(&hold);
	if (WF_SYN_OK != st) {
		goto refuse;
	}
	fp = GetProcAddress(H.mod, WF_SYNTAX_EXPORT);
	if (!fp) {
		st = WF_SYN_E_SYMBOL;
		goto unload;
	}
	fn = (WfSyntaxApiFn)(void *)fp;
	r = fn(WF_SYNTAX_HOST_ABI, &api);
	if (WF_SYN_OK != r || !api || WF_SYNTAX_HOST_ABI != api->abi ||
	        api->size < sizeof(WfSyntaxApi)) {
		st = WF_SYN_OK != r ? r : WF_SYN_E_ABI;
		goto unload;
	}
	H.api = api;
	return WF_SYN_OK;
unload:
	FreeLibrary(H.mod);
	H.mod = NULL;
refuse:
	H.refused = true;
	H.refusal = st;
	return st;
}

uint32_t wf_synhost_open(
        const char *lang_id, const WfSynLimits *limits, WfSynSession **out)
{
	uint32_t st;
	const WfSyntaxApi *api;

	if (!out) {
		return WF_SYN_E_BADARG;
	}
	*out = NULL;
	AcquireSRWLockExclusive(&H.lock);
	st = ensure_locked();
	api = H.api;
	ReleaseSRWLockExclusive(&H.lock);
	if (WF_SYN_OK != st) {
		return st;
	}
	return api->open(lang_id, limits, out);
}

uint32_t wf_synhost_highlight(
        WfSynSession *s, const char *text, size_t len, WfSynResult *out)
{
	return H.api ? H.api->highlight(s, text, len, out) : WF_SYN_E_INTERNAL;
}

void wf_synhost_result_free(WfSynResult *r)
{
	if (H.api) {
		H.api->result_free(r);
	}
}

void wf_synhost_close(WfSynSession *s)
{
	if (H.api && s) {
		H.api->close(s);
	}
}

uint64_t wf_synhost_stat(uint32_t which)
{
	return H.api ? H.api->stat(which) : 0;
}

bool wf_synhost_loaded(void)
{
	return NULL != H.api;
}

void wf_synhost_shutdown(void)
{
	AcquireSRWLockExclusive(&H.lock);
	if (H.api) {
		H.api->shutdown();
		H.api = NULL;
	}
	if (H.mod) {
		FreeLibrary(H.mod);
		H.mod = NULL;
	}
	ReleaseSRWLockExclusive(&H.lock);
}
