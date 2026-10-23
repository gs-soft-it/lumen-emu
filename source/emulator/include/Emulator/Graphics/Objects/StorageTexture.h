#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_STORAGETEXTURE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_STORAGETEXTURE_H_

#include "Lumen/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics {

bool StorageTextureWindowSupported(uint32_t base_level, uint32_t levels);
bool StorageTextureUploadLevelIncluded(uint32_t base_level, uint32_t levels, uint32_t level);

class StorageTextureObject: public GpuObject
{
public:
	static constexpr int PARAM_FORMAT       = 0;
	static constexpr int PARAM_PITCH        = 1;
	static constexpr int PARAM_WIDTH_HEIGHT = 2;
	static constexpr int PARAM_LEVELS       = 3;
	static constexpr int PARAM_TILE         = 4;
	static constexpr int PARAM_NEO          = 5;
	static constexpr int PARAM_SWIZZLE      = 6;
	static constexpr int PARAM_POW2_PAD     = 7;

	StorageTextureObject(uint8_t dfmt, uint8_t nfmt, uint32_t width, uint32_t height, uint32_t pitch, uint32_t base_level,
	                     uint32_t levels, uint32_t tile, bool neo, uint32_t swizzle, bool pow2_pad)
	{
		params[PARAM_FORMAT]       = (static_cast<uint64_t>(dfmt) << 8u) | nfmt;
		params[PARAM_PITCH]        = pitch;
		params[PARAM_WIDTH_HEIGHT] = (static_cast<uint64_t>(width) << 32u) | height;
		params[PARAM_LEVELS]       = (static_cast<uint64_t>(base_level) << 32u) | levels;
		params[PARAM_TILE]         = tile;
		params[PARAM_NEO]          = neo ? 1 : 0;
		params[PARAM_SWIZZLE]      = swizzle;
		params[PARAM_POW2_PAD]     = pow2_pad ? 1 : 0;
		check_hash                 = true;
		type                       = Graphics::GpuMemoryObjectType::StorageTexture;
	}

	bool Equal(const uint64_t* other) const override;

	[[nodiscard]] create_func_t              GetCreateFunc() const override;
	[[nodiscard]] create_with_buffer_func_t  GetCreateWithBufferFunc() const override;
	[[nodiscard]] create_from_objects_func_t GetCreateFromObjectsFunc() const override;
	[[nodiscard]] write_back_func_t          GetWriteBackFunc() const override { return nullptr; };
	[[nodiscard]] delete_func_t              GetDeleteFunc() const override;
	[[nodiscard]] update_func_t              GetUpdateFunc() const override;
	[[nodiscard]] update_with_buffer_func_t  GetUpdateWithBufferFunc() const override;
};

} // namespace Lumen::Libs::Graphics

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_STORAGETEXTURE_H_ */
