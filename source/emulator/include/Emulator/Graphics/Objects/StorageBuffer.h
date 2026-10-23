#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_STORAGEBUFFER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_STORAGEBUFFER_H_

#include "Lumen/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics {

struct StorageVulkanBuffer;
class CommandBuffer;
class GuestWriteDestination;
struct GraphicsGuestLeaseResult;

class StorageBufferGpuObject: public GpuObject
{
public:
	StorageBufferGpuObject(uint64_t stride, uint64_t num_records, bool ronly)
	{
		params[0]  = stride;
		params[1]  = num_records;
		check_hash = true;
		read_only  = ronly;
		type       = Graphics::GpuMemoryObjectType::StorageBuffer;
	}

	bool Equal(const uint64_t* other) const override;

	[[nodiscard]] create_func_t              GetCreateFunc() const override;
	[[nodiscard]] create_from_objects_func_t GetCreateFromObjectsFunc() const override { return nullptr; };
	[[nodiscard]] write_back_func_t          GetWriteBackFunc() const override;
	[[nodiscard]] delete_func_t              GetDeleteFunc() const override;
	[[nodiscard]] update_func_t              GetUpdateFunc() const override;
	[[nodiscard]] update_with_buffer_func_t  GetUpdateWithBufferFunc() const override;
};

void StorageBufferSet(CommandBuffer* cmd_buffer, StorageVulkanBuffer* buffer);

// The caller captures the destination before its GPU wait and establishes
// host visibility before publishing from the persistent coherent mapping.
bool StorageBufferWriteBackToGuest(StorageVulkanBuffer* buffer, const GuestWriteDestination& destination,
                                  GraphicsGuestLeaseResult* result = nullptr);
bool StorageBufferGuestWriteContractTest();

// Caller has detached the canonical owner after proving every recorded use
// completed. This release never waits for a command processor.
void StorageBufferReleaseCompleted(GraphicContext* ctx, StorageVulkanBuffer* buffer);

} // namespace Lumen::Libs::Graphics

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_STORAGEBUFFER_H_ */
