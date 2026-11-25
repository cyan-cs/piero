/*
 * Syntax colours in the preview pane (DR-DRAFT-033). The text is shown at once,
 * plain; if a grammar for the file is installed, the text is handed to the syntax
 * worker, and the colours are taken in when the answer for this very text
 * arrives. Nothing here parses and nothing here loads a DLL.
 */
#include <stdlib.h>
#include <string.h>

#include "wf_app.h"
#include "../syntax/wf_synlang.h"
#include "wf_app_priv.h"

/* The leaf of a path. */
static const wchar_t *leaf_of(const wchar_t *path)
{
	const wchar_t *p = wcsrchr(path, L'\\');

	return p ? p + 1 : path;
}

void wf_app_set_syntax(WfApp *app, WfSynPort port)
{
	app->syn_port = port;
}

void wf_app_syntax_preview_reset(WfApp *app)
{
	wf_synov_clear(&app->prev_ov);
	app->prev_syn_gen = 0;
	if (app->syn_port.close && app->prev_syn_open) {
		app->syn_port.close(app->syn_port.ctx, WF_SYN_SLOT_PREVIEW);
	}
	app->prev_syn_open = false;
}

/* The preview lines joined by '\n', the way the parser reads them: each line
 * begins at the offset the preview already gives it. Returns false when the
 * lines are not laid out that way (then nothing is coloured). */
static char *join_lines(const WfPreview *p, size_t *len)
{
	size_t n = 0;
	char *out;

	for (size_t i = 0; i < p->nlines; i++) {
		size_t l = strlen(p->text.data + p->lines[i]);

		if (p->lines[i] != n) {
			return NULL;
		}
		n += l + 1;
	}
	out = malloc(n ? n : 1u);
	if (!out) {
		return NULL;
	}
	for (size_t i = 0; i < p->nlines; i++) {
		const char *line = p->text.data + p->lines[i];
		size_t l = strlen(line);

		memcpy(out + p->lines[i], line, l);
		out[p->lines[i] + l] = '\n';
	}
	*len = n;
	return out;
}

void wf_app_syntax_preview_start(WfApp *app)
{
	const WfPreview *p = app->preview;
	const WfSynLang *lang;
	char *text;
	size_t len = 0;
	uint32_t gen;

	wf_app_syntax_preview_reset(app);
	if (!app->syn_port.request || !p || WF_PREVIEW_TEXT != p->kind ||
	        !app->preview_path || !p->nlines) {
		return;
	}
	lang = wf_synlang_for_name(leaf_of(app->preview_path));
	/* Markdown is drawn rendered in the pane; the editor colours its source */
	if (!lang || lang->companion || !strcmp(lang->id, "markdown")) {
		return;
	}
	if (app->syn_port.installed &&
	        !app->syn_port.installed(app->syn_port.ctx, lang->id)) {
		return;
	}
	text = join_lines(p, &len);
	if (!text) {
		return;
	}
	gen = ++app->syn_gen;
	if (!gen) {
		gen = ++app->syn_gen;
	}
	if (WF_OK ==
	        app->syn_port.request(app->syn_port.ctx, WF_SYN_SLOT_PREVIEW,
	                gen, lang->id, text, len)) {
		app->prev_syn_gen = gen;
		app->prev_syn_open = true;
		app->prev_syn_status = WF_SYN_OK;
	}
	free(text);
}

bool wf_app_on_syntax(WfApp *app, WfSynAnswer *a)
{
	if (!a || WF_SYN_SLOT_PREVIEW != a->slot) {
		return false;
	}
	if (a->gen != app->prev_syn_gen || !app->preview) {
		wf_synanswer_free(a);
		return true; /* a result for a file that is no longer shown */
	}
	app->prev_syn_status = a->status;
	if (WF_SYN_OK == a->status) {
		(void)wf_synov_set(
		        &app->prev_ov, a->runs, a->nruns, NULL, 0, a->text_len);
		a->runs = NULL;
		a->nruns = 0;
	}
	wf_synanswer_free(a);
	return true;
}

/* The classes of the bytes [from, from + n) of preview line `line` (n at most
 * WF_PREVIEW_LINE_BYTES): true when the pane has colours for this text. */
bool wf_app_syntax_preview_line(
        const WfApp *app, size_t line, size_t from, size_t n, uint8_t *out)
{
	if (!app->prev_ov.active || !app->preview ||
	        line >= app->preview->nlines || !n) {
		return false;
	}
	wf_synov_fill(&app->prev_ov, app->preview->lines[line] + from, n, out);
	return true;
}
