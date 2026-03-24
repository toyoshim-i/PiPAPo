/*
 * module.h — Kernel module system macros
 *
 * Defines MOD_DECLARE_BEGIN, MOD_FUNC, MOD_DECLARE_END differently
 * depending on whether MOD_IMPLEMENTATION is defined:
 *
 *   Without MOD_IMPLEMENTATION (callers):
 *     MOD_DECLARE_BEGIN → opens typedef struct
 *     MOD_FUNC          → function pointer field
 *     MOD_DECLARE_END   → closes struct + extern
 *
 *   With MOD_IMPLEMENTATION (the .c file):
 *     MOD_DECLARE_BEGIN → opens struct initializer
 *     MOD_FUNC          → .field = prefixed_func,
 *     MOD_DECLARE_END   → closes initializer
 *
 * Module headers write the MOD_FUNC list ONCE.  The header self-includes
 * to run the list a second time in implementation mode when
 * MOD_IMPLEMENTATION is defined.
 *
 * On i16, all macros expand to plain extern declarations or no-ops.
 */

#ifndef PPAP_KERNEL_MOD_MODULE_H
#define PPAP_KERNEL_MOD_MODULE_H

/* Token-pasting helpers (two levels for argument expansion) */
#define _MOD_CONCAT2(a, b) a##_##b
#define _MOD_CONCAT(a, b)  _MOD_CONCAT2(a, b)

/*
 * MOD_STATIC — use on module function definitions to enforce the
 * module boundary.  On 32-bit, functions are static (only accessible
 * via the module struct).  On i16, functions must be extern (direct
 * calls from other translation units).
 */
#if !defined(__ia16__)
#define MOD_STATIC static
#else
#define MOD_STATIC
#endif

#endif /* PPAP_KERNEL_MOD_MODULE_H */

/*
 * Macro definitions below are OUTSIDE the include guard.
 * They are redefined each time module.h is included.
 */

#undef MOD_DECLARE_BEGIN
#undef MOD_FUNC
#undef MOD_DECLARE_END

#ifdef _MOD_IMPL_PHASE

/* ── Implementation phase: struct initializer ─────────────────────────── */

#if !defined(__ia16__)

#define MOD_DECLARE_BEGIN(name) \
  mod_##name##_t mod_##name = {

/* .field = mod_field, — maps unprefixed field to prefixed function */
#define MOD_FUNC(mod, ret, func, ...) \
    .func = _MOD_CONCAT(mod, func),

#define MOD_DECLARE_END(name) \
  };

#else /* i16 implementation — no-op */

#define MOD_DECLARE_BEGIN(name)
#define MOD_FUNC(mod, ret, func, ...)
#define MOD_DECLARE_END(name)

#endif

#undef _MOD_IMPL_PHASE

#else /* !_MOD_IMPL_PHASE */

/* ── Declaration phase: struct typedef + extern ───────────────────────── */

#if !defined(__ia16__)

#define MOD_DECLARE_BEGIN(name) \
  typedef struct mod_##name##_s {

/* Unprefixed field name inside struct — no conflict */
#define MOD_FUNC(mod, ret, func, ...) \
    ret (*func)(__VA_ARGS__);

#define MOD_DECLARE_END(name) \
  } mod_##name##_t; \
  extern mod_##name##_t mod_##name;

#else /* i16 declaration — extern functions with prefixed names */

#define MOD_DECLARE_BEGIN(name)

#define MOD_FUNC(mod, ret, func, ...) \
  ret _MOD_CONCAT(mod, func)(__VA_ARGS__);

#define MOD_DECLARE_END(name)

#endif

#endif /* _MOD_IMPL_PHASE */
