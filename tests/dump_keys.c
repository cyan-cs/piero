/*
 * Prints the keys of every profile from the command table and the keymaps
 * themselves. The keybindings guides use this reference generator instead
 * of maintaining a copied table. User configuration is not loaded.
 *
 *   dump_keys            all profiles
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/wf_ekeymap.h"
#include "i18n/wf_i18n.h"
#include "input/wf_keymap.h"
#include "base/wf_macro.h"

typedef struct
{
	const char *title;
	WfProfile profile;
	WfBase base;
} Profile;

static void context(const WfKeymap *m, WfKeyContext ctx)
{
	const WfCommandInfo *list[256];
	size_t n = wf_cmd_list(ctx, list, 256), i;

	printf("\n  %s  [%s]\n", wf_msg(wf_context_label(ctx)),
	        wf_context_name(ctx));
	for (i = 0; i < n; i++) {
		char keys[96];

		keys[0] = '\0';
		(void)wf_keymap_keys_text(
		        m, ctx, list[i]->id, keys, sizeof keys);
		printf("    %-26s %-44s %s\n", keys[0] ? keys : "(none)",
		        wf_msg(list[i]->desc), list[i]->name);
	}
}

int main(void)
{
	static const Profile profiles[] = {
	        {"Standard (no profile chosen: the keys as they always were)",
	                WF_PROFILE_LEGACY, WF_BASE_NANO},
	        {"Nano", WF_PROFILE_NANO, WF_BASE_NANO},
	        {"Vim", WF_PROFILE_VIM, WF_BASE_VIM}};
	size_t p;

	wf_i18n_set_language(WF_UILANG_EN);
	for (p = 0; p < WF_ARRAY_LEN(profiles); p++) {
		WfKeyConfig cfg;
		WfKeymap *m = NULL;

		wf_keyconfig_init(&cfg);
		cfg.profile = profiles[p].profile;
		if (WF_OK != wf_keymap_build_defaults(&cfg, WF_KM_PIERO, &m)) {
			return 1;
		}
		printf("\n== %s ==\n", profiles[p].title);
		context(m, WF_CTX_FM);
		context(m, WF_CTX_PREVIEW);
		if (wf_keymap_modal(m)) {
			context(m, WF_CTX_VIM_NORMAL);
			context(m, WF_CTX_VIM_INSERT);
		} else {
			context(m, WF_CTX_EDIT);
		}
		wf_keymap_free(m);
	}
	return 0;
}
