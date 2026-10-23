#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_H_

#include "Lumen/Core/Common.h"

#include "Emulator/Common.h"

#ifdef LUMEN_EMU_ENABLED

struct VkSurfaceCapabilitiesKHR;

namespace Lumen::Libs::Graphics {

struct GraphicContext;
struct VideoOutVulkanImage;
struct VulkanImage;

VkSurfaceCapabilitiesKHR* VulkanGetSurfaceCapabilities();

GraphicContext* WindowGetGraphicContext();
// Snapshot of the actual live swapchain, not a hardware capability claim.
[[nodiscard]] bool WindowHasSdrOutput();

void WindowInit(uint32_t width, uint32_t height);
void WindowRun();
[[nodiscard]] bool WindowRequestExit();
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
bool WindowDrainIOSRuntimeSession();
[[nodiscard]] bool WindowAbortIOSRuntimeSession();
[[nodiscard]] bool WindowFinishIOSRuntimeSession();
#endif
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool WindowIOSRuntimeLifecycleStateTest();
bool WindowUncappedPresentModeSelectionTest();
bool WindowSwapchainImageCountSelectionTest();
bool WindowSwapchainRecreationPolicyTest();
bool WindowSwapchainPreTransformSelectionTest();
bool WindowSurfaceReplacementResultPolicyTest();
bool WindowGuestPresentationLayoutTest();
bool WindowPresentationConfigurationTest();
bool WindowFlipWaitEventServiceTest();
bool WindowHandoffQueueFamilySelectionTest();
#endif
void WindowWaitForGraphicInitialized();
void WindowDrawBuffer(VideoOutVulkanImage* image);
void WindowDrawBuffer(VulkanImage* image);
void WindowShowWithoutBuffer();

} // namespace Lumen::Libs::Graphics

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_H_ */
