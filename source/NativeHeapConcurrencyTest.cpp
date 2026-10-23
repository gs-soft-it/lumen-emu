#include "Lumen/Core/MemoryAlloc.h"
#include "Lumen/Core/MemoryInitGate.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace Lumen::Core {
namespace {

constexpr const char* SELECTOR = "LUMEN_NATIVE_HEAP_CONCURRENCY";

bool SetSelector(const char* value)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
	return _putenv_s(SELECTOR, value != nullptr ? value : "") == 0;
#else
	return value != nullptr ? setenv(SELECTOR, value, 1) == 0 : unsetenv(SELECTOR) == 0;
#endif
}

struct RestoreSelector
{
	RestoreSelector()
	{
		const char* value = std::getenv(SELECTOR);
		present = value != nullptr;
		if (present) original = value;
	}
	~RestoreSelector()
	{
		SetSelector(present ? original.c_str() : nullptr);
		mem_configure_native_heap_concurrency();
	}
	bool present = false;
	std::string original;
};

bool ColdInitializationGate()
{
	MemoryDetail::InitGate gate;
	std::atomic_uint arrived {0};
	std::atomic_uint initialized {0};
	std::atomic_uint recursive_callbacks {0};
	std::atomic_bool okay {true};
	unsigned published_value = 0;
	std::array<std::thread, 8> workers;
	for (auto& worker: workers)
	{
		worker = std::thread([&] {
			bool owner = false; // This stack belongs only to this worker and its recursive callback.
			arrived.fetch_add(1, std::memory_order_release);
			while (arrived.load(std::memory_order_acquire) != workers.size()) std::this_thread::yield();
			gate.Ensure(owner, [&] {
				initialized.fetch_add(1, std::memory_order_relaxed);
				published_value = 7;
				gate.Ensure(owner, [&] { recursive_callbacks.fetch_add(1, std::memory_order_relaxed); });
				if (!owner || gate.Ready() || published_value != 7) okay.store(false, std::memory_order_relaxed);
				published_value = 42;
			});
			if (owner || !gate.Ready() || published_value != 42) okay.store(false, std::memory_order_relaxed);
		});
	}
	for (auto& worker: workers) worker.join();
	return okay.load() && initialized.load() == 1 && recursive_callbacks.load() == 0 && gate.Ready();
}

bool CheckBytes(const void* data, size_t size, unsigned char value)
{
	const auto* bytes = static_cast<const unsigned char*>(data);
	for (size_t i = 0; i < size; ++i) if (bytes[i] != value) return false;
	return true;
}

bool AllocationContract()
{
	mem_free(nullptr);
	auto* ptr = static_cast<unsigned char*>(mem_realloc(nullptr, 64));
	if (ptr == nullptr || reinterpret_cast<uintptr_t>(ptr) % alignof(std::max_align_t) != 0) return false;
	std::memset(ptr, 0xa7, 64);
	ptr = static_cast<unsigned char*>(mem_realloc(ptr, 257));
	bool okay = ptr != nullptr && CheckBytes(ptr, 64, 0xa7);
	std::memset(ptr, 0x5c, 257);
	ptr = static_cast<unsigned char*>(mem_realloc(ptr, 17));
	okay = okay && ptr != nullptr && CheckBytes(ptr, 17, 0x5c) && mem_check(ptr);
	mem_free(ptr);
	return okay;
}

bool CrossThreadPointers()
{
	constexpr size_t WORKERS = 8;
	constexpr size_t SLOTS = 16;
	std::array<void*, WORKERS * SLOTS> pointers {};
	std::array<std::thread, WORKERS> workers;
	std::atomic_bool okay {true};
	if (!SetSelector("0")) return false;
	mem_configure_native_heap_concurrency();
	for (size_t worker = 0; worker < WORKERS; ++worker)
	{
		workers[worker] = std::thread([&, worker] {
			for (size_t slot = 0; slot < SLOTS; ++slot)
			{
				const size_t index = worker * SLOTS + slot;
				pointers[index] = mem_alloc(32 + index);
				std::memset(pointers[index], static_cast<unsigned char>(index), 32 + index);
			}
		});
	}
	for (auto& worker: workers) worker.join();
	if (!SetSelector("1")) return false;
	mem_configure_native_heap_concurrency();
	for (size_t worker = 0; worker < WORKERS; ++worker)
	{
		workers[worker] = std::thread([&, worker] {
			for (size_t slot = 0; slot < SLOTS; ++slot)
			{
				const size_t index = ((worker + 1) % WORKERS) * SLOTS + slot;
				void* ptr = mem_realloc(pointers[index], 96 + index);
				if (!CheckBytes(ptr, 32 + index, static_cast<unsigned char>(index))) okay.store(false);
				mem_free(ptr);
			}
		});
	}
	for (auto& worker: workers) worker.join();
	return okay.load();
}

bool ConcurrentSelectorChanges()
{
	constexpr unsigned WORKERS = 8;
	constexpr unsigned ROUNDS = 16;
	std::atomic_uint step {0};
	std::atomic_uint finished {0};
	std::atomic_bool okay {true};
	std::array<std::thread, WORKERS> workers;
	for (auto& worker: workers)
	{
		worker = std::thread([&] {
			for (unsigned round = 0; round < ROUNDS; ++round)
			{
				while (step.load(std::memory_order_acquire) < round + 1) std::this_thread::yield();
				if (!AllocationContract()) okay.store(false);
				finished.fetch_add(1, std::memory_order_release);
			}
		});
	}
	for (unsigned round = 0; round < ROUNDS; ++round)
	{
		step.store(round + 1, std::memory_order_release);
		if (!SetSelector((round & 1u) == 0 ? "0" : "1")) okay.store(false);
		mem_configure_native_heap_concurrency();
		while (finished.load(std::memory_order_acquire) < (round + 1) * WORKERS) std::this_thread::yield();
	}
	for (auto& worker: workers) worker.join();
	return okay.load();
}

} // namespace

bool NativeHeapConcurrencyContractTest()
{
	RestoreSelector restore;
	const bool supported = mem_native_heap_concurrency_supported();
	bool okay = ColdInitializationGate();
	constexpr std::array<const char*, 7> values {nullptr, "0", "1", "true", "01", "1x", "-1"};
	for (const char* value: values)
	{
		if (!SetSelector(value)) return false;
		mem_configure_native_heap_concurrency();
		const bool requested = value != nullptr && std::strcmp(value, "1") == 0;
		okay = okay && mem_native_heap_concurrency_enabled() == (supported && requested);
		okay = AllocationContract() && okay;
	}
	// A pointer's allocation family stays identical across an explicit rollback.
	if (!SetSelector("1")) return false;
	mem_configure_native_heap_concurrency();
	void* ptr = mem_alloc(73);
	std::memset(ptr, 0x8b, 73);
	if (!SetSelector("0")) { mem_free(ptr); return false; }
	mem_configure_native_heap_concurrency();
	ptr = mem_realloc(ptr, 109);
	okay = CheckBytes(ptr, 73, 0x8b) && okay;
	mem_free(ptr);
	okay = CrossThreadPointers() && okay;
	okay = ConcurrentSelectorChanges() && okay;
	std::fprintf(stderr, "[NATIVE_HEAP_CONCURRENCY_TEST] result=%s mechanism=%s cold_gate=isolated actual_allocator=initialized\n",
	             okay ? "PASS" : "FAIL", supported ? "qualified-libc" : "legacy-fallback-only");
	return okay;
}

} // namespace Lumen::Core
