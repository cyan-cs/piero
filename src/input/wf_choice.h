#ifndef WF_CHOICE_H
#define WF_CHOICE_H

#include <stddef.h>
#include <stdint.h>

#include "../ui/wf_event.h"

/* Short prompts with a cancel default. Arrow keys move the selection; Enter
 * answers with it. Esc and Ctrl+C choose cancel.
 *
 * Direct answers match configured ASCII letters, ignoring ASCII case.
 * VK_PROCESSKEY events and unmatched characters are ignored. The event
 * interface cannot distinguish committed ASCII from other ASCII input.
 */

#define WF_CHOICE_MAX 4

typedef struct
{
	char letters
	        [WF_CHOICE_MAX]; /* lower-case answer letters, in the order shown */
	uint8_t n;
	uint8_t sel;    /* the selected option, the one Enter answers with */
	uint8_t cancel; /* the option Esc answers with; also where sel starts */
} WfChoice;

typedef enum
{
	WF_CHOICE_IDLE,  /* not for this question */
	WF_CHOICE_MOVED, /* the selection changed: redraw */
	WF_CHOICE_ANSWER /* answered: the letter is in *answer */
} WfChoiceStep;

/* `letters` is 1..WF_CHOICE_MAX lower-case letters; `cancel` indexes the one
 * that cancels. An invalid length or cancel index leaves an empty choice.
 * The caller supplies the lower-case letters. */
void wf_choice_begin(WfChoice *c, const char *letters, size_t cancel);

WfChoiceStep wf_choice_event(WfChoice *c, const WfEvent *ev, char *answer);

/* The selected option's letter, 0 for an empty choice. */
char wf_choice_selected(const WfChoice *c);

/* The virtual key a terminal reports while an IME handles the key. */
#define WF_VK_PROCESSKEY 0xE5

#endif
