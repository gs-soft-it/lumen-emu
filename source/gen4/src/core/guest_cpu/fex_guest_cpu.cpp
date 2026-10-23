// SPDX-License-Identifier: MIT

#include "fex_guest_cpu.h"

#include <cerrno>
#include <cstdlib>
#include <utility>

#if defined(LUMEN_GEN4_DIRECT_FEX) && (defined(__ANDROID__) || defined(__APPLE__))
#include <pthread.h>
#endif

namespace Core {
namespace {

GuestExecutionStage ToGuestStage(Fex::EngineStage stage) {
    switch (stage) {
    case Fex::EngineStage::Request:
        return GuestExecutionStage::Request;
    case Fex::EngineStage::Mapping:
    case Fex::EngineStage::Invalidate:
        return GuestExecutionStage::Mapping;
    case Fex::EngineStage::Thread:
        return GuestExecutionStage::Thread;
    case Fex::EngineStage::Teardown:
        return GuestExecutionStage::Teardown;
    case Fex::EngineStage::Config:
    case Fex::EngineStage::Context:
    case Fex::EngineStage::Execute:
    case Fex::EngineStage::Bridge:
        return GuestExecutionStage::Execute;
    }
    return GuestExecutionStage::Execute;
}

GuestExecutionFailure ToGuestFailure(const Fex::EngineFailure& failure) {
    return {ToGuestStage(failure.Stage), failure.Error == 0 ? EIO : failure.Error
#ifndef LUMEN_GEN4_DIRECT_FEX
        , failure.RuntimeCode, failure.Rip, failure.Rsp, failure.FaultAddress, failure.FaultSignal
#endif
    };
}

} // namespace

FexGuestCpuBackend::FexGuestCpuBackend(std::unique_ptr<Fex::GuestEngine> engine_)
    : engine{std::move(engine_)} {}

FexGuestCpuBackend::~FexGuestCpuBackend() = default;

FexGuestCpuBackend::Thread::~Thread() {
    if (thread != nullptr) {
        owner.DestroyThreadOrAbort(*this);
    }
}

FexGuestCpuBackend::CreateResult FexGuestCpuBackend::Create(Fex::GuestBridge& bridge) {
    auto result = Fex::GuestEngine::Create(bridge);
    if (const auto* failure = std::get_if<Fex::EngineFailure>(&result)) {
        return ToGuestFailure(*failure);
    }
    return std::unique_ptr<FexGuestCpuBackend>{new FexGuestCpuBackend{std::move(std::get<std::unique_ptr<Fex::GuestEngine>>(result))}};
}

FexGuestCpuBackend::ThreadResult FexGuestCpuBackend::CreateThread(const GuestExecutionRequest& request) {
    if (engine == nullptr) {
        return GuestExecutionFailure{GuestExecutionStage::Teardown, ESHUTDOWN};
    }
    auto result = engine->CreateThread(request);
    if (const auto* failure = std::get_if<Fex::EngineFailure>(&result)) {
        return ToGuestFailure(*failure);
    }
    return std::unique_ptr<Thread>{new Thread{*this, std::get<Fex::GuestEngine::Thread*>(result)}};
}

GuestExecutionResult FexGuestCpuBackend::Run(Thread& thread) {
    if (engine == nullptr || thread.thread == nullptr) {
        return GuestExecutionFailure{GuestExecutionStage::Teardown, ESHUTDOWN};
    }
    auto result = engine->Run(*thread.thread);
    if (const auto* failure = std::get_if<Fex::EngineFailure>(&result)) {
        return ToGuestFailure(*failure);
    }
    return std::get<GuestExecutionState>(result);
}

GuestExecutionResult FexGuestCpuBackend::CallGuest(
    std::uintptr_t rip, std::span<const std::uint64_t> arguments
#if !defined(LUMEN_GEN4_DIRECT_FEX) || defined(__APPLE__)
    , std::span<const std::array<std::uint64_t, 2>> xmm
#endif
    ) {
    if (engine == nullptr) {
        return GuestExecutionFailure{GuestExecutionStage::Teardown, ESHUTDOWN};
    }
    auto result = engine->CallGuest(rip, arguments
#if !defined(LUMEN_GEN4_DIRECT_FEX) || defined(__APPLE__)
                                    , xmm
#endif
                                    );
    if (const auto* failure = std::get_if<Fex::EngineFailure>(&result)) {
        return ToGuestFailure(*failure);
    }
    return std::get<GuestExecutionState>(result);
}

FexGuestCpuBackend::OperationResult FexGuestCpuBackend::Invalidate(Thread& thread, std::uintptr_t begin, std::size_t size) {
    if (engine == nullptr || thread.thread == nullptr) {
        return GuestExecutionFailure{GuestExecutionStage::Teardown, ESHUTDOWN};
    }
    auto result = engine->Invalidate(*thread.thread, begin, size);
    if (const auto* failure = std::get_if<Fex::EngineFailure>(&result)) {
        return ToGuestFailure(*failure);
    }
    return true;
}

FexGuestCpuBackend::OperationResult FexGuestCpuBackend::DestroyThread(std::unique_ptr<Thread>& thread) {
    if (engine == nullptr || thread == nullptr || thread->thread == nullptr) {
        return GuestExecutionFailure{GuestExecutionStage::Teardown, ESHUTDOWN};
    }
    auto* nativeThread = thread->thread;
    auto result = engine->DestroyThread(nativeThread);
    if (const auto* failure = std::get_if<Fex::EngineFailure>(&result)) {
        return ToGuestFailure(*failure);
    }
    thread->thread = nullptr;
    thread.reset();
    return true;
}

void FexGuestCpuBackend::DestroyThreadOrAbort(Thread& thread) noexcept {
    if (engine == nullptr || thread.thread == nullptr) {
        std::abort();
    }
    auto* nativeThread = thread.thread;
    const auto result = engine->DestroyThread(nativeThread);
    if (std::holds_alternative<Fex::EngineFailure>(result)) {
        std::abort();
    }
    thread.thread = nullptr;
}

GuestExecutionResult FexGuestCpuBackend::Run(const GuestExecutionRequest& request) {
    auto threadResult = CreateThread(request);
    if (const auto* failure = std::get_if<GuestExecutionFailure>(&threadResult)) {
        return *failure;
    }
    auto thread = std::move(std::get<std::unique_ptr<Thread>>(threadResult));
#if defined(LUMEN_GEN4_DIRECT_FEX) && (defined(__ANDROID__) || defined(__APPLE__))
    // Native pthread_exit across translated frames does not reliably unwind
    // automatic C++ owners on either active host. Use the pthread cleanup
    // chain so the production thread owner always retires its FEX state.
    GuestExecutionResult runResult;
    pthread_cleanup_push([](void* owner) {
        static_cast<std::unique_ptr<Thread>*>(owner)->reset();
    }, &thread);
    runResult = Run(*thread);
    pthread_cleanup_pop(0);
#else
    auto runResult = Run(*thread);
#endif
    const auto teardownResult = DestroyThread(thread);
    if (const auto* failure = std::get_if<GuestExecutionFailure>(&runResult)) {
        return *failure;
    }
    if (const auto* failure = std::get_if<GuestExecutionFailure>(&teardownResult)) {
        return *failure;
    }
    return std::get<GuestExecutionState>(runResult);
}

std::uintptr_t FexGuestCpuBackend::ReturnAddress() const {
    return engine == nullptr ? 0 : engine->ReturnAddress();
}

GuestExecutionRange FexGuestCpuBackend::ReturnRange() const {
    return engine == nullptr ? GuestExecutionRange{} : engine->ReturnRange();
}

GuestExecutionRange FexGuestCpuBackend::CallbackReturnRange() const {
    return engine == nullptr ? GuestExecutionRange{} : engine->CallbackReturnRange();
}

} // namespace Core
