#include <windows.h>

#include <stdbool.h>
#include <string.h>

#include "wf_pcplace_win.h"
#include "wf_wsl_win.h"

static bool fake;
static DWORD fake_drives;
static const wchar_t *const *fake_names;

void wf_pcplace_test_source(
        bool use, unsigned long drives, const wchar_t *const *names)
{
	fake = use;
	fake_drives = (DWORD)drives;
	fake_names = names;
}

static WfStatus add_folder(WfDir *d, const wchar_t *name)
{
	return wf_dir_add(d, name, wcslen(name), WF_ENT_DIR, 0, 0);
}

WfStatus wf_pcplace_list(size_t limit, WfDir **out)
{
	WfDir *d;
	WfStatus st = WF_OK;
	DWORD drives = fake ? fake_drives : GetLogicalDrives();

	if (!out) {
		return WF_E_INVAL;
	}
	*out = NULL;
	d = wf_dir_new();
	if (!d) {
		return WF_E_NOMEM;
	}
	d->limit = limit;
	for (unsigned i = 0; i < 26 && WF_OK == st; i++) {
		if (drives & (1u << i)) {
			wchar_t name[3] = {(wchar_t)(L'A' + i), L':', L'\0'};

			st = add_folder(d, name);
		}
	}
	if (WF_OK == st && fake) {
		for (size_t i = 0; fake_names && fake_names[i] && WF_OK == st;
		        i++) {
			wchar_t name[4 + WF_WSL_NAME_MAX] = L"wsl:";

			wcscat_s(name, 4 + WF_WSL_NAME_MAX, fake_names[i]);
			st = add_folder(d, name);
		}
	} else if (WF_OK == st) {
		WfWslDistro found[16];
		size_t n = wf_wsl_distros(found, 16);

		for (size_t i = 0; i < n && WF_OK == st; i++) {
			wchar_t name[4 + WF_WSL_NAME_MAX] = L"wsl:";

			wcscat_s(name, 4 + WF_WSL_NAME_MAX, found[i].name);
			st = add_folder(d, name);
		}
	}
	if (WF_OK != st) {
		wf_dir_free(d);
		return st;
	}
	*out = d;
	return WF_OK;
}
