/*
 * <limits.h> — implementation limits.
 *
 * Self-contained: uses the compiler's built-in __INT_MAX__-style
 * predefines so the values are correct on every arch (ia16, arm,
 * m68k, riscv, xtensa) without an #include_next chain.
 */

#ifndef _LIMITS_H
#define _LIMITS_H

#define CHAR_BIT 8

#define SCHAR_MAX __SCHAR_MAX__
#define SCHAR_MIN (-__SCHAR_MAX__ - 1)
#define UCHAR_MAX (__SCHAR_MAX__ * 2 + 1)

#if defined(__CHAR_UNSIGNED__)
#define CHAR_MAX UCHAR_MAX
#define CHAR_MIN 0
#else
#define CHAR_MAX SCHAR_MAX
#define CHAR_MIN SCHAR_MIN
#endif

#define MB_LEN_MAX 1

#define SHRT_MAX __SHRT_MAX__
#define SHRT_MIN (-__SHRT_MAX__ - 1)
#define USHRT_MAX (__SHRT_MAX__ * 2 + 1)

#define INT_MAX __INT_MAX__
#define INT_MIN (-__INT_MAX__ - 1)
#define UINT_MAX (__INT_MAX__ * 2u + 1u)

#define LONG_MAX __LONG_MAX__
#define LONG_MIN (-__LONG_MAX__ - 1L)
#define ULONG_MAX (__LONG_MAX__ * 2uL + 1uL)

#define LLONG_MAX __LONG_LONG_MAX__
#define LLONG_MIN (-__LONG_LONG_MAX__ - 1LL)
#define ULLONG_MAX (__LONG_LONG_MAX__ * 2uLL + 1uLL)

/* POSIX system-imposed limits. */
#define PATH_MAX 256
#define NAME_MAX 64
#define ARG_MAX 4096

#endif /* _LIMITS_H */
