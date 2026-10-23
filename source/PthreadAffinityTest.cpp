#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/HostCall.h"
#include "Emulator/Loader/HostCallScalar.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Loader/SystemContent.h"
#include "Emulator/Log.h"

#include "CpuX64.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <type_traits>

namespace {

namespace Kernel = Lumen::Libs::LibKernel;
using Mask = Kernel::KernelCpumask;

// Bound deadlocks and a stalled stress worker when invoked outside CTest too.
class Watchdog final
{
public:
	Watchdog(): m_thread([this] {
		std::unique_lock<std::mutex> lock(m_mutex);
		if (!m_changed.wait_for(lock, std::chrono::seconds(10), [this] { return m_done; }))
		{
			std::fprintf(stderr, "PthreadAffinityTest: FAIL: operation timed out\n");
			std::fflush(stderr);
			std::_Exit(2);
		}
	}) {}

	~Watchdog()
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_done = true;
		}
		m_changed.notify_one();
		m_thread.join();
	}

	Watchdog(const Watchdog&)            = delete;
	Watchdog& operator=(const Watchdog&) = delete;

private:
	std::mutex              m_mutex;
	std::condition_variable m_changed;
	bool                    m_done = false;
	std::thread             m_thread;
};

bool Check(bool condition, const char* message)
{
	if (!condition)
	{
		std::fprintf(stderr, "PthreadAffinityTest: FAIL: %s\n", message);
	}
	return condition;
}

bool Expect(int actual, int expected, const char* operation)
{
	if (actual != expected)
	{
		std::fprintf(stderr, "PthreadAffinityTest: FAIL: %s returned 0x%08x, expected 0x%08x\n", operation,
		             static_cast<unsigned>(actual), static_cast<unsigned>(expected));
		return false;
	}
	return true;
}

// The descriptors follow the process-lifetime main-thread initialization path.
// Their host threads stay alive throughout every API call using the handles.
class LiveThread final
{
public:
	LiveThread(): m_host([this] {
		Kernel::PthreadInitSelfForMainThread();
		std::unique_lock<std::mutex> lock(m_mutex);
		m_thread = Kernel::PthreadSelf();
		m_changed.notify_one();
		m_changed.wait(lock, [this] { return m_stop; });
	})
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		m_changed.wait(lock, [this] { return m_thread != nullptr; });
	}

	~LiveThread()
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_stop = true;
		}
		m_changed.notify_one();
		m_host.join();
	}

	LiveThread(const LiveThread&)            = delete;
	LiveThread& operator=(const LiveThread&) = delete;
	Kernel::Pthread Get() const { return m_thread; }

private:
	std::mutex              m_mutex;
	std::condition_variable m_changed;
	Kernel::Pthread         m_thread = nullptr;
	bool                    m_stop = false;
	std::thread             m_host;
};

class Attribute final
{
public:
	~Attribute()
	{
		if (m_attr != nullptr)
		{
			Kernel::PthreadAttrDestroy(&m_attr);
		}
	}
	bool Init() { return Expect(Kernel::PthreadAttrInit(&m_attr), 0, "attribute init"); }
	Kernel::PthreadAttr* Get() { return &m_attr; }

private:
	Kernel::PthreadAttr m_attr = nullptr;
};

struct GuardedMask
{
	uint64_t before = UINT64_C(0xb639ad5802e7c14f);
	Mask value = UINT64_C(0x8dc3b17a6024e95f);
	uint64_t after = UINT64_C(0x4a87f16c39d250be);

	bool Intact() const
	{
		return Check(before == UINT64_C(0xb639ad5802e7c14f) && after == UINT64_C(0x4a87f16c39d250be),
		             "getter wrote outside its eight-byte output");
	}
};

static_assert(sizeof(Mask) == 8, "guest affinity is a 64-bit mask");
static_assert(offsetof(GuardedMask, value) == 8 && offsetof(GuardedMask, after) == 16 && sizeof(GuardedMask) == 24,
              "output canaries must immediately surround the eight-byte mask");

bool ReadThread(Kernel::Pthread thread, Mask expected)
{
	GuardedMask output;
	return Expect(Kernel::PthreadGetaffinity(thread, &output.value), 0, "thread getaffinity") && output.Intact() &&
	       Check(output.value == expected, "thread affinity differs from the requested 64-bit mask");
}

bool ReadAttribute(const Kernel::PthreadAttr* attr, Mask expected)
{
	GuardedMask output;
	return Expect(Kernel::PthreadAttrGetaffinity(attr, &output.value), 0, "attribute getaffinity") && output.Intact() &&
	       Check(output.value == expected, "attribute affinity differs from the expected 64-bit mask");
}

bool TestErrors(Kernel::Pthread thread)
{
	Attribute attr;
	if (!attr.Init()) return false;
	Kernel::PthreadAttr missing = nullptr;
	GuardedMask output;
	const Mask untouched = output.value;
	return Expect(Kernel::PthreadGetaffinity(nullptr, &output.value), Kernel::KERNEL_ERROR_ESRCH, "null thread") &&
	       Check(output.value == untouched, "null-thread error changed output") && output.Intact() &&
	       Expect(Kernel::PthreadGetaffinity(nullptr, nullptr), Kernel::KERNEL_ERROR_ESRCH, "null thread and output") &&
	       Expect(Kernel::PthreadGetaffinity(thread, nullptr), Kernel::KERNEL_ERROR_EINVAL, "null output") &&
	       Expect(Kernel::PthreadSetaffinity(nullptr, 1), Kernel::KERNEL_ERROR_ESRCH, "set null thread") &&
	       Expect(Kernel::PthreadAttrGetaffinity(nullptr, &output.value), Kernel::KERNEL_ERROR_EINVAL, "null attribute pointer") &&
	       Check(output.value == untouched, "null-attribute-pointer error changed output") && output.Intact() &&
	       Expect(Kernel::PthreadAttrGetaffinity(&missing, &output.value), Kernel::KERNEL_ERROR_EINVAL, "null attribute handle") &&
	       Check(output.value == untouched, "null-attribute-handle error changed output") && output.Intact() &&
	       Expect(Kernel::PthreadAttrGetaffinity(attr.Get(), nullptr), Kernel::KERNEL_ERROR_EINVAL, "null attribute output") &&
	       Expect(Kernel::PthreadAttrSetaffinity(nullptr, 1), Kernel::KERNEL_ERROR_EINVAL, "set null attribute pointer") &&
	       Expect(Kernel::PthreadAttrSetaffinity(&missing, 1), Kernel::KERNEL_ERROR_EINVAL, "set null attribute handle") &&
	       Expect(Kernel::PthreadAttrGet(nullptr, attr.Get()), Kernel::KERNEL_ERROR_EINVAL, "copy null thread") &&
	       Expect(Kernel::PthreadAttrGet(thread, nullptr), Kernel::KERNEL_ERROR_EINVAL, "copy null attribute pointer") &&
	       Expect(Kernel::PthreadAttrGet(thread, &missing), Kernel::KERNEL_ERROR_EINVAL, "copy null attribute handle") &&
	       ReadAttribute(attr.Get(), 0x7f) && ReadThread(thread, 0x7f);
}

bool TestRoundTrip(Kernel::Pthread first, Kernel::Pthread second, Kernel::Pthread self)
{
	Attribute snapshot;
	if (!snapshot.Init() || !ReadThread(first, 0x7f) || !ReadThread(second, 0x7f) || !ReadThread(self, 0x7f) ||
	    !ReadAttribute(snapshot.Get(), 0x7f)) return false;

	constexpr Mask self_mask = UINT64_C(0x8040201008040201);
	if (!Expect(Kernel::PthreadSetaffinity(self, self_mask), 0, "set caller affinity")) return false;
	auto round_trip = [&](Mask mask) {
		return Expect(Kernel::PthreadSetaffinity(first, mask), 0, "set first target") &&
		       Expect(Kernel::PthreadSetaffinity(second, ~mask), 0, "set second target") &&
		       ReadThread(first, mask) && ReadThread(second, ~mask) && ReadThread(self, self_mask) &&
		       Expect(Kernel::PthreadAttrGet(first, snapshot.Get()), 0, "copy first target attributes") &&
		       ReadAttribute(snapshot.Get(), mask) &&
		       Expect(Kernel::PthreadAttrGet(second, snapshot.Get()), 0, "copy second target attributes") &&
		       ReadAttribute(snapshot.Get(), ~mask);
	};
	for (Mask mask: {Mask(0), ~Mask(0), UINT64_C(0x0123456789abcdef), UINT64_C(0xfedcba9876543210)})
	{
		if (!round_trip(mask)) return false;
	}
	for (unsigned bit = 0; bit < 64; ++bit)
	{
		if (!round_trip(Mask(1) << bit)) return false;
	}

	constexpr Mask original = UINT64_C(0x1234abcd5678ef90);
	constexpr Mask changed = UINT64_C(0x9f08e7d6c5b4a321);
	constexpr Mask local = UINT64_C(0x1020304050607080);
	return Expect(Kernel::PthreadSetaffinity(first, original), 0, "set snapshot source") &&
	       Expect(Kernel::PthreadAttrGet(first, snapshot.Get()), 0, "copy snapshot") &&
	       Expect(Kernel::PthreadSetaffinity(first, changed), 0, "change snapshot source") &&
	       ReadAttribute(snapshot.Get(), original) &&
	       Expect(Kernel::PthreadAttrSetaffinity(snapshot.Get(), local), 0, "change independent snapshot") &&
	       ReadAttribute(snapshot.Get(), local) && ReadThread(first, changed) && ReadThread(self, self_mask);
}

bool TestConcurrentAccess(Kernel::Pthread first, Kernel::Pthread second)
{
	constexpr std::array<Mask, 2> first_masks {UINT64_C(0x00000000ffffffff), UINT64_C(0xffffffff00000000)};
	constexpr std::array<Mask, 2> second_masks {UINT64_C(0x55555555aaaaaaaa), UINT64_C(0xaaaaaaaa55555555)};
	constexpr std::array<Mask, 2> attribute_masks {UINT64_C(0x0123456789abcdef), UINT64_C(0xfedcba9876543210)};
	Attribute shared;
	Attribute snapshot;
	if (!shared.Init() || !snapshot.Init() ||
	    !Expect(Kernel::PthreadSetaffinity(first, first_masks[0]), 0, "seed first target") ||
	    !Expect(Kernel::PthreadSetaffinity(second, second_masks[0]), 0, "seed second target") ||
	    !Expect(Kernel::PthreadAttrSetaffinity(shared.Get(), attribute_masks[0]), 0, "seed shared attribute")) return false;

	std::atomic<bool> stop {false};
	std::atomic<unsigned> writes {0};
	std::atomic<bool> writer_failed {false};
	std::thread writer([&] {
		unsigned count = 0;
		while (!stop.load(std::memory_order_relaxed))
		{
			const unsigned index = count++ & 1u;
			if (Kernel::PthreadSetaffinity(first, first_masks[index]) != 0 ||
			    Kernel::PthreadSetaffinity(second, second_masks[index]) != 0 ||
			    Kernel::PthreadAttrSetaffinity(shared.Get(), attribute_masks[index]) != 0)
			{
				writer_failed.store(true, std::memory_order_release);
				break;
			}
			writes.store(count, std::memory_order_release);
			if ((count & 31u) == 0) std::this_thread::yield();
		}
	});
	while (writes.load(std::memory_order_acquire) < 64 && !writer_failed.load(std::memory_order_acquire))
	{
		std::this_thread::yield();
	}
	const unsigned before = writes.load(std::memory_order_acquire);
	auto one_of = [](const GuardedMask& output, const std::array<Mask, 2>& masks) {
		return output.Intact() && Check(output.value == masks[0] || output.value == masks[1],
		                               "concurrent read observed a torn mask or another target's affinity");
	};
	bool reader_ok = !writer_failed.load(std::memory_order_acquire);
	for (unsigned iteration = 0; iteration < 2048 && reader_ok; ++iteration)
	{
		for (unsigned target = 0; target < 2 && reader_ok; ++target)
		{
			const auto thread = target == 0 ? first : second;
			const auto& masks = target == 0 ? first_masks : second_masks;
			GuardedMask direct;
			GuardedMask copied;
			reader_ok = Expect(Kernel::PthreadGetaffinity(thread, &direct.value), 0, "concurrent thread get") &&
			            one_of(direct, masks) &&
			            Expect(Kernel::PthreadAttrGet(thread, snapshot.Get()), 0, "concurrent attribute copy") &&
			            Expect(Kernel::PthreadAttrGetaffinity(snapshot.Get(), &copied.value), 0, "concurrent snapshot get") &&
			            one_of(copied, masks);
		}
		GuardedMask direct;
		reader_ok = reader_ok && Expect(Kernel::PthreadAttrGetaffinity(shared.Get(), &direct.value), 0, "concurrent attribute get") &&
		            one_of(direct, attribute_masks);
		if ((iteration & 31u) == 0) std::this_thread::yield();
	}
	const unsigned after = writes.load(std::memory_order_acquire);
	stop.store(true, std::memory_order_relaxed);
	writer.join();
	return Check(!writer_failed.load(std::memory_order_acquire), "concurrent affinity writer failed") && reader_ok &&
	       Check(after > before, "writer made no progress during concurrent reads");
}

bool TestExports(Kernel::Pthread target)
{
	Lumen::Loader::SymbolDatabase symbols;
	if (!Check(Lumen::Libs::Init(U"libkernel_1", &symbols), "libkernel selection failed")) return false;
	Lumen::Loader::SymbolResolve exact {};
	exact.name = "rcrVFJsQWRY";
	exact.library = "libkernel";
	exact.library_version = 1;
	exact.module = "libkernel";
	exact.module_version_major = 1;
	exact.module_version_minor = 1;
	exact.type = Lumen::Loader::SymbolType::Func;
	const auto* found = symbols.Find(exact);
	if (!Check(found != nullptr && found->vaddr == reinterpret_cast<uint64_t>(&Kernel::PthreadGetaffinity),
	           "getaffinity export has the wrong binding")) return false;
	const auto exported = reinterpret_cast<decltype(&Kernel::PthreadGetaffinity)>(found->vaddr);
	GuardedMask output;
	constexpr Mask mask = UINT64_C(0xfedcba9876543210);
	if (!Expect(Kernel::PthreadSetaffinity(target, mask), 0, "seed exported getter") ||
	    !Expect(exported(target, &output.value), 0, "exported getter") || !output.Intact() ||
	    !Check(output.value == mask, "exported getter lost mask bits")) return false;

	for (int version: {0, 2})
	{
		auto query = exact;
		query.library_version = version;
		if (!Check(symbols.Find(query) == nullptr, "unsupported library version resolved")) return false;
		query = exact;
		query.module_version_major = version;
		if (!Check(symbols.Find(query) == nullptr, "unsupported module major resolved")) return false;
		query = exact;
		query.module_version_minor = version;
		if (!Check(symbols.Find(query) == nullptr, "unsupported module minor resolved")) return false;
	}
	auto query = exact;
	query.library = "original-affinity-test-library";
	if (!Check(symbols.Find(query) == nullptr, "getter resolved in another library")) return false;
	query = exact;
	query.module = "original-affinity-test-module";
	if (!Check(symbols.Find(query) == nullptr, "getter resolved in another module")) return false;
	query = exact;
	query.type = Lumen::Loader::SymbolType::Object;
	if (!Check(symbols.Find(query) == nullptr, "getter resolved as an object")) return false;
	struct Export { const char* nid; uint64_t address; };
	const std::array<Export, 4> existing {{
	    {"bt3CTBKmGyI", reinterpret_cast<uint64_t>(&Kernel::PthreadSetaffinity)},
	    {"x1X76arYMxU", reinterpret_cast<uint64_t>(&Kernel::PthreadAttrGet)},
	    {"8+s5BzZjxSg", reinterpret_cast<uint64_t>(&Kernel::PthreadAttrGetaffinity)},
	    {"3qxgM4ezETA", reinterpret_cast<uint64_t>(&Kernel::PthreadAttrSetaffinity)},
	}};
	for (const auto& expected: existing)
	{
		query = exact;
		query.name = expected.nid;
		const auto* symbol = symbols.Find(query);
		if (!Check(symbol != nullptr && symbol->vaddr == expected.address, "existing affinity export changed")) return false;
	}
	return true;
}

bool TestApplicationCpuMode()
{
	using QueryFunction = int32_t(LUMEN_SYSV_ABI*)();
	static_assert(std::is_same_v<decltype(&Kernel::KernelGetCpumode), QueryFunction>);
	constexpr std::array<int32_t, 4> expected {0, 0, 5, 2};
	constexpr uint32_t mode_bits = 3U << 15U;
	for (uint32_t combination = 0; combination < expected.size(); ++combination)
	{
		for (uint32_t other: {0U, 0x80000000U, 0x4001U, ~mode_bits})
		{
			const uint32_t attributes = (combination << 15U) | (other & ~mode_bits);
			if (!Check(Kernel::AppCpuModeFromAttributes(attributes) == expected[combination],
			           "application CPU mode mapping depends on unrelated ATTRIBUTE bits")) return false;
		}
	}
	Lumen::Loader::SymbolDatabase symbols;
	if (!Check(Lumen::Libs::Init(U"libkernel_1", &symbols), "CPU mode library initialization failed")) return false;
	Lumen::Loader::SymbolResolve exact {};
	exact.name = "VOx8NGmHXTs";
	exact.library = "libkernel";
	exact.library_version = 1;
	exact.module = "libkernel";
	exact.module_version_major = 1;
	exact.module_version_minor = 1;
	exact.type = Lumen::Loader::SymbolType::Func;
	const auto* symbol = symbols.Find(exact);
	if (!Check(symbol != nullptr && symbol->vaddr == reinterpret_cast<uint64_t>(&Kernel::KernelGetCpumode),
	           "application CPU mode NID has the wrong binding")) return false;
	const auto* metadata = symbols.HostCallScalarMetadataFor(symbol->vaddr);
	if (!Check(metadata != nullptr && metadata->arity == 0 && metadata->invoker != nullptr &&
	           metadata->result == Lumen::Loader::HostCallScalarType::Gp,
	           "application CPU mode does not use the zero-argument scalar ABI")) return false;
	for (unsigned mismatch = 0; mismatch < 6; ++mismatch)
	{
		auto wrong = exact;
		switch (mismatch)
		{
			case 0: wrong.library = "libScePosix"; break;
			case 1: wrong.module = "unrelated_module"; break;
			case 2: wrong.library_version = 2; break;
			case 3: wrong.module_version_major = 2; break;
			case 4: wrong.module_version_minor = 0; break;
			case 5: wrong.type = Lumen::Loader::SymbolType::Object; break;
		}
		if (!Check(symbols.Find(wrong) == nullptr, "application CPU mode resolved with an unsupported identity")) return false;
	}
	if (!Check(Lumen::Loader::SystemContentResetForNextSession(), "CPU mode metadata reset failed")) return false;
	Lumen::Cpu::X64State state {};
	for (auto& value: state.gpr) value = UINT64_MAX;
	constexpr int errno_sentinel = 193;
	*Lumen::Libs::Posix::GetErrorAddr() = errno_sentinel;
	const bool invoked = Lumen::Loader::InvokeHostCall(&symbols, symbol->vaddr, &state);
	Attribute attr;
	return Check(invoked && state.gpr[0] == 0 && Kernel::KernelGetCpumode() == 0 &&
	             *Lumen::Libs::Posix::GetErrorAddr() == errno_sentinel &&
	             symbols.FindByStdName(U"sceKernelGetCpumode") == nullptr,
	             "missing CPU mode did not use base mode or changed errno") &&
	       attr.Init() && ReadAttribute(attr.Get(), 0x7f);
}

bool TestAttributeSchedulingQuery()
{
	using QueryFunction = int(LUMEN_SYSV_ABI*)(const Kernel::PthreadAttr*, Kernel::KernelSchedParam*);
	static_assert(std::is_same_v<decltype(&Kernel::PthreadAttrGetschedparam), QueryFunction>);
	static_assert(sizeof(Kernel::KernelSchedParam) == 4 && offsetof(Kernel::KernelSchedParam, sched_priority) == 0);
	Attribute attr;
	if (!attr.Init()) return false;
	const Kernel::KernelSchedParam requested {650};
	if (!Expect(Kernel::PthreadAttrSetschedparam(attr.Get(), &requested), 0, "nondefault attribute priority")) return false;
	Lumen::Loader::SymbolDatabase symbols;
	if (!Check(Lumen::Libs::Init(U"libkernel_1", &symbols), "attribute scheduling library initialization failed")) return false;
	Lumen::Loader::SymbolResolve exact {};
	exact.name = "FXPWHNk8Of0";
	exact.library = "libkernel";
	exact.library_version = 1;
	exact.module = "libkernel";
	exact.module_version_major = 1;
	exact.module_version_minor = 1;
	exact.type = Lumen::Loader::SymbolType::Func;
	const auto* symbol = symbols.Find(exact);
	if (!Check(symbol != nullptr && symbol->vaddr == reinterpret_cast<uint64_t>(&Kernel::PthreadAttrGetschedparam),
	           "attribute scheduling NID has the wrong binding")) return false;
	const auto* metadata = symbols.HostCallScalarMetadataFor(symbol->vaddr);
	if (!Check(metadata != nullptr && metadata->arity == 2 && metadata->invoker != nullptr &&
	           metadata->arguments[0] == Lumen::Loader::HostCallScalarType::Gp &&
	           metadata->arguments[1] == Lumen::Loader::HostCallScalarType::Gp &&
	           metadata->result == Lumen::Loader::HostCallScalarType::Gp,
	           "attribute scheduling scalar ABI differs")) return false;
	for (unsigned mismatch = 0; mismatch < 6; ++mismatch)
	{
		auto wrong = exact;
		switch (mismatch)
		{
			case 0: wrong.library = "libScePosix"; break;
			case 1: wrong.module = "unrelated_module"; break;
			case 2: wrong.library_version = 2; break;
			case 3: wrong.module_version_major = 2; break;
			case 4: wrong.module_version_minor = 0; break;
			case 5: wrong.type = Lumen::Loader::SymbolType::Object; break;
		}
		if (!Check(symbols.Find(wrong) == nullptr, "attribute scheduling resolved with an unsupported identity")) return false;
	}
	const auto invoke = [&](const Kernel::PthreadAttr* input, Kernel::KernelSchedParam* output) {
		Lumen::Cpu::X64State state {};
		state.gpr[7] = reinterpret_cast<uint64_t>(input);
		state.gpr[6] = reinterpret_cast<uint64_t>(output);
		if (!Lumen::Loader::InvokeHostCall(&symbols, symbol->vaddr, &state)) return INT32_MIN;
		return static_cast<int32_t>(static_cast<uint32_t>(state.gpr[0]));
	};
	struct Output { uint32_t before; Kernel::KernelSchedParam value; uint32_t after; };
	static_assert(sizeof(Output) == 12 && offsetof(Output, value) == 4 && offsetof(Output, after) == 8);
	Output output {0xa5a5a5a5U, {-1}, 0x5a5a5a5aU};
	Kernel::PthreadAttr empty = nullptr;
	constexpr int errno_sentinel = 193;
	*Lumen::Libs::Posix::GetErrorAddr() = errno_sentinel;
	if (!Check(invoke(attr.Get(), &output.value) == 0 && output.value.sched_priority == 650 &&
	           output.before == 0xa5a5a5a5U && output.after == 0x5a5a5a5aU &&
	           invoke(nullptr, &output.value) == Kernel::KERNEL_ERROR_EINVAL &&
	           invoke(&empty, &output.value) == Kernel::KERNEL_ERROR_EINVAL &&
	           invoke(attr.Get(), nullptr) == Kernel::KERNEL_ERROR_EINVAL && output.value.sched_priority == 650 &&
	           *Lumen::Libs::Posix::GetErrorAddr() == errno_sentinel &&
	           symbols.FindByStdName(U"scePthreadAttrGetschedparam") == nullptr,
	           "attribute scheduling bridge changed priority, output canaries or error semantics")) return false;
#if defined(__aarch64__)
	namespace Vm = Lumen::Core::VirtualMemory;
	constexpr uint64_t page = 0x4000;
	const uint64_t allocation = Vm::Alloc(0, 3 * page, Vm::Mode::ReadWrite);
	if (!Check(allocation != 0, "attribute scheduling protected fixture allocation failed")) return false;
	auto* bytes = reinterpret_cast<uint8_t*>(allocation);
	std::memset(bytes, 0x6d, 3 * page);
	auto* slot = reinterpret_cast<Kernel::PthreadAttr*>(bytes + page + 16);
	std::memcpy(slot, attr.Get(), sizeof(*slot));
	std::array<uint8_t, 3 * page> before {};
	std::memcpy(before.data(), bytes, before.size());
	bool protected_ok = Vm::Protect(allocation + page, page, Vm::Mode::Read);
	protected_ok = protected_ok && invoke(slot, &output.value) == 0 && output.value.sched_priority == 650 &&
	    invoke(attr.Get(), reinterpret_cast<Kernel::KernelSchedParam*>(bytes + page + 32)) == Kernel::KERNEL_ERROR_EFAULT;
	protected_ok = protected_ok && Vm::Protect(allocation + page, page, Vm::Mode::NoAccess);
	protected_ok = protected_ok && invoke(slot, &output.value) == Kernel::KERNEL_ERROR_EFAULT &&
	    invoke(reinterpret_cast<const Kernel::PthreadAttr*>(bytes + page - 4), &output.value) == Kernel::KERNEL_ERROR_EFAULT &&
	    invoke(attr.Get(), reinterpret_cast<Kernel::KernelSchedParam*>(bytes + page - 2)) == Kernel::KERNEL_ERROR_EFAULT;
	const bool restored = Vm::Protect(allocation + page, page, Vm::Mode::ReadWrite);
	const bool unchanged = restored && std::memcmp(before.data(), bytes, before.size()) == 0 &&
	    output.before == 0xa5a5a5a5U && output.after == 0x5a5a5a5aU && output.value.sched_priority == 650;
	const bool freed = Vm::Free(allocation);
	if (!Check(protected_ok && restored && unchanged && freed,
	           "attribute scheduling protected slot/output validation differs")) return false;
#endif
	return true;
}

} // namespace

int main()
{
	Watchdog watchdog;
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	Lumen::Core::VirtualMemory::Init();
	Kernel::PthreadSubsystem::Instance()->Init(nullptr);
	Kernel::PthreadInitSelfForMainThread();
	LiveThread first;
	LiveThread second;
	const auto self = Kernel::PthreadSelf();
	if (!Check(first.Get() != second.Get() && first.Get() != self && second.Get() != self, "target handles are not distinct") ||
	    !TestErrors(first.Get()) || !TestRoundTrip(first.Get(), second.Get(), self) ||
	    !TestConcurrentAccess(first.Get(), second.Get()) || !TestExports(second.Get()) ||
	    !TestApplicationCpuMode() || !TestAttributeSchedulingQuery() ||
	    !Check(Kernel::PthreadAttributeHostOutputContractTest(), "private attribute output contract")) return 1;
	std::puts("PthreadAffinityTest: PASS");
	return 0;
}
