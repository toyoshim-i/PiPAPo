/*
 * hello.c — First PPAP user-space program
 *
 * Prints a message via sys_write and exits.  Uses raw SVC syscall wrappers
 * (no libc).  This binary is packaged into romfs at /bin/hello.
 *
 * Not yet executed by the kernel — Phase 3 Steps 2-3 implement the ELF
 * loader and exec().  Step 1 only verifies the build + romfs pipeline.
 */

#include "syscall.h"

int main(void)
{
    static const char msg[] = "Hello from user space!\n";
    write(1, msg, sizeof(msg) - 1);
    return 0;
}
