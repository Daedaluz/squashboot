# squashboot

Tiny, statically linked, libc-free init for initramfs.

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
| `CONFIG_UNIX98_PTYS` | `/dev/pts` |
| `CONFIG_CGROUPS` + `CONFIG_CGROUP_V2` | `/sys/fs/cgroup` |
| `CONFIG_CONFIGFS_FS` | `/sys/kernel/config` |
| `CONFIG_PRINTK` | `/dev/kmsg` early logging |

## No libc

squashboot links against no libc at all — not even statically. It builds
freestanding (`-nostdlib -ffreestanding -static -no-pie`) against a vendored,
pinned snapshot of the Linux kernel's own minimal libc replacement,
[`tools/include/nolibc`](https://github.com/torvalds/linux/tree/master/tools/include/nolibc),
copied into `third_party/nolibc/` (dual-licensed `LGPL-2.1 OR MIT`, safe to
vendor outside the kernel tree). The only thing still linked in is `libgcc`,
which supplies compiler runtime helpers and is part of the toolchain, not a
libc.

nolibc covers `_start`, raw syscalls, and a small `string.h`/`stdio.h`, but
not everything this program needs. Two small local modules fill the gaps:

- **`sys.c`/`sys.h`** — `fcntl()`, `unlinkat()`, `execv()`, implemented as
  thin syscall wrappers using nolibc's own raw syscall primitives.
- **`dir.c`/`dir.h`** — `sb_opendir()`/`sb_fdopendir()`/`sb_readdir()`/
  `sb_closedir()`/`sb_dirfd()` and `struct sb_dirent` (with `d_type`), built
  on nolibc's `getdents64()`. They're `sb_`-prefixed rather than the
  standard names because nolibc's own `dirent.h` already defines `DIR`/
  `struct dirent`/`opendir`/`closedir` (without `readdir()`, `dirfd()`, or
  `d_type`), and every nolibc header pulls that in transitively — so reusing
  the standard names would collide.

### Updating vendored nolibc

```sh
cmake --build build --target update-nolibc          # latest stable kernel tag
scripts/update-nolibc.sh v6.13                       # pin to a specific ref
```

nolibc's API is not guaranteed stable across kernel releases — e.g. its raw
syscall macros were renamed (`my_syscallN` → `__nolibc_syscallN`) between the
versions this project has vendored. After updating, rebuild and check
`sys.c`/`dir.c` still compile before committing; the target only fetches, it
doesn't verify.

## Building

Requires CMake ≥ 3.22 and a GCC or Clang toolchain capable of freestanding
builds (`-nostdlib -ffreestanding`). The binary is statically linked with no
dynamic section at all.

```sh
cmake -B build
cmake --build build
# binary: build/squashboot
```

### Release builds

Configure with `CMAKE_BUILD_TYPE=Release` (or `MinSizeRel`). Those configs link
with `-s`, so the binary comes out stripped; `Debug` and `RelWithDebInfo` keep
their symbols.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
# binary: build/squashboot, stripped
```

Pre-built static binaries for `amd64` and `arm64` are available on the
[releases page](../../releases).
