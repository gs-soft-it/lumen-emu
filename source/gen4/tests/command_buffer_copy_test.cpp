// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <coroutine>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include "video_core/amdgpu/command_buffer_copy.h"

namespace {

void Require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

// Exercise the same ownership transfer into an initially suspended promise:
// the parser may resume after a later submission has reused the guest storage.
struct ParserTask {
    struct promise_type {
        std::unique_ptr<AmdGpu::CommandBufferCopy> command_buffers;
        ParserTask get_return_object() {
            return {std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        std::suspend_always yield_value(unsigned) noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::rethrow_exception(std::current_exception()); }
    };

    std::coroutine_handle<promise_type> handle;
};

ParserTask Parse(std::span<const std::uint32_t> dcb, std::span<const std::uint32_t> ccb,
                 std::uint32_t expected) {
    Require(std::all_of(dcb.begin(), dcb.end(),
                        [=](auto word) { return word == expected; }), "DCB changed before resume");
    co_yield 0;
    Require(std::all_of(ccb.begin(), ccb.end(),
                        [=](auto word) { return word == ~expected; }), "CCB changed across yield");
    Require(std::all_of(dcb.begin(), dcb.end(),
                        [=](auto word) { return word == expected; }), "DCB changed across yield");
}

ParserTask Submit(std::span<const std::uint32_t> dcb, std::span<const std::uint32_t> ccb,
                  std::uint32_t expected) {
    auto owner = std::make_unique<AmdGpu::CommandBufferCopy>(dcb, ccb);
    auto task = Parse(owner->dcb, owner->ccb, expected);
    task.handle.promise().command_buffers = std::move(owner);
    return task;
}

void TestIndependentSubmissions() {
    std::vector<std::uint32_t> guest_dcb(300'000);
    std::vector<std::uint32_t> guest_ccb(2'048);
    std::vector<ParserTask> queued;
    // More than the old shared 2 MiB reservation, with every submission retaining
    // its own token-like contents while the guest immediately rewrites the source.
    for (std::uint32_t token = 1; token <= 8; ++token) {
        std::fill(guest_dcb.begin(), guest_dcb.end(), token);
        std::fill(guest_ccb.begin(), guest_ccb.end(), ~token);
        queued.push_back(Submit(guest_dcb, guest_ccb, token));
        Require(queued.back().handle.promise().command_buffers->dcb.data() != guest_dcb.data(),
                "DCB aliases mutable guest source");
        if (queued.size() > 1) {
            Require(queued.back().handle.promise().command_buffers->dcb.data() !=
                        queued.front().handle.promise().command_buffers->dcb.data(),
                    "in-flight submissions share copy storage");
        }
    }
    std::fill(guest_dcb.begin(), guest_dcb.end(), 0);
    std::fill(guest_ccb.begin(), guest_ccb.end(), 0);
    for (auto& task : queued) {
        task.handle.resume();
        Require(!task.handle.done(), "parser did not suspend");
    }
    // Complete in reverse order; releasing one copy must not affect another.
    for (auto it = queued.rbegin(); it != queued.rend(); ++it) {
        it->handle.resume();
        Require(it->handle.done(), "parser did not finish");
        Require(it->handle.promise().command_buffers != nullptr,
                "copy released before coroutine destruction");
        it->handle.destroy();
    }
}

void TestEmptyAndBorrowed() {
    AmdGpu::CommandBufferCopy empty{{}, {}};
    Require(empty.dcb.empty() && empty.ccb.empty(), "empty submission changed");
    const std::vector<std::uint32_t> one{0x68750776};
    AmdGpu::CommandBufferCopy only_dcb{one, {}};
    AmdGpu::CommandBufferCopy only_ccb{{}, one};
    Require(only_dcb.dcb == one && only_dcb.ccb.empty(), "DCB-only copy failed");
    Require(only_ccb.ccb == one && only_ccb.dcb.empty(), "CCB-only copy failed");

    // The disabled setting and nested command streams retain borrowed semantics.
    auto borrowed = Parse(one, {}, one.front());
    Require(!borrowed.handle.promise().command_buffers, "borrowed task acquired an owner");
    borrowed.handle.resume();
    borrowed.handle.resume();
    Require(borrowed.handle.done(), "borrowed parser did not finish");
    borrowed.handle.destroy();
    Require(one.front() == 0x68750776, "borrowed source changed");

    auto cancelled = Submit(one, {}, one.front());
    Require(!cancelled.handle.done(), "new parser was not suspended");
    cancelled.handle.destroy();
}

} // namespace

int main() {
    try {
        TestIndependentSubmissions();
        TestEmptyAndBorrowed();
        std::cout << "command buffer copy ownership/coroutine lifetime gates PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
