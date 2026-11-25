/*
 * The SFTP client against an in-memory server that speaks the wire protocol,
 * delivers it in awkward pieces, and can misbehave in the ways a broken or
 * hostile server can. No network, no process.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "fs/wf_sftpc.h"
#include "wf_fake_sftp.h"
#include "wf_test.h"

static WfStatus srv_write(void *ctx, const uint8_t *b, size_t n, uint32_t ms)
{
	Srv *s = ctx;

	(void)ms;
	if (s->in_n + n > sizeof s->in) {
		return WF_E_RANGE;
	}
	memcpy(s->in + s->in_n, b, n);
	s->in_n += n;
	for (;;) {
		size_t total;

		if (WF_OK != wf_sftp_frame(s->in, s->in_n, &total)) {
			break;
		}
		if (F_EOF_AFTER != s->fault && F_TIMEOUT_AFTER != s->fault) {
			on_request(s, s->in + 4, total - 4);
		} else if (s->requests < s->after) {
			on_request(s, s->in + 4, total - 4);
		} else {
			s->requests++;
		}
		memmove(s->in, s->in + total, s->in_n - total);
		s->in_n -= total;
	}
	return WF_OK;
}

static WfStatus srv_read(
        void *ctx, uint8_t *b, size_t cap, size_t *got, uint32_t ms)
{
	Srv *s = ctx;
	size_t have = s->out_n - s->out_at, give;

	if (0 == have) {
		if (F_TIMEOUT_AFTER == s->fault) {
			Sleep(ms < 20 ? ms
			              : 20); /* a wait, as a silent server is one */
			return WF_E_TIMEOUT;
		}
		*got = 0; /* the end of the stream */
		return WF_OK;
	}
	if (s->slow_ms) {
		Sleep(s->slow_ms);
	}
	give = have < cap ? have : cap;
	if (s->piece && give > s->piece) {
		give = s->piece;
	}
	memcpy(b, s->out + s->out_at, give);
	s->out_at += give;
	*got = give;
	return WF_OK;
}

static Srv *new_srv(Fault f, int after)
{
	Srv *s = calloc(1, sizeof *s);

	CHECK(NULL != s);
	if (s) {
		s->fault = f;
		s->after = after;
	}
	return s;
}

static WfSftpStream stream_of(Srv *s)
{
	WfSftpStream st = {s, srv_write, srv_read};

	return st;
}

#define P(x) ((const uint8_t *)(x))

static void a_listing_arrives_whole_even_one_byte_at_a_time(void)
{
	for (size_t piece = 0; piece <= 1; piece++) {
		Srv *s = new_srv(F_NONE, 0);
		WfSftpStream st = stream_of(s);
		WfSftpClient *c = NULL;
		WfSftpListing l;
		bool a_lower = false, a_upper = false, esc = false,
		     link = false, dev = false;

		s->piece = piece;
		s->ext = true;
		CHECK(WF_OK == wf_sftpc_open(&st, 1000, &c));
		CHECK(wf_sftpc_has_posix_rename(c) && wf_sftpc_has_fsync(c));
		CHECK(WF_OK == wf_sftpc_list(c, P("/"), 1, 1000, &l));
		CHECK(6 == l.n && !l.truncated);
		for (size_t i = 0; i < l.n; i++) {
			const WfSftpEntry *e = &l.e[i];

			a_lower |=
			        5 == e->name_n && !memcmp(e->name, "a.txt", 5);
			a_upper |=
			        5 == e->name_n && !memcmp(e->name, "A.txt", 5);
			esc |= e->name_n > 3 && 0x1b == e->name[3];
			link |= WF_SFTP_S_IFLNK ==
			        (e->attrs.perm & WF_SFTP_S_IFMT);
			dev |= WF_SFTP_S_IFCHR ==
			        (e->attrs.perm & WF_SFTP_S_IFMT);
		}
		CHECK(a_lower && a_upper); /* both, by case */
		CHECK(esc);                /* bytes arrive as they are */
		CHECK(link && dev);        /* reported as what they are */
		CHECK(1 == s->closed_handles && s->open_handles == 1);
		wf_sftp_listing_free(&l);
		CHECK(!wf_sftpc_broken(c));
		wf_sftpc_close(c);
		free(s);
	}
}

static void a_limit_truncates_and_still_closes_the_handle(void)
{
	Srv *s = new_srv(F_NONE, 0);
	WfSftpStream st = stream_of(s);
	WfSftpClient *c = NULL;
	WfSftpListing l;

	CHECK(WF_OK == wf_sftpc_open(&st, 1000, &c));
	CHECK(WF_OK == wf_sftpc_list(c, P("/"), 1, 3, &l));
	CHECK(3 == l.n && l.truncated && 1 == s->closed_handles);
	wf_sftp_listing_free(&l);
	CHECK(WF_E_INVAL == wf_sftpc_list(c, P("/"), 1, 0, &l));
	CHECK(WF_E_INVAL == wf_sftpc_list(c, P("rel"), 3, 10, &l));
	CHECK(WF_E_INVAL == wf_sftpc_list(c, P("/a/../b"), 7, 10, &l));
	wf_sftpc_close(c);
	free(s);
}

static void a_server_error_is_not_a_broken_session(void)
{
	Srv *s = new_srv(F_NONE, 0);
	WfSftpStream st = stream_of(s);
	WfSftpClient *c = NULL;
	WfSftpListing l;
	WfSftpAttrs a;

	CHECK(WF_OK == wf_sftpc_open(&st, 1000, &c));
	CHECK(WF_E_NOENT == wf_sftpc_list(c, P("/nope"), 5, 10, &l));
	CHECK(WF_SFTP_NO_SUCH_FILE == wf_sftpc_server_status(c));
	CHECK(!wf_sftpc_broken(c));
	CHECK(WF_E_NOENT == wf_sftpc_stat(c, P("/nope"), 5, false, &a));
	CHECK(WF_OK == wf_sftpc_stat(c, P("/a.txt"), 6, false, &a));
	CHECK(a.has_size && 100 == a.size);
	wf_sftpc_close(c);
	free(s);

	s = new_srv(F_PERM_DENIED, 0);
	st = stream_of(s);
	CHECK(WF_OK == wf_sftpc_open(&st, 1000, &c));
	CHECK(WF_E_ACCESS == wf_sftpc_list(c, P("/"), 1, 10, &l));
	CHECK(!wf_sftpc_broken(c));
	wf_sftpc_close(c);
	free(s);
}

static void ranges_are_read_in_pieces_and_only_from_regular_files(void)
{
	Srv *s = new_srv(F_NONE, 0);
	WfSftpStream st = stream_of(s);
	WfSftpClient *c = NULL;
	static uint8_t buf[80000];
	size_t got = 99;

	CHECK(WF_OK == wf_sftpc_open(&st, 1000, &c));
	/* a file bigger than one chunk: the right bytes from the right place */
	CHECK(WF_OK ==
	        wf_sftpc_read_range(c, P("/dir/x"), 6, 1000, 66000, buf, &got));
	CHECK(66000 == got);
	for (size_t i = 0; i < got; i++) {
		if (buf[i] != content(1000 + i)) {
			CHECK_FAIL("wrong byte in a range");
			break;
		}
	}
	/* past the end: what there is, then nothing */
	CHECK(WF_OK ==
	        wf_sftpc_read_range(c, P("/a.txt"), 6, 90, 50, buf, &got));
	CHECK(10 == got);
	CHECK(WF_OK ==
	        wf_sftpc_read_range(c, P("/a.txt"), 6, 500, 50, buf, &got));
	CHECK(0 == got);
	CHECK(WF_OK == wf_sftpc_read_range(c, P("/a.txt"), 6, 0, 0, buf, &got));
	CHECK(0 == got);
	/* a link, a device and a directory are not read */
	CHECK(WF_E_INVAL ==
	        wf_sftpc_read_range(c, P("/link"), 5, 0, 10, buf, &got));
	CHECK(WF_E_INVAL ==
	        wf_sftpc_read_range(c, P("/dev"), 4, 0, 10, buf, &got));
	CHECK(WF_E_INVAL ==
	        wf_sftpc_read_range(c, P("/dir"), 4, 0, 10, buf, &got));
	CHECK(WF_E_NOENT ==
	        wf_sftpc_read_range(c, P("/zzz"), 4, 0, 10, buf, &got));
	CHECK(!wf_sftpc_broken(c));
	/* every handle that was opened was closed */
	CHECK(s->open_handles == s->closed_handles);
	wf_sftpc_close(c);
	free(s);
}

static void realpath_answers_must_be_absolute(void)
{
	Srv *s = new_srv(F_NONE, 0);
	WfSftpStream st = stream_of(s);
	WfSftpClient *c = NULL;
	uint8_t out[64];
	size_t n;

	CHECK(WF_OK == wf_sftpc_open(&st, 1000, &c));
	CHECK(WF_OK == wf_sftpc_realpath(c, P("/.."), 3, out, sizeof out, &n));
	CHECK(1 == n && '/' == out[0]);
	CHECK(WF_E_RANGE == wf_sftpc_realpath(c, P("/.."), 3, out, 1, &n));
	CHECK(!wf_sftpc_broken(c));
	/* a relative answer is a server that cannot be trusted */
	CHECK(WF_E_CORRUPT ==
	        wf_sftpc_realpath(c, P("bad!"), 4, out, sizeof out, &n));
	CHECK(wf_sftpc_broken(c));
	wf_sftpc_close(c);
	free(s);
}

/* STATUS OK where a handle, attributes, names or bytes were asked for is not a
 * success: the call fails and the session is broken, and nothing is asked again
 * for ever. */
static void a_status_ok_without_the_data_is_a_protocol_error(void)
{
	static uint8_t buf[100];
	WfSftpListing l;
	WfSftpAttrs a;
	uint8_t out[64];
	size_t got, n;

	for (int op = 0; op < 4; op++) {
		Srv *s = new_srv(F_NONE, 0);
		WfSftpStream st = stream_of(s);
		WfSftpClient *c = NULL;
		WfStatus r;

		CHECK(WF_OK == wf_sftpc_open(&st, 1000, &c));
		s->fault = F_STATUS_OK;
		memset(&a, 0xAA, sizeof a);
		r = 0 == op       ? wf_sftpc_list(c, P("/"), 1, 100, &l)
		        : 1 == op ? wf_sftpc_stat(c, P("/a.txt"), 6, false, &a)
		        : 2 == op
		        ? wf_sftpc_read_range(
		                  c, P("/a.txt"), 6, 0, 50, buf, &got)
		        : wf_sftpc_realpath(c, P("/"), 1, out, sizeof out, &n);
		CHECK(WF_E_CORRUPT == r);
		CHECK(wf_sftpc_broken(c));
		CHECK(s->requests <= 4); /* it did not go on asking */
		wf_sftpc_close(c);
		free(s);
	}
}

typedef struct
{
	ULONGLONG start, after_ms;
	int calls, stop_at_call; /* stop at this call (0: by the time) */
} Stopper;

static bool stop_when(void *ctx)
{
	Stopper *s = ctx;

	s->calls++;
	if (s->stop_at_call) {
		return s->calls >= s->stop_at_call;
	}
	return GetTickCount64() - s->start >= s->after_ms;
}

/* A request that has been replaced ends a wait for a server that says nothing,
 * long before the packet limit, and the session is not used again. */
static void a_replaced_request_ends_a_wait_for_a_silent_server(void)
{
	Srv *s = new_srv(F_TIMEOUT_AFTER, 1); /* the handshake, then nothing */
	WfSftpStream st = stream_of(s);
	WfSftpClient *c = NULL;
	WfSftpListing l;
	Stopper stop = {0, 250, 0, 0};
	ULONGLONG t0;
	WfStatus r;

	stop.start =
	        GetTickCount64(); /* the clock of the stop runs from here */
	CHECK(WF_OK == wf_sftpc_open_ex(&st, 60000, stop_when, &stop, &c));
	stop.start = GetTickCount64();
	t0 = GetTickCount64();
	r = wf_sftpc_list(c, P("/"), 1, 100, &l);
	CHECK(WF_E_CANCELLED == r);
	CHECK(GetTickCount64() - t0 < 10000); /* not the 60 s of the limit */
	CHECK(wf_sftpc_broken(c));
	wf_sftpc_close(c);
	free(s);
}

/* The question is also asked between packets: a server that answers every time
 * does not keep a replaced request going. And it belongs to one request: the
 * client works again, and unasked, after wf_sftpc_set_cancel. */
static void a_replaced_request_ends_a_listing_between_packets(void)
{
	Srv *s = new_srv(F_NONE, 0);
	WfSftpStream st = stream_of(s);
	WfSftpClient *c = NULL;
	WfSftpListing l;
	Stopper stop = {0, 0, 0, 3};
	WfStatus r;

	CHECK(WF_OK == wf_sftpc_open(&st, 1000, &c));
	wf_sftpc_set_cancel(c, stop_when, &stop);
	r = wf_sftpc_list(c, P("/"), 1, 100, &l);
	CHECK(WF_E_CANCELLED == r && wf_sftpc_broken(c));
	CHECK(s->requests <= 3); /* it did not go on asking */
	wf_sftpc_close(c);
	free(s);

	s = new_srv(F_NONE, 0);
	st = stream_of(s);
	stop.calls = 0;
	stop.stop_at_call = 1000000; /* never */
	CHECK(WF_OK == wf_sftpc_open(&st, 1000, &c));
	wf_sftpc_set_cancel(c, stop_when, &stop);
	CHECK(WF_OK == wf_sftpc_list(c, P("/"), 1, 100, &l));
	CHECK(stop.calls > 0); /* it was asked */
	wf_sftp_listing_free(&l);
	wf_sftpc_set_cancel(c, NULL, NULL);
	stop.calls = 0;
	CHECK(WF_OK == wf_sftpc_list(c, P("/"), 1, 100, &l));
	CHECK(0 == stop.calls); /* and no longer asked */
	wf_sftp_listing_free(&l);
	CHECK(!wf_sftpc_broken(c));
	wf_sftpc_close(c);
	free(s);
}

static void a_misbehaving_server_breaks_the_session_and_nothing_else(void)
{
	static const Fault faults[] = {
	        F_WRONG_ID, F_HUGE_LEN, F_LONG_DATA, F_EMPTY_NAMES};
	WfSftpListing l;
	static uint8_t buf[1000];
	size_t got;

	for (size_t i = 0; i < sizeof faults / sizeof *faults; i++) {
		Srv *s = new_srv(F_NONE, 0);
		WfSftpStream st = stream_of(s);
		WfSftpClient *c = NULL;
		WfStatus r;

		CHECK(WF_OK == wf_sftpc_open(&st, 1000, &c));
		s->fault = faults[i]; /* after a good handshake */
		if (F_LONG_DATA == faults[i]) {
			r = wf_sftpc_read_range(
			        c, P("/a.txt"), 6, 0, 50, buf, &got);
		} else {
			r = wf_sftpc_list(c, P("/"), 1, 100, &l);
		}
		CHECK(WF_OK != r);
		CHECK(wf_sftpc_broken(c));
		/* from now on nothing is sent: calls fail at once */
		{
			int before = s->requests;

			CHECK(WF_OK != wf_sftpc_list(c, P("/"), 1, 100, &l));
			CHECK(before == s->requests);
		}
		wf_sftpc_close(c);
		free(s);
	}
}

static void a_dead_stream_is_reported_not_waited_on(void)
{
	Srv *s;
	WfSftpStream st;
	WfSftpClient *c = NULL;
	WfSftpListing l;

	/* silence: a timeout, and the session is broken */
	s = new_srv(F_TIMEOUT_AFTER, 1);
	st = stream_of(s);
	CHECK(WF_OK == wf_sftpc_open(&st, 1000, &c));
	CHECK(WF_E_TIMEOUT == wf_sftpc_list(c, P("/"), 1, 10, &l));
	CHECK(wf_sftpc_broken(c));
	wf_sftpc_close(c);
	free(s);

	/* the connection ends mid-way */
	s = new_srv(F_EOF_AFTER, 2);
	st = stream_of(s);
	CHECK(WF_OK == wf_sftpc_open(&st, 1000, &c));
	CHECK(WF_OK != wf_sftpc_list(c, P("/"), 1, 10, &l));
	CHECK(wf_sftpc_broken(c));
	wf_sftpc_close(c);
	free(s);

	/* it never says VERSION */
	s = new_srv(F_NO_VERSION, 0);
	st = stream_of(s);
	CHECK(WF_E_CORRUPT == wf_sftpc_open(&st, 1000, &c) && !c);
	free(s);

	/* nothing at all at the handshake */
	s = new_srv(F_EOF_AFTER, 0);
	st = stream_of(s);
	CHECK(WF_OK != wf_sftpc_open(&st, 1000, &c) && !c);
	free(s);
}

/* The wait is for the whole reply: a server that dribbles one byte at a time,
 * each inside the limit, is still too slow overall. */
static void a_trickling_reply_runs_out_of_time(void)
{
	Srv *s = new_srv(F_NONE, 0);
	WfSftpStream st = stream_of(s);
	WfSftpClient *c = NULL;

	s->piece = 1;
	s->slow_ms = 30;
	CHECK(WF_E_TIMEOUT == wf_sftpc_open(&st, 120, &c) && !c);
	free(s);
}

static void bad_arguments_are_refused(void)
{
	WfSftpStream none = {NULL, NULL, NULL};
	WfSftpClient *c = NULL;

	CHECK(WF_E_INVAL == wf_sftpc_open(NULL, 1000, &c));
	CHECK(WF_E_INVAL == wf_sftpc_open(&none, 1000, &c));
	CHECK(wf_sftpc_broken(NULL));
	CHECK(!wf_sftpc_has_fsync(NULL));
	wf_sftpc_close(NULL);
	wf_sftp_listing_free(NULL);
}

int main(void)
{
	a_listing_arrives_whole_even_one_byte_at_a_time();
	a_limit_truncates_and_still_closes_the_handle();
	a_server_error_is_not_a_broken_session();
	ranges_are_read_in_pieces_and_only_from_regular_files();
	realpath_answers_must_be_absolute();
	a_replaced_request_ends_a_wait_for_a_silent_server();
	a_replaced_request_ends_a_listing_between_packets();
	a_misbehaving_server_breaks_the_session_and_nothing_else();
	a_status_ok_without_the_data_is_a_protocol_error();
	a_dead_stream_is_reported_not_waited_on();
	a_trickling_reply_runs_out_of_time();
	bad_arguments_are_refused();
	return TEST_RESULT();
}
