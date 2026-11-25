/* Same pooled-literal collision as test_asan_pool.c, inside a DLL whose
 * linker options come from the actual syntax DLL target. */
#include <wchar.h>

static const char *volatile narrow = "";
static const wchar_t *volatile wide = L"";

__declspec(dllexport) unsigned wf_asan_pool_read(unsigned index)
{
	return (unsigned)narrow[index] + (unsigned)wide[0];
}
