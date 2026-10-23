#include "Emulator/Kernel/GuestFile.h"

#include "Lumen/Core/File.h"
#include "Lumen/Core/VirtualMemory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
#include <mach/mach.h>
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
#include <mach/mach_vm.h>
#else
#include <mach/vm_map.h>
#endif
#include <mach/vm_region.h>
#endif

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::GuestFile {

namespace {

using ProfileClock = std::chrono::steady_clock;

struct PreadProfile
{
	std::atomic_uint64_t calls {0};
	std::atomic_uint64_t bytes {0};
	std::atomic_uint64_t mutex_wait_ns {0};
	std::atomic_uint64_t validation_ns {0};
	std::atomic_uint64_t tell_seek_ns {0};
	std::atomic_uint64_t staging_ns {0};
	std::atomic_uint64_t host_read_ns {0};
	std::atomic_uint64_t memcpy_ns {0};
	std::atomic_uint64_t restore_ns {0};
	std::atomic_uint64_t total_ns {0};
	uint64_t             interval = 0;
};

static PreadProfile& pread_profile()
{
	static PreadProfile profile;
	[[maybe_unused]] static const bool configured = [] {
		if (const char* value = std::getenv("LUMEN_GUEST_FILE_PREAD_PROFILE_INTERVAL"); value != nullptr)
		{
			char*                    end    = nullptr;
			const unsigned long long parsed = std::strtoull(value, &end, 10);
			if (end != value && *end == '\0')
			{
				profile.interval = parsed;
			}
		}
		return true;
	}();
	return profile;
}

static uint64_t elapsed_ns(ProfileClock::time_point start, ProfileClock::time_point end)
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
}

static uint64_t take(std::atomic_uint64_t& value)
{
	return value.exchange(0, std::memory_order_relaxed);
}

static void report_pread_profile(uint64_t calls)
{
	auto&          profile  = pread_profile();
	const uint64_t bytes    = take(profile.bytes);
	const uint64_t mutex    = take(profile.mutex_wait_ns);
	const uint64_t scan     = take(profile.validation_ns);
	const uint64_t seek     = take(profile.tell_seek_ns);
	const uint64_t stage    = take(profile.staging_ns);
	const uint64_t read     = take(profile.host_read_ns);
	const uint64_t copy     = take(profile.memcpy_ns);
	const uint64_t restore = take(profile.restore_ns);
	const uint64_t total    = take(profile.total_ns);
	const uint64_t measured = scan + seek + stage + read + copy + restore;
	const uint64_t residual = total > measured ? total - measured : 0;

	std::fprintf(stderr,
	             "[GUEST_FILE_PREAD] calls=%llu bytes=%llu mutex_wait_ns=%llu scan_ns=%llu tell_seek_ns=%llu "
	             "staging_ns=%llu host_read_ns=%llu memcpy_ns=%llu restore_ns=%llu residual_ns=%llu total_ns=%llu\n",
	             static_cast<unsigned long long>(calls), static_cast<unsigned long long>(bytes),
	             static_cast<unsigned long long>(mutex), static_cast<unsigned long long>(scan),
	             static_cast<unsigned long long>(seek), static_cast<unsigned long long>(stage),
	             static_cast<unsigned long long>(read), static_cast<unsigned long long>(copy),
	             static_cast<unsigned long long>(restore), static_cast<unsigned long long>(residual),
	             static_cast<unsigned long long>(total));
}

} // namespace

bool PreadProfileEnabled()
{
	return pread_profile().interval != 0;
}

void PreadProfileRecordMutexWait(uint64_t nanoseconds)
{
	pread_profile().mutex_wait_ns.fetch_add(nanoseconds, std::memory_order_relaxed);
}

#if !defined(LUMEN_FEXCORE_RUNTIME) || (!defined(__APPLE__) && LUMEN_PLATFORM != LUMEN_PLATFORM_ANDROID)
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
using RegionAddress = mach_vm_address_t;
using RegionSize    = mach_vm_size_t;
#else
using RegionAddress = vm_address_t;
using RegionSize    = vm_size_t;
#endif

static kern_return_t query_region(RegionAddress* address, RegionSize* size, natural_t* depth, vm_region_recurse_info_t info,
                                  mach_msg_type_number_t* info_count)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	return mach_vm_region_recurse(mach_task_self(), address, size, depth, info, info_count);
#else
	return vm_region_recurse_64(mach_task_self(), address, size, depth, info, info_count);
#endif
}

static bool is_writable_range(const void* address, uint32_t size)
{
	if (size == 0)
	{
		return true;
	}

	const auto begin = reinterpret_cast<uintptr_t>(address);
	const auto end   = begin + static_cast<uintptr_t>(size);
	if (address == nullptr || end < begin)
	{
		return false;
	}

	auto current = static_cast<RegionAddress>(begin);
	while (current < end)
	{
		Core::VirtualMemory::ManagedRegion managed {};
		const auto managed_state = Core::VirtualMemory::QueryManagedRegion(current, &managed);
		if (managed_state != Core::VirtualMemory::ManagedRegionState::Unmanaged)
		{
			const auto mode = static_cast<uint32_t>(managed.mode);
			if (managed_state != Core::VirtualMemory::ManagedRegionState::Visible ||
			    managed.begin > current || managed.end <= current ||
			    (mode & static_cast<uint32_t>(Core::VirtualMemory::Mode::Write)) == 0)
			{
				return false;
			}
			current = (std::min)(static_cast<RegionAddress>(end), static_cast<RegionAddress>(managed.end));
			continue;
		}

		RegionAddress                   region_address = current;
		RegionSize                      region_size    = 0;
		natural_t                       depth          = 0;
		vm_region_submap_info_data_64_t info           = {};

		for (;;)
		{
			mach_msg_type_number_t info_count = VM_REGION_SUBMAP_INFO_COUNT_64;
			const auto result = query_region(&region_address, &region_size, &depth,
			                                 reinterpret_cast<vm_region_recurse_info_t>(&info), &info_count);
			if (result != KERN_SUCCESS)
			{
				return false;
			}
			if (info.is_submap == 0)
			{
				break;
			}
			depth++;
		}

		const auto region_end = region_address + region_size;
		if (region_address > current || region_end <= current || (info.protection & VM_PROT_WRITE) == 0)
		{
			return false;
		}

		current = (std::min)(static_cast<RegionAddress>(end), region_end);
	}

	return true;
}
#else
static bool is_writable_range(const void* address, uint32_t size)
{
	return address != nullptr || size == 0;
}
#endif
#endif

static ReadResult read_after_validation(Core::File& file, void* guest_buffer, uint32_t size, bool positioned = false,
                                        uint64_t offset = 0, PreadProfile* profile = nullptr)
{
	ReadResult result {};
	if (size == 0)
	{
		return result;
	}

	#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	// Validate the complete Darwin destination range first, then stage host I/O
	// so an I/O failure cannot partially update guest memory.
	constexpr uint32_t staging_limit = 1024 * 1024;
	const uint32_t     staging_size  = (std::min)(size, staging_limit);
	const auto         staging_start = profile != nullptr ? ProfileClock::now() : ProfileClock::time_point {};
	auto*              staging       = new uint8_t[staging_size];
	if (profile != nullptr)
	{
		profile->staging_ns.fetch_add(elapsed_ns(staging_start, ProfileClock::now()), std::memory_order_relaxed);
	}
	auto*              destination   = static_cast<uint8_t*>(guest_buffer);

	while (result.bytes_read < size)
	{
		const uint32_t request = (std::min)(size - result.bytes_read, staging_size);
		uint32_t       current = 0;
		const auto     read_start = profile != nullptr ? ProfileClock::now() : ProfileClock::time_point {};
		const bool     read_ok =
		    positioned ? file.ReadAt(staging, request, offset + result.bytes_read, &current) : (file.Read(staging, request, &current), true);
		if (profile != nullptr)
		{
			profile->host_read_ns.fetch_add(elapsed_ns(read_start, ProfileClock::now()), std::memory_order_relaxed);
		}
		if (!read_ok)
		{
			result.error = ReadError::ReadFailed;
			break;
		}
		if (current != 0)
		{
			const auto copy_start = profile != nullptr ? ProfileClock::now() : ProfileClock::time_point {};
			std::memcpy(destination + result.bytes_read, staging, current);
			if (profile != nullptr)
			{
				profile->memcpy_ns.fetch_add(elapsed_ns(copy_start, ProfileClock::now()), std::memory_order_relaxed);
			}
			result.bytes_read += current;
		}
		if (current < request)
		{
			break;
		}
	}

	const auto staging_end_start = profile != nullptr ? ProfileClock::now() : ProfileClock::time_point {};
	delete[] staging;
	if (profile != nullptr)
	{
		profile->staging_ns.fetch_add(elapsed_ns(staging_end_start, ProfileClock::now()), std::memory_order_relaxed);
	}
#else
	if (positioned)
	{
		if (!file.ReadAt(guest_buffer, size, offset, &result.bytes_read))
		{
			result.error = ReadError::ReadFailed;
		}
	} else
	{
		file.Read(guest_buffer, size, &result.bytes_read);
	}
#endif

	return result;
}

static ReadResult read_validated(Core::File& file, void* guest_buffer, uint32_t size)
{
#if defined(LUMEN_FEXCORE_RUNTIME) && (defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID)
	Core::VirtualMemory::GuestDataLease lease;
	if (size != 0 && !Core::VirtualMemory::TryAcquireGuestDataLease(
	        reinterpret_cast<uint64_t>(guest_buffer), size, Core::VirtualMemory::Mode::Write, &lease))
#else
	if (!is_writable_range(guest_buffer, size))
#endif
	{
		return {0, ReadError::InvalidAddress};
	}
	return read_after_validation(file, guest_buffer, size);
}

ReadResult Read(Core::File& file, void* guest_buffer, uint32_t size)
{
	return read_validated(file, guest_buffer, size);
}

ReadResult Pread(Core::File& file, void* guest_buffer, uint32_t size, uint64_t offset)
{
	auto*      profile    = PreadProfileEnabled() ? &pread_profile() : nullptr;
	const auto start      = profile != nullptr ? ProfileClock::now() : ProfileClock::time_point {};
	const auto scan_start = start;
#if defined(LUMEN_FEXCORE_RUNTIME) && (defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID)
	Core::VirtualMemory::GuestDataLease lease;
	if (size != 0 && !Core::VirtualMemory::TryAcquireGuestDataLease(
	        reinterpret_cast<uint64_t>(guest_buffer), size, Core::VirtualMemory::Mode::Write, &lease))
#else
	if (!is_writable_range(guest_buffer, size))
#endif
	{
		if (profile != nullptr)
		{
			profile->validation_ns.fetch_add(elapsed_ns(scan_start, ProfileClock::now()), std::memory_order_relaxed);
		}
		return {0, ReadError::InvalidAddress};
	}
	const auto scan_end = profile != nullptr ? ProfileClock::now() : ProfileClock::time_point {};
	if (profile != nullptr)
	{
		profile->validation_ns.fetch_add(elapsed_ns(scan_start, scan_end), std::memory_order_relaxed);
	}

	// Read through ordinary staging memory at an explicit host-file offset. The
	// guest destination was validated above and the file cursor is never changed,
	// including for short reads and EOF.
	auto result = read_after_validation(file, guest_buffer, size, true, offset, profile);
	if (profile != nullptr)
	{
		const auto end = ProfileClock::now();
		profile->bytes.fetch_add(result.bytes_read, std::memory_order_relaxed);
		profile->total_ns.fetch_add(elapsed_ns(start, end), std::memory_order_relaxed);
		const uint64_t calls = profile->calls.fetch_add(1, std::memory_order_relaxed) + 1;
		if (calls % profile->interval == 0)
		{
			report_pread_profile(profile->interval);
		}
	}
	return result;
}

static WriteResult write_validated(Core::File& file, const void* guest_buffer,
                                   uint32_t size, bool positioned, uint64_t offset)
{
	if (size == 0) return {};
	const auto address = reinterpret_cast<uint64_t>(guest_buffer);
	if (address == 0 || address > UINT64_MAX - size) return {0, WriteError::InvalidAddress};
#if defined(LUMEN_FEXCORE_RUNTIME) && (defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID)
	Core::VirtualMemory::GuestDataLease lease;
	if (!Core::VirtualMemory::TryAcquireGuestDataLease(
	        address, size, Core::VirtualMemory::Mode::Read, &lease))
	{
		return {0, WriteError::InvalidAddress};
	}
#endif
	WriteResult result {};
	if (!positioned)
	{
		file.Write(guest_buffer, size, &result.bytes_written);
		return result;
	}
	const auto previous = file.Tell();
	if (!file.Seek(offset)) return {0, WriteError::SeekFailed};
	file.Write(guest_buffer, size, &result.bytes_written);
	if (!file.Seek(previous)) result.error = WriteError::SeekFailed;
	return result;
}

WriteResult Write(Core::File& file, const void* guest_buffer, uint32_t size)
{
	return write_validated(file, guest_buffer, size, false, 0);
}

WriteResult Pwrite(Core::File& file, const void* guest_buffer, uint32_t size, uint64_t offset)
{
	return write_validated(file, guest_buffer, size, true, offset);
}

} // namespace Lumen::Libs::GuestFile

#endif // LUMEN_EMU_ENABLED
