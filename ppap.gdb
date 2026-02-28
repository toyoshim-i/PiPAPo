# ppap.gdb — GDB init script for PicoPiAndPortable
#
# Usage:
#   arm-none-eabi-gdb -x ppap.gdb build/ppap.elf
#
# Requires openocd to be running:
#   openocd -f openocd.cfg

set pagination off

# Connect to OpenOCD's GDB server (Core 0 on port 3333)
target remote :3333

# Reset and halt Core 0 before the boot ROM runs.
# Using `reset halt` (not `reset init`) — on the RP2040 there is no
# reset-init script, but `reset init` can have subtle timing differences
# with CMSIS-DAP adapters.  `reset halt` is the safer choice.
monitor reset halt

# All code lives in XIP flash (read-only at runtime) — use hardware BPs.
# openocd.cfg already sets gdb_breakpoint_override hard, but `hbreak` is
# explicit here as a reminder.  The FPB has 4 comparator slots.
#
# Typical session:
#   (gdb) hbreak kmain
#   (gdb) continue          -- runs boot ROM + boot2 + Reset_Handler → kmain
#   (gdb) next / step       -- step through kmain
#   (gdb) info registers
#   (gdb) x/4wx 0x10000100  -- inspect vector table in flash
#   (gdb) x/4wx 0x20000000  -- inspect SRAM

# Useful command aliases
define flash_and_reset
  monitor program build/ppap.elf verify
  monitor reset halt
end

define restart
  monitor reset halt
end
