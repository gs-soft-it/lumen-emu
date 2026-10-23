#include "Emulator/Config.h"

#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/MagicEnum.h"
#include "Lumen/Scripts/Scripts.h"

#include <cstdlib>
#include <cstring>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Config {

struct Config
{
	uint32_t               screen_width                = 1920;
	uint32_t               screen_height               = 1080;
	uint32_t               window_width                = 0;
	uint32_t               window_height               = 0;
	bool                   window_high_dpi             = false;
	bool                   neo                         = true;
	bool                   vulkan_validation_enabled   = false;
	bool                   shader_validation_enabled   = false;
	ShaderOptimizationType shader_optimization_type    = ShaderOptimizationType::None;
	ShaderLogDirection     shader_log_direction        = ShaderLogDirection::Silent;
	String                 shader_log_folder           = U"_Shaders";
	bool                   command_buffer_dump_enabled = false;
	String                 command_buffer_dump_folder  = U"_Buffers";
	Log::Direction         printf_direction            = Log::Direction::Console;
	String                 printf_output_file          = U"_lumen.txt";
	String                 printf_output_folder        = U"_Logs";
	ProfilerDirection      profiler_direction          = ProfilerDirection::None;
	String                 profiler_output_file        = U"_profile.prof";
	bool                   spirv_debug_printf_enabled  = false;
	bool                   diagnostics_enabled         = false;
	bool                   pipeline_dump_enabled       = false;
	String                 pipeline_dump_folder        = U"_Pipelines";
};

static Config* g_config = nullptr;

LUMEN_SUBSYSTEM_INIT(Config)
{
	EXIT_IF(g_config != nullptr);

	g_config = new Config;
}

LUMEN_SUBSYSTEM_UNEXPECTED_SHUTDOWN(Config) {}

LUMEN_SUBSYSTEM_DESTROY(Config) {}

template <class T>
void LoadInt(T& dst, const Scripts::ScriptVar& cfg, const String& key)
{
	auto var = cfg.At(key);
	if (!var.IsNil())
	{
		dst = static_cast<T>(var.ToInteger());
	}
}

void LoadBool(bool& dst, const Scripts::ScriptVar& cfg, const String& key)
{
	auto var = cfg.At(key);
	if (!var.IsNil())
	{
		dst = var.ToBool();
	}
}

template <class T>
void LoadEnum(T& dst, const Scripts::ScriptVar& cfg, const String& key)
{
	auto var = cfg.At(key);
	if (!var.IsNil())
	{
		dst = Core::EnumValue(var.ToString(), dst);
	}
}

void LoadStr(String& dst, const Scripts::ScriptVar& cfg, const String& key)
{
	auto var = cfg.At(key);
	if (!var.IsNil())
	{
		dst = var.ToString();
	}
}

void Load(const Scripts::ScriptVar& cfg)
{
	LoadInt(g_config->screen_width, cfg, U"ScreenWidth");
	LoadInt(g_config->screen_height, cfg, U"ScreenHeight");
	LoadInt(g_config->window_width, cfg, U"WindowWidth");
	LoadInt(g_config->window_height, cfg, U"WindowHeight");
	LoadBool(g_config->window_high_dpi, cfg, U"WindowHighDpi");
	LoadBool(g_config->neo, cfg, U"Neo");
	LoadBool(g_config->vulkan_validation_enabled, cfg, U"VulkanValidationEnabled");
	LoadBool(g_config->shader_validation_enabled, cfg, U"ShaderValidationEnabled");
	LoadEnum(g_config->shader_optimization_type, cfg, U"ShaderOptimizationType");
	LoadEnum(g_config->shader_log_direction, cfg, U"ShaderLogDirection");
	LoadStr(g_config->shader_log_folder, cfg, U"ShaderLogFolder");
	LoadBool(g_config->command_buffer_dump_enabled, cfg, U"CommandBufferDumpEnabled");
	LoadStr(g_config->command_buffer_dump_folder, cfg, U"CommandBufferDumpFolder");
	LoadEnum(g_config->printf_direction, cfg, U"PrintfDirection");
	LoadStr(g_config->printf_output_file, cfg, U"PrintfOutputFile");
	LoadStr(g_config->printf_output_folder, cfg, U"PrintfOutputFolder");
	LoadEnum(g_config->profiler_direction, cfg, U"ProfilerDirection");
	LoadStr(g_config->profiler_output_file, cfg, U"ProfilerOutputFile");
	LoadBool(g_config->spirv_debug_printf_enabled, cfg, U"SpirvDebugPrintfEnabled");
	LoadBool(g_config->diagnostics_enabled, cfg, U"DiagnosticsEnabled");
	LoadBool(g_config->pipeline_dump_enabled, cfg, U"PipelineDumpEnabled");
	LoadStr(g_config->pipeline_dump_folder, cfg, U"PipelineDumpFolder");
}

void BeginSession()
{
	EXIT_IF(g_config == nullptr);
	g_config->window_width    = 0;
	g_config->window_height   = 0;
	g_config->window_high_dpi = false;
}

uint32_t GetScreenWidth()
{
	return g_config->screen_width;
}

uint32_t GetScreenHeight()
{
	return g_config->screen_height;
}

uint32_t GetWindowWidth()
{
	return g_config->window_width;
}

uint32_t GetWindowHeight()
{
	return g_config->window_height;
}

bool WindowHighDpiEnabled()
{
	return g_config->window_high_dpi;
}

bool IsNeo()
{
	return g_config->neo;
}

bool VulkanValidationEnabled()
{
	return g_config->vulkan_validation_enabled;
}

bool ShaderValidationEnabled()
{
	return g_config->shader_validation_enabled;
}

ShaderOptimizationType GetShaderOptimizationType()
{
	return g_config->shader_optimization_type;
}

ShaderLogDirection GetShaderLogDirection()
{
	return g_config->shader_log_direction;
}

String GetShaderLogFolder()
{
	return g_config->shader_log_folder;
}

bool CommandBufferDumpEnabled()
{
	return g_config->command_buffer_dump_enabled;
}

String GetCommandBufferDumpFolder()
{
	return g_config->command_buffer_dump_folder;
}

Log::Direction GetPrintfDirection()
{
	return g_config->printf_direction;
}

String GetPrintfOutputFile()
{
	return g_config->printf_output_file;
}

String GetPrintfOutputFolder()
{
	return g_config->printf_output_folder;
}

ProfilerDirection GetProfilerDirection()
{
	return g_config->profiler_direction;
}

String GetProfilerOutputFile()
{
	return g_config->profiler_output_file;
}

bool SpirvDebugPrintfEnabled()
{
	return g_config->spirv_debug_printf_enabled;
}

bool DiagnosticsEnabled()
{
	return g_config->diagnostics_enabled;
}

bool GraphicsTimingEnabled()
{
	static const bool trace_enabled = [] {
		const char* value = std::getenv("LUMEN_GFX_TIMING_TRACE");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return DiagnosticsEnabled() || trace_enabled;
}

bool PipelineDumpEnabled()
{
	return g_config->pipeline_dump_enabled;
}

String GetPipelineDumpFolder()
{
	return g_config->pipeline_dump_folder;
}

} // namespace Lumen::Config

#endif // LUMEN_EMU_ENABLED
