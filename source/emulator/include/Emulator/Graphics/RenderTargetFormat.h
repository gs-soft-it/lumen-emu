#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RENDERTARGETFORMAT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RENDERTARGETFORMAT_H_

#include <cstdint>

namespace Lumen::Libs::Graphics {

enum class RenderTextureFormat : uint64_t
{
	Unknown,
	R8Unorm,
	R8G8B8A8Unorm,
	R8G8B8A8Srgb,
	B8G8R8A8Unorm,
	B8G8R8A8Srgb,
	R8G8Unorm,
};

enum class RenderTextureSampleView : uint8_t
{
	Unsupported,
	Default,
	Bgra,
};

struct ColorRenderTargetFormatInfo
{
	RenderTextureFormat format            = RenderTextureFormat::Unknown;
	uint32_t            bytes_per_element = 0;

	[[nodiscard]] constexpr bool IsSupported() const { return format != RenderTextureFormat::Unknown && bytes_per_element != 0; }
};

[[nodiscard]] constexpr ColorRenderTargetFormatInfo
DecodeColorRenderTargetFormat(uint32_t data_format, uint32_t number_type, uint32_t component_swap)
{
	if (data_format == 0xau && number_type == 0x0u && component_swap == 0x0u)
	{
		return {RenderTextureFormat::R8G8B8A8Unorm, 4};
	}
	if (data_format == 0xau && number_type == 0x6u && component_swap == 0x0u)
	{
		return {RenderTextureFormat::R8G8B8A8Srgb, 4};
	}
	if (data_format == 0xau && number_type == 0x0u && component_swap == 0x1u)
	{
		return {RenderTextureFormat::B8G8R8A8Unorm, 4};
	}
	if (data_format == 0xau && number_type == 0x6u && component_swap == 0x1u)
	{
		return {RenderTextureFormat::B8G8R8A8Srgb, 4};
	}
	if (data_format == 0x1u && number_type == 0x0u && component_swap == 0x0u)
	{
		return {RenderTextureFormat::R8Unorm, 1};
	}
	if (data_format == 0x3u && number_type == 0x0u && component_swap == 0x0u)
	{
		return {RenderTextureFormat::R8G8Unorm, 2};
	}
	return {};
}

[[nodiscard]] constexpr uint64_t CalculateColorRenderTargetByteSize(uint32_t slice_div64_minus1, const ColorRenderTargetFormatInfo& format)
{
	return format.IsSupported() ? (static_cast<uint64_t>(slice_div64_minus1) + 1u) * 64u * format.bytes_per_element : 0;
}

struct Ps4ColorRenderTargetDescriptor
{
	ColorRenderTargetFormatInfo format;
	uint64_t                    byte_size = 0;

	[[nodiscard]] constexpr bool IsSupported() const { return format.IsSupported() && byte_size != 0; }
};

[[nodiscard]] constexpr Ps4ColorRenderTargetDescriptor
BuildPs4ColorRenderTargetDescriptor(uint32_t data_format, uint32_t number_type, uint32_t component_swap, uint32_t slice_div64_minus1)
{
	const auto format = DecodeColorRenderTargetFormat(data_format, number_type, component_swap);
	return {format, CalculateColorRenderTargetByteSize(slice_div64_minus1, format)};
}

[[nodiscard]] constexpr ColorRenderTargetFormatInfo DecodeSampledRenderTextureFormat(uint32_t data_format, uint32_t number_format)
{
	if (data_format == 0xau && number_format == 0x0u)
	{
		return {RenderTextureFormat::R8G8B8A8Unorm, 4};
	}
	if (data_format == 0xau && number_format == 0x9u)
	{
		return {RenderTextureFormat::R8G8B8A8Srgb, 4};
	}
	if (data_format == 0x1u && number_format == 0x0u)
	{
		return {RenderTextureFormat::R8Unorm, 1};
	}
	if (data_format == 0x3u && number_format == 0x0u)
	{
		return {RenderTextureFormat::R8G8Unorm, 2};
	}
	return {};
}

[[nodiscard]] constexpr uint32_t PackRenderTextureSwizzle(uint32_t x, uint32_t y, uint32_t z, uint32_t w)
{
	return x | (y << 3u) | (z << 6u) | (w << 9u);
}

[[nodiscard]] constexpr bool AreRenderTextureFormatsSampleCompatible(RenderTextureFormat render_format, RenderTextureFormat sampled_format)
{
	if (render_format == RenderTextureFormat::Unknown || sampled_format == RenderTextureFormat::Unknown)
	{
		return false;
	}
	if (render_format == sampled_format)
	{
		return true;
	}
	return ((render_format == RenderTextureFormat::R8G8B8A8Unorm || render_format == RenderTextureFormat::B8G8R8A8Unorm) &&
	        (sampled_format == RenderTextureFormat::R8G8B8A8Unorm || sampled_format == RenderTextureFormat::B8G8R8A8Unorm)) ||
	       ((render_format == RenderTextureFormat::R8G8B8A8Srgb || render_format == RenderTextureFormat::B8G8R8A8Srgb) &&
	        (sampled_format == RenderTextureFormat::R8G8B8A8Srgb || sampled_format == RenderTextureFormat::B8G8R8A8Srgb));
}

[[nodiscard]] constexpr RenderTextureSampleView
SelectRenderTextureSampleView(RenderTextureFormat render_format, RenderTextureFormat sampled_format, uint32_t component_swizzle)
{
	if (!AreRenderTextureFormatsSampleCompatible(render_format, sampled_format))
	{
		return RenderTextureSampleView::Unsupported;
	}

	constexpr uint32_t xyzw = PackRenderTextureSwizzle(4, 5, 6, 7);
	if (sampled_format == RenderTextureFormat::R8Unorm)
	{
		return (component_swizzle == PackRenderTextureSwizzle(4, 0, 0, 1) || component_swizzle == xyzw)
		           ? RenderTextureSampleView::Default
		           : RenderTextureSampleView::Unsupported;
	}
	if (sampled_format == RenderTextureFormat::R8G8Unorm)
	{
		return (component_swizzle == PackRenderTextureSwizzle(4, 5, 0, 1) || component_swizzle == xyzw)
		           ? RenderTextureSampleView::Default
		           : RenderTextureSampleView::Unsupported;
	}
	const bool render_is_bgra = render_format == RenderTextureFormat::B8G8R8A8Unorm || render_format == RenderTextureFormat::B8G8R8A8Srgb;
	if (component_swizzle == xyzw)
	{
		return render_is_bgra ? RenderTextureSampleView::Bgra : RenderTextureSampleView::Default;
	}
	if (component_swizzle == PackRenderTextureSwizzle(6, 5, 4, 7))
	{
		return render_is_bgra ? RenderTextureSampleView::Default : RenderTextureSampleView::Bgra;
	}
	return RenderTextureSampleView::Unsupported;
}

[[nodiscard]] constexpr bool AreRenderTextureExtentsCompatible(uint32_t render_width, uint32_t render_height, uint32_t sampled_width,
                                                               uint32_t sampled_height)
{
	return render_width != 0 && render_height != 0 && render_width == sampled_width && render_height == sampled_height;
}

[[nodiscard]] constexpr bool CanUseRenderTextureAsSampledAlias(bool storage_descriptor, bool read_only_usage)
{
	return !storage_descriptor && read_only_usage;
}

} // namespace Lumen::Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RENDERTARGETFORMAT_H_ */
