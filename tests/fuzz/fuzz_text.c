/* libFuzzer harness: text conversion, sanitising, display width, chunking,
 * file-name validation and archive virtual paths. Invariants: sanitised
 * output is NUL-terminated strict UTF-8; chunking always advances and stays
 * in range; the UTF-16 conversion never reports more units than it was
 * given. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "archive/wf_vpath.h"
#include "base/wf_buf.h"
#include "path/wf_name.h"
#include "text/wf_text.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	const char *s = (const char *)data;
	WfBuf b;
	size_t n16 = size / 2;
	wchar_t *w = malloc((n16 + 1) * sizeof *w);
	wchar_t *out = malloc((size + 1) * sizeof *out);

	if (!w || !out) {
		abort();
	}
	memcpy(w, data, n16 * sizeof *w);
	w[n16] = 0;

	wf_buf_init(&b);
	if (WF_OK == wf_sanitize_utf8(s, size, &b) && b.len) {
		if ('\0' != b.data[b.len]) {
			abort();
		}
		(void)wf_utf8_cells(b.data, b.len);
	}
	wf_buf_clear(&b);
	(void)wf_sanitize_utf16(w, n16, &b);
	wf_buf_free(&b);

	{
		size_t used = 1;

		if (WF_OK == wf_utf8_to_utf16(s, size, out, size, &used) &&
		        used > size) {
			abort();
		}
	}
	for (size_t i = 0, step; i < n16; i += step) {
		step = wf_utf16_chunk_end(w + i, n16 - i, 1 + data[0] % 9);
		if (step < 1 || step > n16 - i) {
			abort();
		}
	}
	(void)wf_name_validate(w, n16, 255);

	{
		wchar_t *arch = NULL;
		const wchar_t *inner = NULL;

		if (wf_vpath_split(w, &arch, &inner)) {
			if (inner < w || inner > w + n16) {
				abort();
			}
			free(arch);
		}
		(void)wf_vpath_archive_len(w);
		(void)wf_archive_name_recognized(w);
	}
	free(w);
	free(out);
	return 0;
}
