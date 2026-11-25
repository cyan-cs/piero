#include <string.h>

#include "base/wf_macro.h"
#include "preview/wf_decode_wire.h"
#include "wf_test.h"

static WfDecodeRequest request(void)
{
	WfDecodeRequest r = {0};

	r.request = 0x1122334455667788ull;
	r.kind = WF_DKIND_PDF;
	r.box_w = 800;
	r.box_h = 600;
	r.page = 3;
	r.input_size = 123456;
	r.password_units = 3;
	r.input_handle = 0x1234;
	return r;
}

static WfDecodeReply good_reply(const WfDecodeRequest *q)
{
	WfDecodeReply p = {0};

	p.request = q->request;
	p.status = WF_DSTAT_OK;
	p.page_count = 10;
	p.src_w = 816;
	p.src_h = 1056;
	p.out_w = 464;
	p.out_h = 600;
	p.flags = WF_DFLAG_ALPHA;
	p.payload = (uint64_t)p.out_w * p.out_h * 4;
	return p;
}

static void a_request_round_trips(void)
{
	static const uint16_t pw[3] = {'s', 0x3042, 'x'};
	WfDecodeRequest r = request(), got;
	unsigned char buf[WF_DWIRE_REQUEST_HEADER + 6];
	size_t n = wf_dwire_request_encode(&r, pw, buf, sizeof buf);

	CHECK(WF_DWIRE_REQUEST_HEADER + 6 == n &&
	        56 == WF_DWIRE_REQUEST_HEADER);
	CHECK(n == wf_dwire_request_size(&r));
	CHECK(0 == memcmp(buf, "WFRQ", 4));
	CHECK(wf_dwire_request_header(buf, &got));
	CHECK(0x1234 == got.input_handle);
	CHECK(got.request == r.request && got.kind == r.kind &&
	        got.box_w == 800 && got.box_h == 600 && 3 == got.page &&
	        123456 == got.input_size && 3 == got.password_units);
	/* the password is UTF-16LE right after the header */
	CHECK(0x42 == buf[WF_DWIRE_REQUEST_HEADER + 2] &&
	        0x30 == buf[WF_DWIRE_REQUEST_HEADER + 3]);
	/* a short buffer is refused, not overrun */
	CHECK(0 == wf_dwire_request_encode(&r, pw, buf, n - 1));
	CHECK(0 == wf_dwire_request_encode(&r, NULL, buf, sizeof buf));
	CHECK(0 == wf_dwire_request_encode(&r, pw, NULL, sizeof buf));
}

static void invalid_requests_are_not_encodable_or_accepted(void)
{
	unsigned char buf[WF_DWIRE_REQUEST_HEADER + 2 * WF_DWIRE_PASSWORD_MAX];
	WfDecodeRequest r, got;
	uint16_t pw[WF_DWIRE_PASSWORD_MAX + 1] = {0};

	r = request();
	r.kind = 9;
	CHECK(0 == wf_dwire_request_size(&r));
	r = request();
	r.box_w = 0;
	CHECK(0 == wf_dwire_request_size(&r));
	r = request();
	r.box_w = WF_DWIRE_BOX_MAX + 1;
	CHECK(0 == wf_dwire_request_size(&r));
	r = request();
	r.box_w = 8192;
	r.box_h = 8192; /* 256 MiB raster: over the reply limit */
	CHECK(0 == wf_dwire_request_size(&r));
	r = request();
	r.input_size = 0;
	CHECK(0 == wf_dwire_request_size(&r));
	r = request();
	r.input_size = WF_DWIRE_INPUT_MAX + 1;
	CHECK(0 == wf_dwire_request_size(&r));
	r = request();
	r.password_units = WF_DWIRE_PASSWORD_MAX + 1;
	CHECK(0 == wf_dwire_request_encode(&r, pw, buf, sizeof buf));
	r = request();
	r.kind = WF_DKIND_IMAGE; /* only a PDF takes a password */
	CHECK(0 == wf_dwire_request_size(&r));
	CHECK(0 == wf_dwire_request_size(NULL));
	/* the largest valid one fits exactly */
	r = request();
	r.password_units = WF_DWIRE_PASSWORD_MAX;
	CHECK(sizeof buf == wf_dwire_request_encode(&r, pw, buf, sizeof buf));
	CHECK(wf_dwire_request_header(buf, &got) &&
	        WF_DWIRE_PASSWORD_MAX == got.password_units);
}

static void a_damaged_request_header_is_refused(void)
{
	static const uint16_t pw[3] = {1, 2, 3};
	WfDecodeRequest r = request(), got;
	unsigned char good[WF_DWIRE_REQUEST_HEADER + 6], b[sizeof good];

	CHECK(sizeof good ==
	        wf_dwire_request_encode(&r, pw, good, sizeof good));
	memcpy(b, good, sizeof b);
	b[0] = 'X';
	CHECK(!wf_dwire_request_header(b, &got));
	memcpy(b, good, sizeof b);
	b[4] = 2; /* version */
	CHECK(!wf_dwire_request_header(b, &got));
	memcpy(b, good, sizeof b);
	b[44] = 2; /* a flag nobody defined */
	CHECK(!wf_dwire_request_header(b, &got));
	b[44] = 1; /* INLINE with a handle named as well: contradictory */
	CHECK(!wf_dwire_request_header(b, &got));
	CHECK(!wf_dwire_request_header(NULL, &got));
	CHECK(!wf_dwire_request_header(good, NULL));
	/* every single-bit flip of the header is either refused or still a
	 * request that passes the same rules the encoder applies */
	for (size_t bit = 0; bit < WF_DWIRE_REQUEST_HEADER * 8; bit++) {
		memcpy(b, good, sizeof b);
		b[bit / 8] ^= (unsigned char)(1u << (bit % 8));
		if (wf_dwire_request_header(b, &got)) {
			CHECK(0 != wf_dwire_request_size(&got));
		}
	}
}

static void an_inline_request_carries_no_handle(void)
{
	WfDecodeRequest r = request(), got;
	unsigned char buf[WF_DWIRE_REQUEST_HEADER + 6];
	static const uint16_t pw[3] = {1, 2, 3};

	r.flags = WF_DREQ_INLINE;
	CHECK(0 == wf_dwire_request_size(&r)); /* the handle is still set */
	r.input_handle = 0;
	CHECK(sizeof buf == wf_dwire_request_encode(&r, pw, buf, sizeof buf));
	CHECK(wf_dwire_request_header(buf, &got) &&
	        WF_DREQ_INLINE == got.flags && 0 == got.input_handle);
}

static void a_good_reply_is_accepted(void)
{
	WfDecodeRequest q = request();
	WfDecodeReply p = good_reply(&q), got;
	unsigned char hdr[WF_DWIRE_REPLY_HEADER];

	wf_dwire_reply_encode(&p, hdr);
	CHECK(0 == memcmp(hdr, "WFRS", 4));
	CHECK(wf_dwire_reply_header(hdr, &q, &got));
	CHECK(got.request == q.request && 464 == got.out_w &&
	        600 == got.out_h && 10 == got.page_count && 816 == got.src_w &&
	        1056 == got.src_h && (WF_DFLAG_ALPHA & got.flags) &&
	        464u * 600 * 4 == got.payload);
}

static void a_reply_that_does_not_answer_the_request_is_refused(void)
{
	WfDecodeRequest q = request();
	WfDecodeReply p;
	WfDecodeReply got;
	unsigned char hdr[WF_DWIRE_REPLY_HEADER];

#define TRY(field, value)                                                      \
	do {                                                                   \
		p = good_reply(&q);                                            \
		p.field = (value);                                             \
		wf_dwire_reply_encode(&p, hdr);                                \
		CHECK(!wf_dwire_reply_header(hdr, &q, &got));                  \
	} while (0)
	TRY(request, q.request + 1); /* somebody else's answer */
	TRY(out_w, 801);             /* wider than the box ... */
	TRY(out_h, 601);             /* ... or taller */
	TRY(out_w, 0);
	TRY(payload, 1); /* size does not match the raster */
	TRY(payload, p.payload + 4);
	TRY(page_count, 0);
	TRY(page_count,
	        3); /* the asked page is 3 (the 4th): 3 pages has none */
	TRY(status, WF_DSTAT_COUNT);     /* unknown status */
	TRY(status, WF_DSTAT_HOST_LOST); /* only the app makes that up */
	TRY(status, WF_DSTAT_ABORTED);   /* ... and that */
	TRY(flags, 2);                   /* unknown flag */
#undef TRY
	/* ... while 4 pages do have it */
	p = good_reply(&q);
	p.page_count = 4;
	wf_dwire_reply_encode(&p, hdr);
	CHECK(wf_dwire_reply_header(hdr, &q, &got));
	/* reserved bytes must stay zero */
	p = good_reply(&q);
	wf_dwire_reply_encode(&p, hdr);
	hdr[44] = 1;
	CHECK(!wf_dwire_reply_header(hdr, &q, &got));
	/* an image has exactly one page */
	q.kind = WF_DKIND_IMAGE;
	q.password_units = 0;
	q.page = 0;
	p = good_reply(&q);
	p.page_count = 2;
	wf_dwire_reply_encode(&p, hdr);
	CHECK(!wf_dwire_reply_header(hdr, &q, &got));
	p.page_count = 1;
	wf_dwire_reply_encode(&p, hdr);
	CHECK(wf_dwire_reply_header(hdr, &q, &got));
}

static void a_refusal_carries_no_picture(void)
{
	WfDecodeRequest q = request();
	WfDecodeReply p = {0}, got;
	unsigned char hdr[WF_DWIRE_REPLY_HEADER];
	static const uint32_t statuses[] = {WF_DSTAT_UNSUPPORTED,
	        WF_DSTAT_CORRUPT, WF_DSTAT_PASSWORD_REQUIRED,
	        WF_DSTAT_PASSWORD_WRONG, WF_DSTAT_TOO_LARGE,
	        WF_DSTAT_PAGE_RANGE, WF_DSTAT_TIMEOUT, WF_DSTAT_INTERNAL};

	for (size_t i = 0; i < WF_ARRAY_LEN(statuses); i++) {
		p.request = q.request;
		p.status = statuses[i];
		p.page_count =
		        7; /* a PDF may still say how many pages it has */
		wf_dwire_reply_encode(&p, hdr);
		CHECK(wf_dwire_reply_header(hdr, &q, &got));
		CHECK(statuses[i] == got.status && 7 == got.page_count);
		/* ... but never a raster */
		p.payload = 16;
		wf_dwire_reply_encode(&p, hdr);
		CHECK(!wf_dwire_reply_header(hdr, &q, &got));
		p.payload = 0;
		p.out_w = 1;
		wf_dwire_reply_encode(&p, hdr);
		CHECK(!wf_dwire_reply_header(hdr, &q, &got));
		p.out_w = 0;
	}
}

static void no_bit_flip_of_a_reply_gets_past_the_rules(void)
{
	WfDecodeRequest q = request();
	WfDecodeReply p = good_reply(&q), got;
	unsigned char hdr[WF_DWIRE_REPLY_HEADER], b[WF_DWIRE_REPLY_HEADER];

	wf_dwire_reply_encode(&p, hdr);
	for (size_t bit = 0; bit < WF_DWIRE_REPLY_HEADER * 8; bit++) {
		memcpy(b, hdr, sizeof b);
		b[bit / 8] ^= (unsigned char)(1u << (bit % 8));
		if (wf_dwire_reply_header(b, &q, &got)) {
			/* whatever is accepted obeys every limit */
			CHECK(got.request == q.request &&
			        got.status < WF_DSTAT_COUNT);
			if (WF_DSTAT_OK == got.status) {
				CHECK(got.out_w >= 1 && got.out_w <= q.box_w &&
				        got.out_h >= 1 &&
				        got.out_h <= q.box_h &&
				        got.payload ==
				                (uint64_t)got.out_w *
				                        got.out_h * 4 &&
				        got.payload <= WF_DWIRE_RASTER_MAX);
			} else {
				CHECK(0 == got.payload && 0 == got.out_w);
			}
		}
	}
}

int main(void)
{
	a_request_round_trips();
	invalid_requests_are_not_encodable_or_accepted();
	a_damaged_request_header_is_refused();
	an_inline_request_carries_no_handle();
	a_good_reply_is_accepted();
	a_reply_that_does_not_answer_the_request_is_refused();
	a_refusal_carries_no_picture();
	no_bit_flip_of_a_reply_gets_past_the_rules();
	return TEST_RESULT();
}
