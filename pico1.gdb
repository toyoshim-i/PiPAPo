# pico1.gdb — GDB init script for official Raspberry Pi Pico target
#
# Usage:
#   gdb-multiarch -x pico1.gdb build/ppap_pico1.elf
#   (gdb) hbreak kmain
#   (gdb) continue
#
# Requires openocd to be running:
#   openocd -f openocd.cfg

set pagination off

# Connect to OpenOCD (Core 0, port 3333)
target remote :3333

# Flash the ELF and halt.
load

# Reset and halt at the very start of the boot ROM.
monitor reset halt

define restart
  monitor reset halt
end
