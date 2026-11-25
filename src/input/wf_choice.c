#include "wf_choice.h"

#include <string.h>

void wf_choice_begin(WfChoice *c, const char *letters, size_t cancel)
{
	size_t n = letters ? strlen(letters) : 0;

	memset(c, 0, sizeof *c);
	if (n < 1 || n > WF_CHOICE_MAX || cancel >= n) {
		return;
	}
	memcpy(c->letters, letters, n);
	c->n = (uint8_t)n;
	c->cancel = c->sel = (uint8_t)cancel;
}

char wf_choice_selected(const WfChoice *c)
{
	return c->n ? c->letters[c->sel] : 0;
}

static int index_of(const WfChoice *c, uint32_t ch)
{
	uint8_t i;

	if (ch >= 'A' && ch <= 'Z') {
		ch += 'a' - 'A';
	}
	for (i = 0; i < c->n; i++) {
		if ((uint32_t)(unsigned char)c->letters[i] == ch) {
			return i;
		}
	}
	return -1;
}

WfChoiceStep wf_choice_event(WfChoice *c, const WfEvent *ev, char *answer)
{
	if (!c->n) {
		return WF_CHOICE_IDLE;
	}
	switch (ev->type) {
	case WF_EV_CHAR: {
		int i;

		/* a key the IME was handling is not that letter */
		if (WF_VK_PROCESSKEY == ev->vk ||
		        (i = index_of(c, ev->ch)) < 0) {
			return WF_CHOICE_IDLE;
		}
		*answer = c->letters[i];
		return WF_CHOICE_ANSWER;
	}
	case WF_EV_CTRL:
		if ('c' == ev->ch) { /* Ctrl+C: the same as Esc */
			*answer = c->letters[c->cancel];
			return WF_CHOICE_ANSWER;
		}
		return WF_CHOICE_IDLE;
	case WF_EV_KEY:
		switch (ev->key) {
		case WF_KEY_UP:
		case WF_KEY_LEFT:
			c->sel = (uint8_t)(c->sel ? c->sel - 1 : c->n - 1);
			return WF_CHOICE_MOVED;
		case WF_KEY_DOWN:
		case WF_KEY_RIGHT:
			c->sel = (uint8_t)(c->sel + 1 < c->n ? c->sel + 1 : 0);
			return WF_CHOICE_MOVED;
		case WF_KEY_ENTER:
			*answer = c->letters[c->sel];
			return WF_CHOICE_ANSWER;
		case WF_KEY_ESC:
			*answer = c->letters[c->cancel];
			return WF_CHOICE_ANSWER;
		default: return WF_CHOICE_IDLE;
		}
	default: return WF_CHOICE_IDLE;
	}
}
