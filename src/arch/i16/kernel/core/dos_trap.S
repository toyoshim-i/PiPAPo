/*
 * dos_trap.S --- INT 21h DOS ISR for i8086 real mode
 *
 * Uses the SAME stack layout as trap.S (INT 30h syscall):
 *   User stack (pushed by CPU + ISR prologue, low-to-high at user_SP):
 *     [+0 ] ES   [+2 ] DS  [+4 ] BP  [+6 ] DI  [+8 ] SI
 *     [+10] DX   [+12] CX  [+14] BX  [+16] AX
 *     [+18] IP   [+20] CS  [+22] FLAGS      (pushed by CPU on INT)
 *   Kernel stack (at SP after prologue):
 *     [+0 ..+33] 34-byte vfork-save slot  (reserved for uniformity)
 *     [+34]      user_SP
 *     [+36]      user_SS
 *
 * After dispatch we jump into trap.S's shared post-dispatch tail,
 * which decides whether the scratch frame should be copied back to
 * user memory before continuing with the generic ia16 switch/restore
 * path.  That tail handles: deferred context switch (zombie/blocked
 * via sys_exit), vfork parent-frame restore, sigreturn, signal
 * delivery, user SS:SP restore and the final IRET.
 *
 * The dispatcher reads/writes a `dos_regs_t` whose layout matches the
 * 24-byte GP+IRET frame at user_SP (see dos_bridge.h).  AX and FLAGS
 * modifications propagate back to the caller via the IRET pop.
 */

  .code16
  .text

  .global i16_dos_isr
  .global i16_dos_int20_isr
  .extern i16_dos_int21h_dispatch_native
  .extern i16_current_ksp
  .extern i16_trap_finish_scratch_frame

/* INT 20h (Program Terminate) — convert to INT 21h AH=4Ch, exit code 0.
 * Sets AX before falling through to i16_dos_isr so the dispatcher sees
 * function 4Ch. */
i16_dos_int20_isr:
  movw  $0x4C00, %ax
  /* fall through */

i16_dos_isr:
  /* CPU pushed FLAGS, CS, IP onto current SS:SP.  IF=0. */

  /* Save GP registers on the current stack in trap.S order */
  pushw %ax
  pushw %bx
  pushw %cx
  pushw %dx
  pushw %si
  pushw %di
  pushw %bp
  pushw %ds
  pushw %es

  /* Capture interrupted SS:SP */
  movw  %ss, %ax
  movw  %sp, %bp

  /* Switch to kernel stack if we came from user mode */
  testw %ax, %ax
  jz    .Lkstack_ready

  xorw  %bx, %bx
  movw  %bx, %ss
  movw  %ss:i16_current_ksp, %sp

.Lkstack_ready:
  /* Push interrupted SS:SP onto kernel stack */
  pushw %ax               /* user_SS */
  pushw %bp               /* user_SP  — points at GP frame on user stack */

  /* DS = ES = 0 for kernel data access */
  xorw  %ax, %ax
  movw  %ax, %ds
  movw  %ax, %es

  /* Reserve the 34-byte vfork-save slot above the C call chain so this
   * entry path produces the same kernel-stack layout as trap.S /
   * switch.S and can share their restore tail unchanged. */
  subw  $34, %sp

  /* Copy the 24-byte GP+IRET frame (ES..FLAGS) from user stack into
   * a kernel-stack scratch buffer.  The dispatcher works on the copy
   * so its regs->* stores do not require a user-segment override. */
  movw  %sp, %bp
  movw  36(%bp), %ax      /* user_SS (at kstack_top - 2) */
  movw  34(%bp), %si      /* user_SP (at kstack_top - 4) */

  subw  $24, %sp          /* scratch room for dos_regs_t copy */
  movw  %sp, %di
  movw  %ax, %ds          /* DS = user_SS (source) */
  movw  $12, %cx          /* 12 words = 24 bytes */
  cld
  rep movsw
  xorw  %ax, %ax
  movw  %ax, %ds          /* DS back to 0 for kernel */

  /* Call dispatcher: int i16_dos_int21h_dispatch_native(dos_regs_t *) */
  movw  %sp, %ax
  pushw %ax
  call  i16_dos_int21h_dispatch_native
  addw  $2, %sp

  /* Hand off to the shared ia16 post-dispatch tail.  It owns the
   * generic policy of whether this scratch frame should be copied back
   * to user memory or dropped because exec/exit/block changed the task
   * state, then continues through the common switch/restore path. */
  jmp   i16_trap_finish_scratch_frame

/* -- Stubs for DOS-reserved vectors we don't implement ---------------- *
 *
 * INT 23h/24h/27h/29h/2Eh/2Fh are reserved by DOS for specific
 * purposes but are not hooked elsewhere in PPAP.  Without these stubs
 * the IVT slots retain whatever BIOS / stage2 last wrote, so a guest
 * probing them reads an undefined CS:IP.  Each stub below replies per
 * DOS spec:
 *   23h  Ctrl-C handler                  — no-op return
 *   24h  critical error handler          — AL=3 ("fail") + return
 *   27h  terminate-and-stay-resident     — treat as AH=4Ch exit 0
 *   29h  fast console output (DOS 2+)    — treat as AH=02h write-char
 *   2Eh  reload transient COMMAND.COM    — no-op return
 *   2Fh  multiplex                       — CF=1 ("not installed")
 *
 * CPU-pushed IRET frame (at entry SS:SP): [+0] IP, [+2] CS, [+4] FLAGS.
 */

  .global i16_dos_int23_isr     /* Ctrl-C handler: ignore */
i16_dos_int23_isr:
  iret

  .global i16_dos_int24_isr     /* Critical error: return AL=3 (fail) */
i16_dos_int24_isr:
  movb  $3, %al
  iret

  .global i16_dos_int27_isr     /* TSR terminate: treat as AH=4Ch exit 0 */
i16_dos_int27_isr:
  movw  $0x4C00, %ax
  jmp   i16_dos_isr

  .global i16_dos_int29_isr     /* Fast console output: AL -> AH=02h/DL */
i16_dos_int29_isr:
  /* INT 29h passes the byte in AL and has no caller-preserved register
   * contract beyond flags.  Convert to INT 21h AH=02h (write char) by
   * loading DL = AL and AH = 02h, then fall into the shared i16 DOS
   * dispatcher so the stdout path is identical to explicit AH=02h. */
  movb  %al, %dl
  movb  $0x02, %ah
  jmp   i16_dos_isr

  .global i16_dos_int2e_isr     /* Reload transient COMMAND.COM: ignore */
i16_dos_int2e_isr:
  iret

  .global i16_dos_int2f_isr     /* Multiplex: set CF in IRET FLAGS */
i16_dos_int2f_isr:
  pushw %bp
  movw  %sp, %bp
  orw   $1, 6(%bp)              /* FLAGS at [bp+6]: +0 BP, +2 IP, +4 CS */
  popw  %bp
  iret
