/* Minimal COFF/ASan ICF reproducer: the padded char and wchar_t literals are
 * byte-identical, but their global descriptors have different valid lengths.
 * Volatile pointers preserve the actual instrumented reads at /O2. */
#include <stdio.h>
#include <wchar.h>
static const char *volatile narrow = "";
static const wchar_t *volatile wide = L"";
int main(int argc, char **argv)
{
	volatile size_t index = argc > 1 ? 1u : 0u;
	(void)argv;
	printf("%u %u\n", (unsigned)narrow[index], (unsigned)wide[0]);
	return 0;
}
