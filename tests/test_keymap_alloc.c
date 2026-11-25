/* Allocation failure must not look like an invalid user binding. */
#include <stdlib.h>
#include <string.h>

#include "input/wf_keymap.h"
#include "wf_test.h"

static size_t attempts, fail_at, owned;

static void *keymap_calloc(size_t n, size_t bytes)
{
	void *p;

	if (++attempts == fail_at) {
		return NULL;
	}
	p = calloc(n, bytes);
	if (p) {
		owned++;
	}
	return p;
}

static void *keymap_realloc(void *old, size_t bytes)
{
	bool was_null = !old;
	void *p;

	if (++attempts == fail_at) {
		return NULL;
	}
	p = realloc(old, bytes);
	if (p && was_null) {
		owned++;
	}
	return p;
}

static void keymap_release_alloc(void *p)
{
	if (p) {
		CHECK(owned > 0);
		owned--;
	}
	free(p);
}

#define calloc keymap_calloc
#define realloc keymap_realloc
#define free keymap_release_alloc
#ifndef WF_KEYMAP_SOURCE
#	define WF_KEYMAP_SOURCE "../src/input/wf_keymap.c"
#endif
#include WF_KEYMAP_SOURCE
#undef free
#undef realloc
#undef calloc

int main(void)
{
	WfKeyConfig cfg;
	WfKeymap *m = NULL;
	WfKeyIssue issue;
	size_t allocations;

	wf_keyconfig_init(&cfg);
	cfg.profile = WF_PROFILE_CUSTOM;
	/* Two distinct shortcuts per command force the override list to grow. */
	for (size_t i = 0; i < wf_cmd_total(); i++) {
		const WfCommandInfo *ci = wf_cmd_at(i);
		WfKeyOverride o = {0};

		if (!(ci->ctx_mask & WF_CTX_BIT(WF_CTX_FM))) {
			continue;
		}
		o.ctx = WF_CTX_FM;
		o.cmd = ci->id;
		o.n = 2;
		for (size_t j = 0; j < 2; j++) {
			o.seq[j].n = 1;
			o.seq[j].s[0] =
			        (WfStroke){(uint32_t)(0x400 + i * 2 + j),
			                WF_MOD_ALT, false};
		}
		CHECK(wf_keyconfig_set(&cfg, &o));
	}
	CHECK(WF_OK == wf_keymap_build(&cfg, 0, &m, &issue) && m);
	allocations = attempts;
	wf_keymap_free(m);
	CHECK(!owned);
	/* The resulting file-manager list exceeds its initial 32 bindings. */
	CHECK(cfg.count * 2 > 64);
	for (size_t failure = 1; failure <= allocations; failure++) {
		attempts = 0;
		fail_at = failure;
		m = NULL;
		CHECK(WF_E_NOMEM == wf_keymap_build(&cfg, 0, &m, &issue));
		CHECK(!m && !owned && WF_KI_NONE == issue.kind);
	}
	return TEST_RESULT();
}
