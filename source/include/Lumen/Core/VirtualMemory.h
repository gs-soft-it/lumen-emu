#ifndef INCLUDE_LUMEN_CORE_VIRTUALMEMORY_H_
#define INCLUDE_LUMEN_CORE_VIRTUALMEMORY_H_

#include "Lumen/Core/Common.h"
#include "Lumen/Core/String.h"

namespace Lumen::Core {

struct SystemInfo
{
	String ProcessorName;
};

SystemInfo GetSystemInfo();

namespace VirtualMemory {

class ExceptionHandlerPrivate;

class ExceptionHandler
{
public:
	enum class ExceptionType
	{
		Unknown,
		AccessViolation
	};

	enum class AccessViolationType
	{
		Unknown,
		Read,
		Write,
		Execute
	};

	struct ExceptionInfo
	{
		ExceptionType       type                   = ExceptionType::Unknown;
		AccessViolationType access_violation_type  = AccessViolationType::Unknown;
		uint64_t            access_violation_vaddr = 0;
		uint64_t            exception_address      = 0;
		uint64_t            rbp                    = 0;
		uint32_t            exception_win_code     = 0;
	};

	using handler_func_t = void (*)(const ExceptionInfo*);

	ExceptionHandler();
	virtual ~ExceptionHandler();

	LUMEN_CLASS_NO_COPY(ExceptionHandler);

	static uint64_t GetSize();

	bool Install(uint64_t base_address, uint64_t handler_addr, uint64_t image_size, handler_func_t func);
	bool Uninstall();

private:
	ExceptionHandlerPrivate* m_p = nullptr;
};

enum class Mode : uint32_t
{
	NoAccess         = 0,
	Read             = 1,
	Write            = 2,
	ReadWrite        = Read | Write,
	Execute          = 4,
	ExecuteRead      = Execute | Read,
	ExecuteWrite     = Execute | Write,
	ExecuteReadWrite = Execute | Read | Write,
};

inline bool IsExecute(Mode mode)
{
	return (mode == Mode::Execute || mode == Mode::ExecuteRead || mode == Mode::ExecuteWrite || mode == Mode::ExecuteReadWrite);
}

void Init();

uint64_t GetMutationGeneration();

// One contiguous piece of an authoritative allocation's backing. Identity is
// lifetime-specific (never a VA or a protection-run address); offset remains
// stable when a mapping is split by mprotect. This is metadata, not permission
// or mapping-lifetime authority: callers retain their FEX/VM admission.
struct GuestBackingSpan
{
	uint64_t identity = 0, offset = 0, address = 0, size = 0, generation = 0;
};
using GuestBackingOwnerQuery = bool (*)(uint64_t address, uint64_t maximum_bytes, GuestBackingSpan* span) noexcept;
using GuestBackingOwnerMark = bool (*)(uint64_t address, uint64_t bytes, uint32_t watch_intents) noexcept;
using GuestBackingOwnerMarkOwned = bool (*)(uint64_t owner, uint64_t address, uint64_t bytes, uint32_t watch_intents) noexcept;
enum class GuestBackingPresence : uint8_t { Unknown, Retired, Live };
using GuestBackingOwnerPresence = GuestBackingPresence (*)(uint64_t identity, uint64_t offset, uint64_t bytes) noexcept;
using GuestBackingOwnerGpuCopy = bool (*)(uint64_t address, void* buffer, uint64_t bytes, bool write) noexcept;
// A console's physical-memory owner may supply allocation identities/offsets
// and enumerate its aliases. It must cover the entire accepted alias set,
// never infer physical equivalence from matching contents or VA arithmetic.
bool RegisterGuestBackingOwner(GuestBackingOwnerQuery query, GuestBackingOwnerMark mark,
    GuestBackingOwnerPresence presence = nullptr, GuestBackingOwnerGpuCopy copy = nullptr,
    GuestBackingOwnerMarkOwned mark_owned = nullptr) noexcept;
bool QueryGuestBackingSpan(uint64_t address, uint64_t maximum_bytes, GuestBackingSpan* span) noexcept;
// Retired proves that no accepted alias still maps any byte of the interval.
// This includes CPU-only aliases. Unknown never permits dirty-data discard;
// callers retain their exclusive VM/FEX admission while using this metadata.
GuestBackingPresence QueryGuestBackingPresence(uint64_t identity, uint64_t offset, uint64_t bytes) noexcept;
// Install conservative JIT guards for every accepted VA of this backing.
// Must run behind the exclusive FEX ownership-acquisition safepoint.
// CPU watch intent bits: Read=1, Write=2, ReadWrite=3; other masks are rejected.
// Read enrollment also watches writes/RMW, preserving prior GPU publication.
bool MarkGuestBackingHandoffRange(uint64_t address, uint64_t bytes, uint32_t watch_intents = 3u) noexcept;
// Resource-specific enrollment, including every physical alias. A nonzero
// owner token is monotonic and must be released only after the renderer has
// retired all ownership/dirty state for that resource, under exclusive FEX
// admission. Release removes only that token's guard contributions. Legacy
// unowned enrollment remains permanently conservative.
uint64_t CreateGuestBackingHandoffOwner() noexcept;
bool MarkGuestBackingHandoffRangeOwned(uint64_t owner, uint64_t address, uint64_t bytes,
    uint32_t watch_intents = 3u) noexcept;
bool ReleaseGuestBackingHandoffOwner(uint64_t owner) noexcept;
// Internal producer transfer only: the caller proves native completion and
// serializes with the interval tracker. An owner may use a private shared-file
// view without granting or changing guest VA rights. Otherwise exact checked
// Read/Write admission is required; unavailable canonical permissions fail
// closed. This transfer never declares an independent CPU write intention.
bool CopyGuestBackingDataForGpu(uint64_t address, void* buffer, uint64_t bytes, bool write) noexcept;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
// Bind a newly allocated, unpublished exact Core mapping to its physical
// owner's shared file. Retains the registered extent and requested rights;
// caller keeps that owner's lifetime transaction until publishing the VA
// and keeps fd open until the mapping is retired.
bool BindFreshGuestMappingToSharedBacking(uint64_t address, uint64_t bytes, int fd,
    uint64_t backing_offset, Mode mode) noexcept;
#endif

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
// True while this thread owns a guest-data VM lease (or a Mac code-read lease).
// Android includes retained HLE, native and mutation-owner data borrows. Mac
// host waits must retain their FEX reader while the VM lease blocks a writer.
bool CurrentThreadHasMemoryLease() noexcept;
#endif

// Pins one managed host interval while code bytes are inspected. A lease is
// granted only when the complete guest interval and every overlapping physical
// host page are readable and non-writable. This makes ordinary guest stores an
// exclusion mechanism as well as preventing Free/Protect/PatchReplace from
// changing the mapping until ReleaseCodeReadLease returns.
//
// The token is intentionally opaque to callers. Leases must not be copied and
// must be released on the acquiring thread before invoking a VM mutator for the
// same mapping.
class CodeReadLease
{
	friend bool TryAcquireCodeReadLease(uint64_t address, uint64_t size,
	                                  CodeReadLease* lease);
	friend void ReleaseCodeReadLease(CodeReadLease* lease);

public:
	CodeReadLease() = default;
	CodeReadLease(const CodeReadLease&) = delete;
	CodeReadLease& operator=(const CodeReadLease&) = delete;
	CodeReadLease(CodeReadLease&&) = delete;
	CodeReadLease& operator=(CodeReadLease&&) = delete;

	bool IsActive() const { return mapping_identity != 0; }
	uint64_t MappingIdentity() const { return mapping_identity; }
	uint64_t HostBegin() const { return host_begin; }
	uint64_t ByteCount() const { return byte_count; }
	uint64_t MutationGeneration() const { return mutation_generation; }

private:
	uint64_t mapping_identity = 0;
	uint64_t host_begin       = 0;
	uint64_t byte_count       = 0;
	uint64_t mutation_generation = 0;
};

bool TryAcquireCodeReadLease(uint64_t address, uint64_t size,
	                         CodeReadLease* lease);
void ReleaseCodeReadLease(CodeReadLease* lease);

enum class GuestDataLeaseDenialReason : uint32_t
{
	None,
	LeaseOrRangeInvalid,
	GateTrylockUnavailable,
	GateMutationActive,
	GateWriterPending,
	GateReaderOverflow,
	LogicalSnapshotDenied,
	LogicalRegistryDenied,
	PhysicalQueryOrRightsDenied,
	DataHandoffDenied,
	MappingLeaseCapabilityUnavailable,
	Count
};

inline const char* GuestDataLeaseDenialReasonName(GuestDataLeaseDenialReason reason)
{
	switch (reason)
	{
		case GuestDataLeaseDenialReason::None: return "none";
		case GuestDataLeaseDenialReason::LeaseOrRangeInvalid: return "lease-or-range-invalid";
		case GuestDataLeaseDenialReason::GateTrylockUnavailable: return "gate-trylock-unavailable";
		case GuestDataLeaseDenialReason::GateMutationActive: return "gate-mutation-active";
		case GuestDataLeaseDenialReason::GateWriterPending: return "gate-writer-pending";
		case GuestDataLeaseDenialReason::GateReaderOverflow: return "gate-reader-overflow";
		case GuestDataLeaseDenialReason::LogicalSnapshotDenied: return "logical-snapshot-denied";
		case GuestDataLeaseDenialReason::LogicalRegistryDenied: return "logical-registry-denied";
		case GuestDataLeaseDenialReason::PhysicalQueryOrRightsDenied: return "physical-query-or-rights-denied";
		case GuestDataLeaseDenialReason::DataHandoffDenied: return "data-handoff-denied";
		case GuestDataLeaseDenialReason::MappingLeaseCapabilityUnavailable: return "mapping-lease-capability-unavailable";
		case GuestDataLeaseDenialReason::Count: break;
	}
	return "unknown";
}

inline const char* GuestDataLeaseDenialPhaseName(GuestDataLeaseDenialReason reason)
{
	switch (reason)
	{
		case GuestDataLeaseDenialReason::None: return "none";
		case GuestDataLeaseDenialReason::LeaseOrRangeInvalid: return "argument";
		case GuestDataLeaseDenialReason::GateTrylockUnavailable:
		case GuestDataLeaseDenialReason::GateMutationActive:
		case GuestDataLeaseDenialReason::GateWriterPending:
		case GuestDataLeaseDenialReason::GateReaderOverflow: return "gate";
		case GuestDataLeaseDenialReason::LogicalSnapshotDenied:
		case GuestDataLeaseDenialReason::LogicalRegistryDenied: return "logical";
		case GuestDataLeaseDenialReason::PhysicalQueryOrRightsDenied: return "physical";
		case GuestDataLeaseDenialReason::DataHandoffDenied: return "handoff";
		case GuestDataLeaseDenialReason::MappingLeaseCapabilityUnavailable: return "capability";
		case GuestDataLeaseDenialReason::Count: break;
	}
	return "unknown";
}

// Observations from one failed acquisition, never a lease or pointer authority.
// Reset at entry and left empty on success. Unknown gate fields remain unknown
// when try_lock fails; a gate denial says nothing about range accessibility.
// Explicit caller storage avoids stale, nested or cross-thread attribution.
struct GuestDataLeaseDenialMetadata
{
	GuestDataLeaseDenialReason reason = GuestDataLeaseDenialReason::None;
	uint64_t address = 0, bytes = 0;
	Mode required = Mode::NoAccess;
	uintptr_t acquisition_thread = 0;
	uint64_t generation = UINT64_MAX;
	bool gate_observed = false, mutation_active = false;
	uint64_t waiting_mutations = UINT64_MAX, code_readers = UINT64_MAX;
	bool physical_observed = false;
	int32_t physical_result = 0;
	uint32_t physical_protection = 0;
	// A fresh caller with no local borrow met transient admission contention.
	// Retry only at an outer boundary after releasing subsystem locks and
	// revalidating its inputs. Range, rights and context failures remain false.
	bool retryable = false;
};

// Borrow a host-accessible guest interval until destruction. Apple backends
// check every logical guest page and the physical host permissions while a VM
// reader gate excludes Protect, Free, decommit, patch and address reuse. Native
// (unmanaged) buffers are checked too, but their lifetime remains caller-owned.
// Release on the acquiring thread and do not invoke a VM mutator while holding
// a lease. Initial acquisition waits for pending mutations; nested borrows on
// an existing reader remain admitted so multi-buffer calls cannot deadlock.
// The thread owning an active Apple VM mutation may also borrow checked data
// to publish transaction outputs. End those borrows before Commit/Abort;
// unrelated readers and all code-read leases retain their ordinary admission.
// The reader gate pins Core mappings through logical and physical validation;
// physical queries do not hold the registry mutex against independent readers.
// Failed acquisition must never fall back to an unchecked pointer.
// Android borrows retain an admitted translator host-call reader, or acquire
// a real native data pin. Nested native borrows share that pin until the last
// release. Core mutation owners may borrow checked transaction outputs under
// their still-live token; all borrows end before commit/abort. Every admission
// checks the logical 4 KiB registry and physical VM permissions. A denied
// admission grants no access and must never fall back to VMA-only checks.
// Adopt an exclusive mutation already owned by this native thread. Android
// and macOS verify the exact token with FEX before permitting checked data
// borrows. macOS also pins mapping lifetime through its shared local gate;
// checked native completions may borrow concurrently. Core mutations remain
// excluded by the FEX epoch and must not nest beneath an adopted owner.
// Release every borrow and this scope before ending that token.
class GuestMemoryMutationOwnerScope
{
public:
	explicit GuestMemoryMutationOwnerScope(uint64_t token);
	~GuestMemoryMutationOwnerScope();
	GuestMemoryMutationOwnerScope(const GuestMemoryMutationOwnerScope&) = delete;
	GuestMemoryMutationOwnerScope& operator=(const GuestMemoryMutationOwnerScope&) = delete;
	bool IsActive() const { return token != 0; }

private:
	uint64_t token = 0;
	[[maybe_unused]] uintptr_t acquiring_thread = 0;
};

// Identifies only an adopted exclusive translator epoch still owned by this
// thread. This is metadata-cache scope, never permission to access guest bytes.
// Zero means no qualified scope. This is a non-recycling Core serial, distinct
// from the backend token; ending/re-adopting changes it even if a backend nonce
// eventually repeats. Callers also observe GetMutationGeneration().
uint64_t GetOwnedGuestMemoryMutationToken() noexcept;

// Adopt a native mapping pin already owned by this thread after an exclusive
// downgrade. This scope never releases that backend pin. Every data borrow
// still checks live permissions and must be covered by the active native
// physical claim. End all borrows and this scope before ending the pin.
class GuestNativeMemoryOwnerScope
{
public:
	explicit GuestNativeMemoryOwnerScope(uint64_t native_token);
	~GuestNativeMemoryOwnerScope();
	GuestNativeMemoryOwnerScope(const GuestNativeMemoryOwnerScope&) = delete;
	GuestNativeMemoryOwnerScope& operator=(const GuestNativeMemoryOwnerScope&) = delete;
	bool IsActive() const { return token != 0; }

private:
	uint64_t token = 0;
	[[maybe_unused]] uintptr_t acquiring_thread = 0;
};

// Fresh backing-identity coverage only, never cached permissions or byte
// coherence. False without a current physical claim or for any uncovered byte.
bool CurrentNativeClaimCoversGuestRange(uint64_t address, uint64_t bytes) noexcept;

class GuestDataRangeSeal;

class GuestDataLease
{
	friend bool TryAcquireGuestDataLease(uint64_t address, uint64_t size,
	                                    Mode required, GuestDataLease* lease, bool wait_for_mutation,
	                                    GuestDataLeaseDenialMetadata* denial);
	friend void ReleaseGuestDataLease(GuestDataLease* lease);
	friend bool ValidateGuestDataLeaseRangeContinuity(const GuestDataLease& lease,
	    uint64_t address, uint64_t size, uint64_t original_generation);
	friend bool CaptureGuestDataRangeSeal(const GuestDataLease& lease, GuestDataRangeSeal* seal);
	friend bool ValidateGuestDataRangeSeal(const GuestDataLease& lease, const GuestDataRangeSeal& seal);

public:
	GuestDataLease() = default;
	~GuestDataLease();
	GuestDataLease(const GuestDataLease&) = delete;
	GuestDataLease& operator=(const GuestDataLease&) = delete;
	GuestDataLease(GuestDataLease&&) = delete;
	GuestDataLease& operator=(GuestDataLease&&) = delete;
	bool IsActive() const { return byte_count != 0; }
	uint64_t HostBegin() const { return host_begin; }
	uint64_t ByteCount() const { return byte_count; }
	uint64_t MutationGeneration() const { return mutation_generation; }

private:
	uint64_t host_begin = 0;
	uint64_t byte_count = 0;
	uint64_t mutation_generation = 0;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || \
    LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	uintptr_t acquiring_thread = 0;
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	uint8_t android_admission = 0;
#endif
};

// required must be Read, Write or ReadWrite. Empty, overflowing, reserved and
// partially accessible ranges are rejected before any bytes are accessed.
// Set wait_for_mutation=false when the caller holds a subsystem lock that
// must not wait for a VM writer. Gate contention or a pending/active mutation
// denies the borrow instead; validation and lifetime guarantees are unchanged.
// Optional denial output is reset at entry and populated only for this failed
// call, after local locks end. Its categories/observations never grant a borrow.
bool TryAcquireGuestDataLease(uint64_t address, uint64_t size,
	                          Mode required, GuestDataLease* lease, bool wait_for_mutation = true,
	                          GuestDataLeaseDenialMetadata* denial = nullptr);
void ReleaseGuestDataLease(GuestDataLease* lease);

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
// Copy guest data under real Core/FEX mapping admission. Untagged reads up to
// 256 bytes on non-MTE AArch64 hosts check the full logical range, then copy
// through the kernel into scratch storage; only a complete copy is published.
// Denied or short copies leave destination unchanged and never fall back to
// unchecked access. Tagged or larger reads, MTE-capable hosts, and unavailable
// capability queries retain the ordinary GuestDataLease path. The destination and unmanaged source storage
// remain caller-owned; no atomic-content guarantee is implied. Empty and
// overflowing ranges fail.
bool TryReadGuestData(uint64_t address, void* destination, uint64_t size,
	                 GuestDataLeaseDenialMetadata* denial = nullptr);

enum class PrivateGuestDataCopyResult { Copied, Unsupported, Denied };
inline constexpr size_t PRIVATE_GUEST_DATA_COPY_LIMIT = 2 * 1024 * 1024;

// Copy into caller-owned PRIVATE scratch, under fresh logical/handoff checks
// and real Core/FEX mapping admission. Only Copied authorizes consuming any
// output bytes. Denied may have overwritten an arbitrary prefix: discard the
// entire result, without caching it, retrying, or falling back to a raw read.
// Unsupported leaves scratch untouched and permits the ordinary lease path:
// tagged pointers, >2 MiB, MTE-capable hosts or unavailable capability queries.
// The scratch must be valid, writable and disjoint from the guest source. It
// must not be published to another reader until Copied; no atomic-content
// guarantee or reusable guest permission proof is created.
PrivateGuestDataCopyResult CopyGuestDataToPrivateScratch(uint64_t address, void* scratch, uint64_t size,
	GuestDataLeaseDenialMetadata* denial = nullptr);

// Untagged writes up to 256 bytes on known non-MTE AArch64 hosts use the same admission
// and complete logical-write proof, limited to one 4/16K host page. The caller
// owns a valid source; its bytes are copied to aligned stack storage before a
// kernel write checks live physical permissions. Other cases retain the normal
// write lease. A positive short kernel result is fatal: guest bytes may already
// have changed, so there is no retry, rollback or recoverable partial failure.
// Successful stores retain memcpy's non-atomic semantics for concurrent readers.
bool TryWriteGuestData(uint64_t address, const void* source, uint64_t size);
#endif

// Validate continuity since an earlier generation while the acquiring thread
// owns this exact active data lease. Every byte must still belong to a managed
// mapping created and last mutated no later than original_generation. Changes
// to other mappings are allowed; remap, Protect/revert, patch and uncertain
// mutator footprints revoke the earlier proof. This is mapping-wide and does
// not prove unchanged content, non-executable rights, or atomic guest stores.
// No mapping pointer escapes and no lease may cross a GPU wait. Backends without
// the Apple reader gate return false. Current permissions remain the caller's
// responsibility and must be checked under the same active lease.
bool ValidateGuestDataLeaseRangeContinuity(const GuestDataLease& lease,
    uint64_t address, uint64_t size, uint64_t original_generation);

void ReleaseGuestDataRangeSeal(GuestDataRangeSeal* seal);

// A revocable mapping-continuity ticket, without a retained data lease. Capture
// and validate under an exact active GuestDataLease; reacquire the required
// current permissions before publishing. Android tickets use reusable slots and are
// revoked before overlapping Core mapping/protection mutations, including
// failed operations that may briefly touch host rights. Unrelated ranges do
// not revoke the ticket. No content, atomic-store or native-buffer lifetime
// guarantee is implied, and no seal grants permission to dereference memory.
class GuestDataRangeSeal
{
	friend bool CaptureGuestDataRangeSeal(const GuestDataLease& lease, GuestDataRangeSeal* seal);
	friend bool ValidateGuestDataRangeSeal(const GuestDataLease& lease, const GuestDataRangeSeal& seal);
	friend void ReleaseGuestDataRangeSeal(GuestDataRangeSeal* seal);

public:
	GuestDataRangeSeal() = default;
	~GuestDataRangeSeal() { ReleaseGuestDataRangeSeal(this); }
	GuestDataRangeSeal(const GuestDataRangeSeal&) = delete;
	GuestDataRangeSeal& operator=(const GuestDataRangeSeal&) = delete;
	GuestDataRangeSeal(GuestDataRangeSeal&&) = delete;
	GuestDataRangeSeal& operator=(GuestDataRangeSeal&&) = delete;
	bool IsActive() const { return ticket != 0; }

private:
	uint64_t ticket = 0;
	uint64_t host_begin = 0;
	uint64_t byte_count = 0;
	uint64_t original_generation = 0;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	// Registry slots never move or die before process exit. The ticket still
	// validates their identity across reuse; this pointer only avoids a scan.
	void* android_record = nullptr;
#endif
};

bool CaptureGuestDataRangeSeal(const GuestDataLease& lease, GuestDataRangeSeal* seal);
bool ValidateGuestDataRangeSeal(const GuestDataLease& lease, const GuestDataRangeSeal& seal);

// For generated-access callbacks only: the caller must already own FEX's
// execution barrier through the complete access. This lock-free snapshot query
// neither acquires a lease nor admits native/unmanaged buffers. Other backends
// fail closed. Ordinary HLE callers must use GuestDataLease instead.
bool CheckGuestDataAccessUnderFexExecutionBarrier(uint64_t address, uint64_t size,
	                                             bool write) noexcept;

// Trusted embedding-owned stack only. The owner guarantees that its complete
// raw allocation remains RW until the synchronous Run returns. This permits
// unmanaged bytes only inside that exact interval; managed rights, guards,
// reservations and unavailable internal snapshots still deny access. The FEX
// execution barrier must remain held through the final native access.
bool CheckOwnedStackAccessUnderFexExecutionBarrier(uint64_t address, uint64_t size,
	bool write, uint64_t owned_begin, uint64_t owned_size) noexcept;

// Sparse virtual reservations are distinct from visible mappings. The opaque
// identity prevents a stale owner from releasing a new reservation that later
// reused the same base address.
struct ReservationHandle
{
	uint64_t base     = 0;
	uint64_t size     = 0;
	uint64_t identity = 0;

	bool IsValid() const { return base != 0 && size != 0 && identity != 0; }
};

enum class ReservedFixedResult : uint32_t;
struct AllocationDenialMetadata;

enum class ReservationPlacement : uint32_t
{
	Exact,
	PreferAddress,
};

// A begun mutation remains behind the platform VM/FEX gates until Commit or
// Abort. Windows and Android also hold their reservation-registry transaction.
// The Android owner may reenter QueryManagedRegion/IsManagedAddress for final
// guest-output validation; do not reenter mutators or other Core VM APIs.
// Windows permits no Core VM reentry. Destruction aborts an unfinished
// transaction. Complete it on the beginning thread, after releasing any
// guest-data borrows acquired inside it.
class MutationTransaction
{
	friend bool BeginReserveAligned(uint64_t address, uint64_t size,
	                               uint64_t alignment,
	                               ReservationHandle* handle,
	                               MutationTransaction* transaction,
	                               ReservationPlacement placement,
	                               AllocationDenialMetadata* denial);
	friend ReservedFixedResult BeginAllocFixedInReservation(
	    uint64_t address, uint64_t size, Mode mode,
	    MutationTransaction* transaction);

public:
	MutationTransaction() = default;
	~MutationTransaction();
	MutationTransaction(const MutationTransaction&) = delete;
	MutationTransaction& operator=(const MutationTransaction&) = delete;
	MutationTransaction(MutationTransaction&&) = delete;
	MutationTransaction& operator=(MutationTransaction&&) = delete;

	bool IsActive() const { return state != nullptr; }
	void Commit();
	bool Abort();

private:
	void* state = nullptr;
};

enum class ReservedFixedResult : uint32_t
{
	NotReserved,
	Failed,
	Begun,
};

bool BeginReserveAligned(uint64_t address, uint64_t size, uint64_t alignment,
	                     ReservationHandle* handle,
	                     MutationTransaction* transaction,
	                     ReservationPlacement placement = ReservationPlacement::Exact,
	                     AllocationDenialMetadata* denial = nullptr);
ReservedFixedResult BeginAllocFixedInReservation(
	uint64_t address, uint64_t size, Mode mode,
	MutationTransaction* transaction);
bool HasReservationOverlap(uint64_t address, uint64_t size);
bool ReleaseReservation(const ReservationHandle& handle);

#if defined(LUMEN_VIRTUAL_RANGE_TEST_HOOK)
bool GuestDataSnapshotPreflightEnabledForTest();

enum class VirtualRangeFaultForTest : uint32_t
{
	None,
	CoreCapacity,
	SnapshotPublish,
	FexAdditiveAdmission,
	FexExclusiveAdmission,
	ActivateHost,
	DecommitHost,
	ReleaseHost,
	AbortHost,
	ProtectAfterHost,
};

struct VirtualRangeTransactionStateForTest
{
	uint64_t begins = 0;
	uint64_t closes = 0;
	uint64_t closes_while_gate_owned = 0;
	uint64_t active = 0;
	uint64_t waiting_guest_data_readers = 0;
	uint64_t waiting_mutations = 0;
};

void FailNextVirtualRangeOperationForTest(VirtualRangeFaultForTest fault);
// Applies only to the calling thread; nullptr removes the hook.
void SetGuestDataPhysicalValidationHookForTest(void (*hook)());
// Called on the mutation owner after the Core gate is released inside the
// FEX publication phase, before guest execution is readmitted. No mutations.
void SetMemoryMutationLocalReleaseHookForTest(void (*hook)());
// The callback runs while the guest-data admission mutex is held. Test
// synchronization only; it must not acquire leases or mutate virtual memory.
void WithGuestDataGateLockedForTest(void (*callback)(void*), void* context);
size_t ReservationCountForTest();
VirtualRangeTransactionStateForTest
GetVirtualRangeTransactionStateForTest();
#endif

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
enum class ManagedRegionState : uint32_t
{
	Unmanaged,
	Reserved,
	Visible
};

struct ManagedRegion
{
	uint64_t begin;
	uint64_t end;
	Mode     mode;
};

ManagedRegionState QueryManagedRegion(uint64_t address, ManagedRegion* region);

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
// Fault reporting cannot fall back to the registry mutex: a failed native
// guest callback may have abandoned that lock. Return false when the immutable
// snapshot is unavailable or a mutation is active.
bool TryQueryManagedRegionSnapshot(uint64_t address,
	                               ManagedRegionState* state,
	                               ManagedRegion* region);

// Optional bounded snapshots of the small managed regions recorded since
// startup (selector LUMEN_MACOS_VM_REGION_SNAPSHOT). "tag" names the point in
// time inside the file name; local diagnostics only.
bool SnapshotRecordedRegions(const char* tag);

// Optional bounded change ledger of the same recorded small regions: keeps one
// in-memory copy per region and reports the numeric byte spans that changed
// between two observations, plus (when LUMEN_MACOS_VM_LEDGER_REFERENCE names an
// address range) the region offsets that hold an eight-byte word inside it.
// "tag" labels the trigger, "force" compares even when the slice interval has
// not elapsed. Process-latched selector LUMEN_MACOS_VM_LEDGER; a no-op when it
// is disabled, safe from any guest thread, and bounded in records and bytes.
// Local diagnostics only: it never changes emulated memory or guest state.
void TickRecordedRegionLedger(const char* tag, bool force);

// Local capture diagnostics: thread attribution shared by the managed-region
// ledger and the guest-root snapshot. Thread-local and numeric, set by the
// emulator layer at guest-thread attach and at every HLE boundary: the guest
// pthread handle of the calling thread and the HLE import target it entered
// most recently (zero before the first setter runs). Read-only for the
// diagnostics; never changes guest state.
void SetCaptureDiagnosticGuestThread(uint64_t guestThreadHandle);
void NoteCaptureDiagnosticHostCall(uint64_t hostCallTarget);
[[nodiscard]] uint64_t CaptureDiagnosticHostCall();

// Optional bounded per-guest-thread root snapshot for the stop-the-world
// capture question (selector LUMEN_MACOS_VM_ROOT_SNAPSHOT). One numeric record
// per guest thread with the fifteen GP words the title's collector scans, in
// the delivered-ucontext order, plus RSP and RIP; the record is the thread's
// most recent published boundary state. The stack window [RSP, RSP+1MiB) is
// read through the kernel, clamped to the private x86-64 stack allocation
// [stackBase, stackBase+stackSize) that owns RSP, and scanned for eight-byte
// words inside LUMEN_MACOS_VM_LEDGER_REFERENCE (a size-relative reference is
// resolved against the recorded regions of that size). `gpr` follows the
// x86-64 order (0=rax .. 4=rsp .. 7=rdi .. 15=r15). Process-latched, off by
// default, bounded in records and bytes; local diagnostic only.
bool GuestRootSnapshotEnabled();
void ReportGuestThreadRoots(const char* tag, uint32_t threadId, uint64_t handle,
                            uint64_t hostCall, uint64_t stackBase,
                            uint64_t stackSize, const uint64_t gpr[16],
                            uint64_t rip);
// One bounded summary record per snapshot call: how many guest-thread slots
// were claimed and how many were reported. Distinguishes a snapshot that ran
// with no published state from one that never ran.
void ReportGuestRootSnapshotSummary(const char* tag, size_t claimed, size_t reported);
#endif

// Process-latched product selector. The promoted macOS default is enabled;
// exact zero, empty and malformed values select the legacy registry path.
// Other platforms remain default-off until separately validated. Exposed so
// subprocess tests can prove both product and rollback behavior without
// relying on log text. FEX Mac also retains an internal permission snapshot
// for signal-safe access validation when this query acceleration is disabled.
bool IsImmutableRegionSnapshotEnabled();

// FEX performs guest data accesses directly. Its Apple mappings and Android
// mappings on 16 KiB hosts must therefore have one effective data permission
// per physical host page. This is a fail-closed policy, not software enforcement
// of arbitrary guest subpage permissions or of allocation padding.
bool RequiresUniformHostPageDataProtection();
#endif

// True only for addresses inside anonymous mappings created by this API.
// Mobile runtimes use this to reject managed guest targets before consulting
// the native image loader.
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || \
    LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
bool IsManagedAddress(uint64_t address);
#endif

enum class AllocationFailure : uint8_t { None, InvalidArgument, AdmissionDenied, HostAllocationFailed };
struct AllocationDenialMetadata
{
	AllocationFailure reason = AllocationFailure::None;
	// Only a provider-qualified, effect-free admission conflict permits retry.
	bool retryable = false;
};
uint64_t Alloc(uint64_t address, uint64_t size, Mode mode, AllocationDenialMetadata* denial = nullptr);
uint64_t AllocAligned(uint64_t address, uint64_t size, Mode mode, uint64_t alignment);
bool     AllocFixed(uint64_t address, uint64_t size, Mode mode);
bool     Free(uint64_t address);
bool     Protect(uint64_t address, uint64_t size, Mode mode, Mode* old_mode = nullptr);
bool     FlushInstructionCache(uint64_t address, uint64_t size);
bool     PatchReplace(uint64_t vaddr, uint64_t value);

} // namespace VirtualMemory

} // namespace Lumen::Core

#endif /* INCLUDE_LUMEN_CORE_VIRTUALMEMORY_H_ */
