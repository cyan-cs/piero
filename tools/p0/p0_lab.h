#ifndef P0_LAB_H
#define P0_LAB_H

#include "p0_area.h"

/* Test-only local DOS device rooted at a fresh, manifested scratch area.
 * No ACL changes, no skipped preflight checks. The alias is verified by ID,
 * serialized against other harnesses, and removed by exact target match. */
typedef struct
{
	P0Area *area;
	wchar_t root[1024], drive[3], target[1024];
	HANDLE mutex, pin;
	int locked, mapped;
	DWORD error;
} P0Lab;

int p0_lab_open(P0Lab *lab, const char *label, int use_drive);
int p0_lab_path(
        const P0Lab *lab, const wchar_t *relative, wchar_t *out, size_t cap);
int p0_lab_close(P0Lab *lab, int keep);

#endif
