#include "Emulator/Kernel/Memory.h"

#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/MagicEnum.h"
#include "Lumen/Core/String.h"
#include "Lumen/Core/Threads.h"
#include "Lumen/Core/Vector.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Graphics/GraphicsRun.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Window.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/RuntimeLinker.h"
#include "../../../cpu/runtime/MemoryHandoff.h"
#if defined(LUMEN_FEXCORE_RUNTIME) || defined(LUMEN_ANDROID_FEXCORE_RUNTIME)
#include "../../../cpu/runtime/GuestCpuRuntime.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>
#if defined(LUMEN_KERNEL_VIRTUAL_RANGE_TEST_HOOK)
#include <thread>
#endif

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
#include <fcntl.h>
#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::LibKernel::Memory {

namespace VirtualMemory = Core::VirtualMemory;

LIB_NAME("libkernel", "libkernel");

enum class OwnerReleaseResult : uint32_t
{
	NotFound,
	Invalid,
	CoreFailed,
	Released,
};

enum class DirectMapResult : uint32_t
{
	Mapped,
	OwnerUnavailable,
	ReservationBusy,
	AddressUnavailable,
};

struct SessionMapping
{
	uint64_t address  = 0;
	uint64_t size     = 0;
	bool     released = false;
};

bool RangesOverlap(uint64_t left, uint64_t left_size, uint64_t right, uint64_t right_size)
{
	return left_size != 0 && right_size != 0 && left <= (std::numeric_limits<uint64_t>::max)() - left_size &&
	       right <= (std::numeric_limits<uint64_t>::max)() - right_size && left < right + right_size && right < left + left_size;
}

static_assert(sizeof(OrbisVirtualQueryInfo) == 72);
static_assert(alignof(OrbisVirtualQueryInfo) == 8);
static_assert(offsetof(OrbisVirtualQueryInfo, start) == 0x00);
static_assert(offsetof(OrbisVirtualQueryInfo, end) == 0x08);
static_assert(offsetof(OrbisVirtualQueryInfo, offset) == 0x10);
static_assert(offsetof(OrbisVirtualQueryInfo, protection) == 0x18);
static_assert(offsetof(OrbisVirtualQueryInfo, memory_type) == 0x1c);
static_assert(offsetof(OrbisVirtualQueryInfo, category_flags) == 0x20);
static_assert(offsetof(OrbisVirtualQueryInfo, name) == 0x21);
static_assert(offsetof(OrbisVirtualQueryInfo, reserved) == 0x41);

constexpr uint64_t ORBIS_VIRTUAL_RANGE_GRANULARITY = 0x4000;
constexpr uint8_t  ORBIS_VIRTUAL_QUERY_FLEXIBLE    = 0x01;
constexpr uint8_t  ORBIS_VIRTUAL_QUERY_DIRECT      = 0x02;
constexpr uint8_t  ORBIS_VIRTUAL_QUERY_COMMITTED   = 0x10;

struct RangeNameRecord
{
	uint64_t             begin = 0;
	uint64_t             end   = 0;
	std::array<char, 32> name {};
};

struct RangeNameInterval
{
	uint64_t begin = 0;
	uint64_t end   = 0;
};

// Guest-driven renames can split existing records. Keep this diagnostic
// metadata bounded independently of the size of the emulated address space.
constexpr size_t MAX_RANGE_NAME_RECORDS = 4096;

static std::mutex                   g_range_name_mutex {};
static std::vector<RangeNameRecord> g_range_names {};

bool AddWithoutOverflow(uint64_t left, uint64_t right, uint64_t* result)
{
	if (result == nullptr || left > (std::numeric_limits<uint64_t>::max)() - right) return false;
	*result = left + right;
	return true;
}

bool RoundUpWithoutOverflow(uint64_t value, uint64_t alignment, uint64_t* result)
{
	if (result == nullptr || alignment == 0 || (alignment & (alignment - 1)) != 0) return false;
	const uint64_t mask = alignment - 1;
	if (value > (std::numeric_limits<uint64_t>::max)() - mask) return false;
	*result = (value + mask) & ~mask;
	return true;
}

enum class GuestNameResult : uint32_t
{
	Ok,
	Fault,
	TooLong,
};

GuestNameResult ReadGuestRangeName(uint64_t guest_name, std::array<char, 32>* output)
{
	if (output == nullptr || guest_name == 0) return GuestNameResult::Fault;
	output->fill('\0');
	for (size_t index = 0; index < output->size(); index++)
	{
		char value = '\0';
		if (guest_name > (std::numeric_limits<uint64_t>::max)() - index) return GuestNameResult::Fault;
		if (!Loader::ReadGuestMemory(guest_name + index, &value, sizeof(value))) return GuestNameResult::Fault;
		if (value == '\0') return GuestNameResult::Ok;
		(*output)[index] = value;
	}
	return GuestNameResult::TooLong;
}

bool ReadGuestNamePrefix(uint64_t guest_name, std::array<char, 32>* output)
{
	if (output == nullptr || guest_name == 0) return false;
	output->fill('\0');
	for (size_t index = 0; index < output->size(); index++)
	{
		char value = '\0';
		if (guest_name > (std::numeric_limits<uint64_t>::max)() - index) return false;
		if (!Loader::ReadGuestMemory(guest_name + index, &value, sizeof(value))) return false;
		(*output)[index] = value;
		if (value == '\0') return true;
	}
	return true;
}

bool RewriteRangeNameRecords(std::vector<RangeNameRecord>* records, uint64_t begin, uint64_t end,
                             const std::array<char, 32>* name)
{
	if (records == nullptr || begin >= end) return true;
	std::vector<RangeNameRecord> replacement;
	replacement.reserve((std::min)(records->size() + 2, MAX_RANGE_NAME_RECORDS + 2));
	for (const auto& record: *records)
	{
		if (record.end <= begin || record.begin >= end)
		{
			replacement.push_back(record);
			continue;
		}
		if (record.begin < begin)
		{
			auto left = record;
			left.end  = begin;
			replacement.push_back(left);
		}
		if (record.end > end)
		{
			auto right  = record;
			right.begin = end;
			replacement.push_back(right);
		}
	}
	if (name != nullptr && (*name)[0] != '\0') replacement.push_back(RangeNameRecord {begin, end, *name});
	std::sort(replacement.begin(), replacement.end(), [](const auto& left, const auto& right) { return left.begin < right.begin; });

	size_t normalized_size = 0;
	for (const auto& record: replacement)
	{
		if (record.begin >= record.end) continue;
		if (normalized_size != 0)
		{
			auto& previous = replacement[normalized_size - 1];
			if (previous.end >= record.begin && previous.name == record.name)
			{
				previous.end = (std::max)(previous.end, record.end);
				continue;
			}
		}
		replacement[normalized_size++] = record;
	}
	replacement.resize(normalized_size);
	if (replacement.size() > MAX_RANGE_NAME_RECORDS) return false;
	records->swap(replacement);
	return true;
}

bool ReplaceRangeName(uint64_t begin, uint64_t size, const std::array<char, 32>& name)
{
	uint64_t end = 0;
	if (size == 0 || !AddWithoutOverflow(begin, size, &end)) return false;
	std::lock_guard<std::mutex> lock(g_range_name_mutex);
	return RewriteRangeNameRecords(&g_range_names, begin, end, name[0] == '\0' ? nullptr : &name);
}

bool ReplaceRangeNameWithinManagedIntervals(uint64_t begin, uint64_t end, std::vector<RangeNameInterval> intervals,
                                            const std::array<char, 32>& name)
{
	if (begin >= end) return true;
	std::sort(intervals.begin(), intervals.end(), [](const auto& left, const auto& right) { return left.begin < right.begin; });
	size_t normalized_size = 0;
	for (const auto& interval: intervals)
	{
		const uint64_t clipped_begin = (std::max)(begin, interval.begin);
		const uint64_t clipped_end   = (std::min)(end, interval.end);
		if (clipped_begin >= clipped_end) continue;
		if (normalized_size != 0 && intervals[normalized_size - 1].end >= clipped_begin)
		{
			intervals[normalized_size - 1].end = (std::max)(intervals[normalized_size - 1].end, clipped_end);
			continue;
		}
		intervals[normalized_size++] = RangeNameInterval {clipped_begin, clipped_end};
	}
	intervals.resize(normalized_size);
	if (intervals.empty()) return true;

	std::lock_guard<std::mutex> lock(g_range_name_mutex);
	auto                        replacement = g_range_names;
	for (const auto& interval: intervals)
	{
		if (!RewriteRangeNameRecords(&replacement, interval.begin, interval.end, name[0] == '\0' ? nullptr : &name)) return false;
	}
	g_range_names.swap(replacement);
	return true;
}

void EraseRangeNames(uint64_t begin, uint64_t size)
{
	uint64_t end = 0;
	if (size == 0 || !AddWithoutOverflow(begin, size, &end)) return;
	std::lock_guard<std::mutex> lock(g_range_name_mutex);
	if (!RewriteRangeNameRecords(&g_range_names, begin, end, nullptr))
	{
		std::vector<RangeNameRecord> empty;
		g_range_names.swap(empty);
	}
}

void ClearRangeNames()
{
	std::lock_guard<std::mutex> lock(g_range_name_mutex);
	std::vector<RangeNameRecord> empty;
	g_range_names.swap(empty);
}

class PhysicalMemory
{
public:
	struct AllocatedBlock
	{
		uint64_t                start_addr;
		uint64_t                backing_identity;
		uint32_t                handoff_watch_intents = 0;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		int                     backing_fd = -1;
#endif
		uint64_t                size;
		uint64_t                map_vaddr;
		uint64_t                map_size;
		uint64_t                map_phys_addr;
		int                     prot;
		VirtualMemory::Mode     mode;
		Graphics::GpuMemoryMode gpu_mode;
		int                     memory_type;
		uint64_t                cleanup_vaddr;
		uint64_t                cleanup_size;
		bool                    cleanup_pending;
		bool                    release_pending;
	};

	PhysicalMemory() { EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread()); }
	virtual ~PhysicalMemory() { LUMEN_NOT_IMPLEMENTED; }

	LUMEN_CLASS_NO_COPY(PhysicalMemory);

	static uint64_t Size() { return static_cast<uint64_t>(5376) * 1024 * 1024; }

	bool Alloc(uint64_t search_start, uint64_t search_end, size_t len, size_t alignment, uint64_t* phys_addr_out, int memory_type);
	OwnerReleaseResult ReleaseCoreFirst(uint64_t start, size_t len, uint64_t* vaddr, uint64_t* size, Graphics::GpuMemoryMode* gpu_mode);
	// Local checked-range model informed by the console return-code matrix
	// (1 October 2026). Whole contiguous unmapped allocations are admitted;
	// the oracle did not query their cross-release postcondition. No effects here.
	enum class CheckedRangeResult : uint32_t
	{
		Ready,
		NotFound,
		Invalid,
		Busy,
	};
	CheckedRangeResult CollectCheckedRange(uint64_t start, size_t len, std::vector<AllocatedBlock>* blocks);
	DirectMapResult MapDirect(uint64_t requested_vaddr, uint64_t phys_addr, size_t len, int prot, VirtualMemory::Mode mode,
	                          Graphics::GpuMemoryMode gpu_mode, bool fixed, size_t alignment, uint64_t* mapped_vaddr);
	bool RollbackDirectMap(uint64_t vaddr, uint64_t size);
	OwnerReleaseResult                 UnmapCoreFirst(uint64_t vaddr, uint64_t size, Graphics::GpuMemoryMode* gpu_mode);
	bool UpdateProtection(uint64_t vaddr, uint64_t size, int prot, VirtualMemory::Mode mode, Graphics::GpuMemoryMode gpu_mode,
	                      uint64_t* owner_vaddr, uint64_t* owner_size, bool* needs_gpu_allocation);
	bool CompleteUnmap(uint64_t vaddr, uint64_t size);
	bool CompleteRelease(uint64_t start, uint64_t len);
	bool Find(uint64_t vaddr, uint64_t* base_addr, size_t* len, int* prot, VirtualMemory::Mode* mode, Graphics::GpuMemoryMode* gpu_mode);
	bool Find(uint64_t phys_addr, bool next, PhysicalMemory::AllocatedBlock* out);
	void SnapshotSessionMappings(std::vector<SessionMapping>* mappings);
	bool ReleaseSessionMappingsCoreFirst(std::vector<SessionMapping>* mappings);

	[[nodiscard]] Core::Mutex&                  GetMutex() { return m_mutex; }
	[[nodiscard]] const Vector<AllocatedBlock>& GetBlocks() const { return m_allocated; }
	[[nodiscard]] Vector<AllocatedBlock>&       GetMutableBlocks() { return m_allocated; }

private:
	Vector<AllocatedBlock> m_allocated;
	Core::Mutex            m_mutex;
};

class FlexibleMemory
{
public:
	struct AllocatedBlock
	{
		uint64_t                map_vaddr;
		uint64_t                map_size;
		int                     prot;
		VirtualMemory::Mode     mode;
		Graphics::GpuMemoryMode gpu_mode;
		bool                    cleanup_pending;
	};

	FlexibleMemory() { EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread()); }
	virtual ~FlexibleMemory() { LUMEN_NOT_IMPLEMENTED; }

	LUMEN_CLASS_NO_COPY(FlexibleMemory);

	static uint64_t Size() { return static_cast<uint64_t>(448) * 1024 * 1024; }
	uint64_t        Available();

	bool Map(uint64_t vaddr, size_t len, int prot, VirtualMemory::Mode mode, Graphics::GpuMemoryMode gpu_mode);
	VirtualMemory::ReservedFixedResult MapReserved(uint64_t vaddr, size_t len, int prot, VirtualMemory::Mode mode,
	                                               Graphics::GpuMemoryMode gpu_mode);
	OwnerReleaseResult                 UnmapCoreFirst(uint64_t vaddr, uint64_t size, Graphics::GpuMemoryMode* gpu_mode);
	bool UpdateProtection(uint64_t vaddr, uint64_t size, int prot, VirtualMemory::Mode mode, Graphics::GpuMemoryMode gpu_mode,
	                      uint64_t* owner_vaddr, uint64_t* owner_size, bool* needs_gpu_allocation);
	bool CompleteUnmap(uint64_t vaddr, uint64_t size);
	bool Find(uint64_t vaddr, uint64_t* base_addr, size_t* len, int* prot, VirtualMemory::Mode* mode, Graphics::GpuMemoryMode* gpu_mode);
	void SnapshotSessionMappings(std::vector<SessionMapping>* mappings);
	bool ReleaseSessionMappingsCoreFirst(std::vector<SessionMapping>* mappings);

	[[nodiscard]] Core::Mutex&                  GetMutex() { return m_mutex; }
	[[nodiscard]] const Vector<AllocatedBlock>& GetBlocks() const { return m_allocated; }

private:
	Vector<AllocatedBlock> m_allocated;
	uint64_t               m_allocated_total = 0;
	Core::Mutex            m_mutex;
};

static PhysicalMemory* g_physical_memory = nullptr;
static FlexibleMemory* g_flexible_memory = nullptr;
static callback_func_t g_alloc_callback  = nullptr;
static callback_func_t g_free_callback   = nullptr;
static std::mutex      g_memory_lifecycle_mutex {};
static std::atomic<uint64_t> g_next_physical_backing_identity {1};
static thread_local bool g_memory_lifecycle_owned = false;

// Host scalars only; the optional diagnostic takes no lifecycle/VM lock and
// never inspects the guest in/out record. Each TU has its own bounded budget.
class KernelVMMutationTraceScope final
{
public:
	KernelVMMutationTraceScope(const char* operation, uint64_t record, uint64_t size,
	                          int prot, int flags, uint64_t physical, uint64_t alignment,
	                          uint64_t scalar_requested = 0, bool scalar_requested_known = false) noexcept
	    : m_operation(operation), m_record(record), m_size(size), m_prot(prot), m_flags(flags),
	      m_physical(physical), m_alignment(alignment), m_requested(scalar_requested),
	      m_requested_known(scalar_requested_known)
	{
		Emit("attempt");
	}
	~KernelVMMutationTraceScope() { Emit(m_phase); }
	void Requested(uint64_t address) noexcept { m_requested = address; m_requested_known = true; }
	void Output(uint64_t address) noexcept { m_output = address; }
	void RegionOutput(uint64_t begin, uint64_t end, uint32_t protection, bool reserved) noexcept
	{
		m_output = begin;
		m_output_end = end;
		m_output_protection = protection;
		m_output_reserved = reserved;
		m_region_known = true;
	}
	void Denied() noexcept { m_phase = "denied"; m_reason = "mutex-contended"; }
	void Retry(uint32_t attempt) noexcept
	{
		m_retry_count = attempt;
		m_reason = "competing-exclusive-mutation";
		Emit("admission-retry");
	}
	int Finish(int result, const char* reason) noexcept
	{
		m_result = result;
		m_result_known = true;
		m_reason = reason;
		m_phase = "return";
		return result;
	}
private:
	void Emit(const char* phase) const noexcept
	{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		static const bool enabled = [] {
			const char* text = std::getenv("LUMEN_MACOS_VM_MUTATION_TRACE");
			return text != nullptr && text[0] == '1' && text[1] == '\0';
		}();
		if (!enabled) return;
		static std::atomic<uint32_t> records {0};
		const uint32_t index = records.fetch_add(1, std::memory_order_relaxed);
		if (index >= 16384) return;
		std::array<char, 640> record {};
		const int length = std::snprintf(record.data(), record.size(),
		    "[KERNEL_VM_MUTATION_TRACE] index=%u operation=%s phase=%s reason=%s"
		    " record=0x%016" PRIx64 " requested_known=%u requested=0x%016" PRIx64
		    " size=0x%016" PRIx64 " prot=0x%08" PRIx32 " flags=0x%08" PRIx32
		    " physical=0x%016" PRIx64 " alignment=0x%016" PRIx64
		    " output=0x%016" PRIx64 " result_known=%u result=0x%08" PRIx32 " retry_count=%u"
		    " region_known=%u output_end=0x%016" PRIx64 " output_protection=0x%08" PRIx32 " output_reserved=%u"
		    " thread=%d hle=0x%016" PRIx64 "\n",
		    index, m_operation, phase, m_reason, m_record, m_requested_known ? 1u : 0u,
		    m_requested, m_size, static_cast<uint32_t>(m_prot), static_cast<uint32_t>(m_flags),
		    m_physical, m_alignment, m_output, m_result_known ? 1u : 0u,
		    static_cast<uint32_t>(m_result), m_retry_count, m_region_known ? 1u : 0u, m_output_end,
		    m_output_protection, m_output_reserved ? 1u : 0u, Core::Thread::GetThreadIdUnique(),
		    VirtualMemory::CaptureDiagnosticHostCall());
		if (length > 0 && static_cast<size_t>(length) < record.size())
		{
			(void)write(STDERR_FILENO, record.data(), static_cast<size_t>(length));
		}
#else
		(void)phase;
#endif
	}
	// These immutable scalars are read only by the macOS diagnostic emitter.
	[[maybe_unused]] const char* m_operation;
	[[maybe_unused]] uint64_t m_record;
	[[maybe_unused]] uint64_t m_size;
	[[maybe_unused]] int m_prot;
	[[maybe_unused]] int m_flags;
	[[maybe_unused]] uint64_t m_physical;
	[[maybe_unused]] uint64_t m_alignment;
	const char* m_phase = "unfinished";
	const char* m_reason = "none";
	uint64_t m_requested = 0;
	uint64_t m_output = 0;
	uint64_t m_output_end = 0;
	uint32_t m_output_protection = 0;
	bool m_output_reserved = false;
	bool m_region_known = false;
	int m_result = 0;
	bool m_requested_known = false;
	bool m_result_known = false;
	uint32_t m_retry_count = 0;
};

#if defined(LUMEN_KERNEL_VIRTUAL_RANGE_TEST_HOOK)
static std::atomic<uint64_t> g_memory_lifecycle_active {0};
static std::atomic<uint64_t> g_memory_lifecycle_waiting {0};
static std::atomic<uint64_t> g_memory_lifecycle_try_denials {0};
static std::atomic_bool      g_gpu_owner_lifecycle_probe_enabled {false};
static std::atomic<uint64_t> g_gpu_owner_allocations {0};
static std::atomic<uint64_t> g_gpu_owner_waits {0};
static std::atomic<uint64_t> g_gpu_owner_frees {0};
static std::atomic<uint64_t> g_gpu_owner_last_allocation_address {0};
static std::atomic<uint64_t> g_gpu_owner_last_allocation_size {0};
static std::atomic<uint64_t> g_gpu_owner_last_free_address {0};
static std::atomic<uint64_t> g_gpu_owner_last_free_size {0};
#endif

enum class MemoryLifecycleMode : uint32_t
{
	Blocking,
	Try,
	Quiescent,
};

constexpr MemoryLifecycleMode GuestMemoryLifecycleMode()
{
#if defined(LUMEN_FEXCORE_RUNTIME) || defined(LUMEN_ANDROID_FEXCORE_RUNTIME)
	return MemoryLifecycleMode::Try;
#else
	return MemoryLifecycleMode::Blocking;
#endif
}

constexpr MemoryLifecycleMode GuestMemoryLifecycleSafeWaitMode()
{
#if defined(LUMEN_FEXCORE_RUNTIME) || defined(LUMEN_ANDROID_FEXCORE_RUNTIME)
	return MemoryLifecycleMode::Quiescent;
#else
	return MemoryLifecycleMode::Blocking;
#endif
}

class MemoryLifecycleGuard final
{
public:
	explicit MemoryLifecycleGuard(MemoryLifecycleMode mode = MemoryLifecycleMode::Blocking)
	    : m_lock(g_memory_lifecycle_mutex, std::defer_lock)
	{
		if (mode != MemoryLifecycleMode::Blocking)
		{
			// Callback reentry cannot wait for the lifecycle owned by this
			// thread. Keep that path closed without touching a nonrecursive
			// mutex which is already owned by the caller.
			if (!g_memory_lifecycle_owned) m_acquired = m_lock.try_lock();
			if (mode == MemoryLifecycleMode::Quiescent && !m_acquired && !g_memory_lifecycle_owned)
			{
#if defined(LUMEN_KERNEL_VIRTUAL_RANGE_TEST_HOOK)
				g_memory_lifecycle_waiting.fetch_add(1, std::memory_order_release);
#endif
				for (;;)
				{
					bool quiescent = false;
					{
						// No guest access or local lifecycle lock is allowed
						// while the FEX HLE reader is released. A mutex owner
						// may need this release to complete an Exclusive VM
						// mutation. Readmission must also finish without our
						// lifecycle mutex before trying to become its owner.
						Loader::ScopedGuestHostWait host_wait;
						quiescent = host_wait.IsActive();
						if (quiescent) Core::Thread::SleepMicro(1000);
					}
					// Standalone/native calls, nested HLE and callers with
					// outstanding VM leases cannot make the wait promise.
					if (!quiescent) break;
					m_acquired = m_lock.try_lock();
					if (m_acquired) break;
				}
#if defined(LUMEN_KERNEL_VIRTUAL_RANGE_TEST_HOOK)
				g_memory_lifecycle_waiting.fetch_sub(1, std::memory_order_release);
#endif
			}
#if defined(LUMEN_KERNEL_VIRTUAL_RANGE_TEST_HOOK)
			if (!m_acquired)
			{
				g_memory_lifecycle_try_denials.fetch_add(1, std::memory_order_release);
			}
#endif
			if (!m_acquired)
			{
				KernelVMMutationTraceScope trace {"lifecycle-try", 0, 0, 0, 0, 0, 0};
				trace.Denied();
				return;
			}
		} else
		{
#if defined(LUMEN_KERNEL_VIRTUAL_RANGE_TEST_HOOK)
			g_memory_lifecycle_waiting.fetch_add(1, std::memory_order_release);
#endif
			m_lock.lock();
			m_acquired = true;
#if defined(LUMEN_KERNEL_VIRTUAL_RANGE_TEST_HOOK)
			g_memory_lifecycle_waiting.fetch_sub(1, std::memory_order_release);
#endif
		}
		g_memory_lifecycle_owned = true;
#if defined(LUMEN_KERNEL_VIRTUAL_RANGE_TEST_HOOK)
		g_memory_lifecycle_active.fetch_add(1, std::memory_order_release);
#endif
	}

	~MemoryLifecycleGuard()
	{
		if (m_acquired) g_memory_lifecycle_owned = false;
#if defined(LUMEN_KERNEL_VIRTUAL_RANGE_TEST_HOOK)
		if (m_acquired) g_memory_lifecycle_active.fetch_sub(1, std::memory_order_release);
#endif
	}

	MemoryLifecycleGuard(const MemoryLifecycleGuard&)            = delete;
	MemoryLifecycleGuard& operator=(const MemoryLifecycleGuard&) = delete;
	bool                  IsAcquired() const { return m_acquired; }

private:
	std::unique_lock<std::mutex> m_lock;
	bool                         m_acquired = false;
};

Graphics::GpuMemoryMode MergeGpuMemoryMode(Graphics::GpuMemoryMode current, Graphics::GpuMemoryMode update)
{
	const bool read = current == Graphics::GpuMemoryMode::Read || current == Graphics::GpuMemoryMode::ReadWrite ||
	                  update == Graphics::GpuMemoryMode::Read || update == Graphics::GpuMemoryMode::ReadWrite;
	const bool write = current == Graphics::GpuMemoryMode::Write || current == Graphics::GpuMemoryMode::ReadWrite ||
	                   update == Graphics::GpuMemoryMode::Write || update == Graphics::GpuMemoryMode::ReadWrite;
	if (read && write) return Graphics::GpuMemoryMode::ReadWrite;
	if (read) return Graphics::GpuMemoryMode::Read;
	if (write) return Graphics::GpuMemoryMode::Write;
	return Graphics::GpuMemoryMode::NoAccess;
}

void WaitForGpuOwner()
{
	// Producer completion may need the FEX ownership gate. The owner identity
	// mutex must already be released before suspending this HLE reader.
	Loader::ScopedGuestHostWait host_wait;
#if defined(LUMEN_KERNEL_VIRTUAL_RANGE_TEST_HOOK)
	if (g_gpu_owner_lifecycle_probe_enabled.load(std::memory_order_acquire))
	{
		bool owners_unlocked = false;
		std::thread observer([&] {
			if (g_physical_memory == nullptr || g_flexible_memory == nullptr ||
			    !g_physical_memory->GetMutex().TryLock()) return;
			g_physical_memory->GetMutex().Unlock();
			if (!g_flexible_memory->GetMutex().TryLock()) return;
			g_flexible_memory->GetMutex().Unlock();
			owners_unlocked = true;
		});
		observer.join();
		EXIT_IF(!owners_unlocked);
		g_gpu_owner_waits.fetch_add(1, std::memory_order_release);
		return;
	}
#endif
	Graphics::GraphicsRunWait();
}

bool PublishBackingBeforeUnmap(uint64_t address, uint64_t bytes)
{
	if (address == 0 || bytes == 0) return false;
	if (!GuestCpu::MemoryHandoffEnabled() || !GuestCpu::NeedsHandoff(address, bytes)) return true;
	// The caller retains its lifecycle and FEX/native HLE admission. The GPU
	// tracker first selects actual dirty physical intersections and acquires
	// exact live publication leases itself. A sticky hash collision must not
	// demand CPU Read rights from an unrelated NoAccess mapping.
	return Graphics::GraphicsRenderPrepareCpuMemoryAccess(address, bytes, GuestCpu::MemoryAccessIntent::Read);
}

void TrackGpuOwnerAllocation(uint64_t address, uint64_t size)
{
#if defined(LUMEN_KERNEL_VIRTUAL_RANGE_TEST_HOOK)
	if (g_gpu_owner_lifecycle_probe_enabled.load(std::memory_order_acquire))
	{
		g_gpu_owner_last_allocation_address.store(address, std::memory_order_release);
		g_gpu_owner_last_allocation_size.store(size, std::memory_order_release);
		g_gpu_owner_allocations.fetch_add(1, std::memory_order_release);
		return;
	}
#endif
	if (!Graphics::GpuMemoryContainsAllocatedRange(address, size))
	{
		Graphics::GpuMemorySetAllocatedRange(address, size);
	}
}

void FreeGpuOwner(uint64_t address, uint64_t size)
{
#if defined(LUMEN_KERNEL_VIRTUAL_RANGE_TEST_HOOK)
	if (g_gpu_owner_lifecycle_probe_enabled.load(std::memory_order_acquire))
	{
		g_gpu_owner_last_free_address.store(address, std::memory_order_release);
		g_gpu_owner_last_free_size.store(size, std::memory_order_release);
		g_gpu_owner_frees.fetch_add(1, std::memory_order_release);
		return;
	}
#endif
	Graphics::GpuMemoryFree(Graphics::WindowGetGraphicContext(), nullptr, address, size, true);
}

struct VirtualRangeOwner
{
	VirtualMemory::ReservationHandle handle {};
	bool                             pending_cleanup = false;
};

static std::mutex         g_virtual_range_mutex {};
static VirtualRangeOwner* g_virtual_range_owners          = nullptr;
static size_t             g_virtual_range_owner_num       = 0;
static size_t             g_virtual_range_owner_capacity  = 0;
static bool               g_virtual_range_cleanup_pending = false;

struct VirtualQueryRegion
{
	uint64_t            begin       = 0;
	uint64_t            end         = 0;
	uint64_t            offset      = 0;
	int32_t             protection  = 0;
	int32_t             memory_type = 0;
	uint8_t             category_flags = 0;
	VirtualMemory::Mode mode        = VirtualMemory::Mode::NoAccess;
	bool                reservation = false;
};

void SnapshotVirtualQueryMappings(std::vector<VirtualQueryRegion>* regions)
{
	if (regions == nullptr || g_physical_memory == nullptr || g_flexible_memory == nullptr) return;
	{
		Core::LockGuard lock(g_physical_memory->GetMutex());
		for (const auto& block: g_physical_memory->GetBlocks())
		{
			uint64_t end = 0;
			if (block.cleanup_pending || block.map_vaddr == 0 || block.map_size == 0 ||
			    !AddWithoutOverflow(block.map_vaddr, block.map_size, &end))
			{
				continue;
			}
			regions->push_back(VirtualQueryRegion {block.map_vaddr, end, block.map_phys_addr, block.prot, block.memory_type,
			                                               static_cast<uint8_t>(ORBIS_VIRTUAL_QUERY_DIRECT |
			                                                                    ORBIS_VIRTUAL_QUERY_COMMITTED),
			                                               block.mode, false});
		}
	}
	{
		Core::LockGuard lock(g_flexible_memory->GetMutex());
		for (const auto& block: g_flexible_memory->GetBlocks())
		{
			uint64_t end = 0;
			if (block.cleanup_pending || block.map_vaddr == 0 || block.map_size == 0 ||
			    !AddWithoutOverflow(block.map_vaddr, block.map_size, &end))
			{
				continue;
			}
			regions->push_back(VirtualQueryRegion {block.map_vaddr, end, 0, block.prot, 0,
			                                               static_cast<uint8_t>(ORBIS_VIRTUAL_QUERY_FLEXIBLE |
			                                                                    ORBIS_VIRTUAL_QUERY_COMMITTED),
			                                               block.mode, false});
		}
	}
}

void SnapshotVirtualQueryReservations(std::vector<VirtualQueryRegion>* regions)
{
	if (regions == nullptr) return;
	std::lock_guard<std::mutex> lock(g_virtual_range_mutex);
	for (size_t index = 0; index < g_virtual_range_owner_num; index++)
	{
		const auto& owner = g_virtual_range_owners[index];
		uint64_t    end   = 0;
		if (owner.pending_cleanup || !owner.handle.IsValid() || !AddWithoutOverflow(owner.handle.base, owner.handle.size, &end)) continue;
		regions->push_back(VirtualQueryRegion {owner.handle.base, end, 0, 0, 0, 0, VirtualMemory::Mode::NoAccess, true});
	}
}

bool ContainsAddress(const VirtualQueryRegion& region, uint64_t address)
{
	return region.begin <= address && address < region.end;
}

bool SelectVirtualQueryRegion(uint64_t address, int32_t flags, const std::vector<VirtualQueryRegion>& mappings,
	                          const std::vector<VirtualQueryRegion>& reservations, VirtualQueryRegion* selected)
{
	if (selected == nullptr) return false;
	for (const auto& mapping: mappings)
	{
		if (ContainsAddress(mapping, address))
		{
			*selected = mapping;
			return true;
		}
	}
	for (const auto& reservation: reservations)
	{
		if (!ContainsAddress(reservation, address)) continue;
		auto hole = reservation;
		for (const auto& mapping: mappings)
		{
			if (mapping.end <= reservation.begin || mapping.begin >= reservation.end) continue;
			if (mapping.end <= address && mapping.end > hole.begin) hole.begin = mapping.end;
			if (mapping.begin > address && mapping.begin < hole.end) hole.end = mapping.begin;
		}
		if (hole.begin < hole.end)
		{
			*selected = hole;
			return true;
		}
	}
	if (flags != 1) return false;

	bool found = false;
	for (const auto& region: mappings)
	{
		if (region.begin < address || (found && region.begin >= selected->begin)) continue;
		*selected = region;
		found     = true;
	}
	for (const auto& region: reservations)
	{
		if (region.begin < address || (found && region.begin >= selected->begin)) continue;
		*selected = region;
		found     = true;
	}
	return found;
}

bool RefineVirtualQueryRegionWithCore(uint64_t address, VirtualQueryRegion* region)
{
	if (region == nullptr) return false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	VirtualMemory::ManagedRegion core_region {};
	const auto                    core_state = VirtualMemory::QueryManagedRegion(address, &core_region);
	const bool state_matches = region->reservation ? core_state == VirtualMemory::ManagedRegionState::Reserved
	                                               : core_state == VirtualMemory::ManagedRegionState::Visible;
	if (!state_matches || core_region.begin >= core_region.end) return false;
	const uint64_t original_begin = region->begin;
	region->begin                 = (std::max)(region->begin, core_region.begin);
	region->end                   = (std::min)(region->end, core_region.end);
	if (!region->reservation && region->begin > original_begin &&
	    (region->category_flags & ORBIS_VIRTUAL_QUERY_DIRECT) != 0)
	{
		region->offset += region->begin - original_begin;
	}
	if (!region->reservation && core_region.mode != region->mode)
	{
		region->protection = static_cast<int32_t>(core_region.mode);
		region->mode       = core_region.mode;
	}
#else
	(void)address;
#endif
	return region->begin < region->end;
}

void ApplyVirtualQueryRangeName(uint64_t address, VirtualQueryRegion* region, std::array<char, 32>* name)
{
	if (region == nullptr || name == nullptr) return;
	const uint64_t original_begin = region->begin;
	name->fill('\0');
	std::lock_guard<std::mutex> lock(g_range_name_mutex);
	for (const auto& record: g_range_names)
	{
		if (record.end <= region->begin || record.begin >= region->end) continue;
		if (record.begin <= address && address < record.end)
		{
			region->begin = (std::max)(region->begin, record.begin);
			region->end   = (std::min)(region->end, record.end);
			*name         = record.name;
			break;
		}
		if (record.end <= address)
		{
			region->begin = (std::max)(region->begin, record.end);
		} else if (record.begin > address)
		{
			region->end = (std::min)(region->end, record.begin);
			break;
		}
	}
	if (region->begin > original_begin && (region->category_flags & ORBIS_VIRTUAL_QUERY_DIRECT) != 0)
	{
		region->offset += region->begin - original_begin;
	}
}

#if defined(LUMEN_KERNEL_VIRTUAL_RANGE_TEST_HOOK)
static std::atomic<uint32_t> g_virtual_range_test_fault {static_cast<uint32_t>(VirtualRangeFaultForTest::None)};

bool ConsumeVirtualRangeFaultForTest(VirtualRangeFaultForTest expected)
{
	uint32_t value = static_cast<uint32_t>(expected);
	return g_virtual_range_test_fault.compare_exchange_strong(value, static_cast<uint32_t>(VirtualRangeFaultForTest::None),
	                                                          std::memory_order_acq_rel, std::memory_order_acquire);
}
#endif

bool FailHleSlotPreflightForTest()
{
#if defined(LUMEN_KERNEL_VIRTUAL_RANGE_TEST_HOOK)
	return ConsumeVirtualRangeFaultForTest(VirtualRangeFaultForTest::HleSlotPreflight);
#else
	return false;
#endif
}

bool FailFinalGuestWriteForTest()
{
#if defined(LUMEN_KERNEL_VIRTUAL_RANGE_TEST_HOOK)
	return ConsumeVirtualRangeFaultForTest(VirtualRangeFaultForTest::FinalGuestWrite);
#else
	return false;
#endif
}

bool EnsureVirtualRangeOwnerCapacity()
{
	if (g_virtual_range_owner_num < g_virtual_range_owner_capacity) return true;
	constexpr size_t initial_capacity = 8;
	const size_t     max_capacity     = (std::numeric_limits<size_t>::max)() / sizeof(VirtualRangeOwner);
	if (g_virtual_range_owner_capacity >= max_capacity) return false;
	const size_t new_capacity =
	    g_virtual_range_owner_capacity == 0
	        ? initial_capacity
	        : (g_virtual_range_owner_capacity > max_capacity / 2 ? max_capacity : g_virtual_range_owner_capacity * 2);
	if (new_capacity <= g_virtual_range_owner_capacity) return false;
	void* resized = std::realloc(g_virtual_range_owners, new_capacity * sizeof(VirtualRangeOwner));
	if (resized == nullptr) return false;
	g_virtual_range_owners         = static_cast<VirtualRangeOwner*>(resized);
	g_virtual_range_owner_capacity = new_capacity;
	return true;
}

void EraseVirtualRangeOwner(size_t index)
{
	if (index >= g_virtual_range_owner_num) return;
	if (index + 1 < g_virtual_range_owner_num)
	{
		std::memmove(&g_virtual_range_owners[index], &g_virtual_range_owners[index + 1],
		             (g_virtual_range_owner_num - index - 1) * sizeof(VirtualRangeOwner));
	}
	g_virtual_range_owner_num--;
}

#if defined(LUMEN_KERNEL_VIRTUAL_RANGE_TEST_HOOK)
void FailNextVirtualRangeOperationForTest(VirtualRangeFaultForTest fault)
{
	g_virtual_range_test_fault.store(static_cast<uint32_t>(fault), std::memory_order_release);
}

VirtualRangeStateForTest GetVirtualRangeStateForTest()
{
	std::lock_guard<std::mutex> lock(g_virtual_range_mutex);
	VirtualRangeStateForTest    state {};
	state.live = g_virtual_range_owner_num;
	for (size_t index = 0; index < g_virtual_range_owner_num; index++)
	{
		if (g_virtual_range_owners[index].pending_cleanup)
		{
			state.pending_cleanup++;
		}
		if (state.first_base == 0)
		{
			state.first_base = g_virtual_range_owners[index].handle.base;
		}
	}
	return state;
}

MemoryLifecycleStateForTest GetMemoryLifecycleStateForTest()
{
	return MemoryLifecycleStateForTest {g_memory_lifecycle_active.load(std::memory_order_acquire),
	                                    g_memory_lifecycle_waiting.load(std::memory_order_acquire),
	                                    g_memory_lifecycle_try_denials.load(std::memory_order_acquire)};
}

RangeNameStateForTest GetRangeNameStateForTest()
{
	std::lock_guard<std::mutex> lock(g_range_name_mutex);
	return RangeNameStateForTest {g_range_names.size(), g_range_names.capacity()};
}

void SetGpuOwnerLifecycleProbeForTest(bool enabled)
{
	g_gpu_owner_lifecycle_probe_enabled.store(false, std::memory_order_release);
	if (enabled)
	{
		g_gpu_owner_allocations.store(0, std::memory_order_release);
		g_gpu_owner_waits.store(0, std::memory_order_release);
		g_gpu_owner_frees.store(0, std::memory_order_release);
		g_gpu_owner_last_allocation_address.store(0, std::memory_order_release);
		g_gpu_owner_last_allocation_size.store(0, std::memory_order_release);
		g_gpu_owner_last_free_address.store(0, std::memory_order_release);
		g_gpu_owner_last_free_size.store(0, std::memory_order_release);
		g_gpu_owner_lifecycle_probe_enabled.store(true, std::memory_order_release);
	}
}

GpuOwnerLifecycleStateForTest GetGpuOwnerLifecycleStateForTest()
{
	return GpuOwnerLifecycleStateForTest {g_gpu_owner_allocations.load(std::memory_order_acquire),
	                                      g_gpu_owner_waits.load(std::memory_order_acquire),
	                                      g_gpu_owner_frees.load(std::memory_order_acquire),
	                                      g_gpu_owner_last_allocation_address.load(std::memory_order_acquire),
	                                      g_gpu_owner_last_allocation_size.load(std::memory_order_acquire),
	                                      g_gpu_owner_last_free_address.load(std::memory_order_acquire),
	                                      g_gpu_owner_last_free_size.load(std::memory_order_acquire)};
}
#endif

static bool QueryPhysicalBacking(uint64_t address, uint64_t maximum_bytes, VirtualMemory::GuestBackingSpan* span) noexcept
{
	if (g_physical_memory == nullptr || span == nullptr) return false;
	Core::LockGuard lock(g_physical_memory->GetMutex());
	for (const auto& block : g_physical_memory->GetBlocks())
	{
		if (block.cleanup_pending || block.map_vaddr == 0 || address < block.map_vaddr ||
		    address - block.map_vaddr >= block.map_size) continue;
		const uint64_t delta = address - block.map_vaddr;
		if (block.map_phys_addr < block.start_addr || block.backing_identity == 0) return false;
		uint64_t live_bytes = block.map_size - delta;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		// Core retirement commits before the owner record is finalized. Metadata
		// in that gap must never authorize a new producer against the retired VA.
		VirtualMemory::ManagedRegion region;
		if (VirtualMemory::QueryManagedRegion(address, &region) != VirtualMemory::ManagedRegionState::Visible ||
		    region.end <= address) return false;
		live_bytes = std::min(live_bytes, region.end - address);
#endif
		*span = {block.backing_identity, block.map_phys_addr - block.start_addr + delta,
		    address, std::min(maximum_bytes, live_bytes), VirtualMemory::GetMutationGeneration()};
		return true;
	}
	return false;
}

static bool MarkPhysicalBackingAliases(uint64_t address, uint64_t bytes, uint32_t watch_intents) noexcept
{
	if (g_physical_memory == nullptr || bytes == 0 || address > UINT64_MAX - bytes ||
	    watch_intents < 1u || watch_intents > 3u) return false;
	Core::LockGuard lock(g_physical_memory->GetMutex());
	const PhysicalMemory::AllocatedBlock* source = nullptr;
	for (const auto& block : g_physical_memory->GetBlocks())
	{
		if (!block.cleanup_pending && block.map_vaddr != 0 && address >= block.map_vaddr &&
		    address - block.map_vaddr < block.map_size && bytes <= block.map_size - (address - block.map_vaddr))
		{ source = &block; break; }
	}
	if (source == nullptr) return false;
	const GuestCpu::PhysicalMemoryRange target {source->backing_identity,
	    source->map_phys_addr - source->start_addr + address - source->map_vaddr, bytes};
	for (auto& alias : g_physical_memory->GetMutableBlocks())
	{
		if (alias.cleanup_pending || alias.map_vaddr == 0 || alias.map_phys_addr < alias.start_addr) continue;
		GuestCpu::PhysicalMemoryRange intersection;
		if (GuestCpu::IntersectPhysicalRanges(target,
		    {alias.backing_identity, alias.map_phys_addr - alias.start_addr, alias.map_size}, &intersection))
		{
			alias.handoff_watch_intents |= watch_intents;
			GuestCpu::MarkHandoffRange(alias.map_vaddr + intersection.offset - (alias.map_phys_addr - alias.start_addr),
			    intersection.size, static_cast<GuestCpu::MemoryAccessIntent>(watch_intents));
		}
	}
	return true;
}

static bool MarkPhysicalBackingAliasesOwned(uint64_t owner, uint64_t address, uint64_t bytes,
    uint32_t watch_intents) noexcept
{
	if (g_physical_memory == nullptr || owner == 0 || bytes == 0 || address > UINT64_MAX - bytes ||
	    watch_intents < 1u || watch_intents > 3u) return false;
	Core::LockGuard lock(g_physical_memory->GetMutex());
	const PhysicalMemory::AllocatedBlock* source = nullptr;
	for (const auto& block : g_physical_memory->GetBlocks())
	{
		if (!block.cleanup_pending && block.map_vaddr != 0 && address >= block.map_vaddr &&
		    address - block.map_vaddr < block.map_size && bytes <= block.map_size - (address - block.map_vaddr))
		{ source = &block; break; }
	}
	if (source == nullptr || source->backing_identity == 0 || source->map_phys_addr < source->start_addr) return false;
	const GuestCpu::PhysicalMemoryRange target {source->backing_identity,
	    source->map_phys_addr - source->start_addr + address - source->map_vaddr, bytes};
	if (!GuestCpu::EnrollMemoryHandoffPhysicalRange(owner, target,
	        static_cast<GuestCpu::MemoryAccessIntent>(watch_intents))) return false;
	for (const auto& alias : g_physical_memory->GetBlocks())
	{
		if (alias.cleanup_pending || alias.map_vaddr == 0 || alias.map_phys_addr < alias.start_addr) continue;
		GuestCpu::PhysicalMemoryRange intersection;
		const GuestCpu::PhysicalMemoryRange mapping {
		    alias.backing_identity, alias.map_phys_addr - alias.start_addr, alias.map_size};
		if (GuestCpu::IntersectPhysicalRanges(target, mapping, &intersection) &&
		    !GuestCpu::MapMemoryHandoffOwnerPhysicalAlias(owner, mapping, alias.map_vaddr)) return false;
	}
	return true;
}

static VirtualMemory::GuestBackingPresence PhysicalBackingPresence(
    uint64_t identity, uint64_t offset, uint64_t bytes) noexcept
{
	using Presence = VirtualMemory::GuestBackingPresence;
	constexpr uint64_t physical_domain = UINT64_C(1) << 62;
	if (g_physical_memory == nullptr || bytes == 0 || offset > UINT64_MAX - bytes ||
	    (identity & (UINT64_C(3) << 62)) != physical_domain) return Presence::Unknown;
	const uint64_t issued_identity = identity & ~physical_domain;
	if (issued_identity == 0 || issued_identity >= g_next_physical_backing_identity.load(std::memory_order_acquire))
		return Presence::Unknown;
	Core::LockGuard lock(g_physical_memory->GetMutex());
	const GuestCpu::PhysicalMemoryRange target {identity, offset, bytes};
	// Enumerate the owner's complete accepted alias set, including mappings
	// whose GPU mode is NoAccess. Retirement of one canonical VA is insufficient.
	for (const auto& alias : g_physical_memory->GetBlocks())
	{
		if (alias.backing_identity != identity || alias.map_vaddr == 0 || alias.map_size == 0) continue;
		if (alias.map_phys_addr < alias.start_addr) return Presence::Unknown;
		GuestCpu::PhysicalMemoryRange intersection;
		const uint64_t alias_offset = alias.map_phys_addr - alias.start_addr;
		if (!GuestCpu::IntersectPhysicalRanges(target, {identity, alias_offset, alias.map_size}, &intersection)) continue;
		const uint64_t delta = intersection.offset - alias_offset;
		if (alias.map_vaddr > UINT64_MAX - delta || alias.map_vaddr + delta > UINT64_MAX - intersection.size)
			return Presence::Unknown;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		// Core retires before owner metadata is finalized. Check every overlapped
		// guest page so a remaining partial/CPU-only alias still proves Live.
		uint64_t address = alias.map_vaddr + delta;
		const uint64_t end = address + intersection.size;
		while (address < end)
		{
			VirtualMemory::ManagedRegion region {};
			if (VirtualMemory::QueryManagedRegion(address, &region) == VirtualMemory::ManagedRegionState::Visible)
				return region.begin <= address && region.end > address ? Presence::Live : Presence::Unknown;
			const uint64_t advance = std::min(end - address, UINT64_C(4096) - address % UINT64_C(4096));
			address += advance;
		}
#else
		return Presence::Live;
#endif
	}
	return Presence::Retired;
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
static bool CopyPhysicalBackingForGpu(uint64_t address, void* buffer, uint64_t bytes, bool write) noexcept
{
	if (g_physical_memory == nullptr || buffer == nullptr || address == 0 || bytes == 0 ||
	    bytes > SIZE_MAX || address > UINT64_MAX - bytes) return false;
	int retained_fd = -1;
	uint64_t backing_offset = 0;
	{
		Core::LockGuard lock(g_physical_memory->GetMutex());
		for (const auto& block : g_physical_memory->GetBlocks())
		{
			if (block.cleanup_pending || block.map_vaddr == 0 || address < block.map_vaddr ||
			    address - block.map_vaddr >= block.map_size || block.backing_fd < 0) continue;
			const uint64_t delta = address - block.map_vaddr;
			if (bytes > block.map_size - delta || block.map_phys_addr < block.start_addr) return false;
			backing_offset = block.map_phys_addr - block.start_addr + delta;
			if (backing_offset >= block.size || bytes > block.size - backing_offset) return false;
			uint64_t current = address;
			const uint64_t end = address + bytes;
			while (current < end)
			{
				VirtualMemory::ManagedRegion region {};
				if (VirtualMemory::QueryManagedRegion(current, &region) != VirtualMemory::ManagedRegionState::Visible ||
				    region.begin > current || region.end <= current) return false;
				current = std::min(end, region.end);
			}
			// The caller's existing CPU/native or mutation-owner admission protects
			// the accepted alias. This private duplicate independently retains the
			// physical file through IO without holding the owner identity mutex.
			retained_fd = fcntl(block.backing_fd, F_DUPFD_CLOEXEC, 0);
			break;
		}
	}
	if (retained_fd < 0) return false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	// Darwin POSIX-shm descriptors need not implement pread/pwrite. Map an
	// owner-private alias with exact transfer bounds instead; guest VA rights
	// (including NoAccess) never change. The duplicate pins the physical
	// object, and the caller's admission pins the accepted logical alias.
	const long page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0) { close(retained_fd); return false; }
	const uint64_t page = static_cast<uint64_t>(page_size);
	const uint64_t delta = backing_offset % page;
	if (bytes > SIZE_MAX - delta) { close(retained_fd); return false; }
	const size_t mapped_size = static_cast<size_t>(bytes + delta);
	void* private_alias = mmap(nullptr, mapped_size, PROT_READ | (write ? PROT_WRITE : 0),
	    MAP_SHARED, retained_fd, static_cast<off_t>(backing_offset - delta));
	close(retained_fd);
	if (private_alias == MAP_FAILED) return false;
	auto* data = static_cast<uint8_t*>(private_alias) + delta;
	if (write) std::memcpy(data, buffer, static_cast<size_t>(bytes));
	else std::memcpy(buffer, data, static_cast<size_t>(bytes));
	if (munmap(private_alias, mapped_size) != 0) std::abort();
	return true;
#else
	uint64_t done = 0;
	while (done < bytes)
	{
		const uint64_t count = std::min(bytes - done, static_cast<uint64_t>((std::numeric_limits<ssize_t>::max)()));
		const auto offset = static_cast<off_t>(backing_offset + done);
		auto* const current = static_cast<uint8_t*>(buffer) + static_cast<size_t>(done);
		const ssize_t transferred = write ? pwrite(retained_fd, current, static_cast<size_t>(count), offset)
		                                  : pread(retained_fd, current, static_cast<size_t>(count), offset);
		if (transferred < 0 && errno == EINTR) continue;
		if (transferred <= 0)
		{
			close(retained_fd);
			// Publication cannot report a recoverable failure after changing a
			// prefix of the backing, which would leave dirty authority ambiguous.
			if (write && done != 0) std::abort();
			return false;
		}
		done += static_cast<uint64_t>(transferred);
	}
	close(retained_fd);
	return true;
#endif
}
#endif

LUMEN_SUBSYSTEM_INIT(Memory)
{
	ClearRangeNames();
	g_physical_memory = new PhysicalMemory;
	g_flexible_memory = new FlexibleMemory;

	VirtualMemory::Init();
	EXIT_IF(!VirtualMemory::RegisterGuestBackingOwner(
	    QueryPhysicalBacking, MarkPhysicalBackingAliases, PhysicalBackingPresence
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	    , CopyPhysicalBackingForGpu
#else
	    , nullptr
#endif
	    , MarkPhysicalBackingAliasesOwned));
}

LUMEN_SUBSYSTEM_UNEXPECTED_SHUTDOWN(Memory) {}

LUMEN_SUBSYSTEM_DESTROY(Memory) {}

static uint64_t get_aligned_pos(uint64_t pos, size_t align)
{
	return (align != 0 ? (pos + (align - 1)) & ~(align - 1) : pos);
}

bool RegisterCallbacks(callback_func_t alloc_func, callback_func_t free_func)
{
	MemoryLifecycleGuard lifecycle_guard {GuestMemoryLifecycleMode()};
	if (!lifecycle_guard.IsAcquired()) return false;
	if (g_physical_memory == nullptr || g_flexible_memory == nullptr || g_alloc_callback != nullptr || g_free_callback != nullptr ||
	    alloc_func == nullptr || free_func == nullptr)
	{
		return false;
	}

	std::vector<SessionMapping> existing_mappings;
	{
		Core::LockGuard lock(g_physical_memory->GetMutex());
		for (const auto& b: g_physical_memory->GetBlocks())
		{
			if (!b.cleanup_pending && b.map_vaddr != 0 && b.map_size != 0)
				existing_mappings.push_back(SessionMapping {b.map_vaddr, b.map_size, false});
		}
	}
	{
		Core::LockGuard lock(g_flexible_memory->GetMutex());
		for (const auto& b: g_flexible_memory->GetBlocks())
		{
			if (!b.cleanup_pending && b.map_vaddr != 0 && b.map_size != 0)
				existing_mappings.push_back(SessionMapping {b.map_vaddr, b.map_size, false});
		}
	}

	g_alloc_callback = alloc_func;
	g_free_callback  = free_func;
	for (const auto& mapping: existing_mappings)
	{
		g_alloc_callback(mapping.address, mapping.size);
	}
	return true;
}

void PhysicalMemory::SnapshotSessionMappings(std::vector<SessionMapping>* mappings)
{
	EXIT_IF(mappings == nullptr);
	Core::LockGuard lock(m_mutex);
	for (const auto& block: m_allocated)
	{
		if (block.cleanup_pending && block.cleanup_vaddr != 0 && block.cleanup_size != 0)
		{
			mappings->push_back(SessionMapping {block.cleanup_vaddr, block.cleanup_size, false});
		} else if (block.map_vaddr != 0 && block.map_size != 0)
		{
			mappings->push_back(SessionMapping {block.map_vaddr, block.map_size, false});
		}
	}
}

bool PhysicalMemory::ReleaseSessionMappingsCoreFirst(std::vector<SessionMapping>* mappings)
{
	EXIT_IF(mappings == nullptr);
	EXIT_IF(!g_memory_lifecycle_owned);
	bool            complete = true;
	for (auto& mapping: *mappings)
	{
		AllocatedBlock snapshot {};
		bool found = false;
		{
			Core::LockGuard lock(m_mutex);
			for (const auto& block: m_allocated)
			{
				if (!block.cleanup_pending && block.map_vaddr == mapping.address && block.map_size == mapping.size)
				{
					snapshot = block;
					found = true;
					break;
				}
			}
		}
		// Session teardown has drained GPU work. Independent native borrowers
		// still need the identity mutex while Core waits for their admission.
		if (!found || !VirtualMemory::Free(mapping.address))
		{
			complete = false;
			continue;
		}
		Core::LockGuard lock(m_mutex);
		AllocatedBlock* owner = nullptr;
		for (auto& block: m_allocated)
			if (block.backing_identity == snapshot.backing_identity) { owner = &block; break; }
		EXIT_IF(owner == nullptr || owner->cleanup_pending || owner->map_vaddr != snapshot.map_vaddr ||
		        owner->map_size != snapshot.map_size || owner->map_phys_addr != snapshot.map_phys_addr ||
		        owner->mode != snapshot.mode || owner->gpu_mode != snapshot.gpu_mode);
		owner->map_vaddr = 0;
		owner->map_size  = 0;
		owner->map_phys_addr = 0;
		owner->prot      = 0;
		owner->mode      = VirtualMemory::Mode::NoAccess;
		owner->gpu_mode  = Graphics::GpuMemoryMode::NoAccess;
		mapping.released = true;
	}
	Core::LockGuard lock(m_mutex);
	for (const auto& block: m_allocated)
	{
		if (block.cleanup_pending || block.map_vaddr != 0 || block.map_size != 0) return false;
	}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	for (const auto& block: m_allocated)
		if (block.backing_fd >= 0) close(block.backing_fd);
#endif
	m_allocated.Clear();
	return complete;
}

void FlexibleMemory::SnapshotSessionMappings(std::vector<SessionMapping>* mappings)
{
	EXIT_IF(mappings == nullptr);
	Core::LockGuard lock(m_mutex);
	for (const auto& block: m_allocated)
	{
		if (block.map_vaddr != 0 && block.map_size != 0)
		{
			mappings->push_back(SessionMapping {block.map_vaddr, block.map_size, false});
		}
	}
}

bool FlexibleMemory::ReleaseSessionMappingsCoreFirst(std::vector<SessionMapping>* mappings)
{
	EXIT_IF(mappings == nullptr);
	EXIT_IF(!g_memory_lifecycle_owned);
	bool            complete = true;
	for (auto& mapping: *mappings)
	{
		bool     found = false;
		{
			Core::LockGuard lock(m_mutex);
			for (const auto& block: m_allocated)
			{
				if (!block.cleanup_pending && block.map_vaddr == mapping.address && block.map_size == mapping.size)
				{
					found = true;
					break;
				}
			}
		}
		if (!found || !VirtualMemory::Free(mapping.address))
		{
			complete = false;
			continue;
		}
		Core::LockGuard lock(m_mutex);
		uint32_t index = 0;
		for (; index < m_allocated.Size(); ++index)
			if (m_allocated[index].map_vaddr == mapping.address && m_allocated[index].map_size == mapping.size) break;
		EXIT_IF(index == m_allocated.Size() || m_allocated[index].cleanup_pending);
		m_allocated.RemoveAt(index);
		m_allocated_total -= mapping.size;
		mapping.released = true;
	}
	Core::LockGuard lock(m_mutex);
	if (!m_allocated.IsEmpty()) return false;
	m_allocated_total = 0;
	return complete;
}

bool ResetForNextSession()
{
	if (g_physical_memory == nullptr || g_flexible_memory == nullptr)
	{
		return false;
	}
	MemoryLifecycleGuard lifecycle_guard;

	std::vector<SessionMapping> physical_mappings;
	std::vector<SessionMapping> flexible_mappings;
	g_physical_memory->SnapshotSessionMappings(&physical_mappings);
	g_flexible_memory->SnapshotSessionMappings(&flexible_mappings);
	bool complete = g_physical_memory->ReleaseSessionMappingsCoreFirst(&physical_mappings);
	if (!g_flexible_memory->ReleaseSessionMappingsCoreFirst(&flexible_mappings))
	{
		complete = false;
	}
	const auto finish_released = [&](const std::vector<SessionMapping>& mappings)
	{
		for (const auto& mapping: mappings)
		{
			if (mapping.released && g_free_callback != nullptr) g_free_callback(mapping.address, mapping.size);
		}
	};
	finish_released(physical_mappings);
	finish_released(flexible_mappings);

	{
		std::lock_guard<std::mutex> lock(g_virtual_range_mutex);
		size_t                      index = 0;
		while (index < g_virtual_range_owner_num)
		{
			const auto& handle          = g_virtual_range_owners[index].handle;
			const auto  failed_child_in = [&handle](const auto& mappings)
			{
				return std::any_of(mappings.begin(), mappings.end(),
				                   [&handle](const SessionMapping& mapping)
				                   {
					                   const uint64_t address = mapping.address;
					                   return address >= handle.base && address - handle.base < handle.size && !mapping.released;
				                   });
			};
			const bool has_failed_child = failed_child_in(physical_mappings) || failed_child_in(flexible_mappings);
			if (has_failed_child || !VirtualMemory::ReleaseReservation(handle))
			{
				g_virtual_range_owners[index].pending_cleanup = true;
				complete                                      = false;
				index++;
				continue;
			}
			EraseVirtualRangeOwner(index);
		}
		g_virtual_range_cleanup_pending = g_virtual_range_owner_num != 0;
	}

	if (complete)
	{
		ClearRangeNames();
		g_alloc_callback = nullptr;
		g_free_callback  = nullptr;
	}
	return complete;
}

bool PhysicalMemory::Alloc(uint64_t search_start, uint64_t search_end, size_t len, size_t alignment, uint64_t* phys_addr_out,
                           int memory_type)
{
	if (phys_addr_out == nullptr)
	{
		return false;
	}

	Core::LockGuard lock(m_mutex);

	uint64_t free_pos = 0;

	for (const auto& b: m_allocated)
	{
		uint64_t n = b.start_addr + b.size;
		if (n > free_pos)
		{
			free_pos = n;
		}
	}

	free_pos = get_aligned_pos(free_pos, alignment);

	if (free_pos >= search_start && free_pos + len <= search_end)
	{
		AllocatedBlock b {};
		b.size        = len;
		const auto backing_identity = g_next_physical_backing_identity.fetch_add(1, std::memory_order_relaxed);
		EXIT_IF(backing_identity == 0 || backing_identity >= (UINT64_C(1) << 62));
		b.backing_identity = backing_identity | (UINT64_C(1) << 62);
		b.start_addr  = free_pos;
		b.gpu_mode    = Graphics::GpuMemoryMode::NoAccess;
		b.map_size    = 0;
		b.map_vaddr   = 0;
		b.map_phys_addr = 0;
		b.prot        = 0;
		b.mode        = VirtualMemory::Mode::NoAccess;
		b.memory_type = memory_type;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		if (b.size > static_cast<uint64_t>((std::numeric_limits<off_t>::max)())) return false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
		b.backing_fd = static_cast<int>(syscall(SYS_memfd_create, "lumen-physical-backing", MFD_CLOEXEC));
#else
		// Anonymous POSIX shared memory retains physical bytes independently
		// of any guest VA. Remove its name immediately; only owned descriptors
		// and accepted mappings retain it, including through unmap/remap.
		char backing_name[64] {};
		std::snprintf(backing_name, sizeof(backing_name), "/lm-%x-%llx",
		    static_cast<unsigned>(getpid()), static_cast<unsigned long long>(backing_identity));
		b.backing_fd = shm_open(backing_name, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (b.backing_fd >= 0)
		{
			if (shm_unlink(backing_name) != 0 || fcntl(b.backing_fd, F_SETFD, FD_CLOEXEC) != 0)
			{
				close(b.backing_fd);
				return false;
			}
		}
#endif
		if (b.backing_fd < 0) return false;
		if (ftruncate(b.backing_fd, static_cast<off_t>(b.size)) != 0)
		{
			close(b.backing_fd);
			return false;
		}
#endif

		m_allocated.Add(b);

		*phys_addr_out = free_pos;
		return true;
	}

	return false;
}

OwnerReleaseResult PhysicalMemory::ReleaseCoreFirst(uint64_t start, size_t len, uint64_t* vaddr, uint64_t* size,
                                                    Graphics::GpuMemoryMode* gpu_mode)
{
	if (vaddr == nullptr || size == nullptr || gpu_mode == nullptr) return OwnerReleaseResult::Invalid;
	EXIT_IF(!g_memory_lifecycle_owned);
	AllocatedBlock snapshot {};
	bool found = false;
	{
		Core::LockGuard lock(m_mutex);
		for (const auto& block: m_allocated)
		{
			if (start < block.start_addr || start - block.start_addr >= block.size) continue;
			if (start != block.start_addr || len != block.size) return OwnerReleaseResult::Invalid;
			if (block.cleanup_pending) return OwnerReleaseResult::CoreFailed;
			snapshot = block;
			found = true;
			break;
		}
	}
	if (!found) return OwnerReleaseResult::NotFound;
	*vaddr = snapshot.map_vaddr;
	*size = snapshot.map_size;
	*gpu_mode = snapshot.gpu_mode;

	// The lifecycle transaction excludes owner writers, while the physical
	// identity mutex remains available to producer completion and CPU handoff.
	if (snapshot.gpu_mode != Graphics::GpuMemoryMode::NoAccess) WaitForGpuOwner();
	if (snapshot.map_vaddr != 0 && snapshot.map_size != 0 &&
	    !PublishBackingBeforeUnmap(snapshot.map_vaddr, snapshot.map_size)) return OwnerReleaseResult::CoreFailed;
	if ((snapshot.map_vaddr != 0 || snapshot.map_size != 0) && !VirtualMemory::Free(snapshot.map_vaddr))
		return OwnerReleaseResult::CoreFailed;

	Core::LockGuard lock(m_mutex);
	for (auto& block: m_allocated)
	{
		if (block.backing_identity != snapshot.backing_identity) continue;
		EXIT_IF(block.start_addr != snapshot.start_addr || block.size != snapshot.size || block.cleanup_pending ||
		        block.map_vaddr != snapshot.map_vaddr || block.map_size != snapshot.map_size ||
		        block.map_phys_addr != snapshot.map_phys_addr || block.prot != snapshot.prot ||
		        block.mode != snapshot.mode || block.gpu_mode != snapshot.gpu_mode);
		block.cleanup_vaddr = snapshot.map_vaddr;
		block.cleanup_size = snapshot.map_size;
		block.cleanup_pending = true;
		block.release_pending = true;
		block.map_vaddr = 0;
		block.map_size = 0;
		block.map_phys_addr = 0;
		block.prot = 0;
		block.mode = VirtualMemory::Mode::NoAccess;
		block.gpu_mode = Graphics::GpuMemoryMode::NoAccess;
		return OwnerReleaseResult::Released;
	}
	EXIT("physical release owner changed during lifecycle transaction");
	return OwnerReleaseResult::CoreFailed;
}

PhysicalMemory::CheckedRangeResult PhysicalMemory::CollectCheckedRange(uint64_t start, size_t len, std::vector<AllocatedBlock>* blocks)
{
	if (blocks == nullptr || len == 0 || start > (std::numeric_limits<uint64_t>::max)() - static_cast<uint64_t>(len))
	{
		return CheckedRangeResult::Invalid;
	}
	blocks->clear();

	const uint64_t  end = start + static_cast<uint64_t>(len);
	Core::LockGuard lock(m_mutex);

	// A start outside every allocation is reported apart from a boundary that
	// does not match the containing allocation.
	bool start_allocated = false;
	for (const auto& block: m_allocated)
	{
		if (start >= block.start_addr && start - block.start_addr < block.size)
		{
			start_allocated = true;
			break;
		}
	}
	if (!start_allocated) return CheckedRangeResult::NotFound;

	uint64_t cursor = start;
	while (cursor < end)
	{
		const AllocatedBlock* next = nullptr;
		for (const auto& block: m_allocated)
		{
			if (block.start_addr == cursor)
			{
				next = &block;
				break;
			}
		}
		// A missing boundary, a partial overlap or a range that runs past the
		// end of an allocation are all boundary mismatches.
		if (next == nullptr) return CheckedRangeResult::Invalid;
		if (next->cleanup_pending || next->release_pending) return CheckedRangeResult::Busy;
		if (next->start_addr + next->size > end)
		{
			return CheckedRangeResult::Invalid;
		}
		blocks->push_back(*next);
		cursor = next->start_addr + next->size;
	}
	return CheckedRangeResult::Ready;
}

DirectMapResult PhysicalMemory::MapDirect(uint64_t requested_vaddr, uint64_t phys_addr, size_t len, int prot,
                                          VirtualMemory::Mode mode, Graphics::GpuMemoryMode gpu_mode, bool fixed,
                                          size_t alignment, uint64_t* mapped_vaddr)
{
	if (mapped_vaddr == nullptr) return DirectMapResult::OwnerUnavailable;
	*mapped_vaddr = 0;
	EXIT_IF(!g_memory_lifecycle_owned);
	AllocatedBlock snapshot {};
	bool found = false;
	{
		Core::LockGuard lock(m_mutex);
		for (const auto& block: m_allocated)
		{
			if (phys_addr < block.start_addr || phys_addr - block.start_addr >= block.size) continue;
			const uint64_t offset = phys_addr - block.start_addr;
			if (len == 0 || static_cast<uint64_t>(len) > block.size - offset || block.cleanup_pending ||
			    block.map_vaddr != 0 || block.map_size != 0) return DirectMapResult::OwnerUnavailable;
			snapshot = block;
			found = true;
			break;
		}
	}
	if (!found) return DirectMapResult::OwnerUnavailable;
#if defined(LUMEN_FEXCORE_RUNTIME) || defined(LUMEN_ANDROID_FEXCORE_RUNTIME)
	// Alloc/Bind each own a nested Core mutation, but their individual close
	// must not readmit translated CPUs between anonymous activation and the
	// shared physical binding, owner record and inherited guard publication.
	// Keep this outer epoch through the reservation commit as well. The
	// owner identity mutex is deliberately not held while gaining admission.
	const uint64_t mapping_epoch = LumenAndroidFexCoreBeginGuestMemoryMutationV2(
	    LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2);
	if (mapping_epoch == 0) return DirectMapResult::ReservationBusy;
	struct CloseMappingEpoch
	{
		uint64_t token;
		~CloseMappingEpoch()
		{
			if (LumenAndroidFexCoreEndGuestMemoryMutationV2(token, 0, 0, 0, nullptr, nullptr) != 1) std::abort();
		}
	} close_mapping_epoch {mapping_epoch};
#endif

	// The global lifecycle transaction excludes owner writers. Never retain
	// the physical identity mutex across Core's FEX exclusive admission: an
	// executing CPU reader may need that mutex before it can park.
	uint64_t out_addr = 0;
	VirtualMemory::MutationTransaction reservation_transaction;
	if (fixed)
	{
		const auto reserved_result =
		    VirtualMemory::BeginAllocFixedInReservation(requested_vaddr, len, mode, &reservation_transaction);
		if (reserved_result == VirtualMemory::ReservedFixedResult::Begun)
		{
			out_addr = requested_vaddr;
		} else if (reserved_result == VirtualMemory::ReservedFixedResult::Failed)
		{
			return DirectMapResult::ReservationBusy;
		} else if (VirtualMemory::AllocFixed(requested_vaddr, len, mode))
		{
			out_addr = requested_vaddr;
		}
	} else
	{
		out_addr = VirtualMemory::AllocAligned(requested_vaddr, len, mode, alignment);
	}
	if (out_addr == 0) return DirectMapResult::AddressUnavailable;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	if (snapshot.backing_fd < 0 || !VirtualMemory::BindFreshGuestMappingToSharedBacking(
	        out_addr, len, snapshot.backing_fd, phys_addr - snapshot.start_addr, mode))
	{
		if (reservation_transaction.IsActive())
		{
			if (!reservation_transaction.Abort()) std::abort();
		} else if (!VirtualMemory::Free(out_addr)) std::abort();
		return DirectMapResult::AddressUnavailable;
	}
#endif
	{
		Core::LockGuard lock(m_mutex);
		AllocatedBlock* owner = nullptr;
		for (auto& block: m_allocated)
		{
			if (block.backing_identity == snapshot.backing_identity) { owner = &block; break; }
		}
		EXIT_IF(owner == nullptr || owner->start_addr != snapshot.start_addr || owner->size != snapshot.size ||
		        owner->cleanup_pending || owner->map_vaddr != 0 || owner->map_size != 0);
		owner->map_vaddr = out_addr;
		owner->map_size = len;
		owner->map_phys_addr = phys_addr;
		owner->prot = prot;
		owner->mode = mode;
		owner->gpu_mode = gpu_mode;
		// Intent survives unmap/remap; publish inherited guards before this
		// mapping's outer exclusive epoch can readmit a translated CPU.
		if (owner->handoff_watch_intents != 0)
			GuestCpu::MarkHandoffRange(out_addr, len, static_cast<GuestCpu::MemoryAccessIntent>(owner->handoff_watch_intents));
		// Live resource tokens keep exact physical offsets across unmap/remap.
		// Publish all their aliases before the outer epoch readmits the CPU;
		// retired resources have already removed their own contributions.
		EXIT_IF(!GuestCpu::MapMemoryHandoffPhysicalAlias(
		    {owner->backing_identity, phys_addr - owner->start_addr, len}, out_addr));
	}
	reservation_transaction.Commit();
	*mapped_vaddr = out_addr;
	return DirectMapResult::Mapped;
}

bool PhysicalMemory::RollbackDirectMap(uint64_t vaddr, uint64_t size)
{
	EXIT_IF(!g_memory_lifecycle_owned);
	uint64_t identity = 0;
	{
		Core::LockGuard lock(m_mutex);
		for (const auto& block: m_allocated)
		{
			if (!block.cleanup_pending && block.map_vaddr == vaddr && block.map_size == size)
			{ identity = block.backing_identity; break; }
		}
	}
	if (identity == 0 || !VirtualMemory::Free(vaddr)) return false;
	Core::LockGuard lock(m_mutex);
	for (auto& block: m_allocated)
	{
		if (block.backing_identity != identity) continue;
		EXIT_IF(block.cleanup_pending || block.map_vaddr != vaddr || block.map_size != size);
		block.map_vaddr = 0;
		block.map_size = 0;
		block.map_phys_addr = 0;
		block.prot = 0;
		block.mode = VirtualMemory::Mode::NoAccess;
		block.gpu_mode = Graphics::GpuMemoryMode::NoAccess;
		return true;
	}
	std::abort();
}

OwnerReleaseResult PhysicalMemory::UnmapCoreFirst(uint64_t vaddr, uint64_t size, Graphics::GpuMemoryMode* gpu_mode)
{
	if (gpu_mode == nullptr) return OwnerReleaseResult::Invalid;
	EXIT_IF(!g_memory_lifecycle_owned);
	AllocatedBlock snapshot {};
	bool found = false;
	{
		Core::LockGuard lock(m_mutex);
		for (const auto& block: m_allocated)
		{
			if (block.cleanup_pending && RangesOverlap(vaddr, size, block.cleanup_vaddr, block.cleanup_size))
				return OwnerReleaseResult::CoreFailed;
			if (block.map_vaddr == 0 || vaddr < block.map_vaddr || vaddr - block.map_vaddr >= block.map_size) continue;
			if (block.map_vaddr != vaddr || block.map_size != size) return OwnerReleaseResult::Invalid;
			snapshot = block;
			found = true;
			break;
		}
	}
	if (!found) return OwnerReleaseResult::NotFound;
	*gpu_mode = snapshot.gpu_mode;
	if (snapshot.gpu_mode != Graphics::GpuMemoryMode::NoAccess) WaitForGpuOwner();
	if (!PublishBackingBeforeUnmap(vaddr, size)) return OwnerReleaseResult::CoreFailed;
	if (!VirtualMemory::Free(vaddr)) return OwnerReleaseResult::CoreFailed;

	Core::LockGuard lock(m_mutex);
	for (auto& block: m_allocated)
	{
		if (block.backing_identity != snapshot.backing_identity) continue;
		EXIT_IF(block.start_addr != snapshot.start_addr || block.size != snapshot.size || block.cleanup_pending ||
		        block.map_vaddr != snapshot.map_vaddr || block.map_size != snapshot.map_size ||
		        block.map_phys_addr != snapshot.map_phys_addr || block.prot != snapshot.prot ||
		        block.mode != snapshot.mode || block.gpu_mode != snapshot.gpu_mode);
		block.cleanup_vaddr = snapshot.map_vaddr;
		block.cleanup_size = snapshot.map_size;
		block.cleanup_pending = true;
		block.release_pending = false;
		block.gpu_mode = Graphics::GpuMemoryMode::NoAccess;
		block.map_size = 0;
		block.map_vaddr = 0;
		block.map_phys_addr = 0;
		block.prot = 0;
		block.mode = VirtualMemory::Mode::NoAccess;
		return OwnerReleaseResult::Released;
	}
	EXIT("physical unmap owner changed during lifecycle transaction");
	return OwnerReleaseResult::CoreFailed;
}

bool PhysicalMemory::UpdateProtection(uint64_t vaddr, uint64_t size, int prot, VirtualMemory::Mode mode,
                                      Graphics::GpuMemoryMode gpu_mode, uint64_t* owner_vaddr, uint64_t* owner_size,
                                      bool* needs_gpu_allocation)
{
	if (owner_vaddr == nullptr || owner_size == nullptr || needs_gpu_allocation == nullptr) return false;

	Core::LockGuard lock(m_mutex);
	for (auto& block: m_allocated)
	{
		if (block.cleanup_pending || vaddr < block.map_vaddr || vaddr - block.map_vaddr >= block.map_size) continue;
		const uint64_t offset = vaddr - block.map_vaddr;
		if (size == 0 || size > block.map_size - offset) return false;
		const auto previous_gpu_mode = block.gpu_mode;
		*owner_vaddr                = block.map_vaddr;
		*owner_size                 = block.map_size;
		if (offset == 0 && size == block.map_size)
		{
			block.prot     = prot;
			block.mode     = mode;
			block.gpu_mode = gpu_mode;
		} else
		{
			// The legacy HLE owner stores one map-wide protection. Preserve it
			// for a partial update, but retain the union of GPU access so later
			// unmap cannot skip synchronization and resource cleanup.
			block.gpu_mode = MergeGpuMemoryMode(block.gpu_mode, gpu_mode);
		}
		*needs_gpu_allocation = previous_gpu_mode == Graphics::GpuMemoryMode::NoAccess &&
		                        block.gpu_mode != Graphics::GpuMemoryMode::NoAccess;
		return true;
	}
	return false;
}

bool PhysicalMemory::CompleteUnmap(uint64_t vaddr, uint64_t size)
{
	Core::LockGuard lock(m_mutex);
	for (auto& block: m_allocated)
	{
		if (block.cleanup_pending && !block.release_pending && block.cleanup_vaddr == vaddr && block.cleanup_size == size)
		{
			block.cleanup_vaddr   = 0;
			block.cleanup_size    = 0;
			block.cleanup_pending = false;
			return true;
		}
	}
	return false;
}

bool PhysicalMemory::CompleteRelease(uint64_t start, uint64_t len)
{
	Core::LockGuard lock(m_mutex);
	uint32_t        index = 0;
	for (const auto& block: m_allocated)
	{
		if (block.start_addr == start && block.size == len && block.cleanup_pending && block.release_pending)
		{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
			if (block.backing_fd >= 0) close(block.backing_fd);
#endif
			m_allocated.RemoveAt(index);
			return true;
		}
		index++;
	}
	return false;
}

bool PhysicalMemory::Find(uint64_t phys_addr, bool next, AllocatedBlock* out)
{
	EXIT_IF(out == nullptr);

	Core::LockGuard lock(m_mutex);

	for (auto& b: m_allocated)
	{
		if (phys_addr >= b.start_addr && phys_addr < b.start_addr + b.size)
		{
			*out = b;
			return true;
		}
	}

	if (next)
	{
		uint64_t        min_start_addr = UINT64_MAX;
		AllocatedBlock* next           = nullptr;
		for (auto& b: m_allocated)
		{
			if (b.start_addr > phys_addr && b.start_addr < min_start_addr)
			{
				min_start_addr = b.start_addr;
				next           = &b;
			}
		}
		if (next != nullptr)
		{
			*out = *next;
			return true;
		}
	}

	return false;
}

bool PhysicalMemory::Find(uint64_t vaddr, uint64_t* base_addr, size_t* len, int* prot, VirtualMemory::Mode* mode,
                          Graphics::GpuMemoryMode* gpu_mode)
{
	Core::LockGuard lock(m_mutex);

	return std::any_of(m_allocated.begin(), m_allocated.end(),
	                   [vaddr, base_addr, len, prot, mode, gpu_mode](auto& b)
	                   {
		                   if (!b.cleanup_pending && vaddr >= b.map_vaddr && vaddr < b.map_vaddr + b.map_size)
		                   {
			                   if (base_addr != nullptr)
			                   {
				                   *base_addr = b.map_vaddr;
			                   }
			                   if (len != nullptr)
			                   {
				                   *len = b.map_size;
			                   }
			                   if (prot != nullptr)
			                   {
				                   *prot = b.prot;
			                   }
			                   if (mode != nullptr)
			                   {
				                   *mode = b.mode;
			                   }
			                   if (gpu_mode != nullptr)
			                   {
				                   *gpu_mode = b.gpu_mode;
			                   }

			                   return true;
		                   }
		                   return false;
	                   });
}

bool FlexibleMemory::Map(uint64_t vaddr, size_t len, int prot, VirtualMemory::Mode mode, Graphics::GpuMemoryMode gpu_mode)
{
	Core::LockGuard lock(m_mutex);
	for (const auto& block: m_allocated)
	{
		if (block.cleanup_pending && RangesOverlap(vaddr, len, block.map_vaddr, block.map_size))
		{
			return false;
		}
	}

	AllocatedBlock b {};
	b.map_vaddr = vaddr;
	b.map_size  = len;
	b.prot      = prot;
	b.mode      = mode;
	b.gpu_mode  = gpu_mode;

	m_allocated.Add(b);
	m_allocated_total += len;

	return true;
}

VirtualMemory::ReservedFixedResult FlexibleMemory::MapReserved(uint64_t vaddr, size_t len, int prot, VirtualMemory::Mode mode,
                                                               Graphics::GpuMemoryMode gpu_mode)
{
	EXIT_IF(!g_memory_lifecycle_owned);
	if (!VirtualMemory::HasReservationOverlap(vaddr, len))
	{
		return VirtualMemory::ReservedFixedResult::NotReserved;
	}
	uint64_t total = 0;
	uint32_t count = 0;
	{
		Core::LockGuard lock(m_mutex);
		if (len == 0 || m_allocated_total > Size() || static_cast<uint64_t>(len) > Size() - m_allocated_total ||
		    m_allocated.Size() == (std::numeric_limits<uint32_t>::max)())
		{
			return VirtualMemory::ReservedFixedResult::Failed;
		}
		for (const auto& block: m_allocated)
			if (block.cleanup_pending && RangesOverlap(vaddr, len, block.map_vaddr, block.map_size))
				return VirtualMemory::ReservedFixedResult::Failed;
		// Capacity and scalar owner state remain stable under the lifecycle guard.
		m_allocated.Expand(1);
		total = m_allocated_total;
		count = m_allocated.Size();
	}
	// Core admission/abort/publication can wait for a CPU reader that resolves
	// this owner's identity. Keep the owner mutex available through all of them.
	VirtualMemory::MutationTransaction transaction;
	const auto                         result = VirtualMemory::BeginAllocFixedInReservation(vaddr, len, mode, &transaction);
	if (result != VirtualMemory::ReservedFixedResult::Begun) return result;

	{
		Core::LockGuard lock(m_mutex);
		EXIT_IF(m_allocated_total != total || m_allocated.Size() != count);
		AllocatedBlock block {};
		block.map_vaddr = vaddr;
		block.map_size  = len;
		block.prot      = prot;
		block.mode      = mode;
		block.gpu_mode  = gpu_mode;
		m_allocated.Add(block);
		m_allocated_total += len;
	}
	transaction.Commit();
	return VirtualMemory::ReservedFixedResult::Begun;
}

OwnerReleaseResult FlexibleMemory::UnmapCoreFirst(uint64_t vaddr, uint64_t size, Graphics::GpuMemoryMode* gpu_mode)
{
	if (gpu_mode == nullptr) return OwnerReleaseResult::Invalid;
	EXIT_IF(!g_memory_lifecycle_owned);
	AllocatedBlock snapshot {};
	bool found = false;
	{
		Core::LockGuard lock(m_mutex);
		for (const auto& block: m_allocated)
		{
			if (block.cleanup_pending && RangesOverlap(vaddr, size, block.map_vaddr, block.map_size))
				return OwnerReleaseResult::CoreFailed;
			if (block.map_vaddr == 0 || vaddr < block.map_vaddr || vaddr - block.map_vaddr >= block.map_size) continue;
			if (block.map_vaddr != vaddr || block.map_size != size) return OwnerReleaseResult::Invalid;
			snapshot = block;
			found = true;
			break;
		}
	}
	if (!found) return OwnerReleaseResult::NotFound;
	*gpu_mode = snapshot.gpu_mode;
	if (snapshot.gpu_mode != Graphics::GpuMemoryMode::NoAccess) WaitForGpuOwner();
	if (!PublishBackingBeforeUnmap(vaddr, size)) return OwnerReleaseResult::CoreFailed;
	if (!VirtualMemory::Free(vaddr)) return OwnerReleaseResult::CoreFailed;

	Core::LockGuard lock(m_mutex);
	for (auto& block: m_allocated)
	{
		if (block.map_vaddr != snapshot.map_vaddr || block.map_size != snapshot.map_size) continue;
		EXIT_IF(block.cleanup_pending || block.prot != snapshot.prot || block.mode != snapshot.mode ||
		        block.gpu_mode != snapshot.gpu_mode);
		block.cleanup_pending = true;
		return OwnerReleaseResult::Released;
	}
	EXIT("flexible unmap owner changed during lifecycle transaction");
	return OwnerReleaseResult::CoreFailed;
}

bool FlexibleMemory::UpdateProtection(uint64_t vaddr, uint64_t size, int prot, VirtualMemory::Mode mode,
                                      Graphics::GpuMemoryMode gpu_mode, uint64_t* owner_vaddr, uint64_t* owner_size,
                                      bool* needs_gpu_allocation)
{
	if (owner_vaddr == nullptr || owner_size == nullptr || needs_gpu_allocation == nullptr) return false;

	Core::LockGuard lock(m_mutex);
	for (auto& block: m_allocated)
	{
		if (block.cleanup_pending || vaddr < block.map_vaddr || vaddr - block.map_vaddr >= block.map_size) continue;
		const uint64_t offset = vaddr - block.map_vaddr;
		if (size == 0 || size > block.map_size - offset) return false;
		const auto previous_gpu_mode = block.gpu_mode;
		*owner_vaddr                = block.map_vaddr;
		*owner_size                 = block.map_size;
		if (offset == 0 && size == block.map_size)
		{
			block.prot     = prot;
			block.mode     = mode;
			block.gpu_mode = gpu_mode;
		} else
		{
			// Partial protection runs are represented by Core VM. The coarse
			// HLE owner keeps its prior query value and only widens GPU cleanup.
			block.gpu_mode = MergeGpuMemoryMode(block.gpu_mode, gpu_mode);
		}
		*needs_gpu_allocation = previous_gpu_mode == Graphics::GpuMemoryMode::NoAccess &&
		                        block.gpu_mode != Graphics::GpuMemoryMode::NoAccess;
		return true;
	}
	return false;
}

bool FlexibleMemory::CompleteUnmap(uint64_t vaddr, uint64_t size)
{
	Core::LockGuard lock(m_mutex);
	uint32_t        index = 0;
	for (const auto& block: m_allocated)
	{
		if (block.cleanup_pending && block.map_vaddr == vaddr && block.map_size == size)
		{
			m_allocated.RemoveAt(index);
			m_allocated_total -= size;
			return true;
		}
		index++;
	}
	return false;
}

bool FlexibleMemory::Find(uint64_t vaddr, uint64_t* base_addr, size_t* len, int* prot, VirtualMemory::Mode* mode,
                          Graphics::GpuMemoryMode* gpu_mode)
{
	Core::LockGuard lock(m_mutex);

	return std::any_of(m_allocated.begin(), m_allocated.end(),
	                   [vaddr, base_addr, len, prot, mode, gpu_mode](auto& b)
	                   {
		                   if (!b.cleanup_pending && vaddr >= b.map_vaddr && vaddr < b.map_vaddr + b.map_size)
		                   {
			                   if (base_addr != nullptr)
			                   {
				                   *base_addr = b.map_vaddr;
			                   }
			                   if (len != nullptr)
			                   {
				                   *len = b.map_size;
			                   }
			                   if (prot != nullptr)
			                   {
				                   *prot = b.prot;
			                   }
			                   if (mode != nullptr)
			                   {
				                   *mode = b.mode;
			                   }
			                   if (gpu_mode != nullptr)
			                   {
				                   *gpu_mode = b.gpu_mode;
			                   }

			                   return true;
		                   }
		                   return false;
	                   });
}

uint64_t FlexibleMemory::Available()
{
	Core::LockGuard lock(m_mutex);

	return Size() - m_allocated_total;
}

int32_t LUMEN_SYSV_ABI KernelReserveVirtualRange(void** address, uint64_t length, int32_t flags, uint64_t alignment)
{
	PRINT_NAME();
	KernelVMMutationTraceScope trace {"reserve-range", reinterpret_cast<uint64_t>(address), length,
	                                 0, flags, 0, alignment};

#if (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(__aarch64__) && defined(LUMEN_FEXCORE_RUNTIME)) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && LUMEN_ABI == LUMEN_ABI_ARM64_V8A && \
     defined(LUMEN_ANDROID_FEXCORE_RUNTIME))
	constexpr uint64_t orbis_granularity = 0x4000;
	const auto         is_power_of_two   = [](uint64_t value) { return value != 0 && (value & (value - 1)) == 0; };
	if (flags != 0 || length == 0 || length % orbis_granularity != 0 || !is_power_of_two(alignment) || alignment < orbis_granularity)
	{
		return trace.Finish(KERNEL_ERROR_EINVAL, "invalid-arguments");
	}

	const uint64_t guest_record = reinterpret_cast<uint64_t>(address);
	// Match the pthread/TLS admission budget: the writer has a two-second
	// safepoint deadline, with one further second for fresh attempts. Host
	// readmission itself is not timed and cannot retain either HLE mutex.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	constexpr uint32_t max_attempts = 4096;
	for (uint32_t attempt = 1; ; ++attempt)
	{
		VirtualMemory::AllocationDenialMetadata denial;
		const char* reason = "none";
		const int result = [&] {
			const auto finish = [&](int value, const char* cause) { reason = cause; return value; };
			// Only scalar inputs survive a wait. Acquire the lifecycle after
			// readmission, then validate and reread the complete in/out record.
			MemoryLifecycleGuard lifecycle_guard {GuestMemoryLifecycleSafeWaitMode()};
			if (!lifecycle_guard.IsAcquired()) return finish(KERNEL_ERROR_ENOMEM, "lifecycle-denied");
			uint64_t requested = 0;
			if (!Loader::ValidateGuestMemory(guest_record, sizeof(requested), true) ||
			    !Loader::ReadGuestMemory(guest_record, &requested, sizeof(requested)))
				return finish(KERNEL_ERROR_EFAULT, "guest-record-unavailable");
			trace.Requested(requested);
			if ((requested != 0 && requested % alignment != 0) || requested > (std::numeric_limits<uint64_t>::max)() - length ||
			    (requested == 0 && length > (std::numeric_limits<uint64_t>::max)() - alignment))
				return finish(KERNEL_ERROR_EINVAL, "invalid-requested-address");

			std::lock_guard<std::mutex> lock(g_virtual_range_mutex);
			if (g_virtual_range_cleanup_pending || FailHleSlotPreflightForTest() || !EnsureVirtualRangeOwnerCapacity())
				return finish(KERNEL_ERROR_ENOMEM, "owner-preflight-failed");
			const size_t owner_index = g_virtual_range_owner_num++;
			g_virtual_range_owners[owner_index] = {};
			VirtualMemory::ReservationHandle handle {};
			VirtualMemory::MutationTransaction transaction;
			if (!VirtualMemory::BeginReserveAligned(requested, length, alignment, &handle, &transaction,
			                                       VirtualMemory::ReservationPlacement::PreferAddress, &denial))
			{
				EraseVirtualRangeOwner(owner_index);
				return finish(KERNEL_ERROR_ENOMEM, "core-reserve-failed");
			}
			g_virtual_range_owners[owner_index].handle = handle;
			trace.Output(handle.base);
			if (FailFinalGuestWriteForTest() || !Loader::ValidateGuestMemory(guest_record, sizeof(handle.base), true) ||
			    !Loader::WriteGuestMemory(guest_record, &handle.base, sizeof(handle.base)))
			{
				const bool aborted = transaction.Abort();
				if (aborted) EraseVirtualRangeOwner(owner_index);
				else
				{
					g_virtual_range_owners[owner_index].pending_cleanup = true;
					g_virtual_range_cleanup_pending = true;
				}
				return finish(KERNEL_ERROR_EFAULT, aborted ? "final-output-aborted" : "final-output-cleanup-pending");
			}
			transaction.Commit();
			EraseRangeNames(handle.base, handle.size);
			return finish(OK, "reserved");
		}();
		// The attempt has destroyed its transaction, placeholder and both
		// local locks. Only an effect-free provider conflict permits donation;
		// invalid arguments, allocation failures and retained leases do not.
		if (result != KERNEL_ERROR_ENOMEM || denial.reason != VirtualMemory::AllocationFailure::AdmissionDenied || !denial.retryable)
			return trace.Finish(result, reason);
		if (attempt == max_attempts || std::chrono::steady_clock::now() >= deadline)
			return trace.Finish(KERNEL_ERROR_ENOMEM, "admission-retry-exhausted");
		trace.Retry(attempt);
		bool waited = false;
		{
			Loader::ScopedGuestHostWait host_wait;
			waited = host_wait.IsActive();
			if (waited) Core::Thread::SleepMicro(1000);
		}
		if (!waited) return trace.Finish(KERNEL_ERROR_ENOMEM, "admission-retry-context-denied");
		if (std::chrono::steady_clock::now() >= deadline)
			return trace.Finish(KERNEL_ERROR_ENOMEM, "admission-retry-exhausted");
	}
#else
	(void)address;
	(void)length;
	(void)flags;
	(void)alignment;
	return trace.Finish(KERNEL_ERROR_ENOMEM, "unsupported-platform");
#endif
}

int32_t LUMEN_SYSV_ABI KernelSetVirtualRangeName(uint64_t address, uint64_t length, uint64_t guest_name)
{
	PRINT_NAME();
	MemoryLifecycleGuard lifecycle_guard {GuestMemoryLifecycleMode()};
	if (!lifecycle_guard.IsAcquired()) return KERNEL_ERROR_EBUSY;

	std::array<char, 32> name {};
	switch (ReadGuestRangeName(guest_name, &name))
	{
		case GuestNameResult::Fault: return KERNEL_ERROR_EFAULT;
		case GuestNameResult::TooLong: return KERNEL_ERROR_ENAMETOOLONG;
		case GuestNameResult::Ok: break;
	}

	const uint64_t aligned_address = address & ~(ORBIS_VIRTUAL_RANGE_GRANULARITY - 1);
	uint64_t       aligned_length  = 0;
	uint64_t       aligned_end     = 0;
	if (!RoundUpWithoutOverflow(length, ORBIS_VIRTUAL_RANGE_GRANULARITY, &aligned_length) ||
	    (aligned_length != 0 && !AddWithoutOverflow(aligned_address, aligned_length, &aligned_end)))
	{
		return KERNEL_ERROR_EINVAL;
	}
	if (aligned_length == 0) return OK;

	std::vector<VirtualQueryRegion> mappings;
	std::vector<VirtualQueryRegion> reservations;
	SnapshotVirtualQueryMappings(&mappings);
	SnapshotVirtualQueryReservations(&reservations);
	// Core mappings owned by unrelated host subsystems are deliberately absent:
	// a name must never outlive or pre-seed an HLE mapping/reservation.
	std::vector<RangeNameInterval> managed_intervals;
	managed_intervals.reserve(mappings.size() + reservations.size());
	for (const auto& mapping: mappings)
	{
		managed_intervals.push_back(RangeNameInterval {mapping.begin, mapping.end});
	}
	for (const auto& reservation: reservations)
	{
		managed_intervals.push_back(RangeNameInterval {reservation.begin, reservation.end});
	}
	if (!ReplaceRangeNameWithinManagedIntervals(aligned_address, aligned_end, std::move(managed_intervals), name))
	{
		return KERNEL_ERROR_ENOMEM;
	}
	std::array<char, 33> printable_name {};
	std::memcpy(printable_name.data(), name.data(), name.size());
	printf("\t range = [0x%016" PRIx64 ", 0x%016" PRIx64 ")\n", aligned_address, aligned_end);
	printf("\t name  = %s\n", printable_name.data());
	return OK;
}

int32_t LUMEN_SYSV_ABI KernelVirtualQuery(uint64_t address, int32_t flags, uint64_t guest_info, uint64_t info_size)
{
	PRINT_NAME();
	KernelVMMutationTraceScope trace {"virtual-query", guest_info, info_size, 0, flags, 0, 0, address, true};
	(void)info_size;
	if (guest_info == 0)
	{
		return trace.Finish(KERNEL_ERROR_EFAULT, "guest-record-unavailable");
	}

	// A top-level FEX HLE query may release its guest reader while waiting for
	// another lifecycle owner. Only scalar inputs precede that wait: the output
	// record is validated after readmission, without retaining a guest lease.
	// Nested calls, active leases and owner reentry keep the nonblocking refusal.
	MemoryLifecycleGuard lifecycle_guard {GuestMemoryLifecycleSafeWaitMode()};
	if (!lifecycle_guard.IsAcquired()) return trace.Finish(KERNEL_ERROR_EBUSY, "lifecycle-denied");
	if (!Loader::ValidateGuestMemory(guest_info, sizeof(OrbisVirtualQueryInfo), true))
	{
		return trace.Finish(KERNEL_ERROR_EFAULT, "guest-record-unavailable");
	}
	if (g_physical_memory == nullptr || g_flexible_memory == nullptr) return trace.Finish(KERNEL_ERROR_EACCES, "owner-unavailable");

	std::vector<VirtualQueryRegion> mappings;
	std::vector<VirtualQueryRegion> reservations;
	SnapshotVirtualQueryMappings(&mappings);
	SnapshotVirtualQueryReservations(&reservations);

	VirtualQueryRegion selected {};
	if (!SelectVirtualQueryRegion(address, flags, mappings, reservations, &selected)) return trace.Finish(KERNEL_ERROR_EACCES, "region-not-found");
	const uint64_t selected_address = ContainsAddress(selected, address) ? address : selected.begin;
	if (!RefineVirtualQueryRegionWithCore(selected_address, &selected)) return trace.Finish(KERNEL_ERROR_EACCES, "core-refinement-failed");

	std::array<char, 32> name {};
	ApplyVirtualQueryRangeName(selected_address, &selected, &name);
	if (selected.begin >= selected.end) return trace.Finish(KERNEL_ERROR_EACCES, "invalid-region");

	OrbisVirtualQueryInfo output {};
	output.start          = selected.begin;
	output.end            = selected.end;
	output.offset         = selected.offset;
	output.protection     = selected.reservation ? 0 : selected.protection;
	output.memory_type    = selected.reservation ? 0 : selected.memory_type;
	output.category_flags = selected.reservation ? 0 : selected.category_flags;
	std::memcpy(output.name, name.data(), name.size());

	if (!Loader::ValidateGuestMemory(guest_info, sizeof(output), true) || !Loader::WriteGuestMemory(guest_info, &output, sizeof(output)))
	{
		return trace.Finish(KERNEL_ERROR_EFAULT, "final-output-unavailable");
	}
	trace.RegionOutput(output.start, output.end, output.protection, selected.reservation);
	printf("\t address = 0x%016" PRIx64 "\n", address);
	printf("\t flags   = %" PRId32 "\n", flags);
	printf("\t range   = [0x%016" PRIx64 ", 0x%016" PRIx64 ")\n", output.start, output.end);
	return trace.Finish(OK, selected.reservation ? "query-reserved" : "query-visible");
}

int32_t LUMEN_SYSV_ABI KernelMapNamedFlexibleMemory(void** addr_in_out, size_t len, int prot, int flags, const char* name)
{
	PRINT_NAME();
	KernelVMMutationTraceScope trace {"map-flexible", reinterpret_cast<uint64_t>(addr_in_out),
	                                 len, prot, flags, 0, 0};

	EXIT_IF(g_flexible_memory == nullptr);

	EXIT_NOT_IMPLEMENTED(addr_in_out == nullptr);
	EXIT_NOT_IMPLEMENTED(flags != 0);
	MemoryLifecycleGuard lifecycle_guard {GuestMemoryLifecycleMode()};
	if (!lifecycle_guard.IsAcquired()) return trace.Finish(KERNEL_ERROR_EBUSY, "lifecycle-denied");
	std::array<char, 32> mapped_name {};
	const bool           mapped_name_valid = ReadGuestNamePrefix(reinterpret_cast<uint64_t>(name), &mapped_name);

	VirtualMemory::Mode     mode     = VirtualMemory::Mode::NoAccess;
	Graphics::GpuMemoryMode gpu_mode = Graphics::GpuMemoryMode::NoAccess;

	switch (prot)
	{
		case 0: mode = VirtualMemory::Mode::NoAccess; break;
		case 1: mode = VirtualMemory::Mode::Read; break;
		case 2:
		case 3: mode = VirtualMemory::Mode::ReadWrite; break;
		case 4: mode = VirtualMemory::Mode::Execute; break;
		case 5: mode = VirtualMemory::Mode::ExecuteRead; break;
		case 6:
		case 7: mode = VirtualMemory::Mode::ExecuteReadWrite; break;
		case 0x32:
		case 0x33:
			mode     = VirtualMemory::Mode::ReadWrite;
			gpu_mode = Graphics::GpuMemoryMode::ReadWrite;
			break;
		default: EXIT("unknown prot: %d\n", prot);
	}

	auto     in_addr                 = reinterpret_cast<uint64_t>(*addr_in_out);
	trace.Requested(in_addr);
	uint64_t out_addr                = 0;
	bool     owner_committed         = false;
	bool     reserved_mapping_failed = false;
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(__aarch64__) && defined(LUMEN_FEXCORE_RUNTIME)) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && LUMEN_ABI == LUMEN_ABI_ARM64_V8A && \
     defined(LUMEN_ANDROID_FEXCORE_RUNTIME))
	if (in_addr != 0)
	{
		const auto reserved_result = g_flexible_memory->MapReserved(in_addr, len, prot, mode, gpu_mode);
		if (reserved_result == VirtualMemory::ReservedFixedResult::Begun)
		{
			out_addr        = in_addr;
			owner_committed = true;
		} else if (reserved_result == VirtualMemory::ReservedFixedResult::Failed)
		{
			reserved_mapping_failed = true;
		}
	}
#endif
	if (!owner_committed && !reserved_mapping_failed) out_addr = VirtualMemory::Alloc(in_addr, len, mode);
	*addr_in_out = reinterpret_cast<void*>(out_addr);
	trace.Output(out_addr);
	if (out_addr == 0)
	{
		return trace.Finish(reserved_mapping_failed ? KERNEL_ERROR_EBUSY : KERNEL_ERROR_ENOMEM,
		                    reserved_mapping_failed ? "reservation-failed" : "allocation-failed");
	}

	if (!owner_committed && !g_flexible_memory->Map(out_addr, len, prot, mode, gpu_mode))
	{
		printf(FG_RED "\t [Fail]\n" FG_DEFAULT);
		VirtualMemory::Free(out_addr);
		return trace.Finish(KERNEL_ERROR_ENOMEM, "owner-failed");
	}

	printf("\t in_addr  = 0x%016" PRIx64 "\n", in_addr);
	printf("\t out_addr = 0x%016" PRIx64 "\n", out_addr);
	printf("\t size     = %" PRIu64 "\n", len);
	printf("\t mode     = %s\n", Core::EnumName(mode).C_Str());
	std::array<char, 33> printable_name {};
	std::memcpy(printable_name.data(), mapped_name.data(), mapped_name.size());
	printf("\t name     = %s\n", mapped_name_valid ? printable_name.data() : "<unreadable>");
	printf("\t gpu_mode = %s\n", Core::EnumName(gpu_mode).C_Str());
	ReplaceRangeName(out_addr, len, mapped_name_valid ? mapped_name : std::array<char, 32> {});

	if (gpu_mode != Graphics::GpuMemoryMode::NoAccess)
	{
		TrackGpuOwnerAllocation(out_addr, len);
	}

	if (g_alloc_callback != nullptr)
	{
		g_alloc_callback(out_addr, len);
	}

	return trace.Finish(OK, "mapped");
}

int LUMEN_SYSV_ABI KernelMapFlexibleMemory(void** addr_in_out, size_t len, int prot, int flags)
{
	return KernelMapNamedFlexibleMemory(addr_in_out, len, prot, flags, nullptr);
}

int LUMEN_SYSV_ABI KernelMunmap(uint64_t vaddr, size_t len)
{
	PRINT_NAME();

	printf("\t start = 0x%016" PRIx64 "\n", vaddr);
	printf("\t len   = 0x%016" PRIx64 "\n", len);

	EXIT_IF(g_physical_memory == nullptr);
	EXIT_IF(g_flexible_memory == nullptr);

	if (vaddr < 0 || len == 0)
	{
		return KERNEL_ERROR_EINVAL;
	}
	MemoryLifecycleGuard lifecycle_guard {GuestMemoryLifecycleMode()};
	if (!lifecycle_guard.IsAcquired()) return KERNEL_ERROR_EBUSY;

	Graphics::GpuMemoryMode gpu_mode       = Graphics::GpuMemoryMode::NoAccess;
	bool                    physical_owner = true;
	OwnerReleaseResult      result         = g_physical_memory->UnmapCoreFirst(vaddr, len, &gpu_mode);
	if (result == OwnerReleaseResult::NotFound)
	{
		physical_owner = false;
		result         = g_flexible_memory->UnmapCoreFirst(vaddr, len, &gpu_mode);
	}
	if (result == OwnerReleaseResult::NotFound || result == OwnerReleaseResult::Invalid) return KERNEL_ERROR_EINVAL;
	if (result == OwnerReleaseResult::CoreFailed) return KERNEL_ERROR_EBUSY;

	if (gpu_mode != Graphics::GpuMemoryMode::NoAccess)
	{
		FreeGpuOwner(vaddr, len);
	}

	if (g_free_callback != nullptr)
	{
		g_free_callback(vaddr, len);
	}
	const bool completed = physical_owner ? g_physical_memory->CompleteUnmap(vaddr, len) : g_flexible_memory->CompleteUnmap(vaddr, len);
	if (!completed) return KERNEL_ERROR_EBUSY;
	EraseRangeNames(vaddr, len);

	return OK;
}

size_t LUMEN_SYSV_ABI KernelGetDirectMemorySize()
{
	PRINT_NAME();

	return PhysicalMemory::Size();
}

uint32_t LUMEN_SYSV_ABI KernelIsAddressSanitizerEnabled()
{
	PRINT_NAME();
	// Lumen does not provide an AddressSanitizer runtime for guest code.
	// Host-side sanitizer instrumentation does not enable that guest capability.
	return 0;
}

int LUMEN_SYSV_ABI KernelDirectMemoryQuery(int64_t offset, int flags, void* info, size_t info_size)
{
	PRINT_NAME();

	EXIT_IF(g_physical_memory == nullptr);

	printf("\t offset    = 0x%016" PRIx64 "\n", offset);
	printf("\t flags     = 0x%08" PRIx32 "\n", flags);
	printf("\t info_size = 0x%016" PRIx64 "\n", info_size);

	struct QueryInfo
	{
		int64_t start;
		int64_t end;
		int     memory_type;
	};

	if (offset < 0 || info_size != sizeof(QueryInfo) || info == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}
	MemoryLifecycleGuard lifecycle_guard {GuestMemoryLifecycleMode()};
	if (!lifecycle_guard.IsAcquired()) return KERNEL_ERROR_EBUSY;

	PhysicalMemory::AllocatedBlock block {};
	if (!g_physical_memory->Find(offset, flags != 0, &block))
	{
		printf(FG_RED "\t[Fail]\n" FG_DEFAULT);
		return KERNEL_ERROR_EACCES;
	}

	auto* query_info = static_cast<QueryInfo*>(info);

	query_info->start       = static_cast<int64_t>(block.start_addr);
	query_info->end         = static_cast<int64_t>(block.start_addr + block.size);
	query_info->memory_type = block.memory_type;

	printf("\t start       = %016" PRIx64 "\n", query_info->start);
	printf("\t end         = %016" PRIx64 "\n", query_info->end);
	printf("\t memory_type = %d\n", query_info->memory_type);
	printf(FG_GREEN "\t[Ok]\n" FG_DEFAULT);

	return OK;
}

int LUMEN_SYSV_ABI KernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t len, size_t alignment, int memory_type,
                                              int64_t* phys_addr_out)
{
	PRINT_NAME();

	EXIT_IF(g_physical_memory == nullptr);

	printf("\t search_start = 0x%016" PRIx64 "\n", search_start);
	printf("\t search_end   = 0x%016" PRIx64 "\n", search_end);
	printf("\t len          = 0x%016" PRIx64 "\n", len);
	printf("\t alignment    = 0x%016" PRIx64 "\n", alignment);
	printf("\t memory_type  = %d\n", memory_type);

	if (search_start < 0 || search_end <= search_start || len == 0 || phys_addr_out == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}
	MemoryLifecycleGuard lifecycle_guard {GuestMemoryLifecycleMode()};
	if (!lifecycle_guard.IsAcquired()) return KERNEL_ERROR_EAGAIN;

	uint64_t addr = 0;
	if (!g_physical_memory->Alloc(search_start, search_end, len, alignment, &addr, memory_type))
	{
		printf(FG_RED "\t[Fail]\n" FG_DEFAULT);
		return KERNEL_ERROR_EAGAIN;
	}

	*phys_addr_out = static_cast<int64_t>(addr);

	printf("\tphys_addr    = %016" PRIx64 "\n", addr);
	printf(FG_GREEN "\t[Ok]\n" FG_DEFAULT);

	return OK;
}

int LUMEN_SYSV_ABI KernelAllocateMainDirectMemory(size_t len, size_t alignment, int memory_type, int64_t* phys_addr_out)
{
	PRINT_NAME();

	EXIT_IF(g_physical_memory == nullptr);

	printf("\t len          = 0x%016" PRIx64 "\n", len);
	printf("\t alignment    = 0x%016" PRIx64 "\n", alignment);
	printf("\t memory_type  = %d\n", memory_type);

	if (len == 0 || phys_addr_out == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}
	MemoryLifecycleGuard lifecycle_guard {GuestMemoryLifecycleMode()};
	if (!lifecycle_guard.IsAcquired()) return KERNEL_ERROR_EAGAIN;

	uint64_t addr = 0;
	if (!g_physical_memory->Alloc(0, UINT64_MAX, len, alignment, &addr, memory_type))
	{
		printf(FG_RED "\t[Fail]\n" FG_DEFAULT);
		return KERNEL_ERROR_EAGAIN;
	}

	*phys_addr_out = static_cast<int64_t>(addr);

	printf("\tphys_addr    = %016" PRIx64 "\n", addr);
	printf(FG_GREEN "\t[Ok]\n" FG_DEFAULT);

	return OK;
}

int LUMEN_SYSV_ABI KernelCheckedReleaseDirectMemory(int64_t start, size_t len)
{
	PRINT_NAME();

	printf("\t start = 0x%016" PRIx64 "\n", start);
	printf("\t len   = 0x%016" PRIx64 "\n", len);

	EXIT_IF(g_physical_memory == nullptr);

	if (start < 0 || len == 0)
	{
		return KERNEL_ERROR_EINVAL;
	}
	MemoryLifecycleGuard lifecycle_guard {GuestMemoryLifecycleMode()};
	if (!lifecycle_guard.IsAcquired()) return KERNEL_ERROR_EBUSY;

	// Console profile (CUH-1216A, fw 12.00, 1 October 2026): the checked
	// variant validates the whole range before any effect. A start outside
	// every allocation is ENOENT; a range that is not exactly covered by whole
	// live allocations is EINVAL. Two contiguous unmapped allocations return
	// success, but their absence was not queried; releasing both is the local
	// Lumen model. A single CPU-mapped allocation succeeds and loses its mapping.
	std::vector<PhysicalMemory::AllocatedBlock> covered;
	switch (g_physical_memory->CollectCheckedRange(static_cast<uint64_t>(start), len, &covered))
	{
		case PhysicalMemory::CheckedRangeResult::NotFound: return KERNEL_ERROR_ENOENT;
		case PhysicalMemory::CheckedRangeResult::Invalid: return KERNEL_ERROR_EINVAL;
		case PhysicalMemory::CheckedRangeResult::Busy: return KERNEL_ERROR_EBUSY;
		case PhysicalMemory::CheckedRangeResult::Ready: break;
	}

	// The console cross-allocation profile was measured without mappings.
	// Lumen cannot yet admit all mapped owners as one atomic VM release:
	// a later Free denial would otherwise leave an already-released prefix.
	// Keep that unqualified lifecycle fail-closed with the existing local EBUSY
	// before any effect; single mapped owners and unmapped ranges remain valid.
	if (covered.size() > 1 && std::any_of(covered.begin(), covered.end(), [](const auto& block)
	                                    { return block.map_vaddr != 0 || block.map_size != 0; }))
	{
		return KERNEL_ERROR_EBUSY;
	}

	for (const auto& block: covered)
	{
		uint64_t                 vaddr    = 0;
		uint64_t                 size     = 0;
		Graphics::GpuMemoryMode  gpu_mode = Graphics::GpuMemoryMode::NoAccess;
		const OwnerReleaseResult result =
		    g_physical_memory->ReleaseCoreFirst(block.start_addr, block.size, &vaddr, &size, &gpu_mode);
		if (result != OwnerReleaseResult::Released) return KERNEL_ERROR_EBUSY;

		if (gpu_mode != Graphics::GpuMemoryMode::NoAccess)
		{
			FreeGpuOwner(vaddr, size);
		}
		if (vaddr != 0 && size != 0 && g_free_callback != nullptr)
		{
			g_free_callback(vaddr, size);
		}
		if (!g_physical_memory->CompleteRelease(block.start_addr, block.size))
		{
			return KERNEL_ERROR_EBUSY;
		}
		if (vaddr != 0 && size != 0) EraseRangeNames(vaddr, size);
	}

	return OK;
}

int LUMEN_SYSV_ABI KernelReleaseDirectMemory(int64_t start, size_t len)
{
	PRINT_NAME();

	printf("\t start = 0x%016" PRIx64 "\n", start);
	printf("\t len   = 0x%016" PRIx64 "\n", len);

	EXIT_IF(g_physical_memory == nullptr);

	if (start < 0 || len == 0)
	{
		return KERNEL_ERROR_EINVAL;
	}
	MemoryLifecycleGuard lifecycle_guard {GuestMemoryLifecycleMode()};
	if (!lifecycle_guard.IsAcquired()) return KERNEL_ERROR_EBUSY;

	uint64_t                 vaddr    = 0;
	uint64_t                 size     = 0;
	Graphics::GpuMemoryMode  gpu_mode = Graphics::GpuMemoryMode::NoAccess;
	const OwnerReleaseResult result   = g_physical_memory->ReleaseCoreFirst(static_cast<uint64_t>(start), len, &vaddr, &size, &gpu_mode);
	// Unchecked console behaviour: the boundary of the allocation containing the
	// start is still validated, but a start outside every allocation is a
	// successful no-op (measured 1 October 2026).
	if (result == OwnerReleaseResult::NotFound) return OK;
	if (result == OwnerReleaseResult::Invalid) return KERNEL_ERROR_EINVAL;
	if (result == OwnerReleaseResult::CoreFailed) return KERNEL_ERROR_EBUSY;

	if (gpu_mode != Graphics::GpuMemoryMode::NoAccess)
	{
		FreeGpuOwner(vaddr, size);
	}

	if (vaddr != 0 && size != 0 && g_free_callback != nullptr)
	{
		g_free_callback(vaddr, size);
	}
	if (!g_physical_memory->CompleteRelease(static_cast<uint64_t>(start), len))
	{
		return KERNEL_ERROR_EBUSY;
	}
	if (vaddr != 0 && size != 0) EraseRangeNames(vaddr, size);

	return OK;
}

int KernelMapDirectMemoryImpl(void** addr, size_t len, int prot, int flags, int64_t direct_memory_start, size_t alignment,
	                          uint64_t guest_name)
{
	PRINT_NAME();
	KernelVMMutationTraceScope trace {"map-direct", reinterpret_cast<uint64_t>(addr), len,
	                                 prot, flags, static_cast<uint64_t>(direct_memory_start), alignment};

	if (g_physical_memory == nullptr) return trace.Finish(KERNEL_ERROR_EBUSY, "owner-unavailable");
	const bool fixed           = flags == 0x10;
	const auto is_power_of_two = [](uint64_t value) { return value != 0 && (value & (value - 1)) == 0; };
	if (addr == nullptr || (flags != 0 && !fixed) || len == 0 || len % ORBIS_VIRTUAL_RANGE_GRANULARITY != 0 ||
	    direct_memory_start < 0)
	{
		return trace.Finish(KERNEL_ERROR_EINVAL, "invalid-arguments");
	}
	const uint64_t physical_start = static_cast<uint64_t>(direct_memory_start);
	if (physical_start % ORBIS_VIRTUAL_RANGE_GRANULARITY != 0 ||
	    physical_start > (std::numeric_limits<uint64_t>::max)() - static_cast<uint64_t>(len) ||
	    (!fixed && (!is_power_of_two(alignment) || alignment < ORBIS_VIRTUAL_RANGE_GRANULARITY)) ||
	    (fixed && alignment != 0 && (!is_power_of_two(alignment) || alignment < ORBIS_VIRTUAL_RANGE_GRANULARITY)))
	{
		return trace.Finish(KERNEL_ERROR_EINVAL, "invalid-physical-alignment");
	}

	VirtualMemory::Mode     mode     = VirtualMemory::Mode::NoAccess;
	Graphics::GpuMemoryMode gpu_mode = Graphics::GpuMemoryMode::NoAccess;

	switch (prot)
	{
		case 0x00: mode = VirtualMemory::Mode::NoAccess; break;
		case 0x01: mode = VirtualMemory::Mode::Read; break;
		case 0x02:
		case 0x03: mode = VirtualMemory::Mode::ReadWrite; break;
		case 0x04: mode = VirtualMemory::Mode::Execute; break;
		case 0x05: mode = VirtualMemory::Mode::ExecuteRead; break;
		case 0x06:
		case 0x07: mode = VirtualMemory::Mode::ExecuteReadWrite; break;
		case 0x32:
		case 0x33:
			mode     = VirtualMemory::Mode::ReadWrite;
			gpu_mode = Graphics::GpuMemoryMode::ReadWrite;
			break;
		default: return trace.Finish(KERNEL_ERROR_EINVAL, "invalid-protection");
	}

	MemoryLifecycleGuard lifecycle_guard {GuestMemoryLifecycleMode()};
	if (!lifecycle_guard.IsAcquired()) return trace.Finish(KERNEL_ERROR_EBUSY, "lifecycle-denied");
	const uint64_t guest_record = reinterpret_cast<uint64_t>(addr);
	uint64_t       in_addr      = 0;
	if (!Loader::ValidateGuestMemory(guest_record, sizeof(in_addr), true) ||
	    !Loader::ReadGuestMemory(guest_record, &in_addr, sizeof(in_addr)))
	{
		return trace.Finish(KERNEL_ERROR_EINVAL, "guest-record-unavailable");
	}
	trace.Requested(in_addr);
	if (in_addr > (std::numeric_limits<uint64_t>::max)() - static_cast<uint64_t>(len) ||
	    (fixed && (in_addr == 0 || in_addr % ORBIS_VIRTUAL_RANGE_GRANULARITY != 0 ||
	               (alignment != 0 && in_addr % alignment != 0))))
	{
		return trace.Finish(KERNEL_ERROR_EINVAL, "invalid-requested-address");
	}
	if (!fixed)
	{
		uint64_t aligned_hint = 0;
		if (!RoundUpWithoutOverflow(in_addr, alignment, &aligned_hint) ||
		    aligned_hint > (std::numeric_limits<uint64_t>::max)() - static_cast<uint64_t>(len))
		{
			return trace.Finish(KERNEL_ERROR_EINVAL, "invalid-address-hint");
		}
	}

	std::array<char, 32> mapped_name {};
	const bool           mapped_name_valid = guest_name != 0 && ReadGuestNamePrefix(guest_name, &mapped_name);
	if (guest_name != 0)
	{
		std::array<char, 33> printable_name {};
		std::memcpy(printable_name.data(), mapped_name.data(), mapped_name.size());
		printf("\t name = %s\n", mapped_name_valid ? printable_name.data() : "<unreadable>");
	}

	uint64_t              out_addr = 0;
	const DirectMapResult map_result =
	    g_physical_memory->MapDirect(in_addr, physical_start, len, prot, mode, gpu_mode, fixed, alignment, &out_addr);
	trace.Output(out_addr);
	if (map_result != DirectMapResult::Mapped)
	{
		const uint64_t empty_output = 0;
		if (!Loader::WriteGuestMemory(guest_record, &empty_output, sizeof(empty_output)))
			return trace.Finish(KERNEL_ERROR_EINVAL, "failure-output-unavailable");
		return trace.Finish(map_result == DirectMapResult::AddressUnavailable ? KERNEL_ERROR_ENOMEM : KERNEL_ERROR_EBUSY,
		                    map_result == DirectMapResult::AddressUnavailable ? "address-unavailable" : "owner-or-reservation-busy");
	}

	printf("\t in_addr  = 0x%016" PRIx64 "\n", in_addr);
	printf("\t out_addr = 0x%016" PRIx64 "\n", out_addr);
	printf("\t phys_start = 0x%016" PRIx64 "\n", physical_start);
	printf("\t size     = 0x%016" PRIx64 "\n", len);
	printf("\t mode     = %s\n", Core::EnumName(mode).C_Str());
	printf("\t align    = 0x%016" PRIx64 "\n", alignment);
	printf("\t gpu_mode = %s\n", Core::EnumName(gpu_mode).C_Str());

	if (!Loader::WriteGuestMemory(guest_record, &out_addr, sizeof(out_addr)))
	{
		return trace.Finish(g_physical_memory->RollbackDirectMap(out_addr, len) ? KERNEL_ERROR_EINVAL : KERNEL_ERROR_EBUSY,
		                    "success-output-unavailable");
	}
	ReplaceRangeName(out_addr, len, mapped_name_valid ? mapped_name : std::array<char, 32> {});

	if (gpu_mode != Graphics::GpuMemoryMode::NoAccess)
	{
		TrackGpuOwnerAllocation(out_addr, len);
	}

	if (g_alloc_callback != nullptr)
	{
		g_alloc_callback(out_addr, len);
	}

	printf(FG_GREEN "\t [Ok]\n" FG_DEFAULT);

	return trace.Finish(OK, "mapped");
}

int LUMEN_SYSV_ABI KernelMapDirectMemory(void** addr, size_t len, int prot, int flags, int64_t direct_memory_start, size_t alignment)
{
	return KernelMapDirectMemoryImpl(addr, len, prot, flags, direct_memory_start, alignment, 0);
}

int LUMEN_SYSV_ABI KernelMapNamedDirectMemory(void** addr, size_t len, int prot, int flags, off_t direct_memory_start, size_t alignment,
                                              const char* name)
{
	PRINT_NAME();
	return KernelMapDirectMemoryImpl(addr, len, prot, flags, direct_memory_start, alignment, reinterpret_cast<uint64_t>(name));
}

int LUMEN_SYSV_ABI KernelQueryMemoryProtection(void* addr, void** start, void** end, int* prot)
{
	PRINT_NAME();

	EXIT_IF(g_physical_memory == nullptr);
	EXIT_IF(g_flexible_memory == nullptr);

	EXIT_NOT_IMPLEMENTED(addr == nullptr);
	MemoryLifecycleGuard lifecycle_guard {GuestMemoryLifecycleMode()};
	if (!lifecycle_guard.IsAcquired()) return KERNEL_ERROR_EBUSY;

	size_t   len  = 0;
	int      p    = 0;
	uint64_t base = 0;

	if (!g_physical_memory->Find(reinterpret_cast<uint64_t>(addr), &base, &len, &p, nullptr, nullptr))
	{
		if (!g_flexible_memory->Find(reinterpret_cast<uint64_t>(addr), &base, &len, &p, nullptr, nullptr))
		{
			return KERNEL_ERROR_EACCES;
		}
	}

	if (start != nullptr)
	{
		*start = reinterpret_cast<void*>(base);
	}
	if (end != nullptr)
	{
		*end = reinterpret_cast<void*>(base + len - 1);
	}
	if (prot != nullptr)
	{
		*prot = p;
	}

	return OK;
}

int LUMEN_SYSV_ABI KernelAvailableFlexibleMemorySize(size_t* size)
{
	PRINT_NAME();

	EXIT_IF(g_flexible_memory == nullptr);

	EXIT_NOT_IMPLEMENTED(size == nullptr);
	MemoryLifecycleGuard lifecycle_guard {GuestMemoryLifecycleMode()};
	if (!lifecycle_guard.IsAcquired()) return KERNEL_ERROR_EBUSY;

	*size = g_flexible_memory->Available();

	printf("\t *size = 0x%016" PRIx64 "\n", *size);

	return OK;
}

int LUMEN_SYSV_ABI KernelMprotect(const void* addr, size_t len, int prot)
{
	PRINT_NAME();
	MemoryLifecycleGuard lifecycle_guard {GuestMemoryLifecycleMode()};
	if (!lifecycle_guard.IsAcquired()) return KERNEL_ERROR_EBUSY;

	auto vaddr = reinterpret_cast<uint64_t>(addr);

	printf("\t addr = 0x%016" PRIx64 "\n", vaddr);
	printf("\t len  = 0x%016" PRIx64 "\n", static_cast<uint64_t>(len));

	VirtualMemory::Mode     mode     = VirtualMemory::Mode::NoAccess;
	Graphics::GpuMemoryMode gpu_mode = Graphics::GpuMemoryMode::NoAccess;

	switch (prot)
	{
		case 0x11:
			mode     = VirtualMemory::Mode::Read;
			gpu_mode = Graphics::GpuMemoryMode::Read;
			break;
		case 0x12:
			mode     = VirtualMemory::Mode::ReadWrite;
			gpu_mode = Graphics::GpuMemoryMode::Read;
			break;
		default: EXIT("unknown prot: %d\n", prot);
	}

	VirtualMemory::Mode old_mode {};
	bool                ok = VirtualMemory::Protect(vaddr, len, mode, &old_mode);

	EXIT_NOT_IMPLEMENTED(!ok);
	uint64_t gpu_address          = vaddr;
	uint64_t gpu_size             = len;
	bool     needs_gpu_allocation = true;
	bool     owner_updated        = false;
	if (ok && g_physical_memory != nullptr && g_flexible_memory != nullptr)
	{
		owner_updated =
		    g_physical_memory->UpdateProtection(vaddr, len, prot, mode, gpu_mode, &gpu_address, &gpu_size, &needs_gpu_allocation);
		if (!owner_updated)
		{
			owner_updated =
			    g_flexible_memory->UpdateProtection(vaddr, len, prot, mode, gpu_mode, &gpu_address, &gpu_size, &needs_gpu_allocation);
		}
	}

	if (gpu_mode != Graphics::GpuMemoryMode::NoAccess && (!owner_updated || needs_gpu_allocation))
	{
		TrackGpuOwnerAllocation(gpu_address, gpu_size);
	}

	printf("\t prot: %s -> %s\n", Core::EnumName(old_mode).C_Str(), Core::EnumName(mode).C_Str());

	return OK;
}

int LUMEN_SYSV_ABI KernelApplyPosixProtection(uint64_t vaddr, size_t len, int prot)
{
	PRINT_NAME();
	MemoryLifecycleGuard lifecycle_guard {GuestMemoryLifecycleMode()};
	if (!lifecycle_guard.IsAcquired()) return KERNEL_ERROR_EBUSY;

	VirtualMemory::Mode     mode     = VirtualMemory::Mode::NoAccess;
	Graphics::GpuMemoryMode gpu_mode = Graphics::GpuMemoryMode::NoAccess;

	switch (prot)
	{
		case 0: mode = VirtualMemory::Mode::NoAccess; break;
		case 1: mode = VirtualMemory::Mode::Read; break;
		case 2:
		case 3: mode = VirtualMemory::Mode::ReadWrite; break;
		case 4: mode = VirtualMemory::Mode::Execute; break;
		case 5: mode = VirtualMemory::Mode::ExecuteRead; break;
		case 6:
		case 7: mode = VirtualMemory::Mode::ExecuteReadWrite; break;
		default: return KERNEL_ERROR_EINVAL;
	}

	VirtualMemory::Mode old_mode {};
	if (!VirtualMemory::Protect(vaddr, len, mode, &old_mode)) return KERNEL_ERROR_EINVAL;

	uint64_t gpu_address          = vaddr;
	uint64_t gpu_size             = len;
	bool     needs_gpu_allocation = false;
	bool     owner_updated        = false;
	if (g_physical_memory != nullptr && g_flexible_memory != nullptr)
	{
		owner_updated = g_physical_memory->UpdateProtection(vaddr, len, prot, mode, gpu_mode, &gpu_address, &gpu_size,
		                                                    &needs_gpu_allocation);
		if (!owner_updated)
		{
			owner_updated = g_flexible_memory->UpdateProtection(vaddr, len, prot, mode, gpu_mode, &gpu_address, &gpu_size,
			                                                    &needs_gpu_allocation);
		}
	}
	if (!owner_updated) return KERNEL_ERROR_EINVAL;

	if (gpu_mode != Graphics::GpuMemoryMode::NoAccess && needs_gpu_allocation)
	{
		TrackGpuOwnerAllocation(gpu_address, gpu_size);
	}

	return OK;
}

} // namespace Lumen::Libs::LibKernel::Memory

#endif // LUMEN_EMU_ENABLED
