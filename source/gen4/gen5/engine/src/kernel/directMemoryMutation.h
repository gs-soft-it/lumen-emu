// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "common/assert.h"
#if defined(LUMEN_PS5_DIRECT_FEX)
#include "DirectFexRuntime.h"
#endif

namespace Libs::LibKernel::Memory {
// Initialized before guest threads, cleared after their shutdown.
inline bool g_guest_memory_ready = false;

// Excludes compilation/cache mutation while an actual host mapping changes.
// This does not suspend guest CPUs and is never held over a GPU wait.
class DirectCodeMutation {
public:
	DirectCodeMutation(uint64_t address, uint64_t size)
	    : m_address(address), m_size(size <= UINT64_MAX - address ? size : 0) {
#if defined(LUMEN_PS5_DIRECT_FEX)
		m_active = g_guest_memory_ready;
		if (m_active && !Lumen::Ps5::DirectFex::BeginCodeMutation()) std::abort();
#endif
	}
	~DirectCodeMutation() {
#if defined(LUMEN_PS5_DIRECT_FEX)
		if (m_active && !Lumen::Ps5::DirectFex::EndCodeMutation(
		    m_address, m_size, m_size != 0)) std::abort();
#endif
	}
	void Publish(uint64_t address, uint64_t size) const {
#if defined(LUMEN_PS5_DIRECT_FEX)
		if (m_active && size != 0 && !Lumen::Ps5::DirectFex::Invalidate(address, size)) std::abort();
#else
		(void)address; (void)size;
#endif
	}
	DirectCodeMutation(const DirectCodeMutation&) = delete;
	DirectCodeMutation& operator=(const DirectCodeMutation&) = delete;
private:
	uint64_t m_address {}, m_size {};
	bool m_active {};
};
} // namespace Libs::LibKernel::Memory
