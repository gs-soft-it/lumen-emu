#include "Lumen/Core/File.h"
#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace {

namespace FS = Lumen::Libs::LibKernel::FileSystem;
namespace VM = Lumen::Core::VirtualMemory;
using Lumen::Loader::HostCallScalarType;
using Lumen::Loader::SymbolDatabase;
using Lumen::Loader::SymbolResolve;
using Lumen::Loader::SymbolType;

using TemporaryDataMount2 = int32_t (LUMEN_SYSV_ABI*)(uint32_t, void*);
using AvailableSpaceKb    = int32_t (LUMEN_SYSV_ABI*)(const void*, uint64_t*);

constexpr char DOWNLOAD_DATA_MOUNT_POINT[] = "/download0";

constexpr int32_t APP_CONTENT_ERROR_PARAMETER = static_cast<int32_t>(0x80d90002u);
constexpr size_t MOUNT_POINT_SIZE = 16;
constexpr std::array<char, MOUNT_POINT_SIZE> MOUNT_POINT {
    '/', 't', 'e', 'm', 'p', '0',
};

bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "AppContentTemporaryDataTest: %s\n", message);
	return condition;
}

class TemporaryWorkingDirectory final
{
public:
	TemporaryWorkingDirectory()
	{
		char path[] = "/tmp/lumen-appcontent-temporary-data-XXXXXX";
		const char* created = mkdtemp(path);
		if (created == nullptr) return;
		m_root = created;
		std::error_code error;
		m_previous = std::filesystem::current_path(error);
		if (error)
		{
			std::filesystem::remove_all(m_root, error);
			m_root.clear();
			return;
		}
		std::filesystem::current_path(m_root, error);
		if (error)
		{
			std::filesystem::remove_all(m_root, error);
			m_root.clear();
		}
	}

	~TemporaryWorkingDirectory()
	{
		std::error_code error;
		if (!m_previous.empty()) std::filesystem::current_path(m_previous, error);
		if (!m_root.empty()) std::filesystem::remove_all(m_root, error);
	}

	bool Valid() const { return !m_root.empty(); }
	const std::filesystem::path& Root() const { return m_root; }

private:
	std::filesystem::path m_previous;
	std::filesystem::path m_root;
};

class Mapping final
{
public:
	explicit Mapping(size_t size): m_size(size), m_address(VM::Alloc(0, size, VM::Mode::ReadWrite)) {}
	~Mapping()
	{
		if (m_address != 0 && !VM::Free(m_address)) std::abort();
	}

	uint64_t Address(size_t offset = 0) const { return m_address + offset; }
	bool Valid() const { return m_address != 0; }
	bool Protect(size_t offset, size_t size, VM::Mode mode) const
	{
		return Valid() && offset <= m_size && size <= m_size - offset &&
		       VM::Protect(Address(offset), size, mode);
	}

private:
	size_t   m_size;
	uint64_t m_address;
};

SymbolResolve ExactBinding(const char* name)
{
	SymbolResolve query {};
	query.name = name;
	query.library = "AppContent";
	query.library_version = 1;
	query.module = "AppContentUtil";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type = SymbolType::Func;
	return query;
}

std::string Detail(const char* name, const char* message)
{
	return std::string(name) + " " + message;
}

template <typename Function>
Function Resolve(SymbolDatabase* symbols, const char* name, const char* standard_name)
{
	const auto* record = symbols->Find(ExactBinding(name));
	if (!Check(record != nullptr, Detail(name, "binding missing").c_str())) return nullptr;
	const auto* scalar = symbols->HostCallScalarMetadataFor(record->vaddr);
	if (!Check(scalar != nullptr && scalar->arity == 2, Detail(name, "scalar ABI is not two arguments").c_str()) ||
	    !Check(scalar->arguments[0] == HostCallScalarType::Gp && scalar->arguments[1] == HostCallScalarType::Gp &&
	                 scalar->result == HostCallScalarType::Gp && scalar->invoker != nullptr,
	             Detail(name, "scalar ABI is not two words returning a word").c_str()))
		return nullptr;
	auto wrong = ExactBinding(name);
	wrong.module = "unrelated";
	if (!Check(symbols->Find(wrong) == nullptr, Detail(name, "identity is over-broad").c_str()) ||
	    !Check(symbols->FindByStdName(Lumen::String::FromUtf8(standard_name)) == nullptr,
	           Detail(name, "gained a standard-name fallback").c_str()))
		return nullptr;
	return reinterpret_cast<Function>(record->vaddr);
}

bool IsExactMountPoint(const void* address)
{
	return std::memcmp(address, MOUNT_POINT.data(), MOUNT_POINT.size()) == 0;
}

std::string ResolvedProbePath()
{
	return FS::GetRealFilename(U"/temp0/write-probe.bin").C_Str();
}

bool CheckWritableIsolatedMount(const std::string& path, const TemporaryWorkingDirectory& working)
{
	const std::filesystem::path candidate = std::filesystem::absolute(path).lexically_normal();
	std::error_code error;
	const auto root = std::filesystem::canonical(working.Root(), error);
	if (error) return Check(false, "could not canonicalize working directory");
	const auto parent = std::filesystem::canonical(candidate.parent_path(), error);
	if (error || parent.parent_path() != root)
		return Check(false, "temporary data escaped the per-launch working directory");
	if (candidate.filename() != "write-probe.bin" ||
	    parent.filename().string().rfind("_AppContentTemporaryData.", 0) != 0)
		return Check(false, "temporary data host name is outside the local profile");

	auto* file = std::fopen(candidate.string().c_str(), "wb");
	if (!Check(file != nullptr, "temporary data directory is not writable")) return false;
	const bool wrote = std::fwrite("ok", 1, 2, file) == 2;
	std::fclose(file);
	const bool exists = Lumen::Core::File::IsFileExisting(Lumen::String::FromUtf8(candidate.string().c_str()));
	std::remove(candidate.string().c_str());
	return Check(wrote && exists, "temporary data write did not reach the translated directory");
}

bool Run(TemporaryDataMount2 mount, const TemporaryWorkingDirectory& working)
{
	constexpr size_t unit = 65536;
	Mapping memory(3 * unit);
	if (!Check(memory.Valid(), "guest memory fixture failed")) return false;

	auto* ordinary = reinterpret_cast<void*>(memory.Address(128));
	std::memset(ordinary, 0xa5, MOUNT_POINT_SIZE);
	if (!Check(mount(0, ordinary) == 0 && IsExactMountPoint(ordinary),
	           "option NONE did not publish the exact bounded mount point"))
		return false;
	const std::string first = ResolvedProbePath();
	if (!CheckWritableIsolatedMount(first, working)) return false;

	std::memset(ordinary, 0xa5, MOUNT_POINT_SIZE);
	if (!Check(mount(0, ordinary) == 0 && IsExactMountPoint(ordinary) &&
	           ResolvedProbePath() == first,
	           "repeated option NONE did not preserve the live session mount"))
		return false;

	std::memset(ordinary, 0xa5, MOUNT_POINT_SIZE);
	if (!Check(mount(2, ordinary) == APP_CONTENT_ERROR_PARAMETER &&
	           ResolvedProbePath() == first,
	           "unsupported option changed mount state"))
		return false;
	const std::array<uint8_t, MOUNT_POINT_SIZE> canary {
	    0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5,
	    0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5,
	};
	if (!Check(std::memcmp(ordinary, canary.data(), canary.size()) == 0,
	           "unsupported option modified output"))
		return false;

	auto* read_only = reinterpret_cast<void*>(memory.Address(2 * unit + 128));
	std::memset(read_only, 0xa5, MOUNT_POINT_SIZE);
	if (!Check(memory.Protect(2 * unit, unit, VM::Mode::Read), "read-only fixture setup failed"))
		return false;
	const int32_t read_only_result = mount(1, read_only);
	if (!Check(memory.Protect(2 * unit, unit, VM::Mode::ReadWrite), "read-only fixture restore failed"))
		return false;
	if (!Check(read_only_result == APP_CONTENT_ERROR_PARAMETER &&
	           std::memcmp(read_only, canary.data(), canary.size()) == 0 &&
	           ResolvedProbePath() == first,
	           "read-only output consumed a directory or changed the mount"))
		return false;

	auto* crossing = reinterpret_cast<void*>(memory.Address(unit - 8));
	std::memset(crossing, 0xa5, 8);
	if (!Check(memory.Protect(unit, unit, VM::Mode::NoAccess), "guard fixture setup failed")) return false;
	const int32_t crossing_result = mount(1, crossing);
	if (!Check(memory.Protect(unit, unit, VM::Mode::ReadWrite), "guard fixture restore failed")) return false;
	if (!Check(crossing_result == APP_CONTENT_ERROR_PARAMETER &&
	           std::memcmp(crossing, canary.data(), 8) == 0 &&
	           ResolvedProbePath() == first,
	           "cross-boundary output partially wrote or changed the mount"))
		return false;

	if (!Check(mount(1, nullptr) == APP_CONTENT_ERROR_PARAMETER &&
	           mount(1, reinterpret_cast<void*>(UINT64_C(1))) == APP_CONTENT_ERROR_PARAMETER &&
	           mount(1, reinterpret_cast<void*>(UINT64_MAX - 7)) == APP_CONTENT_ERROR_PARAMETER &&
	           ResolvedProbePath() == first,
	           "invalid outputs changed the live mount"))
		return false;

	std::memset(ordinary, 0xa5, MOUNT_POINT_SIZE);
	if (!Check(mount(1, ordinary) == 0 && IsExactMountPoint(ordinary),
	           "option FORMAT did not rotate to an empty mount"))
		return false;
	const std::string formatted = ResolvedProbePath();
	if (!Check(formatted != first && CheckWritableIsolatedMount(formatted, working),
	           "option FORMAT reused the previous backing directory"))
		return false;

	if (!Check(FS::ResetForNextSession(), "filesystem session reset failed")) return false;
	std::memset(ordinary, 0xa5, MOUNT_POINT_SIZE);
	if (!Check(mount(0, ordinary) == 0 && IsExactMountPoint(ordinary),
	           "mount did not recover after a filesystem session reset"))
		return false;
	const std::string next_session = ResolvedProbePath();
	return Check(next_session != formatted && CheckWritableIsolatedMount(next_session, working),
	             "next session reused the previous backing directory");
}

size_t CountWorkingEntries(const TemporaryWorkingDirectory& working)
{
	std::error_code error;
	size_t          entries = 0;
	for (auto it = std::filesystem::directory_iterator(working.Root(), error);
	     !error && it != std::filesystem::directory_iterator(); it.increment(error))
	{
		++entries;
	}
	return error ? std::numeric_limits<size_t>::max() : entries;
}

bool RunAvailableSpace(AvailableSpaceKb query, const TemporaryWorkingDirectory& working)
{
	constexpr size_t unit = 65536;
	Mapping memory(3 * unit);
	if (!Check(memory.Valid(), "available-space fixture failed")) return false;

	std::error_code    space_error;
	const auto         host_space = std::filesystem::space(working.Root(), space_error);
	if (!Check(!space_error, "host space query failed")) return false;
	const uint64_t expected_kb = static_cast<uint64_t>(host_space.available / 1024);
	const uint64_t capacity_kb = static_cast<uint64_t>(host_space.capacity / 1024);
	// The profile answers from the live session volume, so the two measurements
	// taken milliseconds apart may differ only by concurrent host activity.
	constexpr uint64_t tolerance_kb = 256ull * 1024ull;
	constexpr uint64_t untouched   = 0xa5a5a5a5a5a5a5a5ull;

	auto* record = static_cast<char*>(reinterpret_cast<void*>(memory.Address(128)));
	std::memset(record, 0, MOUNT_POINT_SIZE);
	std::memcpy(record, DOWNLOAD_DATA_MOUNT_POINT, sizeof(DOWNLOAD_DATA_MOUNT_POINT));
	std::array<char, MOUNT_POINT_SIZE> published {};
	std::memcpy(published.data(), record, published.size());
	auto* output = reinterpret_cast<uint64_t*>(memory.Address(256));

	const std::string probe_before = ResolvedProbePath();
	const size_t      entries_before = CountWorkingEntries(working);

	*output = untouched;
	if (!Check(query(record, output) == 0, "download-data space query failed")) return false;
	const uint64_t first = *output;
	if (!Check(first > 0 && first <= capacity_kb, "available space is outside the session volume")) return false;
	if (!Check(first + tolerance_kb >= expected_kb && expected_kb + tolerance_kb >= first,
	           "available space is not the live session volume measurement"))
		return false;
	if (!Check(std::memcmp(record, published.data(), published.size()) == 0,
	           "space query modified the mount-point record"))
		return false;

	*output = untouched;
	if (!Check(query(record, output) == 0 && *output != untouched && *output + tolerance_kb >= first &&
	               first + tolerance_kb >= *output,
	           "repeated space query drifted outside the session volume"))
		return false;

	std::memset(record, 0, MOUNT_POINT_SIZE);
	*output = untouched;
	if (!Check(query(record, output) == APP_CONTENT_ERROR_PARAMETER && *output == untouched,
	           "empty mount point was answered"))
		return false;

	std::memset(record, 'x', MOUNT_POINT_SIZE);
	if (!Check(query(record, output) == APP_CONTENT_ERROR_PARAMETER && *output == untouched,
	           "unterminated mount point was answered"))
		return false;

	std::memcpy(record, published.data(), published.size());
	auto* read_only = reinterpret_cast<uint64_t*>(memory.Address(2 * unit + 256));
	*read_only = untouched;
	if (!Check(memory.Protect(2 * unit, unit, VM::Mode::Read), "read-only space fixture setup failed")) return false;
	const int32_t read_only_result = query(record, read_only);
	if (!Check(memory.Protect(2 * unit, unit, VM::Mode::ReadWrite), "read-only space fixture restore failed")) return false;
	if (!Check(read_only_result == APP_CONTENT_ERROR_PARAMETER && *read_only == untouched,
	           "read-only output published a value"))
		return false;

	auto* crossing = reinterpret_cast<uint64_t*>(memory.Address(unit - 4));
	*crossing = untouched;
	if (!Check(memory.Protect(unit, unit, VM::Mode::NoAccess), "guard fixture setup failed")) return false;
	const int32_t crossing_result = query(record, crossing);
	const int32_t guarded_input   = query(reinterpret_cast<void*>(memory.Address(unit)), output);
	if (!Check(memory.Protect(unit, unit, VM::Mode::ReadWrite), "guard fixture restore failed")) return false;
	if (!Check(crossing_result == APP_CONTENT_ERROR_PARAMETER && *crossing == untouched,
	           "cross-boundary output partially wrote a value"))
		return false;
	if (!Check(guarded_input == APP_CONTENT_ERROR_PARAMETER, "unreadable mount point was answered")) return false;

	if (!Check(query(nullptr, output) == APP_CONTENT_ERROR_PARAMETER &&
	               query(record, nullptr) == APP_CONTENT_ERROR_PARAMETER &&
	               query(reinterpret_cast<void*>(UINT64_C(1)), output) == APP_CONTENT_ERROR_PARAMETER &&
	               query(reinterpret_cast<void*>(UINT64_MAX - 15), output) == APP_CONTENT_ERROR_PARAMETER &&
	               query(record, reinterpret_cast<uint64_t*>(UINT64_C(1))) == APP_CONTENT_ERROR_PARAMETER &&
	               query(record, reinterpret_cast<uint64_t*>(UINT64_MAX - 7)) == APP_CONTENT_ERROR_PARAMETER,
	           "invalid inputs were answered"))
		return false;

	return Check(ResolvedProbePath() == probe_before && CountWorkingEntries(working) == entries_before,
	             "space query changed host or mount state");
}

} // namespace

int main()
{
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Core::core_file_init();
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	VM::Init();
	FS::FileSystemSubsystem::Instance()->Init(nullptr);

	TemporaryWorkingDirectory working;
	SymbolDatabase         symbols;
	const bool registered = Check(Lumen::Libs::Init(U"libAppContent_1", &symbols), "AppContent registration failed");
	const auto mount = registered ? Resolve<TemporaryDataMount2>(&symbols, "buYbeLOGWmA", "sceAppContentTemporaryDataMount2") : nullptr;
	const auto space = registered ? Resolve<AvailableSpaceKb>(&symbols, "Gl6w5i0JokY", "sceAppContentDownloadDataGetAvailableSpaceKb") : nullptr;
	const bool passed = Check(working.Valid(), "temporary working directory failed") && mount != nullptr &&
	                    Run(mount, working) && space != nullptr && RunAvailableSpace(space, working);
	(void)FS::ResetForNextSession();
	if (passed) std::puts("AppContentTemporaryDataTest: PASS");
	return passed ? 0 : 1;
}
