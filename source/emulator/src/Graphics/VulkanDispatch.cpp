#include "Emulator/Graphics/VulkanDispatch.h"

#if defined(LUMEN_VULKAN_DYNAMIC_DISPATCH)
namespace Lumen::Libs::Graphics {
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
#define LUMEN_VK_GLOBAL(name) PFN_##name name = nullptr;
#define LUMEN_VK_INSTANCE(name) LUMEN_VK_GLOBAL(name)
#define LUMEN_VK_DEVICE(name) LUMEN_VK_GLOBAL(name)
#include "Emulator/Graphics/VulkanDispatchFunctions.inc"
#undef LUMEN_VK_GLOBAL
#undef LUMEN_VK_INSTANCE
#undef LUMEN_VK_DEVICE

namespace {
VkInstance bound_instance = VK_NULL_HANDLE;
VkDevice bound_device = VK_NULL_HANDLE;
}

const char* VulkanDispatchLoadGlobal(PFN_vkGetInstanceProcAddr get_proc)
{
	if (!get_proc) return "vkGetInstanceProcAddr";
	if (vkGetInstanceProcAddr)
		return vkGetInstanceProcAddr == get_proc ? nullptr : "loader-already-bound";
#define LUMEN_VK_GLOBAL(name) \
	auto pending_##name = reinterpret_cast<PFN_##name>(get_proc(VK_NULL_HANDLE, #name)); \
	if (!pending_##name) return #name;
#define LUMEN_VK_INSTANCE(name)
#define LUMEN_VK_DEVICE(name)
#include "Emulator/Graphics/VulkanDispatchFunctions.inc"
#undef LUMEN_VK_GLOBAL
#define LUMEN_VK_GLOBAL(name) name = pending_##name;
#include "Emulator/Graphics/VulkanDispatchFunctions.inc"
#undef LUMEN_VK_GLOBAL
#undef LUMEN_VK_INSTANCE
#undef LUMEN_VK_DEVICE
	vkGetInstanceProcAddr = get_proc;
	return nullptr;
}

const char* VulkanDispatchLoadInstance(VkInstance instance)
{
	if (!instance || !vkGetInstanceProcAddr) return "instance-before-loader";
	if (bound_instance) return bound_instance == instance ? nullptr : "instance-already-bound";
#define LUMEN_VK_GLOBAL(name)
#define LUMEN_VK_INSTANCE(name) \
	auto pending_##name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance, #name)); \
	if (!pending_##name) return #name;
#define LUMEN_VK_DEVICE(name)
#include "Emulator/Graphics/VulkanDispatchFunctions.inc"
#undef LUMEN_VK_INSTANCE
#define LUMEN_VK_INSTANCE(name) name = pending_##name;
#include "Emulator/Graphics/VulkanDispatchFunctions.inc"
#undef LUMEN_VK_GLOBAL
#undef LUMEN_VK_INSTANCE
#undef LUMEN_VK_DEVICE
	bound_instance = instance;
	return nullptr;
}

const char* VulkanDispatchLoadDevice(VkDevice device)
{
	if (!device || !bound_instance || !vkGetDeviceProcAddr) return "device-before-instance";
	if (bound_device) return bound_device == device ? nullptr : "device-already-bound";
#define LUMEN_VK_GLOBAL(name)
#define LUMEN_VK_INSTANCE(name)
#define LUMEN_VK_DEVICE(name) \
	auto pending_##name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name)); \
	if (!pending_##name) return #name;
#include "Emulator/Graphics/VulkanDispatchFunctions.inc"
#undef LUMEN_VK_DEVICE
#define LUMEN_VK_DEVICE(name) name = pending_##name;
#include "Emulator/Graphics/VulkanDispatchFunctions.inc"
#undef LUMEN_VK_GLOBAL
#undef LUMEN_VK_INSTANCE
#undef LUMEN_VK_DEVICE
	bound_device = device;
	return nullptr;
}
} // namespace Lumen::Libs::Graphics
#endif
