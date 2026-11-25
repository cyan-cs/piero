#ifndef WF_TERMINAL_WIN_H
#define WF_TERMINAL_WIN_H

#include <windows.h>

#include "wf_launch.h"

/*
 * Open Terminal Here: a new terminal whose working directory is a folder on a
 * local drive. Windows Terminal (wt.exe, looked for in the folders of the PATH
 * only, where its app execution alias is: not in the folder of this program, not
 * in the current directory, not in a relative or share entry) is started with
 * -d; when it cannot be started,
 * Windows PowerShell and then cmd.exe get a console of their own with the
 * folder as the process's working directory. A candidate that started is never
 * followed by another. The folder is never put into a command that a shell
 * reads: only wt.exe is given it as an argument (quoted by the rules of
 * CommandLineToArgvW), and it is not given a folder that Windows Terminal would
 * split into commands (a ';'). Never elevates, never waits for the new program,
 * never changes this process's directory.
 */

/* Whether `dir` is a folder this can open a terminal in: an absolute path on a
 * drive (C:\ or C:\x\y; not a share, not a \\?\ or device path), shorter than
 * MAX_PATH (a working directory cannot be longer), no control characters, no
 * quote, no wildcard, no : < > | outside the drive letter. It is not looked up
 * on the disk. */
bool wf_terminal_dir_ok(const wchar_t *dir);

/* `arg` as one argument of a command line that CommandLineToArgvW reads back
 * as `arg` (backslashes before a quote and at the end are doubled). False when
 * `cap` is too small. */
bool wf_terminal_quote_arg(const wchar_t *arg, wchar_t *out, size_t cap);

/* WF_OK: a terminal was started. WF_E_INVAL: not a folder this can open a
 * terminal in (see wf_terminal_dir_ok). WF_E_NOENT: the folder is gone or is not
 * a folder. WF_E_ACCESS: it cannot be entered. Otherwise no program could be
 * started: WF_E_NOMEM when one of the attempts failed for lack of memory, else
 * WF_E_OS, and *error is that Win32 error (or, with none for memory, the first
 * attempt's). A program that was left out (no wt.exe on the PATH, a folder with
 * a ';' that wt would split) is not a failure and is not the reason given. It
 * may block (the disk is asked about the folder and the PATH): call it off the
 * thread that reads keys. */
WfStatus wf_terminal_open_here(const wchar_t *dir, DWORD *error);

/* Tests only: when set, this starts the process instead of CreateProcessW. */
extern BOOL (*wf_terminal_test_spawn)(const wchar_t *application,
        wchar_t *command_line,
        const wchar_t *directory,
        DWORD flags,
        DWORD *error);

#endif
