#include "AndroidFexCoreRuntime.h"
#include "FexHostFeaturePolicy.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <sys/mman.h>

namespace {
constexpr std::size_t Page = 16384;
constexpr unsigned AtomicIterations = 10000;

bool Check(bool passed, const char* message) {
    if (!passed) std::fprintf(stderr, "FEX host features: FAIL: %s\n", message);
    return passed;
}

class Mapping {
public:
    Mapping() : data_(mmap(nullptr, Page, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) {}
    ~Mapping() { if (Valid()) munmap(data_, Page); }
    bool Valid() const { return data_ != MAP_FAILED; }
    void* Data() const { return data_; }
    std::uint64_t Address() const { return reinterpret_cast<std::uint64_t>(data_); }
    bool Publish() const { return mprotect(data_, Page, PROT_READ) == 0; }
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
private:
    void* data_;
};

std::uint32_t Query(void* opaque, std::uint64_t address,
                    std::uint64_t* base, std::uint64_t* size,
                    std::uint32_t* writable) noexcept {
    const auto* code = static_cast<const Mapping*>(opaque);
    if (address < code->Address() || address - code->Address() >= Page) return 0;
    *base = code->Address(); *size = Page; *writable = 0;
    return 1;
}

std::uint32_t HostCall(void*, std::uint64_t, std::uint64_t*,
                       std::uint64_t*) noexcept {
    // The V1 ABI requires a callback. Neither original program invokes HLE.
    return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
}

bool Run(const Mapping& code, std::uint64_t entry,
         std::uint64_t data, std::uint64_t output,
         std::uint64_t* result) {
    Mapping stack;
    if (!stack.Valid()) return false;
    LumenAndroidFexCoreGuestStateV1 guest {};
    guest.structSize = sizeof(guest); guest.abiVersion = 1;
    guest.rip = code.Address() + entry;
    guest.gprs[4] = stack.Address() + Page - sizeof(std::uint64_t);
    guest.gprs[7] = data; guest.gprs[6] = output;
    const auto root = LumenAndroidFexCoreRootReturnAddressV1();
    if (root == 0) return false;
    std::memcpy(reinterpret_cast<void*>(guest.gprs[4]), &root, sizeof(root));
    const auto status = LumenAndroidFexCoreRunGuestV1(
        &guest, HostCall, Query, const_cast<Mapping*>(&code));
    *result = guest.gprs[0];
    if (status != LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 || guest.rip != root)
        std::fprintf(stderr, "Atomic root: code=%u rip=0x%llx expected=0x%llx fault=%u\n",
            status, static_cast<unsigned long long>(guest.rip),
            static_cast<unsigned long long>(root), guest.faultSignal);
    return Check(status == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 && guest.rip == root,
                 "original atomic program completes through product FEX");
}

bool AtomicLowering() {
    Mapping code, data;
    if (!Check(code.Valid() && data.Valid(), "atomic mappings")) return false;
    // Original x86-64 programs, no ROM bytes. The first covers XADD, CAS
    // success/failure and XCHG with independently specified outputs. The
    // second has two translated roots increment the same aligned counter.
    constexpr unsigned char operations[] {
        0x48,0xc7,0xc0,5,0,0,0,             // mov rax,5
        0xf0,0x48,0x0f,0xc1,0x07,           // lock xadd [rdi],rax
        0x48,0x89,0x06,                     // mov [rsi],rax
        0x48,0xc7,0xc0,15,0,0,0,            // mov rax,15
        0x48,0xc7,0xc2,42,0,0,0,            // mov rdx,42
        0xf0,0x48,0x0f,0xb1,0x17,           // lock cmpxchg [rdi],rdx
        0x0f,0x94,0xc1,                     // sete cl
        0x48,0x0f,0xb6,0xc9,                // movzx rcx,cl
        0x48,0x89,0x4e,0x08,                // mov [rsi+8],rcx
        0x48,0xc7,0xc0,11,0,0,0,            // mov rax,11
        0xf0,0x48,0x0f,0xb1,0x17,           // lock cmpxchg [rdi],rdx
        0x48,0x89,0x46,0x10,                // mov [rsi+16],rax
        0x0f,0x94,0xc1,                     // sete cl
        0x48,0x0f,0xb6,0xc9,                // movzx rcx,cl
        0x48,0x89,0x4e,0x18,                // mov [rsi+24],rcx
        0x48,0xc7,0xc0,99,0,0,0,            // mov rax,99
        0x48,0x87,0x07,                     // xchg [rdi],rax
        0xc3,
    };
    constexpr unsigned char increments[] {
        0xb9,0x10,0x27,0,0,                 // mov ecx,10000
        0xf0,0x48,0x83,0x07,1,              // lock add qword [rdi],1
        0xff,0xc9,                          // dec ecx
        0x75,0xf7,                          // jne lock-add
        0x48,0x8b,0x07,                     // mov rax,[rdi]
        0xc3,
    };
    std::memcpy(code.Data(), operations, sizeof(operations));
    std::memcpy(static_cast<unsigned char*>(code.Data()) + 256,
                increments, sizeof(increments));
    if (!Check(code.Publish(), "atomic code becomes immutable")) return false;
    auto* words = static_cast<std::uint64_t*>(data.Data());
    words[0] = 10;
    std::uint64_t result {};
    bool passed = Run(code, 0, data.Address(), data.Address() + 64, &result);
    passed = Check(result == 42 && words[0] == 99 &&
        words[8] == 10 && words[9] == 1 && words[10] == 42 && words[11] == 0,
        "XADD/CAS/XCHG values and CAS flags") && passed;
    words[0] = 0;
    std::atomic<unsigned> ready {};
    std::atomic<bool> start {};
    std::array<bool, 2> succeeded {};
    std::array<std::thread, 2> workers;
    for (unsigned index = 0; index != workers.size(); ++index) {
        workers[index] = std::thread([&, index] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            std::uint64_t observed {};
            succeeded[index] = Run(code, 256, data.Address(), 0, &observed) &&
                observed >= AtomicIterations && observed <= 2 * AtomicIterations;
        });
    }
    while (ready.load(std::memory_order_acquire) != workers.size()) std::this_thread::yield();
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    return Check(succeeded[0] && succeeded[1] && words[0] == 2 * AtomicIterations,
                 "two translated atomic writers preserve every update") && passed;
}

bool PolicyAndLatching() {
    using namespace Lumen::FexCore;
    bool passed = Check(!ObservedHostCapability(false, 1) &&
        !ObservedHostCapability(true, 0) && !ObservedHostCapability(true, 2) &&
        !ObservedHostCapability(true, UINT64_MAX) && ObservedHostCapability(true, 1),
        "failed/unknown capability queries deny optional instructions");
    const NativeHostFeatures partial {true, false, true, false};
    const auto allowed = SelectNativeHostFeatures(partial, true);
    const auto denied = SelectNativeHostFeatures(partial, false);
    passed = Check(allowed.atomics && !allowed.flagM && allowed.flagM2 && !allowed.frintts &&
        !denied.atomics && !denied.flagM && !denied.flagM2 && !denied.frintts,
        "selection preserves unavailable capabilities and rollback clears every selected field") && passed;
    passed = Check(NativeHostFeaturesRequested(nullptr) && NativeHostFeaturesRequested("1") &&
        !NativeHostFeaturesRequested("") && !NativeHostFeaturesRequested("0") &&
        !NativeHostFeaturesRequested("01") && !NativeHostFeaturesRequested("true"),
        "only default and exact one enable native lowerings") && passed;
    const char* const initial = std::getenv("LUMEN_EXPERIMENTAL_FEX_NATIVE_HOST_FEATURES");
    const bool wasSet = initial != nullptr;
    const std::string saved = initial != nullptr ? initial : "";
    const bool enabled = NativeHostFeaturesEnabled();
    passed = Check(enabled == NativeHostFeaturesRequested(initial), "initial process selection") && passed;
    passed = Check(setenv("LUMEN_EXPERIMENTAL_FEX_NATIVE_HOST_FEATURES", enabled ? "0" : "1", 1) == 0 &&
        NativeHostFeaturesEnabled() == enabled, "selection remains latched after environment changes") && passed;
    const int restored = wasSet
        ? setenv("LUMEN_EXPERIMENTAL_FEX_NATIVE_HOST_FEATURES", saved.c_str(), 1)
        : unsetenv("LUMEN_EXPERIMENTAL_FEX_NATIVE_HOST_FEATURES");
    return Check(restored == 0, "restore selector before runtime initialization") && passed;
}
} // namespace

int main() {
    bool passed = PolicyAndLatching();
    LumenAndroidFexCoreRuntimeStatusV1 status {};
    passed = Check(LumenAndroidFexCoreRuntimeProbeV1(&status) ==
        LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1, "product runtime probe") && passed;
    if (status.code == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1)
        passed = AtomicLowering() && passed;
    std::printf("FexHostFeaturePolicyTest: %s enabled=%u atomic_updates=%u\n",
                passed ? "PASS" : "FAIL",
                Lumen::FexCore::NativeHostFeaturesEnabled() ? 1U : 0U,
                2 * AtomicIterations);
    return passed ? 0 : 1;
}
