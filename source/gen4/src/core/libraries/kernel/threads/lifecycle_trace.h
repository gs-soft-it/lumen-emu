// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <chrono>
#include <io.h>
#else
#include <time.h>
#include <unistd.h>
#endif

namespace Libraries::Kernel::ThreadLifecycleTrace {

// Independent budgets prevent mutex initialization from hiding thread shutdown.
// The final slot in each budget is a visible truncation marker. Mutex windows
// reset only initialization/destruction budgets, at most eight times; thread
// and error budgets are process-wide. Maximum output is 26632 lines per process.
enum class Kind : unsigned { Thread, MutexInit, MutexDestroy, MutexError, MutexContention, Window };
inline constexpr std::array<unsigned, 6> Limits{1024, 1536, 1280, 192, 64, 9};
inline std::array<std::atomic<unsigned>, 6> Counts{};
inline constexpr unsigned MaxMutexWindows = 8;
inline std::atomic<unsigned> MutexEpoch{};
inline std::atomic<std::uint64_t> Sequence{};
inline std::atomic<bool> Enabled{};
static_assert(std::atomic<unsigned>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

// Call during export registration, before installing cancellation signals. No
// environment access or static-initializer guard is needed on the signal path.
inline void Initialize() {
    static const bool initialized = [] {
        const char* value = std::getenv("LUMEN_THREAD_LIFECYCLE_TRACE");
        Enabled.store(value != nullptr && std::strcmp(value, "1") == 0,
                      std::memory_order_release);
        return true;
    }();
    (void)initialized;
}

class Record {
public:
    Record(Kind kind, const char* event, unsigned epoch = ~0U) noexcept {
        if (!Enabled.load(std::memory_order_acquire)) return;
        const auto index = static_cast<unsigned>(kind);
        auto& count = Counts[index];
        unsigned position = count.load(std::memory_order_relaxed);
        do {
            if (position >= Limits[index]) return;
        } while (!count.compare_exchange_weak(position, position + 1,
                                              std::memory_order_relaxed));
        active = true;
        Text("[THREAD_LIFECYCLE] ");
        Number("seq", Sequence.fetch_add(1, std::memory_order_relaxed));
        const int saved_errno = errno;
#ifdef _WIN32
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        Number("monotonic_ns", static_cast<std::uint64_t>(ns));
#else
        timespec now{};
        if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
            Number("monotonic_ns", static_cast<std::uint64_t>(now.tv_sec) * 1000000000ULL +
                                       static_cast<std::uint64_t>(now.tv_nsec));
        }
#endif
        errno = saved_errno;
        Number("category", index);
        Number("epoch", epoch == ~0U ? MutexEpoch.load(std::memory_order_acquire) : epoch);
        Text("event=");
        if (position + 1 == Limits[index]) {
            Text("budget_exhausted ");
            Number("limit", Limits[index]);
            Submit();
            return;
        }
        Text(event);
        Text(" ");
    }

    Record& Pointer(const char* key, const void* value) noexcept {
        return Number(key, reinterpret_cast<std::uintptr_t>(value), 16);
    }

    Record& Number(const char* key, std::uint64_t value, unsigned base = 10) noexcept {
        if (!active) return *this;
        Text(key);
        Text("=");
        if (base == 16) Text("0x");
        char digits[32];
        unsigned count = 0;
        do {
            digits[count++] = "0123456789abcdef"[value % base];
            value /= base;
        } while (value != 0);
        while (count != 0) Character(digits[--count]);
        Text(" ");
        return *this;
    }

    Record& Name(const char* value) noexcept {
        return Label("name", value);
    }

    Record& Label(const char* key, const char* value) noexcept {
        if (!active || value == nullptr) return *this;
        Text(key);
        Text("=");
        // Only native literals or a live, owned native string are supplied.
        for (unsigned index = 0; index < 48 && value[index] != '\0'; ++index) {
            const unsigned char ch = value[index];
            Character(ch > ' ' && ch < 127 ? ch : '_');
        }
        Text(" ");
        return *this;
    }

    void Submit() noexcept {
        if (!active) return;
        active = false;
        Character('\n');
        const int saved_errno = errno;
        // No logger, allocation, mutex or target dereference: exit can run from
        // the native cancellation signal. Android redirects fd 2 to its crash log.
#ifdef _WIN32
        (void)::_write(2, buffer, static_cast<unsigned>(length));
#else
        (void)::write(STDERR_FILENO, buffer, length);
#endif
        errno = saved_errno;
    }

private:
    void Character(char ch) noexcept {
        if (length < sizeof(buffer)) buffer[length++] = ch;
    }
    void Text(const char* text) noexcept {
        while (*text != '\0' && length + 1 < sizeof(buffer)) Character(*text++);
    }
    char buffer[512]{};
    std::size_t length{};
    bool active{};
};

inline void BeginMutexWindow(const char* reason = "unspecified") noexcept {
    if (!Enabled.load(std::memory_order_acquire)) return;
    unsigned epoch = MutexEpoch.load(std::memory_order_relaxed);
    do {
        if (epoch >= MaxMutexWindows) return;
    } while (!MutexEpoch.compare_exchange_weak(epoch, epoch + 1, std::memory_order_acq_rel));
    // The API trigger is diagnostic only; no thread, mutex object or error
    // state is changed. Concurrent records may straddle a window boundary.
    Counts[static_cast<unsigned>(Kind::MutexInit)].store(0, std::memory_order_relaxed);
    Counts[static_cast<unsigned>(Kind::MutexDestroy)].store(0, std::memory_order_relaxed);
    Record(Kind::Window, "mutex.window.begin", epoch + 1)
        .Number("max_windows", MaxMutexWindows).Label("reason", reason).Submit();
}

inline int ThreadResult(const char* event, const void* caller, const void* target, int ret) noexcept {
    Record(Kind::Thread, event).Pointer("caller", caller).Pointer("target", target)
        .Number("ret_posix", static_cast<unsigned>(ret)).Submit();
    return ret;
}

inline int MutexResult(Kind kind, const char* event, const void* caller, const void* slot,
                       const void* object, int ret, const void* owner = nullptr) noexcept {
    Record(kind, event).Pointer("caller", caller).Pointer("slot", slot).Pointer("object", object)
        .Pointer("owner", owner).Number("ret_posix", static_cast<unsigned>(ret)).Submit();
    return ret;
}

} // namespace Libraries::Kernel::ThreadLifecycleTrace
