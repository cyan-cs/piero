#include <stddef.h>
#include <string.h>

#include "fs/wf_watch_win.h"
#include "path/wf_path.h"
#include "wf_test.h"

static DWORD data[1024];

static size_t event(const wchar_t *name)
{
	size_t bytes = wcslen(name) * sizeof(wchar_t);
	memset(data, 0, sizeof data);
	data[1] = FILE_ACTION_MODIFIED;
	data[2] = (DWORD)bytes;
	memcpy((unsigned char *)(void *)data +
	                offsetof(FILE_NOTIFY_INFORMATION, FileName),
	        name, bytes);
	return offsetof(FILE_NOTIFY_INFORMATION, FileName) + bytes;
}

int main(void)
{
	size_t bytes = event(L"out\\trace.log");
	WfWatchChanges c = wf_watch_classify(data, bytes, L"read.txt");
	CHECK(!c.listing && !c.preview);
	bytes = event(L"other.txt");
	c = wf_watch_classify(data, bytes, L"read.txt");
	CHECK(c.listing && !c.preview);
	bytes = event(L"READ.TXT");
	c = wf_watch_classify(data, bytes, L"read.txt");
	CHECK(c.listing && c.preview);
	bytes = event(L"read.txt:stream");
	c = wf_watch_classify(data, bytes, L"read.txt");
	CHECK(c.listing && c.preview);
	bytes = event(L"folder\\child.txt");
	c = wf_watch_classify(data, bytes, L"folder");
	CHECK(!c.listing && c.preview);
	bytes = event(L"folder2\\child.txt");
	c = wf_watch_classify(data, bytes, L"folder");
	CHECK(!c.listing && !c.preview);
	bytes = event(L"FOLDER~1\\child.txt");
	c = wf_watch_classify(data, bytes, L"folder");
	CHECK(c.listing && c.preview);
	c = wf_watch_classify(data, 0, L"folder");
	CHECK(c.listing && c.preview);
	bytes = event(L"other.txt");
	data[0] = 4; /* overlapping/misaligned chain cannot exclude a change */
	c = wf_watch_classify(data, bytes, L"folder");
	CHECK(c.listing && c.preview);
	data[0] = 0;
	data[2]++;
	c = wf_watch_classify(data, bytes, L"folder");
	CHECK(c.listing && c.preview);
	for (size_t n = 1; n < 12; n++) {
		c = wf_watch_classify(data, n, L"folder");
		CHECK(c.listing && c.preview);
	}
	/* The stand-in places are not folders: opening one waits for a name
	 * lookup that must fail (about eleven seconds, on the screen's thread). */
	{
		WfWatch *w = NULL;

		CHECK(wf_path_host_invalid(L"\\\\pc.invalid\\computer"));
		CHECK(wf_path_host_invalid(L"\\\\SFTP.INVALID\\host\\a"));
		CHECK(!wf_path_host_invalid(L"\\\\server\\share"));
		CHECK(!wf_path_host_invalid(L"\\\\.invalid\\x"));
		CHECK(!wf_path_host_invalid(L"\\\\?\\UNC\\a.invalid\\b"));
		CHECK(!wf_path_host_invalid(L"C:\\invalid"));
		CHECK(WF_E_INVAL == wf_watch_open(WF_PCROOT, &w) && !w);
		CHECK(WF_E_INVAL ==
		                wf_watch_open(L"\\\\sftp.invalid\\host", &w) &&
		        !w);
	}
	return TEST_RESULT();
}
