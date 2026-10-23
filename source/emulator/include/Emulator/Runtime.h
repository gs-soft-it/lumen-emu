#ifndef EMULATOR_INCLUDE_EMULATOR_RUNTIME_H_
#define EMULATOR_INCLUDE_EMULATOR_RUNTIME_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LUMEN_RUNTIME_ABI_VERSION_V1             1u
#define LUMEN_RUNTIME_RESULT_MESSAGE_CAPACITY_V1 512u
#define LUMEN_RUNTIME_EVENT_MESSAGE_CAPACITY_V1  192u

/* Fixed-width values are used instead of C enums so the public ABI does not
 * depend on the compiler's enum representation. */
typedef uint32_t LumenRuntimeProfileV1;
enum
{
	LUMEN_RUNTIME_PROFILE_EBOOT_ONLY_V1  = 0u,
	LUMEN_RUNTIME_PROFILE_KNOWN_SYSTEM_V1 = 1u
};

typedef uint32_t LumenRuntimeResultCodeV1;
enum
{
	LUMEN_RUNTIME_RESULT_OK_V1                         = 0u,
	LUMEN_RUNTIME_RESULT_INVALID_ARGUMENT_V1           = 1u,
	LUMEN_RUNTIME_RESULT_ABI_MISMATCH_V1               = 2u,
	LUMEN_RUNTIME_RESULT_NOT_MAIN_THREAD_V1            = 3u,
	LUMEN_RUNTIME_RESULT_PREFLIGHT_FAILED_V1            = 4u,
	LUMEN_RUNTIME_RESULT_LAUNCH_BUSY_V1                 = 5u,
	LUMEN_RUNTIME_RESULT_ALREADY_LAUNCHED_V1            = 6u,
	LUMEN_RUNTIME_RESULT_WORKING_DIRECTORY_FAILED_V1    = 7u,
	LUMEN_RUNTIME_RESULT_SUBSYSTEM_INITIALIZATION_V1    = 8u,
	LUMEN_RUNTIME_RESULT_SCRIPT_FILE_ERROR_V1           = 9u,
	LUMEN_RUNTIME_RESULT_SCRIPT_SYNTAX_ERROR_V1         = 10u,
	LUMEN_RUNTIME_RESULT_SCRIPT_RUN_ERROR_V1            = 11u,
	LUMEN_RUNTIME_RESULT_SCRIPT_UNKNOWN_ERROR_V1        = 12u,
	LUMEN_RUNTIME_RESULT_INTERNAL_ERROR_V1              = 13u
};

typedef uint32_t LumenRuntimeEventTypeV1;
enum
{
	LUMEN_RUNTIME_EVENT_PREFLIGHT_STARTED_V1       = 0u,
	LUMEN_RUNTIME_EVENT_PREFLIGHT_SUCCEEDED_V1     = 1u,
	LUMEN_RUNTIME_EVENT_INITIALIZATION_STARTED_V1  = 2u,
	LUMEN_RUNTIME_EVENT_INITIALIZATION_SUCCEEDED_V1 = 3u,
	LUMEN_RUNTIME_EVENT_CONFIGURING_V1             = 4u,
	LUMEN_RUNTIME_EVENT_MOUNTING_V1                = 5u,
	LUMEN_RUNTIME_EVENT_LOADING_EXECUTABLE_V1      = 6u,
	LUMEN_RUNTIME_EVENT_LOADING_MODULES_V1         = 7u,
	LUMEN_RUNTIME_EVENT_LINKING_V1                 = 8u,
	LUMEN_RUNTIME_EVENT_EXECUTION_STARTED_V1       = 9u,
	LUMEN_RUNTIME_EVENT_FINISHED_V1                = 10u,
	LUMEN_RUNTIME_EVENT_FAILED_V1                  = 11u
};

typedef struct LumenRuntimeCapabilitiesV1
{
	uint32_t structSize;
	uint32_t abiVersion;
	uint32_t cpuVirtualMemoryAvailable;
	uint32_t loaderAvailable;
	uint32_t hleAvailable;
	uint32_t graphicsAvailable;
	uint32_t audioAvailable;
	uint32_t inputAvailable;
	uint32_t romLaunchAvailable;
	uint32_t launchMustRunOnMainThread;
	uint32_t launchIsOneShot;
	uint32_t jitEnabled;
	uint32_t reserved[4];
} LumenRuntimeCapabilitiesV1;

typedef struct LumenRuntimeResultV1
{
	uint32_t                structSize;
	uint32_t                abiVersion;
	LumenRuntimeResultCodeV1 code;
	char                     message[LUMEN_RUNTIME_RESULT_MESSAGE_CAPACITY_V1];
} LumenRuntimeResultV1;

typedef struct LumenRuntimeEventV1
{
	uint32_t                structSize;
	uint32_t                abiVersion;
	LumenRuntimeEventTypeV1  type;
	uint32_t                progressPermille;
	LumenRuntimeResultCodeV1 code;
	char                     message[LUMEN_RUNTIME_EVENT_MESSAGE_CAPACITY_V1];
} LumenRuntimeEventV1;

/* Events are delivered synchronously on the thread calling preflight/launch.
 * The callback must return normally and must not retain the event pointer. */
typedef void (*LumenRuntimeEventCallbackV1)(const LumenRuntimeEventV1* event, void* userData);

typedef struct LumenRuntimeLaunchRequestV1
{
	uint32_t                     structSize;
	uint32_t                     abiVersion;
	const char*                  installPath;
	const char*                  dataPath;
	LumenRuntimeProfileV1         profile;
	uint32_t                     width;
	uint32_t                     height;
	LumenRuntimeEventCallbackV1   callback;
	void*                        callbackUserData;
} LumenRuntimeLaunchRequestV1;

LumenRuntimeCapabilitiesV1 LumenRuntimeGetCapabilitiesV1(void);

/* Preflight is repeatable and does not initialize emulator subsystems. */
LumenRuntimeResultCodeV1 LumenRuntimePreflightV1(const LumenRuntimeLaunchRequestV1* request, LumenRuntimeResultV1* result);

/* Launch is synchronous and process-wide. On iOS it can be called again only
 * after the preceding session completed its full reset; a reset failure keeps
 * the runtime fail-closed. Other platforms retain the one-shot contract.
 * Android launches from the SDL native thread; other platforms require the
 * process main thread as reported by the capability structure. */
LumenRuntimeResultCodeV1 LumenRuntimeLaunchV1(const LumenRuntimeLaunchRequestV1* request, LumenRuntimeResultV1* result);

#ifdef __cplusplus
} // extern "C"
#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
bool LumenRuntimeExecutionFailureContractTest();
#endif
#endif

#endif // EMULATOR_INCLUDE_EMULATOR_RUNTIME_H_
