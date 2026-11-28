/* Exercise the actual one-shot helper's text sanitizer without driving a UI. */
#define wmain e2e_program_main
#include "../tools/e2e/piero_e2e_ui.c"
#undef wmain
#include "wf_test.h"

int main(void)
{
	clean(NULL, 0);
	wchar_t one[1] = {L'x'};
	clean(one, 1);
	CHECK(!one[0]);
	wchar_t short_text[4] = {L'a', L'\n', 0x65e5, L'!'};
	clean(short_text, sizeof short_text / sizeof *short_text);
	CHECK(short_text[0] == L'a' && short_text[1] == L' ' &&
	        short_text[2] == 0x65e5 && !short_text[3]);
	wchar_t full[MAX_TEXT_OUT + 1];
	for (size_t i = 0; i < sizeof full / sizeof *full; i++) {
		full[i] = L'x';
	}
	clean(full, sizeof full / sizeof *full);
	CHECK(wcslen(full) == MAX_TEXT_OUT);
	full[0] = 0;
	clean(full, sizeof full / sizeof *full);
	CHECK(!full[0]);
	return TEST_RESULT();
}
