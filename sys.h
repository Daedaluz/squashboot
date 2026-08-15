#ifndef SQUASHBOOT_SYS_H
#define SQUASHBOOT_SYS_H

/* Syscalls nolibc doesn't expose a libc-style wrapper for. */

int fcntl(int fd, int cmd, ...);

int unlinkat(int dirfd, const char *path, int flags);

int execv(const char *path, char *const argv[]);

#endif /* SQUASHBOOT_SYS_H */
