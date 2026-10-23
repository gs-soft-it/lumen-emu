#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_PRESENTER_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_PRESENTER_H_

#include "common/common.h"

#include <memory>
#include <span>

namespace Libs::Graphics {

// Accepted guest submissions only, excluding host repeats/overlay refreshes and
// fallback blank frames. Process-wide, safe to sample without accessing the GPU.
[[nodiscard]] uint64_t GetPresentedGuestFrameCount() noexcept;

class CommandBuffer;
class RenderContext;
struct ImageInfo;
struct WindowContext;

class Presenter final {
public:
	struct Frame;
	struct TraceOrigin {
		uint64_t request_id = 0, slice_sequence = 0, submit_id = 0, epoch_token = 0, producer_tick = 0;
	};
	struct Layer {
		Frame* frame;
		int    bus;
		bool   premultiplied_alpha;
		TraceOrigin trace_origin {};
	};

	explicit Presenter(WindowContext& window);
	~Presenter();
	KYTY_CLASS_NO_COPY(Presenter);

	[[nodiscard]] Frame&         PrepareFrame(CommandBuffer& command, const ImageInfo& info);
	[[nodiscard]] Frame&         PrepareBlankFrame(uint32_t width, uint32_t height, bool opaque,
	                                               CommandBuffer* producer = nullptr);
	[[nodiscard]] bool           PresentLastFrame();
	[[nodiscard]] bool           IsGuestPaused() const noexcept;
	[[nodiscard]] bool           NeedsSystemOverlayRefresh() const noexcept;
	[[nodiscard]] RenderContext& Renderer() const noexcept;
	// Host fallback blank versus a newly submitted guest flip group. Diagnostic
	// counters record this origin, without claiming distinct framebuffer pixels.
	void                         Present(Frame& frame);
	void                         Present(std::span<const Layer> layers);
	void                         ClearLayer(int bus);
	void                         Discard(Frame& frame);

private:
	void PresentLayers(std::span<const Layer> layers, bool host_blank);
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_PRESENTER_H_
