/* Copyright (c) Mark Harmstone 2020
 *
 * This file is part of zfs-efi.
 *
 * zfs-efi is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public Licence as published by
 * the Free Software Foundation, either version 3 of the Licence, or
 * (at your option) any later version.
 *
 * zfs-efi is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public Licence for more details.
 *
 * You should have received a copy of the GNU Lesser General Public Licence
 * along with zfs-efi.  If not, see <http://www.gnu.org/licenses/>. */

/*
 * A read-only ZFS driver for UEFI, in the manner of ntfs-efi and btrfs-efi: it
 * puts an EFI_SIMPLE_FILE_SYSTEM_PROTOCOL on the partitions that hold a ZFS
 * pool, so that Quibble can load Windows from a ZFS dataset.
 *
 * The root of the file system is the pool's bootfs, or its root dataset if
 * there is none. The on-disk format is read by FreeBSD's boot-time ZFS reader,
 * in engine/, via the plain C interface of zfsengine.h.
 */

#include <efibind.h>
#include <efidef.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include "misc.h"
#include "quibbleproto.h"
#include "zfsengine.h"

#define UNUSED(x) (void)(x)

/* the reader's own error for a failed checksum, which errno.h lacks */
#define ZFSE_ECKSUM 1122

#define MAX_PATH_LEN 1024

EFI_SYSTEM_TABLE* systable;
EFI_BOOT_SERVICES* bs;
EFI_QUIBBLE_INFO_PROTOCOL* info_proto = NULL;

EFI_DRIVER_BINDING_PROTOCOL drvbind;

typedef struct _volume {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL proto;
    EFI_QUIBBLE_PROTOCOL quibble_proto;
    EFI_HANDLE controller;
    EFI_BLOCK_IO_PROTOCOL* block;
    EFI_DISK_IO_PROTOCOL* disk_io;
    struct zfse_pool* pool;
    struct zfse_mount* mnt;
    struct _volume* next;
} volume;

typedef struct {
    EFI_FILE_PROTOCOL proto;
    volume* vol;
    struct zfse_file* f;
    char* path; // absolute, UTF-8, '/' separated
    CHAR16* name; // last component, for EFI_FILE_INFO
    uint64_t pos;
    bool is_dir;
    bool have_pending;
    struct zfse_dirent pending;
} file_handle;

static volume* volumes = NULL;

#define CONTAINER(ptr, type, field) ((type*)((uint8_t*)(ptr) - offsetof(type, field)))

#ifdef ZFS_TRACE
static void trace(const char* fmt, ...);
#endif

void zfs_print(const char* s) {
#ifdef ZFS_TRACE
    /* what the reader says is on the console too, and long enough to read */
    trace("%s", s);
    bs->Stall(500000);
#endif
    if (info_proto)
        info_proto->Print(s);
}

/* ZFS_TRACE: breadcrumbs on the firmware console, for finding where it dies */
#ifdef ZFS_TRACE
#include <stdarg.h>
int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap);

static void trace(const char* fmt, ...) {
    char s[256];
    CHAR16 w[300];
    va_list ap;
    UINTN i, j = 0;

    va_start(ap, fmt);
    vsnprintf(s, sizeof(s), fmt, ap);
    va_end(ap);

    for (i = 0; s[i] && j < 296; i++) {
        if (s[i] == '\n')
            w[j++] = '\r';
        w[j++] = (CHAR16)(unsigned char)s[i];
    }
    w[j] = 0;

    systable->ConOut->OutputString(systable->ConOut, w);
}

#define tracep(...) do { trace(__VA_ARGS__); bs->Stall(1500000); } while (0)
#define tracef(...) do { trace(__VA_ARGS__); bs->Stall(400000); } while (0)
#else
#define tracef(...) ((void)0)
#define trace(...) ((void)0)
#define tracep(...) ((void)0)
#endif

static void print_error(const char* func, EFI_STATUS Status) {
    char s[200], *p;

    p = stpcpy(s, func);
    p = stpcpy(p, " returned ");
    p = stpcpy(p, error_string(Status));
    p = stpcpy(p, "\n");

    zfs_print(s);
}

static EFI_STATUS errno_to_status(int err) {
    switch (err) {
        case 0:
            return EFI_SUCCESS;

        case ENOENT:
        case ENOTDIR:
            return EFI_NOT_FOUND;

        case ENOMEM:
            return EFI_OUT_OF_RESOURCES;

        case EISDIR:
        case EINVAL:
        case ENAMETOOLONG:
            return EFI_INVALID_PARAMETER;

        case EROFS:
            return EFI_WRITE_PROTECTED;

        case ZFSE_ECKSUM:
            return EFI_VOLUME_CORRUPTED;

        default:
            return EFI_DEVICE_ERROR;
    }
}

/* reading the device */

static int vol_read(void* priv, uint64_t offset, void* buf, size_t bytes) {
    volume* vol = priv;
    EFI_STATUS Status;

    Status = vol->disk_io->ReadDisk(vol->disk_io, vol->block->Media->MediaId, offset, bytes, buf);

    return EFI_ERROR(Status) ? EIO : 0;
}

/* paths */

/* turn "a/./b/../c//d" into "/a/c/d" */
static bool normalize_path(const char* in, char* out, size_t outlen) {
    size_t len = 1;

    if (outlen < 2)
        return false;

    out[0] = '/';
    out[1] = 0;

    while (*in) {
        const char* end;
        size_t clen;

        while (*in == '/')
            in++;

        if (!*in)
            break;

        end = in;
        while (*end && *end != '/')
            end++;

        clen = end - in;

        if (clen == 1 && in[0] == '.') {
            // nothing
        } else if (clen == 2 && in[0] == '.' && in[1] == '.') {
            // drop the last component, if there is one
            while (len > 1 && out[len - 1] != '/')
                len--;

            if (len > 1)
                len--;

            out[len] = 0;
        } else {
            if (len + (len > 1 ? 1 : 0) + clen + 1 > outlen)
                return false;

            if (len > 1)
                out[len++] = '/';

            memcpy(&out[len], in, clen);
            len += clen;
            out[len] = 0;
        }

        in = end;
    }

    return true;
}

static CHAR16* utf8_to_new_utf16(const char* s) {
    EFI_STATUS Status;
    unsigned int len = 0;
    CHAR16* ret;

    Status = utf8_to_utf16(NULL, 0, &len, s, (unsigned int)strlen(s));
    if (EFI_ERROR(Status) && Status != EFI_BUFFER_TOO_SMALL)
        return NULL;

    Status = bs->AllocatePool(EfiBootServicesData, len + sizeof(CHAR16), (void**)&ret);
    if (EFI_ERROR(Status))
        return NULL;

    if (len > 0) {
        Status = utf8_to_utf16((wchar_t*)ret, len, &len, s, (unsigned int)strlen(s));
        if (EFI_ERROR(Status)) {
            bs->FreePool(ret);
            return NULL;
        }
    }

    ret[len / sizeof(CHAR16)] = 0;

    return ret;
}

static char* utf16_to_new_utf8(const CHAR16* s) {
    EFI_STATUS Status;
    unsigned int len = 0;
    char* ret;

    Status = utf16_to_utf8(NULL, 0, &len, (const wchar_t*)s, (unsigned int)(wcslen((const wchar_t*)s) * sizeof(CHAR16)));
    if (EFI_ERROR(Status) && Status != EFI_BUFFER_TOO_SMALL)
        return NULL;

    Status = bs->AllocatePool(EfiBootServicesData, len + 1, (void**)&ret);
    if (EFI_ERROR(Status))
        return NULL;

    if (len > 0) {
        Status = utf16_to_utf8(ret, len, &len, (const wchar_t*)s, (unsigned int)(wcslen((const wchar_t*)s) * sizeof(CHAR16)));
        if (EFI_ERROR(Status)) {
            bs->FreePool(ret);
            return NULL;
        }
    }

    ret[len] = 0;

    return ret;
}

/* file handles */

static EFI_STATUS EFIAPI file_open(struct _EFI_FILE_HANDLE* File, struct _EFI_FILE_HANDLE** NewHandle, CHAR16* FileName,
                                   UINT64 OpenMode, UINT64 Attributes);
static EFI_STATUS EFIAPI file_close(struct _EFI_FILE_HANDLE* File);
static EFI_STATUS EFIAPI file_delete(struct _EFI_FILE_HANDLE* File);
static EFI_STATUS EFIAPI file_read(struct _EFI_FILE_HANDLE* File, UINTN* BufferSize, VOID* Buffer);
static EFI_STATUS EFIAPI file_write(struct _EFI_FILE_HANDLE* File, UINTN* BufferSize, VOID* Buffer);
static EFI_STATUS EFIAPI file_get_position(struct _EFI_FILE_HANDLE* File, UINT64* Position);
static EFI_STATUS EFIAPI file_set_position(struct _EFI_FILE_HANDLE* File, UINT64 Position);
static EFI_STATUS EFIAPI file_get_info(struct _EFI_FILE_HANDLE* File, EFI_GUID* InformationType, UINTN* BufferSize, VOID* Buffer);
static EFI_STATUS EFIAPI file_set_info(struct _EFI_FILE_HANDLE* File, EFI_GUID* InformationType, UINTN BufferSize, VOID* Buffer);
static EFI_STATUS EFIAPI file_flush(struct _EFI_FILE_HANDLE* File);

static void populate_file_handle(EFI_FILE_PROTOCOL* h) {
    h->Revision = EFI_FILE_PROTOCOL_REVISION;
    h->Open = file_open;
    h->Close = file_close;
    h->Delete = file_delete;
    h->Read = file_read;
    h->Write = file_write;
    h->GetPosition = file_get_position;
    h->SetPosition = file_set_position;
    h->GetInfo = file_get_info;
    h->SetInfo = file_set_info;
    h->Flush = file_flush;
}

static void free_handle(file_handle* fh) {
    if (fh->f)
        zfse_close(fh->f);

    if (fh->path)
        bs->FreePool(fh->path);

    if (fh->name)
        bs->FreePool(fh->name);

    bs->FreePool(fh);
}

/* takes a normalised path; vol->mnt must be mounted */
static EFI_STATUS new_handle(volume* vol, const char* path, file_handle** out) {
    EFI_STATUS Status;
    file_handle* fh;
    struct zfse_stat st;
    const char* leaf;
    size_t pathlen = strlen(path);
    int rc;

    Status = bs->AllocatePool(EfiBootServicesData, sizeof(file_handle), (void**)&fh);
    if (EFI_ERROR(Status)) {
        print_error("AllocatePool", Status);
        return Status;
    }

    memset(fh, 0, sizeof(file_handle));

    rc = zfse_open(vol->mnt, path, &fh->f);
    if (rc != 0) {
        bs->FreePool(fh);
        return errno_to_status(rc);
    }

    zfse_stat(fh->f, &st);
    fh->is_dir = st.is_dir ? true : false;

    Status = bs->AllocatePool(EfiBootServicesData, pathlen + 1, (void**)&fh->path);
    if (EFI_ERROR(Status)) {
        print_error("AllocatePool", Status);
        free_handle(fh);
        return Status;
    }

    memcpy(fh->path, path, pathlen + 1);

    leaf = path[0] && path[1] ? strrchr(path, '/') + 1 : "";
    fh->name = utf8_to_new_utf16(leaf);
    if (!fh->name) {
        free_handle(fh);
        return EFI_OUT_OF_RESOURCES;
    }

    populate_file_handle(&fh->proto);
    fh->vol = vol;

    *out = fh;

    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_open(struct _EFI_FILE_HANDLE* File, struct _EFI_FILE_HANDLE** NewHandle, CHAR16* FileName,
                                   UINT64 OpenMode, UINT64 Attributes) {
    EFI_STATUS Status;
    file_handle* fh = CONTAINER(File, file_handle, proto);
    file_handle* nfh;
    char *name, *full, *norm;
    size_t fulllen;

    UNUSED(Attributes);

    // read-only
    if (OpenMode & (EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE))
        return EFI_WRITE_PROTECTED;

    name = utf16_to_new_utf8(FileName);
    if (!name)
        return EFI_OUT_OF_RESOURCES;

    for (char* p = name; *p; p++) {
        if (*p == '\\')
            *p = '/';
    }

    // a leading separator means from the root of the volume, anything else from here
    fulllen = strlen(fh->path) + 1 + strlen(name) + 1;

    Status = bs->AllocatePool(EfiBootServicesData, fulllen + MAX_PATH_LEN, (void**)&full);
    if (EFI_ERROR(Status)) {
        print_error("AllocatePool", Status);
        bs->FreePool(name);
        return Status;
    }

    norm = full + fulllen;

    if (name[0] == '/') {
        strcpy(full, name);
    } else {
        strcpy(full, fh->path);
        strcat(full, "/");
        strcat(full, name);
    }

    bs->FreePool(name);

    if (!normalize_path(full, norm, MAX_PATH_LEN)) {
        bs->FreePool(full);
        return EFI_INVALID_PARAMETER;
    }

    Status = new_handle(fh->vol, norm, &nfh);

    tracef("open %s -> %llx\n", norm, (unsigned long long)Status);

    bs->FreePool(full);

    if (EFI_ERROR(Status))
        return Status;

    *NewHandle = &nfh->proto;

    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_close(struct _EFI_FILE_HANDLE* File) {
    file_handle* fh = CONTAINER(File, file_handle, proto);

    free_handle(fh);

    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_delete(struct _EFI_FILE_HANDLE* File) {
    file_handle* fh = CONTAINER(File, file_handle, proto);

    free_handle(fh);

    return EFI_WARN_DELETE_FAILURE;
}

/* the size EFI_FILE_INFO needs for a name */
static UINTN file_info_size(const CHAR16* name) {
    return offsetof(EFI_FILE_INFO, FileName[0]) + ((wcslen((const wchar_t*)name) + 1) * sizeof(CHAR16));
}

static void fill_file_info(EFI_FILE_INFO* info, const CHAR16* name, uint64_t size, bool is_dir) {
    info->Size = file_info_size(name);
    info->FileSize = size;
    info->PhysicalSize = size;
    memset(&info->CreateTime, 0, sizeof(EFI_TIME));
    memset(&info->LastAccessTime, 0, sizeof(EFI_TIME));
    memset(&info->ModificationTime, 0, sizeof(EFI_TIME));
    info->Attribute = is_dir ? EFI_FILE_DIRECTORY : 0;

    memcpy(info->FileName, name, (wcslen((const wchar_t*)name) + 1) * sizeof(CHAR16));
}

static EFI_STATUS read_dir(file_handle* fh, UINTN* BufferSize, VOID* Buffer) {
    CHAR16* name;
    UINTN size;
    int rc;

    if (!fh->have_pending) {
        rc = zfse_readdir(fh->f, &fh->pending);
        if (rc == ENOENT) {
            *BufferSize = 0;
            return EFI_SUCCESS;
        } else if (rc != 0) {
            return errno_to_status(rc);
        }

        fh->have_pending = true;
    }

    name = utf8_to_new_utf16(fh->pending.name);
    if (!name)
        return EFI_OUT_OF_RESOURCES;

    size = file_info_size(name);

    if (*BufferSize < size) {
        *BufferSize = size;
        bs->FreePool(name);
        return EFI_BUFFER_TOO_SMALL;
    }

    fill_file_info((EFI_FILE_INFO*)Buffer, name, fh->pending.size, fh->pending.is_dir ? true : false);
    *BufferSize = size;

    bs->FreePool(name);

    fh->have_pending = false;

    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_read(struct _EFI_FILE_HANDLE* File, UINTN* BufferSize, VOID* Buffer) {
    file_handle* fh = CONTAINER(File, file_handle, proto);
    size_t got = 0;
    int rc;

    if (fh->is_dir)
        return read_dir(fh, BufferSize, Buffer);

    rc = zfse_read(fh->f, fh->pos, Buffer, *BufferSize, &got);
    tracef("read %s @%llx +%llx -> rc %d got %llx\n", fh->path, (unsigned long long)fh->pos,
           (unsigned long long)*BufferSize, rc, (unsigned long long)got);
    if (rc != 0)
        return errno_to_status(rc);

    fh->pos += got;
    *BufferSize = got;

    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_write(struct _EFI_FILE_HANDLE* File, UINTN* BufferSize, VOID* Buffer) {
    UNUSED(File);
    UNUSED(BufferSize);
    UNUSED(Buffer);

    return EFI_WRITE_PROTECTED;
}

static EFI_STATUS EFIAPI file_get_position(struct _EFI_FILE_HANDLE* File, UINT64* Position) {
    file_handle* fh = CONTAINER(File, file_handle, proto);

    // the UEFI spec says this isn't allowed on directories
    if (fh->is_dir)
        return EFI_UNSUPPORTED;

    *Position = fh->pos;

    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_set_position(struct _EFI_FILE_HANDLE* File, UINT64 Position) {
    file_handle* fh = CONTAINER(File, file_handle, proto);

    if (fh->is_dir) {
        // the only thing allowed on a directory is to start again
        if (Position != 0)
            return EFI_UNSUPPORTED;

        zfse_rewind(fh->f);
        fh->have_pending = false;

        return EFI_SUCCESS;
    }

    if (Position == 0xffffffffffffffff) {
        struct zfse_stat st;

        zfse_stat(fh->f, &st);
        fh->pos = st.size;
    } else {
        fh->pos = Position;
    }

    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_get_info(struct _EFI_FILE_HANDLE* File, EFI_GUID* InformationType, UINTN* BufferSize, VOID* Buffer) {
    file_handle* fh = CONTAINER(File, file_handle, proto);
    EFI_GUID guid = EFI_FILE_INFO_ID;


    if (!memcmp(InformationType, &guid, sizeof(EFI_GUID))) {
        struct zfse_stat st;
        UINTN size = file_info_size(fh->name);

        if (*BufferSize < size) {
            *BufferSize = size;
            return EFI_BUFFER_TOO_SMALL;
        }

        zfse_stat(fh->f, &st);
        fill_file_info((EFI_FILE_INFO*)Buffer, fh->name, st.size, st.is_dir ? true : false);
        *BufferSize = size;

        return EFI_SUCCESS;
    }

    guid = (EFI_GUID)EFI_FILE_SYSTEM_INFO_ID;

    if (!memcmp(InformationType, &guid, sizeof(EFI_GUID))) {
        static const CHAR16 label[] = L"ZFS";
        UINTN size = offsetof(EFI_FILE_SYSTEM_INFO, VolumeLabel) + sizeof(label);
        EFI_FILE_SYSTEM_INFO* info = Buffer;

        if (*BufferSize < size) {
            *BufferSize = size;
            return EFI_BUFFER_TOO_SMALL;
        }

        memset(info, 0, size);
        info->Size = size;
        info->ReadOnly = TRUE;
        info->BlockSize = 4096;
        memcpy(info->VolumeLabel, label, sizeof(label));
        *BufferSize = size;

        return EFI_SUCCESS;
    }

    guid = (EFI_GUID)EFI_FILE_SYSTEM_VOLUME_LABEL_ID;

    if (!memcmp(InformationType, &guid, sizeof(EFI_GUID))) {
        static const CHAR16 label[] = L"ZFS";

        if (*BufferSize < sizeof(label)) {
            *BufferSize = sizeof(label);
            return EFI_BUFFER_TOO_SMALL;
        }

        memcpy(Buffer, label, sizeof(label));
        *BufferSize = sizeof(label);

        return EFI_SUCCESS;
    }

    trace("GetInfo: unknown GUID %08x\n", *(unsigned*)InformationType);

    return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI file_set_info(struct _EFI_FILE_HANDLE* File, EFI_GUID* InformationType, UINTN BufferSize, VOID* Buffer) {
    UNUSED(File);
    UNUSED(InformationType);
    UNUSED(BufferSize);
    UNUSED(Buffer);

    return EFI_WRITE_PROTECTED;
}

static EFI_STATUS EFIAPI file_flush(struct _EFI_FILE_HANDLE* File) {
    UNUSED(File);

    return EFI_WRITE_PROTECTED;
}

/* the volume */

static EFI_STATUS EFIAPI open_volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* This, EFI_FILE_PROTOCOL** Root) {
    volume* vol = CONTAINER(This, volume, proto);
    file_handle* fh;
    EFI_STATUS Status;

    if (!vol->mnt) {
        int rc;

        tracep("open_volume: mounting\n");
        rc = zfse_mount(vol->pool, &vol->mnt);
        tracep("mount rc %d\n", rc);

        if (rc != 0) {
            zfs_print("Could not mount the ZFS root file system.\n");
            return errno_to_status(rc);
        }
    }

    Status = new_handle(vol, "/", &fh);
    if (EFI_ERROR(Status))
        return Status;

    *Root = &fh->proto;

    return EFI_SUCCESS;
}

/*
 * We leave the ARC name alone, so that Quibble uses the one it was given in
 * freeldr.ini, which for a pool on a partition is the ordinary
 * multi(0)disk(0)rdisk(n)partition(m) that Windows makes for itself.
 */
static EFI_STATUS EFIAPI get_arc_name(EFI_QUIBBLE_PROTOCOL* This, char* ArcName, UINTN* ArcNameLen) {
    UNUSED(This);
    UNUSED(ArcName);
    UNUSED(ArcNameLen);

    return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI get_driver_name(EFI_QUIBBLE_PROTOCOL* This, CHAR16* DriverName, UINTN* DriverNameLen) {
    static const CHAR16 name[] = L"OpenZFS";

    UNUSED(This);

    if (*DriverNameLen < sizeof(name)) {
        *DriverNameLen = sizeof(name);
        return EFI_BUFFER_TOO_SMALL;
    }

    *DriverNameLen = sizeof(name);

    memcpy(DriverName, name, sizeof(name));

    return EFI_SUCCESS;
}

/* driver binding */

static EFI_STATUS EFIAPI drv_supported(EFI_DRIVER_BINDING_PROTOCOL* This, EFI_HANDLE ControllerHandle,
                                       EFI_DEVICE_PATH_PROTOCOL* RemainingDevicePath) {
    EFI_STATUS Status;
    EFI_DISK_IO_PROTOCOL* disk_io;
    EFI_GUID guid_disk = EFI_DISK_IO_PROTOCOL_GUID;
    EFI_GUID guid_block = EFI_BLOCK_IO_PROTOCOL_GUID;

    UNUSED(RemainingDevicePath);

    Status = bs->OpenProtocol(ControllerHandle, &guid_disk, (void**)&disk_io, This->DriverBindingHandle,
                              ControllerHandle, EFI_OPEN_PROTOCOL_BY_DRIVER);

    if (EFI_ERROR(Status))
        return Status;

    bs->CloseProtocol(ControllerHandle, &guid_disk, This->DriverBindingHandle, ControllerHandle);

    return bs->OpenProtocol(ControllerHandle, &guid_block, NULL, This->DriverBindingHandle,
                            ControllerHandle, EFI_OPEN_PROTOCOL_TEST_PROTOCOL);
}

static void close_protocols(EFI_DRIVER_BINDING_PROTOCOL* This, EFI_HANDLE ControllerHandle, bool disk) {
    EFI_GUID disk_guid = EFI_DISK_IO_PROTOCOL_GUID;
    EFI_GUID block_guid = EFI_BLOCK_IO_PROTOCOL_GUID;

    bs->CloseProtocol(ControllerHandle, &block_guid, This->DriverBindingHandle, ControllerHandle);

    if (disk)
        bs->CloseProtocol(ControllerHandle, &disk_guid, This->DriverBindingHandle, ControllerHandle);
}

static EFI_STATUS EFIAPI drv_start(EFI_DRIVER_BINDING_PROTOCOL* This, EFI_HANDLE ControllerHandle,
                                   EFI_DEVICE_PATH_PROTOCOL* RemainingDevicePath) {
    EFI_STATUS Status;
    EFI_GUID disk_guid = EFI_DISK_IO_PROTOCOL_GUID;
    EFI_GUID block_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_GUID quibble_guid = EFI_QUIBBLE_PROTOCOL_GUID;
    EFI_BLOCK_IO_PROTOCOL* block;
    EFI_DISK_IO_PROTOCOL* disk_io;
    volume* vol;
    uint64_t size;
    int rc;

    UNUSED(RemainingDevicePath);

    for (vol = volumes; vol; vol = vol->next) {
        if (vol->controller == ControllerHandle) // already set up
            return EFI_SUCCESS;
    }

    Status = bs->OpenProtocol(ControllerHandle, &block_guid, (void**)&block, This->DriverBindingHandle,
                              ControllerHandle, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (EFI_ERROR(Status))
        return Status;

    if (block->Media->BlockSize == 0 || !block->Media->MediaPresent) {
        close_protocols(This, ControllerHandle, false);
        return EFI_UNSUPPORTED;
    }

    Status = bs->OpenProtocol(ControllerHandle, &disk_guid, (void**)&disk_io, This->DriverBindingHandle,
                              ControllerHandle, EFI_OPEN_PROTOCOL_BY_DRIVER);
    if (EFI_ERROR(Status)) {
        close_protocols(This, ControllerHandle, false);
        return Status;
    }

    Status = bs->AllocatePool(EfiBootServicesData, sizeof(volume), (void**)&vol);
    if (EFI_ERROR(Status)) {
        print_error("AllocatePool", Status);
        close_protocols(This, ControllerHandle, true);
        return Status;
    }

    memset(vol, 0, sizeof(volume));

    vol->controller = ControllerHandle;
    vol->block = block;
    vol->disk_io = disk_io;

    size = (block->Media->LastBlock + 1) * block->Media->BlockSize;

    tracep("start %p size %llu\n",ControllerHandle, (unsigned long long)size);
    // is there a ZFS pool here?
    rc = zfse_probe(vol_read, size, vol, &vol->pool);
    tracep("probe rc %d\n",rc);
    if (rc != 0) {
        bs->FreePool(vol);
        close_protocols(This, ControllerHandle, true);
        return EFI_UNSUPPORTED;
    }

    vol->proto.Revision = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION;
    vol->proto.OpenVolume = open_volume;

    vol->quibble_proto.GetArcName = get_arc_name;
    vol->quibble_proto.GetWindowsDriverName = get_driver_name;

    Status = bs->InstallMultipleProtocolInterfaces(&ControllerHandle, &fs_guid, &vol->proto,
                                                   &quibble_guid, &vol->quibble_proto, NULL);
    if (EFI_ERROR(Status)) {
        print_error("InstallMultipleProtocolInterfaces", Status);
        bs->FreePool(vol);
        close_protocols(This, ControllerHandle, true);
        return Status;
    }

    vol->next = volumes;
    volumes = vol;

    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI drv_stop(EFI_DRIVER_BINDING_PROTOCOL* This, EFI_HANDLE ControllerHandle,
                                  UINTN NumberOfChildren, EFI_HANDLE* ChildHandleBuffer) {
    // FIXME - free anything that needs freeing

    close_protocols(This, ControllerHandle, true);

    UNUSED(NumberOfChildren);
    UNUSED(ChildHandleBuffer);

    return EFI_SUCCESS;
}

static void get_info_protocol(EFI_HANDLE image_handle) {
    EFI_GUID guid = EFI_QUIBBLE_INFO_PROTOCOL_GUID;
    EFI_HANDLE* handles = NULL;
    UINTN count;
    EFI_STATUS Status;

    Status = bs->LocateHandleBuffer(ByProtocol, &guid, NULL, &count, &handles);
    if (EFI_ERROR(Status))
        return;

    if (count == 0) {
        bs->FreePool(handles);
        return;
    }

    for (unsigned int i = 0; i < count; i++) {
        Status = bs->OpenProtocol(handles[i], &guid, (void**)&info_proto, image_handle, NULL,
                                  EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
        if (EFI_ERROR(Status))
            continue;

        break;
    }

    bs->FreePool(handles);
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE* SystemTable) {
    EFI_STATUS Status;
    EFI_GUID guid = EFI_DRIVER_BINDING_PROTOCOL_GUID;
    int rc;

    systable = SystemTable;
    bs = SystemTable->BootServices;

    tracep("zfs: efi_main\n");
    get_info_protocol(ImageHandle);

    rc = zfse_init();
    tracep("zfs: init rc %d\n",rc);
    if (rc != 0) {
        zfs_print("Could not initialize the ZFS reader.\n");
        return EFI_OUT_OF_RESOURCES;
    }

    drvbind.Supported = drv_supported;
    drvbind.Start = drv_start;
    drvbind.Stop = drv_stop;
    drvbind.Version = 0x10;
    drvbind.ImageHandle = ImageHandle;
    drvbind.DriverBindingHandle = ImageHandle;

    Status = bs->InstallProtocolInterface(&drvbind.DriverBindingHandle, &guid,
                                          EFI_NATIVE_INTERFACE, &drvbind);
    if (EFI_ERROR(Status)) {
        print_error("InstallProtocolInterface", Status);
        return Status;
    }

    return EFI_SUCCESS;
}
