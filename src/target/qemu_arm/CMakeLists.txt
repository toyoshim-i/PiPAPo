add_executable(ppap_qemu_arm
    ${KERNEL_COMMON_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/target_qemu_arm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/uart_qemu.c
    ${CMAKE_SOURCE_DIR}/src/kernel/blkdev/ramblk.c
)

target_include_directories(ppap_qemu_arm PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/kernel
)
target_compile_definitions(ppap_qemu_arm PRIVATE PPAP_QEMU=1)

target_link_options(ppap_qemu_arm PRIVATE
    -T ${CMAKE_SOURCE_DIR}/ldscripts/qemu.ld
    -nostartfiles
    -Wl,--gc-sections
)
