#include "wf_gitport.h"

#include <stdlib.h>
#include <string.h>

WfGitBlob *wf_gitblob_new(const void *bytes, size_t n)
{
	WfGitBlob *b = calloc(1, sizeof *b);

	if (!b) {
		return NULL;
	}
	b->data = malloc(n + 1);
	if (!b->data) {
		free(b);
		return NULL;
	}
	if (n) {
		memcpy(b->data, bytes, n);
	}
	b->data[n] = '\0';
	b->n = n;
	b->refs = 1;
	return b;
}

WfGitBlob *wf_gitblob_ref(WfGitBlob *b)
{
	if (b) {
		InterlockedIncrement(&b->refs);
	}
	return b;
}

void wf_gitblob_release(WfGitBlob *b)
{
	if (b && 0 == InterlockedDecrement(&b->refs)) {
		free(b->data);
		free(b);
	}
}

void wf_gitanswer_free(WfGitAnswer *a)
{
	if (a) {
		free(a->asked);
		free(a->root);
		wf_gitstatus_free(&a->table);
		wf_gitblob_release(a->base);
		wf_linediff_free(&a->diff);
		memset(a, 0, sizeof *a);
	}
}
