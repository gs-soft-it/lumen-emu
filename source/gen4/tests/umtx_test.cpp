// SPDX-License-Identifier: MIT
#include "core/libraries/kernel/umtx.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/kernel/posix_error.h"
#include <atomic>
#include <cstdlib>
#include <thread>
namespace Libraries::Kernel {
thread_local s32 test_error{};
s32* PS4_SYSV_ABI __Error() { return &test_error; }
}
static void Require(bool value) { if (!value) std::abort(); }
int main() {
    using namespace Libraries::Kernel;
    alignas(8) u64 word = 0x100000000ull;
    const auto address = reinterpret_cast<u64>(&word);
    OrbisKernelTimespec zero{};
    Require(posix_umtx_op(address, 2, 0, 0, reinterpret_cast<u64>(&zero)) == 0);
    Require(posix_umtx_op(address, 2, word, 0, reinterpret_cast<u64>(&zero)) == -1 && test_error == POSIX_ETIMEDOUT);
    OrbisKernelTimespec invalid{0,1000000000};
    Require(posix_umtx_op(address, 15, 0, 0, reinterpret_cast<u64>(&invalid)) == -1 && test_error == POSIX_EINVAL);
    Require(posix_umtx_op(0, 15, 0, 0, 0) == -1 && test_error == POSIX_EFAULT);
    Require(posix_umtx_op(address, 12, 0, 0, 0) == -1 && test_error == POSIX_EOPNOTSUPP);
    alignas(8) u32 semaphore[2]{};
    const auto sem = reinterpret_cast<u64>(semaphore);
    std::atomic_int result{-99};
    std::thread waiter{[&] {
        OrbisKernelTimespec timeout{1,0};
        result = posix_umtx_op(sem, 19, 0, 0, reinterpret_cast<u64>(&timeout));
    }};
    while (std::atomic_ref<u32>{semaphore[0]}.load() == 0) std::this_thread::yield();
    std::atomic_ref<u32>{semaphore[1]}.store(1);
    Require(posix_umtx_op(sem, 20, 0, 0, 0) == 0);
    waiter.join();
    Require(result == 0 && semaphore[0] == 0 && semaphore[1] == 1);
}
