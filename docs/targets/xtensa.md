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
| `CONFIG_ESP_SYSTEM_MEMPROT_FEATURE` | n | Temporary until XT-4: PPAP still needs executable RAM and has not installed a final world/PMS policy yet |

### Planned handoff cleanup

The current implementation still leans on ESP-IDF runtime mechanisms in a
few places. The new plan is to reduce that over time:

- Replace the remaining ESP-IDF heap-backed memory policy with fully
  PPAP-owned RAM / flash region management. XT-2 has already introduced a
  PPAP-owned `RAM_TEXT` arena at boot; `RAM_DATA` and XIP-backed regions
  are still pending.
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
- **Boot reservation:** the current XT-2 implementation reserves a
  PPAP-owned `RAM_TEXT` arena once at boot, using `heap_caps_malloc()`
  only during `mem_region_init()`.
- **Suballocation:** executable RAM text is then allocated and freed through
  `mem_region_alloc()` / `mem_region_free()`, not by direct loader calls
  into ESP-IDF heap APIs.

These rules are architectural, but the **allocator strategy is temporary**.
The desired end state is not "ELF loader calls ESP-IDF heap APIs directly";
it is "PPAP owns explicit IRAM and DRAM regions and suballocates them with
full knowledge of protection and process lifetime."

### Execution direction

Xtensa should converge with the ARM ports on an **immutable-code-first
model**:

- larger immutable text / rodata should stage into PSRAM-backed runtime
  memory
- RAM is reserved for mutable state: `.data`, `.bss`, stack, heap, kernel
  bookkeeping, and cache-off critical routines

Under that model, IRAM is reserved for code that truly needs it:

- timer / trap / scheduler paths that must survive cache-disabled windows
- latency-sensitive routines
- bootstrap / transition stubs
- fallback execution for code that cannot yet use the staged PSRAM path

### Page pool

The PPAP page allocator uses DRAM for kernel stacks and data pages. The
page pool is configured by `mm_init()` using the DRAM range after kernel
BSS.

Longer term, Xtensa should move from a generic "page pool + special IRAM
exceptions" model to a region model such as:

- kernel IRAM for cache-off critical code
- internal IRAM for execution-adjacent allocations that genuinely need
  low-latency internal memory, such as special stacks or literal support
  areas when required by the final Xtensa layout
- PSRAM-backed user text / rodata execution space
- kernel DRAM
- user data / stack / heap DRAM
- device / DMA / framebuffer memory

That makes ownership, freeing, and future PMS policy much clearer than
address-range heuristics. For Xtensa on ESP32-S3, the intended user-space
execution model is now: storage (romfs, SD, other media) is the source of
the image, while PSRAM becomes the preferred runtime arena for larger user
text / rodata. Internal IRAM should be reserved for kernel-critical code,
special stacks, and other execution-adjacent cases that cannot tolerate the
external-memory path.

---

## 5. PIC / ELF Loading

### Current implementation vs target direction

The current Xtensa loader is **RAM-loaded**, not PSRAM-executed:

- text / literal pools are copied into IRAM
- mutable data lives in DRAM
- relocations are applied at load time

That was useful for initial bring-up, but it is not the desired end state.
The target direction is now:

- treat romfs and other filesystems as **image sources**, not executable
  mappings
- stage larger user `.text` / `.rodata` into PSRAM-backed runtime memory
- keep DRAM only for mutable process state
- reserve internal IRAM for cache-off critical code, special stacks, and
  other execution-adjacent cases that still need internal memory

ESP-IDF documents ESP32-S3 support for moving instructions and rodata into
PSRAM (`CONFIG_SPIRAM_FETCH_INSTRUCTIONS`,
`CONFIG_SPIRAM_RODATA`, `CONFIG_SPIRAM_XIP_FROM_PSRAM`), so the intended
Xtensa direction is now better described as **execute from PSRAM-backed
runtime memory**, not direct XIP from romfs.

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

For a future PSRAM-execution path, the goal is to reduce or eliminate
flash-text coupling by using a packaging model that can stage code and
immutable data into PSRAM cleanly, while keeping DRAM relocation only for
mutable process state.

### SHF_ALLOC filter (critical)

The ELF may contain `.rela.xt.prop` and `.rela.xt.lit` sections (Xtensa
metadata) with `R_XTENSA_32` entries whose `r_offset` values are
**section-internal offsets**, not image offsets. Processing them corrupts
code bytes. The loader checks `sh_info` to find each RELA section's target
section and skips non-`SHF_ALLOC` sections.

### Linker scripts

Current RAM-loaded layout:

- `src/user/arch/xtensa/user.ld`
- **text (R+X):** `.literal*` (must precede code for L32R backward reach),
  `.text.crt0`, `.text*`
- **data (RW):** `.rodata`, `.got`, `.data`, `.bss`

Experimental PSRAM/XIP-oriented packaging layout:

- `src/user/arch/xtensa/user_xip.ld`
- **text (R+X):** `.literal*`, `.text.crt0`, `.text*`, `.rodata`
- **data (RW):** `.got`, `.data`, `.bss`
- optional `__ppap_xip_flash_base` linker symbol for fixed-address
  experiments against the ESP32-S3 DROM flash window

This layout is still useful as a diagnostic artifact because it exposes
literal / relocation coupling clearly, but it is no longer the intended
final runtime path by itself. The preferred direction is to reuse the same
analysis for a staged PSRAM execution model.

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
- `MM:   ram_text ... reserved`
- `MM:   ram_data ... reserved`
- `INIT: pid=1 loaded`
- `SCHED: starting scheduler`

This confirms that the current boot-time `RAM_TEXT` / `RAM_DATA` region
reservation completes on hardware and no longer fails in
`mem_region_init()`. No further user-space progress was observed after
that point during the verification run. In particular, the earlier claims
that `init` prints, the shell chain starts, and the `$` prompt appears
should be treated as historical bring-up notes rather than current
confirmed behavior.

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

### Phase XT-2: Define a PPAP-owned memory model — **complete**

Goal: replace the current ad-hoc mix of page allocator, IRAM heap
allocation, and address-range heuristics with an explicit Xtensa memory map.

XT-2 should establish the permanent Xtensa contract:

- storage as the image source for immutable code/data
- PSRAM-backed runtime space for larger immutable user text / rodata
- DRAM for mutable process state
- IRAM only for cache-off critical or otherwise special runtime code

Progress under XT-2 should be reported by the step names below.

#### XT-2.1: Define explicit memory classes

Status: **done**

Use named memory classes rather than implicit address rules. The current
shared vocabulary is:

- `RAM_TEXT`
- `RAM_RODATA`
- `RAM_DATA`
- `EXT_TEXT`
- `EXT_RODATA`
- `ROM_TEXT`
- `ROM_RODATA`
- `RAM_STACK`
- optional `DEVICE_DMA`

This is now implemented in shared process-image metadata and used across
the ELF loader paths.

#### XT-2.2: Add explicit process-image ownership metadata

Status: **done**

Each process image should record its text, rodata, data, stack, and flags
such as XIP vs RAM-loaded. Cleanup should free what was actually allocated,
rather than infer ownership from raw addresses.

This step is implemented: process images are recorded explicitly, and the
old Xtensa-specific IRAM free heuristic has been removed.

#### XT-2.3: Introduce region allocators by purpose

Status: **done**

The loader and kernel should request memory by intent, not by backend:

- executable RAM text
- mutable process data
- kernel-private allocations

Current implementation status:

- a shared `mem_region` layer exists
- Xtensa `RAM_TEXT` is reserved once at boot and suballocated from a
  PPAP-owned arena
- Xtensa `RAM_DATA` now also goes through `mem_region`, including
  `sys_brk` growth at an explicit target address
- non-Xtensa paths still use the existing page-backed backend
- PSRAM-backed execution memory is intentionally deferred to a later step,
  so XT-2.3 closes on the current internal-memory model rather than keeping
  itself open for future execution backends

#### XT-2.4: Reserve PPAP ownership at boot

Status: **done**

Carve out PPAP-owned regions once during Xtensa bootstrap and record them
centrally. After that point, Xtensa runtime code should stop treating
ESP-IDF heap APIs as the long-term allocator interface.

Current implementation status:

- `mem_region_init()` runs during boot
- Xtensa now reserves PPAP-owned `RAM_TEXT` and `RAM_DATA` arenas there
- the Xtensa page pool has been reduced so writable process memory is not
  double-reserved at the earlier size
- the current split now boots on hardware; `scripts/run.sh xtensa_cc`
  also supports a configurable Xtensa flash baud to help with unstable
  USB transport during flashing
- PSRAM-backed execution-space reservation is intentionally deferred to the
  next step, so XT-2.4 closes on current internal-memory ownership rather
  than remaining open for future runtime arenas

#### XT-2.5: Introduce PSRAM-backed execution arenas

Status: **done**

Add Xtensa runtime regions for PSRAM-backed user execution without changing
the already-completed internal-memory groundwork from XT-2.3 and XT-2.4.

Required work for this step:

- detect and initialize the available PSRAM arena during Xtensa bootstrap
- reserve PPAP-owned PSRAM-backed regions explicitly at boot
- expose those regions through `mem_region` so later loader work can request
  execution memory by intent instead of by ESP-IDF API
- define the ownership boundary between internal IRAM support areas,
  PSRAM-backed executable/immutable regions, and DRAM-backed mutable state

XT-2.5 is intentionally the first PSRAM-specific step. Earlier steps should
remain closed and PSRAM-free.

Current implementation status:

- `xtensa_cc` now enables managed PSRAM during bootstrap with
  `SPIRAM_USE_CAPS_ALLOC`, without changing the active internal RAM-loaded
  user runtime path
- Xtensa `mem_region_init()` now detects available PSRAM, logs capacity, and
  reserves PPAP-owned `EXT_TEXT` and `EXT_RODATA` arenas at boot
- those external arenas are exposed through `mem_region`, so later loader
  work can request staged execution memory by intent instead of direct
  ESP-IDF allocation calls
- the active loader still uses the existing internal `RAM_TEXT` /
  `RAM_DATA` path; actual user-image placement into the PSRAM-backed arenas
  remains the next step

#### XT-2.6: Separate execution model from allocation model

Status: **complete**

Keep two executable paths temporarily:

- current RAM-loaded ELF path for bring-up/debug
- future staged PSRAM execution path for larger user text/rodata

Both should use the same PPAP region API so only the image format differs,
not the ownership rules.

Current implementation status:

- the current work here is still exploratory packaging and loader analysis;
  XT-2.5 now reserves real PSRAM-backed arenas, but the active loader does
  not yet place user images into them
- Xtensa RAM-loaded text now goes through `mem_region`
- Xtensa now builds separate RAM-layout and XIP-oriented user ELF variants,
  so packaging analysis can evolve independently of the current bring-up
  loader path
- those `.xip` variants are now also staged into romfs under explicit
  alternate names (for example `/bin/hello.xip`) so the loader path can be
  exercised without changing the default init image
- the loader now recognizes Xtensa XIP-layout artifacts and reports the
  first flash-unsafe text relocation that still blocks direct execution
- Xtensa inline syscall wrappers now remove the `R_XTENSA_PLT` text-reloc
  class from simple XIP-layout binaries
- XT-2.6 now treats literal / relocation support as a logical segment,
  usually backed by `RAM_DATA`, rather than assuming it must stay in
  flash-backed text
- the current experimental XIP linker layout now emits a dedicated
  `.literal` load segment ahead of `.text`, and the four absolute
  `R_XTENSA_32` relocations for static data / rodata references move from
  `.rela.text` into `.rela.literal`
- the loader and `proc_image` metadata now have explicit groundwork for a
  separate literal-support segment, instead of hard-coding "text plus data"
  as the only image shape
- the Xtensa loader now classifies `.literal` as distinct from flash text,
  so XIP-readiness checks no longer treat `.rela.literal` as a
  flash-text-relocation blocker
- `scripts/build.sh xtensa_cc` now reports each `.xip.elf` as
  `text-blocked`, `text-clean, literal-coupled`, or `XIP-clean`, so the
  remaining XT-2.6 blockers are visible in the normal build flow
- the loader now mirrors that distinction internally, recording when an
  Xtensa XIP-layout image remains `literal-coupled` even after
  flash-text relocations have been eliminated
- the current RAM-loaded fallback now models `.literal` as a logical
  `RAM_RODATA` support segment in `proc_image`, even though it still sits
  inside the IRAM allocation for L32R reach
- when an Xtensa XIP-layout image is loaded, the loader now also stages a
  full immutable text/literal copy into the `EXT_TEXT` arena and records it
  explicitly in `proc_image`, while still executing from the current IRAM
  fallback path
- staged immutable segments now preserve their original link-time virtual
  addresses in `proc_image`, and the Xtensa loader splits staged external
  executable bytes (`EXT_TEXT`) from staged immutable companion bytes
  (`EXT_RODATA`) instead of flattening everything into one external blob;
  that staged companion data is now tracked explicitly as its own process
  image segment rather than being overloaded onto the active rodata slot
- `xtensa_cc` now also enables ESP-IDF's PSRAM XiP mode for XT-2.6
  experiments, and `mem_region_init()` logs whether the reserved external
  text / rodata arenas actually land in executable or byte-accessible
  address ranges on the running system
- Xtensa now also builds fixed-base `.xipfix.elf` artifacts linked at the
  ESP32-S3 DROM flash base, so XT-2.6 can compare relocatable and
  prelinked packaging without changing the active runtime path
- those fixed-base artifacts now classify separately as
  `text-clean, literal-prelinked` when the `.literal` words already carry
  DROM flash-window addresses and the remaining relocation records are
  just preserved bookkeeping from `--emit-relocs`
- larger fixed-base programs can still classify as
  `text-clean, data-coupled` when their literal tables reference mutable
  `.data` / `.bss`, which means the remaining XT-2.6 problem is writable
  process-state rebasing rather than flash-text rebasing
- the loader now mirrors those categories internally too, recording
  `literal-prelinked`, `literal-coupled`, and `data-coupled` states in the
  process image metadata instead of collapsing everything into one generic
  “literal-coupled” bucket
- `.rela.text` is now down to `R_XTENSA_SLOT0_OP` references against code
  and the `.literal` table, which is much closer to the intended XIP model
- direct romfs-XIP is no longer the intended end state for Xtensa user
  programs; the new target is staged execution from PSRAM-backed runtime
  memory
- the active runtime still uses the internal RAM-loaded path because PPAP
  does not yet have the XT-2.5 PSRAM-backed execution arena and image
  placement logic to replace it
- **NEW (2026-03-26)**: Loader modifications now implement conditional PSRAM
  execution via `ENABLE_XTENSA_PSRAM_EXEC` compile-time flag. Entry point
  calculation and relocation patching (both RELA and GOT) now use active
  execution base (PSRAM when staged and enabled, IRAM fallback otherwise)
- **NEW (2026-03-26)**: The `ENABLE_XTENSA_PSRAM_EXEC` flag is now enabled by
  default in xtensa_cc target `CMakeLists.txt`, making PSRAM-backed execution
  the standard path for XIP-capable binaries. Non-XIP binaries automatically
  fall back to IRAM execution path

**Validation points for XT-2.6 PSRAM execution (resolved):**

- when `CONFIG_SPIRAM_XIP_FROM_PSRAM` is disabled, PSRAM arenas are
  reserved but not executable; staged copies exist but entry point
  allocation fails gracefully (logs "IRAM fallback @ 0x...")
- entry point allocation from staged text can fail if `mem_region_alloc()`
  returns NULL (insufficient PSRAM arena space); the loader falls back to
  IRAM and continues normally
- relocation patching against staged PSRAM region operates on byte-accessible
  PSRAM while the region is not yet executing; no mutual-exclusion concern
  until preemptive switching is introduced (XT-3 scope)
- loader does not execute from IRAM text when staged PSRAM path is active;
  entry point is set to `staged_text.base + e_entry` in that case
- entry address bounds validation added (2026-03-27): if the computed PSRAM
  entry falls outside `[staged_text.base, staged_text.base + staged_text.size)`,
  the loader logs a diagnostic and falls back to the IRAM entry to avoid
  executing stale or unmapped memory

#### XT-2.7: Make page-tracked writable memory explicit

Status: **complete**

Writable page-backed process memory should be handled through explicit
helpers, rather than open-coded assumptions about `user_pages[0]`,
contiguous slots, or architecture-specific cleanup paths.

Implementation outcomes:

- shared helpers now track page-backed user ranges explicitly
- `sys_brk` and the current ELF loaders use those helpers
- Xtensa tracked writable pages are now allocated and freed through
  `mem_region`, rather than assuming the generic page pool everywhere
- shared process helpers expose explicit tracked-page operations
  (`proc_first_page_backed_slot`, `proc_tracked_page_count`,
  `proc_clear_page_tracking`) so callers do not need to open-code slot-0
  and full-array assumptions
- shared process helpers also cover last-page lookup, address containment,
  and ranged tracked-page release so callers in ptrace, `sys_brk`, and
  loader/runtime setup can avoid direct `user_pages[]` traversal
- `sys_execve` now clears page tracking through the shared helper instead
  of open-coded `user_pages[]` loops, and `/proc/<pid>/stat` VSZ accounting
  now uses explicit tracked-page counting rather than direct array scans
- Human68k PMB lookup now resolves through the shared tracked-base helper
  (first tracked page), avoiding a direct `user_pages[0]` dependency
- `sys_exit`, `sys_vfork`, and `sys_execve` now route page tracking copy /
  restore / release through shared local helpers in `sys_proc.c` rather than
  repeating open-coded `USER_PAGES_MAX` loops for each path
- the `sys_proc` lifecycle paths also use shared `proc` APIs for page
  tracking snapshots and private/shared release decisions, reducing local
  duplication and keeping ownership logic in one layer
- legacy loaders (`flat`, `com`, `sos`, `x`, `r`, `m68k_emu`) now route
  tracked page registration through `proc_track_page`, and selected loader
  cleanup paths use shared tracked-page release helpers instead of direct
  `user_pages[]` clear loops
- closeout audit (2026-03-27): all direct `user_pages[i] =` slot mutations
  are verified to be contained exclusively in `proc.c`; all callers outside
  `proc` use named `proc_*` helpers
- PSRAM ownership chain verified (2026-03-27): `proc_track_page_range` in
  `elf_loader.c` correctly tracks only DRAM data pages; staged PSRAM
  text/rodata regions carry `PROC_IMAGE_SEG_OWNED` and are released by
  `image_release_owned_segments` → `image_segment_release_owned` →
  `mem_region_free` → `mem_region_free_ext_text/ext_rodata`, which is the
  correct and complete release path — no `user_pages[]` slot is required for
  PSRAM-class segments

**XT-2.6 compatibility notes (resolved):**

- XT-2.6 PSRAM execution stages text/rodata but keeps IRAM allocation for
  fallback; the split-base relocation system is compatible with XT-2.7's
  ownership model because the two memory classes (`EXT_TEXT` vs `RAM_DATA`)
  follow separate release chains that do not interfere
- writable data addresses are tracked through `user_pages[]`, while
  executable PSRAM addresses are tracked through `image.staged_text` /
  `image.staged_rodata`; both paths are freed correctly on process exit

#### XT-2.8: Make PSRAM-backed execution the default target model

Status: **complete**

The goal of XT-2.8 — moving from the internal RAM-loaded fallback to a
default staged PSRAM execution model — was achieved as part of XT-2.6.

Specifically:

- `ENABLE_XTENSA_PSRAM_EXEC=1` is set in
  `src/target/xtensa_cc/components/ppap_kernel/CMakeLists.txt`, making
  PSRAM-backed execution the default path for any XIP-capable binary
- `CONFIG_SPIRAM=y` and `CONFIG_SPIRAM_IGNORE_NOTFOUND=y` are set in
  `sdkconfig.defaults`; PSRAM arenas are disabled at runtime when the
  hardware variant has no external RAM (`esp_psram_get_size() == 0`)
- `CONFIG_SPIRAM_XIP_FROM_PSRAM` is intentionally **not** enabled:
  XIP mode requires PSRAM to be present and causes a hard boot abort
  when detection fails; staged execution uses byte-accessible PSRAM
  arenas instead, which degrade gracefully to IRAM-only fallback
- non-XIP binaries automatically fall back to IRAM execution; the
  fallback is now guarded by the entry-bounds check added in XT-2.6
- larger user text / rodata from XIP-capable binaries runs from the
  PSRAM-backed `EXT_TEXT` / `EXT_RODATA` arenas reserved in XT-2.5
- no ad-hoc ESP-IDF heap calls remain in the loader or `mem_region`
  layer; all allocation goes through named `mem_region_alloc` paths

**XT-2 exit criteria (all satisfied):**

- Xtensa memory ownership is described in named regions, not
  address-range heuristics — satisfied by XT-2.1 through XT-2.5
- the loader no longer depends on ad-hoc ESP-IDF heap calls as its
  architectural interface — satisfied by XT-2.3/XT-2.4 (`mem_region`
  owns all region allocation; no `heap_caps_malloc` in loader paths)
- process cleanup is explicit and format-aware — satisfied by XT-2.7
  (`image_release_owned_segments` dispatches by segment ownership and
  memory class; `proc_*` helpers own page-tracking lifecycle)
- the documentable default model is "storage-backed image source,
  PSRAM-backed executable / immutable runtime state, DRAM-backed
  mutable process state" — satisfied by XT-2.5 + XT-2.6 + XT-2.8

**Phase XT-2 is complete.**

### Phase XT-3: Reclaim runtime control from ESP-IDF

Goal: keep ESP-IDF as bootstrap infrastructure while reducing dependence on
its runtime services after `app_main()`.

Status: **in progress**

#### XT-3.1: Establish PPAP-owned interrupt/timer handoff

Status: **complete**

- PPAP disables FreeRTOS ISR-level context switching at timer init by forcing
  `port_xSchedulerRunning[0] = 0`
- PPAP installs the CCOMPARE0 timer ISR via `xt_set_interrupt_handler()`;
  direct `_xt_interrupt_table` patching in this path was attempted and then
  reverted after hardware startup regression during XT-3 bring-up
- `target_early_init()` continues to clear SYSTIMER alarm sources and CPU
  interrupt enable state before PPAP scheduling starts

#### XT-3.2: Make PPAP the explicit syscall/fault policy owner

Status: **in progress**

- syscall and fault handling are already centralized in
  `src/arch/xtensa/xtensa_common.c`
- exception handler registration still goes through ESP-IDF helper hooks;
  next step is to isolate or replace this path with a PPAP-owned mechanism

#### XT-3.3: Move steady-state device control to MMIO-first paths

Status: **not started**

- migrate timers, interrupt routing control, GPIO, UART, SPI, and I2C
  runtime policy to direct MMIO access where practical
- keep ESP-IDF calls only where vendor boot/clock/cache coupling is required

#### XT-3.4: Define and enforce the bootstrap boundary contract

Status: **not started**

- define exactly what remains ESP-IDF-owned after `app_main()`
- assert PPAP ownership for syscall/exception/timer/scheduler policy and
  maintain this boundary in build/runtime checks

### Phase XT-4: Reintroduce protection cleanly

Goal: turn memory protection back on only after the software memory model is
explicit enough to express PPAP policy without hacks.

- Design a PMS layout for kernel vs user separation on ESP32-S3.
- Plan to use **World 0** for the kernel / supervisor runtime and
  **World 1** for user processes, so the world controller becomes the
  coarse kernel-vs-user boundary beneath finer PMS permissions.
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
- Add a PSRAM-execution-capable packaging path so Xtensa programs can follow
  the same "immutable executable image separated from mutable state"
  approach without depending on direct romfs XIP.
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
