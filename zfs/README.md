# zfs-uefi

A read-only ZFS driver for UEFI, in the manner of [ntfs-uefi](../ntfs) and
[btrfs-uefi](../btrfs). It puts an `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` on
partitions that hold a ZFS pool, so that Quibble can load Windows from a ZFS
dataset.

**Status: experimental.** It builds, but it has not been run yet.

## How it works

The root of the file system is the pool's `bootfs` property, or the pool's root
dataset if that isn't set. Files are found and read by FreeBSD's boot-time
ZFS reader (`src/engine/`), which is the same code that FreeBSD's loader uses.

The reader is kept apart from the EFI code. `src/zfsengine.h` is a small plain C
interface to it, and its headers (BSD libsa style, and not gnu-efi) never meet
gnu-efi's. `src/compat/` has just enough of the BSD environment for it to build.

`GetArcName` is deliberately unsupported, so Quibble keeps the ARC name from
`freeldr.ini`. Use the ordinary one for the partition, e.g.

    SystemPath=multi(0)disk(0)rdisk(0)partition(2)\Windows

`GetWindowsDriverName` returns `OpenZFS`, the name of the Windows driver
(<https://github.com/openzfsonwindows/openzfs>), which must be a boot-start
driver in the SYSTEM hive of the Windows installation.

## Building

As for the other drivers:

    cmake -DCMAKE_TOOLCHAIN_FILE=mingw-amd64.cmake -S zfs -B zfs/build
    cmake --build zfs/build

You need `gnu-efi` and the MinGW cross compiler (`mingw-w64`), and `zfs.efi`
goes in the `drivers` directory next to `quibble.efi`.

## What the pool needs

* The reader doesn't know about encryption, so the pool or dataset can't be
  encrypted.
* It only understands the pool features that FreeBSD's loader does (the list is
  `features_for_read` in `src/engine/zfsimpl.c`). If the pool has others turned
  on, it will refuse to mount it and say which one. Set a `compatibility=` when
  creating the pool to keep it to the ones that are supported.
* Compression: the reader understands lz4, lzjb and zle, and uncompressed data
  (none of that has been run here yet). **gzip is not supported**
  (`engine/gzip.c` is a stub, zlib is not wired in yet), **zstd is not** yet,
  and neither is the **blake3** checksum (`engine/blake3_stub.c` deliberately
  gives a hash that won't match, so blocks read as corrupt instead of being
  accepted).

## Testing

`test/zap_case_test.c` is a host test of the case insensitive lookups, with
hand-made ZAPs. The command to build and run it is in the file. It can't tell
whether the hashes agree with ZFS's own; only a pool made by ZFS can.

## Known problems

* **Case-insensitive lookups only fold ASCII.** A dataset with
  `casesensitivity=insensitive` hashes and matches directory entries after
  upper-casing the name, and FreeBSD's reader didn't, so large directories like
  System32 couldn't be searched. `zap_lookup()` and what it calls now do that
  (see `zap_normalize_name()` in `src/engine/zfsimpl.c`), but only for ASCII: a
  name with an accented letter, say, will not be found in such a dataset. It
  is tested against hand-made ZAPs and against a pool made by OpenZFS on Windows (see `test/native_read_test.c`: a 402 entry fat-ZAP directory and mixed-case lookups).
* Only the first device of a pool is probed when the driver starts, so a pool
  of several devices that aren't all there yet will not work.
* The timestamps in `EFI_FILE_INFO` are always zero, and `EFI_FILE_SYSTEM_INFO`
  isn't implemented.

## Origin and licence

Everything under `src/` that isn't listed below is licensed like Quibble, under
the GNU Lesser General Public Licence, version 3 or later (see `LICENCE`).

`src/engine/` is not ours:

The licence is whatever the header of each file says; this is a summary:

| File(s) | From | Licence |
| --- | --- | --- |
| `zfsimpl.c`, `zfsimpl.h` | FreeBSD `stand/libsa/zfs`, `sys/cddl/boot/zfs` | BSD (Doug Rabson, McAfee) |
| `nvlist.c`, `nvlist.h` | FreeBSD `stand/libsa/zfs` | BSD (Toomas Soome) |
| `zfssubr.c`, `fletcher.c`, `sha256.c`, `lzjb.c`, `zle.c`, `blkptr.c`, `skein_zfs.c`, `blake3_zfs.c` | FreeBSD `sys/cddl/boot/zfs` | CDDL 1.0 |
| `lz4.c`, `lz4.h` | FreeBSD `sys/cddl/contrib/opensolaris/common/lz4` | BSD 2-clause (Yann Collet) |
| `skein.c`, `skein.h`, `skein_block.c`, `skein_port.h`, `skein_iv.h`, `skein_debug.h` | FreeBSD `sys/crypto/skein` | public domain (Doug Whiting) |
| `skein_freebsd.h` | FreeBSD `sys/crypto/skein` | BSD (Allan Jude) |
| `list.c`, and `compat/sys/{list,list_impl,blake3,zfs_bootenv,zfs_bootenv_os}.h` | OpenZFS | CDDL 1.0 |
| `compat/sys/queue.h` | FreeBSD `sys/sys/queue.h` | BSD 3-clause |

The CDDL files remain under the CDDL, which is a file-level licence, so the
usual rules about keeping those files' source available and their headers
intact apply to them.

They were taken from FreeBSD `main` as of `d2145e9b9644` (2026-10-20).
`zfsimpl.c` was last changed upstream at `262fa4695d90`, and `zfssubr.c` at
`e2df9bb44109`. Their licence headers are as they came. Changes made here:

* `engine/gzip.c` is a stub in place of FreeBSD's, which needs zlib.
* `engine/libzfs.h` is a cut down version of FreeBSD's `libzfs.h`.
* `engine/blake3_stub.c` is ours.
