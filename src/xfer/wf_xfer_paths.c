#include "wf_xfer.h"

WfStatus wf_xfer_path_lengths(size_t dest_units,
        size_t relative_units,
        size_t top_name_units,
        WfTransferPaths *out)
{
	size_t fixed = 1 + WF_TXN_NAME_UNITS + 1 +
	        (sizeof WF_TXN_PAYLOAD_NAME / sizeof(wchar_t) - 1);
	size_t tail;
	if (!out) {
		return WF_E_INVAL;
	}
	out->final_units = out->transaction_units = 0;
	if (!top_name_units || relative_units < top_name_units) {
		return WF_E_INVAL;
	}
	tail = relative_units - top_name_units;
	if (dest_units > SIZE_MAX - fixed ||
	        tail > SIZE_MAX - dest_units - fixed ||
	        SIZE_MAX == dest_units ||
	        relative_units > SIZE_MAX - dest_units - 1) {
		return WF_E_RANGE;
	}
	out->final_units = dest_units + 1 + relative_units;
	out->transaction_units = dest_units + fixed + tail;
	return WF_OK;
}

bool wf_preflight_can_start(
        WfStatus status, const WfPreflightReport *report, int hydration_allowed)
{
	return WF_OK == status && report && report->checks_complete &&
	        WF_PF_OK == report->reason &&
	        (!report->cloud_files || 1 == hydration_allowed);
}
