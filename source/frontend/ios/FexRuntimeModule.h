// Lumen iOS runtime module boundary. SPDX-License-Identifier: GPL-2.0-only
#ifndef LUMEN_FRONTEND_IOS_FEX_RUNTIME_MODULE_H_
#define LUMEN_FRONTEND_IOS_FEX_RUNTIME_MODULE_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LumenIOSFexModuleHostV1
{
	uint32_t structSize;
	uint32_t abiVersion;
	void* context;
	// uiWindow is the module's UIKit UIWindow. Return zero after the host HUD
	// and dashboard handoff succeeded; nonzero rejects the runtime surface.
	uint32_t (*windowReady)(void* context, void* uiWindow);
	void (*physicalControllerChanged)(void* context, uint32_t connected);
	void (*framePresented)(void* context, uint64_t frameNumber);
	void (*progress)(void* context, uint32_t permille, const char* message);
} LumenIOSFexModuleHostV1;

// Each console framework owns its CPU and engine dependencies. Resolve these
// symbols on that framework's local dlopen handle, never with RTLD_DEFAULT.
uint32_t LumenIOSFexModuleAbiV1(void);     // Must return 1.
uint32_t LumenIOSFexModuleConsoleV1(void); // Must return 4 (PS4) or 5 (PS5).
// Copies the host table before Probe/Launch; zero means accepted. All callbacks
// stay valid until process exit. uiWindow is never retained by the module ABI.
uint32_t LumenIOSFexModuleSetHostV1(const LumenIOSFexModuleHostV1* host);

// Probe maps, writes and executes a tiny JIT fixture in the current process;
// zero means that executable-memory admission succeeded. Engine/context and
// guest initialization belong to Launch. A store channel alone grants nothing.
uint32_t LumenIOSFexModuleProbeV1(void);

enum LumenIOSFexJitProviderV1 {
	LumenIOSFexJitProviderUniversalV1 = 1,
};
// Additive ABI: the host starts its universal-protocol helper and waits for an
// actual debugger attachment before calling this on a serial background queue.
// Returns zero only after preparing the arena, executing/rewriting code,
// detaching, and executing/rewriting code again. No environment flag is needed.
uint32_t LumenIOSFexModulePrepareJitV1(uint32_t provider);

// Main-thread, synchronous, one-shot launch. Paths are canonical absolute
// directories; profile is "eboot-only" or "known-system". A zero result means
// the session returned normally. The framework must never terminate the app.
int LumenIOSFexModuleLaunchV1(const char* root, const char* data, const char* profile);
void LumenIOSFexModuleRequestStopV1(void);

// Stable ordinals match LumenVirtualPadButton (0..13) and LumenVirtualPadAxis
// (0..5), with uint8 axis values (sticks rest at 128, triggers at 0).
void LumenIOSFexModuleSetButtonV1(uint32_t button, uint32_t pressed);
void LumenIOSFexModuleSetAxisV1(uint32_t axis, uint32_t value);

// Optional PS4 export. Returns one while an on-screen IME owns gamepad input.
uint32_t LumenIOSFexModuleImeActiveV1(void);

// Borrowed module-owned UTF-8 text, copied by the caller before another call.
const char* LumenIOSFexModuleDiagnosticV1(void);

#ifdef __cplusplus
}
#endif

#endif
