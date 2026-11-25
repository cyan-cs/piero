/* Read-only look at WSL from Windows, to settle the contract before any WSL
 * browsing is built into the product.
 *
 *   probe_wsl                    list the registered distributions (registry
 *                                only; starts nothing, touches no distro)
 *   probe_wsl --root <name>      also list the top folder of that distribution
 *                                through \\wsl.localhost\<name>. That is
 *                                reading, but it makes WSL start the distro if
 *                                it is stopped; hence the explicit flag.
 *
 * It opens things with FILE_READ_ATTRIBUTES only, writes nothing, and prints
 * counts and capability booleans, never file names. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef IO_REPARSE_TAG_LX_SYMLINK
#	define IO_REPARSE_TAG_LX_SYMLINK 0xA000001DL
#endif

static void list_distributions(void)
{
	HKEY root;
	DWORD index = 0;

	if (ERROR_SUCCESS !=
	        RegOpenKeyExW(HKEY_CURRENT_USER,
	                L"Software\\Microsoft\\Windows\\CurrentVersion"
	                L"\\Lxss",
	                0, KEY_READ, &root)) {
		puts("distributions=none (no Lxss key: WSL is not set up for "
		     "this "
		     "user)");
		return;
	}
	for (;;) {
		wchar_t id[64], name[128];
		DWORD id_n = 64, name_n = sizeof name, version = 0,
		      version_n = sizeof version, state = 0,
		      state_n = sizeof state;
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
		if (ERROR_SUCCESS ==
		        RegQueryValueExW(key, L"DistributionName", NULL, NULL,
		                (BYTE *)name, &name_n)) {
			RegQueryValueExW(key, L"Version", NULL, NULL,
			        (BYTE *)&version, &version_n);
			RegQueryValueExW(key, L"State", NULL, NULL,
			        (BYTE *)&state, &state_n);
			wprintf(L"distribution name=%ls wsl_version=%lu "
			        L"state=%lu\n",
			        name, version, state);
		}
		RegCloseKey(key);
	}
	RegCloseKey(root);
}

static void inspect_root(const wchar_t *name)
{
	wchar_t path[512], pattern[520];
	WIN32_FIND_DATAW fd;
	HANDLE f;
	size_t entries = 0, dirs = 0, files = 0, links = 0, lx_links = 0,
	       other_reparse = 0, zero_mtime = 0;
	HANDLE h;
	FILE_ID_INFO id;
	FILE_CASE_SENSITIVE_INFO cs = {0};
	DWORD t0 = GetTickCount();

	swprintf(path, 512, L"\\\\wsl.localhost\\%ls", name);
	swprintf(pattern, 520, L"%ls\\*", path);
	f = FindFirstFileExW(
	        pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
	if (INVALID_HANDLE_VALUE == f) {
		printf("root_listing=FAIL win32=%lu (distribution stopped, not "
		       "installed, or WSL file service unavailable)\n",
		        GetLastError());
		return;
	}
	do {
		if (!wcscmp(fd.cFileName, L".") ||
		        !wcscmp(fd.cFileName, L"..")) {
			continue;
		}
		entries++;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			dirs++;
		} else {
			files++;
		}
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
			links++;
			if (IO_REPARSE_TAG_LX_SYMLINK == fd.dwReserved0) {
				lx_links++;
			} else {
				other_reparse++;
			}
		}
		zero_mtime += 0 == fd.ftLastWriteTime.dwHighDateTime &&
		        0 == fd.ftLastWriteTime.dwLowDateTime;
	} while (FindNextFileW(f, &fd));
	FindClose(f);
	printf("root_listing=OK entries=%zu dirs=%zu files=%zu reparse=%zu "
	       "(lx_symlink=%zu other=%zu) zero_mtime=%zu listing_ms=%lu\n",
	        entries, dirs, files, links, lx_links, other_reparse,
	        zero_mtime, GetTickCount() - t0);

	h = CreateFileW(path, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	if (INVALID_HANDLE_VALUE == h) {
		printf("root_open=FAIL win32=%lu\n", GetLastError());
		return;
	}
	printf("file_id_info=%s\n",
	        GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof id)
	                ? "available"
	                : "unavailable");
	if (GetFileInformationByHandleEx(
	            h, FileCaseSensitiveInfo, &cs, sizeof cs)) {
		printf("case_sensitive_dir=%s\n",
		        (cs.Flags & FILE_CS_FLAG_CASE_SENSITIVE_DIR) ? "yes"
		                                                     : "no");
	} else {
		printf("case_sensitive_info=unavailable win32=%lu\n",
		        GetLastError());
	}
	CloseHandle(h);
	puts("modified=NO");
}

int wmain(int argc, wchar_t **argv)
{
	const wchar_t *root = NULL;

	for (int i = 1; i < argc; i++) {
		if (!wcscmp(argv[i], L"--root") && i + 1 < argc) {
			root = argv[++i];
		} else {
			fputs("usage: probe_wsl [--root <distribution>]\n",
			        stderr);
			return 2;
		}
	}
	list_distributions();
	if (root) {
		inspect_root(root);
	}
	return 0;
}
