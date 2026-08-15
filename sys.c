#include "nolibc.h"
#include "sys.h"

/*
 * int fcntl(int fd, int cmd, ...);
 *
 * Only the single-argument (long) forms are needed here (F_SETFD/FD_CLOEXEC).
 */
int fcntl(int fd, int cmd, ...) {
    va_list args;
    long arg;
    long nr_fcntl;

    va_start(args, cmd);
    arg = va_arg(args, long);
    va_end(args);

#ifdef __NR_fcntl64
    nr_fcntl = __NR_fcntl64;
#else
    nr_fcntl = __NR_fcntl;
#endif

    return __sysret(my_syscall3(nr_fcntl, fd, cmd, arg));
}

/*
 * int unlinkat(int dirfd, const char *path, int flags);
 */
int unlinkat(int dirfd, const char *path, int flags) {
    return __sysret(my_syscall3(__NR_unlinkat, dirfd, path, flags));
}

/*
 * int execv(const char *path, char *const argv[]);
 */
int execv(const char *path, char *const argv[]) {
    return execve(path, argv, environ);
}
