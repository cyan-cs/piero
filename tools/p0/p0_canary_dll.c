/*
 * An intentionally UNSIGNED, do-nothing DLL. P0-A04/A05 stage it next to the
 * sandboxed worker and try to load it: allowed before the signature policy is
 * switched on, refused afterwards. That difference is the evidence for H-017.
 */
#include <windows.h>

__declspec(dllexport) int p0_canary(void)
{
	return 42;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
	(void)h;
	(void)reason;
	(void)reserved;
	return TRUE;
}
