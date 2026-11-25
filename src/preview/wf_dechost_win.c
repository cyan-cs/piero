#include "wf_dechost_win.h"

#include <stdlib.h>
#include <string.h>

#include "wf_decode_win.h"

enum
{
	EXIT_CLEAN = 0,
	EXIT_BAD_PEER = 2, /* the request stream broke the protocol */
	EXIT_IO = 3,       /* the reply could not be written */
	EXIT_NO_MEMORY = 4
};

/* Reads exactly n bytes. Returns the count read: n, or less at end of file or
 * on an error (0 means the stream ended cleanly before the first byte). */
static size_t read_exact(HANDLE h, void *buf, size_t n)
{
	size_t done = 0;

	while (done < n) {
		DWORD got = 0,
		      want = (DWORD)(n - done > (1u << 24) ? (1u << 24)
		                                           : n - done);

		if (!ReadFile(h, (char *)buf + done, want, &got, NULL) ||
		        0 == got) {
			break;
		}
		done += got;
	}
	return done;
}

static int write_all(HANDLE h, const void *buf, size_t n)
{
	size_t done = 0;

	while (done < n) {
		DWORD put = 0,
		      want = (DWORD)(n - done > (1u << 24) ? (1u << 24)
		                                           : n - done);

		if (!WriteFile(h, (const char *)buf + done, want, &put, NULL) ||
		        0 == put) {
			return 0;
		}
		done += put;
	}
	return 1;
}

/*
 * Reads exactly n bytes from the start of the file. The handle is a DUPLICATE
 * of the app's, so it shares the file position with every other copy: a read
 * that relied on the position found it at the end after the first request and
 * saw an empty file. Each read says where it starts instead.
 */
static size_t read_file_at_start(HANDLE h, void *buf, size_t n)
{
	size_t done = 0;

	while (done < n) {
		OVERLAPPED at;
		DWORD got = 0,
		      want = (DWORD)(n - done > (1u << 24) ? (1u << 24)
		                                           : n - done);

		memset(&at, 0, sizeof at);
		at.Offset = (DWORD)(done & 0xFFFFFFFFu);
		at.OffsetHigh = (DWORD)((uint64_t)done >> 32);
		if (!ReadFile(h, (char *)buf + done, want, &got, &at) ||
		        0 == got) {
			break;
		}
		done += got;
	}
	return done;
}

/* The whole of the file behind `h`, which must be exactly `size` bytes. The
 * handle is closed. NULL means the file was not what the request said. */
static unsigned char *take_input(HANDLE h, uint64_t size)
{
	LARGE_INTEGER actual;
	unsigned char *buf = NULL;

	if (INVALID_HANDLE_VALUE == h || NULL == h ||
	        FILE_TYPE_DISK != GetFileType(h) ||
	        !GetFileSizeEx(h, &actual) ||
	        (uint64_t)actual.QuadPart != size) {
		goto done;
	}
	buf = malloc((size_t)size);
	if (buf && (size_t)size != read_file_at_start(h, buf, (size_t)size)) {
		free(buf);
		buf = NULL;
	}
done:
	if (h && INVALID_HANDLE_VALUE != h) {
		CloseHandle(h);
	}
	return buf;
}

static int reply(HANDLE out,
        const WfDecodeRequest *rq,
        WfDecodeStatus status,
        const WfDecodeInfo *info,
        const WfPixmap *pm)
{
	unsigned char hdr[WF_DWIRE_REPLY_HEADER];
	WfDecodeReply r;

	memset(&r, 0, sizeof r);
	r.request = rq->request;
	r.status = (uint32_t)status;
	r.page_count = info->page_count;
	r.src_w = info->src_w;
	r.src_h = info->src_h;
	r.flags = info->flags;
	if (WF_DSTAT_OK == status && pm) {
		r.out_w = pm->w;
		r.out_h = pm->h;
		r.payload = (uint64_t)pm->w * pm->h * 4;
	}
	wf_dwire_reply_encode(&r, hdr);
	if (!write_all(out, hdr, sizeof hdr)) {
		return 0;
	}
	return 0 == r.payload || write_all(out, pm->px, (size_t)r.payload);
}

int wf_dechost_main(HANDLE in, HANDLE out)
{
	for (;;) {
		unsigned char hdr[WF_DWIRE_REQUEST_HEADER];
		uint16_t units[WF_DWIRE_PASSWORD_MAX];
		wchar_t password[WF_DWIRE_PASSWORD_MAX + 1];
		WfDecodeRequest rq;
		WfDecodeInfo info;
		WfPixmap *pm = NULL;
		unsigned char *input;
		WfDecodeStatus st;
		size_t got = read_exact(in, hdr, sizeof hdr);
		int ok;

		if (0 == got) {
			return EXIT_CLEAN;
		}
		if (got != sizeof hdr || !wf_dwire_request_header(hdr, &rq)) {
			return EXIT_BAD_PEER;
		}
		if (rq.password_units &&
		        (size_t)rq.password_units * 2 !=
		                read_exact(in, units,
		                        (size_t)rq.password_units * 2)) {
			return EXIT_BAD_PEER;
		}
		for (uint32_t i = 0; i < rq.password_units; i++) {
			password[i] = (wchar_t)units[i];
		}
		password[rq.password_units] = L'\0';
		SecureZeroMemory(units, sizeof units);

		memset(&info, 0, sizeof info);
		if (rq.flags & WF_DREQ_INLINE) {
			/* the bytes follow on this pipe; whatever is short leaves the
			 * stream in the middle of a message, so the peer is gone */
			input = malloc((size_t)rq.input_size);
			if (!input) {
				return EXIT_NO_MEMORY;
			}
			if ((size_t)rq.input_size !=
			        read_exact(in, input, (size_t)rq.input_size)) {
				free(input);
				return EXIT_BAD_PEER;
			}
		} else {
			input = take_input((HANDLE)(uintptr_t)rq.input_handle,
			        rq.input_size);
		}
		if (!input) {
			st = WF_DSTAT_CORRUPT; /* not the file the app described */
		} else if (WF_DKIND_PDF == rq.kind) {
			st = wf_decode_pdf(input, (size_t)rq.input_size,
			        rq.page, rq.box_w, rq.box_h, password, &pm,
			        &info);
		} else {
			st = wf_decode_image(input, (size_t)rq.input_size,
			        rq.box_w, rq.box_h, &pm, &info);
		}
		SecureZeroMemory(password, sizeof password);
		free(input);
		ok = reply(out, &rq, st, &info, pm);
		if (pm && rq.kind == WF_DKIND_PDF) {
			SecureZeroMemory(pm->px, wf_pixmap_bytes(pm->w, pm->h));
		}
		wf_pixmap_free(pm);
		if (!ok) {
			return EXIT_IO;
		}
	}
}

static HANDLE hex_handle(const wchar_t *s)
{
	return (HANDLE)(uintptr_t)_wcstoui64(s, NULL, 16);
}

int wf_dechost_from_args(int argc, wchar_t **argv, int *code)
{
	if (argc < 4 || 0 != wcscmp(argv[1], WF_DECHOST_ARG)) {
		return 0;
	}
	*code = wf_dechost_main(hex_handle(argv[2]), hex_handle(argv[3]));
	return 1;
}
