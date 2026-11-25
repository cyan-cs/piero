/* libFuzzer harness: the structure checks that run on untrusted picture
 * bytes before any decoder sees them. They must never read outside the
 * buffer, whatever the bytes say about their own lengths. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "preview/wf_image_check.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	const char *why = NULL;
	/* a copy of exactly this size, so ASan sees any read past the end */
	uint8_t *copy = malloc(size ? size : 1);

	if (!copy) {
		return 0;
	}
	memcpy(copy, data, size);
	(void)wf_png_structure_ok(copy, size, &why);
	(void)wf_ico_structure_ok(copy, size, &why);
	(void)wf_jpeg_structure_ok(copy, size, &why);
	(void)wf_gif_structure_ok(copy, size, &why);
	(void)wf_webp_structure_ok(copy, size, NULL);
	free(copy);
	return 0;
}
