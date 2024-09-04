#include <stdio.h>
#include <sys/mount.h>
#include <dirent.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/loop.h>
#include <errno.h>
#include <stdlib.h>
#include <stdarg.h>

int stdout_fd;
int stderr_fd;

const int squashfs_magic = 0x73717368;

static int mkdirp(const char *path);

static char *find_squasfs();

static int setup_loop(const char *file);

static int recursiveRemove(int fd);

static void mount_pseudofs(const char *src, const char *target, const char *fs);

static void move_mount(const char *src, const char *dst);

static void mountfs(const char *src, const char *dst, const char *fs);

void klog(const char *fmt, ...);

void assert(const char *prefix, int b, ...);

int main(int argc, char *argv[]) {
    // First things first, we need a device tree
    mount_pseudofs("devtmpfs", "/dev/", "devtmpfs");

    // Setup logging
    int fd = open("/dev/kmsg", O_WRONLY | O_NOCTTY | O_CLOEXEC);
    assert("open /dev/kmsg", fd == -1);

    // save stdout and stderr
    stdout_fd = dup(1);
    stderr_fd = dup(2);
    // use kmsg as stdout and stderr
    dup2(fd, 2);
    dup2(fd, 1);
    close(fd);

    // create /dev/pts and /dev/shm
    klog("Mounting pseudo filesystems");
    assert("mkdir /dev/pts", mkdirp("/dev/pts") == -1);
    assert("mkdir /dev/shm", mkdirp("/dev/shm") == -1);

    // mount pseudo filesystems
    mount_pseudofs("pts", "/dev/pts", "devpts");
    mount_pseudofs("tmpfs", "/dev/shm", "tmpfs");
    mount_pseudofs("tmpfs", "/tmp", "tmpfs");
    mount_pseudofs("tmpfs", "/run", "tmpfs");
    mount_pseudofs("proc", "/proc", "proc");
    mount_pseudofs("sysfs", "/sys", "sysfs");
    mount_pseudofs("cgroup2", "/sys/fs/cgroup", "cgroup2");
    mount_pseudofs("configfs", "/sys/kernel/config", "configfs");

    // find the squashfs filesystem and mount it
    char *squashfs = find_squasfs();
    assert("No squashfs filesystem found", squashfs == NULL);

    klog("Found squashfs at %s", squashfs);
    klog("Attaching %s to a loop device", squashfs);

    // setup a loop device and attach the squashfs filesystem to it
    int dev = setup_loop(squashfs);
    free(squashfs);

    char loopdev[15];
    snprintf(loopdev, 15, "/dev/loop%d", dev);

    klog("Mounting %s to /newroot", loopdev);
    mountfs(loopdev, "/newroot", "squashfs");

    // move the pseudo filesystems to the new root
    klog("Moving pseudo filesystems to new root");
    move_mount("/dev", "/newroot/dev");
    move_mount("/proc", "/newroot/proc");
    move_mount("/sys", "/newroot/sys");
    move_mount("/tmp", "/newroot/tmp");
    move_mount("/run", "/newroot/run");

    // chroot to the new root while retaining a reference to the old root
    // so that we can delete the remnants of the old root and free up RAM
    klog("Entering new root");
    assert("chdir to /newroot", chdir("/newroot") != 0);
    int parent = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert("open /", parent == -1);
    move_mount("/newroot", "/");
    assert("chroot to \".\"", chroot(".") != 0);
    assert("chdir \"/\"", chdir("/") != 0);

    klog("Zapping old root");
    recursiveRemove(parent);

    // restore stdout and stderr
    dup2(stdout_fd, 1);
    dup2(stderr_fd, 2);

    // shellinit
    if (access("/init.sh", F_OK) == 0 &&
        access("/sbin/getty", F_OK) == 0) {
        char *args[] = {
                "/sbin/getty",
                "-l",
                "/init.sh",
                "-n",
                "115200",
                "console",
                NULL
        };
        if (execv("/sbin/getty", args) == -1) {
            klog("exec /init.sh: %s", strerror(errno));
        }
    }

    // exec next init
    char *argv2[] = {"/init", NULL};
    if (execv("/init", argv2) == -1) {
        klog("execv /init: %s", strerror(errno));
    }
    argv2[0] = "/sbin/init";
    if (execv("/sbin/init", argv2) == -1) {
        klog("execv /sbin/init: %s", strerror(errno));
    }
    klog("No init found");
    return 1;
}

// find_squashfs finds the first squashfs filesystem in / and returns the path to it
char *find_squasfs() {
    struct dirent *de;
    DIR *dr = opendir("/");
    assert("opendir \"/\"", dr == NULL);
    while ((de = readdir(dr)) != NULL) {
        if (de->d_type == DT_REG) {
            klog("Testing %s for squashfs magic", de->d_name);
            FILE *f = fopen(de->d_name, "r");
            assert("fopen %s", f == NULL, de->d_name);
            int magic;
            fread(&magic, sizeof(magic), 1, f);
            fclose(f);
            if (magic == squashfs_magic) {
                return strdup(de->d_name);
            }
        }
    }
    return NULL;
}

// create dir if it does not exist
int mkdirp(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return 0;
    }
    return mkdir(path, 0755);
}

// setup a loop device and attach a file to it
int setup_loop(const char *file) {
    int fd = open("/dev/loop-control", O_RDWR);
    assert("open loop-control", fd == -1);
    int loop = ioctl(fd, LOOP_CTL_GET_FREE);
    assert("get free loop device", loop == -1);
    close(fd);

    char loop_device[20];
    snprintf(loop_device, 20, "/dev/loop%d", loop);

    fd = open(loop_device, O_RDWR);
    assert("open loop device", fd == -1);
    int filefd = open(file, O_RDWR);
    assert("open target loop file", filefd == -1);
    assert("set loop fd", ioctl(fd, LOOP_SET_FD, filefd) == -1);
    close(filefd);
    close(fd);
    return loop;
}

/* remove all files/directories below dirName -- don't cross mountpoints */
static int recursiveRemove(int fd) {
    struct stat rb;
    DIR *dir;
    int rc = -1;
    int dfd;

    if (!(dir = fdopendir(fd))) {
        klog("failed to open directory");
        goto done;
    }

    /* fdopendir() precludes us from continuing to use the input fd */
    dfd = dirfd(dir);
    if (fstat(dfd, &rb)) {
        klog("stat failed");
        goto done;
    }

    while (1) {
        struct dirent *d;
        int isdir = 0;

        errno = 0;
        if (!(d = readdir(dir))) {
            if (errno) {
                klog("failed to read directory");
                goto done;
            }
            break;    /* end of directory */
        }

        if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
            continue;
#ifdef _DIRENT_HAVE_D_TYPE
        if (d->d_type == DT_DIR || d->d_type == DT_UNKNOWN)
#endif
        {
            struct stat sb;

            if (fstatat(dfd, d->d_name, &sb, AT_SYMLINK_NOFOLLOW)) {
                klog("stat of %s failed", d->d_name);
                continue;
            }

            /* skip if device is not the same */
            if (sb.st_dev != rb.st_dev)
                continue;

            /* remove subdirectories */
            if (S_ISDIR(sb.st_mode)) {
                int cfd;

                cfd = openat(dfd, d->d_name, O_RDONLY);
                if (cfd >= 0)
                    recursiveRemove(cfd);    /* it closes cfd too */
                isdir = 1;
            }
        }

        if (unlinkat(dfd, d->d_name, isdir ? AT_REMOVEDIR : 0))
            klog("failed to unlink %s", d->d_name);
    }

    rc = 0;    /* success */
    done:
    if (dir)
        closedir(dir);
    else
        close(fd);
    return rc;
}


static void mount_pseudofs(const char *src, const char *target, const char *fs) {
    char msg[25];
    snprintf(msg, 25, "mount %s", src);
    assert(msg, mount(src, target, fs, 0, NULL) != 0);
}

static void move_mount(const char *src, const char *dst) {
    char msg[100];
    snprintf(msg, 100, "move mount %s to %s", src, dst);
    assert(msg, mount(src, dst, NULL, MS_MOVE, NULL) != 0);
}

static void mountfs(const char *src, const char *dst, const char *fs) {
    char msg[100];
    snprintf(msg, 100, "mounting %s on %s", src, dst);
    assert(msg, mount(src, dst, fs, MS_RDONLY | MS_I_VERSION, NULL) != 0);
}

void klog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

void assert(const char *prefix, int b, ...) {
    if (b) {
        va_list ap;
        va_start(ap, b);
        vprintf(prefix, ap);
        va_end(ap);
        printf(": %s\n", strerror(errno));
        fflush(stdout);
        exit(-1);
    }
}
