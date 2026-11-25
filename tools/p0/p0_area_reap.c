/*
 * Cleans up a scratch area that an earlier P0 run left behind (for example a
 * crashed run), by repeating the ownership proofs from the files on disk:
 *   .p0-owner    must exist and carry a nonce and the directory's FileId,
 *   .p0-manifest lists exactly what that run created.
 * Only manifest entries are removed, one by one, in reverse order. Anything
 * else found inside is reported and left alone. A directory without a valid
 * marker is refused outright. No wildcards, no recursion.
 *
 *   p0_area_reap <area directory> [--list]
 */
#include <stdio.h>
#include <string.h>

#include "p0_area.h"

int wmain(int argc, wchar_t **argv)
{
	P0Area *a;
	wchar_t report[2048] = L"";
	int left;

	if (argc < 2) {
		fputs("usage: p0_area_reap <area directory> [--list]\n",
		        stderr);
		return 2;
	}
	a = p0_area_open(argv[1]);
	if (!a) {
		fprintf(stderr,
		        "refused: %ls is not an intact P0 scratch area "
		        "(marker/identity check failed)\n",
		        argv[1]);
		return 1;
	}
	if (argc > 2 && !wcscmp(argv[2], L"--list")) {
		printf("valid area: %ls (nothing removed)\n", p0_area_root(a));
		p0_area_forget(a);
		return 0;
	}
	left = p0_area_destroy(a, report, 2048);
	printf("reaped %ls: %d leftover(s)%ls%ls\n", argv[1], left,
	        left ? L": " : L"", report);
	return left ? 1 : 0;
}
