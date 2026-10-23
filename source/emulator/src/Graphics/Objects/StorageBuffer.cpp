#include "Emulator/Graphics/Objects/StorageBuffer.h"

#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/Threads.h"
#include "Lumen/Core/Vector.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/GraphicsRun.h"
#include "Emulator/Graphics/GuestWriteDestination.h"
#include "Emulator/Graphics/HtileCpuObservation.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Profiler.h"

#include <array>
#include <cstring>
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
#include <thread>
#endif

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics {

static Core::Mutex                  g_storage_pool_mutex;
static Vector<StorageVulkanBuffer*> g_storage_pool;
static uint64_t                     g_storage_pool_bytes = 0;
static constexpr uint64_t           STORAGE_POOL_MAX       = 512;
static constexpr uint64_t           STORAGE_POOL_BYTES_MAX = 256ull * 1024ull * 1024ull;

struct StoragePoolTimingStats
{
	uint64_t count  = 0;
	uint64_t hits   = 0;
	uint64_t misses = 0;
};

static void RecordStoragePool(bool hit)
{
	if (!Config::DiagnosticsEnabled())
	{
		return;
	}

	static thread_local StoragePoolTimingStats stats;
	stats.count++;
	stats.hits += (hit ? 1u : 0u);
	stats.misses += (hit ? 0u : 1u);
	if (stats.count == 5000)
	{
		Core::LockGuard lock(g_storage_pool_mutex);
		dbg_printf("[PERF] storage_pool calls=%" PRIu64 " hits=%" PRIu64 " misses=%" PRIu64 " cached=%" PRIu64
		           " bytes=%.3f MiB\n",
		           stats.count, stats.hits, stats.misses, static_cast<uint64_t>(g_storage_pool.Size()),
		           static_cast<double>(g_storage_pool_bytes) / (1024.0 * 1024.0));
		stats = {};
	}
}

static StorageVulkanBuffer* StoragePoolAcquire(uint64_t size)
{
	Core::LockGuard lock(g_storage_pool_mutex);
	for (int i = static_cast<int>(g_storage_pool.Size()) - 1; i >= 0; i--)
	{
		if (g_storage_pool[i]->size == size)
		{
			auto* buffer = g_storage_pool[i];
			g_storage_pool.RemoveAt(i);
			EXIT_IF(g_storage_pool_bytes < buffer->size);
			g_storage_pool_bytes -= buffer->size;
			buffer->cp = nullptr;
			return buffer;
		}
	}
	return nullptr;
}

static bool StoragePoolRelease(StorageVulkanBuffer* buffer)
{
	Core::LockGuard lock(g_storage_pool_mutex);
	if (g_storage_pool.Size() >= STORAGE_POOL_MAX || buffer->size > STORAGE_POOL_BYTES_MAX - g_storage_pool_bytes)
	{
		return false;
	}
	g_storage_pool.Add(buffer);
	g_storage_pool_bytes += buffer->size;
	return true;
}

static void update_func(GraphicContext* ctx, const uint64_t* /*params*/, void* obj, const uint64_t* vaddr, const uint64_t* size,
                        int vaddr_num)
{
	LUMEN_PROFILER_BLOCK("StorageBufferGpuObject::update_func");

	EXIT_IF(ctx == nullptr);
	EXIT_IF(obj == nullptr);
	EXIT_IF(vaddr == nullptr || size == nullptr || vaddr_num != 1);

	auto* vk_obj = reinterpret_cast<StorageVulkanBuffer*>(obj);

	EXIT_IF(vk_obj->mapped_data == nullptr);
	memcpy(vk_obj->mapped_data, reinterpret_cast<void*>(*vaddr), *size);
}

static StorageVulkanBuffer* create_buffer(GraphicContext* ctx, uint64_t size)
{
	auto* vk_obj = StoragePoolAcquire(size);
	RecordStoragePool(vk_obj != nullptr);
	if (vk_obj == nullptr)
	{
		vk_obj = new StorageVulkanBuffer;

		vk_obj->usage           = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		vk_obj->memory.property = static_cast<uint32_t>(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
		                          VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
		vk_obj->buffer = nullptr;
		vk_obj->size   = size;

		VulkanCreateBuffer(ctx, size, vk_obj);
		EXIT_NOT_IMPLEMENTED(vk_obj->buffer == nullptr);

		VulkanMapMemory(ctx, &vk_obj->memory, &vk_obj->mapped_data);
		EXIT_NOT_IMPLEMENTED(vk_obj->mapped_data == nullptr);
	}

	return vk_obj;
}

static void release_buffer(GraphicContext* ctx, StorageVulkanBuffer* vk_obj)
{
	EXIT_IF(vk_obj == nullptr);
	EXIT_IF(vk_obj->buffer == nullptr);
	EXIT_IF(ctx == nullptr);

	if (!StoragePoolRelease(vk_obj))
	{
		EXIT_IF(vk_obj->mapped_data == nullptr);
		VulkanUnmapMemory(ctx, &vk_obj->memory);
		vk_obj->mapped_data = nullptr;
		VulkanDeleteBuffer(ctx, vk_obj);
		delete vk_obj;
	}
}

static void delete_retired_buffer(GraphicContext* ctx, void* obj)
{
	release_buffer(ctx, static_cast<StorageVulkanBuffer*>(obj));
}

void StorageBufferReleaseCompleted(GraphicContext* ctx, StorageVulkanBuffer* buffer)
{
	// The tracker has already proved completion and relinquished ownership.
	// Reusing the ordinary deleter here would wait while renderer/CP scopes
	// remain held, and would infer ownership from only buffer->cp.
	release_buffer(ctx, buffer);
}

static void update_with_buffer_func(GraphicContext* ctx, CommandBuffer* buffer, const uint64_t* params, void* obj,
	                                const uint64_t* vaddr, const uint64_t* size, int vaddr_num)
{
	LUMEN_PROFILER_BLOCK("StorageBufferGpuObject::update_with_buffer_func");

	EXIT_IF(buffer == nullptr);
	EXIT_IF(obj == nullptr);
	EXIT_IF(size == nullptr);

	auto* current     = static_cast<StorageVulkanBuffer*>(obj);
	auto* replacement = create_buffer(ctx, *size);

	update_func(ctx, params, replacement, vaddr, size, vaddr_num);

	auto* retired = new StorageVulkanBuffer(*current);
	*current      = *replacement;
	delete replacement;

	buffer->Retire(retired, delete_retired_buffer);
}

static void* create_func(GraphicContext* ctx, const uint64_t* params, const uint64_t* vaddr, const uint64_t* size, int vaddr_num,
                         VulkanMemory* mem)
{
	LUMEN_PROFILER_BLOCK("StorageBufferGpuObject::Create");

	EXIT_IF(vaddr_num != 1 || size == nullptr || vaddr == nullptr || *vaddr == 0);

	EXIT_IF(mem == nullptr);
	EXIT_IF(ctx == nullptr);

	auto* vk_obj = create_buffer(ctx, *size);

	EXIT_IF(vk_obj->mapped_data == nullptr);
	update_func(ctx, params, vk_obj, vaddr, size, vaddr_num);

	return vk_obj;
}

static void delete_func(GraphicContext* ctx, void* obj, VulkanMemory* /*mem*/)
{
	LUMEN_PROFILER_BLOCK("StorageBufferGpuObject::delete_func");

	auto* vk_obj = reinterpret_cast<StorageVulkanBuffer*>(obj);

	EXIT_IF(vk_obj == nullptr);
	EXIT_IF(vk_obj->buffer == nullptr);
	EXIT_IF(ctx == nullptr);

	EXIT_IF(vk_obj->cp == nullptr);

	// All submitted commands that refer to any element of pDescriptorSets must have completed execution
	GraphicsRunCommandProcessorWait(vk_obj->cp);

	release_buffer(ctx, vk_obj);
}

static void write_back(GraphicContext* ctx, const uint64_t* /*params*/, void* obj, const uint64_t* vaddr, const uint64_t* size,
                       int vaddr_num)
{
	LUMEN_PROFILER_BLOCK("StorageBufferGpuObject::write_back");

	EXIT_IF(ctx == nullptr);
	EXIT_IF(obj == nullptr);
	EXIT_IF(vaddr == nullptr || size == nullptr || vaddr_num != 1);

	auto* vk_obj = reinterpret_cast<StorageVulkanBuffer*>(obj);

	GuestWriteDestination destination;
	EXIT_IF(!destination.Capture(*vaddr, *size));
	EXIT_IF(!StorageBufferWriteBackToGuest(vk_obj, destination));
}

bool StorageBufferWriteBackToGuest(StorageVulkanBuffer* buffer, const GuestWriteDestination& destination,
                                  GraphicsGuestLeaseResult* result)
{
	if (result != nullptr) *result = {};
	if (buffer == nullptr || buffer->mapped_data == nullptr || destination.Size() == 0 || destination.Size() > buffer->size)
	{
		if (result != nullptr) result->reason = "storage-shape";
		return false;
	}
	constexpr auto properties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	if ((buffer->memory.property & properties) != properties)
	{
		if (result != nullptr) result->reason = "storage-host-visibility";
		return false;
	}
	// The caller has already completed the producer and made this coherent
	// mapping visible. Publication does not wait, log or call into the driver.
	if (!destination.Publish(buffer->mapped_data, result)) return false;
	if (HtileCpuObservationEnabled())
	{
		// Observe only this callback's existing host-owned source. The caller
		// waits a command processor, but this callback cannot attest ownership
		// of a particular writer/fence or the current contents of guest RAM.
		static_cast<void>(GetHtileStorageWritebackHistory().Record(destination.Address(), destination.Size(),
		                                                         buffer->memory.unique_id, buffer->mapped_data));
	}
	return true;
}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
static bool GraphicsGuestLeaseRetryContractTest()
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	using namespace Core::VirtualMemory;
	Init();
	const uint64_t address = Alloc(0, 65536, Mode::ReadWrite);
	if (address == 0) return false;
	struct Owner { uint64_t address; ~Owner() { Free(address); } } owner {address};
	GraphicsGuestLeaseResult result;
	GuestDataLease lease;
	uint32_t calls = 0;
	const auto count_acquire = [&](uint64_t begin, uint64_t bytes, Mode mode, GuestDataLease* output) {
		++calls;
		return TryAcquireGuestDataLease(begin, bytes, mode, output, false);
	};
	const auto borrow = [&](uint64_t begin, uint64_t bytes, Mode mode, uint64_t generation,
	                        GuestDataLease* output = nullptr) {
		return TryAcquireGraphicsGuestLease(begin, bytes, mode, generation, output == nullptr ? &lease : output,
		                                   &result, count_acquire);
	};
	const uint64_t generation = GetMutationGeneration();
	if (TryAcquireGraphicsGuestLease(address, 32, Mode::Write, generation, nullptr, &result, count_acquire) ||
	    calls != 0 || result.attempts != 0 || std::strcmp(result.reason, "lease-state") != 0) return false;
	for (const auto scope: {std::array<uint64_t, 2> {0, 32}, {address, 0}, {UINT64_MAX, 32}})
		if (borrow(scope[0], scope[1], Mode::Write, generation) || calls != 0 || result.attempts != 0) return false;
	if (borrow(address, 32, Mode::Execute, generation) || calls != 0 ||
	    borrow(address, 32, Mode::Write, UINT64_MAX) || calls != 0 ||
	    borrow(address, 32, Mode::Write, generation + 1) || calls != 0) return false;
	if (!borrow(address, 32, Mode::Write, generation) || calls != 1 || result.attempts != 1) return false;
	// An already active output is rejected without releasing the caller's lease.
	if (borrow(address, 32, Mode::Write, generation) || calls != 1 || !lease.IsActive() ||
	    std::strcmp(result.reason, "lease-state") != 0) return false;
	ReleaseGuestDataLease(&lease);
	GuestDataLease held;
	if (!TryAcquireGuestDataLease(address, 32, Mode::Read, &held, false) ||
	    !borrow(address, 32, Mode::Write, generation) || result.attempts != 1) return false;
	ReleaseGuestDataLease(&lease);
	calls = 0;
	const auto validated_denial = [&](uint64_t begin, uint64_t bytes, Mode mode, GuestDataLease* output) {
		++calls;
		if (!TryAcquireGuestDataLease(begin, bytes, mode, output, false)) return false;
		// A deterministic acquisition outcome after real validation. The helper
		// must release its own lease and must leave the outer lease intact.
		return false;
	};
	if (TryAcquireGraphicsGuestLease(address, 32, Mode::Write, generation, &lease, &result, validated_denial) ||
	    calls != 1 || result.attempts != 1 || std::strcmp(result.reason, "retry-memory-lease-held") != 0 ||
	    lease.IsActive() || !held.IsActive()) return false;
	ReleaseGuestDataLease(&held);
	// Invalid mapping/protection scope may never become an admission, with or
	// without the optional immutable snapshot.
	for (const auto mode: {Mode::Read, Mode::ExecuteReadWrite})
	{
		if (!Protect(address, 65536, mode)) return false;
		calls = 0;
		if (borrow(address, 32, Mode::Write, GetMutationGeneration()) || lease.IsActive() ||
		    calls == 0 || calls > 16 || result.attempts != calls || !Protect(address, 65536, Mode::ReadWrite)) return false;
	}
	std::array<uint8_t, 64> native {};
	calls = 0;
	if (borrow(reinterpret_cast<uint64_t>(native.data()), native.size(), Mode::Write, GetMutationGeneration()) ||
	    lease.IsActive() || calls != 1 || std::strcmp(result.reason, "destination-unowned") != 0) return false;
	calls = 0;
	const auto wrong_scope = [&](uint64_t begin, uint64_t bytes, Mode mode, GuestDataLease* output) {
		++calls;
		return TryAcquireGuestDataLease(begin + 16, bytes, mode, output, false);
	};
	if (TryAcquireGraphicsGuestLease(address, 32, Mode::Write, GetMutationGeneration(), &lease, &result, wrong_scope) ||
	    calls != 1 || lease.IsActive() || std::strcmp(result.reason, "lease-shape") != 0) return false;
	// Even a successful acquisition returning a newer real lease cannot
	// retarget the original ticket. No mutation occurs while a lease is held.
	calls = 0;
	const uint64_t original_generation = GetMutationGeneration();
	bool changed_generation_valid = true;
	const auto changed_generation = [&](uint64_t begin, uint64_t bytes, Mode mode, GuestDataLease* output) {
		++calls;
		if (!TryAcquireGuestDataLease(begin, bytes, mode, output, false)) { changed_generation_valid = false; return false; }
		ReleaseGuestDataLease(output);
		changed_generation_valid = Protect(address, 65536, Mode::ReadWrite) && !CurrentThreadHasMemoryLease();
		return changed_generation_valid && TryAcquireGuestDataLease(begin, bytes, mode, output, false);
	};
	if (TryAcquireGraphicsGuestLease(address, 32, Mode::Write, original_generation, &lease, &result, changed_generation) ||
	    !changed_generation_valid || calls != 1 || result.attempts != 1 || lease.IsActive() || CurrentThreadHasMemoryLease() ||
	    result.observed_generation == original_generation || std::strcmp(result.reason, "generation-changed") != 0) return false;
	// Exercise actual nonblocking gate rejection while caller CP and tracker
	// locks remain held. No sleep/yield/unlock is needed to exhaust the phase.
	Core::Mutex processor_mutex, tracker_mutex;
	{
		Core::LockGuard processor_lock(processor_mutex);
		Core::LockGuard tracker_lock(tracker_mutex);
		bool processor_available = true, tracker_available = true;
		std::thread observer([&] {
			processor_available = processor_mutex.TryLock();
			if (processor_available) processor_mutex.Unlock();
			tracker_available = tracker_mutex.TryLock();
			if (tracker_available) tracker_mutex.Unlock();
		});
		observer.join();
		if (processor_available || tracker_available) return false;
		MutationTransaction transaction;
		ReservationHandle reservation;
		if (!BeginReserveAligned(0, 65536, 65536, &reservation, &transaction)) return false;
		calls = 0;
		const bool accepted = borrow(address, 32, Mode::Write, GetMutationGeneration());
		if (!transaction.Abort() || accepted || calls != 16 || result.attempts != 16 || lease.IsActive() ||
		    std::strcmp(result.reason, "lease-acquisition-retry-exhausted") != 0) return false;
	}
	std::array<uint8_t, 256> source;
	source.fill(0x37);
	auto* guest = reinterpret_cast<uint8_t*>(address);
	for (const bool exhaust: {false, true})
	{
		std::memset(guest, 0xa5, 65536);
		GuestWriteDestination destination;
		if (!destination.Capture(address + 16, source.size())) return false;
		const uint64_t original_generation = destination.Generation();
		calls = 0;
		bool valid = true;
		const auto acquire = [&](uint64_t begin, uint64_t bytes, Mode mode, GuestDataLease* output) {
			++calls;
			valid = valid && begin == address + 16 && bytes == source.size() && mode == Mode::Write &&
			        destination.Generation() == original_generation && !CurrentThreadHasMemoryLease();
			for (uint64_t i = 0; i < 65536; ++i) valid = valid && guest[i] == 0xa5;
			if (!TryAcquireGuestDataLease(begin, bytes, mode, output, false)) { valid = false; return false; }
			return !exhaust && calls > 1;
		};
		Core::LockGuard processor_lock(processor_mutex);
		Core::LockGuard tracker_lock(tracker_mutex);
		const bool published = destination.PublishWithAcquisitionForTest(source.data(), &result, acquire);
		if (!valid || published == exhaust || calls != (exhaust ? 16u : 2u) || result.attempts != calls ||
		    CurrentThreadHasMemoryLease() || destination.Generation() != original_generation) return false;
		for (uint64_t i = 0; i < 65536; ++i)
			if (guest[i] != (!exhaust && i >= 16 && i < 272 ? 0x37 : 0xa5)) return false;
	}
	std::fprintf(stderr, "[GFX_GUEST_LEASE_RETRY_TEST] immediate-bound=16 recover=2 exhaustion=16"
	                     " real-gate-denial=16 caller-locks=retained nested-success=preserved"
	                     " held-lease-retry=reject scope/protection/execute/unowned=reject"
	                     " storage-ticket=retained copy-after-admission-only guards=preserved sleeps=0\n");
#endif
	return true;
}

bool StorageBufferGuestWriteContractTest()
{
	if (!GraphicsGuestLeaseRetryContractTest()) return false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	{
		using namespace Core::VirtualMemory;
		const uint64_t address = Alloc(0, 65536, Mode::ReadWrite);
		const uint64_t other = Alloc(0, 65536, Mode::ReadWrite);
		if (address == 0 || other == 0) { if (address != 0) Free(address); if (other != 0) Free(other); return false; }
		struct Owner { uint64_t address, other; ~Owner() { Free(address); Free(other); } } owner {address, other};
		auto* bytes = reinterpret_cast<uint8_t*>(address);
		std::memset(bytes, 0xa5, 65536);
		std::array<uint8_t, 32> source;
		source.fill(0x37);
		GuestWriteDestination destination;
		if (!destination.Capture(address + 64, source.size())) return false;
		const auto original = destination.Generation();
		if (!Protect(other, 65536, Mode::Read) || !destination.Validate() || destination.Generation() != original ||
		    GetMutationGeneration() == original) return false;
		GuestWriteDestination retained;
		if (!retained.Capture(address + 64, source.size(), 1, nullptr, original) || retained.Generation() != original) return false;
		GuestDataLease fixed;
		GraphicsGuestLeaseResult result;
		if (TryAcquireGraphicsGuestLease(address + 64, source.size(), Mode::Write, original, &fixed, &result) ||
		    fixed.IsActive() || std::strcmp(result.reason, "generation-changed") != 0) return false;
		uint32_t guards = 0;
		bool guard_valid = true;
		const auto guard = [&](const GuestDataLease& lease) {
			++guards;
			guard_valid &= CurrentThreadHasMemoryLease() && lease.HostBegin() == address + 64 && lease.ByteCount() == source.size() &&
			               destination.Generation() == original && lease.MutationGeneration() > original;
			for (uint64_t i = 0; i < 65536; ++i) guard_valid &= bytes[i] == 0xa5;
			GuestDataLease nested;
			guard_valid &= TryAcquireGraphicsGuestContinuityLease(address + 2048, 16, Mode::Read, original, &nested, nullptr);
			return guard_valid;
		};
		if (destination.PublishGuarded(source.data(), [&](const GuestDataLease& lease) { guard(lease); return false; }, &result) ||
		    !guard_valid || guards != 1 || std::strcmp(result.reason, "publication-footprint-invalid") != 0 ||
		    CurrentThreadHasMemoryLease()) return false;
		for (uint64_t i = 0; i < 65536; ++i) if (bytes[i] != 0xa5) return false;
		if (!destination.PublishGuarded(source.data(), guard, &result) || !guard_valid || guards != 2 ||
		    destination.Generation() != original || CurrentThreadHasMemoryLease()) return false;
		for (uint64_t i = 0; i < 65536; ++i) if (bytes[i] != (i >= 64 && i < 96 ? uint8_t {0x37} : uint8_t {0xa5})) return false;
		std::memset(bytes, 0xa5, 65536);
		uint32_t calls = 0;
		const auto acquire = [&](uint64_t begin, uint64_t size, Mode mode, GuestDataLease* lease) {
			++calls;
			if (calls == 1) { if (!Protect(other, 65536, Mode::ReadWrite)) guard_valid = false; return false; }
			return TryAcquireGuestDataLease(begin, size, mode, lease, false);
		};
		if (!destination.PublishWithAcquisitionForTest(source.data(), &result, acquire) || !guard_valid || calls != 2 ||
		    result.attempts != 2 || destination.Generation() != original || CurrentThreadHasMemoryLease()) return false;
		std::memset(bytes, 0xa5, 65536);
		// Same-map mutations, including revoke/restore and identical-byte ABA,
		// never renew a token that was already captured before preparation.
		if (!Protect(address, 65536, Mode::Read) || !Protect(address, 65536, Mode::ReadWrite) || destination.Validate() ||
		    destination.Publish(source.data()) || retained.Capture(address + 64, source.size(), 1, nullptr, original)) return false;
		for (uint64_t i = 0; i < 65536; ++i) if (bytes[i] != 0xa5) return false;
		if (!destination.Capture(address + 64, source.size())) return false;
		const auto before_remap = destination.Generation();
		if (!Free(address) || !AllocFixed(address, 65536, Mode::ReadWrite)) return false;
		std::memset(bytes, 0xa5, 65536);
		if (destination.Validate() || destination.Publish(source.data()) || destination.Generation() != before_remap) return false;
		for (uint64_t i = 0; i < 65536; ++i) if (bytes[i] != 0xa5) return false;
		std::fprintf(stderr, "[GFX_GUEST_WRITE_CONTINUITY_TEST] original-token=retained unrelated-map-mutation=pass"
		                     " acquire-retry-with-unrelated-change=2 fresh-capture=separate fixed-helper=unchanged"
		                     " guard=once-per-publication nested-inventory-lease=pass guard-before-store=pass"
		                     " guard-failure=no-store full-block-guards=preserved revoke/restore/remap=reject\n");
	}
#endif
	std::array<uint8_t, 256> source {};
	std::array<uint8_t, 288> fallback {};
	for (size_t i = 0; i < source.size(); ++i) source[i] = static_cast<uint8_t>(0x37u + i * 53u);
	uint8_t* guest = fallback.data();
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	using namespace Core::VirtualMemory;
	Init();
	const uint64_t allocation = Alloc(0, 65536, Mode::ReadWrite);
	if (allocation == 0) return false;
	struct Owner { uint64_t address; ~Owner() { Free(address); } } owner {allocation};
	guest = reinterpret_cast<uint8_t*>(allocation);
#endif
	std::memset(guest, 0xa5, fallback.size());
	GuestWriteDestination destination;
	if (!destination.Capture(reinterpret_cast<uint64_t>(guest + 16), source.size())) return false;
	StorageVulkanBuffer buffer;
	buffer.mapped_data = source.data();
	buffer.size = source.size();
	buffer.memory.property = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	const auto unchanged = [&] {
		for (size_t i = 0; i < fallback.size(); ++i) if (guest[i] != 0xa5) return false;
		return true;
	};
	if (StorageBufferWriteBackToGuest(nullptr, destination) || !unchanged()) return false;
	buffer.size = source.size() - 1;
	if (StorageBufferWriteBackToGuest(&buffer, destination) || !unchanged()) return false;
	buffer.size = source.size();
	buffer.mapped_data = nullptr;
	if (StorageBufferWriteBackToGuest(&buffer, destination) || !unchanged()) return false;
	buffer.mapped_data = source.data();
	buffer.memory.property = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
	if (StorageBufferWriteBackToGuest(&buffer, destination) || !unchanged()) return false;
	buffer.memory.property |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	if (StorageBufferWriteBackToGuest(&buffer, GuestWriteDestination {}) || !unchanged()) return false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	Mode old_mode;
	if (!Protect(allocation, 65536, Mode::Read, &old_mode)) return false;
	if (StorageBufferWriteBackToGuest(&buffer, destination) || !unchanged()) return false;
	if (!Protect(allocation, 65536, Mode::ReadWrite, &old_mode)) return false;
	// Returning to RW does not restore the pre-wait generation.
	if (StorageBufferWriteBackToGuest(&buffer, destination) || !unchanged()) return false;
	if (!destination.Capture(reinterpret_cast<uint64_t>(guest + 16), source.size())) return false;
#endif
	if (!StorageBufferWriteBackToGuest(&buffer, destination) ||
	    std::memcmp(guest + 16, source.data(), source.size()) != 0) return false;
	for (size_t i = 0; i < 16; ++i)
		if (guest[i] != 0xa5 || guest[272 + i] != 0xa5) return false;
	return true;
}
#endif

bool StorageBufferGpuObject::Equal(const uint64_t* other) const
{
	return params[0] == other[0] && params[1] == other[1];
}

GpuObject::create_func_t StorageBufferGpuObject::GetCreateFunc() const
{
	return create_func;
}

GpuObject::write_back_func_t StorageBufferGpuObject::GetWriteBackFunc() const
{
	return write_back;
}

GpuObject::delete_func_t StorageBufferGpuObject::GetDeleteFunc() const
{
	return delete_func;
}

GpuObject::update_func_t StorageBufferGpuObject::GetUpdateFunc() const
{
	return update_func;
}

GpuObject::update_with_buffer_func_t StorageBufferGpuObject::GetUpdateWithBufferFunc() const
{
	return update_with_buffer_func;
}

void StorageBufferSet(CommandBuffer* cmd_buffer, StorageVulkanBuffer* buffer)
{
	buffer->cp = cmd_buffer->GetParent();
}

} // namespace Lumen::Libs::Graphics

#endif // LUMEN_EMU_ENABLED
