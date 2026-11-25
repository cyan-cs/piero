#include "wf_hash.h"

void wf_sha256_hex(const uint8_t digest[32], char out[65])
{
	static const char hex[] = "0123456789abcdef";

	for (size_t i = 0; i < 32; i++) {
		out[2 * i] = hex[digest[i] >> 4];
		out[2 * i + 1] = hex[digest[i] & 15];
	}
	out[64] = '\0';
}
