# pico1-attach.gdb — attach to already-running firmware (no reflash)
#
# Usage:
#   gdb-multiarch -x scripts/debug/pico1-attach.gdb build/arm_m/ppap_pico1.elf
#
# Requires openocd to be running:
#   openocd -f scripts/debug/openocd.cfg

set pagination off

# Connect to OpenOCD (Core 0, port 3333)
target remote :3333

# Pause the running CPU and show current PC.
monitor halt
