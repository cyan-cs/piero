#include <string.h>

#include "wf_sgr_rgb.h"
#include "wf_test.h"

int main(void)
{
	const char *valid[] = {
	        "20;40;60m", "20;40;60;48;2;1;2;3m", "20;40;60;1m"};
	const char *invalid[] = {"20;40;60", "256;40;60m", "20;;60m",
	        "a;40;60m", "20;40;60x", "20;40m"};
	char out[32];

	for (size_t i = 0; i < sizeof valid / sizeof *valid; ++i) {
		CHECK(p0_sgr_rgb((const unsigned char *)valid[i],
		        strlen(valid[i]), out, sizeof out));
		CHECK(!strcmp(out, "20;40;60"));
	}
	for (size_t i = 0; i < sizeof invalid / sizeof *invalid; ++i) {
		CHECK(!p0_sgr_rgb((const unsigned char *)invalid[i],
		        strlen(invalid[i]), out, sizeof out));
		CHECK(!out[0]);
	}
	CHECK(!p0_sgr_rgb(
	        (const unsigned char *)"20;40;60m", 5, out, sizeof out));
	CHECK(!p0_sgr_rgb((const unsigned char *)"20;40;60m", 9, out, 8));
	CHECK(!out[0]);
	return TEST_RESULT();
}
