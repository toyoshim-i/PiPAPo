# =============================================================================
# user/Makefile — Cross-compilation build for PPAP user-space programs
# =============================================================================
#
# Builds ARM Thumb ELF binaries using raw SVC syscalls (no libc).
# Integrated into the main CMake build via custom command.
#
# Usage:
#   make                           # build all user binaries
#   make install ROMFS_BIN=<dir>   # copy ELFs to romfs/bin/
#   make clean                     # remove build artifacts
# =============================================================================

CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
AS      := $(CROSS)gcc
LD      := $(CROSS)gcc
OBJDUMP := $(CROSS)objdump
SIZE    := $(CROSS)size
NM      := $(CROSS)nm

ROMFS_BIN ?= ../romfs/bin

# Compiler / assembler flags — Cortex-M0+ Thumb, freestanding, no libc
# PIC flags: all data references go through GOT (r9 = GOT base, set by kernel).
# -mno-pic-data-is-text-relative is essential: .text lives in flash (XIP) while
# .data/.got live in SRAM — they are at unrelated addresses.
CFLAGS  := -mthumb -mcpu=cortex-m0plus -march=armv6s-m \
           -ffreestanding -nostdlib -Os -g -Wall -Werror \
           -fPIC -msingle-pic-base -mpic-register=r9 \
           -mno-pic-data-is-text-relative
ASFLAGS := -mthumb -mcpu=cortex-m0plus -march=armv6s-m
LDFLAGS := -nostdlib -T user.ld -Wl,--gc-sections

# Common objects linked into every user program
COMMON_OBJS := crt0.o syscall.o

# ── Programs ─────────────────────────────────────────────────────────────────
# Add new programs here.  Each FOO needs a FOO.c source file.
PROGRAMS := hello test_vfork

ELFS := $(addsuffix .elf, $(PROGRAMS))

# ── Rules ────────────────────────────────────────────────────────────────────

all: $(ELFS)
	@for elf in $(ELFS); do $(SIZE) $$elf; done

# Link: common objects + program-specific object → ELF
%.elf: $(COMMON_OBJS) %.o user.ld
	$(LD) $(LDFLAGS) -o $@ $(COMMON_OBJS) $*.o
	@$(NM) --numeric-sort $@ | grep -E '_start|main|write|_exit'

# Compile C sources
%.o: %.c syscall.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Assemble .S sources
%.o: %.S
	$(AS) $(ASFLAGS) -c -o $@ $<

# Install ELFs into the romfs bin directory
install: all
	@mkdir -p $(ROMFS_BIN)
	@for elf in $(ELFS); do \
		name=$$(basename $$elf .elf); \
		cp $$elf $(ROMFS_BIN)/$$name; \
	done
	@echo "Installed $(ELFS) → $(ROMFS_BIN)/"

clean:
	rm -f *.o *.elf

.PHONY: all install clean
