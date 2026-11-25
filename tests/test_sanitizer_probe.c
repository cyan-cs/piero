/* Deliberate faults in a test-only process prove the configured runtimes fire. */
#include <limits.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	if (argc == 2 && argv[1][0] == 'a') {
		volatile size_t index = 9;
		char *p = malloc(8);
		if (!p) {
			return 2;
		}
		/* Keep the deliberate store observable under /O2 as well as /Od. */
		((volatile char *)p)[index] = 1;
		free(p);
	} else {
		volatile int value = INT_MAX;
		volatile int result = value + 1;
		(void)result;
	}
	return 0;
}
