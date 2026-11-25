/* libFuzzer harness: the headers exchanged with the decoder host. Whatever
 * is accepted must obey every limit the receiver relies on. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "preview/wf_decode_wire.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	unsigned char req[WF_DWIRE_REQUEST_HEADER], rep[WF_DWIRE_REPLY_HEADER];
	WfDecodeRequest q;
	WfDecodeReply p;

	if (size < sizeof req) {
		return 0;
	}
	memcpy(req, data, sizeof req);
	if (wf_dwire_request_header(req, &q)) {
		if (0 == wf_dwire_request_size(&q) ||
		        q.box_w > WF_DWIRE_BOX_MAX ||
		        q.box_h > WF_DWIRE_BOX_MAX ||
		        (uint64_t)q.box_w * q.box_h * 4 > WF_DWIRE_RASTER_MAX ||
		        q.input_size > WF_DWIRE_INPUT_MAX ||
		        q.password_units > WF_DWIRE_PASSWORD_MAX) {
			abort();
		}
		if (size >= sizeof req + sizeof rep) {
			memcpy(rep, data + sizeof req, sizeof rep);
			if (wf_dwire_reply_header(rep, &q, &p) &&
			        WF_DSTAT_OK == p.status &&
			        (p.payload != (uint64_t)p.out_w * p.out_h * 4 ||
			                p.payload > WF_DWIRE_RASTER_MAX ||
			                p.out_w > q.box_w ||
			                p.out_h > q.box_h)) {
				abort();
			}
		}
	}
	return 0;
}
