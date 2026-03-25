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

PPAP currently uses **ESP-IDF for bootstrap and vendor bring-up**, not as
the long-term owner of the machine. The kernel is built as an ESP-IDF
component so it can reuse the existing boot flow, toolchain packaging, and
chip-specific initialization that would be tedious to rediscover from
scratch on ESP32-S3.

### Current role of ESP-IDF

Today ESP-IDF still provides or influences:

- Flash boot, cache setup, clock PLL
- Toolchain / build / flash integration
- Early SoC initialization before `app_main()`
- Some heap-backed memory allocation used during Xtensa bring-up
- Exception / interrupt registration hooks used by the current port

### Intended ownership boundary

The target direction is: **ESP-IDF gets us to a known-good `app_main()`
environment, then PPAP takes control of the runtime.**

After handoff, PPAP should own:

- Scheduler tick source and interrupt policy
- Exception vectors and syscall/fault handling
- Memory layout, region allocators, and process image loading
- Memory protection policy (PMS) and user/kernel boundaries
- Direct peripheral access where practical: GPIO, SPI, I2C, UART, timers
- Core-1 bring-up policy if SMP is enabled later

ESP-IDF should remain in the picture only where it adds real value:

- Boot ROM / second-stage bootloader integration
- Clock and cache setup
- Flash services and vendor-specific low-level init
- Potential future Wi-Fi/BLE firmware/bootstrap hooks if PPAP chooses to use
  them

This means the current Xtensa port should be understood as a **bootstrap
phase**, not the final software architecture.

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

### Planned handoff cleanup

The current implementation still leans on ESP-IDF runtime mechanisms in a
few places. The new plan is to reduce that over time:

- Replace ad-hoc `heap_caps_malloc()` executable allocations with PPAP-owned
  IRAM / DRAM arenas reserved at boot
- Move from ESP-IDF exception registration hooks toward PPAP-owned runtime
  exception control as much as the ROM / boot model allows
- Re-enable PMS once the PPAP memory map is explicit enough to express
  user/kernel policy cleanly
- Access board peripherals via PPAP drivers talking to MMIO directly, rather
  than treating ESP-IDF as the steady-state HAL

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
access.

For the current bring-up implementation, user text is loaded into IRAM.
That is now considered an **interim strategy**, not the long-term memory
model for the port.

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

These rules are architectural, but the **allocator strategy is temporary**.
The desired end state is not "ELF loader calls ESP-IDF heap APIs directly";
it is "PPAP owns explicit IRAM and DRAM regions and suballocates them with
full knowledge of protection and process lifetime."

### XIP direction

Xtensa should converge with the ARM ports on an **XIP-first model**:

- immutable text executes in place from flash when practical
- read-only data stays flash-backed when practical
- RAM is reserved for mutable state: `.data`, `.bss`, stack, heap, kernel
  bookkeeping, and cache-off critical routines

Under that model, IRAM is reserved for code that truly needs it:

- timer / trap / scheduler paths that must survive cache-disabled windows
- latency-sensitive routines
- bootstrap / transition stubs
- fallback execution for code that cannot yet use XIP

### Page pool

The PPAP page allocator uses DRAM for kernel stacks and data pages. The
page pool is configured by `mm_init()` using the DRAM range after kernel
BSS.

Longer term, Xtensa should move from a generic "page pool + special IRAM
exceptions" model to a region model such as:

- kernel IRAM for cache-off critical code
- flash-mapped XIP text / rodata
- kernel DRAM
- user data / stack / heap DRAM
- device / DMA / framebuffer memory

That makes ownership, freeing, and future PMS policy much clearer than
address-range heuristics, and aligns Xtensa with the existing ARM-port XIP
philosophy: keep code in flash unless there is a concrete reason it must
consume internal RAM.

---

## 5. PIC / ELF Loading

### Current implementation vs target direction

The current Xtensa loader is **RAM-loaded**, not XIP:

- text / literal pools are copied into IRAM
- mutable data lives in DRAM
- relocations are applied at load time

That was useful for initial bring-up, but it is not the desired end state.
The target direction is to follow the ARM ports more closely:

- flash-mapped XIP text where practical
- flash-backed read-only data where practical
- DRAM only for mutable process state
- IRAM reserved for cache-off critical code and other special cases

So the correct reading is: **Xtensa does not currently use XIP for user
programs, but the plan is to move in that direction.**

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

For a future XIP path, the goal is to reduce or eliminate text relocation by
using a more flash-friendly packaging model, such as prelinked or otherwise
XIP-aware binaries, while keeping DRAM relocation only for mutable data.

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

As of 2026-03-26:

### Working

- `scripts/build.sh xtensa_cc` completes successfully via Docker
- `scripts/run.sh xtensa_cc` flashes successfully via Docker + esptool
- Hardware boot reaches PPAP kernel startup on the CardComputer
- VFS/fstab mount path runs successfully
- PID 1 load path runs far enough to print `INIT: pid=1 loaded`
- Boot reaches `SCHED: starting scheduler`
- Literal pool relocation working (R_XTENSA_32, R_XTENSA_PLT)
- IRAM word-copy, PS.UM=1, MEMPROT disable, unicore mode all in place
- FreeRTOS ISR context switching disabled (`port_xSchedulerRunning=0`)
- Timer ISR working (CCOMPARE0), sets `xtensa_switch_pending`
- Cooperative context switch (idle loop → `sched_yield` → `xtensa_do_yield`)
- Fault handler: properly kills user processes and performs context switch
  (previously used `arch_yield()` which only set a flag → infinite loop)

### Verified hardware observation

On 2026-03-26, the current image was built and flashed with the standard
repo workflow:

- `./scripts/build.sh xtensa_cc`
- `PPAP_PORT=/dev/ttyACM0 ./scripts/run.sh xtensa_cc`

Observed serial output reaches:

- `PiPAPo booting... [xtensa_cc]`
- memory map / VFS mount logs
- `INIT: pid=1 loaded`
- `SCHED: starting scheduler`

No further user-space progress was observed after that point during the
verification run. In particular, the earlier claims that `init` prints,
the shell chain starts, and the `$` prompt appears should be treated as
historical bring-up notes rather than current confirmed behavior.

### Known runtime bug: scheduler handoff remains unstable

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

### XT-1 alignment

Phase XT-1 is the correct active focus for the port, but it is **not yet
complete**.

Already aligned with XT-1:

- Standard build / flash workflow works
- Hardware boot reproducibly reaches PPAP scheduler startup
- The active blocker is runtime stability, not basic toolchain bring-up
- The known failure mode sits squarely in XT-1 scope: scheduler / frame /
  exception handoff correctness

Still required for XT-1 completion:

- Confirm repeatable forward progress after `sched_start()`
- Root-cause and fix the saved-frame corruption / stalled handoff
- Re-verify blocking syscalls, yield/resume, `vfork()`, and `execve()`
- Remove timing-sensitive behavior changes caused by debug logging

---

## 9. Development Plan

This plan is for the **Xtensa port itself**: runtime ownership, trap model,
memory layout, scheduler correctness, and validation. It intentionally does
**not** cover CardComputer-specific peripherals such as LCD, keyboard, or
microSD.

### Phase XT-1: Stabilize the current runtime

Goal: make the existing single-core bootstrap path reliable enough that the
same user process can yield, block, resume, exec, and fault repeatedly
without timing-sensitive behavior.

- Reproduce and root-cause the solicited-frame corruption described in
  §8, especially around window spill / restore ordering and exception
  return state.
- Remove timing-sensitive debug dependencies (`klogf` changing behavior).
- Verify that `read()`, `poll()`, `nanosleep()`, `vfork()`, `execve()`, and
  signal-related wakeups survive many scheduler cycles.
- Tighten invariants around saved SP / PS / return PC so bad frames are
  detected early with explicit diagnostics.

### Phase XT-2: Define a PPAP-owned memory model

Goal: replace the current ad-hoc mix of page allocator, IRAM heap
allocation, and address-range heuristics with an explicit Xtensa memory map.

XT-2 should establish the permanent Xtensa contract:

- flash for immutable code/data by default
- DRAM for mutable process state
- IRAM only for cache-off critical or otherwise special runtime code

Recommended steps:

1. Define the memory classes explicitly.
   Use named regions rather than implicit address rules:
   - `kernel_iram`
   - `kernel_dram`
   - `xip_text`
   - `xip_rodata`
   - `user_dram`
   - optional `device_dma`

2. Reserve PPAP ownership at boot.
   Carve out PPAP-owned regions once during Xtensa bootstrap and record
   them centrally. After that point, Xtensa runtime code should stop
   treating ESP-IDF heap APIs as the long-term allocator interface.

3. Introduce region allocators by purpose.
   The loader and kernel should request memory by intent, not by backend:
   - executable cache-off critical code
   - flash-backed immutable text/rodata
   - mutable process data
   - kernel-private allocations

4. Separate execution model from allocation model.
   Keep two executable paths temporarily:
   - current RAM-loaded ELF path for bring-up/debug
   - future XIP-capable path for flash-backed text/rodata
   Both should use the same PPAP region API so only the image format
   differs, not the ownership rules.

5. Add an explicit process-image descriptor.
   Process teardown should free what was actually allocated, rather than
   infer ownership from raw addresses. Each process image should record its
   text, rodata, data, stack, and flags such as XIP vs RAM-loaded.

6. Make XIP the default target model.
   XT-2 should deliberately align Xtensa with the ARM-port philosophy of
   "XIP by default, RAM only when needed" so non-MMU ports converge on one
   mental model instead of each inventing its own special-case rules.

Exit criteria for XT-2:

- Xtensa memory ownership is described in named regions, not address-range
  heuristics
- the loader no longer depends on ad-hoc `heap_caps_malloc()` calls as its
  architectural interface
- process cleanup is explicit and format-aware
- the documentable default model becomes "flash-backed immutable code/data,
  DRAM-backed mutable state"

### Phase XT-3: Reclaim runtime control from ESP-IDF

Goal: keep ESP-IDF as bootstrap infrastructure while reducing dependence on
its runtime services after `app_main()`.

- Minimize reliance on ESP-IDF exception registration hooks where possible,
  and make PPAP the clear owner of syscall / fault / timer policy.
- Continue disabling FreeRTOS runtime behavior that conflicts with PPAP, but
  move toward a cleaner handoff model instead of accumulating one-off stubs.
- Prefer direct MMIO access for timers, interrupt control, GPIO, SPI, I2C,
  and UART in steady-state runtime code.
- Treat ESP-IDF as the launch environment for boot, clock/cache bring-up,
  flashing, and vendor-sensitive initialization only.

### Phase XT-4: Reintroduce protection cleanly

Goal: turn memory protection back on only after the software memory model is
explicit enough to express PPAP policy without hacks.

- Design a PMS layout for kernel vs user separation on ESP32-S3.
- Enforce at least coarse user/kernel boundaries before attempting finer
  protection.
- Aim for W^X-style behavior where practical: flash-mapped executable text,
  writable user data in DRAM, no generic executable heap.
- Make protection configuration derive from the PPAP-owned region model from
  XT-2, not from hard-coded exceptions in the loader.

### Phase XT-5: Improve the scheduler model

Goal: move from the current semi-preemptive bring-up design to a more
principled Xtensa scheduling model.

- Keep cooperative switching as the debugging baseline until XT-1 is stable.
- Evaluate whether true preemptive switching should happen in the exception /
  interrupt return path or remain a deliberate deferred switch model.
- Clarify the contract between timer ISR, syscall handler, and switch code so
  only one component owns each state transition.
- Document the final rule for windowed-kernel / call0-user interaction,
  including new-process entry, blocking syscalls, and restart behavior.

### Phase XT-6: User-space maturity

Goal: move the Xtensa port from raw syscall test binaries toward normal PPAP
userland without destabilizing the port.

- Keep the current small freestanding binaries as bring-up tools until XT-1
  through XT-5 are solid.
- Add an XIP-capable executable packaging path so preinstalled Xtensa
  programs can follow the same "code in flash, mutable state in RAM"
  approach already used on ARM.
- Add musl support only after the process ABI, loader, and signal/restart
  behavior are stable.
- Defer busybox until libc, process startup, and TTY behavior are reliable.
- Treat userland growth as a validation stage for the port, not as the means
  to discover basic scheduler or memory bugs.

### Phase XT-7: Validation and regression strategy

Goal: make the Xtensa port measurable and repeatable even before a full
emulator exists.

- Add focused kernel and user regression tests for Xtensa-only failure modes:
  IRAM word access, relocation correctness, blocking syscall restart,
  exception-to-scheduler handoff, and repeated exec/fork/yield cycles.
- Add lightweight self-checks and counters in debug builds so frame
  corruption or illegal state transitions are caught near the source.
- Keep hardware-driven smoke tests small and deterministic until there is a
  better automated environment.
- If a practical emulator or harness becomes available later, treat it as a
  multiplier for this test strategy, not a prerequisite for basic coverage.

### Explicit non-goals of this plan

- LCD / framebuffer console
- Keyboard scanning
- SD card / FAT integration
- Audio, IR, Wi-Fi, BLE, or board-specific UX work

Those belong to the CardComputer target plan, not the Xtensa port plan.

---

## 10. Known Gotchas

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

## 11. References

- [ESP32-S3 Technical Reference Manual](https://www.espressif.com/sites/default/files/documentation/esp32-s3_technical_reference_manual_en.pdf)
- [Xtensa ISA Reference Manual](https://0x04.net/~mwk/doc/xtensa.pdf)
- [ESP-IDF Programming Guide v5.x](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/)
