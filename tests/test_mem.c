/* wf_malloc_array / wf_realloc_array: the product is checked before it is formed. */
#include <stdint.h>
#include <string.h>

#include "base/wf_mem.h"
#include "wf_test.h"

int main(void)
{
	void *p;
	unsigned char *q;

	/* ordinary sizes behave as malloc / realloc */
	p = wf_malloc_array(10, sizeof(uint32_t));
	CHECK(NULL != p);
	if (p) {
		memset(p, 0xAB, 10 * sizeof(uint32_t));
		p = wf_realloc_array(p, 20, sizeof(uint32_t));
		CHECK(NULL != p);
		if (p) {
			q = p;
			CHECK(0xAB == q[0] && 0xAB == q[39]); /* contents kept */
		}
		free(p);
	}
	p = wf_malloc_array(0, sizeof(uint32_t)); /* like malloc(0): not an error to free */
	free(p);

	/* a product that does not fit is refused, not wrapped */
	CHECK(NULL == wf_malloc_array(SIZE_MAX / sizeof(uint64_t) + 1, sizeof(uint64_t)));
	CHECK(NULL == wf_malloc_array(SIZE_MAX, 2));
	CHECK(NULL == wf_malloc_array(SIZE_MAX / 2 + 1, 2));
	CHECK(NULL == wf_malloc_array(2, SIZE_MAX));

	/* the product 2^64 would wrap to 0 and "succeed" without the check */
	CHECK(NULL == wf_malloc_array((size_t)1 << (sizeof(size_t) * 8 - 1), 2));

	/* the largest count that fits is not refused by the CHECK itself: it is
	 * the allocator that says no, for want of memory, and the old block of a
	 * failed wf_realloc_array stays valid */
	p = wf_malloc_array(4, 8);
	CHECK(NULL != p);
	if (p) {
		memset(p, 0x5A, 32);
		CHECK(NULL == wf_realloc_array(p, SIZE_MAX / 8 + 1, 8));
		q = p;
		CHECK(0x5A == q[0] && 0x5A == q[31]); /* untouched */
		free(p);
	}

	/* a zero element size never divides by zero */
	p = wf_malloc_array(SIZE_MAX, 0);
	free(p);
	return TEST_RESULT();
}
