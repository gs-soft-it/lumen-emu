// SPDX-License-Identifier: MIT
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <optional>
#include "core/libraries/kernel/umtx.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/kernel/sync/address_wait_queue.h"
#include "core/libraries/kernel/sync/host_wait.h"
#include "core/libraries/kernel/time.h"
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
#include "core/fex/fex_guest_engine.h"
#endif

namespace Libraries::Kernel {
s32* PS4_SYSV_ABI __Error();
namespace {
AddressWaitQueue waiters;
using Clock = std::chrono::steady_clock;
s32 Error(s32 error) { *__Error() = error; return -1; }
bool Valid(u64 address, size_t size, bool write = false) {
    if (!address || address % size || address > UINT64_MAX - size) return false;
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    return Core::GuestCpu::ValidateHlePointer(reinterpret_cast<void*>(address), size, write);
#else
    return true;
#endif
}
template<typename Word>
std::optional<Word> ReadWord(u64 address) {
    if (!Valid(address, sizeof(Word))) return std::nullopt;
    return std::atomic_ref<Word>{*reinterpret_cast<Word*>(address)}.load(std::memory_order_acquire);
}
bool AdjustHint(u64 address, bool add) {
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    Core::Fex::MemoryMutationScope publication;
    if (!publication.Valid()) return false;
#endif
    if (!Valid(address, 4, true)) return false;
    auto word = std::atomic_ref<u32>{*reinterpret_cast<u32*>(address)};
    auto value = word.load();
    do {
        if (add && value == UINT32_MAX) return false;
    } while (!word.compare_exchange_weak(value, add ? value + 1 : value ? value - 1 : 0));
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    publication.Publish(address, 4);
#endif
    return true;
}
bool ReadDeadline(u64 size, u64 pointer, Clock::time_point& deadline) {
    deadline = Clock::time_point::max();
    if (!pointer) return true;
    // The bare timespec profile is shared by the supported word and semaphore
    // operations. Extended clock/absolute-time records must not be misread.
    if (size != 0 && size != sizeof(OrbisKernelTimespec)) { Error(POSIX_EOPNOTSUPP); return false; }
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    if (!Core::GuestCpu::ValidateHlePointer(reinterpret_cast<void*>(pointer), sizeof(OrbisKernelTimespec), false)) {
        Error(POSIX_EFAULT); return false;
    }
#endif
    OrbisKernelTimespec value;
    std::memcpy(&value, reinterpret_cast<void*>(pointer), sizeof(value));
    if (value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1000000000) {
        Error(POSIX_EINVAL); return false;
    }
    const auto now = Clock::now();
    const auto maximum = std::chrono::duration_cast<std::chrono::seconds>(Clock::time_point::max() - now).count();
    if (value.tv_sec >= maximum) return true;
    deadline = now + std::chrono::seconds{value.tv_sec} + std::chrono::nanoseconds{value.tv_nsec};
    return true;
}
}

s32 PS4_SYSV_ABI posix_umtx_op(u64 object, s32 operation, u64 value, u64 address1, u64 address2) {
    // ABI operation values are FreeBSD/Orbis UMTX_OP_* values. The owner/robust
    // mutex and rwlock protocols are separate from this wait-word interface.
    if (!object) return Error(POSIX_EFAULT);
    switch (operation) {
    case 3: case 16: // WAKE / WAKE_PRIVATE
        waiters.Wake(object, value); return 0;
    case 20: case 24: // SEM_WAKE / SEM2_WAKE: userspace consumes the actual token.
        waiters.Wake(object, SIZE_MAX); return 0;
    case 21: { // NWAKE_PRIVATE: obj points to an array of user wait addresses.
        if (value > SIZE_MAX / sizeof(u64)) return Error(POSIX_EINVAL);
        if (object > UINT64_MAX - value * sizeof(u64)) return Error(POSIX_EFAULT);
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
        if (value && !Core::GuestCpu::ValidateHlePointer(reinterpret_cast<void*>(object), value * sizeof(u64), false))
            return Error(POSIX_EFAULT);
#endif
        for (u64 i = 0; i < value; ++i) {
            u64 address; std::memcpy(&address, reinterpret_cast<void*>(object + i * sizeof(u64)), sizeof(address));
            waiters.Wake(address, SIZE_MAX);
        }
        return 0;
    }
    case 2: case 11: case 15: case 19: case 23: break;
    default: return Error(POSIX_EOPNOTSUPP);
    }
    Clock::time_point deadline;
    if (!ReadDeadline(address1, address2, deadline)) return -1;
    const bool semaphore = operation == 19;
    auto ticket = waiters.Subscribe(object);
    if (semaphore && !AdjustHint(object, true)) return Error(POSIX_EFAULT);
    s32 result{};
    for (;;) {
        bool ready{};
        if (operation == 2) {
            auto word = ReadWord<u64>(object);
            if (!word) { result = Error(POSIX_EFAULT); break; }
            ready = *word != value;
        } else {
            if (semaphore && object > UINT64_MAX - 4) { result = Error(POSIX_EFAULT); break; }
            auto word = ReadWord<u32>(object + (semaphore ? 4 : 0));
            if (!word) { result = Error(POSIX_EFAULT); break; }
            ready = semaphore ? static_cast<s32>(*word) > 0 : *word != static_cast<u32>(value);
        }
        if (ready || ticket.Released()) break;
        const auto now = Clock::now();
        if (now >= deadline) { result = Error(POSIX_ETIMEDOUT); break; }
        const auto slice = std::min<std::chrono::nanoseconds>(std::chrono::milliseconds{25}, deadline - now);
        NativeHostWait([&] { ticket.WaitFor(slice); }); // native locks end before readmission
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
        Core::Fex::FlushPendingGuestOrbisSignal();
#endif
    }
    if (semaphore && !AdjustHint(object, false)) return Error(POSIX_EFAULT);
    return result;
}

} // namespace Libraries::Kernel
