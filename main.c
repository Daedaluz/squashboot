#include <stdio.h>
#include <sys/mount.h>
#include <dirent.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>


const int squashfs_magic = 0x73717368;


void rrm(int src);

char *find_squasfs();

// create dir if it does not exist
int mkdirp(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return 0;
    }
    return mkdir(path, 0755);
}

int main(int argc, char *argv[]) {
    // First things first, we need a device tree
    if (mount("none", "/dev", "tmpdevfs", 0, NULL) != 0) {
        perror("mount /dev");
        return 1;
    }
    // Setup logging
    int fd = open("/dev/kmsg", O_WRONLY | O_NOCTTY);
    if (fd == -1) {
        perror("open /dev/kmsg");
        return 1;
    }
    // save stdout and stderr
    int stdout_fd = dup(1);
    int stderr_fd = dup(2);
    // use kmsg as stdout and stderr
    dup2(fd, 2);
    dup2(fd, 1);
    close(fd);

    // create /dev/pts
    if (mkdirp("/dev/pts") == -1) {
        perror("mkdir /dev/pts");
        return 1;
    }

    // mount pseudo filesystems
    if (mount("none", "/dev/pts", "devpts", 0, NULL) != 0) {
        perror("mount /dev/pts");
        return 1;
    }
    if (mount("none", "/proc", "proc", 0, NULL) != 0) {
        perror("mount /proc");
        return 1;
    }
    if (mount("none", "/sys", "sysfs", 0, NULL) != 0) {
        perror("mount /sys");
        return 1;
    }

    // find the squashfs filesystem and mount it
    char *squashfs = find_squasfs();
    if (squashfs == NULL) {
        fprintf(stderr, "No squashfs filesystem found\n");
        return 1;
    }
    if (mount(squashfs, "/newroot", "squashfs", MS_RDONLY | MS_I_VERSION, NULL) != 0) {
        perror("mount squashfs to new root");
        return 1;
    }

    // move the pseudo filesystems to the new root
    if (mount("/dev", "/newroot/dev", NULL, MS_MOVE, NULL) != 0) {
        perror("move mount /dev");
        return 1;
    }
    if (mount("/proc", "/newroot/proc", NULL, MS_MOVE, NULL) != 0) {
        perror("move mount /dev");
        return 1;
    }
    if (mount("/sys", "/newroot/sys", NULL, MS_MOVE, NULL) != 0) {
        perror("move mount /dev");
        return 1;
    }

    // chroot to the new root while retaining a reference to the old root
    // so that we can delete the remnants of the old root and free up RAM
    if (chdir("/newroot") == -1) {
        perror("chdir /newroot");
        return 1;
    }
    int parent = open("/", O_DIRECTORY | O_RDWR);
    if (parent == -1) {
        perror("open /");
        return 1;
    }

    if (chroot(".") == -1) {
        perror("chroot");
        return 1;
    }

    if (chdir("/") == -1) {
        perror("chdir /");
        return 1;
    }
    // delete the old root
    rrm(parent);

    // restore stdout and stderr
    dup2(stdout_fd, 1);
    dup2(stderr_fd, 2);

    // exec next init
    char *argv2[] = {"/sbin/init", NULL};
    if (execv("/sbin/init", argv2) == -1) {
        perror("execv /sbin/init");
        return 1;
    }
    argv2[0] = "/init";
    if (execv("/init", argv2) == -1) {
        perror("execv /init");
        return 1;
    }
    fprintf(stderr, "No init found\n");
}

void rrm(int src) {
    struct dirent *de;
    DIR *dr = fdopendir(src);
    if (dr == NULL) {
        perror("fdopendir");
        return;
    }
    while ((de = readdir(dr)) != NULL) {
        if (de->d_type == DT_DIR) {
            int fd = openat(src, de->d_name, O_DIRECTORY | O_RDWR);
            if (fd == -1) {
                perror("openat");
                continue; // continue with the next file, should be returning
            }
            rrm(fd);
            close(fd);
        } else {
            if (unlinkat(src, de->d_name, 0) != 0) {
                perror("unlinkat");
                continue; // continue with the next file, should be returning
            }
        }
    }
    closedir(dr);
    if (unlinkat(src, "", AT_REMOVEDIR) != 0) {
        perror("unlinkat");
        return;
    }
}

// find_squashfs finds the first squashfs filesystem in / and returns the path to it
char *find_squasfs() {
    struct dirent *de;
    DIR *dr = opendir("/");
    if (dr == NULL) {
        perror("opendir");
        return NULL;
    }
    while ((de = readdir(dr)) != NULL) {
        if (de->d_type == DT_REG) {
            FILE *f = fopen(de->d_name, "r");
            if (f == NULL) {
                perror("fopen");
                return NULL;
            }
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
