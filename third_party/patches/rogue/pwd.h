/*
 * Minimal pwd.h stub for Rogue on PPAP
 *
 * Provides a dummy getpwuid() that returns fixed values.
 * Rogue's mdport.c uses this for md_gethomedir() and md_getshell().
 */

#ifndef PPAP_PWD_H
#define PPAP_PWD_H

struct passwd {
    char *pw_name;
    char *pw_dir;
    char *pw_shell;
};

static inline struct passwd *getpwuid(int uid)
{
    static struct passwd pw = {
        .pw_name  = "player",
        .pw_dir   = "/tmp",
        .pw_shell = "/bin/sh",
    };
    (void)uid;
    return &pw;
}

#endif /* PPAP_PWD_H */
