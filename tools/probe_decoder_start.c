/* Starts the product's AppContainer decoder through its normal startup path. */
#include <windows.h>
#include <stdio.h>

#include "preview/wf_dechost_client_win.h"

int wmain(int argc, wchar_t **argv)
{
	WfDecHostOptions options = {0};
	WfDecHost *host = NULL;
	WfDecStartPhase phase = WF_DEC_START_VALIDATE;
	DWORD error = 0;
	if (argc != 2) {
		fputs("usage: probe_decoder_start <product-exe>\n", stderr);
		return 2;
	}
	options.exe = argv[1];
	options.sandbox = 1;
	WfStatus status =
	        wf_dechost_start_with_phase(&options, &host, &error, &phase);
	printf("status=%d phase=%d error=0x%08lx\n", (int)status, (int)phase,
	        (unsigned long)error);
	int isolated = host && wf_dechost_sandboxed(host);
	printf("actual_child_appcontainer=%d\n", isolated);
	wf_dechost_stop(host);
	return status == WF_OK && isolated ? 0 : 1;
}
