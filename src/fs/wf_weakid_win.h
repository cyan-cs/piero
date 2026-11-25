#ifndef WF_WEAKID_WIN_H
#define WF_WEAKID_WIN_H

#include <windows.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "wf_dir.h"

/*
 * The identity a FAT or exFAT volume can give (see WfFileId). FileIdInfo is
 * refused there, but the volume serial and the 64-bit directory-entry index are
 * known, by the handle and by a directory listing alike. This is only a way to
 * say "the same entry as listed": it is used where nothing existing is removed
 * or replaced, and only on a volume that says it is FAT.
 */

static inline bool wf_weak_unsupported(DWORD error)
{
	return ERROR_INVALID_PARAMETER == error || ERROR_NOT_SUPPORTED == error ||
	        ERROR_INVALID_LEVEL == error || ERROR_INVALID_FUNCTION == error;
}

/* The volume of `h` is FAT, FAT32 or exFAT. */
static inline bool wf_weak_volume(HANDLE h)
{
	wchar_t fs[16] = L"";

	return GetVolumeInformationByHandleW(h, NULL, 0, NULL, NULL, NULL, fs, 16) &&
	        (0 == _wcsicmp(fs, L"FAT") || 0 == _wcsicmp(fs, L"FAT32") ||
	                0 == _wcsicmp(fs, L"exFAT"));
}

/* `h` is the root folder of its volume (its index is 0 on FAT). */
static inline bool wf_weak_is_root(HANDLE h)
{
	struct
	{
		FILE_NAME_INFO info;
		wchar_t room[4];
	} name;

	return GetFileInformationByHandleEx(h, FileNameInfo, &name, sizeof name) &&
	        sizeof(wchar_t) == name.info.FileNameLength &&
	        L'\\' == name.info.FileName[0];
}

/* The weak identity from the volume serial and the entry index. */
static inline void wf_weak_fill(WfFileId *out, uint64_t volume, uint64_t index)
{
	memset(out, 0, sizeof *out);
	out->volume = volume;
	memcpy(out->id, &index, sizeof index);
	out->kind = WF_ID_WEAK;
}

/* The weak identity of the open object `h`. False unless the volume is FAT and
 * the entry has an index (the root of the volume has none and is told by its
 * name). */
static inline bool wf_weak_of_handle(HANDLE h, WfFileId *out)
{
	BY_HANDLE_FILE_INFORMATION info;
	uint64_t index;

	if (!wf_weak_volume(h) || !GetFileInformationByHandle(h, &info)) {
		return false;
	}
	index = ((uint64_t)info.nFileIndexHigh << 32) | info.nFileIndexLow;
	if (0 == index && !wf_weak_is_root(h)) {
		return false;
	}
	wf_weak_fill(out, info.dwVolumeSerialNumber, index);
	return true;
}

/* The identity of the open object: the proven one, else (only where the volume
 * has no FileId at all) the weak one. False with *error set when there is none. */
static inline bool wf_handle_identity_any(HANDLE h, WfFileId *out, DWORD *error)
{
	FILE_ID_INFO info;
	DWORD err;

	memset(out, 0, sizeof *out);
	if (GetFileInformationByHandleEx(h, FileIdInfo, &info, sizeof info)) {
		out->volume = info.VolumeSerialNumber;
		memcpy(out->id, info.FileId.Identifier, sizeof out->id);
		out->kind = WF_ID_STRONG;
		return true;
	}
	err = GetLastError();
	if (wf_weak_unsupported(err) && wf_weak_of_handle(h, out)) {
		return true;
	}
	*error = err;
	return false;
}

/* The attributes of the open object. FAT answers FileAttributeTagInfo with an
 * invalid-parameter error, and the older call says the same attributes. Any
 * other failure is a failure. */
static inline bool wf_handle_attributes(HANDLE h, DWORD *attributes, DWORD *error)
{
	FILE_ATTRIBUTE_TAG_INFO tag;
	BY_HANDLE_FILE_INFORMATION info;
	DWORD err;

	if (GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof tag)) {
		*attributes = tag.FileAttributes;
		return true;
	}
	err = GetLastError();
	if (wf_weak_unsupported(err) && GetFileInformationByHandle(h, &info)) {
		*attributes = info.dwFileAttributes;
		return true;
	}
	*error = err;
	return false;
}

/*
 * The boundary with FILE_ID_INFO. The copy engine keeps what it has seen of a
 * file as FILE_ID_INFO (the shape the system and the other modules of the
 * engine use), which has no place for a kind. Here, and only here, a weak
 * identity is carried in it: the last byte is a mark and the seven before it are
 * zero, which no weak identity has otherwise and a proven one practically never
 * does; a proven FileId that did look so is only read as weak, which refuses
 * more, never less. The kind is put back into a WfFileId by wf_fileid_from_info
 * as soon as the engine compares, so that nothing else reads a kind off bytes.
 */
#define WF_WEAK_INFO_MARK 0xFA

static inline bool wf_info_is_weak(const FILE_ID_INFO *info)
{
	static const uint8_t zero[7] = {0};

	return WF_WEAK_INFO_MARK == info->FileId.Identifier[15] &&
	        0 == memcmp(info->FileId.Identifier + 8, zero, sizeof zero);
}

static inline void wf_fileid_to_info(const WfFileId *id, FILE_ID_INFO *out)
{
	memset(out, 0, sizeof *out);
	out->VolumeSerialNumber = id->volume;
	memcpy(out->FileId.Identifier, id->id, sizeof id->id);
	if (wf_fileid_is_weak(id)) {
		out->FileId.Identifier[15] = WF_WEAK_INFO_MARK;
	}
}

static inline void wf_fileid_from_info(const FILE_ID_INFO *info, WfFileId *out)
{
	memset(out, 0, sizeof *out);
	out->volume = info->VolumeSerialNumber;
	memcpy(out->id, info->FileId.Identifier, sizeof out->id);
	if (wf_info_is_weak(info)) {
		out->id[15] =
		        0; /* the mark was the carrier's, not part of the index */
		out->kind = WF_ID_WEAK;
	} else {
		out->kind = WF_ID_STRONG;
	}
}

/* The identity of the open object in the shape of FILE_ID_INFO (a weak one
 * marked, above). */
static inline bool wf_handle_file_id_info(HANDLE h, FILE_ID_INFO *out)
{
	WfFileId any;
	DWORD error;

	if (!wf_handle_identity_any(h, &any, &error)) {
		return false;
	}
	wf_fileid_to_info(&any, out);
	return true;
}

/* What was listed is met by what is found only when they are of one kind and the
 * same (WfFileId equality of `listed` and what `info` says). */
static inline bool wf_info_matches(
        const WfFileId *listed, const FILE_ID_INFO *info)
{
	WfFileId found;

	wf_fileid_from_info(info, &found);
	return WF_ID_NONE != listed->kind && listed->kind == found.kind &&
	        listed->volume == found.volume &&
	        0 == memcmp(listed->id, found.id, sizeof listed->id);
}

#endif
