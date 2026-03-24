/*
 * module.h — Kernel module system macros
 *
 * Each module header (mod_vfs.h) uses MOD_FUNC to declare its API.
 * The same header serves both declaration and definition:
 *
 *   Callers:        #include "mod/mod_vfs.h"        → declarations
 *   Implementation: #define MOD_IMPLEMENTATION       → declarations + definition
 *                   #include "mod/mod_vfs.h"
 *
 * Struct fields use unprefixed names (init, mount).  The MOD_FUNC
 * macro auto-generates prefixed real function names (vfs_init) from
 * the module name + field name.
 */

#ifndef PPAP_KERNEL_MOD_MODULE_H
#define PPAP_KERNEL_MOD_MODULE_H

/* Token-pasting helpers (two levels for argument expansion) */
#define _MOD_CONCAT2(a, b) a##_##b
#define _MOD_CONCAT(a, b)  _MOD_CONCAT2(a, b)

#endif /* PPAP_KERNEL_MOD_MODULE_H */

/*
 * Macros below are OUTSIDE the include guard — they are redefined
 * each time based on _MOD_PHASE.  A module header sets _MOD_PHASE
 * to control which output is generated.
 */

#undef MOD_DECLARE_BEGIN
#undef MOD_FUNC
#undef MOD_DECLARE_END

#if _MOD_PHASE == 2

/* ── Phase 2: struct initializer (implementation) ─────────────────────── */

#if !defined(__ia16__)

#define MOD_DECLARE_BEGIN(name) \
  mod_##name##_t mod_##name = {

#define MOD_FUNC(mod, ret, func, ...) \
    .func = _MOD_CONCAT(mod, func),

#define MOD_DECLARE_END(name) \
  };

#else

#define MOD_DECLARE_BEGIN(name)
#define MOD_FUNC(mod, ret, func, ...)
#define MOD_DECLARE_END(name)

#endif

#else /* _MOD_PHASE == 1 or undefined (declaration) */

/* ── Phase 1: struct typedef + extern (declaration) ───────────────────── */

#if !defined(__ia16__)

#define MOD_DECLARE_BEGIN(name) \
  typedef struct mod_##name##_s {

#define MOD_FUNC(mod, ret, func, ...) \
    ret (*func)(__VA_ARGS__);

#define MOD_DECLARE_END(name) \
  } mod_##name##_t; \
  extern mod_##name##_t mod_##name;

#else

#define MOD_DECLARE_BEGIN(name)

#define MOD_FUNC(mod, ret, func, ...) \
  ret _MOD_CONCAT(mod, func)(__VA_ARGS__);

#define MOD_DECLARE_END(name)

#endif

#endif /* _MOD_PHASE */
