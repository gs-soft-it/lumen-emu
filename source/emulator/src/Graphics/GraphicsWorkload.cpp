#include "Emulator/Graphics/Graphics.h"

#include "Lumen/Core/DbgAssert.h"
#include "Emulator/Loader/RuntimeLinker.h"

#include <cstring>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics::Gen4 {
namespace {

template <typename T> void WriteWorkloadOutput(T* destination, T value)
{
#if defined(__aarch64__) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
	if (!Loader::WriteGuestMemory(reinterpret_cast<uint64_t>(destination), &value, sizeof(value)))
	{
		// No native error result is established for an inaccessible nonnull
		// output. Stop before returning a guest result or making a partial write.
		EXIT("Gnm workload output is not writable");
	}
#else
	std::memcpy(destination, &value, sizeof(value));
#endif
}

} // namespace

// The approved PS4-native contract is stateless: these values do not name
// allocated resources, and the nonnull name is not read or retained.
uint32_t LUMEN_SYSV_ABI GraphicsCreateWorkloadStream(const char* name, uint32_t* stream)
{
	if (name == nullptr || stream == nullptr) return 3;
	WriteWorkloadOutput(stream, uint32_t {1});
	return 0;
}

uint32_t LUMEN_SYSV_ABI GraphicsDestroyWorkloadStream(uint32_t stream)
{
	return stream >= 1 && stream <= 15 ? 0 : 1;
}

uint32_t LUMEN_SYSV_ABI GraphicsBeginWorkload(uint32_t stream, uint64_t* workload)
{
	if (workload == nullptr) return 3;
	const bool valid = stream <= 15;
	WriteWorkloadOutput(workload, uint64_t {valid ? 1u : 0u});
	return valid ? 0 : 1;
}

uint32_t LUMEN_SYSV_ABI GraphicsEndWorkload(uint64_t workload)
{
	return workload == 0 || (workload >> 56) >= 16 ? 2 : 0;
}

int LUMEN_SYSV_ABI GraphicsSubmitCommandBuffersForWorkload(uint64_t workload, uint32_t count, void* dcb_gpu_addrs[],
                                                         const uint32_t* dcb_sizes_in_bytes, void* ccb_gpu_addrs[],
                                                         const uint32_t* ccb_sizes_in_bytes)
{
	(void)workload;
	return GraphicsSubmitCommandBuffers(count, dcb_gpu_addrs, dcb_sizes_in_bytes, ccb_gpu_addrs, ccb_sizes_in_bytes);
}

int LUMEN_SYSV_ABI GraphicsSubmitAndFlipCommandBuffersForWorkload(uint64_t workload, uint32_t count, void* dcb_gpu_addrs[],
                                                                const uint32_t* dcb_sizes_in_bytes, void* ccb_gpu_addrs[],
                                                                const uint32_t* ccb_sizes_in_bytes, int handle, int index,
                                                                int flip_mode, int64_t flip_arg)
{
	(void)workload;
	return GraphicsSubmitAndFlipCommandBuffers(count, dcb_gpu_addrs, dcb_sizes_in_bytes, ccb_gpu_addrs, ccb_sizes_in_bytes,
	                                           handle, index, flip_mode, flip_arg);
}

} // namespace Lumen::Libs::Graphics::Gen4

#endif // LUMEN_EMU_ENABLED
