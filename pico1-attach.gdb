# pico1-attach.gdb — attach to already-running firmware (no reflash)
#
# Usage:
#   gdb-multiarch -x pico1-attach.gdb build/ppap_pico1.elf
#
# Requires openocd to be running:
#   openocd -f openocd.cfg

set pagination off

# Connect to OpenOCD (Core 0, port 3333)
target remote :3333

# Pause the running CPU and show current PC.
monitor halt
