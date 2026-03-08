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
    /* Rainbow "Hello" + colored "user space" */
    static const char banner[] =
        "\033[1;31mH\033[32me\033[33ml\033[34ml\033[35mo"
        "\033[0m from "
        "\033[1;36muser space\033[0m!\n";
    write(1, banner, sizeof(banner) - 1);

    /* Normal 8 colors */
    static const char normal[] =
        "\033[30;47m blk \033[0m"
        "\033[31m red \033[0m"
        "\033[32m grn \033[0m"
        "\033[33m yel \033[0m"
        "\033[34m blu \033[0m"
        "\033[35m mag \033[0m"
        "\033[36m cyn \033[0m"
        "\033[37;40m wht \033[0m";
    write(1, normal, sizeof(normal) - 1);

    /* Bright 8 colors (bold) */
    static const char bright[] =
        "\033[1;30;47m blk \033[0m"
        "\033[1;31m red \033[0m"
        "\033[1;32m grn \033[0m"
        "\033[1;33m yel \033[0m"
        "\033[1;34m blu \033[0m"
        "\033[1;35m mag \033[0m"
        "\033[1;36m cyn \033[0m"
        "\033[1;37;40m wht \033[0m";
    write(1, bright, sizeof(bright) - 1);

    /* Background colors */
    static const char bg[] =
        "\033[40;37m blk \033[0m"
        "\033[41m red \033[0m"
        "\033[42m grn \033[0m"
        "\033[43m yel \033[0m"
        "\033[44m blu \033[0m"
        "\033[45m mag \033[0m"
        "\033[46m cyn \033[0m"
        "\033[47;30m wht \033[0m\n";
    write(1, bg, sizeof(bg) - 1);

    return 0;
}
