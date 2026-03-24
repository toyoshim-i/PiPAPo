/*
 * module.h — Kernel module system macros
 *
 * Platform-agnostic module boundaries for the PPAP kernel.
 *
 * Declaration (in mod/mod_<name>.h):
 *   MOD_DECLARE_BEGIN(name) / MOD_FUNC(...) / MOD_DECLARE_END(name)
 *
 * Definition (in the module's .c file):
 *   MOD_DEFINE_BEGIN(name) / MOD_IMPL(mod, func) / MOD_DEFINE_END()
 *
 * On 32-bit: struct of function pointers.
 *   Callers use mod_<name>.<func>(args).
 *
 * On i16: plain extern declarations.
 *   Callers use <name>_<func>(args) directly.
 *   When i16 exceeds 64 KB, MOD_FUNC will generate far-call thunks.
 */

#ifndef PPAP_KERNEL_MOD_MODULE_H
#define PPAP_KERNEL_MOD_MODULE_H

/* Token-pasting helpers (two levels for argument expansion) */
#define _MOD_CONCAT2(a, b) a##_##b
#define _MOD_CONCAT(a, b)  _MOD_CONCAT2(a, b)

#if !defined(__ia16__)

/* ── 32-bit: struct of function pointers ──────────────────────────────── */

#define MOD_DECLARE_BEGIN(name) \
  typedef struct mod_##name##_s {

/* Unprefixed field name inside struct */
#define MOD_FUNC(mod, ret, func, ...) \
    ret (*func)(__VA_ARGS__);

#define MOD_DECLARE_END(name) \
  } mod_##name##_t; \
  extern mod_##name##_t mod_##name;

/* Definition: struct initializer */
#define MOD_DEFINE_BEGIN(name) \
  mod_##name##_t mod_##name = {

/* Maps unprefixed field to prefixed real function: .init = vfs_init */
#define MOD_IMPL(mod, func) \
    .func = _MOD_CONCAT(mod, func),

#define MOD_DEFINE_END() \
  };

#else /* __ia16__ */

/* ── i16: plain extern declarations ───────────────────────────────────── */

#define MOD_DECLARE_BEGIN(name)

/* Generates prefixed extern: void vfs_init(void); */
#define MOD_FUNC(mod, ret, func, ...) \
  ret _MOD_CONCAT(mod, func)(__VA_ARGS__);

#define MOD_DECLARE_END(name)

/* No-op on i16 (functions linked directly) */
#define MOD_DEFINE_BEGIN(name)
#define MOD_IMPL(mod, func)
#define MOD_DEFINE_END()

#endif /* __ia16__ */

#endif /* PPAP_KERNEL_MOD_MODULE_H */
