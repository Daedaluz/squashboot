#include "nolibc.h"
#include "dir.h"

#define SB_DIR_BUFSZ 4096

struct sb_DIR {
    int fd;
    int buf_pos;
    int buf_len;
    struct sb_dirent entry;
    char buf[SB_DIR_BUFSZ] __nolibc_aligned_as(struct linux_dirent64);
};

sb_DIR *sb_fdopendir(int fd) {
    if (fd < 0) {
        SET_ERRNO(EBADF);
        return NULL;
    }
    sb_DIR *d = malloc(sizeof(*d));
    if (!d) {
        SET_ERRNO(ENOMEM);
        return NULL;
    }
    d->fd = fd;
    d->buf_pos = 0;
    d->buf_len = 0;
    return d;
}

sb_DIR *sb_opendir(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    sb_DIR *d = sb_fdopendir(fd);
    if (!d)
        close(fd);
    return d;
}

int sb_dirfd(sb_DIR *dirp) {
    return dirp->fd;
}

int sb_closedir(sb_DIR *dirp) {
    int fd = dirp->fd;
    free(dirp);
    return close(fd);
}

struct sb_dirent *sb_readdir(sb_DIR *dirp) {
    if (dirp->buf_pos >= dirp->buf_len) {
        int n = getdents64(dirp->fd, (struct linux_dirent64 *) dirp->buf, sizeof(dirp->buf));
        if (n <= 0)
            return NULL;
        dirp->buf_len = n;
        dirp->buf_pos = 0;
    }

    struct linux_dirent64 *ld = (struct linux_dirent64 *) (dirp->buf + dirp->buf_pos);
    dirp->buf_pos += ld->d_reclen;

    dirp->entry.d_ino = ld->d_ino;
    dirp->entry.d_type = ld->d_type;
    strlcpy(dirp->entry.d_name, ld->d_name, sizeof(dirp->entry.d_name));
    return &dirp->entry;
}
