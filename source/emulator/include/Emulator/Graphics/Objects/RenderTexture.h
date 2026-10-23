#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_RENDERTEXTURE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_RENDERTEXTURE_H_

#include "Lumen/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/RenderTargetFormat.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics {

class RenderTextureObject: public GpuObject
{
public:
	static bool SupportsDisplay2dReadback(RenderTextureFormat format, uint32_t width, uint32_t height,
	                                     uint32_t pitch, bool neo, uint64_t address, uint64_t size);
	static constexpr int PARAM_FORMAT     = 0;
	static constexpr int PARAM_WIDTH      = 1;
	static constexpr int PARAM_HEIGHT     = 2;
	static constexpr int PARAM_TILED      = 3;
	static constexpr int PARAM_NEO        = 4;
	static constexpr int PARAM_PITCH      = 5;
	static constexpr int PARAM_WRITE_BACK = 6;
	static constexpr int PARAM_TILE_MODE  = 7;

	explicit RenderTextureObject(RenderTextureFormat pixel_format, uint32_t width, uint32_t height, bool tiled, bool neo, uint32_t pitch,
	                             bool write_back, uint32_t tile_mode)
	{
		params[PARAM_FORMAT]     = static_cast<uint64_t>(pixel_format);
		params[PARAM_WIDTH]      = width;
		params[PARAM_HEIGHT]     = height;
		params[PARAM_TILED]      = tiled ? 1 : 0;
		params[PARAM_NEO]        = neo ? 1 : 0;
		params[PARAM_PITCH]      = pitch;
		params[PARAM_WRITE_BACK] = write_back ? 1 : 0;
		params[PARAM_TILE_MODE]  = tile_mode;
		check_hash               = true;
		type                     = Graphics::GpuMemoryObjectType::RenderTexture;
		access_kind              = GpuMemoryAccessKind::ColorAttachmentReadWrite;
	}

	bool Equal(const uint64_t* other) const override;

	[[nodiscard]] create_func_t              GetCreateFunc() const override;
	[[nodiscard]] create_with_buffer_func_t  GetCreateWithBufferFunc() const override;
	[[nodiscard]] create_from_objects_func_t GetCreateFromObjectsFunc() const override;
	[[nodiscard]] write_back_func_t          GetWriteBackFunc() const override;
	[[nodiscard]] delete_func_t              GetDeleteFunc() const override;
	[[nodiscard]] update_func_t              GetUpdateFunc() const override;
	[[nodiscard]] update_with_buffer_func_t  GetUpdateWithBufferFunc() const override;
};

bool RenderTextureObjectParameterContractTest();

} // namespace Lumen::Libs::Graphics

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_RENDERTEXTURE_H_ */
