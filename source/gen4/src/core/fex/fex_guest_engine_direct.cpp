// SPDX-License-Identifier: MIT

#include "fex_guest_engine.h"
#include "host_mapping_snapshot.h"

// Ucontext layout only — do not include pthread.h here. pthread → semaphore →
// assert → log → spdlog, which the FEXCore-only guest harness does not provide.
#include "core/libraries/kernel/threads/exception.h"
#include "Common/Config.h"
#include "Common/HostFeatures.h"
#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/ArchHelpers/Arm64.h>
#include <FEXCore/Utils/LogManager.h>

#include <sys/mman.h>
#include <unistd.h>
#include <sys/ucontext.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <FEXCore/Utils/AllocatorHooks.h>
#endif

#include <array>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <pthread.h>
#include <bit>

// Weak fallback when exception.cpp is not linked (fexcore-guest-harness).
// Full Gen4 provides the strong definition that reads g_curthread.
namespace Libraries::Kernel {
__attribute__((weak)) u64 FexCurrentGuestStackTop() noexcept {
  return 0;
}
__attribute__((weak)) void FexProcessPendingSignals() {}
} // namespace Libraries::Kernel
#include <limits>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Core::Fex {
namespace {

constexpr long kRequiredPageSize = 4096;
constexpr uint32_t kFexBlockTraceLimit = 256;
constexpr uint64_t kAddLeft = 0x1122'3344'5566'7788ULL;
constexpr uint64_t kAddRight = 0x0102'0304'0506'0708ULL;
constexpr uint64_t kXorLeft = 0xfedc'ba98'7654'3210ULL;
constexpr uint64_t kXorRight = 0x0f0f'0f0f'0f0f'0f0fULL;
constexpr uint64_t kStackSentinel = 0xaabb'ccdd'eeff'0011ULL;
constexpr uint64_t kUnalignedSentinel = 0x8877'6655'4433'2211ULL;
constexpr uint64_t kThreadSentinelA = 0x1111'2222'3333'4444ULL;
constexpr uint64_t kThreadSentinelB = 0x5555'6666'7777'8888ULL;
constexpr uint64_t kCallbackInput = 0x1020'3040'5060'7080ULL;
constexpr uint64_t kInvalidationInitial = 0x0123'4567'89ab'cdefULL;
constexpr uint64_t kInvalidationUpdated = 0xfedc'ba98'7654'3210ULL;

std::atomic_uint32_t FexBlockTraceCount {};

#if defined(__aarch64__)
std::uintptr_t HostProgramCounter(const ucontext_t* context) noexcept {
#if defined(__APPLE__)
  return context->uc_mcontext == nullptr ? 0 : context->uc_mcontext->__ss.__pc;
#else
  return context->uc_mcontext.pc;
#endif
}

std::uintptr_t HostStackPointer(const ucontext_t* context) noexcept {
#if defined(__APPLE__)
  return context->uc_mcontext == nullptr ? 0 : context->uc_mcontext->__ss.__sp;
#else
  return context->uc_mcontext.sp;
#endif
}

std::uint64_t HostRegister(const ucontext_t* context, size_t index) noexcept {
#if defined(__APPLE__)
  if (context->uc_mcontext == nullptr || index > 30) return 0;
  const auto& state = context->uc_mcontext->__ss;
  if (index == 29) return state.__fp;
  if (index == 30) return state.__lr;
  return state.__x[index];
#else
  return index <= 30 ? context->uc_mcontext.regs[index] : 0;
#endif
}

void CommitHostRegisters(ucontext_t* context, std::uintptr_t pc,
                         const std::array<std::uint64_t, 31>& registers) noexcept {
#if defined(__APPLE__)
  auto& state = context->uc_mcontext->__ss;
  std::copy_n(registers.begin(), 29, state.__x);
  state.__fp = registers[29];
  state.__lr = registers[30];
  state.__pc = pc;
#else
  std::copy(registers.begin(), registers.end(), context->uc_mcontext.regs);
  context->uc_mcontext.pc = pc;
#endif
}

bool IsHostAlignmentFault(const siginfo_t* info, const ucontext_t* context) noexcept {
  if (info == nullptr || context == nullptr || info->si_code != BUS_ADRALN) return false;
#if defined(__APPLE__)
  if (context->uc_mcontext == nullptr) return false;
  // Darwin also uses BUS_ADRALN for translation/permission faults. Only the
  // architectural alignment syndrome may enter FEX's instruction patcher.
  const auto syndrome = context->uc_mcontext->__es.__esr;
  const auto exceptionClass = (syndrome >> 26) & 0x3f;
  return (exceptionClass == 0x24 || exceptionClass == 0x25) && (syndrome & 0x3f) == 0x21;
#else
  return true;
#endif
}
#endif

FEXCore::Core::InternalThreadState* CreateNativeThread(FEXCore::Context::Context& context,
                                                       std::uintptr_t rip, std::uintptr_t rsp) {
#if defined(__APPLE__)
  FEXCore::Allocator::ScopedJITWriteProtection writableJit;
#endif
  return context.CreateThread(rip, rsp);
}

// Forward-declare so ActiveFexExecution can hold the HLE bridge for deferred flush.
class BridgeSyscallHandler;

struct ActiveFexExecutionState final {
  FEXCore::Context::Context* Context {};
  FEXCore::Core::InternalThreadState* Thread {};
  FEXCore::SignalDelegator* SignalDelegator {};
  BridgeSyscallHandler* Syscalls {};
};

thread_local ActiveFexExecutionState ActiveFexExecution;

struct PendingOrbisSignalState final {
  std::uintptr_t Handler {};
  int OrbisSig {};
  bool Pending {};
  bool Flushing {};
  bool SiginfoArguments {};
  std::uintptr_t WorkRsp {};
  // Host aarch64 snapshot at kill time so we can spill SRA → guest GPRs before
  // HandleCallback (mid-JIT live state is NOT in CurrentFrame).
  bool HasHostSnapshot {};
  std::uintptr_t HostPc {};
  std::array<std::uint64_t, 31> HostGprs {};
};
thread_local PendingOrbisSignalState PendingOrbisSignal {};

class FexExecutionSignalScope final {
public:
  FexExecutionSignalScope(FEXCore::Context::Context& context,
                          FEXCore::Core::InternalThreadState* thread,
                          FEXCore::SignalDelegator* signalDelegator,
                          BridgeSyscallHandler* syscalls = nullptr)
    : Previous {ActiveFexExecution} {
    ActiveFexExecution = {&context, thread, signalDelegator, syscalls};
  }

  FexExecutionSignalScope(const FexExecutionSignalScope&) = delete;
  FexExecutionSignalScope& operator=(const FexExecutionSignalScope&) = delete;

  ~FexExecutionSignalScope() {
    ActiveFexExecution = Previous;
  }

private:
  ActiveFexExecutionState Previous;
};

void FexMessageHandler(LogMan::DebugLevels level, const char* message) {
  if (message == nullptr) return;
  if (std::strstr(message, "Guest x86 Begin") != nullptr) {
    const auto index = FexBlockTraceCount.fetch_add(1, std::memory_order_relaxed);
    if (index < kFexBlockTraceLimit) {
      std::fprintf(stderr, "LUMEN_HOST_FEX_BLOCK index=%u %s\n", index, message);
    }
  } else if (level <= LogMan::ERROR) {
    std::fprintf(stderr, "LUMEN_HOST_FEX_ERROR level=%u %s\n", static_cast<unsigned>(level),
                 message);
  }
}

EngineFailure Failure(EngineStage stage, int error) {
  return {stage, error == 0 ? EIO : error};
}

bool Contains(const GuestExecutionRange& range, std::uintptr_t address, std::size_t size) {
  if (range.Begin == 0 || range.Size == 0 || size == 0 || address < range.Begin) {
    return false;
  }
  const auto rangeEnd = range.Begin + range.Size;
  const auto addressEnd = address + size;
  return rangeEnd >= range.Begin && addressEnd >= address && addressEnd <= rangeEnd;
}

bool RangesOverlap(const GuestExecutionRange& lhs, const GuestExecutionRange& rhs) {
  const auto lhsEnd = lhs.Begin + lhs.Size;
  const auto rhsEnd = rhs.Begin + rhs.Size;
  return lhsEnd >= lhs.Begin && rhsEnd >= rhs.Begin && lhs.Begin < rhsEnd && rhs.Begin < lhsEnd;
}

EngineResult<std::vector<detail::HostMapping>> CaptureHostMappings() {
#if defined(__APPLE__)
  std::vector<detail::HostMapping> mappings;
  mach_vm_address_t cursor = 0;
  for (;;) {
    mach_vm_address_t begin = cursor;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t information {};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    const auto result = mach_vm_region(mach_task_self(), &begin, &size,
        VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&information), &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    if (result == KERN_INVALID_ADDRESS && !mappings.empty()) return mappings;
    if (result != KERN_SUCCESS || size == 0 || begin > UINTPTR_MAX - size || begin + size <= cursor)
      return Failure(EngineStage::Mapping, EFAULT);
    // A neighboring mapping can merge with a region already visited while
    // another thread changes its permissions. mach_vm_region then returns
    // the containing region, whose beginning precedes the requested cursor.
    // Replace that overlapping suffix with its current permissions instead
    // of rejecting an unrelated stack or emitting overlapping mappings.
    if (begin < cursor) {
      while (!mappings.empty() && mappings.back().Begin >= begin) mappings.pop_back();
      if (!mappings.empty() && mappings.back().End > begin) mappings.back().End = begin;
    }
    const auto protection = information.protection;
    cursor = begin + size;
    mappings.push_back({begin, cursor, (protection & VM_PROT_READ) != 0,
        (protection & VM_PROT_WRITE) != 0, (protection & VM_PROT_EXECUTE) != 0});
  }
#else
  constexpr unsigned kSnapshotAttempts = 8;
  for (unsigned attempt = 1; attempt <= kSnapshotAttempts; ++attempt) {
    std::ifstream maps {"/proc/self/maps"};
    if (attempt == 1 && !maps.is_open()) {
      return Failure(EngineStage::Mapping, errno == 0 ? EACCES : errno);
    }
    auto snapshot = detail::ParseHostMappings(maps);
    if (snapshot) {
      if (attempt > 1) {
        std::fprintf(stderr, "LUMEN_HOST_FEX_MAPPING_RETRY attempts=%u\n", attempt);
      }
      return std::move(*snapshot);
    }
  }
  std::fprintf(stderr, "LUMEN_HOST_FEX_MAPPING_FAIL reason=invalid_proc_maps attempts=%u\n",
               kSnapshotAttempts);
  return Failure(EngineStage::Mapping, EFAULT);
#endif
}

EngineResult<bool> ValidateHostMapping(std::span<const detail::HostMapping> mappings,
                                     const GuestExecutionRange& range) {
  const auto result = detail::CheckHostMapping(mappings, range.Begin, range.Size,
                                              range.Executable, range.Writable);
  if (result == detail::HostMappingResult::Valid) return true;
  const auto error = result == detail::HostMappingResult::Gap ? EFAULT : EACCES;
  const auto* reason = result == detail::HostMappingResult::Gap ? "host_mapping_gap" :
      result == detail::HostMappingResult::CodePermission ? "host_guest_code_permission" :
                                                            "host_write_permission";
  detail::HostMapping conflict{};
  for (const auto& mapping : mappings) {
    if (mapping.End <= range.Begin) continue;
    if (mapping.Begin >= range.Begin + range.Size) break;
    if ((range.Executable && (!mapping.Readable || mapping.Writable)) ||
        (range.Writable && (!mapping.Writable || mapping.Executable))) {
      conflict = mapping;
      break;
    }
  }
  std::fprintf(stderr,
               "LUMEN_HOST_FEX_MAPPING_FAIL reason=%s error=%d begin=%#lx size=%#lx "
               "executable=%d writable=%d host_begin=%#lx host_end=%#lx "
               "host_readable=%d host_writable=%d host_executable=%d\n",
               reason, error, static_cast<unsigned long>(range.Begin),
               static_cast<unsigned long>(range.Size), range.Executable, range.Writable,
               static_cast<unsigned long>(conflict.Begin), static_cast<unsigned long>(conflict.End),
               conflict.Readable, conflict.Writable, conflict.Executable);
  return Failure(EngineStage::Mapping, error);
}

EngineResult<bool> ValidateRequest(const GuestExecutionRequest& request) {
  if (request.Rip == 0 || request.Rsp == 0 || request.MappedRanges.empty()) {
    return Failure(EngineStage::Request, EINVAL);
  }
  const auto snapshot = CaptureHostMappings();
  if (const auto* failure = std::get_if<EngineFailure>(&snapshot)) return *failure;
  const auto& mappings = std::get<std::vector<detail::HostMapping>>(snapshot);
  auto ranges = request.MappedRanges;
  std::sort(ranges.begin(), ranges.end(), [](const auto& left, const auto& right) {
    return left.Begin < right.Begin;
  });

  bool executableRip = false;
  bool writableStack = false;
  for (size_t index = 0; index < ranges.size(); ++index) {
    const auto& range = ranges[index];
    if (range.Begin == 0 || range.Size == 0 || range.Begin % kRequiredPageSize != 0 ||
        range.Size % kRequiredPageSize != 0 || range.Begin + range.Size < range.Begin) {
      return Failure(EngineStage::Request, EINVAL);
    }
    if (range.Executable && range.Writable) {
      std::fprintf(stderr,
                   "LUMEN_HOST_FEX_MAPPING_FAIL reason=guest_wx error=%d index=%zu begin=%#lx size=%#lx\n",
                   EACCES, index, static_cast<unsigned long>(range.Begin),
                   static_cast<unsigned long>(range.Size));
      return Failure(EngineStage::Mapping, EACCES);
    }
    if (index != 0 && RangesOverlap(range, ranges[index - 1])) {
      return Failure(EngineStage::Mapping, EACCES);
    }
    const auto hostMapping = ValidateHostMapping(mappings, range);
    if (const auto* failure = std::get_if<EngineFailure>(&hostMapping)) return *failure;
    executableRip |= range.Executable && Contains(range, request.Rip, 1);
    writableStack |= range.Writable && Contains(range, request.Rsp, 1);
  }
  if (!executableRip || !writableStack) {
    return Failure(EngineStage::Request, EFAULT);
  }
  return true;
}

bool FexTraceEnabled() {
  static const bool enabled = [] {
        const char* value = std::getenv("LUMEN_HOST_FEX_TRACE");
        if (value == nullptr) return false;
        const std::string_view input(value);
        const auto matches = [&](std::string_view expected) {
          if (input.size() != expected.size()) return false;
          for (std::size_t index = 0; index < input.size(); ++index) {
            auto character = input[index];
            if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
            if (character != expected[index]) return false;
          }
          return true;
        };
        return matches("1") || matches("true") || matches("yes") || matches("on");
      }();
  return enabled;
}

class FexConfigLease final {
public:
  static EngineResult<bool> Acquire() {
    std::scoped_lock lock {Mutex};
    if (Users == 0) {
      FEX::Config::InitializeConfigs({});
      FEXCore::Config::Initialize();
      FEXCore::Config::AddLayer(FEX::Config::CreateEnvironmentLayer(nullptr));
      FEXCore::Config::Load();
      FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
      FEXCore::Config::Set(FEXCore::Config::CONFIG_DISABLETELEMETRY, "1");
      FEXCore::Config::Set(FEXCore::Config::CONFIG_SMALLTSCSCALE, "1");
      const bool traceEnabled = FexTraceEnabled();
      FEXCore::Config::Set(FEXCore::Config::CONFIG_X86DISASSEMBLE,
                           traceEnabled ? "1" : "0");
      if (traceEnabled) FexBlockTraceCount.store(0, std::memory_order_relaxed);
      LogMan::Msg::InstallHandler(FexMessageHandler);
    }
    ++Users;
    return true;
  }

  static EngineResult<bool> Release() {
    std::scoped_lock lock {Mutex};
    if (Users == 0) return Failure(EngineStage::Teardown, EINVAL);
    if (--Users == 0) {
      FEXCore::Config::Shutdown();
      LogMan::Msg::UnInstallHandler();
    }
    return true;
  }

private:
  static inline std::mutex Mutex;
  static inline size_t Users {};
};

class Mapping final {
public:
  Mapping(size_t size, int protection)
    : Size {size}
    , Address {mmap(nullptr, size, protection, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)}
    , LastError {Address == MAP_FAILED ? errno : 0} {}

  Mapping(const Mapping&) = delete;
  Mapping& operator=(const Mapping&) = delete;

  ~Mapping() {
    if (Address != MAP_FAILED && munmap(Address, Size) != 0) {
      std::abort();
    }
  }

  [[nodiscard]] bool IsValid() const {
    return Address != MAP_FAILED;
  }

  [[nodiscard]] int Error() const {
    return LastError;
  }

  [[nodiscard]] void* Get() const {
    return Address;
  }

  [[nodiscard]] EngineResult<bool> Protect(int protection) {
    if (mprotect(Address, Size, protection) != 0) {
      return Failure(EngineStage::Mapping, errno);
    }
    return true;
  }

  [[nodiscard]] EngineResult<bool> Release() {
    if (Address == MAP_FAILED) {
      return true;
    }
    if (munmap(Address, Size) != 0) {
      return Failure(EngineStage::Teardown, errno);
    }
    Address = MAP_FAILED;
    return true;
  }

private:
  size_t Size;
  void* Address;
  int LastError;
};

class CallRetStack final {
public:
  explicit CallRetStack(size_t pageSize)
    : PageSize {pageSize}
    , StackSize {(FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE + pageSize - 1) & ~(pageSize - 1)}
    , AllocationSize {StackSize + 2 * pageSize}
    , Address {mmap(nullptr, AllocationSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)}
    , LastError {Address == MAP_FAILED ? errno : 0} {}

  CallRetStack(const CallRetStack&) = delete;
  CallRetStack& operator=(const CallRetStack&) = delete;

  ~CallRetStack() {
    if (Address != MAP_FAILED && munmap(Address, AllocationSize) != 0) {
      std::abort();
    }
  }

  [[nodiscard]] bool IsReserved() const {
    return Address != MAP_FAILED;
  }

  [[nodiscard]] int Error() const {
    return LastError;
  }

  [[nodiscard]] EngineResult<bool> MakeWritable() const {
    if (mprotect(StackBase(), StackSize, PROT_READ | PROT_WRITE) != 0) {
      return Failure(EngineStage::Mapping, errno);
    }
    return true;
  }

  void Initialize(FEXCore::Core::InternalThreadState* thread) const {
    thread->CallRetStackBase = StackBase();
    thread->CurrentFrame->State.callret_sp =
      reinterpret_cast<uint64_t>(StackBase()) + FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE / 4;
  }

private:
  [[nodiscard]] void* StackBase() const {
    return static_cast<uint8_t*>(Address) + PageSize;
  }

  size_t PageSize;
  size_t StackSize;
  size_t AllocationSize;
  void* Address;
  int LastError;
};

class GuestSegmentState final {
public:
  void Initialize(FEXCore::Core::CPUState& state) {
    state.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = GDT.data();
    state.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = GDT.data();
    state.cs_idx = FEXCore::Core::CPUState::DEFAULT_USER_CS << 3;
    auto* codeSegment = FEXCore::Core::CPUState::GetSegmentFromIndex(state, state.cs_idx);
    FEXCore::Core::CPUState::SetGDTBase(codeSegment, 0);
    FEXCore::Core::CPUState::SetGDTLimit(codeSegment, 0xF'FFFFU);
    state.cs_cached = FEXCore::Core::CPUState::CalculateGDTBase(*codeSegment);
    codeSegment->L = 1;
    codeSegment->D = 0;
  }

private:
  std::array<FEXCore::Core::CPUState::gdt_segment, 32> GDT {};
};

class ThreadScope final {
public:
  ThreadScope(FEXCore::Context::Context& context, FEXCore::Core::InternalThreadState* thread)
    : Context {context}
    , Thread {thread} {}

  ThreadScope(const ThreadScope&) = delete;
  ThreadScope& operator=(const ThreadScope&) = delete;

  ~ThreadScope() {
    if (Thread != nullptr) {
      Context.DestroyThread(Thread);
    }
  }

private:
  FEXCore::Context::Context& Context;
  FEXCore::Core::InternalThreadState* Thread;
};

class BridgeSignalDelegator final : public FEXCore::SignalDelegator {
public:
  explicit BridgeSignalDelegator(uintptr_t callbackReturn)
    : CallbackReturn {callbackReturn} {}

  uintptr_t GetThunkCallbackRET() const override {
    return CallbackReturn;
  }

  void SetThunkCallbackRET(uintptr_t callbackReturn) {
    CallbackReturn = callbackReturn;
  }

private:
  uintptr_t CallbackReturn;
};

class CallbackReturnScope final {
public:
  CallbackReturnScope(BridgeSignalDelegator& delegator, uintptr_t callbackReturn)
    : Delegator {delegator}
    , Previous {delegator.GetThunkCallbackRET()} {
    Delegator.SetThunkCallbackRET(callbackReturn);
  }

  CallbackReturnScope(const CallbackReturnScope&) = delete;
  CallbackReturnScope& operator=(const CallbackReturnScope&) = delete;

  ~CallbackReturnScope() {
    Delegator.SetThunkCallbackRET(Previous);
  }

private:
  BridgeSignalDelegator& Delegator;
  uintptr_t Previous;
};

class BridgeSyscallHandler final : public FEXCore::HLE::SyscallHandler {
public:
  struct InvocationState final {
    std::optional<EngineFailure> Failure;
    uint64_t Result {};
    bool WasInvoked {};
    BridgeSyscallHandler* Owner {};
    FEXCore::Core::InternalThreadState* Thread {};

    void Reset() {
      Failure.reset();
      Result = 0;
      WasInvoked = false;
      Owner = nullptr;
      Thread = nullptr;
    }
  };

  class InvocationScope final {
  public:
    InvocationScope(BridgeSyscallHandler& owner, InvocationState& state,
                    FEXCore::Core::InternalThreadState* thread)
      : Previous {ActiveInvocation} {
      state.Reset();
      state.Owner = &owner;
      state.Thread = thread;
      ActiveInvocation = &state;
    }

    InvocationScope(const InvocationScope&) = delete;
    InvocationScope& operator=(const InvocationScope&) = delete;

    ~InvocationScope() {
      ActiveInvocation = Previous;
    }

  private:
    InvocationState* Previous;
  };

  explicit BridgeSyscallHandler(GuestBridge& bridge)
    : Bridge {bridge} {
    OSABI = FEXCore::HLE::SyscallOSABI::OS_GENERIC;
  }

  // Run deferred Orbis exception handler (Unity SIGUSR1 / signal 30) via nested
  // HandleCallback. Mirrors desktop Windows APC ExceptionHandler: guest handler
  // must actually run so GC STW can complete. Never rewrite host RIP/RSP.
  void FlushPendingOrbisSignal() {
    if (!PendingOrbisSignal.Flushing && ActiveFexExecution.Context != nullptr &&
        ActiveFexExecution.Thread != nullptr) {
      Libraries::Kernel::FexProcessPendingSignals();
    }
    if (!PendingOrbisSignal.Pending || PendingOrbisSignal.Flushing ||
        ActiveFexExecution.Context == nullptr || ActiveFexExecution.Thread == nullptr ||
        PendingOrbisSignal.Handler == 0) {
      return;
    }
    const auto handler = PendingOrbisSignal.Handler;
    const auto sig = PendingOrbisSignal.OrbisSig;
    const bool trace_enabled = FexTraceEnabled();
    PendingOrbisSignal.Pending = false;
    PendingOrbisSignal.Handler = 0;
    PendingOrbisSignal.Flushing = true;

    auto* thread = ActiveFexExecution.Thread;
    auto* ctx = ActiveFexExecution.Context;
    auto& state = thread->CurrentFrame->State;
    using namespace FEXCore::X86State;

    // Nested invocation so handler HLE does not clobber the outer syscall frame.
    InvocationState nested_invocation;
    InvocationScope nested_scope {*this, nested_invocation, thread};

    // Guest stack: prefer CurrentFrame RSP when it looks like PS4 VA; otherwise
    // fall back to this pthread's guest stack (first GC kill often arrives before
    // FEX has spilled guest RSP into CurrentFrame — frame holds host values).
    const auto looks_guest = [](uint64_t a) {
      // Tight PS4 user window. Host Android stacks sit at ~0x7e.. / 0x7c.. and
      // must not be used as guest RSP (HandleCallback + x86 stack ops crash).
      return a >= 0x100000000ULL && a < 0x900000000ULL;
    };
    uint64_t work_rsp = PendingOrbisSignal.WorkRsp != 0 ? PendingOrbisSignal.WorkRsp : state.gregs[REG_RSP];
    if (!looks_guest(work_rsp)) {
      // Prefer Orbis pthread guest stack (strong FexCurrentGuestStackTop in
      // exception.cpp). Harness keeps the weak stub → 0.
      const auto top = Libraries::Kernel::FexCurrentGuestStackTop();
      if (top != 0 && looks_guest(top - 0x100)) {
        work_rsp = top - 0x100;
        if (trace_enabled) std::fprintf(stderr,
                     "LUMEN_HOST_FEX_SIGNAL use pthread stack top=%#lx "
                     "(frame_rsp was %#lx)\n",
                     static_cast<unsigned long>(work_rsp),
                     static_cast<unsigned long>(state.gregs[REG_RSP]));
      }
    }

    // Save interrupted frame (may hold host values mid-HLE).
    std::array<uint64_t, 16> saved_gprs {};
    std::copy(std::begin(state.gregs), std::begin(state.gregs) + 16, saved_gprs.begin());
    const uint64_t saved_rip = state.rip;
    const uint64_t saved_rsp = state.gregs[REG_RSP];
    if (!looks_guest(work_rsp)) {
      std::fprintf(stderr,
                   "LUMEN_HOST_FEX_SIGNAL flush abort: no guest stack frame_rsp=%#lx\n",
                   static_cast<unsigned long>(saved_rsp));
      PendingOrbisSignal.Pending = true;
      PendingOrbisSignal.Handler = handler;
      PendingOrbisSignal.OrbisSig = sig;
      PendingOrbisSignal.Flushing = false;
      return;
    }

    // Zero guest GPRs for soft handler: frame may contain host SRA garbage that
    // looks like pointers (null+0xf8 SEGV). Keep FS/GS as-is for TLS.
    for (size_t i = 0; i < 16; ++i) {
      state.gregs[i] = 0;
    }
    state.gregs[REG_RSP] = work_rsp;

    // PS4Util soft-handler pattern (Galak-Z):
    //   lea rax, [counter]; inc [rax]
    //   cmp edi, 30; jne fallback
    //   mov rdx, [rsi+0xf8]   // uctx->uc_mcontext.mc_rsp  (offset 0xf8)
    //   mov rdi, rsi          // uctx
    //   mov rcx, [handler_slot]; mov rsi, rdx; jmp rcx
    // So second arg to real Unity handler is mc_rsp — MUST be valid guest SP.
    uint64_t ctx_addr = 0;
    constexpr uint64_t kUcontextBytes =
        (sizeof(Libraries::Kernel::Ucontext) + 0x3full) & ~uint64_t{0x3f};
    if (work_rsp > kUcontextBytes + 0x100) {
      ctx_addr = (work_rsp - kUcontextBytes) & ~uint64_t{0xf};
      auto* uctx = reinterpret_cast<Libraries::Kernel::Ucontext*>(
          static_cast<uintptr_t>(ctx_addr));
      std::memset(uctx, 0, sizeof(*uctx));
      auto& mc = uctx->uc_mcontext;
      // Stack for the real handler (read at uctx+0xf8).
      mc.mc_rsp = PendingOrbisSignal.SiginfoArguments && looks_guest(saved_rsp) ? saved_rsp : work_rsp;
      mc.mc_rbp = work_rsp;
      if (looks_guest(saved_rip)) {
        mc.mc_rip = saved_rip;
      } else {
        // Dummy guest RIP in eboot range so stack-walkers see a code address.
        mc.mc_rip = 0x8000000a0ULL;
      }
      mc.mc_fsbase = state.fs_cached;
      mc.mc_gsbase = state.gs_cached;
      mc.mc_fs = static_cast<u16>(state.fs_cached & 0xffff);
      mc.mc_gs = static_cast<u16>(state.gs_cached & 0xffff);
      // Handler stack below the uctx blob.
      state.gregs[REG_RSP] = ctx_addr;
    }
    state.gregs[REG_RDI] = static_cast<uint64_t>(static_cast<uint32_t>(sig));
    state.gregs[REG_RSI] = ctx_addr;

    if (PendingOrbisSignal.SiginfoArguments) {
      const auto info_addr = ctx_addr - sizeof(Libraries::Kernel::Siginfo);
      auto* info = reinterpret_cast<Libraries::Kernel::Siginfo*>(info_addr);
      std::memset(info, 0, sizeof(*info));
      info->_si_signo = sig;
      info->_si_code = POSIX_SI_LWP;
      state.gregs[REG_RSI] = info_addr;
      state.gregs[REG_RDX] = ctx_addr;
      state.gregs[REG_RSP] = info_addr;
    }

    if (trace_enabled) {

    std::fprintf(stderr,
                 "LUMEN_HOST_FEX_SIGNAL flush orbis_sig=%d handler=%#lx ctx=%#lx "
                 "work_rsp=%#lx fs=%#lx\n",
                 sig, static_cast<unsigned long>(handler),
                 static_cast<unsigned long>(ctx_addr),
                 static_cast<unsigned long>(work_rsp),
                 static_cast<unsigned long>(state.fs_cached));
    // Dump first 16 bytes of guest handler + probe QueryExecutableRange.
    {
      const auto range = QueryGuestExecutableRange(thread, handler);
      std::fprintf(stderr,
                   "LUMEN_HOST_FEX_SIGNAL handler_range begin=%#lx size=%#lx writable=%d\n",
                   static_cast<unsigned long>(range.Base),
                   static_cast<unsigned long>(range.Size),
                   range.Writable ? 1 : 0);
      const auto* bytes = reinterpret_cast<const unsigned char*>(
          static_cast<uintptr_t>(handler));
      std::fprintf(stderr, "LUMEN_HOST_FEX_SIGNAL handler_bytes");
      for (int i = 0; i < 64; ++i) {
        std::fprintf(stderr, " %02x", bytes[i]);
      }
      std::fprintf(stderr, "\n");
      // Global the first LEA likely targets (rip+7+disp32 at +3).
      if (bytes[0] == 0x48 && bytes[1] == 0x8d && bytes[2] == 0x05) {
        const auto disp = static_cast<int32_t>(bytes[3] | (bytes[4] << 8) |
                                               (bytes[5] << 16) | (bytes[6] << 24));
        const auto target = handler + 7 + static_cast<std::uintptr_t>(disp);
        const auto* g = reinterpret_cast<const unsigned char*>(
            static_cast<uintptr_t>(target));
        std::fprintf(stderr,
                     "LUMEN_HOST_FEX_SIGNAL lea_target=%#lx qwords=%#lx %#lx %#lx %#lx\n",
                     static_cast<unsigned long>(target),
                     static_cast<unsigned long>(
                         *reinterpret_cast<const uint64_t*>(g)),
                     static_cast<unsigned long>(
                         *reinterpret_cast<const uint64_t*>(g + 8)),
                     static_cast<unsigned long>(
                         *reinterpret_cast<const uint64_t*>(g + 16)),
                     static_cast<unsigned long>(
                         *reinterpret_cast<const uint64_t*>(g + 24)));
        // Real Unity handler slot is typically at lea_target+0x10 (third qword).
        const auto real_fn = *reinterpret_cast<const uint64_t*>(g + 16);
        if (real_fn != 0) {
          const auto* rb = reinterpret_cast<const unsigned char*>(
              static_cast<uintptr_t>(real_fn));
          std::fprintf(stderr, "LUMEN_HOST_FEX_SIGNAL real_fn=%#lx bytes",
                       static_cast<unsigned long>(real_fn));
          for (int i = 0; i < 32; ++i) {
            std::fprintf(stderr, " %02x", rb[i]);
          }
          std::fprintf(stderr, "\n");
          // Verify uctx+0xf8 == mc_rsp we wrote.
          if (ctx_addr != 0) {
            const auto at_f8 = *reinterpret_cast<const uint64_t*>(
                static_cast<uintptr_t>(ctx_addr + 0xf8));
            std::fprintf(stderr,
                         "LUMEN_HOST_FEX_SIGNAL uctx+0xf8(mc_rsp)=%#lx work_rsp=%#lx\n",
                         static_cast<unsigned long>(at_f8),
                         static_cast<unsigned long>(work_rsp));
          }
        }
      }
    }
    std::fprintf(stderr,
                 "LUMEN_HOST_FEX_SIGNAL Exception raised successfully orbis_sig=%d handler=%#lx\n",
                 sig, static_cast<unsigned long>(handler));
    std::fprintf(stderr, "LUMEN_HOST_FEX_SIGNAL callback_ret=%#lx\n",
                 static_cast<unsigned long>(thread->CurrentFrame->Pointers.ThunkCallbackRet));

    }
    ctx->HandleCallback(thread, handler);

    if (trace_enabled) std::fprintf(stderr, "LUMEN_HOST_FEX_SIGNAL handler returned rip=%#lx rsp=%#lx\n",
                 static_cast<unsigned long>(state.rip),
                 static_cast<unsigned long>(state.gregs[REG_RSP]));

    // Restore interrupted frame for resume (HLE or JIT trampoline).
    std::copy(saved_gprs.begin(), saved_gprs.end(), std::begin(state.gregs));
    state.rip = saved_rip;
    state.gregs[REG_RSP] = saved_rsp;
    PendingOrbisSignal.Flushing = false;
  }

  uint64_t HandleSyscall(FEXCore::Core::CpuStateFrame* frame, FEXCore::HLE::SyscallArguments*) override {
    // Desktop APC ExceptionHandler runs guest handler on the target thread around
    // normal execution. On FEX, host pthread_kill only wakes + queues Pending;
    // deliver here at HLE boundary (entry) so GC STW sees the handler.
    FlushPendingOrbisSignal();
    auto* invocation = ActiveInvocation;
    if (invocation == nullptr) return static_cast<uint64_t>(-EPERM);
    if (frame == nullptr) {
      invocation->Failure = Failure(EngineStage::Bridge, EFAULT);
      return static_cast<uint64_t>(-EFAULT);
    }

    GuestCpu::HleCallFrame hleFrame{};
    hleFrame.operation = frame->State.gregs[FEXCore::X86State::REG_RAX];
    std::copy(std::begin(frame->State.gregs), std::end(frame->State.gregs), hleFrame.gpr.begin());
    hleFrame.gpr[FEXCore::X86State::REG_RCX] = frame->State.gregs[FEXCore::X86State::REG_R10];
    hleFrame.rsp = frame->State.gregs[FEXCore::X86State::REG_RSP];
    hleFrame.callret_sp = frame->State.callret_sp;
    hleFrame.callret_stack_base = reinterpret_cast<std::uintptr_t>(frame->Thread->CallRetStackBase);
    hleFrame.callret_stack_size = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
    hleFrame.fcw = frame->State.FCW;
    hleFrame.mxcsr = frame->State.mxcsr;
    hleFrame.reset_simd = [](void* context) {
      auto& state = static_cast<FEXCore::Core::CpuStateFrame*>(context)->State;
      std::memset(state.avx_high, 0, sizeof(state.avx_high));
      std::memset(&state.xmm, 0, sizeof(state.xmm));
      std::memset(state.mm, 0, sizeof(state.mm));
    };
    hleFrame.reset_simd_context = frame;
    for (size_t index = 0; index < hleFrame.xmm.size(); ++index) {
      hleFrame.xmm[index][0] = frame->State.xmm.sse.data[index][0];
      hleFrame.xmm[index][1] = frame->State.xmm.sse.data[index][1];
    }
    auto result = Bridge.Invoke(hleFrame);
    if (const auto* error = std::get_if<EngineFailure>(&result)) {
      invocation->Failure = *error;
      frame->State.gregs[FEXCore::X86State::REG_RAX] = static_cast<uint64_t>(-error->Error);
      FlushPendingOrbisSignal();
      return frame->State.gregs[FEXCore::X86State::REG_RAX];
    }

    std::copy(hleFrame.gpr.begin(), hleFrame.gpr.end(), std::begin(frame->State.gregs));
    frame->State.callret_sp = hleFrame.callret_sp;
    frame->State.mxcsr = hleFrame.mxcsr & 0xffc0;
    frame->State.FCW = hleFrame.fcw;
    for (size_t index = 0; index < hleFrame.xmm.size(); ++index) {
      frame->State.xmm.sse.data[index][0] = hleFrame.xmm[index][0];
      frame->State.xmm.sse.data[index][1] = hleFrame.xmm[index][1];
    }
    invocation->Result = frame->State.gregs[FEXCore::X86State::REG_RAX];
    invocation->WasInvoked = true;
    // Kill often lands while target is blocked inside host futex/HLE. Flush after
    // Invoke so handler runs before returning to pure guest JIT.
    FlushPendingOrbisSignal();
    return invocation->Result;
  }

  EngineResult<bool> RegisterThread(FEXCore::Core::InternalThreadState* thread,
                                    const std::vector<GuestExecutionRange>& ranges) {
    if (thread == nullptr) return Failure(EngineStage::Thread, EINVAL);
    std::scoped_lock lock {RangesMutex};
    const auto [_, inserted] = ExecutableRanges.emplace(thread, ranges);
    return inserted ? EngineResult<bool> {true} : EngineResult<bool> {Failure(EngineStage::Thread, EEXIST)};
  }

  void UnregisterThread(FEXCore::Core::InternalThreadState* thread) {
    std::scoped_lock lock {RangesMutex};
    ExecutableRanges.erase(thread);
  }

  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState* thread,
                                                               uint64_t address) override {
    {
      std::scoped_lock lock {RangesMutex};
      const auto ranges = ExecutableRanges.find(thread);
      if (ranges != ExecutableRanges.end()) {
        for (const auto& range : ranges->second) {
          if (range.Executable && Contains(range, static_cast<std::uintptr_t>(address), 1)) {
            return {range.Begin, range.Size, range.Writable};
          }
        }
      }
    }
    const auto dynamicRange = Bridge.QueryExecutableRange(static_cast<std::uintptr_t>(address));
    if (dynamicRange && dynamicRange->Executable && Contains(*dynamicRange, address, 1)) {
      return {dynamicRange->Begin, dynamicRange->Size, dynamicRange->Writable};
    }
    return {};
  }

  FEXCore::Core::InternalThreadState* ActiveThread() const {
    if (ActiveInvocation == nullptr || ActiveInvocation->Owner != this) return nullptr;
    return ActiveInvocation->Thread;
  }

  const std::optional<EngineFailure>& ActiveFailure() const {
    static const std::optional<EngineFailure> noFailure;
    if (ActiveInvocation == nullptr || ActiveInvocation->Owner != this) return noFailure;
    return ActiveInvocation->Failure;
  }

  bool IsWritableRange(FEXCore::Core::InternalThreadState* thread, std::uintptr_t address,
                       std::size_t size) {
    std::scoped_lock lock {RangesMutex};
    const auto ranges = ExecutableRanges.find(thread);
    if (ranges == ExecutableRanges.end()) return false;
    return std::ranges::any_of(ranges->second, [&](const GuestExecutionRange& range) {
      return range.Writable && Contains(range, address, size);
    });
  }

  std::optional<FEXCore::ExecutableFileSectionInfo>
  LookupExecutableFileSection(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return std::nullopt;
  }

  [[nodiscard]] const std::optional<EngineFailure>& FailureResult(const InvocationState& state) const {
    return state.Failure;
  }

  [[nodiscard]] uint64_t Result(const InvocationState& state) const {
    return state.Result;
  }

  [[nodiscard]] bool Invoked(const InvocationState& state) const {
    return state.WasInvoked;
  }

private:
  static thread_local InvocationState* ActiveInvocation;

  GuestBridge& Bridge;
  std::mutex RangesMutex;
  std::unordered_map<FEXCore::Core::InternalThreadState*, std::vector<GuestExecutionRange>> ExecutableRanges;
};

thread_local BridgeSyscallHandler::InvocationState* BridgeSyscallHandler::ActiveInvocation {};

class ExecutableRangeScope final {
public:
  ExecutableRangeScope(BridgeSyscallHandler& handler, FEXCore::Core::InternalThreadState* thread)
    : Handler {handler}
    , Thread {thread} {}

  ExecutableRangeScope(const ExecutableRangeScope&) = delete;
  ExecutableRangeScope& operator=(const ExecutableRangeScope&) = delete;

  ~ExecutableRangeScope() {
    Handler.UnregisterThread(Thread);
  }

private:
  BridgeSyscallHandler& Handler;
  FEXCore::Core::InternalThreadState* Thread;
};

void AppendImmediate(std::vector<uint8_t>& code, uint64_t value) {
  const auto offset = code.size();
  code.resize(offset + sizeof(value));
  std::memcpy(code.data() + offset, &value, sizeof(value));
}

uint64_t DoubleBits(double value) {
  uint64_t bits {};
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

struct GuestCode final {
  std::vector<uint8_t> Bytes;
  size_t CallbackOffset {};
  size_t CallbackReturnOffset {};
  size_t InvalidationOffset {};
  size_t InvalidationImmediateOffset {};
  size_t ThreadOffset {};
};

GuestCode BuildGuestCode() {
  constexpr uint64_t bridgeOperation = 0xB4C4'F001ULL;
  constexpr uint64_t bridgeArgument = 0x1020'3040'5060'7080ULL;

  GuestCode result;
  auto& code = result.Bytes;
  code.insert(code.end(), {0x48, 0xb8}); // mov rax, kAddLeft
  AppendImmediate(code, kAddLeft);
  code.insert(code.end(), {0x48, 0xbb}); // mov rbx, kAddRight
  AppendImmediate(code, kAddRight);
  code.insert(code.end(), {0x48, 0x01, 0xd8}); // add rax, rbx
  code.insert(code.end(), {0x49, 0x89, 0xc0}); // mov r8, rax
  code.insert(code.end(), {0x48, 0xb9}); // mov rcx, kXorLeft
  AppendImmediate(code, kXorLeft);
  code.insert(code.end(), {0x48, 0xba}); // mov rdx, kXorRight
  AppendImmediate(code, kXorRight);
  code.insert(code.end(), {0x48, 0x31, 0xd1}); // xor rcx, rdx
  code.insert(code.end(), {0x49, 0x89, 0xc9}); // mov r9, rcx
  code.insert(code.end(), {0xf2, 0x0f, 0x58, 0xc1}); // addsd xmm0, xmm1

  code.insert(code.end(), {0x48, 0xb8}); // mov rax, bridge operation
  AppendImmediate(code, bridgeOperation);
  code.insert(code.end(), {0x48, 0xbf}); // mov rdi, bridge argument
  AppendImmediate(code, bridgeArgument);
  code.insert(code.end(), {0x0f, 0x05}); // syscall
  code.insert(code.end(), {0x4d, 0x8b, 0x2c, 0x24}); // mov r13, [r12]

  code.push_back(0xe8); // call stack_test
  const size_t callDisplacementOffset = code.size();
  code.resize(code.size() + sizeof(int32_t));
  code.push_back(0xf4); // hlt

  const size_t stackTestOffset = code.size();
  code.insert(code.end(), {0x48, 0xba}); // mov rdx, kStackSentinel
  AppendImmediate(code, kStackSentinel);
  code.push_back(0x52); // push rdx
  code.insert(code.end(), {0x48, 0x8b, 0x34, 0x24}); // mov rsi, [rsp]
  code.push_back(0x5f); // pop rdi
  code.push_back(0xc3); // ret
  const auto displacement = static_cast<int32_t>(stackTestOffset - (callDisplacementOffset + sizeof(int32_t)));
  std::memcpy(code.data() + callDisplacementOffset, &displacement, sizeof(displacement));

  result.CallbackOffset = code.size();
  code.insert(code.end(), {0x48, 0x8d, 0x47, 0x05, 0xc3}); // lea rax, [rdi + 5]; ret
  result.CallbackReturnOffset = code.size();
  code.insert(code.end(), {0x0f, 0x3e}); // FEX CALLBACKRET instruction

  result.InvalidationOffset = code.size();
  code.insert(code.end(), {0x48, 0xb8}); // mov rax, kInvalidationInitial
  result.InvalidationImmediateOffset = code.size();
  AppendImmediate(code, kInvalidationInitial);
  code.push_back(0xf4); // hlt

  result.ThreadOffset = code.size();
  code.insert(code.end(), {0x64, 0x48, 0x8b, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00}); // mov rax, fs:[0]
  code.push_back(0xf4); // hlt
  return result;
}

} // namespace

bool LumenHostQueryGuestRipSyscall(uint64_t* out_rip, uint64_t* out_syscall) noexcept {
  // Async-signal-safe: only dereferences thread-local pointer + frame struct.
  // ActiveFexExecution is thread_local, set by FexExecutionSignalScope during
  // guest execution. If no FEX thread is active (e.g. crash in host-only init),
  // Context is null and we report unavailable.
  const auto& exec = ActiveFexExecution;
  if (exec.Context == nullptr || exec.Thread == nullptr || exec.Thread->CurrentFrame == nullptr) {
    return false;
  }
  // CurrentFrame->State holds the spilled guest GPRs at the last HLE/JIT boundary.
  // During mid-JIT execution these may be stale (live in SRA host regs), but for a
  // SIGSYS delivered at a host syscall boundary this is the best available view.
  const auto& state = exec.Thread->CurrentFrame->State;
  if (out_rip != nullptr) {
    *out_rip = state.rip;
  }
  if (out_syscall != nullptr) {
    *out_syscall = state.gregs[FEXCore::X86State::REG_RAX];
  }
  return true;
}

void FlushPendingGuestOrbisSignal() {
  // Called from blocking HLE waits (semaphore/cond) to deliver a queued Orbis
  // guest signal at a safe point on the parked guest thread. Runs the guest
  // handler via nested HandleCallback (never from async-signal context). No-op
  // when nothing is pending or no FEX guest thread is active on this host thread.
  if (ActiveFexExecution.Syscalls != nullptr) {
    ActiveFexExecution.Syscalls->FlushPendingOrbisSignal();
  }
}

bool DeliverGuestOrbisSignal(int orbis_sig, siginfo_t* info, void* rawContext,
                             std::uintptr_t guest_handler, bool siginfo_arguments,
                             std::uintptr_t work_rsp) noexcept {
  static_cast<void>(info);
  if (guest_handler == 0) {
    return false;
  }

  // Queue for this thread (SigactionHandler runs on the kill target). It is not
  // safe to run the guest handler from async-signal context (nested
  // HandleCallback re-enters the JIT), so only set Pending here. Delivery happens
  // at the next safe HLE boundary: HandleSyscall for running JIT threads, or the
  // blocking semaphore/cond waits (FlushPendingGuestOrbisSignal) for threads
  // parked in a host futex, e.g. the GC Finalizer during stop-the-world.
  PendingOrbisSignal.Handler = guest_handler;
  PendingOrbisSignal.OrbisSig = orbis_sig;
  PendingOrbisSignal.Pending = true;
  PendingOrbisSignal.SiginfoArguments = siginfo_arguments;
  PendingOrbisSignal.WorkRsp = work_rsp;
  PendingOrbisSignal.HasHostSnapshot = false;
  if (!FexTraceEnabled()) return true;
  const bool active = ActiveFexExecution.Thread != nullptr &&
                      ActiveFexExecution.Syscalls != nullptr;

  std::uint64_t host_pc = 0;
  std::uint64_t host_sp = 0;
  bool in_jit = false;

#if defined(__aarch64__)
  if (rawContext != nullptr) {
    const auto* context = reinterpret_cast<const ucontext_t*>(rawContext);
    host_pc = HostProgramCounter(context);
    host_sp = HostStackPointer(context);
    if (ActiveFexExecution.Context != nullptr && ActiveFexExecution.Thread != nullptr) {
      in_jit = ActiveFexExecution.Context->IsAddressInCodeBuffer(
          ActiveFexExecution.Thread, static_cast<std::uintptr_t>(host_pc));
    }
  }
#else
  static_cast<void>(rawContext);
#endif

  std::fprintf(stderr,
               "LUMEN_HOST_FEX_SIGNAL defer orbis_sig=%d handler=%#lx active=%d host_pc=%#lx host_sp=%#lx in_jit=%d\n",
               orbis_sig, static_cast<unsigned long>(guest_handler), active ? 1 : 0,
               static_cast<unsigned long>(host_pc), static_cast<unsigned long>(host_sp),
               in_jit ? 1 : 0);

  // Delivery is deferred to a safe HLE boundary. It is never safe to run the
  // guest handler (nested HandleCallback re-enters the JIT) from async-signal
  // context, and diverting the host PC to resume a blocked libc futex proved
  // unrecoverable (nested signal reentrancy → SIGILL). Instead the queued
  // handler runs from HandleSyscall (running JIT threads) or from the blocking
  // semaphore/cond waits via FlushPendingGuestOrbisSignal (threads parked in a
  // host futex, e.g. the GC Finalizer during stop-the-world).
  return true;
}


bool HandleGuestSignal(int signal, siginfo_t* info, void* rawContext) noexcept {
#if defined(__aarch64__)
  if (info == nullptr || rawContext == nullptr ||
      ActiveFexExecution.Context == nullptr || ActiveFexExecution.Thread == nullptr) {
    return false;
  }

  auto* context = reinterpret_cast<ucontext_t*>(rawContext);
  const auto pc = HostProgramCounter(context);
  if (!ActiveFexExecution.Context->IsAddressInCodeBuffer(ActiveFexExecution.Thread, pc)) {
    return false;
  }

  std::array<std::uint64_t, 31> registers {};
  for (size_t index = 0; index < registers.size(); ++index) registers[index] = HostRegister(context, index);
  if (signal == SIGSEGV || signal == SIGBUS) {
    const auto base = reinterpret_cast<std::uintptr_t>(ActiveFexExecution.Thread->CallRetStackBase);
    constexpr auto size = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
#if defined(__APPLE__)
    constexpr std::uintptr_t page_size = 16384;
#else
    constexpr std::uintptr_t page_size = 4096;
#endif
    if (base > std::numeric_limits<std::uintptr_t>::max() - size || base < page_size ||
        (base & ~(page_size - 1)) == std::numeric_limits<std::uintptr_t>::max() - size - page_size + 1) {
      return false;
    }
    const auto fault = reinterpret_cast<std::uintptr_t>(info->si_addr);
    if (fault >= base - page_size && fault < base + size + page_size) {
      registers[25] = base + size / sizeof(std::uint32_t);
      CommitHostRegisters(context, pc, registers);
      return true;
    }
  }
  if (signal != SIGBUS || ActiveFexExecution.SignalDelegator == nullptr) return false;
  if (!IsHostAlignmentFault(info, context)) return false;
#if defined(__APPLE__)
  FEXCore::Allocator::ScopedJITWriteProtection writableJit;
#endif
  const auto adjustment = FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(
      ActiveFexExecution.Thread,
      FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier, pc, registers.data());
  if (!adjustment.has_value()) {
    return false;
  }
  CommitHostRegisters(context, pc + *adjustment, registers);
  return true;
#else
  static_cast<void>(signal);
  static_cast<void>(info);
  static_cast<void>(rawContext);
  return false;
#endif
}

class GuestEngine::Thread final {
public:
  Thread(std::thread::id owner, GuestExecutionRequest request, size_t pageSize)
    : Owner {owner}
    , Request {std::move(request)}
    , CallRet {pageSize} {}

  std::thread::id Owner;
  GuestExecutionRequest Request;
  FEXCore::Core::InternalThreadState* Native {};
  CallRetStack CallRet;
  GuestSegmentState Segments;
  BridgeSyscallHandler::InvocationState Invocation;
  uint64_t FirstRip {};
  uint64_t LastRip {};
};

class GuestEngine::Impl final {
public:
  explicit Impl(GuestBridge& bridge)
    : Bridge {bridge} {}

  [[nodiscard]] EngineResult<GuestRunResult> Run() {
    if (Context == nullptr || SignalDelegator == nullptr || Syscalls == nullptr) {
      return Failure(EngineStage::Context, EINVAL);
    }
    if (Ran) {
      return Failure(EngineStage::Execute, EALREADY);
    }

    GuestCode guest = BuildGuestCode();
    if (guest.Bytes.size() > PageSize) return Failure(EngineStage::Mapping, E2BIG);
    Mapping code {PageSize, PROT_READ | PROT_WRITE};
    if (!code.IsValid()) return Failure(EngineStage::Mapping, code.Error());
    std::memcpy(code.Get(), guest.Bytes.data(), guest.Bytes.size());
    __builtin___clear_cache(reinterpret_cast<char*>(code.Get()), reinterpret_cast<char*>(code.Get()) + PageSize);
    const auto codeProtection = code.Protect(PROT_READ);
    if (const auto* error = std::get_if<EngineFailure>(&codeProtection)) return *error;

    Mapping stackPage {PageSize, PROT_READ | PROT_WRITE};
    Mapping tlsPage {PageSize, PROT_READ | PROT_WRITE};
    if (!stackPage.IsValid()) return Failure(EngineStage::Mapping, stackPage.Error());
    if (!tlsPage.IsValid()) return Failure(EngineStage::Mapping, tlsPage.Error());
    std::memcpy(tlsPage.Get(), &kThreadSentinelA, sizeof(kThreadSentinelA));

    const uint64_t initialRip = reinterpret_cast<uint64_t>(code.Get());
    CallbackReturnScope callbackReturn {*SignalDelegator, initialRip + guest.CallbackReturnOffset};
    const uint64_t initialRsp = reinterpret_cast<uint64_t>(stackPage.Get()) + PageSize - 16;
    CallRetStack callRetStack{PageSize};
    if (!callRetStack.IsReserved()) return Failure(EngineStage::Mapping, callRetStack.Error());
    const auto callRetStackProtection = callRetStack.MakeWritable();
    if (const auto* error = std::get_if<EngineFailure>(&callRetStackProtection)) return *error;

    auto* thread = CreateNativeThread(*Context, initialRip, initialRsp);
    if (thread == nullptr) return Failure(EngineStage::Thread, ENOMEM);
    ThreadScope threadScope {*Context, thread};
    const std::vector<GuestExecutionRange> ranges {
        {static_cast<std::uintptr_t>(initialRip), PageSize, true, false},
    };
    const auto registered = Syscalls->RegisterThread(thread, ranges);
    if (const auto* failure = std::get_if<EngineFailure>(&registered)) return *failure;
    ExecutableRangeScope rangeScope {*Syscalls, thread};
    BridgeSyscallHandler::InvocationState invocation;
    BridgeSyscallHandler::InvocationScope invocationScope {*Syscalls, invocation, thread};
    GuestSegmentState segmentState;
    segmentState.Initialize(thread->CurrentFrame->State);
    callRetStack.Initialize(thread);
    thread->CurrentFrame->State.fs_cached = reinterpret_cast<uint64_t>(tlsPage.Get());

    std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> initialXmm {};
    std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> initialYmmHigh {};
    constexpr double fpLeft = 1.5;
    constexpr double fpRight = 2.25;
    const auto fpLeftBits = DoubleBits(fpLeft);
    const auto fpRightBits = DoubleBits(fpRight);
    std::memcpy(&initialXmm[0], &fpLeftBits, sizeof(fpLeftBits));
    std::memcpy(&initialXmm[1], &fpRightBits, sizeof(fpRightBits));
    Context->SetXMMRegistersFromState(thread, initialXmm.data(), HostFeatures.SupportsAVX ? initialYmmHigh.data() : nullptr);

    std::memcpy(static_cast<uint8_t*>(stackPage.Get()) + 1, &kUnalignedSentinel,
                sizeof(kUnalignedSentinel));
    thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_R12] =
        reinterpret_cast<uint64_t>(stackPage.Get()) + 1;
    {
      FexExecutionSignalScope signalScope {*Context, thread, SignalDelegator.get(),
                                           Syscalls.get()};
      Context->ExecuteThread(thread);
    }
    if (Syscalls->FailureResult(invocation)) return *Syscalls->FailureResult(invocation);

    auto& state = thread->CurrentFrame->State;
    GuestRunResult result;
    result.Gpr = state.gregs[FEXCore::X86State::REG_R8] == kAddLeft + kAddRight &&
                 state.gregs[FEXCore::X86State::REG_R9] == (kXorLeft ^ kXorRight) &&
                 state.gregs[FEXCore::X86State::REG_RSI] == kStackSentinel &&
                 state.gregs[FEXCore::X86State::REG_RDI] == kStackSentinel &&
                 state.gregs[FEXCore::X86State::REG_RSP] == initialRsp;
    const auto rflags = Context->ReconstructCompactedEFLAGS(thread, false, nullptr, 0);
    result.Rflags = (rflags & ((1U << 0) | (1U << 1) | (1U << 6) | (1U << 11))) == (1U << 1);
    result.Bridge = Syscalls->Invoked(invocation) && state.gregs[FEXCore::X86State::REG_RAX] == Syscalls->Result(invocation);
    result.Unaligned =
        state.gregs[FEXCore::X86State::REG_R13] == kUnalignedSentinel;

    std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> finalXmm {};
    std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> finalYmmHigh {};
    Context->ReconstructXMMRegisters(thread, finalXmm.data(), HostFeatures.SupportsAVX ? finalYmmHigh.data() : nullptr);
    uint64_t fpResultBits {};
    std::memcpy(&fpResultBits, &finalXmm[0], sizeof(fpResultBits));
    result.Xmm = fpResultBits == DoubleBits(fpLeft + fpRight);
    if (!result.Gpr || !result.Rflags || !result.Xmm || !result.Bridge) {
      return Failure(EngineStage::Execute, EPROTO);
    }

    bool firstThread = false;
    bool secondThread = false;
    std::thread firstHostThread {[&] { firstThread = ExecuteThreadTlsCheck(initialRip + guest.ThreadOffset, kThreadSentinelA); }};
    std::thread secondHostThread {[&] { secondThread = ExecuteThreadTlsCheck(initialRip + guest.ThreadOffset, kThreadSentinelB); }};
    firstHostThread.join();
    secondHostThread.join();
    result.Threads = firstThread && secondThread;
    result.Tls = result.Threads;
    if (!result.Threads) return Failure(EngineStage::Thread, EPROTO);

    state.gregs[FEXCore::X86State::REG_RDI] = kCallbackInput;
    state.gregs[FEXCore::X86State::REG_RSP] = initialRsp;
    {
      FexExecutionSignalScope signalScope {*Context, thread, SignalDelegator.get(),
                                           Syscalls.get()};
      Context->HandleCallback(thread, initialRip + guest.CallbackOffset);
    }
    if (state.gregs[FEXCore::X86State::REG_RAX] != kCallbackInput + 5 || state.gregs[FEXCore::X86State::REG_RSP] != initialRsp) {
      return Failure(EngineStage::Execute, EPROTO);
    }

    const uint64_t invalidationRip = initialRip + guest.InvalidationOffset;
    state.rip = invalidationRip;
    state.gregs[FEXCore::X86State::REG_RSP] = initialRsp;
    {
      FexExecutionSignalScope signalScope {*Context, thread, SignalDelegator.get(),
                                           Syscalls.get()};
      Context->ExecuteThread(thread);
    }
    if (state.gregs[FEXCore::X86State::REG_RAX] != kInvalidationInitial) return Failure(EngineStage::Invalidate, EPROTO);

    auto* invalidationImmediate = static_cast<uint8_t*>(code.Get()) + guest.InvalidationImmediateOffset;
    {
      std::scoped_lock codeInvalidationLock {Context->GetCodeInvalidationMutex()};
      const auto writable = code.Protect(PROT_READ | PROT_WRITE);
      if (const auto* error = std::get_if<EngineFailure>(&writable)) return *error;
      std::memcpy(invalidationImmediate, &kInvalidationUpdated, sizeof(kInvalidationUpdated));
      __builtin___clear_cache(reinterpret_cast<char*>(code.Get()), reinterpret_cast<char*>(code.Get()) + PageSize);
      const auto executable = code.Protect(PROT_READ);
      if (const auto* error = std::get_if<EngineFailure>(&executable)) return *error;
      Context->InvalidateCodeBuffersCodeRange(invalidationRip, 11);
      Context->InvalidateThreadCachedCodeRange(thread, invalidationRip, 11);
    }
    state.rip = invalidationRip;
    state.gregs[FEXCore::X86State::REG_RSP] = initialRsp;
    {
      FexExecutionSignalScope signalScope {*Context, thread, SignalDelegator.get(),
                                           Syscalls.get()};
      Context->ExecuteThread(thread);
    }
    result.Invalidation = state.gregs[FEXCore::X86State::REG_RAX] == kInvalidationUpdated;
    if (!result.Invalidation) return Failure(EngineStage::Invalidate, EPROTO);

    Ran = true;
    return result;
  }

  [[nodiscard]] EngineResult<bool> Shutdown() {
    {
      std::scoped_lock lock {ThreadsMutex};
      if (!Threads.empty()) {
        return Failure(EngineStage::Teardown, EBUSY);
      }
    }
    Context.reset();
    SignalDelegator.reset();
    Syscalls.reset();
    CallbackReturn.reset();
    FunctionReturn.reset();
    if (ConfigInitialized) {
      const auto configRelease = FexConfigLease::Release();
      if (const auto* failure = std::get_if<EngineFailure>(&configRelease)) return *failure;
      ConfigInitialized = false;
    }
    return true;
  }

  [[nodiscard]] bool ExecuteThreadTlsCheck(uint64_t rip, uint64_t sentinel) {
    Mapping stackPage {PageSize, PROT_READ | PROT_WRITE};
    Mapping tlsPage {PageSize, PROT_READ | PROT_WRITE};
    if (!stackPage.IsValid() || !tlsPage.IsValid()) return false;
    std::memcpy(tlsPage.Get(), &sentinel, sizeof(sentinel));
    CallRetStack callRetStack{PageSize};
    if (!callRetStack.IsReserved()) return false;
    const auto callRetStackProtection = callRetStack.MakeWritable();
    if (std::holds_alternative<EngineFailure>(callRetStackProtection)) return false;
    const auto initialRsp = reinterpret_cast<uint64_t>(stackPage.Get()) + PageSize - 16;
    auto* thread = CreateNativeThread(*Context, rip, initialRsp);
    if (thread == nullptr) return false;
    ThreadScope threadScope {*Context, thread};
    const std::vector<GuestExecutionRange> ranges {
        {static_cast<std::uintptr_t>(rip & ~(PageSize - 1)), PageSize, true, false},
    };
    const auto registered = Syscalls->RegisterThread(thread, ranges);
    if (std::holds_alternative<EngineFailure>(registered)) return false;
    ExecutableRangeScope rangeScope {*Syscalls, thread};
    BridgeSyscallHandler::InvocationState invocation;
    BridgeSyscallHandler::InvocationScope invocationScope {*Syscalls, invocation, thread};
    GuestSegmentState segmentState;
    segmentState.Initialize(thread->CurrentFrame->State);
    callRetStack.Initialize(thread);
    thread->CurrentFrame->State.fs_cached = reinterpret_cast<uint64_t>(tlsPage.Get());
    {
      FexExecutionSignalScope signalScope {*Context, thread, SignalDelegator.get(),
                                           Syscalls.get()};
      Context->ExecuteThread(thread);
    }
    return thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_RAX] == sentinel &&
           thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_RSP] == initialRsp;
  }

  GuestBridge& Bridge;
  FEXCore::HostFeatures HostFeatures {};
  fextl::unique_ptr<FEXCore::Context::Context> Context;
  std::unique_ptr<BridgeSignalDelegator> SignalDelegator;
  std::unique_ptr<BridgeSyscallHandler> Syscalls;
  std::unique_ptr<Mapping> CallbackReturn;
  std::unique_ptr<Mapping> FunctionReturn;
  size_t PageSize {};
  bool ConfigInitialized {};
  bool Ran {};
  std::mutex ThreadsMutex;
  std::unordered_set<Thread*> Threads;
};

GuestEngine::GuestEngine(std::unique_ptr<Impl> impl)
  : ImplState {std::move(impl)} {}

GuestEngine::~GuestEngine() {
  if (ImplState != nullptr && std::holds_alternative<EngineFailure>(Shutdown())) {
    std::abort();
  }
}

EngineResult<std::unique_ptr<GuestEngine>> GuestEngine::Create(GuestBridge& bridge) {
  const auto pageSize = sysconf(_SC_PAGESIZE);
#if defined(__APPLE__)
  if (pageSize != 16384 || !pthread_jit_write_protect_supported_np())
    return Failure(EngineStage::Mapping, ENOTSUP);
  pthread_jit_write_protect_np(1);
#else
  if (pageSize != kRequiredPageSize) return Failure(EngineStage::Mapping, ENOTSUP);
#endif

  auto impl = std::make_unique<Impl>(bridge);
  impl->PageSize = static_cast<size_t>(pageSize);
  const auto configAcquire = FexConfigLease::Acquire();
  if (const auto* failure = std::get_if<EngineFailure>(&configAcquire)) return *failure;
  impl->ConfigInitialized = true;
  const auto fail = [&impl](EngineFailure original) -> EngineResult<std::unique_ptr<GuestEngine>> {
    const auto teardown = impl->Shutdown();
    if (const auto* error = std::get_if<EngineFailure>(&teardown)) return *error;
    return original;
  };

  impl->HostFeatures = FEX::FetchHostFeatures();
  impl->Context = FEXCore::Context::Context::CreateNewContext(impl->HostFeatures);
  if (impl->Context == nullptr) {
    return fail(Failure(EngineStage::Context, ENOMEM));
  }
  impl->FunctionReturn =
      std::make_unique<Mapping>(impl->PageSize, PROT_READ | PROT_WRITE);
  if (!impl->FunctionReturn->IsValid()) {
    return fail(Failure(EngineStage::Mapping, impl->FunctionReturn->Error()));
  }
  *static_cast<uint8_t*>(impl->FunctionReturn->Get()) = 0xf4;
  __builtin___clear_cache(static_cast<char*>(impl->FunctionReturn->Get()),
                          static_cast<char*>(impl->FunctionReturn->Get()) + impl->PageSize);
  const auto functionProtection = impl->FunctionReturn->Protect(PROT_READ | PROT_EXEC);
  if (const auto* failure = std::get_if<EngineFailure>(&functionProtection)) {
    return fail(*failure);
  }

  impl->CallbackReturn =
      std::make_unique<Mapping>(impl->PageSize, PROT_READ | PROT_WRITE);
  if (!impl->CallbackReturn->IsValid()) {
    return fail(Failure(EngineStage::Mapping, impl->CallbackReturn->Error()));
  }
  auto* callbackReturn = static_cast<uint8_t*>(impl->CallbackReturn->Get());
  callbackReturn[0] = 0x0f;
  callbackReturn[1] = 0x3e;
  __builtin___clear_cache(static_cast<char*>(impl->CallbackReturn->Get()),
                          static_cast<char*>(impl->CallbackReturn->Get()) + impl->PageSize);
  const auto callbackProtection = impl->CallbackReturn->Protect(PROT_READ | PROT_EXEC);
  if (const auto* failure = std::get_if<EngineFailure>(&callbackProtection)) {
    return fail(*failure);
  }
  impl->SignalDelegator = std::make_unique<BridgeSignalDelegator>(
      reinterpret_cast<std::uintptr_t>(impl->CallbackReturn->Get()));
  impl->Syscalls = std::make_unique<BridgeSyscallHandler>(impl->Bridge);
  impl->Context->SetSignalDelegator(impl->SignalDelegator.get());
  impl->Context->SetSyscallHandler(impl->Syscalls.get());
  impl->Context->EnableExitOnHLT();
  const bool initialized = [&] {
#if defined(__APPLE__)
    FEXCore::Allocator::ScopedJITWriteProtection writableJit;
#endif
    return impl->Context->InitCore();
  }();
  if (!initialized) {
    return fail(Failure(EngineStage::Context, EIO));
  }

  return std::unique_ptr<GuestEngine> {new GuestEngine {std::move(impl)}};
}

EngineResult<GuestRunResult> GuestEngine::RunControlledHarness() {
  if (ImplState == nullptr) return Failure(EngineStage::Teardown, ESHUTDOWN);
  return ImplState->Run();
}

EngineResult<GuestEngine::Thread*> GuestEngine::CreateThread(const GuestExecutionRequest& request) {
  if (ImplState == nullptr || ImplState->Context == nullptr || ImplState->Syscalls == nullptr) {
    return Failure(EngineStage::Teardown, ESHUTDOWN);
  }
  const auto validation = ValidateRequest(request);
  if (const auto* failure = std::get_if<EngineFailure>(&validation)) {
    return *failure;
  }

  auto thread = std::make_unique<Thread>(std::this_thread::get_id(), request, ImplState->PageSize);
  if (!thread->CallRet.IsReserved()) {
    return Failure(EngineStage::Mapping, thread->CallRet.Error());
  }
  const auto callRetWritable = thread->CallRet.MakeWritable();
  if (const auto* failure = std::get_if<EngineFailure>(&callRetWritable)) {
    return *failure;
  }

  thread->Native = CreateNativeThread(*ImplState->Context, request.Rip, request.Rsp);
  if (thread->Native == nullptr) {
    return Failure(EngineStage::Thread, ENOMEM);
  }

  auto& state = thread->Native->CurrentFrame->State;
  thread->Segments.Initialize(state);
  state.fs_cached = request.FsBase;
  state.gs_cached = request.GsBase;
  thread->CallRet.Initialize(thread->Native);
  state.rip = request.Rip;
  std::copy(request.Gpr.begin(), request.Gpr.end(), std::begin(state.gregs));
  state.gregs[FEXCore::X86State::REG_RSP] = request.Rsp;
  ImplState->Context->SetFlagsFromCompactedEFLAGS(thread->Native, static_cast<uint32_t>(request.Rflags));

  std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> xmm {};
  std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> ymmHigh {};
  static_assert(std::tuple_size_v<decltype(request.Xmm)> == FEXCore::Core::CPUState::NUM_XMMS);
  for (size_t index = 0; index < xmm.size(); ++index) {
    std::memcpy(&xmm[index], request.Xmm[index].data(), sizeof(xmm[index]));
  }
  ImplState->Context->SetXMMRegistersFromState(thread->Native, xmm.data(),
                                               ImplState->HostFeatures.SupportsAVX ? ymmHigh.data() : nullptr);
  thread->FirstRip = request.Rip;
  const auto registration = ImplState->Syscalls->RegisterThread(thread->Native, request.MappedRanges);
  if (const auto* failure = std::get_if<EngineFailure>(&registration)) {
    ImplState->Context->DestroyThread(thread->Native);
    thread->Native = nullptr;
    return *failure;
  }
  {
    std::scoped_lock lock {ImplState->ThreadsMutex};
    const auto [_, inserted] = ImplState->Threads.insert(thread.get());
    if (!inserted) {
      ImplState->Syscalls->UnregisterThread(thread->Native);
      ImplState->Context->DestroyThread(thread->Native);
      thread->Native = nullptr;
      return Failure(EngineStage::Thread, EEXIST);
    }
  }
  return thread.release();
}

EngineResult<GuestExecutionState> GuestEngine::Run(Thread& thread) {
  if (ImplState == nullptr || ImplState->Context == nullptr || thread.Native == nullptr) {
    return Failure(EngineStage::Teardown, ESHUTDOWN);
  }
  {
    std::scoped_lock lock {ImplState->ThreadsMutex};
    if (!ImplState->Threads.contains(&thread) || thread.Owner != std::this_thread::get_id()) {
      return Failure(EngineStage::Thread, EPERM);
    }
  }

  BridgeSyscallHandler::InvocationScope invocationScope {*ImplState->Syscalls, thread.Invocation,
                                                          thread.Native};
  {
    FexExecutionSignalScope signalScope {*ImplState->Context, thread.Native,
                                         ImplState->SignalDelegator.get(),
                                         ImplState->Syscalls.get()};
    ImplState->Context->ExecuteThread(thread.Native);
  }
  if (ImplState->Syscalls->FailureResult(thread.Invocation)) {
    return *ImplState->Syscalls->FailureResult(thread.Invocation);
  }

  const auto& frame = thread.Native->CurrentFrame->State;
  GuestExecutionState result;
  result.FirstRip = thread.FirstRip;
  result.Rip = frame.rip;
  result.Rsp = frame.gregs[FEXCore::X86State::REG_RSP];
  std::copy(std::begin(frame.gregs), std::end(frame.gregs), result.Gpr.begin());
  result.Rflags = ImplState->Context->ReconstructCompactedEFLAGS(thread.Native, false, nullptr, 0);
  std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> xmm {};
  std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> ymmHigh {};
  ImplState->Context->ReconstructXMMRegisters(thread.Native, xmm.data(),
                                               ImplState->HostFeatures.SupportsAVX ? ymmHigh.data() : nullptr);
  for (size_t index = 0; index < xmm.size(); ++index) {
    std::memcpy(result.Xmm[index].data(), &xmm[index], sizeof(xmm[index]));
  }
  thread.LastRip = result.Rip;
  result.LastRip = thread.LastRip;
  // This Phase-1 context exits only because EnableExitOnHLT() is active.
  result.StopReason = GuestStopReason::Halted;
  return result;
}

EngineResult<GuestExecutionState> GuestEngine::CallGuest(
    std::uintptr_t rip, std::span<const std::uint64_t> arguments
#if defined(__APPLE__)
    , std::span<const std::array<std::uint64_t, 2>> callbackXmm
#endif
    ) {
  if (ImplState == nullptr || ImplState->Context == nullptr || ImplState->Syscalls == nullptr) {
    return Failure(EngineStage::Teardown, ESHUTDOWN);
  }
  auto* thread = ImplState->Syscalls->ActiveThread();
  if (thread == nullptr) return Failure(EngineStage::Thread, ENXIO);
  const auto executable = ImplState->Syscalls->QueryGuestExecutableRange(thread, rip);
  if (executable.Size == 0) return Failure(EngineStage::Mapping, EFAULT);
#if defined(__APPLE__)
  if (arguments.size() > 32 || callbackXmm.size() > 8) return Failure(EngineStage::Request, E2BIG);
#else
  if (arguments.size() > 7) return Failure(EngineStage::Request, E2BIG);
#endif

  auto& frame = thread->CurrentFrame->State;
#if defined(__APPLE__)
  // Native font callbacks carry independent integer/SIMD argument banks and
  // can recurse through HLE. Keep their stack below the suspended caller's
  // red zone and restore that caller after collecting the callback result.
  struct RestoreFrame {
    FEXCore::Core::CPUState& frame;
    FEXCore::Core::CPUState saved {};
    static void CopyGuestState(FEXCore::Core::CPUState& to, const FEXCore::Core::CPUState& from) {
      to.rip = from.rip;
      to.callret_sp = from.callret_sp;
      to.pf_raw = from.pf_raw;
      to.af_raw = from.af_raw;
      to.mxcsr = from.mxcsr;
      to.FCW = from.FCW;
      to.AbridgedFTW = from.AbridgedFTW;
      to.fs_cached = from.fs_cached;
      to.gs_cached = from.gs_cached;
      std::memcpy(to.gregs, from.gregs, sizeof(to.gregs));
      std::memcpy(to.flags, from.flags, sizeof(to.flags));
      std::memcpy(to.mm, from.mm, sizeof(to.mm));
      std::memcpy(to.avx_high, from.avx_high, sizeof(to.avx_high));
      to.xmm = from.xmm;
    }
    explicit RestoreFrame(FEXCore::Core::CPUState& state) : frame{state} { CopyGuestState(saved, frame); }
    ~RestoreFrame() { CopyGuestState(frame, saved); }
  } restoreFrame{frame};
  const auto stackBytes = arguments.size() > 6 ? (arguments.size() - 6) * sizeof(uint64_t) : 0;
  const auto parentRsp = frame.gregs[FEXCore::X86State::REG_RSP];
  if (parentRsp < 256 + stackBytes + 32) return Failure(EngineStage::Mapping, EFAULT);
  const auto callbackRsp = ((parentRsp - 256 - stackBytes) & ~uintptr_t{15}) - 8;
  if (!ImplState->Syscalls->IsWritableRange(thread, callbackRsp - 16, 32 + stackBytes))
    return Failure(EngineStage::Mapping, EFAULT);
  // FEX's callback trampoline subtracts sixteen bytes before pushing RET.
  frame.gregs[FEXCore::X86State::REG_RSP] = callbackRsp + 16;
  for (size_t index = 6; index < arguments.size(); ++index)
    std::memcpy(reinterpret_cast<void*>(callbackRsp + (index - 5) * 8), &arguments[index], 8);
  std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> callbackVectors {};
  std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> callbackHigh {};
  for (size_t index = 0; index < callbackXmm.size(); ++index)
    std::memcpy(&callbackVectors[index], callbackXmm[index].data(), 16);
  ImplState->Context->SetXMMRegistersFromState(thread, callbackVectors.data(),
      ImplState->HostFeatures.SupportsAVX ? callbackHigh.data() : nullptr);
#endif
  constexpr std::array<size_t, 6> argumentRegisters {
      FEXCore::X86State::REG_RDI, FEXCore::X86State::REG_RSI, FEXCore::X86State::REG_RDX,
      FEXCore::X86State::REG_RCX, FEXCore::X86State::REG_R8, FEXCore::X86State::REG_R9,
  };
  const auto registerCount = std::min(arguments.size(), argumentRegisters.size());
  for (size_t index = 0; index < registerCount; ++index) {
    frame.gregs[argumentRegisters[index]] = arguments[index];
  }
#if !defined(__APPLE__)
  if (arguments.size() == 7) {
    const auto rsp = frame.gregs[FEXCore::X86State::REG_RSP];
    if (rsp < sizeof(uint64_t) ||
        !ImplState->Syscalls->IsWritableRange(thread, rsp - sizeof(uint64_t),
                                               sizeof(uint64_t))) {
      return Failure(EngineStage::Mapping, EFAULT);
    }
    std::memcpy(reinterpret_cast<void*>(rsp - sizeof(uint64_t)), &arguments[6],
                sizeof(uint64_t));
  }

#endif
  BridgeSyscallHandler::InvocationState invocation;
  BridgeSyscallHandler::InvocationScope invocationScope {*ImplState->Syscalls, invocation, thread};
  {
    FexExecutionSignalScope signalScope {*ImplState->Context, thread,
                                         ImplState->SignalDelegator.get(),
                                         ImplState->Syscalls.get()};
    ImplState->Context->HandleCallback(thread, rip);
  }
  if (ImplState->Syscalls->FailureResult(invocation)) {
    return *ImplState->Syscalls->FailureResult(invocation);
  }
  if (ImplState->Syscalls->ActiveThread() != thread) {
    return Failure(EngineStage::Thread, EFAULT);
  }

  GuestExecutionState result;
  result.FirstRip = rip;
  result.Rip = frame.rip;
  result.LastRip = frame.rip;
  result.Rsp = frame.gregs[FEXCore::X86State::REG_RSP];
  std::copy(std::begin(frame.gregs), std::end(frame.gregs), result.Gpr.begin());
  result.Rflags = ImplState->Context->ReconstructCompactedEFLAGS(thread, false, nullptr, 0);
  std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> xmm {};
  std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> ymmHigh {};
  ImplState->Context->ReconstructXMMRegisters(
      thread, xmm.data(), ImplState->HostFeatures.SupportsAVX ? ymmHigh.data() : nullptr);
  for (size_t index = 0; index < xmm.size(); ++index) {
    std::memcpy(result.Xmm[index].data(), &xmm[index], sizeof(xmm[index]));
  }
  result.StopReason = GuestStopReason::Returned;
  return result;
}

EngineResult<bool> GuestEngine::Invalidate(Thread& thread, std::uintptr_t begin, std::size_t size) {
  if (ImplState == nullptr || ImplState->Context == nullptr || thread.Native == nullptr) {
    return Failure(EngineStage::Teardown, ESHUTDOWN);
  }
  {
    std::scoped_lock lock {ImplState->ThreadsMutex};
    if (!ImplState->Threads.contains(&thread) || thread.Owner != std::this_thread::get_id()) {
      return Failure(EngineStage::Thread, EPERM);
    }
  }
  if (begin == 0 || size == 0 || begin % kRequiredPageSize != 0 || size % kRequiredPageSize != 0) {
    return Failure(EngineStage::Request, EINVAL);
  }
  const auto executable = std::any_of(thread.Request.MappedRanges.begin(), thread.Request.MappedRanges.end(),
                                      [begin, size](const GuestExecutionRange& range) {
                                        return range.Executable && !range.Writable && Contains(range, begin, size);
                                      });
  if (!executable) {
    return Failure(EngineStage::Invalidate, EFAULT);
  }

  std::scoped_lock lock {ImplState->Context->GetCodeInvalidationMutex()};
  ImplState->Context->InvalidateCodeBuffersCodeRange(begin, size);
  ImplState->Context->InvalidateThreadCachedCodeRange(thread.Native, begin, size);
  return true;
}

EngineResult<bool> GuestEngine::DestroyThread(Thread*& thread) {
  if (ImplState == nullptr || ImplState->Context == nullptr || thread == nullptr || thread->Native == nullptr) {
    return Failure(EngineStage::Teardown, ESHUTDOWN);
  }
  {
    std::scoped_lock lock {ImplState->ThreadsMutex};
    if (!ImplState->Threads.contains(thread) || thread->Owner != std::this_thread::get_id()) {
      return Failure(EngineStage::Thread, EPERM);
    }
  }
  ImplState->Syscalls->UnregisterThread(thread->Native);
  ImplState->Context->DestroyThread(thread->Native);
  thread->Native = nullptr;
  {
    std::scoped_lock lock {ImplState->ThreadsMutex};
    ImplState->Threads.erase(thread);
  }
  delete thread;
  thread = nullptr;
  return true;
}

EngineResult<bool> GuestEngine::Shutdown() {
  if (ImplState == nullptr) return true;
  auto result = ImplState->Shutdown();
  if (std::holds_alternative<bool>(result)) {
    ImplState.reset();
  }
  return result;
}

std::uintptr_t GuestEngine::ReturnAddress() const {
  if (ImplState == nullptr || ImplState->FunctionReturn == nullptr) return 0;
  return reinterpret_cast<std::uintptr_t>(ImplState->FunctionReturn->Get());
}

GuestExecutionRange GuestEngine::ReturnRange() const {
  if (ImplState == nullptr || ImplState->FunctionReturn == nullptr) return {};
  return {reinterpret_cast<std::uintptr_t>(ImplState->FunctionReturn->Get()),
          ImplState->PageSize, true, false};
}

GuestExecutionRange GuestEngine::CallbackReturnRange() const {
  if (ImplState == nullptr || ImplState->CallbackReturn == nullptr) return {};
  return {reinterpret_cast<std::uintptr_t>(ImplState->CallbackReturn->Get()),
          ImplState->PageSize, true, false};
}

} // namespace Core::Fex
