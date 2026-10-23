#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"
#include "Emulator/Config.h"
#include "Emulator/Graphics/VideoOut.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Log.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unistd.h>
#include <vector>

namespace {
namespace Video = Lumen::Libs::VideoOut;
namespace Memory = Lumen::Core::VirtualMemory;
constexpr int INVALID_ADDRESS = Video::VIDEO_OUT_ERROR_INVALID_ADDRESS;
constexpr int INVALID_HANDLE = Video::VIDEO_OUT_ERROR_INVALID_HANDLE;
constexpr int UNSUPPORTED = Video::VIDEO_OUT_ERROR_UNSUPPORTED_OPERATION;

bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "VideoOutConfigureAnySdrTest: %s\n", message);
	return condition;
}

class Mapping
{
public:
	explicit Mapping(size_t size): m_size(size), m_address(Memory::Alloc(0, size, Memory::Mode::ReadWrite)) {}
	~Mapping() { if (m_address != 0) Memory::Free(m_address); }
	Mapping(const Mapping&) = delete;
	Mapping& operator=(const Mapping&) = delete;
	bool Valid() const { return m_address != 0; }
	uint8_t* Data(size_t offset = 0) const { return reinterpret_cast<uint8_t*>(m_address + offset); }
	uint64_t Address() const { return m_address; }
	bool Protect(size_t offset, size_t size, Memory::Mode mode)
	{
		return Valid() && offset <= m_size && size <= m_size - offset && Memory::Protect(m_address + offset, size, mode);
	}
	bool Free()
	{
		if (!Valid() || !Memory::Free(m_address)) return false;
		m_address = 0;
		return true;
	}
private:
	size_t m_size;
	uint64_t m_address;
};

std::array<uint8_t, 32> AnyMode()
{
	std::array<uint8_t, 32> record;
	record.fill(0xff);
	const uint32_t size = 32;
	std::memcpy(record.data(), &size, sizeof(size));
	return record;
}

bool RecordValidation()
{
	const auto original = AnyMode();
	if (!Check(Video::VideoOutMatchesAnySdrMode(original), "the all-any snapshot was rejected")) return false;
	for (size_t index = 0; index != original.size(); ++index)
	{
		auto changed = original;
		changed[index] ^= 1;
		if (!Check(!Video::VideoOutMatchesAnySdrMode(changed), "a changed snapshot byte was accepted")) return false;
	}
	for (uint32_t prefix: {0u, 4u, 31u, 33u, std::numeric_limits<uint32_t>::max()})
	{
		auto changed = original;
		std::memcpy(changed.data(), &prefix, sizeof(prefix));
		if (!Check(!Video::VideoOutMatchesAnySdrMode(changed), "an unsupported size prefix was accepted")) return false;
	}
	return true;
}

bool ShapeChecks(int handle, bool enabled)
{
	const auto* inaccessible = reinterpret_cast<const void*>(uint64_t(1));
	for (uint32_t options_size: {0u, 16u})
		if (!Check(Video::VideoOutConfigureOutputMode(handle, 0, inaccessible, nullptr, 32, options_size) ==
		               (enabled ? INVALID_ADDRESS : UNSUPPORTED),
		           "accepted shape did not preserve the selector's memory-access policy")) return false;
	for (uint32_t reserved: {1u, std::numeric_limits<uint32_t>::max()})
		if (!Check(Video::VideoOutConfigureOutputMode(handle, reserved, inaccessible, nullptr, 32, 0) == UNSUPPORTED,
		           "reserved argument was accepted or the record was read first")) return false;
	for (uint32_t size: {0u, 4u, 31u, 33u, std::numeric_limits<uint32_t>::max()})
		if (!Check(Video::VideoOutConfigureOutputMode(handle, 0, inaccessible, nullptr, size, 0) == UNSUPPORTED,
		           "unsupported mode size was accepted or inspected")) return false;
	for (uint32_t size: {1u, 15u, 17u, std::numeric_limits<uint32_t>::max()})
		if (!Check(Video::VideoOutConfigureOutputMode(handle, 0, inaccessible, nullptr, 32, size) == UNSUPPORTED,
		           "unsupported options size was accepted or the record was read first")) return false;
	return Check(Video::VideoOutConfigureOutputMode(handle, 0, inaccessible, inaccessible, 32, 16) == UNSUPPORTED,
	             "non-null opaque options were accepted or inspected");
}

bool InputGuards(int handle, bool enabled, size_t page_size)
{
	Mapping mapping(page_size * 2);
	if (!Check(mapping.Valid(), "input fixture allocation failed")) return false;
	std::vector<uint8_t> original(page_size * 2, 0x6d);
	const auto mode = AnyMode();
	std::memcpy(original.data() + 19, mode.data(), mode.size());
	std::memcpy(original.data() + page_size - mode.size(), mode.data(), mode.size());
	std::memcpy(mapping.Data(), original.data(), original.size());
	const auto invoke = [&](const void* input, int expected)
	{
		for (uint32_t options_size: {0u, 16u})
			if (!Check(Video::VideoOutConfigureOutputMode(handle, 0, input, nullptr, 32, options_size) == expected,
			           "input guard or missing-presenter rejection was wrong")) return false;
		return true;
	};
	// This fixture never creates a presenter; even a recognized record remains
	// unsupported here. The record matcher and the real-presenter gate are separate.
	if (!invoke(mapping.Data(19), UNSUPPORTED) ||
	    !Check(mapping.Protect(0, page_size * 2, Memory::Mode::Read), "read-only protection failed") ||
	    !invoke(mapping.Data(19), UNSUPPORTED) ||
	    !Check(std::memcmp(mapping.Data(), original.data(), original.size()) == 0, "configuration changed its input") ||
	    !Check(mapping.Protect(page_size, page_size, Memory::Mode::NoAccess), "guard protection failed") ||
	    !invoke(mapping.Data(page_size - mode.size()), UNSUPPORTED)) return false;
	const int unreadable = enabled ? INVALID_ADDRESS : UNSUPPORTED;
	for (size_t prefix: {size_t(1), size_t(4), size_t(31)})
		if (!invoke(mapping.Data(page_size - prefix), unreadable)) return false;
	if (!invoke(mapping.Data(page_size), unreadable) ||
	    !Check(mapping.Protect(0, page_size, Memory::Mode::NoAccess), "full no-access protection failed") ||
	    !invoke(mapping.Data(19), unreadable) ||
	    !Check(Video::VideoOutConfigureOutputMode(-1, 0, mapping.Data(), nullptr, 32, 0) == INVALID_HANDLE,
	           "invalid handle did not take precedence over unreadable input") ||
	    !Check(mapping.Protect(0, page_size * 2, Memory::Mode::ReadWrite), "input protection restore failed") ||
	    !Check(std::memcmp(mapping.Data(), original.data(), original.size()) == 0, "guarded inputs changed")) return false;
	// A changed all-any record must remain unsupported, with every input byte preserved.
	mapping.Data(19)[7] ^= 1;
	original[26] ^= 1;
	if (!invoke(mapping.Data(19), UNSUPPORTED) ||
	    !Check(std::memcmp(mapping.Data(), original.data(), original.size()) == 0, "unsupported record changed")) return false;
	const uint64_t freed = mapping.Address();
	if (!Check(mapping.Free(), "input unmap failed")) return false;
	for (uint64_t address: {uint64_t(0), uint64_t(1), freed, std::numeric_limits<uint64_t>::max() - 15})
		if (!invoke(reinterpret_cast<const void*>(address), unreadable)) return false;
	return true;
}

bool SelectorAndWrapper(bool enabled, size_t page_size)
{
	if (!Check(Video::VideoOutConfigureAnySdrEnabled() == enabled, "selector parsing disagrees with the expected profile") ||
	    !Check(setenv("LUMEN_EXPERIMENTAL_VIDEOOUT_CONFIGURE_ANY_SDR", enabled ? "0" : "1", 1) == 0,
	           "selector environment mutation failed") ||
	    !Check(Video::VideoOutConfigureAnySdrEnabled() == enabled, "selector was not latched for the process") ||
	    !Check(Video::VideoOutConfigureOutputMode(1, 0, nullptr, nullptr, 32, 0) == INVALID_HANDLE,
	           "missing VideoOut context was accepted")) return false;
	Video::VideoOutInit(1280, 720);
	for (int handle: {-1, 0, 1, std::numeric_limits<int>::max()})
		if (!Check(Video::VideoOutConfigureOutputMode(handle, 0, nullptr, nullptr, 32, 0) == INVALID_HANDLE,
		           "unopened handle was accepted")) return false;
	const int handle = Video::VideoOutOpen(255, 0, 0, nullptr);
	if (!Check(handle == 1, "port open failed") || !ShapeChecks(handle, enabled) || !InputGuards(handle, enabled, page_size) ||
	    !Check(Video::VideoOutClose(handle) == 0, "port close failed") ||
	    !Check(Video::VideoOutConfigureOutputMode(handle, 0, nullptr, nullptr, 32, 0) == INVALID_HANDLE,
	           "closed handle was accepted")) return false;
	const int reopened = Video::VideoOutOpen(255, 0, 0, nullptr);
	return Check(reopened == handle, "port reopen failed") && ShapeChecks(reopened, enabled) &&
	       Check(Video::VideoOutClose(reopened) == 0, "reopened port close failed");
}
} // namespace

int main(int argc, char** argv)
{
	if (argc != 2) return 2;
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	Memory::Init();
	const long page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0) return 1;
	bool passed = false;
	if (std::strcmp(argv[1], "--record") == 0) passed = RecordValidation();
	else if (std::strcmp(argv[1], "--enabled") == 0 || std::strcmp(argv[1], "--disabled") == 0)
		passed = SelectorAndWrapper(std::strcmp(argv[1], "--enabled") == 0, static_cast<size_t>(page_size));
	else return 2;
	if (!passed) return 1;
	std::puts("VideoOutConfigureAnySdrTest: PASS");
	return 0;
}
