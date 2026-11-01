// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#if defined(__ANDROID__)
#include <android/api-level.h>
#include <cerrno>
#include <cstdint>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

namespace Libraries::Net::Host {

// Older Android kernels have ppoll, but may lack epoll_pwait2. Polling the
// epoll descriptor preserves sub-millisecond timeouts without rounding them
// down to an immediate epoll_wait or up to a whole millisecond.
inline int EpollWaitMicrosecondsFallback(int fd, epoll_event* events, int maxevents,
                                         int timeout_us) {
    timespec deadline{};
    if (timeout_us > 0) {
        if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
            return -1;
        }
        deadline.tv_sec += timeout_us / 1000000;
        deadline.tv_nsec += static_cast<long>(timeout_us % 1000000) * 1000;
        if (deadline.tv_nsec >= 1000000000) {
            ++deadline.tv_sec;
            deadline.tv_nsec -= 1000000000;
        }
    }

    // Validate the epoll descriptor/event count and collect already-ready
    // events before sleeping, including for a zero timeout.
    int result = epoll_wait(fd, events, maxevents, 0);
    if (result != 0 || timeout_us == 0) {
        return result;
    }

    for (;;) {
        timespec remaining{};
        if (timeout_us > 0) {
            timespec now{};
            if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
                return -1;
            }
            remaining.tv_sec = deadline.tv_sec - now.tv_sec;
            remaining.tv_nsec = deadline.tv_nsec - now.tv_nsec;
            if (remaining.tv_nsec < 0) {
                --remaining.tv_sec;
                remaining.tv_nsec += 1000000000;
            }
            if (remaining.tv_sec < 0 ||
                (remaining.tv_sec == 0 && remaining.tv_nsec == 0)) {
                return 0;
            }
        }

        pollfd ready{fd, POLLIN, 0};
        result = ppoll(&ready, 1, timeout_us < 0 ? nullptr : &remaining, nullptr);
        if (result <= 0) {
            return result; // In particular, return EINTR to the guest without retrying.
        }
        if (ready.revents & POLLNVAL) {
            errno = EBADF;
            return -1;
        }
        result = epoll_wait(fd, events, maxevents, 0);
        if (result != 0) {
            return result;
        }
        // Another waiter can consume the ready event between ppoll and
        // epoll_wait. Continue with the original deadline, not a fresh timeout.
    }
}

inline int EpollWaitMicroseconds(int fd, epoll_event* events, int maxevents, int timeout_us) {
    static_assert(sizeof(time_t) == sizeof(std::int64_t), "Android runtime requires ARM64 time_t");
    // Android's app seccomp policy can reject newer syscalls even when the
    // kernel knows them. Only use the syscall on an OS that exposes its libc
    // wrapper; avoid a hard API35 symbol import in the API33-compatible DSO.
    if (android_get_device_api_level() >= 35) {
        const timespec timeout{timeout_us / 1000000,
                               static_cast<long>(timeout_us % 1000000) * 1000};
        constexpr size_t kernel_sigset_size = sizeof(std::uint64_t);
        const int result = static_cast<int>(syscall(SYS_epoll_pwait2, fd, events, maxevents,
                                                   timeout_us < 0 ? nullptr : &timeout,
                                                   nullptr, kernel_sigset_size));
        if (result != -1 || errno != ENOSYS) {
            return result;
        }
    }
    return EpollWaitMicrosecondsFallback(fd, events, maxevents, timeout_us);
}

} // namespace Libraries::Net::Host
#endif
