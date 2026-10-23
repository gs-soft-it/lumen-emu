#include "Lumen/Core/Threads.h"
#include "Lumen/Core/File.h"
#include "Lumen/Core/VirtualMemory.h"
#include "Emulator/Config.h"
#include "Emulator/Controller.h"
#include "Emulator/Graphics/Graphics.h"
#include "Emulator/Kernel/EventFlag.h"
#include "Emulator/Kernel/EventQueue.h"
#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Kernel/Semaphore.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Libs/NpToolkit2.h"
#include "Emulator/Libs/UserService.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"
#include "Emulator/Network.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <thread>

namespace {
namespace Kernel = Lumen::Libs::LibKernel;
namespace EventQueue = Kernel::EventQueue;
namespace EventFlag = Kernel::EventFlag;
namespace FileSystem = Kernel::FileSystem;
namespace Semaphore = Kernel::Semaphore;
namespace Loader = Lumen::Loader;
namespace Memory = Lumen::Core::VirtualMemory;
namespace Network = Lumen::Libs::Network;
namespace Controller = Lumen::Libs::Controller;
namespace Graphics = Lumen::Libs::Graphics::Gen4;
namespace NpToolkit2 = Lumen::Libs::NpToolkit2;
namespace Posix = Lumen::Libs::Posix;
namespace UserService = Lumen::Libs::UserService;

bool Check(bool condition, const char* message)
{
    if (!condition) std::fprintf(stderr, "HleImplementedExportsTest: %s\n", message);
    return condition;
}

Loader::SymbolResolve Query(const char* nid, const char* library = "libkernel", int module_major = 1,
                            int module_minor = 1, int library_version = 1)
{
    Loader::SymbolResolve query {};
    query.name = nid;
    query.library = library;
    query.library_version = library_version;
    query.module = library;
    query.module_version_major = module_major;
    query.module_version_minor = module_minor;
    query.type = Loader::SymbolType::Func;
    return query;
}

struct Binding
{
    const char* nid;
    const char* library;
    int module_major;
    int module_minor;
    uint64_t target;
    Loader::HostCallScalarMetadata metadata;
};

template <auto Function> Binding Entry(const char* nid, const char* library = "libkernel", int module_major = 1,
                                       int module_minor = 1)
{
    return {nid, library, module_major, module_minor, reinterpret_cast<uint64_t>(Function),
            Loader::MakeHostCallScalarMetadata<Function>()};
}

bool Bindings(const Loader::SymbolDatabase& symbols)
{
    // Missing direct kernel exports of existing implementations. POSIX
    // adapters retain their separate namespaces and error translation.
    const std::array<Binding, 47> bindings {{
        Entry<&FileSystem::KernelFcntl>("SoZkxZkCHaw"),
        Entry<&EventQueue::KernelGetEventData>("kwGyyjohI50"),
        Entry<&EventQueue::KernelGetEventFflags>("Q0qr9AyqJSk"),
        Entry<&EventFlag::KernelPollEventFlag>("9lvj5DjHZiA"),
        Entry<&Semaphore::KernelPollSema>("12wOHk8ywb0"),
        Entry<&Semaphore::KernelCancelSema>("4DM06U2BNEY"),
        Entry<&Kernel::PthreadCancel>("qBDmpCyGssE"),
        Entry<&Kernel::PthreadSetcancelstate>("OAmWq+OHSjw"),
        Entry<&Kernel::PthreadSetcanceltype>("sCJd99Phct0"),
        Entry<&Kernel::PthreadTestcancel>("LapIb799SSE"),
        Entry<&Kernel::PthreadTestcancel>("nYBrkGDqxh8"),
        Entry<&Kernel::PthreadAttrGetinheritsched>("lpMP8HhkBbg"),
        Entry<&Kernel::PthreadAttrGetschedpolicy>("NMyIQ9WgWbU"),
        Entry<&Kernel::PthreadAttrGetstack>("-quPa4SEJUw"),
        Entry<&Kernel::PthreadAttrSetstack>("Bvn74vj6oLo"),
        Entry<&Kernel::PthreadAttrSetstackaddr>("F+yfmduIBB8"),
        Entry<&Kernel::PthreadRwlockTimedrdlock>("iPtZRWICjrM"),
        Entry<&Kernel::PthreadRwlockTimedwrlock>("adh--6nIqTk"),
        Entry<&Kernel::PthreadRwlockTryrdlock>("XD3mDeybCnk"),
        Entry<&Kernel::PthreadRwlockTrywrlock>("bIHoZCTomsI"),
        Entry<&Kernel::PthreadRwlockattrDestroy>("i2ifZ3fS2fo"),
        Entry<&Kernel::PthreadRwlockattrInit>("yOfGg-I1ZII"),
        Entry<&Kernel::PthreadRwlockattrGettype>("Kyls1ChFyrc"),
        Entry<&Kernel::PthreadRwlockattrSettype>("h-OifiouBd8"),
        Entry<&Kernel::KernelClockGetres>("wRYVA5Zolso"),
        Entry<&Kernel::KernelNanosleep>("QvsZxomvUHs"),
        Entry<&Network::Ssl::SslTerm>("0K1yQ6Lv-Yc", "libSceSsl"),
        Entry<&Network::Http::HttpTerm>("Ik-KpLTlf7Q", "libSceHttp"),
        Entry<&Graphics::GraphicsUnregisterAllResourcesForOwner>("yhFCnaz5daw", "libSceGnmDriver"),
        Entry<&Graphics::GraphicsUnregisterOwnerAndResources>("fhKwCVVj9nk", "libSceGnmDriver"),
        Entry<&Graphics::GraphicsUnregisterResource>("k8EXkhIP+lM", "libSceGnmDriver"),
        Entry<&Network::Ssl::SslInit>("hdpVEUDFW3s", "libSceSsl", 2),
        Entry<&Network::Ssl::SslTerm>("0K1yQ6Lv-Yc", "libSceSsl", 2),
        Entry<&NpToolkit2::CoreRequestInitParamsConstruct>("vbh-MWtjMh4", "libSceNpToolkit2", 0, 0),
        Entry<&NpToolkit2::CoreInit>("LLdnqVnnNBQ", "libSceNpToolkit2", 0, 0),
        Entry<&NpToolkit2::CoreRequestInitParamsDestroy>("-bAjEe-hdCg", "libSceNpToolkit2", 0, 0),
        Entry<&NpToolkit2::CoreResponseEmptyConstruct>("tlpXm5KLgqk", "libSceNpToolkit2", 0, 0),
        Entry<&NpToolkit2::CoreResponseEmptyDestroy>("30Zxe35LtBg", "libSceNpToolkit2", 0, 0),
        Entry<&NpToolkit2::TrophyRegisterTrophyPackRequestConstruct>("wkgY4lDEV9k", "libSceNpToolkit2", 0, 0),
        Entry<&NpToolkit2::TrophyRegisterTrophyPackRequestDestroy>("ItZXngg8tfo", "libSceNpToolkit2", 0, 0),
        Entry<&NpToolkit2::TrophyRegisterTrophyPack>("buMmdpEwRuM", "libSceNpToolkit2", 0, 0),
        Entry<&NpToolkit2::CoreRequestBaseGetFunctionType>("El4tFb9f8dk", "libSceNpToolkit2", 0, 0),
        Entry<&UserService::UserServiceGetUserColor>("lUoqwTQu4Go", "libSceUserService", 1, 1),
        Entry<&Network::NpManager::NpGetAccountIdA>("rbknaUjpqWo", "libSceNpManager", 1, 1),
        Entry<&Controller::PadDeviceClassGetExtendedInformation>("AcslpN1jHR8", "libScePad", 1, 1),
        Entry<&Controller::PadOpen>("xk0AcarP3V4", "libScePad", 1, 1),
        Entry<&Controller::PadGetControllerInformation>("gjP9-KQzoUk", "libScePad", 1, 1),
    }};
    for (const auto& binding: bindings)
    {
        const auto* record = symbols.Find(Query(binding.nid, binding.library, binding.module_major, binding.module_minor));
        if (!Check(record != nullptr && record->vaddr == binding.target, binding.nid)) return false;
        const auto* metadata = symbols.HostCallScalarMetadataFor(record->vaddr);
        if (!Check(metadata != nullptr && metadata->arity == binding.metadata.arity &&
                   metadata->arguments == binding.metadata.arguments && metadata->result == binding.metadata.result &&
                   metadata->invoker != nullptr &&
                   symbols.ClassifyHostCallTarget(record->vaddr) == Loader::HostCallRegistryClassification::Host,
                   "implemented export metadata or sealed host registry differs")) return false;
        auto wrong = Query(binding.nid, binding.library, binding.module_major, binding.module_minor);
        wrong.library = "UnrelatedLibrary";
        if (!Check(symbols.Find(wrong) == nullptr, "kernel export gained a namespace fallback")) return false;
        wrong = Query(binding.nid, binding.library, 99, binding.module_minor);
        if (!Check(symbols.Find(wrong) == nullptr, "implemented export gained a version fallback")) return false;
    }
    // Unimplemented bodies and internal helpers must not be registered by a
    // mechanical ABI-name scan or by deriving an unqualified public identity.
    for (const char* nid: {"Uu-iDFC9aUc", "o69RpYO-Mu0", "p0nl5jrDG+s", "L-r3D6MUXeo", "frcXmG-GtuU", "wHoKgNnBops"})
        if (!Check(symbols.Find(Query(nid)) == nullptr, "stub or internal helper was exported")) return false;
    return true;
}

uint64_t Word(const void* pointer) { return reinterpret_cast<uint64_t>(pointer); }
uint64_t Call(const Loader::SymbolDatabase& symbols, const char* nid, std::initializer_list<uint64_t> arguments,
              const char* library = "libkernel", int module_major = 1)
{
    const auto* record = symbols.Find(Query(nid, library, module_major));
    const auto* metadata = record != nullptr ? symbols.HostCallScalarMetadataFor(record->vaddr) : nullptr;
    if (!Check(metadata != nullptr && metadata->arity == arguments.size() && metadata->invoker != nullptr,
               "invocation metadata missing")) return UINT64_MAX;
    return metadata->invoker(arguments.begin());
}
int Result(const Loader::SymbolDatabase& symbols, const char* nid, std::initializer_list<uint64_t> arguments,
           const char* library = "libkernel", int module_major = 1)
{
    return static_cast<int32_t>(static_cast<uint32_t>(Call(symbols, nid, arguments, library, module_major)));
}

bool EventRecords(const Loader::SymbolDatabase& symbols)
{
    EventQueue::KernelEvent event {};
    event.ident = UINT64_C(0x8123456789abcdef);
    event.filter = -14;
    event.flags = 0x20;
    event.fflags = 0xf1234567;
    event.data = static_cast<intptr_t>(INT64_C(-0x7123456789abcdef));
    event.udata = &event;
    std::array<uint8_t, sizeof(event)> original {};
    std::memcpy(original.data(), &event, sizeof(event));
    return Check(Call(symbols, "kwGyyjohI50", {Word(&event)}) == static_cast<uint64_t>(event.data) &&
                 Call(symbols, "Q0qr9AyqJSk", {Word(&event)}) == event.fflags &&
                 std::memcmp(&event, original.data(), sizeof(event)) == 0,
                 "event accessors changed their record or lost high bits");
}

bool PosixAliases(const Loader::SymbolDatabase& symbols)
{
    // These names have both exact Posix and libkernel import identities. The
    // alias must retain the wrapper target, including its return/errno rules.
    struct Alias { const char* nid; uint32_t arity; };
    constexpr std::array<Alias, 3> aliases {{{"n88vx3C5nW8", 2}, {"yS8U2TGCe1A", 2}, {"c7ZnT7V1B98", 1}}};
    for (const auto& alias: aliases)
    {
        auto posix_query = Query(alias.nid);
        posix_query.library = "Posix";
        const auto* original = symbols.Find(posix_query);
        const auto* added = symbols.Find(Query(alias.nid));
        if (!Check(original != nullptr && added != nullptr && original->vaddr == added->vaddr,
                   "explicit POSIX alias changed the wrapper target")) return false;
        const auto* metadata = symbols.HostCallScalarMetadataFor(added->vaddr);
        if (!Check(metadata != nullptr && metadata == symbols.HostCallScalarMetadataFor(original->vaddr) &&
                   metadata->arity == alias.arity && metadata->result == Loader::HostCallScalarType::Gp &&
                   metadata->invoker != nullptr &&
                   symbols.ClassifyHostCallTarget(added->vaddr) == Loader::HostCallRegistryClassification::Host,
                   "explicit POSIX alias lost fixed scalar metadata")) return false;
        for (uint32_t i = 0; i < alias.arity; ++i)
            if (!Check(metadata->arguments[i] == Loader::HostCallScalarType::Gp,
                       "explicit POSIX alias argument is not GP")) return false;
        auto wrong = Query(alias.nid, "libkernel", 99);
        if (!Check(symbols.Find(wrong) == nullptr, "explicit POSIX alias gained a version fallback")) return false;
        wrong = Query(alias.nid, "UnrelatedLibrary");
        if (!Check(symbols.Find(wrong) == nullptr, "explicit POSIX alias gained a namespace fallback")) return false;
    }
    struct RestoreErrno
    {
        int original = *Posix::GetErrorAddr();
        ~RestoreErrno() { *Posix::GetErrorAddr() = original; }
    } restore_errno;
    *Posix::GetErrorAddr() = Posix::POSIX_EBUSY;
    if (!Check(Call(symbols, "n88vx3C5nW8", {0, 0}) == 0 && *Posix::GetErrorAddr() == Posix::POSIX_EBUSY,
               "gettimeofday no-output alias changed errno or returned failure")) return false;
    if (!Check(Call(symbols, "yS8U2TGCe1A", {0, 0}) == UINT32_MAX && *Posix::GetErrorAddr() == Posix::POSIX_EFAULT,
               "nanosleep alias lost POSIX -1/errno translation")) return false;
    *Posix::GetErrorAddr() = Posix::POSIX_EBUSY;
    return Check(Call(symbols, "c7ZnT7V1B98", {0}) == UINT32_MAX && *Posix::GetErrorAddr() == Posix::POSIX_EFAULT,
                 "null rmdir alias did not reject before filesystem lookup");
}

bool AttributesAndLocks(const Loader::SymbolDatabase& symbols)
{
    struct Fixture
    {
        Kernel::PthreadAttr attr = nullptr;
        Kernel::PthreadRwlockattr rwattr = nullptr;
        Kernel::PthreadRwlock rwlock = nullptr;
        uint64_t stack = 0;
        ~Fixture()
        {
            if (rwlock != nullptr) Kernel::PthreadRwlockDestroy(&rwlock);
            if (rwattr != nullptr) Kernel::PthreadRwlockattrDestroy(&rwattr);
            if (attr != nullptr) Kernel::PthreadAttrDestroy(&attr);
            if (stack != 0) Memory::Free(stack);
        }
    } fixture;
    fixture.stack = Memory::Alloc(0, 0x40000, Memory::Mode::ReadWrite);
    if (!Check(fixture.stack != 0 && Kernel::PthreadAttrInit(&fixture.attr) == 0 &&
               Result(symbols, "yOfGg-I1ZII", {Word(&fixture.rwattr)}) == 0,
               "owned pthread fixture initialization failed")) return false;
    std::memset(reinterpret_cast<void*>(fixture.stack), 0xa7, 0x40000);
    const auto attr_identity = fixture.attr;
    if (!Check(Result(symbols, "Bvn74vj6oLo", {Word(&fixture.attr), fixture.stack, 0x20000}) == 0,
               "stack attribute setter did not dispatch")) return false;
    void* stack = nullptr;
    size_t size = 0;
    if (!Check(Result(symbols, "-quPa4SEJUw", {Word(&fixture.attr), Word(&stack), Word(&size)}) == 0 &&
               stack == reinterpret_cast<void*>(fixture.stack) && size == 0x20000,
               "stack attribute roundtrip differs")) return false;
    if (!Check(Result(symbols, "F+yfmduIBB8", {Word(&fixture.attr), fixture.stack + 0x10000}) == 0 &&
               Result(symbols, "-quPa4SEJUw", {Word(&fixture.attr), Word(&stack), Word(&size)}) == 0 &&
               stack == reinterpret_cast<void*>(fixture.stack + 0x10000) && size == 0x20000 &&
               fixture.attr == attr_identity, "stack address setter changed handle or size")) return false;
    int inherited = -1, policy = -1;
    if (!Check(Result(symbols, "lpMP8HhkBbg", {Word(&fixture.attr), Word(&inherited)}) == 0 && inherited == 4 &&
               Result(symbols, "NMyIQ9WgWbU", {Word(&fixture.attr), Word(&policy)}) == 0 && policy == 1,
               "attribute scheduling getters differ from initialized guest state")) return false;
    for (uint64_t offset: {UINT64_C(0), UINT64_C(0x10000), UINT64_C(0x3ffff)})
        if (!Check(*reinterpret_cast<const uint8_t*>(fixture.stack + offset) == 0xa7,
                   "attribute-only setter changed stack bytes")) return false;
    int type = 0;
    if (!Check(Result(symbols, "h-OifiouBd8", {Word(&fixture.rwattr), 1}) == 0 &&
               Result(symbols, "Kyls1ChFyrc", {Word(&fixture.rwattr), Word(&type)}) == 0 && type == 1 &&
               Kernel::PthreadRwlockInit(&fixture.rwlock, &fixture.rwattr, "implemented-exports") == 0,
               "rwlock attribute or lock creation failed")) return false;
    const auto lock_identity = fixture.rwlock;
    for (const char* nid: {"XD3mDeybCnk", "bIHoZCTomsI", "iPtZRWICjrM", "adh--6nIqTk"})
    {
        const bool timed = std::strcmp(nid, "iPtZRWICjrM") == 0 || std::strcmp(nid, "adh--6nIqTk") == 0;
        const int acquired = timed ? Result(symbols, nid, {Word(&fixture.rwlock), 0}) :
                                     Result(symbols, nid, {Word(&fixture.rwlock)});
        if (!Check(acquired == 0, "uncontended rwlock acquisition failed")) return false;
        if (!Check(Kernel::PthreadRwlockUnlock(&fixture.rwlock) == 0 && fixture.rwlock == lock_identity,
                   "rwlock acquisition changed its handle")) return false;
    }
    if (!Check(Kernel::PthreadRwlockWrlock(&fixture.rwlock) == 0, "rwlock contention fixture failed")) return false;
    int read_busy = 0, write_busy = 0;
    std::thread contender([&] {
        read_busy = Result(symbols, "XD3mDeybCnk", {Word(&fixture.rwlock)});
        write_busy = Result(symbols, "bIHoZCTomsI", {Word(&fixture.rwlock)});
    });
    contender.join();
    const int unlocked = Kernel::PthreadRwlockUnlock(&fixture.rwlock);
    if (!Check(read_busy == Kernel::KERNEL_ERROR_EBUSY && write_busy == Kernel::KERNEL_ERROR_EBUSY && unlocked == 0,
               "contended try-rwlock did not retain its real busy result")) return false;
    return Check(Result(symbols, "i2ifZ3fS2fo", {Word(&fixture.rwattr)}) == 0 && fixture.rwattr == nullptr,
                 "rwlock attribute destruction did not clear its owned handle");
}

bool PollingAndTime(const Loader::SymbolDatabase& symbols)
{
    struct Fixture
    {
        Semaphore::KernelSema sema = nullptr;
        EventFlag::KernelEventFlag flag = nullptr;
        ~Fixture()
        {
            if (sema != nullptr) Semaphore::KernelDeleteSema(sema);
            if (flag != nullptr) EventFlag::KernelDeleteEventFlag(flag);
        }
    } fixture;
    constexpr uint64_t bit = UINT64_C(1) << 63;
    if (!Check(Semaphore::KernelCreateSema(&fixture.sema, "implemented-exports", 1, 2, 4, nullptr) == 0 &&
               EventFlag::KernelCreateEventFlag(&fixture.flag, "implemented-exports", 1, bit, nullptr) == 0,
               "owned polling fixture initialization failed")) return false;
    int waiting = -1;
    uint64_t pattern = 0;
    if (!Check(Result(symbols, "12wOHk8ywb0", {Word(fixture.sema), 2}) == 0 &&
               Result(symbols, "12wOHk8ywb0", {Word(fixture.sema), 1}) == Kernel::KERNEL_ERROR_EBUSY &&
               Result(symbols, "4DM06U2BNEY", {Word(fixture.sema), 3, Word(&waiting)}) == 0 && waiting == 0 &&
               Result(symbols, "12wOHk8ywb0", {Word(fixture.sema), 3}) == 0,
               "semaphore poll/cancel did not change the real count")) return false;
    if (!Check(Result(symbols, "9lvj5DjHZiA", {Word(fixture.flag), bit, 0x21, Word(&pattern)}) == 0 && pattern == bit &&
               Result(symbols, "9lvj5DjHZiA", {Word(fixture.flag), bit, 1, Word(&pattern)}) == Kernel::KERNEL_ERROR_EBUSY,
               "event-flag poll did not observe/consume the 64-bit pattern")) return false;
    Kernel::KernelTimespec resolution {}, request {};
    const auto before = request;
    if (!Check(Result(symbols, "wRYVA5Zolso", {4, Word(&resolution)}) == 0 &&
               (resolution.tv_sec > 0 || resolution.tv_nsec > 0) && resolution.tv_nsec < 1000000000 &&
               Result(symbols, "QvsZxomvUHs", {Word(&request), 0}) == 0 &&
               std::memcmp(&request, &before, sizeof(request)) == 0,
               "clock resolution/zero-duration sleep changed their scalar contract")) return false;
    return Check(Result(symbols, "qBDmpCyGssE", {0}) == Kernel::KERNEL_ERROR_EINVAL &&
                 Result(symbols, "OAmWq+OHSjw", {UINT64_MAX, 0}) == Kernel::KERNEL_ERROR_EINVAL,
                 "invalid cancellation request did not fail before host side effects");
}

bool CancellationType(const Loader::SymbolDatabase& symbols)
{
    if (!Check(Call(symbols, "sCJd99Phct0", {1, 0}) == static_cast<uint32_t>(Kernel::KERNEL_ERROR_EINVAL),
               "invalid cancellation type or 32-bit error widening differs")) return false;
    struct Output
    {
        uint64_t address = Memory::Alloc(0, 0x4000, Memory::Mode::ReadWrite);
        ~Output() { if (address != 0) Memory::Free(address); }
    } output;
    if (!Check(output.address != 0, "cancellation output allocation failed")) return false;
    auto* value = reinterpret_cast<int*>(output.address);
    *value = 0x13572468;
    if (!Check(Memory::Protect(output.address, 0x4000, Memory::Mode::Read),
               "cancellation read-only output fixture failed")) return false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
    return Check(Result(symbols, "sCJd99Phct0", {2, output.address}) == Kernel::KERNEL_ERROR_EFAULT &&
                 *value == 0x13572468 &&
                 Result(symbols, "sCJd99Phct0", {0, 0}) == Kernel::KERNEL_ERROR_EOPNOTSUPP,
                 "Android cancellation capability or invalid output differs");
#else
    struct Restore
    {
        int state = 0, type = 0;
        bool restore_state = false, restore_type = false;
        ~Restore()
        {
            if (restore_type) Kernel::PthreadSetcanceltype(type, nullptr);
            if (restore_state) Kernel::PthreadSetcancelstate(state, nullptr);
        }
    } previous;
    if (!Check(Kernel::PthreadSetcancelstate(1, &previous.state) == 0,
               "cancellation disable fixture failed")) return false;
    previous.restore_state = true;
    if (!Check(Kernel::PthreadSetcanceltype(0, &previous.type) == 0,
               "cancellation type fixture failed")) return false;
    previous.restore_type = true;
    int old_type = -1;
    if (!Check(Result(symbols, "sCJd99Phct0", {0, 0}) == 0 &&
               Result(symbols, "sCJd99Phct0", {2, output.address}) == Kernel::KERNEL_ERROR_EFAULT &&
               *value == 0x13572468 &&
               Result(symbols, "sCJd99Phct0", {0, Word(&old_type)}) == 0 && old_type == 0,
               "null/invalid cancellation output changed host state or guest bytes")) return false;
    return Check(Result(symbols, "sCJd99Phct0", {2, Word(&old_type)}) == 0 && old_type == 0 &&
                 Result(symbols, "sCJd99Phct0", {0, Word(&old_type)}) == 0 && old_type == 2,
                 "cancellation type translation or writable output differs");
#endif
}

bool CancellationPoint(const Loader::SymbolDatabase& symbols)
{
    // The POSIX identity and the libkernel POSIX alias must share the single
    // cancellation point, and a caller without a pending cancellation returns
    // with its errno untouched. The POSIX identity keeps the libkernel module.
    Loader::SymbolResolve posix_query = Query("nYBrkGDqxh8");
    posix_query.library = "libScePosix";
    const auto* posix_record = symbols.Find(posix_query);
    const auto* kernel_record = symbols.Find(Query("nYBrkGDqxh8"));
    if (!Check(posix_record != nullptr && kernel_record != nullptr &&
               posix_record->vaddr == kernel_record->vaddr &&
               kernel_record->vaddr == reinterpret_cast<uint64_t>(&Kernel::PthreadTestcancel),
               "pthread_testcancel namespaces do not share the cancellation point")) return false;
    const auto* metadata = symbols.HostCallScalarMetadataFor(kernel_record->vaddr);
    if (!Check(metadata != nullptr && metadata == symbols.HostCallScalarMetadataFor(posix_record->vaddr) &&
               metadata->arity == 0 && metadata->result == Loader::HostCallScalarType::Void &&
               metadata->invoker != nullptr &&
               symbols.ClassifyHostCallTarget(kernel_record->vaddr) == Loader::HostCallRegistryClassification::Host,
               "pthread_testcancel lost its argument-less scalar invocation")) return false;
    struct RestoreErrno
    {
        int original = *Posix::GetErrorAddr();
        ~RestoreErrno() { *Posix::GetErrorAddr() = original; }
    } restore_errno;
    *Posix::GetErrorAddr() = Posix::POSIX_EBUSY;
    const uint64_t kernel_result = metadata->invoker(nullptr);
    const uint64_t posix_result = symbols.HostCallScalarMetadataFor(posix_record->vaddr)->invoker(nullptr);
    return Check(kernel_result == 0 && posix_result == 0 && *Posix::GetErrorAddr() == Posix::POSIX_EBUSY,
                 "pthread_testcancel without a pending cancellation changed errno");
}

bool NetworkContexts(const Loader::SymbolDatabase& symbols)
{
    // Only process-local pool/context lifecycle: no socket, URL, request,
    // callback, resolver or external connection is involved.
    struct Fixture
    {
        int pool = -1, ssl = -1, http = -1;
        ~Fixture()
        {
            if (http >= 0) Network::Http::HttpTerm(http);
            if (ssl >= 0) Network::Ssl::SslTerm(ssl);
            if (pool >= 0) Network::Net::NetPoolDestroy(pool);
        }
    } fixture;
    fixture.pool = Network::Net::NetPoolCreate("implemented-exports", 4096, 0);
    fixture.ssl = Network::Ssl::SslInit(4096);
    if (!Check(fixture.pool >= 0 && fixture.ssl >= 0, "network context fixture failed")) return false;
    fixture.http = Network::Http::HttpInit(fixture.pool, fixture.ssl, 4096);
    if (!Check(fixture.http >= 0, "HTTP context fixture failed")) return false;
    const int first_http = fixture.http, first_ssl = fixture.ssl;
    if (!Check(Result(symbols, "Ik-KpLTlf7Q", {static_cast<uint32_t>(first_http)}, "libSceHttp") == 0,
               "HTTP context termination failed")) return false;
    fixture.http = -1;
    if (!Check(Result(symbols, "Ik-KpLTlf7Q", {static_cast<uint32_t>(first_http)}, "libSceHttp") == Network::HTTP_ERROR_INVALID_ID &&
               Result(symbols, "0K1yQ6Lv-Yc", {static_cast<uint32_t>(first_ssl)}, "libSceSsl") == 0,
               "HTTP double termination or SSL termination differs")) return false;
    fixture.ssl = -1;
    if (!Check(Result(symbols, "0K1yQ6Lv-Yc", {static_cast<uint32_t>(first_ssl)}, "libSceSsl") == Network::SSL_ERROR_INVALID_ID,
               "SSL double termination did not reject the retired context")) return false;
    fixture.ssl = Result(symbols, "hdpVEUDFW3s", {8192}, "libSceSsl", 2);
    fixture.http = Network::Http::HttpInit(fixture.pool, fixture.ssl, 8192);
    if (!Check(fixture.ssl == first_ssl && fixture.http == first_http,
               "retired network context slots were not reusable")) return false;
    if (!Check(Network::Http::HttpTerm(fixture.http) == 0, "reused HTTP context cleanup failed")) return false;
    fixture.http = -1;
    if (!Check(Result(symbols, "0K1yQ6Lv-Yc", {static_cast<uint32_t>(fixture.ssl)}, "libSceSsl", 2) == 0,
               "SSL 2.1 exact binding did not terminate its context")) return false;
    fixture.ssl = -1;
    return true;
}

bool NpToolkit2Construction(const Loader::SymbolDatabase& symbols)
{
    // The NP Toolkit2 request-object lifecycle is adopted as accepted calls
    // with no guest writes: the console bodies belong to the absent
    // libSceNpToolkit2 image, no layout or library state is reproduced, and
    // the exact identities keep every other NP Toolkit2 symbol fail-closed.
    const auto* record = symbols.Find(Query("vbh-MWtjMh4", "libSceNpToolkit2", 0, 0));
    const auto* metadata = record != nullptr ? symbols.HostCallScalarMetadataFor(record->vaddr) : nullptr;
    if (!Check(record != nullptr && metadata != nullptr && metadata->arity == 1 &&
               metadata->arguments[0] == Loader::HostCallScalarType::Gp &&
               metadata->result == Loader::HostCallScalarType::Void && metadata->invoker != nullptr &&
               symbols.ClassifyHostCallTarget(record->vaddr) == Loader::HostCallRegistryClassification::Host,
               "NP Toolkit2 constructor lost its scalar void(void*) identity")) return false;
    const auto* init = symbols.Find(Query("LLdnqVnnNBQ", "libSceNpToolkit2", 0, 0));
    const auto* init_metadata = init != nullptr ? symbols.HostCallScalarMetadataFor(init->vaddr) : nullptr;
    if (!Check(init != nullptr && init_metadata != nullptr && init_metadata->arity == 1 &&
               init_metadata->arguments[0] == Loader::HostCallScalarType::Gp &&
               init_metadata->result == Loader::HostCallScalarType::Gp && init_metadata->invoker != nullptr &&
               symbols.ClassifyHostCallTarget(init->vaddr) == Loader::HostCallRegistryClassification::Host,
               "NP Toolkit2 core init lost its scalar int32(const void*) identity")) return false;
    const auto* destroy = symbols.Find(Query("-bAjEe-hdCg", "libSceNpToolkit2", 0, 0));
    const auto* destroy_metadata = destroy != nullptr ? symbols.HostCallScalarMetadataFor(destroy->vaddr) : nullptr;
    if (!Check(destroy != nullptr && destroy_metadata != nullptr && destroy_metadata->arity == 1 &&
               destroy_metadata->arguments[0] == Loader::HostCallScalarType::Gp &&
               destroy_metadata->result == Loader::HostCallScalarType::Void && destroy_metadata->invoker != nullptr &&
               symbols.ClassifyHostCallTarget(destroy->vaddr) == Loader::HostCallRegistryClassification::Host,
               "NP Toolkit2 request destructor lost its scalar void(void*) identity")) return false;
    // The service call of the trophy family returns the local accepted code: a
    // scalar int32(const void*, void*) whose result the observed call site
    // treats as a status (negative = error).
    const auto* register_call = symbols.Find(Query("buMmdpEwRuM", "libSceNpToolkit2", 0, 0));
    const auto* register_metadata =
        register_call != nullptr ? symbols.HostCallScalarMetadataFor(register_call->vaddr) : nullptr;
    if (!Check(register_call != nullptr && register_metadata != nullptr && register_metadata->arity == 2 &&
               register_metadata->arguments[0] == Loader::HostCallScalarType::Gp &&
               register_metadata->arguments[1] == Loader::HostCallScalarType::Gp &&
               register_metadata->result == Loader::HostCallScalarType::Gp && register_metadata->invoker != nullptr &&
               symbols.ClassifyHostCallTarget(register_call->vaddr) == Loader::HostCallRegistryClassification::Host,
               "NP Toolkit2 trophy service call lost its scalar int32(const void*, void*) identity")) return false;
    // The request function-type accessor adopts the accepted no-state shape:
    // a scalar int32(const void*) that publishes no request-object state and
    // returns the local unpublished marker 0.
    const auto* function_type = symbols.Find(Query("El4tFb9f8dk", "libSceNpToolkit2", 0, 0));
    const auto* function_type_metadata =
        function_type != nullptr ? symbols.HostCallScalarMetadataFor(function_type->vaddr) : nullptr;
    if (!Check(function_type != nullptr && function_type_metadata != nullptr && function_type_metadata->arity == 1 &&
               function_type_metadata->arguments[0] == Loader::HostCallScalarType::Gp &&
               function_type_metadata->result == Loader::HostCallScalarType::Gp &&
               function_type_metadata->invoker != nullptr &&
               symbols.ClassifyHostCallTarget(function_type->vaddr) == Loader::HostCallRegistryClassification::Host,
               "NP Toolkit2 request function type lost its scalar int32(const void*) identity")) return false;
    // The response and trophy request objects adopt the same accepted no-state
    // shape: exact identity, scalar void(void*) metadata, no guest reads or
    // writes and a bounded diagnostic record.
    const std::array<const char*, 4> object_nids {
        "tlpXm5KLgqk", "30Zxe35LtBg", "wkgY4lDEV9k", "ItZXngg8tfo"};
    for (const char* nid: object_nids)
    {
        const auto* object_record = symbols.Find(Query(nid, "libSceNpToolkit2", 0, 0));
        const auto* object_metadata =
            object_record != nullptr ? symbols.HostCallScalarMetadataFor(object_record->vaddr) : nullptr;
        if (!Check(object_record != nullptr && object_metadata != nullptr && object_metadata->arity == 1 &&
                   object_metadata->arguments[0] == Loader::HostCallScalarType::Gp &&
                   object_metadata->result == Loader::HostCallScalarType::Void && object_metadata->invoker != nullptr &&
                   symbols.ClassifyHostCallTarget(object_record->vaddr) == Loader::HostCallRegistryClassification::Host,
                   "NP Toolkit2 object profile lost its scalar void(void*) identity")) return false;
    }
    const std::array<std::array<int, 3>, 4> wrong_identities {{{0, 1, 1}, {1, 0, 1}, {1, 1, 1}, {0, 0, 0}}};
    for (const auto& identity: wrong_identities)
        for (const char* nid: {"vbh-MWtjMh4", "LLdnqVnnNBQ", "-bAjEe-hdCg", "tlpXm5KLgqk", "30Zxe35LtBg",
                               "wkgY4lDEV9k", "ItZXngg8tfo", "buMmdpEwRuM", "El4tFb9f8dk"})
            if (!Check(symbols.Find(Query(nid, "libSceNpToolkit2", identity[0], identity[1], identity[2])) == nullptr,
                       "NP Toolkit2 lifecycle gained a version fallback")) return false;
    for (const char* nid: {"LLdnqVnnNBQ", "-bAjEe-hdCg", "vbh-MWtjMh4", "tlpXm5KLgqk", "30Zxe35LtBg",
                           "wkgY4lDEV9k", "ItZXngg8tfo", "buMmdpEwRuM", "El4tFb9f8dk"})
        if (!Check(symbols.Find(Query(nid, "NpManager", 1, 1)) == nullptr,
                   "NP Toolkit2 lifecycle gained a namespace fallback")) return false;
    if (!Check(symbols.Find(Query("SaKib2Ug0yI", "libSceNpToolkit2", 0, 0)) == nullptr,
               "NP Toolkit2 lifecycle gained a NID fallback")) return false;
    std::array<uint8_t, 64> object {};
    object.fill(0xa5);
    const auto original = object;
    for (int call = 0; call < 2; ++call)
    {
        const uint64_t self = reinterpret_cast<uint64_t>(object.data());
        if (!Check(metadata->invoker(&self) == 0 && object == original,
                   "NP Toolkit2 constructor wrote guest state or failed its accepted call")) return false;
    }
    for (int call = 0; call < 2; ++call)
    {
        const uint64_t self = reinterpret_cast<uint64_t>(object.data());
        if (!Check(init_metadata->invoker(&self) == 0 && object == original,
                   "NP Toolkit2 core init wrote guest state or did not return the accepted result")) return false;
        if (!Check(destroy_metadata->invoker(&self) == 0 && object == original,
                   "NP Toolkit2 request destructor wrote guest state")) return false;
    }
    for (const char* nid: object_nids)
    {
        const auto* object_record = symbols.Find(Query(nid, "libSceNpToolkit2", 0, 0));
        const auto* object_metadata = symbols.HostCallScalarMetadataFor(object_record->vaddr);
        for (int call = 0; call < 2; ++call)
        {
            const uint64_t self = reinterpret_cast<uint64_t>(object.data());
            if (!Check(object_metadata->invoker(&self) == 0 && object == original,
                       "NP Toolkit2 object profile wrote guest state or did not accept its call")) return false;
        }
    }
    for (int call = 0; call < 2; ++call)
    {
        std::array<uint8_t, 64> response {};
        response.fill(0x5a);
        const auto response_original = response;
        const std::array<uint64_t, 2> arguments {reinterpret_cast<uint64_t>(object.data()),
                                                 reinterpret_cast<uint64_t>(response.data())};
        if (!Check(register_metadata->invoker(arguments.data()) == 0 && object == original &&
                       response == response_original,
                   "NP Toolkit2 trophy service call wrote guest state or did not return the accepted result")) return false;
    }
    for (int call = 0; call < 2; ++call)
    {
        const uint64_t self = reinterpret_cast<uint64_t>(object.data());
        if (!Check(function_type_metadata->invoker(&self) == 0 && object == original,
                   "NP Toolkit2 request function type wrote guest state or did not return the unpublished marker")) return false;
    }
    return true;
}

bool UserServiceColorProfile(const Loader::SymbolDatabase& symbols)
{
    // Local profile of sceUserServiceGetUserColor: the offline model keeps
    // exactly one logged-in user (id 1) and answers with the first value of the
    // documented colour enumeration. Other ids are not logged in and every
    // rejection publishes nothing; the read-only and cross-boundary fixtures
    // prove the output range is validated before the single write.
    const auto* record = symbols.Find(Query("lUoqwTQu4Go", "libSceUserService", 1, 1));
    const auto* metadata = record != nullptr ? symbols.HostCallScalarMetadataFor(record->vaddr) : nullptr;
    if (!Check(record != nullptr && metadata != nullptr && metadata->arity == 2 &&
                   metadata->arguments[0] == Loader::HostCallScalarType::Gp &&
                   metadata->arguments[1] == Loader::HostCallScalarType::Gp &&
                   metadata->result == Loader::HostCallScalarType::Gp && metadata->invoker != nullptr &&
                   symbols.ClassifyHostCallTarget(record->vaddr) == Loader::HostCallRegistryClassification::Host,
               "user color profile lost its scalar int32(int, void*) identity")) return false;
    const std::array<std::array<int, 3>, 5> wrong_identities {{{0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 2, 1}, {1, 1, 0}}};
    for (const auto& identity: wrong_identities)
        if (!Check(symbols.Find(Query("lUoqwTQu4Go", "libSceUserService", identity[0], identity[1], identity[2])) == nullptr,
                   "user color profile gained a version fallback")) return false;
    if (!Check(symbols.Find(Query("lUoqwTQu4Go", "NpManager", 1, 1)) == nullptr,
               "user color profile gained a namespace fallback")) return false;

    constexpr size_t unit = 0x4000;
    struct Mapping
    {
        uint64_t address = Memory::Alloc(0, 3 * unit, Memory::Mode::ReadWrite);
        ~Mapping()
        {
            if (address != 0) Memory::Free(address);
        }
    } mapping;
    if (!Check(mapping.address != 0, "user color fixture allocation failed")) return false;
    constexpr int32_t untouched = 0x13572468;
    auto* color = reinterpret_cast<int32_t*>(mapping.address + 128);
    std::array<uint8_t, 16> window {};
    window.fill(0xa5);
    const auto original_window = window;
    std::memcpy(window.data() + 8, &untouched, sizeof(untouched));
    std::memcpy(reinterpret_cast<void*>(mapping.address + 128 - 8), window.data(), window.size());
    if (!Check(Result(symbols, "lUoqwTQu4Go", {1, mapping.address + 128}, "libSceUserService", 1) == 0 && *color == 0,
               "logged-in user color did not publish the local color")) return false;
    if (!Check(std::memcmp(reinterpret_cast<const void*>(mapping.address + 128 - 8), original_window.data(), 8) == 0 &&
                   std::memcmp(reinterpret_cast<const void*>(mapping.address + 128 + 4), original_window.data() + 12, 4) == 0,
               "user color published outside the 32-bit output")) return false;
    *color = untouched;
    if (!Check(Result(symbols, "lUoqwTQu4Go", {1, mapping.address + 128}, "libSceUserService", 1) == 0 && *color == 0,
               "repeated user color query is not stable")) return false;
    for (int32_t user: {0, 2, -1, 0x7fffffff})
    {
        *color = untouched;
        const int32_t result = Result(symbols, "lUoqwTQu4Go",
                                      {static_cast<uint64_t>(static_cast<int64_t>(user)), mapping.address + 128},
                                      "libSceUserService", 1);
        if (!Check(result == UserService::USER_SERVICE_ERROR_NOT_LOGGED_IN && *color == untouched,
                   "query for a user that is not logged in published a color")) return false;
    }
    if (!Check(Result(symbols, "lUoqwTQu4Go", {1, 0}, "libSceUserService", 1) == UserService::USER_SERVICE_ERROR_INVALID_ARGUMENT &&
                   Result(symbols, "lUoqwTQu4Go", {1, UINT64_C(1)}, "libSceUserService", 1) ==
                       UserService::USER_SERVICE_ERROR_INVALID_ARGUMENT &&
                   Result(symbols, "lUoqwTQu4Go", {1, UINT64_MAX - 3}, "libSceUserService", 1) ==
                       UserService::USER_SERVICE_ERROR_INVALID_ARGUMENT,
               "invalid color outputs were answered")) return false;
    auto* read_only = reinterpret_cast<int32_t*>(mapping.address + unit + 128);
    *read_only = untouched;
    if (!Check(Memory::Protect(mapping.address + unit, unit, Memory::Mode::Read),
               "user color read-only fixture setup failed")) return false;
    const int32_t read_only_result = Result(symbols, "lUoqwTQu4Go", {1, mapping.address + unit + 128}, "libSceUserService", 1);
    if (!Check(Memory::Protect(mapping.address + unit, unit, Memory::Mode::ReadWrite),
               "user color read-only fixture restore failed")) return false;
    if (!Check(read_only_result == UserService::USER_SERVICE_ERROR_INVALID_ARGUMENT && *read_only == untouched,
               "read-only color output published a value")) return false;
    auto* crossing = reinterpret_cast<int32_t*>(mapping.address + 2 * unit - 2);
    *crossing = untouched;
    if (!Check(Memory::Protect(mapping.address + 2 * unit, unit, Memory::Mode::NoAccess),
               "user color guard fixture setup failed")) return false;
    const int32_t crossing_result = Result(symbols, "lUoqwTQu4Go", {1, mapping.address + 2 * unit - 2}, "libSceUserService", 1);
    if (!Check(Memory::Protect(mapping.address + 2 * unit, unit, Memory::Mode::ReadWrite),
               "user color guard fixture restore failed")) return false;
    return Check(crossing_result == UserService::USER_SERVICE_ERROR_INVALID_ARGUMENT && *crossing == untouched,
                 "cross-boundary color output partially wrote a value");
}

bool PadOpenProfile(const Loader::SymbolDatabase& symbols)
{
    // Local profile of scePadOpen: the port-type domain measured on the
    // console (oracle LMNQ92801) recognizes standard (0) and special (2), the
    // special port opens device-less and every other type returns the
    // measured invalid-argument error with precedence over the user check.
    if (!Check(Result(symbols, "xk0AcarP3V4", {1, 0, 0, 0}, "libScePad", 1) == Controller::PAD_STANDARD_PORT_HANDLE,
               "standard port open changed")) return false;
    if (!Check(Result(symbols, "xk0AcarP3V4", {1, 2, 0, 0}, "libScePad", 1) == Controller::PAD_SPECIAL_PORT_HANDLE,
               "special port open changed")) return false;
    for (int type: {1, 3, -1, std::numeric_limits<int>::max()})
    {
        for (int user: {1, 0})
        {
            if (!Check(Result(symbols, "xk0AcarP3V4",
                              {static_cast<uint64_t>(static_cast<int64_t>(user)),
                               static_cast<uint64_t>(static_cast<int64_t>(type)), 0, 0},
                              "libScePad", 1) == Controller::PAD_ERROR_INVALID_ARGUMENT,
                       "invalid port type did not return the measured invalid-argument")) return false;
        }
    }
    // The special handle publishes the measured device-less information
    // (connection_type=0, connected_count=0, connected=false, device_class=0)
    // inside the modeled record; bytes outside it keep their canaries.
    std::array<uint8_t, 64> window {};
    window.fill(0xa5);
    const auto original = window;
    const auto* info = window.data() + 16;
    for (int call = 0; call < 2; ++call)
    {
        window = original;
        const int result = Result(symbols, "gjP9-KQzoUk", {Controller::PAD_SPECIAL_PORT_HANDLE, Word(info)}, "libScePad", 1);
        int32_t device_class = -1;
        std::memcpy(&device_class, info + 16, sizeof(device_class));
        if (!Check(result == 0 && info[10] == 0 && info[11] == 0 && info[12] == 0 &&
                       device_class == Controller::PAD_MODELED_DEVICE_CLASS &&
                       std::equal(window.begin(), window.begin() + 16, original.begin()) &&
                       std::equal(window.begin() + 36, window.end(), original.begin() + 36),
                   "special port information did not publish exactly the modeled window")) return false;
    }
    return true;
}

bool PadDeviceClassProfile(const Loader::SymbolDatabase& symbols)
{
    // Measured console profile of scePadDeviceClassGetExtendedInformation
    // (oracle LMNQ92802): every admitted call stores the 20-byte record (a
    // 32-bit device class plus a zeroed 16-byte area); the standard port
    // reports class 0, the device-less special port reports class -1
    // (Invalid) and a rejected handle still receives the default record
    // (class 0) together with the shared invalid-handle error.
    const auto* record = symbols.Find(Query("AcslpN1jHR8", "libScePad", 1, 1));
    const auto* metadata = record != nullptr ? symbols.HostCallScalarMetadataFor(record->vaddr) : nullptr;
    if (!Check(record != nullptr && metadata != nullptr && metadata->arity == 2 &&
                   metadata->arguments[0] == Loader::HostCallScalarType::Gp &&
                   metadata->arguments[1] == Loader::HostCallScalarType::Gp &&
                   metadata->result == Loader::HostCallScalarType::Gp && metadata->invoker != nullptr &&
                   symbols.ClassifyHostCallTarget(record->vaddr) == Loader::HostCallRegistryClassification::Host,
               "device class profile lost its scalar int32(int, void*) identity")) return false;
    const std::array<std::array<int, 3>, 5> wrong_identities {{{0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 2, 1}, {1, 1, 0}}};
    for (const auto& identity: wrong_identities)
        if (!Check(symbols.Find(Query("AcslpN1jHR8", "libScePad", identity[0], identity[1], identity[2])) == nullptr,
                   "device class profile gained a version fallback")) return false;
    if (!Check(symbols.Find(Query("AcslpN1jHR8", "libSceMouse", 1, 1)) == nullptr,
               "device class profile gained a namespace fallback")) return false;

    std::array<uint8_t, 64> window {};
    window.fill(0x5a);
    const auto original = window;
    auto* info = window.data() + 8;
    for (int call = 0; call < 2; ++call)
    {
        window = original;
        const int result = Result(symbols, "AcslpN1jHR8", {1, Word(info)}, "libScePad", 1);
        int32_t device_class = 0;
        std::memcpy(&device_class, window.data() + 8, sizeof(device_class));
        if (!Check(result == 0 && device_class == Controller::PAD_MODELED_DEVICE_CLASS &&
                       std::all_of(window.begin() + 12, window.begin() + 28, [](uint8_t byte) { return byte == 0; }) &&
                       std::equal(window.begin(), window.begin() + 8, original.begin()) &&
                       std::equal(window.begin() + 28, window.end(), original.begin() + 28),
                   "standard device class query did not publish exactly the measured window")) return false;
    }
    window = original;
    int32_t special_class = 0;
    if (!Check(Result(symbols, "AcslpN1jHR8", {Controller::PAD_SPECIAL_PORT_HANDLE, Word(info)}, "libScePad", 1) == 0,
               "special device class query changed its result")) return false;
    std::memcpy(&special_class, window.data() + 8, sizeof(special_class));
    if (!Check(special_class == Controller::PAD_DEVICE_CLASS_INVALID &&
                   std::all_of(window.begin() + 12, window.begin() + 28, [](uint8_t byte) { return byte == 0; }) &&
                   std::equal(window.begin(), window.begin() + 8, original.begin()) &&
                   std::equal(window.begin() + 28, window.end(), original.begin() + 28),
               "device-less special port did not publish the measured invalid class")) return false;

    constexpr std::array<int, 6> invalid_handles {{
        -1, 0, 3, std::numeric_limits<int>::min(), std::numeric_limits<int>::max(), Controller::VIRTUAL_CONTROLLER_ID}};
    for (int handle: invalid_handles)
    {
        window = original;
        auto expected = original;
        std::fill(expected.begin() + 8, expected.begin() + 28, uint8_t {0});
        if (!Check(Result(symbols, "AcslpN1jHR8",
                          {static_cast<uint64_t>(static_cast<int64_t>(handle)), Word(info)}, "libScePad", 1) ==
                       Controller::PAD_ERROR_INVALID_HANDLE &&
                       window == expected,
                   "rejected device class handle did not store exactly the default record")) return false;
    }
    return true;
}

bool NpAccountIdProfile(const Loader::SymbolDatabase& symbols)
{
    // Local offline profile of sceNpGetAccountIdA: the identifier belongs to a
    // PlayStation Network sign-in and the emulated console has none, so every
    // admitted call is answered with the documented signed-out error and the
    // eight-byte output keeps its previous contents. A null output is rejected
    // before that answer and the result is stable on repeated calls.
    const auto* record = symbols.Find(Query("rbknaUjpqWo", "libSceNpManager", 1, 1));
    const auto* metadata = record != nullptr ? symbols.HostCallScalarMetadataFor(record->vaddr) : nullptr;
    if (!Check(record != nullptr && metadata != nullptr && metadata->arity == 2 &&
                   metadata->arguments[0] == Loader::HostCallScalarType::Gp &&
                   metadata->arguments[1] == Loader::HostCallScalarType::Gp &&
                   metadata->result == Loader::HostCallScalarType::Gp && metadata->invoker != nullptr &&
                   symbols.ClassifyHostCallTarget(record->vaddr) == Loader::HostCallRegistryClassification::Host,
               "account id profile lost its scalar int32(int, void*) identity")) return false;
    const std::array<std::array<int, 3>, 5> wrong_identities {{{0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 2, 1}, {1, 1, 0}}};
    for (const auto& identity: wrong_identities)
        if (!Check(symbols.Find(Query("rbknaUjpqWo", "libSceNpManager", identity[0], identity[1], identity[2])) == nullptr,
                   "account id profile gained a version fallback")) return false;
    if (!Check(symbols.Find(Query("rbknaUjpqWo", "libSceUserService", 1, 1)) == nullptr,
               "account id profile gained a namespace fallback")) return false;

    std::array<uint8_t, 24> window {};
    window.fill(0x5a);
    const auto original = window;
    auto* account_id = reinterpret_cast<uint64_t*>(window.data() + 8);
    for (int call = 0; call < 2; ++call)
        if (!Check(Result(symbols, "rbknaUjpqWo", {1, Word(account_id)}, "libSceNpManager", 1) ==
                           Network::NP_ERROR_SIGNED_OUT &&
                       window == original,
                   "signed-out account query published an identifier or changed its result")) return false;
    window = original;
    if (!Check(Result(symbols, "rbknaUjpqWo", {static_cast<uint64_t>(UINT64_C(0x7fffffff)), Word(account_id)},
                      "libSceNpManager", 1) == Network::NP_ERROR_SIGNED_OUT &&
                   window == original,
               "account query for an unknown user published an identifier")) return false;
    return Check(Result(symbols, "rbknaUjpqWo", {1, 0}, "libSceNpManager", 1) == Network::NP_ERROR_INVALID_ARGUMENT,
                 "null account output was not rejected");
}
} // namespace

int main(int argc, char** argv)
{
    const bool dump_symbols = argc == 3 && std::strcmp(argv[1], "--dump-symbols") == 0;
    if (argc != 1 && !dump_symbols)
    {
        std::fprintf(stderr, "usage: HleImplementedExportsTest [--dump-symbols directory]\n");
        return 2;
    }
    Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
    Lumen::Core::core_file_init();
    Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
    Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
    Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
    Memory::Init();
    Kernel::PthreadSubsystem::Instance()->Init(nullptr);
    FileSystem::FileSystemSubsystem::Instance()->Init(nullptr);
    Network::NetworkSubsystem::Instance()->Init(nullptr);
    Loader::SymbolDatabase symbols;
    Lumen::Libs::InitAll(&symbols);
    if (dump_symbols) symbols.DbgDump(Lumen::String::FromUtf8(argv[2]), U"hle-symbols.txt");
    if (!Bindings(symbols) || !PosixAliases(symbols) || !EventRecords(symbols) || !AttributesAndLocks(symbols) || !PollingAndTime(symbols) ||
        !CancellationType(symbols) || !CancellationPoint(symbols) || !NetworkContexts(symbols) || !NpToolkit2Construction(symbols) ||
        !UserServiceColorProfile(symbols) || !NpAccountIdProfile(symbols) || !PadOpenProfile(symbols) ||
        !PadDeviceClassProfile(symbols)) return 1;
    std::puts("HleImplementedExportsTest: PASS");
    return 0;
}
