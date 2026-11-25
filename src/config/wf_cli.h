#ifndef WF_CLI_H
#define WF_CLI_H

#include <stddef.h>
#include <wchar.h>

#include "base/wf_status.h"

typedef enum
{
	WF_CLI_BROWSE,
	WF_CLI_HELP,
	WF_CLI_VERSION,
	WF_CLI_RESET_TERMINAL,
	WF_CLI_ABANDON_RECOVERY,
	WF_CLI_PURGE_STATE,
	WF_CLI_FORGET_SESSION, /* empty the saved session */
	WF_CLI_IMAGE_WORKER
} WfCliAction;
typedef struct
{
	WfCliAction action;
	wchar_t *path, *config_path, *print_last_dir, *recovery_id;
	wchar_t *language; /* --language auto|en|ja: for this run only */
	int setup;         /* --setup: show the first-run theme screen again */
	int continue_session; /* --continue: put back the last saved session */
} WfCli;
typedef enum
{
	WF_CLI_ERROR_NONE,
	WF_CLI_ERROR_UNKNOWN_OPTION,
	WF_CLI_ERROR_MISSING_VALUE,
	WF_CLI_ERROR_DUPLICATE,
	WF_CLI_ERROR_CONFLICT,
	WF_CLI_ERROR_INVALID_TEXT
} WfCliError;
typedef struct
{
	WfCliError error;
	size_t argument;
} WfCliReport;

void wf_cli_init(WfCli *cli);
void wf_cli_dispose(WfCli *cli);
/* argv includes argv[0]. Owns copies; no path-length limit or path resolution.
 * Caller resolves relative paths against the original startup CWD before
 * changing process CWD. `cli` must already be wf_cli_init'ed (it is disposed
 * on entry). Errors leave an initialized empty result.
 * Parsing is not authority to execute unsupported actions, especially worker
 * startup, recovery abandonment, purge, or any gated production operation. */
WfStatus wf_cli_parse(
        WfCli *cli, int argc, const wchar_t *const *argv, WfCliReport *report);

#endif
