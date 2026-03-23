/*
 * test_musl_dir.c — musl-linked: directory listing (like ls)
 */
#include <dirent.h>
#include <stdio.h>

int main(void)
{
    DIR *d = opendir("/bin");
    if (!d) return 1;
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL)
        count++;
    closedir(d);
    printf("readdir: %d entries in /bin\n", count);
    return count > 0 ? 0 : 1;
}
