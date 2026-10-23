#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_DEPTHSTENCILBUFFER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_DEPTHSTENCILBUFFER_H_

#include "Lumen/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics {

class DepthStencilBufferObject: public GpuObject
{
public:
	static constexpr int PARAM_FORMAT = 0;
	static constexpr int PARAM_WIDTH  = 1;
	static constexpr int PARAM_HEIGHT = 2;
	static constexpr int PARAM_HTILE  = 3;
	static constexpr int PARAM_NEO    = 4;
	static constexpr int PARAM_USAGE  = 5;
	static constexpr int PARAM_UNIFORM_ZERO_IMPORT = 6;
	static constexpr int PARAM_RESERVED = 7;
	// Mandatory native/guest zero authority for address modes whose aliasing
	// cannot be represented by an ordinary attachment. Unlike the reuse hint,
	// this obligation is retained in the allocation parameters and identity.
	static constexpr int PARAM_UNIFORM_ZERO_REQUIRED = 8;
	static_assert(PARAM_UNIFORM_ZERO_REQUIRED < GpuObject::PARAMS_MAX);

	DepthStencilBufferObject(uint64_t vk_format, uint32_t width, uint32_t height, bool htile, bool neo, bool sampled,
	                         bool uniform_zero_import = false, bool uniform_zero_reuse = false,
	                         uint32_t uniform_zero_pitch = 0, uint32_t uniform_zero_slice = 0,
	                         bool uniform_zero_required = false)
	{
		params[PARAM_FORMAT] = vk_format;
		params[PARAM_WIDTH]  = width;
		params[PARAM_HEIGHT] = height;
		params[PARAM_HTILE]  = htile ? 1 : 0;
		params[PARAM_NEO]    = neo ? 1 : 0;
		params[PARAM_USAGE]  = sampled ? 1 : 0;
		params[PARAM_UNIFORM_ZERO_IMPORT] = uniform_zero_import ? 1 : 0;
		params[PARAM_RESERVED] = 0;
		params[PARAM_UNIFORM_ZERO_REQUIRED] = uniform_zero_required ? 1 : 0;
		depth_uniform_zero_reuse = uniform_zero_reuse;
		depth_uniform_zero_pitch = uniform_zero_pitch;
		depth_uniform_zero_slice = uniform_zero_slice;
		check_hash           = false;
		type                 = Graphics::GpuMemoryObjectType::DepthStencilBuffer;
	}

	bool Equal(const uint64_t* other) const override;
	//	bool Reuse(const uint64_t* other) const override;

	[[nodiscard]] create_func_t              GetCreateFunc() const override;
	[[nodiscard]] create_from_objects_func_t GetCreateFromObjectsFunc() const override { return nullptr; };
	[[nodiscard]] write_back_func_t          GetWriteBackFunc() const override { return nullptr; };
	[[nodiscard]] delete_func_t              GetDeleteFunc() const override;
	[[nodiscard]] update_func_t              GetUpdateFunc() const override;
};

} // namespace Lumen::Libs::Graphics

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_DEPTHSTENCILBUFFER_H_ */
