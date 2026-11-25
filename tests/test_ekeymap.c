/* The two shipped key binding presets: every binding the design promises, and
 * that the presets differ only where they should. */
#include <string.h>

#include "edit/wf_ekeymap.h"
#include "wf_test.h"

static WfEdAction ctrl(WfKeymapId id, uint32_t c)
{
	WfEvent e = {WF_EV_CTRL, c, WF_KEY_NONE, 0};

	return wf_ekm_lookup(id, &e);
}

static WfEdAction alt(WfKeymapId id, uint32_t c)
{
	WfEvent e = {WF_EV_ALT, c, WF_KEY_NONE, 0};

	return wf_ekm_lookup(id, &e);
}

static WfEdAction key(WfKeymapId id, WfKey k)
{
	WfEvent e = {WF_EV_KEY, 0, k, 0};

	return wf_ekm_lookup(id, &e);
}

static void piero_preset(void)
{
	const WfKeymapId p = WF_KM_PIERO;
	WfEvent cs = {WF_EV_CTRL_SHIFT, 's', WF_KEY_NONE, 0};

	CHECK(WF_CMD_SAVE == ctrl(p, 's').cmd);
	CHECK(WF_CMD_SAVE_AS == wf_ekm_lookup(p, &cs).cmd);
	CHECK(WF_CMD_SAVE_AS ==
	        alt(p, 's').cmd); /* where Ctrl+Shift is not seen */
	CHECK(WF_CMD_EXIT == ctrl(p, 'x').cmd);
	CHECK(WF_CMD_EXIT == key(p, WF_KEY_ESC).cmd);
	CHECK(WF_CMD_FIND == ctrl(p, 'f').cmd);
	CHECK(WF_CMD_REPLACE == ctrl(p, 'h').cmd);
	CHECK(WF_CMD_GOTO == ctrl(p, 'g').cmd);
	CHECK(WF_CMD_UNDO == ctrl(p, 'z').cmd);
	CHECK(WF_CMD_REDO == ctrl(p, 'y').cmd);
	CHECK(WF_CMD_COPY == ctrl(p, 'c').cmd);
	CHECK(WF_CMD_PASTE == ctrl(p, 'v').cmd);
	CHECK(WF_CMD_SELECT_ALL == ctrl(p, 'a').cmd);
	CHECK(WF_CMD_DELETE_LINE == ctrl(p, 'k').cmd);
	CHECK(WF_CMD_DOC_START == key(p, WF_KEY_CTRL_HOME).cmd);
	CHECK(WF_CMD_DOC_END == key(p, WF_KEY_CTRL_END).cmd);
	CHECK(WF_CMD_WORD_LEFT == key(p, WF_KEY_CTRL_LEFT).cmd);
	CHECK(WF_CMD_WORD_RIGHT == key(p, WF_KEY_CTRL_RIGHT).cmd);
	CHECK(WF_CMD_LEFT == key(p, WF_KEY_SHIFT_LEFT).cmd &&
	        key(p, WF_KEY_SHIFT_LEFT).extend);
	CHECK(WF_CMD_WORD_RIGHT == key(p, WF_KEY_CTRL_SHIFT_RIGHT).cmd &&
	        key(p, WF_KEY_CTRL_SHIFT_RIGHT).extend);
	CHECK(WF_CMD_LEFT == key(p, WF_KEY_LEFT).cmd &&
	        !key(p, WF_KEY_LEFT).extend);
}

static void nano_preset(void)
{
	const WfKeymapId n = WF_KM_NANO;

	CHECK(WF_CMD_SAVE == ctrl(n, 's').cmd);
	CHECK(WF_CMD_SAVE_AS == ctrl(n, 'o').cmd);
	CHECK(WF_CMD_EXIT == ctrl(n, 'x').cmd);
	CHECK(WF_CMD_FIND == ctrl(n, 'f').cmd);
	CHECK(WF_CMD_FIND_BACK == ctrl(n, 'b').cmd);
	CHECK(WF_CMD_REPLACE == alt(n, 'r').cmd);
	CHECK(WF_CMD_CUT == ctrl(n, 'k').cmd);
	CHECK(WF_CMD_PASTE == ctrl(n, 'u').cmd);
	CHECK(WF_CMD_COPY == alt(n, '6').cmd);
	CHECK(WF_CMD_MARK == ctrl(n, '6').cmd);
	CHECK(WF_CMD_UNDO == alt(n, 'u').cmd);
	CHECK(WF_CMD_REDO == alt(n, 'e').cmd);
	CHECK(WF_CMD_GOTO == ctrl(n, '_').cmd);
	CHECK(WF_CMD_INSERT_FILE == ctrl(n, 'r').cmd);
	CHECK(WF_CMD_COMMENT == alt(n, '3').cmd);
	/* Esc is not "exit" here */
	CHECK(WF_CMD_CANCEL == key(n, WF_KEY_ESC).cmd);
}

static void presets_differ_where_they_should(void)
{
	/* the same key, a different meaning */
	CHECK(ctrl(WF_KM_PIERO, 'k').cmd != ctrl(WF_KM_NANO, 'k').cmd);
	CHECK(ctrl(WF_KM_PIERO, 'o').cmd != ctrl(WF_KM_NANO, 'o').cmd);
	/* the cursor keys are shared */
	CHECK(key(WF_KM_PIERO, WF_KEY_UP).cmd ==
	        key(WF_KM_NANO, WF_KEY_UP).cmd);
	CHECK(WF_CMD_PGDN == key(WF_KM_PIERO, WF_KEY_PGDN).cmd);
	/* nothing is bound to what the terminal cannot tell */
	CHECK(WF_CMD_NONE == ctrl(WF_KM_PIERO, 'q').cmd);
	CHECK(WF_CMD_NONE == alt(WF_KM_NANO, 'z').cmd);
	{
		WfEvent plain = {WF_EV_CHAR, 'a', WF_KEY_NONE, 0};

		CHECK(WF_CMD_NONE == wf_ekm_lookup(WF_KM_PIERO, &plain).cmd);
	}
	CHECK(wf_ekm_lookup(
	              WF_KM_NONE, &(WfEvent){WF_EV_CTRL, 's', WF_KEY_NONE, 0})
	                .cmd == WF_CMD_NONE);
}

static void names(void)
{
	WfKeymapId id;
	bool reserved;

	CHECK(wf_ekm_from_name("piero", &id, &reserved) && WF_KM_PIERO == id);
	CHECK(wf_ekm_from_name("nano", &id, &reserved) && WF_KM_NANO == id);
	/* vim is a reserved word, not an available preset */
	CHECK(!wf_ekm_from_name("vim", &id, &reserved) && reserved);
	CHECK(!wf_ekm_from_name("emacs", &id, &reserved) && !reserved);
	CHECK(!strcmp(wf_ekm_key_name(WF_KM_PIERO, WF_CMD_SAVE), "^S"));
	CHECK(!strcmp(wf_ekm_key_name(WF_KM_NANO, WF_CMD_UNDO), "M-U"));
	CHECK(!strcmp(wf_ekm_key_name(WF_KM_PIERO, WF_CMD_FIND_NEXT), "F3"));
	CHECK(NULL == wf_ekm_key_name(WF_KM_PIERO, WF_CMD_LEFT));
}

int main(void)
{
	piero_preset();
	nano_preset();
	presets_differ_where_they_should();
	names();
	return TEST_RESULT();
}
