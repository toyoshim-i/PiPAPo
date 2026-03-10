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
else()
    set(PPAP_ARCH arm)
endif()

# --- User-space program lists ---

# Application programs (sources in src/user/)
set(USER_APPS hello getty init ttyctl)
# Install destinations: init -> sbin, ttyctl -> usr/bin, others -> bin

# Test programs (sources in tests/user/)
set(USER_TESTS
    test_exec test_vfork test_fault test_pipe test_brk
    test_fd test_signal test_poll test_sleep_intr test_orphan
    test_id test_fs test_rw test_time test_iov test_stat test_tmpfs
    runtests
)

# --- Busybox applet lists ---

# Applets that link to full busybox binary (transient commands)
set(BB_APPLETS
    cat chmod cp df echo grep head kill ln ls mkdir mv
    printf ps rm rmdir sed sleep sort tail top uname vi wc
)
# Shell applets — link to busybox.sh (dedicated shell binary)
set(BB_SHELL_APPLETS sh hush)
# Sbin applets — link to full busybox via ../bin/busybox
set(BB_SBIN_APPLETS mount umount)

# =============================================================================
# Arch-specific variables
# =============================================================================

if(PPAP_ARCH STREQUAL "m68k")
    set(PPAP_M68K_TC      ${PPAP_ROOT}/tools/m68k-toolchain)
    set(PPAP_CC            ${PPAP_M68K_TC}/bin/m68k-elf-gcc)
    set(PPAP_CROSS_PREFIX  ${PPAP_M68K_TC}/bin/m68k-elf-)
    set(PPAP_STRIP         ${PPAP_M68K_TC}/bin/m68k-elf-strip)
    set(PPAP_SIZE_CMD      ${PPAP_M68K_TC}/bin/m68k-elf-size)
    set(PPAP_ARCH_DIR      ${PPAP_ROOT}/src/user/arch/m68k)
    set(PPAP_TARGET_FLAGS  -m68000)
    set(PPAP_PIC_FLAGS     -msep-data)
    set(PPAP_USER_LD       ${PPAP_ARCH_DIR}/user.ld)
    set(PPAP_BUSYBOX_LD    ${PPAP_ROOT}/third_party/patches/musl/libc_m68k.ld)
    set(PPAP_MUSL_SYSROOT  ${PPAP_SHARED_BUILD}/musl-sysroot)
    set(PPAP_MUSL_TARGET   m68k-elf)
    set(PPAP_SPECS_FILE    ${PPAP_SHARED_BUILD}/musl-m68k.specs)
    set(PPAP_BB_ARCH       m68k)
    set(PPAP_ARCH_LABEL    "m68k (68000)")

    execute_process(COMMAND ${PPAP_CC} -print-file-name=include
        OUTPUT_VARIABLE PPAP_GCC_INCLUDE OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${PPAP_CC} -m68000 -print-libgcc-file-name
        OUTPUT_VARIABLE _libgcc OUTPUT_STRIP_TRAILING_WHITESPACE)
    get_filename_component(PPAP_GCC_LIBDIR ${_libgcc} DIRECTORY)
else()
    set(PPAP_CC            arm-none-eabi-gcc)
    set(PPAP_CROSS_PREFIX  arm-none-eabi-)
    set(PPAP_STRIP         arm-none-eabi-strip)
    set(PPAP_SIZE_CMD      arm-none-eabi-size)
    set(PPAP_ARCH_DIR      ${PPAP_ROOT}/src/user/arch/arm_m)
    set(PPAP_TARGET_FLAGS  -mthumb -mcpu=cortex-m0plus -march=armv6s-m -mfloat-abi=soft)
    set(PPAP_PIC_FLAGS     -fPIC -msingle-pic-base -mpic-register=r9
                           -mno-pic-data-is-text-relative)
    set(PPAP_USER_LD       ${PPAP_ARCH_DIR}/user.ld)
    set(PPAP_BUSYBOX_LD    ${PPAP_ROOT}/third_party/patches/musl/libc_arm_m.ld)
    set(PPAP_MUSL_SYSROOT  ${PPAP_SHARED_BUILD}/musl-sysroot)
    set(PPAP_MUSL_TARGET   arm-none-eabi)
    set(PPAP_SPECS_FILE    ${PPAP_SHARED_BUILD}/musl-arm.specs)
    set(PPAP_BB_ARCH       arm)
    set(PPAP_ARCH_LABEL    "armv6m-thumb (Cortex-M0+)")

    execute_process(COMMAND arm-none-eabi-gcc -print-file-name=include
        OUTPUT_VARIABLE PPAP_GCC_INCLUDE OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND arm-none-eabi-gcc -mthumb -mcpu=cortex-m0plus
                            -print-libgcc-file-name
        OUTPUT_VARIABLE _libgcc OUTPUT_STRIP_TRAILING_WHITESPACE)
    get_filename_component(PPAP_GCC_LIBDIR ${_libgcc} DIRECTORY)
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
    -nostdlib -T ${PPAP_USER_LD} -Wl,--gc-sections -Wl,--build-id=none)

# --- String versions for shell config ---
string(JOIN " " PPAP_TARGET_FLAGS_STR ${PPAP_TARGET_FLAGS})
string(JOIN " " PPAP_PIC_FLAGS_STR    ${PPAP_PIC_FLAGS})
set(PPAP_MUSL_CFLAGS_STR
    "${PPAP_TARGET_FLAGS_STR} -Os -g ${PPAP_PIC_FLAGS_STR} -ffunction-sections -fdata-sections")
set(PPAP_APP_CFLAGS_STR
    "${PPAP_TARGET_FLAGS_STR} -Os -nostdinc -isystem ${PPAP_MUSL_SYSROOT}/include -isystem ${PPAP_GCC_INCLUDE} ${PPAP_PIC_FLAGS_STR} -ffunction-sections -fdata-sections -pie")

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

    set(_obj ${PPAP_SHARED_BUILD}/${name}.o)
    set(_elf ${PPAP_SHARED_BUILD}/${name}.elf)

    # Compile: assembly or C
    get_filename_component(_ext ${source} LAST_EXT)
    if(_ext STREQUAL ".S" OR _ext STREQUAL ".s")
        add_custom_command(
            OUTPUT ${_obj}
            COMMAND ${PPAP_CC} ${PPAP_USER_ASFLAGS} -c -o ${_obj} ${source}
            DEPENDS ${source}
            COMMENT "Assembling ${name}.o (${PPAP_ARCH})"
        )
    else()
        set(_cflags ${PPAP_USER_CFLAGS} ${ARG_EXTRA_CFLAGS})
        add_custom_command(
            OUTPUT ${_obj}
            COMMAND ${PPAP_CC} ${_cflags} -c -o ${_obj} ${source}
            DEPENDS ${source} ${PPAP_ROOT}/src/user/syscall.h
            COMMENT "Compiling ${name}.o (${PPAP_ARCH})"
        )
    endif()

    # Link
    if(ARG_NO_CRT)
        set(_link_objs ${_obj})
    else()
        set(_link_objs ${PPAP_CRT_OBJS} ${_obj})
    endif()

    add_custom_command(
        OUTPUT ${_elf}
        COMMAND ${PPAP_CC} ${PPAP_TARGET_FLAGS} ${PPAP_USER_LDFLAGS}
                -o ${_elf} ${_link_objs}
        DEPENDS ${_link_objs} ${PPAP_USER_LD}
        COMMENT "Linking ${name}.elf (${PPAP_ARCH})"
    )
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
# Registers a custom command to build busybox variants.
# Output: ${PPAP_BB_DIR}/busybox, ${PPAP_BB_DIR}/busybox.sh
function(_ppap_add_busybox)
    add_custom_command(
        OUTPUT ${PPAP_BB_DIR}/busybox ${PPAP_BB_DIR}/busybox.sh
        COMMAND ${CMAKE_COMMAND} -E env "PPAP_CONFIG=${PPAP_CONFIG_FILE}"
                ${PPAP_ROOT}/third_party/build_busybox.sh
        DEPENDS ${PPAP_ROOT}/third_party/build_busybox.sh
                ${PPAP_ROOT}/third_party/patches/busybox/busybox_ppap.fragment
                ${PPAP_ROOT}/third_party/patches/busybox/busybox_sh.fragment
                ${PPAP_BUSYBOX_LD}
                ${PPAP_MUSL_LIBC}
        COMMENT "Building busybox variants (${PPAP_ARCH_LABEL})"
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
    cmake_parse_arguments(ARG "BIG_ENDIAN" "OVERLAY_DIR" "" ${ARGN})

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

    set(_overlay_deps "")
    if(ARG_OVERLAY_DIR AND EXISTS ${ARG_OVERLAY_DIR})
        file(GLOB_RECURSE _overlay_deps ${ARG_OVERLAY_DIR}/*)
    endif()

    # CMake list separator for -D arguments
    string(REPLACE ";" "\\;" _user_elfs_escaped "${PPAP_USER_ELFS}")
    string(REPLACE ";" "\\;" _bb_applets_escaped "${BB_APPLETS}")
    string(REPLACE ";" "\\;" _bb_shell_escaped "${BB_SHELL_APPLETS}")
    string(REPLACE ";" "\\;" _bb_sbin_escaped "${BB_SBIN_APPLETS}")

    add_custom_command(
        OUTPUT ${_romfs_bin}
        COMMAND ${CMAKE_COMMAND}
                -D "STAGING=${_romfs_staging}"
                -D "PROJECT_ROOT=${PPAP_ROOT}"
                -D "USER_ELFS=${_user_elfs_escaped}"
                -D "BB_DIR=${PPAP_BB_DIR}"
                -D "BB_APPLETS=${_bb_applets_escaped}"
                -D "BB_SHELL_APPLETS=${_bb_shell_escaped}"
                -D "BB_SBIN_APPLETS=${_bb_sbin_escaped}"
                -D "ROGUE=${PPAP_ROGUE_DIR}/rogue"
                -D "ETC_DIR=${PPAP_ROOT}/src/etc"
                ${_overlay_args}
                -P ${PPAP_ROOT}/cmake/stage_romfs.cmake
        COMMAND ${PPAP_SHARED_BUILD}/mkromfs ${_mkromfs_flags}
                ${_romfs_staging} ${_romfs_bin}
        DEPENDS ${PPAP_SHARED_BUILD}/mkromfs
                ${PPAP_USER_ELFS}
                ${PPAP_BB_DIR}/busybox
                ${PPAP_ROGUE_DIR}/rogue
                ${PPAP_ROOT}/src/etc/fstab
                ${PPAP_ROOT}/src/etc/inittab
                ${_overlay_deps}
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

_ppap_add_musl()
_ppap_add_busybox()
_ppap_add_rogue()
_ppap_build_user_programs()
_ppap_add_mkromfs()
