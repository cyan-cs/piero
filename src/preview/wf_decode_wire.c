#include "wf_decode_wire.h"

#include <string.h>

static void put32(unsigned char *b, uint32_t v)
{
	b[0] = (unsigned char)v;
	b[1] = (unsigned char)(v >> 8);
	b[2] = (unsigned char)(v >> 16);
	b[3] = (unsigned char)(v >> 24);
}

static void put64(unsigned char *b, uint64_t v)
{
	put32(b, (uint32_t)v);
	put32(b + 4, (uint32_t)(v >> 32));
}

static uint32_t get32(const unsigned char *b)
{
	return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 |
	        (uint32_t)b[3] << 24;
}

static uint64_t get64(const unsigned char *b)
{
	return (uint64_t)get32(b) | (uint64_t)get32(b + 4) << 32;
}

static bool request_ok(const WfDecodeRequest *r)
{
	if (WF_DKIND_IMAGE != r->kind && WF_DKIND_PDF != r->kind) {
		return false;
	}
	if (0 == r->box_w || 0 == r->box_h || r->box_w > WF_DWIRE_BOX_MAX ||
	        r->box_h > WF_DWIRE_BOX_MAX ||
	        (uint64_t)r->box_w * r->box_h * 4 > WF_DWIRE_RASTER_MAX) {
		return false;
	}
	if (0 == r->input_size || r->input_size > WF_DWIRE_INPUT_MAX) {
		return false;
	}
	if ((r->flags & ~(uint32_t)WF_DREQ_INLINE) ||
	        ((r->flags & WF_DREQ_INLINE) && 0 != r->input_handle)) {
		return false;
	}
	if (r->password_units > WF_DWIRE_PASSWORD_MAX ||
	        (r->password_units && WF_DKIND_PDF != r->kind)) {
		return false;
	}
	return true;
}

size_t wf_dwire_request_size(const WfDecodeRequest *r)
{
	if (!r || !request_ok(r)) {
		return 0;
	}
	return WF_DWIRE_REQUEST_HEADER + (size_t)r->password_units * 2;
}

size_t wf_dwire_request_encode(const WfDecodeRequest *r,
        const uint16_t *password,
        void *out,
        size_t cap)
{
	unsigned char *b = out;
	size_t n = wf_dwire_request_size(r);

	if (0 == n || !out || cap < n || (r->password_units && !password)) {
		return 0;
	}
	memset(b, 0, WF_DWIRE_REQUEST_HEADER);
	memcpy(b, "WFRQ", 4);
	put32(b + 4, WF_DWIRE_VERSION);
	put64(b + 8, r->request);
	put32(b + 16, r->kind);
	put32(b + 20, r->box_w);
	put32(b + 24, r->box_h);
	put32(b + 28, r->page);
	put64(b + 32, r->input_size);
	put32(b + 40, r->password_units);
	put32(b + 44, r->flags);
	put64(b + 48, r->input_handle);
	for (uint32_t i = 0; i < r->password_units; i++) {
		b[WF_DWIRE_REQUEST_HEADER + 2 * (size_t)i] =
		        (unsigned char)password[i];
		b[WF_DWIRE_REQUEST_HEADER + 2 * (size_t)i + 1] =
		        (unsigned char)(password[i] >> 8);
	}
	return n;
}

bool wf_dwire_request_header(const void *hdr, WfDecodeRequest *r)
{
	const unsigned char *b = hdr;
	WfDecodeRequest t;

	if (!b || !r || memcmp(b, "WFRQ", 4) ||
	        WF_DWIRE_VERSION != get32(b + 4)) {
		return false;
	}
	t.request = get64(b + 8);
	t.kind = get32(b + 16);
	t.box_w = get32(b + 20);
	t.box_h = get32(b + 24);
	t.page = get32(b + 28);
	t.input_size = get64(b + 32);
	t.password_units = get32(b + 40);
	t.flags = get32(b + 44);
	t.input_handle = get64(b + 48);
	if (!request_ok(&t)) {
		return false;
	}
	*r = t;
	return true;
}

void wf_dwire_reply_encode(const WfDecodeReply *r, void *out56)
{
	unsigned char *b = out56;

	memset(b, 0, WF_DWIRE_REPLY_HEADER);
	memcpy(b, "WFRS", 4);
	put32(b + 4, WF_DWIRE_VERSION);
	put64(b + 8, r->request);
	put32(b + 16, r->status);
	put32(b + 20, r->page_count);
	put32(b + 24, r->src_w);
	put32(b + 28, r->src_h);
	put32(b + 32, r->out_w);
	put32(b + 36, r->out_h);
	put32(b + 40, r->flags);
	put64(b + 48, r->payload);
}

bool wf_dwire_reply_header(
        const void *hdr, const WfDecodeRequest *asked, WfDecodeReply *r)
{
	const unsigned char *b = hdr;
	WfDecodeReply t;

	if (!b || !asked || !r || memcmp(b, "WFRS", 4) ||
	        WF_DWIRE_VERSION != get32(b + 4)) {
		return false;
	}
	t.request = get64(b + 8);
	t.status = get32(b + 16);
	t.page_count = get32(b + 20);
	t.src_w = get32(b + 24);
	t.src_h = get32(b + 28);
	t.out_w = get32(b + 32);
	t.out_h = get32(b + 36);
	t.flags = get32(b + 40);
	t.payload = get64(b + 48);
	if (t.request != asked->request || 0 != get32(b + 44) ||
	        t.status >= WF_DSTAT_COUNT || WF_DSTAT_HOST_LOST == t.status ||
	        WF_DSTAT_ABORTED == t.status ||
	        (t.flags & ~(uint32_t)WF_DFLAG_ALPHA)) {
		return false;
	}
	if (WF_DSTAT_OK != t.status) {
		/* a refusal carries no picture and claims no size */
		if (0 != t.payload || 0 != t.out_w || 0 != t.out_h) {
			return false;
		}
		*r = t;
		return true;
	}
	if (0 == t.out_w || 0 == t.out_h || t.out_w > asked->box_w ||
	        t.out_h > asked->box_h || 0 == t.page_count ||
	        t.page_count > 1000000u ||
	        t.payload != (uint64_t)t.out_w * t.out_h * 4 ||
	        t.payload > WF_DWIRE_RASTER_MAX) {
		return false;
	}
	if (WF_DKIND_IMAGE == asked->kind && 1 != t.page_count) {
		return false;
	}
	if (WF_DKIND_PDF == asked->kind && asked->page >= t.page_count) {
		return false;
	}
	*r = t;
	return true;
}
