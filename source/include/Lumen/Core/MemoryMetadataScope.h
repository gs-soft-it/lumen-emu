// Lumen, 2026-10-17. SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <atomic>
#include <cstdint>

namespace Lumen::Core::VirtualMemory::Detail {
// Distinct from translator tokens (whose nonce may wrap). Saturation disables
// metadata reuse; no previously issued scope can ever become current again.
inline uint64_t NewMemoryMetadataScope() noexcept
{
	static std::atomic<uint64_t> next {1};
	auto value = next.load(std::memory_order_relaxed);
	while (value != UINT64_MAX)
	{
		if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) return value;
	}
	return 0;
}
} // namespace Lumen::Core::VirtualMemory::Detail
