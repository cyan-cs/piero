/* Run interactively in the terminal being validated. No Sixel payload is
 * emitted: this reports the same capability conditions the product uses. */
#include <windows.h>
#include <stdio.h>
#include "term/wf_term.h"

int main(void)
{
	WfTerm *term = NULL;
	WfTermCaps caps;
	WfEvent events[16];
	wchar_t wt[8];
	uint64_t end;
	bool profile, automatic, manual;
	if (wf_term_open(&term) != WF_OK) {
		fputs("Cannot open a VT console; run interactively in the "
		      "target terminal.\n",
		        stderr);
		return 1;
	}
	end = GetTickCount64() + 1000;
	while (GetTickCount64() < end) {
		(void)wf_term_read_events(term, events, 16);
		Sleep(5);
	}
	caps = *wf_term_capabilities(term);
	wf_term_close(term);
	profile = GetEnvironmentVariableW(L"WT_SESSION", wt, 8) != 0;
	manual = caps.cell_w && caps.cell_h;
	automatic =
	        manual && (caps.colors || profile) && wf_term_caps_ready(&caps);
	printf("DA1_received=%d sixel_attribute_4=%d "
	       "rectangle_attribute_28=%d\n",
	        caps.da, caps.sixel, caps.rectangular);
	printf("Sixel_cell_coordinates=%ux%u colour_registers=%u "
	       "WT_profile=%d\n",
	        caps.cell_w, caps.cell_h, caps.colors, profile);
	printf("graphics_auto=%s graphics_sixel=%s graphics_text=text\n",
	        automatic ? "sixel" : "text",
	        manual ? "sixel (explicit override)" : "text (no dimensions)");
	puts("Reported coordinates can be virtual Sixel pixels, not physical "
	     "display pixels.");
	puts("This probe does not verify actual image rendering, erasure, or "
	     "AppContainer startup.");
	return 0;
}
