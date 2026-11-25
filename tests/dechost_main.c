/* The decoder host as its own program, for the tests: the product runs the
 * same loop from wfile.exe --decoder-host. */
#include <windows.h>

#include "preview/wf_dechost_win.h"

int wmain(int argc, wchar_t **argv)
{
	int code = 1;

	return wf_dechost_from_args(argc, argv, &code) ? code : 1;
}
