#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RESOURCE_PROOF_LIMITS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RESOURCE_PROOF_LIMITS_H_

#include <cstdint>

namespace Lumen::Libs::Graphics {

// Shared bounds for complete host resource proofs. Exceeding a bound refuses
// the proof; no caller may accept a truncated inventory or alias peer list.
struct ResourceProofLimits
{
	// The guaranteed inventory's worst logical records remain below 4 MiB:
	// 4096 * 48 + 65536 * 16 + 1280 * (2048 + 152) = 4061184 bytes.
	// A larger complete index is only a scan bound. Capture must account every
	// actual record, link, native backing, scratch and retained bank before copy;
	// reaching the larger bound never guarantees that its metadata fits.
	static constexpr uint32_t MaxGuaranteedLiveObjects = 1280;
	static constexpr uint32_t MaxLiveObjects = 1536;
	static constexpr uint32_t MaxHeaps = 4096;
	static constexpr uint32_t MaxSlots = 65536;
	static constexpr uint32_t MaxPlanes = 3;
	static constexpr uint32_t MaxLinks = 65536;
	static constexpr uint64_t MaxLogicalMetadataBytes = 4u * 1024u * 1024u;
	static constexpr uint32_t MaxSavedObjectBytes = 2048;
	static constexpr uint32_t MaxNativeRecordBytes = 152;
	static constexpr uint64_t MaxSourceBytes = 64u * 1024u * 1024u;
	static constexpr uint64_t MaxSourceBatchBytes = 128u * 1024u * 1024u;
	static constexpr uint64_t MaxDirectedAliasPairs = uint64_t(MaxLiveObjects) * (MaxLiveObjects - 1u);
	static_assert(MaxGuaranteedLiveObjects <= MaxLiveObjects);
};

// Overflow-safe accounting before any proof-owned dynamic allocation/copy.
// The logical byte budget counts records, native snapshots and link entries;
// allocator bookkeeping/capacity is not a content-authority claim.
inline bool ResourceProofAccumulate(uint64_t amount, uint64_t limit, uint64_t* total)
{
	if (total == nullptr || *total > limit || amount > limit - *total) return false;
	*total += amount;
	return true;
}

} // namespace Lumen::Libs::Graphics

#endif
