#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_VIDEOOUTBUFFER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_VIDEOOUTBUFFER_H_

#include "Lumen/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics {

struct VideoOutReadbackContinuation;

enum class VideoOutBufferFormat : uint64_t
{
	Unknown,
	R8G8B8A8Srgb,
	B8G8R8A8Srgb,
};

// Shared experimental rollout; no format or title can enable it implicitly.
bool VideoOutBufferReadbackEnabled();
// Metadata-only validation. Mapping lifetime and producer completion remain
// separate obligations of the tracker and the publication helper.
bool VideoOutBufferReadbackSupported(const uint64_t* params, const VulkanImage* image,
	                                 uint64_t address, uint64_t size);
// The original generation is captured before the processor wait. The
// publication helper proves that exact destination mapping unchanged since
// that token across its image transfer/wait; unrelated VM mutations are allowed.
// Optional host hash output is computed before that publication lease ends.
bool VideoOutBufferWriteBack(GraphicContext* ctx, const uint64_t* params, void* object,
	                         uint64_t address, uint64_t size, uint64_t expected_generation,
	                         uint64_t* published_hash = nullptr,
	                         const VideoOutReadbackContinuation* continuation = nullptr);

class VideoOutBufferObject: public GpuObject
{
public:
	static constexpr int PARAM_FORMAT = 0;
	static constexpr int PARAM_WIDTH  = 1;
	static constexpr int PARAM_HEIGHT = 2;
	static constexpr int PARAM_TILED  = 3;
	static constexpr int PARAM_NEO    = 4;
	static constexpr int PARAM_PITCH  = 5;

	explicit VideoOutBufferObject(VideoOutBufferFormat pixel_format, uint32_t width, uint32_t height, bool tiled, bool neo, uint32_t pitch)
	{
		params[PARAM_FORMAT] = static_cast<uint64_t>(pixel_format);
		params[PARAM_WIDTH]  = width;
		params[PARAM_HEIGHT] = height;
		params[PARAM_TILED]  = tiled ? 1 : 0;
		params[PARAM_NEO]    = neo ? 1 : 0;
		params[PARAM_PITCH]  = pitch;
		check_hash           = true;
		type                 = Graphics::GpuMemoryObjectType::VideoOutBuffer;
	}

	bool Equal(const uint64_t* other) const override;
	static bool SupportsReadback(VideoOutBufferFormat format, uint64_t width, uint64_t height, uint64_t pitch,
	                             bool tiled, bool neo, uint64_t address, uint64_t size);

	[[nodiscard]] create_func_t              GetCreateFunc() const override;
	[[nodiscard]] create_from_objects_func_t GetCreateFromObjectsFunc() const override { return nullptr; };
	[[nodiscard]] write_back_func_t          GetWriteBackFunc() const override;
	[[nodiscard]] delete_func_t              GetDeleteFunc() const override;
	[[nodiscard]] update_func_t              GetUpdateFunc() const override;
};

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
[[nodiscard]] bool VideoOutBufferSampledAliasContractTest();
#endif

} // namespace Lumen::Libs::Graphics

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_VIDEOOUTBUFFER_H_ */
