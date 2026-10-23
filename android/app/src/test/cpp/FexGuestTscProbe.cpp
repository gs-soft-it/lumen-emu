#include "AndroidFexCoreRuntime.h"

#include <array>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <sys/mman.h>
#include <thread>
#include <time.h>
#include <unistd.h>

#if defined(LUMEN_FEX_TSC_PROBE_DIRECT_LINK)
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Loader/Timer.h"
#endif

namespace {
constexpr unsigned Rsp = 4, Rdi = 7;
struct Sample {
  std::uint64_t wall, nativeCounter, kernelTsc, processUs, processCounter;
};
struct Output {
  std::uint32_t cpuid[4];
  std::uint64_t rdtscBefore, rdtscAfter, finalCounter;
};
struct Api {
  decltype(&LumenAndroidFexCoreRegisterHostCallTargetV1) registerTarget;
  decltype(&LumenAndroidFexCoreRootReturnAddressV1) rootReturn;
  decltype(&LumenAndroidFexCoreRunGuestV2) run;
  decltype(&LumenAndroidFexCoreBeginGuestMemoryMutationV1) beginMutation;
  decltype(&LumenAndroidFexCoreEndGuestMemoryMutationV2) endMutation;
  decltype(&LumenAndroidFexCoreQueryGuestTscV1) queryTsc;
  decltype(&LumenAndroidFexCoreRuntimeProbeV1) initialize;
  std::uint64_t (*tscFrequency)();
  std::uint64_t (*readTsc)();
  std::uint64_t (*processTime)();
  std::uint64_t (*processCounter)();
  std::uint64_t (*processFrequency)();
  void (*processStart)();
};
struct Context {
  Api api;
  std::uint64_t code, data, page, sentinel, target;
  unsigned calls;
  Sample before, after;
};

bool Check(bool value, const char *message) {
  if (!value)
    std::fprintf(stderr, "FexGuestTscProbe: %s\n", message);
  return value;
}
std::uint64_t NativeCounter() {
  std::uint64_t value;
  asm volatile("isb\n\tmrs %0,cntvct_el0" : "=r"(value));
  return value;
}
std::uint64_t NativeFrequency() {
  std::uint64_t value;
  asm volatile("mrs %0,cntfrq_el0" : "=r"(value));
  return value;
}
std::uint64_t Wall() {
  timespec time{};
  clock_gettime(CLOCK_MONOTONIC, &time);
  return static_cast<std::uint64_t>(time.tv_sec) * 1000000000 + time.tv_nsec;
}
Sample ReadSample(const Context &context) {
  return {Wall(), NativeCounter(), context.api.readTsc(),
          context.api.processTime(), context.api.processCounter()};
}

// Original owned fixture. CPUID 15h reports the guest TSC ratio, RDTSC
// brackets a host-call wait, and RDTSCP reads the same instruction clock on
// Android. The current Darwin adapter denies RDTSCP without a CPU-index
// source, so its gate explicitly uses a final RDTSC and excludes RDTSCP.
constexpr std::array<std::uint8_t, 74> Program{
    0xb8, 0x15, 0,    0,    0,    0x31, 0xc9, 0x0f, 0xa2, 0x89, 0x07, 0x89,
    0x5f, 0x04, 0x89, 0x4f, 0x08, 0x89, 0x57, 0x0c, 0x0f, 0x31, 0x48, 0xc1,
    0xe2, 0x20, 0x48, 0x09, 0xd0, 0x48, 0x89, 0x47, 0x10, 0x41, 0xff, 0xd4,
    0x0f, 0x31, 0x48, 0xc1, 0xe2, 0x20, 0x48, 0x09, 0xd0, 0x48, 0x89, 0x47,
    0x18,
#if defined(__APPLE__)
    0x0f, 0x31, 0x90,
#else
    0x0f, 0x01, 0xf9,
#endif
    0x48, 0xc1, 0xe2, 0x20, 0x48, 0x09, 0xd0, 0x48, 0x89, 0x47, 0x20, 0xc3,
};
void Identity() {}
std::uint32_t Query(void *opaque, std::uint64_t address, std::uint64_t *base,
                    std::uint64_t *size, std::uint32_t *writable) noexcept {
  const auto &context = *static_cast<const Context *>(opaque);
  if (address < context.code || address - context.code >= context.page)
    return 0;
  *base = context.code;
  *size = context.page;
  *writable = 0;
  return 1;
}
std::uint32_t Callback(void *opaque, std::uint64_t target, std::uint64_t *gprs,
                       LumenAndroidFexCoreSimdViewV2 *,
                       std::uint64_t *next) noexcept {
  auto &context = *static_cast<Context *>(opaque);
  if (target != context.target || context.calls++ != 0 ||
      gprs[Rsp] < context.data ||
      gprs[Rsp] - context.data > context.page * 2 - 8)
    return 0;
  std::uint64_t rip;
  std::memcpy(&rip, reinterpret_cast<const void *>(gprs[Rsp]), sizeof(rip));
  if (rip < context.code || rip - context.code >= context.page)
    return 0;
  context.before = ReadSample(context);
  timespec requested{0, 200000000};
  while (nanosleep(&requested, &requested) != 0 && errno == EINTR) {
  }
  context.after = ReadSample(context);
  gprs[Rsp] += 8;
  *next = rip;
  return LUMEN_ANDROID_FEXCORE_HOST_CALL_CONTINUE_V1;
}

bool QueryControls(const Api &api) {
  using State = LumenAndroidFexCoreGuestTscV1;
  State output{};
  output.counter = 0xdead;
  output.frequency = 0xbeef;
  bool passed =
      Check(api.queryTsc(&output) ==
                    LUMEN_ANDROID_FEXCORE_RUNTIME_CONTEXT_FAILED_V1 &&
                output.counter == 0 && output.frequency == 0 &&
                output.nativeFrequency == 0,
            "query initialized runtime or published a clock before readiness");
  passed = Check(api.queryTsc(nullptr) ==
                     LUMEN_ANDROID_FEXCORE_RUNTIME_INVALID_ARGUMENT_V1,
                 "null query accepted") &&
           passed;
  for (unsigned test = 0; test != 3; ++test) {
    State invalid{};
    if (test == 0)
      invalid.structSize--;
    if (test == 1)
      invalid.abiVersion++;
    if (test == 2)
      invalid.reserved = 1;
    invalid.counter = 0xdead;
    const auto original = invalid;
    passed = Check(api.queryTsc(&invalid) ==
                           LUMEN_ANDROID_FEXCORE_RUNTIME_INVALID_ARGUMENT_V1 &&
                       std::memcmp(&invalid, &original, sizeof(invalid)) == 0,
                   "invalid query mutated output or accepted ABI") &&
             passed;
  }
  std::printf("TSC_QUERY_CONTROLS cases=5 passed=%u\n", passed);
  return passed;
}
bool HostThreadQuery(const Api &api, std::uint64_t frequency) {
  bool passed = true;
  std::thread host([&] {
    std::uint64_t previous = 0;
    for (unsigned test = 0; test != 64; ++test) {
      LumenAndroidFexCoreGuestTscV1 output{};
      const auto before = NativeCounter();
      const auto code = api.queryTsc(&output);
      const auto after = NativeCounter();
      const bool controls = code == 0 && output.scale <= 31 &&
                            output.frequency == frequency &&
                            output.nativeFrequency == NativeFrequency();
      const bool domain = controls &&
                          output.counter >= (before << output.scale) &&
                          output.counter <= (after << output.scale) &&
                          output.counter >= previous;
      passed = domain && passed;
      previous = output.counter;
    }
  });
  host.join();
  std::printf("TSC_HOST_THREAD queries=64 originAgreement=%u\n", passed);
  return Check(passed,
               "host thread requires root TLS or changed CNTVCT origin");
}
std::uint64_t ToNs(std::uint64_t ticks, std::uint64_t frequency) {
  return static_cast<std::uint64_t>(static_cast<unsigned __int128>(ticks) *
                                    1000000000 / frequency);
}
bool CloseEnough(std::uint64_t measured, std::uint64_t reference) {
  const auto difference =
      measured > reference ? measured - reference : reference - measured;
  return reference != 0 && difference <= reference / 20 + 100000;
}
} // namespace

int main(int argc, char **argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  alarm(30);
  Context context{};
#if defined(LUMEN_FEX_TSC_PROBE_DIRECT_LINK)
  if (argc != 1)
    return 2;
  (void)argv;
  namespace Kernel = Lumen::Libs::LibKernel;
  context.api = {&LumenAndroidFexCoreRegisterHostCallTargetV1,
                 &LumenAndroidFexCoreRootReturnAddressV1,
                 &LumenAndroidFexCoreRunGuestV2,
                 &LumenAndroidFexCoreBeginGuestMemoryMutationV1,
                 &LumenAndroidFexCoreEndGuestMemoryMutationV2,
                 &LumenAndroidFexCoreQueryGuestTscV1,
                 &LumenAndroidFexCoreRuntimeProbeV1,
                 &Kernel::KernelGetTscFrequency,
                 &Kernel::KernelReadTsc,
                 &Kernel::KernelGetProcessTime,
                 &Kernel::KernelGetProcessTimeCounter,
                 &Kernel::KernelGetProcessTimeCounterFrequency,
                 &Lumen::Loader::Timer::Start};
#else
  if (argc != 2)
    return 2;
  void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!library) {
    std::fprintf(stderr, "dlopen: %s\n", dlerror());
    return 2;
  }
  auto symbol = [&]<class T>(const char *name) {
    auto value = reinterpret_cast<T>(dlsym(library, name));
    if (!value)
      std::fprintf(stderr, "missing %s\n", name);
    return value;
  };
#define LOAD(field, name)                                                      \
  context.api.field =                                                          \
      symbol.template operator()<decltype(context.api.field)>(name)
  LOAD(registerTarget, "LumenAndroidFexCoreRegisterHostCallTargetV1");
  LOAD(rootReturn, "LumenAndroidFexCoreRootReturnAddressV1");
  LOAD(run, "LumenAndroidFexCoreRunGuestV2");
  LOAD(beginMutation, "LumenAndroidFexCoreBeginGuestMemoryMutationV1");
  LOAD(endMutation, "LumenAndroidFexCoreEndGuestMemoryMutationV2");
  LOAD(queryTsc, "LumenAndroidFexCoreQueryGuestTscV1");
  LOAD(initialize, "LumenAndroidFexCoreRuntimeProbeV1");
  LOAD(tscFrequency, "_ZN5Lumen4Libs9LibKernel21KernelGetTscFrequencyEv");
  LOAD(readTsc, "_ZN5Lumen4Libs9LibKernel13KernelReadTscEv");
  LOAD(processTime, "_ZN5Lumen4Libs9LibKernel20KernelGetProcessTimeEv");
  LOAD(processCounter,
       "_ZN5Lumen4Libs9LibKernel27KernelGetProcessTimeCounterEv");
  LOAD(processFrequency,
       "_ZN5Lumen4Libs9LibKernel36KernelGetProcessTimeCounterFrequencyEv");
  LOAD(processStart, "_ZN5Lumen6Loader5Timer5StartEv");
#undef LOAD
  if (!context.api.registerTarget || !context.api.rootReturn ||
      !context.api.run || !context.api.beginMutation ||
      !context.api.endMutation || !context.api.queryTsc ||
      !context.api.initialize || !context.api.tscFrequency ||
      !context.api.readTsc || !context.api.processTime ||
      !context.api.processCounter || !context.api.processFrequency ||
      !context.api.processStart)
    return 2;
#endif
  if (!QueryControls(context.api))
    return 1;
  if (context.api.initialize(nullptr) != 0)
    return 2;
  context.api.processStart();
  context.page = sysconf(_SC_PAGESIZE);
  context.sentinel = context.api.rootReturn();
  context.target = reinterpret_cast<std::uint64_t>(&Identity);
  if (!context.sentinel || !context.api.registerTarget(context.target))
    return 2;
  void *code = mmap(nullptr, context.page, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  void *data = mmap(nullptr, context.page * 2, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (code == MAP_FAILED || data == MAP_FAILED)
    return 2;
  context.code = reinterpret_cast<std::uint64_t>(code);
  context.data = reinterpret_cast<std::uint64_t>(data);
  std::memcpy(code, Program.data(), Program.size());
  if (mprotect(code, context.page, PROT_READ) != 0)
    return 2;
  const auto nativeFrequency = NativeFrequency();
  const auto tscFrequency = context.api.tscFrequency();
  const auto processFrequency = context.api.processFrequency();
#if defined(__APPLE__)
  std::printf(
      "TSC_INSTRUCTION_SCOPE finalInstruction=RDTSC rdtscpQualified=0\n");
#else
  std::printf(
      "TSC_INSTRUCTION_SCOPE finalInstruction=RDTSCP rdtscpQualified=1\n");
#endif
  bool passed = HostThreadQuery(context.api, tscFrequency);
  unsigned executed = 0, mismatches = 0;
  for (unsigned test = 0; test != 4; ++test) {
    context.calls = 0;
    std::memset(data, 0, sizeof(Output));
    const auto stack = context.data + context.page * 2 - 8;
    std::memcpy(reinterpret_cast<void *>(stack), &context.sentinel, 8);
    LumenAndroidFexCoreGuestStateV1 state{
        sizeof(state), 1, context.code, {}, 0, 0, 0, 0, 0};
    state.gprs[Rsp] = stack;
    state.gprs[Rdi] = context.data;
    state.gprs[12] = context.target;
#if defined(__APPLE__) && defined(LUMEN_FEX_TSC_PROBE_DIRECT_LINK)
    LumenAndroidFexCoreGuestStateV3 root{};
    root.gp = state;
    const auto result = LumenMacOSFexCoreRunGuestWithOwnedStackV3(
        &root, &Callback, &Query, &context, context.data, context.page * 2,
        +[](std::uint64_t address, std::uint64_t length, std::uint32_t write,
            std::uint64_t begin, std::uint64_t size) noexcept -> std::uint32_t {
          return write <= 1 && length != 0 && address >= begin &&
                         address - begin < size &&
                         length <= size - (address - begin)
                     ? 1U
                     : 0U;
        });
#else
    const auto result = context.api.run(&state, &Callback, &Query, &context);
#endif
    const auto &output = *static_cast<const Output *>(data);
    if (result != 0 || context.calls != 1 || !output.cpuid[0]) {
      std::printf("EXEC_FAIL code=%u calls=%u leaf=%u/%u/%u\n", result,
                  context.calls, output.cpuid[0], output.cpuid[1],
                  output.cpuid[2]);
      passed = false;
      break;
    }
    ++executed;
    const auto declared = static_cast<std::uint64_t>(output.cpuid[2]) *
                          output.cpuid[1] / output.cpuid[0];
    const auto wall = context.after.wall - context.before.wall;
    const auto rdtsc = output.rdtscAfter - output.rdtscBefore;
    const auto kernel = context.after.kernelTsc - context.before.kernelTsc;
    const auto processUs = context.after.processUs - context.before.processUs;
    const auto processCounter =
        context.after.processCounter - context.before.processCounter;
    const bool frequencyAgreement = declared == tscFrequency;
    const bool originAgreement =
        output.rdtscBefore <= context.before.kernelTsc &&
        context.after.kernelTsc <= output.rdtscAfter &&
        output.rdtscAfter <= output.finalCounter;
    const bool timeAgreement =
        CloseEnough(ToNs(rdtsc, declared), wall) &&
        CloseEnough(ToNs(kernel, tscFrequency), wall) &&
        CloseEnough(ToNs(processCounter, processFrequency), wall) &&
        CloseEnough(processUs * 1000, wall);
    mismatches += !frequencyAgreement;
    passed = frequencyAgreement && originAgreement && timeAgreement && passed;
    std::printf("CLOCK_SAMPLE index=%u nativeFrequency=%" PRIu64
                " cpuidFrequency=%" PRIu64 " kernelTscFrequency=%" PRIu64
                " processFrequency=%" PRIu64 " wallNs=%" PRIu64
                " rdtscDelta=%" PRIu64 " finalAfterDelta=%" PRIu64
                " nativeDelta=%" PRIu64 " kernelTscDelta=%" PRIu64
                " processUsDelta=%" PRIu64 " processCounterDelta=%" PRIu64
                " rdtscNsAtCpuid=%" PRIu64 " rdtscNsAtKernel=%" PRIu64
                " frequencyAgreement=%u originAgreement=%u timeAgreement=%u\n",
                test, nativeFrequency, declared, tscFrequency, processFrequency,
                wall, rdtsc, output.finalCounter - output.rdtscAfter,
                context.after.nativeCounter - context.before.nativeCounter,
                kernel, processUs, processCounter, ToNs(rdtsc, declared),
                ToNs(rdtsc, tscFrequency), frequencyAgreement, originAgreement,
                timeAgreement);
  }
  const auto token = context.api.beginMutation();
  bool closed = false;
  struct Close {
    Context *context;
    bool *closed;
  } close{&context, &closed};
  const auto ended =
      token ? context.api.endMutation(
                  token, context.code, context.page, 1,
                  +[](void *opaque) noexcept {
                    auto &value = *static_cast<Close *>(opaque);
                    *value.closed =
                        munmap(reinterpret_cast<void *>(value.context->code),
                               value.context->page) == 0 &&
                        munmap(reinterpret_cast<void *>(value.context->data),
                               value.context->page * 2) == 0;
                  },
                  &close)
            : 0;
  passed = passed && executed == 4 && ended && closed;
  std::printf("LUMEN_GUEST_TSC_CLOCK_PROBE executed=%u frequencyMismatches=%u "
              "mappingsClosed=%u passed=%u\n",
              executed, mismatches, ended && closed, passed);
  return passed ? 0 : 1;
}
