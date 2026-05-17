/*
 * panic.h — One-shot kernel panic
 *
 * Single helper for "the kernel cannot continue".  Logs
 *
 *     PANIC: <fmt expanded with the supplied args>
 *
 * then halts the system: signals the target's may-poweroff hook (no-op
 * on real hardware, exit-status on QEMU lanes) and spins in
 * arch_wfi().  Never returns.
 *
 * Use for one-shot startup / allocator failures.  Arch fault handlers
 * that need to emit their own multi-line register dump call
 * target_may_poweroff() plus their preferred spin loop directly.
 */

#ifndef PPAP_KERNEL_CORE_PANIC_H
#define PPAP_KERNEL_CORE_PANIC_H

__attribute__((noreturn, format(printf, 1, 2))) void panic(const char *fmt,
                                                           ...);

#endif /* PPAP_KERNEL_CORE_PANIC_H */
