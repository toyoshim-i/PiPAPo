/*
 * dirent.h --- Directory entry type constants and struct dirent
 *
 * Shared between kernel and user space.
 * PPAP_NAME_MAX must match VFS_NAME_MAX in config.h (63).
 */

#ifndef PPAP_COMMON_DIRENT_H
#define PPAP_COMMON_DIRENT_H

#include <stdint.h>

/* Maximum filename component length (excluding NUL) */
#define PPAP_NAME_MAX  63

/* Directory entry type constants (Linux d_type values) */
#define DT_CHR   2   /* character dev  */
#define DT_DIR   4   /* directory      */
#define DT_REG   8   /* regular file   */
#define DT_LNK  10   /* symbolic link  */

/* PPAP-native directory entry structure */
struct dirent {
    uint32_t d_ino;                      /* inode number            */
    uint8_t  d_type;                     /* DT_REG / DT_DIR / ...  */
    char     d_name[PPAP_NAME_MAX + 1];  /* NUL-terminated filename */
};

#endif /* PPAP_COMMON_DIRENT_H */
