/* Real NTFS fixtures, confined to a fresh P0Area for every case. Adversarial
 * fixtures are intentionally retained: neither this harness nor wfstorage
 * sweeps adopted objects. p0_area_destroy is used only for intact fixtures. */
#include <windows.h>

#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tools/p0/p0_area.h"
#include "../tools/p0/p0_common.h"
#include "../tools/p0/p0_journal.h"
#include "fs/owned_delete.h"
#include "path/wf_canonical_win.h"
#include "wf_test.h"

#define CAP 4096
#define CRASH_CODE 0xC0DE0004u

typedef struct
{
	uint8_t txn[16];
	uint64_t source_size;
	wchar_t parent_path[CAP];
} StorageIntent;

typedef struct
{
	P0Area *area;
	wchar_t parent[CAP], txn[CAP], owner[CAP], payload[CAP], journal[CAP];
	StorageIntent intent;
	P0TempId temp;
	WfOwnershipRecord record;
	DWORD child_exit;
	int stop_mode, payload_present_before_verification;
} Fixture;

static wchar_t g_exe[CAP], g_base[CAP], g_evidence[CAP];
static FILE *g_csv[3];
static wchar_t g_run[128];
static char g_filesystem[64] = "unknown";
static unsigned g_pass[3], g_fail[3], g_blocked[3], g_sequence;
static int g_mutant;

static HANDLE open_read(const wchar_t *path)
{
	return CreateFileW(path, GENERIC_READ,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	        NULL);
}

static int write_new(const wchar_t *path, const void *bytes, DWORD count)
{
	DWORD written = 0;
	HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	int ok;
	if (INVALID_HANDLE_VALUE == h) {
		return 0;
	}
	ok = WriteFile(h, bytes, count, &written, NULL) && written == count &&
	        FlushFileBuffers(h);
	CloseHandle(h);
	return ok;
}

typedef struct
{
	P0Journal *journal;
	P0TempId *temp;
	unsigned callback, stop_at;
	int recorded;
} StopContext;

static DWORD CALLBACK stop_progress(LARGE_INTEGER total,
        LARGE_INTEGER transferred,
        LARGE_INTEGER stream_size,
        LARGE_INTEGER stream_done,
        DWORD stream,
        DWORD reason,
        HANDLE source,
        HANDLE destination,
        void *ctx)
{
	StopContext *stop = ctx;
	(void)total;
	(void)transferred;
	(void)stream_size;
	(void)stream_done;
	(void)stream;
	(void)reason;
	(void)source;
	if (!stop->recorded) {
		if (!p0j_id_of_handle(destination, stop->temp->payload_id) ||
		        !p0j_append(stop->journal, P0J_TEMP_ID, stop->temp,
		                sizeof *stop->temp) ||
		        !p0j_flush(stop->journal)) {
			return PROGRESS_STOP;
		}
		stop->recorded = 1;
	}
	return stop->callback++ >= stop->stop_at ? PROGRESS_STOP
	                                         : PROGRESS_CONTINUE;
}

static void uuid_name(const uint8_t u[16], wchar_t out[80])
{
	swprintf(out, 80,
	        L".wfile-txn-%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%"
	        L"02x%02x%02x%02x%02x",
	        u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9],
	        u[10], u[11], u[12], u[13], u[14], u[15]);
}

/* Only called with the fresh area's explicit paths, by the child process. */
static int child_create(
        const wchar_t *parent, const wchar_t *journal, int crash)
{
	StorageIntent in = {0};
	P0TempId t = {0};
	P0Journal j;
	WfCanonicalPath canonical = {0};
	wchar_t txn[CAP], owner[CAP], payload[CAP], name[80], source[CAP];
	DWORD error;
	HANDLE h = open_read(parent);
	if (INVALID_HANDLE_VALUE == h) {
		return 11;
	}
	if (!wf_canonical_from_handle(h, &canonical, &error)) {
		CloseHandle(h);
		return 12;
	}
	if (!p0j_id_of_handle(h, t.parent_id)) {
		CloseHandle(h);
		return 13;
	}
	CloseHandle(h);
	if (BCryptGenRandom(
	            NULL, in.txn, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG) ||
	        BCryptGenRandom(
	                NULL, t.nonce, 32, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) {
		return 14;
	}
	in.source_size = t.src_size = crash >= 2 ? 1048576 : 4096;
	wcscpy_s(in.parent_path, CAP, canonical.path);
	t.volume = canonical.volume;
	wf_canonical_free(&canonical);
	memcpy(t.txn, in.txn, 16);
	uuid_name(in.txn, name);
	swprintf(txn, CAP, L"%ls\\%ls", parent, name);
	swprintf(owner, CAP, L"%ls\\owner.bin", txn);
	swprintf(payload, CAP, L"%ls\\payload.bin", txn);
	swprintf(t.rel_payload, 128, L"%ls\\payload.bin", name);
	if (!p0j_open(&j, journal)) {
		return 15;
	}
	if (!p0j_append(&j, P0J_INTENT, &in, sizeof in) || !p0j_flush(&j)) {
		return 16;
	}
	if (!CreateDirectoryW(txn, NULL) || !write_new(owner, t.nonce, 32)) {
		return 17;
	}
	h = open_read(txn);
	if (INVALID_HANDLE_VALUE == h || !p0j_id_of_handle(h, t.txn_dir_id)) {
		return 18;
	}
	CloseHandle(h);
	h = open_read(owner);
	if (INVALID_HANDLE_VALUE == h || !p0j_id_of_handle(h, t.owner_id)) {
		return 19;
	}
	CloseHandle(h);
	if (crash >= 2) {
		StopContext stop = {&j, &t, 0, 2 == crash ? 0u : 1u, 0};
		char *data = calloc(1048576, 1);
		BOOL copied;
		DWORD copy_error;
		swprintf(source, CAP, L"%ls\\fixture-source.bin", parent);
		if (!data || !write_new(source, data, 1048576)) {
			free(data);
			return 23;
		}
		free(data);
		copied = CopyFileExW(source, payload, stop_progress, &stop,
		        NULL, COPY_FILE_FAIL_IF_EXISTS);
		copy_error = copied ? 0 : GetLastError();
		if (copied || ERROR_REQUEST_ABORTED != copy_error ||
		        !stop.recorded) {
			return 24;
		}
	} else {
		if (!write_new(payload, "partial owned bytes", 19)) {
			return 20;
		}
		h = open_read(payload);
		if (INVALID_HANDLE_VALUE == h ||
		        !p0j_id_of_handle(h, t.payload_id)) {
			return 20;
		}
		CloseHandle(h);
		if (!p0j_append(&j, P0J_TEMP_ID, &t, sizeof t) ||
		        !p0j_flush(&j)) {
			return 21;
		}
	}
	if (1 == crash) {
		TerminateProcess(GetCurrentProcess(), CRASH_CODE);
		return 22;
	}
	p0j_close(&j);
	return 0;
}

static void unpack_id(FILE_ID_INFO *out, const uint8_t bytes[24])
{
	memcpy(&out->VolumeSerialNumber, bytes, 8);
	memcpy(out->FileId.Identifier, bytes + 8, 16);
}

static int fixture_load(Fixture *f)
{
	uint8_t bytes[16384];
	P0Rec rec[4];
	DWORD got;
	size_t n;
	HANDLE h = open_read(f->journal);
	if (INVALID_HANDLE_VALUE == h) {
		return 0;
	}
	if (!ReadFile(h, bytes, sizeof bytes, &got, NULL)) {
		CloseHandle(h);
		return 0;
	}
	CloseHandle(h);
	n = p0j_parse(bytes, got, rec, 4);
	if (2 != n || P0J_INTENT != rec[0].type ||
	        rec[0].len != sizeof f->intent || P0J_TEMP_ID != rec[1].type ||
	        rec[1].len != sizeof f->temp) {
		return 0;
	}
	memcpy(&f->intent, rec[0].data, sizeof f->intent);
	memcpy(&f->temp, rec[1].data, sizeof f->temp);
	f->record.intent_durable = f->record.temp_id_durable = 1;
	memcpy(f->record.intent_txn, f->intent.txn, 16);
	memcpy(f->record.txn, f->temp.txn, 16);
	memcpy(f->record.nonce, f->temp.nonce, 32);
	f->record.intent_source_size = f->intent.source_size;
	f->record.source_size = f->temp.src_size;
	unpack_id(&f->record.parent, f->temp.parent_id);
	unpack_id(&f->record.directory, f->temp.txn_dir_id);
	unpack_id(&f->record.owner, f->temp.owner_id);
	unpack_id(&f->record.payload, f->temp.payload_id);
	if (f->temp.volume != f->record.parent.VolumeSerialNumber) {
		return 0;
	}
	f->record.parent_path = f->intent.parent_path;
	f->record.rel_payload = f->temp.rel_payload;
	return wf_owned_record_valid(&f->record);
}

static int track_absolute(Fixture *f, const wchar_t *path, int directory)
{
	const wchar_t *root = p0_area_root(f->area);
	size_t n = wcslen(root);
	/* Area paths may be in extended form; root is the ordinary drive path.
	 */
	if (!wcsncmp(path, L"\\\\?\\", 4)) {
		path += 4;
	}
	return !wcsncmp(path, root, n) && L'\\' == path[n] &&
	        p0_area_track(f->area, path + n + 1, directory);
}

static int fixture_create(
        Fixture *f, const char *label, int crash, int long_path, int unicode)
{
	char id[128], ts[20];
	wchar_t rel[CAP] = L"dest", path[CAP], command[CAP * 3], name[80];
	STARTUPINFOW si = {0};
	PROCESS_INFORMATION pi = {0};
	unsigned i;
	uint8_t random[8];
	memset(f, 0, sizeof *f);
	f->stop_mode = crash >= 2 ? crash - 2 : -1;
	p0_utc_compact(ts);
	if (BCryptGenRandom(NULL, random, sizeof random,
	            BCRYPT_USE_SYSTEM_PREFERRED_RNG)) {
		return 0;
	}
	snprintf(id, sizeof id, "%s-%lu-%u-%02x%02x%02x%02x-%s", ts,
	        GetCurrentProcessId(), ++g_sequence, random[0], random[1],
	        random[2], random[3], label);
	f->area = p0_area_create(g_base, id);
	if (!f->area) {
		return 0;
	}
	if (unicode) {
		wcscpy_s(rel, CAP, L"譌･譛ｬ隱・貍｢蟄・・");
	}
	if (!p0_area_mkdir(f->area, rel)) {
		return 0;
	}
	if (long_path) {
		for (i = 0; i < 4; i++) {
			wcscat_s(rel, CAP,
			        L"\\abcdefghijklmnopqrstuvwxyz-"
			        L"abcdefghijklmnopqrstuvwxyz-"
			        L"abcdefghijklmnopqrstuvwxyz");
			if (!p0_area_xpath(f->area, rel, path, CAP) ||
			        !CreateDirectoryW(path, NULL) ||
			        !p0_area_track(f->area, rel, 1)) {
				return 0;
			}
		}
	}
	if (!p0_area_xpath(f->area, rel, f->parent, CAP) ||
	        !p0_area_xpath(f->area, L"journal.wjr", f->journal, CAP)) {
		return 0;
	}
	swprintf(command, CAP * 3, L"\"%ls\" --child \"%ls\" \"%ls\" %d", g_exe,
	        f->parent, f->journal, crash);
	si.cb = sizeof si;
	if (!CreateProcessW(g_exe, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
	            NULL, NULL, &si, &pi)) {
		return 0;
	}
	if (WAIT_OBJECT_0 != WaitForSingleObject(pi.hProcess, 10000)) {
		TerminateProcess(pi.hProcess, 99);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		return 0;
	}
	GetExitCodeProcess(pi.hProcess, &f->child_exit);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	if (f->child_exit != (1 == crash ? CRASH_CODE : 0) ||
	        !fixture_load(f)) {
		return 0;
	}
	uuid_name(f->temp.txn, name);
	swprintf(f->txn, CAP, L"%ls\\%ls", f->parent, name);
	swprintf(f->owner, CAP, L"%ls\\owner.bin", f->txn);
	swprintf(f->payload, CAP, L"%ls\\payload.bin", f->txn);
	if (crash >= 2) {
		swprintf(path, CAP, L"%ls\\fixture-source.bin", f->parent);
		if (!track_absolute(f, path, 0)) {
			return 0;
		}
	}
	return track_absolute(f, f->journal, 0) &&
	        track_absolute(f, f->txn, 1) &&
	        track_absolute(f, f->owner, 0) &&
	        (INVALID_FILE_ATTRIBUTES == GetFileAttributesW(f->payload) ||
	                track_absolute(f, f->payload, 0));
}

static void fixture_finish(Fixture *f, int retained)
{
	wchar_t report[1024];
	if (!f->area) {
		return;
	}
	if (retained) {
		p0_area_forget(f->area);
	} else {
		CHECK(0 == p0_area_destroy(f->area, report, 1024));
	}
	f->area = NULL;
}

static void hex_bytes(FILE *out, const uint8_t *bytes, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		fprintf(out, "%02x", bytes[i]);
	}
}

/* What the payload looks like on disk right now, observed through one handle.
 * Gathering this is separate from writing it down. */
typedef struct
{
	DWORD open_error;
	bool opened, info_known;
	long long size;
	DWORD links, attributes, reparse_tag;
} PayloadObservation;

static PayloadObservation observe_payload(const Fixture *f)
{
	PayloadObservation o = {0};
	FILE_STANDARD_INFO standard;
	FILE_ATTRIBUTE_TAG_INFO tag;
	HANDLE h = open_read(f->payload);

	if (INVALID_HANDLE_VALUE == h) {
		o.open_error = GetLastError();
		return o;
	}
	o.opened = true;
	if (GetFileInformationByHandleEx(
	            h, FileStandardInfo, &standard, sizeof standard) &&
	        GetFileInformationByHandleEx(
	                h, FileAttributeTagInfo, &tag, sizeof tag)) {
		o.info_known = true;
		o.size = (long long)standard.EndOfFile.QuadPart;
		o.links = standard.NumberOfLinks;
		o.attributes = tag.FileAttributes;
		o.reparse_tag = tag.ReparseTag;
	}
	CloseHandle(h);
	return o;
}

/* One line on stdout, and the case counters. */
static void report_case(int group,
        const char *name,
        WfOwnedResult result,
        const WfOwnedReport *r,
        int pass,
        ULONGLONG elapsed)
{
	if (pass) {
		g_pass[group]++;
	} else {
		g_fail[group]++;
	}
	printf("%s %s result=%d error=%lu attempts=%u open_attempts=%u "
	       "elapsed=%llu reason=%s\n",
	        pass ? "PASS" : "FAIL", name, (int)result, r->error,
	        r->attempts, r->open_attempts, (unsigned long long)elapsed,
	        r->reason ? r->reason : "");
	CHECK(pass);
}

/* One row of the group's cases.csv. */
static void write_case_row(FILE *csv,
        const char *name,
        const Fixture *f,
        WfOwnedResult result,
        const WfOwnedReport *r,
        int pass,
        int retained,
        ULONGLONG elapsed)
{
	fprintf(csv, "%s,%s,%d,%lu,%lu,%u,%u,%d,%llu,%lu,%llu,", name,
	        pass ? "PASS" : "FAIL", (int)result, r->error, r->ex_error,
	        r->attempts, r->open_attempts, r->used_fallback,
	        (unsigned long long)elapsed, f->child_exit,
	        (unsigned long long)f->record.payload.VolumeSerialNumber);
	hex_bytes(csv, f->record.payload.FileId.Identifier, 16);
	fprintf(csv, ",%d,", retained);
	p0_json_wstr(csv, p0_area_root(f->area));
	fputc(',', csv);
	p0_json_wstr(csv, f->record.parent_path);
	fputc(',', csv);
	p0_json_str(csv, r->reason ? r->reason : "");
	fputc('\n', csv);
	fflush(csv);
}

/* artifacts/<case>.json: the recorded identities, the nonce, and what was
 * observed on disk. */
static void write_case_json(int group,
        const wchar_t *name_w,
        const Fixture *f,
        const WfOwnedReport *r)
{
	static const char *const keys[] = {
	        "parent", "txn_directory", "owner", "payload"};
	const FILE_ID_INFO *ids[] = {&f->record.parent, &f->record.directory,
	        &f->record.owner, &f->record.payload};
	PayloadObservation seen = observe_payload(f);
	wchar_t artifact[CAP];
	FILE *json;
	unsigned k;

	swprintf(artifact, CAP, L"%ls\\P0-S%02d\\%ls\\artifacts\\%ls.json",
	        g_evidence, group + 4, g_run, name_w);
	json = p0_fopen_w(artifact);
	CHECK(NULL != json);
	if (!json) {
		return;
	}
	fprintf(json,
	        "{\"child_exit\":%lu,\"crash_after_durable_TEMP_ID\":%s,"
	        "\"source_size\":%llu,\"objects\":{",
	        f->child_exit, CRASH_CODE == f->child_exit ? "true" : "false",
	        (unsigned long long)f->record.source_size);
	for (k = 0; k < 4; k++) {
		fprintf(json, "%s\"%s\":{\"volume\":%llu,\"file_id\":\"",
		        k ? "," : "", keys[k],
		        (unsigned long long)ids[k]->VolumeSerialNumber);
		hex_bytes(json, ids[k]->FileId.Identifier, 16);
		fputs("\"}", json);
	}
	fputs("},\"uuid\":\"", json);
	hex_bytes(json, f->record.txn, 16);
	fputs("\",\"nonce\":\"", json);
	hex_bytes(json, f->record.nonce, 32);
	fputs("\",\"relative_payload\":", json);
	p0_json_wstr(json, f->record.rel_payload);
	fputs(",\"canonical_parent\":", json);
	p0_json_wstr(json, f->record.parent_path);
	fprintf(json,
	        ",\"stop_callback_index\":%d,\"payload_present_before_wfile\":"
	        "%s,\"wfile_disposition_attempts\":%u",
	        f->stop_mode,
	        f->payload_present_before_verification ? "true" : "false",
	        r->attempts);
	fprintf(json, ",\"observed_payload_open_error\":%lu", seen.open_error);
	if (seen.info_known) {
		fprintf(json,
		        ",\"observed_size\":%lld,\"observed_links\":%lu,"
		        "\"observed_attributes\":%lu,\"observed_reparse_tag\":%"
		        "lu",
		        seen.size, seen.links, seen.attributes,
		        seen.reparse_tag);
	}
	fputs("}\n", json);
	fclose(json);
}

/* The exact durable journal bytes, kept before normal fixture cleanup. */
static void preserve_journal(int group, const wchar_t *name_w, const Fixture *f)
{
	uint8_t bytes[16384];
	wchar_t artifact[CAP];
	DWORD got;
	HANDLE h = open_read(f->journal);

	CHECK(INVALID_HANDLE_VALUE != h);
	if (INVALID_HANDLE_VALUE == h) {
		return;
	}
	CHECK(ReadFile(h, bytes, sizeof bytes, &got, NULL));
	CloseHandle(h);
	swprintf(artifact, CAP, L"%ls\\P0-S%02d\\%ls\\artifacts\\%ls.wjr",
	        g_evidence, group + 4, g_run, name_w);
	CHECK(write_new(artifact, bytes, got));
}

static void record_case(int group,
        const char *name,
        Fixture *f,
        WfOwnedResult result,
        WfOwnedReport *r,
        int pass,
        int retained,
        ULONGLONG elapsed)
{
	FILE *csv = g_csv[group];
	wchar_t name_w[128];

	report_case(group, name, result, r, pass, elapsed);
	if (!csv) {
		return;
	}
	write_case_row(csv, name, f, result, r, pass, retained, elapsed);
	MultiByteToWideChar(CP_UTF8, 0, name, -1, name_w, 128);
	write_case_json(group, name_w, f, r);
	preserve_journal(group, name_w, f);
}

static int exists(const wchar_t *p)
{
	return INVALID_FILE_ATTRIBUTES != GetFileAttributesW(p);
}

/* Same mklink /J fixture used by p0_commit_rename; unlike symlinks, local
 * directory junction creation normally does not need SeCreateSymbolicLink. */
static int make_junction(const wchar_t *link, const wchar_t *target)
{
	wchar_t command[CAP * 3];
	STARTUPINFOW si = {0};
	PROCESS_INFORMATION pi = {0};
	DWORD code = 1;
	if (wcsncmp(link, L"\\\\?\\", 4) || wcsncmp(target, L"\\\\?\\", 4)) {
		return 0;
	}
	swprintf(command, CAP * 3,
	        L"cmd.exe /c mklink /J \"%ls\" \"%ls\" >nul 2>nul", link + 4,
	        target + 4);
	si.cb = sizeof si;
	if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
	            NULL, NULL, &si, &pi)) {
		return 0;
	}
	if (WAIT_OBJECT_0 != WaitForSingleObject(pi.hProcess, 10000)) {
		TerminateProcess(pi.hProcess, 99);
	}
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return 0 == code;
}

/* Removing any of these AND terms must turn a MANUAL into an unsafe token,
 * caught here before a mutant can actually delete an adversarial fixture. */
static void adversarial(unsigned mutation)
{
	static const char *names[] = {"rename_payload", "move_same_volume",
	        "hard_link", "size_growth", "replace_owner_metadata",
	        "change_nonce", "rename_txn", "unrelated_child",
	        "case_only_rename", "payload_id_mismatch", "volume_mismatch",
	        "uuid_mismatch", "no_temp_id", "parent_id_mismatch",
	        "txn_id_mismatch", "owner_id_mismatch", "reparse_payload",
	        "reparse_ancestor"};
	Fixture f;
	WfVerifiedOwned *o = NULL;
	WfOwnedReport r = {0};
	WfOwnedResult result;
	wchar_t changed[CAP], moved[CAP];
	HANDLE h;
	DWORD wrote;
	LARGE_INTEGER size;
	int ok = fixture_create(&f, names[mutation], 1, 0, 0);
	CHECK(ok);
	if (!ok) {
		fixture_finish(&f, 1);
		return;
	}
	wcscpy_s(changed, CAP, f.payload);
	switch (mutation) {
	case 0:
	case 8:
		swprintf(changed, CAP, L"%ls\\%ls", f.txn,
		        mutation ? L"Payload.bin" : L"adopted.bin");
		CHECK(MoveFileExW(f.payload, changed, 0));
		break;
	case 1:
		swprintf(changed, CAP, L"%ls\\adopted.bin", f.parent);
		CHECK(MoveFileExW(f.payload, changed, 0));
		break;
	case 2:
		swprintf(moved, CAP, L"%ls\\external-link.bin", f.parent);
		CHECK(CreateHardLinkW(moved, f.payload, NULL));
		break;
	case 3:
		h = CreateFileW(f.payload, GENERIC_WRITE, 0, NULL,
		        OPEN_EXISTING, 0, NULL);
		CHECK(INVALID_HANDLE_VALUE != h);
		size.QuadPart = 4097;
		CHECK(SetFilePointerEx(h, size, NULL, FILE_BEGIN) &&
		        SetEndOfFile(h));
		CloseHandle(h);
		break;
	case 4:
		swprintf(moved, CAP, L"%ls\\old-owner.bin", f.parent);
		CHECK(MoveFileExW(f.owner, moved, 0));
		CHECK(write_new(f.owner, f.temp.nonce, 32));
		break;
	case 5:
		h = CreateFileW(f.owner, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
		        0, NULL);
		CHECK(INVALID_HANDLE_VALUE != h);
		f.temp.nonce[0] ^= 1;
		CHECK(WriteFile(h, f.temp.nonce, 32, &wrote, NULL) &&
		        32 == wrote);
		CloseHandle(h);
		break;
	case 6:
		swprintf(moved, CAP, L"%ls\\adopted-txn", f.parent);
		CHECK(MoveFileExW(f.txn, moved, 0));
		swprintf(changed, CAP, L"%ls\\payload.bin", moved);
		break;
	case 7:
		swprintf(moved, CAP, L"%ls\\user.txt", f.txn);
		CHECK(write_new(moved, "user", 4));
		break;
	case 9:  f.record.payload.FileId.Identifier[0] ^= 0x7f; break;
	case 10: f.record.payload.VolumeSerialNumber ^= 1; break;
	case 11: f.record.intent_txn[0] ^= 1; break;
	case 12: f.record.temp_id_durable = 0; break;
	case 13: f.record.parent.FileId.Identifier[0] ^= 0x7f; break;
	case 14: f.record.directory.FileId.Identifier[0] ^= 0x7f; break;
	case 15: f.record.owner.FileId.Identifier[0] ^= 0x7f; break;
	case 16:
		swprintf(moved, CAP, L"%ls\\original.bin", f.parent);
		CHECK(MoveFileExW(f.payload, moved, 0));
		ok = CreateSymbolicLinkW(f.payload, moved,
		             SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0;
		if (!ok) {
			r.error = GetLastError();
			r.reason = "symlink creation unavailable";
			CHECK(ERROR_PRIVILEGE_NOT_HELD == r.error ||
			        ERROR_NOT_SUPPORTED == r.error);
			g_blocked[0]++;
			printf("BLOCKED %s error=%lu\n", names[mutation],
			        r.error);
			if (g_csv[0]) {
				fprintf(g_csv[0], "%s,BLOCKED,1,%lu\n",
				        names[mutation], r.error);
			}
			fixture_finish(&f, 1);
			return;
		}
		wcscpy_s(changed, CAP, moved);
		break;
	case 17:
		/* Move the entire parent, then put a junction-equivalent
		 * directory symlink at its recorded spelling. ID alone still
		 * points at real data. */
		swprintf(moved, CAP, L"%ls-adopted", f.parent);
		CHECK(MoveFileExW(f.parent, moved, 0));
		ok = CreateSymbolicLinkW(f.parent, moved,
		             SYMBOLIC_LINK_FLAG_DIRECTORY |
		                     SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) !=
		        0;
		if (!ok && ERROR_PRIVILEGE_NOT_HELD == GetLastError()) {
			ok = make_junction(f.parent, moved);
		}
		if (!ok) {
			r.error = GetLastError();
			r.reason = "directory symlink creation unavailable";
			CHECK(ERROR_PRIVILEGE_NOT_HELD == r.error ||
			        ERROR_NOT_SUPPORTED == r.error);
			g_blocked[0]++;
			printf("BLOCKED %s error=%lu\n", names[mutation],
			        r.error);
			if (g_csv[0]) {
				fprintf(g_csv[0], "%s,BLOCKED,1,%lu\n",
				        names[mutation], r.error);
			}
			fixture_finish(&f, 1);
			return;
		}
		swprintf(
		        changed, CAP, L"%ls\\%ls", moved, f.record.rel_payload);
		break;
	}
	f.payload_present_before_verification = exists(changed);
	result = wf_owned_verify(WF_OWNED_COPY_PAYLOAD, &f.record, &o, &r);
	ok = WF_OWNED_MANUAL == result && !o && exists(changed);
	/* Refused authority stays refused even at the deletion boundary. */
	if (!o) {
		WfOwnedReport denied;
		CHECK(WF_OWNED_MANUAL == wf_owned_delete(o, &denied));
	}
	record_case(0, names[mutation], &f, result, &r, ok, 1, 0);
	wf_owned_release(o);
	fixture_finish(&f, 1);
}

typedef struct
{
	HANDLE handle;
	DWORD delay;
} DelayedClose;
static DWORD WINAPI delayed_close(void *ctx)
{
	DelayedClose *d = ctx;
	Sleep(d->delay);
	CloseHandle(d->handle);
	return 0;
}

/* A concurrent reader of the payload, as an antivirus scanner holds one:
 * blocking (mode 2), released after a delay (mode 3), or sharing delete
 * (mode 4). */
static void hold_payload_open(unsigned mode,
        const Fixture *f,
        HANDLE *av,
        HANDLE *thread,
        DelayedClose *delayed)
{
	*av = CreateFileW(f->payload, GENERIC_READ,
	        FILE_SHARE_READ | FILE_SHARE_WRITE |
	                (4 == mode ? FILE_SHARE_DELETE : 0),
	        NULL, OPEN_EXISTING, 0, NULL);
	CHECK(INVALID_HANDLE_VALUE != *av);
	if (3 != mode) {
		return;
	}
	delayed->handle = *av;
	delayed->delay = 120;
	*thread = CreateThread(NULL, 0, delayed_close, delayed, 0, NULL);
	CHECK(NULL != *thread);
	if (*thread) {
		*av = INVALID_HANDLE_VALUE; /* the thread owns it now */
	}
}

/* The payload name is already gone from the area's record. */
static void forget_payload(const Fixture *f)
{
	CHECK(p0_area_remove(f->area,
	        wcsrchr(f->payload + 4, L'\\')
	                ? f->payload + 4 + wcslen(p0_area_root(f->area)) + 1
	                : L"invalid"));
}

/* A partial payload, as a stopped copy leaves it. */
static void truncate_payload(const Fixture *f)
{
	HANDLE h = CreateFileW(
	        f->payload, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);

	CHECK(INVALID_HANDLE_VALUE != h);
	CHECK(SetEndOfFile(h));
	CloseHandle(h);
}

/* A drive-letter case change in the extended DOS spelling must not change the
 * canonical identity: the volume-GUID final spelling does not change. */
static void drive_letter_case_is_canonical(const Fixture *f)
{
	wchar_t alternate[CAP];
	WfCanonicalPath a = {0}, b = {0};
	DWORD error;
	HANDLE ha = open_read(f->payload), hb;

	wcscpy_s(alternate, CAP, f->payload);
	alternate[4] = alternate[4] >= L'A' && alternate[4] <= L'Z'
	        ? alternate[4] + (L'a' - L'A')
	        : alternate[4] - (L'a' - L'A');
	hb = open_read(alternate);
	CHECK(INVALID_HANDLE_VALUE != ha && INVALID_HANDLE_VALUE != hb);
	CHECK(0 != wcscmp(alternate, f->payload));
	CHECK(wf_canonical_from_handle(ha, &a, &error) &&
	        wf_canonical_from_handle(hb, &b, &error));
	CHECK(wf_canonical_equal(&a, &b));
	wf_canonical_free(&a);
	wf_canonical_free(&b);
	CloseHandle(ha);
	CloseHandle(hb);
}

/* The state a fixture needs before ownership is verified, per mode. */
static void prepare_deletion_state(unsigned mode,
        const Fixture *f,
        HANDLE *av,
        HANDLE *thread,
        DelayedClose *delayed)
{
	if (1 == mode) {
		CHECK(SetFileAttributesW(f->payload, FILE_ATTRIBUTE_READONLY));
	}
	if (mode >= 2 && mode <= 4) {
		hold_payload_open(mode, f, av, thread, delayed);
	}
	if (8 == mode) {
		forget_payload(f);
	}
	if (10 == mode) {
		truncate_payload(f);
	}
	if (9 == mode) {
		drive_letter_case_is_canonical(f);
	}
}

/* While the token is held, the pinned namespace cannot be renamed, adopted or
 * written to. */
static void token_pins_namespace(const Fixture *f)
{
	wchar_t extra[CAP];
	HANDLE writer;

	swprintf(extra, CAP, L"%ls\\adopted.bin", f->txn);
	CHECK(!MoveFileExW(f->payload, extra, 0));
	CHECK(ERROR_SHARING_VIOLATION == GetLastError() ||
	        ERROR_ACCESS_DENIED == GetLastError());
	swprintf(extra, CAP, L"%ls\\adopted-txn", f->parent);
	CHECK(!MoveFileExW(f->txn, extra, 0));
	swprintf(extra, CAP, L"%ls-adopted", f->parent);
	CHECK(!MoveFileExW(f->parent, extra, 0));
	writer = CreateFileW(f->owner, GENERIC_WRITE, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, 0, NULL);
	CHECK(INVALID_HANDLE_VALUE == writer);
	if (INVALID_HANDLE_VALUE != writer) {
		CloseHandle(writer);
	}
}

static void deletion_case(unsigned mode)
{
	static const char *names[] = {"normal", "readonly", "av_blocked",
	        "av_delayed", "av_shared", "unicode", "long_path",
	        "token_recheck_unrelated", "absent_fixture_no_delete",
	        "same_drive_letter_case", "empty_partial",
	        "token_pins_namespace", "progress_stop_first_callback",
	        "progress_stop_after_data"};
	Fixture f;
	WfVerifiedOwned *o = NULL;
	WfOwnedReport r = {0};
	WfOwnedResult result;
	HANDLE av = INVALID_HANDLE_VALUE, thread = NULL;
	DelayedClose delayed;
	wchar_t extra[CAP];
	ULONGLONG start;
	int ok = fixture_create(&f, names[mode],
	            mode >= 12 ? (int)mode - 10 : 0, 6 == mode, 5 == mode),
	    retained = 7 == mode;
	int present_before;
	CHECK(ok);
	if (!ok) {
		fixture_finish(&f, 1);
		return;
	}
	prepare_deletion_state(mode, &f, &av, &thread, &delayed);
	start = GetTickCount64();
	present_before = exists(f.payload);
	f.payload_present_before_verification = present_before;
	if (mode >= 12) {
		printf("CopyFileEx PROGRESS_STOP: mode=%u "
		       "payload_present_before_wfile=%d\n",
		        mode, present_before);
	}
	result = wf_owned_verify(WF_OWNED_COPY_PAYLOAD, &f.record, &o, &r);
	if (11 == mode && o) {
		token_pins_namespace(&f);
	}
	if (7 == mode && o) {
		swprintf(extra, CAP, L"%ls\\user-added.txt", f.txn);
		CHECK(write_new(extra, "keep", 4));
	}
	if (2 == mode) {
		ok = WF_OWNED_RECOVERY_REQUIRED == result && !o &&
		        6 == r.open_attempts && exists(f.payload);
	} else if (8 == mode || (mode >= 12 && !present_before)) {
		ok = (WF_OWNED_ABSENT == result || WF_OWNED_MANUAL == result) &&
		        !o && !exists(f.payload) && 0 == r.attempts;
	} else {
		CHECK(WF_OWNED_VERIFIED == result && o);
		if (o) {
			result = wf_owned_delete(o, &r);
		}
		if (7 == mode) {
			ok = WF_OWNED_MANUAL == result && exists(f.payload) &&
			        exists(extra);
		} else {
			ok = WF_OWNED_DELETED == result && !exists(f.payload);
		}
		if (o && WF_OWNED_DELETED == result) {
			WfOwnedReport again;
			CHECK(WF_OWNED_MANUAL == wf_owned_delete(o, &again));
		}
	}
	if (4 == mode && WF_OWNED_DELETED == result) {
		char data[20] = {0};
		DWORD got;
		CHECK(ReadFile(av, data, 19, &got, NULL) && 19 == got &&
		        !memcmp(data, "partial owned bytes", 19));
	}
	if (thread) {
		CHECK(WAIT_OBJECT_0 == WaitForSingleObject(thread, 5000));
		CloseHandle(thread);
	}
	if (INVALID_HANDLE_VALUE != av) {
		CloseHandle(av);
	}
	wf_owned_release(o);
	record_case(5 == mode || 6 == mode || 9 == mode ? 1 : 2, names[mode],
	        &f, result, &r, ok && GetTickCount64() - start < 5000, retained,
	        GetTickCount64() - start);
	fixture_finish(&f, retained || !ok);
}

static int open_evidence(const wchar_t *root, const wchar_t *run)
{
	unsigned i;
	for (i = 0; i < 3; i++) {
		static const wchar_t *files[] = {
		        L"cases.csv", L"environment.json", L"run.json"};
		unsigned j;
		for (j = 0; j < 3; j++) {
			wchar_t path[CAP];
			swprintf(path, CAP, L"%ls\\P0-S%02u\\%ls\\%ls", root,
			        4 + i, run, files[j]);
			if (INVALID_FILE_ATTRIBUTES !=
			        GetFileAttributesW(path)) {
				fprintf(stderr,
				        "refusing to overwrite existing "
				        "storage evidence\n");
				return 0;
			}
		}
	}
	for (i = 0; i < 3; i++) {
		wchar_t dir[CAP], path[CAP];
		swprintf(dir, CAP, L"%ls\\P0-S%02u\\%ls", root, 4 + i, run);
		/* The wrapper creates a unique run. Never overwrite an existing
		 * CSV. */
		CHECK(p0_mkdirs(dir));
		swprintf(path, CAP, L"%ls\\artifacts", dir);
		CHECK(p0_mkdirs(path));
		swprintf(path, CAP, L"%ls\\cases.csv", dir);
		CHECK(INVALID_FILE_ATTRIBUTES == GetFileAttributesW(path));
		g_csv[i] = p0_fopen_w(path);
		CHECK(NULL != g_csv[i]);
		if (g_csv[i]) {
			fputs("case,result,class,win32_error,ex_error,delete_"
			      "attempts,open_attempts,fallback,elapsed_ms,"
			      "child_exit,volume,payload_id,retained,scratch,"
			      "canonical_parent,reason\n",
			        g_csv[i]);
		}
		swprintf(path, CAP, L"%ls\\environment.json", dir);
		CHECK(p0_write_environment(
		        path, g_base, "test_storage_win", "1.0"));
	}
	return !wf_test_failures;
}

int wmain(int argc, wchar_t **argv)
{
	unsigned i;
	DWORD before, after;
	if (5 == argc && !wcscmp(argv[1], L"--child")) {
		return child_create(argv[2], argv[3], _wtoi(argv[4]));
	}
	CHECK(0 != GetModuleFileNameW(NULL, g_exe, CAP));
	wcscpy_s(g_base, CAP, g_exe);
	*wcsrchr(g_base, L'\\') = 0;
	wcscat_s(g_base, CAP, L"\\storage-scratch");
	CHECK(p0_mkdirs(g_base));
	{
		wchar_t fs[32];
		HANDLE h = open_read(g_base);
		if (INVALID_HANDLE_VALUE != h) {
			if (GetVolumeInformationByHandleW(
			            h, NULL, 0, NULL, NULL, NULL, fs, 32)) {
				WideCharToMultiByte(CP_UTF8, 0, fs, -1,
				        g_filesystem, sizeof g_filesystem, NULL,
				        NULL);
			}
			CloseHandle(h);
		}
	}
	if (4 == argc && !wcscmp(argv[1], L"--evidence")) {
		wcscpy_s(g_evidence, CAP, argv[2]);
		wcscpy_s(g_run, 128, argv[3]);
		if (!open_evidence(argv[2], argv[3])) {
			return 2;
		}
	}
	g_mutant = 2 == argc && !wcscmp(argv[1], L"--adversarial-only");
	/* Warm up RNG/CRT before measuring handle ownership. */
	{
		Fixture warm;
		int ready = fixture_create(&warm, "warmup", 0, 0, 0);
		CHECK(ready);
		fixture_finish(&warm, !ready);
	}
	CHECK(GetProcessHandleCount(GetCurrentProcess(), &before));
	for (i = 0; i < 18; i++) {
		adversarial(i);
	}
	if (!g_mutant) {
		for (i = 0; i < 14; i++) {
			deletion_case(i);
		}
	}
	CHECK(GetProcessHandleCount(GetCurrentProcess(), &after));
	printf("handles before=%lu after=%lu\n", before, after);
	CHECK(after <= before + 1); /* CRT may lazily open its output handle. */
	for (i = 0; i < 3; i++) {
		if (g_csv[i]) {
			wchar_t path[CAP];
			char test[16];
			char run_id[128];
			const char *const hyp04[] = {"H-011", "H-013", NULL};
			const char *const hyp05[] = {"H-012", NULL};
			const char *const hyp06[] = {
			        "H-008", "H-009", "H-010", NULL};
			const char *const *hyp = 0 == i ? hyp04
			        : 1 == i                ? hyp05
			                                : hyp06;
			const char *decisions[] = {NULL};
			fclose(g_csv[i]);
			swprintf(path, CAP, L"%ls\\P0-S%02u\\%ls\\run.json",
			        argv[2], 4 + i, argv[3]);
			snprintf(test, sizeof test, "P0-S%02u", 4 + i);
			WideCharToMultiByte(CP_UTF8, 0, g_run, -1, run_id,
			        sizeof run_id, NULL, NULL);
			CHECK(p0_write_run_json(path, test, run_id,
			        "native-storage", g_filesystem, hyp,
			        g_fail[i] || wf_test_failures ? "FAIL"
			                : g_blocked[i]        ? "BLOCKED"
			                                      : "PASS",
			        decisions,
			        "Measured environment only. Process crash, not "
			        "power loss. Production copy STOP remains. See "
			        "matrix.json for NOT_TESTED environments."));
		}
	}
	return TEST_RESULT();
}
