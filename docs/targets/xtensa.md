# Xtensa LX7 Targets

Architecture-specific reference for the PPAP Xtensa port. Current target
is the M5Stack CardComputer (`xtensa_cc`, ESP32-S3 dual-core LX7).

---

## 1. Architecture Overview

| Aspect | Detail |
|--------|--------|
| ISA | Xtensa LX7 (32-bit, configurable per chip) |
| Targets | M5Stack CardComputer (`xtensa_cc`) |
| Endianness | Little-endian |
| Word size | 32-bit |
| Registers | 64 physical GPR (16 visible via register window) |
| Kernel ABI | Windowed (ESP-IDF default) |
| User ABI | Call0 (flat register file, `-mabi=call0`) |
| PCB_SP_OFFSET | 0 |
| Syscall | `syscall` instruction (a7=number, a2-a6=args) |
| Timer | CCOMPARE0 (cycle-count compare, level-1 interrupt) |
| Context switch | Timer ISR sets `xtensa_switch_pending`, idle loop calls `sched_yield` |
| FPU | Single-precision (present but unused by PPAP) |
| MPU | PMS (Permission Management System, not yet implemented) |
| Multi-core | Single-core (Core 1 launch stubbed) |

---

## 2. Toolchains

### Kernel

- **Compiler:** `xtensa-esp32s3-elf-gcc` (ESP-IDF toolchain)
- **ABI:** Windowed (ESP-IDF default; kernel is an ESP-IDF component)
- **Setup:** `./scripts/setup_docker.sh xtensa`
- **Activation:** Automatic inside `ppap/xtensa` Docker container

The Xtensa toolchain is **chip-specific** — unlike ARM/RISC-V where one
toolchain covers many chips, each Xtensa configuration (ESP32, ESP32-S2,
ESP32-S3) has its own GCC build because the ISA is configurable per chip
(window size, DSP options, interrupt levels, etc.).

### User-space (current)

- Same `xtensa-esp32s3-elf-gcc` toolchain
- Compiled directly in `scripts/build.sh` (not via CMake)
- Flags: `-mabi=call0 -mlongcalls -ffreestanding -nostdlib -Os -fPIC -Wl,--emit-relocs`
- No libc — raw syscall stubs only

### User-space (planned)

- musl libc cross-compiled for Xtensa Call0 ABI
- busybox port for shell and utilities

---

## 3. ESP-IDF Integration

PPAP uses **ESP-IDF as a HAL layer** (not bare-metal). The kernel is
built as an ESP-IDF component, and ESP-IDF handles:

- Flash boot, cache setup, clock PLL
- Peripheral initialization
- Heap management (used for IRAM allocation)
- Exception vector dispatch

PPAP takes over after `app_main()`.

### Build flow

```
scripts/build.sh xtensa_cc:
  1. Source ESP-IDF export.sh
  2. idf.py set-target esp32s3  (first time only)
  3. Compile user binaries with xtensa-esp32s3-elf-gcc
  4. Generate romfs.bin via mkromfs
  5. idf.py build  (embeds romfs.bin via .incbin)
  6. idf.py flash + monitor
```

### sdkconfig overrides

| Setting | Value | Reason |
|---------|-------|--------|
| `CONFIG_ESP_INT_WDT` | n | PPAP replaces FreeRTOS; watchdog expects FreeRTOS ticks |
| `CONFIG_ESP_TASK_WDT_EN` | n | Same reason |
| `CONFIG_FREERTOS_UNICORE` | y | PPAP doesn't use Core 1; FreeRTOS tasks on Core 1 interfere |
| `CONFIG_ESPTOOLPY_FLASHSIZE_8MB` | y | CardComputer has 8 MB flash |
| `CONFIG_ESP_SYSTEM_MEMPROT_FEATURE` | n | PPAP allocates user code in IRAM via `heap_caps_malloc(MALLOC_CAP_EXEC)` |

### sched_yield name conflict

ESP-IDF's pthread library provides a strong `sched_yield()` symbol. PPAP
renames its own to `ppap_sched_yield()` with a `#define sched_yield
ppap_sched_yield` in `sched.h`.

---

## 4. Memory Architecture

### ESP32-S3 SRAM split

ESP32-S3 SRAM1 is split by the ESP-IDF linker between two buses:

| Region | Address range | Bus | Access |
|--------|--------------|-----|--------|
| IRAM | `0x40370000`-`0x403DFFFF` | Instruction bus | Execute + 32-bit data R/W |
| DRAM | `0x3FC88000`-`0x3FCFFFFF` | Data bus | Byte-level data R/W |

**IRAM and DRAM are NOT dual-mapped.** DRAM pages have no instruction bus
access.  User process code must be loaded into IRAM.

### IRAM restrictions

- **Word-access only:** IRAM supports only 32-bit aligned access. Byte-level
  `memcpy`/`memset` (from ROM) causes `LoadStoreError` (cause=3). The ELF
  loader uses word-at-a-time copy loops for IRAM.
- **Allocation:** `heap_caps_malloc(size, MALLOC_CAP_EXEC)` allocates from
  IRAM heap. `MALLOC_CAP_EXEC = (1<<0)`, not `(1<<4)` which is
  `MALLOC_CAP_PID2`.
- **Deallocation:** IRAM allocations must be freed with `heap_caps_free()`,
  not `page_free()`. The `user_page_free()` helper in `page.h` detects IRAM
  addresses and routes to the correct free function.

### Page pool

The PPAP page allocator uses DRAM for kernel stacks and data pages. The
page pool is configured by `mm_init()` using the DRAM range after kernel
BSS.

---

## 5. PIC / ELF Loading

### Two approaches, one constraint

Like RISC-V, Xtensa user binaries cannot use XIP (execute-in-place from
flash) because the code must run from IRAM. Both text and data segments
are loaded into a contiguous IRAM allocation.

### Literal pool relocation

Xtensa PIC uses `L32R` (PC-relative literal load) for address constants.
The literal pool values are absolute addresses resolved by the linker at
link-time base `0x0`. When loaded at a non-zero IRAM address, these values
need `load_base` added.

**Build side:** user binaries are compiled with `-Wl,--emit-relocs` to
preserve relocation entries in the ELF. Binaries are NOT stripped (strip
would destroy section headers needed for relocation scanning).

**Loader side:** the ELF loader scans `SHT_RELA` sections for:
- `R_XTENSA_32` (type 1) — absolute 32-bit data (literal pool values,
  initialized data pointers)
- `R_XTENSA_PLT` (type 6) — PLT-resolved function addresses in literal pool

For each entry, `load_base` is added to the 32-bit value at `r_offset`.

### SHF_ALLOC filter (critical)

The ELF may contain `.rela.xt.prop` and `.rela.xt.lit` sections (Xtensa
metadata) with `R_XTENSA_32` entries whose `r_offset` values are
**section-internal offsets**, not image offsets. Processing them corrupts
code bytes. The loader checks `sh_info` to find each RELA section's target
section and skips non-`SHF_ALLOC` sections.

### Linker script

`src/user/arch/xtensa/user.ld` — two-segment layout:
- **text (R+X):** `.literal*` (must precede code for L32R backward reach),
  `.text.crt0`, `.text*`, `.rodata`
- **data (RW):** `.got`, `.data`, `.bss`

**L32R reach constraint:** `L32R` computes target as a negative PC-relative
offset (up to -256 KB). Literal pools MUST precede the code that references
them. Placing `.literal*` after `.text*` causes the linker to generate bad
offsets.

---

## 6. Trap and Syscall Handling

### Exception model

Xtensa uses a level-based interrupt model with separate vectors per level.
Level-1 exceptions include syscalls, memory faults, and illegal
instructions. ESP-IDF dispatches level-1 exceptions through a handler table.

PPAP registers handlers via `xt_set_exception_handler()`:

| EXCCAUSE | Handler | Action |
|----------|---------|--------|
| 1 (Syscall) | `xtensa_syscall_handler` | Advance PC+3, dispatch syscall |
| 0, 2-29 (others) | `xtensa_fault_handler` | Kill user process or kernel panic |

Exceptions 1 (Level-1 interrupt) and 5 (Alloca) are left to ESP-IDF.

### PS.UM flag

User processes must run with `PS.UM=1` (User Mode). This routes exceptions
through `UserExceptionVector` where PPAP's handlers are registered. With
`PS.UM=0`, exceptions hit `KernelExceptionVector` which is just
`break 1, 0` (unhandled) in ESP-IDF.

The initial process frame sets `PS = (1u << 5)` (UM=1, WOE=0, INTLEVEL=0).

### Context switch

Semi-preemptive: the timer ISR sets `xtensa_switch_pending`, and the idle
loop performs the actual switch via `sched_yield()` →
`xtensa_do_yield()` (in `switch.S`).

Context switching also happens from the SYSCALL handler: if the current
process blocks (e.g., `read()` with no data) or a preemption tick is
pending, the handler calls `sched_yield()` directly. This uses the
windowed call chain to save/restore through `xtensa_do_yield()`, then
returns to the SYSCALL handler which returns via ESP-IDF's
`_xt_context_restore` → `rfe`.

`switch.S` uses windowed ABI (`entry`/`retw`) for the kernel side. For new
processes, the `.Lnew_process` path loads entry, PS, and user SP from the
initial frame, then jumps directly (`jx`) to the user entry point.

FreeRTOS interrupt-level context switching is disabled
(`port_xSchedulerRunning[0] = 0`) so `_frxt_int_enter`/`_frxt_int_exit`
skip TCB save/restore. PPAP manages its own context switching entirely.

### Syscall restart

`svc_restart[0]` rewinds `frame->pc` by 3 (SYSCALL instruction size) and
restores `frame->a2` from `svc_saved_a0[0]`.

---

## 7. Timer

CCOMPARE0 timer at level-1 interrupt priority:
- ISR rearms `CCOMPARE0 += XTENSA_TICK_INTERVAL` each tick
- Calls `sched_timer_tick(0)` (from_user=0, no user/kernel split yet)
- `INTENABLE` set to only the CCOMPARE0 bit to prevent stray interrupts

---

## 8. Current Status

As of 2026-03-24:

### Working

- Kernel boots, VFS/fstab mount, init+getty+push shell chain starts
- User process executes — init prints "init started", vfork/exec works
- Shell prompt `$` appears (push is running and reading stdin)
- Literal pool relocation working (R_XTENSA_32, R_XTENSA_PLT)
- IRAM word-copy, PS.UM=1, MEMPROT disable, unicore mode all in place
- FreeRTOS ISR context switching disabled (`port_xSchedulerRunning=0`)
- Timer ISR working (CCOMPARE0), sets `xtensa_switch_pending`
- Cooperative context switch (idle loop → `sched_yield` → `xtensa_do_yield`)
- Fault handler: properly kills user processes and performs context switch
  (previously used `arch_yield()` which only set a flag → infinite loop)

### Known bug: init's solicited frame gets zeroed

After the first successful context-switch cycle (idle → init → push → idle),
the second yield to init crashes with `IllegalInsn` at `retw.n` in
`xtensa_do_yield`. The solicited frame saved by init (when it blocked
on vfork) has `pc=0, ps=0` — completely zeroed.

**Confirmed findings (2026-03-24):**
- The solicited frame SP (0x3fcd7ca0) is within init's stack page (valid)
- The frame was correctly saved during the first switch (exit=1, pc=valid)
- Between save and restore, the frame memory was overwritten with zeros
- Adding klogf inside `xtensa_do_switch` (slow UART output) prevents the
  hang, suggesting a timing/synchronization-related issue
- Without debug output, the new-process jump to user code appears to hang
  (no syscalls fire), but with klogf delay it works correctly
- Root cause unclear: possibly stale window state, instruction pipeline
  timing, or memory corruption from the exception/switch chain

**Theories to investigate:**
1. Window spill writes overlapping with the solicited frame memory
2. The exception return path (rfe) restoring stale PS/INTLEVEL that
   masks the timer interrupt needed for the next switch
3. IRAM instruction cache coherence (though IRAM is tightly-coupled)
4. FreeRTOS timer ISR (SYSTIMER, interrupt 12) still firing despite
   INTENABLE being limited to bit 6 (CCOMPARE0)

### Not yet implemented

- **Preemptive context switch:** currently semi-preemptive via idle loop.
  True preemptive switching (in interrupt return path) is deferred.
- **PMS (memory protection):** user/kernel separation via ESP32-S3's
  Permission Management System.
- **Dual-core:** Core 1 launch is stubbed.
- **musl/busybox:** only bare-metal user binaries; no libc.
- **Test suite:** no automated testing yet (no QEMU target; hardware only).

---

## 9. Known Gotchas

| Issue | Detail |
|-------|--------|
| IRAM byte access | LoadStoreError (cause=3). Must use 32-bit word operations. |
| MALLOC_CAP_EXEC | `(1<<0)`, NOT `(1<<4)` which is `MALLOC_CAP_PID2`. |
| PS.UM=0 | Routes to KernelExceptionVector → `break 1, 0` → crash. |
| .rela.xt.prop corruption | R_XTENSA_32 entries in metadata sections have section-internal offsets. Must filter by SHF_ALLOC. |
| L32R literal ordering | Literals must precede referencing code in linker script. |
| `sched_yield` conflict | ESP-IDF pthread provides strong symbol; renamed to `ppap_sched_yield`. |
| `idf.py set-target` | Does fullclean — wipes romfs.bin if generated first. Must run before romfs generation. |
| ninja .incbin tracking | `file(WRITE ...)` generates assembly at configure time; need `OBJECT_DEPENDS` for .incbin target. |
| Strip destroys relocations | User binaries must NOT be stripped (section headers needed for relocation). |
| `klogf` format | Only `%u`/`%x`/`%s` — no `%d`. Use `(uint32_t)` casts. |
| Fault handler yield | `arch_yield()` only sets a flag — rfe returns to faulting instruction → infinite loop. Must call `sched_yield()` from fault handler. |
| `port_xSchedulerRunning` | Must be set to 0 in `xtensa_timer_init()` to prevent FreeRTOS ISR context switching from interfering with PPAP's scheduler. |
| Docker ESP-IDF patching | ESP-IDF sources are read-only in Docker (`/opt/ppap/src/esp-idf`); vector patches (e.g., KernelExceptionVector redirect) must run as root during build. |

---

## 10. References

- [ESP32-S3 Technical Reference Manual](https://www.espressif.com/sites/default/files/documentation/esp32-s3_technical_reference_manual_en.pdf)
- [Xtensa ISA Reference Manual](https://0x04.net/~mwk/doc/xtensa.pdf)
- [ESP-IDF Programming Guide v5.x](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/)
