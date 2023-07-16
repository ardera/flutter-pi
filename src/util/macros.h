/*
 * Copyright © 2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef UTIL_MACROS_H
#define UTIL_MACROS_H

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Compute the size of an array */
#ifndef ARRAY_SIZE
    #define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* For compatibility with Clang's __has_builtin() */
#ifndef __has_builtin
    #define __has_builtin(x) 0
#endif

#ifndef __has_attribute
    #define __has_attribute(x) 0
#endif

/**
 * Define HAVE___BUILTIN_... macros
 */
#if __has_builtin(__builtin_bswap32)
    #define HAVE___BUILTIN_BSWAP32
#endif
#if __has_builtin(__builtin_bswap64)
    #define HAVE___BUILTIN_BSWAP64
#endif
#if __has_builtin(__builtin_clz)
    #define HAVE___BUILTIN_CLZ
#endif
#if __has_builtin(__builtin_clzll)
    #define HAVE___BUILTIN_CLZLL
#endif
#if __has_builtin(__builtin_ctz)
    #define HAVE___BUILTIN_CTZ
#endif
#if __has_builtin(__builtin_expect)
    #define HAVE___BUILTIN_EXPECT
#endif
#if __has_builtin(__builtin_ffs)
    #define HAVE___BUILTIN_FFS
#endif
#if __has_builtin(__builtin_ffsll)
    #define HAVE___BUILTIN_FFSLL
#endif
#if __has_builtin(__builtin_popcount)
    #define HAVE___BUILTIN_POPCOUNT
#endif
#if __has_builtin(__builtin_popcountll)
    #define HAVE___BUILTIN_POPCOUNTLL
#endif
#if __has_builtin(__builtin_unreachable)
    #define HAVE___BUILTIN_UNREACHABLE
#endif
#if __has_builtin(__builtin_types_compatible_p)
    #define HAVE___BUILTIN_TYPES_COMPATIBLE_P
#endif
#if __has_builtin(__builtin_trap)
    #define HAVE___BUILTIN_TRAP
#endif

#if __has_attribute(const)
    #define HAVE_FUNC_ATTRIBUTE_CONST
#endif
#if __has_attribute(flatten)
    #define HAVE_FUNC_ATTRIBUTE_FLATTEN
#endif
#if __has_attribute(malloc)
    #define HAVE_FUNC_ATTRIBUTE_MALLOC
#endif
#if __has_attribute(pure)
    #define HAVE_FUNC_ATTRIBUTE_PURE
#endif
#if __has_attribute(unused)
    #define HAVE_FUNC_ATTRIBUTE_UNUSED
#endif
#if __has_attribute(warn_unused_result)
    #define HAVE_FUNC_ATTRIBUTE_WARN_UNUSED_RESULT
#endif
#if __has_attribute(weak)
    #define HAVE_FUNC_ATTRIBUTE_WEAK
#endif
#if __has_attribute(format)
    #define HAVE_FUNC_ATTRIBUTE_FORMAT
#endif
#if __has_attribute(packed)
    #define HAVE_FUNC_ATTRIBUTE_PACKED
#endif
#if __has_attribute(returns_nonnull)
    #define HAVE_FUNC_ATTRIBUTE_RETURNS_NONNULL
#endif
#if __has_attribute(alias)
    #define HAVE_FUNC_ATTRIBUTE_ALIAS
#endif
#if __has_attribute(noreturn)
    #define HAVE_FUNC_ATTRIBUTE_NORETURN
#endif

/**
 * __builtin_expect macros
 */
#if !defined(HAVE___BUILTIN_EXPECT)
    #define __builtin_expect(x, y) (x)
#endif

#ifndef LIKELY
    #ifdef HAVE___BUILTIN_EXPECT
        #define LIKELY(x) __builtin_expect(!!(x), 1)
        #define UNLIKELY(x) __builtin_expect(!!(x), 0)
    #else
        #define LIKELY(x) (x)
        #define UNLIKELY(x) (x)
    #endif
#endif

/**
 * __builtin_types_compatible_p compat
 */
#if defined(__cplusplus) || !defined(HAVE___BUILTIN_TYPES_COMPATIBLE_P)
    #define __builtin_types_compatible_p(type1, type2) (1)
#endif

/* This should match linux gcc cdecl semantics everywhere, so that we
 * just codegen one calling convention on all platforms.
 */
#ifdef _MSC_VER
    #define UTIL_CDECL __cdecl
#else
    #define UTIL_CDECL
#endif

/**
 * Static (compile-time) assertion.
 */
#define STATIC_ASSERT(cond)         \
    do {                            \
        static_assert(cond, #cond); \
    } while (0)

/**
 * CONTAINER_OF - cast a member of a structure out to the containing structure
 * @ptr:        the pointer to the member.
 * @type:       the type of the container struct this is embedded in.
 * @member:     the name of the member within the struct.
 */
#ifndef __GNUC__
    /* a grown-up compiler is required for the extra type checking: */
    #define CONTAINER_OF(ptr, type, member) (type *) ((uint8_t *) ptr - offsetof(type, member))
#else
    #define __same_type(a, b) __builtin_types_compatible_p(__typeof__(a), __typeof__(b))
    #define CONTAINER_OF(ptr, type, member)                                             \
        ({                                                                              \
            uint8_t *__mptr = (uint8_t *) (ptr);                                        \
            static_assert(                                                              \
                __same_type(*(ptr), ((type *) 0)->member) || __same_type(*(ptr), void), \
                "pointer type mismatch in CONTAINER_OF()"                               \
            );                                                                          \
            ((type *) (__mptr - offsetof(type, member)));                               \
        })
#endif

/**
 * Unreachable macro. Useful for suppressing "control reaches end of non-void
 * function" warnings.
 */
#if defined(HAVE___BUILTIN_UNREACHABLE) || __has_builtin(__builtin_unreachable)
    #define UNREACHABLE_MSG(str)     \
        do {                         \
            assert(!str);            \
            __builtin_unreachable(); \
        } while (0)
    #define UNREACHABLE()            \
        do {                         \
            assert(0);               \
            __builtin_unreachable(); \
        } while (0)
#elif defined(_MSC_VER)
    #define UNREACHABLE_MSG(str) \
        do {                     \
            assert(!str);        \
            __assume(0);         \
        } while (0)
    #define UNREACHABLE() \
        do {              \
            assert(0);    \
            __assume(0);  \
        } while (0)
#else
    #define UNREACHABLE_MSG(str) assert(!str)
    #define UNREACHABLE() assert(0)
#endif

/**
 * Assume macro. Useful for expressing our assumptions to the compiler,
 * typically for purposes of silencing warnings.
 */
#if __has_builtin(__builtin_assume)
    #define ASSUME(expr)            \
        do {                        \
            assert(expr);           \
            __builtin_assume(expr); \
        } while (0)
#elif defined HAVE___BUILTIN_UNREACHABLE
    #define ASSUME(expr) ((expr) ? ((void) 0) : (assert(!"assumption failed"), __builtin_unreachable()))
#elif defined(_MSC_VER)
    #define ASSUME(expr) __assume(expr)
#else
    #define ASSUME(expr) assert(expr)
#endif

/* Attribute const is used for functions that have no effects other than their
 * return value, and only rely on the argument values to compute the return
 * value.  As a result, calls to it can be CSEed.  Note that using memory
 * pointed to by the arguments is not allowed for const functions.
 */
#if !defined(__clang__) && defined(HAVE_FUNC_ATTRIBUTE_CONST)
    #define ATTR_CONST __attribute__((__const__))
#else
    #define ATTR_CONST
#endif

#ifdef HAVE_FUNC_ATTRIBUTE_FLATTEN
    #define FLATTEN __attribute__((__flatten__))
#else
    #define FLATTEN
#endif

#ifdef HAVE_FUNC_ATTRIBUTE_FORMAT
    #if defined(__MINGW_PRINTF_FORMAT)
        #define PRINTFLIKE(f, a) __attribute__((format(__MINGW_PRINTF_FORMAT, f, a)))
    #else
        #define PRINTFLIKE(f, a) __attribute__((format(__printf__, f, a)))
    #endif
#else
    #define PRINTFLIKE(f, a)
#endif

#ifdef HAVE_FUNC_ATTRIBUTE_MALLOC
    #define MALLOCLIKE __attribute__((__malloc__))
#else
    #define MALLOCLIKE
#endif

/* Forced function inlining */
/* Note: Clang also sets __GNUC__ (see other cases below) */
#ifndef ALWAYS_INLINE
    #if defined(__GNUC__)
        #define ALWAYS_INLINE inline __attribute__((always_inline))
    #elif defined(_MSC_VER)
        #define ALWAYS_INLINE __forceinline
    #else
        #define ALWAYS_INLINE inline
    #endif
#endif

/* Used to optionally mark structures with misaligned elements or size as
 * packed, to trade off performance for space.
 */
#ifdef HAVE_FUNC_ATTRIBUTE_PACKED
    #define PACKED __attribute__((__packed__))
#else
    #define PACKED
#endif

/* Attribute pure is used for functions that have no effects other than their
 * return value.  As a result, calls to it can be dead code eliminated.
 */
#ifdef HAVE_FUNC_ATTRIBUTE_PURE
    #define ATTR_PURE __attribute__((__pure__))
#else
    #define ATTR_PURE
#endif

#ifdef HAVE_FUNC_ATTRIBUTE_RETURNS_NONNULL
    #define ATTR_RETURNS_NONNUL __attribute__((__returns_nonnull__))
#else
    #define ATTR_RETURNS_NONNUL
#endif

#ifndef NORETURN
    #ifdef _MSC_VER
        #define NORETURN __declspec(noreturn)
    #elif defined HAVE_FUNC_ATTRIBUTE_NORETURN
        #define NORETURN __attribute__((__noreturn__))
    #else
        #define NORETURN
    #endif
#endif

#ifdef __cplusplus
    /**
 * Macro function that evaluates to true if T is a trivially
 * destructible type -- that is, if its (non-virtual) destructor
 * performs no action and all member variables and base classes are
 * trivially destructible themselves.
 */
    #if defined(__clang__)
        #if __has_builtin(__is_trivially_destructible)
            #define HAS_TRIVIAL_DESTRUCTOR(T) __is_trivially_destructible(T)
        #elif (defined(__has_feature) && __has_feature(has_trivial_destructor))
            #define HAS_TRIVIAL_DESTRUCTOR(T) __has_trivial_destructor(T)
        #endif
    #elif defined(__GNUC__)
        #if ((__GNUC__ > 4) || ((__GNUC__ == 4) && (__GNUC_MINOR__ >= 3)))
            #define HAS_TRIVIAL_DESTRUCTOR(T) __has_trivial_destructor(T)
        #endif
    #elif defined(_MSC_VER) && !defined(__INTEL_COMPILER)
        #define HAS_TRIVIAL_DESTRUCTOR(T) __has_trivial_destructor(T)
    #endif
    #ifndef HAS_TRIVIAL_DESTRUCTOR
        /* It's always safe (if inefficient) to assume that a
        * destructor is non-trivial.
        */
        #define HAS_TRIVIAL_DESTRUCTOR(T) (false)
    #endif
#endif

/**
 * PUBLIC/USED macros
 *
 * If we build the library with gcc's -fvisibility=hidden flag, we'll
 * use the PUBLIC macro to mark functions that are to be exported.
 *
 * We also need to define a USED attribute, so the optimizer doesn't
 * inline a static function that we later use in an alias. - ajax
 */
#ifndef PUBLIC
    #if defined(_WIN32)
        #define PUBLIC __declspec(dllexport)
        #define USED
    #elif defined(__GNUC__)
        #define PUBLIC __attribute__((visibility("default")))
        #define USED __attribute__((used))
    #else
        #define PUBLIC
        #define USED
    #endif
#endif

/**
 * UNUSED marks variables (or sometimes functions) that have to be defined,
 * but are sometimes (or always) unused beyond that. A common case is for
 * a function parameter to be used in some build configurations but not others.
 * Another case is fallback vfuncs that don't do anything with their params.
 *
 * Note that this should not be used for identifiers used in `assert()`;
 * see ASSERTED below.
 */
#ifdef HAVE_FUNC_ATTRIBUTE_UNUSED
    #define UNUSED __attribute__((unused))
#elif defined(_MSC_VER)
    #define UNUSED __pragma(warning(suppress : 4100 4101 4189))
#else
    #define UNUSED
#endif

/**
 * Use ASSERTED to indicate that an identifier is unused outside of an `assert()`,
 * so that assert-free builds don't get "unused variable" warnings.
 */
#ifdef NDEBUG
    #define ASSERTED UNUSED
#else
    #define ASSERTED
#endif

#ifdef HAVE_FUNC_ATTRIBUTE_WARN_UNUSED_RESULT
    #define MUST_CHECK __attribute__((warn_unused_result))
#else
    #define MUST_CHECK
#endif

#if defined(__GNUC__)
    #define ATTR_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
    #define ATTR_NOINLINE __declspec(noinline)
#else
    #define ATTR_NOINLINE
#endif

/**
 * Check that STRUCT::FIELD can hold MAXVAL.  We use a lot of bitfields
 * in Mesa/gallium.  We have to be sure they're of sufficient size to
 * hold the largest expected value.
 * Note that with MSVC, enums are signed and enum bitfields need one extra
 * high bit (always zero) to ensure the max value is handled correctly.
 * This macro will detect that with MSVC, but not GCC.
 */
#define ASSERT_BITFIELD_SIZE(STRUCT, FIELD, MAXVAL)                         \
    do {                                                                    \
        ASSERTED STRUCT s;                                                  \
        s.FIELD = (MAXVAL);                                                 \
        assert((int) s.FIELD == (MAXVAL) && "Insufficient bitfield size!"); \
    } while (0)

/** Compute ceiling of integer quotient of A divided by B. */
#define DIV_ROUND_UP(A, B) (((A) + (B) -1) / (B))

/**
 * Clamp X to [MIN,MAX].  Turn NaN into MIN, arbitrarily.
 *
 * glib defines this as well. So check we don't redefine it.
 */
#ifndef CLAMP
    #define CLAMP(X, MIN, MAX) ((X) > (MIN) ? ((X) > (MAX) ? (MAX) : (X)) : (MIN))
#endif

/* Syntax sugar occuring frequently in graphics code */
#define SATURATE(X) CLAMP(X, 0.0f, 1.0f)

/** Minimum of two values: */
#define MIN2(A, B) ((A) < (B) ? (A) : (B))

/** Maximum of two values: */
#define MAX2(A, B) ((A) > (B) ? (A) : (B))

/** Minimum and maximum of three values: */
#define MIN3(A, B, C) ((A) < (B) ? MIN2(A, C) : MIN2(B, C))
#define MAX3(A, B, C) ((A) > (B) ? MAX2(A, C) : MAX2(B, C))

/** Minimum and maximum of four values: */
#define MIN4(A, B, C, D) ((A) < (B) ? MIN3(A, C, D) : MIN3(B, C, D))
#define MAX4(A, B, C, D) ((A) > (B) ? MAX3(A, C, D) : MAX3(B, C, D))

/** Align a value to a power of two */
#define ALIGN_POT(x, pot_align) (((x) + (pot_align) -1) & ~((pot_align) -1))

/** Checks is a value is a power of two. Does not handle zero. */
#define IS_POT(v) (((v) & ((v) -1)) == 0)

/** Set a single bit */
#define BITFIELD_BIT(b) (1u << (b))
/** Set all bits up to excluding bit b */
#define BITFIELD_MASK(b) ((b) == 32 ? (~0u) : BITFIELD_BIT((b) % 32) - 1)
/** Set count bits starting from bit b  */
#define BITFIELD_RANGE(b, count) (BITFIELD_MASK((b) + (count)) & ~BITFIELD_MASK(b))

/** Set a single bit */
#define BITFIELD64_BIT(b) (1ull << (b))
/** Set all bits up to excluding bit b */
#define BITFIELD64_MASK(b) ((b) == 64 ? (~0ull) : BITFIELD64_BIT(b) - 1)
/** Set count bits starting from bit b  */
#define BITFIELD64_RANGE(b, count) (BITFIELD64_MASK((b) + (count)) & ~BITFIELD64_MASK(b))

static inline int64_t u_intN_max(unsigned bit_size) {
    assert(bit_size <= 64 && bit_size > 0);
    return INT64_MAX >> (64 - bit_size);
}

static inline int64_t u_intN_min(unsigned bit_size) {
    /* On 2's compliment platforms, which is every platform Mesa is likely to
    * every worry about, stdint.h generally calculated INT##_MIN in this
    * manner.
    */
    return (-u_intN_max(bit_size)) - 1;
}

static inline uint64_t u_uintN_max(unsigned bit_size) {
    assert(bit_size <= 64 && bit_size > 0);
    return UINT64_MAX >> (64 - bit_size);
}

/* alignas usage
 * For struct or union, use alignas(align_size) on any member
 * of it will make it aligned to align_size.
 * See https://en.cppreference.com/w/c/language/_Alignas for
 * details. We can use static_assert and alignof to check if
 * the alignment result of alignas(align_size) on struct or
 * union is valid.
 * For example:
 *   static_assert(alignof(struct tgsi_exec_machine) == 16, "")
 * Also, we can use special code to see the size of the aligned
 * struct or union at the compile time with GCC, Clang or MSVC.
 * So we can see if the size of union or struct are as expected
 * when using alignas(align_size) on its member.
 * For example:
 *   char (*__kaboom)[sizeof(struct tgsi_exec_machine)] = 1;
 * can show us the size of struct tgsi_exec_machine at compile
 * time.
 */
#ifndef __cplusplus
    #ifdef _MSC_VER
        #define alignof _Alignof
        #define alignas _Alignas
    #else
        #include <stdalign.h>
    #endif
#endif

/* Macros for static type-safety checking.
 *
 * https://clang.llvm.org/docs/ThreadSafetyAnalysis.html
 */

#if __has_attribute(capability)
typedef int __attribute__((capability("mutex"))) lock_cap_t;

    #define GUARDED_BY(l) __attribute__((guarded_by(l)))
    #define ACQUIRE_CAP(l) __attribute((acquire_capability(l), no_thread_safety_analysis))
    #define RELEASE_CAP(l) __attribute((release_capability(l), no_thread_safety_analysis))
    #define ASSERT_CAP(l) __attribute((assert_capability(l), no_thread_safety_analysis))
    #define REQUIRES_CAP(l) __attribute((requires_capability(l)))
    #define DISABLE_THREAD_SAFETY_ANALYSIS __attribute((no_thread_safety_analysis))

#else

typedef int lock_cap_t;

    #define GUARDED_BY(l)
    #define ACQUIRE_CAP(l)
    #define RELEASE_CAP(l)
    #define ASSERT_CAP(l)
    #define REQUIRES_CAP(l)
    #define DISABLE_THREAD_SAFETY_ANALYSIS

#endif

#define DO_PRAGMA(X) _Pragma(#X)

#if defined(__clang__)
    #define PRAGMA_DIAGNOSTIC_PUSH _Pragma("clang diagnostic push")
    #define PRAGMA_DIAGNOSTIC_POP _Pragma("clang diagnostic pop")
    #define PRAGMA_DIAGNOSTIC_ERROR(X) DO_PRAGMA(clang diagnostic error #X)
    #define PRAGMA_DIAGNOSTIC_WARNING(X) DO_PRAGMA(clang diagnostic warning #X)
    #define PRAGMA_DIAGNOSTIC_IGNORED(X) DO_PRAGMA(clang diagnostic ignored #X)
#elif defined(__GNUC__)
    #define PRAGMA_DIAGNOSTIC_PUSH _Pragma("GCC diagnostic push")
    #define PRAGMA_DIAGNOSTIC_POP _Pragma("GCC diagnostic pop")
    #define PRAGMA_DIAGNOSTIC_ERROR(X) DO_PRAGMA(GCC diagnostic error #X)
    #define PRAGMA_DIAGNOSTIC_WARNING(X) DO_PRAGMA(GCC diagnostic warning #X)
    #define PRAGMA_DIAGNOSTIC_IGNORED(X) DO_PRAGMA(GCC diagnostic ignored #X)
#else
    #define PRAGMA_DIAGNOSTIC_PUSH
    #define PRAGMA_DIAGNOSTIC_POP
    #define PRAGMA_DIAGNOSTIC_ERROR(X)
    #define PRAGMA_DIAGNOSTIC_WARNING(X)
    #define PRAGMA_DIAGNOSTIC_IGNORED(X)
#endif

#define PASTE2(a, b) a##b
#define PASTE3(a, b, c) a##b##c
#define PASTE4(a, b, c, d) a##b##c##d

#define CONCAT2(a, b) PASTE2(a, b)
#define CONCAT3(a, b, c) PASTE3(a, b, c)
#define CONCAT4(a, b, c, d) PASTE4(a, b, c, d)

#if defined(__GNUC__)
    #define PRAGMA_POISON(X) DO_PRAGMA(GCC poison X)
#elif defined(__clang__)
    #define PRAGMA_POISON(X) DO_PRAGMA(clang poison X)
#else
    #define PRAGMA_POISON
#endif

#ifdef HAVE___BUILTIN_TRAP
    #define TRAP() __builtin_trap()
#else
    #define TRAP() abort()
#endif

#define UNIMPLEMENTED()                                                            \
    do {                                                                           \
        fprintf(stderr, "%s%s:%u: Unimplemented\n", __FILE__, __func__, __LINE__); \
        TRAP();                                                                    \
    } while (0)

#ifdef HAVE___BUILTIN_POPCOUNT
    #define HWEIGHT(x) __builtin_popcount(x)
#else
static inline int hamming_weight(uint64_t x) {
    // put count of each 2 bits into those 2 bits
    x -= (x >> 1) & 0x5555555555555555;

    // put count of each 4 bits into those 4 bits
    x = (x & 0x3333333333333333) + ((x >> 2) & 0x3333333333333333);

    // put count of each 8 bits into those 8 bits
    x = (x + (x >> 4)) & 0x0f0f0f0f0f0f0f0f;

    // returns left 8 bits of x + (x<<8) + (x<<16) + (x<<24) + ...
    return (x * 0x0101010101010101) >> 56;
}
    #define HWEIGHT(x) hamming_weight(x)
#endif

#endif /* UTIL_MACROS_H */
