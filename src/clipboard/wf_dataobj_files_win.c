#define COBJMACROS
#include "wf_dataobj_files_win.h"

#include <ole2.h>
#include <shlobj.h>
#include <stdlib.h>
#include <string.h>

#include "wf_clipboard.h"

typedef struct
{
	IDataObject iface;
	LONG refs;
	void *hdrop; /* the CF_HDROP block, kept as made */
	size_t hdrop_bytes;
	CLIPFORMAT cf_effect;
} Files;

static volatile LONG g_live;

LONG wf_dataobj_files_live(void)
{
	return g_live;
}

static Files *self(IDataObject *p)
{
	return (Files *)p; /* iface is the first member */
}

static HRESULT STDMETHODCALLTYPE query_interface(
        IDataObject *p, REFIID iid, void **out)
{
	if (!out) {
		return E_POINTER;
	}
	if (IsEqualIID(iid, &IID_IUnknown) ||
	        IsEqualIID(iid, &IID_IDataObject)) {
		*out = p;
		IDataObject_AddRef(p);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE add_ref(IDataObject *p)
{
	return (ULONG)InterlockedIncrement(&self(p)->refs);
}

static ULONG STDMETHODCALLTYPE release(IDataObject *p)
{
	Files *f = self(p);
	LONG n = InterlockedDecrement(&f->refs);

	if (0 == n) {
		free(f->hdrop);
		free(f);
		InterlockedDecrement(&g_live);
	}
	return (ULONG)n;
}

/* The formats on offer, as FORMATETCs. */
static void formats(const Files *f, FORMATETC out[2])
{
	FORMATETC base = {0, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};

	out[0] = base;
	out[0].cfFormat = CF_HDROP;
	out[1] = base;
	out[1].cfFormat = f->cf_effect;
}

/* S_OK: asked for something that is offered; else the HRESULT to give. */
static HRESULT check(const Files *f, const FORMATETC *want)
{
	if (!want) {
		return E_INVALIDARG;
	}
	if (want->cfFormat != CF_HDROP && want->cfFormat != f->cf_effect) {
		return DV_E_FORMATETC;
	}
	if (!(want->dwAspect & DVASPECT_CONTENT)) {
		return DV_E_DVASPECT;
	}
	if (want->lindex != -1) {
		return DV_E_LINDEX;
	}
	if (!(want->tymed & TYMED_HGLOBAL)) {
		return DV_E_TYMED;
	}
	return S_OK;
}

/* A new HGLOBAL holding `bytes` of `data`; the receiver frees it. */
static HGLOBAL global_copy(const void *data, size_t bytes)
{
	HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
	void *dst;

	if (!h) {
		return NULL;
	}
	dst = GlobalLock(h);
	if (!dst) {
		GlobalFree(h);
		return NULL;
	}
	memcpy(dst, data, bytes);
	GlobalUnlock(h);
	return h;
}

static HRESULT STDMETHODCALLTYPE get_data(
        IDataObject *p, FORMATETC *want, STGMEDIUM *out)
{
	Files *f = self(p);
	HRESULT hr = check(f, want);
	HGLOBAL h;

	if (!out) {
		return E_POINTER;
	}
	memset(out, 0, sizeof *out);
	if (S_OK != hr) {
		return hr;
	}
	if (CF_HDROP == want->cfFormat) {
		h = global_copy(f->hdrop, f->hdrop_bytes);
	} else {
		DWORD copy =
		        DROPEFFECT_COPY; /* the one thing this object offers */

		h = global_copy(&copy, sizeof copy);
	}
	if (!h) {
		return STG_E_MEDIUMFULL;
	}
	out->tymed = TYMED_HGLOBAL;
	out->hGlobal = h;
	out->pUnkForRelease = NULL;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE get_data_here(
        IDataObject *p, FORMATETC *want, STGMEDIUM *out)
{
	(void)p;
	(void)want;
	(void)out;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE query_get_data(IDataObject *p, FORMATETC *want)
{
	return check(self(p), want);
}

static HRESULT STDMETHODCALLTYPE get_canonical(
        IDataObject *p, FORMATETC *in, FORMATETC *out)
{
	(void)p;
	if (!in || !out) {
		return E_INVALIDARG;
	}
	*out = *in;
	out->ptd = NULL;
	return DATA_S_SAMEFORMATETC;
}

/* A receiver writes "Performed DropEffect" and "Paste Succeeded" back after it has
 * moved or copied. Nothing is done on that word: it is not accepted, and it is not
 * a reason to touch a file (DR-DRAFT-021). */
static HRESULT STDMETHODCALLTYPE set_data(
        IDataObject *p, FORMATETC *f, STGMEDIUM *m, BOOL release_it)
{
	(void)p;
	(void)f;
	if (release_it && m) {
		ReleaseStgMedium(m); /* ownership was given: it is not kept */
	}
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE enum_format(
        IDataObject *p, DWORD direction, IEnumFORMATETC **out)
{
	FORMATETC list[2];

	if (!out) {
		return E_POINTER;
	}
	*out = NULL;
	if (DATADIR_GET != direction) {
		return E_NOTIMPL;
	}
	formats(self(p), list);
	return SHCreateStdEnumFmtEtc(2, list, out);
}

static HRESULT STDMETHODCALLTYPE advise(IDataObject *p,
        FORMATETC *f,
        DWORD flags,
        IAdviseSink *sink,
        DWORD *conn)
{
	(void)p;
	(void)f;
	(void)flags;
	(void)sink;
	if (conn) {
		*conn = 0;
	}
	return OLE_E_ADVISENOTSUPPORTED;
}

static HRESULT STDMETHODCALLTYPE unadvise(IDataObject *p, DWORD conn)
{
	(void)p;
	(void)conn;
	return OLE_E_ADVISENOTSUPPORTED;
}

static HRESULT STDMETHODCALLTYPE enum_advise(
        IDataObject *p, IEnumSTATDATA **out)
{
	(void)p;
	if (out) {
		*out = NULL;
	}
	return OLE_E_ADVISENOTSUPPORTED;
}

static IDataObjectVtbl vtbl = {query_interface, add_ref, release, get_data,
        get_data_here, query_get_data, get_canonical, set_data, enum_format,
        advise, unadvise, enum_advise};

WfStatus wf_dataobj_files_create(
        const wchar_t *const *path, size_t n, IDataObject **out)
{
	Files *f;
	WfStatus st;

	*out = NULL;
	f = calloc(1, sizeof *f);
	if (!f) {
		return WF_E_NOMEM;
	}
	st = wf_clip_build_hdrop(path, n, &f->hdrop, &f->hdrop_bytes);
	if (WF_OK != st) {
		free(f);
		return st;
	}
	f->iface.lpVtbl = &vtbl;
	f->refs = 1;
	f->cf_effect =
	        (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
	InterlockedIncrement(&g_live);
	*out = &f->iface;
	return WF_OK;
}
