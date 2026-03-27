/* PiPAPo — RISC-V ePIC CRT startup.
 *
 * Replaces upstream riscv32 crt_arch.h:
 *
 * 1. Removes `lla gp, __global_pointer$` — with ePIC, the kernel ELF loader
 *    sets gp = load_base (data_base - data_va) in the initial trap frame.
 *    All gp-relative data access uses gp + link_addr to reach the correct
 *    runtime address.  CRT must NOT overwrite gp.
 *
 * 2. Passes _DYNAMIC = NULL (a1 = 0) instead of PC-relative `lla a1,
 *    _DYNAMIC`.  With XIP, text lives in romfs and _DYNAMIC in SRAM, so the
 *    PC-relative offset is wrong.  The kernel already applies all relocations,
 *    so musl's self-relocation (__dls3) must be skipped.
 */
__asm__(
".section .sdata,\"aw\"\n"
".text\n"
".global " START "\n"
".type " START ",%function\n"
START ":\n\t"
"mv a0, sp\n\t"
"li a1, 0\n\t"
"andi sp, sp, -16\n\t"
"tail " START "_c"
);
