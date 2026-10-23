#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_GPUMEMORY_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_GPUMEMORY_H_

#include "Lumen/Core/Common.h"
#include "Lumen/Core/Vector.h"

#include "Emulator/Common.h"
#include "Emulator/Graphics/DrawAccessReservation.h"

#include <array>
#include <memory>
#include <vector>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::GuestCpu { enum class MemoryAccessIntent : uint32_t; }

namespace Lumen::Libs::Graphics {

class CommandBuffer;
class CommandProcessor;
class GpuMemory;
struct GraphicContext;
struct VulkanCommandPool;
struct VulkanMemory;
struct VulkanBuffer;
struct VulkanImage;

struct GpuMemoryClaimRange
{
	uint64_t address = 0, bytes = 0;
};

// Captures the complete physical peer closure while the caller owns exclusive
// guest admission. Enrollment installs conservative CPU guards, but does not
// activate a range claim or grant native guest access. The caller must retire
// its active claim before Reset/destruction removes this guard owner.
class GpuMemoryRangeClaimPlan
{
public:
	GpuMemoryRangeClaimPlan();
	~GpuMemoryRangeClaimPlan();
	LUMEN_CLASS_NO_COPY(GpuMemoryRangeClaimPlan);
	bool Capture(const std::vector<GpuMemoryClaimRange>& seeds);
	uint64_t Owner() const;
	bool Covers(uint64_t address, uint64_t bytes) const;
	void Reset();
private:
	friend class GpuMemory;
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
	friend bool GpuMemoryRangeClaimPlanContractTest(uint64_t allocation);
#endif
	bool CaptureWithMemory(GpuMemory* memory, const std::vector<GpuMemoryClaimRange>& seeds);
	struct Storage;
	std::unique_ptr<Storage> m_storage;
};

enum class GpuMemoryMode
{
	NoAccess,
	Read,
	Write,
	ReadWrite
};

enum class GpuMemoryObjectType : uint64_t
{
	Invalid,
	VideoOutBuffer,
	DepthStencilBuffer,
	Label,
	IndexBuffer,
	VertexBuffer,
	StorageBuffer,
	Texture,
	RenderTexture,
	StorageTexture,

	Max
};

enum class GpuMemoryScenario
{
	Common,
	GenerateMips,
	TextureTriplet,
	TextureMipSnapshot,
	TextureReinterpretSnapshot
};

enum class GpuMemoryCpuCopyRole
{
	None,
	Source,
	Destination
};

enum class GpuMemoryAccessKind : uint32_t
{
	Unknown          = 0,
	SampledRead      = 1u << 0u,
	StorageReadWrite = 1u << 1u,
	TransferRead     = 1u << 2u,
	TransferWrite    = 1u << 3u,
	StorageRead      = 1u << 4u,
	ColorAttachmentReadWrite = 1u << 5u,
	DepthStencilAttachmentReadWrite = 1u << 6u,
	DepthStencilAttachmentRead = 1u << 7u
};

enum class GpuMemoryContentMutationKind : uint32_t
{
	None,
	InitialGuestUpload,
	GuestUpload,
	MipSnapshot,
	// A completed GPU image was copied through typed storage views while
	// preserving the guest Thin1D byte interpretation.
	ReinterpretSnapshot,
	CopyFrom,
	// The shader classifier resolved the storage target. This does not prove
	// that control flow executed the store or that the dispatch covered it.
	ClassifiedShaderWrite,
	UnknownPotentialWrite,
	ColorAttachmentWrite
};

enum class GpuMemoryCommandMutationPolicy : uint32_t
{
	AccessOnly,
	PotentialStorageWrite,
	UniqueComputeStorageWrite,
	AdjacentComputeMipWrite
};

struct GpuMemoryObjectHandle
{
	void*    host_object         = nullptr;
	uint64_t lifetime_generation = 0;
	int      heap_id             = -1;
	int      object_id           = -1;
};

struct GpuMemoryCommandAccess
{
	GpuMemoryObjectHandle handle;
	GpuMemoryAccessKind   kind = GpuMemoryAccessKind::Unknown;
	// Evidence from the actual bound native pipeline's decoded shader IR,
	// independent of the resource declaration. Only StorageRead consumes it.
	bool                 storage_no_shader_stores = false;
	// A complete, per-binding proof from the actual compute pipeline. This
	// positive writer evidence is independent of a failed no-store proof.
	bool                 storage_compute_writer = false;
};

struct GpuMemoryStorageBindingAuthority
{
	bool compute = false;
	bool complete = false;
	bool writes = false;
};

// Synchronous draw inventory. These values describe already-bound native
// resources; they neither grant guest-memory access nor permit epoch release.
struct GpuMemoryDrawNativeInput
{
	GpuMemoryObjectHandle handle;
	void* native_object = nullptr;
	GpuMemoryObjectType type = GpuMemoryObjectType::Invalid;
	uint64_t vaddr[3] {}, size[3] {};
	uint32_t ranges = 0;
};

enum class GpuMemoryDrawColorKind { None, RenderTexture, VideoOut };

struct GpuMemoryDrawAccessPlan
{
	static constexpr uint32_t MAX_SHADER = 32;
	static constexpr uint32_t MAX_NATIVE = 128;
	std::array<GpuMemoryCommandAccess, MAX_SHADER> images {}, storage {};
	uint32_t image_count = 0, storage_count = 0;
	std::array<GpuMemoryObjectHandle, MAX_SHADER> sampled_videoout {};
	uint32_t sampled_videoout_count = 0;
	std::array<GpuMemoryDrawNativeInput, MAX_NATIVE> native_inputs {};
	uint32_t native_input_count = 0;
	struct Color
	{
		GpuMemoryDrawColorKind kind = GpuMemoryDrawColorKind::None;
		GpuMemoryObjectHandle handle;
		uint64_t address = 0, size = 0;
	} color;
	struct Depth
	{
		VulkanImage* image = nullptr;
		uint64_t address[3] {}, size[3] {};
		uint32_t planes = 0;
		bool potential_write = false, read_only = false;
		bool depth_clear = false, stencil_clear = false;
	} depth;
	bool draw_recorded = false;
	uint64_t submit_id = 0;
};

class GpuMemoryDrawAccessProof
{
public:
	GpuMemoryDrawAccessProof();
	~GpuMemoryDrawAccessProof();
	GpuMemoryDrawAccessProof(GpuMemoryDrawAccessProof&&) noexcept;
	GpuMemoryDrawAccessProof& operator=(GpuMemoryDrawAccessProof&&) noexcept;
	GpuMemoryDrawAccessProof(const GpuMemoryDrawAccessProof&) = delete;
	GpuMemoryDrawAccessProof& operator=(const GpuMemoryDrawAccessProof&) = delete;
	[[nodiscard]] uint32_t ExpectedCommandOrdinals() const;
private:
	friend class GpuMemory;
	struct Storage;
	std::unique_ptr<Storage> m_storage;
};

// Read-only tracker preflight before native emission, then exact validation
// before the first legacy post-draw effect. A failure can use the old path only
// before reservation/emission. No producer, dirty range or ordinal is changed.
[[nodiscard]] bool GpuMemoryPrepareDrawAccesses(CommandBuffer* buffer, const GpuMemoryDrawAccessPlan& plan,
	GpuMemoryDrawAccessProof* proof, const char** reason = nullptr);
[[nodiscard]] bool GpuMemoryValidateDrawAccesses(CommandBuffer* buffer, const GpuMemoryDrawAccessPlan& plan,
	const GpuMemoryDrawAccessProof& proof, const char** reason = nullptr);
bool GpuMemoryDrawAccessPlanContractTest();

// Bounded capture diagnostics only. These values identify tracked lifetimes
// and ordering evidence; no image ownership or GPU-completion proof escapes.
struct GpuMemoryR8LineagePoint
{
	uint64_t image_unique_id = 0;
	uint64_t lifetime_generation = 0;
	uint64_t content_epoch = 0;
	uint64_t recording_id = 0;
	uint64_t command_ordinal = 0;
	uint64_t submit_serial = 0;
	uint64_t session_epoch = 0;
	GpuMemoryContentMutationKind kind = GpuMemoryContentMutationKind::None;
	int queue = -1;
	bool identity_valid = false;
	bool access_precedes = false;
	bool mutation_precedes = false;
	bool mutation_completed_before_recording = false;
	bool access_ambiguous = false;
	bool pending_storage_bind = false;
};

struct GpuMemoryR8Lineage
{
	const char* reason = "unavailable";
	bool available = false;
	bool producer_handle_matches = false;
	bool copied_epoch_matches = false;
	bool current_identity_valid = false;
	uint64_t copied_producer_epoch = 0;
	uint64_t copied_producer_generation = 0;
	uint64_t current_recording_id = 0;
	uint64_t current_command_ordinal = 0;
	uint64_t current_submit_serial = 0;
	uint64_t current_session_epoch = 0;
	uint64_t submitted_cutoff = 0;
	uint64_t completed_cutoff = 0;
	int current_queue = -1;
	GpuMemoryR8LineagePoint sampled;
	GpuMemoryR8LineagePoint producer;
};

struct GpuMemoryObject
{
	GpuMemoryObjectType type = GpuMemoryObjectType::Invalid;
	void*               obj  = nullptr;
};

class GpuObject
{
public:
	using create_func_t = void* (*)(GraphicContext* ctx, const uint64_t* params, const uint64_t* vaddr, const uint64_t* size, int vaddr_num,
	                                VulkanMemory* mem);
	using create_with_buffer_func_t = void* (*)(GraphicContext* ctx, CommandBuffer* buffer, const uint64_t* params, const uint64_t* vaddr,
	                                            const uint64_t* size, int vaddr_num, VulkanMemory* mem);
	using create_from_objects_func_t = void* (*)(GraphicContext* ctx, CommandBuffer* buffer, const uint64_t* params,
	                                             GpuMemoryScenario scenario, const Vector<GpuMemoryObject>& objects, VulkanMemory* mem);
	using write_back_func_t = void (*)(GraphicContext* ctx, const uint64_t* params, void* obj, const uint64_t* vaddr, const uint64_t* size,
	                                   int vaddr_num);
	using delete_func_t     = void (*)(GraphicContext* ctx, void* obj, VulkanMemory* mem);
	using update_func_t     = void (*)(GraphicContext* ctx, const uint64_t* params, void* obj, const uint64_t* vaddr, const uint64_t* size,
                                   int vaddr_num);
	using update_with_buffer_func_t = void (*)(GraphicContext* ctx, CommandBuffer* buffer, const uint64_t* params, void* obj,
	                                           const uint64_t* vaddr, const uint64_t* size, int vaddr_num);

	// Host resource descriptions include a distinct typed Label preparation
	// role. Keep it separate from the guest byte count and callback arguments.
	static constexpr int PARAMS_MAX = 9;

	GpuObject()          = default;
	virtual ~GpuObject() = default;

	LUMEN_CLASS_DEFAULT_COPY(GpuObject);

	virtual bool Equal(const uint64_t* other) const = 0;
	// virtual bool Reuse(const uint64_t* /*other*/) const { return false; };

	[[nodiscard]] virtual create_func_t              GetCreateFunc() const            = 0;
	[[nodiscard]] virtual create_with_buffer_func_t  GetCreateWithBufferFunc() const  { return nullptr; }
	[[nodiscard]] virtual create_from_objects_func_t GetCreateFromObjectsFunc() const = 0;
	[[nodiscard]] virtual write_back_func_t          GetWriteBackFunc() const         = 0;
	[[nodiscard]] virtual delete_func_t              GetDeleteFunc() const            = 0;
	[[nodiscard]] virtual update_func_t              GetUpdateFunc() const            = 0;
	[[nodiscard]] virtual update_with_buffer_func_t  GetUpdateWithBufferFunc() const  { return nullptr; }

	uint64_t            params[PARAMS_MAX] = {};
	bool                check_hash         = false;
	bool                read_only          = false;
	GpuMemoryObjectType type               = GpuMemoryObjectType::Invalid;
	GpuMemoryAccessKind access_kind        = GpuMemoryAccessKind::Unknown;
	// A validated depth attachment request may retain one already imported
	// image. This is a request capability, never part of allocation identity or
	// evidence that a recorded draw preserved uniform content.
	bool                depth_uniform_zero_reuse = false;
	uint32_t            depth_uniform_zero_pitch = 0;
	uint32_t            depth_uniform_zero_slice = 0;
};

void GpuMemoryInit();

// Called before the CPU consumes or changes guest bytes. Physical aliases are
// resolved through the VM owner; publication is limited to dirty dependencies.
bool GpuMemoryPrepareCpuMemoryAccess(GraphicContext* ctx, uint64_t address, uint64_t bytes,
                                    GuestCpu::MemoryAccessIntent intent);
// Fresh metadata proof only; false selects the ordinary renderer transaction.
// The caller must retain a qualified execution/HLE read admission through its
// actual access, and every native upload/enrollment must exclude that admission.
// No permission, backing identity or decision is retained between requests.
bool GpuMemoryCpuMemoryAccessNeedsNoRenderer(uint64_t address, uint64_t bytes,
                                           GuestCpu::MemoryAccessIntent intent);
// Opt-in tracker-only transaction for the first CPU write to clean native
// snapshots. The caller must retain a counted execution admission through the
// actual store. Failure leaves all write intentions unchanged.
bool GpuMemoryPrepareCleanCpuWriteAccess(uint64_t address, uint64_t bytes,
                                        GuestCpu::MemoryAccessIntent intent);
bool GpuMemoryCpuHandoffEnabled();
bool GpuMemoryHasPendingGpuDirty() noexcept;
bool GpuMemoryPrepareResourceBacking(GraphicContext* ctx, const uint64_t* addresses, const uint64_t* sizes, int count, const GpuObject& info);
bool GpuMemoryCpuHandoffContractTest();

bool  GpuMemoryContainsAllocatedRange(uint64_t vaddr, uint64_t size);
void  GpuMemorySetAllocatedRange(uint64_t vaddr, uint64_t size);
void  GpuMemoryFree(GraphicContext* ctx, CommandBuffer* buffer, uint64_t vaddr, uint64_t size, bool unmap);
void* GpuMemoryCreateObject(uint64_t submit_id, GraphicContext* ctx, CommandBuffer* buffer, uint64_t vaddr, uint64_t size,
	                        const GpuObject& info);
void* GpuMemoryCreateObject(uint64_t submit_id, GraphicContext* ctx, CommandBuffer* buffer, const uint64_t* vaddr, const uint64_t* size,
	                        int vaddr_num, const GpuObject& info);
void* GpuMemoryCreateObjectTracked(uint64_t submit_id, GraphicContext* ctx, CommandBuffer* buffer, uint64_t vaddr, uint64_t size,
	                               const GpuObject& info, GpuMemoryObjectHandle* handle);
void* GpuMemoryCreateStorageObjectTracked(uint64_t submit_id, GraphicContext* ctx, CommandBuffer* buffer,
                                         uint64_t vaddr, uint64_t size, const GpuObject& info,
                                         const GpuMemoryStorageBindingAuthority& authority, GpuMemoryObjectHandle* handle);
// Only protected metadata writers use these boundaries. Validate before the
// guest dispatch; reconcile its completed result before returning to the CP.
[[nodiscard]] bool GpuMemoryValidateUniformDepthZeroStorageDispatch(CommandBuffer* buffer,
    const GpuMemoryCommandAccess* image_accesses, uint32_t image_count,
    const GpuMemoryCommandAccess* storage_accesses, uint32_t storage_count, bool nonzero);
[[nodiscard]] bool GpuMemoryReconcileUniformDepthZeroStorageDispatch(GraphicContext* ctx, CommandBuffer* buffer,
    const GpuMemoryCommandAccess* storage_accesses, uint32_t storage_count);
// Read-only, existing-object validation. This never creates, updates, links, or
// copies an object; it only confirms that the supplied lifetime still names the
// exact live Common object and guest range requested by the caller.
[[nodiscard]] bool GpuMemoryValidateObjectHandle(const GpuMemoryObjectHandle& handle,
	                                             GpuMemoryObjectType expected_type, uint64_t vaddr,
	                                             uint64_t size);
// Pins one already-existing exact object through completion or cancellation of
// this command buffer and returns the validated host object. No tracker object
// is created or updated by this operation.
[[nodiscard]] bool GpuMemoryPinVideoOutBufferForCommand(CommandBuffer* buffer,
	                                                     const GpuMemoryObjectHandle& handle,
	                                                     uint64_t vaddr, uint64_t size,
	                                                     void** host_object);
// Finds and pins one exact, live, unlinked Common RenderTexture for sampled
// use by this command buffer. The returned pointer is valid through command
// completion or cancellation; failure clears both outputs.
[[nodiscard]] bool GpuMemoryPinRenderTextureForCommand(CommandBuffer* buffer,
	                                                    uint64_t vaddr, uint64_t size,
	                                                    GpuMemoryObjectHandle* handle,
	                                                    void** host_object);
// Always-on host usage accounting, after an actual pass or sampled command was
// recorded. Allocation and command pinning alone are not a GPU write. A stale
// handle fails rather than attributing activity to a replacement lifetime.
[[nodiscard]] bool GpuMemoryRecordVideoOutBufferPass(CommandBuffer* buffer, const GpuMemoryObjectHandle& handle,
	                                                   uint64_t vaddr, uint64_t size, bool draw_recorded);
[[nodiscard]] bool GpuMemoryRecordVideoOutBufferRead(CommandBuffer* buffer, const GpuMemoryObjectHandle& handle);
bool  GpuMemoryAccessTrackingEnabled();
bool  GpuMemoryTextureMipSnapshotEnabled();
bool  GpuMemoryRenderTextureSnapshotEnabled();
bool  GpuMemoryExactReuseFastPathEnabled();
// Read-only snapshot of one exact reciprocal Common StorageTexture -> Texture
// pair. Call only for an accepted R8 capture job; tracking OFF is unavailable.
GpuMemoryR8Lineage GpuMemoryQueryR8Lineage(CommandBuffer* buffer, const GpuMemoryObjectHandle& sampled);
void  GpuMemoryRecordCommandAccesses(CommandBuffer* buffer, const GpuMemoryCommandAccess* accesses, uint32_t accesses_num);
// Extended observation entry point. Callers select a policy only after their
// pipeline analysis has established the corresponding write shape. In
// particular, AdjacentComputeMipWrite is the sole policy that may preserve a
// sampled producer dependency. The legacy entry point above remains ABI-stable
// and records storage bindings conservatively as potential writes.
void  GpuMemoryRecordCommandAccessesTracked(CommandBuffer* buffer, const GpuMemoryCommandAccess* accesses,
	                                         uint32_t accesses_num, GpuMemoryCommandMutationPolicy mutation_policy);
// One recorded command, one ordinal. Buffer use metadata supplies only
// conservative access/potential writes and must not enter image classifiers.
void  GpuMemoryRecordCommandAccessGroups(CommandBuffer* buffer, const GpuMemoryCommandAccess* image_accesses,
	                                      uint32_t image_accesses_num, GpuMemoryCommandMutationPolicy image_mutation_policy,
	                                      const GpuMemoryCommandAccess* storage_accesses, uint32_t storage_accesses_num);
// Records the color-attachment use of one exact, still-live RenderTexture.
// Every begun pass is an ordered access; only a pass with a
// non-empty draw is promoted to a known content mutation.
[[nodiscard]] bool GpuMemoryRecordRenderTexturePass(CommandBuffer* buffer,
	                                                 const GpuMemoryObjectHandle& handle,
	                                                 uint64_t vaddr, uint64_t size,
	                                                 bool draw_recorded);
[[nodiscard]] bool GpuMemoryRecordDepthStencilPass(CommandBuffer* buffer, VulkanImage* actual_image,
	const uint64_t* vaddr, const uint64_t* size, int vaddr_num, bool potential_write, bool depth_read_only);
// depth_read_only is authority from the actual native attachment/shader state.
// It can retain an existing depth content seal, never create or refresh one.
void  GpuMemoryResetHash(const uint64_t* vaddr, const uint64_t* size, int vaddr_num, GpuMemoryObjectType type,
	                     CommandBuffer* buffer = nullptr, uint64_t submit_id = 0, bool depth_read_only = false);
void  GpuMemoryDbgDump();
// Diagnostic metadata only: prints the latest 64 GPU producer decisions and 64 WriteBack completions
// when LUMEN_GFX_CPU_READBACK_TRACE=1. Does not query resources or read guest bytes.
void  GpuMemoryDumpCpuReadbackTrace(uint32_t encoder_index, uint64_t image_address, uint64_t image_size);
// Unsupported-EOP diagnostics only, enabled by LUMEN_GFX_EOP_RESOURCE_TRACE=1.
// Caller holds its processor lock; takes only the tracker lock, copies bounded
// metadata, then prints without guest addresses, payloads or resource reads.
void  GpuMemoryTraceEopResources(CommandProcessor* cp, uint32_t packet, uint32_t event_control, uint32_t address_control);
// Experimental host-only checkpoint. Its caller has fenced the processor
// prefix and completed its Label publications. It never admits a native EOP.
class GuestWriteDestination;
struct GraphicsGuestLeaseResult;
enum class GpuMemoryEopPublicationKind : uint8_t { VirtualClock64, Immediate32 };
constexpr uint64_t GpuMemoryEopPublicationBytes(GpuMemoryEopPublicationKind kind)
{
	switch (kind)
	{
		case GpuMemoryEopPublicationKind::VirtualClock64: return sizeof(uint64_t);
		case GpuMemoryEopPublicationKind::Immediate32: return sizeof(uint32_t);
	}
	return 0;
}
enum class GpuMemoryEopCheckpointStatus : uint8_t { Rejected, Materialized, Published };
struct GpuMemoryEopCheckpointResult
{
	GpuMemoryEopCheckpointStatus status = GpuMemoryEopCheckpointStatus::Rejected;
	const char* reason = "not-attempted";
	// Only a failed nonblocking guest-byte lease acquisition may set this.
	bool retryable = false;
};
inline constexpr uint32_t GpuMemoryEopCheckpointMaxAttempts = 16;
inline bool GpuMemoryEopCheckpointRetryAllowed(const GpuMemoryEopCheckpointResult& result, uint32_t completed_attempts)
{
	return result.status == GpuMemoryEopCheckpointStatus::Rejected && result.retryable && completed_attempts > 0 &&
	       completed_attempts < GpuMemoryEopCheckpointMaxAttempts;
}
inline bool GpuMemoryEopCheckpointRetryAllowed(const GpuMemoryEopCheckpointResult& result, uint32_t completed_attempts,
                                             uint64_t original_generation, uint64_t observed_generation)
{
	return GpuMemoryEopCheckpointRetryAllowed(result, completed_attempts) && original_generation != UINT64_MAX &&
	       observed_generation == original_generation;
}
struct GpuMemoryEopTargetRangeResult
{
	const char* reason = nullptr;
	uint64_t heap_count = 0, slot_count = 0, live_count = 0;
	int heap = -1, object = -1, plane = -1;
	GpuMemoryObjectType type = GpuMemoryObjectType::Invalid;
};
// Optional refusal metadata distinguishes a true overlap from malformed
// ranges and bounded proof capacity. It grants no admission on failure.
bool GpuMemoryEopTargetRangeAvailable(uint64_t address, uint64_t size, GpuMemoryEopTargetRangeResult* result = nullptr);
// Checks every live represented interval against the original VM token. The
// final publication calls this while its destination lease pins all VM writers.
bool GpuMemoryEopMappingsUnchanged(uint64_t original_generation, GraphicsGuestLeaseResult* result = nullptr);
GpuMemoryEopCheckpointResult GpuMemoryEopCheckpointMaterialize(GraphicContext* ctx, CommandProcessor* cp,
    int queue, uint64_t epoch, uint64_t serial, uint64_t memory_generation,
    const GuestWriteDestination* publication_destination = nullptr,
    GpuMemoryEopPublicationKind publication_kind = GpuMemoryEopPublicationKind::VirtualClock64,
    uint32_t immediate32 = 0);
void  GpuMemoryFlush(GraphicContext* ctx, CommandBuffer* buffer, uint64_t vaddr, uint64_t size);
void  GpuMemoryFlushAll(GraphicContext* ctx);
void  GpuMemoryFrameDone();
bool GpuMemoryReadOnlyStorageRetirementEnabled();
// Scheduling hint only; drops the tracker before a caller may acquire an
// exclusive epoch. The collector repeats this policy and all lifetime proofs.
bool GpuMemoryReadOnlyStorageRetirementDue();
// Joined graphics parsers only: caller retains RunLock, an exclusive guest
// memory epoch, then CP and renderer; no packet-local descriptors remain.
// Completion checks do not wait or submit.
void GpuMemoryRetireCompletedReadOnlyStorage(GraphicContext* ctx, CommandProcessor* cp, CommandBuffer* buffer);
void  GpuMemoryWriteBack(GraphicContext* ctx, CommandProcessor* cp, const char* origin = nullptr);
struct GpuMemoryRenderFullPreflightState;
class GpuMemoryRenderFullPreflightTicket
{
public:
	explicit GpuMemoryRenderFullPreflightTicket(uint64_t original_generation): m_generation(original_generation) {}
	uint64_t Generation() const { return m_generation; }
private:
	friend class GpuMemory;
	const uint64_t m_generation;
	std::shared_ptr<GpuMemoryRenderFullPreflightState> m_state;
};
struct GpuMemoryRenderFullWriteBackResult
{
	bool complete = false;
	bool retryable = false;
	const char* reason = "not-attempted";
};
// Only CommandProcessor's exclusive render-full scope can unwind all caller
// locks after a precise pre-effect lease refusal. No other WriteBack widens.
GpuMemoryRenderFullWriteBackResult GpuMemoryTryWriteBackRenderFull(GraphicContext* ctx, CommandProcessor* cp,
    GpuMemoryRenderFullPreflightTicket* ticket, bool exclusive_stream);
void  GpuMemoryWriteBack(GraphicContext* ctx, CommandProcessor* cp, uint64_t vaddr, uint64_t size);
[[nodiscard]] bool GpuMemoryWriteBackForCpuCopy(GraphicContext* ctx, CommandProcessor* cp, uint64_t vaddr, uint64_t size,
                                                GpuMemoryCpuCopyRole role);
// Mark must run while new GPU submissions are still excluded after a completed
// all-queue wait. Drain invokes callbacks only after the GPU and GpuMemory locks
// have been released; selective replacement never drains from CreateObject.
void  GpuMemoryMarkRetiredSampledTexturesReadyAfterAllQueuesIdle();
void  GpuMemoryDrainReadyRetiredSampledTextures(GraphicContext* ctx);
bool  GpuMemoryResetForNextSession(GraphicContext* ctx);
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool GpuMemoryReadyMemoryWaitContractTest(
    bool (*observe)(void*, uint64_t, uint64_t, bool, bool (*)(void*, uint64_t, uint64_t), void*), void* context);
bool GpuMemoryRetiredStorageWaitContractTest(CommandProcessor* cp);
bool GpuMemorySampledTextureRetirementContractTest();
bool GpuMemoryReadOnlyStorageCrossAliasContractTest();
bool GpuMemoryExactReuseFastPathContractTest();
bool GpuMemoryLabelSlotReuseContractTest();
bool GpuMemoryPageLookupContractTest();
bool GpuMemoryTextureMipWindowAliasContractTest();
bool GpuMemoryTextureMipChainObservationContractTest();
bool GpuMemoryAllocatedRangeContainmentContractTest();
bool GpuMemoryCpuCopyWriteBackPolicyContractTest();
bool GpuMemoryPinnedLifetimeContractTest();
bool GpuMemoryRenderTextureWriteTrackingContractTest();
bool GpuMemoryStorageCommandAccessContractTest();
bool GpuMemoryStorageWriteBackOwnerContractTest();
bool GpuMemoryEopRecheckContractTest();
bool GpuMemoryEopVirtualClockContractTest();
bool GpuMemoryUniformDepthZeroSnapshotContractTest();
#endif

Vector<GpuMemoryObject> GpuMemoryFindObjects(uint64_t vaddr, uint64_t size, GpuMemoryObjectType type, bool exact, bool only_first,
                                          const char* diagnostic_caller = nullptr);

enum class GpuMemoryDepthStencilBarrierStatus { NotProtected, Complete, Rejected };
struct GpuMemoryDepthStencilBarrierResult
{
	GpuMemoryDepthStencilBarrierStatus status = GpuMemoryDepthStencilBarrierStatus::Rejected;
	const char* reason = "not-attempted";
	uint64_t original_generation = UINT64_MAX;
	// A fresh verification in this operation and a retained completed-readback
	// receipt are distinct proofs; neither flag alone grants admission.
	bool gpu_verified = false;
	bool retained_full_readback = false;
};
// A protected image remains inside the tracker for the complete proof and
// native transition. This capability never exposes a sampled-depth alias.
GpuMemoryDepthStencilBarrierResult GpuMemoryRecordUniformDepthZeroReadOnlyBarrier(
    GraphicContext* ctx, CommandBuffer* buffer, uint64_t vaddr, uint64_t size);
// Host metadata predicate for bounded diagnostics only. It reads no guest
// bytes and grants no protected-resource binding or alias authority.
bool GpuMemoryHasUniformDepthZeroOverlap(uint64_t vaddr, uint64_t size);

enum class GpuMemoryDbCacheProofStatus { Rejected, Proven, Recorded };
// Copied host metadata for the bounded rejected DB-cache guard diagnostic.
// These identities grant no completion, content or mapping authority.
struct GpuMemoryDbCacheGuardIdentity
{
	CommandProcessor* cp = nullptr;
	VulkanCommandPool* pool = nullptr;
	uint64_t recording_id = 0;
	uint64_t command_ordinal = 0;
	uint64_t submission_session_epoch = 0;
	uint64_t recording_submit_serial = 0;
	uint64_t begin_submit_serial_cutoff = 0;
	uint64_t begin_completed_serial_cutoff = 0;
	int queue = -1;
};
struct GpuMemoryDbCacheGuardDiagnostic
{
	bool rejected = false;
	int heap = -1;
	int object = -1;
	uint64_t lifetime = 0;
	int plane_count = 0;
	uint64_t addresses[3] {}, sizes[3] {};
	bool synchronized = false;
	bool synchronized_from_readback = false;
	bool host_content_pending = false;
	bool initial_use_pending = false;
	bool reuse_pending = false;
	bool metadata_write_pending = false;
	bool read_only_barrier = false;
	uint64_t operation_generation = UINT64_MAX;
	uint64_t accumulated_prefix_generation = UINT64_MAX;
	uint64_t synchronized_generation = UINT64_MAX;
	uint64_t barrier_generation = UINT64_MAX;
	// Missing bits mean not evaluated, never a negative completion proof.
	uint32_t evaluation_mask = 0; // 1:latest order, 2:latest completion, 4:layout order, 8:layout completion
	bool latest_follows = false;
	bool latest_completed = false;
	bool layout_follows = false;
	bool layout_completed = false;
	bool retained_readback_attempted = false;
	bool producer_completion_observed = false;
	bool producer_completed = false;
	GpuMemoryDbCacheGuardIdentity current, latest, layout, synchronized_producer;
};
struct GpuMemoryDbCacheProofResult
{
	GpuMemoryDbCacheProofStatus status = GpuMemoryDbCacheProofStatus::Rejected;
	const char* reason = "db-cache-proof-not-attempted";
	uint64_t original_generation = UINT64_MAX;
	uint32_t live_objects = 0;
	uint32_t depth_pairs = 0;
	uint32_t initial_uniform_pairs = 0;
	uint32_t readback_pairs = 0;
	// Distinct retained-native-content authority; a submitted read-only prefix
	// is ordered before the DB barrier, never claimed to be GPU-completed.
	uint32_t retained_readback_pairs = 0;
	uint32_t submitted_read_only_pairs = 0;
	int rejected_plane = -1;
	GpuMemoryDbCacheGuardDiagnostic guard_diagnostic;
	bool retryable = false;
	// Static metadata observation. It never supplies DB admission authority.
	const char* retry_certificate_reason = "db-cache-preflight-not-requested";
};
struct GpuMemoryDbCachePreflightState;
class GpuMemoryDbCachePreflightTicket
{
public:
	static constexpr uint64_t MaxScopeMetadataBytes = 64ull * 1024;
	explicit GpuMemoryDbCachePreflightTicket(uint64_t original_operation_generation): m_generation(original_operation_generation) {}
	// Qualified original CP-gap certificate; the first uninterrupted attempt
	// still uses the existing DB proof when this optional certificate is absent.
	bool IsPrepared() const;
	bool EffectsStarted() const;
	uint64_t OriginalGeneration() const;
private:
	friend class GpuMemory;
	const uint64_t m_generation;
	std::shared_ptr<GpuMemoryDbCachePreflightState> m_state;
	bool m_attempted = false, m_terminal = false;
};
// Caller owns CP and renderer locks. The tracker retains all mapping leases
// through the optional native record callback; this never materializes data.
using GpuMemoryDbCacheRecord = void (*)(CommandBuffer* buffer);
GpuMemoryDbCacheProofResult GpuMemoryProveAlreadySynchronizedDepth(
    CommandBuffer* buffer, GpuMemoryDbCacheRecord record, GpuMemoryDbCachePreflightTicket* ticket = nullptr);

bool VulkanAllocate(GraphicContext* ctx, VulkanMemory* mem);
// Allocation attempt used by optional fast paths that must retain a safe
// fallback when a compatible heap exists but is temporarily exhausted.
// An explicit type is still checked against the requirements and properties.
bool VulkanTryAllocate(GraphicContext* ctx, VulkanMemory* mem, uint32_t memory_type = UINT32_MAX);
void VulkanFree(GraphicContext* ctx, VulkanMemory* mem);
void VulkanMapMemory(GraphicContext* ctx, VulkanMemory* mem, void** data);
void VulkanUnmapMemory(GraphicContext* ctx, VulkanMemory* mem);
void VulkanBindImageMemory(GraphicContext* ctx, VulkanImage* image, VulkanMemory* mem);
void VulkanBindBufferMemory(GraphicContext* ctx, VulkanBuffer* buffer, VulkanMemory* mem);

void GpuMemoryRegisterOwner(uint32_t* owner_handle, const char* name);
void GpuMemoryRegisterResource(uint32_t* resource_handle, uint32_t owner_handle, const void* memory, size_t size, const char* name,
                               uint32_t type, uint64_t user_data);
void GpuMemoryUnregisterAllResourcesForOwner(uint32_t owner_handle);
void GpuMemoryUnregisterOwnerAndResources(uint32_t owner_handle);
void GpuMemoryUnregisterResource(uint32_t resource_handle);

} // namespace Lumen::Libs::Graphics

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_GPUMEMORY_H_ */
