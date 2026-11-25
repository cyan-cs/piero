#include <string.h>

#include "preview/wf_image_wire.h"
#include "wf_test.h"

static void test_invalid_frames_never_accepted(void)
{
	/* 1x1 PBGRA reply for request7; four payload bytes. */
	const unsigned char good[44] = {'W', 'F', 'I', 'M', 1, 0, 0, 0, 7, 0, 0,
	        0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 4, 0, 0, 0, 1, 0, 0, 0,
	        4, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4};
	unsigned char b[44];
	WfImageReply state = {7, 0};
	CHECK(wf_image_reply_accept(&state, good, sizeof good));
	CHECK(!wf_image_reply_accept(
	        &state, good, sizeof good)); /* duplicate */
	for (size_t n = 0; n < sizeof good; n++) {
		state.accepted = 0;
		CHECK(!wf_image_reply_accept(&state, good, n));
	}
	/* Each header field is independently validated, without accepting
	 * merely because total byte count happens to be bounded. */
	for (size_t i = 0; i < WF_IMAGE_WIRE_HEADER; i++) {
		memcpy(b, good, sizeof b);
		b[i] ^= 0x80;
		state.accepted = 0;
		CHECK(!wf_image_reply_accept(&state, b, sizeof b));
	}
	state.request = 6;
	state.accepted = 0;
	CHECK(!wf_image_reply_accept(
	        &state, good, sizeof good)); /* out-of-order */
	state.request = 7;
	CHECK(!wf_image_reply_accept(
	        &state, good, sizeof good + 1u)); /* oversized */
	CHECK(!wf_image_reply_accept(&state, NULL, sizeof good));
	CHECK(!wf_image_reply_accept(NULL, good, sizeof good));
	/* Overflow combinations and random frames run on a real buffer under
	 * optional ASan; no allocator or payload access is needed to reject. */
	for (uint32_t seed = 1; seed < 20000; seed++) {
		uint32_t x = seed;
		for (size_t i = 0; i < sizeof b; i++) {
			x = x * 1664525u + 1013904223u;
			b[i] = (unsigned char)(x >> 24);
		}
		state.accepted = 0;
		CHECK(!wf_image_reply_accept(&state, b, seed % sizeof b));
	}
}
int main(void)
{
	test_invalid_frames_never_accepted();
	return TEST_RESULT();
}
