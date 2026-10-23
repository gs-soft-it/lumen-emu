#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"
#include "Emulator/Config.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/VideoOut.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
namespace Video = Lumen::Libs::VideoOut;
namespace Eq = Lumen::Libs::LibKernel::EventQueue;
namespace Memory = Lumen::Core::VirtualMemory;
namespace Loader = Lumen::Loader;
constexpr uint64_t payload_max = UINT64_C(0x0000ffffffffffff);

bool Check(bool value, const char* message)
{
	if (!value) std::fprintf(stderr, "VideoOutEventDataTest: %s\n", message);
	return value;
}

struct Mapping
{
	uint64_t address;
	size_t size;
	explicit Mapping(size_t bytes): address(Memory::Alloc(0, bytes, Memory::Mode::ReadWrite)), size(bytes) {}
	~Mapping() { if (address != 0) Memory::Free(address); }
	uint8_t* Bytes(size_t offset = 0) const { return reinterpret_cast<uint8_t*>(address + offset); }
};

Loader::SymbolResolve Request()
{
	Loader::SymbolResolve request {};
	request.name = "rWUTcKdkUzQ";
	request.library = "libSceVideoOut";
	request.library_version = 1;
	request.module = "libSceVideoOut";
	request.module_version_major = 0;
	request.module_version_minor = 0;
	request.type = Loader::SymbolType::Func;
	return request;
}

bool BindingAndEncoder(bool enabled, Loader::SymbolDatabase& symbols)
{
	if (!Check(Lumen::Libs::Init(U"libVideoOut_1", &symbols), "binding initialization failed")) return false;
	symbols.SealHostCallRegistry();
	const auto* record = symbols.Find(Request());
	if (!Check(Video::VideoOutEventDataEnabled() == enabled && (record != nullptr) == enabled,
	           "selector and binding diverged")) return false;
	if (enabled)
	{
		const auto* metadata = symbols.HostCallScalarMetadataFor(record->vaddr);
		if (!Check(record->vaddr == reinterpret_cast<uint64_t>(&Video::VideoOutGetEventData) && metadata != nullptr &&
		           metadata->arity == 2 && metadata->arguments[0] == Loader::HostCallScalarType::Gp &&
		           metadata->arguments[1] == Loader::HostCallScalarType::Gp && metadata->result == Loader::HostCallScalarType::Gp &&
		           metadata->invoker != nullptr && symbols.ClassifyHostCallTarget(record->vaddr) == Loader::HostCallRegistryClassification::Host,
		           "getter fixed scalar metadata differs")) return false;
	}
	for (unsigned mismatch = 0; mismatch < 4; ++mismatch)
	{
		auto other = Request();
		if (mismatch == 0) other.module_version_major = 1;
		if (mismatch == 1) other.library = "UnknownVideoOut";
		if (mismatch == 2) other.module = "UnknownVideoOut";
		if (mismatch == 3) other.type = Loader::SymbolType::Object;
		if (!Check(symbols.Find(other) == nullptr, "getter acquired a namespace/type fallback")) return false;
	}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
	if (_putenv_s("LUMEN_EXPERIMENTAL_VIDEOOUT_EVENT_DATA", enabled ? "0" : "1") != 0) return false;
#else
	if (setenv("LUMEN_EXPERIMENTAL_VIDEOOUT_EVENT_DATA", enabled ? "0" : "1", 1) != 0) return false;
#endif
	Loader::SymbolDatabase second;
	if (!Lumen::Libs::Init(U"libVideoOut_1", &second) || (second.Find(Request()) != nullptr) != enabled) return false;
	for (const uint64_t payload: std::array<uint64_t, 8>{0, 1, UINT64_C(0x123480000001), payload_max,
	                                                   payload_max + 1, UINT64_C(1) << 63u, UINT64_MAX - 1, UINT64_MAX})
	{
		uint64_t encoded = 17;
		const auto result = Video::VideoOutEncodeEventPayload(payload, encoded);
		const bool accepted = !enabled || payload <= payload_max;
		if (!Check(accepted ? (result == (enabled ? Video::VideoOutEventPayloadResult::Encoded : Video::VideoOutEventPayloadResult::LegacyRaw) &&
		                                 encoded == (enabled ? payload << 16u : payload)) :
		                                (result == Video::VideoOutEventPayloadResult::OutOfRange && encoded == 17),
		           "payload encoding changed selector, truncated data or modified a rejected output")) return false;
	}
	return Video::VideoOutEventDataEnabled() == enabled && Video::VideoOutEventDataProducerContractTest();
}

bool DecoderAndMemory(size_t page, const Loader::SymbolDatabase& symbols)
{
	Mapping memory(page * 3);
	if (!Check(memory.address != 0, "memory fixture allocation failed")) return false;
	std::vector<uint8_t> original(memory.size);
	for (size_t i = 0; i < original.size(); ++i) original[i] = static_cast<uint8_t>(i * 37 + 11);
	const auto* metadata = symbols.HostCallScalarMetadataFor(reinterpret_cast<uint64_t>(&Video::VideoOutGetEventData));
	const auto invoke = [&](uint64_t input, uint64_t output) {
		const uint64_t arguments[] = {input, output};
		return static_cast<int32_t>(static_cast<uint32_t>(metadata->invoker(arguments)));
	};
	const uint64_t input = memory.address + 64, output = memory.address + page + 64;
	const std::array<uint64_t, 11> data {0, 1, 0xffff, 0x10000, 0x10001, 0xffffffff, UINT64_C(0x100000000),
	    UINT64_C(0x0123456789abcdef), UINT64_C(0x7fffffffffffffff), UINT64_C(0x8000000000000000), UINT64_MAX};
	for (const bool native_shape: {false, true}) for (const uint64_t bits: data)
	{
		Eq::KernelEvent event {};
		event.ident = native_shape ? UINT64_C(0x0007000000000000) : 0;
		event.filter = -13;
		event.flags = native_shape ? 0x20 : 0;
		event.fflags = native_shape ? 0 : 1;
		std::memcpy(&event.data, &bits, sizeof(bits));
		for (const uint64_t seed: {UINT64_C(0x0123456789abcdef), UINT64_C(0xfedcba9876543210)})
		{
			std::memcpy(memory.Bytes(), original.data(), original.size());
			std::memcpy(memory.Bytes(64), &event, sizeof(event));
			std::memcpy(memory.Bytes(page + 64), &seed, sizeof(seed));
			std::vector<uint8_t> expected(memory.Bytes(), memory.Bytes() + memory.size);
			const uint64_t result = bits >> 16u;
			std::memcpy(expected.data() + page + 64, &result, sizeof(result));
			if (!Check(invoke(input, output) == 0 && std::memcmp(memory.Bytes(), expected.data(), expected.size()) == 0,
			           "native matrix decode, whole8 store or surrounding-byte immutability differs")) return false;
		}
	}
	const auto unchanged_call = [&](uint64_t source, uint64_t destination, int expected) {
		std::vector<uint8_t> before(memory.Bytes(), memory.Bytes() + memory.size);
		return invoke(source, destination) == expected && std::memcmp(memory.Bytes(), before.data(), before.size()) == 0;
	};
	for (const int16_t filter: {int16_t(0), int16_t(-14)})
	{
		std::memcpy(memory.Bytes(64 + offsetof(Eq::KernelEvent, filter)), &filter, sizeof(filter));
		if (!Check(unchanged_call(input, output, Video::VIDEO_OUT_ERROR_INVALID_EVENT), "invalid filter modified memory")) return false;
	}
	const int16_t valid_filter = -13;
	std::memcpy(memory.Bytes(64 + offsetof(Eq::KernelEvent, filter)), &valid_filter, sizeof(valid_filter));
	for (const auto pair: std::array<std::array<uint64_t, 2>, 7>{{{0, output}, {input, 0}, {input + 1, output},
	         {input, output + 1}, {UINT64_MAX - 7, output}, {input, UINT64_MAX - 7}, {input, input + 16}}})
		if (!Check(unchanged_call(pair[0], pair[1], Video::VIDEO_OUT_ERROR_INVALID_ADDRESS), "defensive shape error modified memory")) return false;
	if (!Memory::Protect(memory.address, page, Memory::Mode::Read)) return false;
	const bool read_only_input = invoke(input, output) == 0;
	if (!Memory::Protect(memory.address, page, Memory::Mode::ReadWrite) || !Check(read_only_input, "read-only input was rejected")) return false;
	if (!Memory::Protect(memory.address + page, page, Memory::Mode::Read)) return false;
	const bool read_only_output = unchanged_call(input, output, Video::VIDEO_OUT_ERROR_INVALID_ADDRESS);
	if (!Memory::Protect(memory.address + page, page, Memory::Mode::ReadWrite) || !Check(read_only_output, "read-only output was modified")) return false;
	std::vector<uint8_t> before(memory.Bytes(), memory.Bytes() + memory.size);
	if (!Memory::Protect(memory.address + page * 2, page, Memory::Mode::NoAccess)) return false;
	const bool guarded = invoke(memory.address + page * 2 - 16, output) == Video::VIDEO_OUT_ERROR_INVALID_ADDRESS &&
	                     invoke(input, memory.address + page * 2) == Video::VIDEO_OUT_ERROR_INVALID_ADDRESS &&
	                     invoke(input, memory.address + page * 2 - 4) == Video::VIDEO_OUT_ERROR_INVALID_ADDRESS;
	if (!Memory::Protect(memory.address + page * 2, page, Memory::Mode::ReadWrite)) return false;
	return Check(guarded && std::memcmp(memory.Bytes(), before.data(), before.size()) == 0,
	             "guard-crossing input/output wrote a prefix or changed surrounding bytes");
}

bool EarlyAdmissionRejection()
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	// Android's fatal formatter writes its reason to logcat, not the pipe used
	// below. Numeric, queue and memory contracts still run; this child-output
	// qualification needs a separate device/logcat harness.
	std::puts("VideoOutEventDataTest: SKIP fatal diagnostic capture (Android logcat harness required)");
	return true;
#elif LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
	// The typed range checks above run on every target. The POSIX child below
	// additionally observes fatal admission through real render entry points.
	std::puts("VideoOutEventDataTest: SKIP fatal diagnostic capture (POSIX child harness unavailable)");
	return true;
#else
	for (unsigned kind = 0; kind < 3; ++kind)
	{
		int pipefd[2];
		if (pipe(pipefd) != 0) return false;
		std::fflush(nullptr);
		const pid_t child = fork();
		if (child == 0)
		{
			close(pipefd[0]);
			dup2(pipefd[1], STDERR_FILENO);
			dup2(pipefd[1], STDOUT_FILENO);
			close(pipefd[1]);
			using namespace Lumen::Libs::Graphics;
			if (kind == 0) GraphicsRenderWriteAtEndOfPipeOnlyFlip(0, nullptr, 1, 0, 1, -1);
			if (kind == 1) GraphicsRenderWriteAtEndOfPipeWithFlip32(0, nullptr, nullptr, 0, 1, 0, 1, INT64_MAX);
			if (kind == 2) GraphicsRenderWriteAtEndOfPipeWithInterruptWriteBackFlip32(0, nullptr, nullptr, 0, 1, 0, 1,
			                                                                    static_cast<int64_t>(payload_max + 1), 64);
			_exit(0);
		}
		close(pipefd[1]);
		if (child < 0) { close(pipefd[0]); return false; }
		std::string diagnostic;
		char buffer[512];
		ssize_t count = 0;
		while ((count = read(pipefd[0], buffer, sizeof(buffer))) > 0)
			if (diagnostic.size() < 8192) diagnostic.append(buffer, static_cast<size_t>(count));
		close(pipefd[0]);
		int status = 0;
		if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) == 0 ||
		    diagnostic.find("VideoOut event payload rejected: experimental unsigned48") == std::string::npos) return false;
	}
	return true;
#endif
}
} // namespace

int main(int argc, char** argv)
{
	if (argc != 2 || (std::strcmp(argv[1], "--enabled") != 0 && std::strcmp(argv[1], "--disabled") != 0)) return 2;
	const bool enabled = std::strcmp(argv[1], "--enabled") == 0;
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	Memory::Init();
#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
	SYSTEM_INFO info {};
	GetSystemInfo(&info);
	const size_t page = info.dwPageSize;
#else
	const long page_value = sysconf(_SC_PAGESIZE);
	if (page_value <= 0) return 1;
	const size_t page = static_cast<size_t>(page_value);
#endif
	Loader::SymbolDatabase symbols;
	if (!BindingAndEncoder(enabled, symbols) || (enabled && (!DecoderAndMemory(page, symbols) || !EarlyAdmissionRejection()))) return 1;
	std::puts("VideoOutEventDataTest: PASS");
	return 0;
}
