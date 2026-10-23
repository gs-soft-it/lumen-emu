#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEMOVERLAY_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEMOVERLAY_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "libs/imeCommon.h"

#include <memory>

union SDL_Event;
struct SDL_Window;

namespace Libs::Graphics {

struct GraphicContext;

struct SystemOverlayVisualState {
	bool     active;
	uint64_t revision;
};

void                     InitializeSystemOverlayInput(SDL_Window* window);
void                     ShutdownSystemOverlayInput();
bool                     ProcessSystemOverlayInput(const SDL_Event& event);
SystemOverlayVisualState GetSystemOverlayVisualState() noexcept;

// Host-owned UI broker. Tokens retain the active Core/Dialogue owner, and an
// error dialog takes priority. UI callers never receive guest pointers.
bool GetSystemImeHostSnapshot(ImeCommon::HostSnapshot* snapshot);
bool SystemImeHostReplaceText(uint64_t generation, std::u16string_view text, uint32_t cursor);
bool SystemImeHostAccept(uint64_t generation);
bool SystemImeHostCancel(uint64_t generation);

class SystemOverlay final {
public:
	explicit SystemOverlay(GraphicContext& graphics);
	~SystemOverlay();
	KYTY_CLASS_NO_COPY(SystemOverlay);

	[[nodiscard]] bool PrepareFrame(vk::Extent2D extent, vk::Format format, uint32_t image_count);
	void               Record(vk::CommandBuffer command, vk::ImageView target);
	void               ReleaseVulkan();

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEMOVERLAY_H_
