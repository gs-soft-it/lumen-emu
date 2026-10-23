#include "Lumen/Core/File.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Kernel/GuestFile.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
#include <unistd.h>
#endif

namespace {

constexpr uint32_t FILE_OFFSET        = 0x10640;
constexpr uint32_t READ_SIZE          = 512 * 1024;
constexpr uint32_t DESTINATION_OFFSET = 0x640;
constexpr uint32_t SENTINEL_SIZE      = 64;
constexpr uint8_t  SENTINEL           = 0xa5;

uint8_t pattern(uint32_t offset)
{
	return static_cast<uint8_t>((offset * 37u + (offset >> 8u) + 11u) & 0xffu);
}

bool verify_bytes(const uint8_t* data, uint32_t file_offset, uint32_t size)
{
	for (uint32_t i = 0; i < size; i++)
	{
		if (data[i] != pattern(file_offset + i))
		{
			std::printf("guest-file mismatch at %u: got=%u expected=%u\n", i, data[i], pattern(file_offset + i));
			return false;
		}
	}
	return true;
}

bool verify_sentinel(const uint8_t* data, uint32_t size)
{
	for (uint32_t i = 0; i < size; i++)
	{
		if (data[i] != SENTINEL)
		{
			std::printf("guest-file sentinel overwritten at %u\n", i);
			return false;
		}
	}
	return true;
}

bool verify_guest_write_buffers()
{
	namespace VM = Lumen::Core::VirtualMemory;
	namespace GuestFile = Lumen::Libs::GuestFile;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	const long host_page = sysconf(_SC_PAGESIZE);
	if (host_page <= 0) return false;
	const uint64_t page = static_cast<uint64_t>(host_page);
#else
	constexpr uint64_t page = 4096;
#endif
	const uint64_t mapping = VM::Alloc(0, 2 * page, VM::Mode::ReadWrite);
	if (mapping == 0) return false;
	auto* source = reinterpret_cast<uint8_t*>(mapping);
	std::memset(source, 0x5a, static_cast<size_t>(page));
	bool ok = VM::Protect(mapping, page, VM::Mode::Read) &&
	          VM::Protect(mapping + page, page, VM::Mode::NoAccess);
	Lumen::Core::File file;
	ok = file.CreateInMem() && ok;
	if (file.IsInvalid())
	{
		VM::Free(mapping);
		return false;
	}
	std::array<uint8_t, 128> expected {};
	expected.fill(SENTINEL);
	uint32_t written = 0;
	file.Write(expected.data(), static_cast<uint32_t>(expected.size()), &written);
	ok = written == expected.size() && file.Seek(7) && ok;
	const auto sequential = GuestFile::Write(file, source, 16);
	std::memset(expected.data() + 7, 0x5a, 16);
	ok = sequential.error == GuestFile::WriteError::None && sequential.bytes_written == 16 && file.Tell() == 23 && ok;
	const auto positioned = GuestFile::Pwrite(file, source, 16, 48);
	std::memset(expected.data() + 48, 0x5a, 16);
	ok = positioned.error == GuestFile::WriteError::None && positioned.bytes_written == 16 && file.Tell() == 23 && ok;
	const auto intact = [&]
	{
		std::array<uint8_t, 128> actual {};
		uint32_t bytes = 0;
		return file.Size() == expected.size() &&
		       file.ReadAt(actual.data(), static_cast<uint32_t>(actual.size()), 0, &bytes) &&
		       bytes == actual.size() && actual == expected && file.Tell() == 23;
	};
	ok = intact() && ok;
#if defined(LUMEN_FEXCORE_RUNTIME) && defined(__APPLE__)
	const auto denied = [&](const void* address, uint32_t bytes)
	{
		const auto first = GuestFile::Write(file, address, bytes);
		const auto second = GuestFile::Pwrite(file, address, bytes, 91);
		return first.error == GuestFile::WriteError::InvalidAddress && first.bytes_written == 0 &&
		       second.error == GuestFile::WriteError::InvalidAddress && second.bytes_written == 0 && intact();
	};
	ok = denied(source + page, 16) && denied(source + page - 4, 8) &&
	     denied(reinterpret_cast<const void*>(UINTPTR_MAX - 3), 8) && ok;
	const uint64_t tiny = VM::Alloc(0, 1, VM::Mode::ReadWrite);
	ok = tiny != 0 && denied(reinterpret_cast<const void*>(tiny), 2) && ok;
	if (tiny != 0) ok = VM::Free(tiny) && ok;
#endif
	const auto empty_write = GuestFile::Write(file, nullptr, 0);
	const auto empty_pwrite = GuestFile::Pwrite(file, nullptr, 0, UINT64_MAX);
	ok = empty_write.error == GuestFile::WriteError::None && empty_write.bytes_written == 0 &&
	     empty_pwrite.error == GuestFile::WriteError::None && empty_pwrite.bytes_written == 0 && intact() && ok;
	file.Close();
	ok = VM::Free(mapping) && ok;
	if (!ok) std::puts("guest-file-write-source/position/contents=failed");
	return ok;
}

} // namespace

int main()
{
	using Lumen::Core::File;
	namespace GuestFile    = Lumen::Libs::GuestFile;
	namespace VirtualMemory = Lumen::Core::VirtualMemory;

	Lumen::Core::core_file_init();
	VirtualMemory::Init();

	const Lumen::String test_path = U"_lumen_guest_file_test.tmp";
	File::DeleteFile(test_path);

	constexpr uint32_t source_size = FILE_OFFSET + READ_SIZE + 4096;
	std::vector<uint8_t> source(source_size);
	for (uint32_t i = 0; i < source_size; i++)
	{
		source[i] = pattern(i);
	}

	File output;
	if (!output.Create(test_path))
	{
		std::printf("guest-file create failed\n");
		return 1;
	}
	uint32_t written = 0;
	output.Write(source.data(), source_size, &written);
	output.Close();
	if (written != source_size)
	{
		File::DeleteFile(test_path);
		std::printf("guest-file write failed: %u\n", written);
		return 1;
	}

	constexpr uint32_t mapping_size = DESTINATION_OFFSET + READ_SIZE + SENTINEL_SIZE;
	const uint64_t mapping = VirtualMemory::Alloc(0, mapping_size, VirtualMemory::Mode::ExecuteReadWrite);
	if (mapping == 0)
	{
		File::DeleteFile(test_path);
		std::printf("guest-file executable mapping failed\n");
		return 1;
	}
	auto* const mapping_data = reinterpret_cast<uint8_t*>(mapping);
	auto* const destination  = mapping_data + DESTINATION_OFFSET;
	std::memset(mapping_data, SENTINEL, mapping_size);

	File input;
	if (!input.Open(test_path, File::Mode::Read))
	{
		File::DeleteFile(test_path);
		VirtualMemory::Free(mapping);
		std::printf("guest-file open failed\n");
		return 1;
	}

	Lumen::Core::DateTime named_access;
	Lumen::Core::DateTime named_write;
	Lumen::Core::DateTime handle_access;
	Lumen::Core::DateTime handle_write;
	File::GetLastAccessAndWriteTimeUTC(test_path, &named_access, &named_write);
	input.GetLastAccessAndWriteTimeUTC(&handle_access, &handle_write);

	bool ok         = !named_access.IsInvalid() && !named_write.IsInvalid() && !handle_access.IsInvalid() && !handle_write.IsInvalid();
	ok              = ok && named_write == handle_write;
	ok              = ok && input.Seek(123);
	const auto full = GuestFile::Pread(input, destination, READ_SIZE, FILE_OFFSET);
	ok              = ok && full.error == GuestFile::ReadError::None && full.bytes_read == READ_SIZE;
	ok              = ok && input.Tell() == 123;
	ok              = ok && verify_bytes(destination, FILE_OFFSET, READ_SIZE);
	ok              = ok && verify_sentinel(mapping_data, DESTINATION_OFFSET);
	ok              = ok && verify_sentinel(destination + READ_SIZE, SENTINEL_SIZE);

	constexpr uint32_t sequential_size = 4096;
	std::memset(destination, SENTINEL, sequential_size + SENTINEL_SIZE);
	const bool sequential_seek = input.Seek(FILE_OFFSET);
	const auto sequential      = GuestFile::Read(input, destination, sequential_size);
	ok = ok && sequential_seek;
	ok = ok && sequential.error == GuestFile::ReadError::None && sequential.bytes_read == sequential_size;
	ok = ok && input.Tell() == FILE_OFFSET + sequential_size;
	ok = ok && verify_bytes(destination, FILE_OFFSET, sequential_size);
	ok = ok && verify_sentinel(destination + sequential_size, SENTINEL_SIZE);
	const bool position_restored = input.Seek(123);
	ok                           = ok && position_restored;

	std::array<uint8_t, 256> memory_read {};
	File                     memory_input;
	const bool               memory_opened = memory_input.OpenInMem(source.data(), source_size);
	ok                                     = ok && memory_opened;
	Lumen::Core::DateTime memory_access;
	Lumen::Core::DateTime memory_write;
	if (memory_opened)
	{
		memory_input.GetLastAccessAndWriteTimeUTC(&memory_access, &memory_write);
		ok = ok && !memory_access.IsInvalid() && !memory_write.IsInvalid();
		ok = ok && memory_input.Seek(77);
		uint32_t memory_bytes = 0;
		ok = ok && memory_input.ReadAt(memory_read.data(), static_cast<uint32_t>(memory_read.size()), FILE_OFFSET, &memory_bytes);
		ok = ok && memory_bytes == memory_read.size() && memory_input.Tell() == 77;
		ok = ok && verify_bytes(memory_read.data(), FILE_OFFSET, memory_bytes);
		uint32_t memory_eof_bytes = 1;
		ok = ok &&
		     memory_input.ReadAt(memory_read.data(), static_cast<uint32_t>(memory_read.size()), source_size + 1, &memory_eof_bytes);
		ok = ok && memory_eof_bytes == 0 && memory_input.Tell() == 77;
		memory_input.Close();
	}

	constexpr uint32_t eof_size = 257;
	std::memset(destination, SENTINEL, 512);
	const auto eof = GuestFile::Pread(input, destination, 512, source_size - eof_size);
	ok = ok && eof.error == GuestFile::ReadError::None && eof.bytes_read == eof_size;
	ok = ok && input.Tell() == 123;
	ok = ok && verify_bytes(destination, source_size - eof_size, eof_size);
	ok = ok && verify_sentinel(destination + eof_size, 512 - eof_size);

	constexpr uint32_t concurrent_threads = 4;
	constexpr uint32_t concurrent_size    = 4096;
	constexpr uint32_t concurrent_stride  = 8192;
	constexpr uint32_t concurrent_repeats = 32;
	std::atomic_bool   concurrent_ok {true};
	std::array<std::thread, concurrent_threads> workers {};
	std::array<std::array<uint8_t, concurrent_size>, concurrent_threads> concurrent_data {};
	for (uint32_t thread = 0; thread < concurrent_threads; thread++)
	{
		workers[thread] = std::thread([&, thread] {
			for (uint32_t repeat = 0; repeat < concurrent_repeats; repeat++)
			{
				const uint32_t file_offset = FILE_OFFSET + thread * concurrent_stride;
				auto* const   thread_data = concurrent_data[thread].data();
				const auto result = GuestFile::Pread(input, thread_data, concurrent_size, file_offset);
				if (result.error != GuestFile::ReadError::None || result.bytes_read != concurrent_size ||
				    !verify_bytes(thread_data, file_offset, concurrent_size))
				{
					concurrent_ok = false;
					break;
				}
			}
		});
	}
	for (auto& worker: workers)
	{
		worker.join();
	}
	ok = ok && concurrent_ok && input.Tell() == 123;

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	bool guest_subpages = false;
	constexpr uint32_t guest_page_size = 4096;
	const long host_page_size_value = sysconf(_SC_PAGESIZE);
	const uint64_t permission_page_size = VirtualMemory::RequiresUniformHostPageDataProtection()
	    ? static_cast<uint64_t>(host_page_size_value) : guest_page_size;
	const uint64_t guest_page_mapping_size =
	    host_page_size_value > static_cast<long>(4 * permission_page_size)
	        ? static_cast<uint64_t>(host_page_size_value)
	        : static_cast<uint64_t>(4 * permission_page_size);
	const uint64_t guest_page_mapping =
	    VirtualMemory::Alloc(0, guest_page_mapping_size, VirtualMemory::Mode::ReadWrite);
	if (guest_page_mapping == 0)
	{
		ok = false;
	} else
	{
		auto* guest_pages = reinterpret_cast<uint8_t*>(guest_page_mapping);
		std::memset(guest_pages, SENTINEL, static_cast<size_t>(guest_page_mapping_size));
		VirtualMemory::Mode old_mode = VirtualMemory::Mode::NoAccess;
		const bool protected_modes =
		    VirtualMemory::Protect(guest_page_mapping, permission_page_size, VirtualMemory::Mode::Read, &old_mode) &&
		    old_mode == VirtualMemory::Mode::ReadWrite &&
		    VirtualMemory::Protect(guest_page_mapping + permission_page_size, permission_page_size,
		                           VirtualMemory::Mode::ReadWrite, &old_mode) &&
		    old_mode == VirtualMemory::Mode::ReadWrite &&
		    VirtualMemory::Protect(guest_page_mapping + 2 * permission_page_size, permission_page_size,
		                           VirtualMemory::Mode::NoAccess, &old_mode) &&
		    old_mode == VirtualMemory::Mode::ReadWrite &&
		    VirtualMemory::Protect(guest_page_mapping + 3 * permission_page_size, permission_page_size,
		                           VirtualMemory::Mode::Execute, &old_mode) &&
		    old_mode == VirtualMemory::Mode::ReadWrite;

		const auto read_only = GuestFile::Pread(input, guest_pages, 16, FILE_OFFSET);
		const bool read_only_unchanged = verify_sentinel(guest_pages, 16);
		auto* cross_destination = guest_pages + permission_page_size - 8;
		const auto cross = GuestFile::Pread(input, cross_destination, 16, FILE_OFFSET);
		const bool cross_unchanged = verify_sentinel(cross_destination, 16);
		const auto writable = GuestFile::Pread(input, guest_pages + permission_page_size, 16, FILE_OFFSET);
		const bool writable_value = verify_bytes(guest_pages + permission_page_size, FILE_OFFSET, 16);
		const auto no_access = GuestFile::Pread(input, guest_pages + 2 * permission_page_size, 16, FILE_OFFSET);
		const auto execute_only = GuestFile::Pread(input, guest_pages + 3 * permission_page_size, 16, FILE_OFFSET);

		guest_subpages = protected_modes && read_only.error == GuestFile::ReadError::InvalidAddress &&
		                 read_only.bytes_read == 0 && read_only_unchanged &&
		                 writable.error == GuestFile::ReadError::None && writable.bytes_read == 16 && writable_value &&
		                 cross.error == GuestFile::ReadError::InvalidAddress && cross.bytes_read == 0 && cross_unchanged &&
		                 no_access.error == GuestFile::ReadError::InvalidAddress && no_access.bytes_read == 0 &&
		                 execute_only.error == GuestFile::ReadError::InvalidAddress && execute_only.bytes_read == 0 &&
		                 input.Tell() == 123;
		ok = ok && guest_subpages;
		VirtualMemory::Free(guest_page_mapping);
	}

	const uint64_t read_only_mapping = VirtualMemory::Alloc(0, 16384, VirtualMemory::Mode::Read);
	if (read_only_mapping == 0)
	{
		ok = false;
	} else
	{
		const auto invalid = GuestFile::Pread(input, reinterpret_cast<void*>(read_only_mapping), 16, FILE_OFFSET);
		ok = ok && invalid.error == GuestFile::ReadError::InvalidAddress && invalid.bytes_read == 0;
		ok = ok && input.Tell() == 123;
		VirtualMemory::Free(read_only_mapping);
	}

	const auto overflow =
	    GuestFile::Pread(input, reinterpret_cast<void*>(std::numeric_limits<uintptr_t>::max() - 7u), 16, FILE_OFFSET);
	ok = ok && overflow.error == GuestFile::ReadError::InvalidAddress && overflow.bytes_read == 0;
	ok = ok && input.Tell() == 123;
#endif

	input.Close();
	File::DeleteFile(test_path);
	VirtualMemory::Free(mapping);
	ok = verify_guest_write_buffers() && ok;

	if (!ok)
	{
		std::printf("guest-file-read=failed\n");
		return 1;
	}

	std::printf(
	    "guest-file-read-apple-staging=ok guest-file-read-position=ok guest-file-pread-position=ok guest-file-eof=ok "
	    "guest-file-range=ok guest-file-read-at-memory=ok guest-file-handle-times=ok guest-file-pread-concurrency=ok "
	    "guest-file-write-position=ok guest-file-pwrite-position=ok guest-file-write-source=ok "
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	    "guest-file-guest-subpages=ok"
#endif
	    "\n");
	return 0;
}
