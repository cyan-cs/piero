#include "wf_text.h"
#include "../base/wf_macro.h"
#include <string.h>

/* Selected/combined Unicode width ranges; see LICENSES/unicode-data.txt. */

typedef struct
{
	uint32_t lo, hi;
} Range;

/* Combining marks and variation selectors. Zero-width *controls* never get
 * here: the sanitizer turns them into visible labels first. */
static const Range zero_width[] = {
        {0x0300, 0x036F},
        {0x1AB0, 0x1AFF},
        {0x1DC0, 0x1DFF},
        {0x20D0, 0x20FF},
        {0x3099, 0x309A},
        {0xFE00, 0xFE0F},
        {0xFE20, 0xFE2F},
};

/* East Asian Wide / Fullwidth, and the emoji Unicode gives East_Asian_Width=W
 * (Emoji_Presentation=Yes): the terminals this runs in (Windows Terminal) draw
 * those two cells wide (D-025 as revised). Emoji with only text presentation
 * stay one cell, as does anything after a variation selector: a terminal's own
 * choice there cannot be known. */
static const Range wide[] = {
        {0x1100, 0x115F},
        {0x2E80, 0x303E},
        {0x3041, 0x33FF},
        {0x3400, 0x4DBF},
        {0x4E00, 0x9FFF},
        {0xA000, 0xA4CF},
        {0xAC00, 0xD7A3},
        {0xF900, 0xFAFF},
        {0xFE30, 0xFE6F},
        {0xFF01, 0xFF60},
        {0xFFE0, 0xFFE6},
        {0x20000, 0x2FFFD},
        {0x30000, 0x3FFFD},
        /* emoji, East_Asian_Width=W (Unicode 15) */
        {0x231A, 0x231B},
        {0x23E9, 0x23EC},
        {0x23F0, 0x23F0},
        {0x23F3, 0x23F3},
        {0x25FD, 0x25FE},
        {0x2614, 0x2615},
        {0x2648, 0x2653},
        {0x267F, 0x267F},
        {0x2693, 0x2693},
        {0x26A1, 0x26A1},
        {0x26AA, 0x26AB},
        {0x26BD, 0x26BE},
        {0x26C4, 0x26C5},
        {0x26CE, 0x26CE},
        {0x26D4, 0x26D4},
        {0x26EA, 0x26EA},
        {0x26F2, 0x26F3},
        {0x26F5, 0x26F5},
        {0x26FA, 0x26FA},
        {0x26FD, 0x26FD},
        {0x2705, 0x2705},
        {0x270A, 0x270B},
        {0x2728, 0x2728},
        {0x274C, 0x274C},
        {0x274E, 0x274E},
        {0x2753, 0x2755},
        {0x2757, 0x2757},
        {0x2795, 0x2797},
        {0x27B0, 0x27B0},
        {0x27BF, 0x27BF},
        {0x2B1B, 0x2B1C},
        {0x2B50, 0x2B50},
        {0x2B55, 0x2B55},
        {0x1F004, 0x1F004},
        {0x1F0CF, 0x1F0CF},
        {0x1F18E, 0x1F18E},
        {0x1F191, 0x1F19A},
        {0x1F200, 0x1F202},
        {0x1F210, 0x1F23B},
        {0x1F240, 0x1F248},
        {0x1F250, 0x1F251},
        {0x1F260, 0x1F265},
        {0x1F300, 0x1F320},
        {0x1F32D, 0x1F335},
        {0x1F337, 0x1F37C},
        {0x1F37E, 0x1F393},
        {0x1F3A0, 0x1F3CA},
        {0x1F3CF, 0x1F3D3},
        {0x1F3E0, 0x1F3F0},
        {0x1F3F4, 0x1F3F4},
        {0x1F3F8, 0x1F43E},
        {0x1F440, 0x1F440},
        {0x1F442, 0x1F4FC},
        {0x1F4FF, 0x1F53D},
        {0x1F54B, 0x1F54E},
        {0x1F550, 0x1F567},
        {0x1F57A, 0x1F57A},
        {0x1F595, 0x1F596},
        {0x1F5A4, 0x1F5A4},
        {0x1F5FB, 0x1F64F},
        {0x1F680, 0x1F6C5},
        {0x1F6CC, 0x1F6CC},
        {0x1F6D0, 0x1F6D2},
        {0x1F6D5, 0x1F6D7},
        {0x1F6DC, 0x1F6DF},
        {0x1F6EB, 0x1F6EC},
        {0x1F6F4, 0x1F6FC},
        {0x1F7E0, 0x1F7EB},
        {0x1F7F0, 0x1F7F0},
        {0x1F90C, 0x1F93A},
        {0x1F93C, 0x1F945},
        {0x1F947, 0x1F9FF},
        {0x1FA70, 0x1FA7C},
        {0x1FA80, 0x1FA88},
        {0x1FA90, 0x1FABD},
        {0x1FABF, 0x1FAC5},
        {0x1FACE, 0x1FADB},
        {0x1FAE0, 0x1FAE8},
        {0x1FAF0, 0x1FAF8},
};

/* East_Asian_Width=A: one cell, unless the terminal draws them wide. */
#include "wf_width_ambiguous.h"

/* Cells an ambiguous character takes: 1 or 2. Set once at start, before any
 * thread measures text. */
static int ambiguous_cells = 1;

void wf_set_ambiguous_width(int cells)
{
	ambiguous_cells = 2 == cells ? 2 : 1;
}

/* A binary search: `t` is sorted and does not overlap. */
static bool in_sorted_table(const Range *t, size_t n, uint32_t cp)
{
	size_t lo = 0, hi = n;

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;

		if (cp < t[mid].lo) {
			hi = mid;
		} else if (cp > t[mid].hi) {
			lo = mid + 1;
		} else {
			return true;
		}
	}
	return false;
}

static bool in_table(const Range *t, size_t n, uint32_t cp)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (cp >= t[i].lo && cp <= t[i].hi) {
			return true;
		}
	}
	return false;
}

int wf_cp_width(uint32_t cp)
{
	if (cp < 0x300) {
		return cp >= 0xA1 && 2 == ambiguous_cells &&
		                in_sorted_table(
		                        ambiguous, WF_ARRAY_LEN(ambiguous), cp)
		        ? 2
		        : 1;
	}
	if (in_table(zero_width, WF_ARRAY_LEN(zero_width), cp)) {
		return 0;
	}
	if (in_table(wide, WF_ARRAY_LEN(wide), cp)) {
		return 2;
	}
	if (2 == ambiguous_cells &&
	        in_sorted_table(ambiguous, WF_ARRAY_LEN(ambiguous), cp)) {
		return 2;
	}
	return 1;
}

static bool stays_with_previous(uint32_t cp)
{
	return 0 == wf_cp_width(cp) || (cp >= 0x1F3FBu && cp <= 0x1F3FFu);
}

size_t wf_utf8_cluster_next(const char *s, size_t n, size_t at)
{
	uint32_t cp;
	size_t k;

	if (at >= n) {
		return n;
	}
	k = wf_utf8_decode(s + at, n - at, &cp);
	at += k ? k : 1;
	while (at < n) {
		k = wf_utf8_decode(s + at, n - at, &cp);
		if (!k || !stays_with_previous(cp)) {
			break;
		}
		at += k;
	}
	return at;
}

size_t wf_utf8_cluster_prev(const char *s, size_t n, size_t at)
{
	uint32_t cp;

	if (at > n) {
		at = n;
	}
	if (0 == at) {
		return 0;
	}
	do {
		size_t end = at, start = at - 1;

		while (start > 0 && end - start < 4 &&
		        0x80 == ((unsigned char)s[start] & 0xC0)) {
			start--;
		}
		if (wf_utf8_decode(s + start, end - start, &cp) !=
		        end - start) {
			at = end - 1;
			cp = WF_CP_REPLACEMENT;
		} else {
			at = start;
		}
	} while (at > 0 && stays_with_previous(cp));
	return at;
}

int wf_utf8_cells(const char *s, size_t n)
{
	int cells = 0;

	if (!s && n) {
		return 0;
	}
	while (n > 0) {
		uint32_t cp;
		/* ASCII has the same one-cell policy as wf_cp_width, including
		 * control bytes (sanitizing belongs to the caller). */
		if ((unsigned char)*s < 0x80) {
			cells++;
			s++;
			n--;
			continue;
		}
		size_t k = wf_utf8_decode(s, n, &cp);

		cells += wf_cp_width(cp);
		if (0 == k) {
			break;
		}
		s += k;
		n -= k;
	}
	return cells;
}

void wf_utf8_elide(char *out, size_t cap, const char *s, size_t cells)
{
	size_t n = strlen(s), at = 0, used = 0, budget;
	bool shortened;
	if (!cap) {
		return;
	}
	budget = cap - 1;
	for (;;) {
		at = used = 0;
		while (at < n) {
			uint32_t cp;
			size_t k = wf_utf8_decode(s + at, n - at, &cp);
			int width = wf_cp_width(cp);
			size_t w = width > 0 ? (size_t)width : 0;
			if (!k || k > budget - at || w > cells - used) {
				break;
			}
			at += k;
			used += w;
		}
		shortened = at < n;
		if (!shortened || budget != cap - 1 || cap < 4 || cells < 3) {
			break;
		}
		budget -= 3;
		cells -= 3;
	}
	memcpy(out, s, at);
	if (shortened && cap >= 4 && budget == cap - 4) {
		memcpy(out + at, "...", 3);
		at += 3;
	}
	out[at] = 0;
}
