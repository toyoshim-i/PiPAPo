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
    PROC_KSTACK_SIZE=2048u
    PAGE_POOL_BASE=0x3FC90000u
    PAGE_COUNT_MAX=48u
)

# target_xtensa_cc.c's target_init_path() switches between /sbin/init
# and /bin/runtests based on these defines (forwarded by build.sh's
# --test path through idf.py -D PPAP_TESTS=ON).  When tests are on we
# also pull tests/kernel/ktest.c into the build and add its header
# directory, mirroring what cmake/{arm_m,m68k,riscv,xtensa}.cmake do
# for non-IDF targets.
if(PPAP_TESTS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PPAP_TESTS=1)
    target_sources(${COMPONENT_LIB} PRIVATE
        "${PPAP_ROOT}/tests/kernel/ktest.c")
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${PPAP_ROOT}/tests/kernel")
endif()
if(PPAP_TESTS_EXTENDED)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PPAP_TESTS_EXTENDED=1)
endif()
