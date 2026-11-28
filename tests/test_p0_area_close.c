#include <windows.h>
#include <stdio.h>
#include <share.h>

#include "wf_test.h"

static FILE *reader;
static unsigned closes;
static int observe_closes;

static FILE *open_reader(const wchar_t *path, const wchar_t *mode, int share)
{
	if (!observe_closes) {
		return _wfsopen(path, mode, share);
	}
	reader = _wfsopen(path, mode, share);
	closes = 0;
	return reader;
}

static int close_reader(FILE *f)
{
	if (observe_closes && f == reader) {
		/* Defer the real close so a broken path is observable without UB. */
		if (++closes > 1) {
			CHECK_FAIL("marker stream closed twice");
			return EOF;
		}
		return 0;
	}
	return fclose(f);
}

#define _wfsopen open_reader
#define fclose close_reader
#include "../tools/p0/p0_area.c"
#undef fclose
#undef _wfsopen

int main(void)
{
	wchar_t temp[MAX_PATH], path[MAX_PATH * 2];
	char id[64];
	DWORD temp_n = GetTempPathW(MAX_PATH, temp);
	if (!temp_n || temp_n >= MAX_PATH) {
		CHECK_FAIL("temporary path unavailable or too long");
		return TEST_RESULT();
	}
	snprintf(id, sizeof id, "close-%lu", GetCurrentProcessId());
	P0Area *a = p0_area_create(temp, id);
	CHECK(a);
	if (!a) {
		return TEST_RESULT();
	}
	CHECK(swprintf(path, MAX_PATH * 2, L"%ls\\%ls", a->root, MARKER) > 0);
	const char *bad_ids[] = {"",
	        "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz",
	        "000000000000000000000000000000000000000000000000"};
	for (size_t i = 0; i < sizeof bad_ids / sizeof *bad_ids; i++) {
		FILE *f = p0_fopen_w(path);
		CHECK(f);
		if (!f) {
			break;
		}
		fprintf(f, "nonce=%s\nroot_id=%s\n", i ? a->nonce : "bad",
		        bad_ids[i]);
		CHECK(!fclose(f));
		observe_closes = 1;
		P0Area *opened = p0_area_open(a->root);
		CHECK(!opened);
		CHECK(closes == 1);
		if (reader) {
			CHECK(!fclose(reader));
			reader = NULL;
		}
		observe_closes = 0;
		p0_area_forget(opened);
		reader = NULL;
	}
	CHECK(write_marker(a, id));
	CHECK(!p0_area_destroy(a, NULL, 0));
	return TEST_RESULT();
}
