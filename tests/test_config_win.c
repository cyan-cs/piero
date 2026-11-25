#include <stdlib.h>

#include "../tools/p0/p0_lab.h"
#include "config/wf_config_win.h"
#include "wf_test.h"
int main(void)
{
	P0Lab lab;
	WfConfig config;
	WfConfigReport report;
	DWORD error, wrote;
	HANDLE h;
	wchar_t file[1024], *absolute = NULL, *env = NULL;
	const char ini[] =
	        "[ui]\nshow_hidden=true\n[preview]\ntext_max_kib=1024\n";
	wf_config_init(&config);
	CHECK(p0_lab_open(&lab, "config", 1));
	if (!lab.area) {
		return 1;
	}
	CHECK(p0_lab_path(&lab, L"config.ini", file, 1024));
	CHECK(WF_OK ==
	        wf_config_read(&config, file, 1, NULL, NULL, &report, &error));
	CHECK(INVALID_FILE_ATTRIBUTES == GetFileAttributesW(file));
	CHECK(WF_E_NOENT ==
	        wf_config_read(&config, file, 0, NULL, NULL, &report, &error));
	h = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	CHECK(WriteFile(h, ini, sizeof ini - 1, &wrote, NULL));
	CHECK(WF_E_ACCESS ==
	        wf_config_read(&config, file, 0, NULL, NULL, &report, &error));
	CloseHandle(h);
	CHECK(p0_area_track(lab.area, L"config.ini", 0));
	CHECK(WF_OK ==
	        wf_config_read(&config, file, 0, NULL, NULL, &report, &error));
	CHECK(1 == config.show_hidden && 1024 == config.text_max_kib);
	CHECK(WF_OK == wf_startup_absolute(L".", &absolute, &error));
	CHECK(absolute && wcslen(absolute) > 3);
	free(absolute);
	CHECK(SetEnvironmentVariableW(L"PIERO_TEST_STATE_ROOT", lab.root));
	CHECK(wf_startup_env_path(L"PIERO_TEST_STATE_ROOT", L"piero\\journal",
	              &env, &error) == WF_OK);
	CHECK(env && wcsstr(env, L"piero\\journal"));
	free(env);
	CHECK(SetEnvironmentVariableW(L"PIERO_TEST_STATE_ROOT", NULL));
	wf_config_dispose(&config);
	CHECK(0 == p0_lab_close(&lab, 0 != wf_test_failures));
	return TEST_RESULT();
}
