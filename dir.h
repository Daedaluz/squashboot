#ifndef SQUASHBOOT_DIR_H
#define SQUASHBOOT_DIR_H

/*
 * nolibc's own dirent.h provides DIR/opendir/fdopendir/closedir/readdir_r,
 * but no readdir(), no dirfd(), and no d_type in its struct dirent. Since
 * every nolibc header transitively defines those names already, this fills
 * the gap under an sb_-prefixed name instead of colliding with them.
 */

struct sb_dirent {
    unsigned long d_ino;
    unsigned char d_type;
    char d_name[256];
};

typedef struct sb_DIR sb_DIR;

sb_DIR *sb_opendir(const char *path);

sb_DIR *sb_fdopendir(int fd);

struct sb_dirent *sb_readdir(sb_DIR *dirp);

int sb_closedir(sb_DIR *dirp);

int sb_dirfd(sb_DIR *dirp);

#endif /* SQUASHBOOT_DIR_H */
