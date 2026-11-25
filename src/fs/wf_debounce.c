#include "wf_debounce.h"
#include "../base/wf_macro.h"

static const unsigned interval_ms[] = {100, 250, 500, 1000};
#define STEPS (WF_ARRAY_LEN(interval_ms))

void wf_debounce_init(WfDebounce *d)
{
	d->pending = false;
	d->has_fired = false;
	d->step = 0;
	d->due = 0;
	d->last_fire = 0;
}

void wf_debounce_note(WfDebounce *d, uint64_t now)
{
	if (d->pending) {
		return; /* already scheduled; the refresh will see the final
		           state */
	}

	/* Notifications right after a refresh mean a busy directory: back off.
	 */
	if (d->has_fired && now - d->last_fire < 2u * interval_ms[d->step]) {
		if (d->step + 1 < STEPS) {
			d->step++;
		}
	} else {
		d->step = 0;
	}
	d->pending = true;
	d->due = now + interval_ms[d->step];
}

bool wf_debounce_take(WfDebounce *d, uint64_t now)
{
	if (!d->pending || now < d->due) {
		return false;
	}
	d->pending = false;
	d->has_fired = true;
	d->last_fire = now;
	return true;
}

uint32_t wf_debounce_timeout(const WfDebounce *d, uint64_t now)
{
	if (!d->pending) {
		return WF_WAIT_FOREVER;
	}
	return now >= d->due ? 0 : (uint32_t)(d->due - now);
}
