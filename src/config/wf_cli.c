#include "wf_cli.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
void wf_cli_init(WfCli *c)
{
	memset(c, 0, sizeof *c);
}
void wf_cli_dispose(WfCli *c)
{
	free(c->path);
	free(c->config_path);
	free(c->print_last_dir);
	free(c->recovery_id);
	free(c->language);
	wf_cli_init(c);
}
static bool valid_text(const wchar_t *s)
{
	if (!s || !*s) {
		return 0;
	}
	for (size_t i = 0; s[i]; i++) {
		uint32_t ch = (uint32_t)s[i];
		if (ch >= 0xD800 && ch <= 0xDBFF) {
			ch = (uint32_t)s[++i];
			if (ch < 0xDC00 || ch > 0xDFFF) {
				return 0;
			}
		} else if ((ch >= 0xDC00 && ch <= 0xDFFF) || ch > 0x10FFFF) {
			return 0;
		}
	}
	return 1;
}
static WfStatus copy_arg(wchar_t **out, const wchar_t *s)
{
	size_t n = wcslen(s);
	if (n >= SIZE_MAX / sizeof **out) {
		return WF_E_RANGE;
	}
	*out = malloc((n + 1) * sizeof **out);
	if (!*out) {
		return WF_E_NOMEM;
	}
	memcpy(*out, s, (n + 1) * sizeof **out);
	return WF_OK;
}
WfStatus wf_cli_parse(
        WfCli *c, int argc, const wchar_t *const *argv, WfCliReport *report)
{
	WfCliReport local = {WF_CLI_ERROR_NONE, 0};
	WfStatus status = WF_OK;
	int positional = 0;
	if (report) {
		*report = local;
	}
	if (!c) {
		return WF_E_INVAL;
	}
	wf_cli_dispose(c);
	if (argc < 1 || !argv || !argv[0]) {
		return WF_E_INVAL;
	}
	for (int i = 1; i < argc; i++) {
		const wchar_t *arg = argv[i];
		wchar_t **target = NULL;
		WfCliAction action = WF_CLI_BROWSE;
		local.argument = (size_t)i;
		if (!valid_text(arg)) {
			local.error = WF_CLI_ERROR_INVALID_TEXT;
			goto invalid;
		}
		if (!positional && !wcscmp(arg, L"--")) {
			positional = 1;
			continue;
		}
		if (positional || L'-' != arg[0]) {
			if (c->path) {
				local.error = WF_CLI_ERROR_DUPLICATE;
				goto invalid;
			}
			target = &c->path;
		} else if (!wcscmp(arg, L"--setup")) {
			c->setup = 1;
			continue;
		} else if (!wcscmp(arg, L"--continue")) {
			c->continue_session = 1;
			continue;
		} else if (!wcscmp(arg, L"--config")) {
			target = &c->config_path;
		} else if (!wcscmp(arg, L"--language")) {
			target = &c->language;
		} else if (!wcscmp(arg, L"--print-last-dir")) {
			target = &c->print_last_dir;
		} else if (!wcscmp(arg, L"--help")) {
			action = WF_CLI_HELP;
		} else if (!wcscmp(arg, L"--version")) {
			action = WF_CLI_VERSION;
		} else if (!wcscmp(arg, L"--reset-terminal")) {
			action = WF_CLI_RESET_TERMINAL;
		} else if (!wcscmp(arg, L"--purge-state")) {
			action = WF_CLI_PURGE_STATE;
		} else if (!wcscmp(arg, L"--forget-session")) {
			action = WF_CLI_FORGET_SESSION;
		} else if (!wcscmp(arg, L"--image-worker")) {
			action = WF_CLI_IMAGE_WORKER;
		} else if (!wcscmp(arg, L"--abandon-recovery")) {
			action = WF_CLI_ABANDON_RECOVERY;
			target = &c->recovery_id;
		} else {
			local.error = WF_CLI_ERROR_UNKNOWN_OPTION;
			goto invalid;
		}
		if (WF_CLI_BROWSE != action) {
			if (WF_CLI_BROWSE != c->action) {
				local.error = WF_CLI_ERROR_CONFLICT;
				goto invalid;
			}
			c->action = action;
		}
		if (target) {
			if (*target) {
				local.error = WF_CLI_ERROR_DUPLICATE;
				goto invalid;
			}
			if (target != &c->path) {
				if (i + 1 == argc || !argv[i + 1] ||
				        !*argv[i + 1] ||
				        !wcsncmp(argv[i + 1], L"--", 2)) {
					local.error =
					        WF_CLI_ERROR_MISSING_VALUE;
					goto invalid;
				}
				arg = argv[++i];
				if (!valid_text(arg)) {
					local.argument = (size_t)i;
					local.error = WF_CLI_ERROR_INVALID_TEXT;
					goto invalid;
				}
			}
			status = copy_arg(target, arg);
			if (WF_OK != status) {
				goto done;
			}
		}
	}
	/* no order of precedence between where to start and what to resume */
	if (c->continue_session && (c->path || c->setup)) {
		local.error = WF_CLI_ERROR_CONFLICT;
		goto invalid;
	}
	if (WF_CLI_BROWSE != c->action &&
	        (c->path || c->config_path || c->print_last_dir ||
	                c->language || c->continue_session)) {
		local.error = WF_CLI_ERROR_CONFLICT;
		goto invalid;
	}
	local.argument = 0;
	goto done;
invalid:
	status = WF_E_INVAL;
done:
	if (WF_OK != status) {
		wf_cli_dispose(c);
	}
	if (report) {
		*report = local;
	}
	return status;
}
