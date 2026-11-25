/*
 * Writes environment.json and a run.json skeleton for a run performed by a
 * HUMAN (manual terminal smoke tests). The skeleton's result is BLOCKED: it
 * only becomes PASS/FAIL when a person fills in result.json and finalizes it
 * with run_manual_smoke.ps1 -Finalize. Nothing here claims a test was done.
 *
 *   p0_env --dir <evidence dir> --test P0-T01 --run-id <id> --fs <name>
 *          --hyp H-023,H-024 --decisions D-023 --notes "<text>" [--target
 * <path>]
 */
#include <string.h>

#include "p0_common.h"

int wmain(int argc, wchar_t **argv)
{
	wchar_t dir[MAX_PATH * 2] = L"", target[MAX_PATH * 2] = L".",
	                       p[MAX_PATH * 2];
	char test[32] = "", run_id[96] = "", fs[32] = "n/a", notes[512] = "",
	     hyp_s[128] = "", dec_s[128] = "", build[64] = "p0_env-1.0";
	const char *hyp[16], *dec[16];
	size_t nh = 0, nd = 0;
	char *tok, *ctx;
	int i;

	for (i = 1; i + 1 < argc; i += 2) {
		const wchar_t *k = argv[i], *v = argv[i + 1];

		if (!wcscmp(k, L"--dir")) {
			wcscpy_s(dir, MAX_PATH * 2, v);
		} else if (!wcscmp(k, L"--target")) {
			wcscpy_s(target, MAX_PATH * 2, v);
		} else if (!wcscmp(k, L"--test")) {
			WideCharToMultiByte(CP_UTF8, 0, v, -1, test,
			        sizeof test, NULL, NULL);
		} else if (!wcscmp(k, L"--run-id")) {
			WideCharToMultiByte(CP_UTF8, 0, v, -1, run_id,
			        sizeof run_id, NULL, NULL);
		} else if (!wcscmp(k, L"--fs")) {
			WideCharToMultiByte(
			        CP_UTF8, 0, v, -1, fs, sizeof fs, NULL, NULL);
		} else if (!wcscmp(k, L"--hyp")) {
			WideCharToMultiByte(CP_UTF8, 0, v, -1, hyp_s,
			        sizeof hyp_s, NULL, NULL);
		} else if (!wcscmp(k, L"--decisions")) {
			WideCharToMultiByte(CP_UTF8, 0, v, -1, dec_s,
			        sizeof dec_s, NULL, NULL);
		} else if (!wcscmp(k, L"--notes")) {
			WideCharToMultiByte(CP_UTF8, 0, v, -1, notes,
			        sizeof notes, NULL, NULL);
		}
	}
	if (!dir[0] || !test[0] || !run_id[0]) {
		fputs("usage: p0_env --dir <d> --test <id> --run-id <id> [--fs "
		      "x] [--hyp a,b]"
		      " [--decisions a,b] [--notes t] [--target path]\n",
		        stderr);
		return 2;
	}
	for (tok = strtok_s(hyp_s, ",", &ctx); tok && nh < 15;
	        tok = strtok_s(NULL, ",", &ctx)) {
		hyp[nh++] = tok;
	}
	hyp[nh] = NULL;
	for (tok = strtok_s(dec_s, ",", &ctx); tok && nd < 15;
	        tok = strtok_s(NULL, ",", &ctx)) {
		dec[nd++] = tok;
	}
	dec[nd] = NULL;

	p0_mkdirs(dir);
	swprintf(p, MAX_PATH * 2, L"%ls\\environment.json", dir);
	if (!p0_write_environment(p, target, "p0_env", "1.0")) {
		return 1;
	}
	swprintf(p, MAX_PATH * 2, L"%ls\\run.json", dir);
	return p0_write_run_json(
	               p, test, run_id, build, fs, hyp, "BLOCKED", dec, notes)
	        ? 0
	        : 1;
}
