/* CPU-side event + frame construction, without a terminal or file operations.
 * Compile against each revision's headers AND wfcore.lib; WfApp is not an ABI.
 * No timing here claims that a terminal has painted its pixels. */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "ui/wf_app_priv.h"

#define SAMPLES 101
#define STEPS 1000

static WfStatus no_listing(
        void *ctx, uint32_t gen, const wchar_t *path, const wchar_t *parent)
{
	(void)ctx;
	(void)gen;
	(void)path;
	(void)parent;
	return WF_OK;
}

static double clock_us(void)
{
	LARGE_INTEGER count, frequency;
	QueryPerformanceCounter(&count);
	QueryPerformanceFrequency(&frequency);
	return (double)count.QuadPart * 1e6 / (double)frequency.QuadPart;
}

static int compare(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

static void deliver(WfApp *app, WfFrame *frame, WfEvent event)
{
	(void)app->mode->on_event(app, &event);
	wf_app_draw(app, frame);
}

int main(void)
{
	static const char *const names[] = {"cursor + frame", "scroll + frame",
	        "column focus + frame", "settings open/close + frame"};
	static const WfMode *const targets[] = {
	        NULL, NULL, &WF_MODE_PREVIEW_PANE, &WF_MODE_SETTINGS};
	WfApp app;
	WfConfig config;
	WfFrame *frame;
	WfDir *dir = wf_dir_new();
	int result = 1;

	if (!dir) {
		return 1;
	}
	for (unsigned i = 0; i < 4000; i++) {
		wchar_t name[32];
		swprintf(name, 32, L"file%04u.txt", i);
		if (WF_OK != wf_dir_add(dir, name, wcslen(name), 0, i, 0)) {
			goto free_dir;
		}
	}
	if (wf_app_init(&app, 160, 40, (WfFsPort){NULL, no_listing},
	            L"C:\\bench") != WF_OK) {
		goto free_dir;
	}
	wf_config_init(&config);
	wf_app_set_settings(&app, &config, (WfSettingsPort){0});
	wf_app_on_listing(&app, app.nav_gen, WF_OK, dir, NULL);
	dir = NULL; /* app owns the committed listing */
	frame = wf_frame_new(160, 40, &WF_STYLE_BASE);
	if (!frame) {
		goto destroy;
	}
	for (int kind = 0; kind < 4; kind++) {
		double samples[SAMPLES];
		for (int run = -3; run < SAMPLES; run++) {
			double start;
			app.mode = &WF_MODE_NORMAL;
			app.cur.cursor = 1000;
			app.cur.top = 1 == kind ? 966 : 990;
			start = clock_us();
			for (int i = 0; i < STEPS; i++) {
				WfEvent event = {WF_EV_KEY, 0, WF_KEY_DOWN, 0};
				if (0 == kind) {
					event.key =
					        i & 1 ? WF_KEY_UP : WF_KEY_DOWN;
				} else if (2 == kind) {
					event.key = i & 1 ? WF_KEY_ESC
					                  : WF_KEY_RIGHT;
				} else if (3 == kind) {
					event.type =
					        i & 1 ? WF_EV_KEY : WF_EV_CHAR;
					event.key = i & 1 ? WF_KEY_ESC
					                  : WF_KEY_NONE;
					event.ch = i & 1 ? 0 : ',';
				}
				deliver(&app, frame, event);
				if (0 == i && targets[kind] &&
				        app.mode != targets[kind]) {
					fprintf(stderr,
					        "benchmark did not enter its "
					        "target mode\n");
					goto free_frame;
				}
			}
			if (run >= 0) {
				samples[run] = (clock_us() - start) / STEPS;
			}
		}
		qsort(samples, SAMPLES, sizeof samples[0], compare);
		printf("%-30s median %.3f us  p95 %.3f  p99 %.3f  min %.3f  "
		       "max %.3f\n",
		        names[kind], samples[SAMPLES / 2], samples[95],
		        samples[99], samples[0], samples[SAMPLES - 1]);
	}
	result = 0;
free_frame:
	wf_frame_free(frame);
destroy:
	wf_app_destroy(&app);
	wf_config_dispose(&config);
free_dir:
	wf_dir_free(dir);
	return result;
}
