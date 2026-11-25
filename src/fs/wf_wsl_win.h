#ifndef WF_WSL_WIN_H
#define WF_WSL_WIN_H

#include <windows.h>

#include <stddef.h>

/*
 * The WSL distributions registered for this user, read from the registry
 * (HKCU\Software\Microsoft\Windows\CurrentVersion\Lxss). Reading it starts
 * nothing: no distribution, no service, no wsl.exe. The names are exactly as
 * registered (case kept) and only those that fit the rules of a place name
 * (wf_locid_wsl) are returned, so a name from here can be put in a path.
 */
#define WF_WSL_NAME_MAX 129

typedef struct
{
	wchar_t name[WF_WSL_NAME_MAX];
	DWORD version; /* 1 or 2; 0 when the registry does not say */
} WfWslDistro;

/* Fills up to `cap` entries and returns how many; 0 when WSL is not set up. */
size_t wf_wsl_distros(WfWslDistro *out, size_t cap);

/* "Ubuntu, docker-desktop" for a message, cut to `cap` characters. */
void wf_wsl_names_line(wchar_t *out, size_t cap);

#endif
