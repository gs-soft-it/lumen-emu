#include "Emulator/Graphics/Objects/IndexBuffer.h"

#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/Threads.h"
#include "Lumen/Core/Vector.h"

#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Profiler.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics {

class IndexBufferManager
{
public:
	IndexBufferManager() { EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread()); }
	virtual ~IndexBufferManager() { LUMEN_NOT_IMPLEMENTED; }
	LUMEN_CLASS_NO_COPY(IndexBufferManager);

	void RegisterForDelete(VulkanBuffer* buf)
	{
		Core::LockGuard lock(m_mutex);

		m_buffers.Add(buf);
	}

	void DeleteAll(GraphicContext* ctx)
	{
		LUMEN_PROFILER_BLOCK("IndexBufferManager::DeleteAll");

		Core::LockGuard lock(m_mutex);

		for (auto* vk_obj: m_buffers)
		{
			EXIT_IF(vk_obj == nullptr);
			EXIT_IF(vk_obj->buffer == nullptr);
			EXIT_IF(ctx == nullptr);

			VulkanDeleteBuffer(ctx, vk_obj);

			delete vk_obj;
		}

		m_buffers.Clear();
	}

private:
	Core::Mutex m_mutex;

	Vector<VulkanBuffer*> m_buffers;
};

static IndexBufferManager* g_index_buffer_manager = nullptr;

void IndexBufferInit()
{
	EXIT_IF(g_index_buffer_manager != nullptr);

	g_index_buffer_manager = new IndexBufferManager;
}

void IndexBufferDeleteAll(GraphicContext* ctx)
{
	EXIT_IF(g_index_buffer_manager == nullptr);

	g_index_buffer_manager->DeleteAll(ctx);
}

static void create_snapshot(GraphicContext* ctx, CommandBuffer* buffer, const uint64_t* vaddr, const uint64_t* size,
                            int vaddr_num, VulkanBuffer* vk_obj)
{
	EXIT_IF(vaddr_num != 1 || size == nullptr || vaddr == nullptr || *vaddr == 0);
	EXIT_IF(ctx == nullptr);
	EXIT_IF(vk_obj == nullptr || vk_obj->buffer != nullptr);

	vk_obj->usage           = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
	vk_obj->memory.property = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	vk_obj->buffer          = nullptr;

	VulkanCreateBuffer(ctx, *size, vk_obj);
	EXIT_NOT_IMPLEMENTED(vk_obj->buffer == nullptr);

	if (buffer != nullptr)
	{
		UtilUploadVertexInputBuffer(ctx, buffer, vk_obj, reinterpret_cast<const void*>(*vaddr), *size);
		return;
	}

	VulkanBuffer staging_buffer {};
	staging_buffer.usage           = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	staging_buffer.memory.property = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

	VulkanCreateBuffer(ctx, *size, &staging_buffer);
	EXIT_NOT_IMPLEMENTED(staging_buffer.buffer == nullptr);

	void* data = nullptr;
	// vkMapMemory(ctx->device, staging_buffer.memory.memory, staging_buffer.memory.offset, *size, 0, &data);
	VulkanMapMemory(ctx, &staging_buffer.memory, &data);
	memcpy(data, reinterpret_cast<void*>(*vaddr), *size);
	// vkUnmapMemory(ctx->device, staging_buffer.memory.memory);
	VulkanUnmapMemory(ctx, &staging_buffer.memory);

	UtilCopyBuffer(&staging_buffer, vk_obj, *size);

	VulkanDeleteBuffer(ctx, &staging_buffer);
}

static void* create_impl(GraphicContext* ctx, CommandBuffer* buffer, const uint64_t* vaddr, const uint64_t* size,
                         int vaddr_num, VulkanMemory* mem)
{
	LUMEN_PROFILER_BLOCK("IndexBufferGpuObject::Create");

	EXIT_IF(mem == nullptr);

	auto* vk_obj = new VulkanBuffer;
	create_snapshot(ctx, buffer, vaddr, size, vaddr_num, vk_obj);

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

static VulkanBuffer* publish_replacement(VulkanBuffer* current, const VulkanBuffer& replacement)
{
	EXIT_IF(current == nullptr);
	auto* retired = new VulkanBuffer(*current);
	*current      = replacement;
	return retired;
}

static void update_func(GraphicContext* ctx, const uint64_t* /*params*/, void* obj, const uint64_t* vaddr,
	                    const uint64_t* size, int vaddr_num)
{
	LUMEN_PROFILER_BLOCK("IndexBufferGpuObject::update_func");

	auto* current = static_cast<VulkanBuffer*>(obj);
	VulkanBuffer replacement {};
	create_snapshot(ctx, nullptr, vaddr, size, vaddr_num, &replacement);
	auto* retired = publish_replacement(current, replacement);

	EXIT_IF(g_index_buffer_manager == nullptr);
	g_index_buffer_manager->RegisterForDelete(retired);
}

static void delete_retired_buffer(GraphicContext* ctx, void* obj)
{
	auto* vk_obj = static_cast<VulkanBuffer*>(obj);
	EXIT_IF(ctx == nullptr || vk_obj == nullptr || vk_obj->buffer == nullptr);
	VulkanDeleteBuffer(ctx, vk_obj);
	delete vk_obj;
}

static void delete_after_all_queues_idle(GraphicContext* ctx, void* obj, VulkanMemory* /*mem*/)
{
	delete_retired_buffer(ctx, obj);
}

static void update_with_buffer_func(GraphicContext* ctx, CommandBuffer* buffer, const uint64_t* /*params*/, void* obj,
	                                const uint64_t* vaddr, const uint64_t* size, int vaddr_num)
{
	LUMEN_PROFILER_BLOCK("IndexBufferGpuObject::update_with_buffer_func");

	EXIT_IF(buffer == nullptr);
	auto* current = static_cast<VulkanBuffer*>(obj);
	VulkanBuffer replacement {};
	create_snapshot(ctx, buffer, vaddr, size, vaddr_num, &replacement);
	auto* retired = publish_replacement(current, replacement);
	buffer->Retire(retired, delete_retired_buffer);
}

static void delete_func(GraphicContext* /*ctx*/, void* obj, VulkanMemory* /*mem*/)
{
	LUMEN_PROFILER_BLOCK("IndexBufferGpuObject::delete_func");

	EXIT_IF(g_index_buffer_manager == nullptr);

	auto* vk_obj = reinterpret_cast<VulkanBuffer*>(obj);

	g_index_buffer_manager->RegisterForDelete(vk_obj);
}

bool IndexBufferGpuObject::Equal(const uint64_t* /*other*/) const
{
	return true;
}

GpuObject::create_func_t IndexBufferGpuObject::GetCreateFunc() const
{
	return create_func;
}

GpuObject::create_with_buffer_func_t IndexBufferGpuObject::GetCreateWithBufferFunc() const
{
	return create_with_buffer_func;
}

GpuObject::delete_func_t IndexBufferGpuObject::GetDeleteFunc() const
{
	return delete_func;
}

GpuObject::delete_func_t IndexBufferGetDeleteAfterAllQueuesIdleFunc()
{
	return delete_after_all_queues_idle;
}

GpuObject::update_func_t IndexBufferGpuObject::GetUpdateFunc() const
{
	return update_func;
}

GpuObject::update_with_buffer_func_t IndexBufferGpuObject::GetUpdateWithBufferFunc() const
{
	return update_with_buffer_func;
}

bool IndexBufferUpdateContractTest()
{
	IndexBufferGpuObject object;
	VulkanBuffer         current {};
	VulkanBuffer         replacement {};
	current.memory.unique_id     = 17;
	current.usage                = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
	replacement.memory.unique_id = 23;
	replacement.usage            = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
	auto* stable                  = &current;
	auto* retired                 = publish_replacement(&current, replacement);
	const bool valid = stable == &current && current.memory.unique_id == 23 &&
	                   current.usage == (VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT) &&
	                   retired != nullptr && retired->memory.unique_id == 17 &&
	                   retired->usage == VK_BUFFER_USAGE_INDEX_BUFFER_BIT && object.check_hash && !object.read_only &&
	                   object.GetUpdateFunc() != nullptr && object.GetUpdateWithBufferFunc() != nullptr &&
	                   object.GetDeleteFunc() != nullptr && IndexBufferGetDeleteAfterAllQueuesIdleFunc() != nullptr &&
	                   object.GetDeleteFunc() != IndexBufferGetDeleteAfterAllQueuesIdleFunc();
	delete retired;
	return valid;
}

} // namespace Lumen::Libs::Graphics

#endif // LUMEN_EMU_ENABLED
