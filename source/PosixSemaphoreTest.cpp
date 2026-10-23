#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Kernel/Semaphore.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Loader/Timer.h"
#include "Emulator/Log.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace Lumen::Libs::Posix {

bool InlinePosixSemaphoreRecordSupportedForTest(const void* slot);
bool InlinePosixSemaphoreEnvironmentValueEnabledForTest(const char* value);

} // namespace Lumen::Libs::Posix

namespace {

namespace Posix = Lumen::Libs::Posix;
namespace Kernel = Lumen::Libs::LibKernel;
namespace Sema = Kernel::Semaphore;
namespace Memory = Lumen::Core::VirtualMemory;
using Handle = Sema::KernelSema;
using PostFunction = decltype(&Posix::sem_post);
PostFunction g_post = &Posix::sem_post;

static_assert(sizeof(Handle) == 8 && sizeof(int) == 4 && sizeof(unsigned) == 4);
static_assert(sizeof(Kernel::KernelTimespec) == 16 && offsetof(Kernel::KernelTimespec, tv_sec) == 0 &&
              offsetof(Kernel::KernelTimespec, tv_nsec) == 8);
constexpr unsigned k_maximum = static_cast<unsigned>(std::numeric_limits<int32_t>::max());
constexpr int k_errno_sentinel = 193;

class Watchdog final
{
public:
	Watchdog(): m_thread([this] {
		std::unique_lock lock(m_mutex);
		if (!m_changed.wait_for(lock, std::chrono::seconds(10), [this] { return m_done; }))
		{
			std::fputs("PosixSemaphoreTest: FAIL: timed out\n", stderr);
			std::fflush(stderr);
			std::_Exit(2);
		}
	}) {}
	~Watchdog()
	{
		{
			std::lock_guard lock(m_mutex);
			m_done = true;
		}
		m_changed.notify_one();
		m_thread.join();
	}

private:
	std::mutex m_mutex;
	std::condition_variable m_changed;
	bool m_done = false;
	std::thread m_thread;
};

bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "PosixSemaphoreTest: FAIL: %s\n", message);
	return condition;
}

bool TestHostClockSampling()
{
	const auto ordered = [](int64_t first_sec, int64_t first_nsec, int64_t last_sec, int64_t last_nsec) {
		return first_sec < last_sec || (first_sec == last_sec && first_nsec <= last_nsec);
	};
	struct ClockCase { Kernel::KernelClockid guest; clockid_t native; };
	const ClockCase cases[] = {{0, CLOCK_REALTIME}, {9, CLOCK_REALTIME}, {10, CLOCK_REALTIME},
	    {4, CLOCK_MONOTONIC}, {5, CLOCK_MONOTONIC}, {7, CLOCK_MONOTONIC}, {8, CLOCK_MONOTONIC},
	    {11, CLOCK_MONOTONIC}, {12, CLOCK_MONOTONIC}, {13, CLOCK_MONOTONIC}};
	for (const auto& test: cases)
	{
		struct Output { uint64_t before; Kernel::KernelTimespec value; uint64_t after; };
		Output output {0x123456789abcdef0u, {-1, -1}, 0xfedcba9876543210u};
		timespec before {}, after {};
		const auto admissions = Kernel::PthreadArgumentLeaseAcquisitionsForTest();
		if (!Check(clock_gettime(test.native, &before) == 0 &&
		           Kernel::KernelClockGettimeHost(test.guest, output.value) == 0 &&
		           clock_gettime(test.native, &after) == 0 &&
		           ordered(before.tv_sec, before.tv_nsec, output.value.tv_sec, output.value.tv_nsec) &&
		           ordered(output.value.tv_sec, output.value.tv_nsec, after.tv_sec, after.tv_nsec) &&
		           output.value.tv_nsec >= 0 && output.value.tv_nsec < 1000000000 &&
		           output.before == 0x123456789abcdef0u && output.after == 0xfedcba9876543210u &&
		           Kernel::PthreadArgumentLeaseAcquisitionsForTest() == admissions,
		           "host clock used the wrong domain, changed adjacent storage, or acquired guest authority")) return false;
		if (!Check(Kernel::KernelClockGettime(test.guest, &output.value) == 0 &&
		           Kernel::PthreadArgumentLeaseAcquisitionsForTest() == admissions + 1,
		           "guest clock omitted output admission")) return false;
	}
	Lumen::Loader::Timer::Start();
	const auto admissions = Kernel::PthreadArgumentLeaseAcquisitionsForTest();
	const uint64_t before = Kernel::KernelGetProcessTime();
	Kernel::KernelTimespec process {};
	const int result = Kernel::KernelClockGettimeHost(15, process);
	const uint64_t after = Kernel::KernelGetProcessTime();
	const uint64_t sampled = static_cast<uint64_t>(process.tv_sec) * 1000000u +
	                         static_cast<uint64_t>(process.tv_nsec) / 1000u;
	if (!Check(result == 0 && process.tv_sec >= 0 && process.tv_nsec >= 0 && process.tv_nsec < 1000000000 &&
	           process.tv_nsec % 1000 == 0 && sampled >= before && sampled <= after &&
	           Kernel::PthreadArgumentLeaseAcquisitionsForTest() == admissions &&
	           Kernel::KernelClockGettime(15, &process) == 0 &&
	           Kernel::PthreadArgumentLeaseAcquisitionsForTest() == admissions + 1,
	           "process clock conversion or guest/host authority changed")) return false;
	if (!Check(Kernel::KernelClockGettime(0, nullptr) == Kernel::KERNEL_ERROR_EFAULT &&
	           Kernel::KernelClockGettime(-1, nullptr) == Kernel::KERNEL_ERROR_EFAULT &&
	           Kernel::PthreadArgumentLeaseAcquisitionsForTest() == admissions + 1,
	           "null output no longer precedes clock-id validation")) return false;
	std::puts("[HOST_CLOCK_TEST] realtime/monotonic-aliases/process=pass guest-output-admission=retained host-admission=0");
	return true;
}

bool Call(Handle* slot, int shared, unsigned value, int expected_result, int expected_errno, const char* operation)
{
	*Posix::GetErrorAddr() = k_errno_sentinel;
	const int result = Posix::sem_init(slot, shared, value);
	const int error = *Posix::GetErrorAddr();
	if (result != expected_result || error != expected_errno)
	{
		std::fprintf(stderr, "PosixSemaphoreTest: FAIL: %s result=%d errno=%d, expected %d/%d\n",
		             operation, result, error, expected_result, expected_errno);
		return false;
	}
	return true;
}

bool WaitCall(Handle* slot, int expected_result, int expected_errno, const char* operation)
{
	*Posix::GetErrorAddr() = k_errno_sentinel;
	const int result = Posix::sem_wait(slot);
	const int error = *Posix::GetErrorAddr();
	if (result != expected_result || error != expected_errno)
	{
		std::fprintf(stderr, "PosixSemaphoreTest: FAIL: %s result=%d errno=%d, expected %d/%d\n",
		             operation, result, error, expected_result, expected_errno);
		return false;
	}
	return true;
}

bool PostCall(Handle* slot, int expected_result, int expected_errno, const char* operation)
{
	*Posix::GetErrorAddr() = k_errno_sentinel;
	const int result = g_post(slot);
	const int error = *Posix::GetErrorAddr();
	if (result != expected_result || error != expected_errno)
	{
		std::fprintf(stderr, "PosixSemaphoreTest: FAIL: %s result=%d errno=%d, expected %d/%d\n",
		             operation, result, error, expected_result, expected_errno);
		return false;
	}
	return true;
}

bool TimedCall(Handle* slot, const Kernel::KernelTimespec* deadline, int expected_result, int expected_errno,
               const char* operation)
{
	*Posix::GetErrorAddr() = k_errno_sentinel;
	const auto admissions = Kernel::PthreadArgumentLeaseAcquisitionsForTest();
	const int result = Posix::sem_timedwait(slot, deadline);
	const int error = *Posix::GetErrorAddr();
	if (!Check(Kernel::PthreadArgumentLeaseAcquisitionsForTest() == admissions,
	           "sem_timedwait treated its private clock sample as guest storage")) return false;
	if (result != expected_result || error != expected_errno)
	{
		std::fprintf(stderr, "PosixSemaphoreTest: FAIL: %s result=%d errno=%d, expected %d/%d\n",
		             operation, result, error, expected_result, expected_errno);
		return false;
	}
	return true;
}

struct GuardedSlot
{
	alignas(8) std::array<uint8_t, 32> bytes;
	size_t offset = 8;
	explicit GuardedSlot(size_t at = 8): offset(at) { bytes.fill(0xa5); }
	Handle* Address() { return reinterpret_cast<Handle*>(bytes.data() + offset); }
	Handle Read() const
	{
		Handle handle = nullptr;
		std::memcpy(&handle, bytes.data() + offset, sizeof(handle));
		return handle;
	}
	bool Canaries() const
	{
		for (size_t i = 0; i < bytes.size(); ++i)
		{
			if ((i < offset || i >= offset + sizeof(Handle)) && bytes[i] != 0xa5) return false;
		}
		return true;
	}
};

struct InlineRecord
{
	uint32_t magic;
	uint32_t has_waiters;
	uint32_t initial_count;
	uint32_t flags;
	bool operator==(const InlineRecord& other) const
	{
		return magic == other.magic && has_waiters == other.has_waiters &&
		       initial_count == other.initial_count && flags == other.flags;
	}
};

static_assert(sizeof(InlineRecord) == 16 && alignof(InlineRecord) == 4);
constexpr uint32_t k_inline_magic = 0xffff736dU;

struct GuardedInlineSlot
{
	alignas(8) std::array<uint8_t, 48> bytes;
	size_t offset = 8;
	explicit GuardedInlineSlot(size_t at = 8): offset(at) { bytes.fill(0xa5); }
	Handle* Address() { return reinterpret_cast<Handle*>(bytes.data() + offset); }
	InlineRecord Read() const
	{
		InlineRecord record {};
		std::memcpy(&record, bytes.data() + offset, sizeof(record));
		return record;
	}
	void Write(const InlineRecord& record)
	{
		std::memcpy(bytes.data() + offset, &record, sizeof(record));
	}
	bool Canaries() const
	{
		for (size_t i = 0; i < bytes.size(); ++i)
		{
			if ((i < offset || i >= offset + sizeof(InlineRecord)) && bytes[i] != 0xa5) return false;
		}
		return true;
	}
};

template <typename Predicate>
bool WaitUntil(Predicate&& predicate, std::chrono::milliseconds timeout = std::chrono::milliseconds(1000))
{
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (!predicate() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
	return predicate();
}

bool TestErrors()
{
	GuardedSlot slot;
	const auto before = slot.bytes;
	if (!Call(slot.Address(), 0, k_maximum + 1U, -1, Posix::POSIX_EINVAL, "value above local maximum") ||
	    !Call(slot.Address(), 1, UINT32_MAX, -1, Posix::POSIX_EINVAL, "value before pshared") ||
	    !Call(slot.Address(), 1, 0, -1, Posix::POSIX_ENOSYS, "process sharing unsupported") ||
	    !Call(slot.Address(), -1, 0, -1, Posix::POSIX_ENOSYS, "negative nonzero pshared unsupported") ||
	    !Call(nullptr, 1, 0, -1, Posix::POSIX_ENOSYS, "pshared before null") ||
	    !Call(nullptr, 0, 0, -1, Posix::POSIX_EINVAL, "null slot") ||
	    !Call(nullptr, 1, UINT32_MAX, -1, Posix::POSIX_EINVAL, "value before all pointer checks") ||
	    !Check(slot.bytes == before, "validation errors changed output bytes")) return false;
	const auto overflow = reinterpret_cast<Handle*>(std::numeric_limits<uintptr_t>::max() - 3);
	return Call(overflow, 0, 0, -1, Posix::POSIX_EFAULT, "overflowing eight-byte slot") &&
	       Call(overflow, 1, 0, -1, Posix::POSIX_ENOSYS, "pshared before overflow") &&
	       Call(overflow, 0, UINT32_MAX, -1, Posix::POSIX_EINVAL, "value before overflow");
}

bool TestCountAndMaximum()
{
	for (unsigned initial: {0U, 3U, k_maximum})
	{
		GuardedSlot slot;
		if (!Call(slot.Address(), 0, initial, 0, k_errno_sentinel, "valid initialization") ||
		    !Check(slot.Read() != nullptr && slot.Canaries(), "initialization did not write only its pointer slot")) return false;
		const Handle handle = slot.Read();
		if (initial == k_maximum &&
		    !Check(Sema::KernelSignalSema(handle, 1) == Kernel::KERNEL_ERROR_EINVAL,
		           "full INT32_MAX semaphore accepted an overflowing signal")) return false;
		if (initial != 0 &&
		    !Check(Sema::KernelPollSema(handle, static_cast<int>(initial)) == 0,
		           "initial count differs or rejected signal changed it")) return false;
		if (!Check(Sema::KernelPollSema(handle, 1) == Kernel::KERNEL_ERROR_EBUSY, "initial count was not exhausted")) return false;
		Kernel::KernelUseconds timeout = 1000;
		if (!Check(Sema::KernelWaitSema(handle, 1, &timeout) == Kernel::KERNEL_ERROR_ETIMEDOUT && timeout == 0,
		           "empty semaphore wait did not time out") ||
		    !Check(Sema::KernelSignalSema(handle, 2) == 0, "signal after consumption failed")) return false;
		timeout = 10000;
		if (!Check(Sema::KernelWaitSema(handle, 2, &timeout) == 0 &&
		           Sema::KernelPollSema(handle, 1) == Kernel::KERNEL_ERROR_EBUSY,
		           "wait did not consume the posted count") ||
		    !Check(Sema::KernelDeleteSema(handle) == 0, "destroy initialized semaphore failed")) return false;
	}
	// Negative signals are an existing kernel-backend behavior, not a POSIX
	// initialization contract. Comparing availability must not subtract from INT_MIN.
	Handle negative = nullptr;
	if (!Call(&negative, 0, 0, 0, k_errno_sentinel, "initialize negative-count regression")) return false;
	const bool recovered =
	    Check(Sema::KernelSignalSema(negative, std::numeric_limits<int>::min()) == 0 &&
	          Sema::KernelPollSema(negative, 1) == Kernel::KERNEL_ERROR_EBUSY,
	          "INT_MIN count did not remain unavailable") &&
	    Check(Sema::KernelSignalSema(negative, std::numeric_limits<int>::max()) == 0 &&
	          Sema::KernelSignalSema(negative, 1) == 0 &&
	          Sema::KernelPollSema(negative, 1) == Kernel::KERNEL_ERROR_EBUSY,
	          "recovery from INT_MIN did not reach an empty semaphore") &&
	    Check(Sema::KernelSignalSema(negative, 1) == 0 && Sema::KernelPollSema(negative, 1) == 0,
	          "recovered semaphore did not consume a later signal");
	const bool deleted = Check(Sema::KernelDeleteSema(negative) == 0, "negative-count regression cleanup failed");
	return recovered && deleted;
}

class Pages final
{
public:
	~Pages() { if (m_data != nullptr) Memory::Free(reinterpret_cast<uint64_t>(m_data)); }
	bool Init()
	{
		const long page_size = sysconf(_SC_PAGESIZE);
		if (!Check(page_size > 0, "page size unavailable")) return false;
		m_page = static_cast<size_t>(page_size);
		const uint64_t mapped = Memory::Alloc(0, Size(), Memory::Mode::ReadWrite);
		if (!Check(mapped != 0, "test mapping failed")) return false;
		m_data = reinterpret_cast<uint8_t*>(mapped);
		std::memset(m_data, 0x6d, Size());
		return true;
	}
	bool ProtectMiddle(Memory::Mode protection)
	{
		return Check(Memory::Protect(reinterpret_cast<uint64_t>(m_data + m_page), m_page, protection),
		             "managed test mapping protection failed");
	}
	size_t Page() const { return m_page; }
	size_t Size() const { return m_page * 3; }
	uint8_t* Data() { return m_data; }
	std::vector<uint8_t> Snapshot() const { return {m_data, m_data + Size()}; }

private:
	uint8_t* m_data = nullptr;
	size_t m_page = 0;
};

bool TestMemoryAccess()
{
	GuardedSlot unaligned(9);
	if (!Call(unaligned.Address(), 0, 1, 0, k_errno_sentinel, "unaligned pointer slot") ||
	    !Check(unaligned.Canaries() && unaligned.Read() != nullptr, "unaligned output changed adjacent bytes") ||
	    !Check(Sema::KernelPollSema(unaligned.Read(), 1) == 0 && Sema::KernelDeleteSema(unaligned.Read()) == 0,
	           "unaligned output does not identify a functioning semaphore")) return false;
	Pages pages;
	if (!pages.Init()) return false;
	for (Memory::Mode protection: {Memory::Mode::Read, Memory::Mode::NoAccess})
	{
		const auto before = pages.Snapshot();
		if (!pages.ProtectMiddle(protection)) return false;
		auto* slot = reinterpret_cast<Handle*>(pages.Data() + pages.Page() + 32);
		const bool errors = Call(slot, 0, 0, -1, Posix::POSIX_EFAULT, "unwritable slot") &&
		                    Call(slot, 1, 0, -1, Posix::POSIX_ENOSYS, "pshared before protected memory") &&
		                    Call(slot, 0, UINT32_MAX, -1, Posix::POSIX_EINVAL, "value before protected memory");
		if (!pages.ProtectMiddle(Memory::Mode::ReadWrite) || !errors ||
		    !Check(pages.Snapshot() == before, "protected-memory failures wrote any bytes")) return false;
	}
	const auto before = pages.Snapshot();
	if (!pages.ProtectMiddle(Memory::Mode::NoAccess)) return false;
	auto* crossing = reinterpret_cast<Handle*>(pages.Data() + pages.Page() - 4);
	const bool crossing_error = Call(crossing, 0, 0, -1, Posix::POSIX_EFAULT, "partially accessible slot");
	if (!pages.ProtectMiddle(Memory::Mode::ReadWrite) || !crossing_error ||
	    !Check(pages.Snapshot() == before, "partial write rejection changed the readable prefix")) return false;
	const size_t tail = pages.Page() - sizeof(Handle);
	if (!pages.ProtectMiddle(Memory::Mode::NoAccess)) return false;
	const bool initialized = Call(reinterpret_cast<Handle*>(pages.Data() + tail), 0, 1, 0, k_errno_sentinel,
	                              "exact eight-byte tail before inaccessible page");
	if (!pages.ProtectMiddle(Memory::Mode::ReadWrite) || !initialized) return false;
	Handle handle = nullptr;
	std::memcpy(&handle, pages.Data() + tail, sizeof(handle));
	for (size_t index = 0; index < pages.Size(); ++index)
	{
		if ((index < tail || index >= tail + sizeof(Handle)) &&
		    !Check(pages.Data()[index] == before[index], "tail initialization accessed adjacent output bytes")) return false;
	}
	return Check(handle != nullptr && Sema::KernelPollSema(handle, 1) == 0 && Sema::KernelDeleteSema(handle) == 0,
	             "eight-byte tail did not produce a live semaphore");
}

bool TestMemoryAndReset()
{
	if (!TestMemoryAccess()) return false;
	GuardedSlot slot;
	for (unsigned count: {2U, 5U})
	{
		if (!Call(slot.Address(), 0, count, 0, k_errno_sentinel, "reuse after destruction") ||
		    !Check(slot.Read() != nullptr && Sema::KernelPollSema(slot.Read(), static_cast<int>(count)) == 0 &&
		           Sema::KernelDeleteSema(slot.Read()) == 0, "reused storage retained an earlier semaphore count")) return false;
	}
	std::array<Handle, 3> live {};
	for (auto& handle: live)
	{
		if (!Call(&handle, 0, 1, 0, k_errno_sentinel, "session-owned initialization") ||
		    !Check(handle != nullptr, "session-owned object missing")) return false;
	}
	if (!Check(Sema::ResetForNextSession() && Sema::ResetForNextSession(), "session reset failed or was not repeatable")) return false;
	// Old handle values are not dereferenced after reset. Reuse the guest slot
	// only after its previous object has been destroyed by the session reset.
	if (!Call(&live[0], 0, 3, 0, k_errno_sentinel, "initialization after reset")) return false;
	return Check(live[0] != nullptr && Sema::KernelPollSema(live[0], 3) == 0 && Sema::KernelDeleteSema(live[0]) == 0,
	             "initialization after reset retained stale state");
}

bool TestWaitMemory()
{
	Handle empty = nullptr;
	Handle invented = reinterpret_cast<Handle>(uintptr_t {1});
	const auto overflow = reinterpret_cast<Handle*>(std::numeric_limits<uintptr_t>::max() - 3);
	if (!WaitCall(nullptr, -1, Posix::POSIX_EINVAL, "wait null slot") ||
	    !WaitCall(&empty, -1, Posix::POSIX_EINVAL, "wait null handle") ||
	    !WaitCall(&invented, -1, Posix::POSIX_EINVAL, "wait unregistered handle") ||
	    !WaitCall(overflow, -1, Posix::POSIX_EFAULT, "wait overflowing slot") ||
	    !Check(empty == nullptr && invented == reinterpret_cast<Handle>(uintptr_t {1}),
	           "rejected wait changed a handle slot")) return false;
	Pages pages;
	if (!pages.Init()) return false;
	Handle live = nullptr;
	if (!Call(&live, 0, 3, 0, k_errno_sentinel, "initialize wait memory cases")) return false;
	const size_t readable = pages.Page() + 32;
	const size_t unaligned = pages.Page() + 65;
	const size_t tail = pages.Page() - sizeof(Handle);
	for (size_t offset: {readable, unaligned, tail}) std::memcpy(pages.Data() + offset, &live, sizeof(live));
	const auto before = pages.Snapshot();
	if (!pages.ProtectMiddle(Memory::Mode::Read)) return false;
	const bool readable_waits =
	    WaitCall(reinterpret_cast<Handle*>(pages.Data() + readable), 0, k_errno_sentinel, "wait read-only slot") &&
	    WaitCall(reinterpret_cast<Handle*>(pages.Data() + unaligned), 0, k_errno_sentinel, "wait unaligned read-only slot");
	if (!pages.ProtectMiddle(Memory::Mode::NoAccess)) return false;
	const bool guarded_waits =
	    WaitCall(reinterpret_cast<Handle*>(pages.Data() + readable), -1, Posix::POSIX_EFAULT, "wait inaccessible slot") &&
	    WaitCall(reinterpret_cast<Handle*>(pages.Data() + pages.Page() - 4), -1, Posix::POSIX_EFAULT,
	             "wait partly readable slot") &&
	    WaitCall(reinterpret_cast<Handle*>(pages.Data() + tail), 0, k_errno_sentinel, "wait exact eight-byte readable tail");
	if (!pages.ProtectMiddle(Memory::Mode::ReadWrite)) return false;
	const bool preserved = Check(pages.Snapshot() == before &&
	                             Sema::KernelPollSema(live, 1) == Kernel::KERNEL_ERROR_EBUSY,
	                             "wait wrote guest memory or did not consume exactly three tokens");
	const Handle stale = live;
	const bool deleted = Check(Sema::KernelDeleteSema(live) == 0, "wait memory cleanup failed");
	const bool rejected_stale = deleted && WaitCall(&live, -1, Posix::POSIX_EINVAL, "wait handle after deletion") &&
	                            Check(live == stale, "stale wait changed the slot");
	return readable_waits && guarded_waits && preserved && rejected_stale;
}

bool TestPostCountAndErrors()
{
	Handle empty = nullptr;
	Handle invented = reinterpret_cast<Handle>(uintptr_t {1});
	const auto overflow = reinterpret_cast<Handle*>(std::numeric_limits<uintptr_t>::max() - 3);
	if (!PostCall(nullptr, -1, Posix::POSIX_EINVAL, "post null slot") ||
	    !PostCall(&empty, -1, Posix::POSIX_EINVAL, "post null handle") ||
	    !PostCall(&invented, -1, Posix::POSIX_EINVAL, "post unregistered handle") ||
	    !PostCall(overflow, -1, Posix::POSIX_EFAULT, "post overflowing slot") ||
	    !Check(empty == nullptr && invented == reinterpret_cast<Handle>(uintptr_t {1}),
	           "rejected post changed a handle slot")) return false;

	GuardedSlot count;
	if (!Call(count.Address(), 0, 0, 0, k_errno_sentinel, "initialize post count")) return false;
	const auto count_before = count.bytes;
	if (!PostCall(count.Address(), 0, k_errno_sentinel, "first post") ||
	    !PostCall(count.Address(), 0, k_errno_sentinel, "second post") ||
	    !WaitCall(count.Address(), 0, k_errno_sentinel, "wait first posted token") ||
	    !TimedCall(count.Address(), nullptr, 0, k_errno_sentinel, "timed wait second posted token") ||
	    !Check(count.bytes == count_before && Sema::KernelPollSema(count.Read(), 1) == Kernel::KERNEL_ERROR_EBUSY,
	           "post changed its slot or released the wrong count")) return false;
	const Handle stale = count.Read();
	if (!Check(Sema::KernelDeleteSema(stale) == 0, "post count cleanup failed") ||
	    !PostCall(count.Address(), -1, Posix::POSIX_EINVAL, "post stale handle") ||
	    !Check(count.Read() == stale, "stale post changed the handle slot")) return false;

	GuardedSlot full;
	if (!Call(full.Address(), 0, k_maximum, 0, k_errno_sentinel, "initialize full post semaphore")) return false;
	const auto full_before = full.bytes;
	const bool overflowed =
	    PostCall(full.Address(), -1, Posix::POSIX_EOVERFLOW, "post at maximum") &&
	    PostCall(full.Address(), -1, Posix::POSIX_EOVERFLOW, "repeated post at maximum") &&
	    Check(full.bytes == full_before && Sema::KernelPollSema(full.Read(), std::numeric_limits<int>::max()) == 0,
	          "overflowing post changed slot or semaphore count") &&
	    PostCall(full.Address(), 0, k_errno_sentinel, "post after maximum was consumed") &&
	    WaitCall(full.Address(), 0, k_errno_sentinel, "consume post after maximum") &&
	    Check(Sema::KernelPollSema(full.Read(), 1) == Kernel::KERNEL_ERROR_EBUSY,
	          "overflow recovery left an extra token");
	const bool deleted = Check(Sema::KernelDeleteSema(full.Read()) == 0, "full post cleanup failed");
	return overflowed && deleted;
}

bool TestConcurrentPostOverflow()
{
	for (unsigned round = 0; round < 32; ++round)
	{
		GuardedSlot slot;
		if (!Call(slot.Address(), 0, k_maximum - 1, 0, k_errno_sentinel, "initialize concurrent post overflow")) return false;
		const auto before = slot.bytes;
		std::atomic<unsigned> ready {0};
		std::atomic<bool> start {false};
		std::array<int, 2> results {};
		std::array<int, 2> errors {};
		std::array<std::thread, 2> posters;
		for (size_t index = 0; index < posters.size(); ++index)
		{
			posters[index] = std::thread([&, index] {
				*Posix::GetErrorAddr() = k_errno_sentinel;
				ready.fetch_add(1, std::memory_order_release);
				while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
				results[index] = g_post(slot.Address());
				errors[index] = *Posix::GetErrorAddr();
			});
		}
		while (ready.load(std::memory_order_acquire) != posters.size()) std::this_thread::yield();
		start.store(true, std::memory_order_release);
		for (auto& poster: posters) poster.join();
		const unsigned successes = static_cast<unsigned>(results[0] == 0 && errors[0] == k_errno_sentinel) +
		                           static_cast<unsigned>(results[1] == 0 && errors[1] == k_errno_sentinel);
		const unsigned overflows = static_cast<unsigned>(results[0] == -1 && errors[0] == Posix::POSIX_EOVERFLOW) +
		                          static_cast<unsigned>(results[1] == -1 && errors[1] == Posix::POSIX_EOVERFLOW);
		const bool atomic = Check(successes == 1 && overflows == 1 && slot.bytes == before &&
		                          Sema::KernelPollSema(slot.Read(), std::numeric_limits<int>::max()) == 0 &&
		                          Sema::KernelPollSema(slot.Read(), 1) == Kernel::KERNEL_ERROR_EBUSY,
		                          "concurrent posts did not atomically stop at maximum");
		const bool deleted = Check(Sema::KernelDeleteSema(slot.Read()) == 0, "concurrent post cleanup failed");
		if (!atomic || !deleted) return false;
	}
	return true;
}

bool TestPostUnblocksWaiters()
{
	GuardedSlot slot;
	if (!Call(slot.Address(), 0, 0, 0, k_errno_sentinel, "initialize post waiters")) return false;
	const Handle handle = slot.Read();
	const auto before = slot.bytes;
	Kernel::KernelTimespec future {};
	if (!Check(Kernel::KernelClockGettime(0, &future) == 0 && future.tv_sec <= std::numeric_limits<int64_t>::max() - 60,
	           "post waiter deadline unavailable")) return false;
	future.tv_sec += 60;
	std::array<bool, 2> wait_ok {};
	std::atomic<unsigned> completed {0};
	std::array<std::thread, 2> waiters {
	    std::thread([&] {
		    wait_ok[0] = WaitCall(slot.Address(), 0, k_errno_sentinel, "sem_wait released by post");
		    completed.fetch_add(1, std::memory_order_release);
	    }),
	    std::thread([&] {
		    wait_ok[1] = TimedCall(slot.Address(), &future, 0, k_errno_sentinel, "sem_timedwait released by post");
		    completed.fetch_add(1, std::memory_order_release);
	    })
	};
	auto wait_until = [](auto&& predicate) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (!predicate() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
		return predicate();
	};
	const bool registered = wait_until([&] { return Sema::SemaphoreWaiterCountForTest(handle) == 2; });
	const bool first_post = registered && PostCall(slot.Address(), 0, k_errno_sentinel, "post first registered waiter");
	const bool one_completed = first_post && wait_until([&] { return completed.load(std::memory_order_acquire) >= 1; }) &&
	                           Check(completed.load(std::memory_order_acquire) == 1,
	                                 "one post completed more than one waiter");
	const bool one_remains = one_completed && wait_until([&] { return Sema::SemaphoreWaiterCountForTest(handle) == 1; });
	const bool second_post = one_remains && PostCall(slot.Address(), 0, k_errno_sentinel, "post second registered waiter");
	const bool both_completed = second_post && wait_until([&] { return completed.load(std::memory_order_acquire) == 2; });
	if (!both_completed) Sema::KernelSignalSema(handle, 2);
	for (auto& waiter: waiters) waiter.join();
	const bool consumed = Check(registered && first_post && one_completed && one_remains && second_post && both_completed &&
	                            wait_ok[0] && wait_ok[1] && slot.bytes == before &&
	                            Sema::KernelPollSema(handle, 1) == Kernel::KERNEL_ERROR_EBUSY,
	                            "post did not release and consume exactly one token per waiter");
	const bool deleted = Check(Sema::KernelDeleteSema(handle) == 0, "post waiter cleanup failed");
	return consumed && deleted;
}

bool TestPostMemory()
{
	Pages pages;
	if (!pages.Init()) return false;
	Handle live = nullptr;
	if (!Call(&live, 0, 0, 0, k_errno_sentinel, "initialize post memory cases")) return false;
	const size_t readable = pages.Page() + 32;
	const size_t unaligned = pages.Page() + 65;
	const size_t tail = pages.Page() - sizeof(Handle);
	for (size_t offset: {readable, unaligned, tail}) std::memcpy(pages.Data() + offset, &live, sizeof(live));
	const auto before = pages.Snapshot();
	if (!pages.ProtectMiddle(Memory::Mode::Read)) return false;
	const bool readable_posts =
	    PostCall(reinterpret_cast<Handle*>(pages.Data() + readable), 0, k_errno_sentinel, "post read-only slot") &&
	    PostCall(reinterpret_cast<Handle*>(pages.Data() + unaligned), 0, k_errno_sentinel, "post unaligned read-only slot");
	if (!pages.ProtectMiddle(Memory::Mode::NoAccess)) return false;
	const bool guarded_posts =
	    PostCall(reinterpret_cast<Handle*>(pages.Data() + readable), -1, Posix::POSIX_EFAULT, "post inaccessible slot") &&
	    PostCall(reinterpret_cast<Handle*>(pages.Data() + pages.Page() - 4), -1, Posix::POSIX_EFAULT,
	             "post partly readable slot") &&
	    PostCall(reinterpret_cast<Handle*>(pages.Data() + tail), 0, k_errno_sentinel, "post exact eight-byte readable tail");
	if (!pages.ProtectMiddle(Memory::Mode::ReadWrite)) return false;
	const bool preserved = Check(pages.Snapshot() == before && Sema::KernelPollSema(live, 3) == 0 &&
	                             Sema::KernelPollSema(live, 1) == Kernel::KERNEL_ERROR_EBUSY,
	                             "post accessed adjacent memory or released the wrong count");
	const bool deleted = Check(Sema::KernelDeleteSema(live) == 0, "post memory cleanup failed");
	return readable_posts && guarded_posts && preserved && deleted;
}

bool TestBlockingWait(const Kernel::KernelTimespec* deadline = nullptr)
{
	GuardedSlot slot;
	if (!Call(slot.Address(), 0, 0, 0, k_errno_sentinel, "initialize blocking wait")) return false;
	const auto before = slot.bytes;
	const Kernel::KernelTimespec deadline_before = deadline == nullptr ? Kernel::KernelTimespec {} : *deadline;
	const Handle handle = slot.Read();
	std::atomic<bool> done {false};
	int result = -2;
	int error = -2;
	std::thread worker([&] {
		*Posix::GetErrorAddr() = k_errno_sentinel;
		result = deadline == nullptr ? Posix::sem_wait(slot.Address()) : Posix::sem_timedwait(slot.Address(), deadline);
		error = *Posix::GetErrorAddr();
		done.store(true, std::memory_order_release);
	});
	while (!done.load(std::memory_order_acquire) && Sema::SemaphoreWaiterCountForTest(handle) == 0)
		std::this_thread::yield();
	const bool blocked = Check(!done.load(std::memory_order_acquire) &&
	                           Sema::SemaphoreWaiterCountForTest(handle) == 1,
	                           "empty wait returned before a waiter was registered");
	const bool signaled = !done.load(std::memory_order_acquire) &&
	                      Check(Sema::KernelSignalSema(handle, 1) == 0, "release blocked wait failed");
	worker.join();
	const bool completed = Check(result == 0 && error == k_errno_sentinel && slot.bytes == before &&
	                             (deadline == nullptr || (deadline->tv_sec == deadline_before.tv_sec &&
	                                                     deadline->tv_nsec == deadline_before.tv_nsec)) &&
	                             Sema::KernelPollSema(handle, 1) == Kernel::KERNEL_ERROR_EBUSY,
	                             "signaled wait failed, changed inputs/errno, or left its token unconsumed");
	const bool deleted = Check(Sema::KernelDeleteSema(handle) == 0, "blocking wait cleanup failed");
	return blocked && signaled && completed && deleted;
}

bool TestDeleteWithWaiter()
{
	Handle handle = nullptr;
	if (!Check(Sema::KernelCreateSema(&handle, "delete-with-waiter", 0x01, 0, 1, nullptr) == 0 &&
	           handle != nullptr, "delete-wait semaphore creation failed"))
	{
		return false;
	}
	std::atomic_bool finished {false};
	int wait_result = 0;
	std::thread waiter([&] {
		wait_result = Sema::KernelWaitSema(handle, 1, nullptr);
		finished.store(true, std::memory_order_release);
	});
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!finished.load(std::memory_order_acquire) && Sema::SemaphoreWaiterCountForTest(handle) == 0 &&
	       std::chrono::steady_clock::now() < deadline)
	{
		std::this_thread::yield();
	}
	if (finished.load(std::memory_order_acquire) || Sema::SemaphoreWaiterCountForTest(handle) != 1)
	{
		(void)Sema::KernelSignalSema(handle, 1);
		waiter.join();
		(void)Sema::KernelDeleteSema(handle);
		return Check(false, "delete-wait semaphore waiter was not registered");
	}
	const int delete_result = Sema::KernelDeleteSema(handle);
	waiter.join();
	return Check(delete_result == 0 && wait_result == Kernel::KERNEL_ERROR_EACCES,
	             "semaphore deletion did not retain and release its waiter");
}

bool TestTimedWait()
{
	const bool carry_enabled = Posix::SemTimedWaitCarryEnvironmentValueEnabledForTest(
	    std::getenv("LUMEN_EXPERIMENTAL_POSIX_SEM_TIMEDWAIT_CARRY"));
	const Kernel::KernelTimespec past {std::numeric_limits<int64_t>::min(), 0};
	const Kernel::KernelTimespec negative_nsec {0, -1};
	const Kernel::KernelTimespec excessive_nsec {0, 1000000000};
	const Kernel::KernelTimespec above_carry {0, 1000000001};
	const Kernel::KernelTimespec carry_overflow {std::numeric_limits<int64_t>::max(), 1000000000};
	const auto inaccessible = reinterpret_cast<const Kernel::KernelTimespec*>(uintptr_t {1});
	const std::array<const Kernel::KernelTimespec*, 7> ignored {
	    nullptr, &past, &negative_nsec, &excessive_nsec, &above_carry, &carry_overflow, inaccessible};
	GuardedSlot slot;
	if (!Call(slot.Address(), 0, static_cast<unsigned>(ignored.size()), 0, k_errno_sentinel,
	          "initialize immediate timed acquisitions")) return false;
	const auto before = slot.bytes;
	for (const auto* deadline: ignored)
		if (!TimedCall(slot.Address(), deadline, 0, k_errno_sentinel, "immediate acquisition precedes deadline checks")) return false;
	const Handle handle = slot.Read();
	const auto overflowing_slot = reinterpret_cast<Handle*>(std::numeric_limits<uintptr_t>::max() - 3);
	const auto overflowing_time = reinterpret_cast<const Kernel::KernelTimespec*>(std::numeric_limits<uintptr_t>::max() - 7);
	if (!Check(Sema::KernelPollSema(handle, 1) == Kernel::KERNEL_ERROR_EBUSY, "immediate timed wait did not consume one token") ||
	    !TimedCall(nullptr, nullptr, -1, Posix::POSIX_EINVAL, "timed wait null slot") ||
	    !TimedCall(overflowing_slot, nullptr, -1, Posix::POSIX_EFAULT, "timed wait invalid slot before deadline") ||
	    !TimedCall(slot.Address(), nullptr, -1, Posix::POSIX_EINVAL, "blocking timed wait null deadline") ||
	    !TimedCall(slot.Address(), &negative_nsec, -1, Posix::POSIX_EINVAL, "negative nanoseconds") ||
	    !TimedCall(slot.Address(), &excessive_nsec, -1,
	               carry_enabled ? Posix::POSIX_ETIMEDOUT : Posix::POSIX_EINVAL,
	               "nanoseconds at one billion") ||
	    !TimedCall(slot.Address(), &above_carry, -1, Posix::POSIX_EINVAL, "nanoseconds above carry boundary") ||
	    !TimedCall(slot.Address(), &carry_overflow, -1, Posix::POSIX_EINVAL, "carry seconds overflow") ||
	    !TimedCall(slot.Address(), overflowing_time, -1, Posix::POSIX_EFAULT, "overflowing deadline record") ||
	    !TimedCall(slot.Address(), &past, -1, Posix::POSIX_ETIMEDOUT, "expired extreme negative deadline")) return false;
	Kernel::KernelTimespec soon {};
	if (!Check(Kernel::KernelClockGettime(0, &soon) == 0 && soon.tv_sec < std::numeric_limits<int64_t>::max(),
	           "realtime clock unavailable")) return false;
	soon.tv_nsec += 20000000;
	if (soon.tv_nsec >= 1000000000) { soon.tv_nsec -= 1000000000; ++soon.tv_sec; }
	const auto soon_before = soon;
	if (!TimedCall(slot.Address(), &soon, -1, Posix::POSIX_ETIMEDOUT, "finite realtime timeout")) return false;
	Kernel::KernelTimespec after {};
	const bool timed_out = Check(Kernel::KernelClockGettime(0, &after) == 0 &&
	                             (after.tv_sec > soon.tv_sec || (after.tv_sec == soon.tv_sec && after.tv_nsec >= soon.tv_nsec)) &&
	                             soon.tv_sec == soon_before.tv_sec && soon.tv_nsec == soon_before.tv_nsec && slot.bytes == before &&
	                             Sema::KernelSignalSema(handle, 1) == 0,
	                             "timed wait expired early, changed inputs, or failed recovery") &&
	                       TimedCall(slot.Address(), nullptr, 0, k_errno_sentinel, "token remains usable after timeout") &&
	                       Check(Sema::KernelPollSema(handle, 1) == Kernel::KERNEL_ERROR_EBUSY,
	                             "timeout changed semaphore consumption");
	const bool deleted = Check(Sema::KernelDeleteSema(handle) == 0, "timed wait cleanup failed");
	if (!timed_out || !deleted) return false;
	Kernel::KernelTimespec future {};
	if (!Check(Kernel::KernelClockGettime(0, &future) == 0 && future.tv_sec <= std::numeric_limits<int64_t>::max() - 60,
	           "future realtime deadline unavailable")) return false;
	future.tv_sec += 60;
	const Kernel::KernelTimespec far_future {std::numeric_limits<int64_t>::max(), 999999999};
	if (!TestBlockingWait(&future) || !TestBlockingWait(&far_future)) return false;
	if (!carry_enabled) return true;
	Kernel::KernelTimespec carry_future {};
	if (!Check(Kernel::KernelClockGettime(0, &carry_future) == 0 &&
	           carry_future.tv_sec <= std::numeric_limits<int64_t>::max() - 61,
	           "future carry deadline unavailable")) return false;
	carry_future.tv_sec += 60;
	carry_future.tv_nsec = 1000000000;
	return TestBlockingWait(&carry_future);
}

bool TestTimedWaitCarryContract()
{
	if (!Check(!Posix::SemTimedWaitCarryEnvironmentValueEnabledForTest(nullptr) &&
	           !Posix::SemTimedWaitCarryEnvironmentValueEnabledForTest("") &&
	           !Posix::SemTimedWaitCarryEnvironmentValueEnabledForTest("0") &&
	           Posix::SemTimedWaitCarryEnvironmentValueEnabledForTest("1") &&
	           !Posix::SemTimedWaitCarryEnvironmentValueEnabledForTest("01") &&
	           !Posix::SemTimedWaitCarryEnvironmentValueEnabledForTest("true") &&
	           !Posix::SemTimedWaitCarryEnvironmentValueEnabledForTest("1 "),
	           "carry selector did not require exact value one")) return false;

	auto normalized = [](const Kernel::KernelTimespec& input, bool enabled,
	                     bool expected_valid, bool expected_carried,
	                     const Kernel::KernelTimespec& expected, const char* operation) {
		Kernel::KernelTimespec output {91, 92};
		bool carried = true;
		const auto before = input;
		const bool valid = Posix::SemTimedWaitNormalizeDeadlineForTest(input, enabled, &output, &carried);
		return Check(valid == expected_valid && carried == expected_carried &&
		             input.tv_sec == before.tv_sec && input.tv_nsec == before.tv_nsec &&
		             output.tv_sec == expected.tv_sec && output.tv_nsec == expected.tv_nsec,
		             operation);
	};
	const Kernel::KernelTimespec valid {-7, 999999999};
	const Kernel::KernelTimespec exact {7, 1000000000};
	const Kernel::KernelTimespec negative {7, -1};
	const Kernel::KernelTimespec above {7, 1000000001};
	const Kernel::KernelTimespec overflow {std::numeric_limits<int64_t>::max(), 1000000000};
	return normalized(valid, false, true, false, valid, "valid deadline changed while carry disabled") &&
	       normalized(valid, true, true, false, valid, "valid deadline changed while carry enabled") &&
	       normalized(exact, false, false, false, exact, "disabled carry admitted exact boundary") &&
	       normalized(exact, true, true, true, {8, 0}, "enabled carry did not normalize exact boundary") &&
	       normalized(negative, true, false, false, negative, "enabled carry admitted negative nanoseconds") &&
	       normalized(above, true, false, false, above, "enabled carry admitted nanoseconds above boundary") &&
	       normalized(overflow, true, false, false, overflow, "enabled carry overflowed seconds");
}

bool TestTimedMemory()
{
	Pages pages;
	if (!pages.Init()) return false;
	GuardedSlot slot;
	if (!Call(slot.Address(), 0, 0, 0, k_errno_sentinel, "initialize deadline memory cases")) return false;
	const auto slot_before = slot.bytes;
	const Kernel::KernelTimespec past {0, 0};
	const size_t readable = pages.Page() + 32;
	const size_t unaligned = pages.Page() + 65;
	const size_t tail = pages.Page() - sizeof(past);
	for (size_t offset: {readable, unaligned, tail}) std::memcpy(pages.Data() + offset, &past, sizeof(past));
	const auto before = pages.Snapshot();
	const auto at = [&](size_t offset) { return reinterpret_cast<const Kernel::KernelTimespec*>(pages.Data() + offset); };
	if (!pages.ProtectMiddle(Memory::Mode::Read)) return false;
	const bool readable_deadlines =
	    TimedCall(slot.Address(), at(readable), -1, Posix::POSIX_ETIMEDOUT, "read-only deadline") &&
	    TimedCall(slot.Address(), at(unaligned), -1, Posix::POSIX_ETIMEDOUT, "unaligned read-only deadline");
	if (!pages.ProtectMiddle(Memory::Mode::NoAccess)) return false;
	const bool guarded_deadlines =
	    TimedCall(slot.Address(), at(readable), -1, Posix::POSIX_EFAULT, "inaccessible deadline") &&
	    TimedCall(slot.Address(), at(pages.Page() - 8), -1, Posix::POSIX_EFAULT, "partly readable deadline") &&
	    TimedCall(slot.Address(), at(tail), -1, Posix::POSIX_ETIMEDOUT, "exact sixteen-byte deadline tail");
	if (!pages.ProtectMiddle(Memory::Mode::ReadWrite)) return false;
	const bool preserved = Check(pages.Snapshot() == before && slot.bytes == slot_before &&
	                             Sema::KernelPollSema(slot.Read(), 1) == Kernel::KERNEL_ERROR_EBUSY,
	                             "deadline checks changed input memory or semaphore count");
	const bool deleted = Check(Sema::KernelDeleteSema(slot.Read()) == 0, "deadline memory cleanup failed");
	return readable_deadlines && guarded_deadlines && preserved && deleted;
}

bool TestBinding()
{
	Lumen::Loader::SymbolDatabase symbols;
	if (!Check(Lumen::Libs::Init(U"libkernel_1", &symbols), "libkernel registration failed")) return false;
	Lumen::Loader::SymbolResolve exact {};
	exact.library = "libScePosix";
	exact.library_version = 1;
	exact.module = "libkernel";
	exact.module_version_major = 1;
	exact.module_version_minor = 1;
	exact.type = Lumen::Loader::SymbolType::Func;
	struct Export { const char* nid; uint64_t target; };
	const std::array<Export, 4> expected {{
	    {"pDuPEf3m4fI", reinterpret_cast<uint64_t>(&Posix::sem_init)},
	    {"YCV5dGGBcCo", reinterpret_cast<uint64_t>(&Posix::sem_wait)},
	    {"w5IHyvahg-o", reinterpret_cast<uint64_t>(&Posix::sem_timedwait)},
	    {"IKP8typ0QUk", reinterpret_cast<uint64_t>(&Posix::sem_post)}
	}};
	std::array<uint64_t, 4> resolved {};
	std::array<uint64_t, 4> kernel_aliases {};
	for (size_t index = 0; index < expected.size(); ++index)
	{
		exact.name = expected[index].nid;
		const auto* symbol = symbols.Find(exact);
		if (!Check(symbol != nullptr && symbol->vaddr == expected[index].target,
		           "semaphore export binding differs")) return false;
		resolved[index] = symbol->vaddr;
		auto alias = exact;
		alias.library = "libkernel";
		const auto* kernel_symbol = symbols.Find(alias);
		if (!Check(kernel_symbol != nullptr && kernel_symbol->vaddr == symbol->vaddr,
		           "kernel POSIX alias changed the function or error ABI")) return false;
		kernel_aliases[index] = kernel_symbol->vaddr;
		for (unsigned mismatch = 0; mismatch < 6; ++mismatch)
		{
			auto wrong = exact;
			switch (mismatch)
			{
				case 0: wrong.library = "unrelated_posix_library"; break;
				case 1: wrong.module = "unrelated_kernel_module"; break;
				case 2: wrong.library_version = 2; break;
				case 3: wrong.module_version_major = 2; break;
				case 4: wrong.module_version_minor = 0; break;
				case 5: wrong.type = Lumen::Loader::SymbolType::Object; break;
			}
			if (!Check(symbols.Find(wrong) == nullptr, "semaphore export resolved with an unsupported identity")) return false;
			if (mismatch != 0)
			{
				wrong.library = "libkernel";
				if (!Check(symbols.Find(wrong) == nullptr, "kernel POSIX alias resolved with an unsupported identity")) return false;
			}
		}
	}
	if (!Check(symbols.FindByStdName(U"sem_init") == nullptr && symbols.FindByStdName(U"sem_wait") == nullptr &&
	           symbols.FindByStdName(U"sem_timedwait") == nullptr && symbols.FindByStdName(U"sem_post") == nullptr,
	           "semaphore export gained a standard-name fallback")) return false;
	const auto exported = reinterpret_cast<decltype(&Posix::sem_init)>(resolved[0]);
	const auto wait_exported = reinterpret_cast<decltype(&Posix::sem_wait)>(resolved[1]);
	const auto timed_exported = reinterpret_cast<decltype(&Posix::sem_timedwait)>(resolved[2]);
	const auto post_exported = reinterpret_cast<PostFunction>(resolved[3]);
	const auto alias_init = reinterpret_cast<decltype(&Posix::sem_init)>(kernel_aliases[0]);
	const auto alias_wait = reinterpret_cast<decltype(&Posix::sem_wait)>(kernel_aliases[1]);
	const auto alias_timed = reinterpret_cast<decltype(&Posix::sem_timedwait)>(kernel_aliases[2]);
	const auto alias_post = reinterpret_cast<PostFunction>(kernel_aliases[3]);
	*Posix::GetErrorAddr() = k_errno_sentinel;
	if (!Check(alias_init(nullptr, 0, 0) == -1 && *Posix::GetErrorAddr() == Posix::POSIX_EINVAL,
	           "kernel sem_init alias did not retain POSIX errors")) return false;
	*Posix::GetErrorAddr() = k_errno_sentinel;
	if (!Check(alias_wait(nullptr) == -1 && *Posix::GetErrorAddr() == Posix::POSIX_EINVAL,
	           "kernel sem_wait alias did not retain POSIX errors")) return false;
	*Posix::GetErrorAddr() = k_errno_sentinel;
	if (!Check(alias_timed(nullptr, nullptr) == -1 && *Posix::GetErrorAddr() == Posix::POSIX_EINVAL,
	           "kernel sem_timedwait alias did not retain POSIX errors")) return false;
	*Posix::GetErrorAddr() = k_errno_sentinel;
	if (!Check(alias_post(nullptr) == -1 && *Posix::GetErrorAddr() == Posix::POSIX_EINVAL,
	           "kernel sem_post alias did not retain POSIX errors")) return false;
	Handle shared = nullptr;
	*Posix::GetErrorAddr() = k_errno_sentinel;
	const Kernel::KernelTimespec expired {0, 0};
	if (!Check(alias_init(&shared, 0, 1) == 0 && wait_exported(&shared) == 0 &&
	           post_exported(&shared) == 0 && alias_wait(&shared) == 0 &&
	           alias_post(&shared) == 0 && timed_exported(&shared, nullptr) == 0 &&
	           *Posix::GetErrorAddr() == k_errno_sentinel &&
	           alias_timed(&shared, &expired) == -1 && *Posix::GetErrorAddr() == Posix::POSIX_ETIMEDOUT &&
	           Sema::KernelDeleteSema(shared) == 0,
	           "POSIX namespaces did not share tokens or preserve absolute timed-wait errors")) return false;
	g_post = post_exported;
	Handle handle = nullptr;
	*Posix::GetErrorAddr() = k_errno_sentinel;
	return Check(exported(&handle, 0, 3) == 0 && *Posix::GetErrorAddr() == k_errno_sentinel && handle != nullptr &&
	             wait_exported(&handle) == 0 && *Posix::GetErrorAddr() == k_errno_sentinel &&
	             timed_exported(&handle, nullptr) == 0 && *Posix::GetErrorAddr() == k_errno_sentinel &&
	             Sema::KernelPollSema(handle, 1) == 0 &&
	             Sema::KernelPollSema(handle, 1) == Kernel::KERNEL_ERROR_EBUSY &&
	             post_exported(&handle) == 0 && *Posix::GetErrorAddr() == k_errno_sentinel &&
	             wait_exported(&handle) == 0 && *Posix::GetErrorAddr() == k_errno_sentinel &&
	             Sema::KernelPollSema(handle, 1) == Kernel::KERNEL_ERROR_EBUSY && Sema::KernelDeleteSema(handle) == 0,
	             "resolved native exports did not initialize, consume, and post a semaphore token");
}

bool TestPthreadSemaphoreBinding()
{
	Lumen::Loader::SymbolDatabase symbols;
	if (!Check(Lumen::Libs::Init(U"libkernel_1", &symbols), "pthread semaphore registration failed")) return false;
	Lumen::Loader::SymbolResolve exact {};
	exact.library = "libkernel";
	exact.library_version = 1;
	exact.module = "libkernel";
	exact.module_version_major = 1;
	exact.module_version_minor = 1;
	exact.type = Lumen::Loader::SymbolType::Func;
	struct Export { const char* nid; uint64_t target; };
	const std::array<Export, 7> expected {{
	    {"GEnUkDZoUwY", reinterpret_cast<uint64_t>(&Sema::PthreadSemInit)},
	    {"Vwc+L05e6oE", reinterpret_cast<uint64_t>(&Sema::PthreadSemDestroy)},
	    {"C36iRE0F5sE", reinterpret_cast<uint64_t>(&Sema::PthreadSemWait)},
	    {"H2a+IN9TP0E", reinterpret_cast<uint64_t>(&Sema::PthreadSemTrywait)},
	    {"fjN6NQHhK8k", reinterpret_cast<uint64_t>(&Sema::PthreadSemTimedwait)},
	    {"aishVAiFaYM", reinterpret_cast<uint64_t>(&Sema::PthreadSemPost)},
	    {"DjpBvGlaWbQ", reinterpret_cast<uint64_t>(&Sema::PthreadSemGetvalue)}
	}};
	for (const auto& entry: expected)
	{
		exact.name = entry.nid;
		const auto* symbol = symbols.Find(exact);
		if (!Check(symbol != nullptr && symbol->vaddr == entry.target,
		           "pthread semaphore NID did not resolve to its native entrypoint")) return false;
		auto wrong = exact;
		wrong.library = "libScePosix";
		if (!Check(symbols.Find(wrong) == nullptr, "pthread semaphore leaked into POSIX library")) return false;
	}
	return true;
}

bool TestPthreadSemaphore()
{
	*Posix::GetErrorAddr() = k_errno_sentinel;
	GuardedSlot slot(9);
	const auto before = slot.bytes;
	if (!Check(Sema::PthreadSemInit(nullptr, 0, 0, nullptr) == Kernel::KERNEL_ERROR_EINVAL &&
	           Sema::PthreadSemInit(slot.Address(), 1, 0, nullptr) == Kernel::KERNEL_ERROR_EINVAL &&
	           Sema::PthreadSemInit(slot.Address(), 0, UINT32_MAX, nullptr) == Kernel::KERNEL_ERROR_EINVAL &&
	           slot.bytes == before, "pthread init validation changed output or returned wrong errors")) return false;
	if (!Check(Sema::PthreadSemInit(slot.Address(), 0, 2, nullptr) == 0 && slot.Canaries() && slot.Read() != nullptr,
	           "pthread init did not publish only an eight-byte handle")) return false;
	std::array<uint8_t, 12> count_bytes;
	count_bytes.fill(0xa5);
	auto expected_count_bytes = count_bytes;
	const int32_t expected_count = 2;
	std::memcpy(expected_count_bytes.data() + 1, &expected_count, sizeof(expected_count));
	if (!Check(Sema::PthreadSemGetvalue(slot.Address(), nullptr) == Kernel::KERNEL_ERROR_EINVAL &&
	           Sema::PthreadSemGetvalue(slot.Address(), reinterpret_cast<int32_t*>(count_bytes.data() + 1)) == 0 &&
	           count_bytes == expected_count_bytes,
	           "pthread getvalue changed tokens or wrote beyond its unaligned four-byte output")) return false;
	int32_t count = -1;
	if (!Check(Sema::PthreadSemGetvalue(slot.Address(), &count) == 0 && count == 2 &&
	           Sema::PthreadSemTimedwait(slot.Address(), 0) == 0 &&
	           Posix::sem_wait(slot.Address()) == 0 &&
	           Sema::PthreadSemTrywait(slot.Address()) == Kernel::KERNEL_ERROR_EAGAIN &&
	           Sema::PthreadSemTimedwait(slot.Address(), 0) == Kernel::KERNEL_ERROR_ETIMEDOUT &&
	           Sema::PthreadSemGetvalue(slot.Address(), &count) == 0 && count == 0,
	           "pthread/POSIX token accounting or zero-timeout behavior differs")) return false;
	const auto started = std::chrono::steady_clock::now();
	const int timeout_result = Sema::PthreadSemTimedwait(slot.Address(), 2000);
	const auto elapsed = std::chrono::steady_clock::now() - started;
	if (!Check(timeout_result == Kernel::KERNEL_ERROR_ETIMEDOUT && elapsed >= std::chrono::microseconds(2000) &&
	           elapsed < std::chrono::seconds(2), "pthread relative timeout expired early or used incorrect units")) return false;
	if (!Check(Sema::PthreadSemPost(slot.Address()) == 0 && Sema::PthreadSemWait(slot.Address()) == 0 &&
	           Sema::PthreadSemTrywait(slot.Address()) == Kernel::KERNEL_ERROR_EAGAIN,
	           "pthread post/wait failed to exchange exactly one token")) return false;
	Handle stale = slot.Read();
	if (!Check(Sema::PthreadSemDestroy(slot.Address()) == 0 && slot.Read() == nullptr && slot.Canaries() &&
	           Sema::PthreadSemDestroy(slot.Address()) == Kernel::KERNEL_ERROR_EINVAL &&
	           Sema::PthreadSemPost(&stale) == Kernel::KERNEL_ERROR_EINVAL,
	           "pthread destroy did not invalidate the handle")) return false;
	Handle full = nullptr;
	if (!Check(Sema::PthreadSemInit(&full, 0, k_maximum, "limit") == 0 &&
	           Sema::PthreadSemPost(&full) == Kernel::KERNEL_ERROR_EOVERFLOW &&
	           Sema::PthreadSemGetvalue(&full, &count) == 0 && count == static_cast<int32_t>(k_maximum) &&
	           Sema::PthreadSemDestroy(&full) == 0, "pthread overflow changed the semaphore count")) return false;
	Handle invented = reinterpret_cast<Handle>(uintptr_t {1});
	return Check(Sema::PthreadSemWait(nullptr) == Kernel::KERNEL_ERROR_EINVAL &&
	             Sema::PthreadSemTrywait(&full) == Kernel::KERNEL_ERROR_EINVAL &&
	             Sema::PthreadSemTimedwait(&invented, 0) == Kernel::KERNEL_ERROR_EINVAL &&
	             Sema::PthreadSemGetvalue(&invented, &count) == Kernel::KERNEL_ERROR_EINVAL &&
	             *Posix::GetErrorAddr() == k_errno_sentinel,
	             "pthread error encoding or errno preservation differs");
}

bool TestPthreadSemaphoreMemory()
{
	Pages pages;
	if (!pages.Init()) return false;
	Handle handle = nullptr;
	if (!Check(Sema::PthreadSemInit(&handle, 0, 1, nullptr) == 0, "pthread protected test initialization failed")) return false;
	auto* output = reinterpret_cast<Handle*>(pages.Data() + pages.Page() + 32);
	std::memcpy(output, &handle, sizeof(handle));
	const auto before = pages.Snapshot();
	if (!pages.ProtectMiddle(Memory::Mode::Read)) return false;
	const bool read_only = Check(
	    Sema::PthreadSemInit(output, 0, 0, nullptr) == Kernel::KERNEL_ERROR_EFAULT &&
	    Sema::PthreadSemDestroy(output) == Kernel::KERNEL_ERROR_EFAULT &&
	    Sema::PthreadSemGetvalue(&handle, reinterpret_cast<int32_t*>(output)) == Kernel::KERNEL_ERROR_EFAULT &&
	    Sema::PthreadSemTrywait(output) == 0,
	    "pthread read-only handle or output behavior differs");
	if (!pages.ProtectMiddle(Memory::Mode::NoAccess)) return false;
	const bool inaccessible = Check(
	    Sema::PthreadSemPost(output) == Kernel::KERNEL_ERROR_EFAULT &&
	    Sema::PthreadSemTimedwait(output, 0) == Kernel::KERNEL_ERROR_EFAULT &&
	    Sema::PthreadSemInit(reinterpret_cast<Handle*>(pages.Data() + pages.Page() - 4), 0, 0, nullptr) ==
	        Kernel::KERNEL_ERROR_EFAULT,
	    "pthread inaccessible or partially writable handle was accepted");
	if (!pages.ProtectMiddle(Memory::Mode::ReadWrite)) return false;
	const bool preserved = Check(pages.Snapshot() == before, "pthread rejected writes changed output bytes");
	const bool destroyed = Check(Sema::PthreadSemDestroy(&handle) == 0, "pthread protected test cleanup failed");
	return read_only && inaccessible && preserved && destroyed;
}

bool TestPthreadSemaphoreBlocking()
{
	for (bool timed: {false, true})
	{
		Handle handle = nullptr;
		if (!Check(Sema::PthreadSemInit(&handle, 0, 0, nullptr) == 0, "pthread blocking init failed")) return false;
		std::atomic_bool finished {false};
		int result = -1;
		std::thread worker([&] {
			result = timed ? Sema::PthreadSemTimedwait(&handle, UINT32_MAX) : Sema::PthreadSemWait(&handle);
			finished.store(true, std::memory_order_release);
		});
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (!finished.load(std::memory_order_acquire) && Sema::SemaphoreWaiterCountForTest(handle) == 0 &&
		       std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
		const bool waiting = Check(!finished.load(std::memory_order_acquire) &&
		                           Sema::SemaphoreWaiterCountForTest(handle) == 1,
		                           "pthread empty wait did not block");
		const bool busy = waiting && Check(Sema::PthreadSemDestroy(&handle) == Kernel::KERNEL_ERROR_EBUSY,
		                                  "pthread destroy accepted a registered waiter");
		const bool posted = Check(Sema::PthreadSemPost(&handle) == 0, "pthread waiter release failed");
		worker.join();
		const bool consumed = Check(result == 0 && Sema::PthreadSemTrywait(&handle) == Kernel::KERNEL_ERROR_EAGAIN,
		                            "pthread blocked wait failed or retained its token");
		const bool destroyed = Check(Sema::PthreadSemDestroy(&handle) == 0, "pthread blocking cleanup failed");
		if (!waiting || !busy || !posted || !consumed || !destroyed) return false;
	}
	return true;
}

bool TestInlineStaticWaitPost()
{
	if (!Check(Sema::ResetForNextSession(), "inline static pre-test reset failed")) return false;
	GuardedInlineSlot slot;
	const InlineRecord identity {k_inline_magic, 0, 0, 0};
	slot.Write(identity);
	const auto before = slot.bytes;
	std::atomic_bool finished {false};
	int wait_result = -2;
	int wait_error = -2;
	std::thread waiter([&] {
		*Posix::GetErrorAddr() = k_errno_sentinel;
		wait_result = Posix::sem_wait(slot.Address());
		wait_error = *Posix::GetErrorAddr();
		finished.store(true, std::memory_order_release);
	});
	// The record itself is the object: a blocked waiter only shows as the
	// published waiter hint, while the token count stays untouched.
	const bool registered = WaitUntil([&] {
		return finished.load(std::memory_order_acquire) || slot.Read().has_waiters != 0;
	});
	const bool blocked = Check(registered && !finished.load(std::memory_order_acquire) &&
	                           slot.Read().has_waiters == 1 && slot.Read().initial_count == 0,
	                           "inline static zero-count wait did not publish its waiter hint");
	const bool posted = PostCall(slot.Address(), 0, k_errno_sentinel, "inline static post releases wait");
	waiter.join();
	const bool completed = Check(posted && wait_result == 0 && wait_error == k_errno_sentinel &&
	                             slot.Read() == identity && slot.bytes == before && slot.Canaries(),
	                             "inline static wait/post changed the record or errno");
	const bool immediate = PostCall(slot.Address(), 0, k_errno_sentinel, "inline static immediate token") &&
	                       TimedCall(slot.Address(), nullptr, 0, k_errno_sentinel,
	                                 "inline static immediate timed wait");
	const bool reset = Check(Sema::ResetForNextSession(), "inline static cleanup reset failed");
	return blocked && posted && completed && immediate && reset;
}

bool TestInlineInitAndValidation()
{
	if (!Check(!Posix::InlinePosixSemaphoreEnvironmentValueEnabledForTest(nullptr) &&
	           !Posix::InlinePosixSemaphoreEnvironmentValueEnabledForTest("") &&
	           !Posix::InlinePosixSemaphoreEnvironmentValueEnabledForTest("0") &&
	           Posix::InlinePosixSemaphoreEnvironmentValueEnabledForTest("1") &&
	           !Posix::InlinePosixSemaphoreEnvironmentValueEnabledForTest("01") &&
	           !Posix::InlinePosixSemaphoreEnvironmentValueEnabledForTest("true") &&
	           !Posix::InlinePosixSemaphoreEnvironmentValueEnabledForTest("1 "),
	           "inline selector did not require exact value one")) return false;
	if (!Check(Sema::ResetForNextSession(), "inline init pre-test reset failed")) return false;
	GuardedInlineSlot slot(9);
	const InlineRecord initialized {k_inline_magic, 0, 2, 0};
	if (!Call(slot.Address(), 0, 2, 0, k_errno_sentinel, "inline sem_init") ||
	    !Check(slot.Read() == initialized && slot.Canaries(),
	           "inline sem_init did not publish exactly its sixteen-byte identity")) return false;
	const Kernel::KernelTimespec expired {0, 0};
	// The record carries the token count, so consuming a token rewrites it: the
	// magic, the waiter hint and the flags must stay untouched.
	const InlineRecord after_wait {k_inline_magic, 0, 1, 0};
	const InlineRecord after_timed {k_inline_magic, 0, 0, 0};
	if (!WaitCall(slot.Address(), 0, k_errno_sentinel, "inline initialized wait") ||
	    !Check(slot.Read() == after_wait && slot.Canaries(),
	           "inline initialized wait did not consume exactly one record token") ||
	    !TimedCall(slot.Address(), nullptr, 0, k_errno_sentinel, "inline initialized immediate timed wait") ||
	    !Check(slot.Read() == after_timed,
	           "inline initialized immediate timed wait did not consume the record token") ||
	    !TimedCall(slot.Address(), &expired, -1, Posix::POSIX_ETIMEDOUT, "inline initialized timeout") ||
	    !Check(slot.Read() == after_timed, "inline timeout changed the record") ||
	    !Call(slot.Address(), 0, 2, 0, k_errno_sentinel, "inline re-initialization") ||
	    !Check(slot.Read() == initialized, "inline re-initialization did not rewrite the record")) return false;

	// The waiter hint and the token count are mutable guest state, so only the
	// magic, the flags and a negative count invalidate a record.
	for (const InlineRecord& invalid: std::array<InlineRecord, 3> {{
	         {k_inline_magic ^ 1U, 0, 0, 0},
	         {k_inline_magic, 0, k_maximum + 1U, 0},
	         {k_inline_magic, 0, 0, 1}}})
	{
		GuardedInlineSlot candidate;
		candidate.Write(invalid);
		const auto before = candidate.bytes;
		if (!WaitCall(candidate.Address(), -1, Posix::POSIX_EINVAL, "invalid inline identity") ||
		    !Check(candidate.bytes == before &&
		               !Posix::InlinePosixSemaphoreRecordSupportedForTest(candidate.Address()),
		               "invalid inline identity was changed or adopted")) return false;
	}

	GuardedInlineSlot rejected;
	const auto rejected_before = rejected.bytes;
	if (!Call(rejected.Address(), 1, 0, -1, Posix::POSIX_ENOSYS, "inline pshared rejection") ||
	    !Call(rejected.Address(), 0, k_maximum + 1U, -1, Posix::POSIX_EINVAL,
	          "inline maximum rejection") ||
	    !Check(rejected.bytes == rejected_before, "inline init validation changed output")) return false;

	GuardedInlineSlot full;
	const InlineRecord full_identity {k_inline_magic, 0, k_maximum, 0};
	if (!Call(full.Address(), 0, k_maximum, 0, k_errno_sentinel, "inline full initialization") ||
	    !PostCall(full.Address(), -1, Posix::POSIX_EOVERFLOW, "inline post overflow") ||
	    !Check(full.Read() == full_identity && full.Canaries(),
	           "inline post overflow changed the identity")) return false;
	return Check(Sema::ResetForNextSession(), "inline init cleanup reset failed");
}

bool TestInlineMemory()
{
	if (!Check(Sema::ResetForNextSession(), "inline memory pre-test reset failed")) return false;
	Pages pages;
	if (!pages.Init()) return false;
	const InlineRecord identity {k_inline_magic, 0, 0, 0};
	const size_t middle = pages.Page() + 32;
	std::memcpy(pages.Data() + middle, &identity, sizeof(identity));
	const auto before = pages.Snapshot();
	if (!pages.ProtectMiddle(Memory::Mode::Read)) return false;
	const bool read_only = WaitCall(reinterpret_cast<Handle*>(pages.Data() + middle), -1,
	                                Posix::POSIX_EFAULT, "inline read-only object");
	if (!pages.ProtectMiddle(Memory::Mode::NoAccess)) return false;
	const bool inaccessible = WaitCall(reinterpret_cast<Handle*>(pages.Data() + middle), -1,
	                                  Posix::POSIX_EFAULT, "inline inaccessible object");
	const bool partial_read = WaitCall(reinterpret_cast<Handle*>(pages.Data() + pages.Page() - 8), -1,
	                                  Posix::POSIX_EFAULT, "inline partially readable object");
	const bool partial_write = Call(reinterpret_cast<Handle*>(pages.Data() + pages.Page() - 8), 0, 0, -1,
	                                Posix::POSIX_EFAULT, "inline partially writable init object");
	const size_t tail = pages.Page() - sizeof(InlineRecord);
	const bool exact_tail = Call(reinterpret_cast<Handle*>(pages.Data() + tail), 0, 1, 0,
	                            k_errno_sentinel, "inline exact sixteen-byte tail");
	if (!pages.ProtectMiddle(Memory::Mode::ReadWrite)) return false;
	InlineRecord tail_record {};
	std::memcpy(&tail_record, pages.Data() + tail, sizeof(tail_record));
	const InlineRecord expected_tail {k_inline_magic, 0, 1, 0};
	bool surrounding = true;
	for (size_t index = 0; index < pages.Size(); ++index)
	{
		if (index >= tail && index < tail + sizeof(InlineRecord)) continue;
		if (pages.Data()[index] != before[index])
		{
			surrounding = false;
			break;
		}
	}
	const bool preserved = Check(read_only && inaccessible && partial_read && partial_write && exact_tail &&
	                             tail_record == expected_tail && surrounding,
	                             "inline sixteen-byte memory checks changed adjacent bytes or errors");
	const bool reset = Check(Sema::ResetForNextSession(), "inline memory cleanup reset failed");
	return preserved && reset;
}

bool TestInlineCrossFamilyRejection()
{
	if (!Check(Sema::ResetForNextSession(), "inline cross-family pre-test reset failed")) return false;
	GuardedInlineSlot pthread_slot;
	if (!Check(Sema::PthreadSemInit(pthread_slot.Address(), 0, 1, nullptr) == 0,
	           "pthread handle setup for inline rejection failed")) return false;
	const auto pthread_before = pthread_slot.bytes;
	const bool posix_rejected = WaitCall(pthread_slot.Address(), -1, Posix::POSIX_EINVAL,
	                                     "POSIX inline rejects pthread handle") &&
	                            Check(pthread_slot.bytes == pthread_before,
	                                  "POSIX inline rejection changed pthread handle storage");
	const bool pthread_destroyed = Check(Sema::PthreadSemDestroy(pthread_slot.Address()) == 0,
	                                     "pthread cross-family cleanup failed");

	GuardedInlineSlot inline_slot;
	inline_slot.Write({k_inline_magic, 0, 0, 0});
	const auto inline_before = inline_slot.bytes;
	const bool pthread_rejected = Check(
	    Sema::PthreadSemWait(inline_slot.Address()) == Kernel::KERNEL_ERROR_EINVAL &&
	    inline_slot.bytes == inline_before &&
	    Posix::InlinePosixSemaphoreRecordSupportedForTest(inline_slot.Address()),
	    "pthread wrapper accepted or changed a POSIX inline object");
	const bool reset = Check(Sema::ResetForNextSession(), "inline cross-family cleanup reset failed");
	return posix_rejected && pthread_destroyed && pthread_rejected && reset;
}

bool TestInlineConcurrentFirstUse()
{
	if (!Check(Sema::ResetForNextSession(), "inline concurrency pre-test reset failed")) return false;
	GuardedInlineSlot slot;
	const InlineRecord identity {k_inline_magic, 0, 0, 0};
	slot.Write(identity);
	const auto before = slot.bytes;
	std::atomic<unsigned> ready {0};
	std::atomic_bool start {false};
	std::atomic<unsigned> completed {0};
	std::array<int, 2> results {{-2, -2}};
	std::array<int, 2> errors {{-2, -2}};
	std::array<std::thread, 2> waiters;
	for (size_t index = 0; index < waiters.size(); ++index)
	{
		waiters[index] = std::thread([&, index] {
			*Posix::GetErrorAddr() = k_errno_sentinel;
			ready.fetch_add(1, std::memory_order_release);
			while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
			results[index] = Posix::sem_wait(slot.Address());
			errors[index] = *Posix::GetErrorAddr();
			completed.fetch_add(1, std::memory_order_release);
		});
	}
	while (ready.load(std::memory_order_acquire) != waiters.size()) std::this_thread::yield();
	start.store(true, std::memory_order_release);
	// Both waiters publish into the same guest record: the hint counts them and
	// the token count stays zero until a release publishes tokens.
	const bool both_waiting = WaitUntil([&] {
		return completed.load(std::memory_order_acquire) != 0 || slot.Read().has_waiters == waiters.size();
	});
	const bool unique = Check(both_waiting && completed.load(std::memory_order_acquire) == 0 &&
	                          slot.Read().has_waiters == waiters.size() &&
	                          slot.Read().initial_count == 0 &&
	                          Posix::InlinePosixSemaphoreRecordSupportedForTest(slot.Address()),
	                          "concurrent inline waiters did not share one published hint");
	const bool posted = PostCall(slot.Address(), 0, k_errno_sentinel,
	                             "inline concurrent first waiter release") &&
	                    PostCall(slot.Address(), 0, k_errno_sentinel,
	                             "inline concurrent second waiter release");
	for (auto& waiter: waiters) waiter.join();
	const bool consumed = Check(posted && results[0] == 0 && results[1] == 0 &&
	                            errors[0] == k_errno_sentinel && errors[1] == k_errno_sentinel &&
	                            completed.load(std::memory_order_acquire) == 2 && slot.bytes == before &&
	                            slot.Read() == identity,
	                            "inline concurrent waiters did not consume the released tokens");
	const bool reset = Check(Sema::ResetForNextSession(), "inline concurrency cleanup reset failed");
	return unique && posted && consumed && reset;
}

bool TestInlineResetAddressReuse()
{
	if (!Check(Sema::ResetForNextSession(), "inline reuse pre-test reset failed")) return false;
	GuardedInlineSlot slot;
	const InlineRecord identity {k_inline_magic, 0, 0, 0};
	slot.Write(identity);
	// The inline profile owns no process-side state: a post/wait cycle must not
	// change the record and a session reset must leave it exactly as usable.
	if (!PostCall(slot.Address(), 0, k_errno_sentinel, "inline reuse first post") ||
	    !WaitCall(slot.Address(), 0, k_errno_sentinel, "inline reuse first wait")) return false;
	if (!Check(slot.Read() == identity && slot.Canaries() &&
	           Posix::InlinePosixSemaphoreRecordSupportedForTest(slot.Address()),
	           "inline post/wait cycle changed the guest record")) return false;
	if (!Check(Sema::ResetForNextSession() && slot.Read() == identity &&
	           Posix::InlinePosixSemaphoreRecordSupportedForTest(slot.Address()),
	           "inline session reset changed or invalidated the guest record")) return false;
	if (!PostCall(slot.Address(), 0, k_errno_sentinel, "inline reuse second post") ||
	    !WaitCall(slot.Address(), 0, k_errno_sentinel, "inline reuse second wait") ||
	    !Check(slot.Read() == identity, "inline address was not reusable after the session reset")) return false;

	// Destroy clears the record; the same address then accepts a new object.
	*Posix::GetErrorAddr() = k_errno_sentinel;
	if (!Check(Posix::sem_destroy(slot.Address()) == 0 && *Posix::GetErrorAddr() == k_errno_sentinel &&
	           !Posix::InlinePosixSemaphoreRecordSupportedForTest(slot.Address()),
	           "inline destroy did not clear the record")) return false;
	const InlineRecord cleared {};
	if (!Check(slot.Read() == cleared, "inline destroy left record bytes behind")) return false;
	if (!Call(slot.Address(), 0, 1, 0, k_errno_sentinel, "inline reinitialization after destroy") ||
	    !WaitCall(slot.Address(), 0, k_errno_sentinel,
	              "inline reinitialized address remains usable")) return false;
	return Check(Sema::ResetForNextSession() && slot.Read() == identity,
	             "inline reinitialized record or final reset failed");
}

bool RunInlineSuite()
{
	return TestInlineStaticWaitPost() && TestInlineInitAndValidation() && TestInlineMemory() &&
	       TestInlineCrossFamilyRejection() && TestInlineConcurrentFirstUse() &&
	       TestInlineResetAddressReuse();
}

bool RunInlineChild(const char* executable)
{
	const pid_t child = fork();
	if (child < 0) return Check(false, "could not fork inline semaphore profile");
	if (child == 0)
	{
		execl(executable, executable, "--inline-profile", static_cast<char*>(nullptr));
		std::_Exit(126);
	}
	int status = 0;
	if (waitpid(child, &status, 0) != child) return Check(false, "could not wait for inline semaphore profile");
	return Check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	             "inline semaphore profile child failed");
}

} // namespace

int main(int argc, char** argv)
{
	const bool requested_inline = argc == 2 && std::strcmp(argv[1], "--inline-profile") == 0;
	const int environment_result = requested_inline ?
	    setenv("LUMEN_EXPERIMENTAL_POSIX_INLINE_SEMAPHORE", "1", 1) :
	    unsetenv("LUMEN_EXPERIMENTAL_POSIX_INLINE_SEMAPHORE");
	if (!Check(environment_result == 0,
	           "could not establish explicit inline semaphore test profile")) return 1;
	const bool inline_profile = requested_inline;
	if (!inline_profile && !RunInlineChild(argv[0])) return 1;
	Watchdog watchdog;
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	Memory::Init();
	if (!TestHostClockSampling()) return 1;
	const bool passed = inline_profile ? RunInlineSuite() :
	    TestBinding() && TestCountAndMaximum() && TestErrors() && TestMemoryAndReset() &&
	    TestWaitMemory() && TestPostCountAndErrors() && TestConcurrentPostOverflow() &&
	    TestPostUnblocksWaiters() && TestPostMemory() && TestBlockingWait() && TestDeleteWithWaiter() &&
	    TestTimedWaitCarryContract() && TestTimedWait() && TestTimedMemory() &&
	    TestPthreadSemaphoreBinding() && TestPthreadSemaphore() &&
	    TestPthreadSemaphoreMemory() && TestPthreadSemaphoreBlocking();
	const bool cleanup = Sema::ResetForNextSession();
	if (!passed || !Check(cleanup, "final session cleanup failed")) return 1;
	std::puts("PosixSemaphoreTest: PASS");
	return 0;
}
