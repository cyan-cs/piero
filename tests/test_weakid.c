/*
 * The weak identity of a FAT volume (wf_dir.h, wf_weakid_win.h, wf_xfer.h): what
 * it is, and that it is never taken for a proven one. The kind is kept with the
 * identity: a weak one meets only a weak one, a proven one only a proven one,
 * and the bytes alone do not make them the same. FILE_ID_INFO, which has no place
 * for a kind, is the one place where a mark in the bytes stands in for it, and it
 * is turned back into a kind at that boundary. On a volume that has FileIds (this
 * one) no weak identity is ever made.
 */
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "fs/wf_weakid_win.h"
#include "wf_test.h"
#include "xfer/wf_xfer.h"

static WfFileId proven(uint64_t volume, uint8_t first)
{
	WfFileId id;

	memset(&id, 0, sizeof id);
	id.volume = volume;
	id.id[0] = first;
	id.kind = WF_ID_STRONG;
	return id;
}

static void the_kinds_exclude_each_other(void)
{
	WfFileId none, p = proven(7, 1), w;

	memset(&none, 0, sizeof none);
	wf_weak_fill(&w, 7, 0x400200);
	CHECK(WF_ID_NONE == none.kind && WF_ID_WEAK == w.kind);
	CHECK(!wf_fileid_proven(&none) && !wf_fileid_is_weak(&none));
	CHECK(wf_fileid_proven(&p) && !wf_fileid_is_weak(&p));
	CHECK(!wf_fileid_proven(&w) && wf_fileid_is_weak(&w));
	CHECK(!wf_fileid_proven(NULL) && !wf_fileid_is_weak(NULL));
	/* what may be read or opened on: either, and not nothing */
	CHECK(wf_fileid_usable_to_read(&w) && wf_fileid_usable_to_read(&p));
	CHECK(!wf_fileid_usable_to_read(&none) &&
	        !wf_fileid_usable_to_read(NULL));
	/* the index is in the bytes, and the mark of the carrier is not */
	CHECK(0 == w.id[15] && 0x00 == w.id[0] && 0x02 == w.id[1] &&
	        0x40 == w.id[2]);
}

static void weak_and_proven_identities_do_not_meet(void)
{
	WfFileId p = proven(7, 1), q = proven(7, 2), w, w2, w3;

	wf_weak_fill(&w, 7, 0x400200);
	wf_weak_fill(&w2, 7, 0x400200);
	wf_weak_fill(&w3, 7, 0x400240);
	/* proven with proven, as ever */
	CHECK(wf_fileid_equal(&p, &p) && !wf_fileid_equal(&p, &q));
	CHECK(wf_fileid_equal_or_weak(&p, &p) &&
	        !wf_fileid_equal_or_weak(&p, &q));
	/* weak with weak: the same entry on the same volume */
	CHECK(wf_fileid_equal_or_weak(&w, &w2) &&
	        !wf_fileid_equal_or_weak(&w, &w3));
	w2.volume = 8;
	CHECK(!wf_fileid_equal_or_weak(&w, &w2));
	/* never across: not by the strict test, not by the other, same bytes or not */
	CHECK(!wf_fileid_equal(&w, &w));
	p = w;
	p.kind = WF_ID_STRONG; /* the same bytes, claimed as proven */
	CHECK(!wf_fileid_equal_or_weak(&w, &p) &&
	        !wf_fileid_equal_or_weak(&p, &w));
	/* an identity of no kind meets nothing, itself included */
	memset(&p, 0, sizeof p);
	CHECK(!wf_fileid_equal(&p, &p) && !wf_fileid_equal_or_weak(&p, &p));
}

/* FILE_ID_INFO is where the kind has no place: a weak identity is marked there,
 * and read back as weak; a proven one is not marked, and read back as proven. */
static void the_kind_survives_the_win32_shape(void)
{
	WfFileId p = proven(7, 1), w, back;
	FILE_ID_INFO info;

	wf_weak_fill(&w, 7, 0x400200);
	wf_fileid_to_info(&w, &info);
	CHECK(wf_info_is_weak(&info) && 7 == info.VolumeSerialNumber);
	wf_fileid_from_info(&info, &back);
	CHECK(WF_ID_WEAK == back.kind && wf_fileid_equal_or_weak(&w, &back));
	CHECK(0 == back.id[15]); /* the mark was the carrier's */
	wf_fileid_to_info(&p, &info);
	CHECK(!wf_info_is_weak(&info));
	wf_fileid_from_info(&info, &back);
	CHECK(WF_ID_STRONG == back.kind && wf_fileid_equal(&p, &back));
	/* what is found meets what was listed only as the same kind */
	wf_fileid_to_info(&w, &info);
	CHECK(wf_info_matches(&w, &info) && !wf_info_matches(&p, &info));
	wf_fileid_to_info(&p, &info);
	CHECK(wf_info_matches(&p, &info) && !wf_info_matches(&w, &info));
	/* the same numbers as a proven identity are not a weak one's */
	{
		WfFileId same_numbers = w;

		same_numbers.kind = WF_ID_STRONG;
		wf_fileid_to_info(&w, &info);
		CHECK(!wf_info_matches(&same_numbers, &info));
	}
	/* a FileId that happens to carry the mark is read as weak (stricter) */
	memset(&info, 0, sizeof info);
	info.VolumeSerialNumber = 7;
	info.FileId.Identifier[0] = 5;
	info.FileId.Identifier[15] = WF_WEAK_INFO_MARK;
	wf_fileid_from_info(&info, &back);
	CHECK(WF_ID_WEAK == back.kind);
	/* none is never matched */
	memset(&back, 0, sizeof back);
	CHECK(!wf_info_matches(&back, &info));
}

static void a_volume_with_file_ids_gives_no_weak_identity(void)
{
	wchar_t tmp[MAX_PATH];
	WfFileId id;
	DWORD error = 0;
	HANDLE h;

	GetTempPathW(MAX_PATH, tmp);
	h = CreateFileW(tmp, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	        FILE_FLAG_BACKUP_SEMANTICS, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	if (INVALID_HANDLE_VALUE == h) {
		return;
	}
	if (wf_weak_volume(h)) {
		printf("skipped: the temporary folder is on a FAT volume\n");
	} else {
		FILE_ID_INFO info;
		DWORD attributes;

		CHECK(!wf_weak_of_handle(h, &id));
		CHECK(wf_handle_identity_any(h, &id, &error));
		CHECK(wf_fileid_proven(&id));
		CHECK(wf_handle_file_id_info(h, &info) &&
		        !wf_info_is_weak(&info));
		CHECK(wf_handle_attributes(h, &attributes, &error) &&
		        (attributes & FILE_ATTRIBUTE_DIRECTORY));
	}
	CloseHandle(h);
}

int main(void)
{
	the_kinds_exclude_each_other();
	weak_and_proven_identities_do_not_meet();
	the_kind_survives_the_win32_shape();
	a_volume_with_file_ids_gives_no_weak_identity();
	return TEST_RESULT();
}
