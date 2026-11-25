#include "wf_copy_source_win.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "../path/wf_canonical_win.h"
#include "../fs/wf_weakid_win.h"

struct WfCopySourceObserver
{
	HANDLE guard;    /* borrowed FILE_SHARE_READ handle */
	HANDLE observed; /* owned duplicate of the real callback source */
	WfNodeInfo snapshot;
	FILE_BASIC_INFO basic;
	WfCanonicalPath canonical;
	WfCopySourceReport report;
	bool terminal;
};

static bool reject(
        WfCopySourceObserver *o, DWORD error, WfCopySourceReport *report)
{
	o->report.state = WF_COPY_SOURCE_REJECTED;
	o->report.error = error ? error : ERROR_FILE_INVALID;
	*report = o->report;
	return 0;
}

static bool matches(
        WfCopySourceObserver *o, HANDLE handle, bool initial, DWORD *error)
{
	FILE_ID_INFO id;
	FILE_STANDARD_INFO standard;
	FILE_BASIC_INFO basic;
	WfCanonicalPath canonical = {0};
	bool ok = false;
	if (!wf_handle_file_id_info(handle, &id)) {
		goto os_error;
	}
	o->report.observed = id;
	if (!GetFileInformationByHandleEx(
	            handle, FileStandardInfo, &standard, sizeof standard)) {
		goto os_error;
	}
	if (!GetFileInformationByHandleEx(
	            handle, FileBasicInfo, &basic, sizeof basic)) {
		goto os_error;
	}
	*error = ERROR_FILE_INVALID;
	if (!wf_info_matches(&o->snapshot.id, &id) || standard.Directory ||
	        standard.DeletePending || 1 != standard.NumberOfLinks ||
	        standard.EndOfFile.QuadPart < 0 ||
	        (uint64_t)standard.EndOfFile.QuadPart != o->snapshot.size ||
	        (uint64_t)basic.LastWriteTime.QuadPart != o->snapshot.mtime ||
	        (basic.FileAttributes &
	                (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE |
	                        FILE_ATTRIBUTE_RECALL_ON_OPEN |
	                        FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS))) {
		return 0;
	}
	if (!wf_canonical_from_handle(handle, &canonical, error)) {
		return 0;
	}
	if (initial) {
		o->canonical = canonical;
		o->basic = basic;
		return 1;
	}
	*error = ERROR_FILE_INVALID;
	{
		bool same_path;

		/* "not the same path" and "no path to compare" are not one answer */
		if (WF_OK !=
		        wf_canonical_compare(
		                &o->canonical, &canonical, &same_path)) {
			*error = ERROR_INVALID_NAME;
			same_path = false;
		}
		ok = same_path &&
		        basic.CreationTime.QuadPart ==
		                o->basic.CreationTime.QuadPart &&
		        basic.ChangeTime.QuadPart ==
		                o->basic.ChangeTime.QuadPart &&
		        basic.FileAttributes == o->basic.FileAttributes;
	}
	wf_canonical_free(&canonical);
	return ok;
os_error:
	*error = GetLastError();
	return 0;
}

void wf_copy_source_release(WfCopySourceObserver *observer)
{
	if (!observer) {
		return;
	}
	if (INVALID_HANDLE_VALUE != observer->observed) {
		CloseHandle(observer->observed);
	}
	wf_canonical_free(&observer->canonical);
	free(observer);
}

bool wf_copy_source_begin(HANDLE guard,
        const WfNodeInfo *snapshot,
        WfCopySourceObserver **out,
        WfCopySourceReport *report)
{
	static const uint8_t zero[16] = {0};
	WfCopySourceObserver *o;
	wchar_t fs[32], root[50];
	DWORD error = ERROR_INVALID_PARAMETER, flags, drive;
	if (!report) {
		return 0;
	}
	memset(report, 0, sizeof *report);
	report->state = WF_COPY_SOURCE_REJECTED;
	report->error = error;
	if (!out || *out || !snapshot ||
	        !wf_fileid_usable_to_read(&snapshot->id) ||
	        !memcmp(snapshot->id.id, zero, sizeof zero) || !guard ||
	        INVALID_HANDLE_VALUE == guard) {
		return 0;
	}
	if (snapshot->flags & (WF_ENT_DIR | WF_ENT_LINK | WF_ENT_CLOUD)) {
		report->error = ERROR_NOT_SUPPORTED;
		return 0;
	}
	o = calloc(1, sizeof *o);
	if (!o) {
		report->error = ERROR_NOT_ENOUGH_MEMORY;
		return 0;
	}
	o->guard = guard;
	o->observed = INVALID_HANDLE_VALUE;
	o->snapshot = *snapshot;
	o->report.expected.VolumeSerialNumber = snapshot->id.volume;
	memcpy(o->report.expected.FileId.Identifier, snapshot->id.id, 16);
	if (!matches(o, guard, 1, &error)) {
		goto fail;
	}
	if (!GetVolumeInformationByHandleW(
	            guard, NULL, 0, NULL, NULL, &flags, fs, 32)) {
		error = GetLastError();
		goto fail;
	}
	error = ERROR_NOT_SUPPORTED;
	if ((wcscmp(fs, L"NTFS") && wcscmp(fs, L"ReFS")) ||
	        !(flags & FILE_SUPPORTS_OPEN_BY_FILE_ID)) {
		goto fail;
	}
	memcpy(root, o->canonical.path, 49 * sizeof *root);
	root[49] = 0;
	drive = GetDriveTypeW(root);
	if (DRIVE_FIXED != drive && DRIVE_REMOVABLE != drive) {
		goto fail;
	}
	o->report.state = WF_COPY_SOURCE_GUARDED;
	*report = o->report;
	*out = o;
	return 1;
fail:
	reject(o, error, report);
	wf_copy_source_release(o);
	return 0;
}

bool wf_copy_source_observe(WfCopySourceObserver *o,
        HANDLE actual_source,
        DWORD stream,
        WfCopySourceReport *report)
{
	DWORD error;
	if (!o) {
		return 0;
	}
	if (!report) {
		o->report.state = WF_COPY_SOURCE_REJECTED;
		o->report.error = ERROR_INVALID_PARAMETER;
		return 0;
	}
	if (WF_COPY_SOURCE_REJECTED == o->report.state || o->terminal) {
		*report = o->report;
		return 0;
	}
	if (UINT_MAX == o->report.callback_checks) {
		return reject(o, ERROR_ARITHMETIC_OVERFLOW, report);
	}
	o->report.callback_checks++;
	if (1 != stream) {
		return reject(o, ERROR_NOT_SUPPORTED, report);
	}
	if (!actual_source || INVALID_HANDLE_VALUE == actual_source) {
		return reject(o, ERROR_INVALID_HANDLE, report);
	}
	if (!matches(o, o->guard, 0, &error) ||
	        !matches(o, actual_source, 0, &error)) {
		return reject(o, error, report);
	}
	if (INVALID_HANDLE_VALUE == o->observed &&
	        !DuplicateHandle(GetCurrentProcess(), actual_source,
	                GetCurrentProcess(), &o->observed, 0, FALSE,
	                DUPLICATE_SAME_ACCESS)) {
		error = GetLastError();
		o->observed = INVALID_HANDLE_VALUE;
		return reject(o, error, report);
	}
	*report = o->report;
	return 1;
}

bool wf_copy_source_finish(WfCopySourceObserver *o,
        bool copied,
        DWORD copy_error,
        WfCopySourceReport *report)
{
	DWORD error;
	if (!o || !report) {
		return 0;
	}
	if (WF_COPY_SOURCE_REJECTED == o->report.state) {
		*report = o->report;
		return 0;
	}
	if (o->terminal) {
		return reject(o, ERROR_INVALID_STATE, report);
	}
	o->terminal = true;
	if (!copied) {
		return reject(o,
		        copy_error ? copy_error : ERROR_REQUEST_ABORTED,
		        report);
	}
	if (!o->report.callback_checks || INVALID_HANDLE_VALUE == o->observed) {
		o->report.state = WF_COPY_SOURCE_UNOBSERVED;
		o->report.error = ERROR_INVALID_DATA;
		*report = o->report;
		return 0;
	}
	if (!matches(o, o->guard, 0, &error) ||
	        !matches(o, o->observed, 0, &error)) {
		return reject(o, error, report);
	}
	o->report.state = WF_COPY_SOURCE_ACCEPTED;
	o->report.error = 0;
	*report = o->report;
	return 1;
}
