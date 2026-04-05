# squashboot

Tiny, statically linked init for initramfs.

It searches for the first squashfs file in the initramfs root, attaches it to
a loop device, mounts it as the new root, relocates the pseudo-filesystems into
it, then execs `/init`, `/sbin/init`, or (if `/init.sh` and `/sbin/getty` are
present) a getty running `/init.sh`.

## Kernel requirements

The kernel must be built with the options listed in `kernel.config`. A config
fragment is provided so you can merge it into an existing `.config`:

```sh
scripts/kconfig/merge_config.sh .config kernel.config
```

| Config symbol | Purpose |
|---|---|
| `CONFIG_BLK_DEV_LOOP` | Attach the squashfs image file to a loop device |
| `CONFIG_SQUASHFS` | Mount the squashfs root image |
| `CONFIG_SQUASHFS_ZLIB/LZ4/LZO/XZ/ZSTD` | Decompression — enable the codec(s) your image uses |
| `CONFIG_TMPFS` | `/tmp`, `/run`, `/dev/shm` |
| `CONFIG_DEVTMPFS` + `CONFIG_DEVTMPFS_MOUNT` | Populate `/dev` before init runs |
| `CONFIG_PROC_FS` | `/proc` |
| `CONFIG_SYSFS` | `/sys` |
| `CONFIG_DEVPTS_FS` | `/dev/pts` |
| `CONFIG_CGROUPS` + `CONFIG_CGROUP_V2` | `/sys/fs/cgroup` |
| `CONFIG_CONFIGFS_FS` | `/sys/kernel/config` |
| `CONFIG_PRINTK` | `/dev/kmsg` early logging |

## Building

Requires CMake ≥ 3.22 and a C11 compiler. The binary is statically linked.

```sh
cmake -B build
cmake --build build
# binary: build/squashboot
```

Pre-built static binaries for `amd64` and `arm64` are available on the
[releases page](../../releases).
