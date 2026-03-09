# user_programs.cmake --- Shared lists of user-space programs and tests
#
# Included by both root CMakeLists.txt (ARM) and qemu_m68k/CMakeLists.txt.
# Each arch uses its own build mechanism (Makefile vs CMake custom commands)
# but shares these program/test name lists.

# Application programs (sources in src/user/)
set(USER_APPS hello getty init ttyctl)

# Install destinations: name -> directory
# init   -> sbin
# ttyctl -> usr/bin
# others -> bin

# Test programs (sources in tests/user/)
set(USER_TESTS
    test_exec test_vfork test_fault test_pipe test_brk
    test_fd test_signal test_poll test_sleep_intr test_orphan
    test_id test_fs test_rw test_time test_iov test_stat test_tmpfs
    runtests
)
