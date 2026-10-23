#include "Emulator/Graphics/Objects/VertexBuffer.h"

#include "Lumen/Core/DbgAssert.h"

#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Profiler.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics {

#ifndef LUMEN_VULKAN_DYNAMIC_VERTEX_HOST_VISIBLE_DEFAULT
#define LUMEN_VULKAN_DYNAMIC_VERTEX_HOST_VISIBLE_DEFAULT 0
#endif

static_assert(LUMEN_VULKAN_DYNAMIC_VERTEX_HOST_VISIBLE_DEFAULT == 0 ||
                  LUMEN_VULKAN_DYNAMIC_VERTEX_HOST_VISIBLE_DEFAULT == 1,
              "dynamic vertex host-visible default must be boolean");

namespace {

constexpr VkMemoryPropertyFlags HostVisibleVertexMemory =
	VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
constexpr VkMemoryPropertyFlags HostVisibleDeviceLocalVertexMemory =
	HostVisibleVertexMemory | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

struct DynamicVertexMemoryDecision
{
	VkMemoryPropertyFlags property              = 0;
	bool                  valid                 = false;
	bool                  host_visible          = false;
	bool                  allocation_fallback   = false;
};

struct DynamicVertexTelemetry
{
	std::atomic_uint64_t updates {0};
	std::atomic_uint64_t host_visible_allocations {0};
	std::atomic_uint64_t host_visible_reuses {0};
	std::atomic_uint64_t direct_uploads {0};
	std::atomic_uint64_t staged_uploads {0};
	std::atomic_uint64_t allocation_fallbacks {0};
	std::atomic_uint64_t map_fallbacks {0};
	std::atomic_uint64_t context_fallbacks {0};
	std::atomic_bool     finalized {false};
	std::atomic_bool     finalize_result {true};
};

DynamicVertexTelemetry g_dynamic_vertex_telemetry;

const char* dynamic_vertex_selector_receipt()
{
	const char* value = std::getenv("LUMEN_VULKAN_DYNAMIC_VERTEX_HOST_VISIBLE");
	if (value == nullptr)
	{
		return "absent";
	}
	if (value[0] == '\0')
	{
		return "empty";
	}
	if (std::strcmp(value, "0") == 0 || std::strcmp(value, "1") == 0)
	{
		return value;
	}
	return "malformed";
}

bool memory_type_compatible(const VkPhysicalDeviceMemoryProperties& properties, uint32_t memory_type_bits,
	                        VkMemoryPropertyFlags required)
{
	for (uint32_t index = 0; index < properties.memoryTypeCount; ++index)
	{
		if ((memory_type_bits & (uint32_t {1} << index)) != 0 &&
		    (properties.memoryTypes[index].propertyFlags & required) == required)
		{
			return true;
		}
	}
	return false;
}

DynamicVertexMemoryDecision select_dynamic_vertex_memory(
	const VkPhysicalDeviceMemoryProperties& properties, uint32_t memory_type_bits, bool requested)
{
	if (requested)
	{
		// Unified-memory devices should retain DEVICE_LOCAL vertex fetches. On a
		// discrete device, accept the coherent upload heap only when the combined
		// type is unavailable.
		if (memory_type_compatible(properties, memory_type_bits, HostVisibleDeviceLocalVertexMemory))
		{
			return {HostVisibleDeviceLocalVertexMemory, true, true, false};
		}
		if (memory_type_compatible(properties, memory_type_bits, HostVisibleVertexMemory))
		{
			return {HostVisibleVertexMemory, true, true, false};
		}
	}

	// An explicit request is allowed to degrade to the historical staging path
	// rather than passing an out-of-range memory type to vkAllocateMemory.
	if (memory_type_compatible(properties, memory_type_bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
	{
		return {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true, false, requested};
	}
	return {};
}

bool create_dynamic_replacement(GraphicContext* ctx, uint64_t size, VkBufferUsageFlags usage, VulkanBuffer* replacement,
	                            bool* host_visible, bool* allocation_fallback)
{
	EXIT_IF(ctx == nullptr || replacement == nullptr || host_visible == nullptr || allocation_fallback == nullptr);
	EXIT_IF(ctx->device == nullptr || ctx->physical_device == nullptr || size == 0);
	EXIT_IF(replacement->buffer != nullptr);

	VkBufferCreateInfo buffer_info {};
	buffer_info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer_info.size        = size;
	buffer_info.usage       = usage;
	buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	const VkResult create_result = vkCreateBuffer(ctx->device, &buffer_info, nullptr, &replacement->buffer);
	if (create_result != VK_SUCCESS || replacement->buffer == nullptr)
	{
		return false;
	}

	replacement->usage = usage;
	vkGetBufferMemoryRequirements(ctx->device, replacement->buffer, &replacement->memory.requirements);

	VkPhysicalDeviceMemoryProperties memory_properties {};
	vkGetPhysicalDeviceMemoryProperties(ctx->physical_device, &memory_properties);
	const auto decision = select_dynamic_vertex_memory(memory_properties, replacement->memory.requirements.memoryTypeBits,
	                                                   VertexBufferDynamicHostVisibleEnabled());
	if (!decision.valid)
	{
		vkDestroyBuffer(ctx->device, replacement->buffer, nullptr);
		replacement->buffer = nullptr;
		return false;
	}

	replacement->memory.property = decision.property;
	bool allocation_succeeded = decision.host_visible
	    ? VulkanTryAllocate(ctx, &replacement->memory)
	    : VulkanAllocate(ctx, &replacement->memory);
	DynamicVertexMemoryDecision effective_decision = decision;
	if (!allocation_succeeded && decision.host_visible)
	{
		// A small BAR/upload heap can be compatible yet exhausted. Reuse the
		// same buffer requirements with the historical device-local allocation
		// and staging path instead of making the optional optimization fatal.
		effective_decision = select_dynamic_vertex_memory(
		    memory_properties, replacement->memory.requirements.memoryTypeBits, false);
		if (effective_decision.valid)
		{
			replacement->memory.property = effective_decision.property;
			allocation_succeeded = VulkanAllocate(ctx, &replacement->memory);
			effective_decision.allocation_fallback = allocation_succeeded;
		}
	}
	if (!allocation_succeeded)
	{
		vkDestroyBuffer(ctx->device, replacement->buffer, nullptr);
		replacement->buffer = nullptr;
		return false;
	}
	VulkanBindBufferMemory(ctx, replacement, &replacement->memory);

	*host_visible        = effective_decision.host_visible;
	*allocation_fallback = effective_decision.allocation_fallback;
	return true;
}

constexpr uint32_t VertexReplacementPoolMax = 128;
constexpr uint64_t VertexReplacementPoolBytesMax = 64u * 1024u * 1024u;

bool replacement_reuse_override(const char* value)
{
	return value == nullptr || (value[0] == '1' && value[1] == '\0');
}

bool replacement_reuse_enabled()
{
	static const bool enabled = replacement_reuse_override(std::getenv("LUMEN_VULKAN_VERTEX_REPLACEMENT_REUSE"));
	return enabled && VertexBufferDynamicHostVisibleEnabled();
}

struct VertexReplacementKey
{
	const void* owner = nullptr;
	VkDevice device = nullptr;
	uint64_t logical_size = 0;
	VkBufferUsageFlags usage = 0;
	VkMemoryPropertyFlags property = 0;
};

bool replacement_key_matches(const VertexReplacementKey& a, const VertexReplacementKey& b)
{
	return a.owner == b.owner && a.device == b.device && a.logical_size == b.logical_size &&
	       a.usage == b.usage && a.property == b.property;
}

// Pure bookkeeping for completed, exclusively owned backing allocations.
// Logical VkBuffer size is deliberately distinct from allocation requirements.
struct VertexReplacementPolicy
{
	std::array<VertexReplacementKey, VertexReplacementPoolMax> keys {};
	std::array<uint64_t, VertexReplacementPoolMax> allocation_bytes {};
	uint32_t count = 0;
	uint64_t bytes = 0, active = 0, pending = 0;

	int Find(const VertexReplacementKey& key) const
	{
		for (uint32_t i = 0; i < count; ++i)
			if (replacement_key_matches(keys[i], key)) return static_cast<int>(i);
		return -1;
	}

	bool Add(const VertexReplacementKey& key, uint64_t allocated)
	{
		if (key.owner == nullptr || key.device == nullptr || key.logical_size == 0 ||
		    key.usage != (VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
		    (key.property != HostVisibleVertexMemory && key.property != HostVisibleDeviceLocalVertexMemory) ||
		    allocated < key.logical_size || count >= VertexReplacementPoolMax ||
		    allocated > VertexReplacementPoolBytesMax - bytes) return false;
		keys[count] = key;
		allocation_bytes[count++] = allocated;
		bytes += allocated;
		return true;
	}

	void Remove(uint32_t index)
	{
		EXIT_IF(index >= count || bytes < allocation_bytes[index]);
		bytes -= allocation_bytes[index];
		--count;
		keys[index] = keys[count];
		allocation_bytes[index] = allocation_bytes[count];
		keys[count] = {};
		allocation_bytes[count] = 0;
	}

	bool CanReset(const void* owner, VkDevice device) const
	{
		if (active != 0 || pending != 0) return false;
		for (uint32_t i = 0; i < count; ++i)
			if (keys[i].owner == owner && keys[i].device != device) return false;
		return true;
	}
};

struct VertexReplacementCache
{
	std::mutex mutex;
	VertexReplacementPolicy policy;
	std::array<VulkanBuffer, VertexReplacementPoolMax> buffers {};
	uint64_t completed = 0, allocations = 0, reuses = 0, returned = 0, freed = 0, drained = 0;

	void ReportLocked(const char* phase) const
	{
		std::fprintf(stderr, "[GFX_VERTEX_REPLACEMENT_REUSE] phase=%s completed=%" PRIu64 " allocations=%" PRIu64
		             " reuses=%" PRIu64 " returned=%" PRIu64 " freed=%" PRIu64 " drained=%" PRIu64
		             " entries=%u bytes=%" PRIu64 " limit=%" PRIu64 " active=%" PRIu64 " pending=%" PRIu64 "\n",
		             phase, completed, allocations, reuses, returned, freed, drained, policy.count,
		             policy.bytes, VertexReplacementPoolBytesMax, policy.active, policy.pending);
	}
};

// No Vulkan calls from static destruction: release explicitly with a live,
// quiescent device, or let the ordinary runtime process own this bounded cache.
VertexReplacementCache& g_vertex_replacements = *new VertexReplacementCache;

class VertexReplacementOperation
{
public:
	VertexReplacementOperation(bool enabled, bool update): m_enabled(enabled), m_update(update)
	{
		if (m_enabled)
		{
			std::lock_guard<std::mutex> lock(g_vertex_replacements.mutex);
			++g_vertex_replacements.policy.active;
		}
	}
	~VertexReplacementOperation()
	{
		if (!m_enabled) return;
		std::lock_guard<std::mutex> lock(g_vertex_replacements.mutex);
		EXIT_IF(g_vertex_replacements.policy.active == 0);
		--g_vertex_replacements.policy.active;
		if (m_update)
		{
			const uint64_t completed = ++g_vertex_replacements.completed;
			if ((completed & (completed - 1)) == 0) g_vertex_replacements.ReportLocked("completed");
		}
	}
	VertexReplacementOperation(const VertexReplacementOperation&) = delete;
	VertexReplacementOperation& operator=(const VertexReplacementOperation&) = delete;

private:
	bool m_enabled, m_update;
};

bool acquire_vertex_replacement(GraphicContext* ctx, uint64_t size, VkBufferUsageFlags usage, VulkanBuffer* replacement)
{
	VkPhysicalDeviceMemoryProperties properties {};
	vkGetPhysicalDeviceMemoryProperties(ctx->physical_device, &properties);
	std::lock_guard<std::mutex> lock(g_vertex_replacements.mutex);
	auto& policy = g_vertex_replacements.policy;
	for (uint32_t i = 0; i < policy.count; ++i)
	{
		const auto& candidate = g_vertex_replacements.buffers[i];
		const auto decision = select_dynamic_vertex_memory(properties, candidate.memory.requirements.memoryTypeBits, true);
		const VertexReplacementKey requested {ctx, ctx->device, size, usage, decision.property};
		if (!decision.valid || !decision.host_visible || !replacement_key_matches(policy.keys[i], requested)) continue;
		*replacement = candidate;
		policy.Remove(i);
		g_vertex_replacements.buffers[i] = g_vertex_replacements.buffers[policy.count];
		g_vertex_replacements.buffers[policy.count] = {};
		++g_vertex_replacements.reuses;
		return true;
	}
	return false;
}

struct RetiredVertexReplacement
{
	VulkanBuffer buffer;
	VertexReplacementKey key;
};

void recycle_retired_vertex_replacement(GraphicContext* ctx, void* obj)
{
	auto* retired = static_cast<RetiredVertexReplacement*>(obj);
	EXIT_IF(ctx == nullptr || retired == nullptr || retired->buffer.buffer == nullptr ||
	        retired->key.owner != ctx || retired->key.device != ctx->device);
	VertexReplacementOperation operation(true, false);
	bool retained = false;
	{
		std::lock_guard<std::mutex> lock(g_vertex_replacements.mutex);
		auto& policy = g_vertex_replacements.policy;
		const uint32_t index = policy.count;
		retained = policy.Add(retired->key, retired->buffer.memory.requirements.size);
		if (retained)
		{
			g_vertex_replacements.buffers[index] = retired->buffer;
			++g_vertex_replacements.returned;
		}
	}
	if (!retained) VulkanDeleteBuffer(ctx, &retired->buffer);
	delete retired;
	{
		std::lock_guard<std::mutex> lock(g_vertex_replacements.mutex);
		EXIT_IF(g_vertex_replacements.policy.pending == 0);
		--g_vertex_replacements.policy.pending;
		g_vertex_replacements.freed += retained ? 0u : 1u;
	}
}

bool replacement_policy_contract_test()
{
	const int owner_a = 1, owner_b = 2;
	const auto device_a = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(1));
	const auto device_b = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(2));
	const VertexReplacementKey key {&owner_a, device_a, 4096,
	    VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, HostVisibleVertexMemory};
	VertexReplacementPolicy policy;
	if (!policy.CanReset(&owner_a, device_a) || policy.Find(key) != -1 || !policy.Add(key, 8192) ||
	    policy.Find(key) != 0 || policy.bytes != 8192) return false;
	// Every key component matters, including logical size despite allocation padding.
	for (int field = 0; field < 5; ++field)
	{
		auto other = key;
		switch (field)
		{
			case 0: other.owner = &owner_b; break;
			case 1: other.device = device_b; break;
			case 2: other.logical_size = 8192; break;
			case 3: other.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT; break;
			case 4: other.property = HostVisibleDeviceLocalVertexMemory; break;
		}
		if (policy.Find(other) != -1) return false;
	}
	if (policy.CanReset(&owner_a, device_b) || !policy.CanReset(&owner_b, device_a)) return false;
	policy.active = 1;
	if (policy.CanReset(&owner_a, device_a)) return false;
	policy.active = 0;
	policy.pending = 1;
	if (policy.CanReset(&owner_a, device_a)) return false;
	policy.pending = 0;
	policy.Remove(0);
	if (policy.count != 0 || policy.bytes != 0 || policy.Find(key) != -1 ||
	    !policy.CanReset(&owner_a, device_a)) return false; // Checked-out storage cannot be acquired twice.
	if (policy.Add(key, 4095) || policy.Add(key, VertexReplacementPoolBytesMax + 1)) return false;
	auto invalid = key;
	invalid.property = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	if (policy.Add(invalid, 4096)) return false; // Initial and allocation-fallback buffers stay outside the pool.
	invalid = key;
	invalid.logical_size = 0;
	if (policy.Add(invalid, 4096)) return false;
	if (!policy.Add(key, VertexReplacementPoolBytesMax) || policy.Add(key, 4096)) return false;
	policy.Remove(0);
	for (uint32_t i = 0; i < VertexReplacementPoolMax; ++i)
		if (!policy.Add(key, 4096)) return false;
	if (policy.Add(key, 4096) || policy.bytes != uint64_t{VertexReplacementPoolMax} * 4096) return false;
	while (policy.count != 0) policy.Remove(policy.count - 1);
	if (policy.bytes != 0 || !policy.Add(key, 4096)) return false;
	auto second = key;
	second.logical_size = 8192;
	if (!policy.Add(second, 8192)) return false;
	policy.Remove(0);
	if (policy.Find(key) != -1 || policy.Find(second) != 0 || policy.bytes != 8192) return false;
	const char* disabled[] = {"", "0", "01", "10", "true", " 1", "1 "};
	for (const char* value: disabled) if (replacement_reuse_override(value)) return false;
	if (!replacement_reuse_override(nullptr) || !replacement_reuse_override("1")) return false;
	std::fprintf(stderr, "[GFX_VERTEX_REPLACEMENT_REUSE_TEST] exact-key=pass exclusive-checkout=pass"
	                     " allocation-cap=pass entry-cap=pass active-pending-reset=pass fallback=pass selector=pass status=clean\n");
	return true;
}

bool upload_direct(GraphicContext* ctx, VulkanBuffer* vk_obj, const uint64_t* vaddr, const uint64_t* size)
{
	void* data = nullptr;
	const VkResult map_result = vkMapMemory(ctx->device, vk_obj->memory.memory, vk_obj->memory.offset,
	                                      vk_obj->memory.requirements.size, 0, &data);
	if (map_result != VK_SUCCESS)
	{
		return false;
	}
	if (data == nullptr)
	{
		vkUnmapMemory(ctx->device, vk_obj->memory.memory);
		return false;
	}
	std::memcpy(data, reinterpret_cast<const void*>(*vaddr), static_cast<size_t>(*size));
	vkUnmapMemory(ctx->device, vk_obj->memory.memory);
	return true;
}

void publish_direct_upload(CommandBuffer* buffer, VulkanBuffer* vk_obj)
{
	EXIT_IF(buffer == nullptr || vk_obj == nullptr || vk_obj->buffer == nullptr);
	auto* command = buffer->GetBufferOutsideRenderPass();

	VkBufferMemoryBarrier barrier {};
	barrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	barrier.srcAccessMask       = VK_ACCESS_HOST_WRITE_BIT;
	barrier.dstAccessMask       = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer               = vk_obj->buffer;
	barrier.offset               = 0;
	barrier.size                 = VK_WHOLE_SIZE;

	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT,
	                     VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, nullptr, 1, &barrier, 0, nullptr);
}

void upload_staged(GraphicContext* ctx, VulkanBuffer* vk_obj, const uint64_t* vaddr, const uint64_t* size)
{
	VulkanBuffer staging_buffer {};
	staging_buffer.usage           = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	staging_buffer.memory.property = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

	VulkanCreateBuffer(ctx, *size, &staging_buffer);
	EXIT_NOT_IMPLEMENTED(staging_buffer.buffer == nullptr);

	void* data = nullptr;
	VulkanMapMemory(ctx, &staging_buffer.memory, &data);
	memcpy(data, reinterpret_cast<void*>(*vaddr), *size);
	VulkanUnmapMemory(ctx, &staging_buffer.memory);

	UtilCopyBuffer(&staging_buffer, vk_obj, *size);

	VulkanDeleteBuffer(ctx, &staging_buffer);
}

void upload(GraphicContext* ctx, CommandBuffer* recording_buffer, VulkanBuffer* vk_obj, const uint64_t* vaddr,
	        const uint64_t* size, int vaddr_num, bool dynamic_update, bool allow_direct)
{
	EXIT_IF(vaddr_num != 1 || size == nullptr || vaddr == nullptr || *vaddr == 0 || *size == 0);
	EXIT_IF(ctx == nullptr || vk_obj == nullptr);

	if (dynamic_update)
	{
		g_dynamic_vertex_telemetry.updates.fetch_add(1, std::memory_order_relaxed);
	}

	if (allow_direct && upload_direct(ctx, vk_obj, vaddr, size))
	{
		publish_direct_upload(recording_buffer, vk_obj);
		g_dynamic_vertex_telemetry.direct_uploads.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	if (allow_direct)
	{
		g_dynamic_vertex_telemetry.map_fallbacks.fetch_add(1, std::memory_order_relaxed);
	}

	upload_staged(ctx, vk_obj, vaddr, size);
	if (dynamic_update)
	{
		g_dynamic_vertex_telemetry.staged_uploads.fetch_add(1, std::memory_order_relaxed);
	}
}

} // namespace

bool VertexBufferDynamicHostVisibleEnabled()
{
	static const bool enabled = []()
	{
		const char* value = std::getenv("LUMEN_VULKAN_DYNAMIC_VERTEX_HOST_VISIBLE");
		if (value == nullptr)
		{
			return LUMEN_VULKAN_DYNAMIC_VERTEX_HOST_VISIBLE_DEFAULT != 0;
		}
		return std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

bool VertexBufferReportDynamicHostVisibleFinal()
{
	if (g_dynamic_vertex_telemetry.finalized.exchange(true, std::memory_order_acq_rel))
	{
		return g_dynamic_vertex_telemetry.finalize_result.load(std::memory_order_acquire);
	}

	const bool requested = VertexBufferDynamicHostVisibleEnabled();
	const uint64_t updates = g_dynamic_vertex_telemetry.updates.load(std::memory_order_acquire);
	const uint64_t host_visible_allocations =
	    g_dynamic_vertex_telemetry.host_visible_allocations.load(std::memory_order_acquire);
	const uint64_t host_visible_reuses = g_dynamic_vertex_telemetry.host_visible_reuses.load(std::memory_order_acquire);
	const uint64_t direct_uploads = g_dynamic_vertex_telemetry.direct_uploads.load(std::memory_order_acquire);
	const uint64_t staged_uploads = g_dynamic_vertex_telemetry.staged_uploads.load(std::memory_order_acquire);
	const uint64_t allocation_fallbacks =
	    g_dynamic_vertex_telemetry.allocation_fallbacks.load(std::memory_order_acquire);
	const uint64_t map_fallbacks = g_dynamic_vertex_telemetry.map_fallbacks.load(std::memory_order_acquire);
	const uint64_t context_fallbacks =
	    g_dynamic_vertex_telemetry.context_fallbacks.load(std::memory_order_acquire);
	const bool clean = direct_uploads + staged_uploads == updates &&
	                   host_visible_allocations + host_visible_reuses == direct_uploads + map_fallbacks &&
	                   allocation_fallbacks + map_fallbacks + context_fallbacks <= staged_uploads &&
	                   (requested || (host_visible_allocations == 0 && host_visible_reuses == 0 && direct_uploads == 0 &&
	                                  allocation_fallbacks == 0 && map_fallbacks == 0 && context_fallbacks == 0));

	std::fprintf(stderr,
	             "[VULKAN_DYNAMIC_VERTEX_HOST_VISIBLE_FINAL] requested=%s default=%u enabled=%u effective=%u updates=%llu "
	             "host_visible_allocations=%llu host_visible_reuses=%llu direct_uploads=%llu staged_uploads=%llu "
	             "allocation_fallbacks=%llu map_fallbacks=%llu context_fallbacks=%llu status=%s coverage=%s\n",
	             dynamic_vertex_selector_receipt(), LUMEN_VULKAN_DYNAMIC_VERTEX_HOST_VISIBLE_DEFAULT,
	             requested ? 1u : 0u, direct_uploads != 0 ? 1u : 0u,
	             static_cast<unsigned long long>(updates), static_cast<unsigned long long>(host_visible_allocations),
	             static_cast<unsigned long long>(host_visible_reuses),
	             static_cast<unsigned long long>(direct_uploads), static_cast<unsigned long long>(staged_uploads),
	             static_cast<unsigned long long>(allocation_fallbacks), static_cast<unsigned long long>(map_fallbacks),
	             static_cast<unsigned long long>(context_fallbacks), clean ? "clean" : "invalid",
	             updates != 0 ? "positive" : "zero");
	std::fflush(stderr);
	g_dynamic_vertex_telemetry.finalize_result.store(clean, std::memory_order_release);
	return clean;
}

bool VertexBufferResetReplacementCache(GraphicContext* ctx)
{
	if (ctx == nullptr || ctx->device == nullptr) return false;
	std::array<VulkanBuffer, VertexReplacementPoolMax> retired {};
	uint32_t retired_count = 0;
	{
		std::lock_guard<std::mutex> lock(g_vertex_replacements.mutex);
		auto& policy = g_vertex_replacements.policy;
		if (!policy.CanReset(ctx, ctx->device)) return false;
		for (uint32_t i = policy.count; i != 0;)
		{
			--i;
			if (policy.keys[i].owner != ctx) continue;
			retired[retired_count++] = g_vertex_replacements.buffers[i];
			policy.Remove(i);
			g_vertex_replacements.buffers[i] = g_vertex_replacements.buffers[policy.count];
			g_vertex_replacements.buffers[policy.count] = {};
		}
		g_vertex_replacements.drained += retired_count;
		if (g_vertex_replacements.completed != 0) g_vertex_replacements.ReportLocked("reset");
	}
	for (uint32_t i = 0; i < retired_count; ++i) VulkanDeleteBuffer(ctx, &retired[i]);
	return true;
}

void VertexBufferResetDynamicHostVisibleForNextSession()
{
	g_dynamic_vertex_telemetry.updates.store(0, std::memory_order_release);
	g_dynamic_vertex_telemetry.host_visible_allocations.store(0, std::memory_order_release);
	g_dynamic_vertex_telemetry.host_visible_reuses.store(0, std::memory_order_release);
	g_dynamic_vertex_telemetry.direct_uploads.store(0, std::memory_order_release);
	g_dynamic_vertex_telemetry.staged_uploads.store(0, std::memory_order_release);
	g_dynamic_vertex_telemetry.allocation_fallbacks.store(0, std::memory_order_release);
	g_dynamic_vertex_telemetry.map_fallbacks.store(0, std::memory_order_release);
	g_dynamic_vertex_telemetry.context_fallbacks.store(0, std::memory_order_release);
	g_dynamic_vertex_telemetry.finalize_result.store(true, std::memory_order_release);
	g_dynamic_vertex_telemetry.finalized.store(false, std::memory_order_release);
}

bool VertexBufferDynamicHostVisibleContractTest()
{
	if (!replacement_policy_contract_test()) return false;
	VkPhysicalDeviceMemoryProperties properties {};
	properties.memoryTypeCount = 3;
	properties.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	properties.memoryTypes[1].propertyFlags = HostVisibleVertexMemory;
	properties.memoryTypes[2].propertyFlags = HostVisibleDeviceLocalVertexMemory;

	const auto rollback = select_dynamic_vertex_memory(properties, 0x7u, false);
	const auto unified  = select_dynamic_vertex_memory(properties, 0x7u, true);
	const auto discrete = select_dynamic_vertex_memory(properties, 0x3u, true);
	const auto fallback = select_dynamic_vertex_memory(properties, 0x1u, true);
	const auto invalid  = select_dynamic_vertex_memory(properties, 0x0u, true);

	return rollback.valid && !rollback.host_visible && !rollback.allocation_fallback &&
	       rollback.property == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT && unified.valid && unified.host_visible &&
	       !unified.allocation_fallback && unified.property == HostVisibleDeviceLocalVertexMemory && discrete.valid &&
	       discrete.host_visible && !discrete.allocation_fallback && discrete.property == HostVisibleVertexMemory &&
	       fallback.valid && !fallback.host_visible && fallback.allocation_fallback &&
	       fallback.property == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT && !invalid.valid;
}

static void update_func(GraphicContext* ctx, const uint64_t* /*params*/, void* obj, const uint64_t* vaddr, const uint64_t* size,
	                    int vaddr_num)
{
	LUMEN_PROFILER_BLOCK("VertexBufferGpuObject::update_func");

	if (VertexBufferDynamicHostVisibleEnabled() && vaddr_num == 1 && size != nullptr && vaddr != nullptr && *vaddr != 0 &&
	    *size != 0)
	{
		g_dynamic_vertex_telemetry.context_fallbacks.fetch_add(1, std::memory_order_relaxed);
	}
	upload(ctx, nullptr, static_cast<VulkanBuffer*>(obj), vaddr, size, vaddr_num, true, false);
}

static void delete_retired_buffer(GraphicContext* ctx, void* obj)
{
	auto* vk_obj = static_cast<VulkanBuffer*>(obj);

	EXIT_IF(vk_obj == nullptr);
	EXIT_IF(vk_obj->buffer == nullptr);
	EXIT_IF(ctx == nullptr);

	VulkanDeleteBuffer(ctx, vk_obj);
	delete vk_obj;
}

static void update_with_buffer_func(GraphicContext* ctx, CommandBuffer* buffer, const uint64_t* /*params*/, void* obj,
	                                const uint64_t* vaddr, const uint64_t* size, int vaddr_num)
{
	LUMEN_PROFILER_BLOCK("VertexBufferGpuObject::update_with_buffer_func");

	EXIT_IF(buffer == nullptr);
	EXIT_IF(obj == nullptr);
	EXIT_IF(ctx == nullptr || vaddr_num != 1 || size == nullptr || vaddr == nullptr || *vaddr == 0 || *size == 0);

	auto* vk_obj = static_cast<VulkanBuffer*>(obj);
	const bool reuse_enabled = replacement_reuse_enabled();
	VertexReplacementOperation operation(reuse_enabled, true);

	VulkanBuffer replacement {};
	bool         host_visible        = false;
	bool         allocation_fallback = false;
	bool         reused = false;
	if (VertexBufferDynamicHostVisibleEnabled())
	{
		reused = reuse_enabled && acquire_vertex_replacement(ctx, *size, vk_obj->usage, &replacement);
		if (reused) { host_visible = true; }
		else
		{
			EXIT_NOT_IMPLEMENTED(!create_dynamic_replacement(ctx, *size, vk_obj->usage, &replacement, &host_visible,
			                                               &allocation_fallback));
			if (reuse_enabled)
			{
				std::lock_guard<std::mutex> lock(g_vertex_replacements.mutex);
				++g_vertex_replacements.allocations;
			}
		}
	} else
	{
		// Preserve the historical allocation and staged-upload calls at the
		// operational boundary selected by the exact-zero rollback.
		replacement.usage           = vk_obj->usage;
		replacement.memory.property = vk_obj->memory.property;
		VulkanCreateBuffer(ctx, *size, &replacement);
		EXIT_NOT_IMPLEMENTED(replacement.buffer == nullptr);
	}
	if (host_visible)
	{
		(reused ? g_dynamic_vertex_telemetry.host_visible_reuses : g_dynamic_vertex_telemetry.host_visible_allocations)
		    .fetch_add(1, std::memory_order_relaxed);
	} else if (allocation_fallback)
	{
		g_dynamic_vertex_telemetry.allocation_fallbacks.fetch_add(1, std::memory_order_relaxed);
	}

	upload(ctx, buffer, &replacement, vaddr, size, vaddr_num, true, host_visible);

	if (reuse_enabled)
	{
		// The sole buffered-update dispatch passes the existing GpuMemory block's
		// unchanged size. Different guest ranges create a new tracker object;
		// allocation padding must never substitute for this logical VkBuffer size.
		auto* retired = new RetiredVertexReplacement {*vk_obj,
		    {ctx, ctx->device, *size, vk_obj->usage, vk_obj->memory.property}};
		{
			std::lock_guard<std::mutex> lock(g_vertex_replacements.mutex);
			++g_vertex_replacements.policy.pending;
		}
		*vk_obj = replacement;
		// The GFX fence covers prior vertex consumers too. A canceled recording
		// must defer reuse until a later queue fence or the device-idle drain.
		buffer->RetireAfterQueueIdle(retired, recycle_retired_vertex_replacement);
		return;
	}

	auto* retired = new VulkanBuffer(*vk_obj);
	*vk_obj       = replacement;

	buffer->Retire(retired, delete_retired_buffer);
}

static void* create_impl(GraphicContext* ctx, CommandBuffer* buffer, const uint64_t* vaddr, const uint64_t* size,
                         int vaddr_num, VulkanMemory* mem)
{
	LUMEN_PROFILER_BLOCK("VertexBufferGpuObject::Create");

	EXIT_IF(vaddr_num != 1 || size == nullptr || vaddr == nullptr || *vaddr == 0);
	EXIT_IF(mem == nullptr);
	EXIT_IF(ctx == nullptr);

	auto* vk_obj = new VulkanBuffer;

	vk_obj->usage           = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	vk_obj->memory.property = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	vk_obj->buffer          = nullptr;

	VulkanCreateBuffer(ctx, *size, vk_obj);
	EXIT_NOT_IMPLEMENTED(vk_obj->buffer == nullptr);

	if (buffer == nullptr)
	{
		upload(ctx, nullptr, vk_obj, vaddr, size, vaddr_num, false, false);
	} else
	{
		UtilUploadVertexInputBuffer(ctx, buffer, vk_obj, reinterpret_cast<const void*>(*vaddr), *size);
	}

	return vk_obj;
}

static void* create_func(GraphicContext* ctx, const uint64_t* /*params*/, const uint64_t* vaddr, const uint64_t* size,
                         int vaddr_num, VulkanMemory* mem)
{
	return create_impl(ctx, nullptr, vaddr, size, vaddr_num, mem);
}

static void* create_with_buffer_func(GraphicContext* ctx, CommandBuffer* buffer, const uint64_t* /*params*/,
                                     const uint64_t* vaddr, const uint64_t* size, int vaddr_num, VulkanMemory* mem)
{
	EXIT_IF(buffer == nullptr);
	return create_impl(ctx, buffer, vaddr, size, vaddr_num, mem);
}

static void delete_buffer_now(GraphicContext* ctx, void* obj)
{
	auto* vk_obj = reinterpret_cast<VulkanBuffer*>(obj);

	EXIT_IF(vk_obj == nullptr);
	EXIT_IF(vk_obj->buffer == nullptr);
	EXIT_IF(ctx == nullptr);

	VulkanDeleteBuffer(ctx, vk_obj);

	delete vk_obj;
}

static void delete_func(GraphicContext* ctx, void* obj, VulkanMemory* /*mem*/)
{
	LUMEN_PROFILER_BLOCK("VertexBufferGpuObject::delete_func");
	delete_buffer_now(ctx, obj);
}

static void delete_after_all_queues_idle(GraphicContext* ctx, void* obj, VulkanMemory* /*mem*/)
{
	delete_buffer_now(ctx, obj);
}

bool VertexBufferGpuObject::Equal(const uint64_t* /*other*/) const
{
	return true;
}

GpuObject::create_func_t VertexBufferGpuObject::GetCreateFunc() const
{
	return create_func;
}

GpuObject::create_with_buffer_func_t VertexBufferGpuObject::GetCreateWithBufferFunc() const
{
	return create_with_buffer_func;
}

GpuObject::delete_func_t VertexBufferGpuObject::GetDeleteFunc() const
{
	return delete_func;
}

GpuObject::delete_func_t VertexBufferGetDeleteAfterAllQueuesIdleFunc()
{
	return delete_after_all_queues_idle;
}

GpuObject::update_func_t VertexBufferGpuObject::GetUpdateFunc() const
{
	return update_func;
}

GpuObject::update_with_buffer_func_t VertexBufferGpuObject::GetUpdateWithBufferFunc() const
{
	return update_with_buffer_func;
}

} // namespace Lumen::Libs::Graphics

#endif // LUMEN_EMU_ENABLED
