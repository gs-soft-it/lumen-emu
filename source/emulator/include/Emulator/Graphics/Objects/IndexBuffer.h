#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_INDEXBUFFER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_INDEXBUFFER_H_

#include "Lumen/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics {

struct GraphicContext;

void IndexBufferInit();
void IndexBufferDeleteAll(GraphicContext* ctx);
// Direct destruction is valid only after a completed all-queue idle. Normal
// tracker deletion must continue to use IndexBufferGpuObject::GetDeleteFunc().
GpuObject::delete_func_t IndexBufferGetDeleteAfterAllQueuesIdleFunc();
bool IndexBufferUpdateContractTest();

class IndexBufferGpuObject: public GpuObject
{
public:
	IndexBufferGpuObject()
	{
		check_hash = true;
		type       = Graphics::GpuMemoryObjectType::IndexBuffer;
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

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_INDEXBUFFER_H_ */
