#ifndef WF_CONFIG_SAVE_WIN_H
#define WF_CONFIG_SAVE_WIN_H

#include <windows.h>

#include "../edit/wf_efile_win.h"
#include "wf_config.h"
#include "wf_ini_edit.h"

/*
 * Writing settings back to the configuration file.
 *
 * Saving goes through the editor's verified replacement (wf_efile_save_over):
 * a complete new file is written next to the old one, read back and compared,
 * and only then published through journaled handle renames. A failure after
 * publication can leave updated bytes and an unverified baseline; further
 * saves are blocked until reopen. Only the settings that changed are
 * touched; comments and keys this program does not know stay where they are.
 *
 * The file is remembered as it was when the program read it. If it has been
 * changed by anyone else since, saving is refused (WF_CS_CONFLICT) rather than
 * overwriting their edit; the caller can reload and try again.
 */
typedef struct
{
	wchar_t *path;     /* owned */
	WfFileStamp stamp; /* valid == false: the file did not exist */
	DWORD unverified_error; /* nonzero: publication happened; reopen before retry */
} WfConfigFile;

typedef enum
{
	WF_CS_OK,
	WF_CS_CONFLICT, /* changed (or appeared) since it was read */
	WF_CS_REFUSED, /* cannot be edited safely: link, binary, read-only... */
	WF_CS_IO,
	WF_CS_COMMITTED_UNVERIFIED
} WfConfigSaveVerdict;

typedef struct
{
	WfConfigSaveVerdict verdict;
	DWORD error;
	const char *reason; /* static text */
} WfConfigSaveResult;

/* Notes the file as it is now. A missing file is fine. */
WfStatus wf_config_file_open(WfConfigFile *f, const wchar_t *path);
void wf_config_file_close(WfConfigFile *f);

/* Applies `sets` to the file (creating it, and its folder, if there is none
 * and none appeared meanwhile). On success the remembered state is the file
 * as written. */
WfStatus wf_config_file_save(WfConfigFile *f,
        const WfIniSet *sets,
        size_t count,
        WfConfigSaveResult *result);

#endif
