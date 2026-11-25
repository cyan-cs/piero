/* Which names need a confirmation before they are opened. */
#include "fs/wf_launch.h"
#include "wf_test.h"
#include "base/wf_macro.h"

static void listed_extensions_ask(void)
{
	static const wchar_t *const ask[] = {L"a.exe", L"a.EXE", L"a.Com",
	        L"a.bat", L"a.cmd", L"a.ps1", L"a.js", L"a.vbs", L"a.wsf",
	        L"a.hta", L"a.lnk", L"a.url", L"a.msi", L"a.scr", L"a.reg",
	        L"a.jar", L"a.msix", L"a.appref-ms", L"x.tar.exe",
	        /* interpreters that Windows may associate: never on a list of
		 * "dangerous" types, always asked */
	        L"a.py", L"a.pyw", L"a.pyc", L"a.pyz", L"a.rb", L"a.pl",
	        L"a.php", L"a.sh", L"a.ts", L"a.mjs", L"a.svg", L"a.docm",
	        L"a.xlsm", L"a.iso", L"a.vhd", L"a.something-new", L"notes",
	        L".bashrc", L"exe", L"a.exex"};
	size_t i;

	for (i = 0; i < WF_ARRAY_LEN(ask); i++) {
		CHECK(wf_launch_needs_confirm(ask[i]));
	}
}

static void ordinary_documents_do_not(void)
{
	static const wchar_t *const quiet[] = {L"a.txt", L"photo.PNG", L"x.pdf",
	        L"a.exe.txt", L"readme.md", L"a.MP3", L"a.c", L"b.JPeG"};
	size_t i;

	for (i = 0; i < WF_ARRAY_LEN(quiet); i++) {
		CHECK(!wf_launch_needs_confirm(quiet[i]));
	}
}

/* Win32 drops trailing dots and spaces, so "a.exe." runs a.exe. */
static void trailing_dots_and_spaces_do_not_hide_the_type(void)
{
	CHECK(wf_launch_needs_confirm(L"a.exe."));
	CHECK(wf_launch_needs_confirm(L"a.exe "));
	CHECK(wf_launch_needs_confirm(L"a.exe. . "));
	CHECK(wf_launch_needs_confirm(L"a.bat.."));
	CHECK(!wf_launch_needs_confirm(L"a.txt."));
	CHECK(wf_launch_needs_confirm(L"a.py. "));
}

/* Nothing to judge means ask. */
static void unjudgeable_names_ask(void)
{
	CHECK(wf_launch_needs_confirm(NULL));
	CHECK(wf_launch_needs_confirm(L""));
	CHECK(wf_launch_needs_confirm(L"."));
	CHECK(wf_launch_needs_confirm(L".. "));
}

int main(void)
{
	listed_extensions_ask();
	ordinary_documents_do_not();
	trailing_dots_and_spaces_do_not_hide_the_type();
	unjudgeable_names_ask();
	return TEST_RESULT();
}
