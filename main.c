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
#include <ftw.h>
#include "util.h"

const char *get_fd_type(int fd) {
    struct stat statbuf;

    // Perform fstat on the file descriptor
    if (fstat(fd, &statbuf) == -1) {
        perror("fstat");
        return "Unknown (Error)";
    }

    // Determine the file type
    if (S_ISREG(statbuf.st_mode)) {
        return "Regular File";
    } else if (S_ISDIR(statbuf.st_mode)) {
        return "Directory";
    } else if (S_ISCHR(statbuf.st_mode)) {
        return "Character Device";
    } else if (S_ISBLK(statbuf.st_mode)) {
        return "Block Device";
    } else if (S_ISFIFO(statbuf.st_mode)) {
        return "FIFO (Named Pipe)";
    } else if (S_ISSOCK(statbuf.st_mode)) {
        return "Socket";
    } else if (S_ISLNK(statbuf.st_mode)) {
        return "Symbolic Link";
    } else {
        return "Unknown";
    }
}

char found_path[255];
int target_inode;

int stdout_fd;
int stderr_fd;

// Callback function for ftw
int find_by_inode(const char *fpath, const struct stat *sb, int typeflag) {
    if (sb->st_ino < 10) {
        char buff[255];
        int n = snprintf(buff, 255, "%s: %lu\n", fpath, sb->st_ino);
        write(stdout_fd, buff, n);
    }
    if (sb->st_ino == target_inode) {
        strncpy(found_path, fpath, 255 - 1);
        found_path[255 - 1] = '\0';  // Ensure null-termination
        return 1; // Stop the search
    }
    return 0; // Continue search
}

// Function to locate a file by inode number
const char *locate_file_by_inode(const char *start_dir, ino_t inode) {
    target_inode = inode;
    found_path[0] = '\0';  // Clear previous results

    ftw(start_dir, find_by_inode, FTW_D | FTW_F | FTW_DNR | FTW_NS | FTW_SL);
    return found_path[0] != '\0' ? found_path : NULL;
}

const int squashfs_magic = 0x73717368;

int mkdirp(const char *path);

void rrm(int src);

char *find_squasfs();

int setup_loop(const char *file);

void handler(int sig) {
    klog("cought signal %s(%d)", strsignal(sig), sig);
    exit(-1);
}

void mount_pseudofs(const char *src, const char *target, const char *fs) {
    char msg[25];
    snprintf(msg, 25, "mount %s", src);
    assert(msg, mount(src, target, fs, 0, NULL) != 0);
}

void move_mount(const char *src, const char *dst) {
    char msg[100];
    snprintf(msg, 100, "move mount %s to %s", src, dst);
    assert(msg, mount(src, dst, NULL, MS_MOVE, NULL) != 0);
}

void mountfs(const char *src, const char *dst, const char *fs) {
    char msg[100];
    snprintf(msg, 100, "mounting %s on %s", src, dst);
    assert(msg, mount(src, dst, fs, MS_RDONLY | MS_I_VERSION, NULL) != 0);
}

void set_printk_ratelimit(int burst, int n) {
    int burstfd = open("/proc/sys/kernel/printk_ratelimit_burst", O_RDWR);
    int ratefd = open("/proc/sys/kernel/printk_ratelimit", O_RDWR);
    assert("unable to open /proc/sys/kernel/printk_ratelimit_burst", burstfd == -1);
    assert("unable to open /proc/sys/kernel/printk_ratelimit", ratefd == -1);
    char buff[10];
    snprintf(buff, 10, "%d\n", burst);
    write(burstfd, buff, strlen(buff));
    snprintf(buff, 10, "%d\n", n);
    write(burstfd, buff, strlen(buff));
    write(ratefd, "50\n", 3);
    close(burstfd);
    close(ratefd);
}

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

    set_printk_ratelimit(50, 50);

    // find the squashfs filesystem and mount it
    char *squashfs = find_squasfs();
    assert("No squashfs filesystem found", squashfs == NULL);

    klog("Found squashfs at %s", squashfs);
    klog("Attaching %s to a loop device", squashfs);

    // setup a loop device and attach the squashfs filesystem to it
    int dev = setup_loop(squashfs);
    free(squashfs);

    char loopdev[20];
    snprintf(loopdev, 20, "/dev/loop%d", dev);

    klog("Mounting %s to /newroot", loopdev);
    mountfs(loopdev, "/newroot", "squashfs");

    // move the pseudo filesystems to the new root
    klog("Moving pseudo filesystems to new root");
    move_mount("/dev", "/newroot/dev");
    move_mount("/proc", "/newroot/proc");
    move_mount("/sys", "/newroot/sys");
    move_mount("/tmp", "/newroot/tmp");
    move_mount("/run", "/newroot/run");

    //mount("/", "/newroot/mnt", NULL, MS_BIND, NULL);

    // chroot to the new root while retaining a reference to the old root
    // so that we can delete the remnants of the old root and free up RAM
    klog("Entering new root");
    assert("chdir to /newroot", chdir("/newroot") != 0);
    int parent = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert("open /", parent == -1);
    move_mount("/newroot", "/");
    assert("chroot to \".\"", chroot(".") != 0);
    assert("chdir \"/\"", chdir("/") != 0);

    // TODO: delete the old root
    klog("Zapping old root");
    //rrm(parent);
    close(parent);

    // restore stdout and stderr
    dup2(stdout_fd, 1);
    dup2(stderr_fd, 2);

    // shellinit
    if (access("/init.sh", F_OK) == 0) {
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
    };
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

// FIXME: this is not working
void rrm(int src) {
    struct dirent *de;
    DIR *dr = fdopendir(src);
    if (dr == NULL) {
        klog("fdopendir: %s\n", strerror(errno));
        return;
    }
    while (1) {
        de = readdir(dr);
        if (de == NULL) {
            klog("readdir: %s\n", strerror(errno));
            break;
        }
        if (de->d_type == DT_DIR) {
            int isdot = strcmp(de->d_name, ".") == 0;
            int isdotdot = strcmp(de->d_name, "..") == 0;
            if (isdot || isdotdot) {
                klog("Skipping %s\n", de->d_name);
                continue;
            }
            klog("Recursing into %s %d %d\n", de->d_name, strcmp(de->d_name, "."), strcmp(de->d_name, ".."));
            int fd = openat(src, de->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (fd == -1) {
                klog("openat %s: %s\n", de->d_name, strerror(errno));
                continue; // continue with the next file, should be returning
            }
            klog("Deleting subdir %s\n", de->d_name);
            close(fd);
        } else {
            klog("Deleting %s\n", de->d_name);
            if (unlinkat(src, de->d_name, 0) != 0) {
                klog("unlinkat %s: %s\n", de->d_name, strerror(errno));
                continue; // continue with the next file, should be returning
            }
        }
    }
    klog("Deleting directory\n");
    closedir(dr);
    if (unlinkat(src, "", AT_REMOVEDIR) != 0) {
        klog("unlinkat %s: %s\n", de->d_name, strerror(errno));
        return;
    }
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
