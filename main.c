#include "nolibc.h"
#include "sys.h"
#include "dir.h"
#include <linux/loop.h>

/* nolibc's types.h defines the S_IF.., S_IR.., S_IW.., S_IX.. mode bits but
 * not these three, which log_file_stat() needs. */
#ifndef S_ISUID
#define S_ISUID 0004000
#endif
#ifndef S_ISGID
#define S_ISGID 0002000
#endif
#ifndef S_ISVTX
#define S_ISVTX 0001000
#endif

int stdout_fd;
int stderr_fd;

const int squashfs_magic = 0x73717368;

static int mkdirp(const char *path);

static void log_file_stat(const char *path);

static char *find_squasfs();

static int setup_loop(const char *file);

static int recursiveRemove(int fd);

static void mount_pseudofs(const char *src, const char *target, const char *fs);

static void relocate_mount(const char *src, const char *dst);

static void mountfs(const char *src, const char *dst, const char *fs);

static void klog(const char *fmt, ...);

static void assert(const char *prefix, int b, ...);

static void print_filesystems(void);

static void check_filesystems(void);

static int set_printk_ratelimit(int n);

static void print_squashfs_info(const char *dev);

static void check_squashfs_fits(const char *dev);

static void xwrite(int fd, const void *buf, size_t len);

int main(int argc, char *argv[]) {
    // First things first, we need a device tree
    mkdirp("/dev");
    mount_pseudofs("devtmpfs", "/dev/", "devtmpfs");

    // create standard directories
    mkdirp("/run");
    mkdirp("/tmp");
    mkdirp("/proc");
    mkdirp("/sys");

    // Mount proc early so we can disable printk ratelimiting before logging starts
    mount_pseudofs("proc", "/proc", "proc");
    set_printk_ratelimit(0);

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

    klog("squashboot %s (%s)", GIT_VERSION, GIT_COMMIT);

    // create /dev/pts and /dev/shm
    klog("Mounting pseudo filesystems");
    assert("mkdir /dev/pts", mkdirp("/dev/pts") == -1);
    assert("mkdir /dev/shm", mkdirp("/dev/shm") == -1);

    // mount pseudo filesystems
    print_filesystems();
    check_filesystems();
    mount_pseudofs("tmpfs", "/tmp", "tmpfs");
    mount_pseudofs("tmpfs", "/run", "tmpfs");
    mount_pseudofs("sysfs", "/sys", "sysfs");
    mount_pseudofs("pts", "/dev/pts", "devpts");
    mount_pseudofs("tmpfs", "/dev/shm", "tmpfs");
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
    print_squashfs_info(loopdev);
    check_squashfs_fits(loopdev);
    klog("Mounting %s to /newroot", loopdev);
    mountfs(loopdev, "/newroot", "squashfs");

    // move the pseudo filesystems to the new root
    klog("Moving pseudo filesystems to new root");
    relocate_mount("/dev", "/newroot/dev");
    relocate_mount("/proc", "/newroot/proc");
    relocate_mount("/sys", "/newroot/sys");
    relocate_mount("/tmp", "/newroot/tmp");
    relocate_mount("/run", "/newroot/run");

    // chroot to the new root while retaining a reference to the old root
    // so that we can delete the remnants of the old root and free up RAM
    klog("Entering new root");
    assert("chdir to /newroot", chdir("/newroot") != 0);
    int parent = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert("open /", parent == -1);
    relocate_mount("/newroot", "/");
    assert("chroot to \".\"", chroot(".") != 0);
    assert("chdir \"/\"", chdir("/") != 0);

    klog("Zapping old root");
    recursiveRemove(parent);

    // restore stdout and stderr
    dup2(1, 10);
    fcntl(10, F_SETFD, FD_CLOEXEC);
    dup2(stdout_fd, 1);
    dup2(stderr_fd, 2);

    // shellinit
    if (access("/init.sh", F_OK) == 0) {
        static const char *gettys[] = {
            "/sbin/getty",
            "/usr/sbin/getty",
            "/bin/getty",
            "/sbin/agetty",
            "/bin/agetty",
            "/usr/sbin/agetty",
            NULL,
        };
        for (int i = 0; gettys[i]; i++) {
            if (access(gettys[i], F_OK) != 0)
                continue;
            char *args[] = {
                (char *)gettys[i], "-l", "/init.sh", "-n", "115200", "console", NULL
            };
            klog("Launching %s with /init.sh", gettys[i]);
            execv(gettys[i], args);
            klog("exec %s: %s", gettys[i], strerror(errno));
            log_file_stat(gettys[i]);
        }
	char *args[] = {
		(char*)"/init.sh", NULL
	};
	execv("/init.sh", args);
    }

    // exec next init — try common paths
    static const char *inits[] = {
        "/init",
        "/sbin/init",
        "/bin/init",
        "/usr/sbin/init",
        "/lib/systemd/systemd",
        "/usr/lib/systemd/systemd",
        NULL,
    };
    for (int i = 0; inits[i]; i++) {
        char *argv2[] = {(char *)inits[i], NULL};
        execv(inits[i], argv2);
        klog("execv %s: %s", inits[i], strerror(errno));
        log_file_stat(inits[i]);
    }

    // nothing worked — list / to help diagnose
    klog("No init found. Contents of /:");
    sb_DIR *dr = sb_opendir("/");
    if (dr) {
        struct sb_dirent *de;
        while ((de = sb_readdir(dr)) != NULL)
            klog("  %s", de->d_name);
        sb_closedir(dr);
    }
    return 1;
}

// find_squashfs finds the first squashfs filesystem in / and returns the path to it
char *find_squasfs() {
    struct sb_dirent *de;
    sb_DIR *dr = sb_opendir("/");
    klog("Finding squashfs");
    assert("opendir \"/\"", dr == NULL);
    while ((de = sb_readdir(dr)) != NULL) {
        if (de->d_type == DT_REG) {
            klog("Testing %s for squashfs magic", de->d_name);
            int mfd = open(de->d_name, O_RDONLY);
            assert("open %s", mfd == -1, de->d_name);
            int magic = 0;
            if (read(mfd, &magic, sizeof(magic)) != (ssize_t) sizeof(magic)) {
                close(mfd);
                continue;
            }
            close(mfd);
            if (magic == squashfs_magic) {
                return strdup(de->d_name);
            }
        }
    }
    return NULL;
}

// create dir if it does not exist
static void log_file_stat(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0)
        return;
    char mode[11];
    mode[0]  = S_ISDIR(st.st_mode) ? 'd' : S_ISLNK(st.st_mode) ? 'l' : '-';
    mode[1]  = (st.st_mode & S_IRUSR) ? 'r' : '-';
    mode[2]  = (st.st_mode & S_IWUSR) ? 'w' : '-';
    mode[3]  = (st.st_mode & S_IXUSR) ? ((st.st_mode & S_ISUID) ? 's' : 'x') : ((st.st_mode & S_ISUID) ? 'S' : '-');
    mode[4]  = (st.st_mode & S_IRGRP) ? 'r' : '-';
    mode[5]  = (st.st_mode & S_IWGRP) ? 'w' : '-';
    mode[6]  = (st.st_mode & S_IXGRP) ? ((st.st_mode & S_ISGID) ? 's' : 'x') : ((st.st_mode & S_ISGID) ? 'S' : '-');
    mode[7]  = (st.st_mode & S_IROTH) ? 'r' : '-';
    mode[8]  = (st.st_mode & S_IWOTH) ? 'w' : '-';
    mode[9]  = (st.st_mode & S_IXOTH) ? ((st.st_mode & S_ISVTX) ? 't' : 'x') : ((st.st_mode & S_ISVTX) ? 'T' : '-');
    mode[10] = '\0';
    klog("  %s uid=%-5u gid=%-5u size=%llu", mode,
         st.st_uid, st.st_gid, (unsigned long long)st.st_size);

    // Read the file header to find the interpreter
    unsigned char buf[4096];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n < 4) return;

    // Shebang
    if (buf[0] == '#' && buf[1] == '!') {
        char *p = (char *)buf + 2;
        char *end = (char *)buf + n;
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        char *interp_end = p;
        while (interp_end < end && *interp_end != ' ' && *interp_end != '\n' && *interp_end != '\r')
            interp_end++;
        *interp_end = '\0';
        struct stat ist;
        if (stat(p, &ist) != 0)
            klog("  shebang interpreter not found: %s", p);
        else
            klog("  shebang interpreter: %s (found)", p);
        return;
    }

    // ELF
    if (buf[0] != 0x7f || buf[1] != 'E' || buf[2] != 'L' || buf[3] != 'F') return;

    const char *interp = NULL;
    if (buf[4] == ELFCLASS64 && n >= (ssize_t)sizeof(Elf64_Ehdr)) {
        Elf64_Ehdr *eh = (Elf64_Ehdr *)buf;
        for (int i = 0; i < eh->e_phnum; i++) {
            off_t off = eh->e_phoff + (off_t)i * eh->e_phentsize;
            if (off + (off_t)sizeof(Elf64_Phdr) > n) break;
            Elf64_Phdr *ph = (Elf64_Phdr *)(buf + off);
            if (ph->p_type == PT_INTERP && ph->p_offset + ph->p_filesz <= (Elf64_Off)n) {
                interp = (char *)buf + ph->p_offset;
                break;
            }
        }
    } else if (buf[4] == ELFCLASS32 && n >= (ssize_t)sizeof(Elf32_Ehdr)) {
        Elf32_Ehdr *eh = (Elf32_Ehdr *)buf;
        for (int i = 0; i < eh->e_phnum; i++) {
            off_t off = eh->e_phoff + (off_t)i * eh->e_phentsize;
            if (off + (off_t)sizeof(Elf32_Phdr) > n) break;
            Elf32_Phdr *ph = (Elf32_Phdr *)(buf + off);
            if (ph->p_type == PT_INTERP && ph->p_offset + ph->p_filesz <= (Elf32_Off)n) {
                interp = (char *)buf + ph->p_offset;
                break;
            }
        }
    }

    if (!interp) return;
    struct stat ist;
    if (stat(interp, &ist) != 0)
        klog("  ELF interpreter not found: %s", interp);
    else
        klog("  ELF interpreter: %s (found)", interp);
}

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
    int loop = ioctl(fd, LOOP_CTL_GET_FREE, 0);
    assert("get free loop device", loop == -1);
    close(fd);

    char loop_device[20];
    snprintf(loop_device, 20, "/dev/loop%d", loop);

    fd = open(loop_device, O_RDWR);
    assert("open loop device", fd == -1);
    int filefd = open(file, O_RDONLY);
    assert("open target loop file", filefd == -1);
    assert("set loop fd", ioctl(fd, LOOP_SET_FD, filefd) == -1);
    close(filefd);

    struct loop_info64 info = {0};
    info.lo_flags = LO_FLAGS_READ_ONLY | LO_FLAGS_PARTSCAN;
    assert("set loop status", ioctl(fd, LOOP_SET_STATUS64, &info) == -1);

    close(fd);
    return loop;
}

/* remove all files/directories below dirName -- don't cross mountpoints */
static int recursiveRemove(int fd) {
    struct stat rb;
    sb_DIR *dir;
    int rc = -1;
    int dfd;

    if (!(dir = sb_fdopendir(fd))) {
        klog("failed to open directory");
        goto done;
    }

    /* fdopendir() precludes us from continuing to use the input fd */
    dfd = sb_dirfd(dir);
    if (fstat(dfd, &rb)) {
        klog("stat failed");
        goto done;
    }

    while (1) {
        struct sb_dirent *d;
        int isdir = 0;

        errno = 0;
        if (!(d = sb_readdir(dir))) {
            if (errno) {
                klog("failed to read directory");
                goto done;
            }
            break;    /* end of directory */
        }

        if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
            continue;
        if (d->d_type == DT_DIR || d->d_type == DT_UNKNOWN)
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
        sb_closedir(dir);
    else
        close(fd);
    return rc;
}


static void mount_pseudofs(const char *src, const char *target, const char *fs) {
    char msg[150];
    snprintf(msg, 150, "mount src(%s) to target(%s) with fs(%s)", src, target, fs);
    assert(msg, mount(src, target, fs, 0, NULL) != 0);
}

static void relocate_mount(const char *src, const char *dst) {
    char msg[100];
    snprintf(msg, 100, "move mount %s to %s", src, dst);
    assert(msg, mount(src, dst, NULL, MS_MOVE, NULL) != 0);
}

static void xwrite(int fd, const void *buf, size_t len) {
    ssize_t n = write(fd, buf, len);
    (void)n;
}

static void mountfs(const char *src, const char *dst, const char *fs) {
    char msg[100];
    snprintf(msg, 100, "mounting %s on %s", src, dst);
    assert(msg, mount(src, dst, fs, MS_RDONLY, NULL) != 0);
}

static void klog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static void assert(const char *prefix, int b, ...) {
    if (b) {
        va_list ap;
        char msg[256];
        va_start(ap, b);
        vsnprintf(msg, sizeof(msg), prefix, ap);
        va_end(ap);
        printf("%s: %s\n", msg, strerror(errno));
        fflush(stdout);
	klog("Exit");
        exit(-1);
    }
}

static void check_filesystems(void) {
    const char *required[] = {
        "squashfs", "tmpfs", "devtmpfs", "proc", "sysfs", "devpts", NULL
    };

    char buf[4096];
    int fd = open("/proc/filesystems", O_RDONLY);
    assert("open /proc/filesystems", fd == -1);
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    assert("read /proc/filesystems", n <= 0);
    buf[n] = '\0';

    char msg[64];
    for (int i = 0; required[i] != NULL; i++) {
        int found = 0;
        char *p = buf;
        while (p && *p) {
            char *nl = strchr(p, '\n');
            if (nl) *nl = 0;
            char *tab = strchr(p, '\t');
            if (tab && strcmp(tab + 1, required[i]) == 0) {
                found = 1;
                if (nl) *nl = '\n';
                break;
            }
            if (nl) *nl = '\n';
            p = nl ? nl + 1 : NULL;
        }
        snprintf(msg, sizeof(msg), "filesystem '%s' available", required[i]);
        assert(msg, !found);
    }
}

static void print_filesystems(void) {
    char buf[4096];
    int fd = open("/proc/filesystems", O_RDONLY);
    if (fd < 0) {
        xwrite(1, "Could not read /proc/filesystems\n", 33);
        return;
    }
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return;
    buf[n] = '\0';

    xwrite(1, "Available filesystems:\n", 23);

    char line[80];
    char *p = buf;
    char *nl;
    while ((nl = strchr(p, '\n')) != NULL) {
        size_t len = nl - p;
        if (len > sizeof(line) - 3)
            len = sizeof(line) - 3;
        line[0] = ' ';
        line[1] = ' ';
        memcpy(line + 2, p, len);
        line[2 + len] = '\n';
        xwrite(1, line, 3 + len);
        p = nl + 1;
    }
    if (*p) {
        size_t len = strlen(p);
        if (len > sizeof(line) - 3)
            len = sizeof(line) - 3;
        line[0] = ' ';
        line[1] = ' ';
        memcpy(line + 2, p, len);
        line[2 + len] = '\n';
        xwrite(1, line, 3 + len);
    }
}

static void check_squashfs_fits(const char *dev) {
    struct {
        __u32 s_magic;
        __u32 inodes;
        __u32 mkfs_time;
        __u32 block_size;
        __u32 fragments;
        __u16 compression;
        __u16 block_log;
        __u16 flags;
        __u16 no_ids;
        __u16 s_major;
        __u16 s_minor;
        __u64 root_inode;
        __u64 bytes_used;
    } __attribute__((packed)) sb;

    int fd = open(dev, O_RDONLY);
    assert("check_squashfs_fits: open loop device", fd == -1);
    ssize_t n = read(fd, &sb, sizeof(sb));
    close(fd);
    assert("check_squashfs_fits: read superblock", n != (ssize_t)sizeof(sb));

    char buf[4096];
    fd = open("/proc/meminfo", O_RDONLY);
    assert("check_squashfs_fits: open /proc/meminfo", fd == -1);
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    assert("check_squashfs_fits: read /proc/meminfo", n <= 0);
    buf[n] = '\0';

    unsigned long long avail_kb = 0;
    char *p = strstr(buf, "MemAvailable:");
    assert("check_squashfs_fits: MemAvailable not found", p == NULL);
    p += 13;
    while (*p == ' ') p++;
    while (*p >= '0' && *p <= '9')
        avail_kb = avail_kb * 10 + (*p++ - '0');

    unsigned long long avail_bytes = avail_kb * 1024;
    /* nolibc's printf has no float support; do the MiB.1 rounding by hand. */
    unsigned long long need_mib_x10 = (sb.bytes_used * 10) / (1024 * 1024);
    unsigned long long avail_mib_x10 = (avail_bytes * 10) / (1024 * 1024);
    klog("squashfs needs %llu.%llu MiB, %llu.%llu MiB available",
         need_mib_x10 / 10, need_mib_x10 % 10,
         avail_mib_x10 / 10, avail_mib_x10 % 10);
    assert("not enough memory to mount squashfs", avail_bytes < sb.bytes_used);
}

static void print_squashfs_info(const char *dev) {
    struct {
        __u32 s_magic;
        __u32 inodes;
        __u32 mkfs_time;
        __u32 block_size;
        __u32 fragments;
        __u16 compression;
        __u16 block_log;
        __u16 flags;
        __u16 no_ids;
        __u16 s_major;
        __u16 s_minor;
        __u64 root_inode;
        __u64 bytes_used;
    } __attribute__((packed)) sb;

    static const char *comp[] = {
        "unknown", "gzip", "lzma", "lzo", "xz", "lz4", "zstd"
    };

    int fd = open(dev, O_RDONLY);
    if (fd < 0) {
        klog("squashfs info: cannot open %s", dev);
        return;
    }
    ssize_t n = read(fd, &sb, sizeof(sb));
    close(fd);
    if (n != (ssize_t)sizeof(sb)) {
        klog("squashfs info: short read");
        return;
    }

    const char *compression = sb.compression < 7 ? comp[sb.compression] : "unknown";
    klog("squashfs %u.%u  inodes: %u  size: %llu bytes  block: %u  compression: %s  fragments: %u",
         sb.s_major, sb.s_minor,
         sb.inodes,
         (unsigned long long)sb.bytes_used,
         sb.block_size,
         compression,
         sb.fragments);
}

static int set_printk_ratelimit(int n) {
    char buf[32];
    int fd;
    int len;
    ssize_t written;

    // Disable ratelimiting for userspace /dev/kmsg writes
    fd = open("/proc/sys/kernel/printk_devkmsg", O_WRONLY);
    if (fd >= 0) {
        xwrite(fd, "on\n", 3);
        close(fd);
    }

    // Set the kernel-internal printk ratelimit interval
    fd = open("/proc/sys/kernel/printk_ratelimit", O_WRONLY);
    if (fd < 0)
        return -1;
    len = snprintf(buf, sizeof(buf), "%d\n", n);
    written = write(fd, buf, len);
    close(fd);
    return written == len ? 0 : -1;
}
