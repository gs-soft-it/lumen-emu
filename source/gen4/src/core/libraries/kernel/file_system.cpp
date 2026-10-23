// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <map>
#include <limits>
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
#include "core/fex/guest_memory_scope.h"
#include "core/guest_cpu/hle_call_adapter.h"
#endif
#include <ranges>
#include <magic_enum/magic_enum.hpp>

#include "common/assert.h"
#include "common/error.h"
#include "common/logging/log.h"
#include "common/scope_exit.h"
#include "common/singleton.h"
#include "core/file_sys/devices/console_device.h"
#include "core/file_sys/devices/deci_tty6_device.h"
#include "core/file_sys/devices/logger.h"
#include "core/file_sys/devices/nop_device.h"
#include "core/file_sys/devices/random_device.h"
#include "core/file_sys/devices/rng_device.h"
#include "core/file_sys/devices/srandom_device.h"
#include "core/file_sys/devices/urandom_device.h"
#include "core/file_sys/directories/normal_directory.h"
#include "core/file_sys/directories/pfs_directory.h"
#include "core/file_sys/fs.h"
#include "core/libraries/kernel/file_system.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/libs.h"
#include "core/libraries/network/sockets.h"
#include "core/memory.h"
#include "kernel.h"

#ifdef _WIN32
#include <io.h>
#include <winsock2.h>
#include <sys/stat.h>
#include <sys/utime.h>
#else
#include <fcntl.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/stat.h>
#endif

namespace D = Core::Devices;
namespace fs = std::filesystem;
using FactoryDevice = std::function<std::shared_ptr<D::BaseDevice>(u32, const char*, int, u16)>;

#define GET_DEVICE_FD(fd)                                                                          \
    [](u32, const char*, int, u16) {                                                               \
        return Common::Singleton<Core::FileSys::HandleTable>::Instance()->GetFile(fd)->device;     \
    }

// prefix path, only dev devices
static std::map<std::string, FactoryDevice> available_device = {
    // clang-format off
    {"/dev/stdin", GET_DEVICE_FD(0)},
    {"/dev/stdout", GET_DEVICE_FD(1)},
    {"/dev/stderr", GET_DEVICE_FD(2)},

    {"/dev/fd/0", GET_DEVICE_FD(0)},
    {"/dev/fd/1", GET_DEVICE_FD(1)},
    {"/dev/fd/2", GET_DEVICE_FD(2)},

    {"/dev/deci_stdin", GET_DEVICE_FD(0)},
    {"/dev/deci_stdout", GET_DEVICE_FD(1)},
    {"/dev/deci_stderr", GET_DEVICE_FD(2)},

    {"/dev/null", GET_DEVICE_FD(0)}, // fd0 (stdin) is a nop device

    {"/dev/urandom",  &D::URandomDevice::Create },
    {"/dev/random",   &D::RandomDevice::Create },
    {"/dev/srandom",  &D::SRandomDevice::Create },
    {"/dev/console",  &D::ConsoleDevice::Create },
    {"/dev/deci_tty6",&D::DeciTty6Device::Create },
    {"/dev/rng",      &D::RngDevice::Create },
    // clang-format on
};

namespace Libraries::Kernel {

s32 PS4_SYSV_ABI open(const char* raw_path, s32 flags, u16 mode) {
    LOG_INFO(Kernel_Fs, "path = {} flags = {:#x} mode = {:#o}", raw_path, flags, mode);

    auto* h = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();

    bool read = (flags & 0x3) == ORBIS_KERNEL_O_RDONLY;
    bool write = (flags & 0x3) == ORBIS_KERNEL_O_WRONLY;
    bool rdwr = (flags & 0x3) == ORBIS_KERNEL_O_RDWR;

    if (!read && !write && !rdwr) {
        // Start by checking for invalid flags.
        *__Error() = POSIX_EINVAL;
        LOG_ERROR(Kernel_Fs, "Opening path {} failed, invalid flags {:#x}", raw_path, flags);
        return -1;
    }

    if (strlen(raw_path) > 255) {
        *__Error() = POSIX_ENAMETOOLONG;
        LOG_ERROR(Kernel_Fs, "Opening path {} failed, path is too long", raw_path);
        return -1;
    }

    bool nonblock = (flags & ORBIS_KERNEL_O_NONBLOCK) != 0;
    bool append = (flags & ORBIS_KERNEL_O_APPEND) != 0;
    // Flags fsync and sync behave the same
    bool sync = (flags & ORBIS_KERNEL_O_SYNC) != 0 || (flags & ORBIS_KERNEL_O_FSYNC) != 0;
    bool create = (flags & ORBIS_KERNEL_O_CREAT) != 0;
    bool truncate = (flags & ORBIS_KERNEL_O_TRUNC) != 0;
    bool excl = (flags & ORBIS_KERNEL_O_EXCL) != 0;
    bool dsync = (flags & ORBIS_KERNEL_O_DSYNC) != 0;
    bool direct = (flags & ORBIS_KERNEL_O_DIRECT) != 0;
    bool directory = (flags & ORBIS_KERNEL_O_DIRECTORY) != 0;

    if (sync || direct || dsync || nonblock) {
        LOG_WARNING(Kernel_Fs, "flags {:#x} not fully handled", flags);
    }

    std::string_view path{raw_path};
    u32 handle = h->CreateHandle();
    auto* file = h->GetFile(handle);
    file->status_flags = flags & ~(ORBIS_KERNEL_O_CREAT | ORBIS_KERNEL_O_TRUNC | ORBIS_KERNEL_O_EXCL);

    if (path.starts_with("/dev/")) {
        for (const auto& [prefix, factory] : available_device) {
            if (path.starts_with(prefix)) {
                file->is_opened = true;
                file->type = Core::FileSys::FileType::Device;
                file->m_guest_name = path;
                file->device = factory(handle, path.data(), flags, mode);
                return handle;
            }
        }
    }

    bool read_only = false;
    file->m_guest_name = path;
    file->m_host_name = mnt->GetHostPath(file->m_guest_name, &read_only);
    bool exists = fs::exists(file->m_host_name);
    s32 e = 0;

    if (create) {
        if (excl && exists) {
            // Error if file exists
            h->DeleteHandle(handle);
            *__Error() = POSIX_EEXIST;
            LOG_ERROR(Kernel_Fs, "Creating {} failed, file already exists", raw_path);
            return -1;
        }

        if (!exists) {
            if (read_only) {
                // Can't create files in a read only directory
                h->DeleteHandle(handle);
                *__Error() = POSIX_EROFS;
                LOG_ERROR(Kernel_Fs, "Creating {} failed, path is read-only", raw_path);
                return -1;
            }
            // Create a file if it doesn't exist
            Common::FS::IOFile out(file->m_host_name, Common::FS::FileAccessMode::Create);
        }
    } else if (!exists) {
        // If we're not creating a file, and it doesn't exist, return ENOENT
        h->DeleteHandle(handle);
        *__Error() = POSIX_ENOENT;
        LOG_ERROR(Kernel_Fs, "Opening path {} failed, file does not exist", raw_path);
        return -1;
    }

    if (fs::is_directory(file->m_host_name) || directory) {
        // Directories can be opened even if the directory flag isn't set.
        // In these cases, error behavior is identical to the directory code path.
        directory = true;
    }

    if (directory) {
        if (!fs::is_directory(file->m_host_name)) {
            // If the opened file is not a directory, return ENOTDIR.
            // This will trigger when create & directory is specified, this is expected.
            h->DeleteHandle(handle);
            *__Error() = POSIX_ENOTDIR;
            LOG_ERROR(Kernel_Fs, "Opening directory {} failed, file is not a directory", raw_path);
            return -1;
        }

        if (write || rdwr) {
            // Cannot open directories with any type of write access
            h->DeleteHandle(handle);
            *__Error() = POSIX_EISDIR;
            LOG_ERROR(Kernel_Fs, "Opening directory {} failed, cannot open directories for writing",
                      raw_path);
            return -1;
        }

        if (truncate) {
            // Cannot open directories with truncate
            h->DeleteHandle(handle);
            *__Error() = POSIX_EISDIR;
            LOG_ERROR(Kernel_Fs, "Opening directory {} failed, cannot truncate directories",
                      raw_path);
            return -1;
        }

        file->type = Core::FileSys::FileType::Directory;
        file->is_opened = true;
        if (file->m_guest_name.starts_with("/app0")) {
            // TODO: Properly identify type for paths like "/app0/.."
            file->directory = Core::Directories::PfsDirectory::Create(file->m_guest_name);
        } else {
            file->directory = Core::Directories::NormalDirectory::Create(file->m_guest_name);
        }
    } else {
        file->type = Core::FileSys::FileType::Regular;

        if (truncate && read_only) {
            // Can't open files with truncate flag in a read only directory
            h->DeleteHandle(handle);
            *__Error() = POSIX_EROFS;
            LOG_ERROR(Kernel_Fs, "Truncating {} failed, path is read-only", raw_path);
            return -1;
        } else if (truncate) {
            // Open the file as read-write so we can truncate regardless of flags.
            // Since open starts by closing the file, this won't interfere with later open calls.
            e = file->f.Open(file->m_host_name, Common::FS::FileAccessMode::ReadWrite);
            if (e == 0) {
                // If the file was opened successfully, reduce size to 0
                file->f.SetSize(0);
            }
        }

        if (read) {
            // Open exclusively for reading
            e = file->f.Open(file->m_host_name, Common::FS::FileAccessMode::Read);
        } else if (read_only) {
            // Can't open files with write/read-write access in a read only directory
            h->DeleteHandle(handle);
            *__Error() = POSIX_EROFS;
            LOG_ERROR(Kernel_Fs, "Opening {} for writing failed, path is read-only", raw_path);
            return -1;
        } else if (write) {
            if (append) {
                // Open exclusively for appending
                e = file->f.Open(file->m_host_name, Common::FS::FileAccessMode::Append);
            } else {
                // Open exclusively for writing
                e = file->f.Open(file->m_host_name, Common::FS::FileAccessMode::Write);
            }
        } else if (rdwr) {
            // Read and write
            if (append) {
                // Open for reading and appending
                e = file->f.Open(file->m_host_name, Common::FS::FileAccessMode::ReadAppend);
            } else {
                // Open for reading and writing
                e = file->f.Open(file->m_host_name, Common::FS::FileAccessMode::ReadWrite);
            }
        }
    }

    if (e != 0) {
        // Open failed in platform-specific code, errno needs to be converted.
        h->DeleteHandle(handle);
        SetPosixErrno(e);
        LOG_ERROR(Kernel_Fs, "Opening {} failed, error = {}", raw_path, *__Error());
        return -1;
    }

    file->is_opened = true;
    return handle;
}

s32 PS4_SYSV_ABI posix_open(const char* filename, s32 flags, u16 mode) {
    return open(filename, flags, mode);
}

s32 PS4_SYSV_ABI sceKernelOpen(const char* path, s32 flags, /* SceKernelMode*/ u16 mode) {
    s32 result = open(path, flags, mode);
    if (result < 0) {
        return ErrnoToSceKernelError(*__Error());
    }
    return result;
}

s32 PS4_SYSV_ABI close(s32 fd) {
    auto* h = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    auto* file = h->GetFile(fd);
    if (file == nullptr) {
        *__Error() = POSIX_EBADF;
        return -1;
    }
    if (fd < 3) {
        *__Error() = POSIX_EPERM;
        return -1;
    }
    if (file->type == Core::FileSys::FileType::Regular) {
        file->f.Close();
    } else if (file->type == Core::FileSys::FileType::Socket) {
        file->socket->Close();
    }
    file->is_opened = false;
    LOG_INFO(Kernel_Fs, "Closing {}", file->m_guest_name);
    // FIXME: Lock file mutex before deleting it?
    h->DeleteHandle(fd);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI posix_close(s32 fd) {
    return close(fd);
}

s32 PS4_SYSV_ABI sceKernelClose(s32 fd) {
    s32 result = close(fd);
    if (result < 0) {
        LOG_ERROR(Kernel_Fs, "error = {}", *__Error());
        return ErrnoToSceKernelError(*__Error());
    }
    return result;
}

s64 PS4_SYSV_ABI write(s32 fd, const void* buf, u64 nbytes) {
    auto* h = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    auto* file = h->GetFile(fd);
    if (file == nullptr) {
        *__Error() = POSIX_EBADF;
        return -1;
    }

    std::scoped_lock lk{file->m_mutex};
    if (file->type == Core::FileSys::FileType::Device) {
        s64 result = file->device->write(buf, nbytes);
        if (result < 0) {
            ErrSceToPosix(result);
            return -1;
        }
        return result;
    } else if (file->type == Core::FileSys::FileType::Socket) {
        // Socket functions handle errnos internally.
        return file->socket->SendPacket(buf, nbytes, 0, nullptr, 0);
    } else if (file->type == Core::FileSys::FileType::Directory) {
        *__Error() = POSIX_EBADF;
        return -1;
    }

    return file->f.WriteRaw<u8>(buf, nbytes);
}

s64 PS4_SYSV_ABI posix_write(s32 fd, const void* buf, u64 nbytes) {
    return write(fd, buf, nbytes);
}

s64 PS4_SYSV_ABI sceKernelWrite(s32 fd, const void* buf, u64 nbytes) {
    s64 result = write(fd, buf, nbytes);
    if (result < 0) {
        LOG_ERROR(Kernel_Fs, "error = {}", *__Error());
        return ErrnoToSceKernelError(*__Error());
    }
    return result;
}

s64 ReadFile(Common::FS::IOFile& file, void* buf, u64 nbytes) {
    const auto* memory = Core::Memory::Instance();
    // Invalidate up to the actual number of bytes that could be read.
    const auto remaining = file.GetSize() - file.Tell();
    memory->InvalidateMemory(reinterpret_cast<VAddr>(buf), std::min<u64>(nbytes, remaining));

    return file.ReadRaw<u8>(buf, nbytes);
}

s64 PS4_SYSV_ABI readv(s32 fd, const OrbisKernelIovec* iov, s32 iovcnt) {
    auto* h = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    auto* file = h->GetFile(fd);
    if (file == nullptr) {
        *__Error() = POSIX_EBADF;
        return -1;
    }

    std::scoped_lock lk{file->m_mutex};
    if (file->type == Core::FileSys::FileType::Device) {
        s64 result = file->device->readv(iov, iovcnt);
        if (result < 0) {
            ErrSceToPosix(result);
            return -1;
        }
        return result;
    } else if (file->type == Core::FileSys::FileType::Directory) {
        s64 result = file->directory->readv(iov, iovcnt);
        if (result < 0) {
            ErrSceToPosix(result);
            return -1;
        }
        return result;
    }

    if (file->f.IsWriteOnly()) {
        *__Error() = POSIX_EBADF;
        return -1;
    }

    s64 total_read = 0;
    for (s32 i = 0; i < iovcnt; i++) {
        total_read += ReadFile(file->f, iov[i].iov_base, iov[i].iov_len);
    }
    return total_read;
}

s64 PS4_SYSV_ABI posix_readv(s32 fd, const OrbisKernelIovec* iov, s32 iovcnt) {
    return readv(fd, iov, iovcnt);
}

s64 PS4_SYSV_ABI sceKernelReadv(s32 fd, const OrbisKernelIovec* iov, s32 iovcnt) {
    s64 result = readv(fd, iov, iovcnt);
    if (result < 0) {
        LOG_ERROR(Kernel_Fs, "error = {}", *__Error());
        return ErrnoToSceKernelError(*__Error());
    }
    return result;
}

s64 PS4_SYSV_ABI writev(s32 fd, const OrbisKernelIovec* iov, s32 iovcnt) {
    auto* h = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    auto* file = h->GetFile(fd);
    if (file == nullptr) {
        *__Error() = POSIX_EBADF;
        return -1;
    }

    std::scoped_lock lk{file->m_mutex};

    if (file->type == Core::FileSys::FileType::Device) {
        s64 result = file->device->writev(iov, iovcnt);
        if (result < 0) {
            ErrSceToPosix(result);
            return -1;
        }
        return result;
    } else if (file->type == Core::FileSys::FileType::Directory) {
        *__Error() = POSIX_EBADF;
        return -1;
    }

    s64 total_written = 0;
    for (s32 i = 0; i < iovcnt; i++) {
        total_written += file->f.WriteRaw<u8>(iov[i].iov_base, iov[i].iov_len);
    }
    return total_written;
}

s64 PS4_SYSV_ABI posix_writev(s32 fd, const OrbisKernelIovec* iov, s32 iovcnt) {
    return writev(fd, iov, iovcnt);
}

s64 PS4_SYSV_ABI sceKernelWritev(s32 fd, const OrbisKernelIovec* iov, s32 iovcnt) {
    s64 result = writev(fd, iov, iovcnt);
    if (result < 0) {
        LOG_ERROR(Kernel_Fs, "error = {}", *__Error());
        return ErrnoToSceKernelError(*__Error());
    }
    return result;
}

s64 PS4_SYSV_ABI posix_lseek(s32 fd, s64 offset, s32 whence) {
    auto* h = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    auto* file = h->GetFile(fd);
    if (file == nullptr) {
        *__Error() = POSIX_EBADF;
        return -1;
    }

    std::scoped_lock lk{file->m_mutex};
    if (file->type == Core::FileSys::FileType::Device) {
        s64 result = file->device->lseek(offset, whence);
        if (result < 0) {
            ErrSceToPosix(result);
            return -1;
        }
        return result;
    } else if (file->type == Core::FileSys::FileType::Directory) {
        s64 result = file->directory->lseek(offset, whence);
        if (result < 0) {
            ErrSceToPosix(result);
            return -1;
        }
        return result;
    }

    Common::FS::SeekOrigin origin{};
    if (whence == 0) {
        origin = Common::FS::SeekOrigin::SetOrigin;
    } else if (whence == 1) {
        origin = Common::FS::SeekOrigin::CurrentPosition;
    } else if (whence == 2) {
        origin = Common::FS::SeekOrigin::End;
    } else if (whence == 3 || whence == 4) {
        // whence parameter belongs to an unsupported POSIX extension
        *__Error() = POSIX_ENOTTY;
        return -1;
    } else {
        // whence parameter is invalid
        *__Error() = POSIX_EINVAL;
        return -1;
    }

    if (!file->f.Seek(offset, origin)) {
        if (errno != 0) {
            // Seek failed in platform-specific code, errno needs to be converted.
            SetPosixErrno(errno);
            return -1;
        }
        // Shouldn't be possible, but just in case.
        return -1;
    }

    s64 result = file->f.Tell();
    if (result < 0) {
        // Tell failed in platform-specific code, errno needs to be converted.
        SetPosixErrno(errno);
        return -1;
    }
    return result;
}

s64 PS4_SYSV_ABI sceKernelLseek(s32 fd, s64 offset, s32 whence) {
    s64 result = posix_lseek(fd, offset, whence);
    if (result < 0) {
        LOG_ERROR(Kernel_Fs, "error = {}", *__Error());
        return ErrnoToSceKernelError(*__Error());
    }
    return result;
}

s64 PS4_SYSV_ABI read(s32 fd, void* buf, u64 nbytes) {
    auto* h = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    auto* file = h->GetFile(fd);
    if (file == nullptr) {
        *__Error() = POSIX_EBADF;
        return -1;
    }

    std::scoped_lock lk{file->m_mutex};
    if (file->type == Core::FileSys::FileType::Device) {
        s64 result = file->device->read(buf, nbytes);
        if (result < 0) {
            ErrSceToPosix(result);
            return -1;
        }
        return result;
    } else if (file->type == Core::FileSys::FileType::Directory) {
        s64 result = file->directory->read(buf, nbytes);
        if (result < 0) {
            ErrSceToPosix(result);
            return -1;
        }
        return result;
    } else if (file->type == Core::FileSys::FileType::Socket) {
        // Socket functions handle errnos internally.
        return file->socket->ReceivePacket(buf, nbytes, 0, nullptr, 0);
    }

    if (file->f.IsWriteOnly()) {
        *__Error() = POSIX_EBADF;
        return -1;
    }

    return ReadFile(file->f, buf, nbytes);
}

s64 PS4_SYSV_ABI posix_read(s32 fd, void* buf, u64 nbytes) {
    return read(fd, buf, nbytes);
}

s64 PS4_SYSV_ABI sceKernelRead(s32 fd, void* buf, u64 nbytes) {
    s64 result = read(fd, buf, nbytes);
    if (result < 0) {
        LOG_ERROR(Kernel_Fs, "error = {}", *__Error());
        return ErrnoToSceKernelError(*__Error());
    }
    return result;
}

s32 PS4_SYSV_ABI posix_mkdir(const char* path, u16 mode) {
    LOG_INFO(Kernel_Fs, "path = {} mode = {:#o}", path, mode);
    if (strlen(path) > 255) {
        *__Error() = POSIX_ENAMETOOLONG;
        return -1;
    }
    if (path == nullptr) {
        *__Error() = POSIX_ENOTDIR;
        return -1;
    }
    auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();

    bool ro = false;
    const auto dir_name = mnt->GetHostPath(path, &ro);

    if (fs::exists(dir_name)) {
        *__Error() = POSIX_EEXIST;
        return -1;
    }

    if (ro) {
        *__Error() = POSIX_EROFS;
        return -1;
    }

    // CUSA02456: path = /aotl after sceSaveDataMount(mode = 1)
    std::error_code ec;
    if (dir_name.empty() || !fs::create_directory(dir_name, ec)) {
        *__Error() = POSIX_EIO;
        return -1;
    }

    if (!fs::exists(dir_name)) {
        *__Error() = POSIX_ENOENT;
        return -1;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelMkdir(const char* path, u16 mode) {
    s32 result = posix_mkdir(path, mode);
    if (result < 0) {
        LOG_ERROR(Kernel_Fs, "error = {}", *__Error());
        return ErrnoToSceKernelError(*__Error());
    }
    return result;
}

s32 PS4_SYSV_ABI posix_rmdir(const char* path) {
    if (strlen(path) > 255) {
        *__Error() = POSIX_ENAMETOOLONG;
        return -1;
    }
    auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    bool ro = false;

    const fs::path dir_name = mnt->GetHostPath(path, &ro);

    if (ro) {
        *__Error() = POSIX_EROFS;
        return -1;
    }

    if (dir_name.empty() || !fs::is_directory(dir_name)) {
        *__Error() = POSIX_ENOTDIR;
        return -1;
    }

    if (!fs::exists(dir_name)) {
        *__Error() = POSIX_ENOENT;
        return -1;
    }

    std::error_code ec;
    s32 result = fs::remove_all(dir_name, ec);

    if (ec) {
        *__Error() = POSIX_EIO;
        return -1;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelRmdir(const char* path) {
    s32 result = posix_rmdir(path);
    if (result < 0) {
        LOG_ERROR(Kernel_Fs, "error = {}", *__Error());
        return ErrnoToSceKernelError(*__Error());
    }
    return result;
}

static bool SnapshotPath(const char* path, std::string& result);
static s32 ReadNativeStat(const fs::path& path, int descriptor, OrbisKernelStat* output) {
    if (!output) { *__Error() = POSIX_EFAULT; return -1; }
#ifdef _WIN32
    struct _stat64 native{};
    const auto rc = descriptor < 0 ? _wstat64(path.c_str(), &native) : _fstat64(descriptor, &native);
#else
    struct stat native{};
    const auto rc = descriptor < 0 ? ::stat(path.c_str(), &native) : ::fstat(descriptor, &native);
#endif
    if (rc < 0) { SetPosixErrno(errno); return -1; }
    OrbisKernelStat result{};
    result.st_dev = static_cast<u32>(native.st_dev);
    result.st_ino = static_cast<u32>(native.st_ino);
    result.st_mode = native.st_mode;
    result.st_nlink = native.st_nlink;
    result.st_size = native.st_size;
#ifdef _WIN32
    result.st_atim = {native.st_atime, 0};
    result.st_mtim = {native.st_mtime, 0};
    result.st_ctim = {native.st_ctime, 0};
    result.st_blksize = 512;
    result.st_blocks = (result.st_size + 511) / 512;
#elif defined(__APPLE__)
    result.st_atim = {native.st_atimespec.tv_sec, native.st_atimespec.tv_nsec};
    result.st_mtim = {native.st_mtimespec.tv_sec, native.st_mtimespec.tv_nsec};
    result.st_ctim = {native.st_ctimespec.tv_sec, native.st_ctimespec.tv_nsec};
    result.st_birthtim = {native.st_birthtimespec.tv_sec, native.st_birthtimespec.tv_nsec};
    result.st_blksize = native.st_blksize;
    result.st_blocks = native.st_blocks;
#else
    result.st_atim = {native.st_atim.tv_sec, native.st_atim.tv_nsec};
    result.st_mtim = {native.st_mtim.tv_sec, native.st_mtim.tv_nsec};
    result.st_ctim = {native.st_ctim.tv_sec, native.st_ctim.tv_nsec};
    result.st_blksize = native.st_blksize;
    result.st_blocks = native.st_blocks;
#endif
    *output = result;
    return 0;
}

static s32 PublishFileStat(OrbisKernelStat* output, const OrbisKernelStat& native) {
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    Core::Fex::MemoryMutationScope publication;
    if (!publication.Valid() || !Core::GuestCpu::ValidateHlePointer(output, sizeof(native), true)) {
        *__Error() = POSIX_EFAULT; return -1;
    }
#else
    if (!output) { *__Error() = POSIX_EFAULT; return -1; }
#endif
    *output = native;
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    publication.Publish(reinterpret_cast<u64>(output), sizeof(native));
#endif
    return 0;
}

s32 QueryFileMetadata(const char* native_path, OrbisKernelStat& result) {
    if (!native_path) return ORBIS_KERNEL_ERROR_EFAULT;
    const auto host = Common::Singleton<Core::FileSys::MntPoints>::Instance()->GetHostPath(native_path);
    return ReadNativeStat(host, -1, &result) < 0 ? ErrnoToSceKernelError(*__Error()) : 0;
}

s32 PS4_SYSV_ABI posix_stat(const char* path, OrbisKernelStat* sb) {
#ifdef LUMEN_GEN4_DIRECT_FEX
    LOG_DEBUG(Kernel_Fs, "(PARTIAL) path = {}", path);
    if (strlen(path) > 255) {
        *__Error() = POSIX_ENAMETOOLONG;
        return -1;
    }
    auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    const auto path_name = mnt->GetHostPath(path);
    std::memset(sb, 0, sizeof(OrbisKernelStat));
    return ReadNativeStat(path_name, -1, sb);
#else
    std::string guest;
    if (!SnapshotPath(path, guest)) return -1;
    OrbisKernelStat native{};
    const auto rc = QueryFileMetadata(guest.c_str(), native);
    if (rc < 0) { ErrSceToPosix(rc); return -1; }
    return PublishFileStat(sb, native);
#endif
}

s32 PS4_SYSV_ABI sceKernelStat(const char* path, OrbisKernelStat* sb) {
    s32 result = posix_stat(path, sb);
    if (result < 0) {
        LOG_ERROR(Kernel_Fs, "error = {}", *__Error());
        return ErrnoToSceKernelError(*__Error());
    }
    return result;
}

s32 PS4_SYSV_ABI sceKernelCheckReachability(const char* path) {
    if (strlen(path) > 255) {
        return ORBIS_KERNEL_ERROR_ENAMETOOLONG;
    }

    auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    std::string_view guest_path{path};
    for (const auto& prefix : available_device | std::views::keys) {
        if (guest_path.starts_with(prefix)) {
            return ORBIS_OK;
        }
    }
    const auto path_name = mnt->GetHostPath(guest_path);
    if (!fs::exists(path_name)) {
        return ORBIS_KERNEL_ERROR_ENOENT;
    }
    return ORBIS_OK;
}

static s32 NativeFstat(s32 fd, OrbisKernelStat* sb) {
    LOG_DEBUG(Kernel_Fs, "(PARTIAL) fd = {}", fd);
    if (sb == nullptr) {
        *__Error() = POSIX_EFAULT;
        return -1;
    }
    auto* h = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    auto* file = h->GetFile(fd);
    if (file == nullptr) {
        *__Error() = POSIX_EBADF;
        return -1;
    }

    switch (file->type) {
    case Core::FileSys::FileType::Device: {
        s32 result = file->device->fstat(sb);
        if (result < 0) {
            ErrSceToPosix(result);
            return -1;
        }
        return result;
    }
    case Core::FileSys::FileType::Regular:
        return ReadNativeStat(file->m_host_name, file->f.GetNativeDescriptor(), sb);
    case Core::FileSys::FileType::Directory:
        return ReadNativeStat(file->m_host_name, -1, sb);
    case Core::FileSys::FileType::Socket: {
        // Socket functions handle errnos internally
        return file->socket->fstat(sb);
    }
    case Core::FileSys::FileType::Epoll:
    case Core::FileSys::FileType::Resolver:
    case Core::FileSys::FileType::Equeue: {
        LOG_ERROR(Kernel_Fs, "(STUBBED) file type {}", magic_enum::enum_name(file->type.load()));
        break;
    }
    default:
        UNREACHABLE_MSG("{}", u32(file->type.load()));
    }
    return ORBIS_OK;
}

s32 QueryFileMetadata(s32 fd, OrbisKernelStat& result) {
    const auto rc = NativeFstat(fd, &result);
    return rc < 0 ? ErrnoToSceKernelError(*__Error()) : rc;
}
s32 PS4_SYSV_ABI fstat(s32 fd, OrbisKernelStat* sb) {
    OrbisKernelStat native{};
    if (NativeFstat(fd, &native) < 0) return -1;
    return PublishFileStat(sb, native);
}

s32 PS4_SYSV_ABI posix_fstat(s32 fd, OrbisKernelStat* sb) {
    return fstat(fd, sb);
}

s32 PS4_SYSV_ABI sceKernelFstat(s32 fd, OrbisKernelStat* sb) {
    s32 result = fstat(fd, sb);
    if (result < 0) {
        LOG_ERROR(Kernel_Fs, "error = {}", *__Error());
        return ErrnoToSceKernelError(*__Error());
    }
    return result;
}

s32 PS4_SYSV_ABI posix_ftruncate(s32 fd, s64 length) {
    auto* h = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    auto* file = h->GetFile(fd);

    if (file == nullptr) {
        *__Error() = POSIX_EBADF;
        return -1;
    }

    if (file->type == Core::FileSys::FileType::Device) {
        s32 result = file->device->ftruncate(length);
        if (result < 0) {
            ErrSceToPosix(result);
            return -1;
        }
        return result;
    }

    if (file->m_host_name.empty()) {
        *__Error() = POSIX_EACCES;
        return -1;
    }

    file->f.SetSize(length);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelFtruncate(s32 fd, s64 length) {
    s32 result = posix_ftruncate(fd, length);
    if (result < 0) {
        LOG_ERROR(Kernel_Fs, "error = {}", *__Error());
        return ErrnoToSceKernelError(*__Error());
    }
    return result;
}

s32 PS4_SYSV_ABI posix_rename(const char* from, const char* to) {
    auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    bool ro = false;
    const auto src_path = mnt->GetHostPath(from, &ro);
    if (strlen(from) > 255) {
        *__Error() = POSIX_ENAMETOOLONG;
        return -1;
    }
    if (strlen(to) > 255) {
        *__Error() = POSIX_ENAMETOOLONG;
        return -1;
    }
    if (!fs::exists(src_path)) {
        *__Error() = POSIX_ENOENT;
        return -1;
    }
    if (ro) {
        *__Error() = POSIX_EROFS;
        return -1;
    }
    const auto dst_path = mnt->GetHostPath(to, &ro);
    if (ro) {
        *__Error() = POSIX_EROFS;
        return -1;
    }
    const bool src_is_dir = fs::is_directory(src_path);
    const bool dst_is_dir = fs::is_directory(dst_path);

    if (fs::exists(dst_path)) {
        if (src_is_dir && !dst_is_dir) {
            *__Error() = POSIX_ENOTDIR;
            return -1;
        }
        if (!src_is_dir && dst_is_dir) {
            *__Error() = POSIX_EISDIR;
            return -1;
        }
        if (dst_is_dir && !fs::is_empty(dst_path)) {
            *__Error() = POSIX_ENOTEMPTY;
            return -1;
        }
    }

    // On Windows, fs::rename will error if the file has been opened before.
    fs::copy(src_path, dst_path,
             fs::copy_options::overwrite_existing | fs::copy_options::recursive);
    auto* h = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    auto file = h->GetFile(src_path);
    if (file) {
        auto access_mode = file->f.GetAccessMode();
        file->f.Close();
        fs::remove(src_path);
        file->f.Open(dst_path, access_mode);
    } else {
        fs::remove_all(src_path);
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelRename(const char* from, const char* to) {
    s32 result = posix_rename(from, to);
    if (result < 0) {
        LOG_ERROR(Kernel_Fs, "error = {}", *__Error());
        return ErrnoToSceKernelError(*__Error());
    }
    return result;
}

s64 PS4_SYSV_ABI posix_preadv(s32 fd, OrbisKernelIovec* iov, s32 iovcnt, s64 offset) {
    if (offset < 0) {
        *__Error() = POSIX_EINVAL;
        return -1;
    }

    auto* h = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    auto* file = h->GetFile(fd);
    if (file == nullptr) {
        *__Error() = POSIX_EBADF;
        return -1;
    }

    std::scoped_lock lk{file->m_mutex};
    if (file->type == Core::FileSys::FileType::Device) {
        s64 result = file->device->preadv(iov, iovcnt, offset);
        if (result < 0) {
            ErrSceToPosix(result);
            return -1;
        }
        return result;
    } else if (file->type == Core::FileSys::FileType::Directory) {
        s64 result = file->directory->preadv(iov, iovcnt, offset);
        if (result < 0) {
            ErrSceToPosix(result);
            return -1;
        }
        return result;
    }

    if (file->f.IsWriteOnly()) {
        *__Error() = POSIX_EBADF;
        return -1;
    }

    const s64 pos = file->f.Tell();
    SCOPE_EXIT {
        file->f.Seek(pos);
    };
    if (!file->f.Seek(offset)) {
        *__Error() = POSIX_EIO;
        return -1;
    }
    s64 total_read = 0;
    for (s32 i = 0; i < iovcnt; i++) {
        total_read += ReadFile(file->f, iov[i].iov_base, iov[i].iov_len);
    }
    return total_read;
}

s64 PS4_SYSV_ABI sceKernelPreadv(s32 fd, OrbisKernelIovec* iov, s32 iovcnt, s64 offset) {
    s64 result = posix_preadv(fd, iov, iovcnt, offset);
    if (result < 0) {
        LOG_ERROR(Kernel_Fs, "error = {}", *__Error());
        return ErrnoToSceKernelError(*__Error());
    }
    return result;
}

s64 PS4_SYSV_ABI posix_pread(s32 fd, void* buf, u64 nbytes, s64 offset) {
    OrbisKernelIovec iovec{buf, nbytes};
    return posix_preadv(fd, &iovec, 1, offset);
}

s64 PS4_SYSV_ABI sceKernelPread(s32 fd, void* buf, u64 nbytes, s64 offset) {
    OrbisKernelIovec iovec{buf, nbytes};
    return sceKernelPreadv(fd, &iovec, 1, offset);
}

s32 PS4_SYSV_ABI posix_fsync(s32 fd) {
    auto* h = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    auto* file = h->GetFile(fd);
    if (file == nullptr) {
        *__Error() = POSIX_EBADF;
        return -1;
    }

    if (file->type == Core::FileSys::FileType::Device) {
        s32 result = file->device->fsync();
        if (result < 0) {
            ErrSceToPosix(result);
            return -1;
        }
        return result;
    }
    file->f.Flush();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelFsync(s32 fd) {
    s32 result = posix_fsync(fd);
    if (result < 0) {
        LOG_ERROR(Kernel_Fs, "error = {}", *__Error());
        return ErrnoToSceKernelError(*__Error());
    }
    return result;
}

static s64 GetDents(s32 fd, char* buf, u64 nbytes, s64* basep) {
    if (buf == nullptr) {
        *__Error() = POSIX_EFAULT;
        return -1;
    }
    auto* h = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    auto* file = h->GetFile(fd);
    if (file == nullptr) {
        *__Error() = POSIX_EBADF;
        return -1;
    }

    if (nbytes < 512) {
        *__Error() = POSIX_EINVAL;
        return -1;
    }

    switch (file->type) {
    case Core::FileSys::FileType::Directory: {
        s64 result = file->directory->getdents(buf, nbytes, basep);
        if (result < 0) {
            ErrSceToPosix(result);
            return -1;
        }
        return result;
    }
    case Core::FileSys::FileType::Device: {
        s64 result = file->device->getdents(buf, nbytes, basep);
        if (result < 0) {
            ErrSceToPosix(result);
            return -1;
        }
        return result;
    }
    default: {
        // Not directory or device
        *__Error() = POSIX_EINVAL;
        return -1;
    }
    }

    return ORBIS_OK;
}

s64 PS4_SYSV_ABI posix_getdents(s32 fd, char* buf, u64 nbytes) {
    return GetDents(fd, buf, nbytes, nullptr);
}

s64 PS4_SYSV_ABI sceKernelGetdents(s32 fd, char* buf, u64 nbytes) {
    s64 result = posix_getdents(fd, buf, nbytes);
    if (result < 0) {
        LOG_ERROR(Kernel_Fs, "error = {}", *__Error());
        return ErrnoToSceKernelError(*__Error());
    }
    return result;
}

s64 PS4_SYSV_ABI getdirentries(s32 fd, char* buf, u64 nbytes, s64* basep) {
    return GetDents(fd, buf, nbytes, basep);
}

s64 PS4_SYSV_ABI posix_getdirentries(s32 fd, char* buf, u64 nbytes, s64* basep) {
    return GetDents(fd, buf, nbytes, basep);
}

s64 PS4_SYSV_ABI sceKernelGetdirentries(s32 fd, char* buf, u64 nbytes, s64* basep) {
    s64 result = GetDents(fd, buf, nbytes, basep);
    if (result < 0) {
        LOG_ERROR(Kernel_Fs, "error = {}", *__Error());
        return ErrnoToSceKernelError(*__Error());
    }
    return result;
}

s64 PS4_SYSV_ABI posix_pwritev(s32 fd, const OrbisKernelIovec* iov, s32 iovcnt, s64 offset) {
    if (offset < 0) {
        *__Error() = POSIX_EINVAL;
        return -1;
    }

    auto* h = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    auto* file = h->GetFile(fd);
    if (file == nullptr) {
        *__Error() = POSIX_EBADF;
        return -1;
    }

    std::scoped_lock lk{file->m_mutex};

    if (file->type == Core::FileSys::FileType::Device) {
        s64 result = file->device->pwritev(iov, iovcnt, offset);
        if (result < 0) {
            ErrSceToPosix(result);
            return -1;
        }
        return result;
    } else if (file->type == Core::FileSys::FileType::Directory) {
        *__Error() = POSIX_EBADF;
        return -1;
    }

    const s64 pos = file->f.Tell();
    SCOPE_EXIT {
        file->f.Seek(pos);
    };
    if (!file->f.Seek(offset)) {
        *__Error() = POSIX_EIO;
        return -1;
    }
    s64 total_written = 0;
    for (s32 i = 0; i < iovcnt; i++) {
        total_written += file->f.WriteRaw<u8>(iov[i].iov_base, iov[i].iov_len);
    }
    return total_written;
}

s64 PS4_SYSV_ABI posix_pwrite(s32 fd, void* buf, u64 nbytes, s64 offset) {
    OrbisKernelIovec iovec{buf, nbytes};
    return posix_pwritev(fd, &iovec, 1, offset);
}

s64 PS4_SYSV_ABI sceKernelPwrite(s32 fd, void* buf, u64 nbytes, s64 offset) {
    s64 result = posix_pwrite(fd, buf, nbytes, offset);
    if (result < 0) {
        LOG_ERROR(Kernel_Fs, "error = {}", *__Error());
        return ErrnoToSceKernelError(*__Error());
    }
    return result;
}

s64 PS4_SYSV_ABI sceKernelPwritev(s32 fd, const OrbisKernelIovec* iov, s32 iovcnt, s64 offset) {
    s64 result = posix_pwritev(fd, iov, iovcnt, offset);
    if (result < 0) {
        LOG_ERROR(Kernel_Fs, "error = {}", *__Error());
        return ErrnoToSceKernelError(*__Error());
    }
    return result;
}

s32 PS4_SYSV_ABI posix_unlink(const char* path) {
    if (strlen(path) > 255) {
        *__Error() = POSIX_ENAMETOOLONG;
        return -1;
    }
    if (path == nullptr) {
        *__Error() = POSIX_EINVAL;
        return -1;
    }

    auto* h = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();

    bool ro = false;
    const auto host_path = mnt->GetHostPath(path, &ro);
    if (host_path.empty()) {
        *__Error() = POSIX_ENOENT;
        return -1;
    }

    if (ro) {
        *__Error() = POSIX_EROFS;
        return -1;
    }

    if (fs::is_directory(host_path)) {
        *__Error() = POSIX_EPERM;
        return -1;
    }

    auto* file = h->GetFile(host_path);
    if (file == nullptr) {
        // File to unlink hasn't been opened, manually open and unlink it.
        Common::FS::IOFile file(host_path, Common::FS::FileAccessMode::ReadWrite);
        file.Unlink();
    } else {
        file->f.Unlink();
    }

    LOG_INFO(Kernel_Fs, "Unlinked {}", path);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelUnlink(const char* path) {
    s32 result = posix_unlink(path);
    if (result < 0) {
        LOG_ERROR(Kernel_Fs, "error = {}", *__Error());
        return ErrnoToSceKernelError(*__Error());
    }
    return result;
}

#ifdef _WIN32

typedef struct {
    u64 fds_bits[16];
} fd_set_posix;

static void FD_SET_POSIX(s32 fd, fd_set_posix* set) {
    set->fds_bits[fd / (8 * sizeof(u64))] |= (1ULL << (fd % (8 * sizeof(u64))));
}

static void FD_CLR_POSIX(s32 fd, fd_set_posix* set) {
    set->fds_bits[fd / (8 * sizeof(u64))] &= ~(1ULL << (fd % (8 * sizeof(u64))));
}

static bool FD_ISSET_POSIX(s32 fd, fd_set_posix* set) {
    return (set->fds_bits[fd / (8 * sizeof(u64))] & (1ULL << (fd % (8 * sizeof(u64))))) != 0;
}

static void FD_ZERO_POSIX(fd_set_posix* set) {
    std::memset(set, 0, sizeof(fd_set_posix));
}

s32 PS4_SYSV_ABI posix_select(s32 nfds, fd_set_posix* readfds, fd_set_posix* writefds,
                              fd_set_posix* exceptfds, OrbisKernelTimeval* timeout) {
    LOG_DEBUG(Kernel_Fs, "nfds = {}, readfds = {}, writefds = {}, exceptfds = {}, timeout = {}",
              nfds, fmt::ptr(readfds), fmt::ptr(writefds), fmt::ptr(exceptfds), fmt::ptr(timeout));

    auto* h = Common::Singleton<Core::FileSys::HandleTable>::Instance();

    fd_set read_host = {}, write_host = {}, except_host = {};
    FD_ZERO(&read_host);
    FD_ZERO(&write_host);
    FD_ZERO(&except_host);

    fd_set_posix read_ready, write_ready, except_ready;
    FD_ZERO_POSIX(&read_ready);
    FD_ZERO_POSIX(&write_ready);
    FD_ZERO_POSIX(&except_ready);

    std::map<s32, s32> host_to_guest;
    s32 socket_max_fd = -1;

    for (s32 i = 0; i < nfds; ++i) {
        bool want_read = readfds && FD_ISSET_POSIX(i, readfds);
        bool want_write = writefds && FD_ISSET_POSIX(i, writefds);
        bool want_except = exceptfds && FD_ISSET_POSIX(i, exceptfds);
        if (!(want_read || want_write || want_except)) {
            continue;
        }

        auto* file = h->GetFile(i);
        if (!file || ((file->type == Core::FileSys::FileType::Regular && !file->f.IsOpen()) ||
                      (file->type == Core::FileSys::FileType::Socket && !file->is_opened))) {
            LOG_ERROR(Kernel_Fs, "fd {} is null or not opened", i);
            *__Error() = POSIX_EBADF;
            return -1;
        }

        s32 native_fd = -1;
        switch (file->type) {
        case Core::FileSys::FileType::Regular:
            native_fd = static_cast<s32>(file->f.GetFileMapping());
            break;
        case Core::FileSys::FileType::Socket: {
            auto sock = file->socket->Native();
            native_fd = sock ? static_cast<s32>(*sock) : -1;
            break;
        }
        case Core::FileSys::FileType::Device:
            native_fd = -1;
            break;
        default:
            UNREACHABLE();
            break;
        }

        if (file->type == Core::FileSys::FileType::Regular ||
            file->type == Core::FileSys::FileType::Device) {
            // Disk files always ready
            // For devices, stdin (fd 0) is never read-ready.
            if (want_read && i != 0) {
                FD_SET_POSIX(i, &read_ready);
            }
            if (want_write) {
                FD_SET_POSIX(i, &write_ready);
            }
            // exceptfds not supported on regular files
        } else if (file->type == Core::FileSys::FileType::Socket) {
            if (want_read) {
                FD_SET(native_fd, &read_host);
            }
            if (want_write) {
                FD_SET(native_fd, &write_host);
            }
            if (want_except) {
                FD_SET(native_fd, &except_host);
            }
            socket_max_fd = std::max(socket_max_fd, native_fd);
        }

        if (native_fd == -1) {
            LOG_WARNING(Kernel_Fs, "Unsupported fd {}", i);
            continue;
        }

        host_to_guest[native_fd] = i;
    }

    LOG_DEBUG(Kernel_Fs,
              "Before select(): read_host.fd_count = {}, write_host.fd_count = {}, "
              "except_host.fd_count = {}",
              read_host.fd_count, write_host.fd_count, except_host.fd_count);

    if (read_host.fd_count == 0 && write_host.fd_count == 0 && except_host.fd_count == 0) {
        LOG_WARNING(Kernel_Fs, "No sockets in fd_sets, select() will return immediately");
    }

    if (readfds) {
        FD_ZERO_POSIX(readfds);
    }
    if (writefds) {
        FD_ZERO_POSIX(writefds);
    }
    if (exceptfds) {
        FD_ZERO_POSIX(exceptfds);
    }

    s32 result = 0;
    if (socket_max_fd != -1) {
        timeval tv = {};
        timeval* tv_ptr = nullptr;
        if (timeout) {
            tv.tv_sec = timeout->tv_sec;
            tv.tv_usec = timeout->tv_usec;
            tv_ptr = &tv;
        }
        result = select(0, read_host.fd_count > 0 ? &read_host : nullptr,
                        write_host.fd_count > 0 ? &write_host : nullptr,
                        except_host.fd_count > 0 ? &except_host : nullptr, tv_ptr);
        if (result == SOCKET_ERROR) {
            s32 err = WSAGetLastError();
            LOG_ERROR(Kernel_Fs, "select() failed with error {}", err);
            switch (err) {
            case WSAEFAULT:
                *__Error() = POSIX_EFAULT;
                break;
            case WSAEINVAL:
                *__Error() = POSIX_EINVAL;
                break;
            case WSAENOBUFS:
                *__Error() = POSIX_ENOBUFS;
                break;
            default:
                LOG_ERROR(Kernel_Fs, "Unhandled error case {}", err);
                break;
            }
            return -1;
        }

        for (s32 i = 0; i < read_host.fd_count; ++i) {
            s32 fd = static_cast<s32>(read_host.fd_array[i]);
            FD_SET_POSIX(host_to_guest[fd], readfds);
        }
        for (s32 i = 0; i < write_host.fd_count; ++i) {
            s32 fd = static_cast<s32>(write_host.fd_array[i]);
            FD_SET_POSIX(host_to_guest[fd], writefds);
        }
        for (s32 i = 0; i < except_host.fd_count; ++i) {
            s32 fd = static_cast<s32>(except_host.fd_array[i]);
            FD_SET_POSIX(host_to_guest[fd], exceptfds);
        }
    }

    // Add regular/device files ready count
    s32 disk_ready = 0;
    for (s32 i = 0; i < nfds; ++i) {
        if (FD_ISSET_POSIX(i, &read_ready)) {
            FD_SET_POSIX(i, readfds);
            disk_ready++;
        }
        if (FD_ISSET_POSIX(i, &write_ready)) {
            FD_SET_POSIX(i, writefds);
            disk_ready++;
        }
    }

    return result + disk_ready;
}
#else
s32 PS4_SYSV_ABI posix_select(s32 nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds,
                              OrbisKernelTimeval* timeout) {
    LOG_DEBUG(Kernel_Fs, "nfds = {}, readfds = {}, writefds = {}, exceptfds = {}, timeout = {}",
              nfds, fmt::ptr(readfds), fmt::ptr(writefds), fmt::ptr(exceptfds), fmt::ptr(timeout));

    auto* h = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    fd_set read_host, write_host, except_host;
    FD_ZERO(&read_host);
    FD_ZERO(&write_host);
    FD_ZERO(&except_host);

    std::map<s32, s32> host_to_guest;
    s32 max_fd = -1;

    for (s32 i = 0; i < nfds; ++i) {
        auto read = readfds && FD_ISSET(i, readfds);
        auto write = writefds && FD_ISSET(i, writefds);
        auto except = exceptfds && FD_ISSET(i, exceptfds);
        if (read || write || except) {
            auto* file = h->GetFile(i);
            if (file == nullptr ||
                ((file->type == Core::FileSys::FileType::Regular && !file->f.IsOpen()) ||
                 (file->type == Core::FileSys::FileType::Socket && !file->is_opened))) {
                LOG_ERROR(Kernel_Fs, "fd {} is null or not opened", i);
                *__Error() = POSIX_EBADF;
                return -1;
            }

            s32 native_fd = [&] {
                switch (file->type) {
                case Core::FileSys::FileType::Regular:
                    return static_cast<s32>(file->f.GetFileMapping());
                case Core::FileSys::FileType::Device:
                    return -1;
                case Core::FileSys::FileType::Socket: {
                    auto sock = file->socket->Native();
                    // until P2P sockets contain a proper socket
                    return sock ? static_cast<s32>(*sock) : -1;
                }
                default:
                    UNREACHABLE();
                }
            }();
            if (native_fd == -1) {
                LOG_WARNING(Kernel_Fs, "Unsupported fd {}", i);
                continue;
            }
            host_to_guest.emplace(native_fd, i);

            max_fd = std::max(max_fd, native_fd);

            if (read) {
                FD_SET(native_fd, &read_host);
            }
            if (write) {
                FD_SET(native_fd, &write_host);
            }
            if (except) {
                FD_SET(native_fd, &except_host);
            }
        }
    }

    if (max_fd == -1) {
        LOG_WARNING(Kernel_Fs, "all requested file descriptors are unsupported");
        return 0;
    }

    s32 ret = select(max_fd + 1, &read_host, &write_host, &except_host, (timeval*)timeout);

    if (ret > 0) {
        if (readfds) {
            FD_ZERO(readfds);
        }
        if (writefds) {
            FD_ZERO(writefds);
        }
        if (exceptfds) {
            FD_ZERO(exceptfds);
        }

        for (s32 i = 0; i < max_fd + 1; ++i) {
            if (readfds && FD_ISSET(i, &read_host)) {
                FD_SET(host_to_guest[i], readfds);
            }
            if (writefds && FD_ISSET(i, &write_host)) {
                FD_SET(host_to_guest[i], writefds);
            }
            if (exceptfds && FD_ISSET(i, &except_host)) {
                FD_SET(host_to_guest[i], exceptfds);
            }
        }
    }
    if (ret < 0) {
        s32 error = errno;
        LOG_ERROR(Kernel_Fs, "native select call failed with {} ({})", error,
                  Common::NativeErrorToString(error));
        SetPosixErrno(error);
    }

    return ret;
}
#endif

static s32 FileError(s32 error) {
    *__Error() = error;
    return -1;
}
static s32 NativeFileResult(int result) {
    if (result < 0) { SetPosixErrno(errno); return -1; }
    return result;
}
static bool SnapshotPath(const char* path, std::string& result) {
    if (!path) { FileError(POSIX_EFAULT); return false; }
    for (size_t i = 0; i <= ORBIS_MAX_PATH; ++i) {
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
        if (!Core::GuestCpu::ValidateHlePointer(path + i, 1, false)) {
            FileError(POSIX_EFAULT); return false;
        }
#endif
        const auto c = path[i];
        if (!c) return true;
        result += c;
    }
    FileError(POSIX_ENAMETOOLONG); return false;
}
static bool SnapshotTimes(const OrbisKernelTimeval* times, std::array<OrbisKernelTimeval, 2>& values) {
    if (!times) return true;
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    if (!Core::GuestCpu::ValidateHlePointer(times, sizeof(values), false)) {
        FileError(POSIX_EFAULT); return false;
    }
#endif
    std::memcpy(values.data(), times, sizeof(values));
    for (const auto& t : values) {
        if (t.tv_usec < 0 || t.tv_usec >= 1000000) { FileError(POSIX_EINVAL); return false; }
    }
    return true;
}
static bool IsReadOnly(const std::string& guest_path) {
    const auto mount = Common::Singleton<Core::FileSys::MntPoints>::Instance()->GetMount(guest_path);
    return mount && mount->read_only;
}

s32 PS4_SYSV_ABI posix_fcntl(s32 fd, s32 command, u64 argument) {
    auto* file = Common::Singleton<Core::FileSys::HandleTable>::Instance()->GetFile(fd);
    if (!file || !file->is_opened) return FileError(POSIX_EBADF);
    switch (command) {
    case 1: return file->descriptor_flags.load(); // F_GETFD
    case 2: // F_SETFD
        if (argument & ~1ull) return FileError(POSIX_EINVAL);
        file->descriptor_flags = static_cast<u32>(argument); return 0;
    case 3: return file->status_flags.load(); // F_GETFL
    case 4: { // F_SETFL: change only the status bits supported by the backend.
        constexpr u32 known = 3 | ORBIS_KERNEL_O_NONBLOCK | ORBIS_KERNEL_O_APPEND |
            ORBIS_KERNEL_O_DIRECTORY | ORBIS_KERNEL_O_SYNC | ORBIS_KERNEL_O_DSYNC | ORBIS_KERNEL_O_DIRECT;
        if (argument & ~u64{known}) return FileError(POSIX_EINVAL);
        std::scoped_lock lock{file->m_mutex};
        const u32 previous = file->status_flags;
        const u32 requested = static_cast<u32>(argument);
        constexpr u32 mutable_bits = ORBIS_KERNEL_O_NONBLOCK | ORBIS_KERNEL_O_APPEND;
        if ((requested ^ previous) & (ORBIS_KERNEL_O_SYNC | ORBIS_KERNEL_O_DSYNC | ORBIS_KERNEL_O_DIRECT))
            return FileError(POSIX_EOPNOTSUPP);
        if (file->type == Core::FileSys::FileType::Socket) {
            if ((previous ^ requested) & ORBIS_KERNEL_O_APPEND) return FileError(POSIX_EOPNOTSUPP);
            const s32 nonblock = (requested & ORBIS_KERNEL_O_NONBLOCK) != 0;
            if (file->socket->SetSocketOptions(Net::ORBIS_NET_SOL_SOCKET, Net::ORBIS_NET_SO_NBIO,
                                               &nonblock, sizeof(nonblock)) < 0) return -1;
        } else if (file->type == Core::FileSys::FileType::Regular) {
#ifdef _WIN32
            if ((previous ^ requested) & ORBIS_KERNEL_O_APPEND) return FileError(POSIX_EOPNOTSUPP);
#else
            const int native = file->f.GetNativeDescriptor();
            const int flags = ::fcntl(native, F_GETFL);
            if (flags < 0 || ::fcntl(native, F_SETFL, (flags & ~(O_NONBLOCK | O_APPEND)) |
                ((requested & ORBIS_KERNEL_O_NONBLOCK) ? O_NONBLOCK : 0) |
                ((requested & ORBIS_KERNEL_O_APPEND) ? O_APPEND : 0)) < 0) return NativeFileResult(-1);
#endif
        } else if ((previous ^ requested) & mutable_bits) {
            return FileError(POSIX_EOPNOTSUPP);
        }
        file->status_flags = (previous & ~mutable_bits) | (requested & mutable_bits);
        return 0;
    }
    default: return FileError(POSIX_EINVAL);
    }
}
s32 PS4_SYSV_ABI sceKernelFcntl(s32 fd, s32 command, u64 argument) {
    const s32 result = posix_fcntl(fd, command, argument);
    return result < 0 ? ErrnoToSceKernelError(*__Error()) : result;
}

s32 PS4_SYSV_ABI posix_chmod(const char* path, u32 mode) {
    std::string guest;
    if (!SnapshotPath(path, guest)) return -1;
    bool read_only{};
    const auto host = Common::Singleton<Core::FileSys::MntPoints>::Instance()->GetHostPath(guest, &read_only);
    if (read_only) return FileError(POSIX_EROFS);
#ifdef _WIN32
    return NativeFileResult(_wchmod(host.c_str(), mode & (_S_IREAD | _S_IWRITE)));
#else
    return NativeFileResult(::chmod(host.c_str(), mode & 07777));
#endif
}
s32 PS4_SYSV_ABI posix_fchmod(s32 fd, u32 mode) {
    auto* file = Common::Singleton<Core::FileSys::HandleTable>::Instance()->GetFile(fd);
    if (!file || !file->is_opened) return FileError(POSIX_EBADF);
    if (IsReadOnly(file->m_guest_name)) return FileError(POSIX_EROFS);
    std::scoped_lock lock{file->m_mutex};
    if (file->type != Core::FileSys::FileType::Regular && file->type != Core::FileSys::FileType::Directory)
        return FileError(POSIX_EOPNOTSUPP);
#ifdef _WIN32
    if (file->type == Core::FileSys::FileType::Directory)
        return NativeFileResult(_wchmod(file->m_host_name.c_str(), mode & (_S_IREAD | _S_IWRITE)));
    auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(file->f.GetNativeDescriptor()));
    FILE_BASIC_INFO info{};
    if (!GetFileInformationByHandleEx(handle, FileBasicInfo, &info, sizeof(info))) return FileError(POSIX_EIO);
    if (mode & 0222) info.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
    else info.FileAttributes |= FILE_ATTRIBUTE_READONLY;
    return SetFileInformationByHandle(handle, FileBasicInfo, &info, sizeof(info)) ? 0 : FileError(POSIX_EIO);
#else
    if (file->type == Core::FileSys::FileType::Directory)
        return NativeFileResult(::chmod(file->m_host_name.c_str(), mode & 07777));
    return NativeFileResult(::fchmod(file->f.GetNativeDescriptor(), mode & 07777));
#endif
}
s32 PS4_SYSV_ABI posix_utimes(const char* path, const OrbisKernelTimeval* times) {
    std::string guest;
    std::array<OrbisKernelTimeval, 2> values{};
    if (!SnapshotPath(path, guest) || !SnapshotTimes(times, values)) return -1;
    bool read_only{};
    const auto host = Common::Singleton<Core::FileSys::MntPoints>::Instance()->GetHostPath(guest, &read_only);
    if (read_only) return FileError(POSIX_EROFS);
#ifdef _WIN32
    const __utimbuf64 native{values[0].tv_sec, values[1].tv_sec};
    return NativeFileResult(_wutime64(host.c_str(), times ? &native : nullptr));
#else
    const timeval native[2]{{values[0].tv_sec, static_cast<suseconds_t>(values[0].tv_usec)},
                           {values[1].tv_sec, static_cast<suseconds_t>(values[1].tv_usec)}};
    return NativeFileResult(::utimes(host.c_str(), times ? native : nullptr));
#endif
}
s32 PS4_SYSV_ABI posix_futimes(s32 fd, const OrbisKernelTimeval* times) {
    auto* file = Common::Singleton<Core::FileSys::HandleTable>::Instance()->GetFile(fd);
    if (!file || !file->is_opened) return FileError(POSIX_EBADF);
    if (IsReadOnly(file->m_guest_name)) return FileError(POSIX_EROFS);
    std::array<OrbisKernelTimeval, 2> values{};
    if (!SnapshotTimes(times, values)) return -1;
    std::scoped_lock lock{file->m_mutex};
    if (file->type != Core::FileSys::FileType::Regular && file->type != Core::FileSys::FileType::Directory)
        return FileError(POSIX_EOPNOTSUPP);
#ifdef _WIN32
    const __utimbuf64 native{values[0].tv_sec, values[1].tv_sec};
    if (file->type == Core::FileSys::FileType::Directory)
        return NativeFileResult(_wutime64(file->m_host_name.c_str(), times ? &native : nullptr));
    return NativeFileResult(_futime64(file->f.GetNativeDescriptor(), times ? &native : nullptr));
#else
    const timeval native[2]{{values[0].tv_sec, static_cast<suseconds_t>(values[0].tv_usec)},
                           {values[1].tv_sec, static_cast<suseconds_t>(values[1].tv_usec)}};
    if (file->type == Core::FileSys::FileType::Directory)
        return NativeFileResult(::utimes(file->m_host_name.c_str(), times ? native : nullptr));
    return NativeFileResult(::futimes(file->f.GetNativeDescriptor(), times ? native : nullptr));
#endif
}
s32 PS4_SYSV_ABI sceKernelChmod(const char* path, u32 mode) {
    return posix_chmod(path, mode) < 0 ? ErrnoToSceKernelError(*__Error()) : 0;
}
s32 PS4_SYSV_ABI sceKernelFchmod(s32 fd, u32 mode) {
    return posix_fchmod(fd, mode) < 0 ? ErrnoToSceKernelError(*__Error()) : 0;
}
s32 PS4_SYSV_ABI sceKernelUtimes(const char* path, const OrbisKernelTimeval* times) {
    return posix_utimes(path, times) < 0 ? ErrnoToSceKernelError(*__Error()) : 0;
}
s32 PS4_SYSV_ABI sceKernelFutimes(s32 fd, const OrbisKernelTimeval* times) {
    return posix_futimes(fd, times) < 0 ? ErrnoToSceKernelError(*__Error()) : 0;
}

void RegisterFileSystem(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("8nY19bKoiZk", "libScePosix", 1, "libkernel", posix_fcntl);
    LIB_FUNCTION("SoZkxZkCHaw", "libkernel", 1, "libkernel", sceKernelFcntl);
    LIB_FUNCTION("z0dtnPxYgtg", "libScePosix", 1, "libkernel", posix_chmod);
    LIB_FUNCTION("n01yNbQO5W4", "libScePosix", 1, "libkernel", posix_fchmod);
    LIB_FUNCTION("GDuV00CHrUg", "libScePosix", 1, "libkernel", posix_utimes);
    LIB_FUNCTION("+0EDo7YzcoU", "libScePosix", 1, "libkernel", posix_futimes);
    LIB_FUNCTION("fgIsQ10xYVA", "libkernel", 1, "libkernel", sceKernelChmod);
    LIB_FUNCTION("UtszJWHrDcA", "libkernel", 1, "libkernel", sceKernelFchmod);
    LIB_FUNCTION("0Cq8ipKr9n0", "libkernel", 1, "libkernel", sceKernelUtimes);
    LIB_FUNCTION("NLq2b1jOaN0", "libkernel", 1, "libkernel", sceKernelFutimes);
    LIB_FUNCTION("VAzswvTOCzI", "libScePosix", 1, "libkernel", posix_unlink);

    LIB_FUNCTION("6c3rCVE-fTU", "libkernel", 1, "libkernel", open);
    LIB_FUNCTION("wuCroIGjt2g", "libScePosix", 1, "libkernel", posix_open);
    LIB_FUNCTION("wuCroIGjt2g", "libkernel", 1, "libkernel", posix_open);
    LIB_FUNCTION("1G3lF1Gg1k8", "libkernel", 1, "libkernel", sceKernelOpen);
    LIB_FUNCTION("NNtFaKJbPt0", "libkernel", 1, "libkernel", close);
    LIB_FUNCTION("bY-PO6JhzhQ", "libScePosix", 1, "libkernel", posix_close);
    LIB_FUNCTION("bY-PO6JhzhQ", "libkernel", 1, "libkernel", posix_close);
    LIB_FUNCTION("UK2Tl2DWUns", "libkernel", 1, "libkernel", sceKernelClose);
    LIB_FUNCTION("FxVZqBAA7ks", "libkernel", 1, "libkernel", write);
    LIB_FUNCTION("FN4gaPmuFV8", "libScePosix", 1, "libkernel", posix_write);
    LIB_FUNCTION("FN4gaPmuFV8", "libkernel", 1, "libkernel", posix_write);
    LIB_FUNCTION("4wSze92BhLI", "libkernel", 1, "libkernel", sceKernelWrite);
    LIB_FUNCTION("+WRlkKjZvag", "libkernel", 1, "libkernel", readv);
    LIB_FUNCTION("YSHRBRLn2pI", "libkernel", 1, "libkernel", writev);
    LIB_FUNCTION("kAt6VDbHmro", "libkernel", 1, "libkernel", sceKernelWritev);
    LIB_FUNCTION("Oy6IpwgtYOk", "libScePosix", 1, "libkernel", posix_lseek);
    LIB_FUNCTION("Oy6IpwgtYOk", "libkernel", 1, "libkernel", posix_lseek);
    LIB_FUNCTION("oib76F-12fk", "libkernel", 1, "libkernel", sceKernelLseek);
    LIB_FUNCTION("DRuBt2pvICk", "libkernel", 1, "libkernel", read);
    LIB_FUNCTION("AqBioC2vF3I", "libScePosix", 1, "libkernel", posix_read);
    LIB_FUNCTION("AqBioC2vF3I", "libkernel", 1, "libkernel", posix_read);
    LIB_FUNCTION("Cg4srZ6TKbU", "libkernel", 1, "libkernel", sceKernelRead);
    LIB_FUNCTION("JGMio+21L4c", "libScePosix", 1, "libkernel", posix_mkdir);
    LIB_FUNCTION("JGMio+21L4c", "libkernel", 1, "libkernel", posix_mkdir);
    LIB_FUNCTION("1-LFLmRFxxM", "libkernel", 1, "libkernel", sceKernelMkdir);
    LIB_FUNCTION("c7ZnT7V1B98", "libScePosix", 1, "libkernel", posix_rmdir);
    LIB_FUNCTION("c7ZnT7V1B98", "libkernel", 1, "libkernel", posix_rmdir);
    LIB_FUNCTION("naInUjYt3so", "libkernel", 1, "libkernel", sceKernelRmdir);
    LIB_FUNCTION("E6ao34wPw+U", "libScePosix", 1, "libkernel", posix_stat);
    LIB_FUNCTION("E6ao34wPw+U", "libkernel", 1, "libkernel", posix_stat);
    LIB_FUNCTION("eV9wAD2riIA", "libkernel", 1, "libkernel", sceKernelStat);
    LIB_FUNCTION("uWyW3v98sU4", "libkernel", 1, "libkernel", sceKernelCheckReachability);
    LIB_FUNCTION("mqQMh1zPPT8", "libScePosix", 1, "libkernel", posix_fstat);
    LIB_FUNCTION("mqQMh1zPPT8", "libkernel", 1, "libkernel", posix_fstat);
    LIB_FUNCTION("kBwCPsYX-m4", "libkernel", 1, "libkernel", sceKernelFstat);
    LIB_FUNCTION("ih4CD9-gghM", "libkernel", 1, "libkernel", posix_ftruncate);
    LIB_FUNCTION("ih4CD9-gghM", "libScePosix", 1, "libkernel", posix_ftruncate);
    LIB_FUNCTION("VW3TVZiM4-E", "libkernel", 1, "libkernel", sceKernelFtruncate);
    LIB_FUNCTION("NN01qLRhiqU", "libScePosix", 1, "libkernel", posix_rename);
    LIB_FUNCTION("NN01qLRhiqU", "libkernel", 1, "libkernel", posix_rename);
    LIB_FUNCTION("52NcYU9+lEo", "libkernel", 1, "libkernel", sceKernelRename);
    LIB_FUNCTION("yTj62I7kw4s", "libkernel", 1, "libkernel", sceKernelPreadv);
    LIB_FUNCTION("ezv-RSBNKqI", "libScePosix", 1, "libkernel", posix_pread);
    LIB_FUNCTION("ezv-RSBNKqI", "libkernel", 1, "libkernel", posix_pread);
    LIB_FUNCTION("+r3rMFwItV4", "libkernel", 1, "libkernel", sceKernelPread);
    LIB_FUNCTION("juWbTNM+8hw", "libScePosix", 1, "libkernel", posix_fsync);
    LIB_FUNCTION("juWbTNM+8hw", "libkernel", 1, "libkernel", posix_fsync);
    LIB_FUNCTION("fTx66l5iWIA", "libkernel", 1, "libkernel", sceKernelFsync);
    LIB_FUNCTION("j2AIqSqJP0w", "libkernel", 1, "libkernel", sceKernelGetdents);
    LIB_FUNCTION("sfKygSjIbI8", "libkernel", 1, "libkernel", getdirentries);
    LIB_FUNCTION("2G6i6hMIUUY", "libkernel", 1, "libkernel", posix_getdents);
    LIB_FUNCTION("taRWhTJFTgE", "libkernel", 1, "libkernel", sceKernelGetdirentries);
    LIB_FUNCTION("C2kJ-byS5rM", "libkernel", 1, "libkernel", posix_pwrite);
    LIB_FUNCTION("C2kJ-byS5rM", "libScePosix", 1, "libkernel", posix_pwrite);
    LIB_FUNCTION("FCcmRZhWtOk", "libScePosix", 1, "libkernel", posix_pwritev);
    LIB_FUNCTION("FCcmRZhWtOk", "libkernel", 1, "libkernel", posix_pwritev);
    LIB_FUNCTION("nKWi-N2HBV4", "libkernel", 1, "libkernel", sceKernelPwrite);
    LIB_FUNCTION("mBd4AfLP+u8", "libkernel", 1, "libkernel", sceKernelPwritev);
    LIB_FUNCTION("VAzswvTOCzI", "libkernel", 1, "libkernel", posix_unlink);
    LIB_FUNCTION("AUXVxWeJU-A", "libkernel", 1, "libkernel", sceKernelUnlink);
    LIB_FUNCTION("T8fER+tIGgk", "libScePosix", 1, "libkernel", posix_select);
    LIB_FUNCTION("T8fER+tIGgk", "libkernel", 1, "libkernel", posix_select);
}

} // namespace Libraries::Kernel
