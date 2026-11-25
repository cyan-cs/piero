#include "wf_lock.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* Win32 codes, kept numeric: this file does not include <windows.h>. */
enum
{
	ERR_ACCESS_DENIED = 5,
	ERR_SHARING_VIOLATION = 32,
	ERR_LOCK_VIOLATION = 33,
	ERR_USER_MAPPED_FILE = 1224
};

WfLockClass wf_lock_classify(unsigned long e)
{
	if (ERR_SHARING_VIOLATION == e || ERR_LOCK_VIOLATION == e ||
	        ERR_USER_MAPPED_FILE == e) {
		return WF_LOCK_IN_USE;
	}
	if (ERR_ACCESS_DENIED == e) {
		return WF_LOCK_MAYBE;
	}
	return WF_LOCK_NOT;
}

static const wchar_t *const CORE_IMAGES[] = {L"csrss.exe", L"wininit.exe",
        L"winlogon.exe", L"services.exe", L"lsass.exe", L"lsaiso.exe",
        L"smss.exe", L"svchost.exe", L"dwm.exe", L"fontdrvhost.exe",
        L"sihost.exe", L"taskhostw.exe", L"spoolsv.exe", L"audiodg.exe",
        L"runtimebroker.exe", L"wudfhost.exe", L"explorer.exe", L"conhost.exe",
        L"ctfmon.exe", L"searchindexer.exe", L"registry", L"memory compression",
        L"system"};

bool wf_lock_core_image_name(const wchar_t *leaf)
{
	if (!leaf) {
		return false;
	}
	for (size_t i = 0; i < sizeof CORE_IMAGES / sizeof CORE_IMAGES[0];
	        i++) {
		if (0 == _wcsicmp(leaf, CORE_IMAGES[i])) {
			return true;
		}
	}
	return false;
}

WfLockDeny wf_lock_decide(const WfProcFacts *f, bool ending)
{
	if (!f) {
		return WF_DENY_UNKNOWN;
	}
	if (f->self_pid && f->pid == f->self_pid) {
		return WF_DENY_SELF;
	}
	if (f->ancestor) {
		return WF_DENY_ANCESTOR;
	}
	if (!f->identity_ok) {
		return WF_DENY_IDENTITY;
	}
	if (!f->facts_ok) {
		return WF_DENY_UNKNOWN;
	}
	if (f->pid <= 4 || 0 == f->session) {
		return WF_DENY_SYSTEM;
	}
	if (f->critical) {
		return WF_DENY_CRITICAL;
	}
	if (f->is_protected) {
		return WF_DENY_PROTECTED;
	}
	if (WF_LOCKAPP_SERVICE == f->type || WF_LOCKAPP_EXPLORER == f->type ||
	        WF_LOCKAPP_CRITICAL == f->type) {
		return WF_DENY_KIND;
	}
	if (!f->user_known) {
		return WF_DENY_UNKNOWN;
	}
	if (!f->same_user) {
		return WF_DENY_OTHER_USER;
	}
	if (f->system_image) {
		return WF_DENY_SYSTEM_IMAGE;
	}
	if (ending && !f->can_terminate) {
		return WF_DENY_NO_ACCESS;
	}
	if (!ending && !f->has_window) {
		return WF_DENY_NO_WINDOW;
	}
	return WF_DENY_NONE;
}

void wf_locklist_free(WfLockList *l)
{
	if (!l) {
		return;
	}
	free(l->h);
	memset(l, 0, sizeof *l);
}

bool wf_lock_same_process(const WfLockHolder *a, const WfLockHolder *b)
{
	return a && b && a->pid && a->pid == b->pid && a->start &&
	        a->start == b->start;
}

void wf_lockanswer_free(WfLockAnswer *a)
{
	if (!a) {
		return;
	}
	wf_locklist_free(&a->list);
}
