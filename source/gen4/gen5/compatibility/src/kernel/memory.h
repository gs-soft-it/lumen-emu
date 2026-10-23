#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_MEMORY_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_MEMORY_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/virtualMemory.h"

#include <functional>
#include <vector>

namespace Libs::Graphics {
class RenderContext;
enum class PageFaultAccess;
} // namespace Libs::Graphics

namespace Libs::LibKernel::Memory {

void Initialize();
void Shutdown();

struct Lifecycle {
	static constexpr const char* name       = "Memory";
	static constexpr auto        initialize = Libs::LibKernel::Memory::Initialize;
	static constexpr auto        shutdown   = Libs::LibKernel::Memory::Shutdown;
};

using callback_func_t = void (*)(uintptr_t addr, size_t size);

constexpr uint32_t KERNEL_MAXIMUM_NAME_LENGTH = 32;
constexpr uint64_t kExtendedMemoryBase       = 0x080000000000ull;
constexpr uint64_t kExtendedMemorySize       = 512ull * 1024 * 1024 * 1024;

struct VirtualQueryInfo {
	uintptr_t start;
	uintptr_t end;
	uint64_t  offset;
	int32_t   protection;
	int32_t   memory_type;
	uint32_t  is_flexible  : 1;
	uint32_t  is_direct    : 1;
	uint32_t  is_stack     : 1;
	uint32_t  is_pooled    : 1;
	uint32_t  is_committed : 1;
	uint32_t  is_gpu_prt   : 1;
	uint32_t  amm_usage    : 1;
	uint32_t  reserved     : 1;
	char      name[KERNEL_MAXIMUM_NAME_LENGTH];
	uint8_t   gpu_mask_id;
	uint8_t   reserved2;
};

static_assert(sizeof(VirtualQueryInfo) == 72, "VirtualQueryInfo struct size is incorrect");

struct KernelBatchMapEntry {
	void*         start;
	uint64_t      offset;
	uint64_t      length;
	unsigned char protection;
	unsigned char type;
	int16_t       reserved;
	int32_t       operation;
};

static_assert(sizeof(KernelBatchMapEntry) == 32, "KernelBatchMapEntry struct size is incorrect");

struct KernelMemoryPoolBatchEntry {
	uint32_t op;
	uint32_t flags;
	union {
		struct {
			void*    addr;
			uint64_t len;
			uint8_t  prot;
			uint8_t  type;
		} commit;
		struct {
			void*    addr;
			uint64_t len;
		} decommit;
		struct {
			void*    addr;
			uint64_t len;
			uint8_t  prot;
		} protect;
		struct {
			void*    addr;
			uint64_t len;
			uint8_t  prot;
			uint8_t  type;
		} type_protect;
		struct {
			void*    dst;
			void*    src;
			uint64_t len;
		} move;
		uintptr_t padding[3];
	};
};

static_assert(sizeof(KernelMemoryPoolBatchEntry) == 32,
              "KernelMemoryPoolBatchEntry struct size is incorrect");

struct KernelMemoryPoolBlockStats {
	int32_t available_flushed_blocks;
	int32_t available_cached_blocks;
	int32_t allocated_flushed_blocks;
	int32_t allocated_cached_blocks;
};

static_assert(sizeof(KernelMemoryPoolBlockStats) == 16,
              "KernelMemoryPoolBlockStats struct size is incorrect");

void                   RegisterCallbacks(callback_func_t alloc_func, callback_func_t free_func);
void                   SetFlexibleMemorySize(uint64_t size);
int AllocateDirectMemory(int64_t search_start, int64_t search_end, size_t size, size_t alignment,
                         int memory_type, int64_t* phys_addr_out, bool automatic = false);
int MapAutomaticMemory(uint64_t vaddr, size_t size, int type, int prot);
// Uniform-metadata provenance snapshot, not a memory lease. OFF/unavailable
// returns false/zero. A successful query admits one complete backing mapping
// without physical aliases; retain normal guest/GPU synchronization and lifetime.
// Only the same nonzero even generation is reusable; odd means mutation active.
// These functions never acquire the outer VM operation mutex or wait for GPU work.
bool QueryUniqueBackingGeneration(uint64_t vaddr, uint64_t size, uint64_t* generation);
uint64_t BackingMutationGeneration() noexcept;
bool                   TryWriteBacking(uint64_t vaddr, const void* data, uint64_t size);
bool                   TryReadBacking(uint64_t vaddr, void* data, uint64_t size);
// False includes incomplete/revoked backing. The caller must stabilize CPU writes.
bool                   TryEqualBacking(uint64_t vaddr, const void* data, uint64_t size);
bool                   TryReadGpuCleanBacking(uint64_t vaddr, void* data, uint64_t size);
// GPU-thread specialization read: publish buffer producers across physical aliases;
// dirty images still require the renderer dependency path.
bool                   TryReadBufferBacking(uint64_t vaddr, void* data, uint64_t size);
bool                   TryReadSparseBacking(uint64_t vaddr, void* data, uint64_t size);
// Same backing-reader authority as TryReadSparseBacking: caller stabilizes CPU
// writes. Hash every byte afresh only when one shared VM view and one physical
// backing map contain the full span. Both mapping locks remain held through
// XXH3; no host pointer escapes. False leaves hash unchanged and requires the
// existing sparse reader (holes/splits/private/pooled/invalid ranges are not elided).
bool                   TryHashContiguousBacking(uint64_t vaddr, uint64_t size, uint64_t* hash);
[[nodiscard]] uint64_t ClampRangeSize(uint64_t vaddr, uint64_t size);
void                   WriteBacking(uint64_t vaddr, const void* data, uint64_t size) noexcept;
void                   InvalidateMemory(uint64_t vaddr, uint64_t size);
// Synchronously publish a complete native update on the renderer owner. The
// callback and its storage must remain valid until return; hold no VM/cache lock.
// Call while the renderer accepts work, never from a GPU completion callback.
void PublishGpuMemory(uint64_t vaddr, uint64_t size, const std::function<void()>& write);
void                   InstallGpuResources(Graphics::RenderContext* renderer) noexcept;
[[nodiscard]] bool HandleGpuFault(Graphics::PageFaultAccess access, uint64_t fault_vaddr) noexcept;

int KYTY_SYSV_ABI KernelMapNamedFlexibleMemory(void** addr_in_out, size_t len, int prot, int flags,
                                               const char* name);
int KYTY_SYSV_ABI KernelMapFlexibleMemory(void** addr_in_out, size_t len, int prot, int flags);
int KYTY_SYSV_ABI KernelSetVirtualRangeName(const void* addr, uint64_t len, const char* name);
int KYTY_SYSV_ABI KernelClearVirtualRangeName(const void* addr, uint64_t len);
int KYTY_SYSV_ABI KernelMunmap(uint64_t vaddr, size_t len);
size_t KYTY_SYSV_ABI KernelGetDirectMemorySize();
int KYTY_SYSV_ABI    KernelAvailableDirectMemorySize(int64_t search_start, int64_t search_end,
                                                     size_t alignment, int64_t* phys_addr_out,
                                                     size_t* size_out);
int KYTY_SYSV_ABI    KernelGetPageTableStats(int* cpu_total, int* cpu_available, int* gpu_total,
                                             int* gpu_available);
int KYTY_SYSV_ABI KernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t len,
                                             size_t alignment, int memory_type,
                                             int64_t* phys_addr_out);
int KYTY_SYSV_ABI KernelAllocateMainDirectMemory(size_t len, size_t alignment, int memory_type,
                                                 int64_t* phys_addr_out);
int KYTY_SYSV_ABI KernelCheckedReleaseDirectMemory(int64_t start, size_t len);
int KYTY_SYSV_ABI KernelReleaseDirectMemory(int64_t start, size_t len);
int KYTY_SYSV_ABI KernelMapDirectMemory(void** addr, size_t len, int prot, int flags,
                                        int64_t direct_memory_start, size_t alignment);
int KYTY_SYSV_ABI KernelMapDirectMemory2(void** addr, size_t len, int type, int prot, int flags,
                                         int64_t direct_memory_start, size_t alignment);
int KYTY_SYSV_ABI KernelMapNamedDirectMemory(void** addr, size_t len, int prot, int flags,
                                             int64_t direct_memory_start, size_t alignment,
                                             const char* name);
int KYTY_SYSV_ABI KernelSetPrtAperture(int index, void* addr, size_t len);
int KYTY_SYSV_ABI KernelGetPrtAperture(int index, void** addr, size_t* len);
int KYTY_SYSV_ABI KernelIsAddressSanitizerEnabled();
int KYTY_SYSV_ABI KernelQueryMemoryProtection(void* addr, void** start, void** end, int* prot);
int KYTY_SYSV_ABI KernelDirectMemoryQuery(int64_t offset, int flags, void* info, size_t info_size);
int KYTY_SYSV_ABI KernelVirtualQuery(const void* addr, int flags, VirtualQueryInfo* info,
                                     uint64_t info_size);
int KYTY_SYSV_ABI KernelIsStack(void* addr, void** start, void** end);
int KYTY_SYSV_ABI KernelReserveVirtualRange(void** addr, size_t len, int flags, size_t alignment);
int KYTY_SYSV_ABI KernelAvailableFlexibleMemorySize(size_t* size);
int KYTY_SYSV_ABI KernelConfiguredFlexibleMemorySize(size_t* size);
int KYTY_SYSV_ABI KernelMprotect(const void* addr, size_t len, int prot);
int KYTY_SYSV_ABI KernelMtypeprotect(const void* addr, size_t len, int type, int prot);
int KYTY_SYSV_ABI KernelBatchMap(KernelBatchMapEntry* entries, int num_entries,
                                 int* num_entries_out);
int KYTY_SYSV_ABI KernelBatchMap2(KernelBatchMapEntry* entries, int num_entries,
                                  int* num_entries_out, int flags);
int KYTY_SYSV_ABI KernelMemoryPoolExpand(int64_t search_start, int64_t search_end, size_t len,
                                         size_t alignment, int64_t* phys_addr_out);
int KYTY_SYSV_ABI KernelMemoryPoolReserve(void* addr_in, size_t len, size_t alignment, int flags,
                                          void** addr_out);
int KYTY_SYSV_ABI KernelMemoryPoolCommit(void* addr, size_t len, int type, int prot, int flags);
int KYTY_SYSV_ABI KernelMemoryPoolDecommit(void* addr, size_t len, int flags);
int KYTY_SYSV_ABI KernelMemoryPoolBatch(const KernelMemoryPoolBatchEntry* entries, int num_entries,
                                        int* num_entries_out, int flags);
int KYTY_SYSV_ABI KernelMemoryPoolGetBlockStats(KernelMemoryPoolBlockStats* output,
                                                size_t                      output_size);

uint64_t AllocateProgramMemory(uint64_t search_addr, uint64_t size,
                               Common::VirtualMemory::Mode mode, const char* name);
void SetProgramMemoryProtection(uint64_t vaddr, uint64_t size, Common::VirtualMemory::Mode mode);
// Trusted loader relocation only: Code-owner validation, temporary native write
// admission, original guest rights restored before FEX invalidation/readmission.
bool PatchProgramMemory64(uint64_t vaddr, uint64_t value, bool* changed) noexcept;
uint64_t AllocateRuntimeMemory(uint64_t search_addr, uint64_t size,
                               Common::VirtualMemory::Mode mode, const char* name,
                               bool fixed = false);
uint64_t AllocateGuestStackMemory(uint64_t search_addr, uint64_t size,
                                  Common::VirtualMemory::Mode mode, const char* name);
bool     ProtectGuestMemory(uint64_t vaddr, uint64_t size, Common::VirtualMemory::Mode mode,
                            Common::VirtualMemory::Mode* old_mode = nullptr);
// Transient PageManager watch state; does not change the guest mapping's semantic protection.
bool ProtectGuestHostMemory(uint64_t vaddr, uint64_t size, Common::VirtualMemory::Mode mode);
bool FreeGuestMemory(uint64_t vaddr, uint64_t size);

// Checked FEX embedding access. QueryGuestAccess is signal-safe only while the
// caller owns the audited execution/mutation barrier or a memory-operation gate.
// Read/write callbacks serialize the complete permission check and copy.
bool InitializeFexMemoryAccess() noexcept;
bool QueryGuestAccess(uint64_t vaddr, uint64_t size, Common::VirtualMemory::Mode mode) noexcept;
// Caller owns a real FEX exclusive epoch or native data pin. GPU producers need shared
// backing, GPU write rights and no executable guest pages, independently of
// CPU view protection.
bool QueryGpuBackingWrite(uint64_t vaddr, uint64_t size) noexcept;
struct GuestBackingAlias {
	uint64_t address = 0;
	uint64_t size = 0;
	bool source = false;
};
enum class GuestBackingKind { Direct, Flexible, Private };
struct GuestBackingRange {
	uint64_t address = 0;
	uint64_t size = 0;
	uint64_t backing_id = 0;
	uint64_t backing_offset = 0;
	uint64_t mapping_id = 0;
	int protection = 0;
	GuestBackingKind kind = GuestBackingKind::Private;
};
// Resolve the complete VA interval to physical allocation identities and offsets.
// Distinct aliases share backing_id+offset, but have distinct mapping_id. Splits
// and protection changes retain identities; release/reuse never reuses an ID.
// IDs describe process-local lifetimes, not pointers, access rights or leases.
// Caller must retain the real exclusive epoch/native lifetime admission while
// using results. This query does not permit CPU/GPU concurrency or change the
// EXCLUSIVE publication policy. Reserved, pooled, invalid and uncovered ranges
// fail closed and leave ranges empty; pooled enrollment remains unimplemented.
bool QueryGuestBackingRanges(uint64_t vaddr, uint64_t size,
                             std::vector<GuestBackingRange>* ranges) noexcept;

// A borrowed host alias of a fully proved GPU-writable backing envelope. The
// import owner must finish GPU work and destroy its native imported resources
// before this descriptor. It keeps the current EXCLUSIVE epoch closed; it is
// never an independent permission to retain a pointer across CPU readmission.
class GuestGpuBackingImport final {
public:
	GuestGpuBackingImport() noexcept = default;
	~GuestGpuBackingImport();
	GuestGpuBackingImport(GuestGpuBackingImport&& other) noexcept;
	GuestGpuBackingImport& operator=(GuestGpuBackingImport&& other) noexcept;
	GuestGpuBackingImport(const GuestGpuBackingImport&) = delete;
	GuestGpuBackingImport& operator=(const GuestGpuBackingImport&) = delete;
	[[nodiscard]] explicit operator bool() const noexcept { return token_ != 0; }
	[[nodiscard]] void* HostPointer() const noexcept { return host_pointer_; }
	[[nodiscard]] uint64_t ByteSize() const noexcept { return byte_size_; }
	[[nodiscard]] uint64_t GuestOffset() const noexcept { return guest_offset_; }
	void Reset() noexcept;
private:
	friend bool TryAcquireGuestGpuBackingImport(uint64_t, uint64_t, uint64_t, GuestGpuBackingImport*) noexcept;
	void* host_pointer_ = nullptr;
	uint64_t byte_size_ = 0;
	uint64_t guest_offset_ = 0;
	uint64_t token_ = 0;
};

// Round the host pointer down and its extent up to the supplied power-of-two
// import alignment, proving the ENTIRE corresponding guest envelope. Denies
// outside a top-level native FEX exclusive epoch, physical discontinuity,
// private/pooled/uncommitted backing, non-GPU-writable bytes or executable
// aliases. No effects/output on denial. Unsupported Vulkan requirements must
// fall back before issuing commands; the caller may not enlarge this envelope.
bool TryAcquireGuestGpuBackingImport(uint64_t vaddr, uint64_t size,
                                    uint64_t import_alignment,
                                    GuestGpuBackingImport* output) noexcept;
// Caller pins a real FEX exclusive epoch or native data lease. Return every virtual mapping of
// the requested backing bytes, including the source, as sorted disjoint spans.
// source identifies spans within the original virtual interval; alternate
// mappings stay separate even when their addresses happen to be adjacent.
// Failed coverage/identity validation leaves aliases empty. Existing-backing VM
// mutations drain retained owners before effects; fresh additive maps require
// no prior physical mapping. No owner can survive backing reuse.
// A single input interval containing repeated Direct physical bytes is rejected.
bool QueryGuestBackingAliases(uint64_t vaddr, uint64_t size,
                              std::vector<GuestBackingAlias>* aliases) noexcept;
// Invalid alias/rights coverage is conservatively CPU-visible. Any read, write
// or execute right on any physical alias requires completed CPU publication.
bool QueryGuestBackingCpuVisible(uint64_t vaddr, uint64_t size) noexcept;
// Any executable physical alias forbids a concurrent native raw store, even
// when the source view is CPU-only writable. Invalid identity/rights coverage
// conservatively returns true. Caller retains real mapping lifetime admission.
bool QueryGuestBackingHasExecutableAlias(uint64_t vaddr, uint64_t size) noexcept;
uint32_t QueryGuestExecutable(void* opaque, uint64_t address, uint64_t* range_base,
                              uint64_t* range_size, uint32_t* writable) noexcept;
bool ReadGuestMemory(void* opaque, uint64_t address, void* destination, size_t size) noexcept;
// Synchronous HLE bridge read. The optional owned-stack path retains an existing
// counted reader and proves the full range from the current immutable snapshot.
bool TryReadOwnedGuestStackForHostCall(uint64_t address, void* destination, size_t size) noexcept;
bool ReadGuestMemoryForHostCall(void* opaque, uint64_t address, void* destination, size_t size) noexcept;
bool WriteGuestMemory(void* opaque, uint64_t address, const void* source, size_t size) noexcept;
// Synchronous callback frame data copy under the current HLE reader. The full
// scratch/return/spill span must be within that thread's owned private RW stack.
// No mappings, code, rights or mutation epochs are changed; release before Run.
bool PrepareGuestCallbackStack(void* opaque, uint64_t rsp, uint64_t root_return,
                               const uint64_t* spills, size_t spill_count) noexcept;

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
void     TestSetBackingReadCallback(callback_func_t callback);
void     TestFailNextPhysicalMemoryUnmap();
void     TestFailPhysicalMemoryUnmapAfter(uint32_t successful_unmaps);
void     TestFailGuestBackingStoreUnmapAfter(uint32_t successful_unmaps);
void     TestFailNextFixedReserveRangeRegistration();
void     TestFailNextVirtualRangeReplacement();
bool     TestPlaceholderRangeIsFree(uint64_t vaddr, uint64_t size);
bool     TestGuestAddressRangeIsOwned(uint64_t vaddr, uint64_t size);
bool     TestGuestBackingOutsideAddressSpace();
uint64_t TestGuestBackingSize();
bool     TestGuestFreeRangeBounds();
#endif

} // namespace Libs::LibKernel::Memory

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_MEMORY_H_ */
