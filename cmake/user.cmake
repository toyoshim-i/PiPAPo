# user.cmake — Common userland build configuration
#
# Included by cmake/arm_m.cmake and cmake/m68k.cmake.
# Expects PPAP_SHARED_BUILD to be set before inclusion (the directory
# where arch-shared artifacts like musl, busybox, rogue, user ELFs live).
#
# At include time, automatically registers build commands for:
#   - musl libc, busybox, rogue (third-party)
#   - User-space programs and busybox applet lists
#   - mkromfs host tool
#
# Sets PPAP_USER_ELFS — list of user ELF paths for romfs generation.
#
# Per-target API:
#   ppap_generate_romfs(target [BIG_ENDIAN] [OVERLAY_DIR dir])
#
# Also provides:
#   - PPAP_* variables for arch-specific compiler/linker configuration
#   - Generated ppap-target-config.sh for shell build scripts
#   - Generated specs file for musl-linked binaries

include_guard(GLOBAL)

# --- Project root (derived from this file's location: cmake/..) ---
get_filename_component(PPAP_ROOT ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)

# --- Shared build directory ---
# Set by cmake/arm_m.cmake or cmake/m68k.cmake before including this file.
# Falls back to CMAKE_BINARY_DIR for single-target builds.
if(NOT DEFINED PPAP_SHARED_BUILD)
    set(PPAP_SHARED_BUILD "${CMAKE_BINARY_DIR}")
endif()
file(MAKE_DIRECTORY "${PPAP_SHARED_BUILD}")

# --- Detect architecture from cmake toolchain ---
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "m68k")
    set(PPAP_ARCH m68k)
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "xtensa")
    set(PPAP_ARCH xtensa)
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "hazard3")
    set(PPAP_ARCH riscv)
else()
    set(PPAP_ARCH arm)
endif()

# --- Subsystem and eCPU build options ---
if(PPAP_ARCH STREQUAL "arm")
    # ARM: all subsystems and eCPUs enabled
    set(PPAP_ENABLE_HUMAN68K ON)
    set(PPAP_ENABLE_CPM ON)
    set(PPAP_ENABLE_SOS ON)
    set(PPAP_ENABLE_ECPU_Z80 ON)
    set(PPAP_ENABLE_ECPU_M68K ON)
elseif(PPAP_ARCH STREQUAL "m68k")
    # Native m68k: subsystems ON, but m68k eCPU OFF (native execution)
    set(PPAP_ENABLE_HUMAN68K ON)
    set(PPAP_ENABLE_CPM ON)
    set(PPAP_ENABLE_SOS ON)
    set(PPAP_ENABLE_ECPU_Z80 ON)
    set(PPAP_ENABLE_ECPU_M68K OFF)
else()
    # RISC-V / Xtensa: all eCPUs are pure C — enable everything.
    set(PPAP_ENABLE_HUMAN68K ON)
    set(PPAP_ENABLE_CPM ON)
    set(PPAP_ENABLE_SOS ON)
    set(PPAP_ENABLE_ECPU_Z80 ON)
    set(PPAP_ENABLE_ECPU_M68K ON)
endif()

# --- User-space program lists ---

# Application programs (sources in src/user/)
set(USER_APPS hello getty init trace pdb push)
# ttyctl is pico1calc-only (LCD terminal control)
if(CMAKE_PROJECT_NAME STREQUAL "ppap_pico1calc")
    list(APPEND USER_APPS ttyctl)
endif()
# Install destinations: init -> sbin, ttyctl -> usr/bin, others -> bin

# Optional per-app extra sources (for multi-file user programs).
set(PPAP_USER_EXTRA_SOURCES_pdb
    ${PPAP_ROOT}/src/user/pdb_util.c
    ${PPAP_ROOT}/src/user/pdb_trace_util.c
    ${PPAP_ROOT}/src/user/pdb_cmd.c
    ${PPAP_ROOT}/src/user/pdb_regs.c
    ${PPAP_ROOT}/src/user/pdb_target.c
    ${PPAP_ROOT}/src/user/pdb_inspect.c
    ${PPAP_ROOT}/src/user/pdb_break.c
)
set(PPAP_USER_EXTRA_SOURCES_push
    ${PPAP_ROOT}/src/user/push_line.c
)

# Test programs (sources in tests/user/)
set(USER_TESTS
    test_exec test_elf test_vfork test_fault test_pipe test_brk
    test_fd test_signal test_poll test_sleep_intr test_orphan
    test_id test_fs test_rw test_time test_iov test_stat test_tmpfs
    test_float test_signal_float test_x68k test_h68k_dos test_cpm test_sos test_zexdoc test_zexall test_trace test_pdb
    test_pdb_arm_disas trace_peek_target
    test_musl
    runtests runtests_ext
)

# Musl-linked test programs (sources in tests/user/, linked against musl libc)
set(USER_MUSL_TESTS
    test_musl_child test_musl_fileio test_musl_dir test_musl_fmt)

# --- Busybox applet lists ---

# Applets that link to busybox binary
set(BB_APPLETS
    cat chmod cp df echo grep head hush kill ln ls mkdir mv
    printf ps rm rmdir sed sleep sort tail top uname vi wc
)
# Sbin applets — link to busybox via ../bin/busybox
set(BB_SBIN_APPLETS mount umount)

# =============================================================================
# Arch-specific variables
# =============================================================================

if(DEFINED ENV{PPAP_M68K_TOOLCHAIN})
    set(PPAP_M68K_TC    $ENV{PPAP_M68K_TOOLCHAIN})
endif()
set(PPAP_M68K_CC        ${PPAP_M68K_TC}/bin/m68k-elf-gcc)
set(PPAP_M68K_OBJCOPY   ${PPAP_M68K_TC}/bin/m68k-elf-objcopy)
set(PPAP_M68K_R68K_LD   ${PPAP_ROOT}/src/user/arch/m68k/r68k.ld)

if(PPAP_ARCH STREQUAL "m68k")
    set(PPAP_CC            ${PPAP_M68K_CC})
    set(PPAP_CROSS_PREFIX  ${PPAP_M68K_TC}/bin/m68k-elf-)
    set(PPAP_STRIP         ${PPAP_M68K_TC}/bin/m68k-elf-strip)
    set(PPAP_OBJCOPY       ${PPAP_M68K_OBJCOPY})
    set(PPAP_SIZE_CMD      ${PPAP_M68K_TC}/bin/m68k-elf-size)
    set(PPAP_ARCH_DIR      ${PPAP_ROOT}/src/user/arch/m68k)
    set(PPAP_TARGET_FLAGS  -m68000)
    set(PPAP_PIC_FLAGS     -msep-data)
    set(PPAP_USER_LD       ${PPAP_ARCH_DIR}/user.ld)
    set(PPAP_R68K_LD       ${PPAP_M68K_R68K_LD})
    set(PPAP_BUSYBOX_LD    ${PPAP_ROOT}/third_party/patches/musl/libc_m68k.ld)
    set(PPAP_MUSL_SYSROOT  ${PPAP_SHARED_BUILD}/musl-sysroot)
    set(PPAP_MUSL_TARGET   m68k-elf)
    set(PPAP_SPECS_FILE    ${PPAP_SHARED_BUILD}/musl-m68k.specs)
    set(PPAP_BB_ARCH       m68k)
    set(PPAP_ARCH_LABEL    "m68k (68000)")

    execute_process(COMMAND ${PPAP_CC} -print-file-name=include
        OUTPUT_VARIABLE PPAP_GCC_INCLUDE OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${PPAP_CC} -m68000 -print-libgcc-file-name
        OUTPUT_VARIABLE PPAP_LIBGCC OUTPUT_STRIP_TRAILING_WHITESPACE)
    get_filename_component(PPAP_GCC_LIBDIR ${PPAP_LIBGCC} DIRECTORY)
elseif(PPAP_ARCH STREQUAL "riscv")
    # Prefer the Linux toolchain (built from source with --with-abi=ilp32)
    # for -pie support (proper GOT-based PIC + soft-float PIC libgcc).
    # Fall back to bare-metal toolchain if Linux toolchain is not built yet.
    if(DEFINED ENV{PPAP_RISCV_LINUX_TOOLCHAIN})
        set(PPAP_RISCV_LINUX_TC $ENV{PPAP_RISCV_LINUX_TOOLCHAIN})
    else()
        set(PPAP_RISCV_LINUX_TC ${PPAP_ROOT}/tools/riscv-linux-toolchain)
    endif()
    if(DEFINED ENV{PICO_TOOLCHAIN_PATH})
        set(PPAP_RISCV_ELF_TC $ENV{PICO_TOOLCHAIN_PATH})
    else()
        set(PPAP_RISCV_ELF_TC ${PPAP_ROOT}/tools/riscv-toolchain)
    endif()
    # Check if the Linux toolchain's libgcc is usable (soft-float + PIC).
    # Prebuilt riscv-collab packages default to rv32gc (double-float),
    # which can't link with our ilp32 musl.  When hard-float is detected,
    # fall back to bare-metal for everything.
    set(_use_linux_tc OFF)
    if(EXISTS ${PPAP_RISCV_LINUX_TC}/bin/riscv32-unknown-linux-gnu-gcc)
        execute_process(
            COMMAND ${PPAP_RISCV_LINUX_TC}/bin/riscv32-unknown-linux-gnu-gcc
                    -march=rv32imac_zicsr -mabi=ilp32 -print-libgcc-file-name
            OUTPUT_VARIABLE _linux_libgcc OUTPUT_STRIP_TRAILING_WHITESPACE)
        # Check float ABI via readelf (need bare-metal readelf since
        # the archive may not exist on the host)
        set(_readelf "readelf")
        if(EXISTS ${PPAP_RISCV_ELF_TC}/bin/riscv32-unknown-elf-readelf)
            set(_readelf ${PPAP_RISCV_ELF_TC}/bin/riscv32-unknown-elf-readelf)
        endif()
        execute_process(COMMAND ${_readelf} -h ${_linux_libgcc}
            OUTPUT_VARIABLE _libgcc_hdr ERROR_QUIET)
        if(NOT _libgcc_hdr MATCHES "double-float")
            set(_use_linux_tc ON)
        else()
            message(STATUS
                "RISC-V: Linux libgcc is hard-float — "
                "falling back to bare-metal toolchain")
        endif()
    endif()
    if(_use_linux_tc)
        set(PPAP_RISCV_TC      ${PPAP_RISCV_LINUX_TC})
        set(PPAP_CC            ${PPAP_RISCV_TC}/bin/riscv32-unknown-linux-gnu-gcc)
        set(PPAP_CROSS_PREFIX  ${PPAP_RISCV_TC}/bin/riscv32-unknown-linux-gnu-)
        set(PPAP_STRIP         ${PPAP_RISCV_TC}/bin/riscv32-unknown-linux-gnu-strip)
        set(PPAP_OBJCOPY       ${PPAP_RISCV_TC}/bin/riscv32-unknown-linux-gnu-objcopy)
        set(PPAP_SIZE_CMD      ${PPAP_RISCV_TC}/bin/riscv32-unknown-linux-gnu-size)
        set(PPAP_MUSL_TARGET   riscv32-unknown-linux-gnu)
        set(PPAP_RISCV_PIE     ON)
        message(STATUS "RISC-V: using Linux toolchain (PIE enabled)")
    else()
        set(PPAP_RISCV_TC      ${PPAP_RISCV_ELF_TC})
        set(PPAP_CC            ${PPAP_RISCV_TC}/bin/riscv32-unknown-elf-gcc)
        set(PPAP_CROSS_PREFIX  ${PPAP_RISCV_TC}/bin/riscv32-unknown-elf-)
        set(PPAP_STRIP         ${PPAP_RISCV_TC}/bin/riscv32-unknown-elf-strip)
        set(PPAP_OBJCOPY       ${PPAP_RISCV_TC}/bin/riscv32-unknown-elf-objcopy)
        set(PPAP_SIZE_CMD      ${PPAP_RISCV_TC}/bin/riscv32-unknown-elf-size)
        set(PPAP_MUSL_TARGET   riscv32-unknown-elf)
        set(PPAP_RISCV_PIE     OFF)
        message(STATUS "RISC-V: using bare-metal toolchain (no PIE, SRAM text+data)")
    endif()
    set(PPAP_ARCH_DIR      ${PPAP_ROOT}/src/user/arch/riscv)
    set(PPAP_TARGET_FLAGS  -march=rv32imac_zicsr -mabi=ilp32)
    set(PPAP_PIC_FLAGS     -fPIC -mcmodel=medany)
    set(PPAP_USER_LD       ${PPAP_ARCH_DIR}/user.ld)
    set(PPAP_BUSYBOX_LD    ${PPAP_ROOT}/third_party/patches/musl/libc_riscv.ld)
    set(PPAP_MUSL_SYSROOT  ${PPAP_SHARED_BUILD}/musl-sysroot)
    set(PPAP_SPECS_FILE    ${PPAP_SHARED_BUILD}/musl-riscv.specs)
    set(PPAP_BB_ARCH       riscv32)
    set(PPAP_ARCH_LABEL    "rv32imac (Hazard3)")

    execute_process(COMMAND ${PPAP_CC} -print-file-name=include
        OUTPUT_VARIABLE PPAP_GCC_INCLUDE OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${PPAP_CC} ${PPAP_TARGET_FLAGS}
                            -print-libgcc-file-name
        OUTPUT_VARIABLE PPAP_LIBGCC OUTPUT_STRIP_TRAILING_WHITESPACE)
    get_filename_component(PPAP_GCC_LIBDIR ${PPAP_LIBGCC} DIRECTORY)
else()
    set(PPAP_CC            arm-none-eabi-gcc)
    set(PPAP_CROSS_PREFIX  arm-none-eabi-)
    set(PPAP_STRIP         arm-none-eabi-strip)
    set(PPAP_SIZE_CMD      arm-none-eabi-size)
    set(PPAP_ARCH_DIR      ${PPAP_ROOT}/src/user/arch/arm_m)
    if(PPAP_ARM_HARDFLOAT)
        # Cortex-M33 with hardware FPU (softfp ABI — compatible with soft-float musl)
        set(PPAP_TARGET_FLAGS  -mthumb -mcpu=cortex-m33 -mfloat-abi=softfp -mfpu=fpv5-sp-d16)
        set(PPAP_ARCH_LABEL    "armv8m-thumb (Cortex-M33, FPU)")
    else()
        set(PPAP_TARGET_FLAGS  -mthumb -mcpu=cortex-m0plus -march=armv6s-m -mfloat-abi=soft)
        set(PPAP_ARCH_LABEL    "armv6m-thumb (Cortex-M0+)")
    endif()
    set(PPAP_PIC_FLAGS     -fPIC -msingle-pic-base -mpic-register=r9
                           -mno-pic-data-is-text-relative)
    set(PPAP_USER_LD       ${PPAP_ARCH_DIR}/user.ld)
    set(PPAP_BUSYBOX_LD    ${PPAP_ROOT}/third_party/patches/musl/libc_arm_m.ld)
    set(PPAP_MUSL_SYSROOT  ${PPAP_SHARED_BUILD}/musl-sysroot)
    set(PPAP_MUSL_TARGET   arm-none-eabi)
    set(PPAP_SPECS_FILE    ${PPAP_SHARED_BUILD}/musl-arm.specs)
    set(PPAP_BB_ARCH       arm)

    execute_process(COMMAND arm-none-eabi-gcc -print-file-name=include
        OUTPUT_VARIABLE PPAP_GCC_INCLUDE OUTPUT_STRIP_TRAILING_WHITESPACE)
    # libgcc must match the target float ABI
    execute_process(COMMAND arm-none-eabi-gcc ${PPAP_TARGET_FLAGS}
                            -print-libgcc-file-name
        OUTPUT_VARIABLE PPAP_LIBGCC OUTPUT_STRIP_TRAILING_WHITESPACE)
    get_filename_component(PPAP_GCC_LIBDIR ${PPAP_LIBGCC} DIRECTORY)
endif()

# Derived paths (shared artifacts in PPAP_SHARED_BUILD)
set(PPAP_MUSL_LIBC  ${PPAP_MUSL_SYSROOT}/lib/libc.a)
set(PPAP_BB_DIR     ${PPAP_SHARED_BUILD}/busybox)
set(PPAP_ROGUE_DIR  ${PPAP_SHARED_BUILD}/rogue)

# --- User program compile/link flags (cmake lists) ---
set(PPAP_USER_CFLAGS
    ${PPAP_TARGET_FLAGS}
    -ffreestanding -nostdlib -Os -g -Wall
    ${PPAP_PIC_FLAGS}
    -I${PPAP_ARCH_DIR} -I${PPAP_ROOT}/src/user -I${PPAP_ROOT}/src)

set(PPAP_USER_ASFLAGS ${PPAP_TARGET_FLAGS})

set(PPAP_USER_LDFLAGS
    -nostdlib -T ${PPAP_USER_LD} -Wl,--gc-sections -Wl,--build-id=none
    $<$<STREQUAL:${PPAP_ARCH},riscv>:-Wl$<COMMA>--no-relax>)

# --- String versions for shell config ---
string(JOIN " " PPAP_TARGET_FLAGS_STR ${PPAP_TARGET_FLAGS})
string(JOIN " " PPAP_PIC_FLAGS_STR    ${PPAP_PIC_FLAGS})
set(PPAP_MUSL_CFLAGS_STR
    "${PPAP_TARGET_FLAGS_STR} -Os -g ${PPAP_PIC_FLAGS_STR} -ffunction-sections -fdata-sections")
# RISC-V: -pie only when Linux toolchain is available (soft-float PIC libgcc).
# Bare-metal linker doesn't support -pie; uses --emit-relocs instead.
if(PPAP_ARCH STREQUAL "riscv" AND NOT PPAP_RISCV_PIE)
    set(_PIE_FLAG "")
else()
    set(_PIE_FLAG "-pie")
endif()
set(PPAP_APP_CFLAGS_STR
    "${PPAP_TARGET_FLAGS_STR} -Os -nostdinc -isystem ${PPAP_MUSL_SYSROOT}/include -isystem ${PPAP_GCC_INCLUDE} ${PPAP_PIC_FLAGS_STR} -ffunction-sections -fdata-sections ${_PIE_FLAG}")

# =============================================================================
# Generate specs file (used by busybox/rogue linking)
# =============================================================================
file(WRITE ${PPAP_SPECS_FILE}
"*startfile:
${PPAP_MUSL_SYSROOT}/lib/crt1.o ${PPAP_MUSL_SYSROOT}/lib/crti.o

*endfile:
${PPAP_MUSL_SYSROOT}/lib/crtn.o

*lib:
${PPAP_MUSL_SYSROOT}/lib/libc.a

*libgcc:
${PPAP_GCC_LIBDIR}/libgcc.a
")

# =============================================================================
# Generate shell config for build scripts
# =============================================================================
set(PPAP_CONFIG_FILE ${PPAP_SHARED_BUILD}/ppap-target-config.sh)
file(WRITE ${PPAP_CONFIG_FILE}
"# Generated by cmake — do not edit
PPAP_ARCH=\"${PPAP_ARCH}\"
PPAP_CC=\"${PPAP_CC}\"
PPAP_CROSS_PREFIX=\"${PPAP_CROSS_PREFIX}\"
PPAP_STRIP=\"${PPAP_STRIP}\"
PPAP_SIZE_CMD=\"${PPAP_SIZE_CMD}\"
PPAP_TARGET_FLAGS=\"${PPAP_TARGET_FLAGS_STR}\"
PPAP_PIC_FLAGS=\"${PPAP_PIC_FLAGS_STR}\"
PPAP_MUSL_SYSROOT=\"${PPAP_MUSL_SYSROOT}\"
PPAP_MUSL_TARGET=\"${PPAP_MUSL_TARGET}\"
PPAP_MUSL_CFLAGS=\"${PPAP_MUSL_CFLAGS_STR}\"
PPAP_APP_CFLAGS=\"${PPAP_APP_CFLAGS_STR}\"
PPAP_GCC_INCLUDE=\"${PPAP_GCC_INCLUDE}\"
PPAP_GCC_LIBDIR=\"${PPAP_GCC_LIBDIR}\"
PPAP_SPECS_FILE=\"${PPAP_SPECS_FILE}\"
PPAP_BUSYBOX_LD=\"${PPAP_BUSYBOX_LD}\"
PPAP_BB_ARCH=\"${PPAP_BB_ARCH}\"
PPAP_ARCH_LABEL=\"${PPAP_ARCH_LABEL}\"
PPAP_PIE_FLAG=\"${_PIE_FLAG}\"
")

# =============================================================================
# User program build functions
# =============================================================================

# ppap_user_program(name source [NO_CRT] [EXTRA_CFLAGS flag1 flag2 ...])
#
# Creates custom commands to compile and link a single user-space ELF.
# NO_CRT skips linking crt0.o + syscall.o (for pure-assembly programs).
# EXTRA_CFLAGS adds flags (e.g., -I for test headers).
function(ppap_user_program name source)
    cmake_parse_arguments(ARG "NO_CRT" "" "EXTRA_CFLAGS" ${ARGN})

    set(_elf ${PPAP_SHARED_BUILD}/${name}.elf)
    set(_objs "")
    set(_sources ${source})
    set(_extra_var "PPAP_USER_EXTRA_SOURCES_${name}")

    if(DEFINED ${_extra_var})
        list(APPEND _sources ${${_extra_var}})
    endif()

    foreach(_src ${_sources})
        get_filename_component(_src_base ${_src} NAME_WE)
        set(_obj ${PPAP_SHARED_BUILD}/${name}_${_src_base}.o)

        # Compile: assembly or C
        get_filename_component(_ext ${_src} LAST_EXT)
        if(_ext STREQUAL ".S" OR _ext STREQUAL ".s")
            add_custom_command(
                OUTPUT ${_obj}
                COMMAND ${PPAP_CC} ${PPAP_USER_ASFLAGS} -c -o ${_obj} ${_src}
                DEPENDS ${_src}
                COMMENT "Assembling ${name}_${_src_base}.o (${PPAP_ARCH})"
            )
        else()
            set(_cflags ${PPAP_USER_CFLAGS} ${ARG_EXTRA_CFLAGS})
            add_custom_command(
                OUTPUT ${_obj}
                COMMAND ${PPAP_CC} ${_cflags} -c -o ${_obj} ${_src}
                DEPENDS ${_src} ${PPAP_ROOT}/src/user/syscall.h
                COMMENT "Compiling ${name}_${_src_base}.o (${PPAP_ARCH})"
            )
        endif()

        list(APPEND _objs ${_obj})
    endforeach()

    # Link
    if(ARG_NO_CRT)
        set(_link_objs ${_objs})
    else()
        set(_link_objs ${PPAP_CRT_OBJS} ${_objs})
    endif()

    add_custom_command(
        OUTPUT ${_elf}
        COMMAND ${PPAP_CC} ${PPAP_TARGET_FLAGS} ${PPAP_USER_LDFLAGS}
                -o ${_elf} ${_link_objs} ${PPAP_LIBGCC}
        DEPENDS ${_link_objs} ${PPAP_USER_LD} ${PPAP_LIBGCC}
        COMMENT "Linking ${name}.elf (${PPAP_ARCH})"
    )
endfunction()

# ppap_musl_test_program(name source)
#
# Creates custom commands to compile and link a user-space ELF against musl
# libc (using the specs file and busybox linker script).  Used for test
# binaries that need to exercise musl code paths (e.g. double-free on exit).
function(ppap_musl_test_program name source)
    set(_elf ${PPAP_SHARED_BUILD}/${name}.elf)
    set(_obj ${PPAP_SHARED_BUILD}/${name}.o)

    # Compile with musl headers (same flags as busybox/rogue apps)
    set(_musl_cflags
        ${PPAP_TARGET_FLAGS} -Os -nostdinc
        -isystem ${PPAP_MUSL_SYSROOT}/include
        -isystem ${PPAP_GCC_INCLUDE}
        ${PPAP_PIC_FLAGS}
        -ffunction-sections -fdata-sections)
    if(_PIE_FLAG)
        list(APPEND _musl_cflags ${_PIE_FLAG})
    endif()
    add_custom_command(
        OUTPUT ${_obj}
        COMMAND ${PPAP_CC} ${_musl_cflags}
                -c -o ${_obj} ${source}
        DEPENDS ${source} ${PPAP_MUSL_LIBC}
        COMMENT "Compiling ${name}.o (musl, ${PPAP_ARCH})"
    )

    # Link with musl's crt + libc via specs file
    set(_musl_ldflags
        -specs=${PPAP_SPECS_FILE}
        -T ${PPAP_BUSYBOX_LD}
        -Wl,--gc-sections -Wl,--build-id=none
        $<$<STREQUAL:${PPAP_ARCH},riscv>:-Wl$<COMMA>--no-relax>)
    if(_PIE_FLAG)
        list(APPEND _musl_ldflags ${_PIE_FLAG})
    endif()
    if(PPAP_ARCH STREQUAL "riscv" AND NOT PPAP_RISCV_PIE)
        list(APPEND _musl_ldflags -Wl,--emit-relocs)
    endif()
    add_custom_command(
        OUTPUT ${_elf}
        COMMAND ${PPAP_CC} ${PPAP_TARGET_FLAGS} ${_musl_ldflags}
                -o ${_elf} ${_obj}
        DEPENDS ${_obj} ${PPAP_SPECS_FILE} ${PPAP_BUSYBOX_LD} ${PPAP_MUSL_LIBC}
        COMMENT "Linking ${name}.elf (musl, ${PPAP_ARCH})"
    )
endfunction()

# ppap_r68k_program(name source)
#
# Creates custom commands to compile, link, and objcopy a Human68k R-format
# flat binary (.r).  The binary has no headers and no relocations.
# Only available on m68k targets.
function(ppap_r68k_program name source)
    set(_obj ${PPAP_SHARED_BUILD}/${name}.o)
    set(_elf ${PPAP_SHARED_BUILD}/${name}_r68k.elf)
    set(_bin ${PPAP_SHARED_BUILD}/${name}.r)
    set(_cc ${PPAP_CC})
    set(_objcopy ${PPAP_OBJCOPY})
    set(_target_flags ${PPAP_TARGET_FLAGS})
    set(_r68k_ld ${PPAP_R68K_LD})

    if(NOT PPAP_ARCH STREQUAL "m68k")
        set(_cc ${PPAP_M68K_CC})
        set(_objcopy ${PPAP_M68K_OBJCOPY})
        set(_target_flags -m68000)
        set(_r68k_ld ${PPAP_M68K_R68K_LD})
    endif()

    # Compile
    get_filename_component(_ext ${source} LAST_EXT)
    if(_ext STREQUAL ".S" OR _ext STREQUAL ".s")
        add_custom_command(
            OUTPUT ${_obj}
            COMMAND ${_cc} ${_target_flags} -c -o ${_obj} ${source}
            DEPENDS ${source}
            COMMENT "Assembling ${name}.o (r68k)"
        )
    else()
        add_custom_command(
            OUTPUT ${_obj}
            COMMAND ${_cc} ${_target_flags}
                    -ffreestanding -nostdlib -Os -Wall -Werror
                    -c -o ${_obj} ${source}
            DEPENDS ${source}
            COMMENT "Compiling ${name}.o (r68k)"
        )
    endif()

    # Link (flat binary via r68k.ld, no CRT)
    add_custom_command(
        OUTPUT ${_elf}
        COMMAND ${_cc} ${_target_flags}
                -nostdlib -T ${_r68k_ld} -Wl,--build-id=none
                -o ${_elf} ${_obj}
        DEPENDS ${_obj} ${_r68k_ld}
        COMMENT "Linking ${name} (r68k)"
    )

    # objcopy to raw binary
    add_custom_command(
        OUTPUT ${_bin}
        COMMAND ${_objcopy} -O binary ${_elf} ${_bin}
        DEPENDS ${_elf}
        COMMENT "Generating ${name}.r (r68k flat binary)"
    )

    # Track output for romfs integration
    set_property(GLOBAL APPEND PROPERTY PPAP_R68K_BINS ${_bin})
endfunction()

# _ppap_build_user_programs()  [internal]
#
# Builds CRT objects, all user apps, and (if PPAP_TESTS) all test programs.
# Sets PPAP_USER_ELFS in the caller's scope.
function(_ppap_build_user_programs)
    # --- CRT objects ---
    add_custom_command(
        OUTPUT ${PPAP_SHARED_BUILD}/crt0.o
        COMMAND ${PPAP_CC} ${PPAP_USER_ASFLAGS} -c
                -o ${PPAP_SHARED_BUILD}/crt0.o ${PPAP_ARCH_DIR}/crt0.S
        DEPENDS ${PPAP_ARCH_DIR}/crt0.S
        COMMENT "Assembling crt0.o (${PPAP_ARCH})"
    )
    add_custom_command(
        OUTPUT ${PPAP_SHARED_BUILD}/syscall.o
        COMMAND ${PPAP_CC} ${PPAP_USER_ASFLAGS} -c
                -o ${PPAP_SHARED_BUILD}/syscall.o ${PPAP_ARCH_DIR}/syscall.S
        DEPENDS ${PPAP_ARCH_DIR}/syscall.S
        COMMENT "Assembling syscall.o (${PPAP_ARCH})"
    )
    set(PPAP_CRT_OBJS
        ${PPAP_SHARED_BUILD}/crt0.o ${PPAP_SHARED_BUILD}/syscall.o)

    # --- Application programs ---
    set(_all_elfs "")
    foreach(app ${USER_APPS})
        # getty: ARM uses arch-specific .S (no CRT), m68k uses generic .c
        if(app STREQUAL "getty" AND EXISTS ${PPAP_ARCH_DIR}/getty.S)
            ppap_user_program(${app} ${PPAP_ARCH_DIR}/getty.S NO_CRT)
        else()
            ppap_user_program(${app} ${PPAP_ROOT}/src/user/${app}.c)
        endif()
        list(APPEND _all_elfs ${PPAP_SHARED_BUILD}/${app}.elf)
    endforeach()

    # --- Test programs ---
    if(PPAP_TESTS)
        set(_tests_dir ${PPAP_ROOT}/tests/user)
        foreach(tst ${USER_TESTS})
            ppap_user_program(${tst} ${_tests_dir}/${tst}.c
                EXTRA_CFLAGS -I${_tests_dir})
            list(APPEND _all_elfs ${PPAP_SHARED_BUILD}/${tst}.elf)
        endforeach()
    endif()

    # --- Musl-linked test programs ---
    if(PPAP_TESTS)
        foreach(tst ${USER_MUSL_TESTS})
            ppap_musl_test_program(${tst} ${_tests_dir}/${tst}.c)
            list(APPEND _all_elfs ${PPAP_SHARED_BUILD}/${tst}.elf)
        endforeach()
    endif()

    # --- R68K user tests (Human68k DOS call tests) ---
    # Requires m68k cross-compiler; skip when not available (e.g. Docker
    # containers that only have the target arch's toolchain).
    if(PPAP_TESTS AND EXISTS ${PPAP_M68K_CC})
        set(_r68k_dir ${PPAP_ROOT}/tests/user/r68k)
        ppap_r68k_program(test_dos_basic ${_r68k_dir}/test_dos_basic.S)
        ppap_r68k_program(test_dos_mem   ${_r68k_dir}/test_dos_mem.S)
        ppap_r68k_program(test_dos_file  ${_r68k_dir}/test_dos_file.S)
        ppap_r68k_program(test_dos_dir    ${_r68k_dir}/test_dos_dir.S)
        ppap_r68k_program(test_dos_rename ${_r68k_dir}/test_dos_rename.S)
    endif()

    set(PPAP_USER_ELFS ${_all_elfs} PARENT_SCOPE)
endfunction()

# =============================================================================
# Third-party build targets
# =============================================================================

# _ppap_add_musl()  [internal]
# Registers a custom command to build musl libc.
# Output: ${PPAP_MUSL_LIBC}
function(_ppap_add_musl)
    file(GLOB_RECURSE _musl_overlay
        ${PPAP_ROOT}/third_party/patches/musl/overlay/*)

    add_custom_command(
        OUTPUT ${PPAP_MUSL_LIBC}
        COMMAND ${CMAKE_COMMAND} -E env "PPAP_CONFIG=${PPAP_CONFIG_FILE}"
                ${PPAP_ROOT}/third_party/build_musl.sh
        DEPENDS ${PPAP_ROOT}/third_party/build_musl.sh
                ${_musl_overlay}
        COMMENT "Building musl libc (${PPAP_ARCH_LABEL})"
    )
endfunction()

# _ppap_add_busybox()  [internal]
# Registers a custom command to build busybox.
# Output: ${PPAP_BB_DIR}/busybox
function(_ppap_add_busybox)
    add_custom_command(
        OUTPUT ${PPAP_BB_DIR}/busybox
        COMMAND ${CMAKE_COMMAND} -E env "PPAP_CONFIG=${PPAP_CONFIG_FILE}"
                ${PPAP_ROOT}/third_party/build_busybox.sh
        DEPENDS ${PPAP_ROOT}/third_party/build_busybox.sh
                ${PPAP_ROOT}/third_party/patches/busybox/busybox_ppap.fragment
                ${PPAP_BUSYBOX_LD}
                ${PPAP_MUSL_LIBC}
        COMMENT "Building busybox (${PPAP_ARCH_LABEL})"
    )
endfunction()

# _ppap_add_rogue()  [internal]
# Registers a custom command to build rogue.
# Output: ${PPAP_ROGUE_DIR}/rogue
function(_ppap_add_rogue)
    file(GLOB _rogue_patches ${PPAP_ROOT}/third_party/patches/rogue/*)

    add_custom_command(
        OUTPUT ${PPAP_ROGUE_DIR}/rogue
        COMMAND ${CMAKE_COMMAND} -E env "PPAP_CONFIG=${PPAP_CONFIG_FILE}"
                ${PPAP_ROOT}/third_party/build_rogue.sh
        DEPENDS ${PPAP_ROOT}/third_party/build_rogue.sh
                ${_rogue_patches}
                ${PPAP_BUSYBOX_LD}
                ${PPAP_MUSL_LIBC}
        COMMENT "Building rogue (${PPAP_ARCH_LABEL})"
    )
endfunction()

# =============================================================================
# Romfs image pipeline
# =============================================================================

# _ppap_add_mkromfs()  [internal]
# Registers a custom command to build the mkromfs host tool.
# Output: ${PPAP_SHARED_BUILD}/mkromfs
function(_ppap_add_mkromfs)
    add_custom_command(
        OUTPUT ${PPAP_SHARED_BUILD}/mkromfs
        COMMAND cc -O2 -I ${PPAP_ROOT}/src/kernel/fs
                -o ${PPAP_SHARED_BUILD}/mkromfs
                ${PPAP_ROOT}/tools/mkromfs/mkromfs.c
        DEPENDS ${PPAP_ROOT}/tools/mkromfs/mkromfs.c
                ${PPAP_ROOT}/src/kernel/fs/romfs_format.h
        COMMENT "Building mkromfs host tool"
    )
endfunction()

# ppap_generate_romfs(target [BIG_ENDIAN] [OVERLAY_DIR dir])
#
# Generates a romfs image and links it into the given cmake target.
# Uses PPAP_USER_ELFS (set at include time) for user-space binaries.
# BIG_ENDIAN passes -b to mkromfs (for m68k).
# OVERLAY_DIR specifies a directory whose contents overlay src/etc/.
function(ppap_generate_romfs target)
    cmake_parse_arguments(ARG "BIG_ENDIAN;NO_ROGUE" "OVERLAY_DIR" "EXCLUDE_APPS" ${ARGN})

    # Rogue binary (optional — skip with NO_ROGUE for size-constrained targets)
    if(ARG_NO_ROGUE)
        set(_rogue_path "")
        set(_rogue_dep "")
    else()
        set(_rogue_path "${PPAP_ROGUE_DIR}/rogue")
        set(_rogue_dep  "${PPAP_ROGUE_DIR}/rogue")
    endif()

    set(_romfs_staging ${CMAKE_BINARY_DIR}/romfs_${target})
    set(_romfs_bin     ${CMAKE_BINARY_DIR}/romfs_${target}.bin)
    set(_romfs_asm     ${CMAKE_BINARY_DIR}/romfs_data_${target}.S)

    # mkromfs flags
    set(_mkromfs_flags "")
    if(ARG_BIG_ENDIAN)
        set(_mkromfs_flags -b)
    endif()

    # .section syntax: ARM uses %progbits, m68k omits it
    if(PPAP_ARCH STREQUAL "m68k")
        set(_section "    .section .romfs, \"a\"")
    else()
        set(_section "    .section .romfs, \"a\", %progbits")
    endif()

    # Overlay arguments and dependencies
    set(_overlay_args "")
    if(ARG_OVERLAY_DIR)
        set(_overlay_args -D "OVERLAY_DIR=${ARG_OVERLAY_DIR}")
    endif()
    if(PPAP_EXTRA_OVERLAY)
        list(APPEND _overlay_args -D "EXTRA_OVERLAY_DIR=${PPAP_EXTRA_OVERLAY}")
    endif()

    set(_overlay_deps "")
    if(ARG_OVERLAY_DIR AND EXISTS ${ARG_OVERLAY_DIR})
        file(GLOB_RECURSE _overlay_glob ${ARG_OVERLAY_DIR}/*)
        foreach(_path IN LISTS _overlay_glob)
            if(NOT IS_DIRECTORY "${_path}")
                list(APPEND _overlay_deps "${_path}")
            endif()
        endforeach()
    endif()
    if(PPAP_EXTRA_OVERLAY AND EXISTS ${PPAP_EXTRA_OVERLAY})
        file(GLOB_RECURSE _extra_overlay_glob ${PPAP_EXTRA_OVERLAY}/*)
        foreach(_path IN LISTS _extra_overlay_glob)
            if(NOT IS_DIRECTORY "${_path}")
                list(APPEND _overlay_deps "${_path}")
            endif()
        endforeach()
    endif()
    list(SORT _overlay_deps)
    set(_overlay_manifest ${CMAKE_BINARY_DIR}/romfs_overlay_${target}.manifest)
    file(WRITE ${_overlay_manifest} "")
    foreach(_path IN LISTS _overlay_deps)
        file(SHA256 "${_path}" _overlay_sha)
        file(APPEND ${_overlay_manifest} "${_path}|${_overlay_sha}\n")
    endforeach()

    # R68K binaries (Human68k .r programs)
    get_property(_r68k_bins GLOBAL PROPERTY PPAP_R68K_BINS)
    set(_r68k_copy_cmds "")
    if(_r68k_bins)
        list(APPEND _r68k_copy_cmds
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    ${_romfs_staging}/subsys/human68k)
        foreach(_rbin IN LISTS _r68k_bins)
            list(APPEND _r68k_copy_cmds
                COMMAND ${CMAKE_COMMAND} -E copy ${_rbin}
                        ${_romfs_staging}/subsys/human68k/)
        endforeach()
    endif()

    # Shared Human68k X-format fixtures used by userland tests.
    file(GLOB _x68k_bins
        ${PPAP_ROOT}/src/target/qemu_m68k/romfs/subsys/human68k/*.x)
    set(_x68k_copy_cmds "")
    if(_x68k_bins)
        list(APPEND _x68k_copy_cmds
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    ${_romfs_staging}/subsys/human68k)
        foreach(_xbin IN LISTS _x68k_bins)
            list(APPEND _x68k_copy_cmds
                COMMAND ${CMAKE_COMMAND} -E copy ${_xbin}
                        ${_romfs_staging}/subsys/human68k/)
        endforeach()
    endif()

    # CMake list separator for -D arguments
    string(REPLACE ";" "\\;" _user_elfs_escaped "${PPAP_USER_ELFS}")
    string(REPLACE ";" "\\;" _bb_applets_escaped "${BB_APPLETS}")
    string(REPLACE ";" "\\;" _bb_sbin_escaped "${BB_SBIN_APPLETS}")
    set(_exclude_args "")
    if(ARG_EXCLUDE_APPS)
        string(REPLACE ";" ":" _exclude_colon "${ARG_EXCLUDE_APPS}")
        set(_exclude_args -D "EXCLUDE_APPS=${_exclude_colon}")
    endif()

    add_custom_command(
        OUTPUT ${_romfs_bin}
        COMMAND ${CMAKE_COMMAND}
                -D "STAGING=${_romfs_staging}"
                -D "PROJECT_ROOT=${PPAP_ROOT}"
                -D "USER_ELFS=${_user_elfs_escaped}"
                -D "BB_DIR=${PPAP_BB_DIR}"
                -D "BB_APPLETS=${_bb_applets_escaped}"
                -D "BB_SBIN_APPLETS=${_bb_sbin_escaped}"
                -D "ROGUE=${_rogue_path}"
                -D "ETC_DIR=${PPAP_ROOT}/src/etc"
                ${_overlay_args}
                ${_exclude_args}
                -P ${PPAP_ROOT}/cmake/stage_romfs.cmake
        ${_r68k_copy_cmds}
        ${_x68k_copy_cmds}
        COMMAND ${PPAP_SHARED_BUILD}/mkromfs ${_mkromfs_flags}
                ${_romfs_staging} ${_romfs_bin}
        DEPENDS ${PPAP_SHARED_BUILD}/mkromfs
                ${PPAP_USER_ELFS}
                ${PPAP_BB_DIR}/busybox
                ${_rogue_dep}
                ${PPAP_ROOT}/src/etc/fstab
                ${PPAP_ROOT}/src/etc/inittab
                ${_overlay_manifest}
                ${_r68k_bins}
                ${_x68k_bins}
                ${PPAP_ROOT}/cmake/stage_romfs.cmake
        COMMENT "Generating romfs for ${target}"
    )

    file(WRITE ${_romfs_asm}
        "${_section}\n    .incbin \"${_romfs_bin}\"\n")
    set_source_files_properties(${_romfs_asm}
        PROPERTIES OBJECT_DEPENDS ${_romfs_bin})

    add_custom_target(romfs_image_${target} DEPENDS ${_romfs_bin})
    add_dependencies(${target} romfs_image_${target})
    target_sources(${target} PRIVATE ${_romfs_asm})
endfunction()

# =============================================================================
# Auto-register all shared builds at include time.
#
# After this point, callers only need ppap_generate_romfs() per target.
# Adding a new userland app or third-party component requires editing
# only this file — all targets pick it up.
# =============================================================================

# =============================================================================
# Cross-compiled m68k binaries for ARM targets (eCPU emulation)
# =============================================================================

# ppap_m68k_cross_program(name source)
#
# When building for ARM, cross-compiles a standalone m68k ELF binary using
# the m68k toolchain.  The resulting ELF is added to PPAP_M68K_CROSS_ELFS
# and included in the romfs.  No-op on native m68k builds.
function(ppap_m68k_cross_program name source)
    if(PPAP_ARCH STREQUAL "m68k")
        return()
    endif()

    # If the ELF was pre-built (e.g. by build.sh using a Docker container),
    # use it directly instead of invoking m68k-elf-gcc.
    set(_prebuilt ${PPAP_ROOT}/build/shared_m68k/${name}_m68k.elf)
    if(EXISTS ${_prebuilt})
        set_property(GLOBAL APPEND PROPERTY PPAP_M68K_CROSS_ELFS ${_prebuilt})
        return()
    endif()

    if(NOT DEFINED ENV{PPAP_M68K_TOOLCHAIN})
        message(FATAL_ERROR
            "m68k-elf-gcc not available and no pre-built "
            "${name}_m68k.elf in build/shared_m68k/. "
            "Run: ./scripts/build.sh to pre-build cross binaries via Docker.")
    endif()
    set(_m68k_tc  $ENV{PPAP_M68K_TOOLCHAIN})
    set(_m68k_cc  ${_m68k_tc}/bin/m68k-elf-gcc)
    set(_m68k_ld  ${PPAP_ROOT}/src/user/arch/m68k/user.ld)
    execute_process(COMMAND ${_m68k_cc} -m68000 -print-libgcc-file-name
        OUTPUT_VARIABLE _m68k_libgcc OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(_obj ${PPAP_SHARED_BUILD}/${name}_m68k.o)
    set(_elf ${PPAP_SHARED_BUILD}/${name}_m68k.elf)

    get_filename_component(_ext ${source} LAST_EXT)
    if(_ext STREQUAL ".S" OR _ext STREQUAL ".s")
        add_custom_command(
            OUTPUT ${_obj}
            COMMAND ${_m68k_cc} -m68000 -c -o ${_obj} ${source}
            DEPENDS ${source}
            COMMENT "Cross-assembling ${name}_m68k.o (m68k for ARM)"
        )
    else()
        add_custom_command(
            OUTPUT ${_obj}
            COMMAND ${_m68k_cc} -m68000 -ffreestanding -nostdlib -Os
                    -c -o ${_obj} ${source}
            DEPENDS ${source}
            COMMENT "Cross-compiling ${name}_m68k.o (m68k for ARM)"
        )
    endif()

    add_custom_command(
        OUTPUT ${_elf}
        COMMAND ${_m68k_cc} -m68000 -nostdlib -T ${_m68k_ld}
                -Wl,--gc-sections -Wl,--build-id=none
                -o ${_elf} ${_obj} ${_m68k_libgcc}
        DEPENDS ${_obj} ${_m68k_ld} ${_m68k_libgcc}
        COMMENT "Cross-linking ${name}_m68k.elf (m68k for ARM)"
    )

    set_property(GLOBAL APPEND PROPERTY PPAP_M68K_CROSS_ELFS ${_elf})
endfunction()

# _ppap_build_m68k_cross_programs()  [internal]
#
# Builds m68k user programs for cross-arch emulation on ARM.
# Appends cross-compiled ELFs to PPAP_USER_ELFS.
function(_ppap_build_m68k_cross_programs)
    if(PPAP_ARCH STREQUAL "m68k")
        return()
    endif()

    ppap_m68k_cross_program(hello
        ${PPAP_ROOT}/src/user/arch/m68k/hello.S)

    get_property(_cross_elfs GLOBAL PROPERTY PPAP_M68K_CROSS_ELFS)
    list(APPEND PPAP_USER_ELFS ${_cross_elfs})
    set(PPAP_USER_ELFS ${PPAP_USER_ELFS} PARENT_SCOPE)
endfunction()

# =============================================================================
# Auto-register all shared builds at include time.
#
# After this point, callers only need ppap_generate_romfs() per target.
# Adding a new userland app or third-party component requires editing
# only this file — all targets pick it up.
# =============================================================================

_ppap_add_musl()
_ppap_add_busybox()
_ppap_add_rogue()
_ppap_build_user_programs()
_ppap_build_m68k_cross_programs()
_ppap_add_mkromfs()
