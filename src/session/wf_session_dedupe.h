#ifndef WF_SESSION_DEDUPE_H
#define WF_SESSION_DEDUPE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Last submitted snapshot, used only to suppress duplicates. Submission does
 * not imply disk persistence. Forget on submit/write failure so an unchanged
 * snapshot can be retried.
 */
typedef struct
{
	char *last; /* malloc'd copy; NULL: nothing is remembered */
	size_t n;
} WfSessionDedupe;

static inline bool wf_dedupe_is_repeat(
        const WfSessionDedupe *d, const char *text, size_t n)
{
	return d->last && d->n == n &&
	        (!n || (text && !memcmp(d->last, text, n)));
}

/* Copies n bytes; NULL text is allowed for n = 0. Failure (invalid size/input
 * or no memory) discards the old cache so it cannot suppress a later retry. */
static inline bool wf_dedupe_remember(
        WfSessionDedupe *d, const char *text, size_t n)
{
	char *keep = n == SIZE_MAX || (!text && n) ? NULL : malloc(n + 1);

	if (!keep) {
		free(d->last);
		d->last = NULL;
		d->n = 0;
		return false;
	}
	if (n) {
		memcpy(keep, text, n);
	}
	free(d->last);
	d->last = keep;
	d->n = n;
	return true;
}

static inline void wf_dedupe_forget(WfSessionDedupe *d)
{
	free(d->last);
	d->last = NULL;
	d->n = 0;
}

#endif
