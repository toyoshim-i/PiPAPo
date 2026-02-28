# ppap.gdb — GDB init script for PicoPiAndPortable
#
# Two workflows:
#
#   Flash + debug from scratch (recommended):
#     gdb-multiarch -x ppap.gdb build/ppap.elf
#     (gdb) hbreak kmain
#     (gdb) continue
#
#   Attach to already-running firmware (no reflash):
#     gdb-multiarch -x ppap-attach.gdb build/ppap.elf
#
# Requires openocd to be running:
#   openocd -f openocd.cfg

set pagination off

# Connect to OpenOCD (Core 0, port 3333)
target remote :3333

# Flash the ELF and halt.
# `load` writes ppap.elf to flash via OpenOCD, then halts the CPU.
# This is the officially documented RP2040 debug workflow and is required
# for `monitor reset halt` to reliably stop before our code runs.
# Without `load`, OpenOCD may not have the memory map initialised and
# reset-halt timing over CMSIS-DAP can be unreliable.
load

# Reset and halt at the very start of the boot ROM.
monitor reset halt

# Ready.  Typical session:
#   (gdb) hbreak kmain          -- hardware BP (XIP flash is read-only)
#   (gdb) continue              -- runs boot ROM + boot2 + Reset_Handler → kmain
#   (gdb) next                  -- step over uart_init_console()
#   (gdb) info registers
#   (gdb) x/4wx 0x10000100      -- vector table in flash (SP + Reset_Handler)
#   (gdb) x/4wx 0x20000000      -- start of SRAM

define restart
  monitor reset halt
end
