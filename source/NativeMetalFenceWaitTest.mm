#include "emulator/src/Graphics/Metal/MetalVulkanInternal.h"
#include "emulator/include/Emulator/Graphics/NativeFenceWaitPins.h"

#include <chrono>
#include <cstdio>
#include <future>
#include <thread>

namespace {
using namespace std::chrono_literals;

bool Expect(VkResult actual, VkResult expected, const char* name)
{
	if (actual == expected) return true;
	std::fprintf(stderr, "NativeMetalFenceWaitTest: %s result=%d expected=%d\n", name, actual, expected);
	return false;
}

bool Await(std::future<VkResult>& result, VkDevice device, VkResult expected, const char* name)
{
	if (result.wait_for(2s) != std::future_status::ready)
	{
		// A broken notification must fail the test without stranding a worker.
		Lumen::MetalVulkan::MarkDeviceLost(device);
		std::fprintf(stderr, "NativeMetalFenceWaitTest: %s notification timeout\n", name);
		(void)result.get();
		return false;
	}
	return Expect(result.get(), expected, name);
}

bool FenceSets()
{
	VkDevice_T device;
	VkFence_T first, second, unrelated;
	first.device = second.device = unrelated.device = &device;
	const VkFence fences[] {&first, &second};
	if (!Expect(vkWaitForFences(&device, 2, fences, VK_TRUE, 0), VK_TIMEOUT, "all-zero") ||
	    !Expect(vkWaitForFences(&device, 2, fences, VK_FALSE, 0), VK_TIMEOUT, "any-zero")) return false;
	Lumen::MetalVulkan::SignalFence(&second);
	if (!Expect(vkWaitForFences(&device, 2, fences, VK_FALSE, 0), VK_SUCCESS, "any-already-signaled") ||
	    !Expect(vkWaitForFences(&device, 2, fences, VK_TRUE, 0), VK_TIMEOUT, "all-partial-zero")) return false;
	Lumen::MetalVulkan::SignalFence(&first);
	if (!Expect(vkWaitForFences(&device, 2, fences, VK_TRUE, 0), VK_SUCCESS, "all-already-signaled")) return false;
	if (!Expect(vkResetFences(&device, 2, fences), VK_SUCCESS, "reset") ||
	    !Expect(vkWaitForFences(&device, 2, fences, VK_TRUE, 2'000'000), VK_TIMEOUT, "finite-timeout")) return false;

	auto all = std::async(std::launch::async, [&] { return vkWaitForFences(&device, 2, fences, VK_TRUE, UINT64_MAX); });
	Lumen::MetalVulkan::SignalFence(&first);
	if (all.wait_for(5ms) != std::future_status::timeout)
	{
		Lumen::MetalVulkan::SignalFence(&second);
		(void)all.get();
		std::fprintf(stderr, "NativeMetalFenceWaitTest: all completed with one unsignaled fence\n");
		return false;
	}
	Lumen::MetalVulkan::SignalFence(&second);
	if (!Await(all, &device, VK_SUCCESS, "all-notified")) return false;
	vkResetFences(&device, 2, fences);
	auto any = std::async(std::launch::async, [&] { return vkWaitForFences(&device, 2, fences, VK_FALSE, UINT64_MAX); });
	Lumen::MetalVulkan::SignalFence(&unrelated);
	device.fenceCompletionCondition.notify_all(); // A spurious/unrelated wake cannot grant success.
	if (any.wait_for(5ms) != std::future_status::timeout)
	{
		Lumen::MetalVulkan::SignalFence(&second);
		(void)any.get();
		std::fprintf(stderr, "NativeMetalFenceWaitTest: any completed on an unrelated fence\n");
		return false;
	}
	Lumen::MetalVulkan::SignalFence(&second);
	return Await(any, &device, VK_SUCCESS, "any-notified");
}

bool CompletionRaces()
{
	VkDevice_T device;
	VkFence_T fence;
	fence.device = &device;
	const VkFence fences[] {&fence};
	for (unsigned iteration = 0; iteration < 64; ++iteration)
	{
		Lumen::MetalVulkan::ResetFence(&fence);
		// Exercise completion before predicate evaluation, during admission and
		// after the waiter has had time to enter its condition-variable wait.
		const uint64_t timeout = iteration % 3 == 0 ? UINT64_MAX - 1 :
		    (iteration % 3 == 1 ? uint64_t {INT64_MAX} + 1 : UINT64_MAX);
		auto result = std::async(std::launch::async, [&] { return vkWaitForFences(&device, 1, fences, VK_TRUE, timeout); });
		if (iteration % 2 != 0) std::this_thread::sleep_for(1ms);
		Lumen::MetalVulkan::SignalFence(&fence);
		if (!Await(result, &device, VK_SUCCESS, "completion-race")) return false;
	}
	Lumen::MetalVulkan::ResetFence(&fence);
	auto first = std::async(std::launch::async, [&] { return vkWaitForFences(&device, 1, fences, VK_TRUE, UINT64_MAX); });
	auto second = std::async(std::launch::async, [&] { return vkWaitForFences(&device, 1, fences, VK_FALSE, UINT64_MAX); });
	std::this_thread::sleep_for(1ms);
	Lumen::MetalVulkan::SignalFence(&fence);
	const bool first_ok = Await(first, &device, VK_SUCCESS, "shared-completion-first");
	return Await(second, &device, VK_SUCCESS, "shared-completion-second") && first_ok;
}

bool DeviceLoss()
{
	VkDevice_T device;
	VkFence_T fence;
	fence.device = &device;
	const VkFence fences[] {&fence};
	auto event = std::make_shared<Lumen::MetalVulkan::EventState>();
	event->device = &device;
	auto waiter = std::async(std::launch::async, [&] { return vkWaitForFences(&device, 1, fences, VK_TRUE, UINT64_MAX); });
	std::this_thread::sleep_for(1ms);
	// This models an error from a different queue's event handler. The waited
	// fence remains unsignaled and has no completion callback of its own.
	Lumen::MetalVulkan::CompleteEvent(event, event->generation, MTLCommandBufferStatusError);
	if (!Await(waiter, &device, VK_ERROR_DEVICE_LOST, "cross-queue-event-loss")) return false;
	Lumen::MetalVulkan::SignalFence(&fence);
	return Expect(vkWaitForFences(&device, 1, fences, VK_TRUE, 0), VK_ERROR_DEVICE_LOST, "loss-before-signaled-success");
}

bool PinnedNativeWait()
{
	using Lumen::Libs::Graphics::NativeFenceWaitPins;
	for (const bool lost: {false, true})
	{
		VkDevice_T device;
		VkFence_T fence;
		fence.device = &device;
		const VkFence fences[] {&fence};
		NativeFenceWaitPins pins;
		std::mutex processor;
		auto pin = pins.Capture();
		std::promise<void> peer_locked;
		auto native = std::async(std::launch::async, [&] {
			const auto result = vkWaitForFences(&device, 1, fences, VK_TRUE, UINT64_MAX);
			// No processor lock or simulated guest readmission is needed here.
			pin.Release();
			return result;
		});
		auto peer = std::async(std::launch::async, [&] {
			std::lock_guard lock(processor);
			peer_locked.set_value();
			pins.BeforeResetOrReuse();
			return lost ? VK_ERROR_DEVICE_LOST : vkResetFences(&device, 1, fences);
		});
		peer_locked.get_future().wait();
		const bool blocked = peer.wait_for(2ms) == std::future_status::timeout;
		if (lost) Lumen::MetalVulkan::MarkDeviceLost(&device);
		else Lumen::MetalVulkan::SignalFence(&fence);
		if (!Await(native, &device, lost ? VK_ERROR_DEVICE_LOST : VK_SUCCESS, "pinned-native") ||
		    !Await(peer, &device, lost ? VK_ERROR_DEVICE_LOST : VK_SUCCESS, "pinned-peer") || !blocked) return false;
		if (!lost && vkWaitForFences(&device, 1, fences, VK_TRUE, 0) != VK_TIMEOUT) return false;
	}
	return true;
}

bool InvalidInputs()
{
	VkDevice_T device, other;
	VkFence_T fence;
	fence.device = &other;
	const VkFence wrong[] {&fence};
	const VkFence null[] {VK_NULL_HANDLE};
	return Expect(vkWaitForFences(VK_NULL_HANDLE, 1, wrong, VK_TRUE, 0), VK_ERROR_INITIALIZATION_FAILED, "null-device") &&
	    Expect(vkWaitForFences(&device, 0, wrong, VK_TRUE, 0), VK_ERROR_INITIALIZATION_FAILED, "empty-set") &&
	    Expect(vkWaitForFences(&device, 1, nullptr, VK_TRUE, 0), VK_ERROR_INITIALIZATION_FAILED, "null-set") &&
	    Expect(vkWaitForFences(&device, 1, null, VK_TRUE, 0), VK_ERROR_INITIALIZATION_FAILED, "null-fence") &&
	    Expect(vkWaitForFences(&device, 1, wrong, VK_TRUE, 0), VK_ERROR_INITIALIZATION_FAILED, "foreign-device");
}
} // namespace

int main()
{
	if (!InvalidInputs() || !FenceSets() || !CompletionRaces() || !DeviceLoss() || !PinnedNativeWait()) return 1;
	std::puts("NativeMetalFenceWaitTest: PASS");
	return 0;
}
