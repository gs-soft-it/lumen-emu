// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "kernel/memory.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

namespace Libs::LibKernel::Memory {

enum class GuestIoError { None, InvalidAddress, NoMemory };

// Kernel I/O cannot recover a GPU-watch fault in its copyin/copyout path. Keep
// identity buffers unchanged, but stage nonidentity buffers through user-space
// Read/WriteGuestMemory so the watched alias and guest permissions remain active.
class GuestIoBuffer {
public:
    static GuestIoError Validate(const void* pointer, uint64_t size, bool output) {
        if (size == 0) return GuestIoError::None;
        const auto guest = reinterpret_cast<uint64_t>(pointer);
        uint64_t host {};
        if (guest == 0 || size > std::numeric_limits<size_t>::max() ||
            !TranslateGuestMemory(guest, size, &host) ||
            (host != guest && !QueryTranslatedGuestAccess(guest, size, output)))
            return GuestIoError::InvalidAddress;
        return GuestIoError::None;
    }

    GuestIoError Prepare(const void* pointer, uint64_t size, bool output) {
        const auto error = Validate(pointer, size, output);
        if (error != GuestIoError::None) return error;
        m_guest = reinterpret_cast<uint64_t>(pointer);
        m_size = static_cast<size_t>(size);
        m_output = output;
        m_staged = size != 0 && IsGuestNonIdentity(m_guest, size);
        if (!m_staged) return GuestIoError::None;
        try {
            m_bytes.resize(m_size);
        } catch (const std::bad_alloc&) {
            return GuestIoError::NoMemory;
        } catch (const std::length_error&) {
            return GuestIoError::NoMemory;
        }
        if (!output && !ReadGuestMemory(nullptr, m_guest, m_bytes.data(), m_size))
            return GuestIoError::InvalidAddress;
        return GuestIoError::None;
    }

    void* Data() { return m_staged ? m_bytes.data() : reinterpret_cast<void*>(m_guest); }
    const void* Data() const {
        return m_staged ? m_bytes.data() : reinterpret_cast<const void*>(m_guest);
    }
    bool Commit(uint64_t bytes) const {
        if (!m_output || bytes > m_size) return false;
        return !m_staged || bytes == 0 ||
            WriteGuestMemory(nullptr, m_guest, m_bytes.data(), static_cast<size_t>(bytes));
    }

private:
    uint64_t m_guest {};
    size_t m_size {};
    bool m_output {};
    bool m_staged {};
    std::vector<std::byte> m_bytes;
};

} // namespace Libs::LibKernel::Memory
