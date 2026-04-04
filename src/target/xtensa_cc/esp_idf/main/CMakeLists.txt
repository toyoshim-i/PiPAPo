# main component — ESP-IDF entry point
#
# Contains app_main() which calls kmain().
# All PPAP kernel sources are in the ppap_kernel component.

set(PPAP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)

idf_component_register(
    SRCS
        "${PPAP_ROOT}/src/target/xtensa_cc/kernel/core/target_xtensa_cc.c"
    INCLUDE_DIRS
        "${PPAP_ROOT}/src"
        "${PPAP_ROOT}/src/arch/xtensa"
        "${PPAP_ROOT}/src/target/xtensa_cc"
    REQUIRES ppap_kernel
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    PPAP_KERNEL=1
    PPAP_SYS_HZ=240000000u
    PAGE_POOL_BASE=0x3FC90000u
    PAGE_COUNT_MAX=24u
)
