#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_

#include "Lumen/Core/Common.h"

#include "Emulator/Common.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics {

class CommandBuffer;
struct LabelScalarRecordingIdentity;
class CommandProcessor;
struct GraphicContext;

void GraphicsRunInit();
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
bool GraphicsRunResetIOSRuntimeSession();
#endif

void     GraphicsRunSubmit(uint32_t* cmd_draw_buffer, uint32_t num_draw_dw, uint32_t* cmd_const_buffer, uint32_t num_const_dw);
void     GraphicsRunSubmitAndFlip(uint32_t* cmd_draw_buffer, uint32_t num_draw_dw, uint32_t* cmd_const_buffer, uint32_t num_const_dw,
                                  int handle, int index, int flip_mode, int64_t flip_arg);
uint32_t GraphicsRunMapComputeQueue(uint32_t pipe_id, uint32_t queue_id, uint32_t* ring_addr, uint32_t ring_size_dw,
                                    uint32_t* read_ptr_addr);
void     GraphicsRunUnmapComputeQueue(uint32_t id);
void     GraphicsRunWait();
void     GraphicsRunFlushMemory(GraphicContext* ctx);
void     GraphicsRunDone();
bool     GraphicsRunReportRootOwnershipFinal();
void     GraphicsRunDingDong(uint32_t ring_id, uint32_t offset_dw);
int      GraphicsRunGetFrameNum();
bool     GraphicsRunAreSubmitsAllowed();

// Emits bounded command-stream provenance for the descriptor currently being
// parsed. This is a no-op unless LUMEN_GFX_COMMAND_PROVENANCE is enabled.
void GraphicsRunDumpDescriptorProvenance(const char* stage, uint32_t start_register, uint32_t register_count);
bool GraphicsRunCurrentCommandRootOwned();

void GraphicsRunCommandProcessorLock(CommandProcessor* cp);
void GraphicsRunCommandProcessorUnlock(CommandProcessor* cp);
void GraphicsRunCommandProcessorFlush(CommandProcessor* cp);
// Submit a shader-write -> host-read dependency before the existing fence wait.
void GraphicsRunCommandProcessorFlushForHostRead(CommandProcessor* cp);
void GraphicsRunCommandProcessorWait(CommandProcessor* cp);
// Complete the exact successfully submitted queue/session prefix, retaining
// later independent submissions and the current recording. False grants no
// completion proof: callers retain the ordinary full-wait fallback.
bool GraphicsRunCommandProcessorWaitForPrefix(CommandProcessor* cp, int queue, uint64_t session_epoch,
                                             uint64_t recording_submit_serial);
// Query the live contiguous prefix of successful native submissions while
// holding the processor lock. Begin assignment/cutoffs alone grant no proof;
// a submitted recording may still be incomplete and grants no content seal.
bool GraphicsRunCommandProcessorSubmitted(CommandProcessor* cp, int queue, uint64_t session_epoch,
                                         uint64_t recording_submit_serial);
// Query the current completion timeline while holding the processor lock.
// Unlike a recording's Begin cutoff this includes fences waited afterwards.
bool GraphicsRunCommandProcessorCompleted(CommandProcessor* cp, int queue, uint64_t session_epoch,
                                         uint64_t recording_submit_serial);
// EOP CPU-only continuation keeps RunLock and the original completed prefix.
bool GraphicsRunCommandProcessorEopResumeValid(CommandProcessor* cp, int queue, uint64_t session_epoch,
                                              uint64_t recording_submit_serial);
// The caller owns CP's lock and keeps its batch RunLock across any pause. This
// proves exclusive graphics recording ownership, without requiring completion;
// compare the caller's frozen recording identity separately after every pause.
// The buffer pointer is matched to the current slot before it is dereferenced.
bool GraphicsRunCommandProcessorExclusiveRecordingValid(CommandProcessor* cp, CommandBuffer* buffer);
// Current recording is protected by the calling draw/dispatch's retained CP
// mutex, even with concurrent CE/DE streams. No submission/flush authority is
// granted; releasing CP revokes the scoped witness until the next draw.
bool GraphicsRunCommandProcessorRetainedRecordingValid(CommandProcessor* cp, CommandBuffer* buffer);
// Synchronous draw reservations additionally retain the existing exclusive
// memory epoch whenever CPU/GPU handoff is enabled. This never admits a gap.
bool GraphicsRunDrawAccessReservationScopeValid(CommandProcessor* cp, CommandBuffer* buffer);
// Caller owns CP. A foreign/stale buffer is rejected before dereference; only
// the exclusive packet parser under the ring's RunLock returns a valid tuple.
LabelScalarRecordingIdentity GraphicsRunCommandProcessorScalarLabelIdentity(CommandProcessor* cp, CommandBuffer* buffer);
// Initial Label capture also admits a DCB/CCB batch, with CP kept locked
// throughout every pause. This tuple grants no EOP or scalar completion scope.
LabelScalarRecordingIdentity GraphicsRunCommandProcessorDeferredLabelIdentity(CommandProcessor* cp, CommandBuffer* buffer);
// The caller owns the command-processor lock. Writeback may rotate the bounded
// pool, so CPU-copy code resolves the current recording only afterwards.
CommandBuffer* GraphicsRunCommandProcessorGetCurrentBuffer(CommandProcessor* cp);

// Embedded data marker tracking - for storage buffer address resolution
uint64_t GetLastEmbeddedDataAddress();
uint32_t GetLastEmbeddedDataSize();
bool HasValidEmbeddedData();

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool GraphicsRunStateSynchronizationTest();
bool GraphicsRunParserPauseProofContractTest();
bool GraphicsRunDeferredLabelCaptureScopeContractTest();
bool GraphicsRunDx10ClampBitExtractionContractTest();
bool GraphicsRunConstRamBoundsContractTest();
bool GraphicsRunDmaDataMemoryCopyContractTest();
bool GraphicsRunDmaDataGdsReadContractTest();
// Uses the production exclusive-epoch release/reacquire boundary with an
// empty native prefix. The callback runs with no borrowed guest-memory lease.
// This is a FEX admission gate, not a headless substitute for a Vulkan draw.
bool GraphicsRunOwnedPreparationPauseTest(void (*paused)(void*), void* context);
bool GraphicsRunNativeRangePhaseTest(uint64_t address, uint64_t bytes, void (*native)(void*), void* context);
CommandProcessor* GraphicsRunCreateHeadlessTestProcessor();
bool GraphicsRunTryProcessorLockForTest(CommandProcessor* cp);
bool GraphicsRunNativeRecordingLocksTest(CommandProcessor* cp, void (*before_renderer)(void*), void* context,
    bool (*locked)(void*));
#endif

} // namespace Lumen::Libs::Graphics

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_ */
