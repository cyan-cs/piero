#include "wf_status.h"

const char *wf_status_name(WfStatus s)
{
	switch (s) {
	case WF_OK:          return "OK";
	case WF_E_NOMEM:     return "NOMEM";
	case WF_E_INVAL:     return "INVAL";
	case WF_E_RANGE:     return "RANGE";
	case WF_E_ACCESS:    return "ACCESS";
	case WF_E_NOENT:     return "NOENT";
	case WF_E_OS:        return "OS";
	case WF_E_TIMEOUT:   return "TIMEOUT";
	case WF_E_FORMAT:    return "FORMAT";
	case WF_E_CORRUPT:   return "CORRUPT";
	case WF_E_ENCRYPTED: return "ENCRYPTED";
	case WF_E_CHANGED:   return "CHANGED";
	case WF_E_CANCELLED: return "CANCELLED";
	}
	return "UNKNOWN";
}
