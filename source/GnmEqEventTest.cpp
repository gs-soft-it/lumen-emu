#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"
#include "Emulator/Config.h"
#include "Emulator/Graphics/Graphics.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Kernel/EventQueue.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"

#if defined(LUMEN_FEXCORE_RUNTIME)
#include "AndroidFexCoreRuntime.h"
#endif

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>

namespace {
namespace Graphics = Lumen::Libs::Graphics;
namespace Gnm = Graphics::Gen4;
namespace Queue = Lumen::Libs::LibKernel::EventQueue;
namespace Kernel = Lumen::Libs::LibKernel;

bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "GnmEqEventTest: %s\n", message);
	return condition;
}

class EventQueue
{
public:
	EventQueue() { Queue::KernelCreateEqueue(&m_queue, "gnm-event-fixture"); }
	~EventQueue() { Close(); }
	EventQueue(const EventQueue&) = delete;
	EventQueue& operator=(const EventQueue&) = delete;
	Queue::KernelEqueue Get() const { return m_queue; }
	bool Close()
	{
		if (m_queue == nullptr) return true;
		const int result = Queue::KernelDeleteEqueue(m_queue);
		m_queue = nullptr;
		return result == 0;
	}
private:
	Queue::KernelEqueue m_queue = nullptr;
};

bool Empty(Queue::KernelEqueue queue)
{
	std::array<uint8_t, sizeof(Queue::KernelEvent)> original;
	original.fill(0xa7);
	Queue::KernelEvent output;
	std::memcpy(&output, original.data(), original.size());
	int count = -1;
	const Kernel::KernelUseconds timeout = 0;
	return Check(Queue::KernelWaitEqueue(queue, &output, 1, &count, &timeout) == Kernel::KERNEL_ERROR_ETIMEDOUT &&
	             count == 0 && std::memcmp(&output, original.data(), original.size()) == 0,
	             "empty nonblocking poll changed output, count or timeout result");
}

bool Delivered(Queue::KernelEqueue queue, uintptr_t id, uintptr_t user, uint64_t data, bool check_data = true)
{
	Queue::KernelEvent output {};
	int count = 0;
	const Kernel::KernelUseconds timeout = 0;
	return Check(Queue::KernelWaitEqueue(queue, &output, 1, &count, &timeout) == 0 && count == 1 &&
	             output.ident == id && output.filter == Queue::KERNEL_EVFILT_GRAPHICS &&
	             output.udata == reinterpret_cast<void*>(user) &&
	             (id > 6 || (output.flags == 0x20 && output.fflags == 0)) &&
	             (!check_data || static_cast<uint64_t>(output.data) == data),
	             "explicit synthetic notification lost identifier, payload or user data") && Empty(queue);
}

bool EventAccessorBindingsContract()
{
	namespace Loader = Lumen::Loader;
	static_assert(sizeof(uintptr_t) == 8 && sizeof(int) == 4 && sizeof(int16_t) == 2);
	Loader::SymbolDatabase symbols;
	if (!Check(Lumen::Libs::Init(U"libkernel_1", &symbols), "libkernel registration failed")) return false;
	const auto query = [](const char* nid) {
		Loader::SymbolResolve value {};
		value.name = nid;
		value.library = "libkernel";
		value.library_version = 1;
		value.module = "libkernel";
		value.module_version_major = 1;
		value.module_version_minor = 1;
		value.type = Loader::SymbolType::Func;
		return value;
	};
	const auto* filter = symbols.Find(query("23CPPI1tyBY"));
	const auto* id = symbols.Find(query("mJ7aghmgvfc"));
	if (!Check(filter != nullptr && filter->vaddr == reinterpret_cast<uint64_t>(&Queue::KernelGetEventFilter) &&
	           id != nullptr && id->vaddr == reinterpret_cast<uint64_t>(&Queue::KernelGetEventId),
	           "event accessor binding identity or target differs")) return false;
	symbols.SealHostCallRegistry();
	const auto* filter_metadata = symbols.HostCallScalarMetadataFor(filter->vaddr);
	const auto* id_metadata = symbols.HostCallScalarMetadataFor(id->vaddr);
	for (const auto* metadata: {filter_metadata, id_metadata})
		if (!Check(metadata != nullptr && metadata->arity == 1 && metadata->arguments[0] == Loader::HostCallScalarType::Gp &&
		           metadata->result == Loader::HostCallScalarType::Gp && metadata->invoker != nullptr,
		           "event accessor scalar ABI metadata differs")) return false;
	if (!Check(symbols.ClassifyHostCallTarget(filter->vaddr) == Loader::HostCallRegistryClassification::Host &&
	           symbols.ClassifyHostCallTarget(id->vaddr) == Loader::HostCallRegistryClassification::Host,
	           "event accessor is missing from the sealed host registry")) return false;
	const auto filter_call = reinterpret_cast<decltype(&Queue::KernelGetEventFilter)>(filter->vaddr);
	const auto id_call = reinterpret_cast<decltype(&Queue::KernelGetEventId)>(id->vaddr);
	for (int16_t filter_value: std::array<int16_t, 6>{INT16_MIN, Queue::KERNEL_EVFILT_GRAPHICS, -1, 0, 1, INT16_MAX})
		for (uintptr_t id_value: std::array<uintptr_t, 4>{0, 0x100000000ull, 0x80000000deadbeefull, UINT64_MAX})
		{
			Queue::KernelEvent event {};
			event.ident = id_value;
			event.filter = filter_value;
			event.flags = 0x7aa7;
			event.fflags = 0x93c55ca9;
			event.data = static_cast<intptr_t>(0x7123456789abcdefull);
			event.udata = &event;
			std::array<uint8_t, sizeof(event)> original;
			std::memcpy(original.data(), &event, sizeof(event));
			const uint64_t arguments[] = {reinterpret_cast<uint64_t>(&event)};
			// The signed 16-bit field promotes to int; scalar dispatch preserves
			// that int's EAX bits. The identifier retains all 64 pointer-sized bits.
			const auto expected_filter_word = static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int>(filter_value)));
			if (!Check(filter_call(&event) == static_cast<int>(filter_value) && id_call(&event) == id_value &&
			           filter_metadata->invoker(arguments) == expected_filter_word &&
			           id_metadata->invoker(arguments) == static_cast<uint64_t>(id_value) &&
			           std::memcmp(&event, original.data(), sizeof(event)) == 0,
			           "event accessor changed its record, filter sign or identifier high bits")) return false;
		}
	std::fprintf(stderr, "[EVENT_ACCESSOR_BINDINGS_TEST] namespace=libkernel-v1.1 filter-sign=pass id64=pass"
	                     " scalar-metadata=pass record-unchanged=pass\n");
	return true;
}

bool RegistrationMatrix()
{
	EventQueue queue;
	if (!Check(queue.Get() != nullptr, "queue creation failed")) return false;
	for (int id = 0; id <= 64; ++id)
	{
		if (!Check(Gnm::GraphicsAddEqEvent(queue.Get(), id, reinterpret_cast<void*>(uintptr_t(id + 1))) == 0,
		           "observed accepted graphics identifier was rejected") || !Empty(queue.Get())) return false;
	}
	for (int id = 0; id <= 64; ++id)
		if (!Check(Gnm::GraphicsDeleteEqEvent(queue.Get(), id) == 0, "exact graphics deletion failed")) return false;
	return Check(Gnm::GraphicsAddEqEvent(queue.Get(), 65, nullptr) == Kernel::KERNEL_ERROR_EPERM &&
	             Gnm::GraphicsAddEqEvent(queue.Get(), -1, nullptr) == Kernel::KERNEL_ERROR_EINVAL &&
	             Gnm::GraphicsAddEqEvent(queue.Get(), std::numeric_limits<int>::max(), nullptr) == Kernel::KERNEL_ERROR_EINVAL &&
	             Gnm::GraphicsAddEqEvent(nullptr, 1, nullptr) == Kernel::KERNEL_ERROR_EBADF &&
	             Gnm::GraphicsAddEqEvent(nullptr, 64, nullptr) == Kernel::KERNEL_ERROR_EBADF,
	             "observed graphics registration error mismatch") && Empty(queue.Get());
}

bool IsolationAndCleanup()
{
	EventQueue first, second;
	if (first.Get() == nullptr || second.Get() == nullptr) return false;
	if (!Check(Gnm::GraphicsAddEqEvent(first.Get(), 1, reinterpret_cast<void*>(uintptr_t {0x11})) == 0 &&
	           Gnm::GraphicsAddEqEvent(first.Get(), 2, reinterpret_cast<void*>(uintptr_t {0x12})) == 0 &&
	           Gnm::GraphicsAddEqEvent(first.Get(), 64, reinterpret_cast<void*>(uintptr_t {0x14})) == 0 &&
	           Gnm::GraphicsAddEqEvent(second.Get(), 2, reinterpret_cast<void*>(uintptr_t {0x22})) == 0,
	           "multiple registrations collided")) return false;
	Graphics::GraphicsRenderTriggerEvent(1, 0x12345678);
	if (!Delivered(first.Get(), 1, 0x11, 0x12345678) || !Empty(second.Get())) return false;
	Graphics::GraphicsRenderTriggerEopEvent();
	if (!Delivered(first.Get(), 64, 0x14, 0, false) || !Empty(second.Get())) return false;
	Graphics::GraphicsRenderTriggerEvent(2, 0x87654321);
	if (!Delivered(first.Get(), 2, 0x12, 0x87654321) || !Delivered(second.Get(), 2, 0x22, 0x87654321)) return false;
	if (!Check(Gnm::GraphicsDeleteEqEvent(first.Get(), 2) == 0, "single identifier deletion failed")) return false;
	Graphics::GraphicsRenderTriggerEvent(2, 17);
	if (!Empty(first.Get()) || !Delivered(second.Get(), 2, 0x22, 17)) return false;
	if (!Check(first.Close(), "multi-event queue deletion failed")) return false;
	Graphics::GraphicsRenderTriggerEvent(1, 18);
	Graphics::GraphicsRenderTriggerEopEvent();
	if (!Empty(second.Get())) return false;
	if (!Check(Queue::KernelAddUserEvent(second.Get(), 700) == 0 && Graphics::GraphicsRenderResetForNextSession(),
	           "graphics registration reset failed")) return false;
	Graphics::GraphicsRenderTriggerEvent(2, 19);
	if (!Empty(second.Get()) || !Check(Queue::KernelTriggerUserEvent(second.Get(), 700, nullptr) == 0,
	                                  "graphics reset removed an unrelated filter")) return false;
	Queue::KernelEvent event {};
	int count = 0;
	const Kernel::KernelUseconds timeout = 0;
	return Check(Queue::KernelWaitEqueue(second.Get(), &event, 1, &count, &timeout) == 0 && count == 1 &&
	             event.ident == 700 && event.filter == Queue::KERNEL_EVFILT_USER,
	             "unrelated user event did not survive graphics reset");
}

bool IdentityAndLifetime()
{
	for (unsigned iteration = 0; iteration != 64; ++iteration)
	{
		EventQueue queue;
		Queue::KernelEqueueIdentity identity;
		if (!Check(queue.Get() != nullptr && Queue::GetEqueueIdentity(queue.Get(), &identity) &&
		           Gnm::GraphicsAddEqEvent(queue.Get(), 1, nullptr) == 0, "lifetime fixture failed")) return false;
		std::atomic<bool> stopped {false};
		std::atomic<unsigned> calls {0};
		std::thread producer([&] {
			while (!stopped.load(std::memory_order_acquire))
			{
				Graphics::GraphicsRenderTriggerEvent(1, 0x55);
				calls.fetch_add(1, std::memory_order_release);
			}
		});
		while (calls.load(std::memory_order_acquire) < 8) std::this_thread::yield();
		const bool closed = queue.Close();
		stopped.store(true, std::memory_order_release);
		producer.join();
		if (!Check(closed && Queue::TriggerEventIfRegistered(identity, 1, Queue::KERNEL_EVFILT_GRAPHICS, nullptr) ==
		           Kernel::KERNEL_ERROR_EBADF, "deleted queue identity was dereferenced or accepted")) return false;
		EventQueue replacement;
		Queue::KernelEqueueIdentity current;
		if (!Check(Queue::GetEqueueIdentity(replacement.Get(), &current) && current.generation != identity.generation &&
		           Gnm::GraphicsAddEqEvent(replacement.Get(), 1, nullptr) == 0,
		           "replacement identity fixture failed")) return false;
		// Explicitly test the recycled-address shape even when the allocator
		// chooses a different address in this particular execution.
		const Queue::KernelEqueueIdentity stale_at_current_address {current.queue, identity.generation};
		if (!Check(Queue::TriggerEventIfRegistered(stale_at_current_address, 1, Queue::KERNEL_EVFILT_GRAPHICS, nullptr) ==
		           Kernel::KERNEL_ERROR_EBADF, "stale generation triggered a replacement queue") || !Empty(replacement.Get())) return false;
		if (!Check(Gnm::GraphicsDeleteEqEvent(replacement.Get(), 1) == 0 &&
		           Queue::TriggerEventIfRegistered(current, 1, Queue::KERNEL_EVFILT_GRAPHICS, nullptr) == Kernel::KERNEL_ERROR_ENOENT,
		           "missing event trigger did not return ENOENT")) return false;
	}
	return Graphics::GraphicsRenderResetForNextSession();
}
} // namespace

int main(int argc, char** argv)
{
	if (argc != 2) return 2;
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	Lumen::Core::VirtualMemory::Init();
#if defined(LUMEN_FEXCORE_RUNTIME)
	if (LumenAndroidFexCoreRuntimeProbeV1(nullptr) != LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1) return 1;
#endif
	Graphics::GraphicsRenderInit();
	bool passed = false;
	if (std::strcmp(argv[1], "contract") == 0) passed = EventAccessorBindingsContract() && RegistrationMatrix() && IsolationAndCleanup();
	else if (std::strcmp(argv[1], "completion") == 0) passed = Graphics::GraphicsRenderInterruptCompletionContractTest();
	else if (std::strcmp(argv[1], "lifetime") == 0) passed = IdentityAndLifetime();
	else return 2;
	if (!passed) return 1;
	std::puts("GnmEqEventTest: PASS");
	return 0;
}
