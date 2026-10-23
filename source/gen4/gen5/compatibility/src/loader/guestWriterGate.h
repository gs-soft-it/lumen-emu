// Lumen, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "loader/guestHostWait.h"
#include <mutex>

namespace Loader::GuestExecution {
// Serialize PS5 VM, loader and GPU writer intent before the FEX barrier.
// Audited fresh/native writers acquire it before VM/cache locks and release it
// AFTER the native epoch closes. Strict borrowed entries retain core admission.
// It never authorizes memory access on its own.
inline std::recursive_mutex guest_writer_gate;

class GuestWriterGate final {
public:
    enum class Admission { Strict, FreshScalarEntry, NativeRootEntry };
    explicit GuestWriterGate(Admission admission = Admission::Strict) {
#if defined(LUMEN_PS5_DIRECT_FEX)
        (void)admission;
        guest_writer_gate.lock();
        locked_ = true;
#elif defined(LUMEN_PS5_FEXCORE)
        // Borrowed/nested entries retain the existing core admission policy;
        // they neither queue here nor donate an outer reader.
        if (admission == Admission::Strict) return;
        if (guest_writer_gate.try_lock()) { locked_ = true; return; }
        if (admission == Admission::FreshScalarEntry) {
            // Caller has only opaque addresses/scalars or native snapshots.
            // No epoch owner needs this gate to close; readmission while
            // holding writer intent is safe and prevents admission starvation.
            GuestHostWaitScope wait;
            if (!wait.Acquired() && LumenAndroidFexCoreRetainGuestDataLeaseV1() != 0) {
                // A nested/borrowed HLE cannot donate its retained reader.
                // Waiting here would retain precisely the hazard that the
                // gate's exclusive writer is waiting to drain. Native entry
                // has no HLE reader; keep its ordinary gate serialization.
                if (LumenAndroidFexCoreReleaseGuestDataLeaseV1() == 0)
                    LUMEN_PS5_GUEST_COORDINATION_FAILURE("PS5 writer-gate reader proof release failed\n");
                acquired_ = false;
                return;
            }
            guest_writer_gate.lock();
            locked_ = true;
        } else if (admission == Admission::NativeRootEntry) {
            // Only an audited native root before guest Run or a GPU worker.
            // No guest execution or HLE reader is retained by this caller.
            guest_writer_gate.lock();
            locked_ = true;
        }
#else
        (void)admission;
#endif
    }
    bool Acquired() const noexcept { return acquired_; }
    ~GuestWriterGate() {
#if defined(LUMEN_PS5_FEXCORE)
        if (locked_) guest_writer_gate.unlock();
#endif
    }
    GuestWriterGate(const GuestWriterGate&) = delete;
    GuestWriterGate& operator=(const GuestWriterGate&) = delete;
private:
    bool locked_ = false;
    bool acquired_ = true;
};
} // namespace Loader::GuestExecution
