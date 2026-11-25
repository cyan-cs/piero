/* libFuzzer harness: INI configuration parsing and the INI editor. Input is
 * borrowed and the editor output must be NUL-terminated. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "config/wf_config.h"
#include "config/wf_ini_edit.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	WfConfig c;
	WfConfigReport rep;
	char *copy, *out = NULL;
	size_t out_n = 0;
	static const WfIniSet sets[] = {
	        {"ui", "theme", "nano"}, {"keys", "mode", NULL}};

	wf_config_init(&c);
	memset(&rep, 0, sizeof rep);
	(void)wf_config_parse(&c, data, size, NULL, NULL, &rep);
	wf_config_dispose(&c);

	copy = malloc(size + 1);
	if (!copy) {
		abort();
	}
	memcpy(copy, data, size);
	copy[size] = '\0';
	if (WF_OK == wf_ini_apply(copy, size, sets, 2, &out, &out_n)) {
		if ('\0' != out[out_n]) {
			abort();
		}
		free(out);
	}
	free(copy);
	return 0;
}
