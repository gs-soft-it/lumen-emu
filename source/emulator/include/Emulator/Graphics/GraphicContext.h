#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICCONTEXT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICCONTEXT_H_

#include "Lumen/Core/Common.h"
#include "Lumen/Core/Threads.h"

#include "Emulator/Common.h"

#include "Emulator/Graphics/VulkanDispatch.h" // IWYU pragma: export

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics {

class CommandProcessor;

struct VulkanSwapchain
{
	VkSwapchainKHR swapchain               = nullptr;
	VkFormat       swapchain_format        = VK_FORMAT_UNDEFINED;
	VkColorSpaceKHR swapchain_color_space  = VK_COLOR_SPACE_MAX_ENUM_KHR;
	VkExtent2D     swapchain_extent        = {};
	VkImage*       swapchain_images        = nullptr;
	VkImageView*   swapchain_image_views   = nullptr;
	VkImageLayout* swapchain_image_layouts = nullptr;
	VkSemaphore*   submit_semaphores       = nullptr;
	VkSemaphore    acquire_semaphore       = nullptr;
	uint32_t       swapchain_images_count  = 0;
	uint32_t       current_index           = 0;
};

struct VulkanCommandPool
{
	// The persistent processor ring must fit in one pool: access and content
	// provenance retain that pool identity across consecutive submissions.
	static constexpr uint32_t PROCESSOR_BUFFER_COUNT = 8;

	Core::Mutex      mutex;
	VkCommandPool    pool          = nullptr;
	VkCommandBuffer* buffers       = nullptr;
	VkFence*         fences        = nullptr;
	VkSemaphore*     semaphores    = nullptr;
	bool*            busy          = nullptr;
	uint32_t         buffers_count = 0;
};

struct VulkanQueueInfo
{
	Core::Mutex* mutex    = nullptr;
	uint32_t     family   = static_cast<uint32_t>(-1);
	uint32_t     index    = static_cast<uint32_t>(-1);
	VkQueue      vk_queue = nullptr;
};

struct GraphicContext
{
	static constexpr int QUEUES_NUM          = 11;
	static constexpr int QUEUE_GFX           = 8;
	static constexpr int QUEUE_GFX_NUM       = 1;
	static constexpr int QUEUE_UTIL          = 9;
	static constexpr int QUEUE_UTIL_NUM      = 1;
	static constexpr int QUEUE_PRESENT       = 10;
	static constexpr int QUEUE_PRESENT_NUM   = 1;
	static constexpr int QUEUE_COMPUTE_START = 0;
	static constexpr int QUEUE_COMPUTE_NUM   = 8;

	uint32_t                 screen_width    = 0;
	uint32_t                 screen_height   = 0;
	VkInstance               instance        = nullptr;
	VkDebugUtilsMessengerEXT debug_messenger = nullptr;
	VkPhysicalDevice         physical_device = nullptr;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	VkFormat                 guest_d24s8_format = VK_FORMAT_D24_UNORM_S8_UINT;
#endif
	VkDevice                 device          = nullptr;
	VulkanQueueInfo          queues[QUEUES_NUM];
};

struct VulkanMemory
{
	VkMemoryRequirements  requirements = {};
	VkMemoryPropertyFlags property     = 0;
	VkDeviceMemory        memory       = nullptr;
	VkDeviceSize          offset       = 0;
	uint32_t              type         = 0;
	uint64_t              unique_id    = 0;
};

enum class VulkanImageType
{
	Unknown,
	VideoOut,
	DepthStencil,
	Texture,
	StorageTexture,
	RenderTexture
};

struct VulkanImage
{
	static constexpr int VIEW_MAX           = 4;
	static constexpr int VIEW_DEFAULT       = 0;
	static constexpr int VIEW_BGRA          = 1;
	static constexpr int VIEW_DEPTH_TEXTURE = 2;
	static constexpr int VIEW_VIDEOOUT_UNORM_DEFAULT = 2;
	static constexpr int VIEW_VIDEOOUT_UNORM_BGRA    = 3;

	explicit VulkanImage(VulkanImageType type): type(type) {}

	VulkanImageType        type                 = VulkanImageType::Unknown;
	VkFormat               format               = VK_FORMAT_UNDEFINED;
	VkExtent2D             extent               = {};
	VkImage                image                = nullptr;
	VkImageView            image_view[VIEW_MAX] = {};
	VkImageLayout          layout               = VK_IMAGE_LAYOUT_UNDEFINED;
	VkImageUsageFlags      usage                = 0;
	VkImageCreateFlags     flags                = 0;
	uint32_t               mip_levels           = 0;
	Graphics::VulkanMemory memory;
};

struct VideoOutVulkanImage: public VulkanImage
{
	VideoOutVulkanImage(): VulkanImage(VulkanImageType::VideoOut) {}
};

struct DepthStencilVulkanImage: public VulkanImage
{
	DepthStencilVulkanImage(): VulkanImage(VulkanImageType::DepthStencil) {}
	bool compressed = false;
};

struct TextureVulkanImage: public VulkanImage
{
	TextureVulkanImage(): VulkanImage(VulkanImageType::Texture) {}
};

struct StorageTextureVulkanImage: public VulkanImage
{
	StorageTextureVulkanImage(): VulkanImage(VulkanImageType::StorageTexture) {}
	VkExtent2D physical_extent       = {};
	uint32_t   descriptor_base_level = 0;
	uint32_t   descriptor_levels     = 0;
};

struct RenderTextureVulkanImage: public VulkanImage
{
	RenderTextureVulkanImage(): VulkanImage(VulkanImageType::RenderTexture) {}
};

struct VulkanBuffer
{
	VkBuffer           buffer = nullptr;
	VulkanMemory       memory;
	VkBufferUsageFlags usage = 0;
};

struct StorageVulkanBuffer: public VulkanBuffer
{
	CommandProcessor* cp          = nullptr;
	void*             mapped_data = nullptr;
	uint64_t          size        = 0;
};

} // namespace Lumen::Libs::Graphics

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICCONTEXT_H_ */
