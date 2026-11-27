/* Exercise libarchive's real normalizers after the Unicode data update. */
#include <locale.h>
#include <stdint.h>
#include <string.h>
#include "wf_test.h"
#define __LIBARCHIVE_TEST
#include "archive_string.h"

static size_t utf8(const uint32_t *scalars, char out[32])
{
	size_t n = 0;
	for (; *scalars; scalars++) {
		uint32_t cp = *scalars;
		if (cp < 0x80) {
			out[n++] = (char)cp;
		} else if (cp < 0x800) {
			out[n++] = (char)(0xC0 | (cp >> 6));
			out[n++] = (char)(0x80 | (cp & 63));
		} else if (cp < 0x10000) {
			out[n++] = (char)(0xE0 | (cp >> 12));
			out[n++] = (char)(0x80 | ((cp >> 6) & 63));
			out[n++] = (char)(0x80 | (cp & 63));
		} else {
			out[n++] = (char)(0xF0 | (cp >> 18));
			out[n++] = (char)(0x80 | ((cp >> 12) & 63));
			out[n++] = (char)(0x80 | ((cp >> 6) & 63));
			out[n++] = (char)(0x80 | (cp & 63));
		}
	}
	return n;
}

static void normalized(
        const uint32_t *input, const uint32_t *expected, int form)
{
	struct archive *a = archive_read_new();
	struct archive_string text = {0};
	struct archive_string_conv *conversion;
	char from[32], to[32];
	size_t from_n = utf8(input, from), to_n = utf8(expected, to);
	CHECK(a != NULL);
	if (!a) {
		return;
	}
	conversion = archive_string_conversion_to_charset(a, "UTF-8", 0);
	CHECK(conversion != NULL);
	if (conversion) {
		archive_string_conversion_set_opt(conversion, form);
		CHECK(0 == archive_strncpy_l(&text, from, from_n, conversion));
		if (text.length != to_n || !text.s ||
		        memcmp(text.s, to, to_n)) {
			printf("normalization form %d, first scalar U+%04X: "
			       "got %zu bytes, expected %zu\n",
			        form, (unsigned)input[0], text.length, to_n);
		}
		CHECK(text.length == to_n);
		CHECK(text.s && text.length == to_n &&
		        !memcmp(text.s, to, to_n));
	}
	archive_string_free(&text);
	CHECK(ARCHIVE_OK == archive_read_free(a));
}

int main(void)
{
	/* Use UTF-8 at both ends regardless of the runner's Windows code page. */
	CHECK(setlocale(LC_CTYPE, ".UTF8") != NULL);
	static const struct
	{
		uint32_t decomposed[4], composed[4];
	} cases[] = {
	        {{0x65, 0x301, 0}, {0xE9, 0}},
	        {{0x306F, 0x3099, 0}, {0x3070, 0}},
	        {{0x1100, 0x1161, 0}, {0xAC00, 0}},
	        /* Post-6.0 mappings: Todhri and Tulu-Tigalari. */
	        {{0x105D2, 0x307, 0}, {0x105C9, 0}},
	        {{0x11382, 0x113C9, 0}, {0x11383, 0}},
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		normalized(cases[i].decomposed, cases[i].composed,
		        SCONV_SET_OPT_NORMALIZATION_C);
		normalized(cases[i].composed, cases[i].decomposed,
		        SCONV_SET_OPT_NORMALIZATION_D);
	}
	/* Composition exclusions and combining-class order must remain correct. */
	normalized((uint32_t[]){0x308, 0x301, 0}, (uint32_t[]){0x308, 0x301, 0},
	        SCONV_SET_OPT_NORMALIZATION_C);
	normalized((uint32_t[]){0x41, 0x301, 0x327, 0},
	        (uint32_t[]){0x41, 0x327, 0x301, 0},
	        SCONV_SET_OPT_NORMALIZATION_D);
	return TEST_RESULT();
}
