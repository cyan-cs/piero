#ifndef WF_P0_SGR_RGB_H
#define WF_P0_SGR_RGB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* The three parameters after 38;2;. A following ';' starts another SGR
 * attribute (for example 48;2; background), not another colour channel. */
static inline bool p0_sgr_rgb(
        const unsigned char *input, size_t n, char *out, size_t cap)
{
	unsigned rgb[3];
	size_t at = 0;

	if (!out || !cap) {
		return false;
	}
	out[0] = '\0';
	for (unsigned channel = 0; channel < 3; ++channel) {
		size_t start = at;
		unsigned value = 0;

		while (at < n && input[at] >= '0' && input[at] <= '9') {
			value = value * 10 + (unsigned)(input[at++] - '0');
			if (value > 255) {
				return false;
			}
		}
		if (at == start || at == n) {
			return false;
		}
		rgb[channel] = value;
		if (channel < 2) {
			if (input[at++] != ';') {
				return false;
			}
		} else if (input[at] != ';' && input[at] != 'm') {
			return false;
		}
	}
	int written = snprintf(out, cap, "%u;%u;%u", rgb[0], rgb[1], rgb[2]);
	if (written < 0 || (size_t)written >= cap) {
		out[0] = '\0';
		return false;
	}
	return true;
}

#endif
