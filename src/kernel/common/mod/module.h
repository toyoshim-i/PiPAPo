/*
 * module.h — Kernel module system macros
 *
 * Platform-agnostic module boundaries for the PPAP kernel.
 * ALL platforms use the same struct-of-function-pointers pattern.
 * Callers always use mod_<name>.<func>(args) on every platform.
 *
 * Declaration (in mod/mod_<name>.h):
 *   MOD_DECLARE_BEGIN(name) / MOD_FUNC(...) / MOD_DECLARE_END(name)
 *
 * Definition (in the module's .c file):
 *   MOD_DEFINE_BEGIN(name) / MOD_IMPL(mod, func) / MOD_DEFINE_END()
 *
 * On all platforms: struct of function pointers, callers use
 * mod_<name>.<func>(args).  On i16 when segments are split,
 * the struct entries become far-call thunks.
 */

#ifndef PPAP_KERNEL_COMMON_MOD_MODULE_H
#define PPAP_KERNEL_COMMON_MOD_MODULE_H

/* Token-pasting helpers (two levels for argument expansion) */
#define _MOD_CONCAT2(a, b) a##_##b
#define _MOD_CONCAT(a, b)  _MOD_CONCAT2(a, b)

/* ── Declaration: struct typedef + extern instance ────────────────────── */

#define MOD_DECLARE_BEGIN(name) \
  typedef struct mod_##name##_s {

/* Unprefixed field name inside struct — no name conflicts.
 * When PPAP_MOD_FAR is defined (i16 segment split active),
 * __far generates 32-bit segment:offset pointers and lcall.
 * Otherwise near pointers (same segment, normal call). */
#if defined(PPAP_MOD_FAR)
#define MOD_FUNC(mod, ret, func, ...) \
    ret __far (*func)(__VA_ARGS__);
#else
#define MOD_FUNC(mod, ret, func, ...) \
    ret (*func)(__VA_ARGS__);
#endif

#define MOD_DECLARE_END(name) \
  } mod_##name##_t; \
  extern mod_##name##_t mod_##name;

/* ── Definition: struct initializer ───────────────────────────────────── */

#define MOD_DEFINE_BEGIN(name) \
  mod_##name##_t mod_##name = {

/* Maps unprefixed field to prefixed real function: .init = vfs_init */
#define MOD_IMPL(mod, func) \
    .func = _MOD_CONCAT(mod, func),

#define MOD_DEFINE_END() \
  };

#endif /* PPAP_KERNEL_COMMON_MOD_MODULE_H */
