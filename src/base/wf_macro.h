#ifndef WF_MACRO_H
#define WF_MACRO_H

#include <stddef.h>

/*
 * Element count of an array, fixed at compile time.
 *
 * Passing a pointer (including an array parameter, which has decayed to one)
 * would silently divide the pointer size by the element size. Under clang the
 * second term turns that into a compile error: the bit-field width becomes -1
 * when `a` and `&a[0]` have the same type. MSVC lacks the builtin, so it only
 * gets the plain division; the clang-cl build is the one that enforces this.
 *
 * Usable in constant expressions, e.g. as an array bound or in
 * _Static_assert. `a` is evaluated only by sizeof, never at run time.
 */
#if defined(__clang__)
#	define WF_ARRAY_LEN(a)                                                \
		(sizeof(a) / sizeof((a)[0]) + sizeof(struct {                  \
			int wf_argument_is_not_an_array                        \
			    : __builtin_types_compatible_p(                    \
				      __typeof__(a), __typeof__(&(a)[0]))      \
			      ? -1                                             \
			      : 1;                                             \
		}) * 0)
#else
#	define WF_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#endif

#endif
