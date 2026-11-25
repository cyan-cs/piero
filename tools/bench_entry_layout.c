/* Opt-in evaluation: is a compact entry with a name arena worth replacing
 * WfEntry (two allocations an entry, a stored display string) with?
 *
 * OLD  = the real wf_dir_add / wf_dir_sort of the product.
 * NEW  = a prototype in this file only: fixed-size entries, one arena for the
 *        UTF-16 names, sorting an index array. The File ID stays in the entry
 *        (operations validate against it) and the sanitized display string is
 *        made for the visible rows only.
 * Same names, same comparator (wf_natcmp, directories first, ordinal tie),
 * same process, medians of REPS runs. Nothing here touches the product.
 * Creates no file. */
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "fs/wf_dir.h"

#define REPS 7u
#define VISIBLE_ROWS 60u

typedef struct
{
	uint64_t size, mtime, ctime;
	uint32_t attributes, flags, reparse_tag;
	uint32_t name_off;
	uint16_t name_len;
	uint8_t meta_known;
	WfFileId identity; /* kept: file operations validate against it */
} Compact;

typedef struct
{
	Compact *e;
	uint32_t *order;
	wchar_t *arena;
	size_t n, arena_n, arena_cap;
} CompactDir;

typedef enum
{
	NAMES_SHORT,
	NAMES_LONG,
	NAMES_JAPANESE
} NameSet;

static LARGE_INTEGER freq;

static double now_ms(void)
{
	LARGE_INTEGER t;

	QueryPerformanceCounter(&t);
	return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return (x > y) - (x < y);
}

static double median(double *v, size_t n)
{
	qsort(v, n, sizeof *v, cmp_double);
	return v[n / 2];
}

static size_t private_bytes(void)
{
	PROCESS_MEMORY_COUNTERS_EX m = {0};

	m.cb = sizeof m;
	GetProcessMemoryInfo(
	        GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&m, sizeof m);
	return (size_t)m.PrivateUsage;
}

static void make_name(NameSet set, size_t i, size_t n, wchar_t *out, size_t cap)
{
	/* reversed numbering: the input is NOT already sorted */
	size_t k = n - i;

	switch (set) {
	case NAMES_SHORT: swprintf(out, cap, L"file%06zu.txt", k); break;
	case NAMES_LONG:
		swprintf(out, cap,
		        L"a-fairly-long-document-name-that-goes-on-and-on-and-"
		        L"on-"
		        L"and-on-and-on-and-on-and-on-and-on-and-on-and-on-and-"
		        L"on-"
		        L"%06zu-final-version-really-final.docx",
		        k);
		break;
	default:
		swprintf(out, cap,
		        L"\x6587\x66F8\x30D5\x30A1\x30A4\x30EB\x305D\x306E%06zu"
		        L"\x6700\x7D42\x7248.txt",
		        k);
		break;
	}
}

static WfDir *old_build(NameSet set, size_t n)
{
	WfDir *d = wf_dir_new();
	wchar_t name[256];

	for (size_t i = 0; d && i < n; i++) {
		make_name(set, i, n, name, 256);
		if (WF_OK !=
		        wf_dir_add(d, name, wcslen(name),
		                i % 17 == 0 ? WF_ENT_DIR : 0,
		                (uint64_t)(i * 2654435761u) & 0xffffff,
		                (uint64_t)(i * 40503u))) {
			wf_dir_free(d);
			return NULL;
		}
	}
	return d;
}

static void compact_free(CompactDir *c)
{
	free(c->e);
	free(c->order);
	free(c->arena);
	memset(c, 0, sizeof *c);
}

static bool compact_build(CompactDir *c, NameSet set, size_t n)
{
	wchar_t name[256];

	memset(c, 0, sizeof *c);
	c->e = malloc(n * sizeof *c->e);
	c->order = malloc(n * sizeof *c->order);
	c->arena_cap = n * 24; /* grows below if the names are longer */
	c->arena = malloc(c->arena_cap * sizeof *c->arena);
	if (!c->e || !c->order || !c->arena) {
		compact_free(c);
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		size_t len;
		Compact *e = &c->e[i];

		make_name(set, i, n, name, 256);
		len = wcslen(name);
		if (c->arena_n + len + 1 > c->arena_cap) {
			size_t cap = c->arena_cap * 2 + len + 1;
			wchar_t *g = realloc(c->arena, cap * sizeof *g);

			if (!g) {
				compact_free(c);
				return false;
			}
			c->arena = g;
			c->arena_cap = cap;
		}
		memcpy(c->arena + c->arena_n, name, (len + 1) * sizeof *name);
		memset(e, 0, sizeof *e);
		e->name_off = (uint32_t)c->arena_n;
		e->name_len = (uint16_t)len;
		e->flags = i % 17 == 0 ? WF_ENT_DIR : 0;
		e->size = (uint64_t)(i * 2654435761u) & 0xffffff;
		e->mtime = (uint64_t)(i * 40503u);
		c->arena_n += len + 1;
		c->order[i] = (uint32_t)i;
		c->n++;
	}
	return true;
}

static const wchar_t *cname(const CompactDir *c, uint32_t i)
{
	return c->arena + c->e[i].name_off;
}

static int ordinal(const wchar_t *a, const wchar_t *b)
{
	for (; *a && *a == *b; a++, b++) {
	}
	return *a == *b ? 0 : ((uint16_t)*a < (uint16_t)*b ? -1 : 1);
}

typedef struct
{
	const CompactDir *c;
	int key; /* 0 name, 1 size, 2 date */
} SortCtx;

static int compact_cmp(const SortCtx *x, uint32_t ia, uint32_t ib)
{
	const Compact *a = &x->c->e[ia], *b = &x->c->e[ib];
	int ad = 0 != (a->flags & WF_ENT_DIR),
	    bd = 0 != (b->flags & WF_ENT_DIR);
	int r = 0;

	if (ad != bd) {
		return ad ? -1 : 1;
	}
	if (1 == x->key) {
		r = (a->size > b->size) - (a->size < b->size);
	} else if (2 == x->key) {
		r = (a->mtime > b->mtime) - (a->mtime < b->mtime);
	}
	if (!r) {
		r = wf_natcmp(cname(x->c, ia), cname(x->c, ib));
	}
	if (!r) {
		r = ordinal(cname(x->c, ia), cname(x->c, ib));
	}
	return r;
}

/* Stable top-down merge sort of the index array (same algorithm as the
 * product's, on 4-byte indices instead of whole entries). */
static void imsort(uint32_t *a, uint32_t *tmp, size_t n, const SortCtx *x)
{
	size_t mid = n / 2, i = 0, j = mid, k = 0;

	if (n < 2) {
		return;
	}
	imsort(a, tmp, mid, x);
	imsort(a + mid, tmp, n - mid, x);
	while (i < mid && j < n) {
		tmp[k++] = compact_cmp(x, a[j], a[i]) < 0 ? a[j++] : a[i++];
	}
	while (i < mid) {
		tmp[k++] = a[i++];
	}
	while (j < n) {
		tmp[k++] = a[j++];
	}
	memcpy(a, tmp, n * sizeof *a);
}

static bool compact_sort(CompactDir *c, int key)
{
	SortCtx x = {c, key};
	uint32_t *tmp = malloc(c->n * sizeof *tmp);

	if (!tmp) {
		return false;
	}
	for (size_t i = 0; i < c->n; i++) {
		c->order[i] = (uint32_t)i;
	}
	imsort(c->order, tmp, c->n, &x);
	free(tmp);
	return true;
}

/* The display string for the visible rows only. */
static size_t compact_visible(const CompactDir *c)
{
	size_t bytes = 0;

	for (uint32_t r = 0; r < VISIBLE_ROWS && r < c->n; r++) {
		const wchar_t *name = cname(c, c->order[r]);
		int u8 = WideCharToMultiByte(
		        CP_UTF8, 0, name, -1, NULL, 0, NULL, NULL);

		bytes += u8 > 0 ? (size_t)u8 : 0;
	}
	return bytes;
}

static WfSortKey old_key(int key)
{
	return 0 == key ? WF_SORT_NAME : 1 == key ? WF_SORT_SIZE : WF_SORT_DATE;
}

static const char *set_name(NameSet s)
{
	return NAMES_SHORT == s   ? "short_ascii"
	        : NAMES_LONG == s ? "long_120"
	                          : "japanese";
}

int wmain(void)
{
	static const size_t sizes[] = {1000, 10000, 100000};
	static const NameSet sets[] = {NAMES_SHORT, NAMES_LONG, NAMES_JAPANESE};

	QueryPerformanceFrequency(&freq);
	printf("sizeof(WfEntry)=%zu  sizeof(prototype Compact)=%zu  "
	       "(Compact keeps the 32-byte File ID)\n",
	        sizeof(WfEntry), sizeof(Compact));
	printf("%-12s %7s | %-34s | %-34s\n", "names", "n",
	        "OLD build_ms  KiB/entry  sort_name/size/date_ms",
	        "NEW build_ms  KiB/entry  sort_name/size/date_ms");
	for (size_t si = 0; si < 3; si++) {
		for (size_t zi = 0; zi < 3; zi++) {
			NameSet set = sets[si];
			size_t n = sizes[zi];
			double ob[REPS], os[3][REPS], nb[REPS], ns[3][REPS];
			size_t old_mem = 0, new_mem = 0;

			for (unsigned r = 0; r < REPS; r++) {
				size_t before = private_bytes();
				double t = now_ms();
				WfDir *d = old_build(set, n);

				ob[r] = now_ms() - t;
				if (!d) {
					return 1;
				}
				if (!r) {
					old_mem = private_bytes() - before;
				}
				for (int k = 0; k < 3; k++) {
					/* every sort starts from the unsorted order */
					WfDir *u = k ? old_build(set, n) : d;

					t = now_ms();
					wf_dir_sort(u, old_key(k), false);
					os[k][r] = now_ms() - t;
					if (k) {
						wf_dir_free(u);
					}
				}
				wf_dir_free(d);

				CompactDir c;

				before = private_bytes();
				t = now_ms();
				if (!compact_build(&c, set, n)) {
					return 1;
				}
				nb[r] = now_ms() - t;
				if (!r) {
					new_mem = private_bytes() - before;
				}
				for (int k = 0; k < 3; k++) {
					t = now_ms();
					compact_sort(&c, k);
					ns[k][r] = now_ms() - t;
				}
				(void)compact_visible(&c);
				compact_free(&c);
			}
			printf("%-12s %7zu | %8.2f %9.3f   %7.2f/%6.2f/%6.2f | "
			       "%8.2f %9.3f   %7.2f/%6.2f/%6.2f\n",
			        set_name(set), n, median(ob, REPS),
			        (double)old_mem / (double)n / 1024.0,
			        median(os[0], REPS), median(os[1], REPS),
			        median(os[2], REPS), median(nb, REPS),
			        (double)new_mem / (double)n / 1024.0,
			        median(ns[0], REPS), median(ns[1], REPS),
			        median(ns[2], REPS));
		}
	}
	return 0;
}
