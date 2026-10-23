// Lumen modification, 2026-10-02: FEX gate normalization lives in Emulator/Common.h.
#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_RUNTIMELINKER_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_RUNTIMELINKER_H_

#include "Lumen/Core/Common.h"
#include "Lumen/Core/Hashmap.h"
#include "Lumen/Core/String.h"
#include "Lumen/Core/Threads.h"
#include "Lumen/Core/Vector.h"

#include "Emulator/Common.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include <string>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Core::VirtualMemory {
class ExceptionHandler;
} // namespace Lumen::Core::VirtualMemory

namespace Lumen::Loader {

class Elf64;
struct Elf64_Phdr;
struct Elf64_Sym;
struct Elf64_Rela;
class RuntimeLinker;

using module_func_t = int (*)(size_t args, const void* argp);

// A supported arm64/FEX HLE may release its host-memory reader only around
// native work which uses copied inputs and independently live host objects.
// No guest pointer may be accessed in this scope, including through another
// HLE. Existing VM leases or an ineligible Run keep the scope inert and retain
// the ordinary reader. After destruction, revalidate every guest output before
// writing it; readmission cannot preserve mappings changed during the wait.
class ScopedGuestHostWait final
{
public:
	ScopedGuestHostWait() noexcept;
	~ScopedGuestHostWait() noexcept;
	LUMEN_CLASS_NO_COPY(ScopedGuestHostWait);
	[[nodiscard]] bool IsActive() const noexcept { return m_token != 0; }

private:
	[[maybe_unused]] uint64_t m_token = 0;
};

// Session result, separate from cooperative cancellation. Only an actual
// backend failure records a nonzero code; ordinary stop/return never clears
// it. Reset only while no guest roots from the preceding session remain.
void ResetGuestExecutionBackendFailure();
bool RecordGuestExecutionBackendFailure(uint32_t code);
uint32_t GuestExecutionBackendFailure();

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
// Mobile shells stop each guest cooperatively before releasing their runtime
// presentation. iOS resets in-process; Android then terminates only its
// isolated runtime process and reveals the still-running dashboard process.
void ResetGuestExecutionStop();
void RequestGuestExecutionStop();
// Requests the whole guest session to close. The return value reports whether
// SDL accepted the quit event; CPU stop remains latched if delivery fails.
[[nodiscard]] bool RequestGuestSessionExit();
bool GuestExecutionStopRequested();
bool GuestExecutionIsIdle();
void GuestExecutionActivityBegin();
void GuestExecutionActivityEnd();
void GuestExecutionActivityEndAllForCurrentThread();
#endif

#if defined(__aarch64__)
uint64_t RunGuestX64(uint64_t addr, uint64_t arg0 = 0, uint64_t arg1 = 0, uint64_t arg2 = 0);
// Explicit handoff for an audited HLE which holds only native snapshots and
// scalar addresses, with no guest borrow or local lock. The provider releases the
// outer reader and validates the parent mapping lifetime before and after the
// callback. A denied handoff never falls back to ordinary nested execution.
bool TryRunGuestX64WithoutGuestBorrow(uint64_t addr, uint64_t arg0,
                                    uint64_t arg1, uint64_t arg2,
                                    uint64_t* result);
#endif

// GP registers captured at the HLE boundary of the host call currently
// executing on this thread; null outside a host call. The pointer refers to
// 16 registers in x86-64 order and stays valid for the duration of that
// synchronous call. Blocking HLE waits use it to reconstruct the interrupted
// state of a pending guest exception delivered on this thread.
const uint64_t* TryGetActiveHostCallGuestGprs();

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
// Bounds of the private x86-64 stack allocation that owns the current guest
// run's RSP on this thread; false outside a run or when the allocation is
// unknown. Nested runs push and restore their own bounds. Used by the local
// guest-root snapshot (selector LUMEN_MACOS_VM_ROOT_SNAPSHOT) to bound its
// stack window and by the guest thread-attribute query (`PthreadAttrGet` on
// the calling thread) to report the stack the thread actually runs on, never
// to infer access from a guest RSP.
bool CurrentGuestRunStackBounds(uint64_t* base, uint64_t* size);
#endif

struct ModuleId
{
	bool operator==(const ModuleId& other) const
	{
		return version_major == other.version_major && version_minor == other.version_minor && name == other.name;
	}

	String id;
	int    version_major;
	int    version_minor;
	String name;
};

struct LibraryId
{
	bool operator==(const LibraryId& other) const { return version == other.version && name == other.name; }

	String id;
	int    version;
	String name;
};

struct ThreadLocalStorage
{
	uint64_t image_vaddr   = 0;
	uint64_t image_size    = 0;
	uint64_t handler_vaddr = 0;

	Core::Hashmap<int, uint8_t*> tlss;
	Core::Mutex                  mutex;
};

// HeapAPI structure - functions provided by the application for memory allocation
struct HeapAPI
{
	void* (*heap_malloc)(size_t);
	void (*heap_free)(void*);
	void* (*heap_calloc)(size_t, size_t);
	void* (*heap_realloc)(void*, size_t);
	void* (*heap_memalign)(size_t, size_t);
	int (*heap_posix_memalign)(void**, size_t, size_t);
	int (*heap_reallocalign)(void);
	void (*heap_malloc_stats)(void);
	int (*heap_malloc_stats_fast)(void);
	size_t (*heap_malloc_usable_size)(void*);
};

struct DynamicInfo
{
	void*    hash_table      = nullptr;
	uint64_t hash_table_size = 0;

	char*    str_table      = nullptr;
	uint64_t str_table_size = 0;

	Elf64_Sym* symbol_table            = nullptr;
	uint64_t   symbol_table_total_size = 0;
	uint64_t   symbol_table_entry_size = 0;

	uint64_t init_vaddr          = 0;
	uint64_t fini_vaddr          = 0;
	uint64_t init_array_vaddr    = 0;
	uint64_t fini_array_vaddr    = 0;
	uint64_t preinit_array_vaddr = 0;
	uint64_t init_array_size     = 0;
	uint64_t fini_array_size     = 0;
	uint64_t preinit_array_size  = 0;
	uint64_t pltgot_vaddr        = 0;

	Elf64_Rela* jmprela_table      = nullptr;
	uint64_t    jmprela_table_size = 0;

	Elf64_Rela* rela_table            = nullptr;
	uint64_t    rela_table_total_size = 0;
	uint64_t    rela_table_entry_size = 0;

	uint64_t relative_count = 0;

	uint64_t debug   = 0;
	uint64_t textrel = 0;
	uint64_t flags   = 0;

	const char* so_name = nullptr;

	Vector<const char*> needed;
	Vector<ModuleId>    export_modules;
	Vector<ModuleId>    import_modules;
	Vector<LibraryId>   export_libs;
	Vector<LibraryId>   import_libs;
};

struct Program
{
	int32_t                                unique_id = -1;
	RuntimeLinker*                         rt        = nullptr;
	String                                 file_name;
	Elf64*                                 elf               = nullptr;
	Core::VirtualMemory::ExceptionHandler* exception_handler = nullptr;
	bool                                   exception_handler_ready = false;
	DynamicInfo*                           dynamic_info      = nullptr;
	uint64_t                               base_vaddr        = 0;
	uint64_t                               base_size         = 0;
	uint64_t                               base_size_aligned = 0;
	SymbolDatabase*                        export_symbols    = nullptr;
	SymbolDatabase*                        import_symbols    = nullptr;
	ThreadLocalStorage                     tls;
	bool                                   fail_if_global_not_resolved = true;
	bool                                   defer_unresolved_plt_functions = false;
	bool                                   unload_in_progress             = false;
	bool                                   relocations_applied            = false;
	// Live guest load references taken through `sceKernelLoadStartModule`. The
	// console loader keeps one instance per module file the way dlopen does:
	// a repeated load of a live module returns this instance and gains a
	// reference, and the module may be stopped and unloaded only after the last
	// reference is released. Modules mapped by the launcher start at zero.
	uint32_t                               load_reference_count             = 0;
	Vector<uint32_t>                       unresolved_rela_indices;
	Vector<uint32_t>                       unresolved_jmprela_indices;
	bool                                   dbg_print_reloc             = false;
	uint64_t                               proc_param_vaddr            = 0;
	uint64_t                               custom_call_plt_vaddr       = 0;
	uint32_t                               custom_call_plt_num         = 0;
};

struct ProgramUnwindInfo
{
	char     name[256] {};
	uint64_t eh_frame_hdr_addr = 0;
	uint64_t eh_frame_addr     = 0;
	uint64_t eh_frame_size     = 0;
	uint64_t seg0_addr         = 0;
	uint64_t seg0_size         = 0;
};

#if defined(__aarch64__) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
bool ValidateGuestMemory(uint64_t address, size_t size, bool write);
// Consume the destination only on success; a failed snapshot may be partial.
bool ReadGuestMemory(uint64_t address, void* destination, size_t size);
bool WriteGuestMemory(uint64_t address, const void* source, size_t size);
#if (defined(LUMEN_FEXCORE_RUNTIME) && (defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID)) || \
    (defined(LUMEN_RUNTIME_LINKER_ADDRESS_TEST_HOOK) && defined(__APPLE__))
// Record the first denied HLE access in the active call. The bridge consumes
// it before publishing results or returning to guest code.
void ReportGuestMemoryFault(uint64_t address, size_t size, bool write);
#endif
#if (defined(LUMEN_FEXCORE_RUNTIME) || defined(LUMEN_RUNTIME_LINKER_ADDRESS_TEST_HOOK)) && \
    (defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID)
// Copy through borrowed spans; a bounded string needs no trailing NUL. Kernel
// APIs returning EFAULT suppress the fatal HLE fault report explicitly.
bool ReadGuestString(uint64_t address, size_t limit, std::string* text, bool report_fault = true);
#endif
#endif

class RuntimeLinker
{
public:
	RuntimeLinker();
	virtual ~RuntimeLinker();
	void Clear();
	// The caller must first quiesce guest execution and purge every subsystem
	// that can retain guest pointers. Unlike Clear(), this rebuilds the symbol
	// database so the same process-lifetime linker can serve another session.
	[[nodiscard]] bool ResetForNextSession();

	LUMEN_CLASS_NO_COPY(RuntimeLinker);

	void DbgDump(const String& folder);

	Program* LoadProgram(const String& elf_name, bool defer_unresolved_plt_functions = false);

	// Module-load admission for `sceKernelLoadStartModule`. A module file that
	// is already live keeps its single mapped instance (no second copy and no
	// second initializer run) and gains one load reference; a new file is
	// loaded and marked with its first guest reference. `already_loaded`
	// distinguishes the two outcomes so the caller can skip relocation and
	// start for a duplicate request.
	struct ModuleLoadAdmission
	{
		Program* program        = nullptr;
		bool     already_loaded = false;
	};
	[[nodiscard]] ModuleLoadAdmission AcquireModuleLoadReference(const String& elf_name,
	                                                            bool defer_unresolved_plt_functions);
	// Releases one guest load reference. Returns true when no other reference
	// keeps the module live and the caller may stop and unload it.
	[[nodiscard]] bool ReleaseModuleLoadReference(Program* program);
	void     SaveMainProgram(const String& elf_name);
	void     SaveProgram(Program* program, const String& elf_name);
	[[nodiscard]] bool StopUnloadProgram(Program* program, size_t args,
	                                     const void* argp, module_func_t func,
	                                     int* result);

	[[nodiscard]] uint64_t GetEntry();
	[[nodiscard]] uint64_t GetProcParam();

	void RelocateAll();

	void Execute();
	int  StartModule(Program* program, size_t args, const void* argp, module_func_t func);
	int  StopModule(Program* program, size_t args, const void* argp, module_func_t func);
	void StartAllModules();
	void StopAllModules();
	void DeleteTlss(int thread_id);

	void Resolve(const String& name, SymbolType type, Program* program, SymbolRecord* out_info, bool* bind_self);
	[[nodiscard]] uint64_t FindExport(const SymbolResolve& symbol);

	// sceKernelDlsym backend: resolve an exported symbol by its plain PS4 NID
	// within a single module. Handle 0 selects the main executable (the first
	// loaded program); other handles are the LoadStartModule ids. Returns false
	// when the module handle or the NID is unknown.
	[[nodiscard]] bool FindProgramExportByNid(int32_t handle, const String& nid, uint64_t* vaddr);
	static RuntimeLinker*  Instance();
	[[nodiscard]] bool     GuestExceptionRoutingReady() const;

	SymbolDatabase* Symbols() { return m_symbols; }

#if defined(LUMEN_FEXCORE_RUNTIME)
	[[nodiscard]] bool DescribeFexUnresolvedPlt(
	    Program* program, uint64_t index, String* description);
#endif

	static uint64_t ReadFromElf(Program* program, uint64_t vaddr);
	Program*        FindProgramByAddr(uint64_t vaddr);
	bool            QueryProgramUnwindInfo(uint64_t vaddr,
	                                      ProgramUnwindInfo* info);
	Program*        FindProgramById(int32_t id);
	Program*        FindProgramByName(const char* name);

	static uint8_t* TlsGetAddr(Program* program);
	static void     DeleteTls(Program* program, int thread_id);
	void            PrepareCurrentThreadTls();

#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK)
	[[nodiscard]] bool SetupSyntheticTlsForPthreadExitTest();
	using TlsRetirementAdmissionTestHook = void (*)(uint32_t phase, int thread_id, uint32_t attempt);
	static void SetTlsRetirementAdmissionTestHook(TlsRetirementAdmissionTestHook hook);
	[[nodiscard]] uint64_t AddSyntheticTlsForRetirementTest(int thread_id, uint64_t mutation_token);
	[[nodiscard]] bool HasTlsForRetirementTest(int thread_id);
#endif

#if defined(LUMEN_MODULE_LOAD_DEDUPE_TEST_HOOK)
	// Test-owned seed for the guest module-load bookkeeping: an image-less
	// Program with the given resolved file name. It owns no mappings, so the
	// ordinary Clear() path releases it like any other entry.
	Program* RegisterSyntheticProgramForTest(const String& file_name);
#endif

	void StackTrace(uint64_t frame_ptr);

	// HeapAPI management
	void SetHeapAPI(void* func[]);
	HeapAPI* GetHeapAPI() { return m_heap_api; }

private:
	void ClearLocked(bool rebuild_symbols);
	Program*         LoadProgramLocked(const String& elf_name, bool defer_unresolved_plt_functions);
	[[nodiscard]] Program* FindLoadedProgramByFileNameLocked(const String& elf_name) const;
	[[nodiscard]] bool CanUnloadProgramLocked(Program* program);

#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
	friend bool RuntimeLinkerSessionResetContractTest();
#endif

	static void LoadProgramToMemory(Program* program);
	static void ParseProgramDynamicInfo(Program* program);
	static void CreateSymbolDatabase(Program* program);
	static void Relocate(Program* program);
	static void RetryUnresolvedRelocations(Program* program);
	static void DeleteProgram(Program* program);
	static void SetupTlsHandler(Program* program);

	Program* FindProgram(const ModuleId& m, const LibraryId& l,
	                     Program* requester = nullptr);

	static const ModuleId*  FindModule(const Program& program, const String& id);
	static const LibraryId* FindLibrary(const Program& program, const String& id);

	Vector<Program*> m_programs;
	SymbolDatabase*  m_symbols   = nullptr;
	bool             m_relocated = false;
	HeapAPI*         m_heap_api  = nullptr;
	uint64_t         m_entry_params = 0;
	Core::Mutex      m_mutex;
};

#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK)
// Test-owned instance of the same managed startup block used by Execute.
// Release it only after the test guest root has returned.
[[nodiscard]] uint64_t PrepareEntryParametersForTest(const char* name);
#endif

#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
bool RuntimeLinkerSessionResetContractTest();
#endif

#if defined(LUMEN_RUNTIME_LINKER_ADDRESS_TEST_HOOK)
enum class UnresolvedRelocationActionForTest: uint8_t
{
	Fail,
	InvalidObject,
	PreserveExistingTarget,
};

[[nodiscard]] UnresolvedRelocationActionForTest
RuntimeLinkerUnresolvedRelocationActionForTest(
	SymbolType type, bool jmprela_table, bool weak,
	bool defer_unresolved_plt_functions);

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
[[nodiscard]] bool FexUnresolvedArgumentRegionsEnabledForTest(const char* value);
[[nodiscard]] bool FormatFexUnresolvedArgumentRegionForTest(
	char* output, size_t capacity, int thread_id, size_t index, uint64_t address);
#endif

[[nodiscard]] bool RuntimeLinkerValidateSegmentLayoutForTest(
	uint64_t vaddr, uint64_t file_offset, uint64_t file_size,
	uint64_t memory_size, uint64_t alignment, uint64_t* aligned_size,
	uint64_t* end);
[[nodiscard]] bool RuntimeLinkerCalculateProgramSpanForTest(
	const Elf64_Phdr* phdr, size_t phnum, uint64_t* span);
[[nodiscard]] bool RuntimeLinkerAdvanceModuleLayoutCursorForTest(
	uint64_t actual_base, uint64_t allocation_size, uint64_t* next_cursor);
#endif

bool RuntimeLinkerLibcMemsetPreferenceContractTest();

} // namespace Lumen::Loader

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_RUNTIMELINKER_H_ */
