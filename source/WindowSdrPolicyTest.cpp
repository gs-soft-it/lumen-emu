#include "Emulator/Graphics/SdrPresentationPolicy.h"

#include <array>
#include <cstdio>

namespace Graphics = Lumen::Libs::Graphics;

int main()
{
	const VkSurfaceFormatKHR unorm {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
	const VkSurfaceFormatKHR srgb {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
	const VkSurfaceFormatKHR hdr {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_HDR10_ST2084_EXT};
	const VkSurfaceFormatKHR other {VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
	const VkSurfaceFormatKHR undefined {VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
	const VkSurfaceFormatKHR unknown {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_MAX_ENUM_KHR};
	if (!Graphics::IsSupportedSdrSurfaceFormat(unorm) || !Graphics::IsSupportedSdrSurfaceFormat(srgb)) return 1;
	for (auto rejected: {hdr, other, undefined, unknown})
		if (Graphics::IsSupportedSdrSurfaceFormat(rejected)) return 1;
	VkSurfaceFormatKHR selected = hdr;
	const auto unchanged = [&]() { return selected.format == hdr.format && selected.colorSpace == hdr.colorSpace; };
	const std::array<VkSurfaceFormatKHR, 3> no_sdr {hdr, other, undefined};
	if (Graphics::TrySelectSdrSurfaceFormat(nullptr, 1, &selected) ||
	    Graphics::TrySelectSdrSurfaceFormat(&unorm, 0, &selected) ||
	    Graphics::TrySelectSdrSurfaceFormat(&unorm, 1, nullptr) ||
	    Graphics::TrySelectSdrSurfaceFormat(no_sdr.data(), no_sdr.size(), &selected) || !unchanged()) return 1;
	for (const auto& formats: {std::array<VkSurfaceFormatKHR, 3> {hdr, srgb, unorm},
	                          std::array<VkSurfaceFormatKHR, 3> {unorm, hdr, srgb},
	                          std::array<VkSurfaceFormatKHR, 3> {srgb, unorm, hdr}})
	{
		if (!Graphics::TrySelectSdrSurfaceFormat(formats.data(), formats.size(), &selected) ||
		    selected.format != unorm.format || selected.colorSpace != unorm.colorSpace) return 1;
	}
	// Reapply the same policy to changing surface inventories, as on recreation:
	// an earlier successful choice must not permit a later non-SDR fallback.
	if (!Graphics::TrySelectSdrSurfaceFormat(&srgb, 1, &selected) || selected.format != srgb.format ||
	    Graphics::TrySelectSdrSurfaceFormat(&hdr, 1, &selected) ||
	    !Graphics::TrySelectSdrSurfaceFormat(&unorm, 1, &selected) || selected.format != unorm.format) return 1;
	std::puts("WindowSdrPolicyTest: PASS");
	return 0;
}
