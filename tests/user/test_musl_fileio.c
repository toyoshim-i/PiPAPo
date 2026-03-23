/*
 * test_musl_fileio.c — musl-linked: file I/O (like cat, df)
 */
#include <stdio.h>

int main(void)
{
    /* Read a system file (like cat /etc/hostname) */
    FILE *f = fopen("/etc/hostname", "r");
    if (!f) return 1;
    char buf[64];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return 1; }
    fclose(f);
    printf("hostname: %s", buf);

    /* Read multiple files (like df reading /proc/mounts) */
    const char *files[] = {
        "/etc/hostname", "/etc/motd", "/etc/passwd", NULL
    };
    for (int i = 0; files[i]; i++) {
        f = fopen(files[i], "r");
        if (!f) continue;
        while (fgets(buf, sizeof(buf), f))
            ;
        fclose(f);
    }
    return 0;
}
