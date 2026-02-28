# ppap-attach.gdb — attach to already-running firmware (no reflash)
#
# Use this when the firmware is already flashed and running.
# The CPU will be wherever it currently is (likely in kmain's for(;;) loop).
#
# Usage:
#   gdb-multiarch -x ppap-attach.gdb build/ppap.elf
#
# Requires openocd to be running:
#   openocd -f openocd.cfg

set pagination off

# Connect to OpenOCD (Core 0, port 3333)
target remote :3333

# Pause the running CPU and show current PC.
monitor halt

# The CPU is now halted wherever it was running (usually kmain's for(;;)).
# Useful commands from here:
#   (gdb) info registers        -- show all registers including PC
#   (gdb) backtrace             -- show call stack
#   (gdb) hbreak <func>         -- set hardware breakpoint for next run
#   (gdb) monitor reset halt    -- restart from boot ROM
