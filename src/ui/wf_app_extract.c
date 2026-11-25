/*
 * Taking items out of a ZIP (y inside the archive, p in a folder): the picked
 * paths are kept as text, and `p` in a real folder starts an EXTRACT job for
 * them on the copy worker. Nothing is read here: the archive is read, checked
 * and written by the job, and every name an item has in the archive is checked
 * again there. A pick is a copy; x inside an archive stays refused.
 */
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../archive/wf_vpath.h"
#include "../base/wf_mem.h"
#include "../path/wf_path.h"
#include "../text/wf_text.h"
#include "wf_app_priv.h"

void wf_app_extract_drop(WfApp *app)
{
	for (size_t i = 0; i < app->arc_pick.n; i++) {
		free(app->arc_pick.inner[i]);
		free(app->arc_pick.leaf[i]);
	}
	free(app->arc_pick.inner);
	free(app->arc_pick.leaf);
	free(app->arc_pick.archive);
	memset(&app->arc_pick, 0, sizeof app->arc_pick);
}

/* y inside an archive: the selected entries on screen, or the one under the
 * cursor, are what a later p extracts. */
void wf_app_extract_pick(WfApp *app)
{
	wchar_t *archive = NULL;
	const wchar_t *inner;
	const wchar_t **names;
	size_t n, hidden, i;
	char m[96];

	if (!wf_vpath_split(app->cwd, &archive, &inner)) {
		return;
	}
	names = calloc(app->cur.nvis ? app->cur.nvis : 1, sizeof *names);
	if (!names) {
		free(archive);
		wf_app_set_msg(app, wf_msg(MSG_XFER_NOMEM));
		return;
	}
	if (!wf_app_collect_targets(app, names, &n, &hidden)) {
		free(names);
		free(archive);
		return;
	}
	wf_app_extract_drop(app);
	app->arc_pick.inner = calloc(n, sizeof *app->arc_pick.inner);
	app->arc_pick.leaf = calloc(n, sizeof *app->arc_pick.leaf);
	app->arc_pick.archive = archive;
	if (!app->arc_pick.inner || !app->arc_pick.leaf) {
		free(names);
		wf_app_extract_drop(app);
		wf_app_set_msg(app, wf_msg(MSG_XFER_NOMEM));
		return;
	}
	for (i = 0; i < n; i++) {
		size_t len = wcslen(inner) + 1 + wcslen(names[i]) + 1;
		wchar_t *p = calloc(len, sizeof *p);
		wchar_t *leaf = wf_path_dup(names[i]);

		if (!p || !leaf) {
			free(p);
			free(leaf);
			break; /* this item is not in the pick: n stays what is whole */
		}
		swprintf(p, len, L"%ls%ls%ls", inner, *inner ? L"\\" : L"",
		        names[i]);
		app->arc_pick.inner[i] = p;
		app->arc_pick.leaf[i] = leaf;
		app->arc_pick.n = i + 1;
	}
	free(names);
	if (app->arc_pick.n != n) {
		wf_app_extract_drop(app); /* frees the n items that are whole */
		wf_app_set_msg(app, wf_msg(MSG_XFER_NOMEM));
		return;
	}
	/* a pick from an archive replaces any pick of real files */
	(void)wf_app_xfer_drop_slot(app);
	(void)wf_msgf(m, sizeof m,
	        wf_msg_count(
	                MSG_AR_PICKED_ONE, MSG_AR_PICKED_OTHER, (long long)n),
	        &WF_ARG_N(n), 1);
	wf_app_set_msg(app, m);
}

static void extract_go(WfApp *app, WfCollide how)
{
	(void)wf_app_extract_start(app, how);
}

static void extract_skip(WfApp *app, void *ctx)
{
	(void)ctx;
	extract_go(app, WF_COLLIDE_SKIP);
}

static void extract_keep_both(WfApp *app, void *ctx)
{
	(void)ctx;
	extract_go(app, WF_COLLIDE_RENAME);
}

static void extract_overwrite(WfApp *app, void *ctx)
{
	(void)ctx;
	extract_go(app, WF_COLLIDE_OVERWRITE);
}

static int name_cmp(const void *a, const void *b)
{
	return _wcsicmp(*(const wchar_t *const *)a, *(const wchar_t *const *)b);
}

/* A few picks against a short listing are compared one by one; many against a
 * long one are looked up in the listing's names, sorted once by the same rule
 * (case ignored). The engine still has the last word on every name. */
#define TAKEN_SORT_MIN_WORK 4096u

/* The first picked item whose name is in the loaded directory, or NULL.
 * All entries are checked, including those hidden by the view's filter;
 * collisions at execution time follow the job's selected policy. */
static const wchar_t *first_taken(const WfApp *app)
{
	const WfDir *dir = app->cur.dir;
	const wchar_t **sorted = NULL;
	const wchar_t *found = NULL;

	if (!dir || !app->arc_pick.n) {
		return NULL;
	}
	if ((uint64_t)app->arc_pick.n * dir->n >= TAKEN_SORT_MIN_WORK) {
		sorted = wf_malloc_array(dir->n, sizeof *sorted);
	}
	if (sorted) {
		for (size_t k = 0; k < dir->n; k++) {
			sorted[k] = dir->e[k].name;
		}
		qsort(sorted, dir->n, sizeof *sorted, name_cmp);
	}
	for (size_t i = 0; i < app->arc_pick.n && !found; i++) {
		const wchar_t *leaf = app->arc_pick.leaf[i];

		if (!leaf) {
			continue;
		}
		if (sorted) {
			if (bsearch(&leaf, sorted, dir->n, sizeof *sorted,
			            name_cmp)) {
				found = leaf;
			}
			continue;
		}
		for (size_t k = 0; k < dir->n; k++) {
			if (!_wcsicmp(dir->e[k].name, leaf)) {
				found = leaf;
				break;
			}
		}
	}
	free(sorted);
	return found;
}

static void offer_collision(WfApp *app, const wchar_t *name)
{
	char l1[WF_CONFIRM_LINE_BYTES], shown[64];
	const char *lines[7];
	WfBuf b;
	WfStatus st;

	wf_buf_init(&b);
	(void)wf_sanitize_utf16(name, wcslen(name), &b);
	wf_utf8_elide(shown, sizeof shown, wf_buf_cstr(&b), 46);
	wf_buf_free(&b);
	(void)wf_msgf(
	        l1, sizeof l1, MSG_XFER_ALREADY_THERE, &WF_ARG_S(shown), 1);
	lines[0] = wf_msg(MSG_JOB_VERB_COPY);
	lines[1] = l1;
	lines[2] = wf_msg(MSG_XFER_COLLIDE_1);
	lines[3] = wf_msg(MSG_XFER_COLLIDE_2);
	lines[4] = wf_msg(MSG_XFER_COLLIDE_3);
	lines[5] = wf_msg(MSG_XFER_COLLIDE_4);
	lines[6] = wf_msg(MSG_XFER_COLLIDE_5);
	st = wf_app_confirm_begin_alt2(app, lines, 7, 's', 'k', 'o',
	        wf_msg(MSG_XFER_COLLIDE_KEYS), extract_skip, extract_keep_both,
	        extract_overwrite, NULL);
	if (WF_OK != st) {
		wf_app_set_error(app, st);
	}
}

/* p with items picked in an archive. True when this took the key. */
bool wf_app_extract_paste(WfApp *app)
{
	const wchar_t *taken;

	if (!app->arc_pick.n) {
		return false;
	}
	if (wf_app_archive_guard(app)) {
		return true; /* into an archive, or any place that is read only */
	}
	if (app->want_path || !app->cur.dir) {
		wf_app_set_msg(app, wf_msg(MSG_XFER_STALE_LISTING));
		return true;
	}
	taken = first_taken(app);
	if (taken) {
		offer_collision(app, taken);
		return true;
	}
	(void)wf_app_extract_start(app, WF_COLLIDE_STOP);
	return true;
}
