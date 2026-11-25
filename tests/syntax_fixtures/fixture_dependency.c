/* The library the FIXTURE_DEP grammar imports: it exists only so that the import
 * can be made; the test never puts it where the loader may find it, except in
 * one case that checks where the loader does look. */
#include <windows.h>

__declspec(dllexport) int fixture_dependency(void);

__declspec(dllexport) int fixture_dependency(void)
{
	return 1;
}
