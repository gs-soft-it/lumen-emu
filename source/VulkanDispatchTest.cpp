#include "Emulator/Graphics/VulkanDispatch.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace Lumen::Libs::Graphics;
namespace {
const char* missing = nullptr;
unsigned checks = 0, global_calls = 0, instance_calls = 0, device_calls = 0;
const auto instance = reinterpret_cast<VkInstance>(uintptr_t(0x100));
const auto device = reinterpret_cast<VkDevice>(uintptr_t(0x200));
void Check(bool value) { ++checks; if (!value) { std::fprintf(stderr, "check %u failed\n", checks); std::exit(1); } }
VKAPI_ATTR void VKAPI_CALL Stub() {}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL DeviceProc(VkDevice value, const char* name)
{
	Check(value == device);
	++device_calls;
	return missing && std::strcmp(missing, name) == 0 ? nullptr : Stub;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL InstanceProc(VkInstance value, const char* name)
{
	if (value == VK_NULL_HANDLE) ++global_calls;
	else { Check(value == instance); ++instance_calls; }
	if (missing && std::strcmp(missing, name) == 0) return nullptr;
	if (std::strcmp(name, "vkGetDeviceProcAddr") == 0)
		return reinterpret_cast<PFN_vkVoidFunction>(DeviceProc);
	return Stub;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL DifferentProc(VkInstance, const char*) { return Stub; }
}
int main()
{
	Check(VulkanDispatchLoadGlobal(nullptr) != nullptr);
	Check(VulkanDispatchLoadInstance(instance) != nullptr);
	Check(VulkanDispatchLoadDevice(device) != nullptr);
	missing = "vkEnumerateInstanceLayerProperties";
	Check(std::strcmp(VulkanDispatchLoadGlobal(InstanceProc), missing) == 0);
	Check(vkGetInstanceProcAddr == nullptr && vkCreateInstance == nullptr);
	missing = nullptr;
	Check(VulkanDispatchLoadGlobal(InstanceProc) == nullptr);
	const auto global_count = global_calls;
	Check(VulkanDispatchLoadGlobal(InstanceProc) == nullptr && global_calls == global_count);
	Check(VulkanDispatchLoadGlobal(DifferentProc) != nullptr);
	Check(VulkanDispatchLoadDevice(device) != nullptr);
	missing = "vkGetPhysicalDeviceSurfaceSupportKHR";
	Check(std::strcmp(VulkanDispatchLoadInstance(instance), missing) == 0);
	Check(vkCreateDevice == nullptr && vkGetDeviceProcAddr == nullptr);
	missing = nullptr;
	Check(VulkanDispatchLoadInstance(instance) == nullptr);
	const auto instance_count = instance_calls;
	Check(VulkanDispatchLoadInstance(instance) == nullptr && instance_calls == instance_count);
	Check(VulkanDispatchLoadInstance(reinterpret_cast<VkInstance>(uintptr_t(0x101))) != nullptr);
	missing = "vkWaitForFences";
	Check(std::strcmp(VulkanDispatchLoadDevice(device), missing) == 0);
	Check(vkAllocateMemory == nullptr && vkQueueSubmit == nullptr);
	missing = nullptr;
	Check(VulkanDispatchLoadDevice(device) == nullptr);
	const auto device_count = device_calls;
	Check(VulkanDispatchLoadDevice(device) == nullptr && device_calls == device_count);
	Check(VulkanDispatchLoadDevice(reinterpret_cast<VkDevice>(uintptr_t(0x201))) != nullptr);
#define LUMEN_VK_GLOBAL(name) Check(name != nullptr);
#define LUMEN_VK_INSTANCE(name) LUMEN_VK_GLOBAL(name)
#define LUMEN_VK_DEVICE(name) LUMEN_VK_GLOBAL(name)
#include "Emulator/Graphics/VulkanDispatchFunctions.inc"
#undef LUMEN_VK_GLOBAL
#undef LUMEN_VK_INSTANCE
#undef LUMEN_VK_DEVICE
	std::printf("VulkanDispatchTest: PASS checks=%u global=%u instance=%u device=%u\n",
	    checks, global_calls, instance_calls, device_calls);
}
