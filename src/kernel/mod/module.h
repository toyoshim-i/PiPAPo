/*
 * module.h — Kernel module system macros
 *
 * Platform-agnostic module boundaries for the PPAP kernel.
 *
 * On 32-bit platforms: struct of function pointers.
 *   Callers use mod_<name>.<func>(args).
 *   Struct fields are unprefixed (e.g. .init, .mount).
 *
 * On i16 (8086): plain extern declarations with prefixed names.
 *   Callers use <name>_<func>(args) directly.
 *
 * Usage:
 *
 *   // In mod/mod_vfs.h:
 *   MOD_DECLARE_BEGIN(vfs)
 *     MOD_FUNC(vfs, void, init, void)
 *     MOD_FUNC(vfs, int,  mount, const char *, ...)
 *   MOD_DECLARE_END(vfs)
 *
 *   // In vfs.c:
 *   MOD_DEFINE_BEGIN(vfs)
 *     MOD_IMPL(vfs, init)       // .init = vfs_init
 *   MOD_DEFINE_END()
 *
 *   // Callers (32-bit): mod_vfs.init()
 *   // Callers (i16):    vfs_init()
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

/* Field name is unprefixed (just 'func'), no conflict inside struct */
#define MOD_FUNC(mod, ret, func, ...) \
    ret (*func)(__VA_ARGS__);

#define MOD_DECLARE_END(name) \
  } mod_##name##_t; \
  extern mod_##name##_t mod_##name;

#define MOD_DEFINE_BEGIN(name) \
  mod_##name##_t mod_##name = {

/* Maps unprefixed field to prefixed real function: .init = vfs_init */
#define MOD_IMPL(mod, func) \
    .func = _MOD_CONCAT(mod, func),

#define MOD_DEFINE_END() \
  };

#else /* __ia16__ */

/* ── i16: plain extern declarations with prefixed names ───────────────── */

#define MOD_DECLARE_BEGIN(name)

/* Generates: ret mod_func(args); e.g. void vfs_init(void); */
#define MOD_FUNC(mod, ret, func, ...) \
  ret _MOD_CONCAT(mod, func)(__VA_ARGS__);

#define MOD_DECLARE_END(name)

#define MOD_DEFINE_BEGIN(name)
#define MOD_IMPL(mod, func)
#define MOD_DEFINE_END()

#endif /* __ia16__ */

#endif /* PPAP_KERNEL_MOD_MODULE_H */
