#pragma once

#include "AndroidFexCoreRuntime.h"

#include <cstddef>
#include <cstdlib>

namespace Lumen::Android::FexCoreLab {

inline bool RelocationPatchEpochRequested(const char* value) noexcept {
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

// Amortize FEX admission across a bounded sequence of ordinary loader patches.
// This owns no Core VM gate, mapping, host protection or invalidation range.
// Every slot must still use PatchReplace: its nested publisher runs before
// returning, including code invalidation and release of its local VM gate.
// Only the final outer close disarms safepoints and readmits other guest runs.
// Never enter guest code or a guest callback while an epoch is open.
class RelocationPatchEpoch final {
public:
    static constexpr std::size_t MaxAttempts = 256;

    explicit RelocationPatchEpoch(bool enabled) noexcept : enabled_(enabled) {}
    ~RelocationPatchEpoch() { Close(); }
    RelocationPatchEpoch(const RelocationPatchEpoch&) = delete;
    RelocationPatchEpoch& operator=(const RelocationPatchEpoch&) = delete;

    bool AdmitNext() noexcept {
        if (failed_) return false;
        if (!enabled_) return true;
        if (attemptsInEpoch_ == MaxAttempts) Close();
        if (token_ == 0) {
            token_ = LumenAndroidFexCoreBeginGuestMemoryMutationV2(
                LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2);
            if (token_ == 0) {
                failed_ = true;
                return false;
            }
            ++epochs_;
        }
        ++attemptsInEpoch_;
        ++attempts_;
        return true;
    }

    void Close() noexcept {
        if (token_ == 0) return;
        // Inner PatchReplace calls already published every affected range.
        // An empty outer publication must neither invalidate again nor touch
        // Core's local gate. Failed close cannot safely reopen guest execution.
        if (LumenAndroidFexCoreEndGuestMemoryMutationV2(
                token_, 0, 0, 0, nullptr, nullptr) != 1) std::abort();
        token_ = 0;
        attemptsInEpoch_ = 0;
    }

    std::size_t Attempts() const noexcept { return attempts_; }
    std::size_t Epochs() const noexcept { return epochs_; }

private:
    const bool enabled_;
    bool failed_ = false;
    std::uint64_t token_ = 0;
    std::size_t attemptsInEpoch_ = 0;
    std::size_t attempts_ = 0;
    std::size_t epochs_ = 0;
};

} // namespace Lumen::Android::FexCoreLab
