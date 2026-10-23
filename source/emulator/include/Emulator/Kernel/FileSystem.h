#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_FILESYSTEM_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_FILESYSTEM_H_

#include "Lumen/Core/Common.h"
#include "Lumen/Core/String.h"
#include "Lumen/Core/Subsystems.h"

#include "Emulator/Common.h"
#include "Emulator/Kernel/Pthread.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::LibKernel::FileSystem {

struct FileStat
{
	uint32_t       st_dev;
	uint32_t       st_ino;
	uint16_t       st_mode;
	uint16_t       st_nlink;
	uint32_t       st_uid;
	uint32_t       st_gid;
	uint32_t       st_rdev;
	KernelTimespec st_atim;
	KernelTimespec st_mtim;
	KernelTimespec st_ctim;
	int64_t        st_size;
	int64_t        st_blocks;
	uint32_t       st_blksize;
	uint32_t       st_flags;
	uint32_t       st_gen;
	int32_t        st_lspare;
	KernelTimespec st_birthtim;
	unsigned int: (8 / 2) * (16 - static_cast<int>(sizeof(KernelTimespec)));
	unsigned int: (8 / 2) * (16 - static_cast<int>(sizeof(KernelTimespec)));
};

LUMEN_SUBSYSTEM_DEFINE(FileSystem);

// Release every session-owned descriptor and mount while keeping the
// process-owned FileSystem subsystem ready for the next guest. The caller
// must have already stopped all guest threads. Returns false when the
// subsystem has not been initialized, without changing partial state.
[[nodiscard]] bool ResetForNextSession();

void   Mount(const String& folder, const String& point);
void   Umount(const String& folder_or_point);
String GetRealFilename(const String& mounted_file_name);

#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
[[nodiscard]] bool FileSystemSessionResetContractTest();
#endif

int LUMEN_SYSV_ABI     KernelOpen(const char* path, int flags, uint16_t mode);
int LUMEN_SYSV_ABI     KernelClose(int d);
int LUMEN_SYSV_ABI     KernelFcntl(int d, int command, uint64_t argument);
int LUMEN_SYSV_ABI     KernelFsync(int d);
int LUMEN_SYSV_ABI     KernelFtruncate(int d, int64_t length);
int64_t LUMEN_SYSV_ABI KernelRead(int d, void* buf, size_t nbytes);
int64_t LUMEN_SYSV_ABI KernelPread(int d, void* buf, size_t nbytes, int64_t offset);
// Fills a guest destination for a file-backed posix_mmap: the range is zeroed
// first and then read from the descriptor through the same guarded staging
// path as KernelPread. A short fill reports the file extent and the caller
// keeps the zeroed tail. Returns OK or a kernel error code.
int LUMEN_SYSV_ABI     KernelReadMappedRange(int d, void* guest_buffer, size_t nbytes, int64_t offset, uint64_t* bytes_read);
int64_t LUMEN_SYSV_ABI KernelWrite(int d, const void* buf, size_t nbytes);
int64_t LUMEN_SYSV_ABI KernelPwrite(int d, const void* buf, size_t nbytes, int64_t offset);
int64_t LUMEN_SYSV_ABI KernelLseek(int d, int64_t offset, int whence);
int LUMEN_SYSV_ABI     KernelStat(const char* path, FileStat* sb);
int LUMEN_SYSV_ABI     KernelFstat(int d, FileStat* sb);
int LUMEN_SYSV_ABI     KernelFchmod(int d, uint32_t mode);
int LUMEN_SYSV_ABI     KernelChmod(const char* path, uint32_t mode);
int LUMEN_SYSV_ABI     KernelUtimes(const char* path, const KernelTimeval* times);
int LUMEN_SYSV_ABI     KernelFutimes(int d, const KernelTimeval* times);
int LUMEN_SYSV_ABI     KernelUnlink(const char* path);
int LUMEN_SYSV_ABI     KernelRename(const char* old_path, const char* new_path);
int LUMEN_SYSV_ABI     KernelRmdir(const char* path);
int LUMEN_SYSV_ABI     KernelGetdirentries(int fd, char* buf, int nbytes, int64_t* basep);
int LUMEN_SYSV_ABI     KernelGetdents(int fd, char* buf, int nbytes);
int LUMEN_SYSV_ABI     KernelMkdir(const char* path, uint16_t mode);

} // namespace Lumen::Libs::LibKernel::FileSystem

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_FILESYSTEM_H_ */
