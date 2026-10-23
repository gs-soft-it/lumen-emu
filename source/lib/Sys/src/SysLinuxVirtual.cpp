#include "Lumen/Core/Common.h"

#if LUMEN_PLATFORM != LUMEN_PLATFORM_ANDROID && LUMEN_PLATFORM != LUMEN_PLATFORM_LINUX && LUMEN_PLATFORM != LUMEN_PLATFORM_OSX && \
    LUMEN_PLATFORM != LUMEN_PLATFORM_IOS
//#error "LUMEN_PLATFORM != LUMEN_PLATFORM_LINUX"
#else

#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/String.h"
#include "Lumen/Core/VirtualMemory.h"
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
#include "Lumen/Core/VirtualMemoryImmutableSnapshot.h"
#endif
#include "Lumen/Sys/SysVirtual.h"

#include "cpuinfo.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

// IWYU pragma: no_include <asm/mman-common.h>
// IWYU pragma: no_include <asm/mman.h>
// IWYU pragma: no_include <bits/pthread_types.h>
// IWYU pragma: no_include <linux/mman.h>

#if defined(MAP_FIXED_NOREPLACE) && LUMEN_PLATFORM == LUMEN_PLATFORM_LINUX
#define LUMEN_FIXED_NOREPLACE
#endif

namespace Lumen::Core {

static pthread_mutex_t              g_virtual_mutex {};
static std::map<uintptr_t, size_t>* g_allocs   = nullptr;
static std::map<uintptr_t, int>*    g_protects = nullptr;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
static std::map<uintptr_t, uint64_t>* g_backing_ids = nullptr;
static uint64_t g_next_backing_id = 1;
static VirtualMemory::ImmutableSnapshot::Publisher g_immutable_snapshot {};
namespace {
bool publish_android_full_snapshot_locked();
}
#endif

void sys_get_system_info(SystemInfo* info)
{
	EXIT_IF(info == nullptr);

	const auto* p = cpuinfo_get_package(0);

	EXIT_IF(p == nullptr);

	info->ProcessorName = String::FromUtf8(p->name);
}

void sys_virtual_init()
{
	pthread_mutexattr_t attr {};

	pthread_mutexattr_init(&attr);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_LINUX
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_FAST_NP);
#else
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL);
#endif
	pthread_mutex_init(&g_virtual_mutex, &attr);
	pthread_mutexattr_destroy(&attr);

	g_allocs   = new std::map<uintptr_t, size_t>;
	g_protects = new std::map<uintptr_t, int>;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	g_backing_ids = new std::map<uintptr_t, uint64_t>;
#endif

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	if (VirtualMemory::ImmutableSnapshot::Requested())
	{
		pthread_mutex_lock(&g_virtual_mutex);
		(void)publish_android_full_snapshot_locked();
		pthread_mutex_unlock(&g_virtual_mutex);
	}
#endif

	cpuinfo_initialize();
}

static int get_protection_flag(VirtualMemory::Mode mode)
{
	int protect = PROT_NONE;
	switch (mode)
	{
	#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
		case VirtualMemory::Mode::Execute:
		case VirtualMemory::Mode::ExecuteRead: protect = PROT_READ; break;
		case VirtualMemory::Mode::ExecuteWrite:
		case VirtualMemory::Mode::ExecuteReadWrite: protect = PROT_READ | PROT_WRITE; break; // NOLINT
	#else
		case VirtualMemory::Mode::Execute: protect = PROT_EXEC; break;
		case VirtualMemory::Mode::ExecuteRead: protect = PROT_EXEC | PROT_READ; break; // NOLINT
		case VirtualMemory::Mode::ExecuteWrite:
		case VirtualMemory::Mode::ExecuteReadWrite: protect = PROT_EXEC | PROT_WRITE | PROT_READ; break; // NOLINT
	#endif
		case VirtualMemory::Mode::Read: protect = PROT_READ; break;
		case VirtualMemory::Mode::Write:
		case VirtualMemory::Mode::ReadWrite: protect = PROT_READ | PROT_WRITE; break; // NOLINT
		case VirtualMemory::Mode::NoAccess:
		default: protect = PROT_NONE; break;
	}
	return protect;
}

static int get_tracked_protection_flag(VirtualMemory::Mode mode)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	// Android guest-code mappings deliberately omit host PROT_EXEC: x86 bytes
	// are translated, never entered by the AArch64 CPU. Keep the requested
	// guest protection separately so QueryManagedRegion and old_mode still
	// describe executable guest ranges accurately.
	switch (mode)
	{
		case VirtualMemory::Mode::NoAccess: return PROT_NONE;
		case VirtualMemory::Mode::Read: return PROT_READ;
		case VirtualMemory::Mode::Write: return PROT_WRITE;
		case VirtualMemory::Mode::ReadWrite: return PROT_READ | PROT_WRITE; // NOLINT
		case VirtualMemory::Mode::Execute: return PROT_EXEC;
		case VirtualMemory::Mode::ExecuteRead: return PROT_EXEC | PROT_READ; // NOLINT
		case VirtualMemory::Mode::ExecuteWrite: return PROT_EXEC | PROT_WRITE; // NOLINT
		case VirtualMemory::Mode::ExecuteReadWrite: return PROT_EXEC | PROT_READ | PROT_WRITE; // NOLINT
		default: return PROT_NONE;
	}
#else
	return get_protection_flag(mode);
#endif
}

static VirtualMemory::Mode get_protection_flag(int mode)
{
	switch (mode)
	{
		case PROT_NONE: return VirtualMemory::Mode::NoAccess;
		case PROT_READ: return VirtualMemory::Mode::Read;
		case PROT_WRITE: return VirtualMemory::Mode::Write;
		case PROT_READ | PROT_WRITE: return VirtualMemory::Mode::ReadWrite; // NOLINT
		case PROT_EXEC: return VirtualMemory::Mode::Execute;
		case PROT_EXEC | PROT_WRITE: return VirtualMemory::Mode::ExecuteWrite;                 // NOLINT
		case PROT_EXEC | PROT_READ: return VirtualMemory::Mode::ExecuteRead;                   // NOLINT
		case PROT_EXEC | PROT_WRITE | PROT_READ: return VirtualMemory::Mode::ExecuteReadWrite; // NOLINT
		default: return VirtualMemory::Mode::NoAccess;
	}
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
namespace {

constexpr uintptr_t SNAPSHOT_PAGE_SIZE = 4096;
constexpr uintptr_t SNAPSHOT_PRECEDING_PAGE_LIMIT = 127;
constexpr uintptr_t SNAPSHOT_FOLLOWING_PAGE_LIMIT = 128;

// Match the guest address windows enforced by the Windows backend. Android's
// 48-bit userspace otherwise places anonymous mappings above the range
// accepted by guest allocators even though mmap succeeded.
constexpr uintptr_t GUEST_SYSTEM_MANAGED_MIN = 0x0000040000u;
constexpr uintptr_t GUEST_SYSTEM_MANAGED_MAX = 0x07FFFFBFFFu;
constexpr uintptr_t GUEST_USER_MIN           = 0x1000000000u;
constexpr uintptr_t GUEST_USER_MAX           = 0xFBFFFFFFFFu;
// Guest libc treats lower bases as an invalid mspace. Explicit loader hints
// may still use the complete system-managed window, but unconstrained
// allocations must never publish a base that guest allocators reject.
constexpr uintptr_t GUEST_ANYWHERE_MIN       = 0x0000400000u;
static pthread_mutex_t g_android_guest_mapping_mutex = PTHREAD_MUTEX_INITIALIZER;

bool android_guest_range_contains(uintptr_t begin, size_t size,
	                              uintptr_t minimum, uintptr_t maximum)
{
	return size != 0 && begin >= minimum && begin <= maximum &&
	       size - 1 <= maximum - begin;
}

bool android_guest_anywhere_range(uintptr_t begin, size_t size)
{
	return android_guest_range_contains(
	           begin, size, GUEST_SYSTEM_MANAGED_MIN,
	           GUEST_SYSTEM_MANAGED_MAX) ||
	       android_guest_range_contains(
	           begin, size, GUEST_USER_MIN, GUEST_USER_MAX);
}

bool android_align_up(uintptr_t value, uintptr_t alignment,
	                  uintptr_t* result)
{
	if (result == nullptr || alignment == 0 ||
	    value > (std::numeric_limits<uintptr_t>::max)() - (alignment - 1))
	{
		return false;
	}
	*result = (value + alignment - 1) & ~(alignment - 1);
	return true;
}

void* android_mmap_exact_hint(uintptr_t address, size_t size,
	                         int protection, int flags)
{
	void* mapping = mmap(reinterpret_cast<void*>(address), size,
	                     protection, flags | MAP_FIXED_NOREPLACE,
	                     -1, 0); // NOLINT
	if (mapping == MAP_FAILED && errno == EINVAL)
	{
		// Kernels predating MAP_FIXED_NOREPLACE may reject the flag. A normal
		// hint never replaces an existing VMA; accept this fallback only when
		// the kernel selected the exact candidate.
		mapping = mmap(reinterpret_cast<void*>(address), size,
		               protection, flags, -1, 0); // NOLINT
	}
	if (mapping == MAP_FAILED) return MAP_FAILED;
	if (reinterpret_cast<uintptr_t>(mapping) == address) return mapping;
	munmap(mapping, size);
	errno = EEXIST;
	return MAP_FAILED;
}

bool android_find_guest_gap(uintptr_t minimum, uintptr_t maximum,
	                        size_t mapped_size, uintptr_t alignment,
	                        uintptr_t* result)
{
	uintptr_t candidate = 0;
	if (result == nullptr ||
	    !android_align_up(minimum, alignment, &candidate))
	{
		return false;
	}

	FILE* const maps = fopen("/proc/self/maps", "r");
	if (maps == nullptr) return false;

	char line[1024] {};
	while (fgets(line, sizeof(line), maps) != nullptr)
	{
		uint64_t raw_begin = 0;
		uint64_t raw_end   = 0;
		if (sscanf(line, "%" SCNx64 "-%" SCNx64,
		           &raw_begin, &raw_end) != 2 || raw_end <= candidate)
		{
			continue;
		}
		if (raw_begin > maximum) break;

		const auto mapping_begin = static_cast<uintptr_t>(raw_begin);
		const auto mapping_end   = static_cast<uintptr_t>(raw_end);
		if (mapping_begin > candidate &&
		    mapped_size <= mapping_begin - candidate)
		{
			break;
		}
		if (mapping_end > candidate &&
		    !android_align_up(mapping_end, alignment, &candidate))
		{
			candidate = 0;
			break;
		}
	}
	const bool found = android_guest_range_contains(
	    candidate, mapped_size, minimum, maximum);
	fclose(maps);
	if (found) *result = candidate;
	return found;
}

uintptr_t android_host_page_size()
{
	static const uintptr_t size = []
	{
		const long detected = sysconf(_SC_PAGESIZE);
		return detected == 4096 || detected == 16384
		           ? static_cast<uintptr_t>(detected) : uintptr_t {0};
	}();
	return size;
}

void* android_mmap_guest_window(uintptr_t preferred, size_t size,
	                           uint64_t alignment, int protection, int flags)
{
	const uintptr_t host_page_size = android_host_page_size();
	if (host_page_size == 0 || alignment == 0 ||
	    alignment > (std::numeric_limits<uintptr_t>::max)())
	{
		return MAP_FAILED;
	}
	const uintptr_t effective_alignment =
	    static_cast<uintptr_t>(alignment) > host_page_size
	        ? static_cast<uintptr_t>(alignment) : host_page_size;
	if (size > (std::numeric_limits<size_t>::max)() -
	               (host_page_size - 1))
	{
		return MAP_FAILED;
	}
	const size_t mapped_size =
	    (size + host_page_size - 1) & ~(host_page_size - 1);

	pthread_mutex_lock(&g_android_guest_mapping_mutex);
	struct Window
	{
		uintptr_t minimum;
		uintptr_t maximum;
	};
	std::array<Window, 2> windows {};
	size_t window_count = 0;
	if (preferred == 0)
	{
		windows[window_count++] = {
		    GUEST_ANYWHERE_MIN, GUEST_SYSTEM_MANAGED_MAX};
		windows[window_count++] = {GUEST_USER_MIN, GUEST_USER_MAX};
	} else if (android_guest_range_contains(
	               preferred, 1, GUEST_SYSTEM_MANAGED_MIN,
	               GUEST_SYSTEM_MANAGED_MAX))
	{
		windows[window_count++] = {preferred, GUEST_SYSTEM_MANAGED_MAX};
		windows[window_count++] = {GUEST_USER_MIN, GUEST_USER_MAX};
	} else
	{
		windows[window_count++] = {preferred, GUEST_USER_MAX};
	}

	for (size_t index = 0; index < window_count; index++)
	{
		const auto& window = windows[index];
		for (;;)
		{
			uintptr_t candidate = 0;
			if (!android_find_guest_gap(
			        window.minimum, window.maximum, mapped_size,
			        effective_alignment, &candidate))
			{
				break;
			}
			void* const mapping = android_mmap_exact_hint(
			    candidate, size, protection, flags);
			if (mapping != MAP_FAILED)
			{
				pthread_mutex_unlock(&g_android_guest_mapping_mutex);
				return mapping;
			}
			if (errno != EEXIST)
			{
				break;
			}
		}
	}
	pthread_mutex_unlock(&g_android_guest_mapping_mutex);
	return MAP_FAILED;
}

bool valid_android_mapping(uint64_t address, uint64_t size,
	                       VirtualMemory::Mode mode)
{
	const uintptr_t host_page_size = android_host_page_size();
	return static_cast<uint32_t>(mode) <=
	           static_cast<uint32_t>(VirtualMemory::Mode::ExecuteReadWrite) &&
	       host_page_size != 0 && size != 0 &&
	       size <= (std::numeric_limits<size_t>::max)() - (host_page_size - 1) &&
	       address <= (std::numeric_limits<uintptr_t>::max)() - size;
}

uintptr_t android_mapping_end(uintptr_t begin, size_t size)
{
	const uintptr_t mask = android_host_page_size() - 1;
	return begin + ((size + mask) & ~mask);
}

class AndroidSnapshotMutation final
{
public:
	explicit AndroidSnapshotMutation(bool enabled = true): m_enabled(enabled)
	{
		if (m_enabled) g_immutable_snapshot.BeginMutation();
	}
	~AndroidSnapshotMutation()
	{
		Finish();
	}

	void Finish()
	{
		if (m_enabled) g_immutable_snapshot.EndMutation();
		m_enabled = false;
	}

	AndroidSnapshotMutation(const AndroidSnapshotMutation&) = delete;
	AndroidSnapshotMutation& operator=(const AndroidSnapshotMutation&) = delete;

private:
	bool m_enabled = false;
};

bool add_without_overflow(uintptr_t left, size_t right, uintptr_t* result)
{
	if (result == nullptr || right > (std::numeric_limits<uintptr_t>::max)() - left)
	{
		return false;
	}
	*result = left + static_cast<uintptr_t>(right);
	return true;
}

void clip_android_snapshot_run(uint64_t address,
	                           VirtualMemory::ManagedRegion* region)
{
	(void)VirtualMemory::ImmutableSnapshot::ClipRegionToPageWindow(
	    address, SNAPSHOT_PAGE_SIZE, SNAPSHOT_PRECEDING_PAGE_LIMIT,
	    SNAPSHOT_FOLLOWING_PAGE_LIMIT, region);
}

bool append_android_mapping_snapshot(
	uintptr_t owner, size_t size,
	VirtualMemory::ImmutableSnapshot::Builder& builder)
{
	uintptr_t allocation_end = 0;
	if (owner == 0 || size == 0 ||
	    !add_without_overflow(owner, size, &allocation_end))
	{
		return false;
	}

	const uintptr_t first_page = owner / SNAPSHOT_PAGE_SIZE;
	const uintptr_t last_page = (allocation_end - 1) / SNAPSHOT_PAGE_SIZE;
	uintptr_t page = first_page;
	while (page <= last_page)
	{
		const auto protection_entry = g_protects->find(page);
		const int protection = protection_entry == g_protects->end()
		                           ? PROT_NONE
		                           : protection_entry->second;
		uintptr_t run_last = page;
		while (run_last < last_page)
		{
			const auto next = g_protects->find(run_last + 1);
			const int next_protection =
			    next == g_protects->end() ? PROT_NONE : next->second;
			if (next_protection != protection) break;
			run_last++;
		}
		const uintptr_t run_begin =
		    page == first_page ? owner : page * SNAPSHOT_PAGE_SIZE;
		const uintptr_t run_end = run_last == last_page
		                              ? allocation_end
		                              : (run_last + 1) * SNAPSHOT_PAGE_SIZE;
		if (!builder.Append({run_begin, run_end, owner,
		                     VirtualMemory::ManagedRegionState::Visible,
		                     get_protection_flag(protection)}))
		{
			return false;
		}
		if (run_last == (std::numeric_limits<uintptr_t>::max)()) break;
		page = run_last + 1;
	}
	const uintptr_t physical_end = android_mapping_end(owner, size);
	return allocation_end == physical_end ||
	       builder.Append({allocation_end, physical_end, owner,
	                       VirtualMemory::ManagedRegionState::Reserved,
	                       VirtualMemory::Mode::NoAccess});
}

bool publish_android_full_snapshot_locked()
{
	if (!VirtualMemory::ImmutableSnapshot::Requested()) return true;
	VirtualMemory::ImmutableSnapshot::Builder builder;
	for (const auto& allocation: *g_allocs)
	{
		if (!append_android_mapping_snapshot(allocation.first, allocation.second,
		                                     builder))
		{
			g_immutable_snapshot.Invalidate();
			return false;
		}
	}
	return g_immutable_snapshot.Publish(builder);
}

bool publish_android_registered_mapping_locked(uintptr_t owner, size_t size,
	                                           VirtualMemory::Mode mode)
{
	if (!VirtualMemory::ImmutableSnapshot::Requested()) return true;
	size_t current_count = 0;
	const auto* current =
	    g_immutable_snapshot.CurrentEntriesForWriter(&current_count);
	if (current == nullptr) return publish_android_full_snapshot_locked();
	uintptr_t end = 0;
	if (!add_without_overflow(owner, size, &end) ||
	    current_count > (std::numeric_limits<size_t>::max)() - 2)
	{
		g_immutable_snapshot.Invalidate();
		return false;
	}
	VirtualMemory::ImmutableSnapshot::Builder builder;
	const uintptr_t physical_end = android_mapping_end(owner, size);
	if (!builder.Reserve(current_count + 2) ||
	    !builder.AppendAll(current, current_count) ||
	    !builder.Append({owner, end, owner,
	                     VirtualMemory::ManagedRegionState::Visible, mode}) ||
	    (end != physical_end &&
	     !builder.Append({end, physical_end, owner,
	                      VirtualMemory::ManagedRegionState::Reserved,
	                      VirtualMemory::Mode::NoAccess})))
	{
		g_immutable_snapshot.Invalidate();
		return false;
	}
	return g_immutable_snapshot.Publish(builder);
}

bool publish_android_protection_locked(uintptr_t begin, uintptr_t end,
	                                   VirtualMemory::Mode mode)
{
	if (!VirtualMemory::ImmutableSnapshot::Requested()) return true;
	size_t current_count = 0;
	const auto* current =
	    g_immutable_snapshot.CurrentEntriesForWriter(&current_count);
	if (current == nullptr) return publish_android_full_snapshot_locked();
	if (begin >= end || current_count > (std::numeric_limits<size_t>::max)() - 2)
	{
		g_immutable_snapshot.Invalidate();
		return false;
	}

	VirtualMemory::ImmutableSnapshot::Builder builder;
	if (!builder.Reserve(current_count + 2))
	{
		g_immutable_snapshot.Invalidate();
		return false;
	}
	bool replaced = false;
	for (size_t index = 0; index < current_count; index++)
	{
		const auto& entry = current[index];
		if (entry.state != VirtualMemory::ManagedRegionState::Visible ||
		    entry.end <= begin || entry.begin >= end)
		{
			if (!builder.Append(entry))
			{
				g_immutable_snapshot.Invalidate();
				return false;
			}
			continue;
		}
		replaced = true;
		if (entry.begin < begin &&
		    !builder.Append({entry.begin, begin, entry.owner, entry.state,
		                     entry.mode}))
		{
			g_immutable_snapshot.Invalidate();
			return false;
		}
		const uintptr_t overlap_begin = entry.begin > begin ? entry.begin : begin;
		const uintptr_t overlap_end = entry.end < end ? entry.end : end;
		if (!builder.Append({overlap_begin, overlap_end, entry.owner,
		                     VirtualMemory::ManagedRegionState::Visible, mode}))
		{
			g_immutable_snapshot.Invalidate();
			return false;
		}
		if (entry.end > end &&
		    !builder.Append({end, entry.end, entry.owner, entry.state,
		                     entry.mode}))
		{
			g_immutable_snapshot.Invalidate();
			return false;
		}
	}
	return replaced ? g_immutable_snapshot.Publish(builder)
	                : publish_android_full_snapshot_locked();
}

bool publish_android_freed_mapping_locked(uintptr_t owner)
{
	if (!VirtualMemory::ImmutableSnapshot::Requested()) return true;
	size_t current_count = 0;
	const auto* current =
	    g_immutable_snapshot.CurrentEntriesForWriter(&current_count);
	if (current == nullptr) return publish_android_full_snapshot_locked();
	VirtualMemory::ImmutableSnapshot::Builder builder;
	if (!builder.Reserve(current_count))
	{
		g_immutable_snapshot.Invalidate();
		return false;
	}
	bool removed = false;
	for (size_t index = 0; index < current_count; index++)
	{
		if (current[index].owner == owner)
		{
			removed = true;
			continue;
		}
		if (!builder.Append(current[index]))
		{
			g_immutable_snapshot.Invalidate();
			return false;
		}
	}
	return removed ? g_immutable_snapshot.Publish(builder)
	               : publish_android_full_snapshot_locked();
}

} // namespace
#endif

uint64_t sys_virtual_alloc(uint64_t address, uint64_t size, VirtualMemory::Mode mode)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	if (!valid_android_mapping(address, size, mode)) return 0;
	if (address == 0 || android_guest_anywhere_range(
	                        static_cast<uintptr_t>(address), 1))
	{
		return sys_virtual_alloc_aligned(address, size, mode, 1);
	}
#endif
	EXIT_IF(g_allocs == nullptr);

	auto addr = static_cast<uintptr_t>(address);

	int protect         = get_protection_flag(mode);
	int tracked_protect = get_tracked_protection_flag(mode);

	void* ptr = mmap(reinterpret_cast<void*>(addr), size, protect, MAP_PRIVATE | MAP_ANON, -1, 0); // NOLINT

	auto ret_addr = reinterpret_cast<uintptr_t>(ptr);

	if (ptr != MAP_FAILED)
	{
		pthread_mutex_lock(&g_virtual_mutex);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
		{
			AndroidSnapshotMutation snapshot_mutation(
			    VirtualMemory::ImmutableSnapshot::Requested());
#endif
			(*g_allocs)[ret_addr] = size;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
			if (g_next_backing_id >= (UINT64_C(1) << 62)) std::abort();
			(*g_backing_ids)[ret_addr] = g_next_backing_id++;
#endif
			uintptr_t page_start  = ret_addr >> 12u;
			uintptr_t page_end    = (ret_addr + size - 1) >> 12u;
			for (uintptr_t page = page_start; page <= page_end; page++)
			{
				(*g_protects)[page] = tracked_protect;
			}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
			(void)publish_android_registered_mapping_locked(
			    ret_addr, size, mode);
		}
#endif
		pthread_mutex_unlock(&g_virtual_mutex);
	}

	return ptr == MAP_FAILED ? 0 : ret_addr;
}

static uintptr_t align_up(uintptr_t addr, uint64_t alignment)
{
	return (addr + alignment - 1) & ~(alignment - 1);
}

uint64_t sys_virtual_alloc_aligned(uint64_t address, uint64_t size, VirtualMemory::Mode mode, uint64_t alignment)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	if (!valid_android_mapping(address, size, mode)) return 0;
	const bool guest_window_request =
	    address == 0 || android_guest_anywhere_range(
	                        static_cast<uintptr_t>(address), 1);
#endif
	if (size == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0 ||
	    address > (std::numeric_limits<uintptr_t>::max)() || size > (std::numeric_limits<size_t>::max)())
	{
		return 0;
	}

	EXIT_IF(g_allocs == nullptr);

	auto addr            = static_cast<uintptr_t>(address);
	int  protect         = get_protection_flag(mode);
	int  tracked_protect = get_tracked_protection_flag(mode);

	void* ptr = nullptr;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	if (guest_window_request)
	{
		ptr = android_mmap_guest_window(
		    addr, static_cast<size_t>(size), alignment, protect,
		    MAP_PRIVATE | MAP_ANON);
	} else
#endif
	{
		ptr = mmap(reinterpret_cast<void*>(addr), static_cast<size_t>(size),
		           protect, MAP_PRIVATE | MAP_ANON, -1, 0); // NOLINT
	}

	auto ret_addr = reinterpret_cast<uintptr_t>(ptr);

	if (ptr != MAP_FAILED && ((ret_addr & (alignment - 1)) != 0))
	{
		if (munmap(ptr, static_cast<size_t>(size)) != 0)
		{
			return 0;
		}
		ptr = MAP_FAILED;
	}

	if (ptr == MAP_FAILED)
	{
		const long raw_page_size = sysconf(_SC_PAGESIZE);
		if (raw_page_size <= 0) return 0;
		const auto page_size = static_cast<uint64_t>(raw_page_size);
		if ((page_size & (page_size - 1)) != 0 || size > (std::numeric_limits<uint64_t>::max)() - (page_size - 1)) return 0;

		const uint64_t mapped_size = (size + page_size - 1) & ~(page_size - 1);
		if (mapped_size > (std::numeric_limits<size_t>::max)() ||
		    alignment > (std::numeric_limits<size_t>::max)() - mapped_size)
		{
			return 0;
		}
		const auto reservation_size = static_cast<size_t>(mapped_size + alignment);
		void* reservation = nullptr;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
		if (guest_window_request)
		{
			reservation = android_mmap_guest_window(
			    addr, reservation_size, alignment, protect,
			    MAP_PRIVATE | MAP_ANON | MAP_NORESERVE);
		} else
#endif
		{
			reservation = mmap(reinterpret_cast<void*>(addr), reservation_size,
			                   protect, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE,
			                   -1, 0); // NOLINT
		}
		if (reservation == MAP_FAILED) return 0;

		const auto reservation_addr = reinterpret_cast<uintptr_t>(reservation);
		if (reservation_addr > (std::numeric_limits<uintptr_t>::max)() - (alignment - 1))
		{
			munmap(reservation, reservation_size);
			return 0;
		}
		const uintptr_t aligned_addr = align_up(reservation_addr, alignment);
		const size_t prefix_size = static_cast<size_t>(aligned_addr - reservation_addr);
		const size_t suffix_size = reservation_size - prefix_size - static_cast<size_t>(mapped_size);
		if ((prefix_size != 0 && munmap(reservation, prefix_size) != 0) ||
		    (suffix_size != 0 &&
		     munmap(reinterpret_cast<void*>(aligned_addr + mapped_size), suffix_size) != 0))
		{
			// If prefix trimming succeeded, aligned_addr is the first address
			// still owned by this reservation. Unmapping through the old end
			// also cleans up a failed suffix trim without replacing any mapping.
			munmap(reinterpret_cast<void*>(prefix_size == 0 ? reservation_addr : aligned_addr),
			       reservation_size - prefix_size);
			return 0;
		}
		ptr      = reinterpret_cast<void*>(aligned_addr);
		ret_addr = aligned_addr;
	}

	if (ret_addr == 0 || ret_addr > (std::numeric_limits<uintptr_t>::max)() - (size - 1))
	{
		munmap(ptr, static_cast<size_t>(size));
		return 0;
	}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	if (guest_window_request &&
	    !android_guest_anywhere_range(ret_addr, static_cast<size_t>(size)))
	{
		munmap(ptr, static_cast<size_t>(size));
		return 0;
	}
#endif

	pthread_mutex_lock(&g_virtual_mutex);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	{
		AndroidSnapshotMutation snapshot_mutation(
		    VirtualMemory::ImmutableSnapshot::Requested());
#endif
		(*g_allocs)[ret_addr] = size;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
		if (g_next_backing_id >= (UINT64_C(1) << 62)) std::abort();
		(*g_backing_ids)[ret_addr] = g_next_backing_id++;
#endif
		uintptr_t page_start  = ret_addr >> 12u;
		uintptr_t page_end    = (ret_addr + size - 1) >> 12u;
		for (uintptr_t page = page_start; page <= page_end; page++)
		{
			(*g_protects)[page] = tracked_protect;
		}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
		(void)publish_android_registered_mapping_locked(
		    ret_addr, size, mode);
	}
#endif
	pthread_mutex_unlock(&g_virtual_mutex);

	return ret_addr;
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_LINUX && !defined(LUMEN_FIXED_NOREPLACE)
static bool is_mmaped(void* ptr, size_t length)
{
	FILE* file = fopen("/proc/self/maps", "r");
	char  line[1024];
	bool  ret  = false;
	auto  addr = reinterpret_cast<uintptr_t>(ptr);

	[[maybe_unused]] int result = 0;

	while (feof(file) == 0)
	{
		if (fgets(line, 1024, file) == nullptr)
		{
			break;
		}
		uint64_t start = 0;
		uint64_t end   = 0;
		// NOLINTNEXTLINE(cert-err34-c)
		if (sscanf(line, "%" SCNx64 "-%" SCNx64, &start, &end) != 2)
		{
			continue;
		}
		if (addr >= start && addr + length <= end)
		{
			ret = true;
			break;
		}
	}
	result = fclose(file);
	return ret;
}
#endif

bool sys_virtual_alloc_fixed(uint64_t address, uint64_t size, VirtualMemory::Mode mode)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	if (!valid_android_mapping(address, size, mode) || address == 0 ||
	    (address & (android_host_page_size() - 1)) != 0) return false;
#endif
	EXIT_IF(g_allocs == nullptr);

	auto addr            = static_cast<uintptr_t>(address);
	int  protect         = get_protection_flag(mode);
	int  tracked_protect = get_tracked_protection_flag(mode);

#ifdef LUMEN_FIXED_NOREPLACE
	// NOLINTNEXTLINE
	void* ptr = mmap(reinterpret_cast<void*>(addr), size, protect, MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANON, -1, 0);
#elif LUMEN_PLATFORM == LUMEN_PLATFORM_LINUX
	// NOLINTNEXTLINE
	void* ptr = (is_mmaped(reinterpret_cast<void*>(addr), size)
	                 ? MAP_FAILED
	                 : mmap(reinterpret_cast<void*>(addr), size, protect, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0));
#else
	// Darwin has no MAP_FIXED_NOREPLACE. Treat the requested address as a hint and
	// reject a different result instead of replacing an existing process mapping.
	void* ptr = mmap(reinterpret_cast<void*>(addr), size, protect, MAP_PRIVATE | MAP_ANON, -1, 0);
#endif

	auto ret_addr = reinterpret_cast<uintptr_t>(ptr);

	if (ptr != MAP_FAILED && ret_addr != addr)
	{
		munmap(ptr, size);
		ret_addr = 0;
		ptr      = MAP_FAILED;
	}

	if (ptr != MAP_FAILED)
	{
		pthread_mutex_lock(&g_virtual_mutex);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
		{
			AndroidSnapshotMutation snapshot_mutation(
			    VirtualMemory::ImmutableSnapshot::Requested());
#endif
			(*g_allocs)[ret_addr] = size;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
			if (g_next_backing_id >= (UINT64_C(1) << 62)) std::abort();
			(*g_backing_ids)[ret_addr] = g_next_backing_id++;
#endif
			uintptr_t page_start  = ret_addr >> 12u;
			uintptr_t page_end    = (ret_addr + size - 1) >> 12u;
			for (uintptr_t page = page_start; page <= page_end; page++)
			{
				(*g_protects)[page] = tracked_protect;
			}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
			(void)publish_android_registered_mapping_locked(
			    ret_addr, size, mode);
		}
#endif
		pthread_mutex_unlock(&g_virtual_mutex);

		return true;
	}

	return false;
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
bool sys_virtual_requires_uniform_host_page_data_protection()
{
	return android_host_page_size() > SNAPSHOT_PAGE_SIZE;
}

static VirtualMemory::ManagedRegionState query_android_managed_region(uint64_t address, uint64_t requested_end,
                                                                     VirtualMemory::ManagedRegion* region)
{
	if (region != nullptr)
	{
		*region = VirtualMemory::ManagedRegion {0, 0, VirtualMemory::Mode::NoAccess};
	}
	if (requested_end != 0 && requested_end <= address)
	{
		return VirtualMemory::ManagedRegionState::Unmanaged;
	}
	if (VirtualMemory::ImmutableSnapshot::Requested())
	{
		VirtualMemory::ManagedRegionState snapshot_state =
		    VirtualMemory::ManagedRegionState::Unmanaged;
		if (g_immutable_snapshot.Lookup(address, &snapshot_state, region, true))
		{
			if (snapshot_state == VirtualMemory::ManagedRegionState::Visible)
			{
				if (requested_end == 0)
				{
					clip_android_snapshot_run(address, region);
				} else if (region != nullptr)
				{
					region->begin = address;
					if (region->end > requested_end) region->end = requested_end;
				}
			}
			return snapshot_state;
		}
	}
	if (address == 0 || g_allocs == nullptr || g_protects == nullptr)
	{
		return VirtualMemory::ManagedRegionState::Unmanaged;
	}

	constexpr uint32_t  tracked_page_shift   = 12;
	constexpr uintptr_t query_run_page_limit = 256;
	constexpr uintptr_t preceding_page_limit = (query_run_page_limit - 1) / 2;
	constexpr uintptr_t following_page_limit = query_run_page_limit - preceding_page_limit - 1;
	const auto          target               = static_cast<uintptr_t>(address);
	auto                state                = VirtualMemory::ManagedRegionState::Unmanaged;

	pthread_mutex_lock(&g_virtual_mutex);
	auto allocation = g_allocs->upper_bound(target);
	if (allocation != g_allocs->begin())
	{
		--allocation;
		const uintptr_t begin = allocation->first;
		const uintptr_t size  = allocation->second;
		const uintptr_t physical_end = android_mapping_end(begin, size);
		if (target >= begin + size && target < physical_end)
		{
			if (region != nullptr)
			{
				*region = {begin + size, physical_end,
				           VirtualMemory::Mode::NoAccess};
			}
			state = VirtualMemory::ManagedRegionState::Reserved;
		}
		if (size != 0 && size <= (std::numeric_limits<uintptr_t>::max)() - begin && target >= begin && target - begin < size)
		{
			const uintptr_t allocation_end        = begin + size;
			const uintptr_t allocation_first_page = begin >> tracked_page_shift;
			const uintptr_t allocation_last_page  = (allocation_end - 1) >> tracked_page_shift;
			const uintptr_t target_page           = target >> tracked_page_shift;
			const uintptr_t requested_last_page = requested_end == 0 ? allocation_last_page :
			    std::min(allocation_last_page, static_cast<uintptr_t>((requested_end - 1) >> tracked_page_shift));
			uintptr_t       first_page            = target_page;
			uintptr_t       last_page             = target_page;
			int             protection            = PROT_NONE;
			if (auto protection_entry = g_protects->find(target_page); protection_entry != g_protects->end())
			{
				protection                = protection_entry->second;
				auto      first_entry     = protection_entry;
				uintptr_t preceding_pages = 0;
				while (requested_end == 0 && preceding_pages < preceding_page_limit && first_page > allocation_first_page && first_entry != g_protects->begin())
				{
					auto previous = first_entry;
					--previous;
					if (previous->first != first_page - 1 || previous->second != protection)
					{
						break;
					}
					first_entry = previous;
					first_page--;
					preceding_pages++;
				}

				auto      last_entry      = protection_entry;
				uintptr_t following_pages = 0;
				while (following_pages < following_page_limit && last_page < requested_last_page)
				{
					auto next = last_entry;
					++next;
					if (next == g_protects->end() || next->first != last_page + 1 || next->second != protection)
					{
						break;
					}
					last_entry = next;
					last_page++;
					following_pages++;
				}
			}

			if (region != nullptr)
			{
				const uintptr_t run_begin = first_page == allocation_first_page ? begin : first_page << tracked_page_shift;
				const uintptr_t run_end   = last_page == allocation_last_page ? allocation_end : (last_page + 1) << tracked_page_shift;
				*region                   = VirtualMemory::ManagedRegion {run_begin, run_end, get_protection_flag(protection)};
				if (requested_end != 0)
				{
					region->begin = address;
					if (region->end > requested_end) region->end = requested_end;
				}
			}
			state = VirtualMemory::ManagedRegionState::Visible;
		}
	}
	pthread_mutex_unlock(&g_virtual_mutex);
	return state;
}

VirtualMemory::ManagedRegionState sys_virtual_query_managed_region(uint64_t address, VirtualMemory::ManagedRegion* region)
{
	return query_android_managed_region(address, 0, region);
}

VirtualMemory::ManagedRegionState sys_virtual_query_managed_data_region(uint64_t address, uint64_t end,
                                                                      VirtualMemory::ManagedRegion* region)
{
	if (end == 0)
	{
		if (region != nullptr) *region = VirtualMemory::ManagedRegion {0, 0, VirtualMemory::Mode::NoAccess};
		return VirtualMemory::ManagedRegionState::Unmanaged;
	}
	return query_android_managed_region(address, end, region);
}

bool sys_virtual_is_managed_address(uint64_t address)
{
	if (address == 0 || g_allocs == nullptr)
	{
		return false;
	}
	if (VirtualMemory::ImmutableSnapshot::Requested())
	{
		VirtualMemory::ManagedRegionState snapshot_state =
		    VirtualMemory::ManagedRegionState::Unmanaged;
		if (g_immutable_snapshot.Lookup(address, &snapshot_state, nullptr, false))
		{
			return snapshot_state == VirtualMemory::ManagedRegionState::Visible;
		}
	}

	const auto target = static_cast<uintptr_t>(address);
	bool       managed = false;
	pthread_mutex_lock(&g_virtual_mutex);
	auto allocation = g_allocs->upper_bound(target);
	if (allocation != g_allocs->begin())
	{
		--allocation;
		const uintptr_t begin = allocation->first;
		managed = target >= begin && target - begin < allocation->second;
	}
	pthread_mutex_unlock(&g_virtual_mutex);
	return managed;
}
#endif

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
bool sys_virtual_query_allocation_range(uint64_t address, uint64_t* begin, uint64_t* size)
{
	if (begin != nullptr) *begin = 0;
	if (size != nullptr) *size = 0;
	if (begin == nullptr || size == nullptr || g_allocs == nullptr) return false;
	const auto base = static_cast<uintptr_t>(address & ~UINT64_C(0xfff));
	pthread_mutex_lock(&g_virtual_mutex);
	const auto allocation = g_allocs->find(base);
	const bool found = allocation != g_allocs->end() && allocation->second != 0;
	if (found)
	{
		*begin = allocation->first;
		*size = allocation->second;
	}
	pthread_mutex_unlock(&g_virtual_mutex);
	return found;
}

bool sys_virtual_query_backing_span(uint64_t address, uint64_t maximum_bytes,
                                   VirtualMemory::GuestBackingSpan* span) noexcept
{
	if (span != nullptr) *span = {};
	if (span == nullptr || address == 0 || maximum_bytes == 0 ||
	    address > UINT64_MAX - maximum_bytes || g_allocs == nullptr || g_backing_ids == nullptr) return false;
	pthread_mutex_lock(&g_virtual_mutex);
	auto allocation = g_allocs->upper_bound(static_cast<uintptr_t>(address));
	bool found = false;
	if (allocation != g_allocs->begin())
	{
		--allocation;
		const auto begin = allocation->first;
		const auto identity = g_backing_ids->find(begin);
		if (address >= begin && address - begin < allocation->second && identity != g_backing_ids->end())
		{
			const auto remaining = allocation->second - (address - begin);
			*span = {identity->second, address - begin, address, std::min(maximum_bytes, remaining), 0};
			found = true;
		}
	}
	pthread_mutex_unlock(&g_virtual_mutex);
	return found;
}

VirtualMemory::GuestBackingPresence sys_virtual_query_backing_presence(
    uint64_t identity, uint64_t offset, uint64_t bytes) noexcept
{
	using Presence = VirtualMemory::GuestBackingPresence;
	if (identity == 0 || bytes == 0 || offset > UINT64_MAX - bytes ||
	    g_allocs == nullptr || g_backing_ids == nullptr) return Presence::Unknown;
	pthread_mutex_lock(&g_virtual_mutex);
	Presence result = Presence::Unknown;
	// Private identities are issued monotonically in this reserved domain. An
	// arbitrary id, or an id from a physical owner, cannot prove retirement.
	if (identity < g_next_backing_id)
	{
		result = Presence::Retired;
		for (const auto& backing : *g_backing_ids)
		{
			if (backing.second != identity) continue;
			const auto allocation = g_allocs->find(backing.first);
			if (allocation == g_allocs->end()) { result = Presence::Unknown; break; }
			if (offset < allocation->second) result = Presence::Live;
			break;
		}
	}
	pthread_mutex_unlock(&g_virtual_mutex);
	return result;
}
#endif

bool sys_virtual_free(uint64_t address)
{
	EXIT_IF(g_allocs == nullptr);
	size_t size = 0;

	auto addr = static_cast<uintptr_t>(address & ~static_cast<uint64_t>(0xfffu));

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	{
		pthread_mutex_lock(&g_virtual_mutex);
		auto allocation = g_allocs->find(addr);
		if (allocation == g_allocs->end())
		{
			pthread_mutex_unlock(&g_virtual_mutex);
			return false;
		}
		size = allocation->second;
		bool unmapped = false;
		{
			AndroidSnapshotMutation snapshot_mutation(
			    VirtualMemory::ImmutableSnapshot::Requested());
			if (munmap(reinterpret_cast<void*>(addr), size) == 0)
			{
				g_allocs->erase(allocation);
				g_backing_ids->erase(addr);
				const uintptr_t page_start = addr >> 12u;
				const uintptr_t page_end = (addr + size - 1) >> 12u;
				for (uintptr_t page = page_start; page <= page_end; page++)
				{
					if (auto protection = g_protects->find(page);
					    protection != g_protects->end())
					{
						g_protects->erase(protection);
					}
				}
				(void)publish_android_freed_mapping_locked(addr);
				unmapped = true;
			}
		}
		pthread_mutex_unlock(&g_virtual_mutex);
		return unmapped;
	}
#endif

	pthread_mutex_lock(&g_virtual_mutex);
	if (auto s = g_allocs->find(addr); s != g_allocs->end())
	{
		size = s->second;
		g_allocs->erase(s);
	}
	pthread_mutex_unlock(&g_virtual_mutex);

	if (size == 0)
	{
		return false;
	}

	if (munmap(reinterpret_cast<void*>(addr), size) == 0)
	{
		uintptr_t page_start = addr >> 12u;
		uintptr_t page_end   = (addr + size - 1) >> 12u;
		pthread_mutex_lock(&g_virtual_mutex);
		for (uintptr_t page = page_start; page <= page_end; page++)
		{
			if (auto s = g_protects->find(page); s != g_protects->end())
			{
				g_protects->erase(s);
			}
		}
		pthread_mutex_unlock(&g_virtual_mutex);
		return true;
	}

	return false;
}

bool sys_virtual_protect(uint64_t address, uint64_t size, VirtualMemory::Mode mode, VirtualMemory::Mode* old_mode)
{
	auto addr = static_cast<uintptr_t>(address);

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	{
		if (!valid_android_mapping(address, size, mode) || address == 0 ||
		    g_allocs == nullptr || g_protects == nullptr) return false;
		const uintptr_t host_size = android_host_page_size();
		const uintptr_t requested_end = addr + size;
		if (requested_end > (std::numeric_limits<uintptr_t>::max)() -
		                        (host_size - 1)) return false;
		const uintptr_t guest_begin = addr & ~(SNAPSHOT_PAGE_SIZE - 1);
		const uintptr_t guest_end =
		    (requested_end + SNAPSHOT_PAGE_SIZE - 1) & ~(SNAPSHOT_PAGE_SIZE - 1);
		const uintptr_t physical_begin = addr & ~(host_size - 1);
		const uintptr_t physical_end =
		    (requested_end + host_size - 1) & ~(host_size - 1);
		const int protection = get_protection_flag(mode);
		const int tracked_protection = get_tracked_protection_flag(mode);

		pthread_mutex_lock(&g_virtual_mutex);
		// Check the entire request before mprotect: reserved padding and gaps
		// cannot become visible, including when the public snapshot is off.
		auto allocation = g_allocs->upper_bound(addr);
		bool representable = allocation != g_allocs->begin();
		if (representable)
		{
			--allocation;
			uintptr_t cursor = addr;
			while (cursor < requested_end)
			{
				if (allocation == g_allocs->end() || cursor < allocation->first ||
				    cursor - allocation->first >= allocation->second)
				{
					representable = false;
					break;
				}
				cursor = allocation->first + allocation->second;
				++allocation;
			}
		}
		// Each visible guest sibling must retain the same effective data
		// rights as the requested pages. R and RX are representable together;
		// R/RW and guard/R are not. Allocation padding owns no guest rights.
		for (uintptr_t page = physical_begin / SNAPSHOT_PAGE_SIZE;
		     representable && page < physical_end / SNAPSHOT_PAGE_SIZE; page++)
		{
			const auto current = g_protects->find(page);
			const uintptr_t page_address = page * SNAPSHOT_PAGE_SIZE;
			if (page_address >= guest_begin && page_address < guest_end)
			{
				representable = current != g_protects->end();
			} else if (current != g_protects->end())
			{
				representable = get_protection_flag(
				                    get_protection_flag(current->second)) == protection;
			}
		}
		if (!representable)
		{
			pthread_mutex_unlock(&g_virtual_mutex);
			return false;
		}

		const auto previous = get_protection_flag(
		    g_protects->find(guest_begin / SNAPSHOT_PAGE_SIZE)->second);
		bool protected_ok = false;
		{
			AndroidSnapshotMutation snapshot_mutation(
			    VirtualMemory::ImmutableSnapshot::Requested());
			if (mprotect(reinterpret_cast<void*>(physical_begin),
			             physical_end - physical_begin, protection) == 0)
			{
				for (uintptr_t page = guest_begin / SNAPSHOT_PAGE_SIZE;
				     page < guest_end / SNAPSHOT_PAGE_SIZE; page++)
				{
					(*g_protects)[page] = tracked_protection;
				}
				(void)publish_android_protection_locked(guest_begin, guest_end, mode);
				if (old_mode != nullptr) *old_mode = previous;
				protected_ok = true;
			} else
			{
				// Linux may change an earlier VMA before mprotect fails on a
				// later one. The registry still contains every original mode;
				// restore the complete host envelope before releasing either
				// gate or allowing the caller to observe this failed mutation.
				for (uintptr_t page = physical_begin; page < physical_end; page += host_size)
				{
					const auto original = g_protects->lower_bound(page / SNAPSHOT_PAGE_SIZE);
					if (original == g_protects->end() ||
					    original->first >= (page + host_size) / SNAPSHOT_PAGE_SIZE ||
					    mprotect(reinterpret_cast<void*>(page), host_size,
					             get_protection_flag(get_protection_flag(original->second))) != 0)
					{
						// Returning would expose permissions inconsistent with
						// old_mode, generation, and the unchanged guest snapshot.
						std::abort();
					}
				}
			}
		}
		pthread_mutex_unlock(&g_virtual_mutex);
		return protected_ok;
	}
#endif

	pthread_mutex_lock(&g_virtual_mutex);
	if (old_mode != nullptr)
	{
		if (auto s = g_protects->find(addr >> 12u); s != g_protects->end())
		{
			*old_mode = get_protection_flag(s->second);
		} else
		{
			*old_mode = VirtualMemory::Mode::NoAccess;
		}
	}
	pthread_mutex_unlock(&g_virtual_mutex);

	uintptr_t page_start = addr >> 12u;
	uintptr_t page_end   = (addr + size - 1) >> 12u;
	const int protection = get_protection_flag(mode);
	const int tracked_protection = get_tracked_protection_flag(mode);
	if (mprotect(reinterpret_cast<void*>(page_start << 12u), (page_end - page_start + 1) << 12u, protection) == 0)
	{
		pthread_mutex_lock(&g_virtual_mutex);
		for (uintptr_t page = page_start; page <= page_end; page++)
		{
			(*g_protects)[page] = tracked_protection;
		}
		pthread_mutex_unlock(&g_virtual_mutex);
		return true;
	}

	return false;
}

bool sys_virtual_flush_instruction_cache(uint64_t /*address*/, uint64_t /*size*/)
{
	return true;
}

bool sys_virtual_patch_replace(uint64_t vaddr, uint64_t value)
{
	constexpr uintptr_t tracked_page_size = 4096;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	const uintptr_t physical_page_size = android_host_page_size();
	if (physical_page_size == 0) return false;
#else
	constexpr uintptr_t physical_page_size = tracked_page_size;
#endif
	constexpr size_t patch_size = sizeof(value);
	if (vaddr > (std::numeric_limits<uintptr_t>::max)() -
	                (patch_size - 1))
	{
		return false;
	}
	const auto address = static_cast<uintptr_t>(vaddr);
	if (static_cast<uint64_t>(address) != vaddr)
	{
		return false;
	}
	const uintptr_t last_address = address + patch_size - 1;
	const uintptr_t first_page = address / physical_page_size;
	const uintptr_t last_page = last_address / physical_page_size;
	const size_t page_count = static_cast<size_t>(last_page - first_page + 1);
	if (page_count == 0 || page_count > 2 || g_allocs == nullptr ||
	    g_protects == nullptr)
	{
		return false;
	}

	std::array<int, 2> original_protections {};
	std::array<bool, 2> restore_failed {};
	bool any_executable = false;
	pthread_mutex_lock(&g_virtual_mutex);
	auto allocation = g_allocs->upper_bound(address);
	if (allocation == g_allocs->begin())
	{
		pthread_mutex_unlock(&g_virtual_mutex);
		return false;
	}
	--allocation;
	const uintptr_t allocation_begin = allocation->first;
	const size_t allocation_size = allocation->second;
	if (address < allocation_begin || patch_size > allocation_size ||
	    address - allocation_begin > allocation_size - patch_size)
	{
		pthread_mutex_unlock(&g_virtual_mutex);
		return false;
	}

	for (size_t index = 0; index < page_count; index++)
	{
		const uintptr_t physical_begin = (first_page + index) * physical_page_size;
		bool found = false;
		for (uintptr_t page = physical_begin / tracked_page_size;
		     page < (physical_begin + physical_page_size) / tracked_page_size; page++)
		{
			const auto protection = g_protects->find(page);
			if (protection == g_protects->end()) continue; // Allocation padding.
			const auto guest_mode = get_protection_flag(protection->second);
			const int physical_protection = get_protection_flag(guest_mode);
			if (found && original_protections[index] != physical_protection)
			{
				pthread_mutex_unlock(&g_virtual_mutex);
				return false;
			}
			original_protections[index] = physical_protection;
			any_executable |= VirtualMemory::IsExecute(guest_mode);
			found = true;
		}
		if (!found)
		{
			pthread_mutex_unlock(&g_virtual_mutex);
			return false;
		}
	}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	// Snapshot readers cannot observe an immutable RX mapping during the
	// privileged writable window. FEX execution is already excluded by the
	// outer Core mutation barrier; the registry mutex excludes legacy queries.
	AndroidSnapshotMutation snapshot_mutation(
	    VirtualMemory::ImmutableSnapshot::Requested());
#endif
	const uintptr_t protected_begin = first_page * physical_page_size;
	const bool writable = mprotect(reinterpret_cast<void*>(protected_begin),
	                               page_count * physical_page_size,
	                               PROT_READ | PROT_WRITE) == 0;
	bool changed = false;
	if (writable)
	{
		uint64_t previous = 0;
		std::memcpy(&previous, reinterpret_cast<const void*>(address), patch_size);
		changed = previous != value;
		if (changed) std::memcpy(reinterpret_cast<void*>(address), &value, patch_size);
	}

	// A failed multi-page mprotect can have changed an earlier page. Restore
	// every physical page even when opening the writable window failed.
	bool restored = true;
	for (size_t index = 0; index < page_count; index++)
	{
		const uintptr_t page_address = (first_page + index) * physical_page_size;
		if (mprotect(reinterpret_cast<void*>(page_address), physical_page_size,
		             original_protections[index]) != 0)
		{
			restore_failed[index] = true;
			restored = false;
		}
	}

	if (!restored)
	{
		for (size_t index = 0; index < page_count; index++)
		{
			if (!restore_failed[index]) continue;
			const uintptr_t page_address = (first_page + index) * physical_page_size;
			const bool revoked = mprotect(reinterpret_cast<void*>(page_address),
			                              physical_page_size, PROT_NONE) == 0;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
			// Continuing with an unrestorable writable guest page would widen
			// guest permissions after the FEX execution barrier is released.
			if (!revoked) std::abort();
#endif
			const auto safe_mode = revoked ? VirtualMemory::Mode::NoAccess
			                               : VirtualMemory::Mode::ReadWrite;
			for (uintptr_t page = page_address / tracked_page_size;
			     page < (page_address + physical_page_size) / tracked_page_size; page++)
			{
				if (auto entry = g_protects->find(page); entry != g_protects->end())
				{
					entry->second = get_tracked_protection_flag(safe_mode);
				}
			}
		}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
		(void)publish_android_full_snapshot_locked();
#endif
	}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	snapshot_mutation.Finish();
#endif
	pthread_mutex_unlock(&g_virtual_mutex);

	if (!writable || !restored ||
	    (any_executable &&
	     !sys_virtual_flush_instruction_cache(vaddr, patch_size)))
	{
		return false;
	}
	return changed;
}

} // namespace Lumen::Core

#endif
