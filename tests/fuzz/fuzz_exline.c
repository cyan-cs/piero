/* libFuzzer harness: the Vim command line. Invariant: no crash or overrun,
 * a refused line carries a NUL-terminated name/arg, the error text fits. */
#include <stdint.h>
#include <string.h>

#include "input/wf_exline.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	for (int s = 0; s < 2; s++) {
		WfEx ex;
		char msg[256];

		memset(&ex, 0xA5, sizeof ex);
		(void)wf_ex_parse((const char *)data, size,
		        s ? WF_EXS_EDITOR : WF_EXS_FM, &ex);
		if (NULL == memchr(ex.name, 0, sizeof ex.name) ||
		        NULL == memchr(ex.arg, 0, sizeof ex.arg)) {
			__builtin_trap();
		}
		wf_ex_error_text(&ex, msg, sizeof msg);
	}
	return 0;
}
