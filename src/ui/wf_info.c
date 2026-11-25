#include "wf_info.h"

#include <stdio.h>
#include <string.h>

#include "../i18n/wf_i18n.h"
#include "../base/wf_macro.h"

/* Win32 values, spelled out so this file stays free of <windows.h>. */
enum
{
	ATTR_READONLY = 0x00000001,
	ATTR_HIDDEN = 0x00000002,
	ATTR_SYSTEM = 0x00000004,
	ATTR_ARCHIVE = 0x00000020,
	ATTR_TEMPORARY = 0x00000100,
	ATTR_SPARSE = 0x00000200,
	ATTR_COMPRESSED = 0x00000800,
	ATTR_OFFLINE = 0x00001000,
	ATTR_NOT_INDEXED = 0x00002000,
	ATTR_ENCRYPTED = 0x00004000,
	ATTR_REPARSE = 0x00000400,
	ATTR_RECALL_OPEN = 0x00040000,
	ATTR_PINNED = 0x00080000,
	ATTR_UNPINNED = 0x00100000,
	ATTR_RECALL_DATA = 0x00400000
};

#define FILETIME_TICKS_PER_SECOND 10000000ull
#define FILETIME_TICKS_PER_DAY (86400ull * FILETIME_TICKS_PER_SECOND)
#define DAYS_1601_TO_1970 134774

void wf_info_format_size(uint64_t bytes, char *out, size_t cap)
{
	char digits[24], grouped[32];
	static const char *const unit[] = {
	        "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
	size_t n = 0, g = 0, i;
	int k = 0;
	uint64_t v = bytes;

	if (!out || !cap) {
		return;
	}
	do {
		digits[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	for (i = 0; i < n; i++) {
		if (i && i % 3 == 0) {
			grouped[g++] = ',';
		}
		grouped[g++] = digits[i];
	}
	for (i = 0; i < g / 2; i++) {
		char t = grouped[i];

		grouped[i] = grouped[g - 1 - i];
		grouped[g - 1 - i] = t;
	}
	grouped[g] = 0;
	if (bytes < 1024) {
		(void)wf_msgf(out, cap, MSG_INFO_BYTES, &WF_ARG_S(grouped), 1);
		return;
	}
	while (k < 5 && (bytes >> (10 * (k + 1))) >= 1024) {
		k++;
	}
	{
		unsigned shift = 10u * (unsigned)(k + 1);
		uint64_t whole = bytes >> shift;
		/* two decimals from the top 7 fraction bits: no overflow even
		 * for EiB-sized values */
		uint64_t part = bytes & ((1ull << shift) - 1);
		unsigned frac = (unsigned)(((part >> (shift - 7)) * 100) >> 7);

		char scaled[32];

		snprintf(scaled, sizeof scaled, "%llu.%02u",
		        (unsigned long long)whole, frac);
		(void)wf_msgf(out, cap, MSG_INFO_BYTES_UNIT,
		        (WfMsgArg[]){WF_ARG_S(grouped), WF_ARG_S(scaled),
		                WF_ARG_S(unit[k])},
		        3);
	}
}

void wf_info_format_time(WfFileTime t, char *out, size_t cap)
{
	int64_t z;
	int64_t era, doe, yoe, y, doy, mp, d, m;
	uint64_t days, secs;

	if (!out || !cap) {
		return;
	}
	if (0 == t) {
		snprintf(out, cap, "%s", wf_msg(MSG_INFO_UNKNOWN));
		return;
	}
	days = t / FILETIME_TICKS_PER_DAY;
	secs = (t % FILETIME_TICKS_PER_DAY) / FILETIME_TICKS_PER_SECOND;
	/* days since 1601-01-01 -> civil date (proleptic Gregorian) */
	z = (int64_t)days - DAYS_1601_TO_1970 + 719468;
	era = (z >= 0 ? z : z - 146096) / 146097;
	doe = z - era * 146097;
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	y = yoe + era * 400;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	d = doy - (153 * mp + 2) / 5 + 1;
	m = mp < 10 ? mp + 3 : mp - 9;
	if (m <= 2) {
		y++;
	}
	if (y > 9999) {
		snprintf(out, cap, "%s", wf_msg(MSG_INFO_OUT_OF_RANGE));
		return;
	}
	snprintf(out, cap, "%04lld-%02lld-%02lld %02u:%02u:%02u", (long long)y,
	        (long long)m, (long long)d, (unsigned)(secs / 3600),
	        (unsigned)(secs / 60 % 60), (unsigned)(secs % 60));
}

void wf_info_format_attributes(uint32_t attributes, char *out, size_t cap)
{
	static const struct
	{
		uint32_t bit;
		const char *name;
	} names[] = {{ATTR_READONLY, "ReadOnly"}, {ATTR_HIDDEN, "Hidden"},
	        {ATTR_SYSTEM, "System"}, {ATTR_ARCHIVE, "Archive"},
	        {ATTR_TEMPORARY, "Temporary"}, {ATTR_SPARSE, "Sparse"},
	        {ATTR_COMPRESSED, "Compressed"}, {ATTR_OFFLINE, "Offline"},
	        {ATTR_NOT_INDEXED, "NotIndexed"}, {ATTR_ENCRYPTED, "Encrypted"},
	        {ATTR_RECALL_OPEN, "RecallOnOpen"},
	        {ATTR_RECALL_DATA, "RecallOnDataAccess"},
	        {ATTR_PINNED, "Pinned"}, {ATTR_UNPINNED, "Unpinned"}};
	size_t used = 0, i;

	if (!out || !cap) {
		return;
	}
	out[0] = 0;
	for (i = 0; i < WF_ARRAY_LEN(names); i++) {
		int w;

		if (!(attributes & names[i].bit)) {
			continue;
		}
		w = snprintf(out + used, cap - used, "%s%s", used ? " " : "",
		        names[i].name);
		if (w < 0 || (size_t)w >= cap - used) {
			out[cap - 1] = 0;
			return;
		}
		used += (size_t)w;
	}
	if (!used) {
		snprintf(out, cap, "%s", wf_msg(MSG_INFO_NONE));
	}
}

void wf_info_format_reparse(uint32_t tag, char *out, size_t cap)
{
	if (!out || !cap) {
		return;
	}
	switch (tag) {
	case 0xA000000Cu:
		snprintf(out, cap, "%s", wf_msg(MSG_INFO_SYMLINK));
		return;
	case 0xA0000003u:
		snprintf(out, cap, "%s", wf_msg(MSG_INFO_JUNCTION));
		return;
	case 0x8000001Bu:
		snprintf(out, cap, "%s", wf_msg(MSG_INFO_APPEXEC));
		return;
	case 0x80000017u:
		snprintf(out, cap, "%s", wf_msg(MSG_INFO_WOF));
		return;
	default: break;
	}
	if (0x9000001Au == (tag & 0xFFFF0FFFu)) { /* IO_REPARSE_TAG_CLOUD_* */
		snprintf(out, cap, "%s", wf_msg(MSG_INFO_CLOUD_PLACEHOLDER));
	} else {
		char hex[16];

		snprintf(hex, sizeof hex, "%08X", (unsigned)tag);
		(void)wf_msgf(out, cap, MSG_INFO_REPARSE, &WF_ARG_S(hex), 1);
	}
}

/* Bounded strcat (strncat is deprecated under the project's CRT policy). */
static void append(char *dst, size_t cap, const char *s)
{
	size_t n = strlen(dst);

	if (n < cap) {
		snprintf(dst + n, cap - n, "%s", s);
	}
}

/* Appends "label" + "value" as one line; both are cut to fit. */
static void put(WfInfo *info, const char *label, const char *value)
{
	if (info->n >= WF_INFO_LINES) {
		return;
	}
	snprintf(info->text[info->n++], WF_INFO_LINE_BYTES, "%s%s", label,
	        value);
}

static void time_line(WfInfo *info,
        const char *label,
        WfFileTime t,
        bool known,
        WfLocalTime local)
{
	char text[48];
	WfFileTime shown = t;
	bool is_local = false;

	if (!known || 0 == t) {
		put(info, label, wf_msg(MSG_INFO_UNKNOWN));
		return;
	}
	if (local) {
		WfFileTime converted = local(t);

		if (converted) {
			shown = converted;
			is_local = true;
		}
	}
	wf_info_format_time(shown, text, sizeof text);
	if (!is_local && strcmp(text, wf_msg(MSG_INFO_OUT_OF_RANGE))) {
		append(text, sizeof text, " UTC");
	}
	put(info, label, text);
}

static void selection_lines(
        WfInfo *info, const WfDir *dir, const WfNameSet *sel)
{
	size_t files = 0, folders = 0, i, found;
	uint64_t total = 0;
	bool saturated = false;
	char size[64], line[WF_INFO_LINE_BYTES], more[80];

	if (!sel || 0 == sel->n || !dir) {
		put(info, wf_msg(MSG_INFO_LABEL_SELECTED),
		        wf_msg(MSG_INFO_NONE));
		return;
	}
	for (i = 0; i < dir->n; i++) {
		const WfEntry *e = &dir->e[i];

		if (!wf_nameset_has(sel, e->name)) {
			continue;
		}
		if (e->flags & WF_ENT_DIR) {
			folders++;
		} else {
			files++;
			if (total > UINT64_MAX - e->size) {
				total = UINT64_MAX;
				saturated = true;
			} else {
				total += e->size;
			}
		}
	}
	found = files + folders;
	{
		char n_items[40], n_files[40], n_folders[40];

		(void)wf_msgf(n_items, sizeof n_items,
		        wf_msg_count(MSG_INFO_ITEMS_ONE, MSG_INFO_ITEMS_OTHER,
		                (long long)sel->n),
		        &WF_ARG_N(sel->n), 1);
		(void)wf_msgf(n_files, sizeof n_files,
		        wf_msg_count(MSG_INFO_FILES_ONE, MSG_INFO_FILES_OTHER,
		                (long long)files),
		        &WF_ARG_N(files), 1);
		(void)wf_msgf(n_folders, sizeof n_folders,
		        wf_msg_count(MSG_INFO_FOLDERS_ONE,
		                MSG_INFO_FOLDERS_OTHER, (long long)folders),
		        &WF_ARG_N(folders), 1);
		(void)wf_msgf(line, sizeof line, MSG_INFO_SELECTION,
		        (WfMsgArg[]){WF_ARG_S(n_items), WF_ARG_S(n_files),
		                WF_ARG_S(n_folders)},
		        3);
	}
	if (found < sel->n) {
		(void)wf_msgf(more, sizeof more, MSG_INFO_NOT_IN_LISTING,
		        &WF_ARG_N(sel->n - found), 1);
		append(line, sizeof line, more);
	}
	put(info, wf_msg(MSG_INFO_LABEL_SELECTED), line);
	wf_info_format_size(total, size, sizeof size);
	if (saturated) {
		char scaled[sizeof size];

		snprintf(scaled, sizeof scaled, "%s", size);
		(void)wf_msgf(size, sizeof size, MSG_INFO_AT_LEAST,
		        &WF_ARG_S(scaled), 1);
	}
	snprintf(line, sizeof line, "%s", size);
	append(line, sizeof line, wf_msg(MSG_INFO_FILES_ONLY));
	put(info, wf_msg(MSG_INFO_LABEL_TOTAL), line);
}

void wf_info_build(WfInfo *out,
        const WfEntry *entry,
        const WfDir *dir,
        const WfNameSet *sel,
        WfLocalTime local)
{
	wf_info_build_counted(out, entry, dir, sel, local, NULL);
}

void wf_info_build_counted(WfInfo *out,
        const WfEntry *entry,
        const WfDir *dir,
        const WfNameSet *sel,
        WfLocalTime local,
        const char *folder_total)
{
	wf_info_build_facts(out, entry, dir, sel, local, folder_total, NULL);
}

/* The lines of DR-DRAFT-026. Every part says "unknown" or why it is missing; a
 * part that was not read is never shown as a number. */
static void facts_lines(WfInfo *out, const WfNtfsFacts *f)
{
	char buf[WF_INFO_LINE_BYTES], num[40], more[40];
	const char *blank = "           "; /* the width of a label */
	bool any = false;

	if (!f || WF_NF_NOT_ASKED == f->state) {
		return;
	}
	if (WF_NF_REMOTE == f->state) {
		put(out, wf_msg(MSG_NTFS_LABEL_FACTS),
		        wf_msg(MSG_NTFS_NOT_READ));
		return;
	}
	if (WF_NF_OK != f->state) {
		snprintf(num, sizeof num, "%lu", (unsigned long)f->error);
		(void)wf_msgf(
		        buf, sizeof buf, MSG_NTFS_FAILED, &WF_ARG_S(num), 1);
		put(out, wf_msg(MSG_NTFS_LABEL_FACTS), buf);
		return;
	}
	if (f->attributes & ATTR_REPARSE) {
		put(out, wf_msg(MSG_NTFS_LABEL_TARGET),
		        f->target_known ? f->target
		                        : wf_msg(MSG_NTFS_TARGET_UNREAD));
	}
	if (!f->links_known) {
		put(out, wf_msg(MSG_NTFS_LABEL_NAMES),
		        wf_msg(MSG_INFO_UNKNOWN));
	} else if (f->links <= 1) {
		put(out, wf_msg(MSG_NTFS_LABEL_NAMES),
		        wf_msg(MSG_NTFS_NAMES_ONE));
	} else {
		snprintf(num, sizeof num, "%lu", (unsigned long)f->links);
		snprintf(more, sizeof more, "%lu",
		        (unsigned long)(f->links - 1));
		(void)wf_msgf(buf, sizeof buf, MSG_NTFS_NAMES_MANY,
		        (WfMsgArg[]){WF_ARG_S(num), WF_ARG_S(more)}, 2);
		put(out, wf_msg(MSG_NTFS_LABEL_NAMES), buf);
	}
	if (!f->streams_known) {
		put(out, wf_msg(MSG_NTFS_LABEL_STREAMS),
		        wf_msg(MSG_NTFS_STREAMS_UNLISTED));
	} else if (0 == f->n_streams) {
		put(out, wf_msg(MSG_NTFS_LABEL_STREAMS), wf_msg(MSG_INFO_NONE));
	} else {
		size_t shown = f->n_streams < WF_NF_STREAMS ? f->n_streams
		                                            : WF_NF_STREAMS;

		for (size_t i = 0; i < shown; i++) {
			wf_info_format_size(f->stream[i].size, num, sizeof num);
			(void)wf_msgf(buf, sizeof buf, MSG_NTFS_STREAM_ITEM,
			        (WfMsgArg[]){WF_ARG_S(f->stream[i].name),
			                WF_ARG_S(num)},
			        2);
			put(out, i ? blank : wf_msg(MSG_NTFS_LABEL_STREAMS),
			        buf);
		}
		if (f->n_streams > shown) {
			snprintf(num, sizeof num, "%lu",
			        (unsigned long)(f->n_streams - shown));
			(void)wf_msgf(buf, sizeof buf, MSG_NTFS_STREAMS_MORE,
			        &WF_ARG_S(num), 1);
			put(out, blank, buf);
		}
	}
	if (f->volume_known) {
		snprintf(num, sizeof num, "%04X-%04X",
		        (unsigned)(f->serial >> 16),
		        (unsigned)(f->serial & 0xFFFFu));
		(void)wf_msgf(buf, sizeof buf, MSG_NTFS_VOLUME_LINE,
		        (WfMsgArg[]){WF_ARG_S(f->root), WF_ARG_S(f->fs_name),
		                WF_ARG_S(num)},
		        3);
		put(out, wf_msg(MSG_NTFS_LABEL_VOLUME), buf);
	} else {
		put(out, wf_msg(MSG_NTFS_LABEL_VOLUME),
		        wf_msg(MSG_INFO_UNKNOWN));
	}
	buf[0] = 0;
	{
		static const struct
		{
			uint32_t bit;
			WfMsg word;
		} kinds[] = {{ATTR_COMPRESSED, MSG_NTFS_COMPRESSED},
		        {ATTR_ENCRYPTED, MSG_NTFS_ENCRYPTED},
		        {ATTR_SPARSE, MSG_NTFS_SPARSE},
		        {ATTR_OFFLINE, MSG_NTFS_OFFLINE}};

		for (size_t i = 0; i < WF_ARRAY_LEN(kinds); i++) {
			if (f->attributes & kinds[i].bit) {
				if (any) {
					append(buf, sizeof buf, ", ");
				}
				append(buf, sizeof buf, wf_msg(kinds[i].word));
				any = true;
			}
		}
	}
	put(out, wf_msg(MSG_NTFS_LABEL_STORED),
	        any ? buf : wf_msg(MSG_NTFS_ORDINARY));
}

void wf_info_build_facts(WfInfo *out,
        const WfEntry *entry,
        const WfDir *dir,
        const WfNameSet *sel,
        WfLocalTime local,
        const char *folder_total,
        const WfNtfsFacts *facts)
{
	wf_info_build_full(
	        out, entry, dir, sel, local, folder_total, facts, NULL);
}

void wf_info_build_full(WfInfo *out,
        const WfEntry *entry,
        const WfDir *dir,
        const WfNameSet *sel,
        WfLocalTime local,
        const char *folder_total,
        const WfNtfsFacts *facts,
        const char *hash)
{
	if (!out) {
		return;
	}
	memset(out, 0, sizeof *out);
	put(out, wf_msg(MSG_INFO_TITLE), "");
	if (!entry) {
		put(out, wf_msg(MSG_INFO_NO_ENTRY), "");
	} else {
		char buf[96], extra[48];
		const char *dot = strrchr(entry->display, '.');
		bool link = entry->flags & WF_ENT_LINK;

		put(out, wf_msg(MSG_INFO_LABEL_NAME), entry->display);
		if (entry->flags & WF_ENT_DIR) {
			snprintf(buf, sizeof buf, "%s",
			        wf_msg(link ? MSG_INFO_FOLDER_LINK
			                    : MSG_INFO_FOLDER));
		} else if (dot && dot != entry->display && dot[1]) {
			(void)wf_msgf(buf, sizeof buf, MSG_INFO_TYPE_WITH_EXT,
			        (WfMsgArg[]){WF_ARG_S(wf_msg(
			                             link ? MSG_INFO_FILE_LINK
			                                  : MSG_INFO_FILE)),
			                WF_ARG_S(dot)},
			        2);
		} else {
			snprintf(buf, sizeof buf, "%s",
			        wf_msg(link ? MSG_INFO_FILE_LINK
			                    : MSG_INFO_FILE));
		}
		if (entry->flags & WF_ENT_CLOUD) {
			append(buf, sizeof buf, wf_msg(MSG_INFO_CLOUD));
		}
		put(out, wf_msg(MSG_INFO_LABEL_TYPE), buf);
		if (entry->flags & WF_ENT_DIR) {
			put(out, wf_msg(MSG_INFO_LABEL_SIZE),
			        folder_total ? folder_total
			                     : wf_msg(MSG_INFO_NA_FOLDER));
		} else {
			wf_info_format_size(entry->size, buf, sizeof buf);
			put(out, wf_msg(MSG_INFO_LABEL_SIZE), buf);
		}
		time_line(out, wf_msg(MSG_INFO_LABEL_CREATED), entry->ctime,
		        entry->meta_known, local);
		time_line(out, wf_msg(MSG_INFO_LABEL_MODIFIED), entry->mtime,
		        true, local);
		if (entry->meta_known) {
			wf_info_format_attributes(
			        entry->attributes, buf, sizeof buf);
			put(out, wf_msg(MSG_INFO_LABEL_ATTRIBUTES), buf);
			put(out, wf_msg(MSG_INFO_LABEL_READONLY),
			        wf_msg((entry->attributes & ATTR_READONLY)
			                        ? MSG_INFO_YES
			                        : MSG_INFO_NO));
		} else {
			put(out, wf_msg(MSG_INFO_LABEL_ATTRIBUTES),
			        wf_msg(MSG_INFO_UNKNOWN));
			put(out, wf_msg(MSG_INFO_LABEL_READONLY),
			        wf_msg(MSG_INFO_UNKNOWN));
		}
		if (link) {
			wf_info_format_reparse(
			        entry->reparse_tag, extra, sizeof extra);
			put(out, wf_msg(MSG_INFO_LABEL_LINK), extra);
		}
		facts_lines(out, facts);
		if (hash) {
			put(out, wf_msg(MSG_INFO_LABEL_SHA), hash);
		}
	}
	put(out, "", "");
	selection_lines(out, dir, sel);
}
