#ifndef WF_MEM_H
#define WF_MEM_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/*
 * Arrays whose element count is a run-time value. `n_elements * element_bytes`
 * is checked before it is formed: when the product does not fit in size_t the
 * result is NULL exactly as for an allocation that failed, so the callers'
 * out-of-memory path is also the overflow path and nothing is allocated.
 *
 * wf_realloc_array keeps the old block alive on failure, as realloc does.
 * A count of 0 behaves like malloc(0)/realloc(p, 0) of the C library.
 *
 * (The parameters are not called "count" or "size": tests that replace malloc
 * define globals of those names, and -Wshadow is an error here.)
 */
static inline void *wf_malloc_array(size_t n_elements, size_t element_bytes)
{
	if (element_bytes && n_elements > SIZE_MAX / element_bytes) {
		return NULL;
	}
	return malloc(n_elements * element_bytes);
}

static inline void *wf_realloc_array(
        void *block, size_t n_elements, size_t element_bytes)
{
	if (element_bytes && n_elements > SIZE_MAX / element_bytes) {
		return NULL;
	}
	return realloc(block, n_elements * element_bytes);
}

#endif
