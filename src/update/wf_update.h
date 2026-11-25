#ifndef WF_UPDATE_H
#define WF_UPDATE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WF_UPDATE_BODY_MAX (64u * 1024u)
#define WF_UPDATE_RELEASES "https://github.com/cyan-cs/piero/releases"
typedef enum
{
	WF_UPDATE_IDLE,
	WF_UPDATE_CHECKING,
	WF_UPDATE_CURRENT,
	WF_UPDATE_AVAILABLE,
	WF_UPDATE_NO_RELEASE,
	WF_UPDATE_LIMITED,
	WF_UPDATE_UNAVAILABLE,
	WF_UPDATE_INVALID,
	WF_UPDATE_THROTTLED
} WfUpdateState;
typedef struct
{
	WfUpdateState state;
	char latest[64];
} WfUpdateResult;
/* Metadata only. No asset URL, checksum or response body authorizes execution. */
WfUpdateResult wf_update_response(const char *current,
        unsigned http_status,
        const void *json,
        size_t size);
bool wf_update_due(bool enabled,
        bool consent,
        bool state_valid,
        uint64_t now,
        uint64_t previous);
bool wf_update_allowed(
        bool enabled, bool consent, bool config_valid, bool manual);
#endif
