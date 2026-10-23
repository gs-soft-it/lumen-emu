#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/Allocator.h>

#include <rpmalloc/rpmalloc.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>

namespace {

constexpr std::uint64_t kExpectedResult = UINT64_C(0x12345678);
constexpr std::size_t kGuestPageSize = 4096;

enum class ResultCode : int {
  Success = 0,
  SelectorDenied = 64,
  HostRejected = 65,
  JitCapabilityFailed = 66,
  AllocatorFailed = 67,
  ConfigurationFailed = 68,
  ContextFailed = 69,
  GuestMappingFailed = 70,
  GuestThreadFailed = 71,
  GuestResultFailed = 72,
  AdapterFailed = 73,
};

struct Receipt {
  ResultCode Code {ResultCode::HostRejected};
  const char* Reason {"uninitialized"};
  std::uint32_t BootstrapSmoke {};
  std::uint32_t Context {};
  std::uint32_t GuestExecuted {};
  std::uint32_t MapJit {};
  std::uint32_t ThreadWx {};
  std::uint32_t PrecompileCalls {};
  std::uint32_t ExecutePrecompileHookDelta {};
  std::uint32_t ExecuteCompileMissDelta {};
  std::uint32_t SyscallCalls {};
  std::uint32_t QueryAllowed {};
  std::uint32_t QueryDenied {};
  std::uint32_t MarkAccepted {};
  std::uint32_t MarkRejected {};
  std::uint32_t Invalidates {};
  std::uint32_t LookupMisses {};
  std::uint32_t OvercommitMarks {};
  std::uint32_t OvercommitUnmarks {};
  std::uint32_t SleepCalls {};
  std::uint32_t LogicalCores {};
  std::size_t HostPageSize {};
  std::uint64_t GuestResult {};
};

int EmitReceipt(const Receipt& ReceiptState) {
  const bool Passed = ReceiptState.Code == ResultCode::Success;
  std::printf(
    "LUMEN_MACOS_FEXCORE_BOOTSTRAP_V0 "
    "pass=%u code=%d reason=%s bootstrapSmoke=%u runtimeReady=0 "
    "romRoute=0 fallback=0 nodaRoute=0 precompiledOnly=1 lazyJit=0 "
    "hltStop=exit-on-any-hlt-debug-only "
    "hostType=unknown hostPageSize=%zu guestPageSize=%zu "
    "mapJit=%u rawRwxAttempted=0 threadWx=%u "
    "jitPublisher=thread-toggle-debug-only "
    "icache=sys_icache_invalidate x18AllocatorExcluded=1 "
    "x18RuntimeCanary=0 pageGuardQualified=0 context=%u guestExecuted=%u "
    "opt=O2 assertions=1 maxInst=5000 smcConfig=mtrack "
    "guestCodeProtection=ro syscallPolicy=abort-deny "
    "precompileCalls=%u executePrecompileHookDelta=%u "
    "executeCompileMissDelta=%u syscallCalls=%u "
    "queryAllowed=%u queryDenied=%u markAccepted=%u markRejected=%u "
    "invalidates=%u lookupMisses=%u overcommitMarks=%u "
    "overcommitUnmarks=%u sleepCalls=%u "
    "monoHacks=0 "
    "logicalCores=%u guestResult=0x%016llx upstream=e17fdf9 "
    "port=2d729002\n",
    Passed ? 1U : 0U,
    static_cast<int>(ReceiptState.Code),
    ReceiptState.Reason,
    ReceiptState.BootstrapSmoke,
    ReceiptState.HostPageSize,
    kGuestPageSize,
    ReceiptState.MapJit,
    ReceiptState.ThreadWx,
    ReceiptState.Context,
    ReceiptState.GuestExecuted,
    ReceiptState.PrecompileCalls,
    ReceiptState.ExecutePrecompileHookDelta,
    ReceiptState.ExecuteCompileMissDelta,
    ReceiptState.SyscallCalls,
    ReceiptState.QueryAllowed,
    ReceiptState.QueryDenied,
    ReceiptState.MarkAccepted,
    ReceiptState.MarkRejected,
    ReceiptState.Invalidates,
    ReceiptState.LookupMisses,
    ReceiptState.OvercommitMarks,
    ReceiptState.OvercommitUnmarks,
    ReceiptState.SleepCalls,
    ReceiptState.LogicalCores,
    static_cast<unsigned long long>(ReceiptState.GuestResult));
  std::fflush(stdout);
  return static_cast<int>(ReceiptState.Code);
}

class JitWriteScope final {
public:
  JitWriteScope() noexcept {
    pthread_jit_write_protect_np(0);
  }

  ~JitWriteScope() {
    pthread_jit_write_protect_np(1);
  }

  JitWriteScope(const JitWriteScope&) = delete;
  JitWriteScope& operator=(const JitWriteScope&) = delete;
};

class RpmallocFinalizer final {
public:
  ~RpmallocFinalizer() {
    rpmalloc_finalize();
  }

  RpmallocFinalizer() = default;
  RpmallocFinalizer(const RpmallocFinalizer&) = delete;
  RpmallocFinalizer& operator=(const RpmallocFinalizer&) = delete;
};

class ConfigFinalizer final {
public:
  ~ConfigFinalizer() {
    FEXCore::Config::Shutdown();
  }

  ConfigFinalizer() = default;
  ConfigFinalizer(const ConfigFinalizer&) = delete;
  ConfigFinalizer& operator=(const ConfigFinalizer&) = delete;
};

class MappedPage final {
public:
  MappedPage(void* Pointer, std::size_t Size) noexcept
    : Pointer {Pointer}, Size {Size} {}

  ~MappedPage() {
    if (Pointer != MAP_FAILED && Pointer != nullptr) {
      munmap(Pointer, Size);
    }
  }

  MappedPage(const MappedPage&) = delete;
  MappedPage& operator=(const MappedPage&) = delete;

  void* Get() const noexcept {
    return Pointer;
  }

private:
  void* Pointer {};
  std::size_t Size {};
};

class LabSyscallHandler final : public FEXCore::HLE::SyscallHandler {
public:
  struct CounterSnapshot {
    std::uint32_t PrecompileCalls {};
    std::uint32_t SyscallCalls {};
    std::uint32_t QueryAllowed {};
    std::uint32_t QueryDenied {};
    std::uint32_t MarkAccepted {};
    std::uint32_t MarkRejected {};
    std::uint32_t Invalidates {};
    std::uint32_t LookupMisses {};
    std::uint32_t OvercommitMarks {};
    std::uint32_t OvercommitUnmarks {};
    std::uint32_t SleepCalls {};
  };

  LabSyscallHandler(std::uint64_t MappingBase,
                    std::uint64_t MappingSize,
                    std::uint64_t CodeBase,
                    std::uint64_t CodeSize)
    : MappingBase {MappingBase},
      MappingSize {MappingSize},
      CodeBase {CodeBase},
      CodeSize {CodeSize},
      Valid {MappingSize != 0 && CodeSize != 0 &&
             CodeBase >= MappingBase &&
             CodeBase - MappingBase < MappingSize &&
             CodeSize <= MappingSize - (CodeBase - MappingBase)} {
    OSABI = FEXCore::HLE::SyscallOSABI::OS_GENERIC;
  }

  [[noreturn]] std::uint64_t HandleSyscall(
    FEXCore::Core::CpuStateFrame*,
    FEXCore::HLE::SyscallArguments*) override {
    SyscallCalls.fetch_add(1, std::memory_order_relaxed);
    std::abort();
  }

  std::optional<FEXCore::ExecutableFileSectionInfo>
  LookupExecutableFileSection(
    FEXCore::Core::InternalThreadState*, std::uint64_t) override {
    LookupMisses.fetch_add(1, std::memory_order_relaxed);
    return std::nullopt;
  }

  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(
    FEXCore::Core::InternalThreadState*, std::uint64_t Address) override {
    if (Address < CodeBase || Address - CodeBase >= CodeSize) {
      QueryDenied.fetch_add(1, std::memory_order_relaxed);
      return {0, 0, false};
    }
    QueryAllowed.fetch_add(1, std::memory_order_relaxed);
    return {CodeBase, CodeSize, false};
  }

  void MarkGuestExecutableRange(
    FEXCore::Core::InternalThreadState*,
    std::uint64_t Start,
    std::uint64_t Length) override {
    const bool Contained = Length != 0 && Start >= MappingBase &&
      Start - MappingBase < MappingSize &&
      Length <= MappingSize - (Start - MappingBase);
    const bool IntersectsCode = Contained &&
      Start < CodeBase + CodeSize && CodeBase < Start + Length;
    if (IntersectsCode) {
      MarkAccepted.fetch_add(1, std::memory_order_relaxed);
    } else {
      MarkRejected.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void InvalidateGuestCodeRange(
    FEXCore::Core::InternalThreadState*,
    std::uint64_t,
    std::uint64_t) override {
    Invalidates.fetch_add(1, std::memory_order_relaxed);
  }

  void MarkOvercommitRange(std::uint64_t, std::uint64_t) override {
    OvercommitMarks.fetch_add(1, std::memory_order_relaxed);
  }

  void UnmarkOvercommitRange(std::uint64_t, std::uint64_t) override {
    OvercommitUnmarks.fetch_add(1, std::memory_order_relaxed);
  }

  void PreCompile() override {
    PrecompileCalls.fetch_add(1, std::memory_order_relaxed);
  }

  void SleepThread(
    FEXCore::Context::Context*,
    FEXCore::Core::CpuStateFrame*) override {
    SleepCalls.fetch_add(1, std::memory_order_relaxed);
  }

  bool IsValid() const {
    return Valid;
  }

  CounterSnapshot Snapshot() const {
    return {
      .PrecompileCalls = PrecompileCalls.load(std::memory_order_relaxed),
      .SyscallCalls = SyscallCalls.load(std::memory_order_relaxed),
      .QueryAllowed = QueryAllowed.load(std::memory_order_relaxed),
      .QueryDenied = QueryDenied.load(std::memory_order_relaxed),
      .MarkAccepted = MarkAccepted.load(std::memory_order_relaxed),
      .MarkRejected = MarkRejected.load(std::memory_order_relaxed),
      .Invalidates = Invalidates.load(std::memory_order_relaxed),
      .LookupMisses = LookupMisses.load(std::memory_order_relaxed),
      .OvercommitMarks = OvercommitMarks.load(std::memory_order_relaxed),
      .OvercommitUnmarks = OvercommitUnmarks.load(std::memory_order_relaxed),
      .SleepCalls = SleepCalls.load(std::memory_order_relaxed),
    };
  }

private:
  const std::uint64_t MappingBase;
  const std::uint64_t MappingSize;
  const std::uint64_t CodeBase;
  const std::uint64_t CodeSize;
  const bool Valid;
  std::atomic<std::uint32_t> PrecompileCalls {};
  std::atomic<std::uint32_t> SyscallCalls {};
  std::atomic<std::uint32_t> QueryAllowed {};
  std::atomic<std::uint32_t> QueryDenied {};
  std::atomic<std::uint32_t> MarkAccepted {};
  std::atomic<std::uint32_t> MarkRejected {};
  std::atomic<std::uint32_t> Invalidates {};
  std::atomic<std::uint32_t> LookupMisses {};
  std::atomic<std::uint32_t> OvercommitMarks {};
  std::atomic<std::uint32_t> OvercommitUnmarks {};
  std::atomic<std::uint32_t> SleepCalls {};
};

void CaptureAdapterReceipt(
  Receipt* ReceiptState,
  const LabSyscallHandler::CounterSnapshot& Counters,
  std::uint32_t ExecutePrecompileHookDelta,
  std::uint32_t ExecuteCompileMissDelta) {
  ReceiptState->PrecompileCalls = Counters.PrecompileCalls;
  ReceiptState->ExecutePrecompileHookDelta = ExecutePrecompileHookDelta;
  ReceiptState->ExecuteCompileMissDelta = ExecuteCompileMissDelta;
  ReceiptState->SyscallCalls = Counters.SyscallCalls;
  ReceiptState->QueryAllowed = Counters.QueryAllowed;
  ReceiptState->QueryDenied = Counters.QueryDenied;
  ReceiptState->MarkAccepted = Counters.MarkAccepted;
  ReceiptState->MarkRejected = Counters.MarkRejected;
  ReceiptState->Invalidates = Counters.Invalidates;
  ReceiptState->LookupMisses = Counters.LookupMisses;
  ReceiptState->OvercommitMarks = Counters.OvercommitMarks;
  ReceiptState->OvercommitUnmarks = Counters.OvercommitUnmarks;
  ReceiptState->SleepCalls = Counters.SleepCalls;
}

bool ReadSysctlU64(const char* Name, std::uint64_t* Value) {
  if (Name == nullptr || Value == nullptr) {
    return false;
  }
  std::uint64_t Storage {};
  std::size_t Size = sizeof(Storage);
  if (sysctlbyname(Name, &Storage, &Size, nullptr, 0) != 0) {
    return false;
  }
  if (Size == sizeof(std::uint32_t)) {
    std::uint32_t Narrow {};
    std::memcpy(&Narrow, &Storage, sizeof(Narrow));
    *Value = Narrow;
    return true;
  }
  if (Size != sizeof(Storage)) {
    return false;
  }
  *Value = Storage;
  return true;
}

bool BuildHostFeatures(FEXCore::HostFeatures* Features,
                       std::uint32_t* LogicalCores) {
  if (Features == nullptr || LogicalCores == nullptr) {
    return false;
  }

  std::uint64_t CacheLineSize {};
  std::uint64_t HostLogicalCores {};
  if (!ReadSysctlU64("hw.cachelinesize", &CacheLineSize) ||
      !ReadSysctlU64("hw.logicalcpu", &HostLogicalCores) ||
      CacheLineSize < sizeof(std::uint32_t) ||
      (CacheLineSize & (CacheLineSize - 1)) != 0 ||
      HostLogicalCores == 0 || HostLogicalCores > 255) {
    return false;
  }

  std::uint32_t CacheLineLog2 {};
  for (std::uint64_t Bytes = sizeof(std::uint32_t);
       Bytes < CacheLineSize; Bytes <<= 1) {
    ++CacheLineLog2;
  }

  Features->DCacheLineLog2 = CacheLineLog2;
  Features->SupportsCacheMaintenanceOps = true;
  Features->SupportsAVX = true;
  Features->Supports3DNow = true;
#ifdef FEX_HAS_PRESERVE_ALL_ATTR
  Features->SupportsPreserveAllABI = FEX_HAS_PRESERVE_ALL_ATTR != 0;
#endif
  Features->HostType = FEXCore::HostFeatures::HostTypeEnum::Unknown;
  Features->CPUMIDRs.resize(static_cast<std::size_t>(HostLogicalCores), 0);
  *LogicalCores = static_cast<std::uint32_t>(HostLogicalCores);
  return true;
}

void ConfigureLongMode(FEXCore::Core::CPUState& State) {
  State.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] =
    State.private_gdt;
  State.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] =
    State.private_gdt;
  State.cs_idx = FEXCore::Core::CPUState::DEFAULT_USER_CS << 3;
  auto* CodeSegment =
    FEXCore::Core::CPUState::GetSegmentFromIndex(State, State.cs_idx);
  FEXCore::Core::CPUState::SetGDTBase(CodeSegment, 0);
  FEXCore::Core::CPUState::SetGDTLimit(CodeSegment, 0xF'FFFFU);
  CodeSegment->L = 1;
  CodeSegment->D = 0;
  State.cs_cached = FEXCore::Core::CPUState::CalculateGDTBase(*CodeSegment);
}

bool ConfigureFEX() {
  FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
  FEXCore::Config::Set(FEXCore::Config::CONFIG_DISABLETELEMETRY, "1");
  FEXCore::Config::Set(
    FEXCore::Config::CONFIG_DISABLE_VIXL_INDIRECT_RUNTIME_CALLS, "1");
  FEXCore::Config::Set(FEXCore::Config::CONFIG_MULTIBLOCK, "1");
  FEXCore::Config::Set(FEXCore::Config::CONFIG_MAXINST, "5000");
  FEXCore::Config::Set(FEXCore::Config::CONFIG_SMCCHECKS, "1");
  FEXCore::Config::Set(FEXCore::Config::CONFIG_MONOHACKS, "0");

  return FEXCore::Config::Get_IS64BIT_MODE() &&
    FEXCore::Config::Get_DISABLETELEMETRY() &&
    FEXCore::Config::Get_DISABLE_VIXL_INDIRECT_RUNTIME_CALLS() &&
    FEXCore::Config::Get_MULTIBLOCK() &&
    FEXCore::Config::Get_MAXINST() == 5000 &&
    FEXCore::Config::Get_SMCCHECKS() ==
      FEXCore::Config::CONFIG_SMC_MTRACK &&
    !FEXCore::Config::Get_MONOHACKS();
}

} // namespace

int main() {
  Receipt Status {};
  const char* Selector = std::getenv("LUMEN_MACOS_FEXCORE_RUN");
  if (Selector == nullptr || std::strcmp(Selector, "1") != 0) {
    Status.Code = ResultCode::SelectorDenied;
    Status.Reason = "selector-denied";
    return EmitReceipt(Status);
  }

  const long ObservedHostPageSize = sysconf(_SC_PAGESIZE);
  if (ObservedHostPageSize <= 0 ||
      static_cast<std::size_t>(ObservedHostPageSize) != 16384) {
    Status.Code = ResultCode::HostRejected;
    Status.Reason = "host-page-size";
    return EmitReceipt(Status);
  }
  Status.HostPageSize = static_cast<std::size_t>(ObservedHostPageSize);

  if (rpmalloc_initialize(nullptr) != 0) {
    Status.Code = ResultCode::AllocatorFailed;
    Status.Reason = "rpmalloc";
    return EmitReceipt(Status);
  }
  RpmallocFinalizer FinalizeRpmalloc;
  FEXCore::Allocator::InitializeThread();

  FEXCore::HostFeatures Features {};
  if (!BuildHostFeatures(&Features, &Status.LogicalCores)) {
    Status.Code = ResultCode::HostRejected;
    Status.Reason = "host-features";
    return EmitReceipt(Status);
  }

#ifndef MAP_JIT
  Status.Code = ResultCode::JitCapabilityFailed;
  Status.Reason = "map-jit-unavailable";
  return EmitReceipt(Status);
#else
  if (pthread_jit_write_protect_supported_np() == 0) {
    Status.Code = ResultCode::JitCapabilityFailed;
    Status.Reason = "thread-wx-unavailable";
    return EmitReceipt(Status);
  }
  pthread_jit_write_protect_np(1);
  Status.ThreadWx = 1;

  void* MapJit = mmap(nullptr, Status.HostPageSize,
                      PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
  if (MapJit == MAP_FAILED) {
    Status.Code = ResultCode::JitCapabilityFailed;
    Status.Reason = "map-jit-allocation";
    return EmitReceipt(Status);
  }
  MappedPage MapJitPage {MapJit, Status.HostPageSize};
  Status.MapJit = 1;
#endif

  FEXCore::Config::Initialize();
  ConfigFinalizer FinalizeConfig;
  FEXCore::Config::Load();
  if (!ConfigureFEX()) {
    Status.Code = ResultCode::ConfigurationFailed;
    Status.Reason = "fex-config";
    return EmitReceipt(Status);
  }

  constexpr std::array<std::uint8_t, 11> GuestCode {
    0x48, 0xB8, 0x78, 0x56, 0x34, 0x12, 0x00, 0x00, 0x00, 0x00,
    0xF4,
  };
  void* Guest = mmap(nullptr, Status.HostPageSize,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON, -1, 0);
  if (Guest == MAP_FAILED) {
    Status.Code = ResultCode::GuestMappingFailed;
    Status.Reason = "guest-map";
    return EmitReceipt(Status);
  }
  MappedPage GuestPage {Guest, Status.HostPageSize};
  std::memcpy(Guest, GuestCode.data(), GuestCode.size());
  if (mprotect(Guest, Status.HostPageSize, PROT_READ) != 0) {
    Status.Code = ResultCode::GuestMappingFailed;
    Status.Reason = "guest-protect";
    return EmitReceipt(Status);
  }
  const auto GuestStart = reinterpret_cast<std::uintptr_t>(Guest);

  FEXCore::SignalDelegator Signals {};
  LabSyscallHandler Syscalls {
    GuestStart, Status.HostPageSize, GuestStart, GuestCode.size()};
  const auto DeniedRange = Syscalls.QueryGuestExecutableRange(
    nullptr, GuestStart + GuestCode.size());
  if (!Syscalls.IsValid() || DeniedRange.Base != 0 ||
      DeniedRange.Size != 0 || DeniedRange.Writable) {
    CaptureAdapterReceipt(&Status, Syscalls.Snapshot(), 0, 0);
    Status.Code = ResultCode::AdapterFailed;
    Status.Reason = "adapter-range";
    return EmitReceipt(Status);
  }
  auto Context = FEXCore::Context::Context::CreateNewContext(Features);
  if (Context == nullptr) {
    Status.Code = ResultCode::ContextFailed;
    Status.Reason = "context-create";
    return EmitReceipt(Status);
  }
  Context->EnableExitOnHLT();
  Context->SetSignalDelegator(&Signals);
  Context->SetSyscallHandler(&Syscalls);
  {
    JitWriteScope WritableJit;
    if (!Context->InitCore()) {
      Status.Code = ResultCode::ContextFailed;
      Status.Reason = "context-init";
      return EmitReceipt(Status);
    }
  }
  Status.Context = 1;

  const auto LeafOne = Context->RunCPUIDFunction(1, 0);
  const std::uint32_t ReportedLogicalCores = (LeafOne.ebx >> 16) & 0xFFU;
  if (ReportedLogicalCores != Status.LogicalCores) {
    Status.Code = ResultCode::ConfigurationFailed;
    Status.Reason = "cpuid-topology";
    return EmitReceipt(Status);
  }

  FEXCore::Core::CPUState Seed {};
  Seed.rip = GuestStart;
  auto* Thread = Context->CreateThread(&Seed);
  if (Thread == nullptr) {
    Status.Code = ResultCode::GuestThreadFailed;
    Status.Reason = "thread-create";
    return EmitReceipt(Status);
  }
  ConfigureLongMode(Thread->CurrentFrame->State);
  FEXCore::Allocator::RegisterTLSData(Thread);
  const auto BeforeCompile = Syscalls.Snapshot();
  {
    JitWriteScope WritableJit;
    Context->CompileRIP(Thread, GuestStart);
  }
  const auto AfterCompile = Syscalls.Snapshot();
  const bool CompilePolicyPassed =
    BeforeCompile.PrecompileCalls == 0 &&
    AfterCompile.PrecompileCalls == 1 &&
    AfterCompile.SyscallCalls == 0 &&
    AfterCompile.QueryAllowed > 0 &&
    AfterCompile.QueryDenied == 1 &&
    AfterCompile.MarkAccepted > 0 &&
    AfterCompile.MarkRejected == 0 &&
    AfterCompile.Invalidates == 0 &&
    AfterCompile.LookupMisses > 0 &&
    AfterCompile.OvercommitMarks > 0 &&
    AfterCompile.SleepCalls == 0;
  if (!CompilePolicyPassed) {
    FEXCore::Allocator::UninstallTLSData(Thread);
    Context->DestroyThread(Thread);
    Context.reset();
    CaptureAdapterReceipt(&Status, Syscalls.Snapshot(), 0, 0);
    Status.Code = ResultCode::AdapterFailed;
    Status.Reason = "adapter-compile-policy";
    return EmitReceipt(Status);
  }

  // Execution deliberately happens with the per-thread write window closed.
  // Only the precompiled block may run in this bootstrap milestone.
  pthread_jit_write_protect_np(1);
  Context->ExecuteThread(Thread);
  const auto AfterExecute = Syscalls.Snapshot();
  FEXCore::Allocator::UninstallTLSData(Thread);

  Status.GuestResult =
    Thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_RAX];
  const std::uint64_t FinalRip = Thread->CurrentFrame->State.rip;
  Context->DestroyThread(Thread);
  Context.reset();

  const auto FinalCounters = Syscalls.Snapshot();
  const std::uint32_t ExecutePrecompileHookDelta =
    FinalCounters.PrecompileCalls >= AfterCompile.PrecompileCalls
      ? FinalCounters.PrecompileCalls - AfterCompile.PrecompileCalls
      : UINT32_MAX;
  const std::uint32_t ExecuteCompileMissDelta =
    FinalCounters.LookupMisses >= AfterCompile.LookupMisses
      ? FinalCounters.LookupMisses - AfterCompile.LookupMisses
      : UINT32_MAX;
  CaptureAdapterReceipt(
    &Status, FinalCounters,
    ExecutePrecompileHookDelta, ExecuteCompileMissDelta);
  if (ExecutePrecompileHookDelta != 1 || ExecuteCompileMissDelta != 0 ||
      AfterExecute.PrecompileCalls != AfterCompile.PrecompileCalls + 1 ||
      FinalCounters.PrecompileCalls != AfterExecute.PrecompileCalls ||
      AfterExecute.LookupMisses != AfterCompile.LookupMisses ||
      AfterExecute.QueryAllowed != AfterCompile.QueryAllowed ||
      AfterExecute.QueryDenied != AfterCompile.QueryDenied ||
      AfterExecute.MarkAccepted != AfterCompile.MarkAccepted ||
      AfterExecute.MarkRejected != AfterCompile.MarkRejected ||
      FinalCounters.LookupMisses != AfterExecute.LookupMisses ||
      FinalCounters.QueryAllowed != AfterExecute.QueryAllowed ||
      FinalCounters.QueryDenied != AfterExecute.QueryDenied ||
      FinalCounters.MarkAccepted != AfterExecute.MarkAccepted ||
      FinalCounters.MarkRejected != AfterExecute.MarkRejected ||
      FinalCounters.SyscallCalls != 0 ||
      FinalCounters.QueryAllowed == 0 || FinalCounters.QueryDenied != 1 ||
      FinalCounters.MarkAccepted == 0 || FinalCounters.MarkRejected != 0 ||
      FinalCounters.Invalidates != 0 || FinalCounters.LookupMisses == 0 ||
      FinalCounters.OvercommitMarks == 0 ||
      FinalCounters.OvercommitUnmarks != FinalCounters.OvercommitMarks ||
      FinalCounters.SleepCalls != 0) {
    Status.Code = ResultCode::AdapterFailed;
    Status.Reason = "adapter-execute-policy";
    return EmitReceipt(Status);
  }

  if (Status.GuestResult != kExpectedResult ||
      FinalRip != GuestStart + GuestCode.size() - 1) {
    Status.Code = ResultCode::GuestResultFailed;
    Status.Reason = "guest-result";
    return EmitReceipt(Status);
  }

  Status.GuestExecuted = 1;
  Status.BootstrapSmoke = 1;
  Status.Code = ResultCode::Success;
  Status.Reason = "ok";
  return EmitReceipt(Status);
}
