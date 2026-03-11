# Test CMake configuration
message("Testing PPAP_ARCH detection...")

# Simulate ARM
set(CMAKE_SYSTEM_PROCESSOR "")
include(cmake/user.cmake)
message("ARM - PPAP_ARCH: ${PPAP_ARCH}")
message("ARM - PPAP_ENABLE_HUMAN68K: ${PPAP_ENABLE_HUMAN68K}")
message("ARM - PPAP_ENABLE_CPM: ${PPAP_ENABLE_CPM}")
message("ARM - PPAP_ENABLE_ECPU_M68K: ${PPAP_ENABLE_ECPU_M68K}")
message("ARM - PPAP_ENABLE_ECPU_Z80: ${PPAP_ENABLE_ECPU_Z80}")

# Simulate m68k
unset(PPAP_ARCH)
unset(PPAP_ENABLE_HUMAN68K)
unset(PPAP_ENABLE_CPM)
unset(PPAP_ENABLE_ECPU_M68K)
unset(PPAP_ENABLE_ECPU_Z80)
set(CMAKE_SYSTEM_PROCESSOR "m68k")
include(cmake/user.cmake)
message("m68k - PPAP_ARCH: ${PPAP_ARCH}")
message("m68k - PPAP_ENABLE_HUMAN68K: ${PPAP_ENABLE_HUMAN68K}")
message("m68k - PPAP_ENABLE_CPM: ${PPAP_ENABLE_CPM}")
message("m68k - PPAP_ENABLE_ECPU_M68K: ${PPAP_ENABLE_ECPU_M68K}")
message("m68k - PPAP_ENABLE_ECPU_Z80: ${PPAP_ENABLE_ECPU_Z80}")
