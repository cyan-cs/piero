#ifndef WF_DIAG_H
#define WF_DIAG_H
#include <windows.h>

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
	WF_DIAG_ERROR,
	WF_DIAG_WARNING,
	WF_DIAG_INFO,
	WF_DIAG_DEBUG
} WfDiagLevel;
typedef enum
{
	WF_DIAG_STARTUP,
	WF_DIAG_CONFIG,
	WF_DIAG_LIST,
	WF_DIAG_PREVIEW,
	WF_DIAG_COPY,
	WF_DIAG_MOVE,
	WF_DIAG_OPEN,
	WF_DIAG_RENAME,
	WF_DIAG_CREATE,
	WF_DIAG_RECOVERY
} WfDiagStage;
typedef enum
{
	WF_DIAG_BEGIN,
	WF_DIAG_OK,
	WF_DIAG_FAILED,
	WF_DIAG_REFUSED,
	WF_DIAG_CANCELED
} WfDiagResult;
typedef struct
{
	WfDiagLevel level;
	WfDiagStage stage;
	WfDiagResult result;
	uint64_t operation_id;
	DWORD win32_error;
	int32_t hresult;
	const wchar_t
	        *path; /* borrowed; ignored entirely unless log_paths=true */
} WfDiagEvent;
typedef struct WfDiagnostics WfDiagnostics;

/* Dedicated private current-user root. Parent must already exist. At most
 * 3 x 1MiB files; rotation never truncates or adopts a pathname. Unknown,
 * modified, aliased, or incompletely written objects refuse. Caller reports
 * refusal to stderr. No contents/free-form message argument exists.
 * Default product policy: level=WARNING, log_paths=0.
 * Writes serialize under an SRW lock. Stop producers before close. */
bool wf_diag_open(const wchar_t *root,
        WfDiagLevel level,
        bool log_paths,
        WfDiagnostics **out,
        DWORD *error);
bool wf_diag_write(
        WfDiagnostics *logger, const WfDiagEvent *event, DWORD *error);
void wf_diag_close(WfDiagnostics *logger);

#endif
