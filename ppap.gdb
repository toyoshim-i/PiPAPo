# ppap.gdb — GDB init script for PicoPiAndPortable
#
# Usage:
#   arm-none-eabi-gdb -x ppap.gdb build/ppap.elf
#
# Or from within GDB:
#   (gdb) source ppap.gdb
#
# Requires openocd to be running:
#   openocd -f openocd.cfg

# Connect to OpenOCD's GDB server
target extended-remote :3333

# Halt both cores and reset to a clean state
monitor reset init

# NOTE: always use hardware breakpoints (hbreak / hb) for code in XIP flash.
# openocd.cfg sets gdb_breakpoint_override hard so plain `break` also works,
# but `hbreak` is explicit and safe regardless of that setting.
# The Cortex-M0+ FPB has 4 hardware breakpoint slots.

# Useful aliases
define flash
  monitor program build/ppap.elf verify reset
end

define restart
  monitor reset init
end
