#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DRAWCAPTURE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DRAWCAPTURE_H_

#include <cstdint>
#include <span>

namespace Libs::Graphics {

class CommandBuffer;
class RenderContext;
struct RenderColorInfo;

struct DrawCaptureInfo {
	uint64_t submit_id = 0;
	uint64_t vertex_id = 0;
	uint64_t vertex_hash = 0;
	uint64_t pixel_id = 0;
	uint64_t pixel_hash = 0;
	uint32_t count = 0;
	uint32_t instances = 0;
	bool indexed = false;
	bool attachment_clear = false;
};

// Diagnostic-only: consume ShaderLogFolder/draw-capture.request, containing
// "center_u center_v width height count [skip_draws [pixel_shader_hash_hex]]".
// A zero hash matches every pixel shader. Skip counts draws with an eligible
// output after filtering, while count bounds output copies. Capture begins on
// the next guest frame, with at most 512 copies/32 MiB over at most 64 frames.
// Raw bytes preserve the backing format; metadata also records the view format.
void CaptureDrawOutputs(RenderContext& context, CommandBuffer& command,
                        std::span<const RenderColorInfo> colors, const DrawCaptureInfo& draw);

} // namespace Libs::Graphics

#endif
