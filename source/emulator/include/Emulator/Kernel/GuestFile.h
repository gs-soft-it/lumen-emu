#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_GUESTFILE_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_GUESTFILE_H_

#include "Lumen/Core/Common.h"

#include "Emulator/Common.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Core {
class File;
}

namespace Lumen::Libs::GuestFile {

enum class ReadError
{
	None,
	InvalidAddress,
	SeekFailed,
	ReadFailed,
};

struct ReadResult
{
	uint32_t  bytes_read = 0;
	ReadError error      = ReadError::None;
};

ReadResult Read(Core::File& file, void* guest_buffer, uint32_t size);
ReadResult Pread(Core::File& file, void* guest_buffer, uint32_t size, uint64_t offset);

enum class WriteError
{
	None,
	InvalidAddress,
	SeekFailed,
};

struct WriteResult
{
	uint32_t bytes_written = 0;
	WriteError error = WriteError::None;
};

// The caller serializes access to the file cursor. The complete guest source
// is borrowed before host I/O (including Pwrite's seek), through its last use.
WriteResult Write(Core::File& file, const void* guest_buffer, uint32_t size);
WriteResult Pwrite(Core::File& file, const void* guest_buffer, uint32_t size, uint64_t offset);

bool PreadProfileEnabled();
void PreadProfileRecordMutexWait(uint64_t nanoseconds);

} // namespace Lumen::Libs::GuestFile

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_GUESTFILE_H_ */
