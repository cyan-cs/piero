#include "wf_fake_probe.h"
#include "wf_test.h"

static FakeProbe fs;
static WfProbeOps ops;

static void setup(void)
{
	fp_init(&fs);
	ops = fp_ops(&fs);
	fp_dir(&fs, L"C:\\src", 10);
	fp_dir(&fs, L"C:\\dst", 20);
	fp_file(&fs, L"C:\\src\\a", 10, 11);
	fp_file(&fs, L"C:\\src\\b", 20, 12);
}

static void path_lengths(void)
{
	WfTransferPaths p;
	CHECK(WF_OK == wf_xfer_path_lengths(10, 1, 1, &p) &&
	        12 == p.final_units && 70 == p.transaction_units);
	CHECK(WF_OK == wf_xfer_path_lengths(10, 255, 255, &p) &&
	        266 == p.final_units && 70 == p.transaction_units);
	CHECK(WF_OK == wf_xfer_path_lengths(10, 14, 4, &p) &&
	        25 == p.final_units && 80 == p.transaction_units);
	CHECK(WF_E_RANGE == wf_xfer_path_lengths(SIZE_MAX - 59, 1, 1, &p));
	CHECK(WF_E_RANGE == wf_xfer_path_lengths(10, SIZE_MAX, 1, &p));
	CHECK(WF_E_INVAL == wf_xfer_path_lengths(10, 1, 2, &p));
	CHECK(WF_E_INVAL == wf_xfer_path_lengths(10, 0, 0, &p));
	CHECK(WF_E_INVAL == wf_xfer_path_lengths(10, 1, 1, NULL));
}

static void batch_checks(void)
{
	const wchar_t *names[] = {L"a", L"b"};
	WfTransferSlot slot;
	WfPreflightReport r;
	WfPreflightPolicy policy = WF_PREFLIGHT_DEFAULT;
	WfStatus st;
	setup();
	CHECK(wf_xfer_capture(&ops, WF_XFER_COPY, L"C:\\src", names, 2, 0,
	              &slot) == WF_OK);
	st = wf_preflight_run(&ops, &slot, L"C:\\dst", &policy, NULL, &r);
	CHECK(wf_preflight_can_start(st, &r, 0));
	CHECK(12 == r.longest_final && 70 == r.longest_txn);
	CHECK(!wf_preflight_can_start(WF_E_NOMEM, &r, 1));
	CHECK(!wf_preflight_can_start(WF_OK, NULL, 1));
	r.checks_complete = 0;
	CHECK(!wf_preflight_can_start(WF_OK, &r, 1));
	fp_file(&fs, L"C:\\dst\\b", 99, 50);
	st = wf_preflight_run(&ops, &slot, L"C:\\dst", &policy, NULL, &r);
	CHECK(WF_PF_COLLISION == r.reason && 1 == r.bad_index &&
	        !wf_preflight_can_start(st, &r, 1));
	CHECK(NULL == fp_find(&fs, L"C:\\dst\\a") &&
	        99 == fp_find(&fs, L"C:\\dst\\b")->size);
	fp_find(&fs, L"C:\\dst\\b")->io_error = 1;
	st = wf_preflight_run(&ops, &slot, L"C:\\dst", &policy, NULL, &r);
	CHECK(WF_PF_IO_ERROR == r.reason && !wf_preflight_can_start(st, &r, 1));
	fs.count--; /* remove synthetic collision, no real FS mutation */
	fp_find(&fs, L"C:\\src\\b")->id = 99;
	st = wf_preflight_run(&ops, &slot, L"C:\\dst", &policy, NULL, &r);
	CHECK(WF_PF_SOURCE_CHANGED == r.reason &&
	        !wf_preflight_can_start(st, &r, 1));
	fp_find(&fs, L"C:\\src\\b")->id = 12;
	fs.access_status = WF_E_ACCESS;
	st = wf_preflight_run(&ops, &slot, L"C:\\dst", &policy, NULL, &r);
	CHECK(WF_PF_DEST_ACCESS == r.reason &&
	        !wf_preflight_can_start(st, &r, 1));
	fs.access_status = WF_OK;
	policy.max_path_units = 69;
	st = wf_preflight_run(&ops, &slot, L"C:\\dst", &policy, NULL, &r);
	CHECK(WF_PF_PATH_TOO_LONG == r.reason &&
	        !wf_preflight_can_start(st, &r, 1));
	policy = WF_PREFLIGHT_DEFAULT;
	fp_find(&fs, L"C:\\src\\b")->size = UINT64_MAX - 10;
	wf_slot_free(&slot);
	CHECK(wf_xfer_capture(&ops, WF_XFER_COPY, L"C:\\src", names, 2, 0,
	              &slot) == WF_OK);
	fs.vols[0].info.free_bytes = UINT64_MAX;
	st = wf_preflight_run(&ops, &slot, L"C:\\dst", &policy, NULL, &r);
	CHECK(WF_PF_NO_SPACE == r.reason &&
	        !wf_preflight_can_start(st, &r,
	                1)); /* reserve addition must not saturate to success */
	wf_slot_free(&slot);
	setup();
	CHECK(wf_xfer_capture(&ops, WF_XFER_COPY, L"C:\\src", names, 2, 0,
	              &slot) == WF_OK);
	fp_find(&fs, L"C:\\src\\b")->flags |= WF_ENT_CLOUD;
	st = wf_preflight_run(&ops, &slot, L"C:\\dst", &policy, NULL, &r);
	CHECK(WF_PF_SOURCE_CHANGED == r.reason &&
	        !wf_preflight_can_start(st, &r, 0));
	wf_slot_free(&slot);
	setup();
	fp_find(&fs, L"C:\\src\\b")->flags = WF_ENT_CLOUD;
	CHECK(wf_xfer_capture(&ops, WF_XFER_COPY, L"C:\\src", names, 2, 0,
	              &slot) == WF_OK);
	st = wf_preflight_run(&ops, &slot, L"C:\\dst", &policy, NULL, &r);
	CHECK(1 == r.cloud_files && 20 == r.cloud_bytes);
	CHECK(!wf_preflight_can_start(st, &r, 0) &&
	        wf_preflight_can_start(st, &r, 1));
	CHECK(!wf_preflight_can_start(st, &r, 2));
	wf_slot_free(&slot);
	setup();
	{
		const wchar_t *one[] = {L"cloud-dir"};
		FpNode *node = fp_add(&fs, L"C:\\src\\cloud-dir",
		        WF_ENT_DIR | WF_ENT_LINK | WF_ENT_CLOUD, 0, 80);
		node->tag = 0x9000001a;
		CHECK(wf_xfer_capture(&ops, WF_XFER_COPY, L"C:\\src", one, 1, 0,
		              &slot) == WF_OK);
		st = wf_preflight_run(
		        &ops, &slot, L"C:\\dst", &policy, NULL, &r);
		CHECK(WF_PF_REPARSE == r.reason &&
		        !wf_preflight_can_start(st, &r, 1));
		wf_slot_free(&slot);
	}
	setup();
	{
		wchar_t leaf[256], full[300];
		const wchar_t *one[] = {leaf};
		size_t i;
		for (i = 0; i < 255; i++) {
			leaf[i] = L'漢';
		}
		leaf[255] = 0;
		swprintf(full, 300, L"C:\\src\\%ls", leaf);
		fp_file(&fs, full, 1, 90);
		CHECK(wf_xfer_capture(&ops, WF_XFER_COPY, L"C:\\src", one, 1, 0,
		              &slot) == WF_OK);
		policy.max_path_units = 200;
		st = wf_preflight_run(
		        &ops, &slot, L"C:\\dst", &policy, NULL, &r);
		CHECK(WF_PF_PATH_TOO_LONG == r.reason &&
		        266 == r.longest_final && 70 == r.longest_txn &&
		        !wf_preflight_can_start(st, &r, 1));
		wf_slot_free(&slot);
	}
}

int main(void)
{
	path_lengths();
	batch_checks();
	return TEST_RESULT();
}
