#include "wf_commands.h"

#include <string.h>

#include "../edit/wf_ekeymap.h"
#include "../base/wf_macro.h"

static const WfCommandInfo COMMANDS[] = {
        {WF_CMD_LEFT, "ed.left", MSG_CMD_ED_LEFT,
                WF_CTX_BIT(WF_CTX_EDIT) | WF_CTX_BIT(WF_CTX_VIM_NORMAL) |
                        WF_CTX_BIT(WF_CTX_VIM_INSERT),
                WF_CF_COUNT},
        {WF_CMD_RIGHT, "ed.right", MSG_CMD_ED_RIGHT,
                WF_CTX_BIT(WF_CTX_EDIT) | WF_CTX_BIT(WF_CTX_VIM_NORMAL) |
                        WF_CTX_BIT(WF_CTX_VIM_INSERT),
                WF_CF_COUNT},
        {WF_CMD_UP, "ed.up", MSG_CMD_ED_UP,
                WF_CTX_BIT(WF_CTX_EDIT) | WF_CTX_BIT(WF_CTX_VIM_NORMAL) |
                        WF_CTX_BIT(WF_CTX_VIM_INSERT),
                WF_CF_COUNT},
        {WF_CMD_DOWN, "ed.down", MSG_CMD_ED_DOWN,
                WF_CTX_BIT(WF_CTX_EDIT) | WF_CTX_BIT(WF_CTX_VIM_NORMAL) |
                        WF_CTX_BIT(WF_CTX_VIM_INSERT),
                WF_CF_COUNT},
        {WF_CMD_HOME, "ed.line_start", MSG_CMD_ED_HOME,
                WF_CTX_BIT(WF_CTX_EDIT) | WF_CTX_BIT(WF_CTX_VIM_NORMAL) |
                        WF_CTX_BIT(WF_CTX_VIM_INSERT),
                0},
        {WF_CMD_END, "ed.line_end", MSG_CMD_ED_END,
                WF_CTX_BIT(WF_CTX_EDIT) | WF_CTX_BIT(WF_CTX_VIM_NORMAL) |
                        WF_CTX_BIT(WF_CTX_VIM_INSERT),
                0},
        {WF_CMD_PGUP, "ed.page_up", MSG_CMD_ED_PGUP,
                WF_CTX_BIT(WF_CTX_EDIT) | WF_CTX_BIT(WF_CTX_VIM_NORMAL) |
                        WF_CTX_BIT(WF_CTX_VIM_INSERT),
                WF_CF_COUNT},
        {WF_CMD_PGDN, "ed.page_down", MSG_CMD_ED_PGDN,
                WF_CTX_BIT(WF_CTX_EDIT) | WF_CTX_BIT(WF_CTX_VIM_NORMAL) |
                        WF_CTX_BIT(WF_CTX_VIM_INSERT),
                WF_CF_COUNT},
        {WF_CMD_DOC_START, "ed.doc_start", MSG_CMD_ED_DOC_START,
                WF_CTX_BIT(WF_CTX_EDIT) | WF_CTX_BIT(WF_CTX_VIM_NORMAL) |
                        WF_CTX_BIT(WF_CTX_VIM_INSERT),
                WF_CF_LINE},
        {WF_CMD_DOC_END, "ed.doc_end", MSG_CMD_ED_DOC_END,
                WF_CTX_BIT(WF_CTX_EDIT) | WF_CTX_BIT(WF_CTX_VIM_NORMAL) |
                        WF_CTX_BIT(WF_CTX_VIM_INSERT),
                WF_CF_LINE},
        {WF_CMD_WORD_LEFT, "ed.word_left", MSG_CMD_ED_WORD_LEFT,
                WF_CTX_BIT(WF_CTX_EDIT) | WF_CTX_BIT(WF_CTX_VIM_NORMAL) |
                        WF_CTX_BIT(WF_CTX_VIM_INSERT),
                WF_CF_COUNT},
        {WF_CMD_WORD_RIGHT, "ed.word_right", MSG_CMD_ED_WORD_RIGHT,
                WF_CTX_BIT(WF_CTX_EDIT) | WF_CTX_BIT(WF_CTX_VIM_NORMAL) |
                        WF_CTX_BIT(WF_CTX_VIM_INSERT),
                WF_CF_COUNT},
        {WF_CMD_NEWLINE, "ed.newline", MSG_CMD_ED_NEWLINE,
                WF_CTX_BIT(WF_CTX_EDIT) | WF_CTX_BIT(WF_CTX_VIM_INSERT), 0},
        {WF_CMD_BACKSPACE, "ed.backspace", MSG_CMD_ED_BACKSPACE,
                WF_CTX_BIT(WF_CTX_EDIT) | WF_CTX_BIT(WF_CTX_VIM_INSERT), 0},
        {WF_CMD_DELETE, "ed.delete", MSG_CMD_ED_DELETE,
                WF_CTX_BIT(WF_CTX_EDIT) | WF_CTX_BIT(WF_CTX_VIM_INSERT), 0},
        {WF_CMD_TAB, "ed.tab", MSG_CMD_ED_TAB,
                WF_CTX_BIT(WF_CTX_EDIT) | WF_CTX_BIT(WF_CTX_VIM_INSERT), 0},
        {WF_CMD_BACKTAB, "ed.backtab", MSG_CMD_ED_BACKTAB,
                WF_CTX_BIT(WF_CTX_EDIT) | WF_CTX_BIT(WF_CTX_VIM_INSERT), 0},
        {WF_CMD_DELETE_LINE, "ed.delete_line", MSG_CMD_ED_DELETE_LINE,
                WF_CTX_BIT(WF_CTX_EDIT), 0},
        {WF_CMD_CUT, "ed.cut", MSG_CMD_ED_CUT, WF_CTX_BIT(WF_CTX_EDIT), 0},
        {WF_CMD_COPY, "ed.copy", MSG_CMD_ED_COPY, WF_CTX_BIT(WF_CTX_EDIT), 0},
        {WF_CMD_PASTE, "ed.paste", MSG_CMD_ED_PASTE, WF_CTX_BIT(WF_CTX_EDIT),
                0},
        {WF_CMD_SELECT_ALL, "ed.select_all", MSG_CMD_ED_SELECT_ALL,
                WF_CTX_BIT(WF_CTX_EDIT), 0},
        {WF_CMD_MARK, "ed.mark", MSG_CMD_ED_MARK, WF_CTX_BIT(WF_CTX_EDIT), 0},
        {WF_CMD_UNDO, "ed.undo", MSG_CMD_ED_UNDO,
                WF_CTX_BIT(WF_CTX_EDIT) | WF_CTX_BIT(WF_CTX_VIM_NORMAL), 0},
        {WF_CMD_REDO, "ed.redo", MSG_CMD_ED_REDO,
                WF_CTX_BIT(WF_CTX_EDIT) | WF_CTX_BIT(WF_CTX_VIM_NORMAL), 0},
        {WF_CMD_COMMENT, "ed.comment", MSG_CMD_ED_COMMENT,
                WF_CTX_BIT(WF_CTX_EDIT) | WF_CTX_BIT(WF_CTX_VIM_NORMAL), 0},
        {WF_CMD_COMPLETE, "ed.complete", MSG_CMD_ED_COMPLETE,
                WF_CTX_BIT(WF_CTX_EDIT) | WF_CTX_BIT(WF_CTX_VIM_INSERT), 0},
        {WF_CMD_SAVE, "ed.save", MSG_CMD_ED_SAVE,
                WF_CTX_BIT(WF_CTX_EDIT) | WF_CTX_BIT(WF_CTX_VIM_NORMAL) |
                        WF_CTX_BIT(WF_CTX_VIM_INSERT),
                0},
        {WF_CMD_SAVE_AS, "ed.save_as", MSG_CMD_ED_SAVE_AS,
                WF_CTX_BIT(WF_CTX_EDIT), 0},
        {WF_CMD_EXIT, "ed.exit", MSG_CMD_ED_EXIT, WF_CTX_BIT(WF_CTX_EDIT),
                WF_CF_REQUIRED},
        {WF_CMD_FIND, "ed.find", MSG_CMD_ED_FIND,
                WF_CTX_BIT(WF_CTX_EDIT) | WF_CTX_BIT(WF_CTX_VIM_NORMAL), 0},
        {WF_CMD_FIND_BACK, "ed.find_back", MSG_CMD_ED_FIND_BACK,
                WF_CTX_BIT(WF_CTX_EDIT), 0},
        {WF_CMD_FIND_NEXT, "ed.find_next", MSG_CMD_ED_FIND_NEXT,
                WF_CTX_BIT(WF_CTX_EDIT) | WF_CTX_BIT(WF_CTX_VIM_NORMAL), 0},
        {WF_CMD_FIND_PREV, "ed.find_prev", MSG_CMD_ED_FIND_PREV,
                WF_CTX_BIT(WF_CTX_EDIT) | WF_CTX_BIT(WF_CTX_VIM_NORMAL), 0},
        {WF_CMD_REPLACE, "ed.replace", MSG_CMD_ED_REPLACE,
                WF_CTX_BIT(WF_CTX_EDIT), 0},
        {WF_CMD_GOTO, "ed.goto", MSG_CMD_ED_GOTO, WF_CTX_BIT(WF_CTX_EDIT), 0},
        {WF_CMD_INSERT_FILE, "ed.insert_file", MSG_CMD_ED_INSERT_FILE,
                WF_CTX_BIT(WF_CTX_EDIT), 0},
        {WF_CMD_OPEN_FILE, "ed.open_file", MSG_CMD_ED_OPEN_FILE,
                WF_CTX_BIT(WF_CTX_EDIT), 0},
        {WF_CMD_NEXT_BUFFER, "ed.next_buffer", MSG_CMD_ED_NEXT_BUFFER,
                WF_CTX_BIT(WF_CTX_EDIT) | WF_CTX_BIT(WF_CTX_VIM_NORMAL),
                WF_CF_COUNT},
        {WF_CMD_PREV_BUFFER, "ed.prev_buffer", MSG_CMD_ED_PREV_BUFFER,
                WF_CTX_BIT(WF_CTX_EDIT) | WF_CTX_BIT(WF_CTX_VIM_NORMAL),
                WF_CF_COUNT},
        {WF_CMD_MD_PREVIEW, "ed.md_preview", MSG_CMD_ED_MD_PREVIEW,
                WF_CTX_BIT(WF_CTX_EDIT) | WF_CTX_BIT(WF_CTX_VIM_NORMAL) |
                        WF_CTX_BIT(WF_CTX_VIM_INSERT),
                0},
        {WF_CMD_HELP, "ed.help", MSG_CMD_ED_HELP,
                WF_CTX_BIT(WF_CTX_EDIT) | WF_CTX_BIT(WF_CTX_VIM_NORMAL), 0},
        {WF_CMD_CANCEL, "ed.cancel", MSG_CMD_ED_CANCEL,
                WF_CTX_BIT(WF_CTX_EDIT) | WF_CTX_BIT(WF_CTX_VIM_NORMAL), 0},
        {WF_CMD_VIM_INSERT, "ed.insert_mode", MSG_CMD_ED_VIM_INSERT,
                WF_CTX_BIT(WF_CTX_VIM_NORMAL), 0},
        {WF_CMD_VIM_APPEND, "ed.append_mode", MSG_CMD_ED_VIM_APPEND,
                WF_CTX_BIT(WF_CTX_VIM_NORMAL), 0},
        {WF_CMD_VIM_OPEN_BELOW, "ed.open_below", MSG_CMD_ED_VIM_OPEN_BELOW,
                WF_CTX_BIT(WF_CTX_VIM_NORMAL), 0},
        {WF_CMD_VIM_OPEN_ABOVE, "ed.open_above", MSG_CMD_ED_VIM_OPEN_ABOVE,
                WF_CTX_BIT(WF_CTX_VIM_NORMAL), 0},
        {WF_CMD_VIM_NORMAL, "ed.normal_mode", MSG_CMD_ED_VIM_NORMAL,
                WF_CTX_BIT(WF_CTX_VIM_INSERT), WF_CF_REQUIRED},
        {WF_CMD_VIM_YANK_LINE, "ed.yank_line", MSG_CMD_ED_VIM_YANK_LINE,
                WF_CTX_BIT(WF_CTX_VIM_NORMAL), 0},
        {WF_CMD_VIM_CUT_LINE, "ed.cut_line", MSG_CMD_ED_VIM_CUT_LINE,
                WF_CTX_BIT(WF_CTX_VIM_NORMAL), 0},
        {WF_CMD_VIM_PASTE_AFTER, "ed.paste_after", MSG_CMD_ED_VIM_PASTE_AFTER,
                WF_CTX_BIT(WF_CTX_VIM_NORMAL), 0},
        {WF_CMD_VIM_PASTE_BEFORE, "ed.paste_before",
                MSG_CMD_ED_VIM_PASTE_BEFORE, WF_CTX_BIT(WF_CTX_VIM_NORMAL), 0},
        {WF_CMD_VIM_CMDLINE, "ed.cmdline", MSG_CMD_ED_VIM_CMDLINE,
                WF_CTX_BIT(WF_CTX_VIM_NORMAL), WF_CF_REQUIRED},
        {WF_CMD_VIM_DELETE_CHAR, "ed.delete_char", MSG_CMD_ED_VIM_DELETE_CHAR,
                WF_CTX_BIT(WF_CTX_VIM_NORMAL), 0},
        {WF_CMD_VIM_OP_DELETE, "ed.op_delete", MSG_CMD_ED_VIM_OP_DELETE,
                WF_CTX_BIT(WF_CTX_VIM_NORMAL), WF_CF_COUNT},
        {WF_CMD_VIM_OP_YANK, "ed.op_yank", MSG_CMD_ED_VIM_OP_YANK,
                WF_CTX_BIT(WF_CTX_VIM_NORMAL), WF_CF_COUNT},
        {WF_CMD_VIM_OP_CHANGE, "ed.op_change", MSG_CMD_ED_VIM_OP_CHANGE,
                WF_CTX_BIT(WF_CTX_VIM_NORMAL), WF_CF_COUNT},
        {WF_CMD_VIM_DELETE_EOL, "ed.delete_eol", MSG_CMD_ED_VIM_DELETE_EOL,
                WF_CTX_BIT(WF_CTX_VIM_NORMAL), 0},
        {WF_CMD_VIM_CHANGE_EOL, "ed.change_eol", MSG_CMD_ED_VIM_CHANGE_EOL,
                WF_CTX_BIT(WF_CTX_VIM_NORMAL), 0},
        {WF_CMD_VIM_VISUAL, "ed.visual", MSG_CMD_ED_VIM_VISUAL,
                WF_CTX_BIT(WF_CTX_VIM_NORMAL), 0},
        {WF_CMD_VIM_VISUAL_LINE, "ed.visual_line", MSG_CMD_ED_VIM_VISUAL_LINE,
                WF_CTX_BIT(WF_CTX_VIM_NORMAL), 0},
        {WF_CMD_VIM_REGISTER, "ed.register", MSG_CMD_ED_VIM_REGISTER,
                WF_CTX_BIT(WF_CTX_VIM_NORMAL), 0},
        {WF_CMD_VIM_RECORD, "ed.record", MSG_CMD_ED_VIM_RECORD,
                WF_CTX_BIT(WF_CTX_VIM_NORMAL), 0},
        {WF_CMD_VIM_PLAY, "ed.play", MSG_CMD_ED_VIM_PLAY,
                WF_CTX_BIT(WF_CTX_VIM_NORMAL), WF_CF_COUNT},
        {WF_CMD_VIM_DOT, "ed.repeat", MSG_CMD_ED_VIM_DOT,
                WF_CTX_BIT(WF_CTX_VIM_NORMAL), WF_CF_COUNT},
        {WF_FM_NAV_UP, "fm.nav.up", MSG_CMD_FM_NAV_UP, WF_CTX_BIT(WF_CTX_FM),
                WF_CF_COUNT},
        {WF_FM_NAV_DOWN, "fm.nav.down", MSG_CMD_FM_NAV_DOWN,
                WF_CTX_BIT(WF_CTX_FM), WF_CF_COUNT},
        {WF_FM_NAV_PARENT, "fm.nav.parent", MSG_CMD_FM_NAV_PARENT,
                WF_CTX_BIT(WF_CTX_FM), 0},
        {WF_FM_NAV_ENTER, "fm.nav.enter", MSG_CMD_FM_NAV_ENTER,
                WF_CTX_BIT(WF_CTX_FM), 0},
        {WF_FM_NAV_RIGHT, "fm.nav.right", MSG_CMD_FM_NAV_RIGHT,
                WF_CTX_BIT(WF_CTX_FM), 0},
        {WF_FM_NAV_OPEN, "fm.nav.open", MSG_CMD_FM_NAV_OPEN,
                WF_CTX_BIT(WF_CTX_FM), 0},
        {WF_FM_NAV_FIRST, "fm.nav.first", MSG_CMD_FM_NAV_FIRST,
                WF_CTX_BIT(WF_CTX_FM), WF_CF_LINE},
        {WF_FM_NAV_LAST, "fm.nav.last", MSG_CMD_FM_NAV_LAST,
                WF_CTX_BIT(WF_CTX_FM), WF_CF_LINE},
        {WF_FM_HISTORY_BACK, "fm.history.back", MSG_CMD_FM_HISTORY_BACK,
                WF_CTX_BIT(WF_CTX_FM), 0},
        {WF_FM_HISTORY_FORWARD, "fm.history.forward",
                MSG_CMD_FM_HISTORY_FORWARD, WF_CTX_BIT(WF_CTX_FM), 0},
        {WF_FM_SMART_GO, "fm.go", MSG_CMD_FM_SMART_GO, WF_CTX_BIT(WF_CTX_FM),
                0},
        {WF_FM_FILTER, "fm.filter", MSG_CMD_FM_FILTER, WF_CTX_BIT(WF_CTX_FM),
                0},
        {WF_FM_HIDDEN, "fm.hidden", MSG_CMD_FM_HIDDEN, WF_CTX_BIT(WF_CTX_FM),
                0},
        {WF_FM_RELOAD, "fm.reload", MSG_CMD_FM_RELOAD, WF_CTX_BIT(WF_CTX_FM),
                0},
        {WF_FM_INFO, "fm.info", MSG_CMD_FM_INFO, WF_CTX_BIT(WF_CTX_FM), 0},
        {WF_FM_PREVIEW_FULL, "fm.preview.full", MSG_CMD_FM_PREVIEW_FULL,
                WF_CTX_BIT(WF_CTX_FM), 0},
        {WF_FM_PREVIEW_LOAD, "fm.preview.load", MSG_CMD_FM_PREVIEW_LOAD,
                WF_CTX_BIT(WF_CTX_FM), 0},
        {WF_FM_SELECT, "fm.select", MSG_CMD_FM_SELECT, WF_CTX_BIT(WF_CTX_FM),
                0},
        {WF_FM_CANCEL, "fm.cancel", MSG_CMD_FM_CANCEL, WF_CTX_BIT(WF_CTX_FM),
                0},
        {WF_FM_CREATE, "fm.create", MSG_CMD_FM_CREATE, WF_CTX_BIT(WF_CTX_FM),
                0},
        {WF_FM_RENAME, "fm.rename", MSG_CMD_FM_RENAME, WF_CTX_BIT(WF_CTX_FM),
                0},
        {WF_FM_RECYCLE, "fm.recycle", MSG_CMD_FM_RECYCLE, WF_CTX_BIT(WF_CTX_FM),
                0},
        {WF_FM_COPY_STAGE, "fm.copy", MSG_CMD_FM_COPY_STAGE,
                WF_CTX_BIT(WF_CTX_FM), 0},
        {WF_FM_MOVE_STAGE, "fm.move", MSG_CMD_FM_MOVE_STAGE,
                WF_CTX_BIT(WF_CTX_FM), 0},
        {WF_FM_PASTE, "fm.paste", MSG_CMD_FM_PASTE, WF_CTX_BIT(WF_CTX_FM), 0},
        {WF_FM_QUEUE_PAUSE, "fm.queue.pause", MSG_CMD_FM_QUEUE_PAUSE,
                WF_CTX_BIT(WF_CTX_FM), 0},
        {WF_FM_QUEUE_CANCEL, "fm.queue.cancel", MSG_CMD_FM_QUEUE_CANCEL,
                WF_CTX_BIT(WF_CTX_FM), 0},
        {WF_FM_UNDO, "fm.undo", MSG_CMD_FM_UNDO, WF_CTX_BIT(WF_CTX_FM), 0},
        {WF_FM_JOBLOG, "fm.joblog", MSG_CMD_FM_JOBLOG, WF_CTX_BIT(WF_CTX_FM),
                0},
        {WF_FM_BACKUPS, "fm.backups", MSG_CMD_FM_BACKUPS, WF_CTX_BIT(WF_CTX_FM),
                0},
        {WF_FM_EDIT, "fm.edit", MSG_CMD_FM_EDIT, WF_CTX_BIT(WF_CTX_FM), 0},
        {WF_FM_SORT, "fm.sort", MSG_CMD_FM_SORT, WF_CTX_BIT(WF_CTX_FM), 0},
        {WF_FM_SETTINGS, "fm.settings", MSG_CMD_FM_SETTINGS,
                WF_CTX_BIT(WF_CTX_FM), WF_CF_REQUIRED},
        {WF_FM_HELP, "fm.help", MSG_CMD_FM_HELP, WF_CTX_BIT(WF_CTX_FM),
                WF_CF_REQUIRED},
        {WF_FM_QUIT, "fm.quit", MSG_CMD_FM_QUIT, WF_CTX_BIT(WF_CTX_FM),
                WF_CF_REQUIRED},
        {WF_FM_CMDLINE, "fm.cmdline", MSG_CMD_FM_CMDLINE, WF_CTX_BIT(WF_CTX_FM),
                0},
        {WF_FM_SMART_GO_PATH, "fm.go_path", MSG_CMD_FM_SMART_GO_PATH,
                WF_CTX_BIT(WF_CTX_FM), 0},
        {WF_FM_CREATE_FILE, "fm.create.file", MSG_CMD_FM_CREATE_FILE,
                WF_CTX_BIT(WF_CTX_FM), 0},
        {WF_FM_CREATE_FOLDER, "fm.create.folder", MSG_CMD_FM_CREATE_FOLDER,
                WF_CTX_BIT(WF_CTX_FM), 0},
        {WF_FM_DIRSIZE, "fm.dirsize", MSG_CMD_FM_DIRSIZE, WF_CTX_BIT(WF_CTX_FM),
                0},
        {WF_FM_CLIP_PASTE, "fm.clip.paste", MSG_CMD_FM_CLIP_PASTE,
                WF_CTX_BIT(WF_CTX_FM), 0},
        {WF_FM_LOCKS, "fm.locks", MSG_CMD_FM_LOCKS, WF_CTX_BIT(WF_CTX_FM), 0},
        {WF_FM_CLIP_COPY, "fm.clip.copy", MSG_CMD_FM_CLIP_COPY,
                WF_CTX_BIT(WF_CTX_FM), 0},
        {WF_FM_FUZZY, "fm.fuzzy", MSG_CMD_FM_FUZZY, WF_CTX_BIT(WF_CTX_FM), 0},
        {WF_FM_BULK_RENAME, "fm.bulk_rename", MSG_CMD_FM_BULK_RENAME,
                WF_CTX_BIT(WF_CTX_FM), 0},
        {WF_FM_HASH, "fm.hash", MSG_CMD_FM_HASH, WF_CTX_BIT(WF_CTX_FM), 0},
        {WF_FM_TAB_NEW, "fm.tab_new", MSG_CMD_FM_TAB_NEW, WF_CTX_BIT(WF_CTX_FM),
                0},
        {WF_FM_TAB_CLOSE, "fm.tab_close", MSG_CMD_FM_TAB_CLOSE,
                WF_CTX_BIT(WF_CTX_FM), 0},
        {WF_FM_TAB_PREV, "fm.tab_prev", MSG_CMD_FM_TAB_PREV,
                WF_CTX_BIT(WF_CTX_FM), 0},
        {WF_FM_TAB_NEXT, "fm.tab_next", MSG_CMD_FM_TAB_NEXT,
                WF_CTX_BIT(WF_CTX_FM), 0},
        {WF_FM_PLACE_SET, "fm.place_set", MSG_CMD_FM_PLACE_SET,
                WF_CTX_BIT(WF_CTX_FM), 0},
        {WF_FM_PLACE_GO, "fm.place_go", MSG_CMD_FM_PLACE_GO,
                WF_CTX_BIT(WF_CTX_FM), 0},
        {WF_FM_DUPES, "fm.dupes", MSG_CMD_FM_DUPES, WF_CTX_BIT(WF_CTX_FM), 0},
        {WF_FM_TERMINAL, "fm.terminal", MSG_CMD_FM_TERMINAL,
                WF_CTX_BIT(WF_CTX_FM), 0},
        {WF_PV_UP, "pv.up", MSG_CMD_PV_UP, WF_CTX_BIT(WF_CTX_PREVIEW), 0},
        {WF_PV_DOWN, "pv.down", MSG_CMD_PV_DOWN, WF_CTX_BIT(WF_CTX_PREVIEW), 0},
        {WF_PV_PAN_LEFT, "pv.pan_left", MSG_CMD_PV_PAN_LEFT,
                WF_CTX_BIT(WF_CTX_PREVIEW), 0},
        {WF_PV_PAN_RIGHT, "pv.pan_right", MSG_CMD_PV_PAN_RIGHT,
                WF_CTX_BIT(WF_CTX_PREVIEW), 0},
        {WF_PV_BACK, "pv.back", MSG_CMD_PV_BACK, WF_CTX_BIT(WF_CTX_PREVIEW), 0},
        {WF_PV_EDGE_LEFT, "pv.edge_left", MSG_CMD_PV_EDGE_LEFT,
                WF_CTX_BIT(WF_CTX_PREVIEW), 0},
        {WF_PV_EDGE_RIGHT, "pv.edge_right", MSG_CMD_PV_EDGE_RIGHT,
                WF_CTX_BIT(WF_CTX_PREVIEW), 0},
        {WF_PV_TOP, "pv.top", MSG_CMD_PV_TOP, WF_CTX_BIT(WF_CTX_PREVIEW),
                WF_CF_LINE},
        {WF_PV_BOTTOM, "pv.bottom", MSG_CMD_PV_BOTTOM,
                WF_CTX_BIT(WF_CTX_PREVIEW), WF_CF_LINE},
        {WF_PV_CLOSE, "pv.close", MSG_CMD_PV_CLOSE, WF_CTX_BIT(WF_CTX_PREVIEW),
                WF_CF_REQUIRED},
        {WF_PV_TOGGLE_FULL, "pv.toggle_full", MSG_CMD_PV_TOGGLE_FULL,
                WF_CTX_BIT(WF_CTX_PREVIEW), 0},
        {WF_PV_LOAD, "pv.load", MSG_CMD_PV_LOAD, WF_CTX_BIT(WF_CTX_PREVIEW), 0},
        {WF_PV_EDIT, "pv.edit", MSG_CMD_PV_EDIT, WF_CTX_BIT(WF_CTX_PREVIEW), 0},
        {WF_PV_PAGE_NEXT, "pv.page_next", MSG_CMD_PV_PAGE_NEXT,
                WF_CTX_BIT(WF_CTX_PREVIEW), WF_CF_COUNT},
        {WF_PV_PAGE_PREV, "pv.page_prev", MSG_CMD_PV_PAGE_PREV,
                WF_CTX_BIT(WF_CTX_PREVIEW), WF_CF_COUNT},
        {WF_PV_ZOOM_IN, "pv.zoom_in", MSG_CMD_PV_ZOOM_IN,
                WF_CTX_BIT(WF_CTX_PREVIEW), 0},
        {WF_PV_ZOOM_OUT, "pv.zoom_out", MSG_CMD_PV_ZOOM_OUT,
                WF_CTX_BIT(WF_CTX_PREVIEW), 0},
        {WF_PV_FIT, "pv.fit", MSG_CMD_PV_FIT, WF_CTX_BIT(WF_CTX_PREVIEW), 0},
        {WF_PV_PASSWORD, "pv.password", MSG_CMD_PV_PASSWORD,
                WF_CTX_BIT(WF_CTX_PREVIEW), 0},
};

#define COMMAND_COUNT (WF_ARRAY_LEN(COMMANDS))

static const char *const CONTEXT_NAMES[] = {"fm", "preview", "edit",
        "vim_normal", "vim_insert", "smart_go", "filter", "cmdline", "settings",
        "capture", "value", "confirm"};
_Static_assert(WF_ARRAY_LEN(CONTEXT_NAMES) == WF_CTX_COUNT,
        "every key context needs a config name");

const char *wf_context_name(WfKeyContext c)
{
	return (unsigned)c < WF_CTX_COUNT ? CONTEXT_NAMES[c] : "?";
}

bool wf_context_from_name(const char *name, WfKeyContext *out)
{
	/* the names of yazi's layers are understood too */
	static const struct
	{
		const char *name;
		WfKeyContext ctx;
	} ALIASES[] = {{"mgr", WF_CTX_FM}, {"pager", WF_CTX_PREVIEW},
	        {"editor", WF_CTX_EDIT}, {"editor-normal", WF_CTX_VIM_NORMAL},
	        {"editor-insert", WF_CTX_VIM_INSERT}};
	unsigned i;

	for (i = 0; i < WF_CTX_BINDABLE; i++) {
		if (!strcmp(name, CONTEXT_NAMES[i])) {
			*out = (WfKeyContext)i;
			return true;
		}
	}
	for (i = 0; i < WF_ARRAY_LEN(ALIASES); i++) {
		if (!strcmp(name, ALIASES[i].name)) {
			*out = ALIASES[i].ctx;
			return true;
		}
	}
	return false;
}

WfMsg wf_context_label(WfKeyContext c)
{
	static const WfMsg labels[] = {MSG_CTX_FM, MSG_CTX_PREVIEW,
	        MSG_CTX_EDIT, MSG_CTX_VIM_NORMAL, MSG_CTX_VIM_INSERT,
	        MSG_CTX_SMART_GO, MSG_CTX_FILTER, MSG_CTX_CMDLINE,
	        MSG_CTX_SETTINGS, MSG_CTX_CAPTURE, MSG_CTX_VALUE,
	        MSG_CTX_CONFIRM};

	_Static_assert(WF_ARRAY_LEN(labels) == WF_CTX_COUNT,
	        "every key context needs a label");

	return labels[(unsigned)c < WF_CTX_COUNT ? c : 0];
}

size_t wf_cmd_total(void)
{
	return COMMAND_COUNT;
}

const WfCommandInfo *wf_cmd_at(size_t index)
{
	return index < COMMAND_COUNT ? &COMMANDS[index] : NULL;
}

const WfCommandInfo *wf_cmd_info(uint16_t id)
{
	size_t i;

	for (i = 0; i < COMMAND_COUNT; i++) {
		if (COMMANDS[i].id == id) {
			return &COMMANDS[i];
		}
	}
	return NULL;
}

const WfCommandInfo *wf_cmd_by_name(const char *name)
{
	size_t i;

	for (i = 0; i < COMMAND_COUNT; i++) {
		if (!strcmp(COMMANDS[i].name, name)) {
			return &COMMANDS[i];
		}
	}
	return NULL;
}

size_t wf_cmd_list(WfKeyContext ctx, const WfCommandInfo **out, size_t cap)
{
	size_t i, n = 0;

	for (i = 0; i < COMMAND_COUNT; i++) {
		if (!(COMMANDS[i].ctx_mask & WF_CTX_BIT(ctx))) {
			continue;
		}
		if (n < cap) {
			out[n] = &COMMANDS[i];
		}
		n++;
	}
	return n;
}
