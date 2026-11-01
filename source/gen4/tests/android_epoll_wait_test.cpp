// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone Android/API33 correctness test; no emulator or game is required.
#include "core/libraries/network/android_epoll_wait.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <pthread.h>
#include <signal.h>
#include <thread>

#define REQUIRE(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "FAIL line %d: %s (errno=%d)\n", __LINE__, #condition, errno); \
        std::abort(); \
    } \
} while (false)

namespace {
using Wait = int (*)(int, epoll_event*, int, int);

std::int64_t NowNs() {
    timespec value{};
    REQUIRE(clock_gettime(CLOCK_MONOTONIC, &value) == 0);
    return value.tv_sec * 1000000000LL + value.tv_nsec;
}

void Delay(long ns) {
    timespec remaining{0, ns};
    while (nanosleep(&remaining, &remaining) != 0) {
        REQUIRE(errno == EINTR);
    }
}

void CheckWait(Wait wait) {
    const int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    REQUIRE(epoll_fd >= 0);
    epoll_event event{};
    REQUIRE(wait(epoll_fd, &event, 1, 0) == 0);
    const auto before = NowNs();
    REQUIRE(wait(epoll_fd, &event, 1, 137) == 0);
    REQUIRE(NowNs() - before >= 137000); // Must not truncate a sub-ms wait to zero.
    errno = 0;
    REQUIRE(wait(-1, &event, 1, 1000) == -1 && errno == EBADF);
    errno = 0;
    REQUIRE(wait(epoll_fd, &event, 0, 1000) == -1 && errno == EINVAL);

    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);
    event.events = EPOLLIN | EPOLLET;
    event.data.u64 = 0x12345678;
    REQUIRE(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, pipe_fds[0], &event) == 0);
    std::thread writer([&] {
        Delay(20000000);
        REQUIRE(write(pipe_fds[1], "a", 1) == 1);
    });
    // Every negative timeout denotes infinity, not just -1.
    REQUIRE(wait(epoll_fd, &event, 1, -7) == 1);
    REQUIRE(event.events & EPOLLIN);
    REQUIRE(event.data.u64 == 0x12345678);
    writer.join();
    char byte;
    REQUIRE(read(pipe_fds[0], &byte, 1) == 1 && byte == 'a');
    REQUIRE(wait(epoll_fd, &event, 1, 0) == 0);

    struct sigaction action{};
    action.sa_handler = [](int) {};
    REQUIRE(sigemptyset(&action.sa_mask) == 0);
    struct sigaction previous{};
    REQUIRE(sigaction(SIGUSR1, &action, &previous) == 0);
    const auto waiting_thread = pthread_self();
    std::thread interrupter([&] {
        Delay(20000000);
        REQUIRE(pthread_kill(waiting_thread, SIGUSR1) == 0);
    });
    errno = 0;
    REQUIRE(wait(epoll_fd, &event, 1, -1) == -1 && errno == EINTR);
    interrupter.join();
    REQUIRE(sigaction(SIGUSR1, &previous, nullptr) == 0);
    REQUIRE(close(pipe_fds[0]) == 0);
    REQUIRE(close(pipe_fds[1]) == 0);
    REQUIRE(close(epoll_fd) == 0);
}

void CheckCompetingWaiters() {
    const int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    int pipe_fds[2];
    REQUIRE(epoll_fd >= 0 && pipe(pipe_fds) == 0);
    epoll_event event{EPOLLIN | EPOLLET, {.u64 = 1}};
    REQUIRE(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, pipe_fds[0], &event) == 0);
    std::atomic<int> started{0};
    int results[2]{};
    std::int64_t elapsed[2]{};
    auto waiter = [&](int index) {
        epoll_event ready{};
        const auto before = NowNs();
        ++started;
        results[index] = Libraries::Net::Host::EpollWaitMicrosecondsFallback(
            epoll_fd, &ready, 1, 100000);
        elapsed[index] = NowNs() - before;
    };
    std::thread first(waiter, 0), second(waiter, 1);
    while (started.load() != 2) {
        std::this_thread::yield();
    }
    Delay(20000000);
    REQUIRE(write(pipe_fds[1], "b", 1) == 1);
    first.join();
    second.join();
    REQUIRE((results[0] == 1 && results[1] == 0) || (results[0] == 0 && results[1] == 1));
    REQUIRE(elapsed[results[0] == 0 ? 0 : 1] >= 100000000);
    REQUIRE(close(pipe_fds[0]) == 0);
    REQUIRE(close(pipe_fds[1]) == 0);
    REQUIRE(close(epoll_fd) == 0);
}
} // namespace

int main() {
    alarm(10); // A broken infinite-wait/EINTR path must not hang the test.
    CheckWait(Libraries::Net::Host::EpollWaitMicroseconds);
    CheckWait(Libraries::Net::Host::EpollWaitMicrosecondsFallback);
    CheckCompetingWaiters();
    alarm(0);
    std::puts("Android epoll timeout/readiness/EINTR/concurrent-wait contracts PASS");
}
