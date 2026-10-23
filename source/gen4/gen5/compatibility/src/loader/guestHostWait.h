// Lumen, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "loader/guestCoordinationFailure.h"
#if defined(LUMEN_PS5_FEXCORE) && !defined(LUMEN_PS5_DIRECT_FEX)
#include "AndroidFexCoreRuntime.h"
#endif

#include <cstdint>

namespace Loader::GuestExecution {

// The complete top-level HLE callback must retain only native-owned state:
// snapshot native handles first, release guest/VM leases and local locks,
// then perform a native wait that cannot invoke guest code. Readmission at
// destruction precedes checked guest copyout or VM cleanup. An ineligible
// (including nested) callback keeps its existing reader; it never drops an
// outer frame's protection. A zero token is also ordinary outside guest Run.
class GuestHostWaitScope final {
public:
    GuestHostWaitScope() noexcept {
#if defined(LUMEN_PS5_FEXCORE) && !defined(LUMEN_PS5_DIRECT_FEX)
        token_ = LumenAndroidFexCoreBeginQuiescentHostWaitV1();
#endif
    }
    ~GuestHostWaitScope() {
#if defined(LUMEN_PS5_FEXCORE) && !defined(LUMEN_PS5_DIRECT_FEX)
        if (token_ != 0 && LumenAndroidFexCoreEndQuiescentHostWaitV1(token_) == 0)
            LUMEN_PS5_GUEST_COORDINATION_FAILURE("PS5 guest host-wait readmission failed\n");
#endif
    }
    // Direct Gen4 has no retained execution reader to suspend; Acquired stays
    // false and never advertises a shared-provider wait capability.
    // A caller that stages I/O may require successful reader suspension before
    // consuming data. Zero preserves the existing outer reader unchanged.
    bool Acquired() const noexcept { return token_ != 0; }
    GuestHostWaitScope(const GuestHostWaitScope&) = delete;
    GuestHostWaitScope& operator=(const GuestHostWaitScope&) = delete;
private:
    std::uint64_t token_ {};
};

} // namespace Loader::GuestExecution
