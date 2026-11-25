#include "wf_gitenv.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../archive/wf_vpath.h"
#include "../fs/wf_loc.h"

/* After the server name of a WSL path: the distribution and the rest. */
static bool wsl_parts(const wchar_t *path,
        const wchar_t **distro,
        size_t *distro_n,
        const wchar_t **rest)
{
	const wchar_t *p = path;

	if (!wf_wsl_unc_is(path)) {
		return false;
	}
	p += !_wcsnicmp(p, L"\\\\?\\UNC\\", 8) ? 8 : 2;
	while (*p && L'\\' != *p && L'/' != *p) { /* the server name */
		p++;
	}
	while (L'\\' == *p || L'/' == *p) {
		p++;
	}
	*distro = p;
	*distro_n = 0;
	while (p[*distro_n] && L'\\' != p[*distro_n] && L'/' != p[*distro_n]) {
		(*distro_n)++;
	}
	*rest = p + *distro_n;
	return *distro_n > 0;
}

static bool drive_letter(wchar_t c)
{
	return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
}

WfGitEnvKind wf_gitenv_classify_text(const wchar_t *path, WfGitEnv *out)
{
	WfGitEnv env;
	const wchar_t *d, *rest;
	size_t n;
	char name[WF_WSL_NAME_MAX];
	WfLocId id;

	memset(&env, 0, sizeof env);
	if (!path || wf_vpath_is(path)) {
		goto done;
	}
	if (wf_wsl_unc_is(path)) {
		if (!wsl_parts(path, &d, &n, &rest) || n >= WF_WSL_NAME_MAX) {
			goto done;
		}
		for (size_t i = 0; i < n; i++) {
			if (d[i] > 0x7F) { /* a distribution name is ASCII */
				goto done;
			}
			name[i] = (char)d[i];
		}
		name[n] = '\0';
		if (WF_OK != wf_locid_wsl(&id, name)) {
			goto done;
		}
		memcpy(env.distro, name, n + 1);
		env.kind = WF_GENV_WSL;
		goto done;
	}
	{
		const wchar_t *p = path;

		if (!_wcsnicmp(p, L"\\\\?\\", 4)) {
			p += 4;
		}
		if (drive_letter(p[0]) && L':' == p[1] &&
		        (L'\\' == p[2] || L'/' == p[2] || !p[2])) {
			env.kind = WF_GENV_WINDOWS;
		}
	}
done:
	if (out) {
		*out = env;
	}
	return env.kind;
}

WfGitEnvKind wf_gitenv_classify(const wchar_t *path, WfGitEnv *out)
{
	WfGitEnv env;

	if (WF_GENV_WINDOWS == wf_gitenv_classify_text(path, &env)) {
		wchar_t root[4];
		const wchar_t *p =
		        path + (!_wcsnicmp(path, L"\\\\?\\", 4) ? 4 : 0);
		UINT type;

		root[0] = p[0];
		root[1] = L':';
		root[2] = L'\\';
		root[3] = L'\0';
		type = GetDriveTypeW(root);
		if (DRIVE_REMOTE == type || DRIVE_CDROM == type ||
		        DRIVE_NO_ROOT_DIR == type || DRIVE_UNKNOWN == type) {
			memset(&env, 0, sizeof env);
		}
	}
	if (out) {
		*out = env;
	}
	return env.kind;
}

WfStatus wf_gitenv_wsl_linux_path(const wchar_t *path, char *out, size_t cap)
{
	const wchar_t *d, *rest;
	size_t n, len = 0;
	wchar_t *tmp;
	int bytes;

	if (!path || !out || cap < 2 || !wsl_parts(path, &d, &n, &rest)) {
		return WF_E_INVAL;
	}
	/* "\home\u\x" to "/home/u/x"; the root of the distribution is "/" */
	len = wcslen(rest);
	tmp = malloc((len + 2) * sizeof *tmp);
	if (!tmp) {
		return WF_E_NOMEM;
	}
	{
		size_t k = 0;

		for (size_t i = 0; i < len; i++) {
			wchar_t c = rest[i];

			if (L'\\' == c || L'/' == c) {
				if (k && L'/' == tmp[k - 1]) {
					continue; /* one separator for a run */
				}
				c = L'/';
			}
			tmp[k++] = c;
		}
		if (k > 1 && L'/' == tmp[k - 1]) {
			k--; /* no trailing separator */
		}
		if (!k) {
			tmp[k++] = L'/';
		}
		tmp[k] = L'\0';
		len = k;
	}
	bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, tmp,
	        (int)len, out, (int)(cap - 1), NULL, NULL);
	free(tmp);
	if (bytes <= 0) {
		return WF_E_INVAL; /* a lone surrogate, or it does not fit */
	}
	out[bytes] = '\0';
	return wf_posix_path_ok((const uint8_t *)out, (size_t)bytes)
	        ? WF_OK
	        : WF_E_INVAL;
}

bool wf_git_nearest_dotgit(const wchar_t *dir, wchar_t *root, size_t cap)
{
	wchar_t buf[MAX_PATH * 4];
	size_t len, min_len = 0, skip = 0;

	if (!dir || !root || !cap) {
		return false;
	}
	len = wcslen(dir);
	if (len + 8 >= ARRAYSIZE(buf)) {
		return false;
	}
	memcpy(buf, dir, (len + 1) * sizeof *buf);
	while (len > 2 && (L'\\' == buf[len - 1] || L'/' == buf[len - 1])) {
		buf[--len] = L'\0';
	}
	if (!_wcsnicmp(buf, L"\\\\?\\UNC\\", 8)) {
		skip = 8; /* the name of the server follows */
	} else if (!_wcsnicmp(buf, L"\\\\?\\", 4)) {
		skip = 4;
	}
	if (L':' == buf[skip + 1]) {
		min_len = skip + 2; /* "X:" */
	} else {
		/* \\server\share: the end of the second name */
		size_t i = skip ? skip : 2, names = 0;

		for (; buf[i] && names < 2; i++) {
			if (L'\\' == buf[i] || L'/' == buf[i]) {
				names++;
				if (2 == names) {
					break;
				}
			}
		}
		min_len = i;
		if (names < 2) {
			min_len =
			        len; /* just the server and share, nothing above */
		}
	}
	for (int level = 0; level < 128; level++) {
		wchar_t probe[MAX_PATH * 4 + 8];
		DWORD attrs;
		size_t cut;

		swprintf(probe, ARRAYSIZE(probe), L"%ls\\.git", buf);
		attrs = GetFileAttributesW(probe);
		if (INVALID_FILE_ATTRIBUTES != attrs) {
			if (len + 1 > cap) {
				return false;
			}
			memcpy(root, buf, (len + 1) * sizeof *buf);
			return true;
		}
		if (len <= min_len) {
			return false;
		}
		for (cut = len; cut > min_len && L'\\' != buf[cut - 1] &&
		        L'/' != buf[cut - 1];
		        cut--) {
		}
		while (cut > min_len &&
		        (L'\\' == buf[cut - 1] || L'/' == buf[cut - 1])) {
			cut--;
		}
		if (cut < min_len || cut == len) {
			return false;
		}
		len = cut;
		buf[len] = L'\0';
	}
	return false;
}

bool wf_gitenv_rel_path(const WfGitEnv *env,
        const wchar_t *root,
        const wchar_t *path,
        char *out,
        size_t cap)
{
	size_t rl = wcslen(root);
	wchar_t *tmp;
	int n;
	bool under;

	if (!env || wcslen(path) <= rl + 1) {
		return false;
	}
	under = WF_GENV_WSL == env->kind ? 0 == wcsncmp(path, root, rl)
	                                 : CSTR_EQUAL ==
	                CompareStringOrdinal(
	                        path, (int)rl, root, (int)rl, TRUE);
	if (!under || (L'\\' != path[rl] && L'/' != path[rl])) {
		return false;
	}
	tmp = malloc((wcslen(path + rl + 1) + 1) * sizeof *tmp);
	if (!tmp) {
		return false;
	}
	wcscpy_s(tmp, wcslen(path + rl + 1) + 1, path + rl + 1);
	for (wchar_t *p = tmp; *p; p++) {
		if (L'\\' == *p) {
			*p = L'/';
		}
	}
	n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, tmp, -1, out,
	        (int)cap, NULL, NULL);
	free(tmp);
	return n > 1;
}
