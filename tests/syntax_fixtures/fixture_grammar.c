/*
 * A stand-in for a grammar DLL (DR-DRAFT-033 tests): it exports the entry point
 * the loader looks for and hands back something that is not a real language. The
 * loader must refuse each of these (the variant is chosen at build time) and
 * never run a parse on them.
 *
 *   FIXTURE_ABI_NEW: a language that says it is ABI 99 (newer than the runtime)
 *   FIXTURE_ABI_OLD: ... ABI 12 (older than the runtime reads)
 *   FIXTURE_NULL:    the entry point returns NULL
 *   FIXTURE_DEP:     imports a function of dependency.dll, which is not in
 *                    System32 (and so must not be found)
 */
#include <stdint.h>
#include <windows.h>

#ifdef FIXTURE_DEP
__declspec(dllimport) int fixture_dependency(void);
#endif

/* the first member of a TSLanguage is its ABI version; nothing else is read
 * before the loader refuses it */
typedef struct
{
	uint32_t abi_version;
	uint32_t pad[64];
} FakeLanguage;

#if defined(FIXTURE_ABI_NEW)
static const FakeLanguage lang = {99, {0}};
#elif defined(FIXTURE_ABI_OLD)
static const FakeLanguage lang = {12, {0}};
#else
static const FakeLanguage lang = {14, {0}};
#endif

struct TSLanguage; /* the type of the real grammars' entry point, whose address is all that is passed on */

__declspec(dllexport) const struct TSLanguage *tree_sitter_json(void);

__declspec(dllexport) const struct TSLanguage *tree_sitter_json(void)
{
#if defined(FIXTURE_NULL)
	(void)&lang;
	return NULL;
#elif defined(FIXTURE_DEP)
	return fixture_dependency() ? (const struct TSLanguage *)&lang : NULL;
#else
	return (const struct TSLanguage *)&lang;
#endif
}
