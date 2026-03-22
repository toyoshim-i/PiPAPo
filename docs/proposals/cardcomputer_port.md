# M5Stack CardComputer Target Port Plan

Porting PPAP to the M5Stack CardComputer (based on STAMP S3 / ESP32-S3),
introducing the Xtensa LX7 architecture — the fourth ISA for PPAP.

The CardComputer integrates a 240x135 IPS display, a 56-key keyboard,
speaker, IR transmitter, and a microSD slot in a pocket-sized form factor,
making it an attractive self-contained PPAP terminal.

---

## 1. ESP32-S3 / Xtensa LX7 Cores

### 1.1 ISA Overview

The ESP32-S3 contains dual Xtensa LX7 cores designed by Cadence (licensed
by Espressif):

| Feature | Xtensa LX7 (ESP32-S3) |
|---------|----------------------|
| ISA | **Xtensa LX7** (32-bit, configurable) |
| Pipeline | 5-stage, in-order, dual-issue |
| Cores | 2 (up to 240 MHz each) |
| Registers | 64 GPR (windowed: 16 visible at a time, 4-entry rotation) |
| FPU | Single-precision (optional, present on ESP32-S3) |
| SIMD | PIE (Processor Instruction Extension) for AI acceleration |
| Interrupts | 32 vectored interrupts, 7 priority levels |
| MMU/MPU | Region-based PMS (Permission Management System) |
| Endianness | Little-endian |
| Caches | 32 KB I-cache + 32 KB D-cache |
| ABI | Windowed ABI (default) or Call0 ABI (flat register file) |

### 1.2 Xtensa LX7 vs Other PPAP Architectures

| Aspect | Cortex-M0+ (pico1) | Cortex-M33 (pico2) | Hazard3 RV32 (pico2rv) | Xtensa LX7 (xtensa_cc) |
|--------|--------------------|--------------------|----------------------|--------------------------|
| ISA | Thumb (ARMv6-M) | Thumb-2 (ARMv8-M) | RV32IMAC | Xtensa LX7 |
| GPRs | 16 | 16 | 32 (x0=zero) | 64 (16 visible, windowed) |
| FPU | None | VFPv5-SP | None | Single-precision |
| Memory protection | MPU (8 regions) | MPU (8 regions) | PMP (8 regions) | PMS (regions configurable) |
| Exception model | NVIC (vectored) | NVIC (vectored) | CLINT (trap-based) | Vectored (32 interrupts) |
| Context switch | PendSV | PendSV | Software trap | Software interrupt |
| Privilege | Privileged/Unpriv | Privileged/Unpriv | M-mode/U-mode | Kernel/User (ring 0/3) |
| Atomics | None | LDREX/STREX | AMO (lr/sc) | S32C1I (compare-and-swap) |
| Code density | 16/32 bit | 16/32 bit | 16/32 bit (RVC) | 24-bit (dense) |
| Clock | 133 MHz | 150 MHz | 150 MHz | 240 MHz |
| SRAM | 264 KB | 520 KB | 520 KB | 512 KB |

### 1.3 Xtensa-Specific Considerations

**Register windowing** is the key difference from all other PPAP
architectures.  The Xtensa LX7 has a physical register file of 64
registers.  A "window" of 16 registers is visible at a time (a0-a15).
Function calls (`CALL4`/`CALL8`/`CALL12`) rotate the window by 4/8/12
registers.  When the window overflows, a `WindowOverflow` exception spills
registers to the stack.

For PPAP, we will use the **Call0 ABI** (flat register file, no windowing)
to simplify the context switch and exception handling.  The Call0 ABI uses
a0 (return address), a1 (stack pointer), and a2-a15 as
arguments/temporaries — conceptually similar to a RISC architecture with 16
GPRs.  ESP-IDF supports Call0 ABI via `-mabi=call0`.

**Why Call0 ABI:**
- No window overflow/underflow exceptions to handle
- Context switch saves only 16 registers (like ARM Cortex-M)
- Simpler trap entry/exit (no ROTW, no WindowBase manipulation)
- Compatible with ESP-IDF HAL libraries when built with matching ABI

---

## 2. CardComputer Hardware

### 2.1 Block Diagram

```
+-----------------------------------------------+
|  M5Stack CardComputer                          |
|                                                |
|  +------------------+   +------------------+   |
|  | STAMP S3 Module  |   | ST7789V2 Display |   |
|  | (ESP32-S3-FN8)   |-->| 240x135 IPS      |   |
|  | 8 MB Flash (QIO) |   | SPI interface     |   |
|  | 512 KB SRAM      |   +------------------+   |
|  | Wi-Fi + BLE 5.0  |                          |
|  +------------------+   +------------------+   |
|         |               | 56-key Keyboard  |   |
|         +-------------->| GPIO matrix scan |   |
|         |               +------------------+   |
|         |                                      |
|         +---> microSD slot (SPI)               |
|         +---> I2S speaker (NS4168)             |
|         +---> IR transmitter (GPIO44)          |
|         +---> USB-C (native USB + UART)        |
|         +---> Grove port (I2C)                 |
+-----------------------------------------------+
```

### 2.2 Key Peripherals and Pin Assignments

| Peripheral | Interface | Pins | Notes |
|-----------|-----------|------|-------|
| ST7789V2 display | SPI2 | MOSI=35, SCK=36, CS=37, DC=34, RST=33, BL=38 | 240x135, 65K colors |
| Keyboard | GPIO matrix | Directly from ESP32-S3 GPIOs | 7 rows x 8 columns |
| microSD | SPI (HSPI) | MISO=39, MOSI=14, SCK=40, CS=12 | FAT32 |
| Speaker | I2S | BCLK=41, LRCK=43, DIN=42 | NS4168 amplifier |
| IR TX | GPIO | GPIO44 | 38 kHz modulation |
| USB | Native USB | GPIO19 (D-), GPIO20 (D+) | CDC-ACM for UART |
| UART0 | UART | TX=43, RX=44 | Shared with I2S/IR |

### 2.3 Memory Map (ESP32-S3)

| Region | Address Range | Size | Description |
|--------|-------------- |------|-------------|
| Internal SRAM 0 | 0x40370000-0x403AFFFF | 256 KB | Instruction bus |
| Internal SRAM 1 | 0x3FC88000-0x3FCFFFFF | 480 KB | Data bus |
| Internal SRAM 2 | 0x3FCF0000-0x3FCFFFFF | 64 KB | (overlaps SRAM 1) |
| RTC FAST | 0x600FE000-0x600FFFFF | 8 KB | RTC domain |
| RTC SLOW | 0x50000000-0x50001FFF | 8 KB | RTC domain |
| Flash (XIP) | 0x42000000-0x427FFFFF | up to 8 MB | Via cache, read-only |
| PSRAM | N/A | N/A | Not populated on STAMP S3 |
| Peripheral bus | 0x60000000-0x600FFFFF | 1 MB | APB/AHB peripherals |

The usable SRAM for PPAP is approximately **512 KB** (internal SRAM 0+1),
comparable to the RP2350's 520 KB.

---

## 3. Goals and Scope

### 3.1 Primary Goals

1. **New Xtensa architecture layer** (`src/arch/xtensa/`) — boot, context
   switch, trap/syscall, interrupt handling, all using Call0 ABI.
2. **CardComputer target** (`src/target/xtensa_cc/`) — ESP32-S3 clock,
   GPIO, UART, SPI, and SD card initialization.
3. **Display support** — ST7789V2 framebuffer console (`/dev/tty1`) using
   the existing `TARGET_CAP_DISPLAY` + `tty_backend_t` infrastructure.
4. **Keyboard support** — GPIO matrix scan providing `TARGET_CAP_KBD`
   input to `/dev/tty1`, making the CardComputer a standalone terminal.
5. **Preemptive scheduling** — single-core, timer-driven preemption via
   Xtensa timer interrupt.
6. **Pass test suite** — `runtests` on QEMU ESP32-S3 (if available) or
   hardware.

### 3.2 Extended Goals

- Dual-core ESP32-S3 (launch core 1 via `TARGET_CAP_CORE1`).
- microSD card support (`TARGET_CAP_SD`) for persistent filesystem.
- USB CDC-ACM as `/dev/ttyUSB0` (second TTY).
- Wi-Fi networking stack (future; would be a major new subsystem).

### 3.3 Out of Scope

- Bluetooth / BLE support.
- I2S audio / speaker driver.
- IR transmitter.
- PSRAM (not present on STAMP S3).
- Wi-Fi (first port focuses on bare-metal peripherals).
- Windowed ABI (we commit to Call0 ABI for this port).

---

## 4. Architecture Layer: New `src/arch/xtensa/`

### 4.1 File Inventory

| File | Purpose |
|------|---------|
| `arch.h` | IRQ save/restore, yield, barriers (PS register manipulation) |
| `cpu.h` | Special register definitions (PS, EPC, EXCCAUSE, etc.) |
| `boot.S` | `_start`, vector table, initial stack setup |
| `switch.S` | Context switch via software interrupt |
| `trap.S` | Exception/interrupt entry, syscall dispatch (`SYSCALL` instruction) |
| `xtensa_common.c` | Timer setup, interrupt routing, early init |

### 4.2 `arch.h` — Architecture Abstraction

```c
/*
 * Xtensa PS (Processor State) register bits:
 *   INTLEVEL (bits 3:0) — current interrupt level (0 = all enabled)
 *   EXCM (bit 4) — exception mode
 *   UM (bit 5) — user mode (1 = user, 0 = kernel)
 *   RING (bits 7:6) — ring level
 */

/* arch_irq_save: raise INTLEVEL to mask all, return old PS */
static inline uint32_t arch_irq_save(void) {
    uint32_t old_ps;
    __asm__ volatile ("rsil %0, 15" : "=a"(old_ps));  /* INTLEVEL=15 */
    return old_ps;
}

/* arch_irq_restore: restore PS from saved value */
static inline void arch_irq_restore(uint32_t saved) {
    __asm__ volatile ("wsr %0, ps; rsync" :: "a"(saved));
}

static inline void arch_irq_enable(void) {
    __asm__ volatile ("rsil a0, 0" ::: "a0");  /* INTLEVEL=0 */
}
static inline void arch_irq_disable(void) {
    __asm__ volatile ("rsil a0, 15" ::: "a0"); /* INTLEVEL=15 */
}

/* arch_yield: trigger software interrupt for context switch */
static inline void arch_yield(void) {
    __asm__ volatile ("syscall");  /* or write to INTSET for SW IRQ */
}

static inline void arch_wfi(void) { __asm__ volatile ("waiti 0"); }
static inline void arch_wfe(void) { __asm__ volatile ("waiti 0"); }
static inline void arch_sev(void) { /* inter-core IRQ via INTSET */ }

static inline void arch_dsb_isb(void) {
    __asm__ volatile ("memw" ::: "memory");  /* memory barrier */
    __asm__ volatile ("isync" ::: "memory"); /* instruction sync */
}
```

### 4.3 `cpu.h` — Special Register Definitions

Key Xtensa special registers:

| Register | SR# | Purpose |
|----------|-----|---------|
| `PS` | 230 | Processor State (INTLEVEL, UM, EXCM) |
| `EPC1` | 177 | Exception PC (level 1) |
| `EXCCAUSE` | 232 | Exception cause code |
| `EXCSAVE1` | 209 | Scratch register for level-1 exception handler |
| `EXCVADDR` | 238 | Exception virtual address (for load/store faults) |
| `CCOUNT` | 234 | Cycle counter (free-running) |
| `CCOMPARE0` | 240 | Cycle compare (timer interrupt when CCOUNT matches) |
| `VECBASE` | 231 | Vector base address |
| `INTENABLE` | 228 | Interrupt enable mask (32 bits) |
| `INTERRUPT` | 226 | Interrupt status (read) / INTSET (write) |
| `INTCLEAR` | 227 | Interrupt clear (write) |
| `PRID` | 235 | Processor ID (0 or 1 for dual-core) |

### 4.4 Exception and Interrupt Model

Xtensa uses a **level-based** interrupt model:

- **Level 1 (exceptions)**: Syscalls (`SYSCALL` instruction), memory
  faults, illegal instructions — all dispatch through the level-1 vector.
- **Levels 2-6 (medium-priority interrupts)**: Each level has its own
  vector entry point.  Timer interrupts typically at level 1 or 3.
- **Level 7 (NMI)**: Non-maskable.

Exception causes relevant to PPAP:

| EXCCAUSE | Value | PPAP Mapping |
|----------|-------|-------------|
| IllegalInstruction | 0 | Kernel panic / signal SIGILL |
| Syscall | 1 | `sys_dispatch()` |
| LoadStoreFault | 3, 28, 29 | Signal SIGSEGV |
| LoadStoreAlignment | 9 | Signal SIGBUS |
| Privileged | 8 | Signal SIGSEGV (user code violation) |

### 4.5 Context Switch Design

With Call0 ABI, the context switch is straightforward:

**Callee-saved registers** (Call0 ABI): a0 (return address), a1 (SP),
a12-a15.  However, since we save/restore the full user context on
trap entry, the context switch saves all 16 registers (a0-a15) plus PS
and EPC1.

```
pcb_t.sp layout (18 words, 72 bytes):
  [0]  a0  (return address)
  [1]  a1  (stack pointer, for reference)
  [2]  a2  ... a15
  [16] PS
  [17] EPC1
```

Context switch flow:
1. Timer interrupt fires → vector entry saves a0-a15, PS, EPC1 to
   current task's kernel stack.
2. Call `sched_schedule()` to pick next task.
3. Restore a0-a15, PS, EPC1 from next task's stack.
4. `RFE` (Return From Exception) resumes at new task's EPC1.

### 4.6 Syscall Dispatch

```
User code:  SYSCALL          (a2=syscall_nr, a3-a7=args)
  → EXCCAUSE=1, EPC1=PC of SYSCALL instruction
  → trap.S: save context, extract a2 (syscall_nr)
  → call sys_dispatch(nr, a3, a4, a5, a6, a7)
  → return value in a2
  → advance EPC1 by 3 bytes (SYSCALL is a 3-byte instruction)
  → RFE
```

### 4.7 User/Kernel Mode Separation

The ESP32-S3 PMS (Permission Management System) replaces the traditional
MPU/PMP:

- **Splitting address space**: PMS divides SRAM into regions, each with
  independent R/W/X permissions for kernel (World 0) and user (World 1).
- **World switch**: On exception entry, hardware switches to World 0
  (kernel).  `RFE` with PS.UM=1 returns to World 1 (user).
- PPAP's `mpu_configure_user()` maps to PMS region configuration.
- Similar region count constraints as ARM MPU (power-of-2 alignment).

---

## 5. Target Layer: `src/target/xtensa_cc/`

### 5.1 File Inventory

| File | Purpose |
|------|---------|
| `xtensa_cc.h` | Pin definitions, clock frequencies, display parameters |
| `target_xtensa_cc.c` | target_early_init/late_init, UART, SPI, display, keyboard |
| `st7789.c` | ST7789V2 SPI display driver (framebuffer → SPI DMA) |
| `keyboard.c` | GPIO matrix scanner, keymap, key-repeat logic |
| `CMakeLists.txt` | Build configuration |

### 5.2 `target_caps()`

```c
uint32_t target_caps(void) {
    return TARGET_CAP_SPI
         | TARGET_CAP_DISPLAY
         | TARGET_CAP_KBD;
    /* TARGET_CAP_SD added later when microSD driver lands */
}
```

### 5.3 Display Driver (ST7789V2)

The ST7789V2 is driven over SPI2 at up to 80 MHz:

- **Resolution**: 240x135 pixels, 16-bit RGB565
- **Framebuffer size**: 240 x 135 x 2 = 64,800 bytes (~63 KB)
- **Strategy**: maintain a text-mode buffer (similar to pico1calc),
  render glyphs to the SPI framebuffer, flush dirty regions every 20 ms
  via `sched_set_display_poll()`.
- **Initialization**: SPI2 setup → ST7789 reset sequence → set rotation
  → clear screen → backlight on.
- **TTY backend**: register `tty_backend_t` with putc (glyph render) and
  flush (SPI DMA transfer) callbacks.

With an 8x8 font, the 240x135 display provides a **30x16 character**
terminal — small but usable for a UNIX shell.

### 5.4 Keyboard Driver

The CardComputer's keyboard is a 7x8 GPIO matrix:

- **Scan**: drive each row low in sequence, read column GPIOs to detect
  key presses.  Debounce with 10 ms delay.
- **Keymap**: ASCII mapping with Fn/Shift modifiers for symbols and
  control characters.
- **Integration**: polled from the display poll callback (every 20 ms),
  feeds characters into the TTY1 input ring buffer via
  `tty_backend_t.getc`.
- **Special keys**: Fn+key combos for Ctrl-C, Ctrl-D, Ctrl-Z, arrow
  keys (VT100 escape sequences).

### 5.5 Boot Sequence

ESP32-S3 boot is managed by the on-chip ROM bootloader:

1. ROM reads flash header at 0x0 (SPI boot).
2. ROM loads second-stage bootloader (ESP-IDF or custom) to SRAM.
3. Second-stage sets up flash cache (XIP), clock, and jumps to
   application entry.

For PPAP, we have two options:

- **Option A (recommended)**: Use ESP-IDF as a HAL layer.  Build PPAP as
  an ESP-IDF component; ESP-IDF handles flash boot, cache setup, clock
  PLL, and peripheral initialization.  PPAP takes over after
  `app_main()`.
- **Option B**: Bare-metal, custom second-stage bootloader.  More control
  but requires reimplementing flash cache setup, PLL, and SPI boot —
  significant effort for little gain.

Option A aligns with how ESP-IDF is typically used and gives us
battle-tested clock/flash/peripheral initialization for free.

### 5.6 UART / Console

- **Primary console**: USB CDC-ACM via ESP32-S3's native USB peripheral
  (appears as `/dev/ttyACM0` on host).  Used for `ttyS0`.
- **Display console**: ST7789 + keyboard as `tty1`.
- Default console depends on whether keyboard is detected at boot (always
  present on CardComputer, so `tty1` is default).

---

## 6. Build System Integration

### 6.1 Toolchain

PPAP currently uses two cross-toolchain families:

| Target | Kernel toolchain | User toolchain | Source |
|--------|-----------------|----------------|--------|
| ARM (pico1, pico2, qemu_arm) | `arm-none-eabi-gcc` | `arm-none-eabi-gcc` + musl | Ubuntu package / Pico SDK |
| RISC-V (pico2rv) | `riscv32-unknown-elf-gcc` | same + musl | Official RISC-V toolchain |
| m68k (x68k, qemu_m68k) | `m68k-elf-gcc` | `m68k-elf-gcc` + musl | `third_party/build_gcc_m68k.sh` |
| **Xtensa (xtensa_cc)** | `xtensa-esp-elf-gcc` | same + musl | **ESP-IDF toolchain** |

#### 6.1.1 Directory Convention

PPAP separates third-party sources from built/downloaded artifacts:

| Directory | Purpose | Tracking |
|-----------|---------|----------|
| `third_party/esp-idf` | ESP-IDF source (HAL, bootloader, build system) | **git submodule** |
| `tools/xtensa-toolchain/` | Xtensa cross-compiler, esptool, Python venv | `.gitignore` (not tracked) |

This matches the existing pattern: `third_party/pico-sdk` (source) vs
`tools/riscv-toolchain/` (prebuilt binaries), `third_party/openocd`
(source) vs `tools/openocd-rp/` (built binary).

#### 6.1.2 Kernel Toolchain: `xtensa-esp-elf-gcc`

The Xtensa toolchain is **chip-specific** — unlike ARM/RISC-V where one
toolchain covers many chips, each Xtensa configuration (ESP32, ESP32-S2,
ESP32-S3) has its own GCC build because the ISA is configurable per chip
(window size, DSP options, interrupt levels, etc.).

**Installation** via `scripts/setup_toolchain.sh` (Step 1d):

ESP-IDF's `install.sh` downloads the correct toolchain version.  The
`IDF_TOOLS_PATH` environment variable redirects the install from the
default `~/.espressif/` to the project-local `tools/xtensa-toolchain/`:

```sh
# Automated by setup_toolchain.sh:
IDF_TOOLS_PATH=tools/xtensa-toolchain third_party/esp-idf/install.sh esp32s3
```

This installs `xtensa-esp-elf-gcc`, `esptool.py`, `idf.py`, and a
Python venv, all under `tools/xtensa-toolchain/`.

To activate the toolchain in a shell session:
```sh
export IDF_TOOLS_PATH=$PWD/tools/xtensa-toolchain
source third_party/esp-idf/export.sh
# Now xtensa-esp-elf-gcc, esptool.py, idf.py are on PATH
```

#### 6.1.2 Call0 ABI Configuration

The Xtensa toolchain defaults to the **windowed ABI**.  PPAP requires
**Call0 ABI** for simplified context switching.  Key compiler flags:

```
-mabi=call0          # Use Call0 (flat register file) ABI
-mcpu=esp32s3        # Target ESP32-S3 Xtensa configuration
-fno-builtin         # Avoid libc assumptions (kernel freestanding)
-ffreestanding        # No hosted environment
-nostdlib            # No default libraries
```

The toolchain cmake file (`cmake/toolchain_xtensa.cmake`):

```cmake
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR xtensa)

set(CMAKE_C_COMPILER xtensa-esp-elf-gcc)
set(CMAKE_CXX_COMPILER xtensa-esp-elf-g++)
set(CMAKE_ASM_COMPILER xtensa-esp-elf-gcc)
set(CMAKE_OBJCOPY xtensa-esp-elf-objcopy)
set(CMAKE_SIZE xtensa-esp-elf-size)

set(CMAKE_C_FLAGS_INIT "-mabi=call0 -mcpu=esp32s3 -mno-rtti")
set(CMAKE_ASM_FLAGS_INIT "-mabi=call0 -mcpu=esp32s3")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
```

**Warning**: All object files linked together **must** use the same ABI.
Mixing Call0 and windowed objects causes silent corruption (register
spill/fill mismatches).  If using ESP-IDF HAL, the HAL libraries must
also be built with `-mabi=call0`.  ESP-IDF v5.x supports this via
`CONFIG_COMPILER_CALL0_ABI=y` in `sdkconfig`.

#### 6.1.3 User-Space Toolchain (musl)

User-space programs need musl libc cross-compiled for Xtensa Call0:

```sh
# third_party/build_musl_xtensa.sh (following build_musl.sh pattern)
CROSS_COMPILE=xtensa-esp-elf-
CFLAGS="-mabi=call0 -mcpu=esp32s3"
./configure --target=xtensa-esp-elf --prefix=... \
    CC="${CROSS_COMPILE}gcc" CFLAGS="${CFLAGS}"
make && make install
```

The musl port for Xtensa is **upstream** (merged in musl 1.2.x), so no
custom patches are needed — but it must be configured for Call0 ABI.

The same `PPAP_SHARED_BUILD` pattern applies:
```
build/xtensa/          # shared userland artifacts
build/xtensa_cc/       # target-specific kernel build
```

#### 6.1.4 Flashing Tool

ESP32-S3 uses `esptool.py` for flashing over USB (native USB or UART):

```sh
esptool.py --chip esp32s3 --port /dev/ttyACM0 \
    write_flash 0x0 build/xtensa_cc/ppap_xtensa_cc.bin
```

When using ESP-IDF (Option A), `idf.py flash` wraps this with correct
partition table offsets and bootloader bundling.

For the PPAP build system, `scripts/run.sh --build xtensa_cc` would:
1. Build via CMake (or `idf.py build`)
2. Flash via `esptool.py`
3. Open `minicom` / `screen` on USB serial for console

#### 6.1.5 Debugging

- **JTAG**: ESP32-S3 has built-in USB JTAG (no external probe needed).
  OpenOCD with Espressif patches supports it:
  ```sh
  openocd -f board/esp32s3-builtin.cfg
  ```
- **GDB**: `xtensa-esp-elf-gdb` (shipped with the toolchain).
  ```sh
  xtensa-esp-elf-gdb build/xtensa_cc/ppap_xtensa_cc.elf \
      -ex "target remote :3333"
  ```
- **Gotcha**: Xtensa GDB requires the matching `xtensa-config.c`
  overlay; using a generic `gdb-multiarch` will not work (unlike ARM).

### 6.2 CMake Integration

New files for the build system:

```
cmake/toolchain_xtensa.cmake          # cross-toolchain (see 6.1.2)
cmake/xtensa.cmake                    # common Xtensa setup (like riscv.cmake)
src/arch/xtensa/CMakeLists.txt        # arch sources
src/target/xtensa_cc/CMakeLists.txt    # target sources
third_party/build_musl_xtensa.sh      # musl cross-build script
```

`cmake/xtensa.cmake` follows the existing `riscv.cmake` / `arm_m.cmake`
pattern:

```cmake
include_guard(GLOBAL)
set(PPAP_SHARED_BUILD "${PPAP_ROOT}/build/xtensa")
include(${CMAKE_CURRENT_LIST_DIR}/user.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/kernel.cmake)

function(ppap_xtensa_target_common target)
    target_include_directories(${target} PRIVATE
        ${PPAP_ROOT}/src ${PPAP_ROOT}/src/kernel)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Werror -Wno-unused-parameter)
    target_compile_definitions(${target} PRIVATE PPAP_KERNEL=1)
    # ... subsystem flags ...
endfunction()
```

If using ESP-IDF as HAL (Option A), the target CMakeLists.txt wraps the
ESP-IDF component model.  PPAP kernel sources are registered as an
ESP-IDF component, and ESP-IDF's build system handles toolchain setup,
partition table, and bootloader linking.

### 6.3 QEMU

Espressif maintains a QEMU fork with ESP32-S3 support:

```sh
# Build Espressif QEMU (third_party/build_qemu_xtensa.sh)
git clone https://github.com/espressif/qemu.git third_party/qemu-xtensa
cd third_party/qemu-xtensa
./configure --target-list=xtensa-softmmu --enable-slirp
make -j$(nproc)
```

Run:
```sh
qemu-system-xtensa -M esp32s3 -nographic \
    -drive file=build/xtensa_cc/flash_image.bin,if=mtd,format=raw
```

QEMU ESP32-S3 support is experimental — peripheral coverage is
incomplete (SPI display and GPIO matrix likely unsupported).  Use QEMU
for kernel-level smoke tests (boot, scheduler, syscalls) and hardware
for display/keyboard integration.

---

## 7. Implementation Plan

### Phase CC-1: Architecture Skeleton

**Goal**: Minimal boot to serial output on ESP32-S3.

| Step | Description |
|------|-------------|
| CC-1a | Create `src/arch/xtensa/` with `arch.h`, `cpu.h` (Call0 ABI) |
| CC-1b | Implement `boot.S` — `_start`, stack setup, call `kernel_main` |
| CC-1c | Create `src/target/xtensa_cc/` — `target_early_init()` with UART |
| CC-1d | First boot: "PiPAPo booting..." on USB serial |

### Phase CC-2: Interrupts and Timer

| Step | Description |
|------|-------------|
| CC-2a | Implement `trap.S` — exception vector, level-1 handler |
| CC-2b | Timer interrupt via CCOMPARE0 → `sched_tick()` |
| CC-2c | Verify timer-driven prints (heartbeat) |

### Phase CC-3: Context Switch and Scheduler

| Step | Description |
|------|-------------|
| CC-3a | Implement `switch.S` — save/restore a0-a15, PS, EPC1 |
| CC-3b | Wire `sched_schedule()`, verify round-robin switching |
| CC-3c | PMS configuration for user/kernel separation |
| CC-3d | Syscall path: `SYSCALL` → `trap.S` → `sys_dispatch()` |
| CC-3e | Pass core test suite (fork, exec, pipe, signals) |

### Phase CC-4: Display

| Step | Description |
|------|-------------|
| CC-4a | SPI2 driver for ST7789V2 (init sequence, pixel write) |
| CC-4b | Framebuffer console: 8x8 font, 30x16 text grid |
| CC-4c | Register `tty_backend_t` for `tty1`, hook display poll |
| CC-4d | Boot messages visible on LCD |

### Phase CC-5: Keyboard

| Step | Description |
|------|-------------|
| CC-5a | GPIO matrix scan, debounce, ASCII keymap |
| CC-5b | Wire to `tty1` input ring buffer |
| CC-5c | Interactive shell on the CardComputer display |

### Phase CC-6: SD Card (extended)

| Step | Description |
|------|-------------|
| CC-6a | HSPI driver for microSD |
| CC-6b | FAT32 read support (or reuse existing SD driver) |
| CC-6c | Mount SD as `/mnt/sd`, set `TARGET_CAP_SD` |

---

## 8. Risks and Open Questions

| Risk | Mitigation |
|------|-----------|
| Xtensa Call0 ABI compatibility with ESP-IDF HAL | Verify with ESP-IDF v5.x; may need to rebuild HAL libs with `-mabi=call0` |
| 512 KB SRAM is tight with 63 KB framebuffer | Use dirty-rect flushing to avoid full framebuffer; or use text-mode-only buffer (~2 KB) with on-demand glyph rendering |
| ESP-IDF HAL dependency adds complexity | Isolate HAL calls behind target layer; keep arch layer pure Xtensa |
| QEMU ESP32-S3 support is immature | Prioritize hardware testing; QEMU for smoke tests only |
| PMS (permission system) documentation is sparse | Reference ESP-IDF source and TRM; PMS is simpler than ARM MPU |
| Keyboard matrix pin assignments not fully documented | Reverse-engineer from M5Stack Arduino library source |

---

## 9. References

- [ESP32-S3 Technical Reference Manual](https://www.espressif.com/sites/default/files/documentation/esp32-s3_technical_reference_manual_en.pdf)
- [Xtensa ISA Reference Manual](https://0x04.net/~mwk/doc/xtensa.pdf)
- [M5Stack CardComputer product page](https://docs.m5stack.com/en/core/CardComputer)
- [M5Stack CardComputer schematic](https://docs.m5stack.com/en/core/CardComputer) (pinout section)
- [ESP-IDF Programming Guide v5.x](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/)
- [Espressif QEMU fork](https://github.com/espressif/qemu) (ESP32-S3 machine support)
