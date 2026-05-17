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

ESP32-S3 internal SRAM is split into three physical banks, not two:

| Bank | Size | IRAM alias | DRAM alias | Notes |
|------|------|------------|------------|-------|
| SRAM0 | 32 KB | `0x40370000`-`0x40377FFF` | none | Cache / instruction-only |
| SRAM1 | 416 KB | `0x40378000`-`0x403DFFFF` | `0x3FC88000`-`0x3FCEFFFF` | **Dual-mapped** — same physical bytes accessible from either bus |
| SRAM2 | 64 KB | none | `0x3FCF0000`-`0x3FCFFFFF` | Data-only |

The IRAM and DRAM aliases of SRAM1 point at the same physical RAM.
Which alias you use determines which CPU bus and which cache services
the access:

- IRAM alias → instruction bus → instruction cache, **32-bit aligned
  loads/stores only**. Byte writes through this alias cause
  `LoadStoreError` (cause=3).
- DRAM alias → data bus → data cache, byte-granular reads and writes.

So "executable" is not a per-page attribute on ESP32-S3 — it is the
address range you reach the bytes through. ESP-IDF's heap tracks
allocations by the alias the caller asked for (`MALLOC_CAP_EXEC` gives
IRAM-alias pointers; `MALLOC_CAP_8BIT` gives DRAM-alias pointers).
Freeing via the wrong alias is undefined; mixing reads and writes
across the two aliases requires explicit cache management
(`Cache_WriteBack_Addr` + `Cache_Invalidate_Addr`) to avoid the
instruction cache serving stale bytes after a data-alias write.

For the current bring-up implementation, user text is loaded into IRAM
via the `MALLOC_CAP_EXEC` heap. That is now considered an **interim
strategy**, not the long-term memory model for the port — the
dual-mapping is what would let a future unification stage user text
through a single SRAM1 region accessed via both aliases.

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

### Cooperative RAM use with ESP-IDF

PPAP does **not** own any region of the ESP32-S3 SRAM by linker
script.  ESP-IDF still owns its `.data`, `.bss`, FreeRTOS task stacks,
driver buffers, and the runtime heap.  PPAP rents from that heap at
boot via `heap_caps_malloc()` and then suballocates inside what it
got.  That is what "cooperative" means in this port — the budget is
not fixed by a link map, it is whatever ESP-IDF's heap is willing to
hand over the first time `mm_init()` and `mem_region_init()` ask.

The current xtensa_cc rentals (matching the `MM:` boot banner) are:

| Arena | Bus / cap | Allocator | Default | Purpose |
|-------|-----------|-----------|---------|---------|
| `kernel` | DRAM, linker-reserved | ESP-IDF link map (`__bss_end`) | `~32 KB` | PPAP `.data` + `.bss` (kernel image — not allocated at runtime). |
| `pages` | DRAM, `MALLOC_CAP_8BIT \| MALLOC_CAP_INTERNAL` | [page.c `mm_init()`](/src/kernel/core/mm/page.c) — slot-based 4 KB free stack | `PAGE_COUNT_MAX × PAGE_SIZE` = `192 KB` (48 × 4 KB) | Generic 4 KB pages, the **single DRAM pool** on xtensa.  Used for kernel stacks (one per PCB), user stack pages, `PPAP_MEM_RAM_DATA` allocations (ELF data/bss, eCPU state, `sys_brk` heap, tmpfs/UFS pages), and any FD-side buffers.  Multi-page requests use `mm_page_alloc_contiguous` (linear scan). |
| `ram_text` | IRAM, `MALLOC_CAP_EXEC` | [mem_region.c `mem_region_xtensa_text_init`](/src/kernel/core/mm/mem_region.c) — linear free-list with 16-byte alignment | `MEM_REGION_RAM_TEXT_ARENA_SIZE` = `128 KB` | `PPAP_MEM_RAM_TEXT`: user ELF `.text` and other executable suballocations.  Kept separate from the page pool because xtensa user text MUST live in IRAM (instruction bus), and the page pool is DRAM (data bus).  The linear allocator can serve non-page-aligned sizes (e.g. an 1820-byte `.text` consumes 1824 bytes, not a full 4 KB page). |
| `ext_text` | PSRAM (`MALLOC_CAP_SPIRAM`) | same linear arena | `512 KB` | Reserved for staged user text on chips with PSRAM.  CardComputer has no PSRAM, so this arena stays disabled (`psram unavailable; external arenas disabled`). |
| `ext_rodata` | PSRAM | same | `256 KB` | Same — PSRAM staging for large rodata. |

Totals on xtensa_cc (no PSRAM): about **352 KB of internal SRAM**
under PPAP — 224 KB DRAM plus 128 KB IRAM.  ESP32-S3 has ~512 KB of
internal SRAM, so ESP-IDF retains roughly the other ~160 KB for its
own state (FreeRTOS tasks, driver buffers, the WiFi/BT subsystem on
chips that use them, and the runtime heap PPAP draws from).

Per-target tuning lives in
[src/target/xtensa_cc/esp_idf/components/ppap_kernel/CMakeLists.txt](/src/target/xtensa_cc/esp_idf/components/ppap_kernel/CMakeLists.txt)
as `target_compile_definitions`:

- `PAGE_COUNT_MAX` — drives the `pages` arena (page allocator).
- `MEM_REGION_RAM_TEXT_ARENA_SIZE` — drives `ram_text` (IRAM linear
  arena).
- `MEM_REGION_EXT_TEXT_ARENA_SIZE` / `MEM_REGION_EXT_RODATA_ARENA_SIZE`
  — PSRAM-only, no-op on chips without PSRAM.

Raising any of these widens the rental from ESP-IDF.  Both arenas
**auto-downsize** at boot: if the requested size exceeds the largest
contiguous free block ESP-IDF can hand over, `mm_init()` /
`mem_region_linear_arena_init()` halve the request until
`heap_caps_malloc` succeeds, and log the actual reserved size if it
differs from the requested one (`page pool downsized to N pages` or
`ram_text … KB reserved (requested … KB)`).

### Why `+PAGE_SIZE` / `+MEM_REGION_ALIGN` slack on `heap_caps_malloc`?

`heap_caps_malloc` returns a region aligned to its internal allocator
metadata, not to PAGE_SIZE or to 16 B.  Each arena init asks for
`requested_size + alignment - 1` and rounds the returned pointer up to
the boundary it needs (`PAGE_SIZE` for the page-aligned arenas,
`MEM_REGION_ALIGN = 16` for the linear arenas).  The slack is the
worst-case adjustment so the *usable aligned* region is still at least
`requested_size`.  Once aligned, the kernel records the new base and
the prefix bytes are abandoned (never reachable but also never
returned to the IDF heap).

### Why there is no separate `ram_data` arena

Earlier revisions kept a second `ram_data` arena alongside the page
pool, also `MALLOC_CAP_8BIT` internal DRAM, also 4 KB-page-tracked.
The two were sized independently (96 KB pages + 96 KB ram_data) on
the theory that segregating stack-page churn from ELF / eCPU
allocations would prevent fragmentation from blocking contiguous
multi-page requests.

In practice the split hurt more than it helped — neither pool
individually had enough contiguous space for a binary that the
merged pool would have served easily, which produced the original
OOM cascade observed in `test_cpm` / `test_sos` (those tests failed
at `execve` before reaching their internal asserts).  Xtensa now
follows every other arch and routes `PPAP_MEM_RAM_DATA` through
[`mem_region_alloc_page_backed`](/src/kernel/core/mm/mem_region.c) →
`mm_page_alloc_contiguous`, drawing from the single page pool.  The
freed-up 96 KB went straight into `PAGE_COUNT_MAX` (24 → 48), so the
total DRAM rental is unchanged.

`ram_text` remains separate because it has a hard functional reason:
xtensa user text must live on the instruction bus (IRAM alias), not
the data bus (DRAM alias) that the page pool comes from.

### Release path and free-list cap

`mem_region_linear_arena_free` (used by `ram_text` and the PSRAM
arenas) returns the freed block to the arena's `free[]` array,
address-sorted, and coalesces with adjacent neighbours on both sides.
Tracking is correct in the normal case, with two caveats worth
knowing:

- **`MEM_REGION_FREE_MAX = 16` entries** caps the free list.  If
  enough fragmented frees pile up that the count would exceed 16, the
  arena logs `MM: <name> free-list overflow` and the freed block
  becomes unreachable.  Recovery requires a coalesce-friendly free
  pattern reducing the count below the cap.
- **Arenas are never returned to ESP-IDF.**  Once `heap_caps_malloc`
  hands the chunk to PPAP at boot, there is no path that calls
  `heap_caps_free` on it.  A build that never exec's a user binary
  still keeps the full `ram_text` arena tied up.  This matters if a
  future feature wants to reclaim PPAP-owned SRAM for ESP-IDF (e.g.
  switching off the test/runtime split at runtime).

Page-pool frees (now including `PPAP_MEM_RAM_DATA`) go through
`mm_page_free` — no free-list cap, no coalescing required because
the allocator only tracks individual page slots.

### Changing page attributes from PPAP

ESP32-S3 has no MMU.  There is no per-page exec/data flag to toggle
at runtime — the access semantics come from which alias (IRAM vs
DRAM) you reach the bytes through (see [ESP32-S3 SRAM split](#esp32-s3-sram-split)).
The Permission Management System (PMS) does support coarse-region
exec/read/write control, but ESP-IDF locks down its configuration at
boot and the regions are fixed at chip level.

What this means concretely:

- **No "make this page executable" call exists** for PPAP to make.
- **To execute code in a SRAM1 page**, allocate it with
  `MALLOC_CAP_EXEC` (forces an IRAM-alias pointer); write through the
  DRAM alias of the same physical bytes if you need byte-level
  stores, then `Cache_WriteBack_Addr` / `Cache_Invalidate_Addr`
  before jumping to the IRAM alias to avoid the instruction cache
  serving stale bytes.
- **The current loader does not exercise this path** — it writes user
  `.text` into IRAM via word-aligned copy loops (per
  [IRAM restrictions](#iram-restrictions)) and reads through the same
  IRAM alias.  Dual-alias use is on the table for a future unification
  step but is not a per-page attribute change in the MMU sense.

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
