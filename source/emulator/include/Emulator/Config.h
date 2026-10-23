#ifndef EMULATOR_INCLUDE_EMULATOR_CONFIG_H_
#define EMULATOR_INCLUDE_EMULATOR_CONFIG_H_

#include "Lumen/Core/Common.h"
#include "Lumen/Core/String.h"
#include "Lumen/Core/Subsystems.h"

#include "Emulator/Common.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Scripts {
class ScriptVar;
} // namespace Lumen::Scripts

namespace Lumen::Config {

LUMEN_SUBSYSTEM_DEFINE(Config);

enum class ShaderOptimizationType
{
	None,
	Size,
	Performance
};

enum class ShaderLogDirection
{
	Silent,
	Console,
	File
};

enum class ProfilerDirection
{
	None,
	File,
	Network,
	FileAndNetwork
};

void Load(const Scripts::ScriptVar& cfg);

// Reset title-derived configuration before loading a new guest. Persistent
// user settings remain intact.
void BeginSession();

uint32_t GetScreenWidth();
uint32_t GetScreenHeight();
uint32_t GetWindowWidth();
uint32_t GetWindowHeight();
bool     WindowHighDpiEnabled();
bool     IsNeo();
bool     VulkanValidationEnabled();

bool                   ShaderValidationEnabled();
ShaderOptimizationType GetShaderOptimizationType();
ShaderLogDirection     GetShaderLogDirection();
String                 GetShaderLogFolder();

bool   CommandBufferDumpEnabled();
String GetCommandBufferDumpFolder();

Log::Direction GetPrintfDirection();
String         GetPrintfOutputFile();
String         GetPrintfOutputFolder();

ProfilerDirection GetProfilerDirection();
String            GetProfilerOutputFile();

bool SpirvDebugPrintfEnabled();
bool DiagnosticsEnabled();
bool GraphicsTimingEnabled();

bool   PipelineDumpEnabled();
String GetPipelineDumpFolder();

} // namespace Lumen::Config

#endif

#endif /* EMULATOR_INCLUDE_EMULATOR_CONFIG_H_ */
