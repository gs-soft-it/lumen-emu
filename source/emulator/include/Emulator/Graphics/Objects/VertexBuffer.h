#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_VERTEXBUFFER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_VERTEXBUFFER_H_

#include "Lumen/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics {

// Process-latched runtime selector. The compile-time default is supplied by
// the platform build, while only the byte-exact value "1" enables an explicit
// override. Exact zero, empty and malformed values select the rollback path.
bool VertexBufferDynamicHostVisibleEnabled();

// Emits one process/session receipt for the dynamic vertex upload policy.
// Repeatable runtimes must reset the counters only at their quiescent session
// boundary.
bool VertexBufferReportDynamicHostVisibleFinal();
void VertexBufferResetDynamicHostVisibleForNextSession();

// Replacement backing cache (default ON; LUMEN_VULKAN_VERTEX_REPLACEMENT_REUSE=0 disables).
// Call only with a live device and quiescent graphics workers, after command
// retirement. Refuses reset while an update or retirement is active/pending.
bool VertexBufferResetReplacementCache(GraphicContext* ctx);

// Pure selector/allocation-policy coverage used by the shared graphics-state
// test. It does not require a Vulkan device.
bool VertexBufferDynamicHostVisibleContractTest();

// Direct destruction is valid only after a completed all-queue idle. Normal
// tracker deletion must continue to use VertexBufferGpuObject::GetDeleteFunc().
GpuObject::delete_func_t VertexBufferGetDeleteAfterAllQueuesIdleFunc();

class VertexBufferGpuObject: public GpuObject
{
public:
	VertexBufferGpuObject()
	{
		check_hash = true;
		type       = Graphics::GpuMemoryObjectType::VertexBuffer;
	}

	bool Equal(const uint64_t* other) const override;

	[[nodiscard]] create_func_t              GetCreateFunc() const override;
	[[nodiscard]] create_with_buffer_func_t  GetCreateWithBufferFunc() const override;
	[[nodiscard]] create_from_objects_func_t GetCreateFromObjectsFunc() const override { return nullptr; };
	[[nodiscard]] write_back_func_t          GetWriteBackFunc() const override { return nullptr; };
	[[nodiscard]] delete_func_t              GetDeleteFunc() const override;
	[[nodiscard]] update_func_t              GetUpdateFunc() const override;
	[[nodiscard]] update_with_buffer_func_t  GetUpdateWithBufferFunc() const override;
};

} // namespace Lumen::Libs::Graphics

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_VERTEXBUFFER_H_ */
