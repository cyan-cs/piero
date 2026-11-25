/*
 * SFTP v3 packets against a scripted server: the bytes a server could send,
 * including the ones a hostile or broken server would. Nothing here touches a
 * network or a process.
 */
#include <stdlib.h>
#include <string.h>

#include "fs/wf_sftp.h"
#include "wf_test.h"

typedef struct
{
	uint8_t b[WF_SFTP_MAX_PACKET + 16];
	size_t n;
} Pkt;

static void u32(Pkt *p, uint32_t v)
{
	p->b[p->n++] = (uint8_t)(v >> 24);
	p->b[p->n++] = (uint8_t)(v >> 16);
	p->b[p->n++] = (uint8_t)(v >> 8);
	p->b[p->n++] = (uint8_t)v;
}

static void str(Pkt *p, const void *s, size_t n)
{
	u32(p, (uint32_t)n);
	memcpy(p->b + p->n, s, n);
	p->n += n;
}

static void begin(Pkt *p, uint8_t type)
{
	p->n = 0;
	p->b[p->n++] = type;
}

static WfStatus parse(Pkt *p, WfSftpMsg *m)
{
	return wf_sftp_parse(p->b, p->n, m);
}

static void attrs_size_perm_time(
        Pkt *p, uint64_t size, uint32_t perm, uint32_t mtime)
{
	u32(p, 0x1 | 0x4 | 0x8);
	u32(p, (uint32_t)(size >> 32));
	u32(p, (uint32_t)size);
	u32(p, perm);
	u32(p, 1);
	u32(p, mtime);
}

static void requests_are_built_exactly(void)
{
	static const uint8_t init[9] = {0, 0, 0, 5, 1, 0, 0, 0, 3};
	static const uint8_t opendir[17] = {
	        0, 0, 0, 13, 11, 0, 0, 0, 7, 0, 0, 0, 4, '/', 't', 'm', 'p'};
	static const uint8_t read_only_tail[8] = {0, 0, 0, 1, 0, 0, 0, 0};
	uint8_t o[512];
	size_t n = wf_sftp_build_init(o, sizeof o);

	CHECK(9 == n && !memcmp(o, init, 9));
	n = wf_sftp_build_path(
	        o, sizeof o, WF_SFTP_OPENDIR, 7, (const uint8_t *)"/tmp", 4);
	CHECK(17 == n && !memcmp(o, opendir, 17));
	n = wf_sftp_build_path(
	        o, sizeof o, WF_SFTP_OPEN, 1, (const uint8_t *)"/a", 2);
	/* read-only open flags and no attributes: nothing can create or write */
	CHECK(n > 8 && 0 == memcmp(o + n - 8, read_only_tail, 8));
	n = wf_sftp_build_read(
	        o, sizeof o, 3, (const uint8_t *)"hh", 2, 0x100000002ull, 4096);
	/* ... offset high word 1, low word 2, then the length 4096 */
	CHECK(n > 16 && 1 == o[n - 9] && 2 == o[n - 5] && 0x10 == o[n - 2]);

	/* refused instead of built wrongly */
	CHECK(0 ==
	        wf_sftp_build_path(o, sizeof o, WF_SFTP_OPENDIR, 1,
	                (const uint8_t *)"a\0b", 3));
	CHECK(0 ==
	        wf_sftp_build_path(o, sizeof o, WF_SFTP_OPENDIR, 1,
	                (const uint8_t *)"", 0));
	CHECK(0 ==
	        wf_sftp_build_path(
	                o, sizeof o, 99, 1, (const uint8_t *)"/a", 2));
	CHECK(0 ==
	        wf_sftp_build_path(
	                o, 8, WF_SFTP_OPENDIR, 1, (const uint8_t *)"/tmp", 4));
	CHECK(0 ==
	        wf_sftp_build_handle(o, sizeof o, WF_SFTP_CLOSE, 1,
	                (const uint8_t *)"h", 0));
	CHECK(0 ==
	        wf_sftp_build_read(
	                o, sizeof o, 1, (const uint8_t *)"h", 1, 0, 0));
	CHECK(0 ==
	        wf_sftp_build_read(o, sizeof o, 1, (const uint8_t *)"h", 1, 0,
	                WF_SFTP_MAX_PACKET));
}

static void framing_waits_for_whole_packets_and_rejects_absurd_ones(void)
{
	size_t total;
	uint8_t a[8] = {0, 0, 0, 4, 1, 2, 3, 4};

	CHECK(WF_E_RANGE == wf_sftp_frame(a, 3, &total) && 0 == total);
	CHECK(WF_E_RANGE == wf_sftp_frame(a, 7, &total) && 8 == total);
	CHECK(WF_OK == wf_sftp_frame(a, 8, &total) && 8 == total);
	a[0] = 0xFF; /* 4 GiB announced */
	CHECK(WF_E_CORRUPT == wf_sftp_frame(a, 8, &total));
	memset(a, 0, 4); /* length 0 */
	CHECK(WF_E_CORRUPT == wf_sftp_frame(a, 8, &total));
	a[1] = 4; /* exactly the limit is fine, one more is not */
	CHECK(WF_E_RANGE == wf_sftp_frame(a, 8, &total));
	a[3] = 1;
	CHECK(WF_E_CORRUPT == wf_sftp_frame(a, 8, &total));
}

static void version_and_extensions(void)
{
	static Pkt p;
	WfSftpMsg m;

	begin(&p, WF_SFTP_VERSION);
	u32(&p, 3);
	str(&p, "posix-rename@openssh.com", 24);
	str(&p, "1", 1);
	str(&p, "fsync@openssh.com", 17);
	str(&p, "1", 1);
	str(&p, "hardlink@openssh.com", 20);
	str(&p, "1", 1);
	CHECK(WF_OK == parse(&p, &m) && 3 == m.version);
	CHECK(m.ext_posix_rename && m.ext_fsync);

	/* an extension offered with another version string is not enabled */
	begin(&p, WF_SFTP_VERSION);
	u32(&p, 3);
	str(&p, "posix-rename@openssh.com", 24);
	str(&p, "2", 1);
	CHECK(WF_OK == parse(&p, &m) && !m.ext_posix_rename && !m.ext_fsync);

	begin(&p, WF_SFTP_VERSION);
	u32(&p, 6);
	CHECK(WF_E_FORMAT == parse(&p, &m)); /* not our protocol */
	begin(&p, WF_SFTP_VERSION);
	u32(&p, 3);
	str(&p, "x", 1); /* a name with no value */
	CHECK(WF_E_CORRUPT == parse(&p, &m));
}

static void status_handle_data_and_attrs(void)
{
	static Pkt p;
	WfSftpMsg m;

	begin(&p, WF_SFTP_STATUS);
	u32(&p, 9);
	u32(&p, WF_SFTP_NO_SUCH_FILE);
	str(&p, "no", 2);
	str(&p, "", 0);
	CHECK(WF_OK == parse(&p, &m) && 9 == m.id &&
	        WF_SFTP_NO_SUCH_FILE == m.status);
	p.n -= 4;                      /* the language tag missing */
	CHECK(WF_OK == parse(&p, &m)); /* some servers stop after the message */
	p.b[p.n++] = 0;                /* a stray byte at the end */
	CHECK(WF_E_CORRUPT == parse(&p, &m));

	begin(&p, WF_SFTP_HANDLE);
	u32(&p, 1);
	str(&p, "\x01\x02\x03", 3);
	CHECK(WF_OK == parse(&p, &m) && 3 == m.handle_n && 1 == m.handle[0]);
	begin(&p, WF_SFTP_HANDLE);
	u32(&p, 1);
	str(&p, "", 0);
	CHECK(WF_E_CORRUPT == parse(&p, &m)); /* empty handle */
	begin(&p, WF_SFTP_HANDLE);
	u32(&p, 1);
	u32(&p, 1000); /* announces more than is there */
	CHECK(WF_E_CORRUPT == parse(&p, &m));
	begin(&p, WF_SFTP_HANDLE);
	u32(&p, 1);
	{
		uint8_t big[WF_SFTP_MAX_HANDLE + 1];

		memset(big, 7, sizeof big);
		str(&p, big, sizeof big);
	}
	CHECK(WF_E_CORRUPT == parse(&p, &m)); /* oversized handle */

	begin(&p, WF_SFTP_DATA);
	u32(&p, 2);
	str(&p, "hello", 5);
	CHECK(WF_OK == parse(&p, &m) && 5 == m.data_n &&
	        !memcmp(m.data, "hello", 5));
	p.n--;
	CHECK(WF_E_CORRUPT == parse(&p, &m)); /* short */

	begin(&p, WF_SFTP_ATTRS);
	u32(&p, 4);
	attrs_size_perm_time(
	        &p, 0x123456789ull, WF_SFTP_S_IFLNK | 0777, 1700000000);
	CHECK(WF_OK == parse(&p, &m) && m.attrs.has_size &&
	        0x123456789ull == m.attrs.size &&
	        WF_SFTP_S_IFLNK == (m.attrs.perm & WF_SFTP_S_IFMT) &&
	        1700000000u == m.attrs.mtime);

	begin(&p, WF_SFTP_ATTRS);
	u32(&p, 4);
	u32(&p, 0x10); /* a flag nobody defined in v3 */
	CHECK(WF_E_CORRUPT == parse(&p, &m));

	/* requests and unknown types are not replies */
	begin(&p, WF_SFTP_OPEN);
	CHECK(WF_E_CORRUPT == parse(&p, &m));
	begin(&p, 250);
	CHECK(WF_E_CORRUPT == parse(&p, &m));
	CHECK(WF_E_CORRUPT == wf_sftp_parse((const uint8_t *)"", 0, &m));
}

static void name_lists_skip_dots_and_refuse_bad_names(void)
{
	static Pkt p;
	static const char control_name[5] = {'\xFF', '\x1b', '[', '2', 'J'};
	static const char with_nul[3] = {'a', 0, 'b'};
	WfSftpMsg m;
	WfSftpName e;
	bool got;

	begin(&p, WF_SFTP_NAME);
	u32(&p, 5);
	u32(&p, 5);
	str(&p, ".", 1);
	str(&p, "drwxr-xr-x .", 12);
	u32(&p, 0);
	str(&p, "..", 2);
	str(&p, "x", 1);
	u32(&p, 0);
	str(&p, "File", 4);
	str(&p, "-rw-r--r-- File", 15);
	attrs_size_perm_time(&p, 10, WF_SFTP_S_IFREG | 0644, 100);
	str(&p, "file", 4); /* differs only by case: both must arrive */
	str(&p, "l", 1);
	attrs_size_perm_time(&p, 11, WF_SFTP_S_IFREG | 0644, 100);
	str(&p, control_name, 5); /* not UTF-8, with a control: kept as bytes */
	str(&p, "l", 1);
	u32(&p, 0);
	CHECK(WF_OK == parse(&p, &m) && 5 == m.names);

	CHECK(WF_OK == wf_sftp_name_next(&m, &e, &got) && got);
	CHECK(4 == e.name_n && !memcmp(e.name, "File", 4) &&
	        10 == e.attrs.size);
	CHECK(WF_OK == wf_sftp_name_next(&m, &e, &got) && got);
	CHECK(4 == e.name_n && !memcmp(e.name, "file", 4) &&
	        11 == e.attrs.size);
	CHECK(WF_OK == wf_sftp_name_next(&m, &e, &got) && got);
	CHECK(5 == e.name_n && 0xFF == e.name[0]);
	CHECK(WF_OK == wf_sftp_name_next(&m, &e, &got) && !got);

	/* a server that names an entry with a slash or NUL is not trusted */
	for (int bad = 0; bad < 3; bad++) {
		const char *const names[] = {"a/b", with_nul, ""};
		static const size_t lens[] = {3, 3, 0};

		begin(&p, WF_SFTP_NAME);
		u32(&p, 1);
		u32(&p, 1);
		str(&p, names[bad], lens[bad]);
		str(&p, "l", 1);
		u32(&p, 0);
		CHECK(WF_OK == parse(&p, &m));
		CHECK(WF_E_CORRUPT == wf_sftp_name_next(&m, &e, &got));
	}

	/* more entries announced than the packet can hold */
	begin(&p, WF_SFTP_NAME);
	u32(&p, 1);
	u32(&p, 100000);
	CHECK(WF_E_CORRUPT == parse(&p, &m));
	begin(&p, WF_SFTP_NAME);
	u32(&p, 1);
	u32(&p, 3);
	str(&p, "a", 1);
	str(&p, "l", 1);
	u32(&p, 0);
	CHECK(WF_E_CORRUPT == parse(&p, &m)); /* 3 announced, 1 present */
	/* a count that fits but a name that runs past the end */
	begin(&p, WF_SFTP_NAME);
	u32(&p, 1);
	u32(&p, 1);
	u32(&p, 500);
	memset(p.b + p.n, 'x', 20);
	p.n += 20;
	CHECK(WF_OK == parse(&p, &m));
	CHECK(WF_E_CORRUPT == wf_sftp_name_next(&m, &e, &got));
	/* bytes left after the last entry */
	begin(&p, WF_SFTP_NAME);
	u32(&p, 1);
	u32(&p, 1);
	str(&p, "a", 1);
	str(&p, "l", 1);
	u32(&p, 0);
	u32(&p, 0xdeadbeef);
	CHECK(WF_OK == parse(&p, &m));
	CHECK(WF_OK == wf_sftp_name_next(&m, &e, &got) && got);
	CHECK(WF_E_CORRUPT == wf_sftp_name_next(&m, &e, &got));
}

static void every_prefix_of_a_good_reply_is_handled(void)
{
	/* a valid NAME reply cut at every length: never a crash, and success
	 * with both entries only for the whole packet */
	static Pkt p;
	size_t whole;

	begin(&p, WF_SFTP_NAME);
	u32(&p, 1);
	u32(&p, 2);
	str(&p, "alpha", 5);
	str(&p, "long", 4);
	attrs_size_perm_time(&p, 1, WF_SFTP_S_IFDIR | 0755, 5);
	str(&p, "beta", 4);
	str(&p, "long", 4);
	u32(&p, 0);
	whole = p.n;
	for (size_t cut = 0; cut <= whole; cut++) {
		WfSftpMsg m;
		WfSftpName e;
		bool got = true;
		size_t seen = 0;
		bool clean = true;

		if (WF_OK == wf_sftp_parse(p.b, cut, &m)) {
			while (clean && got) {
				clean = WF_OK ==
				        wf_sftp_name_next(&m, &e, &got);
				seen += clean && got;
			}
		} else {
			clean = false;
		}
		CHECK((whole == cut) == (clean && 2 == seen));
	}
}

static void realpath_requires_the_complete_name_record(void)
{
	static Pkt p;
	WfSftpMsg m;
	const uint8_t *name;
	size_t n, whole;

	begin(&p, WF_SFTP_NAME);
	u32(&p, 7);
	u32(&p, 1);
	str(&p, "/home/box", 9);
	str(&p, "long name", 9);
	attrs_size_perm_time(&p, 42, WF_SFTP_S_IFDIR | 0755, 100);
	whole = p.n;
	for (size_t cut = 0; cut <= whole; cut++) {
		if (WF_OK != wf_sftp_parse(p.b, cut, &m)) {
			continue;
		}
		name = p.b;
		n = 123;
		if (cut == whole) {
			CHECK(WF_OK == wf_sftp_name_first(&m, &name, &n));
			CHECK(9 == n && !memcmp(name, "/home/box", 9));
		} else {
			CHECK(WF_E_CORRUPT ==
			        wf_sftp_name_first(&m, &name, &n));
			CHECK(!name && !n);
		}
	}
	p.b[p.n++] = 0;
	CHECK(WF_OK == parse(&p, &m));
	CHECK(WF_E_CORRUPT == wf_sftp_name_first(&m, &name, &n));
	CHECK(!name && !n);

	begin(&p, WF_SFTP_NAME);
	u32(&p, 7);
	u32(&p, 1);
	str(&p, "/home/box", 9);
	str(&p, "", 0);
	u32(&p, 0x10); /* unknown attribute flags cannot be sized */
	CHECK(WF_OK == parse(&p, &m));
	CHECK(WF_E_CORRUPT == wf_sftp_name_first(&m, &name, &n));
	CHECK(!name && !n);
}

int main(void)
{
	requests_are_built_exactly();
	framing_waits_for_whole_packets_and_rejects_absurd_ones();
	version_and_extensions();
	status_handle_data_and_attrs();
	name_lists_skip_dots_and_refuse_bad_names();
	every_prefix_of_a_good_reply_is_handled();
	realpath_requires_the_complete_name_record();
	return TEST_RESULT();
}
