// Lumen PS5 integration modifications, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
#include "Emulator/Runtime.h"

#include "Lumen/Core/Core.h"
#include "Lumen/Core/MemoryAlloc.h"
#include "Lumen/Core/SDLSubsystem.h"
#include "Lumen/Core/String.h"
#include "Lumen/Core/Subsystems.h"
#include "Lumen/Core/Threads.h"
#include "Lumen/Math/MathAll.h"
#include "Lumen/Scripts/Scripts.h"

#include "Emulator/Emulator.h"
#include "Emulator/Loader/RuntimeLinker.h"
#include "Emulator/StartupTrace.h"
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
#include "CpuA64Dynarec.h"
#include "Emulator/Graphics/Objects/VertexBuffer.h"
#if defined(LUMEN_ANDROID_FEXCORE_RUNTIME)
#include "AndroidFexCoreLabAdmission.h"
#endif
#endif

#include "SDL.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <pthread.h>
#endif

namespace {

namespace fs = std::filesystem;

constexpr uint32_t MinWidth      = 320u;
constexpr uint32_t MaxWidth      = 7680u;
constexpr uint32_t MinHeight     = 240u;
constexpr uint32_t MaxHeight     = 4320u;
constexpr size_t   MaxPathLength = 32u * 1024u;

enum class LaunchState: uint32_t
{
	Ready,
	Launching,
	Finished,
	Poisoned
};

enum class OptionalFileState
{
	Missing,
	Valid,
	Invalid
};

struct ValidatedRequest
{
	fs::path install_root;
	fs::path data_root;
	bool     load_libc  = false;
	bool     load_fios2 = false;
};

std::atomic<uint32_t> g_launch_state {static_cast<uint32_t>(LaunchState::Ready)};
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
bool g_process_initialized = false;
#endif

#if !defined(__APPLE__) && LUMEN_PLATFORM != LUMEN_PLATFORM_ANDROID
const std::thread::id g_initial_thread = std::this_thread::get_id();
#endif

template <size_t Size>
void copy_message(char (&destination)[Size], const char* message)
{
	static_assert(Size > 0, "message buffer must not be empty");
	if (message == nullptr)
	{
		destination[0] = '\0';
		return;
	}
	const size_t length = (std::min)(std::strlen(message), Size - 1u);
	std::memcpy(destination, message, length);
	destination[length] = '\0';
}

void set_result(LumenRuntimeResultV1* result, LumenRuntimeResultCodeV1 code, const char* message)
{
	if (result == nullptr)
	{
		return;
	}
	std::memset(result, 0, sizeof(*result));
	result->structSize = sizeof(*result);
	result->abiVersion = LUMEN_RUNTIME_ABI_VERSION_V1;
	result->code       = code;
	copy_message(result->message, message);
}

void set_result(LumenRuntimeResultV1* result, LumenRuntimeResultCodeV1 code, const std::string& message)
{
	set_result(result, code, message.c_str());
}

void set_execution_result(bool script_ok, uint32_t backend_failure, LumenRuntimeResultV1* result)
{
	if (!script_ok) return;
	if (backend_failure != 0)
		set_result(result, LUMEN_RUNTIME_RESULT_INTERNAL_ERROR_V1,
		    "Guest CPU backend failed with code " + std::to_string(backend_failure) + ".");
	else
		set_result(result, LUMEN_RUNTIME_RESULT_OK_V1, "Runtime execution returned normally.");
}

void append_result_message(LumenRuntimeResultV1* result, const char* suffix)
{
	if (result == nullptr || suffix == nullptr || suffix[0] == '\0')
	{
		return;
	}
	std::string message(result->message);
	if (!message.empty())
	{
		message += "; ";
	}
	message += suffix;
	copy_message(result->message, message.c_str());
}

void emit_event(const LumenRuntimeLaunchRequestV1* request, LumenRuntimeEventTypeV1 type, uint32_t progress,
                LumenRuntimeResultCodeV1 code, const char* message)
{
	if (request == nullptr || request->callback == nullptr)
	{
		return;
	}
	LumenRuntimeEventV1 event {};
	event.structSize       = sizeof(event);
	event.abiVersion       = LUMEN_RUNTIME_ABI_VERSION_V1;
	event.type             = type;
	event.progressPermille = (std::min)(progress, 1000u);
	event.code             = code;
	copy_message(event.message, message);
	request->callback(&event, request->callbackUserData);
}

bool is_nonempty_bounded_string(const char* value)
{
	if (value == nullptr || value[0] == '\0')
	{
		return false;
	}
	for (size_t index = 1; index < MaxPathLength; index++)
	{
		if (value[index] == '\0')
		{
			return true;
		}
	}
	return false;
}

LumenRuntimeResultCodeV1 validate_request_header(const LumenRuntimeLaunchRequestV1* request, std::string* error)
{
	if (request == nullptr)
	{
		*error = "Runtime request is null.";
		return LUMEN_RUNTIME_RESULT_INVALID_ARGUMENT_V1;
	}
	if (request->structSize < sizeof(LumenRuntimeLaunchRequestV1) || request->abiVersion != LUMEN_RUNTIME_ABI_VERSION_V1)
	{
		*error = "Runtime request ABI version or structure size is incompatible.";
		return LUMEN_RUNTIME_RESULT_ABI_MISMATCH_V1;
	}
	if (!is_nonempty_bounded_string(request->installPath) || !is_nonempty_bounded_string(request->dataPath))
	{
		*error = "Install and data paths must be non-empty bounded UTF-8 strings.";
		return LUMEN_RUNTIME_RESULT_INVALID_ARGUMENT_V1;
	}
	if (request->profile != LUMEN_RUNTIME_PROFILE_EBOOT_ONLY_V1 && request->profile != LUMEN_RUNTIME_PROFILE_KNOWN_SYSTEM_V1)
	{
		*error = "Runtime launch profile is unsupported.";
		return LUMEN_RUNTIME_RESULT_INVALID_ARGUMENT_V1;
	}
	if (request->width < MinWidth || request->width > MaxWidth || request->height < MinHeight || request->height > MaxHeight)
	{
		*error = "Runtime dimensions are outside the supported range.";
		return LUMEN_RUNTIME_RESULT_INVALID_ARGUMENT_V1;
	}
	return LUMEN_RUNTIME_RESULT_OK_V1;
}

bool path_has_prefix(const fs::path& root, const fs::path& child)
{
	auto root_part  = root.begin();
	auto child_part = child.begin();
	for (; root_part != root.end() && child_part != child.end(); ++root_part, ++child_part)
	{
		if (*root_part != *child_part)
		{
			return false;
		}
	}
	return root_part == root.end();
}

bool canonicalize_real_directory(const char* raw_path, const char* label, fs::path* canonical_path, std::string* error)
{
	const fs::path requested = fs::u8path(raw_path);
	if (!requested.is_absolute())
	{
		*error = std::string(label) + " must be absolute.";
		return false;
	}

	std::error_code status_error;
	const auto      status = fs::symlink_status(requested, status_error);
	if (status_error || fs::is_symlink(status) || !fs::is_directory(status))
	{
		*error = std::string(label) + " is missing, linked or not a directory.";
		return false;
	}

	std::error_code canonical_error;
	const fs::path  canonical = fs::canonical(requested, canonical_error);
	if (canonical_error || canonical.empty() || !canonical.is_absolute())
	{
		*error = std::string("Cannot canonicalize ") + label + ".";
		return false;
	}

	status_error.clear();
	const auto canonical_status = fs::symlink_status(canonical, status_error);
	if (status_error || fs::is_symlink(canonical_status) || !fs::is_directory(canonical_status))
	{
		*error = std::string("Canonical ") + label + " is not a real directory.";
		return false;
	}

	*canonical_path = canonical;
	return true;
}

bool validate_child_directory(const fs::path& root, const fs::path& path, const char* label, std::string* error)
{
	std::error_code status_error;
	const auto      status = fs::symlink_status(path, status_error);
	if (status_error || fs::is_symlink(status) || !fs::is_directory(status))
	{
		*error = std::string(label) + " is missing, linked or not a directory.";
		return false;
	}

	std::error_code canonical_error;
	const fs::path  canonical = fs::canonical(path, canonical_error);
	if (canonical_error || !path_has_prefix(root, canonical))
	{
		*error = std::string(label) + " resolves outside the installation root.";
		return false;
	}
	return true;
}

bool validate_required_file(const fs::path& root, const fs::path& path, const char* label, std::string* error)
{
	std::error_code status_error;
	const auto      status = fs::symlink_status(path, status_error);
	if (status_error || fs::is_symlink(status) || !fs::is_regular_file(status))
	{
		*error = std::string(label) + " is missing, linked or not a regular file.";
		return false;
	}

	std::error_code size_error;
	if (fs::file_size(path, size_error) == 0 || size_error)
	{
		*error = std::string(label) + " is empty or unreadable.";
		return false;
	}

	std::error_code canonical_error;
	const fs::path  canonical = fs::canonical(path, canonical_error);
	if (canonical_error || !path_has_prefix(root, canonical))
	{
		*error = std::string(label) + " resolves outside the installation root.";
		return false;
	}
	return true;
}

OptionalFileState validate_optional_file(const fs::path& root, const fs::path& path, const char* label, std::string* error)
{
	std::error_code status_error;
	const auto      status = fs::symlink_status(path, status_error);
	if (status_error == std::errc::no_such_file_or_directory || (!status_error && !fs::exists(status)))
	{
		return OptionalFileState::Missing;
	}
	if (status_error || fs::is_symlink(status) || !fs::is_regular_file(status))
	{
		*error = std::string(label) + " is linked, unreadable or not a regular file.";
		return OptionalFileState::Invalid;
	}

	std::error_code size_error;
	if (fs::file_size(path, size_error) == 0 || size_error)
	{
		*error = std::string(label) + " is empty or unreadable.";
		return OptionalFileState::Invalid;
	}

	std::error_code canonical_error;
	const fs::path  canonical = fs::canonical(path, canonical_error);
	if (canonical_error || !path_has_prefix(root, canonical))
	{
		*error = std::string(label) + " resolves outside the installation root.";
		return OptionalFileState::Invalid;
	}
	return OptionalFileState::Valid;
}

bool validate_paths(const LumenRuntimeLaunchRequestV1* request, ValidatedRequest* validated, std::string* error)
{
	if (!canonicalize_real_directory(request->installPath, "Installation root", &validated->install_root, error) ||
	    !canonicalize_real_directory(request->dataPath, "Runtime data root", &validated->data_root, error))
	{
		return false;
	}

	const fs::path sce_sys = validated->install_root / "sce_sys";
	if (!validate_required_file(validated->install_root, validated->install_root / "eboot.bin", "eboot.bin", error) ||
	    !validate_child_directory(validated->install_root, sce_sys, "sce_sys", error))
	{
		return false;
	}

	// This ABI configures the PS4 engine. Metadata containing a PS5 marker,
	// including an ambiguous dual-format root or dangling link, cannot enter it.
	std::error_code metadata_error;
	const auto ps5_metadata = fs::symlink_status(sce_sys / "param.json", metadata_error);
	if (ps5_metadata.type() != fs::file_type::not_found ||
	    (metadata_error && metadata_error != std::errc::no_such_file_or_directory))
	{
		*error = "PS5 metadata cannot use the PS4 runtime API; the complete PS5 runtime is not available.";
		return false;
	}
	if (!validate_required_file(validated->install_root, sce_sys / "param.sfo", "sce_sys/param.sfo", error))
	{
		return false;
	}

	if (request->profile == LUMEN_RUNTIME_PROFILE_KNOWN_SYSTEM_V1)
	{
		const fs::path module_root = validated->install_root / "sce_module";
		if (!validate_child_directory(validated->install_root, module_root, "sce_module", error))
		{
			return false;
		}

		const OptionalFileState libc =
		    validate_optional_file(validated->install_root, module_root / "libc.prx", "sce_module/libc.prx", error);
		if (libc == OptionalFileState::Invalid)
		{
			return false;
		}
		const OptionalFileState fios2 = validate_optional_file(validated->install_root, module_root / "libSceFios2.prx",
		                                                        "sce_module/libSceFios2.prx", error);
		if (fios2 == OptionalFileState::Invalid)
		{
			return false;
		}

		validated->load_libc  = libc == OptionalFileState::Valid;
		validated->load_fios2 = fios2 == OptionalFileState::Valid;
		if (!validated->load_libc && !validated->load_fios2)
		{
			*error = "Known-system profile requires libc.prx or libSceFios2.prx.";
			return false;
		}
	}

	return true;
}

#if LUMEN_PLATFORM != LUMEN_PLATFORM_ANDROID
bool is_process_main_thread()
{
#if defined(__APPLE__)
	return pthread_main_np() != 0;
#else
	return std::this_thread::get_id() == g_initial_thread;
#endif
}
#endif

bool runtime_host_configuration_supported(std::string* error)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
#if defined(LUMEN_ANDROID_FEXCORE_RUNTIME)
	if (!Lumen::Android::FexCoreLab::IsSupportedHostPageSize(::sysconf(_SC_PAGESIZE)))
#else
	if (::sysconf(_SC_PAGESIZE) != 4096)
#endif
	{
		if (error != nullptr)
		{
#if defined(LUMEN_ANDROID_FEXCORE_RUNTIME)
			*error = "Android FEX laboratory requires a 4096- or 16384-byte host page size.";
#else
			*error = "Android full runtime currently requires a 4096-byte host page size.";
#endif
		}
		return false;
	}
#else
	(void)error;
#endif
	return true;
}

bool runtime_jit_enabled()
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && \
    defined(LUMEN_ANDROID_DYNAREC_PROVIDER_AARCH64_WX) && \
    defined(LUMEN_ANDROID_DYNAREC_POLICY_SELECTIVE_NODA)
	const char* requested = std::getenv("LUMEN_X64_A64_DYNAREC");
	return requested != nullptr && requested[0] == '1' &&
	       requested[1] == '\0' &&
	       Lumen::Cpu::A64Dynarec::IsHostSupported() &&
	       Lumen::Cpu::A64Dynarec::VerifyAndroidNodaIsolationOwnership();
#else
	return false;
#endif
}

class WorkingDirectoryGuard final
{
public:
	bool Change(const fs::path& target, std::string* error)
	{
		std::error_code current_error;
		m_previous = fs::current_path(current_error);
		if (current_error || m_previous.empty())
		{
			*error = "Cannot read the process working directory.";
			return false;
		}

		std::error_code change_error;
		fs::current_path(target, change_error);
		if (change_error)
		{
			*error = "Cannot set the runtime data working directory.";
			return false;
		}
		m_changed = true;
		return true;
	}

	bool Restore(std::string* error)
	{
		if (!m_changed)
		{
			return true;
		}
		std::error_code restore_error;
		fs::current_path(m_previous, restore_error);
		if (restore_error)
		{
			if (error != nullptr)
			{
				*error = "Cannot restore the process working directory.";
			}
			return false;
		}
		m_changed = false;
		return true;
	}

	~WorkingDirectoryGuard()
	{
		std::string ignored;
		Restore(&ignored);
	}

	WorkingDirectoryGuard() = default;
	WorkingDirectoryGuard(const WorkingDirectoryGuard&)            = delete;
	WorkingDirectoryGuard& operator=(const WorkingDirectoryGuard&) = delete;

private:
	fs::path m_previous;
	bool     m_changed = false;
};

LumenRuntimeResultCodeV1 script_error_code(Lumen::Scripts::ScriptError error)
{
	switch (error)
	{
		case Lumen::Scripts::ScriptError::Ok: return LUMEN_RUNTIME_RESULT_OK_V1;
		case Lumen::Scripts::ScriptError::FileError: return LUMEN_RUNTIME_RESULT_SCRIPT_FILE_ERROR_V1;
		case Lumen::Scripts::ScriptError::SyntaxError: return LUMEN_RUNTIME_RESULT_SCRIPT_SYNTAX_ERROR_V1;
		case Lumen::Scripts::ScriptError::RunError: return LUMEN_RUNTIME_RESULT_SCRIPT_RUN_ERROR_V1;
		case Lumen::Scripts::ScriptError::UnknownError: return LUMEN_RUNTIME_RESULT_SCRIPT_UNKNOWN_ERROR_V1;
	}
	return LUMEN_RUNTIME_RESULT_SCRIPT_UNKNOWN_ERROR_V1;
}

bool run_script_phase(const LumenRuntimeLaunchRequestV1* request, LumenRuntimeEventTypeV1 event_type, uint32_t progress,
	                  const char* phase_name, const char* source, LumenRuntimeResultV1* result)
{
	emit_event(request, event_type, progress, LUMEN_RUNTIME_RESULT_OK_V1, phase_name);
	Lumen::Scripts::ResetErrMsg();
	const auto error = Lumen::Scripts::RunString(Lumen::Core::String::FromUtf8(source));
	if (error == Lumen::Scripts::ScriptError::Ok)
	{
		return true;
	}

	std::string message(phase_name);
	message += " failed";
	const char* detail = Lumen::Scripts::GetErrMsg().C_Str();
	if (detail != nullptr && detail[0] != '\0')
	{
		message += ": ";
		message += detail;
	}
	set_result(result, script_error_code(error), message);
	return false;
}

bool restore_working_directory(WorkingDirectoryGuard* working_directory, LumenRuntimeResultV1* result)
{
	std::string restore_error;
	if (!working_directory->Restore(&restore_error))
	{
		if (result->code == LUMEN_RUNTIME_RESULT_OK_V1)
		{
			set_result(result, LUMEN_RUNTIME_RESULT_WORKING_DIRECTORY_FAILED_V1, restore_error);
		} else
		{
			append_result_message(result, restore_error.c_str());
		}
		return false;
	}
	return true;
}

constexpr const char* ConfigureScript = R"lua(
local cfg = {
    ScreenWidth = lumen_runtime_width_v1;
    ScreenHeight = lumen_runtime_height_v1;
    Neo = false;
)lua"
#if defined(LUMEN_ANDROID_VULKAN_VALIDATION)
    "    VulkanValidationEnabled = true;\n"
#else
    "    VulkanValidationEnabled = false;\n"
#endif
R"lua(
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
lumen_init(cfg)
)lua";

constexpr const char* MountScript = R"lua(
lumen_mount(lumen_runtime_install_root_v1, '/app0')
lumen_load_param_sfo(lumen_runtime_install_root_v1 .. '/sce_sys/param.sfo')
)lua";

constexpr const char* LoadExecutableScript = "lumen_load_elf('/app0/eboot.bin')";

constexpr const char* LoadModulesScript = R"lua(
if lumen_runtime_load_libc_v1 then
    lumen_load_elf('/app0/sce_module/libc.prx', 0)
end
if lumen_runtime_load_fios2_v1 then
    lumen_load_elf('/app0/sce_module/libSceFios2.prx', 0)
end
)lua";

constexpr const char* LinkScript    = "lumen_load_symbols_all()";
constexpr const char* ExecuteScript = "lumen_execute()";

} // namespace

#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
bool LumenRuntimeExecutionFailureContractTest()
{
	using namespace Lumen::Loader;
	ResetGuestExecutionBackendFailure();
	if (GuestExecutionBackendFailure() != 0 || RecordGuestExecutionBackendFailure(0)) return false;
	LumenRuntimeResultV1 result {};
	set_execution_result(true, GuestExecutionBackendFailure(), &result);
	if (result.code != LUMEN_RUNTIME_RESULT_OK_V1) return false;
	if (!RecordGuestExecutionBackendFailure(9) || RecordGuestExecutionBackendFailure(10) ||
	    RecordGuestExecutionBackendFailure(0) || GuestExecutionBackendFailure() != 9) return false;
	set_execution_result(true, GuestExecutionBackendFailure(), &result);
	if (result.code != LUMEN_RUNTIME_RESULT_INTERNAL_ERROR_V1 ||
	    std::strstr(result.message, "code 9") == nullptr) return false;
	set_result(&result, LUMEN_RUNTIME_RESULT_SCRIPT_RUN_ERROR_V1, "existing script failure");
	set_execution_result(false, GuestExecutionBackendFailure(), &result);
	if (result.code != LUMEN_RUNTIME_RESULT_SCRIPT_RUN_ERROR_V1 ||
	    std::strcmp(result.message, "existing script failure") != 0) return false;
	ResetGuestExecutionBackendFailure();
	std::atomic_uint winners {0};
	std::array<std::thread, 8> writers;
	for (uint32_t index = 0; index < writers.size(); ++index)
		writers[index] = std::thread([&, index] {
			if (RecordGuestExecutionBackendFailure(1 + index)) winners.fetch_add(1, std::memory_order_relaxed);
		});
	for (auto& writer: writers) writer.join();
	const auto first = GuestExecutionBackendFailure();
	if (winners.load() != 1 || first < 1 || first > writers.size()) return false;
	set_execution_result(true, first, &result);
	if (result.code != LUMEN_RUNTIME_RESULT_INTERNAL_ERROR_V1) return false;
	ResetGuestExecutionBackendFailure();
	set_execution_result(true, GuestExecutionBackendFailure(), &result);
	if (result.code != LUMEN_RUNTIME_RESULT_OK_V1) return false;
	std::fprintf(stderr, "[RUNTIME_BACKEND_FAILURE_TEST] clean-return/stop=pass zero=ignored first-failure=sticky"
	    " concurrent-writers=8 winners=1 backend-failure=internal-error script-error=preserved reset=clean\n");
	return true;
}
#endif

extern "C" LumenRuntimeCapabilitiesV1 LumenRuntimeGetCapabilitiesV1(void)
{
	LumenRuntimeCapabilitiesV1 capabilities {};
	capabilities.structSize                    = sizeof(capabilities);
	capabilities.abiVersion                    = LUMEN_RUNTIME_ABI_VERSION_V1;
	capabilities.cpuVirtualMemoryAvailable     = 1u;
	capabilities.loaderAvailable               = 1u;
	capabilities.hleAvailable                  = 1u;
	capabilities.graphicsAvailable             = 1u;
	capabilities.audioAvailable                = 1u;
	capabilities.inputAvailable                = 1u;
	std::string host_error;
	capabilities.romLaunchAvailable = runtime_host_configuration_supported(&host_error) ? 1u : 0u;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	capabilities.launchMustRunOnMainThread = 0u;
#else
	capabilities.launchMustRunOnMainThread = 1u;
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	capabilities.launchIsOneShot               = 0u;
#else
	capabilities.launchIsOneShot               = 1u;
#endif
	capabilities.jitEnabled = runtime_jit_enabled() ? 1u : 0u;
	return capabilities;
}

extern "C" LumenRuntimeResultCodeV1 LumenRuntimePreflightV1(const LumenRuntimeLaunchRequestV1* request,
	                                                      LumenRuntimeResultV1* result)
{
	if (result == nullptr)
	{
		return LUMEN_RUNTIME_RESULT_INVALID_ARGUMENT_V1;
	}
	set_result(result, LUMEN_RUNTIME_RESULT_INTERNAL_ERROR_V1, "Runtime preflight did not complete.");

	std::string error;
	const auto  header_code = validate_request_header(request, &error);
	if (header_code != LUMEN_RUNTIME_RESULT_OK_V1)
	{
		set_result(result, header_code, error);
		return header_code;
	}
	if (!runtime_host_configuration_supported(&error))
	{
		set_result(result, LUMEN_RUNTIME_RESULT_PREFLIGHT_FAILED_V1, error);
		emit_event(request, LUMEN_RUNTIME_EVENT_FAILED_V1, 0u, result->code, result->message);
		return result->code;
	}

	emit_event(request, LUMEN_RUNTIME_EVENT_PREFLIGHT_STARTED_V1, 0u, LUMEN_RUNTIME_RESULT_OK_V1,
	           "Validating runtime launch paths.");
	ValidatedRequest validated;
	if (!validate_paths(request, &validated, &error))
	{
		set_result(result, LUMEN_RUNTIME_RESULT_PREFLIGHT_FAILED_V1, error);
		emit_event(request, LUMEN_RUNTIME_EVENT_FAILED_V1, 0u, result->code, result->message);
		return result->code;
	}

	set_result(result, LUMEN_RUNTIME_RESULT_OK_V1, "Runtime preflight succeeded.");
	emit_event(request, LUMEN_RUNTIME_EVENT_PREFLIGHT_SUCCEEDED_V1, 1000u, result->code, result->message);
	return result->code;
}

extern "C" LumenRuntimeResultCodeV1 LumenRuntimeLaunchV1(const LumenRuntimeLaunchRequestV1* request, LumenRuntimeResultV1* result)
{
	if (result == nullptr)
	{
		return LUMEN_RUNTIME_RESULT_INVALID_ARGUMENT_V1;
	}
	set_result(result, LUMEN_RUNTIME_RESULT_INTERNAL_ERROR_V1, "Runtime launch did not complete.");

	std::string error;
	const auto  header_code = validate_request_header(request, &error);
	if (header_code != LUMEN_RUNTIME_RESULT_OK_V1)
	{
		set_result(result, header_code, error);
		return header_code;
	}
	if (!runtime_host_configuration_supported(&error))
	{
		set_result(result, LUMEN_RUNTIME_RESULT_PREFLIGHT_FAILED_V1, error);
		emit_event(request, LUMEN_RUNTIME_EVENT_FAILED_V1, 0u, result->code, result->message);
		return result->code;
	}
#if LUMEN_PLATFORM != LUMEN_PLATFORM_ANDROID
	if (!is_process_main_thread())
	{
		set_result(result, LUMEN_RUNTIME_RESULT_NOT_MAIN_THREAD_V1, "Runtime launch must be called from the process main thread.");
		emit_event(request, LUMEN_RUNTIME_EVENT_FAILED_V1, 0u, result->code, result->message);
		return result->code;
	}
#endif

	uint32_t expected = static_cast<uint32_t>(LaunchState::Ready);
	if (!g_launch_state.compare_exchange_strong(expected, static_cast<uint32_t>(LaunchState::Launching)))
	{
		const auto code = expected == static_cast<uint32_t>(LaunchState::Launching) ? LUMEN_RUNTIME_RESULT_LAUNCH_BUSY_V1
		                                                                          : LUMEN_RUNTIME_RESULT_ALREADY_LAUNCHED_V1;
		set_result(result, code, code == LUMEN_RUNTIME_RESULT_LAUNCH_BUSY_V1 ? "A runtime launch is already in progress."
		                                                                  : "The runtime is unavailable because its previous lifecycle did not reset safely.");
		emit_event(request, LUMEN_RUNTIME_EVENT_FAILED_V1, 0u, result->code, result->message);
		return result->code;
	}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	// iOS keeps the emulator in-process across launches. The previous session is
	// quiescent before the state machine returns to Ready, so this is the sole
	// safe boundary at which to recycle the fixed startup-trace buffer.
	Lumen::Emulator::StartupTrace::Reset();
#endif

	emit_event(request, LUMEN_RUNTIME_EVENT_PREFLIGHT_STARTED_V1, 0u, LUMEN_RUNTIME_RESULT_OK_V1,
	           "Validating runtime launch paths.");
	ValidatedRequest validated;
	if (!validate_paths(request, &validated, &error))
	{
		set_result(result, LUMEN_RUNTIME_RESULT_PREFLIGHT_FAILED_V1, error);
		emit_event(request, LUMEN_RUNTIME_EVENT_FAILED_V1, 0u, result->code, result->message);
		g_launch_state.store(static_cast<uint32_t>(LaunchState::Ready));
		return result->code;
	}
	emit_event(request, LUMEN_RUNTIME_EVENT_PREFLIGHT_SUCCEEDED_V1, 150u, LUMEN_RUNTIME_RESULT_OK_V1,
	           "Runtime preflight succeeded.");

	WorkingDirectoryGuard working_directory;
	if (!working_directory.Change(validated.data_root, &error))
	{
		set_result(result, LUMEN_RUNTIME_RESULT_WORKING_DIRECTORY_FAILED_V1, error);
		emit_event(request, LUMEN_RUNTIME_EVENT_FAILED_V1, 150u, result->code, result->message);
		g_launch_state.store(static_cast<uint32_t>(LaunchState::Ready));
		return result->code;
	}

	auto* subsystems = Lumen::Core::SubsystemsList::Instance();
	bool  initialize_process = true;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	initialize_process = !g_process_initialized;
#endif
	if (initialize_process)
	{
		SDL_SetMainReady();
		Lumen::Core::mem_set_max_size(static_cast<size_t>(2048) * 1024u * 1024u - 1u);

		static char  executable_name[] = "lumen-in-process-runtime";
		static char* arguments[]       = {executable_name, nullptr};
		subsystems->SetArgs(1, arguments);

		auto* core     = Lumen::Core::CoreSubsystem::Instance();
		auto* scripts  = Lumen::Scripts::ScriptsSubsystem::Instance();
		auto* math     = Lumen::Math::MathSubsystem::Instance();
		auto* sdl      = Lumen::Core::SDLSubsystem::Instance();
		auto* threads  = Lumen::Core::ThreadsSubsystem::Instance();
		auto* emulator = Lumen::Emulator::EmulatorSubsystem::Instance();

		subsystems->Add(core, {});
		subsystems->Add(scripts, {core});
		subsystems->Add(math, {core});
		subsystems->Add(sdl, {core});
		subsystems->Add(threads, {core, sdl});
		subsystems->Add(emulator, {core, scripts});

		emit_event(request, LUMEN_RUNTIME_EVENT_INITIALIZATION_STARTED_V1, 200u, LUMEN_RUNTIME_RESULT_OK_V1,
		           "Initializing in-process runtime subsystems.");
		if (!subsystems->InitAll(false))
		{
			std::string message("Runtime subsystem initialization failed");
			if (subsystems->GetFailName() != nullptr)
			{
				message += " in ";
				message += subsystems->GetFailName();
			}
			if (subsystems->GetFailMsg() != nullptr && subsystems->GetFailMsg()[0] != '\0')
			{
				message += ": ";
				message += subsystems->GetFailMsg();
			}
			set_result(result, LUMEN_RUNTIME_RESULT_SUBSYSTEM_INITIALIZATION_V1, message);
			subsystems->DestroyAll(false);
			(void)restore_working_directory(&working_directory, result);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
			g_launch_state.store(static_cast<uint32_t>(LaunchState::Poisoned));
#else
			g_launch_state.store(static_cast<uint32_t>(LaunchState::Finished));
#endif
			emit_event(request, LUMEN_RUNTIME_EVENT_FAILED_V1, 200u, result->code, result->message);
			return result->code;
		}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
		g_process_initialized = true;
#endif
		emit_event(request, LUMEN_RUNTIME_EVENT_INITIALIZATION_SUCCEEDED_V1, 300u, LUMEN_RUNTIME_RESULT_OK_V1,
		           "In-process runtime subsystems initialized.");
	} else
	{
		emit_event(request, LUMEN_RUNTIME_EVENT_INITIALIZATION_SUCCEEDED_V1, 300u, LUMEN_RUNTIME_RESULT_OK_V1,
		           "In-process runtime subsystems are ready.");
	}

	Lumen::Scripts::SetGlobalString("lumen_runtime_install_root_v1",
	                               Lumen::Core::String::FromUtf8(validated.install_root.u8string().c_str()));
	Lumen::Scripts::SetGlobalString("lumen_runtime_data_root_v1", Lumen::Core::String::FromUtf8(validated.data_root.u8string().c_str()));
	Lumen::Scripts::SetGlobalInteger("lumen_runtime_width_v1", request->width);
	Lumen::Scripts::SetGlobalInteger("lumen_runtime_height_v1", request->height);
	Lumen::Scripts::SetGlobalBoolean("lumen_runtime_load_libc_v1", validated.load_libc);
	Lumen::Scripts::SetGlobalBoolean("lumen_runtime_load_fios2_v1", validated.load_fios2);

	bool script_ok = run_script_phase(request, LUMEN_RUNTIME_EVENT_CONFIGURING_V1, 350u, "Configuring emulator", ConfigureScript, result) &&
	                 run_script_phase(request, LUMEN_RUNTIME_EVENT_MOUNTING_V1, 450u, "Mounting installation", MountScript, result) &&
	                 run_script_phase(request, LUMEN_RUNTIME_EVENT_LOADING_EXECUTABLE_V1, 550u, "Loading eboot.bin", LoadExecutableScript,
	                                  result) &&
	                 run_script_phase(request, LUMEN_RUNTIME_EVENT_LOADING_MODULES_V1, 650u, "Loading selected system modules",
	                                  LoadModulesScript, result) &&
	                 run_script_phase(request, LUMEN_RUNTIME_EVENT_LINKING_V1, 750u, "Resolving runtime symbols", LinkScript, result) &&
	                 run_script_phase(request, LUMEN_RUNTIME_EVENT_EXECUTION_STARTED_V1, 850u, "Executing guest", ExecuteScript, result);

	set_execution_result(script_ok, Lumen::Loader::GuestExecutionBackendFailure(), result);

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	const bool session_reset = Lumen::Emulator::IOSRuntimeResetForNextSession();
	if (!session_reset)
	{
		if (result->code == LUMEN_RUNTIME_RESULT_OK_V1)
		{
			set_result(result, LUMEN_RUNTIME_RESULT_INTERNAL_ERROR_V1,
			           "Runtime execution ended, but the session could not be reset safely.");
		} else
		{
			append_result_message(result, "The runtime session could not be reset safely.");
			result->code = LUMEN_RUNTIME_RESULT_INTERNAL_ERROR_V1;
		}
	}
	const bool directory_restored = restore_working_directory(&working_directory, result);
	if (!directory_restored)
	{
		// A failed restore poisons the process-lifetime runtime even when an
		// earlier script error supplied the original result code. Keep the
		// detailed message, but do not advertise a retry that the state machine
		// will reject.
		result->code = LUMEN_RUNTIME_RESULT_INTERNAL_ERROR_V1;
	}
	// Close the current startup epoch before publishing Ready. The final event
	// callback is synchronous and may immediately begin another launch, whose
	// Reset() must never erase records from this session.
	Lumen::Emulator::StartupTrace::Flush();
	g_launch_state.store(static_cast<uint32_t>(session_reset && directory_restored ? LaunchState::Ready : LaunchState::Poisoned));
#else
	/* Process-terminal runtimes destroy their subsystems after execution. */
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	if (!Lumen::Libs::Graphics::VertexBufferReportDynamicHostVisibleFinal())
	{
		if (result->code == LUMEN_RUNTIME_RESULT_OK_V1)
		{
			set_result(result, LUMEN_RUNTIME_RESULT_INTERNAL_ERROR_V1,
			           "Runtime execution ended with invalid dynamic vertex upload telemetry.");
		} else
		{
			append_result_message(result, "Dynamic vertex upload telemetry was invalid.");
			result->code = LUMEN_RUNTIME_RESULT_INTERNAL_ERROR_V1;
		}
	}
#endif
	subsystems->DestroyAll(false);
	(void)restore_working_directory(&working_directory, result);
	g_launch_state.store(static_cast<uint32_t>(LaunchState::Finished));
#endif

	if (script_ok && result->code == LUMEN_RUNTIME_RESULT_OK_V1)
	{
		emit_event(request, LUMEN_RUNTIME_EVENT_FINISHED_V1, 1000u, result->code, result->message);
	} else
	{
		emit_event(request, LUMEN_RUNTIME_EVENT_FAILED_V1, 850u, result->code, result->message);
	}
	return result->code;
}
