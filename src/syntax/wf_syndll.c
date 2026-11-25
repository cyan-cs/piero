/*
 * syntax\piero-syntax.dll: the one export (DR-DRAFT-033). The trust table below
 * is generated at build time from the grammar DLLs, queries and manifests that
 * were built with this DLL; a grammar of another release does not match it.
 */
#include <windows.h>

#include "wf_dllload_win.h"
#include "wf_syncore.h"
#include "wf_syntax_abi.h"
#include "wf_syntax_trust.h"

__declspec(dllexport) uint32_t wf_syntax_api(
        uint32_t host_abi, const WfSyntaxApi **out);

__declspec(dllexport) uint32_t wf_syntax_api(
        uint32_t host_abi, const WfSyntaxApi **out)
{
	wchar_t dir[1024];
	uint32_t st;

	if (!out) {
		return WF_SYN_E_BADARG;
	}
	*out = NULL;
	if (WF_SYNTAX_HOST_ABI != host_abi) {
		return WF_SYN_E_ABI;
	}
	/* the grammars are looked for beside this DLL and nowhere else */
	st = wf_dll_module_dir((const void *)wf_syntax_api, dir, 1024);
	if (WF_SYN_OK != st) {
		return st;
	}
	wf_syncore_configure(dir, WF_SYN_TRUST, WF_SYN_TRUST_COUNT);
	*out = wf_syncore_api();
	return WF_SYN_OK;
}
