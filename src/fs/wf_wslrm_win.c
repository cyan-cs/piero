#include "wf_wslrm_win.h"

bool wf_wslrm_entry(const wchar_t *ext, DWORD attrs, DWORD *error)
{
	bool ok;

	if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
		/* A link: open the entry itself (never what it points to) and let
		 * closing the handle remove it; the plain delete call resolves the
		 * target on this share and fails for a link. */
		HANDLE h = CreateFileW(ext, DELETE,
		        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		        NULL, OPEN_EXISTING,
		        FILE_FLAG_OPEN_REPARSE_POINT |
		                FILE_FLAG_BACKUP_SEMANTICS |
		                FILE_FLAG_DELETE_ON_CLOSE,
		        NULL);

		ok = INVALID_HANDLE_VALUE != h;
		if (ok) {
			CloseHandle(h);
		}
	} else {
		ok = (attrs & FILE_ATTRIBUTE_DIRECTORY)
		        ? 0 != RemoveDirectoryW(ext)
		        : 0 != DeleteFileW(ext);
	}
	if (!ok && error) {
		*error = GetLastError();
	}
	return ok;
}

bool wf_wslrm_created_file(const wchar_t *ext)
{
	return 0 != DeleteFileW(ext);
}
