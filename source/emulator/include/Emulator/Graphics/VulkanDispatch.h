#ifndef EMULATOR_GRAPHICS_VULKAN_DISPATCH_H_
#define EMULATOR_GRAPHICS_VULKAN_DISPATCH_H_

#if defined(LUMEN_VULKAN_DYNAMIC_DISPATCH) && !defined(VK_NO_PROTOTYPES)
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>

#if defined(LUMEN_VULKAN_DYNAMIC_DISPATCH)
namespace Lumen::Libs::Graphics {
// Namespaced pointers avoid exporting/interposing C Vulkan symbols. Startup
// publishes each complete stage before any rendering workers can consume it.
extern PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
#define LUMEN_VK_GLOBAL(name) extern PFN_##name name;
#define LUMEN_VK_INSTANCE(name) LUMEN_VK_GLOBAL(name)
#define LUMEN_VK_DEVICE(name) LUMEN_VK_GLOBAL(name)
#include "VulkanDispatchFunctions.inc"
#undef LUMEN_VK_GLOBAL
#undef LUMEN_VK_INSTANCE
#undef LUMEN_VK_DEVICE

// Null means success; failure names the missing symbol or invalid stage.
// One loader/instance/device per isolated Android runtime. Repeating the same
// binding is harmless, replacing an active binding is rejected.
const char* VulkanDispatchLoadGlobal(PFN_vkGetInstanceProcAddr get_proc);
const char* VulkanDispatchLoadInstance(VkInstance instance);
const char* VulkanDispatchLoadDevice(VkDevice device);
} // namespace Lumen::Libs::Graphics
#endif
#endif
