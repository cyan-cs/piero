#ifndef WF_APP_SETTINGS_PRIV_H
#define WF_APP_SETTINGS_PRIV_H

#include "wf_app_priv.h"

/*
 * The settings screen is three columns: sections (Settings, Licenses, About),
 * the entries of a section, and the details of an entry. The columns share
 * how they are drawn, selected and scrolled; what is in the details belongs to
 * the page that makes them: items of the configuration, the key list, the
 * themes and license texts. The license page reads notice files; settings
 * changes are saved through the configuration port.
 */

enum
{
	WF_SEC_SETTINGS,
	WF_SEC_LICENSES,
	WF_SEC_ABOUT,
	WF_SEC_COUNT
};

enum
{
	WF_CAT_GENERAL,
	WF_CAT_KEYBOARD,
	WF_CAT_APPEARANCE,
	WF_CAT_FILES,
	WF_CAT_PREVIEW,
	WF_CAT_EDITOR,
	WF_CAT_ARCHIVES,
	WF_CAT_OPERATIONS,
	WF_CAT_UPDATES,
	WF_CAT_COUNT
};

typedef enum
{
	K_BOOL,
	K_CHOICE,
	K_INT,
	K_INFO
} Kind;

typedef enum
{
	INFO_NONE,
	INFO_ARCHIVE_MODE,
	INFO_ARCHIVE_NAMES,
	INFO_DELETE,
	INFO_COPY_VERIFY,
	INFO_EDITOR_BACKUP
} Info;

typedef struct
{
	int category;
	WfMsg label;
	Kind kind;
	size_t offset;
	const char *choices;
	int min, max, step;
	const WfMsg *labels; /* choice values shown as messages; NULL: as is */
	WfMsg help;
	WfMsg when; /* WF_MSG_COUNT: now (or when saved); else a tag shown */
	Info info;
} Item;

/* One line of a page's details. */
typedef struct
{
	char label[72];
	char value[72];
	bool header; /* a title, not a row the cursor stops on */
	bool dim;
	bool cycle;  /* Enter changes it: shown as < value > under the cursor */
	bool swatch; /* `rgb` is shown as a coloured block after the value */
	uint32_t rgb;
} WfSetRow;

/* What the core needs from a page. */
typedef struct
{
	size_t (*count)(const WfApp *app);
	void (*row)(const WfApp *app, size_t i, WfSetRow *out);
	/* Enter, characters and the like; WF_ACT_NONE when the key is not its. */
	WfAction (*event)(WfApp *app, const WfEvent *ev);
	/* the line under the details: what the row under the cursor does */
	const char *(*help)(const WfApp *app);
	WfMsg (*hint)(const WfApp *app);
} WfSetPage;

void wf_set_draw(const WfApp *app, WfFrame *f);
void wf_set_layout(const WfApp *app, int cols, int x[3], int w[3]);
extern const WfMsg wf_set_section_names[WF_SEC_COUNT];
extern const WfMsg wf_set_category_names[WF_CAT_COUNT];
int wf_set_view_rows(const WfApp *app);
const char *wf_set_doc_row(const char *p,
        const char *end,
        int width,
        const char **line,
        size_t *len);
size_t wf_set_doc_rows(const WfApp *app, int width);
const WfSetPage *wf_set_page_of(const WfApp *app);
const Item *wf_set_item_at(const WfApp *app, size_t row);
int wf_set_get_int(const WfConfig *c, const Item *it);
size_t wf_set_entry_count(const WfApp *app);
void wf_set_choice_label(
        const WfApp *app, const Item *it, int v, char *out, size_t cap);
int wf_set_choice_count(const char *choices);
bool wf_set_is_dirty(const WfApp *app);
size_t wf_set_char_after(const char *s, size_t n, size_t at);
extern const WfSetPage WF_SETPAGE_KEYS;
extern const WfSetPage WF_SETPAGE_THEMES;
extern const WfSetPage WF_SETPAGE_UPDATES;
extern const WfMode WF_MODE_UPDATE_CONSENT;
void wf_app_update_consent_begin(WfApp *app);

void wf_set_note(WfApp *app, bool is_error, const char *text);
/* Keeps the row under the cursor in view. */
void wf_set_scroll_fit(WfApp *app);
void wf_set_note_msg(WfApp *app, bool is_error, WfMsg id);
/* The draft changed in a way that shows on screen. */
void wf_set_preview(WfApp *app);
/* Opens the one-line text field. */
void wf_set_text_begin(WfApp *app, int kind, const char *initial);
void wf_set_text_end(WfApp *app);

void wf_set_keys_refresh(WfApp *app); /* the draft's keys changed */
void wf_set_keys_free(WfApp *app);
WfAction wf_set_keys_capture(WfApp *app, const WfEvent *ev);
bool wf_set_keys_text_commit(WfApp *app);
void wf_set_keys_search_commit(WfApp *app);
void wf_set_keys_reset_all(WfApp *app);
/* Opens a yes/no question (WF_SS_DELETE, WF_SS_RESET_ALL); it starts on No. */
void wf_set_ask_yes_no(WfApp *app, WfSettingsSub sub);
/* Says what is wrong with a key assignment, in the language in force. */
void wf_key_issue_text(const WfKeyIssue *issue, char *out, size_t cap);

void wf_set_apply_live_changes(WfApp *app, const WfConfig *before);
bool wf_set_save_draft(WfApp *app);

WfAction wf_set_text_event(WfApp *app, const WfEvent *ev);
WfAction wf_set_pick_event(WfApp *app, const WfEvent *ev);

void wf_set_put_int(WfConfig *c, const Item *it, int v);
void wf_set_apply_language(WfApp *app, const WfConfig *c);
void wf_set_palette_of(const WfConfig *c, WfThemePalette *p);

bool wf_set_theme_text_commit(WfApp *app); /* hex colour or name */
void wf_set_theme_color_preview(WfApp *app, bool cancel);
WfAction wf_set_theme_warn_event(WfApp *app, const WfEvent *ev);
WfAction wf_set_theme_delete_event(WfApp *app, const WfEvent *ev);
/* Esc / Left inside the colours of a theme: back to the list of themes.
 * Returns whether it was handled. */
bool wf_set_theme_back(WfApp *app);

void wf_about_scan(WfApp *app); /* finds the licence files */
size_t wf_about_count(const WfApp *app, int section);
void wf_about_title(
        const WfApp *app, int section, size_t i, char *out, size_t cap);
/* The text of an entry; malloc'd, UTF-8, NUL-terminated. NULL on allocation
 * failure or when the project page exceeds its formatting buffer. */
char *wf_about_text(const WfApp *app, int section, size_t i, size_t *n);

#endif
