# ppap-userland.cmake — Common userland build configuration
#
# Included by root CMakeLists.txt (ARM targets) and
# qemu_m68k/CMakeLists.txt (m68k target).
#
# Provides:
#   - PPAP_* variables for arch-specific compiler/linker configuration
#   - ppap_build_user_programs(out_var) — build all user ELFs
#   - ppap_add_musl() — custom command for musl libc
#   - ppap_add_busybox() — custom command for busybox variants
#   - ppap_add_rogue() — custom command for rogue
#   - Generated ppap-target-config.sh for shell build scripts
#   - Generated specs file for musl-linked binaries

include_guard(GLOBAL)

# --- Project root (derived from this file's location: cmake/..) ---
get_filename_component(PPAP_ROOT ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)

# --- Detect architecture from cmake toolchain ---
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "m68k")
    set(PPAP_ARCH m68k)
else()
    set(PPAP_ARCH arm)
endif()

# --- Include shared source and program lists ---
include(${CMAKE_CURRENT_LIST_DIR}/kernel_sources.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/user_programs.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/busybox_applets.cmake)

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
    set(PPAP_BUSYBOX_LD    ${PPAP_ROOT}/third_party/configs/busybox-m68k.ld)
    set(PPAP_MUSL_SYSROOT  ${CMAKE_BINARY_DIR}/musl-sysroot)
    set(PPAP_MUSL_TARGET   m68k-elf)
    set(PPAP_SPECS_FILE    ${CMAKE_BINARY_DIR}/musl-m68k.specs)
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
    set(PPAP_BUSYBOX_LD    ${PPAP_ROOT}/third_party/configs/busybox.ld)
    set(PPAP_MUSL_SYSROOT  ${CMAKE_BINARY_DIR}/musl-sysroot)
    set(PPAP_MUSL_TARGET   arm-none-eabi)
    set(PPAP_SPECS_FILE    ${CMAKE_BINARY_DIR}/musl-arm.specs)
    set(PPAP_BB_ARCH       arm)
    set(PPAP_ARCH_LABEL    "armv6m-thumb (Cortex-M0+)")

    execute_process(COMMAND arm-none-eabi-gcc -print-file-name=include
        OUTPUT_VARIABLE PPAP_GCC_INCLUDE OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND arm-none-eabi-gcc -mthumb -mcpu=cortex-m0plus
                            -print-libgcc-file-name
        OUTPUT_VARIABLE _libgcc OUTPUT_STRIP_TRAILING_WHITESPACE)
    get_filename_component(PPAP_GCC_LIBDIR ${_libgcc} DIRECTORY)
endif()

# Derived paths
set(PPAP_MUSL_LIBC  ${PPAP_MUSL_SYSROOT}/lib/libc.a)
set(PPAP_BB_DIR     ${CMAKE_BINARY_DIR}/busybox)
set(PPAP_ROGUE_DIR  ${CMAKE_BINARY_DIR}/rogue)

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
set(PPAP_CONFIG_FILE ${CMAKE_BINARY_DIR}/ppap-target-config.sh)
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

    set(_obj ${CMAKE_BINARY_DIR}/${name}.o)
    set(_elf ${CMAKE_BINARY_DIR}/${name}.elf)

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

# ppap_build_user_programs(out_var)
#
# Builds CRT objects, all user apps, and (if PPAP_TESTS) all test programs.
# Sets ${out_var} to the list of output ELF paths in the caller's scope.
function(ppap_build_user_programs out_var)
    # --- CRT objects ---
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/crt0.o
        COMMAND ${PPAP_CC} ${PPAP_USER_ASFLAGS} -c
                -o ${CMAKE_BINARY_DIR}/crt0.o ${PPAP_ARCH_DIR}/crt0.S
        DEPENDS ${PPAP_ARCH_DIR}/crt0.S
        COMMENT "Assembling crt0.o (${PPAP_ARCH})"
    )
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/syscall.o
        COMMAND ${PPAP_CC} ${PPAP_USER_ASFLAGS} -c
                -o ${CMAKE_BINARY_DIR}/syscall.o ${PPAP_ARCH_DIR}/syscall.S
        DEPENDS ${PPAP_ARCH_DIR}/syscall.S
        COMMENT "Assembling syscall.o (${PPAP_ARCH})"
    )
    set(PPAP_CRT_OBJS
        ${CMAKE_BINARY_DIR}/crt0.o ${CMAKE_BINARY_DIR}/syscall.o)

    # --- Application programs ---
    set(_all_elfs "")
    foreach(app ${USER_APPS})
        # getty: ARM uses arch-specific .S (no CRT), m68k uses generic .c
        if(app STREQUAL "getty" AND EXISTS ${PPAP_ARCH_DIR}/getty.S)
            ppap_user_program(${app} ${PPAP_ARCH_DIR}/getty.S NO_CRT)
        else()
            ppap_user_program(${app} ${PPAP_ROOT}/src/user/${app}.c)
        endif()
        list(APPEND _all_elfs ${CMAKE_BINARY_DIR}/${app}.elf)
    endforeach()

    # --- Test programs ---
    if(PPAP_TESTS)
        set(_tests_dir ${PPAP_ROOT}/tests/user)
        foreach(tst ${USER_TESTS})
            ppap_user_program(${tst} ${_tests_dir}/${tst}.c
                EXTRA_CFLAGS -I${_tests_dir})
            list(APPEND _all_elfs ${CMAKE_BINARY_DIR}/${tst}.elf)
        endforeach()
    endif()

    set(${out_var} ${_all_elfs} PARENT_SCOPE)
endfunction()

# =============================================================================
# Third-party build targets
# =============================================================================

# ppap_add_musl()
# Registers a custom command to build musl libc.
# Output: ${PPAP_MUSL_LIBC}
function(ppap_add_musl)
    file(GLOB_RECURSE _musl_overlay
        ${PPAP_ROOT}/third_party/patches/musl/overlay/*)

    add_custom_command(
        OUTPUT ${PPAP_MUSL_LIBC}
        COMMAND ${CMAKE_COMMAND} -E env "PPAP_CONFIG=${PPAP_CONFIG_FILE}"
                ${PPAP_ROOT}/third_party/build-musl.sh
        DEPENDS ${PPAP_ROOT}/third_party/build-musl.sh
                ${_musl_overlay}
        COMMENT "Building musl libc (${PPAP_ARCH_LABEL})"
    )
endfunction()

# ppap_add_busybox()
# Registers a custom command to build busybox variants.
# Output: ${PPAP_BB_DIR}/busybox, ${PPAP_BB_DIR}/busybox.sh
function(ppap_add_busybox)
    add_custom_command(
        OUTPUT ${PPAP_BB_DIR}/busybox ${PPAP_BB_DIR}/busybox.sh
        COMMAND ${CMAKE_COMMAND} -E env "PPAP_CONFIG=${PPAP_CONFIG_FILE}"
                ${PPAP_ROOT}/third_party/build-busybox.sh
        DEPENDS ${PPAP_ROOT}/third_party/build-busybox.sh
                ${PPAP_ROOT}/third_party/configs/busybox_ppap.fragment
                ${PPAP_ROOT}/third_party/configs/busybox_sh.fragment
                ${PPAP_BUSYBOX_LD}
                ${PPAP_MUSL_LIBC}
        COMMENT "Building busybox variants (${PPAP_ARCH_LABEL})"
    )
endfunction()

# ppap_add_rogue()
# Registers a custom command to build rogue.
# Output: ${PPAP_ROGUE_DIR}/rogue
function(ppap_add_rogue)
    file(GLOB _rogue_patches ${PPAP_ROOT}/third_party/patches/rogue/*)

    add_custom_command(
        OUTPUT ${PPAP_ROGUE_DIR}/rogue
        COMMAND ${CMAKE_COMMAND} -E env "PPAP_CONFIG=${PPAP_CONFIG_FILE}"
                ${PPAP_ROOT}/third_party/build-rogue.sh
        DEPENDS ${PPAP_ROOT}/third_party/build-rogue.sh
                ${_rogue_patches}
                ${PPAP_BUSYBOX_LD}
                ${PPAP_MUSL_LIBC}
        COMMENT "Building rogue (${PPAP_ARCH_LABEL})"
    )
endfunction()

# =============================================================================
# Romfs image pipeline
# =============================================================================

# ppap_add_mkromfs()
# Registers a custom command to build the mkromfs host tool.
# Output: ${CMAKE_BINARY_DIR}/mkromfs
function(ppap_add_mkromfs)
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/mkromfs
        COMMAND cc -O2 -I ${PPAP_ROOT}/src/kernel/fs
                -o ${CMAKE_BINARY_DIR}/mkromfs
                ${PPAP_ROOT}/tools/mkromfs/mkromfs.c
        DEPENDS ${PPAP_ROOT}/tools/mkromfs/mkromfs.c
                ${PPAP_ROOT}/src/kernel/fs/romfs_format.h
        COMMENT "Building mkromfs host tool"
    )
endfunction()

# ppap_generate_romfs(target fstab inittab
#                     [BIG_ENDIAN]
#                     USER_ELFS elf1 elf2 ...)
#
# Generates a romfs image and links it into the given cmake target.
# BIG_ENDIAN passes -b to mkromfs (for m68k).
# Requires ppap_add_mkromfs() to have been called first.
function(ppap_generate_romfs target fstab inittab)
    cmake_parse_arguments(ARG "BIG_ENDIAN" "" "USER_ELFS" ${ARGN})

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

    add_custom_command(
        OUTPUT ${_romfs_bin}
        COMMAND ${PPAP_ROOT}/cmake/stage-romfs.sh
                ${_romfs_staging} ${PPAP_ROOT}
                --user-elfs ${ARG_USER_ELFS}
                --bb-dir ${PPAP_BB_DIR}
                --bb-applets ${BB_APPLETS}
                --bb-shell-applets ${BB_SHELL_APPLETS}
                --bb-sbin-applets ${BB_SBIN_APPLETS}
                --rogue ${PPAP_ROGUE_DIR}/rogue
                --etc-dir ${PPAP_ROOT}/src/etc
                --fstab ${fstab}
                --inittab ${inittab}
        COMMAND ${CMAKE_BINARY_DIR}/mkromfs ${_mkromfs_flags}
                ${_romfs_staging} ${_romfs_bin}
        DEPENDS ${CMAKE_BINARY_DIR}/mkromfs
                ${ARG_USER_ELFS}
                ${PPAP_BB_DIR}/busybox
                ${PPAP_ROGUE_DIR}/rogue
                ${PPAP_ROOT}/src/etc/fstab
                ${PPAP_ROOT}/src/etc/inittab
                ${fstab}
                ${inittab}
                ${PPAP_ROOT}/cmake/stage-romfs.sh
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
