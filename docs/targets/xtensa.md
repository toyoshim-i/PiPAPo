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
| Syscall | `ill` instruction / EXCCAUSE=0 (a7=number, a2-a6=args) |
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
- **Setup:** `./scripts/setup.sh xtensa`
- **Activation:** Automatic inside `ppap/xtensa` Docker container

The Xtensa toolchain is **chip-specific** — unlike ARM/RISC-V where one
toolchain covers many chips, each Xtensa configuration (ESP32, ESP32-S2,
ESP32-S3) has its own GCC build because the ISA is configurable per chip
(window size, DSP options, interrupt levels, etc.).

### User-space

- Same `xtensa-esp32s3-elf-gcc` toolchain (call0 ABI)
- Compiled directly in `scripts/build.sh` (not via CMake)
- Flags: `-mabi=call0 -ffreestanding -nostdlib -Os -fPIC
  -ffunction-sections -fdata-sections -Wl,--emit-relocs
  -Wl,--gc-sections`
- Links against PPAP libc (`src/user/lib/`) like every other target.

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
| `CONFIG_ESP_SYSTEM_MEMPROT_FEATURE` | n | Temporary: PPAP still needs executable RAM and has not installed a final world/PMS policy yet |

### Planned handoff cleanup

The current implementation still leans on ESP-IDF runtime mechanisms in a
few places. The new plan is to reduce that over time:

- Replace the remaining ESP-IDF heap-backed memory policy with fully
  PPAP-owned RAM / flash region management.  Boot already reserves a
  PPAP-owned `RAM_TEXT` arena; `RAM_DATA` and XIP-backed regions are
  still pending.
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
- **Boot reservation:** boot reserves a PPAP-owned `RAM_TEXT` arena
  once at boot, using `heap_caps_malloc()` only during
  `mem_region_init()`.
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
link-time base `0x0`. When loaded at non-zero IRAM/DRAM addresses, these
values must be relocated to the actual runtime addresses.

**Build side:** user binaries are compiled with `-Wl,--emit-relocs` to
preserve relocation entries in the ELF. Binaries are NOT stripped (strip
would destroy section headers needed for relocation scanning).

Xtensa user binaries do **not** use `.rela.dyn` or GOT/PLT for text
relocations. Splitting literal-pool relocations into GOT/PLT was
investigated and abandoned — the Xtensa L32R instruction encodes a
negative PC-relative offset into the literal pool, so the literal words
must remain in the text segment (IRAM) within L32R reach of the code
that references them. The only relocation mechanism is `--emit-relocs`
`.rela.text`, processed during the SRAM copy at load time.

**Loader side:** the Xtensa `elf_reloc_arch()` in `elf_loader.c` scans
all `SHT_RELA` sections (including `.rela.text`) for:
- `R_XTENSA_32` (type 1) — absolute 32-bit data (literal pool values,
  initialized data pointers)
- `R_XTENSA_PLT` (type 6) — PLT-resolved function addresses in literal pool

For each entry, the loader reads the link-time value at `r_offset` and
applies a **split relocation** via `elf_split_addr()`:
- if the link-time address falls in the text range (< `data_va`):
  relocated value = `text_base + link_addr`
- if the link-time address falls in the data range (≥ `data_va`):
  relocated value = `data_base + (link_addr - data_va)`

This split is necessary because Xtensa loads text into IRAM and data
into DRAM at independent base addresses. A single `load_base` offset
would produce wrong addresses for literal pool entries that reference
the data segment (rodata strings, initialized data pointers, etc.).

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
instructions. ESP-IDF dispatches level-1 exceptions through
`_xt_exception_table`, but **intercepts EXCCAUSE=1 (Syscall)** with a
hardcoded stub (`_xt_syscall_exc` in `xtensa_vectors.S`) that returns -1
without dispatching through the table. This means handlers registered in
`_xt_exception_table[1]` are never called for `syscall` instructions.

PPAP works around this by using the `ill` (illegal instruction) opcode
as the syscall trap instead of `syscall`. EXCCAUSE=0 (IllegalInstruction)
falls through to the table dispatch. The combined handler at table index 0
reads the 3-byte instruction at EPC1: if it is `ill` (0x000000), it
dispatches as a syscall; otherwise, it falls through to the fault handler.

| EXCCAUSE | Handler | Action |
|----------|---------|--------|
| 0 (IllegalInsn) | `xtensa_ill_handler` | If opcode=ILL → syscall; else fault |
| 1 (Syscall) | `xtensa_fault_handler` | Safety net (ESP-IDF intercepts first) |
| 2-29 (others) | `xtensa_fault_handler` | Kill user process or kernel panic |

Exceptions 4 (Level-1 interrupt) and 5 (Alloca) are left to ESP-IDF.

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

`syscall_restart[0]` rewinds `frame->pc` by 3 (SYSCALL instruction size) and
restores `frame->a2` from `syscall_saved_arg0[0]`.

---

## 7. Timer

CCOMPARE0 timer at level-1 interrupt priority:
- ISR rearms `CCOMPARE0 += XTENSA_TICK_INTERVAL` each tick
- Calls `sched_timer_tick(0)` (from_user=0, no user/kernel split yet)
- `INTENABLE` set to only the CCOMPARE0 bit to prevent stray interrupts

---

## 8. Known Gotchas

| Issue | Detail |
|-------|--------|
| ESP-IDF syscall stub | `_xt_user_exc` intercepts `EXCCAUSE_SYSCALL` (1) with a hardcoded `beqi` branch to `_xt_syscall_exc`, which returns `-1` without dispatching through `_xt_exception_table`. PPAP uses `ill` (EXCCAUSE=0) as the syscall trap instead, avoiding the intercept entirely. |
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

## 9. References

- [ESP32-S3 Technical Reference Manual](https://www.espressif.com/sites/default/files/documentation/esp32-s3_technical_reference_manual_en.pdf)
- [Xtensa ISA Reference Manual](https://0x04.net/~mwk/doc/xtensa.pdf)
- [ESP-IDF Programming Guide v5.x](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/)
