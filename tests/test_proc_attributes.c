#include <windows.h>

#include <stdbool.h>
#include <stdlib.h>

#include "wf_test.h"

typedef enum
{
	FAIL_ALLOC,
	FAIL_INIT,
	FAIL_UPDATE,
	FAIL_SPAWN
} Failure;
static Failure failure;
static void *attributes;
static bool initialized, allocation_pending;
static unsigned init_calls, delete_calls, free_calls, spawn_calls;

static BOOL WINAPI test_initialize(LPPROC_THREAD_ATTRIBUTE_LIST list,
        DWORD count,
        DWORD flags,
        PSIZE_T bytes)
{
	if (!list) {
		allocation_pending = true;
		return InitializeProcThreadAttributeList(
		        list, count, flags, bytes);
	}
	attributes = list;
	init_calls++;
	if (FAIL_INIT == failure) {
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	initialized =
	        !!InitializeProcThreadAttributeList(list, count, flags, bytes);
	return initialized;
}

static BOOL WINAPI test_update(LPPROC_THREAD_ATTRIBUTE_LIST list,
        DWORD flags,
        DWORD_PTR attribute,
        PVOID value,
        SIZE_T bytes,
        PVOID previous,
        PSIZE_T returned)
{
	CHECK(initialized);
	if (FAIL_UPDATE == failure) {
		SetLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	return UpdateProcThreadAttribute(
	        list, flags, attribute, value, bytes, previous, returned);
}

static VOID WINAPI test_delete(LPPROC_THREAD_ATTRIBUTE_LIST list)
{
	delete_calls++;
	CHECK(initialized && attributes == list);
	if (initialized) {
		DeleteProcThreadAttributeList(list);
		initialized = false;
	}
}

static void test_free(void *p)
{
	if (p && p == attributes) {
		free_calls++;
	}
	free(p);
}

#if defined(WF_TEST_PROC_DECODER)
static void *test_malloc(size_t bytes)
{
	if (allocation_pending) {
		allocation_pending = false;
		if (FAIL_ALLOC == failure) {
			SetLastError(ERROR_INVALID_DATA);
			return NULL;
		}
	}
	return malloc(bytes);
}
#else
static LPVOID WINAPI test_heap_alloc(HANDLE heap, DWORD flags, SIZE_T bytes)
{
	if (allocation_pending) {
		allocation_pending = false;
		if (FAIL_ALLOC == failure) {
			SetLastError(ERROR_INVALID_DATA);
			return NULL;
		}
	}
	return HeapAlloc(heap, flags, bytes);
}
#endif

#if !defined(WF_TEST_PROC_DECODER)
static BOOL WINAPI test_heap_free(HANDLE heap, DWORD flags, LPVOID p)
{
	if (p && p == attributes) {
		free_calls++;
	}
	return HeapFree(heap, flags, p);
}
#endif

static BOOL forbidden_spawn(void)
{
	spawn_calls++;
	SetLastError(ERROR_ACCESS_DENIED);
	return FALSE;
}

#define InitializeProcThreadAttributeList test_initialize
#define UpdateProcThreadAttribute test_update
#define DeleteProcThreadAttributeList test_delete
#define CreateProcessW(...) forbidden_spawn()
#define free test_free
#if defined(WF_TEST_PROC_SSH)
#	define HeapAlloc test_heap_alloc
#	define HeapFree test_heap_free
#	include "../src/fs/wf_sshproc_win.c"
#elif defined(WF_TEST_PROC_GIT)
#	define HeapAlloc test_heap_alloc
#	define HeapFree test_heap_free
#	include "../src/git/wf_gitproc_win.c"
#elif defined(WF_TEST_PROC_DECODER)
#	define malloc test_malloc
#	include "../src/preview/wf_dechost_client_win.c"
#endif
#undef free
#undef HeapFree
#undef HeapAlloc
#undef malloc

static void refused_start(void)
{
#if defined(WF_TEST_PROC_SSH)
	WfSshProc *p = NULL;
	CHECK(WF_E_OS ==
	        wf_sshproc_start(L"not-started.exe", L"not-started.exe", &p));
	CHECK(!p);
#elif defined(WF_TEST_PROC_GIT)
	WfGitRun run = {0};
	WfGitRunResult result;
	run.exe = L"not-started.exe";
	CHECK(WF_E_OS == wf_gitproc_run(&run, &result));
	CHECK(!result.out && !result.out_n);
	CHECK(result.os_error ==
	        (DWORD)(failure <= FAIL_INIT ? ERROR_NOT_ENOUGH_MEMORY
	                                     : ERROR_ACCESS_DENIED));
	wf_gitproc_result_free(&result);
#elif defined(WF_TEST_PROC_DECODER)
	WfDecHostOptions opt = {L"not-started.exe", 0, 0};
	WfDecHost *host = NULL;
	DWORD error = 0;
	WfDecStartPhase phase;
	CHECK(WF_E_OS ==
	        wf_dechost_start_with_phase(&opt, &host, &error, &phase));
	CHECK(!host);
	CHECK(error ==
	        (DWORD)(failure <= FAIL_INIT ? ERROR_NOT_ENOUGH_MEMORY
	                                     : ERROR_ACCESS_DENIED));
	CHECK(phase ==
	        (FAIL_SPAWN == failure ? WF_DEC_START_PROCESS
	                               : WF_DEC_START_ATTRIBUTES));
#endif
}

int main(void)
{
	for (int mode = FAIL_ALLOC; mode <= FAIL_SPAWN; mode++) {
		failure = (Failure)mode;
		attributes = NULL;
		initialized = false;
		allocation_pending = false;
		init_calls = delete_calls = free_calls = spawn_calls = 0;
		refused_start();
		CHECK(init_calls == (FAIL_ALLOC == failure ? 0u : 1u));
		CHECK(free_calls == (FAIL_ALLOC == failure ? 0u : 1u));
		CHECK(delete_calls == (failure <= FAIL_INIT ? 0u : 1u));
		CHECK(spawn_calls == (FAIL_SPAWN == failure ? 1u : 0u));
		CHECK(!initialized);
	}
	return TEST_RESULT();
}
