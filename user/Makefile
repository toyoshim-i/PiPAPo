# =============================================================================
# user/Makefile — Cross-compilation build for PPAP user-space programs
# =============================================================================
#
# Builds ARM Thumb ELF binaries using raw SVC syscalls (no libc).
# Integrated into the main CMake build via custom command.
#
# Usage:
#   make BUILD_DIR=../build/user   # build all user binaries (out-of-source)
#   make install BUILD_DIR=../build/user ROMFS_BIN=<dir>  # install to romfs
#   make clean BUILD_DIR=../build/user   # remove build artifacts
# =============================================================================

CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
AS      := $(CROSS)gcc
LD      := $(CROSS)gcc
OBJDUMP := $(CROSS)objdump
SIZE    := $(CROSS)size
NM      := $(CROSS)nm

# Source directory (where this Makefile lives)
SRC_DIR := $(dir $(lastword $(MAKEFILE_LIST)))

# Build output directory (defaults to source dir for standalone use)
BUILD_DIR ?= .

ROMFS_BIN ?= ../romfs/bin
TESTS_DIR ?= ../tests/user

# Search tests/user/ and source dir for sources
VPATH := $(SRC_DIR) $(TESTS_DIR)

# Compiler / assembler flags — Cortex-M0+ Thumb, freestanding, no libc
# PIC flags: all data references go through GOT (r9 = GOT base, set by kernel).
# -mno-pic-data-is-text-relative is essential: .text lives in flash (XIP) while
# .data/.got live in SRAM — they are at unrelated addresses.
CFLAGS  := -mthumb -mcpu=cortex-m0plus -march=armv6s-m \
           -ffreestanding -nostdlib -Os -g -Wall -Werror \
           -fPIC -msingle-pic-base -mpic-register=r9 \
           -mno-pic-data-is-text-relative \
           -I$(SRC_DIR) -I$(TESTS_DIR)
ASFLAGS := -mthumb -mcpu=cortex-m0plus -march=armv6s-m
LDFLAGS := -nostdlib -T $(SRC_DIR)user.ld -Wl,--gc-sections

# Common objects linked into every user program
COMMON_OBJS := $(BUILD_DIR)/crt0.o $(BUILD_DIR)/syscall.o

# ── Programs ─────────────────────────────────────────────────────────────────
# Application programs (sources in user/)
APPS := hello getty init ttyctl
# Test programs (sources in tests/user/, found via VPATH)
# Only built when PPAP_TESTS=1 is passed from CMake
TESTS_ALL := test_vfork test_exec test_pipe test_brk test_fd test_signal test_poll test_sleep_intr test_orphan runtests
ifeq ($(PPAP_TESTS),1)
TESTS := $(TESTS_ALL)
else
TESTS :=
endif
PROGRAMS := $(APPS) $(TESTS)

ELFS := $(addprefix $(BUILD_DIR)/, $(addsuffix .elf, $(PROGRAMS)))

# ── Rules ────────────────────────────────────────────────────────────────────

all: $(BUILD_DIR) $(ELFS)
	@for elf in $(ELFS); do $(SIZE) $$elf; done

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# getty: pure assembly (no crt0/syscall needed, no data segment → 1 page)
# See getty.c for a portable C reference (requires PIC, uses 2 pages).
$(BUILD_DIR)/getty.o: getty.S
	$(AS) $(ASFLAGS) -c -o $@ $<

$(BUILD_DIR)/getty.elf: $(BUILD_DIR)/getty.o $(SRC_DIR)user.ld
	$(LD) $(LDFLAGS) -o $@ $(BUILD_DIR)/getty.o
	@$(NM) --numeric-sort $@ | grep -E '_start|main|write|_exit'

# Link: common objects + program-specific object → ELF
$(BUILD_DIR)/%.elf: $(COMMON_OBJS) $(BUILD_DIR)/%.o $(SRC_DIR)user.ld
	$(LD) $(LDFLAGS) -o $@ $(COMMON_OBJS) $(BUILD_DIR)/$*.o
	@$(NM) --numeric-sort $@ | grep -E '_start|main|write|_exit'

# Compile C sources
$(BUILD_DIR)/%.o: %.c $(SRC_DIR)syscall.h $(wildcard $(TESTS_DIR)/utest.h)
	$(CC) $(CFLAGS) -c -o $@ $<

# Assemble .S sources
$(BUILD_DIR)/%.o: %.S
	$(AS) $(ASFLAGS) -c -o $@ $<

ROMFS_SBIN ?= ../romfs/sbin

# Install ELFs into the romfs bin directory (init goes to sbin)
install: all
	@mkdir -p $(ROMFS_BIN) $(ROMFS_SBIN)
	@# Remove stale test binaries when PPAP_TESTS is off
ifneq ($(PPAP_TESTS),1)
	@for t in $(TESTS_ALL); do rm -f $(ROMFS_BIN)/$$t; done
endif
	@for elf in $(ELFS); do \
		name=$$(basename $$elf .elf); \
		if [ "$$name" = "init" ]; then \
			cp $$elf $(ROMFS_SBIN)/$$name; \
		else \
			cp $$elf $(ROMFS_BIN)/$$name; \
		fi; \
	done
	@echo "Installed $(ELFS) → $(ROMFS_BIN)/ and $(ROMFS_SBIN)/"

clean:
	rm -rf $(BUILD_DIR)/*.o $(BUILD_DIR)/*.elf

# Prevent make from deleting .o files as intermediates
.PRECIOUS: $(BUILD_DIR)/%.o

.PHONY: all install clean
