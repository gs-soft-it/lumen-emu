#ifndef EMULATOR_GRAPHICS_SDR_PRESENTATION_POLICY_H_
#define EMULATOR_GRAPHICS_SDR_PRESENTATION_POLICY_H_

#include <cstdint>
#include <vulkan/vulkan_core.h>

namespace Lumen::Libs::Graphics {

// This deliberately narrow host policy represents the presenter's existing
// preferred SDR path. It does not claim to enumerate all SDR Vulkan formats.
inline bool IsSupportedSdrSurfaceFormat(VkSurfaceFormatKHR format)
{
	return format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
	       (format.format == VK_FORMAT_B8G8R8A8_UNORM || format.format == VK_FORMAT_B8G8R8A8_SRGB);
}

inline bool TrySelectSdrSurfaceFormat(const VkSurfaceFormatKHR* formats, uint32_t count, VkSurfaceFormatKHR* selected)
{
	if (formats == nullptr || selected == nullptr) return false;
	// Keep Lumen's existing UNORM preference, independent of enumeration order.
	constexpr VkFormat preferences[] {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB};
	for (VkFormat preferred: preferences)
	{
		for (uint32_t index = 0; index < count; ++index)
		{
			if (formats[index].format == preferred && IsSupportedSdrSurfaceFormat(formats[index]))
			{
				*selected = formats[index];
				return true;
			}
		}
	}
	return false;
}

} // namespace Lumen::Libs::Graphics

#endif
