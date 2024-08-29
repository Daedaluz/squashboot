#include <stdio.h>
#include <sys/mount.h>
#include <dirent.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/loop.h>
#include <malloc.h>
#include <errno.h>
#include <signal.h>

const int squashfs_magic = 0x73717368;

int mkdirp(const char *path);

void rrm(int src);

char *find_squasfs();

int setup_loop(const char *file);

void handler (int sig) {
    printf("Caught signal %d\n", sig);
    fflush(stdout);
}


int main(int argc, char *argv[]) {
    // First things first, we need a device tree
    if (mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) != 0) {
        printf("mount /dev: %s\n", strerror(errno));
        fflush(stdout);
        return 1;
    }
    // Setup logging
    int fd = open("/dev/kmsg", O_WRONLY | O_NOCTTY| O_CLOEXEC);
    if (fd == -1) {
        printf("open /dev/kmsg: %s\n", strerror(errno));
        fflush(stdout);
        return 1;
    }
    // save stdout and stderr
    int stdout_fd = dup(1);
    int stderr_fd = dup(2);
    // use kmsg as stdout and stderr
    dup2(fd, 2);
    dup2(fd, 1);
    close(fd);

    printf("Mounting pseudo filesystems\n");
    fflush(stdout);

    // create /dev/pts
    if (mkdirp("/dev/pts") == -1) {
        printf("mkdir /dev/pts: %s\n", strerror(errno));
        fflush(stdout);
        return 1;
    }

    // mount pseudo filesystems
    if (mount("pts", "/dev/pts", "devpts", 0, NULL) != 0) {
        printf("mount /dev/pts: %s\n", strerror(errno));
        fflush(stdout);
        return 1;
    }
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        printf("mount /proc: %s\n", strerror(errno));
        fflush(stdout);
        return 1;
    }
    if (mount("sysfs", "/sys", "sysfs", 0, NULL) != 0) {
        printf("mount /sys: %s\n", strerror(errno));
        fflush(stdout);
        return 1;
    }

    printf("Locating squashfs rootfs\n");
    fflush(stdout);

    // find the squashfs filesystem and mount it
    char *squashfs = find_squasfs();
    if (squashfs == NULL) {
        fprintf(stderr, "No squashfs filesystem found\n");
        return 1;
    }

    printf("Found squashfs at %s\n", squashfs);
    fflush(stdout);
    printf("Attaching %s to /dev/loop0\n", squashfs);
    fflush(stdout);
    // setup a loop device and attach the squashfs filesystem to it
    int dev = setup_loop(squashfs);
    free(squashfs);
    if (dev == -1) {
        return 1;
    }

    char loopdev[20];
    snprintf(loopdev, 20, "/dev/loop%d", dev);

    printf("Mounting %s to /newroot\n", loopdev);
    fflush(stdout);
    if (mount(loopdev, "/newroot", "squashfs", MS_RDONLY | MS_I_VERSION, NULL) != 0) {
        printf("mount %s: %s", loopdev, strerror(errno));
        return 1;
    }

    printf("Moving pseudo filesystems to new root\n");
    fflush(stdout);
    // move the pseudo filesystems to the new root
    if (mount("/dev", "/newroot/dev", NULL, MS_MOVE, NULL) != 0) {
        printf("move mount /dev: %s", strerror(errno));
        fflush(stdout);
        return 1;
    }
    if (mount("/proc", "/newroot/proc", NULL, MS_MOVE, NULL) != 0) {
        printf("move mount /proc: %s", strerror(errno));
        fflush(stdout);
        return 1;
    }
    if (mount("/sys", "/newroot/sys", NULL, MS_MOVE, NULL) != 0) {
        printf("move mount /sys: %s", strerror(errno));
        fflush(stdout);
        return 1;
    }

    // chroot to the new root while retaining a reference to the old root
    // so that we can delete the remnants of the old root and free up RAM
    printf("Entering new root\n");
    fflush(stdout);
    if (chdir("/newroot") == -1) {
        printf("chdir /newroot error: %s\n", strerror(errno));
        return 1;
    }
    int parent = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent == -1) {
        printf("open / error: %s\n", strerror(errno));
        fflush(stdout);
        return 1;
    }

    if (chroot(".") == -1) {
        printf("chroot . error: %s\n", strerror(errno));
        fflush(stdout);
        return 1;
    }

    if (chdir("/") == -1) {
        printf("chdir / error: %s\n", strerror(errno));
        fflush(stdout);
        return 1;
    }
    printf("Zaping old root\n");
    fflush(stdout);
    // TODO: delete the old root
    //rrm(parent);
    close(parent);
    printf("Restoring stdout / stderr and hand over to new init\n");
    fflush(stdout);
    // restore stdout and stderr
    dup2(stdout_fd, 1);
    dup2(stderr_fd, 2);

    // exec next init
    char *argv2[] = {"/sbin/init", NULL};
    if (execv("/sbin/init", argv2) == -1) {
        printf("execv /sbin/init: %s\n", strerror(errno));
        fflush(stdout);
        return 1;
    }
    argv2[0] = "/init";
    if (execv("/init", argv2) == -1) {
        printf("execv /init: %s\n", strerror(errno));
        fflush(stdout);
        return 1;
    }
    fprintf(stderr, "No init found\n");
    return 1;
}

// FIXME: this is not working
void rrm(int src) {
    struct dirent *de;
    DIR *dr = fdopendir(src);
    if (dr == NULL) {
        printf("fdopendir: %s\n", strerror(errno));
        fflush(stdout);
        return;
    }
    while (1) {
        de = readdir(dr);
        if (de == NULL) {
            printf("readdir: %s\n", strerror(errno));
            fflush(stdout);
            break;
        }
        if (de->d_type == DT_DIR) {
            int isdot = strcmp(de->d_name, ".") == 0;
            int isdotdot = strcmp(de->d_name, "..") == 0;
            if (isdot || isdotdot) {
                printf("Skipping %s\n", de->d_name);
                fflush(stdout);
                continue;
            }
            printf("Recursing into %s %d %d\n", de->d_name, strcmp(de->d_name, "."), strcmp(de->d_name, ".."));
            fflush(stdout);
            int fd = openat(src, de->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (fd == -1) {
                printf("openat %s: %s\n", de->d_name, strerror(errno));
                fflush(stdout);
                continue; // continue with the next file, should be returning
            }
            printf("Deleting subdir %s\n", de->d_name);
            rrm(fd);
            close(fd);
        } else {
            printf("Deleting %s\n", de->d_name);
            fflush(stdout);
            if (unlinkat(src, de->d_name, 0) != 0) {
                printf("unlinkat %s: %s\n", de->d_name, strerror(errno));
                fflush(stdout);
                continue; // continue with the next file, should be returning
            }
        }
    }
    printf("Deleting directory\n");
    closedir(dr);
    if (unlinkat(src, "", AT_REMOVEDIR) != 0) {
        printf("unlinkat %s: %s\n", de->d_name, strerror(errno));
        fflush(stdout);
        return;
    }
}

// find_squashfs finds the first squashfs filesystem in / and returns the path to it
char *find_squasfs() {
    struct dirent *de;
    DIR *dr = opendir("/");
    if (dr == NULL) {
        printf("opendir /: %s\n", strerror(errno));
        fflush(stdout);
        return NULL;
    }
    while ((de = readdir(dr)) != NULL) {
        if (de->d_type == DT_REG) {
            FILE *f = fopen(de->d_name, "r");
            if (f == NULL) {
                printf("fopen %s: %s", de->d_name, strerror(errno));
                return NULL;
            }
            printf("Testing %s for squashfs magic\n", de->d_name);
            fflush(stdout);
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
    if (fd == -1) {
        printf("open /dev/loop-control: %s\n", strerror(errno));
        fflush(stdout);
        return -1;
    }
    int loop = ioctl(fd, LOOP_CTL_GET_FREE);
    if (loop == -1) {
        printf("ioctl LOOP_CTL_GET_FREE: %s\n", strerror(errno));
        fflush(stdout);
        return -1;
    }
    close(fd);

    char loop_device[20];
    snprintf(loop_device, 20, "/dev/loop%d", loop);

    fd = open(loop_device, O_RDWR);
    if (fd == -1) {
        printf("open loop device %s\n", strerror(errno));
        fflush(stdout);
        return -1;
    }

    int filefd = open(file, O_RDWR);

    if (ioctl(fd, LOOP_SET_FD, filefd) == -1) {
        printf("ioctl LOOP_SET_FD: %s\n", strerror(errno));
        fflush(stdout);
        return -1;
    }
    close(filefd);
    return loop;
}
