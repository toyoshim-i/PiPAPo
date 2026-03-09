# busybox_applets.cmake --- Single source of truth for busybox applet lists
#
# Included by both root CMakeLists.txt (ARM) and qemu_m68k/CMakeLists.txt.
# Also used by cmake/stage_romfs.sh via exported environment variables.

# Applets that link to full busybox binary (transient commands)
set(BB_APPLETS
    cat chmod cp df echo grep head kill ln ls mkdir mv
    printf ps rm rmdir sed sleep sort tail top uname vi wc
)

# Shell applets --- link to busybox.sh (dedicated shell binary)
set(BB_SHELL_APPLETS sh hush)

# Sbin applets --- link to full busybox via ../bin/busybox
set(BB_SBIN_APPLETS mount umount)
