#include "wf_image_wire.h"

#include <string.h>

static uint32_t le32(const unsigned char *b)
{
	return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
	        ((uint32_t)b[3] << 24);
}
static uint64_t le64(const unsigned char *b)
{
	return (uint64_t)le32(b) | ((uint64_t)le32(b + 4) << 32);
}

bool wf_image_reply_accept(
        WfImageReply *state, const void *message, size_t actual_bytes)
{
	const unsigned char *b = message;
	uint32_t width, height, stride;
	uint64_t payload;
	if (!state || !b || state->accepted ||
	        actual_bytes < WF_IMAGE_WIRE_HEADER ||
	        actual_bytes > WF_IMAGE_WIRE_HEADER + WF_IMAGE_WIRE_RASTER) {
		return false;
	}
	if (memcmp(b, "WFIM", 4) || 1 != le32(b + 4) ||
	        le64(b + 8) != state->request || 1 != le32(b + 28)) {
		return false;
	}
	width = le32(b + 16);
	height = le32(b + 20);
	stride = le32(b + 24);
	payload = le64(b + 32);
	if (!width || !height || width > UINT32_MAX / 4u ||
	        stride != width * 4u || payload != (uint64_t)height * stride ||
	        payload > WF_IMAGE_WIRE_RASTER ||
	        payload != actual_bytes - WF_IMAGE_WIRE_HEADER) {
		return false;
	}
	state->accepted = true;
	return true;
}
