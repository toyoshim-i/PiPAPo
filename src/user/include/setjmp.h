/*
 * <setjmp.h> — non-local jumps.
 *
 * jmp_buf is an arch-specific array of words; the layout is private to
 * src/arch/<arch>/user/setjmp.S.  64 bytes (16 longs) is enough for
 * every arch we currently target.
 *
 * Pure POSIX semantics: setjmp() returns 0 directly; longjmp() returns
 * `val` (or 1 if val == 0).  No signal-mask handling — sigsetjmp /
 * siglongjmp are not provided.
 */

#ifndef _SETJMP_H
#define _SETJMP_H

typedef long jmp_buf[16];

int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

#endif /* _SETJMP_H */
