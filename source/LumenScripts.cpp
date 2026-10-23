#include "Lumen/Core/Common.h"
#include "Lumen/Core/Core.h"
#include "Lumen/Core/Debug.h"
#include "Lumen/Core/File.h"
#include "Lumen/Core/Language.h"
#include "Lumen/Core/MemoryAlloc.h"
#include "Lumen/Core/SDLSubsystem.h"
#include "Lumen/Core/Threads.h"
#include "Lumen/Math/MathAll.h"
#include "Lumen/Scripts/BuildTools.h"
#include "Lumen/Scripts/Scripts.h"
#include "Lumen/Sys/SysDbg.h"
#include "Lumen/Sys/SysSync.h"
#include "Lumen/UnitTest.h"

#include "Emulator/Emulator.h"
#include "Emulator/StartupTrace.h"

#if defined(LUMEN_FEXCORE_RUNTIME)
#include "cpu/runtime/GameTranslationCache.h"
#include "cpu/runtime/GuestCpuRuntime.h"
#endif

#include "LumenGitVersion.h"

#include "sqlite3.h"
#include "sha2.h"

#if defined(_MSC_VER) && defined(_CPPUNWIND)
#error "Lumen first-party Windows targets must be compiled with C++ exceptions disabled"
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
#include <windows.h>
#include <shellapi.h>
#endif

using namespace Lumen;
using namespace Core;
using namespace Math;
using namespace Scripts;
using namespace Emulator;
using namespace BuildTools;
using namespace UnitTest;

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
#error "can't compile for android"
#endif

namespace {

namespace fs = std::filesystem;

constexpr int       ManagedUsageExit     = 2;
constexpr int       ManagedPreflightExit = 3;
constexpr int       ScriptErrorExit      = 4;
constexpr int       SubsystemErrorExit   = 5;

class StartupTraceFlushGuard final
{
public:
	~StartupTraceFlushGuard() { Lumen::Emulator::StartupTrace::Flush(); }
};

enum class ManagedMode
{
	None,
	Preflight,
	Launch
};

enum class ManagedModulePolicy
{
	EbootOnly,
	KnownSystem
};

struct ManagedCommand
{
	ManagedMode         mode          = ManagedMode::None;
	ManagedModulePolicy module_policy = ManagedModulePolicy::EbootOnly;
	std::string         install_root;
	std::string         eboot_sha256;
	std::string         param_sha256;
	std::string         event_token;
	int                 width      = 1920;
	int                 height     = 1080;
	bool                load_libc  = false;
	bool                load_fios2 = false;
};

#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
// WC_ERR_INVALID_CHARS is part of the Win32 API, but older MinGW header sets
// hide the symbolic name behind their selected Windows-version macros.
// Preserve the documented strict UTF-16 validation flag on those toolchains.
#if defined(WC_ERR_INVALID_CHARS)
constexpr DWORD WindowsStrictWideCharConversion = WC_ERR_INVALID_CHARS;
#else
constexpr DWORD WindowsStrictWideCharConversion = 0x00000080UL;
#endif

struct WindowsUtf8Arguments
{
	std::vector<std::string> values;
	std::vector<char*>       pointers;

	bool Read()
	{
		int      wide_count = 0;
		wchar_t** wide_args = CommandLineToArgvW(GetCommandLineW(), &wide_count);
		if (wide_args == nullptr || wide_count <= 0)
		{
			return false;
		}
		values.reserve(static_cast<std::size_t>(wide_count));
		for (int index = 0; index < wide_count; ++index)
		{
			const int byte_count =
			    WideCharToMultiByte(CP_UTF8, WindowsStrictWideCharConversion, wide_args[index], -1, nullptr, 0, nullptr, nullptr);
			if (byte_count <= 0)
			{
				LocalFree(wide_args);
				return false;
			}
			std::string value(static_cast<std::size_t>(byte_count), '\0');
			if (WideCharToMultiByte(CP_UTF8, WindowsStrictWideCharConversion, wide_args[index], -1, value.data(), byte_count,
			                        nullptr, nullptr) != byte_count)
			{
				LocalFree(wide_args);
				return false;
			}
			value.pop_back();
			values.push_back(std::move(value));
		}
		LocalFree(wide_args);
		pointers.reserve(values.size() + 1U);
		for (auto& value: values)
		{
			pointers.push_back(value.data());
		}
		pointers.push_back(nullptr);
		return true;
	}
};
#endif

bool IsRegularFileWithoutLink(const fs::path& path, bool require_nonempty, std::string* error)
{
	std::error_code status_error;
	const auto      status = fs::symlink_status(path, status_error);
	if (status_error || fs::is_symlink(status) || !fs::is_regular_file(status))
	{
		*error = "required managed file is missing, linked or not regular: " + path.filename().u8string();
		return false;
	}
	if (require_nonempty)
	{
		std::error_code size_error;
		const auto      size = fs::file_size(path, size_error);
		if (size_error || size == 0)
		{
			*error = "required managed file is empty: " + path.filename().u8string();
			return false;
		}
	}
	return true;
}

bool IsDirectoryWithoutLink(const fs::path& path, std::string* error)
{
	std::error_code status_error;
	const auto      status = fs::symlink_status(path, status_error);
	if (status_error || fs::is_symlink(status) || !fs::is_directory(status))
	{
		*error = "required managed directory is missing, linked or not a directory: " + path.filename().u8string();
		return false;
	}
	return true;
}

bool Sha256File(const fs::path& path, std::string* digest, std::string* error)
{
	std::ifstream input(path, std::ios::binary);
	if (!input)
	{
		*error = "cannot open critical ROM file";
		return false;
	}

	sha256_ctx                                context {};
	std::array<unsigned char, 64U * 1024U>   buffer {};
	std::array<unsigned char, SHA256_DIGEST_SIZE> bytes {};
	sha256_init(&context);
	while (input)
	{
		input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
		const auto count = input.gcount();
		if (count > 0)
		{
			sha256_update(&context, buffer.data(), static_cast<unsigned int>(count));
		}
	}
	if (!input.eof())
	{
		*error = "cannot read critical ROM file";
		return false;
	}
	sha256_final(&context, bytes.data());

	constexpr char hex[] = "0123456789abcdef";
	digest->clear();
	digest->reserve(bytes.size() * 2U);
	for (const auto byte: bytes)
	{
		digest->push_back(hex[(byte >> 4U) & 0x0fU]);
		digest->push_back(hex[byte & 0x0fU]);
	}
	return true;
}

#if defined(LUMEN_FEXCORE_RUNTIME)
std::string Sha256Bytes(std::string_view content)
{
	sha256_ctx context {};
	std::array<unsigned char, SHA256_DIGEST_SIZE> bytes {};
	sha256_init(&context);
	while (!content.empty())
	{
		const auto count = std::min<size_t>(content.size(), 65536U);
		sha256_update(&context, reinterpret_cast<const unsigned char*>(content.data()), static_cast<unsigned int>(count));
		content.remove_prefix(count);
	}
	sha256_final(&context, bytes.data());

	constexpr char hex[] = "0123456789abcdef";
	std::string digest;
	digest.reserve(bytes.size() * 2U);
	for (const auto byte: bytes)
	{
		digest.push_back(hex[(byte >> 4U) & 0x0fU]);
		digest.push_back(hex[byte & 0x0fU]);
	}
	return digest;
}

bool PrepareManagedTranslationCache(const ManagedCommand& command, std::string* error)
{
	if (GuestCpu::Selected() != GuestCpu::Provider::Fex)
	{
		return true;
	}
	bool enabled = false;
	if (!GuestCpu::GameTranslationCache::Requested(&enabled, error))
	{
		return false;
	}
	if (!enabled)
	{
		return true;
	}
	std::string identity;
	if (std::getenv("LUMEN_FEX_TRANSLATION_CACHE_CONTENT_ID") == nullptr &&
	    !GuestCpu::GameTranslationCache::ExecutableContentId(
	        fs::u8path(command.install_root), fs::path("eboot.bin"),
	        [error](const fs::path& path, std::string* digest) { return Sha256File(path, digest, error); },
	        Sha256Bytes, &identity, error))
	{
		return false;
	}
	return GuestCpu::GameTranslationCache::PrepareEnvironment(identity, error);
}
#endif

bool ParseDimension(const char* text, int minimum, int maximum, int* value)
{
	if (text == nullptr || text[0] == '\0')
	{
		return false;
	}
	errno       = 0;
	char* end  = nullptr;
	const long parsed = std::strtol(text, &end, 10);
	if (errno != 0 || end == text || end == nullptr || *end != '\0' || parsed < minimum || parsed > maximum)
	{
		return false;
	}
	*value = static_cast<int>(parsed);
	return true;
}

bool IsDigest(const std::string& value)
{
	if (value.size() != SHA256_DIGEST_SIZE * 2U)
	{
		return false;
	}
	return std::all_of(value.cbegin(), value.cend(), [](unsigned char ch) { return std::isxdigit(ch) != 0; });
}

bool IsEventToken(const std::string& value)
{
	if (value.empty() || value.size() > 80U)
	{
		return false;
	}
	return std::all_of(value.cbegin(), value.cend(),
	                   [](unsigned char ch) { return std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.'; });
}

bool ParseManagedCommand(int argc, char* argv[], ManagedCommand* command, std::string* error)
{
	if (argc < 2)
	{
		return true;
	}
	const std::string mode(argv[1]);
	if (mode != "--launch-managed" && mode != "--preflight-managed")
	{
		return true;
	}
	command->mode = mode == "--launch-managed" ? ManagedMode::Launch : ManagedMode::Preflight;
	if (argc < 3 || argv[2] == nullptr || argv[2][0] == '\0')
	{
		*error = "managed launch requires a ROM directory";
		return false;
	}
	command->install_root = argv[2];

	bool has_eboot    = false;
	bool has_param    = false;
	bool has_policy   = false;
	bool has_width    = false;
	bool has_height   = false;
	bool has_token    = false;
	for (int index = 3; index < argc; index += 2)
	{
		if (index + 1 >= argc)
		{
			*error = "managed launch option is missing its value";
			return false;
		}
		const std::string option(argv[index]);
		const char*       value = argv[index + 1];
		if (option == "--eboot-sha256" && !has_eboot)
		{
			command->eboot_sha256 = value;
			std::transform(command->eboot_sha256.begin(), command->eboot_sha256.end(), command->eboot_sha256.begin(),
			               [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
			has_eboot = true;
		} else if (option == "--param-sha256" && !has_param)
		{
			command->param_sha256 = value;
			std::transform(command->param_sha256.begin(), command->param_sha256.end(), command->param_sha256.begin(),
			               [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
			has_param = true;
		} else if (option == "--module-policy" && !has_policy)
		{
			const std::string policy(value);
			if (policy == "eboot-only")
			{
				command->module_policy = ManagedModulePolicy::EbootOnly;
			} else if (policy == "known-system")
			{
				command->module_policy = ManagedModulePolicy::KnownSystem;
			} else
			{
				*error = "unsupported managed module policy";
				return false;
			}
			has_policy = true;
		} else if (option == "--width" && !has_width)
		{
			has_width = ParseDimension(value, 320, 7680, &command->width);
			if (!has_width)
			{
				*error = "invalid managed launch width";
				return false;
			}
		} else if (option == "--height" && !has_height)
		{
			has_height = ParseDimension(value, 240, 4320, &command->height);
			if (!has_height)
			{
				*error = "invalid managed launch height";
				return false;
			}
		} else if (option == "--event-token" && !has_token)
		{
			command->event_token = value;
			has_token            = true;
		} else
		{
			*error = "unknown or duplicate managed launch option: " + option;
			return false;
		}
	}
	if (!has_eboot || !IsDigest(command->eboot_sha256) || !has_param || !IsDigest(command->param_sha256) || !has_policy ||
	    !has_width || !has_height || !has_token ||
	    !IsEventToken(command->event_token))
	{
		*error = "managed launch options are incomplete or invalid";
		return false;
	}
	return true;
}

bool ValidateManagedCommand(ManagedCommand* command, std::string* error)
{
	const fs::path requested = fs::u8path(command->install_root);
	if (!requested.is_absolute())
	{
		*error = "managed ROM directory must be absolute";
		return false;
	}
	std::error_code root_status_error;
	const auto      root_status = fs::symlink_status(requested, root_status_error);
	if (root_status_error || fs::is_symlink(root_status) || !fs::is_directory(root_status))
	{
		*error = "managed ROM directory is missing, linked or not a directory";
		return false;
	}
	std::error_code canonical_error;
	const fs::path canonical = fs::canonical(requested, canonical_error);
	if (canonical_error || canonical.empty())
	{
		*error = "cannot canonicalize managed ROM directory";
		return false;
	}
	command->install_root = canonical.u8string();

	const fs::path eboot   = canonical / "eboot.bin";
	const fs::path sce_sys = canonical / "sce_sys";
	const fs::path param   = canonical / "sce_sys" / "param.sfo";
	if (!IsRegularFileWithoutLink(eboot, true, error) || !IsDirectoryWithoutLink(sce_sys, error) ||
	    !IsRegularFileWithoutLink(param, true, error))
	{
		return false;
	}
	std::string actual_eboot_sha256;
	std::string actual_param_sha256;
	if (!Sha256File(eboot, &actual_eboot_sha256, error) || !Sha256File(param, &actual_param_sha256, error) ||
	    actual_eboot_sha256 != command->eboot_sha256 || actual_param_sha256 != command->param_sha256)
	{
		if (error->empty())
		{
			*error = "critical ROM files changed after dashboard validation";
		}
		return false;
	}

	if (command->module_policy == ManagedModulePolicy::KnownSystem)
	{
		const fs::path module_directory = canonical / "sce_module";
		if (!IsDirectoryWithoutLink(module_directory, error))
		{
			return false;
		}
		const fs::path libc  = canonical / "sce_module" / "libc.prx";
		const fs::path fios2 = canonical / "sce_module" / "libSceFios2.prx";
		std::error_code libc_error;
		std::error_code fios_error;
		const auto libc_status  = fs::symlink_status(libc, libc_error);
		const auto fios_status  = fs::symlink_status(fios2, fios_error);
		command->load_libc      = !libc_error && !fs::is_symlink(libc_status) && fs::is_regular_file(libc_status) &&
		                          fs::file_size(libc, libc_error) > 0 && !libc_error;
		command->load_fios2     = !fios_error && !fs::is_symlink(fios_status) && fs::is_regular_file(fios_status) &&
		                          fs::file_size(fios2, fios_error) > 0 && !fios_error;
		if (!command->load_libc && !command->load_fios2)
		{
			*error = "known-system module policy selected but no supported module is present";
			return false;
		}
	}
	return true;
}

void PrintManagedEvent(const ManagedCommand& command, const char* state)
{
	std::printf("LUMEN_RUNTIME_EVENT token=%s state=%s\n", command.event_token.c_str(), state);
	std::fflush(stdout);
}

int ReportScriptError(const String& script_name, ScriptError error)
{
	std::fprintf(stderr, "can't run file %s\n", script_name.C_Str());
	switch (error)
	{
		case ScriptError::FileError: std::fprintf(stderr, "file error\n"); break;
		case ScriptError::SyntaxError: std::fprintf(stderr, "syntax error:\n%s\n", GetErrMsg().C_Str()); break;
		case ScriptError::RunError: std::fprintf(stderr, "run error:\n%s\n", GetErrMsg().C_Str()); break;
		case ScriptError::UnknownError: std::fprintf(stderr, "unknown error\n"); break;
		case ScriptError::Ok: return 0;
	}
	return ScriptErrorExit;
}

int RunManagedGame(const ManagedCommand& command)
{
	SetGlobalString("lumen_managed_game_dir", String::FromUtf8(command.install_root.c_str()));
	SetGlobalInteger("lumen_managed_width", command.width);
	SetGlobalInteger("lumen_managed_height", command.height);
	SetGlobalBoolean("lumen_managed_load_libc", command.load_libc);
	SetGlobalBoolean("lumen_managed_load_fios2", command.load_fios2);

	static const String source = String::FromUtf8(R"lua(
local cfg = {
    ScreenWidth = lumen_managed_width;
    ScreenHeight = lumen_managed_height;
    Neo = false;
    VulkanValidationEnabled = false;
    ShaderValidationEnabled = false;
    ShaderOptimizationType = 'Performance';
    ShaderLogDirection = 'Silent';
    ShaderLogFolder = '_Shaders';
    CommandBufferDumpEnabled = false;
    CommandBufferDumpFolder = '_Buffers';
    PrintfDirection = 'Silent';
    PrintfOutputFile = '_lumen.txt';
    ProfilerDirection = 'None';
    ProfilerOutputFile = '_profile.prof';
    SpirvDebugPrintfEnabled = false;
    DiagnosticsEnabled = false;
    ExecutableNullStubsEnabled = false;
}

lumen_init(cfg);
lumen_mount(lumen_managed_game_dir, '/app0');
lumen_load_param_sfo(lumen_managed_game_dir .. '/sce_sys/param.sfo');
lumen_load_elf('/app0/eboot.bin');
if lumen_managed_load_libc then
    lumen_load_elf('/app0/sce_module/libc.prx', 0);
end
if lumen_managed_load_fios2 then
    lumen_load_elf('/app0/sce_module/libSceFios2.prx', 0);
end
lumen_load_symbols_all();
lumen_execute();
)lua");

	PrintManagedEvent(command, "STARTING");
	const ScriptError error = RunString(source);
	if (error != ScriptError::Ok)
	{
		PrintManagedEvent(command, "SCRIPT_ERROR");
		return ReportScriptError(U"<managed-launch>", error);
	}
	PrintManagedEvent(command, "FINISHED");
	return 0;
}

} // namespace

static String get_build_string()
{
	Date date = Date::FromMacros(String::FromUtf8(__DATE__));

#if LUMEN_BUILD == LUMEN_BUILD_DEBUG
	String type = U"Debug";
#elif LUMEN_BUILD == LUMEN_BUILD_RELEASE
	String type = U"Release";
#else
	String type = U"????";
#endif

#if LUMEN_LUA_VER == LUMEN_LUA_5_2
	String lua = U"5.2";
#elif LUMEN_LUA_VER == LUMEN_LUA_5_3
	String lua  = U"5.3";
#else
	String lua  = U"?.?";
#endif

	String compiler = Debug::GetCompiler() + U"-" + Debug::GetLinker() + U"-" + Debug::GetBitness();

	String str = String::FromPrintf("%s, %s, ver = %s, git = %s, lua = %s, date = %s", type.C_Str(), compiler.C_Str(), LUMEN_VERSION,
	                                LUMEN_GIT_VERSION, lua.C_Str(), date.ToString().C_Str());

	return str;
}

static int run_script(const String& lua_file_name, const StringList& args)
{
	SetGlobalStringArray("arg", args, 2);

	bool   is_file = true;
	String statement;

	if (lua_file_name.StartsWith(U'{') && lua_file_name.EndsWith(U'}'))
	{
		statement = lua_file_name.Mid(1, lua_file_name.Size() - 2);
		is_file   = false;
	}

	ScriptError err = is_file ? RunFile(lua_file_name) : RunString(statement);

	if (err != ScriptError::Ok)
	{
		return ReportScriptError(lua_file_name, err);
	}
	return 0;
}

int main(int argc, char* argv[])
{
	Lumen::Emulator::StartupTrace::Reset();
	Lumen::Emulator::StartupTrace::Emit("child", "main.begin");
	StartupTraceFlushGuard startup_trace_flush_guard;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
	// QProcess creates the child with a UTF-16 command line. Re-read that wide
	// command line instead of accepting the CRT's active-code-page argv so a
	// managed library path remains valid outside the current ANSI code page.
	WindowsUtf8Arguments windows_arguments;
	if (!windows_arguments.Read())
	{
		std::fprintf(stderr, "cannot decode the Windows command line as UTF-8\n");
		return ManagedUsageExit;
	}
	argc = static_cast<int>(windows_arguments.values.size());
	argv = windows_arguments.pointers.data();
#endif

	ManagedCommand managed_command;
	std::string    managed_error;
	if (!ParseManagedCommand(argc, argv, &managed_command, &managed_error))
	{
		std::fprintf(stderr, "managed launch usage error: %s\n", managed_error.c_str());
		return ManagedUsageExit;
	}
	if (managed_command.mode != ManagedMode::None)
	{
		Lumen::Emulator::StartupTrace::Emit("child", "managed_preflight.begin");
	}
	if (managed_command.mode != ManagedMode::None && !ValidateManagedCommand(&managed_command, &managed_error))
	{
		PrintManagedEvent(managed_command, "PREFLIGHT_FAILED");
		std::fprintf(stderr, "managed launch preflight failed: %s\n", managed_error.c_str());
		return ManagedPreflightExit;
	}
#if defined(LUMEN_FEXCORE_RUNTIME)
	// Prepare the game manifest before subsystem initialization can admit FEX.
	// Preflight and arbitrary Lua scripts have no prepared game cache context.
	if (managed_command.mode == ManagedMode::Launch && !PrepareManagedTranslationCache(managed_command, &managed_error))
	{
		PrintManagedEvent(managed_command, "PREFLIGHT_FAILED");
		std::fprintf(stderr, "managed launch cache preflight failed: %s\n", managed_error.c_str());
		return ManagedPreflightExit;
	}
#endif
	if (managed_command.mode != ManagedMode::None)
	{
		Lumen::Emulator::StartupTrace::Emit("child", "managed_preflight.end");
	}
	if (managed_command.mode == ManagedMode::Preflight)
	{
		PrintManagedEvent(managed_command, "PREFLIGHT_OK");
		return 0;
	}

	mem_set_max_size(static_cast<size_t>(2048) * 1024 * 1024 - 1);

	auto& slist = *SubsystemsList::Instance();

	slist.SetArgs(argc, argv);

	auto Scripts  = ScriptsSubsystem::Instance();
	auto Core     = CoreSubsystem::Instance();
	auto UnitTest = UnitTestSubsystem::Instance();
#if LUMEN_PROJECT != LUMEN_PROJECT_BUILD_TOOLS
	auto Math     = MathSubsystem::Instance();
	auto SDL      = SDLSubsystem::Instance();
	auto Threads  = ThreadsSubsystem::Instance();
	auto Emulator = EmulatorSubsystem::Instance();
#endif
	auto BuildTools = BuildToolsSubsystem::Instance();

	slist.Add(Core, {});
	slist.Add(Scripts, {Core});
#if LUMEN_PROJECT != LUMEN_PROJECT_BUILD_TOOLS
	slist.Add(Math, {Core});
	slist.Add(SDL, {Core});
	slist.Add(Threads, {Core, SDL});
	slist.Add(Emulator, {Core, Scripts});
#endif
	slist.Add(BuildTools, {Core, Scripts});
	slist.Add(UnitTest, {Core});

	int exit_code = 0;
	Lumen::Emulator::StartupTrace::Emit("child", "base_init.begin");
	const bool initialized = slist.InitAll(false);
	Lumen::Emulator::StartupTrace::Emit("child", "base_init.end");
	if (!initialized)
	{
		printf("Failed to initialize '%s' subsystem: %s\n", slist.GetFailName(), slist.GetFailMsg());
		exit_code = SubsystemErrorExit;
	} else
	{
		if (managed_command.mode == ManagedMode::Launch)
		{
			PrintManagedEvent(managed_command, "PREFLIGHT_OK");
			exit_code = RunManagedGame(managed_command);
		} else if (argc >= 2)
		{
			StringList args;
			for (int i = 0; i < argc; i++)
			{
				String str = String::FromUtf8(argv[i]);
				if (str.StartsWith(U"'") && str.EndsWith(U"'"))
				{
					// Eclipse bug: https://bugs.eclipse.org/bugs/show_bug.cgi?id=494246
					//				https://bugs.eclipse.org/bugs/show_bug.cgi?id=516027
					str = str.Mid(1, str.Size() - 2);
				}
				args.Add(str);
			}
			exit_code = run_script(args.At(1), args);
		} else
		{
			printf("%s\n", get_build_string().C_Str());
			printf("fc_script <lua_script> args...\n\n");
			PrintHelp();
		}

		slist.DestroyAll(false);
	}

	return exit_code;
}
