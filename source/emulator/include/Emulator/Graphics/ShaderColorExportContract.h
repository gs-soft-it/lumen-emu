#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERCOLOREXPORTCONTRACT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERCOLOREXPORTCONTRACT_H_

#include <cstdint>

namespace Lumen::Libs::Graphics {

enum class PixelColorExportEncoding : uint8_t
{
	Unsupported,
	Mrt0Rg16FloatCompressed,
	Mrt0Rgba16FloatCompressed,
	Mrt0Rgba32Float,
};

[[nodiscard]] constexpr PixelColorExportEncoding DecodePixelColorExportEncoding(uint32_t target_output_mode, uint32_t shader_mask)
{
	if (target_output_mode == 4u && shader_mask == 0x3u)
	{
		return PixelColorExportEncoding::Mrt0Rg16FloatCompressed;
	}
	if (target_output_mode == 4u && shader_mask == 0xfu)
	{
		return PixelColorExportEncoding::Mrt0Rgba16FloatCompressed;
	}
	if (target_output_mode == 9u && shader_mask == 0xfu)
	{
		return PixelColorExportEncoding::Mrt0Rgba32Float;
	}
	return PixelColorExportEncoding::Unsupported;
}

[[nodiscard]] constexpr bool IsPixelColorExportSupported(PixelColorExportEncoding encoding)
{
	return encoding != PixelColorExportEncoding::Unsupported;
}

[[nodiscard]] constexpr bool ArePixelColorExportEncodingsCompatible(PixelColorExportEncoding register_encoding,
                                                                    PixelColorExportEncoding instruction_encoding)
{
	return IsPixelColorExportSupported(register_encoding) && register_encoding == instruction_encoding;
}

[[nodiscard]] constexpr bool IsPixelColorExportStateSupported(bool embedded, uint32_t target_output_mode, uint32_t shader_mask)
{
	return embedded || IsPixelColorExportSupported(DecodePixelColorExportEncoding(target_output_mode, shader_mask));
}

} // namespace Lumen::Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERCOLOREXPORTCONTRACT_H_ */
