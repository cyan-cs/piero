/*
 * DR-DRAFT-017 through the real program: the recovery gate, and the choice of
 * state folder, are exercised by starting piero.exe in a pseudo console and
 * pressing the keys a person would press. Included by p0_conpty_host.c (it
 * shares that file's terminal, evidence and fixture helpers; there is no
 * second harness).
 *
 * Safety of the test itself:
 *  - LOCALAPPDATA and APPDATA are pointed at the root of an owned scratch
 *    area (p0_lab) for the child only; the harness checks that the area is the
 *    harness's own before anything is changed, and the user's real folders are
 *    never named.
 *  - every file the product might touch is compared before and after: SHA-256
 *    and FileId of the source and of the edited file, the whole older state
 *    folder, and the listing of the destination.
 *  - on success the owned area is removed again (and only it); on a failure it
 *    is kept and its path is written to the evidence.
 */
#include <aclapi.h>
#include <sddl.h>

/* rpcndr.h (via the headers above) defines "small" as a type; the host that
 * includes this file uses it as a variable name */
#undef small

#include "recovery/wf_recovery_store.h"

#define GATE_ID_OLD L"11111111-1111-1111-1111-111111111111"
#define GATE_ID_NEW L"22222222-2222-2222-2222-222222222222"
#define GATE_NOTE_TEXT "hello\r\nworld\r\n"
#define GATE_SAVE_REFUSED "Not saved"
#define GATE_COPY_REFUSED "Copy blocked: unverified history"
#define GATE_OPEN_REFUSED "OPEN unavailable: piero's private state folder"
#define GATE_COPY_NO_STATE "COPY unavailable: the folder for piero's records"
#define GATE_COPY_NO_RECORD "COPY unavailable: the copy record cannot be opened"
#define GATE_RECYCLE_NO_RECORD                                                 \
	"RECYCLE unavailable: the copy record cannot be opened"
#define GATE_DOC_TEXT "GATE_SOURCE_MARKER\r\nline two\r\n"

typedef struct
{
	char sha[P0_HASH_HEX];
	FILE_ID_INFO id;
	bool ok;
} GateSnap;

typedef enum
{
	REC_NONE,
	REC_VALID,        /* a normal unresolved record */
	REC_BAD_NAME,     /* a file the store cannot name */
	REC_BAD_CONTENT,  /* a record whose bytes were overwritten */
	REC_NO_META,      /* the journal without its metadata */
	REC_NO_JOURNAL,   /* the metadata without its journal */
	REC_LOCKED,       /* a valid record that cannot be read */
	REC_RECEIPTS_BAD, /* an inventory file that is not one */
	REC_RECEIPTS_DIR  /* a folder where the inventory file belongs */
} GateRecord;

typedef struct
{
	Product pr;
	wchar_t area[1024], doc[1100], note[1100], backup[1100], dest[1100],
	        browse[1100], cfg[1100];
	GateSnap doc0, note0;
	char *dest0, *browse0, *old0;
	HANDLE lock;
	bool failed_before;
	const wchar_t *lang; /* --language of the next start; NULL: en */
	const wchar_t
	        *extra_args; /* more arguments of the next start; NULL: none */
	bool no_start_path;  /* the next start is given no folder */
	const char
	        *wait_for; /* what the next start shows first; NULL: doc.txt */
	int cli_n;
	char tag[48];
} Gate;

static bool gate_snap(const wchar_t *path, GateSnap *s)
{
	HANDLE h;

	memset(s, 0, sizeof *s);
	if (!p0_sha256_file(path, s->sha)) {
		return false;
	}
	h = CreateFileW(path, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	s->ok = 0 !=
	        GetFileInformationByHandleEx(
	                h, FileIdInfo, &s->id, sizeof s->id);
	CloseHandle(h);
	return s->ok;
}

static bool gate_same(const GateSnap *a, const GateSnap *b)
{
	return a->ok && b->ok && !strcmp(a->sha, b->sha) &&
	        a->id.VolumeSerialNumber == b->id.VolumeSerialNumber &&
	        !memcmp(a->id.FileId.Identifier, b->id.FileId.Identifier, 16);
}

static bool gate_write(const wchar_t *path, const char *text)
{
	HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD wrote = 0;
	bool ok;

	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	ok = WriteFile(h, text, (DWORD)strlen(text), &wrote, NULL) &&
	        wrote == strlen(text);
	CloseHandle(h);
	return ok;
}

/* "relative|size|sha256" for every file below `dir` (names of folders too),
 * in the order the file system lists them. Links are named, never followed. */
static void gate_tree(const wchar_t *dir,
        const wchar_t *rel,
        char **text,
        size_t *n,
        size_t *cap)
{
	wchar_t pattern[1200], path[1200], sub[1200];
	WIN32_FIND_DATAW fd;
	HANDLE h;

	swprintf(pattern, 1200, L"%ls\\*", dir);
	h = FindFirstFileW(pattern, &fd);
	if (INVALID_HANDLE_VALUE == h) {
		return;
	}
	do {
		char line[2300], hash[P0_HASH_HEX] = "-";
		unsigned long long size =
		        ((unsigned long long)fd.nFileSizeHigh << 32) |
		        fd.nFileSizeLow;
		bool link = 0 !=
		        (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
		bool folder =
		        0 != (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);

		if (!wcscmp(fd.cFileName, L".") ||
		        !wcscmp(fd.cFileName, L"..")) {
			continue;
		}
		swprintf(path, 1200, L"%ls\\%ls", dir, fd.cFileName);
		swprintf(sub, 1200, L"%ls%ls%ls", rel, rel[0] ? L"\\" : L"",
		        fd.cFileName);
		if (!folder && !link) {
			p0_sha256_file(path, hash);
		}
		snprintf(line, sizeof line, "%ls|%s|%llu|%s\n", sub,
		        folder ? "dir"
		                : link ? "link"
		                       : "file",
		        size, hash);
		if (*n + strlen(line) + 1 > *cap) {
			*cap = (*n + strlen(line) + 1) * 2;
			*text = realloc(*text, *cap);
		}
		memcpy(*text + *n, line, strlen(line) + 1);
		*n += strlen(line);
		if (folder && !link) {
			gate_tree(path, sub, text, n, cap);
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
}

static char *gate_tree_text(const wchar_t *dir)
{
	size_t n = 0, cap = 256;
	char *text = malloc(cap);

	if (!text) {
		return NULL;
	}
	text[0] = '\0';
	gate_tree(dir, L"", &text, &n, &cap);
	return text;
}

/* Removes `dir` and everything in it; a link is removed itself, never entered. */
static void gate_remove_tree(const wchar_t *dir)
{
	wchar_t pattern[1200], path[1200];
	WIN32_FIND_DATAW fd;
	HANDLE h;

	swprintf(pattern, 1200, L"%ls\\*", dir);
	h = FindFirstFileW(pattern, &fd);
	if (INVALID_HANDLE_VALUE != h) {
		do {
			if (!wcscmp(fd.cFileName, L".") ||
			        !wcscmp(fd.cFileName, L"..")) {
				continue;
			}
			swprintf(path, 1200, L"%ls\\%ls", dir, fd.cFileName);
			SetFileAttributesW(path,
			        FILE_ATTRIBUTE_NORMAL |
			                (fd.dwFileAttributes &
			                        FILE_ATTRIBUTE_DIRECTORY));
			if (fd.dwFileAttributes &
			        FILE_ATTRIBUTE_REPARSE_POINT) {
				if (!DeleteFileW(path)) {
					RemoveDirectoryW(path);
				}
			} else if (fd.dwFileAttributes &
			        FILE_ATTRIBUTE_DIRECTORY) {
				gate_remove_tree(path);
			} else {
				DeleteFileW(path);
			}
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	RemoveDirectoryW(dir);
}

static bool gate_run(wchar_t *cmdline, DWORD ms, DWORD *code)
{
	STARTUPINFOW si = {0};
	PROCESS_INFORMATION pi = {0};
	bool ok;

	si.cb = sizeof si;
	if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE, CREATE_NO_WINDOW,
	            NULL, NULL, &si, &pi)) {
		return false;
	}
	ok = WAIT_OBJECT_0 == WaitForSingleObject(pi.hProcess, ms);
	if (!ok) {
		TerminateProcess(
		        pi.hProcess, 99); /* the harness's own helper */
	}
	GetExitCodeProcess(pi.hProcess, code);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return ok;
}

static bool gate_path(Gate *g, const wchar_t *rel, wchar_t *out)
{
	return 0 != p0_lab_path(&g->pr.lab, rel, out, 1100);
}

static bool gate_mkdir(Gate *g, const wchar_t *rel)
{
	wchar_t p[1100];

	return gate_path(g, rel, p) &&
	        (CreateDirectoryW(p, NULL) ||
	                ERROR_ALREADY_EXISTS == GetLastError());
}

/* An unresolved record for the older (or the new) store, made the way the
 * product makes one, then damaged as the case wants. */
static bool gate_record(
        Gate *g, const wchar_t *state, const wchar_t *id, GateRecord kind)
{
	wchar_t rel[160], root[1100], file[1200];
	WfRecoveryStore *store = NULL;
	DWORD error = 0;
	bool ok;

	if (REC_NONE == kind) {
		return true;
	}
	if (!gate_mkdir(g, state)) {
		return false;
	}
	if (REC_RECEIPTS_BAD == kind || REC_RECEIPTS_DIR == kind) {
		swprintf(rel, 160, L"%ls\\copy-journal.txt", state);
		if (!gate_path(g, rel, file)) {
			return false;
		}
		if (REC_RECEIPTS_DIR == kind) {
			return CreateDirectoryW(file, NULL) != 0;
		}
		return gate_write(file, "not a receipt inventory\r\n");
	}
	swprintf(rel, 160, L"%ls\\journal", state);
	if (!gate_path(g, rel, root) ||
	        !wf_recovery_store_open(root, true, &store, &error)) {
		return false;
	}
	if (REC_BAD_NAME == kind) {
		wf_recovery_store_close(store);
		swprintf(file, 1200, L"%ls\\garbage.wjr", root);
		return gate_write(file, "x");
	}
	ok = wf_recovery_store_put(
	        store, id, L"REMOVED-TEST-VOLUME", "journal", 7, &error);
	wf_recovery_store_close(store);
	if (!ok) {
		return false;
	}
	swprintf(file, 1200, L"%ls\\%ls.wjr", root, id);
	if (REC_BAD_CONTENT == kind) {
		return gate_write(file, "garbage that is no journal at all");
	}
	if (REC_NO_META == kind) {
		swprintf(file, 1200, L"%ls\\%ls.meta", root, id);
		return DeleteFileW(file) != 0;
	}
	if (REC_NO_JOURNAL == kind) {
		return DeleteFileW(file) != 0;
	}
	return true; /* REC_VALID, and REC_LOCKED until gate_lock() */
}

/* REC_LOCKED: the record is held open without sharing for as long as the
 * program runs (taken after the folder was listed, so that listing it works). */
static bool gate_lock(Gate *g, const wchar_t *state, const wchar_t *id)
{
	wchar_t rel[200], file[1200];

	swprintf(rel, 200, L"%ls\\journal\\%ls.wjr", state, id);
	if (!gate_path(g, rel, file)) {
		return false;
	}
	g->lock = CreateFileW(file, GENERIC_READ, 0, NULL, OPEN_EXISTING,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	return INVALID_HANDLE_VALUE != g->lock;
}

/* The owned area, and the files every case works on. */
static bool gate_open(Gate *g, Ev *e, const char *tag)
{
	Product *pr = &g->pr;

	memset(g, 0, sizeof *g);
	g->lock = INVALID_HANDLE_VALUE;
	snprintf(g->tag, sizeof g->tag, "%s", tag);
	pr->e = e;
	if (!p0_lab_open(&pr->lab, "state-gate", 1)) {
		ev_check(e, "isolated_fixture", 0, "owned NTFS root",
		        "unavailable", 0);
		return false;
	}
	wcscpy_s(g->area, 1024, p0_area_root(pr->lab.area));
	/* the area must be the harness's own, and not the user's profile */
	{
		wchar_t local[1024];
		DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 1024);
		bool owned = wcsstr(g->area, L"\\preflight-scratch\\") &&
		        !(n && n < 1024 && !_wcsnicmp(g->area, local, n));

		ev_check(e, "area_is_owned_and_isolated", 0,
		        "inside preflight-scratch, outside the real "
		        "LOCALAPPDATA",
		        owned ? "isolated" : "NOT isolated", owned);
		if (!owned) {
			return false;
		}
	}
	PRODUCT_REQUIRE(gate_mkdir(g, L"browse") && gate_mkdir(g, L"dest"));
	PRODUCT_REQUIRE(gate_path(g, L"browse\\doc.txt", g->doc) &&
	        gate_path(g, L"browse\\note.txt", g->note) &&
	        gate_path(g, L"browse\\note.txt~", g->backup) &&
	        gate_path(g, L"dest", g->dest) &&
	        gate_path(g, L"browse", g->browse) &&
	        gate_path(g, L"config.ini", g->cfg));
	PRODUCT_REQUIRE(gate_write(g->doc, GATE_DOC_TEXT) &&
	        gate_write(g->note, GATE_NOTE_TEXT));
	/* a script that Open must ask about and never run (gate_probe) */
	{
		wchar_t script[1100];

		PRODUCT_REQUIRE(gate_mkdir(g, L"probe") &&
		        gate_path(g, L"probe\\run.bat", script) &&
		        gate_write(script,
		                "@echo off\r\necho ran> \"%~dp0ran.txt\"\r\n"));
	}
	PRODUCT_REQUIRE(gate_write(g->cfg,
	        "[meta]\r\nschema_version = 1\r\nonboarding_completed = "
	        "true\r\n[updates]\r\nenabled = false\r\nconsent_recorded = "
	        "true\r\n"));
	PRODUCT_REQUIRE(
	        gate_snap(g->doc, &g->doc0) && gate_snap(g->note, &g->note0));
	g->dest0 = gate_tree_text(g->dest);
	g->browse0 = gate_tree_text(g->browse);
	PRODUCT_REQUIRE(g->dest0 && g->browse0);
	return true;
}

/* Evidence of the session (kept under the case's name), then the area. A
 * failing case keeps its area and says where it is. */
static void gate_close(Gate *g, bool keep)
{
	Product *pr = &g->pr;
	wchar_t rel[160];
	char note[1300];

	if (pr->started) {
		pty_snapshot(&pr->p, pr->screen, sizeof pr->screen);
		swprintf(rel, 160, L"artifacts\\%hs-screen.txt", g->tag);
		ev_write_text(pr->e, rel, pr->screen);
		EnterCriticalSection(&pr->p.lock);
		swprintf(rel, 160, L"traces\\%hs.vt", g->tag);
		ev_write_raw(pr->e, rel, pr->p.raw, pr->p.raw_n);
		LeaveCriticalSection(&pr->p.lock);
		pty_finish(&pr->p);
		pr->started = false;
	}
	if (INVALID_HANDLE_VALUE != g->lock && g->lock) {
		CloseHandle(g->lock);
		g->lock = INVALID_HANDLE_VALUE;
	}
	free(g->dest0);
	free(g->browse0);
	free(g->old0);
	g->dest0 = g->browse0 = g->old0 = NULL;
	free(pr->saved_local);
	free(pr->saved_roaming);
	pr->saved_local = pr->saved_roaming = NULL;
	p0_lab_close(&pr->lab, 1); /* forgets the area; it is removed below */
	if (keep) {
		snprintf(note, sizeof note, "Retained owned fixture: %ls\n",
		        g->area);
		swprintf(rel, 160, L"artifacts\\%hs-fixture.txt", g->tag);
		ev_write_text(pr->e, rel, note);
	} else if (wcsstr(g->area, L"\\preflight-scratch\\")) {
		gate_remove_tree(g->area);
	}
}

static bool gate_launch(Gate *g)
{
	Product *pr = &g->pr;
	wchar_t check[1100];
	DWORD need;
	int ok;

	free(pr->saved_local);
	free(pr->saved_roaming);
	pr->saved_local = pr->saved_roaming = NULL;
	need = GetEnvironmentVariableW(L"LOCALAPPDATA", NULL, 0);
	if (need) {
		pr->saved_local = malloc((size_t)need * sizeof(wchar_t));
		PRODUCT_REQUIRE(pr->saved_local &&
		        GetEnvironmentVariableW(
		                L"LOCALAPPDATA", pr->saved_local, need));
	}
	need = GetEnvironmentVariableW(L"APPDATA", NULL, 0);
	if (need) {
		pr->saved_roaming = malloc((size_t)need * sizeof(wchar_t));
		PRODUCT_REQUIRE(pr->saved_roaming &&
		        GetEnvironmentVariableW(
		                L"APPDATA", pr->saved_roaming, need));
	}
	PRODUCT_REQUIRE(
	        SetEnvironmentVariableW(L"LOCALAPPDATA", pr->lab.root) &&
	        SetEnvironmentVariableW(L"APPDATA", pr->lab.root));
	/* the child is started only if the isolation is what we say it is */
	PRODUCT_REQUIRE(GetEnvironmentVariableW(L"LOCALAPPDATA", check, 1100) &&
	        !_wcsicmp(check, pr->lab.root) &&
	        GetEnvironmentVariableW(L"APPDATA", check, 1100) &&
	        !_wcsicmp(check, pr->lab.root));
	{
		wchar_t start[1110] = L"";

		if (!g->no_start_path) {
			swprintf(start, 1110, L" \"%ls\"", g->browse);
		}
		swprintf(pr->command, 2048,
		        L"\"%ls\" --language %ls --config \"%ls\"%ls%ls%ls",
		        g_child, g->lang ? g->lang : L"en", g->cfg,
		        g->extra_args ? L" " : L"",
		        g->extra_args ? g->extra_args : L"", start);
	}
	pr->started = pty_start(&pr->p, pr->command, 160, 30);
	PRODUCT_REQUIRE(
	        SetEnvironmentVariableW(L"LOCALAPPDATA", pr->saved_local) &&
	        SetEnvironmentVariableW(L"APPDATA", pr->saved_roaming));
	PRODUCT_REQUIRE(pr->started);
	{
		double t0 = now_ms();
		char took[64];

		ok = product_wait_ms(&pr->p,
		        g->wait_for ? g->wait_for : "doc.txt", pr->screen,
		        sizeof pr->screen, 30000);
		snprintf(took, sizeof took, "%s after %.0f ms",
		        ok ? "observed" : "missing", now_ms() - t0);
		ev_check(pr->e, "program_started", 0,
		        "listing of the fixture folder", took, ok);
	}
	return ok;
}

/* q, and the exit within a bound. A program that does not leave is a failure. */
static bool gate_quit(Gate *g)
{
	Product *pr = &g->pr;
	DWORD code = 99;
	bool ok;
	char name[96];

	pty_send(&pr->p, "q", 1);
	ok = WAIT_OBJECT_0 == WaitForSingleObject(pr->p.pi.hProcess, 5000);
	if (ok) {
		GetExitCodeProcess(pr->p.pi.hProcess, &code);
	}
	snprintf(name, sizeof name, "%s_exits_cleanly", g->tag);
	ev_check(pr->e, name, 0, "exit 0 within 5 s",
	        ok && 0 == code ? "exit 0" : "stuck or failed",
	        ok && 0 == code);
	if (!ok) {
		return false;
	}
	/* the program is gone: its lock on the record can go too */
	if (INVALID_HANDLE_VALUE != g->lock && g->lock) {
		CloseHandle(g->lock);
		g->lock = INVALID_HANDLE_VALUE;
	}
	pty_finish(&pr->p);
	pr->started = false;
	return code == 0;
}

/* Runs `piero.exe` with one argument line and `input` on its stdin; its
 * output goes to `out`. LOCALAPPDATA and APPDATA are the owned area's. */
static int gate_cli(
        Gate *g, const wchar_t *args, const char *input, char *out, size_t cap)
{
	Product *pr = &g->pr;
	HANDLE in_r, in_w, log;
	SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
	STARTUPINFOW si = {0};
	PROCESS_INFORMATION pi = {0};
	wchar_t cmd[2048], logrel[64], logpath[1100];
	DWORD code = 99, wrote;
	wchar_t *saved_local = NULL, *saved_roaming = NULL;
	DWORD need;
	bool started;

	out[0] = '\0';
	swprintf(logrel, 64, L"cli-%d.log", ++g->cli_n);
	if (!gate_path(g, logrel, logpath) ||
	        !CreatePipe(&in_r, &in_w, &sa, 0)) {
		return 99;
	}
	SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
	log = CreateFileW(logpath, GENERIC_WRITE, FILE_SHARE_READ, &sa,
	        CREATE_NEW, 0, NULL);
	if (INVALID_HANDLE_VALUE == log) {
		CloseHandle(in_r);
		CloseHandle(in_w);
		return 99;
	}
	need = GetEnvironmentVariableW(L"LOCALAPPDATA", NULL, 0);
	if (need) {
		saved_local = malloc((size_t)need * sizeof(wchar_t));
		GetEnvironmentVariableW(L"LOCALAPPDATA", saved_local, need);
	}
	need = GetEnvironmentVariableW(L"APPDATA", NULL, 0);
	if (need) {
		saved_roaming = malloc((size_t)need * sizeof(wchar_t));
		GetEnvironmentVariableW(L"APPDATA", saved_roaming, need);
	}
	SetEnvironmentVariableW(L"LOCALAPPDATA", pr->lab.root);
	SetEnvironmentVariableW(L"APPDATA", pr->lab.root);
	si.cb = sizeof si;
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = in_r;
	si.hStdOutput = si.hStdError = log;
	swprintf(cmd, 2048, L"\"%ls\" %ls", g_child, args);
	started = CreateProcessW(g_child, cmd, NULL, NULL, TRUE,
	                  CREATE_NO_WINDOW, NULL, NULL, &si, &pi) != 0;
	SetEnvironmentVariableW(L"LOCALAPPDATA", saved_local);
	SetEnvironmentVariableW(L"APPDATA", saved_roaming);
	free(saved_local);
	free(saved_roaming);
	CloseHandle(in_r);
	CloseHandle(log);
	if (started) {
		if (input) {
			WriteFile(in_w, input, (DWORD)strlen(input), &wrote,
			        NULL);
		}
		CloseHandle(in_w);
		if (WAIT_OBJECT_0 != WaitForSingleObject(pi.hProcess, 8000)) {
			TerminateProcess(pi.hProcess, 99); /* our own child */
			WaitForSingleObject(pi.hProcess, 5000);
		}
		GetExitCodeProcess(pi.hProcess, &code);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
	} else {
		CloseHandle(in_w);
	}
	read_small(logpath, out, cap);
	return (int)code;
}

#define GATE_STEP(keys, needle, suffix)                                        \
	do {                                                                   \
		snprintf(name, sizeof name, "%s_%s", g->tag, suffix);          \
		PRODUCT_STEP(keys, needle, name);                              \
	} while (0)

/*
 * Tries the two changes the gate is for, with the real keys: a replacing save
 * in the editor, and a copy. `blocked` says what must happen. Either way the
 * outcome is judged on disk, not on the words on the screen.
 */
/* badge: 1 the header must say "Needs review", 0 it must not, -1 either way */
static bool gate_session(
        Gate *g, bool blocked, int badge, const char *copy_refusal)
{
	Product *pr = &g->pr;
	char name[96], text[256];
	GateSnap now;
	int ok;
	char *tree;
	wchar_t copy[1200];

	if (!gate_launch(g)) {
		return false;
	}
	/* The program shows what it found on its own screen: a header badge
	 * ("Needs review") when its recovery information cannot be called
	 * clean. (Its startup lines go to stderr, which a pseudo console does
	 * not carry.) A start that is allowed shows none. */
	ok = product_wait(&pr->p, "NORMAL", pr->screen, sizeof pr->screen) &&
	        NULL != strstr(pr->screen, "Needs review");
	snprintf(name, sizeof name, "%s_startup_detects", g->tag);
	product_snapshot(pr, name);
	ev_check(pr->e, name, 0,
	        badge < 0       ? "either way (the refusal is what counts)"
	                : badge ? "the header says: Needs review"
	                        : "no safety badge",
	        ok ? "badge shown" : "no badge", badge < 0 || ok == badge);
	if (badge >= 0 && ok != badge) {
		return false;
	}

	/* ---- the editor: a replacing save */
	GATE_STEP("\x1b[1;5B", "note.txt", "cursor_to_note");
	GATE_STEP("e", "Ln 1/3", "editor_opens");
	GATE_STEP("X", "[Modified]", "editor_typing");
	pty_send(&pr->p, "\x13", 1); /* Ctrl+S */
	ok = product_wait(&pr->p, blocked ? GATE_SAVE_REFUSED : "Saved",
	        pr->screen, sizeof pr->screen);
	snprintf(name, sizeof name, "%s_editor_ctrl_s", g->tag);
	product_snapshot(pr, name);
	ev_check(pr->e, name, 0, blocked ? "the save is refused" : "Saved",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		return false;
	}
	Sleep(250);
	ok = gate_snap(g->note, &now);
	if (blocked) {
		/* the file is the very same file with the very same bytes, and
		 * nothing was made beside it */
		bool same = ok && gate_same(&now, &g->note0);
		char *after = gate_tree_text(g->browse);
		bool listing = after && !strcmp(after, g->browse0);

		free(after);
		snprintf(name, sizeof name, "%s_note_unchanged", g->tag);
		ev_check(pr->e, name, 0, "same SHA-256 and FileId",
		        same ? "unchanged" : "CHANGED", same);
		snprintf(
		        name, sizeof name, "%s_no_backup_or_temporary", g->tag);
		ev_check(pr->e, name, 0, "the folder lists exactly as before",
		        listing ? "same listing" : "NEW FILES", listing);
		if (!same || !listing) {
			return false;
		}
		/* the lists come back, and the keyboard still works */
		GATE_STEP("\x18", "Save changes to note.txt?",
		        "editor_leave_asks");
		GATE_STEP("d", "NORMAL", "editor_discard_returns");
		ok = gate_snap(g->note, &now) && gate_same(&now, &g->note0);
		snprintf(name, sizeof name, "%s_note_unchanged_after_discard",
		        g->tag);
		ev_check(pr->e, name, 0, "same SHA-256 and FileId",
		        ok ? "unchanged" : "CHANGED", ok);
		if (!ok) {
			return false;
		}
	} else {
		bool saved = read_text_file(g->note, text, sizeof text) &&
		        !strcmp(text, "Xhello\r\nworld\r\n");
		bool kept = read_text_file(g->backup, text, sizeof text) &&
		        !strcmp(text, GATE_NOTE_TEXT);

		snprintf(name, sizeof name, "%s_saved_with_backup", g->tag);
		ev_check(pr->e, name, 0,
		        "new text on disk, original in note.txt~",
		        saved && kept ? "yes" : "no", saved && kept);
		if (!saved || !kept) {
			return false;
		}
		GATE_STEP("\x18", "NORMAL", "editor_leaves");
	}

	/* ---- the copy */
	GATE_STEP("\x1b[1;5A", "doc.txt", "cursor_to_doc");
	GATE_STEP("y", "COPY 1", "copy_captured");
	GATE_STEP("g", "PATH", "copy_path_prompt");
	{
		char utf8[1100];

		PRODUCT_REQUIRE(WideCharToMultiByte(CP_UTF8, 0, g->dest, -1,
		        utf8, sizeof utf8, NULL, NULL));
		pty_send(&pr->p, utf8, strlen(utf8));
	}
	GATE_STEP("\r", "0 items", "copy_destination");
	pty_send(&pr->p, "p", 1);
	ok = product_wait(&pr->p,
	        blocked ? (copy_refusal ? copy_refusal : GATE_COPY_REFUSED)
	                : "COPY done: 1 file(s)",
	        pr->screen, sizeof pr->screen);
	snprintf(name, sizeof name, "%s_copy_paste", g->tag);
	product_snapshot(pr, name);
	ev_check(pr->e, name, 0,
	        blocked ? "the copy is refused" : "COPY done: 1 file(s)",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		return false;
	}
	if (blocked) {
		/* an internal name is never the explanation */
		ok = NULL == strstr(pr->screen, "P0-STORAGE") &&
		        NULL == strstr(pr->screen, "engine disabled");
		snprintf(name, sizeof name, "%s_copy_refusal_no_internal_names",
		        g->tag);
		ev_check(pr->e, name, 0, "no gate or engine name on screen",
		        ok ? "none" : "SHOWN", ok);
		if (!ok) {
			return false;
		}
	}
	Sleep(400); /* a job that was wrongly accepted would be finished by now */
	swprintf(copy, 1200, L"%ls\\doc.txt", g->dest);
	tree = gate_tree_text(g->dest);
	if (blocked) {
		bool empty =
		        tree && !strcmp(tree, g->dest0) && !file_exists(copy);

		snprintf(name, sizeof name, "%s_destination_untouched", g->tag);
		ev_check(pr->e, name, 0, "the destination lists as before",
		        empty ? "unchanged" : "CHANGED", empty);
		free(tree);
		if (!empty) {
			return false;
		}
	} else {
		bool made = same_small_file(g->doc, copy) &&
		        0 == copy_temporaries(g->dest);

		free(tree);
		snprintf(name, sizeof name, "%s_copy_made", g->tag);
		ev_check(pr->e, name, 0, "same bytes, no temporary",
		        made ? "identical" : "different", made);
		if (!made) {
			return false;
		}
	}
	ok = gate_snap(g->doc, &now) && gate_same(&now, &g->doc0);
	snprintf(name, sizeof name, "%s_source_unchanged", g->tag);
	ev_check(pr->e, name, 0, "same SHA-256 and FileId",
	        ok ? "unchanged" : "CHANGED", ok);
	if (!ok) {
		return false;
	}
	if (blocked) {
		/* the program is still answering: it goes back to the folder
		 * and lists it, and the files are still there */
		if (!go_by_path_bar(
		            pr, g->browse, "still_responds_goes_back")) {
			return false;
		}
		ok = product_wait(
		        &pr->p, "doc.txt", pr->screen, sizeof pr->screen);
		snprintf(name, sizeof name, "%s_still_responds", g->tag);
		ev_check(pr->e, name, 0, "the folder is listed again",
		        ok ? "observed" : "missing", ok);
		if (!ok) {
			return false;
		}
	}
	return gate_quit(g);
}

static int g_gate_skips;

/* PIERO_GATE_ONLY=<text> runs the cases whose name contains it (a developer's
 * filter; the CTest entry runs them all). */
static bool gate_wanted(const char *tag)
{
	char only[64];
	DWORD n = GetEnvironmentVariableA("PIERO_GATE_ONLY", only, sizeof only);

	return !n || n >= sizeof only || NULL != strstr(tag, only);
}

static bool gate_check(Gate *g, const char *what, bool ok, const char *expected)
{
	char name[96];

	snprintf(name, sizeof name, "%s_%s", g->tag, what);
	ev_check(g->pr.e, name, 0, expected, ok ? "yes" : "NO", ok);
	return ok;
}

/* A folder's content, as text, equal to what it was. */
static bool gate_tree_is(const wchar_t *dir, const char *before)
{
	char *now = gate_tree_text(dir);
	bool same = now && before && !strcmp(now, before);

	free(now);
	return same;
}

typedef struct
{
	const char *tag;
	bool piero_dir; /* %LOCALAPPDATA%\piero exists */
	bool wfile_dir; /* %LOCALAPPDATA%\wfile exists (empty unless a record) */
	GateRecord old_kind; /* what the older folder holds */
	bool blocked;        /* the program must refuse to copy and replace */
	const char *why;     /* a reason the program must give at startup */
} GateCase;

/* Builds the state of a case in the owned area and starts from it. */
static bool gate_prepare(Gate *g, Ev *e, const GateCase *c)
{
	wchar_t old_dir[1100];

	if (!gate_open(g, e, c->tag)) {
		return false;
	}
	if (c->piero_dir && !gate_mkdir(g, L"piero")) {
		return false;
	}
	if ((c->wfile_dir || c->old_kind != REC_NONE) &&
	        !gate_mkdir(g, L"wfile")) {
		return false;
	}
	if (!gate_record(g, L"wfile", GATE_ID_OLD, c->old_kind)) {
		return gate_check(g, "fixture_record", false, "record made");
	}
	if (gate_path(g, L"wfile", old_dir) && file_exists(old_dir)) {
		g->old0 = gate_tree_text(old_dir);
	}
	if (REC_LOCKED == c->old_kind && !gate_lock(g, L"wfile", GATE_ID_OLD)) {
		return gate_check(g, "fixture_lock", false, "record held open");
	}
	return true;
}

/* One start of the program from that state, and the verdict on the older
 * folder afterwards: the program only ever reads it. */
static bool gate_case(Ev *e, const GateCase *c)
{
	Gate *g = calloc(1, sizeof *g);
	bool ok = false;
	int fails = e->fails;

	if (!g || !gate_wanted(c->tag)) {
		free(g);
		return true;
	}
	if (gate_prepare(g, e, c)) {
		ok = gate_session(g, c->blocked, c->blocked, NULL);
		if (ok && c->piero_dir && g->old0) {
			wchar_t old_dir[1100];

			ok = gate_path(g, L"wfile", old_dir) &&
			        gate_check(g, "older_folder_unchanged",
			                gate_tree_is(old_dir, g->old0),
			                "the older folder is byte for byte as "
			                "before");
		}
	}
	ok = ok && e->fails == fails;
	if (!ok) {
		ev_check(e, c->tag, 0, "case passes", "case FAILED", 0);
	}
	gate_close(g, !ok);
	free(g);
	return ok;
}

static bool gate_resolution(Ev *e)
{
	static const char tag[] = "resolve";
	Gate *g;
	bool ok = false;
	char out[4096];
	wchar_t args[200], old_journal[1100], new_journal[1100], old_wjr[1200];
	char *new_before = NULL, *old_before = NULL;
	int code;
	int fails = e->fails;

	if (!gate_wanted(tag)) {
		return true;
	}
	g = calloc(1, sizeof *g);
	if (!g) {
		return false;
	}
	if (!gate_open(g, e, tag) || !gate_mkdir(g, L"piero") ||
	        !gate_record(g, L"wfile", GATE_ID_OLD, REC_VALID) ||
	        !gate_record(g, L"piero", GATE_ID_NEW, REC_VALID) ||
	        !gate_path(g, L"wfile\\journal", old_journal) ||
	        !gate_path(g, L"piero\\journal", new_journal)) {
		goto done;
	}
	swprintf(old_wjr, 1200, L"%ls\\%ls.wjr", old_journal, GATE_ID_OLD);
	new_before = gate_tree_text(new_journal);
	old_before = gate_tree_text(old_journal);
	g->old0 = NULL;
	swprintf(args, 200, L"--abandon-recovery %ls", GATE_ID_OLD);

	/* 1: the older record is there, so the program refuses */
	if (!gate_session(g, true, true, NULL)) {
		goto done;
	}
	/* 2: a wrong confirmation changes nothing, and the gate stays */
	code = gate_cli(g, args, "wrong\n", out, sizeof out);
	if (!gate_check(g, "wrong_id_refused",
	            3 == code &&
	                    strstr(out,
	                            "Recovery ID: "
	                            "11111111") != NULL &&
	                    file_exists(old_wjr),
	            "exit 3, the older record shown and kept") ||
	        !gate_check(g, "older_store_unchanged_after_refusal",
	                gate_tree_is(old_journal, old_before),
	                "byte for byte as before")) {
		goto done;
	}
	if (!gate_session(g, true, true, NULL)) {
		goto done;
	}
	/* 3: the exact ID resolves it, and only it */
	code = gate_cli(g, args, "11111111-1111-1111-1111-111111111111\n", out,
	        sizeof out);
	if (!gate_check(g, "exact_id_abandons",
	            0 == code && strstr(out, "RECOVERY_ABANDONED") != NULL &&
	                    !file_exists(old_wjr),
	            "exit 0, the older record gone") ||
	        !gate_check(g, "the_other_store_is_untouched",
	                gate_tree_is(new_journal, new_before),
	                "the piero store is byte for byte as before")) {
		goto done;
	}
	/* 4: the next start is free: replacing saves and copies work again */
	ok = gate_session(g, false, true,
	        NULL); /* the piero store keeps its own record */
done:
	ok = ok && e->fails == fails;
	if (!ok) {
		ev_check(e, tag, 0, "case passes", "case FAILED", 0);
	}
	free(new_before);
	free(old_before);
	gate_close(g, !ok);
	free(g);
	return ok;
}

typedef enum
{
	SEL_NEW_ONLY,
	SEL_OLD_ONLY,
	SEL_BOTH,
	SEL_NEITHER,
	SEL_FILE,
	SEL_JUNCTION,
	SEL_DENIED
} SelKind;

static bool gate_dir(const wchar_t *path)
{
	DWORD a = GetFileAttributesW(path);

	return INVALID_FILE_ATTRIBUTES != a && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool gate_selection(Ev *e, SelKind kind, const char *tag)
{
	Gate *g;
	bool blocked =
	        SEL_FILE == kind || SEL_JUNCTION == kind || SEL_DENIED == kind;
	bool ok = false, denied = false;
	const wchar_t *cli_id =
	        SEL_OLD_ONLY == kind ? GATE_ID_OLD : GATE_ID_NEW;
	wchar_t piero[1100], wfile[1100], target[1100], args[200], cmd[2300],
	        journal[1100];
	char out[4096], shown[96];
	char *piero_before = NULL, *wfile_before = NULL;
	int code, fails = e->fails;
	DWORD rc = 0;

	if (!gate_wanted(tag)) {
		return true;
	}
	g = calloc(1, sizeof *g);
	if (!g || !gate_open(g, e, tag)) {
		goto done;
	}
	if (!gate_path(g, L"piero", piero) || !gate_path(g, L"wfile", wfile)) {
		goto done;
	}
	switch (kind) {
	case SEL_NEW_ONLY:
		ok = gate_mkdir(g, L"piero") &&
		        gate_record(g, L"piero", GATE_ID_NEW, REC_VALID);
		break;
	case SEL_OLD_ONLY:
		ok = gate_record(g, L"wfile", GATE_ID_OLD, REC_VALID);
		break;
	case SEL_BOTH:
		ok = gate_mkdir(g, L"wfile") &&
		        gate_record(g, L"piero", GATE_ID_NEW, REC_VALID);
		break;
	case SEL_NEITHER: ok = true; break;
	case SEL_FILE:    ok = gate_write(piero, "not a folder"); break;
	case SEL_JUNCTION:
		ok = gate_mkdir(g, L"junction-target") &&
		        gate_path(g, L"junction-target", target);
		if (ok) {
			swprintf(cmd, 2300,
			        L"cmd.exe /c mklink /J \"%ls\" \"%ls\"", piero,
			        target);
			ok = gate_run(cmd, 10000, &rc) && 0 == rc;
		}
		break;
	case SEL_DENIED:
		ok = gate_mkdir(g, L"piero");
		if (ok) {
			swprintf(cmd, 2300,
			        L"icacls.exe \"%ls\" /deny "
			        L"*S-1-1-0:(OI)(CI)(F)",
			        piero);
			ok = gate_run(cmd, 10000, &rc) && 0 == rc;
			denied = ok;
			if (!ok) {
				/* no way to deny access here: not a pass */
				printf("SKIP %s: icacls could not deny "
				       "access\n",
				        tag);
				g_gate_skips++;
				ok = true;
				goto skipped;
			}
		}
		break;
	}
	if (!ok) {
		gate_check(g, "fixture", false, "state made");
		goto done;
	}
	/* the program starts from it */
	/* an unresolved record in the folder in use is a notice (the badge),
	 * not a stop */
	ok = gate_session(g, blocked,
	        SEL_DENIED == kind ? -1
	                           : blocked || SEL_NEW_ONLY == kind ||
	                        SEL_OLD_ONLY == kind || SEL_BOTH == kind,
	        /* the copy is refused for the reason that is true of the state:
		 * the folder cannot be used, or the record cannot be written */
	        SEL_DENIED == kind ? GATE_COPY_NO_RECORD
	                : SEL_FILE == kind || SEL_JUNCTION == kind
	                ? GATE_COPY_NO_STATE
	                : NULL);
	if (!ok) {
		goto done;
	}
	swprintf(journal, 1100, L"%ls\\journal", piero);
	piero_before = gate_dir(piero) &&
	                !(GetFileAttributesW(piero) &
	                        FILE_ATTRIBUTE_REPARSE_POINT) &&
	                !denied
	        ? gate_tree_text(piero)
	        : NULL;
	wfile_before = gate_dir(wfile) ? gate_tree_text(wfile) : NULL;

	/* which folder the program used */
	{
		wchar_t cj[1200];

		switch (kind) {
		case SEL_NEW_ONLY:
		case SEL_BOTH:
		case SEL_NEITHER:
			swprintf(cj, 1200, L"%ls\\copy-journal.txt", piero);
			ok = gate_check(g, "program_used_piero",
			        gate_dir(piero) && file_exists(cj) &&
			                (SEL_BOTH == kind || !gate_dir(wfile)),
			        "piero holds the copy journal, wfile was not "
			        "made");
			break;
		case SEL_OLD_ONLY:
			swprintf(cj, 1200, L"%ls\\copy-journal.txt", wfile);
			ok = gate_check(g, "program_used_wfile",
			        gate_dir(wfile) && file_exists(cj) &&
			                !file_exists(piero),
			        "wfile holds the copy journal, piero was not "
			        "made");
			break;
		case SEL_FILE:
			ok = gate_check(g, "name_left_alone",
			        !gate_dir(piero) && file_exists(piero) &&
			                !gate_dir(wfile),
			        "the file is still a file, nothing was made");
			break;
		case SEL_JUNCTION:
			ok = gate_check(g, "link_not_followed",
			        gate_tree_is(target, "") &&
			                (GetFileAttributesW(piero) &
			                        FILE_ATTRIBUTE_REPARSE_POINT),
			        "the link is still a link and its target is "
			        "empty");
			break;
		case SEL_DENIED:
			ok = gate_check(g, "denied_folder_left_alone",
			        !gate_dir(wfile), "wfile was not made");
			break;
		}
		if (!ok) {
			goto done;
		}
	}
	/* the recovery command chooses the same folder */
	swprintf(args, 200, L"--abandon-recovery %ls", cli_id);
	code = gate_cli(g, args, "wrong\n", out, sizeof out);
	snprintf(shown, sizeof shown, "Recovery ID: %.8ls", cli_id);
	switch (kind) {
	case SEL_NEW_ONLY:
	case SEL_OLD_ONLY:
	case SEL_BOTH:
		ok = gate_check(g, "cli_finds_the_record_in_the_same_folder",
		        3 == code && strstr(out, shown) != NULL,
		        "exit 3 after showing the record, nothing abandoned");
		break;
	case SEL_NEITHER:
	case SEL_FILE:
	case SEL_JUNCTION:
	case SEL_DENIED:
		ok = gate_check(g, "cli_finds_no_state",
		        3 == code && strstr(out, "unavailable") != NULL &&
		                NULL == strstr(out, "Recovery ID:"),
		        "exit 3, recovery metadata unavailable");
		break;
	}
	if (ok && SEL_BOTH == kind) {
		/* an ID neither store has: nothing is found, nothing changes */
		code = gate_cli(g,
		        L"--abandon-recovery "
		        L"99999999-9999-9999-9999-999999999999",
		        "wrong\n", out, sizeof out);
		ok = gate_check(g, "cli_unknown_id",
		        3 == code && strstr(out, "not found") != NULL,
		        "exit 3, recovery ID not found");
	}
	/* and the command only looked */
	if (ok) {
		bool same = true;

		if (piero_before) {
			same = same && gate_tree_is(piero, piero_before);
		}
		if (wfile_before) {
			same = same && gate_tree_is(wfile, wfile_before);
		}
		ok = gate_check(g, "cli_changed_nothing", same,
		        "both state folders are byte for byte as before");
	}
done:
	if (SEL_DENIED == kind && denied && g) {
		/* the owned area must be removable again */
		swprintf(cmd, 2300, L"icacls.exe \"%ls\" /remove:d *S-1-1-0",
		        piero);
		gate_run(cmd, 10000, &rc);
	}
skipped:
	ok = ok && e->fails == fails;
	if (g) {
		if (!ok) {
			ev_check(e, tag, 0, "case passes", "case FAILED", 0);
		}
		gate_close(g, !ok);
	}
	free(piero_before);
	free(wfile_before);
	free(g);
	return ok;
}

static int scn_state_gate(Ev *e)
{
	/* A and B: the older folder holds something the program must not step over */
	static const GateCase blocked[] = {
	        {"gate_unresolved_record", true, true, REC_VALID, true,
	                "older state folder"},
	        {"gate_bad_record_name", true, true, REC_BAD_NAME, true,
	                "older state folder"},
	        {"gate_bad_record_content", true, true, REC_BAD_CONTENT, true,
	                "older state folder"},
	        {"gate_journal_without_metadata", true, true, REC_NO_META, true,
	                "older state folder"},
	        {"gate_metadata_without_journal", true, true, REC_NO_JOURNAL,
	                true, "older state folder"},
	        {"gate_unreadable_record", true, true, REC_LOCKED, true,
	                "older state folder"},
	        {"gate_damaged_receipts", true, true, REC_RECEIPTS_BAD, true,
	                "older state folder"},
	        {"gate_receipts_are_a_folder", true, true, REC_RECEIPTS_DIR,
	                true, "older state folder"},
	};
	/* the same session where nothing is wrong: the keys do work */
	static const GateCase open[] = {
	        {"control_both_older_empty", true, true, REC_NONE, false, NULL},
	        {"control_only_piero", true, false, REC_NONE, false, NULL},
	        {"control_only_wfile", false, true, REC_NONE, false, NULL},
	        {"control_neither", false, false, REC_NONE, false, NULL},
	};
	bool good = true;
	size_t i;

	for (i = 0; i < sizeof open / sizeof open[0]; i++) {
		good = gate_case(e, &open[i]) && good;
	}
	for (i = 0; i < sizeof blocked / sizeof blocked[0]; i++) {
		good = gate_case(e, &blocked[i]) && good;
	}
	good = gate_resolution(e) && good;
	good = gate_selection(e, SEL_NEW_ONLY, "select_only_piero") && good;
	good = gate_selection(e, SEL_OLD_ONLY, "select_only_wfile") && good;
	good = gate_selection(e, SEL_BOTH, "select_both") && good;
	good = gate_selection(e, SEL_NEITHER, "select_neither") && good;
	good = gate_selection(e, SEL_FILE, "select_file_in_place") && good;
	good = gate_selection(e, SEL_JUNCTION, "select_junction") && good;
	good = gate_selection(e, SEL_DENIED, "select_access_denied") && good;
	if (!good || e->fails) {
		return 1;
	}
	return g_gate_skips ? 77 : 0;
}

/*
 * The program keeps a private folder (owner only, no inherited rights, one
 * entry) inside its state folder for the working directory of what it opens
 * and for its logs. These cases start the real program from a state that is
 * new, left by a normal exit, made by someone else, linked, denied, or holding
 * something in the working directory, and look at the disk and at what Open
 * does. The folders are judged here by the same facts the program uses: owner,
 * protected DACL, one entry, not a link.
 */
typedef struct
{
	bool found, directory, link, owner_is_user, protected_dacl;
	DWORD aces;
	wchar_t sddl[600];
} GatePrivacy;

static bool gate_privacy(const wchar_t *path, GatePrivacy *p)
{
	HANDLE token = NULL, h;
	TOKEN_USER *user = NULL;
	DWORD bytes = 0, revision;
	PSID owner = NULL;
	PACL dacl = NULL;
	PSECURITY_DESCRIPTOR sd = NULL;
	SECURITY_DESCRIPTOR_CONTROL control = 0;
	FILE_ATTRIBUTE_TAG_INFO tag;
	wchar_t *text = NULL;
	bool ok = false;

	memset(p, 0, sizeof *p);
	h = CreateFileW(path, READ_CONTROL | FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return false;
	}
	p->found = true;
	if (!GetFileInformationByHandleEx(
	            h, FileAttributeTagInfo, &tag, sizeof tag) ||
	        GetSecurityInfo(h, SE_FILE_OBJECT,
	                OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
	                &owner, NULL, &dacl, NULL, &sd) != ERROR_SUCCESS ||
	        !GetSecurityDescriptorControl(sd, &control, &revision) ||
	        !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
		goto done;
	}
	GetTokenInformation(token, TokenUser, NULL, 0, &bytes);
	user = bytes ? malloc(bytes) : NULL;
	if (!user ||
	        !GetTokenInformation(token, TokenUser, user, bytes, &bytes)) {
		goto done;
	}
	p->directory = 0 != (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY);
	p->link = 0 != (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
	p->owner_is_user = owner && EqualSid(owner, user->User.Sid);
	p->protected_dacl = 0 != (control & SE_DACL_PROTECTED);
	p->aces = dacl ? dacl->AceCount : 0;
	if (ConvertSecurityDescriptorToStringSecurityDescriptorW(sd,
	            SDDL_REVISION_1,
	            OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
	            &text, NULL)) {
		wcsncpy_s(p->sddl, 600, text, _TRUNCATE);
		LocalFree(text);
	}
	ok = true;
done:
	if (sd) {
		LocalFree(sd);
	}
	if (token) {
		CloseHandle(token);
	}
	free(user);
	CloseHandle(h);
	return ok;
}

static bool gate_is_private(const GatePrivacy *p)
{
	return p->found && p->directory && !p->link && p->owner_is_user &&
	        p->protected_dacl && 1 == p->aces;
}

/* A folder made the way the program makes its own: owner only. */
static bool gate_mkdir_private(Gate *g, const wchar_t *rel)
{
	wchar_t path[1100], sddl[512], *sid = NULL;
	HANDLE token = NULL;
	TOKEN_USER *user = NULL;
	DWORD bytes = 0;
	SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, FALSE};
	bool ok = false;

	if (!gate_path(g, rel, path) ||
	        !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
		return false;
	}
	GetTokenInformation(token, TokenUser, NULL, 0, &bytes);
	user = bytes ? malloc(bytes) : NULL;
	if (user &&
	        GetTokenInformation(token, TokenUser, user, bytes, &bytes) &&
	        ConvertSidToStringSidW(user->User.Sid, &sid)) {
		/* Elevated TokenOwner may be Administrators. Match the product's
		 * explicit user owner as well as its protected, user-only DACL. */
		swprintf(sddl, 512, L"O:%lsD:P(A;;FA;;;%ls)", sid, sid);
		if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl,
		            SDDL_REVISION_1, &sa.lpSecurityDescriptor, NULL)) {
			PSID owner = NULL;
			BOOL defaulted = TRUE;
			bool owned = GetSecurityDescriptorOwner(
			                     sa.lpSecurityDescriptor, &owner,
			                     &defaulted) &&
			        owner && !defaulted &&
			        EqualSid(owner, user->User.Sid);

			gate_check(g, "fixture_explicit_user_owner", owned,
			        "fixture descriptor explicitly names "
			        "TokenUser");
			ok = owned && 0 != CreateDirectoryW(path, &sa);
			LocalFree(sa.lpSecurityDescriptor);
		}
	}
	if (sid) {
		LocalFree(sid);
	}
	CloseHandle(token);
	free(user);
	return ok;
}

/* Ctrl+L, the path, Enter; done when the mode label is back (in the language
 * of the session). */
static bool gate_goto(Gate *g, const wchar_t *dir, const char *name)
{
	Product *pr = &g->pr;
	char utf8[1024], keys[1100];
	int ok;

	PRODUCT_REQUIRE(WideCharToMultiByte(
	        CP_UTF8, 0, dir, -1, utf8, sizeof utf8, NULL, NULL));
	snprintf(keys, sizeof keys, "\x0c%s\r", utf8);
	PRODUCT_STEP(keys,
	        g->lang && !wcscmp(g->lang, L"ja") ? "通常" : "NORMAL", name);
	return true;
}

/*
 * Open, through the keys. Enabled: Enter on a script asks first; Esc runs
 * nothing. Refused: the program says so at once. Either way the script did not
 * run.
 */
static bool gate_probe(Gate *g, bool enabled, const char *refusal)
{
	Product *pr = &g->pr;
	wchar_t dir[1100], ran[1200];
	char name[96];
	int ok;

	if (!gate_path(g, L"probe", dir)) {
		return false;
	}
	snprintf(name, sizeof name, "%s_probe_goes_there", g->tag);
	if (!gate_goto(g, dir, name)) {
		return false;
	}
	if (enabled) {
		GATE_STEP("\r", "Opening can run code", "open_asks");
		Sleep(350); /* the question ignores input for 300 ms */
		GATE_STEP("\x1b", "NORMAL", "open_cancelled");
	} else {
		GATE_STEP("\r", refusal, "open_refused");
	}
	Sleep(800);
	swprintf(ran, 1200, L"%ls\\ran.txt", dir);
	snprintf(name, sizeof name, "%s_script_did_not_run", g->tag);
	ev_check(pr->e, name, 0, "no ran.txt",
	        file_exists(ran) ? "ran" : "absent", !file_exists(ran));
	return !file_exists(ran);
}

static bool gate_probe_session(Gate *g, bool enabled, const char *refusal)
{
	return gate_launch(g) && gate_probe(g, enabled, refusal) &&
	        gate_quit(g);
}

/* What the program made, judged on the disk. */
static bool gate_made_private(Gate *g, const wchar_t *state)
{
	wchar_t rel[160], path[1100];
	GatePrivacy p;
	bool ok = true;
	static const wchar_t *const parts[] = {L"", L"\\cwd", L"\\logs"};

	for (size_t i = 0; i < 3; i++) {
		char what[64];

		swprintf(rel, 160, L"%ls%ls", state, parts[i]);
		ok = gate_path(g, rel, path) && ok;
		snprintf(what, sizeof what, "%ls_is_private", rel);
		ok = gate_check(g, what,
		             gate_privacy(path, &p) && gate_is_private(&p),
		             "owner only, no inherited rights, one entry, a "
		             "folder") &&
		        ok;
	}
	return ok;
}

static bool gate_first_run_end(Gate *g, Ev *e, bool ok, int fails)
{
	ok = ok && e->fails == fails;
	if (!ok) {
		ev_check(e, g->tag, 0, "case passes", "case FAILED", 0);
	}
	gate_close(g, !ok);
	free(g);
	return ok;
}

/* A new profile: the program makes the folders, a second start finds them as
 * they were, and Open works both times. */
static bool gate_first_run_fresh(Ev *e)
{
	static const char tag[] = "first_run_new_profile";
	Gate *g;
	bool ok = false;
	wchar_t piero[1100], cwd[1100], wfile[1100];
	GatePrivacy before, after, cwd_before, cwd_after;
	int fails = e->fails;

	if (!gate_wanted(tag) || !(g = calloc(1, sizeof *g))) {
		return true;
	}
	if (!gate_open(g, e, tag) || !gate_path(g, L"piero", piero) ||
	        !gate_path(g, L"piero\\cwd", cwd) ||
	        !gate_path(g, L"wfile", wfile)) {
		goto done;
	}
	if (!gate_check(g, "no_state_before",
	            !file_exists(piero) && !file_exists(wfile),
	            "neither state folder exists") ||
	        !gate_probe_session(g, true, NULL) ||
	        !gate_made_private(g, L"piero") ||
	        !gate_check(g, "older_name_not_made", !file_exists(wfile),
	                "wfile was not made") ||
	        !gate_check(g, "working_directory_is_empty",
	                gate_tree_is(cwd, ""),
	                "the working directory holds nothing")) {
		goto done;
	}
	ok = gate_privacy(piero, &before) && gate_privacy(cwd, &cwd_before);
	/* the second start, after a normal exit */
	ok = ok && gate_probe_session(g, true, NULL) &&
	        gate_privacy(piero, &after) && gate_privacy(cwd, &cwd_after) &&
	        gate_check(g, "second_start_left_the_folders_as_they_were",
	                !wcscmp(before.sddl, after.sddl) &&
	                        !wcscmp(cwd_before.sddl, cwd_after.sddl) &&
	                        gate_is_private(&after) &&
	                        gate_is_private(&cwd_after) &&
	                        gate_tree_is(cwd, ""),
	                "same owner and rights, still private, still empty");
done:
	return gate_first_run_end(g, e, ok, fails);
}

/* The folder was made by something else (a test harness, an installer, a
 * restore from a backup): it is not private, so the program does not use it
 * for Open or logs. It does not change it, and says why. */
static bool gate_first_run_foreign(Ev *e, const char *refusal)
{
	static const char tag[] = "first_run_folder_made_by_another_program";
	Gate *g;
	bool ok = false;
	wchar_t piero[1100], cwd[1100], logs[1100];
	GatePrivacy before, after;
	int fails = e->fails;

	if (!gate_wanted(tag) || !(g = calloc(1, sizeof *g))) {
		return true;
	}
	if (!gate_open(g, e, tag) || !gate_mkdir(g, L"piero") ||
	        !gate_path(g, L"piero", piero) ||
	        !gate_path(g, L"piero\\cwd", cwd) ||
	        !gate_path(g, L"piero\\logs", logs)) {
		goto done;
	}
	if (!gate_check(g, "premise_folder_is_not_private",
	            gate_privacy(piero, &before) && !gate_is_private(&before),
	            "the folder made here has inherited rights") ||
	        !gate_probe_session(g, false, refusal)) {
		goto done;
	}
	ok = gate_privacy(piero, &after) &&
	        gate_check(g, "folder_rights_not_touched",
	                !wcscmp(before.sddl, after.sddl),
	                "the program did not change the folder's rights") &&
	        gate_check(g, "no_working_directory_or_logs_in_it",
	                !file_exists(cwd) && !file_exists(logs),
	                "no cwd and no logs were made in a folder it cannot "
	                "trust");
done:
	return gate_first_run_end(g, e, ok, fails);
}

/* An older folder that is private is used as it is. */
static bool gate_first_run_legacy(Ev *e)
{
	static const char tag[] = "first_run_older_private_folder";
	Gate *g;
	bool ok = false;
	wchar_t piero[1100];
	int fails = e->fails;

	if (!gate_wanted(tag) || !(g = calloc(1, sizeof *g))) {
		return true;
	}
	if (!gate_open(g, e, tag) || !gate_mkdir_private(g, L"wfile") ||
	        !gate_path(g, L"piero", piero)) {
		goto done;
	}
	ok = gate_probe_session(g, true, NULL) &&
	        gate_made_private(g, L"wfile") &&
	        gate_check(g, "new_name_not_made", !file_exists(piero),
	                "piero was not made") &&
	        gate_probe_session(g, true, NULL);
done:
	return gate_first_run_end(g, e, ok, fails);
}

/* Both exist and are private: piero is used, wfile is not touched. */
static bool gate_first_run_both(Ev *e)
{
	static const char tag[] = "first_run_both_private";
	Gate *g;
	bool ok = false;
	wchar_t wfile[1100];
	char *old_before = NULL;
	int fails = e->fails;

	if (!gate_wanted(tag) || !(g = calloc(1, sizeof *g))) {
		return true;
	}
	if (!gate_open(g, e, tag) || !gate_mkdir_private(g, L"wfile") ||
	        !gate_mkdir_private(g, L"piero") ||
	        !gate_path(g, L"wfile", wfile)) {
		goto done;
	}
	old_before = gate_tree_text(wfile);
	ok = gate_probe_session(g, true, NULL) &&
	        gate_made_private(g, L"piero") &&
	        gate_check(g, "older_folder_untouched",
	                gate_tree_is(wfile, old_before),
	                "wfile is byte for byte as before") &&
	        gate_probe_session(g, true, NULL);
done:
	free(old_before);
	return gate_first_run_end(g, e, ok, fails);
}

/* The working directory is not empty at the next start (something put a file
 * there): the program refuses to use it, and does not delete the file. */
static bool gate_first_run_cwd_used(Ev *e, const char *refusal)
{
	static const char tag[] = "first_run_working_directory_not_empty";
	Gate *g;
	bool ok = false;
	wchar_t foreign[1100];
	char text[64];
	int fails = e->fails;

	if (!gate_wanted(tag) || !(g = calloc(1, sizeof *g))) {
		return true;
	}
	if (!gate_open(g, e, tag) || !gate_probe_session(g, true, NULL) ||
	        !gate_path(g, L"piero\\cwd\\foreign.txt", foreign) ||
	        !gate_write(foreign, "KEEP")) {
		goto done;
	}
	ok = gate_probe_session(g, false, refusal) &&
	        gate_check(g, "foreign_file_kept",
	                read_text_file(foreign, text, sizeof text) &&
	                        !strcmp(text, "KEEP"),
	                "the file in the working directory is still there");
done:
	return gate_first_run_end(g, e, ok, fails);
}

/* A link in place of the folder is not followed. */
static bool gate_first_run_link(Ev *e, const char *refusal)
{
	static const char tag[] = "first_run_state_folder_is_a_link";
	Gate *g;
	bool ok = false;
	wchar_t piero[1100], target[1100], cmd[2300];
	DWORD rc = 0;
	int fails = e->fails;

	if (!gate_wanted(tag) || !(g = calloc(1, sizeof *g))) {
		return true;
	}
	if (!gate_open(g, e, tag) || !gate_mkdir(g, L"link-target") ||
	        !gate_path(g, L"link-target", target) ||
	        !gate_path(g, L"piero", piero)) {
		goto done;
	}
	swprintf(cmd, 2300, L"cmd.exe /c mklink /J \"%ls\" \"%ls\"", piero,
	        target);
	if (!gate_run(cmd, 10000, &rc) || 0 != rc) {
		gate_check(g, "fixture", false, "a junction was made");
		goto done;
	}
	ok = gate_probe_session(g, false, refusal) &&
	        gate_check(g, "link_not_followed_nothing_made_in_target",
	                gate_tree_is(target, "") &&
	                        (GetFileAttributesW(piero) &
	                                FILE_ATTRIBUTE_REPARSE_POINT),
	                "the link is still a link and its target is empty");
done:
	return gate_first_run_end(g, e, ok, fails);
}

static bool gate_refusal_session(Gate *g,
        const char *copy_refusal,
        const char *recycle_refusal,
        const char *open_refusal);

/* Access to the state folder denied: Open is refused, and the folder is left. */
static bool gate_first_run_denied(Ev *e, const char *refusal)
{
	static const char tag[] = "first_run_state_folder_denied";
	Gate *g;
	bool ok = false, denied = false;
	wchar_t piero[1100], cmd[2300];
	DWORD rc = 0;
	int fails = e->fails;

	if (!gate_wanted(tag) || !(g = calloc(1, sizeof *g))) {
		return true;
	}
	if (!gate_open(g, e, tag) || !gate_mkdir(g, L"piero") ||
	        !gate_path(g, L"piero", piero)) {
		goto done;
	}
	swprintf(cmd, 2300, L"icacls.exe \"%ls\" /deny *S-1-1-0:(OI)(CI)(F)",
	        piero);
	if (!gate_run(cmd, 10000, &rc) || 0 != rc) {
		printf("SKIP %s: icacls could not deny access\n", tag);
		g_gate_skips++;
		ok = true;
		goto done;
	}
	denied = true;
	/* the same refusal, in the words of each language */
	g->lang = L"en";
	ok = gate_refusal_session(
	        g, GATE_COPY_NO_RECORD, GATE_RECYCLE_NO_RECORD, refusal);
	g->lang = L"ja";
	ok = ok &&
	        gate_refusal_session(g,
	                "コピー不可: コピー記録を書き込めません",
	                "ごみ箱不可: コピー記録を書き込めません",
	                "開けません: 非公開の状態フォルダー");
done:
	if (denied) {
		swprintf(cmd, 2300, L"icacls.exe \"%ls\" /remove:d *S-1-1-0",
		        piero);
		gate_run(cmd, 10000, &rc);
	}
	return gate_first_run_end(g, e, ok, fails);
}

/*
 * One start in which both the copy and Open are refused for the same state:
 * what the user is told must name the actual cause (never an internal gate),
 * the copy must have changed nothing, and the script must not have run. The
 * keys do not depend on the language: the path bar and the folder listing.
 */
static bool gate_refusal_session(Gate *g,
        const char *copy_refusal,
        const char *recycle_refusal,
        const char *open_refusal)
{
	Product *pr = &g->pr;
	char name[96];
	GateSnap now;
	wchar_t copy[1200];
	char *tree;
	int ok;

	if (!gate_launch(g)) {
		return false;
	}
	/* capture the source, go to the destination, paste */
	pty_send(&pr->p, "y", 1);
	Sleep(500);
	if (!gate_goto(g, g->dest, "refusal_goes_to_destination")) {
		return false;
	}
	pty_send(&pr->p, "p", 1);
	ok = product_wait(&pr->p, copy_refusal, pr->screen, sizeof pr->screen);
	snprintf(name, sizeof name, "%s_copy_refusal_words", g->tag);
	product_snapshot(pr, name);
	ev_check(pr->e, name, 0, copy_refusal, ok ? "shown" : "missing", ok);
	if (!ok) {
		return false;
	}
	/* an internal name is never the explanation */
	ok = NULL == strstr(pr->screen, "P0-STORAGE") &&
	        NULL == strstr(pr->screen, "engine disabled") &&
	        NULL == strstr(pr->screen, "G-OPEN");
	snprintf(
	        name, sizeof name, "%s_copy_refusal_no_internal_names", g->tag);
	ev_check(pr->e, name, 0, "no gate or engine name on screen",
	        ok ? "none" : "SHOWN", ok);
	if (!ok) {
		return false;
	}
	Sleep(400);
	swprintf(copy, 1200, L"%ls\\doc.txt", g->dest);
	tree = gate_tree_text(g->dest);
	ok = tree && !strcmp(tree, g->dest0) && !file_exists(copy) &&
	        gate_snap(g->doc, &now) && gate_same(&now, &g->doc0);
	free(tree);
	snprintf(name, sizeof name, "%s_copy_refused_changed_nothing", g->tag);
	ev_check(pr->e, name, 0,
	        "destination as before, source same SHA-256 and FileId",
	        ok ? "unchanged" : "CHANGED", ok);
	if (!ok) {
		return false;
	}
	/* Recycle runs on the same service: Delete on the source says why, asks
	 * nothing, and the file is still there, the same. */
	if (!gate_goto(g, g->browse, "refusal_back_to_the_source")) {
		return false;
	}
	pty_send(&pr->p, "\x1b[3~", 4);
	ok = product_wait(
	        &pr->p, recycle_refusal, pr->screen, sizeof pr->screen);
	snprintf(name, sizeof name, "%s_recycle_refusal_words", g->tag);
	product_snapshot(pr, name);
	ev_check(pr->e, name, 0, recycle_refusal, ok ? "shown" : "missing", ok);
	if (!ok) {
		return false;
	}
	ok = NULL == strstr(pr->screen, "gates") &&
	        NULL == strstr(pr->screen, "P0-") &&
	        NULL !=
	                strstr(pr->screen,
	                        g->lang && !wcscmp(g->lang, L"ja") ? "通常"
	                                                           : "NORMAL");
	snprintf(name, sizeof name, "%s_recycle_refusal_asks_nothing", g->tag);
	ev_check(pr->e, name, 0,
	        "normal mode, no gate name, no question on screen",
	        ok ? "as expected" : "OTHER", ok);
	if (!ok) {
		return false;
	}
	Sleep(400);
	ok = gate_snap(g->doc, &now) && gate_same(&now, &g->doc0);
	snprintf(name, sizeof name, "%s_recycle_refused_changed_nothing",
	        g->tag);
	ev_check(pr->e, name, 0, "source still there, same SHA-256 and FileId",
	        ok ? "unchanged" : "CHANGED", ok);
	if (!ok) {
		return false;
	}
	/* Open */
	if (!gate_probe(g, false, open_refusal)) {
		return false;
	}
	return gate_quit(g);
}

static int scn_first_run(Ev *e)
{
	bool good = true;
	/* what Open says when it cannot work */
	static const char refusal[] = GATE_OPEN_REFUSED;

	good = gate_first_run_fresh(e) && good;
	good = gate_first_run_legacy(e) && good;
	good = gate_first_run_both(e) && good;
	good = gate_first_run_foreign(e, refusal) && good;
	good = gate_first_run_cwd_used(e, refusal) && good;
	good = gate_first_run_link(e, refusal) && good;
	good = gate_first_run_denied(e, refusal) && good;
	if (!good || e->fails) {
		return 1;
	}
	return g_gate_skips ? 77 : 0;
}
