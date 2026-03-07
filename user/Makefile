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
TESTS_DIR ?= ../tests/user

# Search tests/user/ for test_*.c and runtests.c sources
VPATH := $(TESTS_DIR)

# Compiler / assembler flags — Cortex-M0+ Thumb, freestanding, no libc
# PIC flags: all data references go through GOT (r9 = GOT base, set by kernel).
# -mno-pic-data-is-text-relative is essential: .text lives in flash (XIP) while
# .data/.got live in SRAM — they are at unrelated addresses.
CFLAGS  := -mthumb -mcpu=cortex-m0plus -march=armv6s-m \
           -ffreestanding -nostdlib -Os -g -Wall -Werror \
           -fPIC -msingle-pic-base -mpic-register=r9 \
           -mno-pic-data-is-text-relative \
           -I. -I$(TESTS_DIR)
ASFLAGS := -mthumb -mcpu=cortex-m0plus -march=armv6s-m
LDFLAGS := -nostdlib -T user.ld -Wl,--gc-sections

# Common objects linked into every user program
COMMON_OBJS := crt0.o syscall.o

# ── Programs ─────────────────────────────────────────────────────────────────
# Application programs (sources in user/)
APPS := hello getty
# Test programs (sources in tests/user/, found via VPATH)
# Only built when PPAP_TESTS=1 is passed from CMake
TESTS_ALL := test_vfork test_exec test_pipe test_brk test_fd test_signal test_poll test_sleep_intr test_orphan runtests
ifeq ($(PPAP_TESTS),1)
TESTS := $(TESTS_ALL)
else
TESTS :=
endif
PROGRAMS := $(APPS) $(TESTS)

ELFS := $(addsuffix .elf, $(PROGRAMS))

# ── Rules ────────────────────────────────────────────────────────────────────

all: $(ELFS)
	@for elf in $(ELFS); do $(SIZE) $$elf; done

# getty: pure assembly (no crt0/syscall needed, no data segment → 1 page)
# See getty.c for a portable C reference (requires PIC, uses 2 pages).
getty.o: getty.S
	$(AS) $(ASFLAGS) -c -o $@ $<

getty.elf: getty.o user.ld
	$(LD) $(LDFLAGS) -o $@ getty.o
	@$(NM) --numeric-sort $@ | grep -E '_start|main|write|_exit'

# Link: common objects + program-specific object → ELF
%.elf: $(COMMON_OBJS) %.o user.ld
	$(LD) $(LDFLAGS) -o $@ $(COMMON_OBJS) $*.o
	@$(NM) --numeric-sort $@ | grep -E '_start|main|write|_exit'

# Compile C sources
%.o: %.c syscall.h $(wildcard $(TESTS_DIR)/utest.h)
	$(CC) $(CFLAGS) -c -o $@ $<

# Assemble .S sources
%.o: %.S
	$(AS) $(ASFLAGS) -c -o $@ $<

# Install ELFs into the romfs bin directory
install: all
	@mkdir -p $(ROMFS_BIN)
	@# Remove stale test binaries when PPAP_TESTS is off
ifneq ($(PPAP_TESTS),1)
	@for t in $(TESTS_ALL); do rm -f $(ROMFS_BIN)/$$t; done
endif
	@for elf in $(ELFS); do \
		name=$$(basename $$elf .elf); \
		cp $$elf $(ROMFS_BIN)/$$name; \
	done
	@echo "Installed $(ELFS) → $(ROMFS_BIN)/"

clean:
	rm -f *.o *.elf

.PHONY: all install clean
